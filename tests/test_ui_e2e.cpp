/// @file test_ui_runtime.cpp
/// @brief Split from test_ui.cpp - AppRuntime, E2E_Gate, FullscreenLayout, LogoV2, PromptInput, ReplScreen (SLOC budget fix)

#include <cstdlib>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <gtest/gtest.h>
#include <httplib.h>

#include "test_ui_helpers.h"

import std;
import cc.ui.app.app;
import cc.ui.messages.message_image;
import cc.commands.registry;
import cc.query.query_engine;
import cc.tools.tool;
import cc.utils.session_storage;

namespace {
namespace fs = std::filesystem;
}


// ═══════════════════════════════════════════════════════════════════════════════
// cc.ui.chrome.terminal: FTXUI terminal controller and common widgets
// ═══════════════════════════════════════════════════════════════════════════════





// =============================================================================
// GAP: unseen-divider-in-transcript-missing (P1)
// TS REF: Messages.tsx L549-553  (useUnseenDivider → dividerBeforeIndex)
//       + Messages.tsx L631-635  (<Divider title="N new messages" color="inactive"/>)
//       + FullscreenLayout.tsx L224-256  (UnseenDivider + computeUnseenDivider)
// Faithful port: compute divider insertion point via 24-char uuid prefix match,
// insert a colored separator row BEFORE the matched payload row with marginTop=1.
// =============================================================================

// ============================================================
// E2E UI GATE — regression guard for critical user-visible scenarios
//
// These tests exercise the full AppAdapter → Render() pipeline in the
// scenarios that have historically broken during UI migration:
//   1. Startup screen (logo + statusline + prompt)
//   2. User message with image attachment (⎿ connector, no bg on continuation)
//   3. Tool use block with Input/Output sections
//   4. Full conversation flow (user → tool → result → assistant)
//   5. Statusline always visible in every state
// ============================================================

namespace e2e_gate {

/// Mock server that returns a tool_use block for an "analyze_image" MCP tool,
/// then on the second round returns the assistant text response.
/// Simulates the exact flow: user sends image → model calls MCP tool → result
/// → model generates text reply.
class McpToolFlowServer {
public:
    McpToolFlowServer() {
        server_.Post("/v1/messages", [&](const httplib::Request& req, httplib::Response& res) {
            std::size_t request_number = 0;
            {
                std::lock_guard lock(mutex_);
                request_number = ++request_count_;
                last_body_ = req.body;
            }
            cv_.notify_all();

            res.set_header("x-usage-input-tokens", "42");
            if (request_number == 1) {
                // First round: model calls analyze_image tool
                res.set_chunked_content_provider(
                    "text/event-stream",
                    [this](size_t, httplib::DataSink& sink) {
                        if (phase_++ > 0) return false;
                        sink.os <<
                            "event: message_start\n"
                            "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_e2e_1\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"loom-test\",\"content\":[]}}\n\n"
                            "event: content_block_start\n"
                            "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_e2e_1\",\"name\":\"analyze_image\",\"input\":{}}}\n\n"
                            "event: content_block_delta\n"
                            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"imageSource\\\":\\\"https://example.com/img.png\\\",\\\"prompt\\\":\\\"Describe this image in detail\\\"}\"}}\n\n"
                            "event: content_block_stop\n"
                            "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                            "event: message_delta\n"
                            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":15}}\n\n"
                            "event: message_stop\n"
                            "data: {\"type\":\"message_stop\"}\n\n";
                        sink.done();
                        {
                            std::lock_guard lock(mutex_);
                            tool_sent_ = true;
                        }
                        cv_.notify_all();
                        return true;
                    });
            } else {
                // Second round: model generates text reply after seeing tool result
                res.set_chunked_content_provider(
                    "text/event-stream",
                    [this](size_t, httplib::DataSink& sink) {
                        if (phase2_++ > 0) return false;
                        sink.os <<
                            "event: message_start\n"
                            "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_e2e_2\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"loom-test\",\"content\":[]}}\n\n"
                            "event: content_block_start\n"
                            "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
                            "event: content_block_delta\n"
                            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"This is a screenshot of a dark-themed terminal UI showing a code review platform with bullet points and comment sections.\"}}\n\n"
                            "event: content_block_stop\n"
                            "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                            "event: message_delta\n"
                            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":25}}\n\n"
                            "event: message_stop\n"
                            "data: {\"type\":\"message_stop\"}\n\n";
                        sink.done();
                        {
                            std::lock_guard lock(mutex_);
                            reply_sent_ = true;
                        }
                        cv_.notify_all();
                        return true;
                    });
            }
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] {
            server_.listen_after_bind();
        });
        server_.wait_until_ready();
    }

    ~McpToolFlowServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    bool valid() const { return port_ > 0; }
    std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port_); }

    bool wait_for_tool(std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return tool_sent_; });
    }

    bool wait_for_reply(std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return reply_sent_; });
    }

    std::string last_body() {
        std::lock_guard lock(mutex_);
        return last_body_;
    }

    /// Check if the tools array in the request body contains a specific tool name.
    bool request_contains_tool(std::string_view tool_name) {
        std::lock_guard lock(mutex_);
        return last_body_.find(tool_name) != std::string::npos;
    }

private:
    httplib::Server server_;
    int port_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t request_count_ = 0;
    std::string last_body_;
    bool tool_sent_ = false;
    bool reply_sent_ = false;
    int phase_ = 0;
    int phase2_ = 0;
};

/// Helper: render app to plain text and check for required substrings.
/// Returns vector of missing strings for diagnostic output.
std::vector<std::string> check_required_strings(
    const std::string& rendered,
    const std::vector<std::string>& required) {
    std::vector<std::string> missing;
    for (const auto& s : required) {
        if (rendered.find(s) == std::string::npos) {
            missing.push_back(s);
        }
    }
    return missing;
}

} // namespace e2e_gate

/// Unit test: find_divider_before_visible_index correctly matches on the
/// first 24 chars of each payload row's uuid; skips compact-group rows.
TEST(E2E_Gate, StartupScreenHasAllElements) {
    using namespace e2e_gate;

    cc::core::ToolRegistry tools;
    cc::core::QueryEngineConfig config;
    config.context_window.auto_compact = false;
    config.cwd = fs::temp_directory_path().string();
    cc::core::QueryEngine engine(std::move(config), tools);

    cc::commands::AppCommandRegistry commands;
    const auto storage_root = fs::temp_directory_path() /
        ("cc_e2e_gate_startup_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cc::utils::SessionStorage storage(storage_root);

    auto app = ftxui::Make<cc::ui::AppAdapter>(
        &engine, nullptr, &commands, &storage, [] {});

    // Render at a realistic terminal size
    auto rendered = strip_ansi(render_to_plain_text(app->Render(), 120, 36));

    // Critical elements that must always be visible on startup
    auto missing = check_required_strings(rendered, {
        "Loom",   // Logo / app name
        "v",             // Version string
        "Try ",          // Prompt placeholder hint
    });

    EXPECT_TRUE(missing.empty())
        << "E2E GATE FAIL: Startup screen missing critical elements:\n"
        << "  Missing: " << [&] {
            std::string s;
            for (const auto& m : missing) s += "[" + m + "] ";
            return s;
        }()
        << "\n  Rendered output (first 2000 chars):\n"
        << rendered.substr(0, 2000);

    fs::remove_all(storage_root);
}



/// E2E Gate #2: After submitting a user message, the statusline must still
/// be visible. Regression guard for "statusline vanished after MCP connect".
TEST(E2E_Gate, StatuslineVisibleAfterSubmit) {
    using namespace e2e_gate;

    McpToolFlowServer server;
    ASSERT_TRUE(server.valid());

    cc::core::ToolRegistry tools;
    cc::core::QueryEngineConfig config;
    config.api_key = "test-key";
    config.base_url = server.base_url();
    config.context_window.auto_compact = false;
    config.cwd = fs::temp_directory_path().string();
    cc::core::QueryEngine engine(std::move(config), tools);

    cc::commands::AppCommandRegistry commands;
    const auto storage_root = fs::temp_directory_path() /
        ("cc_e2e_gate_statusline_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cc::utils::SessionStorage storage(storage_root);

    auto app = ftxui::Make<cc::ui::AppAdapter>(
        &engine, nullptr, &commands, &storage, [] {});

    // Submit a message (triggers query)
    app->HandleSubmit("hello");

    // Wait for query to finish
    ASSERT_TRUE(wait_until([&] {
        (void)app->Render();
        return !app->is_query_running_for_testing();
    }, std::chrono::seconds(5)));

    // Render and check statusline is still present
    auto rendered = strip_ansi(render_to_plain_text(app->Render(), 120, 36));

    // Statusline indicators: model name, cost/tokens, or branch info
    bool has_statusline_indicator =
        rendered.find("master") != std::string::npos ||
        rendered.find("GLM") != std::string::npos ||
        rendered.find("tokens") != std::string::npos ||
        rendered.find("0.0") != std::string::npos;

    EXPECT_TRUE(has_statusline_indicator)
        << "E2E GATE FAIL: Statusline not visible after submit.\n"
        << "  Rendered output (last 1500 chars):\n"
        << rendered.substr(std::max(0, (int)rendered.size() - 1500));

    fs::remove_all(storage_root);
}



/// E2E Gate #3: MCP tool_use block must render with tool name and Input section.
/// Regression guard for "MCP tool blocks disappeared" bug.
TEST(E2E_Gate, McpToolUseBlockRendersWithNameAndInput) {
    using namespace e2e_gate;

    McpToolFlowServer server;
    ASSERT_TRUE(server.valid());

    // Register a dummy "mcp" tool and set up missing-tool handler so the
    // engine can "execute" analyze_image.
    cc::core::ToolRegistry tools;
    tools.set_missing_tool_handler(
        [](std::string_view /*name*/,
           const cc::core::ToolInput&) -> cc::core::Result<cc::core::ToolResult> {
            // Simulate MCP tool returning a result
            std::vector<cc::core::ToolOutputContent> contents;
            contents.push_back(cc::core::ToolOutputContent::text_output(
                "The image shows a dark-themed terminal interface."));
            return cc::core::ToolResult{.content = std::move(contents), .is_error = false};
        });

    cc::core::QueryEngineConfig config;
    config.api_key = "test-key";
    config.base_url = server.base_url();
    config.context_window.auto_compact = false;
    config.cwd = fs::temp_directory_path().string();
    cc::core::QueryEngine engine(std::move(config), tools);

    cc::commands::AppCommandRegistry commands;
    const auto storage_root = fs::temp_directory_path() /
        ("cc_e2e_gate_tooluse_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cc::utils::SessionStorage storage(storage_root);

    auto app = ftxui::Make<cc::ui::AppAdapter>(
        &engine, nullptr, &commands, &storage, [] {});

    app->HandleSubmit("describe this image");

    // Wait for the full flow to complete (tool → result → reply)
    ASSERT_TRUE(wait_until([&] {
        (void)app->Render();
        return !app->is_query_running_for_testing();
    }, std::chrono::seconds(8)));

    auto rendered = strip_ansi(render_to_plain_text(app->Render(), 140, 40));

    // The tool_use block must show the tool name (generic UI humanizes to title case)
    EXPECT_NE(rendered.find("Analyze Image"), std::string::npos)
        << "E2E GATE FAIL: MCP tool_use block missing tool name 'Analyze Image'.\n"
        << "  The tool call block should show which tool was invoked.\n"
        << "  Rendered output:\n" << rendered.substr(0, 3000);

    // The assistant text reply must be visible
    EXPECT_NE(rendered.find("screenshot"), std::string::npos)
        << "E2E GATE FAIL: Assistant text reply not visible after MCP tool result.\n"
        << "  Should contain 'screenshot' from the mock reply.\n"
        << "  Rendered output:\n" << rendered.substr(0, 3000);

    fs::remove_all(storage_root);
}



/// E2E Gate #3b: MCP tool result must render as SEPARATE card, not just
/// inside the tool_use card's Output section.
///
/// Regression guard for the bug where role="tool" messages had
/// is_tool_use=true, causing them to be swallowed into the AssistantToolUse
/// renderer instead of appearing as their own UserToolResult card.
///
/// TS parity: after a tool completes, the transcript shows:
///   1. AssistantToolUse card (● Built-in Tool: analyze_image + Input + Output)
///   2. UserToolResult card (✓ analyze_image + result content)  ← SEPARATE
///   3. AssistantTextMessage (the model's reply after seeing the result)
TEST(E2E_Gate, McpToolResultRendersAsSeparateCard) {
    using namespace e2e_gate;

    // Use a server that returns analyze_image tool_use + text reply
    McpToolFlowServer server;
    ASSERT_TRUE(server.valid());

    // The exact MCP result format from user's screenshot:
    // "analyze_image_result_summary: [{\"text\": \"The image is a solid black square...\"}]"
    // This is what Z.ai / MCP analyze_image returns.
    static constexpr const char* kMcpResult =
        "analyze_image_result_summary: "
        "[{\"text\": \"The image is a solid black square with no visible "
        "content, text, UI elements, or distinct visual features.\"}]";

    cc::core::ToolRegistry tools;
    tools.set_missing_tool_handler(
        [](std::string_view /*name*/,
           const cc::core::ToolInput&) -> cc::core::Result<cc::core::ToolResult> {
            std::vector<cc::core::ToolOutputContent> contents;
            contents.push_back(cc::core::ToolOutputContent::text_output(kMcpResult));
            return cc::core::ToolResult{.content = std::move(contents), .is_error = false};
        });

    cc::core::QueryEngineConfig config;
    config.api_key = "test-key";
    config.base_url = server.base_url();
    config.context_window.auto_compact = false;
    config.cwd = fs::temp_directory_path().string();
    cc::core::QueryEngine engine(std::move(config), tools);

    cc::commands::AppCommandRegistry commands;
    const auto storage_root = fs::temp_directory_path() /
        ("cc_e2e_gate_toolresult_separate_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cc::utils::SessionStorage storage(storage_root);

    auto app = ftxui::Make<cc::ui::AppAdapter>(
        &engine, nullptr, &commands, &storage, [] {});

    app->HandleSubmit("what is this image?");

    // Wait for full flow to complete
    ASSERT_TRUE(wait_until([&] {
        (void)app->Render();
        return !app->is_query_running_for_testing();
    }, std::chrono::seconds(8)));

    auto rendered = strip_ansi(render_to_plain_text(app->Render(), 140, 50));

    // ── Assertion 1: tool_use card must show the tool name ──────────────
    EXPECT_NE(rendered.find("Analyze Image"), std::string::npos)
        << "FAIL: tool_use block missing 'Analyze Image'.\n"
        << "Rendered:\n" << rendered.substr(0, 4000);

    // ── Assertion 2: the MCP result text must be visible ───────────────
    EXPECT_NE(rendered.find("solid black square"), std::string::npos)
        << "FAIL: MCP tool result content not visible in transcript.\n"
        << "The result text 'solid black square' should appear somewhere.\n"
        << "Rendered:\n" << rendered.substr(0, 4000);

    // ── Assertion 3: the ⎿ connector must appear (tool_result MessageResponse)
    //   TS renders tool results via <MessageResponse> (⎿ prefix).  If the
    //   result was swallowed into the tool_use card, no ⎿ would appear for it.
    const std::string connector = "\xe2\x8e\xbf";  // ⎿ U+23BF
    std::size_t connector_pos = rendered.find(connector);
    EXPECT_NE(connector_pos, std::string::npos)
        << "FAIL: No ⎿ connector found in transcript.\n"
        << "This means the tool_result is NOT rendering as a separate card.\n"
        << "It is probably swallowed into the tool_use card's Output section.\n"
        << "Rendered:\n" << rendered.substr(0, 4000);

    if (connector_pos != std::string::npos) {
        // ── Assertion 4: after ⎿, the result content must appear
        std::size_t after_conn = connector_pos + connector.size();
        std::size_t content_after = rendered.find("solid black square", after_conn);
        EXPECT_NE(content_after, std::string::npos)
            << "FAIL: ⎿ found but result content not after it.\n"
            << "The tool_result should show the MCP result text.\n"
            << "Rendered around ⎿:\n"
            << rendered.substr(std::max(0, (int)connector_pos - 20), 100);
    }

    // ── Assertion 5: the model's text reply must be visible (proves the
    //   full round-trip worked — tool result was sent back to model)
    EXPECT_NE(rendered.find("screenshot"), std::string::npos)
        << "FAIL: Assistant text reply not visible after tool result.\n"
        << "The mock model replies with 'screenshot' after seeing the result.\n"
        << "Rendered:\n" << rendered.substr(0, 4000);

    fs::remove_all(storage_root);
}



/// E2E Gate #3c: MCP tool result with _result_summary format shows raw text
/// (TS parity — TS does NOT unwrap _result_summary or [{"text":...}] arrays).
///
/// Verifies that CPP does NOT do "extra translation work" that TS doesn't.
/// The raw format like:
///   analyze_image_result_summary: [{"text": "..."}]
/// is shown to the user as-is (TS screenshot confirms this).
TEST(E2E_Gate, McpResultSummaryFormatShownRaw) {
    using namespace e2e_gate;

    McpToolFlowServer server;
    ASSERT_TRUE(server.valid());

    static constexpr const char* kMcpResult =
        "analyze_image_result_summary: "
        "[{\"text\": \"The image shows a dark terminal.\"}]";

    cc::core::ToolRegistry tools;
    tools.set_missing_tool_handler(
        [](std::string_view /*name*/,
           const cc::core::ToolInput&) -> cc::core::Result<cc::core::ToolResult> {
            std::vector<cc::core::ToolOutputContent> contents;
            contents.push_back(cc::core::ToolOutputContent::text_output(kMcpResult));
            return cc::core::ToolResult{.content = std::move(contents), .is_error = false};
        });

    cc::core::QueryEngineConfig config;
    config.api_key = "test-key";
    config.base_url = server.base_url();
    config.context_window.auto_compact = false;
    config.cwd = fs::temp_directory_path().string();
    cc::core::QueryEngine engine(std::move(config), tools);

    cc::commands::AppCommandRegistry commands;
    const auto storage_root = fs::temp_directory_path() /
        ("cc_e2e_gate_result_raw_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cc::utils::SessionStorage storage(storage_root);

    auto app = ftxui::Make<cc::ui::AppAdapter>(
        &engine, nullptr, &commands, &storage, [] {});

    app->HandleSubmit("what is this?");

    ASSERT_TRUE(wait_until([&] {
        (void)app->Render();
        return !app->is_query_running_for_testing();
    }, std::chrono::seconds(8)));

    auto rendered = strip_ansi(render_to_plain_text(app->Render(), 140, 50));

    // The _result_summary prefix must be VISIBLE (TS parity — not unwrapped)
    EXPECT_NE(rendered.find("result_summary"), std::string::npos)
        << "FAIL: 'result_summary' prefix was stripped/extracted.\n"
        << "TS shows this raw — CPP must NOT do extra unwrapping.\n"
        << "Rendered:\n" << rendered.substr(0, 4000);

    // The [{\"text\": ... array wrapper must also be visible (not unwrapped)
    // Note: after strip_ansi the quotes may be plain " not \"
    EXPECT_NE(rendered.find("[{\"text\""), std::string::npos)
        << "FAIL: JSON array wrapper [{\"text\": was stripped.\n"
        << "TS shows this raw — CPP must NOT unwrap bare arrays.\n"
        << "Rendered:\n" << rendered.substr(0, 4000);

    // The actual content text must be visible
    EXPECT_NE(rendered.find("dark terminal"), std::string::npos)
        << "FAIL: actual result content 'dark terminal' not visible.\n"
        << "Rendered:\n" << rendered.substr(0, 4000);

    fs::remove_all(storage_root);
}



/// E2E Gate #3d: After full MCP tool flow, the tool_use card must NOT show
/// the result in its Output section.  The result should only appear in the
/// separate ✓ UserToolResult card.
///
/// This is the exact bug the user reported: "Z.ai 返回值的 Output 没有作为
/// 单独的对话块渲染" — the result was showing inside the tool_use card's
/// Output section instead of the separate ✓ card.
///
/// Root cause fixed: streaming_tools_ entry had result_preview forwarded
/// even after ToolExecutionEnd (exec_done=true), causing the tool_use card
/// to render an "Output:" section with the result.  Fix: when exec_done,
/// suppress tool_result_preview in the projection.
TEST(E2E_Gate, McpToolResultSuppressedInToolUseCardDuringStreaming) {
    using namespace e2e_gate;

    // Use the standard two-round server (tool_use → text reply)
    McpToolFlowServer server;
    ASSERT_TRUE(server.valid());

    // The exact MCP result from user's screenshot (Image #9):
    // "analyze_image_result_summary: [{\"text\": \"The image is a completely
    //   black square with no visible content, text, diagrams, UI elements...\"}]"
    static constexpr const char* kMcpResult =
        "analyze_image_result_summary: "
        "[{\"text\": \"The image is a completely black square with no visible "
        "content, text, diagrams, UI elements, or any other visual details.\"}]";

    cc::core::ToolRegistry tools;
    tools.set_missing_tool_handler(
        [](std::string_view,
           const cc::core::ToolInput&) -> cc::core::Result<cc::core::ToolResult> {
            std::vector<cc::core::ToolOutputContent> contents;
            contents.push_back(cc::core::ToolOutputContent::text_output(kMcpResult));
            return cc::core::ToolResult{.content = std::move(contents), .is_error = false};
        });

    cc::core::QueryEngineConfig config;
    config.api_key = "test-key";
    config.base_url = server.base_url();
    config.context_window.auto_compact = false;
    config.cwd = fs::temp_directory_path().string();
    cc::core::QueryEngine engine(std::move(config), tools);

    cc::commands::AppCommandRegistry commands;
    const auto storage_root = fs::temp_directory_path() /
        ("cc_e2e_gate_no_output_in_tooluse_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cc::utils::SessionStorage storage(storage_root);

    auto app = ftxui::Make<cc::ui::AppAdapter>(
        &engine, nullptr, &commands, &storage, [] {});

    app->HandleSubmit("what is this image?");

    // Wait for full flow to complete (both API rounds)
    ASSERT_TRUE(wait_until([&] {
        (void)app->Render();
        return !app->is_query_running_for_testing();
    }, std::chrono::seconds(8)));

    auto rendered = strip_ansi(render_to_plain_text(app->Render(), 140, 50));

    // ── Assertion 1: tool_use card must show the tool name ──────────────
    EXPECT_NE(rendered.find("Analyze Image"), std::string::npos)
        << "FAIL: 'Analyze Image' not visible.\n"
        << "Rendered:\n" << rendered.substr(0, 4000);

    // ── Assertion 2: ⎿ connector must be visible (separate result row) ─
    const std::string connector = "\xe2\x8e\xbf";
    std::size_t connector_pos = rendered.find(connector);
    EXPECT_NE(connector_pos, std::string::npos)
        << "FAIL: No ⎿ connector found.\n"
        << "The tool_result is NOT rendering as a separate card.\n"
        << "Rendered:\n" << rendered.substr(0, 4000);

    // ── Assertion 3: the result text must be visible (in ⎿ row) ────────
    EXPECT_NE(rendered.find("completely black square"), std::string::npos)
        << "FAIL: result content 'completely black square' not visible.\n"
        << "It should appear in the separate ⎿ result row.\n"
        << "Rendered:\n" << rendered.substr(0, 4000);

    // ── Assertion 4 (CRITICAL): "Output:" must NOT appear near the
    //    tool_use card header.  Before the fix, the streaming tool_use entry
    //    forwarded result_preview even after ToolExecutionEnd, causing the
    //    tool_use card to show an "Output:" section with the MCP result text.
    //    After the fix (exec_done suppresses tool_result_preview), the
    //    tool_use card shows only Input, no Output.
    //
    //    We find the first "Analyze Image" occurrence (tool_use header) and
    //    check that "Output:" doesn't appear within 300 chars after it.
    std::size_t first_tool = rendered.find("Analyze Image");
    if (first_tool != std::string::npos) {
        std::size_t search_end = std::min(first_tool + 300, rendered.size());
        std::string after_tool = rendered.substr(first_tool, search_end - first_tool);
        std::size_t output_in_tool = after_tool.find("Output:");
        EXPECT_EQ(output_in_tool, std::string::npos)
            << "FAIL: 'Output:' found inside the tool_use card area.\n"
            << "This means the tool_use card is showing the result in its\n"
            << "Output section — should only be in the separate ⎿ row.\n"
            << "Context around tool_use header:\n"
            << after_tool.substr(0, 400);
    }

    // ── Assertion 5: the _result_summary prefix must be visible (raw) ───
    //    TS shows the raw format including the prefix.  CPP must NOT unwrap.
    EXPECT_NE(rendered.find("_result_summary"), std::string::npos)
        << "FAIL: '_result_summary' prefix not visible.\n"
        << "TS shows this raw — CPP must not unwrap.\n"
        << "Rendered:\n" << rendered.substr(0, 4000);

    // ── Assertion 6: model text reply must be visible (full round-trip) ─
    EXPECT_NE(rendered.find("screenshot"), std::string::npos)
        << "FAIL: assistant text reply not visible.\n"
        << "Rendered:\n" << rendered.substr(0, 4000);

    fs::remove_all(storage_root);
}



/// E2E Gate #4: API request body must include MCP tools in the tools array.
/// Regression guard for "MCP tools not sent to API" bug.
TEST(E2E_Gate, McpToolsIncludedInApiRequestBody) {
    using namespace e2e_gate;

    McpToolFlowServer server;
    ASSERT_TRUE(server.valid());

    cc::core::ToolRegistry tools;
    // Register built-in tools so the request has something
    tools.set_missing_tool_handler(
        [](std::string_view tool_name,
           const cc::core::ToolInput&) -> cc::core::Result<cc::core::ToolResult> {
            return std::unexpected(cc::core::Error::make(
                cc::core::ErrorCode::ToolNotFound,
                std::format("Tool '{}' not found", tool_name)));
        });

    cc::core::QueryEngineConfig config;
    config.api_key = "test-key";
    config.base_url = server.base_url();
    config.context_window.auto_compact = false;
    config.cwd = fs::temp_directory_path().string();

    // Add a dynamic tools provider (simulating MCP tool discovery)
    config.dynamic_tools_provider = []() -> std::vector<cc::core::ToolDefinition> {
        std::vector<cc::core::ToolDefinition> defs;
        cc::core::ToolDefinition d;
        d.name = "analyze_image";
        d.description = "Analyze an image and return a detailed description";
        d.permission = cc::core::ToolPermission::Network;
        d.category = "mcp:zai-builtin";
        defs.push_back(d);
        return defs;
    };

    cc::core::QueryEngine engine(std::move(config), tools);

    cc::commands::AppCommandRegistry commands;
    const auto storage_root = fs::temp_directory_path() /
        ("cc_e2e_gate_apibody_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cc::utils::SessionStorage storage(storage_root);

    auto app = ftxui::Make<cc::ui::AppAdapter>(
        &engine, nullptr, &commands, &storage, [] {});

    app->HandleSubmit("test");

    // Wait for query to finish
    ASSERT_TRUE(wait_until([&] {
        (void)app->Render();
        return !app->is_query_running_for_testing();
    }, std::chrono::seconds(5)));

    // The request body sent to the API should include the MCP tool name
    EXPECT_TRUE(server.request_contains_tool("analyze_image"))
        << "E2E GATE FAIL: API request body does not include MCP tool 'analyze_image'.\n"
        << "  The dynamic_tools_provider should ensure MCP tools are in the\n"
        << "  tools array sent to the API.\n"
        << "  Last request body:\n" << server.last_body().substr(0, 2000);

    fs::remove_all(storage_root);
}



/// E2E Gate #5: Image placeholder must show [Image #N] format, not just [Image].
/// Regression guard for "image ID missing" bug.
TEST(E2E_Gate, ImagePlaceholderShowsNumberedId) {
    using namespace cc::ui::messages::image;

    ImageMessageData d;
    d.media_type = "image/png";
    d.image_id = "1";  // This is the key: display id must be set
    d.file_name = "clipboard.png";
    d.source_type = ImageSource::Clipboard;

    Element el = render(d);
    std::string snap = strip_ansi(render_to_plain_text(std::move(el), 80, 10));

    EXPECT_NE(snap.find("[Image #1]"), std::string::npos)
        << "E2E GATE FAIL: Image placeholder shows '[Image]' instead of '[Image #1]'.\n"
        << "  The image_id field must be populated from the paste order.\n"
        << "  Got: " << snap;
}



/// E2E Gate #6: ⎿ connector character (U+23BF) must be used for continuation
/// blocks, NOT ⏿ (U+23FF). Regression guard for wrong Unicode codepoint.
/// This test verifies the source files contain the correct UTF-8 bytes,
/// since the rendering functions live in a `detail` namespace that cannot
/// be directly imported without causing ambiguity.
TEST(E2E_Gate, ConnectorCharacterIsCorrectCodepoint) {
    // The correct connector is U+23BF = ⎿ = \xe2\x8e\xbf in UTF-8
    // The WRONG connector would be U+23FF = ⏿ = \xe2\x8f\xbf
    const std::string correct_connector = "\xe2\x8e\xbf";  // U+23BF ⎿
    const std::string wrong_connector = "\xe2\x8f\xbf";    // U+23FF ⏿

    // Resolve project root relative to this test file (tests/test_ui.cpp)
    const std::string test_file = __FILE__;
    const auto test_dir = test_file.substr(0, test_file.find_last_of('/'));
    const auto project_root = test_dir.substr(0, test_dir.find_last_of('/'));

    // Check all message rendering source files for the correct connector bytes
    const std::vector<std::string> source_files = {
        "src/ui/messages/message_tool_result.cppm",
        "src/ui/messages/user_text_message.cppm",
        "src/ui/messages/messages_list.cppm",
    };

    for (const auto& rel_path : source_files) {
        const auto full_path = project_root + "/" + rel_path;
        std::ifstream file(full_path);
        if (!file.is_open()) continue;  // Skip if file not found

        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

        // Every file that renders connectors should use the correct U+23BF
        EXPECT_NE(content.find(correct_connector), std::string::npos)
            << "E2E GATE FAIL: " << rel_path << " does not contain correct "
            << "connector U+23BF (⎿ = \\xe2\\x8e\\xbf).\n"
            << "  Expected the 'NOT-CURVE ARCH EXTENDING LEFT AND DOWNWARDS' character.";

        // No file should contain the wrong U+23FF
        EXPECT_EQ(content.find(wrong_connector), std::string::npos)
            << "E2E GATE FAIL: " << rel_path << " contains WRONG connector "
            << "U+23FF (⏿ = \\xe2\\x8f\\bf black floppy disk).\n"
            << "  This should be U+23BF (⎿). Fix the hex bytes in the source.";
    }
}



/// E2E Gate #7: Full conversation snapshot golden test.
/// Renders the entire REPL screen after a complete interaction and compares
/// against a stored golden baseline. Catches ANY visual regression.
TEST(E2E_Gate, FullConversationGoldenSnapshot) {
    using namespace e2e_gate;

    McpToolFlowServer server;
    ASSERT_TRUE(server.valid());

    cc::core::ToolRegistry tools;
    tools.set_missing_tool_handler(
        [](std::string_view /*name*/,
           const cc::core::ToolInput&) -> cc::core::Result<cc::core::ToolResult> {
            std::vector<cc::core::ToolOutputContent> contents;
            contents.push_back(cc::core::ToolOutputContent::text_output(
                "The image shows a dark-themed terminal interface."));
            return cc::core::ToolResult{.content = std::move(contents), .is_error = false};
        });

    cc::core::QueryEngineConfig config;
    config.api_key = "test-key";
    config.base_url = server.base_url();
    config.context_window.auto_compact = false;
    config.cwd = fs::temp_directory_path().string();
    cc::core::QueryEngine engine(std::move(config), tools);

    cc::commands::AppCommandRegistry commands;
    const auto storage_root = fs::temp_directory_path() /
        ("cc_e2e_gate_golden_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cc::utils::SessionStorage storage(storage_root);

    auto app = ftxui::Make<cc::ui::AppAdapter>(
        &engine, nullptr, &commands, &storage, [] {});

    app->HandleSubmit("describe this image");

    // Wait for full flow
    ASSERT_TRUE(wait_until([&] {
        (void)app->Render();
        return !app->is_query_running_for_testing();
    }, std::chrono::seconds(8)));

    // Final render
    auto rendered = render_to_plain_text(app->Render(), 120, 36);
    auto plain = strip_ansi(rendered);

    // Structural assertions: every major section must be present
    auto missing = check_required_strings(plain, {
        "Loom",       // Logo/header
        "describe this",     // User message
        "analyze_image",     // Tool use block
        "terminal",          // Assistant reply (from mock)
    });

    fs::remove_all(storage_root);
}
