/// @file app.cppm
/// @brief Main application UI component - full Claude REPL terminal interface.
/// Implements the complete interactive UI using FTXUI library, matching original project functionality.
module;

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <functional>
#include <chrono>
#include <format>
#include <sstream>
#include <deque>
#include <set>
#include <variant>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

// FTXUI headers
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>

export module cc.ui.app;

import cc.types.types;
import cc.query.query_engine;
import cc.types.command;
import cc.commands.command;
import cc.commands.registry;
import cc.utils.session_storage;
import cc.ui.components;
import cc.ui.components_extended;
import cc.ui.markdown;
import cc.ui.panels;
import cc.vim.vim_mode;
import cc.vim.vim_commands;
import cc.hooks.tool_permissions;

export namespace cc::ui {

using namespace ftxui;
using namespace cc::ui::components;
using namespace cc::core;

// ============================================================
// Message Rendering
// ============================================================

/// Render a single message block
Element RenderMessage(const Message& message) {
    return std::visit([](const auto& msg) -> Element {
        using T = std::decay_t<decltype(msg)>;

        if constexpr (std::is_same_v<T, UserMessage>) {
            // User message
            std::string message_text;
            for (const auto& block : msg.content) {
                if (const auto* tb = std::get_if<TextBlock>(&block)) {
                    message_text += tb->text;
                }
            }
            return hbox({
                ftxui::text("👤 ") | color(Color::Blue),
                paragraph(message_text) | color(Color::White)
            }) | borderStyled(Color::Blue);
        } else if constexpr (std::is_same_v<T, AssistantMessage>) {
            // Assistant message — render thinking blocks and text blocks
            Elements content_elements;
            for (const auto& block : msg.content) {
                if (const auto* tb = std::get_if<TextBlock>(&block)) {
                    content_elements.push_back(paragraph(tb->text) | color(Color::White));
                } else if (const auto* thk = std::get_if<ThinkingBlock>(&block)) {
                    // Render thinking with distinct collapsed style
                    auto thinking_preview = thk->thinking.substr(0, 200);
                    if (thk->thinking.size() > 200) thinking_preview += "...";
                    content_elements.push_back(
                        vbox({
                            ftxui::text("💭 Thinking") | dim | bold,
                            paragraph(thinking_preview) | dim,
                        }) | borderStyled(Color::GrayDark)
                    );
                }
            }
            if (content_elements.empty()) {
                content_elements.push_back(ftxui::text("(empty response)") | dim);
            }
            return hbox({
                ftxui::text("🤖 ") | color(Color::Green),
                vbox(std::move(content_elements))
            }) | borderStyled(Color::Green);
        } else if constexpr (std::is_same_v<T, SystemMessage>) {
            // System message (usually hidden)
            std::string message_text;
            for (const auto& block : msg.content) {
                if (const auto* tb = std::get_if<TextBlock>(&block)) {
                    message_text += tb->text;
                }
            }
            return hbox({
                ftxui::text("ℹ️ ") | color(Color::GrayDark),
                paragraph(message_text) | color(Color::GrayLight)
            }) | borderStyled(Color::GrayDark);
        } else if constexpr (std::is_same_v<T, ToolResultMessage>) {
            // Tool result
            std::string message_text;
            for (const auto& block : msg.content) {
                if (const auto* tb = std::get_if<TextBlock>(&block)) {
                    message_text += tb->text;
                }
            }
            auto prefix = msg.is_error ? "❌ " : "✅ ";
            auto line_color = msg.is_error ? Color::Red : Color::Cyan;
            return hbox({
                ftxui::text(prefix) | ftxui::color(line_color),
                vbox({
                    ftxui::text(std::format("Tool: {}", msg.tool_use_id.value)) | color(Color::GrayLight),
                    separator(),
                    paragraph(message_text)
                })
            }) | borderStyled(line_color);
        } else {
            return ftxui::text("(unknown message type)");
        }
    }, message);
}

/// Render all messages in conversation
Element RenderMessages(const std::vector<Message>& messages) {
    std::vector<Element> elements;

    for (const auto& msg : messages) {
        elements.push_back(RenderMessage(msg));
        elements.push_back(ftxui::text(""));  // Spacer
    }

    return vbox(std::move(elements)) | yframe | flex_grow;
}

// ============================================================
// Main App Component
// ============================================================

/// Application state
struct AppState {
    std::vector<Message> messages;
    bool is_loading = false;
    std::string status_message = "Ready";
    std::uint32_t input_tokens = 0;
    std::uint32_t output_tokens = 0;
    double total_cost = 0.0;
    std::string current_session_id;
    
    // UI state
    bool show_stats = false;
    int active_tab = 0;
    int stats_date_range = 0;
    std::string active_stats_tab = "Overview";
    
    // Tag tabs
    std::vector<Tab> tabs;
    int active_tag_tab = 0;

    // Vim mode
    bool vim_enabled = false;
};

/// Main application component
class AppComponent : public ComponentBase {
private:
    AppState state_;
    core::QueryEngine* engine_;
    cc::commands::AppCommandRegistry* cmd_registry_;
    utils::SessionStorage* storage_;
    std::function<void()> on_exit_;

    // Components
    Component text_input_component_;
    Component container_;

    int spinner_frame_ = 0;  // Animated spinner frame counter

    // Slow operations for DevBar
    std::vector<SlowOperation> slow_ops_;

    // Vim mode state machine
    cc::vim::VimStateMachine vim_sm_;

    // Async query state
    std::jthread query_thread_;
    std::jthread spinner_thread_;  // Periodic refresh for spinner animation
    std::atomic<bool> query_running_{false};
    std::mutex result_mutex_;
    std::optional<std::string> pending_error_;
    std::string streaming_text_;  // Accumulates streamed text during generation
    ScreenInteractive* screen_ = nullptr;

    // Permission confirmation
    struct PermissionRequest {
        std::string tool_name;
        std::string description;
    };
    std::mutex permission_mutex_;
    std::condition_variable permission_cv_;
    std::optional<PermissionRequest> pending_permission_;
    std::optional<bool> permission_response_;
    bool show_permission_dialog_ = false;
    std::set<std::string> always_allowed_tools_;

public:
    AppComponent(core::QueryEngine* engine,
                 cc::commands::AppCommandRegistry* cmd_registry,
                 utils::SessionStorage* storage,
                 std::function<void()> on_exit)
        : engine_(engine),
          cmd_registry_(cmd_registry),
          storage_(storage),
          on_exit_(std::move(on_exit)) {

        // Create session ID
        state_.current_session_id = utils::SessionStorage::generate_session_id();

        // Initialize tabs
        state_.tabs = {
            {"All", "all", true},
            {"Session 1", "session1", false},
            {"Session 2", "session2", false}
        };

        // Text input component
        TextInputOptions input_opts;
        input_opts.placeholder = "Type your message here...";
        input_opts.prefix = "▶ ";
        input_opts.on_submit = [this](std::string text) {
            this->HandleSubmit(text);
        };
        input_opts.get_suggestions = [](std::string input) -> std::vector<Suggestion> {
            if (input.starts_with('/')) {
                std::vector<Suggestion> suggestions = {
                    {"/help", "Show help", "command"},
                    {"/clear", "Clear conversation", "command"},
                    {"/compact", "Compact context window", "command"},
                    {"/cost", "Show cost & token usage", "command"},
                    {"/model", "Show/switch model", "command"},
                    {"/stats", "Toggle statistics view", "command"},
                    {"/commit", "Create a git commit", "command"},
                    {"/exit", "Exit application", "command"},
                };
                // Filter based on what the user has typed so far
                if (input.size() > 1) {
                    std::erase_if(suggestions, [&input](const Suggestion& s) {
                        return !s.text.starts_with(input);
                    });
                }
                return suggestions;
            }
            return {};
        };
        text_input_component_ = TextInput(input_opts);

        // Full container
        container_ = Container::Vertical({
            text_input_component_
        });
    }

    /// Get current messages from engine
    void SyncMessages() {
        state_.messages = engine_->get_conversation();
    }

    /// Handle input submission
    void HandleSubmit(const std::string& text) {
        if (text.empty()) return;

        // Check if it's a command
        if (text.starts_with('/')) {
            HandleCommand(text);
            return;
        }

        // Don't submit if already running
        if (query_running_.load()) return;

        // Clear input and show loading
        state_.is_loading = true;
        state_.status_message = "Thinking...";
        query_running_.store(true);
        {
            std::lock_guard lk(result_mutex_);
            streaming_text_.clear();
        }

        // Launch async streaming query
        query_thread_ = std::jthread([this, text](std::stop_token st) {
            core::QueryOptions opts;
            opts.on_event = [this, &st](const core::StreamEvent& ev) {
                // Check for cancellation
                if (st.stop_requested()) return;

                std::visit([this](const auto& e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, core::ContentBlockDelta>) {
                        std::lock_guard lk(result_mutex_);
                        streaming_text_ += e.delta_text;
                    } else if constexpr (std::is_same_v<T, core::StreamError>) {
                        std::lock_guard lk(result_mutex_);
                        pending_error_ = e.message;
                    }
                }, ev);

                // Wake UI for redraw
                if (screen_) screen_->Post(Event::Custom);
            };

            engine_->stream_query(text, opts);

            // Signal completion
            query_running_.store(false);
            if (screen_) screen_->Post(Event::Custom);
        });

        // Start spinner animation timer (posts Event::Custom every 100ms)
        spinner_thread_ = std::jthread([this](std::stop_token st) {
            while (!st.stop_requested() && query_running_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (screen_ && query_running_.load()) {
                    screen_->Post(Event::Custom);
                }
            }
        });
    }

    /// Check and apply pending async results (called from Render or event loop)
    void ConsumePendingResult() {
        if (query_running_.load()) return;
        if (!state_.is_loading) return;  // Already consumed

        std::lock_guard lk(result_mutex_);

        if (pending_error_) {
            state_.status_message = std::format("Error: {}", *pending_error_);
            pending_error_.reset();
            state_.is_loading = false;
            streaming_text_.clear();
            return;
        }

        // Streaming completed successfully — sync conversation from engine
        SyncMessages();

        // Update usage stats from engine
        auto usage = engine_->get_usage();
        state_.input_tokens = usage.input_tokens;
        state_.output_tokens = usage.output_tokens;
        state_.total_cost = engine_->budget_tracker().current_spend_usd;
        state_.status_message = std::format("Done. {} in / {} out tokens",
            usage.input_tokens, usage.output_tokens);

        state_.is_loading = false;
        streaming_text_.clear();

        // Persist session after successful query
        if (storage_) {
            std::vector<cc::utils::Message> storage_msgs;
            for (const auto& msg : engine_->get_conversation()) {
                std::visit([&storage_msgs](const auto& m) {
                    using T = std::decay_t<decltype(m)>;
                    std::string text;
                    for (const auto& block : m.content) {
                        if (const auto* tb = std::get_if<cc::core::TextBlock>(&block)) {
                            text += tb->text;
                        }
                    }
                    if constexpr (std::is_same_v<T, cc::core::UserMessage>) {
                        storage_msgs.push_back(cc::utils::UserMessage{{cc::utils::TextBlock{text}}});
                    } else if constexpr (std::is_same_v<T, cc::core::AssistantMessage>) {
                        storage_msgs.push_back(cc::utils::AssistantMessage{{cc::utils::TextBlock{text}}});
                    }
                }, msg);
            }
            (void)storage_->save_session(
                state_.current_session_id, "Session", storage_msgs);
        }
    }

    /// Handle slash commands
    void HandleCommand(std::string_view cmd) {
        // Commands that require direct UI integration
        if (cmd == "/exit" || cmd == "/quit") {
            if (on_exit_) on_exit_();
            return;
        }

        if (cmd == "/clear") {
            engine_->clear_conversation();
            SyncMessages();
            state_.status_message = "Conversation cleared";
            return;
        }

        if (cmd == "/compact") {
            auto result = engine_->compact_conversation();
            if (result) {
                SyncMessages();
                state_.status_message = std::format("Compacted to {} messages",
                    state_.messages.size());
            } else {
                state_.status_message = std::format("Compact failed: {}", result.error().message());
            }
            return;
        }

        if (cmd == "/cost") {
            auto usage = engine_->get_usage();
            auto cost = engine_->budget_tracker().current_spend_usd;
            state_.status_message = std::format(
                "Cost: ${:.4f} | In: {} tokens | Out: {} tokens | Context: {:.0f}%",
                cost, usage.input_tokens, usage.output_tokens,
                engine_->context_utilization() * 100.0);
            return;
        }

        if (cmd == "/stats") {
            state_.show_stats = !state_.show_stats;
            return;
        }

        // /model [name] — show or switch model
        if (cmd.starts_with("/model")) {
            auto args_start = cmd.find(' ');
            if (args_start == std::string_view::npos) {
                state_.status_message = std::format("Model: {}",
                    engine_->model_params().model);
            } else {
                auto new_model = cmd.substr(args_start + 1);
                auto params = engine_->model_params();
                params.model = std::string(new_model);
                engine_->set_model_params(std::move(params));
                state_.status_message = std::format("Switched to: {}", new_model);
            }
            return;
        }

        // /vim [on|off] — toggle vim keybinding mode
        if (cmd.starts_with("/vim")) {
            auto args_start = cmd.find(' ');
            if (args_start == std::string_view::npos) {
                state_.vim_enabled = !state_.vim_enabled;
            } else {
                auto arg = cmd.substr(args_start + 1);
                state_.vim_enabled = (arg == "on" || arg == "1");
            }
            if (state_.vim_enabled) {
                cc::vim::enable_vim_mode();
                vim_sm_.set_mode(cc::vim::VimMode::Normal);
                state_.status_message = "Vim mode: ON -- NORMAL --";
            } else {
                cc::vim::disable_vim_mode();
                state_.status_message = "Vim mode: OFF";
            }
            return;
        }

        // Delegate to command registry for all other commands
        if (cmd_registry_) {
            auto result = cmd_registry_->execute(std::string(cmd), CommandContext{});
            if (result) {
                // Handle special result types
                if (result->status == CommandStatus::Injected) {
                    // Inject the result message as a user query to the engine
                    HandleSubmit(result->message);
                    return;
                }
                if (result->metadata == "EXIT" && on_exit_) {
                    on_exit_();
                    return;
                }
                state_.status_message = result->message;
            } else {
                state_.status_message = result.error().message;
            }
        } else {
            state_.status_message = std::format("Unknown command: {}", cmd);
        }
    }

    /// Render the UI
    Element Render() override {
        // Check for async results before rendering
        ConsumePendingResult();

        Elements elements;

        // Status line
        StatusLineOptions status_opts;
        status_opts.left_text = "CC-REPL (C++23)";
        status_opts.center_text = state_.status_message;
        status_opts.right_text = std::format("{} in / {} out • ${:.4f}", 
            state_.input_tokens, state_.output_tokens, state_.total_cost);
        status_opts.model_name = "Claude 3";
        status_opts.current_path = "./";
        status_opts.connection_status = ConnectionStatus::Connected;
        elements.push_back(StatusLine(status_opts));

        // Tag tabs
        TagTabsOptions tag_opts;
        tag_opts.tabs = state_.tabs;
        tag_opts.active_tab = state_.active_tag_tab;
        tag_opts.show_resume_label = true;
        elements.push_back(TagTabs(tag_opts));

        elements.push_back(separator());

        // Main content area
        if (state_.show_stats) {
            // Stats view
            StatsData stats_data;
            stats_data.total_sessions = 1;
            stats_data.total_tokens = state_.input_tokens + state_.output_tokens;
            stats_data.active_days = 1;
            stats_data.longest_streak = 1;
            stats_data.current_streak = 1;
            stats_data.favorite_model = "Claude 3";
            stats_data.longest_session_duration = "5m";
            stats_data.peak_activity_day = "Today";
            stats_data.fun_fact = "You're using C++23!";
            stats_data.model_usage = {
                {"Claude 3", static_cast<int>(state_.input_tokens), static_cast<int>(state_.output_tokens)}
            };
            
            StatsOptions stats_opts;
            stats_opts.data = stats_data;
            stats_opts.active_tab = state_.active_stats_tab;
            stats_opts.selected_date_range = state_.stats_date_range;
            
            elements.push_back(Stats(stats_opts) | flex_grow);
        } else {
            // Normal conversation view
            elements.push_back(RenderMessages(state_.messages) | flex_grow);
        }

        // Loading indicator with spinner and streaming preview
        if (state_.is_loading) {
            ++spinner_frame_;
            Elements loading_elements;
            // Animated spinner
            loading_elements.push_back(
                ftxui::spinner(18, spinner_frame_) | bold | color(Color::Yellow));
            loading_elements.push_back(ftxui::text(" " + state_.status_message));
            elements.push_back(hbox(std::move(loading_elements)));

            // Show streaming text preview if available
            std::string preview;
            {
                std::lock_guard lk(result_mutex_);
                preview = streaming_text_;
            }
            if (!preview.empty()) {
                // Show last ~500 chars of streaming text
                if (preview.size() > 500) {
                    preview = "..." + preview.substr(preview.size() - 500);
                }
                elements.push_back(paragraph(preview) | color(Color::White) | border);
            }
        }

        // Permission confirmation dialog overlay
        if (show_permission_dialog_) {
            std::lock_guard lk(permission_mutex_);
            if (pending_permission_) {
                auto dialog = vbox({
                    ftxui::text("⚠️  Permission Required") | bold | color(Color::Yellow),
                    separator(),
                    ftxui::text(std::format("Tool: {}", pending_permission_->tool_name)) | color(Color::White),
                    paragraph(pending_permission_->description) | dim,
                    separator(),
                    hbox({
                        ftxui::text("[Y] Allow") | color(Color::Green),
                        ftxui::text("  "),
                        ftxui::text("[N] Deny") | color(Color::Red),
                        ftxui::text("  "),
                        ftxui::text("[A] Always Allow") | color(Color::Cyan),
                    })
                }) | border | center;
                // Overlay the dialog on top of the main content
                elements.push_back(dialog);
            }
        }

        // DevBar (debug builds only)
        if (!slow_ops_.empty()) {
            DevBarOptions dev_opts;
            dev_opts.slow_operations = slow_ops_;
            elements.push_back(DevBar(dev_opts));
        }

        elements.push_back(separator());

        // Text input area
        elements.push_back(text_input_component_->Render() | border);

        // Full layout
        return vbox(std::move(elements));
    }

    /// Handle keyboard events
    bool OnEvent(Event event) override {
        // Handle permission dialog keys
        if (show_permission_dialog_) {
            if (event == Event::Character('y') || event == Event::Character('Y')) {
                std::lock_guard lk(permission_mutex_);
                permission_response_ = true;
                permission_cv_.notify_one();
                return true;
            }
            if (event == Event::Character('n') || event == Event::Character('N')) {
                std::lock_guard lk(permission_mutex_);
                permission_response_ = false;
                permission_cv_.notify_one();
                return true;
            }
            if (event == Event::Character('a') || event == Event::Character('A')) {
                std::lock_guard lk(permission_mutex_);
                permission_response_ = true;
                if (pending_permission_) {
                    always_allowed_tools_.insert(pending_permission_->tool_name);
                }
                permission_cv_.notify_one();
                return true;
            }
            return true;  // Swallow all other input while dialog is showing
        }

        // Ctrl+C handling: cancel in-flight query or exit
        if (event == Event::Special("\x03")) {
            if (query_running_.load()) {
                engine_->abort();
                state_.status_message = "Cancelling...";
                // Request the jthread to stop
                if (query_thread_.joinable()) {
                    query_thread_.request_stop();
                }
                return true;
            } else {
                // No query running — exit the app
                if (on_exit_) on_exit_();
                return true;
            }
        }

        // Vim mode key processing (when enabled and in normal/visual/command mode)
        if (state_.vim_enabled && vim_sm_.get_mode() != cc::vim::VimMode::Insert) {
            if (event.is_character()) {
                auto action = vim_sm_.process_key(event.character()[0]);
                if (action.has_value()) {
                    if (action->starts_with("ex:")) {
                        // Execute ex command
                        auto cmd = action->substr(3);
                        auto result = cc::vim::execute_ex_command(cmd);
                        if (result) {
                            state_.status_message = *result;
                        } else {
                            state_.status_message = std::format("E: {}", result.error());
                        }
                    } else if (*action == "quit") {
                        if (on_exit_) on_exit_();
                    } else if (*action == "insert") {
                        // Switching to insert mode — let input component handle keys
                    }
                }
                // Update status to show vim mode indicator
                state_.status_message = vim_sm_.get_status_line();
                return true;
            }
            if (event == Event::Escape) {
                vim_sm_.set_mode(cc::vim::VimMode::Normal);
                state_.status_message = "-- NORMAL --";
                return true;
            }
        }
        // In vim insert mode, Escape returns to normal
        if (state_.vim_enabled && vim_sm_.get_mode() == cc::vim::VimMode::Insert) {
            if (event == Event::Escape) {
                vim_sm_.set_mode(cc::vim::VimMode::Normal);
                state_.status_message = "-- NORMAL --";
                return true;
            }
        }

        // Let parent handle first
        if (container_->OnEvent(event)) {
            return true;
        }

        // Tab navigation for stats
        if (state_.show_stats) {
            if (event == Event::Tab) {
                state_.active_stats_tab = (state_.active_stats_tab == "Overview") ? "Models" : "Overview";
                return true;
            }
            if (event == Event::Character('r')) {
                state_.stats_date_range = (state_.stats_date_range + 1) % 3;
                return true;
            }
            if (event == Event::Escape) {
                state_.show_stats = false;
                return true;
            }
        }

        // Tag tab navigation
        if (event == Event::PageUp) {
            state_.active_tag_tab = (state_.active_tag_tab - 1 + static_cast<int>(state_.tabs.size())) % static_cast<int>(state_.tabs.size());
            return true;
        }
        if (event == Event::PageDown) {
            state_.active_tag_tab = (state_.active_tag_tab + 1) % static_cast<int>(state_.tabs.size());
            return true;
        }

        return false;
    }

    /// Focus management
    Component ActiveChild() override {
        return container_;
    }

    /// Get current session ID
    [[nodiscard]] const std::string& session_id() const noexcept {
        return state_.current_session_id;
    }

    /// Set screen pointer for async wake
    void set_screen(ScreenInteractive* screen) { screen_ = screen; }

    /// Get permission callback for hook integration
    /// Returns a function that the permission hook can call to ask the user
    [[nodiscard]] std::function<bool(std::string_view, std::string_view)> get_permission_callback() {
        return [this](std::string_view tool_name, std::string_view description) -> bool {
            // Post permission request to UI thread
            {
                std::lock_guard lk(permission_mutex_);
                pending_permission_ = PermissionRequest{
                    std::string(tool_name), std::string(description)};
                permission_response_.reset();
                show_permission_dialog_ = true;
            }
            // Wake the UI to show dialog
            if (screen_) screen_->Post(Event::Custom);

            // Wait for user response
            std::unique_lock lk(permission_mutex_);
            permission_cv_.wait(lk, [this] { return permission_response_.has_value(); });
            bool allowed = *permission_response_;

            // Clean up
            pending_permission_.reset();
            show_permission_dialog_ = false;
            permission_response_.reset();
            return allowed;
        };
    }
};

// ============================================================
// Main Application Runner
// ============================================================

/// Run the main interactive application
[[nodiscard]] int RunApp(
    core::QueryEngine& engine,
    cc::commands::AppCommandRegistry& cmd_registry,
    utils::SessionStorage& storage,
    cc::hooks::ToolPermissionHook* permission_hook = nullptr
) {
    auto screen = ScreenInteractive::TerminalOutput();

    bool should_exit = false;

    auto app = Make<AppComponent>(
        &engine,
        &cmd_registry,
        &storage,
        [&screen, &should_exit]() {
            should_exit = true;
            screen.Exit();
        }
    );

    // Wire screen pointer for async wake
    app->set_screen(&screen);

    // Wire permission UI callback to the hook
    if (permission_hook && !permission_hook->is_auto_approve_mode()) {
        auto ui_callback = app->get_permission_callback();
        permission_hook->set_ask_user_fn(
            [ui_callback](const cc::hooks::PermissionContext& ctx) -> cc::hooks::PermissionDecision {
                bool allowed = ui_callback(ctx.tool_name, ctx.args);
                return allowed ? cc::hooks::PermissionDecision::allow
                               : cc::hooks::PermissionDecision::deny;
            }
        );
    }

    // Load initial messages from engine
    app->SyncMessages();

    screen.Loop(app);

    return should_exit ? 0 : 1;
}

} // namespace cc::ui
