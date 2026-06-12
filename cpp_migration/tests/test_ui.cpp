/// @file test_ui.cpp
/// @brief cc_ui module unit tests for the migrated FTXUI-based APIs.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <gtest/gtest.h>
#include <httplib.h>

import cc.ui.terminal;
import cc.ui.components;
import cc.ui.panels;
import cc.ui.messages;
import cc.ui.prompt_input;
import cc.ui.markdown;
import cc.ui.components_extended;
import cc.ui.wizard_dialog;
import cc.ui.app;
import cc.commands.registry;
import cc.query.query_engine;
import cc.tools.tool;
import cc.utils.session_storage;

namespace {

namespace fs = std::filesystem;

void expect_element(const ftxui::Element& element) {
    EXPECT_NE(element, nullptr);
}

std::string render_to_plain_text(ftxui::Element element, int width = 80, int height = 20) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, element);
    return screen.ToString();
}

/// Strip ANSI escape sequences from a rendered string so that assertions
/// compare semantic content rather than color/style codes.
std::string strip_ansi(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
            // CSI sequence: skip until we find a non-digit/param byte
            i += 2;
            while (i < s.size() && (s[i] < 0x40 || s[i] > 0x7E)) ++i;
            if (i < s.size()) ++i;  // skip the final byte
            continue;
        }
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

bool wait_until(std::function<bool()> predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

class LocalChunkedAnthropicStreamServer {
public:
    LocalChunkedAnthropicStreamServer() {
        server_.Post("/v1/messages", [&](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard lock(mutex_);
                ++request_count_;
                last_body_ = req.body;
            }
            cv_.notify_all();

            res.set_header("x-usage-input-tokens", "7");
            auto phase = std::make_shared<int>(0);
            res.set_chunked_content_provider(
                "text/event-stream",
                [this, phase](size_t /*offset*/, httplib::DataSink& sink) {
                    if (*phase == 0) {
                        sink.os <<
                            "event: message_start\n"
                            "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_ui_cancel\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-test\",\"content\":[]}}\n\n"
                            "event: content_block_start\n"
                            "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
                            "event: content_block_delta\n"
                            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"partial UI stream\"}}\n\n";
                        {
                            std::lock_guard lock(mutex_);
                            first_delta_sent_ = true;
                        }
                        cv_.notify_all();
                        ++(*phase);
                        return true;
                    }

                    {
                        std::unique_lock lock(mutex_);
                        if (!cv_.wait_for(lock, std::chrono::seconds(3), [this] {
                                return continue_after_cancel_;
                            })) {
                            return false;
                        }
                    }

                    sink.os <<
                        "event: content_block_delta\n"
                        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\" after cancel\"}}\n\n"
                        "event: content_block_stop\n"
                        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                        "event: message_delta\n"
                        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":3}}\n\n"
                        "event: message_stop\n"
                        "data: {\"type\":\"message_stop\"}\n\n";
                    sink.done();
                    ++(*phase);
                    return true;
                });
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] {
            server_.listen_after_bind();
        });
        server_.wait_until_ready();
    }

    ~LocalChunkedAnthropicStreamServer() {
        release_after_cancel();
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] bool valid() const noexcept {
        return port_ > 0;
    }

    [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    void release_after_cancel() {
        {
            std::lock_guard lock(mutex_);
            continue_after_cancel_ = true;
        }
        cv_.notify_all();
    }

    [[nodiscard]] bool wait_for_first_delta(
        std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] {
            return first_delta_sent_;
        });
    }

    [[nodiscard]] std::optional<std::string> last_body() const {
        std::lock_guard lock(mutex_);
        return last_body_;
    }

private:
    httplib::Server server_;
    int port_{0};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t request_count_{0};
    std::optional<std::string> last_body_;
    bool first_delta_sent_{false};
    bool continue_after_cancel_{false};
};

struct ReleaseAfterCancelGuard {
    LocalChunkedAnthropicStreamServer& server;

    ~ReleaseAfterCancelGuard() {
        server.release_after_cancel();
    }
};

class LocalToolUseAnthropicStreamServer {
public:
    LocalToolUseAnthropicStreamServer() {
        server_.Post("/v1/messages", [&](const httplib::Request& req, httplib::Response& res) {
            std::size_t request_number = 0;
            {
                std::lock_guard lock(mutex_);
                request_number = ++request_count_;
                last_body_ = req.body;
            }
            cv_.notify_all();

            res.set_header("x-usage-input-tokens", "11");
            auto phase = std::make_shared<int>(0);
            res.set_chunked_content_provider(
                "text/event-stream",
                [this, phase, request_number](size_t /*offset*/, httplib::DataSink& sink) {
                    if (request_number > 1) {
                        if (*phase > 0) return false;
                        sink.os <<
                            "event: message_start\n"
                            "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_ui_tool_followup\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-test\",\"content\":[]}}\n\n"
                            "event: content_block_start\n"
                            "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
                            "event: content_block_delta\n"
                            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"tool preview done\"}}\n\n"
                            "event: content_block_stop\n"
                            "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                            "event: message_delta\n"
                            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":4}}\n\n"
                            "event: message_stop\n"
                            "data: {\"type\":\"message_stop\"}\n\n";
                        sink.done();
                        ++(*phase);
                        return true;
                    }

                    if (*phase == 0) {
                        sink.os <<
                            "event: message_start\n"
                            "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_ui_tool\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-test\",\"content\":[]}}\n\n"
                            "event: content_block_start\n"
                            "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_ui_1\",\"name\":\"Bash\",\"input\":{}}}\n\n"
                            "event: content_block_delta\n"
                            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"command\\\":\\\"npm test\\\"}\"}}\n\n";
                        {
                            std::lock_guard lock(mutex_);
                            tool_delta_sent_ = true;
                        }
                        cv_.notify_all();
                        ++(*phase);
                        return true;
                    }

                    if (*phase > 1) return false;

                    {
                        std::unique_lock lock(mutex_);
                        if (!cv_.wait_for(lock, std::chrono::seconds(3), [this] {
                                return continue_after_preview_;
                            })) {
                            return false;
                        }
                    }

                    sink.os <<
                        "event: content_block_stop\n"
                        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                        "event: message_delta\n"
                        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":5}}\n\n"
                        "event: message_stop\n"
                        "data: {\"type\":\"message_stop\"}\n\n";
                    sink.done();
                    ++(*phase);
                    return true;
                });
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] {
            server_.listen_after_bind();
        });
        server_.wait_until_ready();
    }

    ~LocalToolUseAnthropicStreamServer() {
        release_after_preview();
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] bool valid() const noexcept {
        return port_ > 0;
    }

    [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    void release_after_preview() {
        {
            std::lock_guard lock(mutex_);
            continue_after_preview_ = true;
        }
        cv_.notify_all();
    }

    [[nodiscard]] bool wait_for_tool_delta(
        std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] {
            return tool_delta_sent_;
        });
    }

    [[nodiscard]] std::optional<std::string> last_body() const {
        std::lock_guard lock(mutex_);
        return last_body_;
    }

private:
    httplib::Server server_;
    int port_{0};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t request_count_{0};
    std::optional<std::string> last_body_;
    bool tool_delta_sent_{false};
    bool continue_after_preview_{false};
};

struct ReleaseAfterToolPreviewGuard {
    LocalToolUseAnthropicStreamServer& server;

    ~ReleaseAfterToolPreviewGuard() {
        server.release_after_preview();
    }
};

class LocalThinkingAnthropicStreamServer {
public:
    LocalThinkingAnthropicStreamServer() {
        server_.Post("/v1/messages", [&](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard lock(mutex_);
                ++request_count_;
                last_body_ = req.body;
            }
            cv_.notify_all();

            res.set_header("x-usage-input-tokens", "13");
            auto phase = std::make_shared<int>(0);
            res.set_chunked_content_provider(
                "text/event-stream",
                [this, phase](size_t /*offset*/, httplib::DataSink& sink) {
                    if (*phase == 0) {
                        sink.os <<
                            "event: message_start\n"
                            "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_ui_thinking\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-test\",\"content\":[]}}\n\n"
                            "event: content_block_start\n"
                            "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n"
                            "event: content_block_delta\n"
                            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"private streaming thinking\"}}\n\n";
                        {
                            std::lock_guard lock(mutex_);
                            thinking_delta_sent_ = true;
                        }
                        cv_.notify_all();
                        ++(*phase);
                        return true;
                    }

                    if (*phase > 1) return false;

                    {
                        std::unique_lock lock(mutex_);
                        if (!cv_.wait_for(lock, std::chrono::seconds(3), [this] {
                                return continue_after_preview_;
                            })) {
                            return false;
                        }
                    }

                    sink.os <<
                        "event: content_block_stop\n"
                        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                        "event: content_block_start\n"
                        "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
                        "event: content_block_delta\n"
                        "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"visible answer after thinking\"}}\n\n"
                        "event: content_block_stop\n"
                        "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
                        "event: message_delta\n"
                        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":6}}\n\n"
                        "event: message_stop\n"
                        "data: {\"type\":\"message_stop\"}\n\n";
                    sink.done();
                    ++(*phase);
                    return true;
                });
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] {
            server_.listen_after_bind();
        });
        server_.wait_until_ready();
    }

    ~LocalThinkingAnthropicStreamServer() {
        release_after_preview();
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] bool valid() const noexcept {
        return port_ > 0;
    }

    [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    void release_after_preview() {
        {
            std::lock_guard lock(mutex_);
            continue_after_preview_ = true;
        }
        cv_.notify_all();
    }

    [[nodiscard]] bool wait_for_thinking_delta(
        std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] {
            return thinking_delta_sent_;
        });
    }

    [[nodiscard]] std::optional<std::string> last_body() const {
        std::lock_guard lock(mutex_);
        return last_body_;
    }

private:
    httplib::Server server_;
    int port_{0};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t request_count_{0};
    std::optional<std::string> last_body_;
    bool thinking_delta_sent_{false};
    bool continue_after_preview_{false};
};

struct ReleaseAfterThinkingPreviewGuard {
    LocalThinkingAnthropicStreamServer& server;

    ~ReleaseAfterThinkingPreviewGuard() {
        server.release_after_preview();
    }
};

} // namespace

// ═══════════════════════════════════════════════════════════════════════════════
// cc.ui.terminal: FTXUI terminal controller and common widgets
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Terminal, ColorThemesAreConstructible) {
    auto dark = cc::ui::ColorTheme::dark();
    auto light = cc::ui::ColorTheme::light();

    (void)dark;
    (void)light;
    SUCCEED();
}

TEST(Terminal, DefaultKeyBindingsContainCoreActions) {
    auto bindings = cc::ui::default_key_bindings();

    EXPECT_FALSE(bindings.empty());
    EXPECT_TRUE(std::any_of(bindings.begin(), bindings.end(), [](const auto& binding) {
        return binding.action == cc::ui::KeyAction::Submit;
    }));
    EXPECT_TRUE(std::any_of(bindings.begin(), bindings.end(), [](const auto& binding) {
        return binding.action == cc::ui::KeyAction::Interrupt;
    }));
}

TEST(Terminal, SpinnerRendersWhenActive) {
    cc::ui::Spinner spinner("Working");
    EXPECT_FALSE(spinner.is_active());

    spinner.start();
    EXPECT_TRUE(spinner.is_active());
    expect_element(spinner.render(cc::ui::ColorTheme::dark()));

    spinner.stop();
    EXPECT_FALSE(spinner.is_active());
}

TEST(Terminal, TerminalUIExposesControlAPI) {
    cc::ui::TerminalUI ui;
    bool submitted = false;
    bool interrupted = false;

    ui.set_on_submit([&](std::string) { submitted = true; });
    ui.set_on_interrupt([&] { interrupted = true; });
    ui.update_status(cc::ui::StatusBarData{.model_name = "test-model", .input_tokens = 1, .output_tokens = 2, .cost_usd = 0.0, .session_id = std::nullopt});
    ui.show_spinner("Testing");
    ui.hide_spinner();

    EXPECT_FALSE(submitted);
    EXPECT_FALSE(interrupted);
}

TEST(Terminal, StatusBarRendersTokensAndCost) {
    cc::ui::StatusBarData data{
        .model_name = "claude-test",
        .input_tokens = 123,
        .output_tokens = 45,
        .cost_usd = 0.0123,
        .session_id = std::optional<std::string>{"session-1"},
    };

    auto rendered = render_to_plain_text(cc::ui::render_status_bar(data, cc::ui::ColorTheme::dark()), 90, 5);

    EXPECT_NE(rendered.find("claude-test"), std::string::npos);
    EXPECT_NE(rendered.find("123"), std::string::npos);
    EXPECT_NE(rendered.find("45"), std::string::npos);
    EXPECT_NE(rendered.find("$0.0123"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════════
// cc.ui.components: reusable FTXUI render helpers
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Components, RenderToolUseReturnsElement) {
    cc::ui::ToolUseDisplayData data{
        .tool_name = "bash",
        .input_summary = "ls -la",
        .output_preview = "ok",
        .is_running = false,
        .is_error = false,
        .duration = std::chrono::milliseconds(12),
    };

    auto rendered = render_to_plain_text(cc::ui::render_tool_use(data), 80, 8);

    EXPECT_NE(rendered.find("bash"), std::string::npos);
    EXPECT_NE(rendered.find("ls -la"), std::string::npos);
    EXPECT_NE(rendered.find("ok"), std::string::npos);
    EXPECT_NE(rendered.find("12ms"), std::string::npos);
}

TEST(Components, RenderPermissionPromptReturnsElement) {
    cc::ui::PermissionPromptData data{
        .tool_name = "Edit",
        .description = "Modify a file",
        .input_preview = "src/main.cpp",
        .affected_paths = {"src/main.cpp"},
    };

    auto rendered = render_to_plain_text(cc::ui::render_permission_prompt(data), 90, 10);

    EXPECT_NE(rendered.find("Permission Required"), std::string::npos);
    EXPECT_NE(rendered.find("Edit"), std::string::npos);
    EXPECT_NE(rendered.find("Modify a file"), std::string::npos);
    EXPECT_NE(rendered.find("src/main.cpp"), std::string::npos);
    EXPECT_NE(rendered.find("[y]es"), std::string::npos);
}

TEST(Components, RenderSearchBoxReturnsElement) {
    std::vector<cc::ui::SearchResult> results = {
        {.label = "commit", .detail = "Create a commit", .icon = "/"},
        {.label = "config", .detail = "Edit config", .icon = "/"},
    };

    expect_element(cc::ui::render_search_box("co", results, 0));
}

TEST(Components, TextInputRendersPlaceholderWithoutExtraCursorSpace) {
    cc::ui::components::TextInputOptions options;
    options.prefix = "▶ ";
    options.placeholder = "Type";
    auto component = cc::ui::components::TextInput(options);

    auto rendered = render_to_plain_text(component->Render(), 20, 3);

    EXPECT_NE(rendered.find("Type"), std::string::npos);
    EXPECT_EQ(rendered.find("  Type"), std::string::npos);
}

TEST(Components, TextInputHidesPlaceholderAfterTyping) {
    cc::ui::components::TextInputOptions options;
    options.prefix = "▶ ";
    options.placeholder = "Type";
    auto component = cc::ui::components::TextInput(options);

    ASSERT_TRUE(component->OnEvent(ftxui::Event::Character("a")));
    auto rendered = render_to_plain_text(component->Render(), 20, 3);

    EXPECT_NE(rendered.find("a"), std::string::npos);
    EXPECT_EQ(rendered.find("Type"), std::string::npos);
}

TEST(Components, TextInputTabAfterEmptyHistorySearchDoesNotCrash) {
    auto component = cc::ui::components::TextInput({});

    EXPECT_TRUE(component->OnEvent(ftxui::Event::Character("\x12")));
    EXPECT_FALSE(component->OnEvent(ftxui::Event::Tab));
}

TEST(Components, TextInputMultilineInsertNewline) {
    cc::ui::components::TextInputOptions opts;
    opts.multiline = true;
    auto impl = cc::ui::components::MakeTextInputCore(opts);

    // Pre-populate two lines so Enter inserts newline (not submit).
    // In multiline mode, Enter submits when there is only 1 line;
    // it inserts a newline only when 2+ lines exist.
    impl->PasteText("hello\nworld");
    ASSERT_EQ(impl->text(), "hello\nworld");
    ASSERT_EQ(impl->cursor(), 11);

    impl->HandleEvent(ftxui::Event::Return);

    EXPECT_EQ(impl->text(), "hello\nworld\n");
    EXPECT_EQ(impl->cursor(), 12);
}

TEST(Components, TextInputBackspace) {
    cc::ui::components::TextInputOptions opts;
    auto component = cc::ui::components::TextInput(opts);

    component->OnEvent(ftxui::Event::Character("a"));
    component->OnEvent(ftxui::Event::Character("b"));
    component->OnEvent(ftxui::Event::Backspace);

    auto rendered = render_to_plain_text(component->Render(), 20, 3);
    EXPECT_NE(rendered.find("a"), std::string::npos);
    EXPECT_EQ(rendered.find("ab"), std::string::npos);
}

TEST(Components, TextInputSelectAll) {
    cc::ui::components::TextInputOptions opts;
    auto component = cc::ui::components::TextInput(opts);

    component->OnEvent(ftxui::Event::Character("h"));
    component->OnEvent(ftxui::Event::Character("e"));
    component->OnEvent(ftxui::Event::Character("l"));
    component->OnEvent(ftxui::Event::Character("l"));
    component->OnEvent(ftxui::Event::Character("o"));
    // Ctrl+A = select all
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Character("\x01")));
    // Backspace should delete all
    component->OnEvent(ftxui::Event::Backspace);

    auto rendered = render_to_plain_text(component->Render(), 20, 3);
    // After delete all, placeholder should be visible
    EXPECT_NE(rendered.find("Type your message"), std::string::npos);
}

TEST(Components, TextInputUndoRedo) {
    cc::ui::components::TextInputOptions opts;
    auto component = cc::ui::components::TextInput(opts);

    component->OnEvent(ftxui::Event::Character("a"));
    component->OnEvent(ftxui::Event::Character("b"));
    component->OnEvent(ftxui::Event::Backspace); // delete 'b'

    // Ctrl+Z undo
    component->OnEvent(ftxui::Event::Character("\x1a"));

    auto rendered_undo = render_to_plain_text(component->Render(), 20, 3);
    // After undo, should have "ab" again
    EXPECT_NE(rendered_undo.find("ab"), std::string::npos);

    // Ctrl+Y redo
    component->OnEvent(ftxui::Event::Character("\x19"));

    auto rendered_redo = render_to_plain_text(component->Render(), 20, 3);
    // After redo, should be back to "a"
    EXPECT_NE(rendered_redo.find("a"), std::string::npos);
    EXPECT_EQ(rendered_redo.find("ab"), std::string::npos);
}

TEST(Components, TextInputMaskInput) {
    cc::ui::components::TextInputOptions opts;
    opts.mask_input = true;
    opts.mask_char = '*';
    auto component = cc::ui::components::TextInput(opts);

    component->OnEvent(ftxui::Event::Character("s"));
    component->OnEvent(ftxui::Event::Character("e"));
    component->OnEvent(ftxui::Event::Character("c"));
    component->OnEvent(ftxui::Event::Character("r"));
    component->OnEvent(ftxui::Event::Character("e"));
    component->OnEvent(ftxui::Event::Character("t"));

    auto rendered = render_to_plain_text(component->Render(), 20, 3);
    // Should show mask chars, not actual text
    EXPECT_EQ(rendered.find("secret"), std::string::npos);
    EXPECT_NE(rendered.find("******"), std::string::npos);
}

TEST(Components, TextInputInputFilter) {
    cc::ui::components::TextInputOptions opts;
    opts.input_filter = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
    auto component = cc::ui::components::TextInput(opts);

    // Digits should be accepted
    component->OnEvent(ftxui::Event::Character("1"));
    component->OnEvent(ftxui::Event::Character("2"));
    // Letters should be rejected
    component->OnEvent(ftxui::Event::Character("a"));
    component->OnEvent(ftxui::Event::Character("b"));

    auto rendered = render_to_plain_text(component->Render(), 20, 3);
    EXPECT_NE(rendered.find("12"), std::string::npos);
    EXPECT_EQ(rendered.find("a"), std::string::npos);
}

TEST(Components, TextInputSuggestionsDropdown) {
    cc::ui::components::TextInputOptions opts;
    opts.get_suggestions = [](const std::string& input, int, const cc::ui::components::PromptContext&) -> std::vector<cc::ui::components::Suggestion> {
        if (input.starts_with('/')) {
            return {
                {"/help", "/help", "Show help", cc::ui::components::SuggestionCategory::Command},
                {"/clear", "/clear", "Clear screen", cc::ui::components::SuggestionCategory::Command},
            };
        }
        return {};
    };
    auto component = cc::ui::components::TextInput(opts);

    component->OnEvent(ftxui::Event::Character("/"));
    auto rendered = render_to_plain_text(component->Render(), 60, 10);
    // Should show suggestions dropdown
    EXPECT_NE(rendered.find("Suggestions"), std::string::npos);
    EXPECT_NE(rendered.find("/help"), std::string::npos);
    EXPECT_NE(rendered.find("/clear"), std::string::npos);
}

TEST(Components, TextInputSuggestionAccept) {
    int accept_count = 0;
    std::string accepted_text;
    cc::ui::components::TextInputOptions opts;
    opts.get_suggestions = [](const std::string& input, int, const cc::ui::components::PromptContext&) -> std::vector<cc::ui::components::Suggestion> {
        if (input.starts_with('/')) {
            return {
                {"/help", "/help", "Show help", cc::ui::components::SuggestionCategory::Command},
            };
        }
        return {};
    };
    opts.on_change = [&](const std::string& text, const auto&) {
        accepted_text = text;
    };
    auto component = cc::ui::components::TextInput(opts);

    component->OnEvent(ftxui::Event::Character("/"));
    // Tab to select (next suggestion, only 1 so stays on 0)
    component->OnEvent(ftxui::Event::Tab);
    // Enter to accept
    component->OnEvent(ftxui::Event::Return);

    // After accept, text should be "/help"
    EXPECT_EQ(accepted_text, "/help");
}

TEST(Components, TextInputHistorySearch) {
    cc::ui::components::TextInputOptions opts;
    opts.show_history = true;
    auto component = cc::ui::components::TextInput(opts);

    // Add some history by typing and submitting
    component->OnEvent(ftxui::Event::Character("hello world"));
    component->OnEvent(ftxui::Event::Return);
    component->OnEvent(ftxui::Event::Character("hello there"));
    component->OnEvent(ftxui::Event::Return);
    component->OnEvent(ftxui::Event::Character("goodbye"));
    component->OnEvent(ftxui::Event::Return);

    // Enter search mode (Ctrl+R)
    component->OnEvent(ftxui::Event::Character("\x12"));
    // Type search query
    component->OnEvent(ftxui::Event::Character("h"));
    component->OnEvent(ftxui::Event::Character("e"));
    component->OnEvent(ftxui::Event::Character("l"));

    auto rendered = render_to_plain_text(component->Render(), 60, 10);
    // Should show search mode UI
    EXPECT_NE(rendered.find("reverse-i-search"), std::string::npos);
    EXPECT_NE(rendered.find("`hel`"), std::string::npos);
}

TEST(Components, TextInputHistorySearchAccept) {
    cc::ui::components::TextInputOptions opts;
    opts.show_history = true;
    std::string result_text;
    opts.on_submit = [&](const std::string& text, const auto&) {
        result_text = text;
    };
    auto component = cc::ui::components::TextInput(opts);

    // Add some history
    component->OnEvent(ftxui::Event::Character("apple banana"));
    component->OnEvent(ftxui::Event::Return);
    component->OnEvent(ftxui::Event::Character("cherry date"));
    component->OnEvent(ftxui::Event::Return);

    // Enter search mode
    component->OnEvent(ftxui::Event::Character("\x12"));
    // Search for "cherry"
    component->OnEvent(ftxui::Event::Character("c"));
    component->OnEvent(ftxui::Event::Character("h"));
    component->OnEvent(ftxui::Event::Character("e"));
    // Accept with Enter
    component->OnEvent(ftxui::Event::Return);

    // After accepting, submit should give "cherry date"
    // But Enter in search mode just populates the buffer; need another Enter to submit
    // Let's submit now
    component->OnEvent(ftxui::Event::Return);
    EXPECT_EQ(result_text, "cherry date");
}

TEST(Components, TextInputArrowNavigation) {
    auto impl = cc::ui::components::MakeTextInputCore({});

    impl->insert_char('a');
    impl->insert_char('b');
    impl->insert_char('c');
    ASSERT_EQ(impl->text(), "abc");
    ASSERT_EQ(impl->cursor(), 3);

    // Move left twice, then insert 'X'
    impl->move_cursor(-1, false);
    impl->move_cursor(-1, false);
    EXPECT_EQ(impl->cursor(), 1);
    impl->insert_char('X');

    EXPECT_EQ(impl->text(), "aXbc");
    EXPECT_EQ(impl->cursor(), 2);

    // Also verify via rendered output (with ANSI stripped)
    auto rendered = strip_ansi(render_to_plain_text(impl->Render(), 20, 3));
    EXPECT_NE(rendered.find("aXbc"), std::string::npos);
}

TEST(Components, TextInputHomeEnd) {
    auto impl = cc::ui::components::MakeTextInputCore({});

    impl->insert_char('h');
    impl->insert_char('e');
    impl->insert_char('l');
    impl->insert_char('l');
    impl->insert_char('o');
    ASSERT_EQ(impl->text(), "hello");
    ASSERT_EQ(impl->cursor(), 5);

    // Home, then insert '!'
    impl->move_home(false);
    EXPECT_EQ(impl->cursor(), 0);
    impl->insert_char('!');
    EXPECT_EQ(impl->text(), "!hello");
    EXPECT_EQ(impl->cursor(), 1);

    // End, then insert '?'
    impl->move_end(false);
    EXPECT_EQ(impl->cursor(), 6);
    impl->insert_char('?');
    EXPECT_EQ(impl->text(), "!hello?");
    EXPECT_EQ(impl->cursor(), 7);

    // Also verify rendered text (strip ANSI for text content check)
    auto rendered = strip_ansi(render_to_plain_text(impl->Render(), 20, 3));
    EXPECT_NE(rendered.find("!hello?"), std::string::npos);
}

TEST(Components, TextInputSubmitCallback) {
    std::string submitted_text;
    cc::ui::components::TextInputOptions opts;
    opts.multiline = false;
    opts.on_submit = [&](const std::string& text, const auto&) {
        submitted_text = text;
    };
    auto component = cc::ui::components::TextInput(opts);

    component->OnEvent(ftxui::Event::Character("h"));
    component->OnEvent(ftxui::Event::Character("i"));
    component->OnEvent(ftxui::Event::Return);

    EXPECT_EQ(submitted_text, "hi");
}

TEST(Components, TextInputEscapeCallback) {
    bool escape_called = false;
    cc::ui::components::TextInputOptions opts;
    opts.on_escape = [&]() { escape_called = true; };
    auto component = cc::ui::components::TextInput(opts);

    component->OnEvent(ftxui::Event::Escape);
    EXPECT_TRUE(escape_called);
}

TEST(Components, TextInputLineNumbers) {
    cc::ui::components::TextInputOptions opts;
    opts.multiline = true;
    opts.show_line_numbers = true;
    auto impl = cc::ui::components::MakeTextInputCore(opts);

    // Two lines so show_line_numbers actually renders (guard: total_lines > 1)
    impl->PasteText("line 1\nline 2");

    auto rendered = strip_ansi(render_to_plain_text(impl->Render(), 40, 5));
    EXPECT_NE(rendered.find(" 1 "), std::string::npos);
    EXPECT_NE(rendered.find(" 2 "), std::string::npos);
    EXPECT_NE(rendered.find("line 1"), std::string::npos);
    EXPECT_NE(rendered.find("line 2"), std::string::npos);
}

TEST(Components, TextInputPasteText) {
    cc::ui::components::TextInputOptions opts;
    std::shared_ptr<cc::ui::components::TextInputImpl> impl;
    auto component = cc::ui::components::TextInput(opts, &impl);

    ASSERT_NE(impl, nullptr);
    impl->PasteText("pasted text");

    auto rendered = render_to_plain_text(component->Render(), 40, 3);
    EXPECT_NE(rendered.find("pasted text"), std::string::npos);
}

TEST(Components, TextInputDeleteChar) {
    auto impl = cc::ui::components::MakeTextInputCore({});

    impl->insert_char('a');
    impl->insert_char('b');
    impl->insert_char('c');
    ASSERT_EQ(impl->text(), "abc");
    ASSERT_EQ(impl->cursor(), 3);

    // Move left once (cursor between 'b' and 'c'), then Delete
    // removes the char AFTER the cursor ('c') → "ab"
    impl->move_cursor(-1, false);
    EXPECT_EQ(impl->cursor(), 2);
    impl->delete_char();

    EXPECT_EQ(impl->text(), "ab");
    EXPECT_EQ(impl->cursor(), 2);

    // Move left once more (cursor between 'a' and 'b'), delete → "a"
    impl->move_cursor(-1, false);
    impl->delete_char();
    EXPECT_EQ(impl->text(), "a");
    EXPECT_EQ(impl->cursor(), 1);

    // Also verify via rendered output (strip ANSI for text check)
    auto rendered = strip_ansi(render_to_plain_text(impl->Render(), 20, 3));
    EXPECT_NE(rendered.find("a"), std::string::npos);
    EXPECT_EQ(rendered.find("ab"), std::string::npos);
    EXPECT_EQ(rendered.find("abc"), std::string::npos);
}

TEST(Components, TextInputHistoryUpDown) {
    cc::ui::components::TextInputOptions opts;
    opts.multiline = false;
    auto component = cc::ui::components::TextInput(opts);

    // Add some history by submitting
    component->OnEvent(ftxui::Event::Character("first"));
    component->OnEvent(ftxui::Event::Return);
    component->OnEvent(ftxui::Event::Character("second"));
    component->OnEvent(ftxui::Event::Return);

    // Arrow up should go back in history
    component->OnEvent(ftxui::Event::ArrowUp);

    auto rendered_up = render_to_plain_text(component->Render(), 20, 3);
    EXPECT_NE(rendered_up.find("second"), std::string::npos);

    // Arrow up again
    component->OnEvent(ftxui::Event::ArrowUp);

    auto rendered_up2 = render_to_plain_text(component->Render(), 20, 3);
    EXPECT_NE(rendered_up2.find("first"), std::string::npos);

    // Arrow down should go forward
    component->OnEvent(ftxui::Event::ArrowDown);

    auto rendered_down = render_to_plain_text(component->Render(), 20, 3);
    EXPECT_NE(rendered_down.find("second"), std::string::npos);
}

TEST(WizardDialog, RendersStepFactoryContent) {
    using namespace cc::ui::wizard_dialog;

    bool factory_called = false;
    WizardConfig cfg;
    cfg.title = "Setup";
    StepsFn builder = [&](WizardContext& ctx) {
        WizardStep step;
        step.id = "custom";
        step.title = "Custom";
        step.render = [&](WizardContext&) -> ftxui::Element {
            factory_called = true;
            return ftxui::vbox({
                ftxui::text("Custom"),
                ftxui::text("custom factory body"),
            });
        };
        ctx.steps.push_back(std::move(step));
    };

    auto component = MakeWizard(std::move(cfg), std::move(builder));
    auto rendered = render_to_plain_text(component->Render(), 80, 12);
    EXPECT_TRUE(factory_called);
    EXPECT_NE(rendered.find("custom factory body"), std::string::npos);
    // Description field doesn't exist in current WizardStep, so old check for
    // description-absent is moot; we verify the render() body instead.
    EXPECT_NE(rendered.find("Setup"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════════
// AppRuntime: integration tests that exercise the AppAdapter end-to-end
// ═══════════════════════════════════════════════════════════════════════════════

TEST(AppRuntime, CommandsAndStatusRenderWithoutTerminalLoop) {
    cc::core::ToolRegistry tools;
    cc::core::QueryEngineConfig config;
    config.context_window.auto_compact = false;
    config.cwd = fs::temp_directory_path().string();
    cc::core::QueryEngine engine(std::move(config), tools);

    cc::commands::AppCommandRegistry commands;
    const auto storage_root = fs::temp_directory_path() /
        ("cc_repl_ui_app_runtime_test_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cc::utils::SessionStorage storage(storage_root);

    bool exited = false;
    auto app = ftxui::Make<cc::ui::AppAdapter>(
        &engine,
        &commands,
        &storage,
        [&] {
            exited = true;
        });

    // --- Initial render: status bar + system message + prompt input ---
    app->SyncState();
    auto initial = render_to_plain_text(app->Render(), 120, 28);
    // Default model appears in the status bar
    EXPECT_NE(initial.find("claude-sonnet"), std::string::npos);
    // System prompt message is shown
    EXPECT_NE(initial.find("You are Claude"), std::string::npos);
    // Prompt input indicator is present
    EXPECT_NE(initial.find(">"), std::string::npos);

    // --- /model haiku-runtime: changes model in status bar ---
    app->HandleCommand("/model haiku-runtime");
    auto switched = render_to_plain_text(app->Render(), 120, 28);
    EXPECT_NE(switched.find("haiku-runtime"), std::string::npos);

    // --- /cost: sets status tip (visible via testing accessor) ---
    app->HandleCommand("/cost");
    auto status_msg = app->status_message_for_testing();
    EXPECT_NE(status_msg.find("Cost: $"), std::string::npos);
    EXPECT_NE(status_msg.find("In:"), std::string::npos);
    EXPECT_NE(status_msg.find("Out:"), std::string::npos);
    EXPECT_NE(status_msg.find("Ctx:"), std::string::npos);

    // --- /clear: clears conversation, status bar retains current model ---
    app->HandleCommand("/clear");
    auto cleared = render_to_plain_text(app->Render(), 120, 28);
    EXPECT_NE(cleared.find("haiku-runtime"), std::string::npos);  // status bar model unchanged

    // --- /exit: triggers on_exit callback ---
    app->HandleCommand("/exit");
    EXPECT_TRUE(exited);

    fs::remove_all(storage_root);
}

TEST(AppRuntime, CtrlCWithoutRunningQueryRequestsExit) {
    cc::core::ToolRegistry tools;
    cc::core::QueryEngineConfig config;
    config.context_window.auto_compact = false;
    config.cwd = fs::temp_directory_path().string();
    cc::core::QueryEngine engine(std::move(config), tools);

    cc::commands::AppCommandRegistry commands;
    const auto storage_root = fs::temp_directory_path() /
        ("cc_repl_ui_interrupt_test_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cc::utils::SessionStorage storage(storage_root);

    bool exited = false;
    auto app = ftxui::Make<cc::ui::AppAdapter>(
        &engine,
        &commands,
        &storage,
        [&] {
            exited = true;
        });

    EXPECT_TRUE(app->OnEvent(ftxui::Event::Special("\x03")));
    EXPECT_TRUE(exited);

    fs::remove_all(storage_root);
}

TEST(AppRuntime, CtrlCWhileStreamingQueryCancelsWithoutExiting) {
    LocalChunkedAnthropicStreamServer server;
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
        ("cc_repl_ui_stream_cancel_test_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cc::utils::SessionStorage storage(storage_root);

    bool exited = false;
    auto app = ftxui::Make<cc::ui::AppAdapter>(
        &engine,
        &commands,
        &storage,
        [&] {
            exited = true;
        });
    ReleaseAfterCancelGuard release_guard{server};

    app->HandleSubmit("show streaming cancel behavior");
    ASSERT_TRUE(server.wait_for_first_delta());
    ASSERT_TRUE(wait_until([&] {
        auto rendered = render_to_plain_text(app->Render(), 120, 32);
        return rendered.find("partial UI stream") != std::string::npos;
    }, std::chrono::seconds(2)));
    EXPECT_TRUE(app->is_loading_for_testing());
    EXPECT_TRUE(app->is_query_running_for_testing());

    EXPECT_TRUE(app->OnEvent(ftxui::Event::Special("\x03")));
    EXPECT_FALSE(exited);
    EXPECT_TRUE(app->is_loading_for_testing());
    EXPECT_TRUE(app->is_query_running_for_testing());
    EXPECT_EQ(app->status_message_for_testing(), "Cancelling...");

    auto cancelling = render_to_plain_text(app->Render(), 120, 32);
    EXPECT_NE(cancelling.find("Cancelling..."), std::string::npos);
    EXPECT_NE(cancelling.find("partial UI stream"), std::string::npos);

    server.release_after_cancel();
    EXPECT_TRUE(wait_until([&] {
        (void)app->Render();
        return !app->is_query_running_for_testing();
    }, std::chrono::seconds(3)));
    (void)app->Render();
    EXPECT_FALSE(app->is_loading_for_testing());

    fs::remove_all(storage_root);
}

TEST(AppRuntime, StreamingToolUseShowsSpinnerAndLoadingState) {
    LocalToolUseAnthropicStreamServer server;
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
        ("cc_repl_ui_stream_tool_test_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cc::utils::SessionStorage storage(storage_root);

    auto app = ftxui::Make<cc::ui::AppAdapter>(
        &engine,
        &commands,
        &storage,
        [] {});
    ReleaseAfterToolPreviewGuard release_guard{server};

    EXPECT_FALSE(app->is_loading_for_testing());
    EXPECT_FALSE(app->is_query_running_for_testing());

    app->HandleSubmit("show streaming tool use");
    ASSERT_TRUE(server.wait_for_tool_delta());

    // While streaming: query is running, spinner is visible, tool name shown in spinner verb
    ASSERT_TRUE(wait_until([&] {
        (void)app->Render();
        return app->is_query_running_for_testing();
    }, std::chrono::seconds(2)));
    EXPECT_TRUE(app->is_loading_for_testing());
    EXPECT_TRUE(app->is_query_running_for_testing());

    // Rendered output should contain the tool name in the spinner line
    auto during = strip_ansi(render_to_plain_text(app->Render(), 140, 36));
    EXPECT_NE(during.find("Bash"), std::string::npos);

    server.release_after_preview();
    EXPECT_TRUE(wait_until([&] {
        (void)app->Render();
        return !app->is_query_running_for_testing();
    }, std::chrono::seconds(4)));
    (void)app->Render();
    EXPECT_FALSE(app->is_loading_for_testing());

    fs::remove_all(storage_root);
}

TEST(AppRuntime, StreamingThinkingShowsSpinnerAndFinalContent) {
    LocalThinkingAnthropicStreamServer server;
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
        ("cc_repl_ui_stream_thinking_test_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cc::utils::SessionStorage storage(storage_root);

    auto app = ftxui::Make<cc::ui::AppAdapter>(
        &engine,
        &commands,
        &storage,
        [] {});
    ReleaseAfterThinkingPreviewGuard release_guard{server};

    EXPECT_FALSE(app->is_loading_for_testing());
    EXPECT_FALSE(app->is_query_running_for_testing());

    app->HandleSubmit("show streaming thinking");
    ASSERT_TRUE(server.wait_for_thinking_delta());

    // While streaming: query is running, spinner shows Thinking mode
    ASSERT_TRUE(wait_until([&] {
        (void)app->Render();
        return app->is_query_running_for_testing();
    }, std::chrono::seconds(2)));
    EXPECT_TRUE(app->is_loading_for_testing());
    EXPECT_TRUE(app->is_query_running_for_testing());

    // Rendered output should contain "Thinking" (in spinner line)
    auto during = render_to_plain_text(app->Render(), 140, 36);
    EXPECT_NE(during.find("Thinking"), std::string::npos);

    server.release_after_preview();
    EXPECT_TRUE(wait_until([&] {
        (void)app->Render();
        return !app->is_query_running_for_testing();
    }, std::chrono::seconds(4)));
    auto done = render_to_plain_text(app->Render(), 140, 36);
    EXPECT_FALSE(app->is_loading_for_testing());
    // Final message contains the visible answer text
    EXPECT_NE(done.find("visible answer after thinking"), std::string::npos);

    fs::remove_all(storage_root);
}

TEST(AppRuntime, PermissionCallbackRendersAndResolvesUserChoices) {
    cc::core::ToolRegistry tools;
    cc::core::QueryEngineConfig config;
    config.context_window.auto_compact = false;
    config.cwd = fs::temp_directory_path().string();
    cc::core::QueryEngine engine(std::move(config), tools);

    cc::commands::AppCommandRegistry commands;
    const auto storage_root = fs::temp_directory_path() /
        ("cc_repl_ui_permission_dialog_test_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cc::utils::SessionStorage storage(storage_root);

    auto app = ftxui::Make<cc::ui::AppAdapter>(
        &engine,
        &commands,
        &storage,
        [] {});
    auto permission_callback = app->get_permission_callback();

    std::atomic<bool> allow_done{false};
    std::atomic<bool> allow_result{false};
    std::jthread allow_worker([&] {
        allow_result.store(permission_callback("Bash", "Run npm test"), std::memory_order_release);
        allow_done.store(true, std::memory_order_release);
    });

    const bool allow_prompt_shown = wait_until([&] {
        auto rendered = strip_ansi(render_to_plain_text(app->Render(), 120, 34));
        return rendered.find("Permission Required") != std::string::npos &&
               rendered.find("Tool:") != std::string::npos &&
               rendered.find("Bash") != std::string::npos &&
               rendered.find("Run npm test") != std::string::npos;
    }, std::chrono::milliseconds(1000));
    EXPECT_TRUE(allow_prompt_shown);
    EXPECT_TRUE(app->OnEvent(ftxui::Event::Character('y')));
    EXPECT_TRUE(wait_until([&] { return allow_done.load(std::memory_order_acquire); },
                           std::chrono::milliseconds(1000)));
    EXPECT_TRUE(allow_result.load(std::memory_order_acquire));
    allow_worker.join();

    std::atomic<bool> deny_done{false};
    std::atomic<bool> deny_result{true};
    std::jthread deny_worker([&] {
        deny_result.store(permission_callback("Write", "Modify src/main.cpp"), std::memory_order_release);
        deny_done.store(true, std::memory_order_release);
    });

    const bool deny_prompt_shown = wait_until([&] {
        auto rendered = strip_ansi(render_to_plain_text(app->Render(), 120, 34));
        return rendered.find("Permission Required") != std::string::npos &&
               rendered.find("Tool:") != std::string::npos &&
               rendered.find("Write") != std::string::npos &&
               rendered.find("Modify src/main.cpp") != std::string::npos;
    }, std::chrono::milliseconds(1000));
    EXPECT_TRUE(deny_prompt_shown);
    EXPECT_TRUE(app->OnEvent(ftxui::Event::Character('n')));
    EXPECT_TRUE(wait_until([&] { return deny_done.load(std::memory_order_acquire); },
                           std::chrono::milliseconds(1000)));
    EXPECT_FALSE(deny_result.load(std::memory_order_acquire));
    deny_worker.join();

    std::atomic<bool> always_done{false};
    std::atomic<bool> always_result{false};
    std::jthread always_worker([&] {
        always_result.store(permission_callback("Read", "Read package.json"), std::memory_order_release);
        always_done.store(true, std::memory_order_release);
    });

    const bool always_prompt_shown = wait_until([&] {
        auto rendered = strip_ansi(render_to_plain_text(app->Render(), 120, 34));
        return rendered.find("Permission Required") != std::string::npos &&
               rendered.find("Tool:") != std::string::npos &&
               rendered.find("Read") != std::string::npos &&
               rendered.find("Read package.json") != std::string::npos;
    }, std::chrono::milliseconds(1000));
    EXPECT_TRUE(always_prompt_shown);
    EXPECT_TRUE(app->OnEvent(ftxui::Event::Character('a')));
    EXPECT_TRUE(wait_until([&] { return always_done.load(std::memory_order_acquire); },
                           std::chrono::milliseconds(1000)));
    EXPECT_TRUE(always_result.load(std::memory_order_acquire));
    always_worker.join();

    std::atomic<bool> repeated_done{false};
    std::atomic<bool> repeated_result{false};
    std::jthread repeated_worker([&] {
        repeated_result.store(permission_callback("Read", "Read package-lock.json"), std::memory_order_release);
        repeated_done.store(true, std::memory_order_release);
    });

    const bool completed_without_prompt = wait_until(
        [&] { return repeated_done.load(std::memory_order_acquire); },
        std::chrono::milliseconds(200));
    EXPECT_TRUE(completed_without_prompt);
    if (!completed_without_prompt) {
        EXPECT_TRUE(wait_until([&] {
            auto rendered = render_to_plain_text(app->Render(), 120, 34);
            return rendered.find("Permission Required") != std::string::npos &&
                   rendered.find("Read package-lock.json") != std::string::npos;
        }, std::chrono::milliseconds(1000)));
        EXPECT_TRUE(app->OnEvent(ftxui::Event::Character('y')));
        EXPECT_TRUE(wait_until([&] { return repeated_done.load(std::memory_order_acquire); },
                               std::chrono::milliseconds(1000)));
    }
    EXPECT_TRUE(repeated_result.load(std::memory_order_acquire));
    repeated_worker.join();

    fs::remove_all(storage_root);
}

TEST(AppRuntime, RenderMessageShowsThinkingContent) {
    cc::core::AssistantMessage assistant;
    assistant.content.push_back(cc::core::ThinkingBlock{
        .thinking = "private reasoning preview",
        .signature = "sig-1",
    });

    auto rendered = render_to_plain_text(
        cc::ui::RenderMessage(cc::core::Message{std::move(assistant)}), 140, 24);

    EXPECT_NE(rendered.find("Thinking"), std::string::npos);
    EXPECT_NE(rendered.find("private reasoning preview"), std::string::npos);
}

TEST(AppRuntime, RenderMessageShowsToolUseContent) {
    cc::core::AssistantMessage assistant;
    assistant.content.push_back(cc::core::ToolUseBlock{
        .id = cc::core::ToolUseId{"tool-ui-1"},
        .name = "Bash",
        .input_json = R"({"command":"npm test"})",
    });

    auto rendered = render_to_plain_text(
        cc::ui::RenderMessage(cc::core::Message{std::move(assistant)}), 140, 24);

    EXPECT_NE(rendered.find("Bash"), std::string::npos);
}

TEST(AppRuntime, RenderMessageShowsAssistantText) {
    cc::core::AssistantMessage assistant;
    assistant.content.push_back(cc::core::TextBlock{"visible assistant answer"});

    auto rendered = render_to_plain_text(
        cc::ui::RenderMessage(cc::core::Message{std::move(assistant)}), 140, 24);

    EXPECT_NE(rendered.find("visible assistant answer"), std::string::npos);
}

TEST(AppRuntime, RenderMessageShowsUserMessage) {
    cc::core::UserMessage user;
    user.content.push_back(cc::core::TextBlock{"hello world"});

    auto rendered = render_to_plain_text(
        cc::ui::RenderMessage(cc::core::Message{std::move(user)}), 140, 24);

    EXPECT_NE(rendered.find("hello world"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════════
// cc.ui.panels: panel data models and state transitions
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Panels, PanelTypeNamesAreStable) {
    EXPECT_EQ(cc::ui::panel_name(cc::ui::PanelType::Settings), "Settings");
    EXPECT_EQ(cc::ui::panel_name(cc::ui::PanelType::Permissions), "Permissions");
}

TEST(Panels, SettingsPanelFiltersAndUpdatesEntries) {
    cc::ui::SettingsPanel panel;
    panel.load({
        {.key = "model", .value = "sonnet", .description = "Model", .category = "core", .is_readonly = false, .allowed_values = {}},
        {.key = "theme", .value = "dark", .description = "Theme", .category = "ui", .is_readonly = false, .allowed_values = {"dark", "light"}},
    });

    panel.set_filter("model");
    auto filtered = panel.filtered_entries();
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0]->key, "model");

    auto updated = panel.update("theme", "light");
    EXPECT_TRUE(updated.has_value());
    auto invalid = panel.update("theme", "blue");
    EXPECT_FALSE(invalid.has_value());
}

TEST(Panels, McpPanelComputesStatusSummary) {
    cc::ui::McpPanel panel;
    auto now = std::chrono::steady_clock::now();
    panel.set_servers({
        {.name = "one", .uri = "stdio://one", .status = cc::ui::McpStatus::Connected, .capabilities = {}, .error_message = std::nullopt, .tool_count = 0, .last_heartbeat = now},
        {.name = "two", .uri = "stdio://two", .status = cc::ui::McpStatus::Disconnected, .capabilities = {}, .error_message = std::nullopt, .tool_count = 0, .last_heartbeat = now},
    });

    EXPECT_EQ(panel.status_summary(), "1/2 connected");
    EXPECT_EQ(cc::ui::McpPanel::status_icon(cc::ui::McpStatus::Error), "✗");
}

TEST(Panels, TasksPanelTracksTaskLifecycle) {
    cc::ui::TasksPanel panel;
    panel.add_task({.id = "task-1", .description = "Run tests", .status = cc::ui::TaskStatus::Running, .progress = 0.0, .error = std::nullopt, .created_at = std::chrono::system_clock::now(), .completed_at = std::nullopt});
    EXPECT_EQ(panel.active_count(), 1u);

    panel.update_progress("task-1", 0.5);
    panel.complete_task("task-1");
    EXPECT_EQ(panel.active_count(), 0u);
}

TEST(Panels, DiffPanelAggregatesStats) {
    cc::ui::DiffPanel panel;
    panel.set_diffs({{.file_path = "main.cpp", .hunks = {}, .additions = 3, .deletions = 1, .is_binary = false, .is_new_file = false, .is_deleted = false}});

    auto [additions, deletions] = panel.total_stats();
    EXPECT_EQ(additions, 3u);
    EXPECT_EQ(deletions, 1u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// cc.ui.messages: message parsing and renderable views
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Messages, ParseMarkdownRecognizesHeadingListAndCodeBlock) {
    auto blocks = cc::ui::parse_markdown("# Title\n- item\n```cpp\nint main() {}\n```");

    ASSERT_EQ(blocks.size(), 3u);
    EXPECT_EQ(blocks[0].type, cc::ui::BlockType::Heading);
    EXPECT_EQ(blocks[1].type, cc::ui::BlockType::List);
    EXPECT_EQ(blocks[2].type, cc::ui::BlockType::CodeBlock);
    EXPECT_EQ(blocks[2].language, "cpp");
}

TEST(Messages, ToolUseViewFormatsStatusAndDuration) {
    cc::ui::ToolUseView view{
        .tool_name = "bash",
        .tool_input = "{}",
        .tool_output = "done",
        .status = cc::ui::ToolStatus::Success,
        .started_at = std::chrono::system_clock::now(),
        .completed_at = std::nullopt,
        .expanded = false,
        .duration_ms = 1200,
    };

    EXPECT_EQ(view.status_icon(), "✓");
    EXPECT_EQ(view.duration_display(), "1.2s");
    view.toggle_expand();
    EXPECT_TRUE(view.expanded);
}

TEST(Messages, ErrorViewFormatsErrorCode) {
    cc::ui::ErrorView error{
        .message = "failed",
        .error_code = "E_TEST",
        .timestamp = std::chrono::system_clock::now(),
        .suggestion = std::nullopt,
        .is_retryable = false,
    };

    EXPECT_EQ(error.formatted(), "[E_TEST] failed");
}

TEST(Messages, ThinkingViewCanToggleCollapse) {
    cc::ui::ThinkingView view{
        .content = "first line\nsecond line",
        .timestamp = std::chrono::system_clock::now(),
        .collapsed = true,
        .token_count = 0,
    };

    EXPECT_TRUE(view.summary().starts_with("first line"));
    view.toggle_collapse();
    EXPECT_FALSE(view.collapsed);
}

// ═══════════════════════════════════════════════════════════════════════════════
// cc.ui.prompt_input: prompt buffer, history, typeahead, vim behavior
// ═══════════════════════════════════════════════════════════════════════════════

TEST(PromptInput, InputBufferInsertMoveAndDelete) {
    cc::ui::InputBuffer buffer;
    buffer.insert("Hello");
    EXPECT_EQ(buffer.content(), "Hello");
    EXPECT_EQ(buffer.cursor().col, 5u);

    buffer.move_cursor(cc::ui::VimMotion::Left);
    EXPECT_EQ(buffer.cursor().col, 4u);
    buffer.delete_char();
    EXPECT_EQ(buffer.content(), "Hell");
}

TEST(PromptInput, InputBufferBackspaceDeletesWholeUtf8Codepoint) {
    cc::ui::InputBuffer buffer;
    buffer.insert("你a");

    buffer.backspace();
    EXPECT_EQ(buffer.content(), "你");
    buffer.backspace();
    EXPECT_TRUE(buffer.empty());
}

TEST(PromptInput, InputBufferSupportsMultiLineSelections) {
    cc::ui::InputBuffer buffer;
    buffer.insert("one\ntwo");

    auto text = buffer.get_selection_text({.start = {.line = 0, .col = 1}, .end = {.line = 1, .col = 2}});
    EXPECT_EQ(text, "ne\ntw");
}

TEST(PromptInput, HistoryManagerNavigatesAndSearches) {
    cc::ui::HistoryManager history;
    history.push("first command");
    history.push("second command");

    auto prev = history.navigate_up();
    ASSERT_TRUE(prev.has_value());
    EXPECT_EQ(*prev, "second command");

    auto matches = history.search("first");
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0], "first command");
}

TEST(PromptInput, TypeaheadSuggestsSlashCommandsAndSelection) {
    cc::ui::Typeahead typeahead;
    typeahead.set_commands({
        {.text = "commit", .description = "Create commit", .category = "command"},
        {.text = "config", .description = "Edit config", .category = "command"},
        {.text = "help", .description = "Show help", .category = "command"},
    });

    auto suggestions = typeahead.suggest("/co");
    ASSERT_EQ(suggestions.size(), 2u);
    typeahead.select_next(suggestions.size());
    ASSERT_TRUE(typeahead.selected_index().has_value());
    EXPECT_EQ(*typeahead.selected_index(), 0u);
}

TEST(PromptInput, VimHandlerProcessesNormalModeCommands) {
    cc::ui::InputBuffer buffer;
    buffer.insert("hello");
    cc::ui::VimHandler vim;
    vim.set_mode(cc::ui::VimMode::Normal);

    EXPECT_TRUE(vim.process_key('0', buffer));
    EXPECT_EQ(buffer.cursor().col, 0u);
    EXPECT_TRUE(vim.process_key('$', buffer));
    EXPECT_EQ(buffer.cursor().col, 5u);
    EXPECT_TRUE(vim.process_key('i', buffer));
    EXPECT_EQ(vim.mode(), cc::ui::VimMode::Insert);
}

TEST(PromptInput, VimHandlerDefaultsToInsertMode) {
    cc::ui::InputBuffer buffer;
    buffer.insert("abc");
    cc::ui::VimHandler vim;

    EXPECT_EQ(vim.mode(), cc::ui::VimMode::Insert);
    EXPECT_FALSE(vim.process_key('x', buffer));
    EXPECT_EQ(buffer.content(), "abc");
}

TEST(PromptInput, VimEscapeMovesCursorLeftWhenLeavingInsertMode) {
    cc::ui::InputBuffer buffer;
    buffer.insert("abc");
    cc::ui::VimHandler vim;

    EXPECT_TRUE(vim.process_key('\x1b', buffer));

    EXPECT_EQ(vim.mode(), cc::ui::VimMode::Normal);
    EXPECT_EQ(buffer.cursor().col, 2u);
}

TEST(PromptInput, VimYankLinePasteIsLinewise) {
    cc::ui::InputBuffer buffer;
    buffer.insert("one\ntwo");
    buffer.move_cursor(cc::ui::VimMotion::Up);
    cc::ui::VimHandler vim;
    vim.set_mode(cc::ui::VimMode::Normal);

    EXPECT_TRUE(vim.process_key('y', buffer));
    EXPECT_TRUE(vim.process_key('y', buffer));
    EXPECT_TRUE(vim.process_key('p', buffer));

    EXPECT_EQ(buffer.content(), "one\none\ntwo");
}

TEST(Markdown, OrderedListSupportsMultiDigitNumbers) {
    auto rendered = render_to_plain_text(cc::ui::render_markdown("10. tenth"));

    EXPECT_NE(rendered.find("10."), std::string::npos);
    EXPECT_NE(rendered.find("tenth"), std::string::npos);
    EXPECT_EQ(rendered.find("1. . tenth"), std::string::npos);
}

TEST(Markdown, CodeBlockDoesNotInjectLineNumbers) {
    auto rendered = render_to_plain_text(cc::ui::render_markdown("```txt\nfoo\nbar\n```"));

    EXPECT_NE(rendered.find("foo"), std::string::npos);
    EXPECT_NE(rendered.find("bar"), std::string::npos);
    EXPECT_EQ(rendered.find("  1 "), std::string::npos);
    EXPECT_EQ(rendered.find("  2 "), std::string::npos);
}

TEST(Markdown, ParsesGfmTablesWithoutRenderingSeparatorAsParagraph) {
    auto rendered = render_to_plain_text(cc::ui::render_markdown("| A | B |\n|---|---|\n| 1 | 2 |"));

    EXPECT_NE(rendered.find("A"), std::string::npos);
    EXPECT_NE(rendered.find("B"), std::string::npos);
    EXPECT_NE(rendered.find("1"), std::string::npos);
    EXPECT_NE(rendered.find("2"), std::string::npos);
    EXPECT_EQ(rendered.find("---"), std::string::npos);
}
