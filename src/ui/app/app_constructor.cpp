// app_constructor.cpp — impl unit for AppAdapter constructor, kept OUT of
// app_autocomplete.cpp to stay under clang's 2GB source-location budget.
//
// Contains: AppAdapter constructor (the largest method with the heaviest
// import closure: dialogs, hooks, MCP handlers, settings, statusline, etc.)
//
// Splitting the constructor into its own TU removes 18 heavy imports from
// app_autocomplete.cpp, which was crashing with SIGSEGV in
// ASTReader::FindExternalVisibleDeclsByName during DefineUsedVTables.
module;

#include <cstdio>
#include <cstdlib>

#include <ftxui/component/event.hpp>

module cc.ui.app.app;

import std;

// ── Base imports (only those actually used by constructor) ─────────────
import cc.ui.prompt.autocomplete_sources;
import cc.ui.screens.repl_screen;
import cc.utils.session_storage;

// ── Constructor-only imports (moved out of app_autocomplete.cpp) ────────
import cc.hooks.cost_hook;
import cc.services.mcp.elicitation_handler;
import cc.services.mcp.at_mention_handler;
import cc.tools.ask_user;
import cc.ui.dialogs.default_renderers;
import cc.ui.dialogs.elicitation;
import cc.ui.dialogs.system;
import cc.ui.dialogs.triggers;
import cc.ui.app.app_dialog_registration;
import cc.ui.tools.init;
import cc.ui.prompt.prompt_input_footer;
import cc.utils.statusline_runner;
import cc.utils.model.model;
import cc.constants.constants;
import cc.skills.load_skills_dir;
import cc.state.app_state;
import cc.state.store;
import cc.query.query_engine;
import cc.hooks.lifecycle_hooks;
import cc.commands.command;
import cc.ui.foundation.theme_provider;

namespace cc::ui {

namespace repl = cc::ui::repl_screen;
namespace acsrc = cc::ui::autocomplete_sources;
namespace dtrig = cc::ui::dialogs::triggers;
namespace dsys = cc::ui::dialogs::system;

// ── Constructor (moved out of app_autocomplete.cpp to reduce import closure) ──
AppAdapter::AppAdapter(void* engine, void* lifecycle_hooks,
                       void* cmd_registry, void* storage,
                       std::function<void()> on_exit)
    : impl_(nullptr),
      on_exit_(std::move(on_exit)),
      screen_state_(std::make_shared<repl::ReplScreenState>()) {
    construct_impl(engine, lifecycle_hooks, cmd_registry, storage);
    construct_teammate();
    construct_settings();
    auto* engine_ = static_cast<cc::core::QueryEngine*>(engine);
    auto* lifecycle_hooks_ =
        static_cast<cc::hooks::LifecycleHookRegistry*>(lifecycle_hooks);
    auto* storage_ = static_cast<cc::utils::SessionStorage*>(storage);

    // ── M7: Register default dialog renderers in the registry ────
    cc::ui::dialogs::default_renderers::register_default_renderers(
        screen_state_->dialog_renderers);

    // ── SL-11: deterministic next-action suggestion on QueryEnd ──
    if (lifecycle_hooks_) {
        wire_prompt_suggestion_hook(lifecycle_hooks_, engine_, screen_state_);
    }

    current_session_id_ = utils::SessionStorage::generate_session_id();
    session_start_time_ = std::chrono::steady_clock::now();

    // TS REF: sessionStorage.ts recordTranscript + dumpPrompts.ts
    if (engine_) {
        if (storage_) {
            engine_->set_session_storage(storage_->storage_dir());
            auto dump_dir = storage_->storage_dir().parent_path() / "dump-prompts";
            engine_->set_dump_prompts_dir(std::move(dump_dir));
        }
    }

    // Register all built-in tool UI renderers in the global registry.
    cc::ui::tools::register_builtin_tool_uis();

    // Register every built-in dialog renderer into the dialog registry.
    cc::ui::app_dialogs::register_default_dialog_renderers(
        screen_state_->dialog_renderers);
    cc::ui::app_dialogs::register_modal_dialog_renderers(
        screen_state_->dialog_renderers);
    cc::ui::app_dialogs::register_bottom_dialog_renderers(
        screen_state_->dialog_renderers);
    cc::ui::app_dialogs::register_all_dialog_renderers(
        screen_state_->dialog_renderers);
    cc::ui::app_dialogs::register_hooks_dialog_renderer(
        screen_state_->dialog_renderers);
    cc::ui::app_dialogs::register_teams_dialog_renderer(
        screen_state_->dialog_renderers);
    // The faithful MCP elicitation dialog (y/n shortcuts, Esc -> on_cancel)
    // overrides the minimal inline renderer in default_renderers. Registered
    // last so it wins the by-index slot.
    cc::ui::dialogs::elicitation::RegisterElicitationDialog(
        screen_state_->dialog_renderers);

    // Seed a stable per-session welcome-tip index.
    std::size_t tip_hash = 0;
    for (unsigned char c : current_session_id_)
        tip_hash = tip_hash * 131u + static_cast<std::size_t>(c);
    screen_state_->welcome_tip_index = tip_hash;

    // ── Load settings from disk and project into screen state ──────────
    init_settings_manager();
    this->ProjectSettingsToScreenState();
    this->ProjectRuntimeMetadataToScreenState();

    // Cache autocomplete suggestions at startup.
    cached_skills_ = acsrc::collect_skill_suggestions(screen_state_->cwd);
    cached_plugin_commands_ = acsrc::collect_plugin_commands(screen_state_->cwd);

    // Dynamic skill discovery.
    skills_changed_unsubscribe_ = cc::skills::SkillRegistry::instance().on_skills_changed(
        [this]() {
            cached_skills_ = acsrc::collect_skill_suggestions(screen_state_->cwd);
            PostRenderEvent();
        });

    // Re-project settings whenever they change on disk.
    subscribe_settings_changed([this] {
        this->ProjectSettingsToScreenState();
        this->TriggerStatuslineUpdate();
        PostRenderEvent();
    });

    repl::ReplScreenCallbacks cbs;
    cbs.on_submit = [this](const std::string& text, repl::InputMode mode) {
        this->HandleSubmit(text, mode);
    };
    // Idle Ctrl+C footer key/string (TS useTextInput handleCtrlC projects
    // pending state with key 'Ctrl-C' via onExitMessage).
    set_exit_message_impl("Press Ctrl+C again to exit");
    cbs.on_interrupt = [this]() {
        if (query_running_.load()) {
            // A running query aborts immediately (TS app:interrupt owned by
            // useCancelRequest) — never arms the exit double-press and never
            // leaves a stale footer from a previous idle press.
            static_cast<cc::core::QueryEngine*>(engine_raw())->abort();
            if (query_thread_.joinable())
                query_thread_.request_stop();
            screen_state_->spinner_tip = "Cancelling...";
            reset_exit_handler();
            screen_state_->exit_message_until.reset();
            return;
        }
        // TS REF: src/hooks/useTextInput.ts:108-120 handleCtrlC =
        // useDoublePress(onExitMessage(pending,'Ctrl-C'), onExit, onFirstPress)
        // with DOUBLE_PRESS_TIMEOUT_MS = 800 (useDoublePress.ts:6).
        if (handle_ctrl_c()) {
            if (on_exit_) on_exit_();
            return;
        }
        // First press: clear non-empty input FIRST (TS onFirstPress:
        // onChange('') + setOffset(0) + onHistoryReset), then arm footer.
        if (!screen_state_->input_text.empty()) {
            repl::set_prompt_input_text(screen_state_, {}, 0);
            screen_state_->history_index = std::string::npos;
        }
        screen_state_->exit_message_key = "Ctrl-C";
        screen_state_->exit_message_until =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
        PostRenderEvent();
    };
    cbs.on_exit = [this]() {
        if (on_exit_) on_exit_();
    };
    // TS REF: src/hooks/useGlobalKeybindings.tsx:225-228 handleRedraw ->
    // ink forceRedraw: ERASE_SCREEN (CSI 2 J) + CURSOR_HOME (CSI H), then
    // FTXUI repaints the full frame after the consumed keystroke. Input
    // state must not be mutated.
    cbs.on_redraw = [this]() {
        std::fputs("\x1b[2J\x1b[H", stdout);
        std::fflush(stdout);
        this->PostRenderEvent();
    };
    // TS REF: src/hooks/useTextInput.ts:142-150 — Esc double-press persists
    // the original value via addToHistory before clearing. Same persistence
    // call as the submit path (app_handle_submit.cpp:110).
    cbs.on_save_to_history = [this](const std::string& text) {
        acsrc::append_prompt_history(text, current_session_id_,
                                     screen_state_->cwd);
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
            (void)action;
            if (action == 1) {
                screen_state_->status_bar.cost_usd = 0.0;
            }
            if (action == 2) {
                if (on_exit_) on_exit_();
            }
        }
        screen_state_->mode = repl::ReplMode::Normal;
    };
    cbs.on_mode_change = [this](repl::ReplMode) {
        this->TriggerStatuslineUpdate();
    };
    cbs.enqueue_slash_command = [this](const std::string& cmd) {
        this->HandleCommand(cmd);
    };
    cbs.on_local_jsx_cancel = [this] {
        const std::string command_name =
            screen_state_->active_local_jsx_command_name;
        this->DismissLocalJsxCommand(
            command_name == "agents"
                ? "Agents dialog dismissed"
                : "Skills dialog dismissed");
    };
    cbs.on_local_jsx_event = [this](const Event& ev) {
        return this->HandleLocalJsxEvent(ev);
    };
    cbs.on_permission_cycle = [](cc::ui::prompt::footer::PermissionMode mode) {
        (void)mode;
    };

    // P2 gap api-error-retry: Retry button.
    cbs.on_retry = [this]() {
        if (query_running_.load()) return;
        if (last_submitted_text_.empty()) return;
        this->HandleSubmit(last_submitted_text_);
    };

    // P2 gap api-error-retry: Clear-session button.
    cbs.on_clear_session = [this]() {
        static_cast<cc::core::QueryEngine*>(engine_raw())->clear_conversation();
        local_command_messages_.clear();
        screen_state_->divider_index.reset();
        screen_state_->unseen_divider.reset();
        screen_state_->unseen_message_count = 0;
        screen_state_->pill_visible = false;
        screen_state_->scroll_offset = 0;
        screen_state_->scroll_pinned_to_bottom = true;
        this->SyncState();
    };

    // TS REF: Messages.tsx L703-712 — share StreamingMarkdown instance.
    cbs.streaming_md = &streaming_markdown_;

    repl_component_ = repl::ReplScreen(screen_state_, std::move(cbs));

    // If spawned as a tmux/iTerm pane teammate, start the filesystem inbox
    // poller that delivers addressed tasks to this REPL (no-op otherwise).
    start_teammate_inbox_worker();
    // Leader-side counterpart: poll the team-lead mailbox for stage-A
    // teammate permission_request envelopes (no-op without team identity).
    start_leader_inbox_worker();

    // ── Cost threshold hook wiring (M7.5) ────────────────────────────
    {
        const auto& bt = static_cast<cc::core::QueryEngine*>(engine_raw())->budget_tracker();
        cc::hooks::set_cost_budget(bt.max_budget_usd);

        cost_listener_id_ = cc::hooks::on_cost_update(
            [this](cc::hooks::CostUpdate data) {
                if (cost_threshold_shown_) return;
                auto warning = cc::hooks::check_cost_threshold();
                if (!warning.has_value()) return;
                if (!warning->starts_with("Session cost")) return;

                cost_threshold_shown_ = true;
                const auto& bt = static_cast<cc::core::QueryEngine*>(engine_raw())->budget_tracker();
                dtrig::PushCostThreshold(
                    screen_state_->dialog_queue,
                    bt.max_budget_usd,
                    data.session_cost,
                    screen_state_->model_display_name,
                    [this](bool continue_, bool reset) {
                        if (reset) {
                            cost_threshold_shown_ = false;
                        }
                        if (continue_ || reset) {
                            screen_state_->dialog_queue.pop_bottom(
                                /*is_prompt_input_active=*/false);
                            PostRenderEvent();
                        } else {
                            if (on_exit_) on_exit_();
                        }
                    });
                PostRenderEvent();
            });
    }

    // ── MCP elicitation responder (M7.5) ────────────────────────────
    cc::services::mcp::set_elicitation_responder(
        [this](const cc::services::mcp::ElicitationRequest& req)
            -> std::expected<std::map<std::string, std::string>, std::string>
        {
            {
                std::lock_guard lk(elicitation_mutex_);
                elicitation_response_.reset();
                dtrig::PushElicitation(
                    screen_state_->dialog_queue,
                    req.server_name,
                    /*request_id=*/0,
                    req.message,
                    [this](bool approve) {
                        std::lock_guard lk(elicitation_mutex_);
                        elicitation_response_ = approve;
                        elicitation_cv_.notify_one();
                    });
            }
            PostRenderEvent();

            std::unique_lock lk(elicitation_mutex_);
            elicitation_cv_.wait(lk, [this] {
                return elicitation_response_.has_value();
            });
            bool approved = *elicitation_response_;
            elicitation_response_.reset();

            screen_state_->dialog_queue.pop_bottom(
                /*is_prompt_input_active=*/false);
            PostRenderEvent();

            if (approved) {
                return std::map<std::string, std::string>{};
            }
            return std::unexpected(std::string{"User declined elicitation request from "} + req.server_name);
        });

    // ── IDE at_mentioned responder (AT-09) ───────────────────────────
    cc::services::mcp::set_at_mention_responder(
        [this](const cc::services::mcp::AtMentionNotification& n)
            -> void
        {
            if (n.file_path.empty()) return;
            std::string token = "@" + n.file_path;
            if (n.line_start.has_value()) {
                token += "#L" + std::to_string(*n.line_start);
                if (n.line_end.has_value() && *n.line_end != *n.line_start) {
                    token += "-" + std::to_string(*n.line_end);
                }
            }
            {
                std::lock_guard<std::mutex> lk(
                    screen_state_->pending_at_mention_mutex);
                screen_state_->pending_at_mention_inserts.push_back(
                    std::move(token));
            }
            PostRenderEvent();
        });

    // ── Ask-user tool → PromptDialog (M7.5) ────────────────────────
    cc::tools::set_global_ask_user_responder(
        [this](std::string_view question,
               std::optional<std::string> default_answer)
            -> std::optional<std::string>
        {
            std::string dialog_id;
            {
                std::lock_guard lk(ask_user_mutex_);
                ask_user_response_.reset();

                dsys::PromptDialogPayload p;
                p.id = "ask_user_" +
                    std::to_string(std::chrono::steady_clock::now()
                                       .time_since_epoch()
                                       .count());
                p.title = "Question";
                p.prompt_text = std::string(question);
                p.default_value = std::move(default_answer);
                p.on_response = [this](std::optional<std::string> value) {
                    std::lock_guard lk(ask_user_mutex_);
                    ask_user_response_ = std::move(value);
                    ask_user_cv_.notify_one();
                };
                dialog_id = p.id;
                screen_state_->dialog_queue.push(std::move(p));
            }
            PostRenderEvent();

            std::unique_lock lk2(ask_user_mutex_);
            ask_user_cv_.wait(lk2, [this] {
                return ask_user_response_.has_value();
            });
            auto result = *ask_user_response_;
            ask_user_response_.reset();

            screen_state_->dialog_queue.remove(dialog_id);
            PostRenderEvent();

            return result;
        });

    this->SyncState();

    // ── Statusline worker thread ──────────────────────────────────────
    statusline_thread_ = std::jthread([this](std::stop_token st) {
        while (!st.stop_requested()) {
            std::unique_lock lk(statusline_mutex_);
            statusline_cv_.wait(lk, [this, &st] {
                return statusline_dirty_.load() || st.stop_requested();
            });
            if (st.stop_requested()) break;

            lk.unlock();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(statusline_debounce_ms_));
            if (st.stop_requested()) break;
            if (!statusline_dirty_.exchange(false)) continue;

            std::string cmd;
            {
                cmd = screen_state_->status_line_command;
            }
            if (cmd.empty()) continue;

            statusline_running_.store(true);

            std::string new_json = this->BuildStatuslineInputJson();
            const auto now = std::chrono::steady_clock::now();

            const bool memo_hit =
                (cmd == statusline_last_cmd_) &&
                (new_json == statusline_last_input_json_) &&
                (statusline_last_run_ !=
                     std::chrono::steady_clock::time_point{} &&
                 (now - statusline_last_run_) < std::chrono::seconds(30));

            if (memo_hit) {
                statusline_running_.store(false);
                continue;
            }

            std::string sl_output;
            const bool sl_ok =
                this->ExecuteStatuslineCommand(cmd, new_json, 5000, sl_output);

            statusline_running_.store(false);

            statusline_last_cmd_ = cmd;
            statusline_last_input_json_ = new_json;
            statusline_last_run_ = now;

            if (sl_ok && !sl_output.empty()) {
                screen_state_->status_line_text = std::move(sl_output);
            } else {
                screen_state_->status_line_text.clear();
            }

            if (!st.stop_requested()) {
                PostRenderEvent();
            }
        }
    });

    this->TriggerStatuslineUpdate();
    StartUiAnimationTicker();
}

// Build the StatusLineCommandInput payload from current engine state.
// Faithful to TS buildStatusLineCommandInput(); kept out of the
// app.cppm BMI along with the statusline_runner/model/constants imports.
[[nodiscard]] std::string AppAdapter::BuildStatuslineInputJson() {
        namespace sl = cc::utils::statusline;

        sl::StatusLineCommandInput input;

        // Version
        input.version = std::string(cc::core::constants::kVersion);

        // Model info
        const auto& model = static_cast<cc::core::QueryEngine*>(engine_raw())->model_params().model;
        input.model.id = model;
        input.model.display_name = cc::utils::get_model_display_name(model);

        // Workspace
        const auto cwd = static_cast<cc::core::QueryEngine*>(engine_raw())->working_directory();
        input.workspace.current_dir = cwd;
        input.workspace.project_dir = cwd;
        // added_dirs: not easily accessible at the app level; populated by
        // tool permission context when additional directories are configured.
        // Left empty (empty vector) to match TS semantics for default config.
        input.workspace.added_dirs = {};

        // Output style from settings
        input.output_style_name = output_style_setting();

        // Cost / usage
        const auto& usage = static_cast<cc::core::QueryEngine*>(engine_raw())->get_usage();
        const auto& budget = static_cast<cc::core::QueryEngine*>(engine_raw())->budget_tracker();
        input.cost.total_cost_usd = budget.current_spend_usd;
        // Session duration: time since AppAdapter construction
        auto session_dur = std::chrono::steady_clock::now() - session_start_time_;
        input.cost.total_duration_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(session_dur).count();
        // total_api_duration_ms: not separately tracked at the app layer
        // (would require summing individual API call durations).
        input.cost.total_api_duration_ms = 0;
        // total_lines_added / total_lines_removed: not tracked at this level
        // (would need to aggregate from FileEditTool results).
        input.cost.total_lines_added = 0;
        input.cost.total_lines_removed = 0;

        // Context window
        input.context_window.total_input_tokens = usage.input_tokens;
        input.context_window.total_output_tokens = usage.output_tokens;
        input.context_window.context_window_size =
            static_cast<std::int64_t>(static_cast<cc::core::QueryEngine*>(engine_raw())->max_context_tokens());
        const bool has_usage = usage.input_tokens > 0 || usage.output_tokens > 0 ||
            usage.cache_creation_tokens > 0 || usage.cache_read_tokens > 0;
        if (has_usage) {
            input.context_window.current_usage = sl::StatusLineCurrentUsageInfo{
                .input_tokens = usage.input_tokens,
                .output_tokens = usage.output_tokens,
                .cache_creation_input_tokens = usage.cache_creation_tokens,
                .cache_read_input_tokens = usage.cache_read_tokens,
            };
            const auto input_context_tokens =
                static_cast<std::int64_t>(usage.input_tokens) +
                static_cast<std::int64_t>(usage.cache_creation_tokens) +
                static_cast<std::int64_t>(usage.cache_read_tokens);
            if (input.context_window.context_window_size > 0) {
                auto pct = static_cast<int>(std::llround(
                    static_cast<double>(input_context_tokens) /
                    static_cast<double>(input.context_window.context_window_size) *
                    100.0));
                pct = std::clamp(pct, 0, 100);
                input.context_window.used_percentage = static_cast<double>(pct);
                input.context_window.remaining_percentage = static_cast<double>(100 - pct);
            }
        }

        // 200k threshold flag
        input.exceeds_200k_tokens =
            (usage.input_tokens + usage.output_tokens) > 200'000;

        // Session name: use session id as identifier (TS uses getCurrentSessionTitle
        // which derives from first user message; session id is always available)
        input.session_name = current_session_id_;
        // session_id: TS StatusLineCommandInput.session_id — used by user scripts
        // for the #hashtag display (e.g. #a1b2c3). Same value as session_name.
        input.session_id = current_session_id_;

        // Vim mode (optional — only populated if vim enabled)
        if (auto mode_str = vim_statusline_label()) {
            input.vim = sl::StatusLineVimInfo{.mode = std::move(*mode_str)};
        }

        // rate_limits, agent, remote, worktree: not available at the app level
        // (would require additional service wiring). Left unpopulated (nullopt)
        // which matches TS semantics where undefined fields are omitted from JSON.

    return sl::to_json(input);
}

bool AppAdapter::ExecuteStatuslineCommand(std::string_view command,
                                       std::string json_input,
                                       int timeout_ms,
                                       std::string& output) {
    namespace sl = cc::utils::statusline;
    if (command.empty()) return false;
    auto result = sl::execute_statusline_command_json(
        command, std::move(json_input), timeout_ms);
    if (!result.success) return false;
    output = std::move(result.output);
    return true;
}

[[nodiscard]] bool reduced_motion_enabled() {
    return cc::ui::design::theme::current_theme().a11y.reduced_motion;
}

}  // namespace cc::ui
