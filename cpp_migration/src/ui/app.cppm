/// @file app.cppm
/// @brief Application entry point — thin adapter that drives repl_screen from
///        the production QueryEngine.
module;

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <expected>
#include <functional>
#include <chrono>
#include <format>
#include <deque>
#include <map>
#include <set>
#include <variant>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

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
import cc.ui.repl_screen;

export namespace cc::ui {

using namespace ftxui;
using namespace cc::ui::components;
using namespace cc::core;

namespace repl = cc::ui::repl_screen;

[[nodiscard]] inline std::vector<Message> compact_runtime_messages(void* state) {
    auto* engine = static_cast<core::QueryEngine*>(state);
    return engine ? engine->get_conversation() : std::vector<Message>{};
}

[[nodiscard]] inline VoidResult compact_runtime_apply(void* state) {
    auto* engine = static_cast<core::QueryEngine*>(state);
    if (!engine) {
        return std::unexpected(Error::make(
            ErrorCode::InternalError,
            "No active query engine is available for compaction"));
    }
    auto compacted = engine->compact_conversation();
    if (!compacted) {
        return std::unexpected(Error::make(
            ErrorCode::InternalError,
            compacted.error().format()));
    }
    return VoidResult{};
}

[[nodiscard]] inline CommandContext command_context_for_engine(core::QueryEngine* engine) {
    return CommandContext{
        .args = {},
        .raw_input = {},
        .runtime_state = engine,
        .compact_message_provider = compact_runtime_messages,
        .compact_applier = compact_runtime_apply,
    };
}

// ============================================================
// Projection: Engine state -> ReplScreenState
// ============================================================

[[nodiscard]] inline repl::MessageDisplayEntry project_message(const Message& msg) {
    return std::visit([](const auto& m) -> repl::MessageDisplayEntry {
        using T = std::decay_t<decltype(m)>;
        repl::MessageDisplayEntry e;
        e.timestamp = std::chrono::system_clock::now();

        if constexpr (std::is_same_v<T, UserMessage>) {
            e.role = "user";
            for (const auto& block : m.content) {
                if (const auto* tb = std::get_if<TextBlock>(&block))
                    e.content_preview += tb->text;
            }
        } else if constexpr (std::is_same_v<T, AssistantMessage>) {
            e.role = "assistant";
            for (const auto& block : m.content) {
                if (const auto* tb = std::get_if<TextBlock>(&block)) {
                    e.content_preview += tb->text;
                } else if (const auto* thk = std::get_if<ThinkingBlock>(&block)) {
                    e.is_thinking = true;
                    if (e.content_preview.empty())
                        e.content_preview = thk->thinking.substr(0, 200);
                } else if (const auto* tool = std::get_if<ToolUseBlock>(&block)) {
                    e.is_tool_use = true;
                    e.tool_name = tool->name;
                    e.tool_input_json = tool->input_json;
                }
            }
        } else if constexpr (std::is_same_v<T, SystemMessage>) {
            e.role = "system";
            for (const auto& block : m.content) {
                if (const auto* tb = std::get_if<TextBlock>(&block))
                    e.content_preview += tb->text;
            }
        } else if constexpr (std::is_same_v<T, ToolResultMessage>) {
            e.role = "tool";
            e.is_tool_use = true;
            e.tool_status = m.is_error ? "error" : "success";
            for (const auto& block : m.content) {
                if (const auto* tb = std::get_if<TextBlock>(&block))
                    e.content_preview += tb->text;
            }
        }
        if (e.content_preview.size() > 500)
            e.content_preview.resize(500);
        return e;
    }, msg);
}

// ============================================================
// Convenience: render a single core Message to an Element.
// Used by tests and callers that want a quick rendering of one message.
// ============================================================

[[nodiscard]] inline Element RenderMessage(const Message& msg) {
    std::vector<repl::MessageDisplayEntry> entries{ project_message(msg) };
    return repl::RenderMessages(entries, -1, 40);
}

// ============================================================
// App Adapter Component
// ============================================================

class AppAdapter : public ComponentBase {
private:
    core::QueryEngine* engine_;
    cc::commands::AppCommandRegistry* cmd_registry_;
    utils::SessionStorage* storage_;
    std::function<void()> on_exit_;

    std::shared_ptr<repl::ReplScreenState> screen_state_;
    Component repl_component_;

    std::string current_session_id_;

    // Async query state
    std::jthread query_thread_;
    std::jthread spinner_thread_;
    std::atomic<bool> query_running_{false};
    std::mutex result_mutex_;
    std::optional<std::string> pending_error_;
    std::string streaming_text_;
    struct StreamingToolPreview {
        std::string tool_name;
        std::string input_json;
        bool complete = false;
    };
    struct StreamingThinkingPreview {
        std::string text;
        bool complete = false;
    };
    std::map<std::uint32_t, StreamingToolPreview> streaming_tools_;
    std::map<std::uint32_t, StreamingThinkingPreview> streaming_thinking_;
    ScreenInteractive* screen_ = nullptr;

    // Permission confirmation
    std::mutex permission_mutex_;
    std::condition_variable permission_cv_;
    std::optional<bool> permission_response_;
    std::set<std::string> always_allowed_tools_;

    // Vim mode
    bool vim_enabled_ = false;
    cc::vim::VimStateMachine vim_sm_;

public:
    AppAdapter(core::QueryEngine* engine,
               cc::commands::AppCommandRegistry* cmd_registry,
               utils::SessionStorage* storage,
               std::function<void()> on_exit)
        : engine_(engine),
          cmd_registry_(cmd_registry),
          storage_(storage),
          on_exit_(std::move(on_exit)),
          screen_state_(std::make_shared<repl::ReplScreenState>()) {

        current_session_id_ = utils::SessionStorage::generate_session_id();

        repl::ReplScreenCallbacks cbs;
        cbs.on_submit = [this](const std::string& text, repl::InputMode) {
            HandleSubmit(text);
        };
        cbs.on_interrupt = [this]() {
            if (query_running_.load()) {
                engine_->abort();
                if (query_thread_.joinable())
                    query_thread_.request_stop();
                screen_state_->spinner_tip = "Cancelling...";
            } else {
                if (on_exit_) on_exit_();
            }
        };
        cbs.on_exit = [this]() {
            if (on_exit_) on_exit_();
        };
        cbs.on_permission_response = [this](bool allowed, std::optional<bool> always) {
            std::lock_guard lk(permission_mutex_);
            permission_response_ = allowed;
            if (always && *always && screen_state_->permission_request) {
                always_allowed_tools_.insert(screen_state_->permission_request->tool_name);
            }
            screen_state_->permission_request.reset();
            screen_state_->mode = repl::ReplMode::Normal;
            permission_cv_.notify_one();
        };
        cbs.on_dialog_action = [this](repl::ReplMode mode, int action) {
            if (mode == repl::ReplMode::CostThreshold) {
                if (action == 2 && on_exit_) on_exit_();
            }
            screen_state_->mode = repl::ReplMode::Normal;
        };
        cbs.on_mode_change = [](repl::ReplMode) {};
        cbs.enqueue_slash_command = [this](const std::string& cmd) {
            HandleCommand(cmd);
        };

        repl_component_ = repl::ReplScreen(screen_state_, std::move(cbs));

        SyncState();
    }

    void HandleSubmit(const std::string& text) {
        if (text.empty()) return;

        if (text.starts_with('/')) {
            HandleCommand(text);
            return;
        }

        if (query_running_.load()) return;

        query_running_.store(true);
        screen_state_->spinner_mode = repl::SpinnerMode::Requesting;
        screen_state_->spinner_verb = "Thinking";
        {
            std::lock_guard lk(result_mutex_);
            pending_error_.reset();
            streaming_text_.clear();
            streaming_tools_.clear();
            streaming_thinking_.clear();
        }

        query_thread_ = std::jthread([this, text](std::stop_token st) {
            core::QueryOptions opts;
            opts.on_event = [this, &st](const core::StreamEvent& ev) {
                if (st.stop_requested()) return;

                std::visit([this](const auto& e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, core::ContentBlockStart>) {
                        std::lock_guard lk(result_mutex_);
                        if (const auto* tool = std::get_if<core::ToolUseBlock>(&e.block)) {
                            streaming_tools_[e.index] = StreamingToolPreview{
                                .tool_name = tool->name,
                                .input_json = tool->input_json == "{}" ? std::string{} : tool->input_json,
                                .complete = false,
                            };
                            screen_state_->spinner_mode = repl::SpinnerMode::ToolUse;
                            screen_state_->spinner_verb = tool->name;
                        } else if (const auto* thinking = std::get_if<core::ThinkingBlock>(&e.block)) {
                            streaming_thinking_[e.index] = StreamingThinkingPreview{
                                .text = thinking->thinking,
                                .complete = false,
                            };
                            screen_state_->spinner_mode = repl::SpinnerMode::Thinking;
                        }
                    } else if constexpr (std::is_same_v<T, core::ContentBlockDelta>) {
                        std::lock_guard lk(result_mutex_);
                        if (auto tool = streaming_tools_.find(e.index); tool != streaming_tools_.end()) {
                            tool->second.input_json += e.delta_text;
                        } else if (auto thinking = streaming_thinking_.find(e.index); thinking != streaming_thinking_.end()) {
                            thinking->second.text += e.delta_text;
                        } else {
                            streaming_text_ += e.delta_text;
                            screen_state_->spinner_mode = repl::SpinnerMode::Responding;
                            screen_state_->spinner_verb = std::nullopt;
                        }
                    } else if constexpr (std::is_same_v<T, core::ContentBlockStop>) {
                        std::lock_guard lk(result_mutex_);
                        if (auto tool = streaming_tools_.find(e.index); tool != streaming_tools_.end())
                            tool->second.complete = true;
                        if (auto thinking = streaming_thinking_.find(e.index); thinking != streaming_thinking_.end())
                            thinking->second.complete = true;
                    } else if constexpr (std::is_same_v<T, core::StreamError>) {
                        std::lock_guard lk(result_mutex_);
                        pending_error_ = e.message;
                    }
                }, ev);

                if (screen_) screen_->Post(Event::Custom);
            };

            engine_->stream_query(text, opts);

            query_running_.store(false);
            if (screen_) screen_->Post(Event::Custom);
        });

        spinner_thread_ = std::jthread([this](std::stop_token st) {
            while (!st.stop_requested() && query_running_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (screen_ && query_running_.load())
                    screen_->Post(Event::Custom);
            }
        });
    }

    void HandleCommand(std::string_view cmd) {
        if (cmd == "/exit" || cmd == "/quit") {
            if (on_exit_) on_exit_();
            return;
        }
        if (cmd == "/clear") {
            engine_->clear_conversation();
            SyncState();
            return;
        }
        if (cmd == "/compact") {
            auto result = engine_->compact_conversation();
            if (result) SyncState();
            return;
        }
        if (cmd == "/cost") {
            auto usage = engine_->get_usage();
            auto cost = engine_->budget_tracker().current_spend_usd;
            screen_state_->spinner_tip = std::format(
                "Cost: ${:.4f} | In: {} | Out: {} | Ctx: {:.0f}%",
                cost, usage.input_tokens, usage.output_tokens,
                engine_->context_utilization() * 100.0);
            return;
        }
        if (cmd.starts_with("/model")) {
            auto args_start = cmd.find(' ');
            if (args_start != std::string_view::npos) {
                auto new_model = cmd.substr(args_start + 1);
                auto params = engine_->model_params();
                params.model = std::string(new_model);
                engine_->set_model_params(std::move(params));
            }
            SyncState();
            return;
        }
        if (cmd.starts_with("/vim")) {
            auto args_start = cmd.find(' ');
            if (args_start == std::string_view::npos)
                vim_enabled_ = !vim_enabled_;
            else {
                auto arg = cmd.substr(args_start + 1);
                vim_enabled_ = (arg == "on" || arg == "1");
            }
            if (vim_enabled_) cc::vim::enable_vim_mode();
            else cc::vim::disable_vim_mode();
            return;
        }

        if (cmd_registry_) {
            auto result = cmd_registry_->execute(std::string(cmd), command_context_for_engine(engine_));
            if (result) {
                if (result->status == CommandStatus::Injected) {
                    HandleSubmit(result->message);
                    return;
                }
                if (result->metadata == "EXIT" && on_exit_) {
                    on_exit_();
                    return;
                }
            }
        }
    }

    void SyncState() {
        auto messages = engine_->get_conversation();
        screen_state_->messages.clear();
        screen_state_->messages.reserve(messages.size());
        for (const auto& msg : messages) {
            // The LLM system prompt is an API argument, never a visible message (TS parity:
            // REPL.tsx passes systemPrompt separately, not in the messages array). Skip it.
            if (std::holds_alternative<SystemMessage>(msg)) continue;
            screen_state_->messages.push_back(project_message(msg));
        }

        auto usage = engine_->get_usage();
        screen_state_->status_bar.model_name = engine_->model_params().model;
        screen_state_->status_bar.input_tokens = static_cast<int>(usage.input_tokens);
        screen_state_->status_bar.output_tokens = static_cast<int>(usage.output_tokens);
        screen_state_->status_bar.cost_usd = engine_->budget_tracker().current_spend_usd;
        // Real tokens consumed so far (input grows with the context window). The previous
        // formula context_utilization()*max_tokens was meaningless - max_tokens is the OUTPUT cap.
        screen_state_->status_bar.context_token_count =
            static_cast<int>(usage.input_tokens + usage.output_tokens);
    }

    void ConsumePendingResult() {
        if (query_running_.load()) return;
        if (screen_state_->spinner_mode == repl::SpinnerMode::Hidden) return;

        std::lock_guard lk(result_mutex_);

        if (pending_error_) {
            pending_error_.reset();
        }

        SyncState();
        screen_state_->spinner_mode = repl::SpinnerMode::Hidden;
        screen_state_->spinner_verb = std::nullopt;
        screen_state_->spinner_tip = std::nullopt;
        streaming_text_.clear();
        streaming_tools_.clear();
        streaming_thinking_.clear();

        if (storage_) {
            std::vector<cc::utils::Message> storage_msgs;
            for (const auto& msg : engine_->get_conversation()) {
                std::visit([&storage_msgs](const auto& m) {
                    using T = std::decay_t<decltype(m)>;
                    std::string text;
                    for (const auto& block : m.content) {
                        if (const auto* tb = std::get_if<cc::core::TextBlock>(&block))
                            text += tb->text;
                    }
                    if constexpr (std::is_same_v<T, cc::core::UserMessage>)
                        storage_msgs.push_back(cc::utils::UserMessage{{cc::utils::TextBlock{text}}});
                    else if constexpr (std::is_same_v<T, cc::core::AssistantMessage>)
                        storage_msgs.push_back(cc::utils::AssistantMessage{{cc::utils::TextBlock{text}}});
                }, msg);
            }
            (void)storage_->save_session(current_session_id_, "Session", storage_msgs);
        }
    }

    Element Render() override {
        ConsumePendingResult();

        if (query_running_.load()) {
            std::lock_guard lk(result_mutex_);
            if (!streaming_text_.empty()) {
                repl::MessageDisplayEntry streaming_entry;
                streaming_entry.role = "assistant";
                streaming_entry.content_preview = streaming_text_.size() > 500
                    ? streaming_text_.substr(streaming_text_.size() - 500)
                    : streaming_text_;
                streaming_entry.is_streaming = true;
                streaming_entry.timestamp = std::chrono::system_clock::now();

                auto messages = engine_->get_conversation();
                screen_state_->messages.clear();
                screen_state_->messages.reserve(messages.size() + 1);
                for (const auto& msg : messages) {
                    if (std::holds_alternative<SystemMessage>(msg)) continue;
                    screen_state_->messages.push_back(project_message(msg));
                }
                screen_state_->messages.push_back(std::move(streaming_entry));
                screen_state_->scroll_pinned_to_bottom = true;
            }
        }

        return repl_component_->Render();
    }

    bool OnEvent(Event event) override {
        return repl_component_->OnEvent(event);
    }

    Component ActiveChild() override {
        return repl_component_;
    }

    void set_screen(ScreenInteractive* screen) { screen_ = screen; }

    [[nodiscard]] std::function<bool(std::string_view, std::string_view)> get_permission_callback() {
        return [this](std::string_view tool_name, std::string_view description) -> bool {
            {
                std::lock_guard lk(permission_mutex_);
                if (always_allowed_tools_.contains(std::string(tool_name)))
                    return true;
                screen_state_->permission_request = repl::PermissionRequestInfo{
                    .tool_name = std::string(tool_name),
                    .description = std::string(description),
                    .file_path = std::nullopt,
                    .risk_labels = {},
                };
                screen_state_->mode = repl::ReplMode::ToolPermission;
                permission_response_.reset();
            }
            if (screen_) screen_->Post(Event::Custom);

            std::unique_lock lk(permission_mutex_);
            permission_cv_.wait(lk, [this] { return permission_response_.has_value(); });
            bool allowed = *permission_response_;
            permission_response_.reset();
            return allowed;
        };
    }

    [[nodiscard]] bool is_query_running_for_testing() const noexcept {
        return query_running_.load();
    }

    [[nodiscard]] bool is_loading_for_testing() const noexcept {
        return screen_state_->spinner_mode != repl::SpinnerMode::Hidden;
    }

    [[nodiscard]] std::string status_message_for_testing() const {
        return screen_state_->spinner_tip.value_or(std::string{});
    }
};

// ============================================================
// Main Application Runner
// ============================================================

[[nodiscard]] int RunApp(
    core::QueryEngine& engine,
    cc::commands::AppCommandRegistry& cmd_registry,
    utils::SessionStorage& storage,
    cc::hooks::ToolPermissionHook* permission_hook = nullptr
) {
    // Use the alternate-screen fullscreen like TS (AlternateScreen) - the REPL owns the terminal.
    auto screen = ScreenInteractive::Fullscreen();

    bool should_exit = false;

    auto app = Make<AppAdapter>(
        &engine,
        &cmd_registry,
        &storage,
        [&screen, &should_exit]() {
            should_exit = true;
            screen.Exit();
        }
    );

    app->set_screen(&screen);

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

    app->SyncState();

    screen.Loop(app);

    return should_exit ? 0 : 1;
}

} // namespace cc::ui

extern "C" int cc_ui_run_app_bridge(
    cc::core::QueryEngine* engine,
    cc::commands::AppCommandRegistry* cmd_registry,
    cc::utils::SessionStorage* storage,
    cc::hooks::ToolPermissionHook* permission_hook
) {
    return cc::ui::RunApp(*engine, *cmd_registry, *storage, permission_hook);
}
