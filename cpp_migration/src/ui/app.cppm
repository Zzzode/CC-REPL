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
#include <initializer_list>
#include <deque>
#include <map>
#include <set>
#include <variant>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <cmath>
#include <iterator>

#include <unistd.h>  // for getcwd

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
import cc.hooks.cost_hook;
import cc.services.mcp.elicitation_handler;
import cc.tools.ask_user;
import cc.ui.repl_screen;
import cc.ui.dialogs.default_renderers;
import cc.ui.dialogs.system;
import cc.ui.dialogs.cost_threshold_dialog;
import cc.ui.dialogs.triggers;
import cc.ui.app_dialog_registration;
import cc.ui.tools.init;
import cc.utils.settings_manager;
import cc.utils.statusline_runner;
import cc.constants.constants;
import cc.utils.model.model;
import cc.ui.common.declared_cursor;

export namespace cc::ui {

using namespace ftxui;
using namespace cc::ui::components;
using namespace cc::core;

namespace repl = cc::ui::repl_screen;
namespace dsys = cc::ui::dialogs::system;

[[nodiscard]] inline std::optional<std::string> non_empty_env(const char* name) {
    if (const char* value = std::getenv(name); value && *value) {
        return std::string(value);
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string> first_non_empty_env(std::initializer_list<const char*> names) {
    for (const auto* name : names) {
        if (auto value = non_empty_env(name)) return value;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<bool> parse_bool_text(const std::string& value) {
    if (value == "true" || value == "1" || value == "yes" || value == "on") return true;
    if (value == "false" || value == "0" || value == "no" || value == "off") return false;
    return std::nullopt;
}

[[nodiscard]] inline std::optional<int> parse_int_text(const std::string& value) {
    try {
        return std::stoi(value);
    } catch (...) {
        return std::nullopt;
    }
}

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
// project_messages — TS-faithful projection that splits a single
// AssistantMessage into MULTIPLE display rows when it mixes a ThinkingBlock
// with a TextBlock / ToolUseBlock.  TS renders these as separate sibling
// messages (a collapsed `∴ Thinking` row followed by the visible answer /
// tool-use row); the legacy single-entry projection collapsed them into one
// thinking row, which hid the visible answer once M4 routed thinking rows
// through RenderThinkingMessageFaithful (collapsed → raw text hidden).
//
// Non-assistant messages and assistant messages with a single block kind
// still project to exactly one entry (identical to project_message).
// ============================================================
[[nodiscard]] inline std::vector<repl::MessageDisplayEntry>
project_messages(const Message& msg) {
    std::vector<repl::MessageDisplayEntry> out;
    const auto now = std::chrono::system_clock::now();

    std::visit([&](const auto& m) {
        using T = std::decay_t<decltype(m)>;

        if constexpr (std::is_same_v<T, AssistantMessage>) {
            // Pass 1: collect block kinds present.
            std::string text_acc;
            std::string thinking_acc;
            std::vector<const ToolUseBlock*> tools;
            for (const auto& block : m.content) {
                if (const auto* tb = std::get_if<TextBlock>(&block)) {
                    text_acc += tb->text;
                } else if (const auto* thk = std::get_if<ThinkingBlock>(&block)) {
                    if (!thinking_acc.empty()) thinking_acc.push_back('\n');
                    thinking_acc += thk->thinking;
                } else if (const auto* tool = std::get_if<ToolUseBlock>(&block)) {
                    tools.push_back(tool);
                }
            }

            const bool has_thinking = !thinking_acc.empty();
            const bool has_text     = !text_acc.empty();
            const bool mixed        = has_thinking && (has_text || !tools.empty());

            if (!mixed) {
                // Single-kind assistant message → one entry, identical to
                // project_message semantics.
                out.push_back(project_message(msg));
                return;
            }

            // Mixed → emit thinking row, then text row, then tool rows, in
            // TS block order (thinking precedes the visible answer; the
            // upstream stream already orders them this way).
            if (has_thinking) {
                repl::MessageDisplayEntry t;
                t.role = "assistant";
                t.is_thinking = true;
                t.content_preview = thinking_acc.substr(0, 200);
                t.timestamp = now;
                out.push_back(std::move(t));
            }
            if (has_text) {
                repl::MessageDisplayEntry a;
                a.role = "assistant";
                a.content_preview = text_acc;
                if (a.content_preview.size() > 500) a.content_preview.resize(500);
                a.timestamp = now;
                out.push_back(std::move(a));
            }
            for (const auto* tool : tools) {
                repl::MessageDisplayEntry tu;
                tu.role = "assistant";
                tu.is_tool_use = true;
                tu.tool_name = tool->name;
                tu.tool_input_json = tool->input_json;
                tu.timestamp = now;
                out.push_back(std::move(tu));
            }
        } else {
            // Non-assistant → identical to the single-entry projection.
            out.push_back(project_message(msg));
        }
    }, msg);

    if (out.empty()) out.push_back(project_message(msg));
    return out;
}

// ============================================================
// Convenience: render a single core Message to an Element.
// Used by tests and callers that want a quick rendering of one message.
// ============================================================

[[nodiscard]] inline Element RenderMessage(const Message& msg) {
    return repl::RenderMessages(project_messages(msg), -1, 40);
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

    // Session start time for duration tracking (statusline cost.total_duration_ms)
    std::chrono::steady_clock::time_point session_start_time_;

    // Async query state
    std::jthread query_thread_;
    std::jthread spinner_thread_;
    std::atomic<bool> query_running_{false};
    std::atomic<std::int64_t> welcome_animation_deadline_ms_{0};
    std::atomic<std::uint64_t> ui_animation_tick_count_{0};
    std::mutex result_mutex_;
    std::optional<std::string> pending_error_;
    std::string streaming_text_;
    struct StreamingToolPreview {
        std::string tool_name;
        std::string tool_use_id;  ///< M6: matches ToolExecution* events
        std::string input_json;
        std::string result_preview;  ///< M6: live streaming result preview
        bool complete = false;
        bool is_error = false;
    };
    struct StreamingThinkingPreview {
        std::string text;
        bool complete = false;
    };
    std::map<std::uint32_t, StreamingToolPreview> streaming_tools_;
    std::map<std::uint32_t, StreamingThinkingPreview> streaming_thinking_;
    std::atomic<ScreenInteractive*> screen_{nullptr};

    // Permission confirmation
    std::mutex permission_mutex_;
    std::condition_variable permission_cv_;
    std::optional<bool> permission_response_;
    std::set<std::string> always_allowed_tools_;

    // MCP Elicitation (synchronous dialog response pattern,
    // same as tool permission — blocks worker thread on UI response).
    std::mutex elicitation_mutex_;
    std::condition_variable elicitation_cv_;
    std::optional<bool> elicitation_response_;

    // Ask-user prompt (same synchronous dialog response pattern).
    // Used by the ask_user_question tool to show a PromptDialog instead
    // of falling back to stdio.
    std::mutex ask_user_mutex_;
    std::condition_variable ask_user_cv_;
    std::optional<std::optional<std::string>> ask_user_response_;

    // Vim mode
    bool vim_enabled_ = false;
    cc::vim::VimStateMachine vim_sm_;

    // Settings manager — loads settings from disk and watches for changes.
    // Projections into screen_state_ are applied on init and on file change.
    std::unique_ptr<cc::utils::settings_manager::SettingsManager> settings_manager_;
    cc::utils::settings_manager::UnsubscribeFn settings_unsubscribe_;

    // Cost threshold hook — listener ID + shown guard to avoid re-prompting.
    int cost_listener_id_ = -1;
    bool cost_threshold_shown_ = false;

    // Statusline runner — async execution of user-configurable shell command.
    // Triggered on mount, after messages change, and when settings change.
    // Faithful to TS StatusLine.tsx's debounced doUpdate() pattern.
    std::jthread statusline_thread_;
    std::atomic<bool> statusline_dirty_{false};
    std::atomic<bool> statusline_running_{false};
    std::mutex statusline_mutex_;
    std::condition_variable statusline_cv_;
    int statusline_debounce_ms_ = 300;  // TS: 300ms debounce

    [[nodiscard]] static std::int64_t steady_now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void RestartWelcomeAnimationClock() {
        constexpr std::int64_t kWelcomeAnimationMs = 3000;
        welcome_animation_deadline_ms_.store(
            steady_now_ms() + kWelcomeAnimationMs,
            std::memory_order_relaxed);
    }

    void StartUiAnimationTicker() {
        spinner_thread_ = std::jthread([this](std::stop_token st) {
            constexpr auto kTick = std::chrono::milliseconds(50);
            int query_statusline_tick = 0;
            while (!st.stop_requested()) {
                std::this_thread::sleep_for(kTick);
                if (st.stop_requested()) break;

                const bool query_active = query_running_.load();
                const bool welcome_active =
                    steady_now_ms() <
                    welcome_animation_deadline_ms_.load(std::memory_order_relaxed);
                if (!query_active && !welcome_active) {
                    query_statusline_tick = 0;
                    continue;
                }

                ui_animation_tick_count_.fetch_add(1, std::memory_order_relaxed);
                PostRenderEvent();

                if (query_active && ++query_statusline_tick % 20 == 0) {
                    TriggerStatuslineUpdate();
                }
            }
        });
    }

    void PostRenderEvent() {
        if (auto* screen = screen_.load(std::memory_order_acquire)) {
            screen->Post(Event::Custom);
        }
    }

    [[nodiscard]] static std::string lowercase_ascii(std::string_view value) {
        std::string out(value);
        for (char& ch : out) {
            ch = static_cast<char>(
                std::tolower(static_cast<unsigned char>(ch)));
        }
        return out;
    }

    void RefreshAutocompleteSuggestions() {
        const auto previous_suggestions = screen_state_->autocomplete_suggestions;
        const int previous_index = screen_state_->autocomplete_index;
        screen_state_->autocomplete_suggestions.clear();
        screen_state_->autocomplete_index = -1;

        if (!cmd_registry_) return;
        const std::string& input = screen_state_->input_text;
        if (input.empty() || input.front() != '/') return;

        // TS commandSuggestions hides slash suggestions once arguments start.
        if (input.find_first_of(" \t\n") != std::string::npos) return;

        const std::string query = lowercase_ascii(std::string_view(input).substr(1));
        struct Candidate {
            std::string display_text;
            std::string description;
        };
        std::vector<Candidate> candidates;
        for (const auto* def : cmd_registry_->visible_commands()) {
            if (!def) continue;
            const auto name = lowercase_ascii(def->name);
            if (!query.empty() && !name.starts_with(query)) continue;
            candidates.push_back(Candidate{
                .display_text = "/" + def->name,
                .description = def->description,
            });
        }

        std::ranges::sort(candidates, {}, &Candidate::display_text);
        screen_state_->autocomplete_suggestions.reserve(candidates.size());
        for (auto& candidate : candidates) {
            screen_state_->autocomplete_suggestions.push_back(
                repl::ReplScreenState::AutocompleteSuggestion{
                    .display_text = std::move(candidate.display_text),
                    .description = std::move(candidate.description),
                });
        }
        if (!screen_state_->autocomplete_suggestions.empty()) {
            int preserved_index = 0;
            if (previous_index >= 0 &&
                previous_index < static_cast<int>(previous_suggestions.size())) {
                const auto& previous =
                    previous_suggestions[static_cast<std::size_t>(previous_index)];
                auto it = std::ranges::find(
                    screen_state_->autocomplete_suggestions,
                    previous.display_text,
                    &repl::ReplScreenState::AutocompleteSuggestion::display_text);
                if (it != screen_state_->autocomplete_suggestions.end()) {
                    preserved_index = static_cast<int>(
                        std::distance(
                            screen_state_->autocomplete_suggestions.begin(),
                            it));
                }
            }
            screen_state_->autocomplete_index = preserved_index;
        }
    }

public:
    ~AppAdapter() override {
        if (query_running_.load() && engine_) {
            engine_->abort();
        }
        if (query_thread_.joinable()) query_thread_.request_stop();
        if (spinner_thread_.joinable()) spinner_thread_.request_stop();
        if (statusline_thread_.joinable()) statusline_thread_.request_stop();
        {
            std::lock_guard lk(statusline_mutex_);
            statusline_dirty_.store(true);
        }
        {
            std::lock_guard lk(permission_mutex_);
            permission_response_ = false;
        }
        {
            std::lock_guard lk(elicitation_mutex_);
            elicitation_response_ = false;
        }
        {
            std::lock_guard lk(ask_user_mutex_);
            ask_user_response_ = std::optional<std::string>{};
        }
        statusline_cv_.notify_all();
        permission_cv_.notify_all();
        elicitation_cv_.notify_all();
        ask_user_cv_.notify_all();
    }

    AppAdapter(core::QueryEngine* engine,
               cc::commands::AppCommandRegistry* cmd_registry,
               utils::SessionStorage* storage,
               std::function<void()> on_exit)
        : engine_(engine),
          cmd_registry_(cmd_registry),
          storage_(storage),
          on_exit_(std::move(on_exit)),
          screen_state_(std::make_shared<repl::ReplScreenState>()) {

        // ── M7: Register default dialog renderers in the registry ────
        // Overlay (ToolPermission), plus any future default slots.
        cc::ui::dialogs::default_renderers::register_default_renderers(
            screen_state_->dialog_renderers);

        current_session_id_ = utils::SessionStorage::generate_session_id();
        session_start_time_ = std::chrono::steady_clock::now();

        // Register all built-in tool UI renderers in the global registry.
        // Must happen before any message rendering so tool-use rows get
        // faithful per-tool summaries (userFacingName, message, tag, etc.).
        cc::ui::tools::register_builtin_tool_uis();

        // Register every built-in dialog renderer (default + modal + bottom +
        // all) into the dialog registry. The actual fan-out lives in four
        // out-of-line module implementation units (cc.ui.app_dialog_registration,
        // one per renderer aggregator) so the ~44 dialog implementations they
        // pull in never enter THIS module's source-location budget.
        cc::ui::app_dialogs::register_default_dialog_renderers(
            screen_state_->dialog_renderers);
        cc::ui::app_dialogs::register_modal_dialog_renderers(
            screen_state_->dialog_renderers);
        cc::ui::app_dialogs::register_bottom_dialog_renderers(
            screen_state_->dialog_renderers);
        cc::ui::app_dialogs::register_all_dialog_renderers(
            screen_state_->dialog_renderers);

        // Seed a stable per-session welcome-tip index (deterministic hash of the
        // session id) so the tip varies per session but never flickers per frame.
        std::size_t tip_hash = 0;
        for (unsigned char c : current_session_id_)
            tip_hash = tip_hash * 131u + static_cast<std::size_t>(c);
        screen_state_->welcome_tip_index = tip_hash;
        RestartWelcomeAnimationClock();

        // ── Load settings from disk and project into screen state ──────────
        // Settings come from ~/.claude/settings.json (user),
        // ./.claude/settings.json (project), etc. — merged by SettingsManager.
        // We project the subset the renderer needs (model, statusLine, …) into
        // screen_state_ so repl_screen can read it without depending on the
        // settings manager directly.
        settings_manager_ = std::make_unique<cc::utils::settings_manager::SettingsManager>();
        settings_manager_->initialize();
        ProjectSettingsToScreenState();
        ProjectRuntimeMetadataToScreenState();

        // Re-project settings whenever they change on disk (e.g. user edits
        // settings.json from another terminal, or the /config command saves).
        settings_unsubscribe_ = settings_manager_->on_change(
            [this](cc::utils::settings_manager::SettingSource) {
                ProjectSettingsToScreenState();
                // Settings change → statusline may need re-run (command changed).
                TriggerStatuslineUpdate();
                PostRenderEvent();
            });

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
                // Cost threshold dialog is a single-button info panel
                // (0-arg on_done) with no action enum.  The P0 contract
                // just lets the user ack; Reset/Quit semantics live in
                // the PushCostTrigger callback (see below).  The legacy
                // Action::Continue/Reset/Quit code path is unused and
                // retained as a no-op only to match the ReplScreenState
                // status transition below.
                (void)action;
                if (action == 1) {
                    // Reset session cost counter to $0.00.
                    screen_state_->status_bar.cost_usd = 0.0;
                }
                if (action == 2) {
                    if (on_exit_) on_exit_();
                }
            }
            screen_state_->mode = repl::ReplMode::Normal;
        };
        cbs.on_mode_change = [](repl::ReplMode) {};
        cbs.enqueue_slash_command = [this](const std::string& cmd) {
            HandleCommand(cmd);
        };

        repl_component_ = repl::ReplScreen(screen_state_, std::move(cbs));

        // ── Cost threshold hook wiring (M7.5) ────────────────────────────
        // Push CostThreshold dialog onto the queue when session cost exceeds
        // the configured budget.  Faithful to TS's use_cost_hook pattern where
        // the hook subscribes to cost updates and triggers modal prompts.
        {
            const auto& bt = engine_->budget_tracker();
            cc::hooks::set_cost_budget(bt.max_budget_usd);

            cost_listener_id_ = cc::hooks::on_cost_update(
                [this](cc::hooks::CostUpdate data) {
                    if (cost_threshold_shown_) return;
                    auto warning = cc::hooks::check_cost_threshold();
                    if (!warning.has_value()) return;

                    // Only show the hard threshold dialog, not the 80% warning.
                    // The 80% warning is shown in the status bar / cost tooltip.
                    if (!warning->starts_with("Session cost")) return;

                    cost_threshold_shown_ = true;
                    namespace dtrig = cc::ui::dialogs::triggers;
                    const auto& bt = engine_->budget_tracker();
                    dtrig::PushCostThreshold(
                        screen_state_->dialog_queue,
                        bt.max_budget_usd,
                        data.session_cost,
                        screen_state_->model_display_name,
                        [this](bool continue_, bool reset) {
                            if (reset) {
                                // User reset the counter — allow re-trigger.
                                cost_threshold_shown_ = false;
                            }
                            if (continue_ || reset) {
                                // Dismiss the dialog from the bottom slot.
                                screen_state_->dialog_queue.pop_bottom(
                                    /*is_prompt_input_active=*/false);
                                PostRenderEvent();
                            } else {
                                // Quit
                                if (on_exit_) on_exit_();
                            }
                        });
                    PostRenderEvent();
                });
        }

        // ── MCP elicitation responder (M7.5) ────────────────────────────
        // When an MCP server requests user input (elicitation), push an
        // Elicitation dialog onto the queue and block until the user responds.
        // Faithful to TS's mcp elicitation_handler pattern where the UI
        // suspends the request while showing a modal prompt.
        cc::services::mcp::set_elicitation_responder(
            [this](const cc::services::mcp::ElicitationRequest& req)
                -> std::expected<std::map<std::string, std::string>, std::string>
            {
                // Push dialog onto the queue
                namespace dtrig = cc::ui::dialogs::triggers;
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

                // Wait for user response
                std::unique_lock lk(elicitation_mutex_);
                elicitation_cv_.wait(lk, [this] {
                    return elicitation_response_.has_value();
                });
                bool approved = *elicitation_response_;
                elicitation_response_.reset();

                // Dismiss the dialog
                screen_state_->dialog_queue.pop_bottom(
                    /*is_prompt_input_active=*/false);
                PostRenderEvent();

                if (approved) {
                    // Elicitation dialog in this simplified build is just
                    // approve/deny (no structured form fields yet). Return
                    // empty map on approval.
                    return std::map<std::string, std::string>{};
                }
                return std::unexpected(std::string{"User declined elicitation request from "} + req.server_name);
            });

        // ── Ask-user tool → PromptDialog (M7.5) ────────────────────────
        // Wire the ask_user_question tool through the DialogQueue so it
        // renders as a proper PromptDialog instead of using stdio.
        // Faithful to TS use_tool_use() pattern where tool-use prompts go
        // through the focused-input-dialog system.
        cc::tools::set_global_ask_user_responder(
            [this](std::string_view question,
                   std::optional<std::string> default_answer)
                -> std::optional<std::string>
            {
                std::string dialog_id;
                {
                    std::lock_guard lk(ask_user_mutex_);
                    ask_user_response_.reset();
                    namespace dtrig = cc::ui::dialogs::triggers;
                    namespace dsys = cc::ui::dialogs::system;

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

                // Wait for user response
                std::unique_lock lk2(ask_user_mutex_);
                ask_user_cv_.wait(lk2, [this] {
                    return ask_user_response_.has_value();
                });
                auto result = *ask_user_response_;
                ask_user_response_.reset();

                // Dismiss dialog
                screen_state_->dialog_queue.remove(dialog_id);
                PostRenderEvent();

                return result;
            });

        SyncState();

        // ── Statusline worker thread ──────────────────────────────────────
        // Faithful to TS StatusLine.tsx's debounced doUpdate() pattern.
        // Runs the user's statusline shell command in the background whenever
        // relevant state changes (messages, model, settings), with 300ms
        // debounce.  Updates screen_state_->status_line_text and posts a
        // render event when done.
        statusline_thread_ = std::jthread([this](std::stop_token st) {
            while (!st.stop_requested()) {
                // Wait for dirty flag (with debounce)
                std::unique_lock lk(statusline_mutex_);
                statusline_cv_.wait(lk, [this, &st] {
                    return statusline_dirty_.load() || st.stop_requested();
                });
                if (st.stop_requested()) break;

                // Debounce: wait a bit, re-check dirty flag
                lk.unlock();
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(statusline_debounce_ms_));
                if (st.stop_requested()) break;
                if (!statusline_dirty_.exchange(false)) continue;

                // Skip if command is empty
                std::string cmd;
                {
                    // Read screen_state_ under the same lock pattern as render.
                    // status_line_command is set on UI thread but read here;
                    // it's a std::string which is safe to read when not
                    // concurrently modified (settings change is rare and
                    // ProjectSettingsToScreenState writes it atomically-ish).
                    // For safety, copy under a lock-free assumption — in
                    // practice settings change events are serialized with
                    // Post() so this is always consistent.
                    cmd = screen_state_->status_line_command;
                }
                if (cmd.empty()) continue;

                statusline_running_.store(true);

                // Build input payload from current engine state
                auto input = BuildStatuslineInput();

                // Execute command
                namespace sl = cc::utils::statusline;
                auto result = sl::execute_statusline_command(cmd, input, 5000);

                statusline_running_.store(false);

                if (result.success && !result.output.empty()) {
                    screen_state_->status_line_text = std::move(result.output);
                } else {
                    // Failure / empty output → clear display text
                    screen_state_->status_line_text.clear();
                }

                // Trigger re-render on UI thread
                if (!st.stop_requested()) {
                    PostRenderEvent();
                }
            }
        });

        // Fire initial statusline update on mount
        TriggerStatuslineUpdate();
        StartUiAnimationTicker();
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
                                .tool_use_id = tool->id.value,
                                .input_json = tool->input_json == "{}" ? std::string{} : tool->input_json,
                                .result_preview = {},
                                .complete = false,
                                .is_error = false,
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
                    } else if constexpr (std::is_same_v<T, core::ToolExecutionStart>) {
                        // M6: Tool execution has started — populate result_preview
                        // slot so the progress line can go live. Match the in-flight
                        // streaming tool-use block by tool_use_id (set from the
                        // ToolUseBlock id during ContentBlockStart).
                        std::lock_guard lk(result_mutex_);
                        for (auto& [idx, preview] : streaming_tools_) {
                            if (preview.tool_use_id == e.tool_use_id &&
                                !preview.complete &&
                                preview.result_preview.empty()) {
                                preview.result_preview = "Starting…";
                                break;
                            }
                        }
                    } else if constexpr (std::is_same_v<T, core::ToolExecutionProgress>) {
                        // M6: Live tool execution progress update.
                        std::lock_guard lk(result_mutex_);
                        for (auto& [idx, preview] : streaming_tools_) {
                            if (preview.tool_use_id == e.tool_use_id && !preview.complete) {
                                preview.result_preview = e.partial_result;
                                break;
                            }
                        }
                    } else if constexpr (std::is_same_v<T, core::ToolExecutionEnd>) {
                        // M6: Tool execution complete — store final result preview.
                        std::lock_guard lk(result_mutex_);
                        for (auto& [idx, preview] : streaming_tools_) {
                            if (preview.tool_use_id == e.tool_use_id) {
                                preview.result_preview = e.result;
                                preview.is_error = e.is_error;
                                break;
                            }
                        }
                    } else if constexpr (std::is_same_v<T, core::StreamError>) {
                        std::lock_guard lk(result_mutex_);
                        pending_error_ = e.message;
                    }
                }, ev);

                PostRenderEvent();
            };

            engine_->stream_query(text, opts);

            query_running_.store(false);
            PostRenderEvent();
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
                std::string old_model = params.model;
                params.model = std::string(new_model);
                engine_->set_model_params(std::move(params));

                // M7.5: Show model switch confirmation banner
                namespace dtrig = cc::ui::dialogs::triggers;
                dtrig::PushModelSwitch(
                    screen_state_->dialog_queue,
                    old_model,
                    std::string(new_model),
                    [this](bool confirm) {
                        (void)confirm; // always confirmed via /model command
                        screen_state_->dialog_queue.pop_bottom(
                            /*is_prompt_input_active=*/false);
                        PostRenderEvent();
                    });
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

        if (cmd == "/config" || cmd == "/settings") {
            screen_state_->mode = repl::ReplMode::SettingsView;
            // Note: if the engine later exposes a ConfigManager accessor,
            // wire it here.  For now the dialog uses its internal fallback.
            screen_state_->settings_config = nullptr;
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
                // M7.5: Try to interpret command metadata as a dialog trigger.
                // Commands produce metadata strings (e.g. "CREATE_AGENT",
                // "UI:plugins:manage-plugins") and we map them to queue pushes.
                if (result->metadata && !result->metadata->empty()) {
                    namespace dtrig = cc::ui::dialogs::triggers;
                    auto enqueue_fn = [this](std::string_view c) {
                        if (c.starts_with('/')) {
                            HandleCommand(c);
                        }
                    };
                    if (dtrig::PushFromCommandMetadata(
                            screen_state_->dialog_queue,
                            *result->metadata,
                            enqueue_fn))
                    {
                        PostRenderEvent();
                        return;
                    }
                }
            }
        }
    }

    void ProjectRuntimeMetadataToScreenState() {
        screen_state_->app_version = std::string(cc::core::constants::kVersion);

        const auto& model_id = engine_->model_params().model;
        screen_state_->status_bar.model_name = model_id;
        screen_state_->model_display_name =
            cc::utils::get_model_display_name(model_id);

        auto usage = engine_->get_usage();
        screen_state_->status_bar.input_tokens =
            static_cast<int>(usage.input_tokens);
        screen_state_->status_bar.output_tokens =
            static_cast<int>(usage.output_tokens);
        screen_state_->status_bar.cost_usd =
            engine_->budget_tracker().current_spend_usd;
        screen_state_->status_bar.context_token_count =
            static_cast<int>(usage.input_tokens + usage.output_tokens);

        char cwd_buf[4096];
        if (auto* cwd = ::getcwd(cwd_buf, sizeof(cwd_buf))) {
            screen_state_->cwd = cwd;
        }
    }

    /// Project settings from SettingsManager into screen_state_.
    /// Mirrors how the TS engine projects AppState.settings into the REPL
    /// screen's model/status-line fields.  Only the subset needed by the
    /// renderer is projected — the engine owns the full settings object.
    void ProjectSettingsToScreenState() {
        if (!settings_manager_) return;

        namespace sm = cc::utils::settings_manager;
        auto settings = settings_manager_->get_initial_settings();

        // --- default model ---
        auto model_it = settings.find("model");
        if (model_it != settings.end() &&
            std::holds_alternative<std::string>(model_it->second)) {
            screen_state_->settings_model = std::get<std::string>(model_it->second);
        } else {
            screen_state_->settings_model.clear();
        }

        // --- status line config (settings.statusLine) ---
        std::optional<std::string> status_line_type;
        std::string status_line_command;
        std::optional<bool> status_line_enabled;
        int status_line_padding = 0;

        auto sl_it = settings.find("statusLine");
        if (sl_it != settings.end() &&
            std::holds_alternative<std::map<std::string, std::string>>(sl_it->second)) {
            const auto& sl_map = std::get<std::map<std::string, std::string>>(sl_it->second);

            auto type_it = sl_map.find("type");
            if (type_it != sl_map.end()) {
                status_line_type = type_it->second;
            }

            // enabled flag
            auto enabled_it = sl_map.find("enabled");
            if (enabled_it != sl_map.end()) {
                status_line_enabled = parse_bool_text(enabled_it->second);
            }

            // shell command
            auto cmd_it = sl_map.find("command");
            if (cmd_it != sl_map.end()) {
                status_line_command = cmd_it->second;
            }

            // horizontal padding
            auto pad_it = sl_map.find("padding");
            if (pad_it != sl_map.end()) {
                if (auto parsed = parse_int_text(pad_it->second)) {
                    status_line_padding = *parsed;
                }
            }
        }

        if (auto command = first_non_empty_env({
                "CC_REPL_STATUS_LINE_COMMAND",
                "CLAUDE_CODE_STATUS_LINE_COMMAND"})) {
            status_line_command = *command;
            status_line_type = "command";
        }
        if (auto enabled = first_non_empty_env({
                "CC_REPL_STATUS_LINE_ENABLED",
                "CLAUDE_CODE_STATUS_LINE_ENABLED"})) {
            status_line_enabled = parse_bool_text(*enabled);
        }
        if (auto padding = first_non_empty_env({
                "CC_REPL_STATUS_LINE_PADDING",
                "CLAUDE_CODE_STATUS_LINE_PADDING"})) {
            if (auto parsed = parse_int_text(*padding)) {
                status_line_padding = *parsed;
            }
        }

        const bool type_allows_command = !status_line_type || *status_line_type == "command";
        const bool enabled = status_line_enabled.value_or(
            !status_line_command.empty() && type_allows_command);
        screen_state_->status_line_command = std::move(status_line_command);
        screen_state_->status_line_padding = status_line_padding;
        screen_state_->status_line_enabled =
            enabled && type_allows_command && !screen_state_->status_line_command.empty();
        if (!screen_state_->status_line_enabled) {
            screen_state_->status_line_text.clear();
        }
    }

    /// Trigger an async statusline update (debounced).
    /// Faithful to TS scheduleUpdate() — sets a dirty flag and wakes the
    /// worker thread; the actual command runs after the debounce period.
    void TriggerStatuslineUpdate() {
        if (screen_state_->status_line_command.empty()) return;
        statusline_dirty_.store(true);
        statusline_cv_.notify_one();
    }

    /// Build the StatusLineCommandInput payload from current engine state.
    /// Faithful to TS buildStatusLineCommandInput() — populates model info,
    /// workspace, cost, context window, version, etc.
    [[nodiscard]] cc::utils::statusline::StatusLineCommandInput BuildStatuslineInput() {
        namespace sl = cc::utils::statusline;

        sl::StatusLineCommandInput input;

        // Version
        input.version = std::string(cc::core::constants::kVersion);

        // Model info
        const auto& model = engine_->model_params().model;
        input.model.id = model;
        input.model.display_name = cc::utils::get_model_display_name(model);

        // Workspace
        char cwd_buf[4096];
        if (auto* cwd = ::getcwd(cwd_buf, sizeof(cwd_buf))) {
            input.workspace.current_dir = cwd;
            input.workspace.project_dir = cwd;
        }
        // added_dirs: not easily accessible at the app level; populated by
        // tool permission context when additional directories are configured.
        // Left empty (empty vector) to match TS semantics for default config.
        input.workspace.added_dirs = {};

        // Output style from settings
        if (settings_manager_) {
            auto settings = settings_manager_->get_initial_settings();
            auto os_it = settings.find("outputStyle");
            if (os_it != settings.end() &&
                std::holds_alternative<std::string>(os_it->second)) {
                input.output_style_name = std::get<std::string>(os_it->second);
            } else {
                input.output_style_name = "full";  // default
            }
        } else {
            input.output_style_name = "full";  // default
        }

        // Cost / usage
        const auto& usage = engine_->get_usage();
        const auto& budget = engine_->budget_tracker();
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
            static_cast<std::int64_t>(engine_->max_context_tokens());
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

        // Vim mode (optional — only populated if vim enabled)
        if (vim_enabled_) {
            std::string mode_str;
            switch (vim_sm_.get_mode()) {
                case cc::vim::VimMode::Normal:     mode_str = "NORMAL"; break;
                case cc::vim::VimMode::Insert:     mode_str = "INSERT"; break;
                case cc::vim::VimMode::Visual:     mode_str = "VISUAL"; break;
                case cc::vim::VimMode::VisualLine: mode_str = "VISUAL LINE"; break;
                case cc::vim::VimMode::Command:    mode_str = "COMMAND"; break;
                case cc::vim::VimMode::Replace:    mode_str = "REPLACE"; break;
                default:                           mode_str = "INSERT"; break;
            }
            input.vim = sl::StatusLineVimInfo{.mode = std::move(mode_str)};
        }

        // rate_limits, agent, remote, worktree: not available at the app level
        // (would require additional service wiring). Left unpopulated (nullopt)
        // which matches TS semantics where undefined fields are omitted from JSON.

        return input;
    }

    void SyncState() {
        auto messages = engine_->get_conversation();
        screen_state_->messages.clear();
        screen_state_->messages.reserve(messages.size());
        for (const auto& msg : messages) {
            // The LLM system prompt is an API argument, never a visible message (TS parity:
            // REPL.tsx passes systemPrompt separately, not in the messages array). Skip it.
            if (std::holds_alternative<SystemMessage>(msg)) continue;
            // TS-faithful split: an assistant turn mixing a ThinkingBlock with
            // a TextBlock / ToolUseBlock renders as SEPARATE sibling rows
            // (thinking row then the visible answer / tool-use).  See
            // project_messages for the rationale.
            auto projected = project_messages(msg);
            for (auto& e : projected)
                screen_state_->messages.push_back(std::move(e));
        }

        ProjectRuntimeMetadataToScreenState();

        // Notify cost hook subscribers (drives CostThreshold dialog, etc.).
        cc::hooks::update_cost(cc::hooks::CostUpdate{
            .session_cost = engine_->budget_tracker().current_spend_usd,
            .monthly_cost = 0.0,  // TODO: wire through monthly cost from API
            .input_tokens = screen_state_->status_bar.input_tokens,
            .output_tokens = screen_state_->status_bar.output_tokens,
        });
    }

    void ConsumePendingResult() {
        if (query_running_.load()) return;
        if (screen_state_->spinner_mode == repl::SpinnerMode::Hidden) return;

        std::lock_guard lk(result_mutex_);

        auto pending_error = std::move(pending_error_);
        pending_error_.reset();

        SyncState();
        if (pending_error && !pending_error->empty()) {
            repl::MessageDisplayEntry error_entry;
            error_entry.role = "system";
            error_entry.content_preview = "Error: " + *pending_error;
            error_entry.is_error = true;
            error_entry.timestamp = std::chrono::system_clock::now();
            screen_state_->messages.push_back(std::move(error_entry));
        }
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

        // Query finished → update statusline with final cost/token counts
        TriggerStatuslineUpdate();
    }

    Element Render() override {
        ProjectRuntimeMetadataToScreenState();
        ConsumePendingResult();

        if (query_running_.load()) {
            std::lock_guard lk(result_mutex_);

            const auto now = std::chrono::system_clock::now();
            auto messages = engine_->get_conversation();
            screen_state_->messages.clear();
            screen_state_->messages.reserve(
                messages.size() + streaming_tools_.size() +
                streaming_thinking_.size() + 1);

            // Project completed messages (skip system prompt, split mixed assistant).
            for (const auto& msg : messages) {
                if (std::holds_alternative<SystemMessage>(msg)) continue;
                auto projected = project_messages(msg);
                for (auto& e : projected)
                    screen_state_->messages.push_back(std::move(e));
            }

            // ── Faithful live-path: project in-flight content blocks ──
            // TS renders each content block as a separate message row as it
            // streams in (thinking → text → tool-use, in block-index order).
            // We follow the same pattern: collect all active streaming blocks
            // by their index, then project each into a MessageDisplayEntry
            // that flows through RenderMessages → render_payload_row → the
            // matching faithful Element renderer (thinking / tool-use / text).
            // This way tool-use rows and thinking rows appear progressively
            // during streaming, not just after the full message completes.
            bool has_in_flight = !streaming_text_.empty() ||
                                 !streaming_tools_.empty() ||
                                 !streaming_thinking_.empty();

            if (has_in_flight) {
                // Gather block indices (keys of all streaming maps) to sort.
                std::vector<std::uint32_t> block_indices;
                for (const auto& [i, _] : streaming_thinking_)
                    block_indices.push_back(i);
                for (const auto& [i, _] : streaming_tools_)
                    block_indices.push_back(i);
                // Text block always has index equal to the highest block
                // index (or 0 if no other blocks).  We track it implicitly.
                // Assign it a sentinel index for sorting purposes.
                std::uint32_t text_idx = 0;
                if (!streaming_text_.empty()) {
                    // Text block index = max of all other indices + 1, or 0.
                    for (std::uint32_t i : block_indices)
                        if (i > text_idx) text_idx = i;
                    if (!block_indices.empty()) text_idx += 1;
                    block_indices.push_back(text_idx);
                }
                std::sort(block_indices.begin(), block_indices.end());

                // Project each block in index order.
                for (std::uint32_t idx : block_indices) {
                    // Thinking block
                    auto thk = streaming_thinking_.find(idx);
                    if (thk != streaming_thinking_.end()) {
                        repl::MessageDisplayEntry e;
                        e.role = "assistant";
                        e.is_thinking = true;
                        e.content_preview = thk->second.text.substr(0, 200);
                        e.timestamp = now;
                        screen_state_->messages.push_back(std::move(e));
                        continue;
                    }
                    // Tool-use block
                    auto tlu = streaming_tools_.find(idx);
                    if (tlu != streaming_tools_.end()) {
                        repl::MessageDisplayEntry e;
                        e.role = "assistant";
                        e.is_tool_use = true;
                        e.tool_name = tlu->second.tool_name;
                        e.tool_input_json = tlu->second.input_json;
                        // In-flight tool-use → "running" status so the
                        // faithful renderer shows the animated dot + progress.
                        e.tool_status = "running";
                        e.content_preview = tlu->second.input_json;
                        // M6 result_preview: live streaming result text.
                        // Populated by ToolExecutionProgress events from the
                        // engine's tool execution pipeline.
                        if (!tlu->second.result_preview.empty())
                            e.tool_result_preview = tlu->second.result_preview;
                        e.is_error = tlu->second.is_error;
                        e.timestamp = now;
                        screen_state_->messages.push_back(std::move(e));
                        continue;
                    }
                    // Text block (streaming)
                    if (!streaming_text_.empty() && idx == text_idx) {
                        repl::MessageDisplayEntry e;
                        e.role = "assistant";
                        e.content_preview = streaming_text_.size() > 500
                            ? streaming_text_.substr(streaming_text_.size() - 500)
                            : streaming_text_;
                        e.is_streaming = true;
                        e.timestamp = now;
                        screen_state_->messages.push_back(std::move(e));
                        continue;
                    }
                }

                screen_state_->scroll_pinned_to_bottom = true;
            }
        }

        // Reset cursor to hidden each frame (TS Ink default behavior).
        // Any active declared_cursor decorator on descendant elements will
        // override this with a visible cursor at the declared position.
        // This enables IME preedit text to appear inline at the insertion
        // point and lets screen readers / magnifiers follow the input.
        namespace dc = cc::ui::common::declared_cursor;
        return repl_component_->Render() | dc::cursor_reset();
    }

    bool OnEvent(Event event) override {
        const bool handled = repl_component_->OnEvent(event);
        if (handled) RefreshAutocompleteSuggestions();
        return handled;
    }

    Component ActiveChild() override {
        return repl_component_;
    }

    void set_screen(ScreenInteractive* screen) {
        screen_.store(screen, std::memory_order_release);
        RestartWelcomeAnimationClock();
    }

    [[nodiscard]] std::function<bool(std::string_view, std::string_view)> get_permission_callback() {
        return [this](std::string_view tool_name, std::string_view description) -> bool {
            {
                std::lock_guard lk(permission_mutex_);
                if (always_allowed_tools_.contains(std::string(tool_name)))
                    return true;

                // M7.5: Use DialogQueue via trigger helper
                namespace dtrig = cc::ui::dialogs::triggers;
                dtrig::PushToolPermission(
                    screen_state_->dialog_queue,
                    std::string(tool_name),
                    std::string(description),
                    /*on_response=*/[this, tool_name = std::string(tool_name)](
                        dsys::ToolPermissionPayload::Decision decision,
                        bool /*sandbox*/)
                    {
                        std::lock_guard lk(permission_mutex_);
                        bool allowed =
                            (decision == dsys::ToolPermissionPayload::Decision::AllowOnce ||
                             decision == dsys::ToolPermissionPayload::Decision::AlwaysAllow);
                        if (decision == dsys::ToolPermissionPayload::Decision::AlwaysAllow) {
                            always_allowed_tools_.insert(tool_name);
                        }
                        // Pop the dialog from the queue
                        screen_state_->dialog_queue.pop_overlay();
                        permission_response_ = allowed;
                        permission_cv_.notify_one();
                    },
                    /*on_abort=*/[this] {
                        std::lock_guard lk(permission_mutex_);
                        screen_state_->dialog_queue.pop_overlay();
                        permission_response_ = false;
                        permission_cv_.notify_one();
                    },
                    /*can_always_allow=*/true);

                permission_response_.reset();
            }
            PostRenderEvent();

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

    [[nodiscard]] std::uint64_t ui_animation_tick_count_for_testing() const noexcept {
        return ui_animation_tick_count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::string status_message_for_testing() const {
        return screen_state_->spinner_tip.value_or(std::string{});
    }

    [[nodiscard]] bool status_line_enabled_for_testing() const noexcept {
        return screen_state_->status_line_enabled;
    }

    [[nodiscard]] std::string status_line_command_for_testing() const {
        return screen_state_->status_line_command;
    }

    [[nodiscard]] int status_line_padding_for_testing() const noexcept {
        return screen_state_->status_line_padding;
    }

    [[nodiscard]] std::string status_bar_model_for_testing() const {
        return screen_state_->status_bar.model_name;
    }

    [[nodiscard]] std::size_t autocomplete_suggestion_count_for_testing() const noexcept {
        return screen_state_->autocomplete_suggestions.size();
    }

    [[nodiscard]] std::vector<std::string> autocomplete_suggestions_for_testing() const {
        std::vector<std::string> out;
        out.reserve(screen_state_->autocomplete_suggestions.size());
        for (const auto& suggestion : screen_state_->autocomplete_suggestions) {
            out.push_back(suggestion.display_text);
        }
        return out;
    }

    [[nodiscard]] int autocomplete_index_for_testing() const noexcept {
        return screen_state_->autocomplete_index;
    }

    [[nodiscard]] bool has_pending_dialog_for_testing() const noexcept {
        return screen_state_->dialog_queue.has_overlay() ||
               screen_state_->dialog_queue.has_any_bottom() ||
               screen_state_->dialog_queue.has_modal() ||
               screen_state_->dialog_queue.has_standalone();
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
