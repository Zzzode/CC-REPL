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

TEST(AppRuntime, CommandsNavigationAndStatusRenderWithoutTerminalLoop) {
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
    auto app = ftxui::Make<cc::ui::AppComponent>(
        &engine,
        &commands,
        &storage,
        [&] {
            exited = true;
        });

    app->SyncMessages();
    auto initial = render_to_plain_text(app->Render(), 120, 28);
    EXPECT_NE(initial.find("CC-REPL (C++23)"), std::string::npos);
    EXPECT_NE(initial.find("Resume"), std::string::npos);
    EXPECT_NE(initial.find("All"), std::string::npos);

    app->HandleCommand("/stats");
    auto stats = render_to_plain_text(app->Render(), 120, 34);
    EXPECT_NE(stats.find("Stats Overview"), std::string::npos);
    EXPECT_NE(stats.find("Favorite model:"), std::string::npos);
    EXPECT_NE(stats.find("You're using C++23!"), std::string::npos);

    EXPECT_TRUE(app->OnEvent(ftxui::Event::Tab));
    auto models = render_to_plain_text(app->Render(), 120, 34);
    EXPECT_NE(models.find("Model Usage"), std::string::npos);

    EXPECT_TRUE(app->OnEvent(ftxui::Event::Character('r')));
    auto date_range = render_to_plain_text(app->Render(), 120, 34);
    EXPECT_NE(date_range.find("Last 7 days"), std::string::npos);

    EXPECT_TRUE(app->OnEvent(ftxui::Event::Escape));
    auto closed_stats = render_to_plain_text(app->Render(), 120, 28);
    EXPECT_EQ(closed_stats.find("Stats Overview"), std::string::npos);

    app->HandleCommand("/model haiku-runtime");
    auto switched = render_to_plain_text(app->Render(), 120, 28);
    EXPECT_NE(switched.find("Switched to: haiku-runtime"), std::string::npos);

    app->HandleCommand("/model");
    auto model_status = render_to_plain_text(app->Render(), 120, 28);
    EXPECT_NE(model_status.find("Model: haiku-runtime"), std::string::npos);

    app->HandleCommand("/cost");
    auto cost_status = render_to_plain_text(app->Render(), 120, 28);
    EXPECT_NE(cost_status.find("Cost: $"), std::string::npos);
    EXPECT_NE(cost_status.find("In: "), std::string::npos);
    EXPECT_NE(cost_status.find("Out: "), std::string::npos);

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
    auto app = ftxui::Make<cc::ui::AppComponent>(
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
    auto app = ftxui::Make<cc::ui::AppComponent>(
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

TEST(AppRuntime, StreamingToolUseRendersRunningPreview) {
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

    auto app = ftxui::Make<cc::ui::AppComponent>(
        &engine,
        &commands,
        &storage,
        [] {});
    ReleaseAfterToolPreviewGuard release_guard{server};

    app->HandleSubmit("show streaming tool use");
    ASSERT_TRUE(server.wait_for_tool_delta());
    ASSERT_TRUE(wait_until([&] {
        auto rendered = render_to_plain_text(app->Render(), 140, 36);
        return rendered.find("Bash") != std::string::npos &&
               rendered.find(R"({"command":"npm test"})") != std::string::npos;
    }, std::chrono::seconds(2)));
    EXPECT_TRUE(app->is_loading_for_testing());
    EXPECT_TRUE(app->is_query_running_for_testing());

    server.release_after_preview();
    EXPECT_TRUE(wait_until([&] {
        (void)app->Render();
        return !app->is_query_running_for_testing();
    }, std::chrono::seconds(4)));
    (void)app->Render();
    EXPECT_FALSE(app->is_loading_for_testing());

    fs::remove_all(storage_root);
}

TEST(AppRuntime, StreamingThinkingRendersRunningPreview) {
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

    auto app = ftxui::Make<cc::ui::AppComponent>(
        &engine,
        &commands,
        &storage,
        [] {});
    ReleaseAfterThinkingPreviewGuard release_guard{server};

    app->HandleSubmit("show streaming thinking");
    ASSERT_TRUE(server.wait_for_thinking_delta());
    ASSERT_TRUE(wait_until([&] {
        auto rendered = render_to_plain_text(app->Render(), 140, 36);
        return rendered.find("Thinking") != std::string::npos &&
               rendered.find("private streaming thinking") != std::string::npos;
    }, std::chrono::seconds(2)));
    EXPECT_TRUE(app->is_loading_for_testing());
    EXPECT_TRUE(app->is_query_running_for_testing());

    server.release_after_preview();
    EXPECT_TRUE(wait_until([&] {
        (void)app->Render();
        return !app->is_query_running_for_testing();
    }, std::chrono::seconds(4)));
    auto done = render_to_plain_text(app->Render(), 140, 36);
    EXPECT_FALSE(app->is_loading_for_testing());
    EXPECT_NE(done.find("Thinking"), std::string::npos);
    EXPECT_NE(done.find("private streaming thinking"), std::string::npos);
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

    auto app = ftxui::Make<cc::ui::AppComponent>(
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
        auto rendered = render_to_plain_text(app->Render(), 120, 34);
        return rendered.find("Permission Required") != std::string::npos &&
               rendered.find("Tool: Bash") != std::string::npos &&
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
        auto rendered = render_to_plain_text(app->Render(), 120, 34);
        return rendered.find("Permission Required") != std::string::npos &&
               rendered.find("Tool: Write") != std::string::npos &&
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
        auto rendered = render_to_plain_text(app->Render(), 120, 34);
        return rendered.find("Permission Required") != std::string::npos &&
               rendered.find("Tool: Read") != std::string::npos &&
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

TEST(AppRuntime, RenderMessageShowsThinkingToolUseAndAssistantText) {
    cc::core::AssistantMessage assistant;
    assistant.content.push_back(cc::core::ThinkingBlock{
        .thinking = "private reasoning preview",
        .signature = "sig-1",
    });
    assistant.content.push_back(cc::core::ToolUseBlock{
        .id = cc::core::ToolUseId{"tool-ui-1"},
        .name = "Bash",
        .input_json = R"({"command":"npm test"})",
    });
    assistant.content.push_back(cc::core::TextBlock{"visible assistant answer"});

    auto rendered = render_to_plain_text(cc::ui::RenderMessage(cc::core::Message{std::move(assistant)}), 140, 24);

    EXPECT_NE(rendered.find("Thinking"), std::string::npos);
    EXPECT_NE(rendered.find("private reasoning preview"), std::string::npos);
    EXPECT_NE(rendered.find("Bash"), std::string::npos);
    EXPECT_NE(rendered.find(R"({"command":"npm test"})"), std::string::npos);
    EXPECT_NE(rendered.find("visible assistant answer"), std::string::npos);
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
