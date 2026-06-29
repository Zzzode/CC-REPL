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
#include <unordered_set>
#include <variant>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <filesystem>

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
import cc.utils.skill_usage;
import cc.ui.components;
import cc.ui.components_extended;
import cc.ui.markdown;
import cc.ui.panels;
import cc.vim.vim_mode;
import cc.vim.vim_commands;
import cc.hooks.tool_permissions;
import cc.hooks.cost_hook;
import cc.services.mcp.elicitation_handler;
import cc.services.mcp.at_mention_handler;
import cc.tools.ask_user;
import cc.tools.agent_runtime;
import cc.tools.agent_display;
import cc.ui.repl_screen;
import cc.ui.autocomplete_sources;
import cc.ui.prompt.at_attachments;
import cc.ui.prompt.file_index;
import cc.ui.prompt.fuzzy_rank_nucleo;
import cc.ui.agents.agent_cards;
import cc.ui.agents.shared_widgets;
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
import cc.hooks.lifecycle_hooks;
import cc.ui.common.declared_cursor;

export namespace cc::ui {

// SL-11: defined in app_prompt_suggestion_wiring.cpp (impl unit) to keep the
// heavy cc.services.prompt_suggestion import out of this thin module (clang
// 2GB source-location budget).
void wire_prompt_suggestion_hook(cc::hooks::LifecycleHookRegistry& hooks,
                                 core::QueryEngine* engine,
                                 std::shared_ptr<cc::ui::repl_screen::ReplScreenState> state);

using namespace ftxui;
using namespace cc::ui::components;
using namespace cc::core;

namespace repl = cc::ui::repl_screen;
namespace dsys = cc::ui::dialogs::system;
namespace agent_runtime = cc::tools::agent_runtime;
namespace agent_display = cc::tools::agent_display;
namespace agent_cards = cc::ui::agents::cards;
namespace agent_shared = cc::ui::agents::shared;
namespace acsrc = cc::ui::autocomplete_sources;
namespace atatt = cc::ui::prompt::at_attachments;
namespace fidx = cc::ui::prompt::file_index;

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

[[nodiscard]] inline std::string trim_ascii_copy(std::string_view value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

[[nodiscard]] inline std::string summarize_agent_description(
    std::string_view description) {
    auto newline = description.find('\n');
    if (newline != std::string_view::npos) {
        description = description.substr(0, newline);
    }
    std::string out = trim_ascii_copy(description);
    constexpr std::size_t kMaxSummaryBytes = 160;
    if (out.size() > kMaxSummaryBytes) {
        out.resize(kMaxSummaryBytes);
        out += "...";
    }
    return out;
}

[[nodiscard]] inline std::string lowercase_ascii(std::string_view value) {
    std::string out(value);
    for (char& ch : out) {
        ch = static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

// AT-12: nucleo-grade fuzzy ranking (drop-in for the former fuzzy_rank_ascii).
namespace frn = cc::ui::prompt::fuzzy_rank_nucleo;

[[nodiscard]] inline agent_cards::AgentCardData project_agent_definition_card(
    const agent_runtime::AgentDefinition& agent) {
    agent_cards::AgentCardData card;
    card.id = agent.agent_type;
    card.name = agent.agent_type;
    card.agent_type = agent.agent_type;
    card.source = agent.source;
    card.description = summarize_agent_description(agent.when_to_use);
    card.description_long = agent.when_to_use;
    card.role_tags.push_back(std::string(agent_display::source_display_name(agent.source)));
    card.tools = agent.tools;
    card.status = agent_shared::AgentStatus::Idle;
    if (!agent.model.empty()) {
        card.model_override = agent.model;
    }
    card.permission_mode = agent.permission_mode;
    card.is_subagent = true;
    return card;
}

struct AutocompleteToken {
    std::size_t start = 0;
    std::size_t end = 0;
    std::string text;
};

[[nodiscard]] inline bool ascii_isspace(char ch) {
    return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

[[nodiscard]] inline AutocompleteToken token_around_cursor(
    std::string_view input,
    std::size_t cursor) {
    if (cursor == std::string::npos || cursor > input.size()) {
        cursor = input.size();
    }

    std::size_t start = cursor;
    while (start > 0 && !ascii_isspace(input[start - 1])) --start;

    std::size_t end = cursor;
    while (end < input.size() && !ascii_isspace(input[end])) ++end;

    return AutocompleteToken{
        .start = start,
        .end = cursor,
        .text = std::string(input.substr(start, cursor - start)),
    };
}

// AT-12: fuzzy_match_ascii / fuzzy_rank_ascii removed — all autocomplete
// ranking now delegates to cc::ui::prompt::fuzzy_rank_nucleo (frn::), which
// ports the nucleo/fzf-v2 scorer (boundary/camel/consecutive/gap/path bonuses)
// while preserving the exact {0..3} base range so the tier offsets (alias +1,
// skill +4, plugin +6) and the rank-ascending sort stay unchanged. See
// ui/prompt/fuzzy_rank_nucleo.cppm. lowercase_ascii() above is retained.

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

[[nodiscard]] inline CommandContext command_context_for_engine(
    core::QueryEngine* engine,
    std::string cwd = {}) {
    if (cwd.empty() && engine) cwd = engine->working_directory();
    return CommandContext{
        .args = {},
        .raw_input = {},
        .cwd = std::move(cwd),
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
    cc::hooks::LifecycleHookRegistry* lifecycle_hooks_{nullptr};
    cc::commands::AppCommandRegistry* cmd_registry_;
    utils::SessionStorage* storage_;
    std::function<void()> on_exit_;

    std::shared_ptr<repl::ReplScreenState> screen_state_;
    Component repl_component_;
    std::vector<repl::MessageDisplayEntry> local_command_messages_;

    std::string current_session_id_;

    // Session start time for duration tracking (statusline cost.total_duration_ms)
    std::chrono::steady_clock::time_point session_start_time_;

    // Async query state
    std::jthread query_thread_;
    std::jthread spinner_thread_;
    std::atomic<bool> query_running_{false};
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

    void StartUiAnimationTicker() {
        spinner_thread_ = std::jthread([this](std::stop_token st) {
            constexpr auto kTick = std::chrono::milliseconds(50);
            // TS is event-driven: Ink re-renders only on state changes, never
            // on a fixed timer.  This ticker exists solely to advance ANIMATIONS
            // (the welcome-intro asterisk hue sweep, the query spinner).  Once
            // the welcome intro has played (asterisk_sweep_ms × sweep_count =
            // 1500 × 2 = 3000ms ≈ 60 ticks) the screen is static, so we stop
            // forcing re-renders at idle — FTXUI otherwise re-emits the whole
            // frame + cursor-move sequences 20×/s, which flickers on terminals
            // that paint hidden-cursor movement.  Event-driven re-renders
            // (input, queries, statusline, cost hooks) still work normally.
            constexpr int kWelcomeIntroTicks = 80;  // 80 × 50ms = 4s (3s sweep + margin)
            int query_statusline_tick = 0;
            int welcome_render_ticks = 0;
            while (!st.stop_requested()) {
                std::this_thread::sleep_for(kTick);
                if (st.stop_requested()) break;

                const bool query_active = query_running_.load();
                const bool welcome_active =
                    screen_state_ &&
                    screen_state_->messages.empty() &&
                    screen_state_->spinner_mode == repl::SpinnerMode::Hidden;
                if (!welcome_active) welcome_render_ticks = 0;

                // Re-render only while an animation is actually advancing:
                // an active query (spinner) or the welcome-intro sweep.  At
                // static idle we skip — no animation to drive.
                if (query_active) {
                    // spinner animation: keep ticking
                } else if (welcome_active &&
                           welcome_render_ticks < kWelcomeIntroTicks) {
                    ++welcome_render_ticks;
                } else {
                    query_statusline_tick = 0;
                    continue;
                }

                ui_animation_tick_count_.fetch_add(1, std::memory_order_relaxed);
                PostRenderEvent();

                if (query_active && ++query_statusline_tick % 20 == 0) {
                    this->TriggerStatuslineUpdate();
                }
            }
        });
    }

    void PostRenderEvent() {
        if (auto* screen = screen_.load(std::memory_order_acquire)) {
            screen->Post(Event::Custom);
        }
    }

    void AppendLocalMessagesToScreenState() {
        screen_state_->messages.insert(
            screen_state_->messages.end(),
            local_command_messages_.begin(),
            local_command_messages_.end());
    }

    void AppendLocalCommandInputMessage(std::string command) {
        if (command.empty()) return;
        repl::MessageDisplayEntry entry;
        entry.role = "user";
        entry.content_preview = std::move(command);
        entry.is_local_command_input = true;
        entry.timestamp = std::chrono::system_clock::now();
        local_command_messages_.push_back(std::move(entry));
    }

    void AppendLocalCommandMessage(std::string message, bool is_error = false) {
        if (message.empty()) return;
        repl::MessageDisplayEntry entry;
        entry.role = "system";
        entry.content_preview = std::move(message);
        entry.is_local_command_output = true;
        entry.is_error = is_error;
        entry.timestamp = std::chrono::system_clock::now();
        local_command_messages_.push_back(std::move(entry));
        this->SyncState();
        PostRenderEvent();
    }

    void AppendCommandResult(const CommandResult& result) {
        AppendLocalCommandMessage(
            result.message,
            !result.ok || result.status == CommandStatus::Failed);
    }

    void ClearActiveLocalJsxCommand() {
        screen_state_->active_local_jsx_command = false;
        screen_state_->active_local_jsx_command_name.clear();
        screen_state_->active_local_jsx_command_args.clear();
        screen_state_->active_local_jsx_content.clear();
        screen_state_->active_agents_selection_position = 0;
    }

    void DismissLocalJsxCommand(std::string result_message) {
        if (!screen_state_->active_local_jsx_command) return;
        std::string command = "/" + screen_state_->active_local_jsx_command_name;
        if (!screen_state_->active_local_jsx_command_args.empty()) {
            command += " " + screen_state_->active_local_jsx_command_args;
        }

        ClearActiveLocalJsxCommand();
        AppendLocalCommandInputMessage(std::move(command));
        AppendLocalCommandMessage(std::move(result_message), false);
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
        screen_state_->autocomplete_stable_name_width = 0;  // INF-03: slash branch sets it

        const std::string& input = screen_state_->input_text;
        const std::size_t cursor =
            screen_state_->input_cursor == std::string::npos ||
                screen_state_->input_cursor > input.size()
            ? input.size()
            : screen_state_->input_cursor;
        const auto token = token_around_cursor(input, cursor);

        auto add_suggestion = [&](std::string display,
                                  std::string description,
                                  std::string insert,
                                  std::size_t start,
                                  std::size_t end,
                                  bool submit_on_return = false,
                                  std::string id = "") {
            if (id.empty()) id = display;  // INF-02: stable-id fallback
            screen_state_->autocomplete_suggestions.push_back(
                repl::ReplScreenState::AutocompleteSuggestion{
                    .display_text = std::move(display),
                    .description = std::move(description),
                    .insert_text = std::move(insert),
                    .replacement_start = start,
                    .replacement_end = end,
                    .submit_on_return = submit_on_return,
                    .id = std::move(id),
                });
        };

        auto restore_index = [&] {
            if (screen_state_->autocomplete_suggestions.empty()) return;
            int preserved_index = 0;
            if (previous_index >= 0 &&
                previous_index < static_cast<int>(previous_suggestions.size())) {
                const auto& previous =
                    previous_suggestions[static_cast<std::size_t>(previous_index)];
                // INF-02: match by stable id (falls back to display_text via
                // add_suggestion's default), so same-display items from
                // different sources don't collide.
                auto it = std::ranges::find(
                    screen_state_->autocomplete_suggestions,
                    previous.id,
                    &repl::ReplScreenState::AutocompleteSuggestion::id);
                if (it != screen_state_->autocomplete_suggestions.end()) {
                    preserved_index = static_cast<int>(
                        std::distance(
                            screen_state_->autocomplete_suggestions.begin(),
                            it));
                }
            }
            screen_state_->autocomplete_index = preserved_index;
        };

        if (input.empty()) {
            // SL-11: empty-prompt deterministic next-action suggestion.
            // Surface the QueryEnd-generated suggestion as the lone popup
            // entry while idle (not responding). A non-empty input falls
            // through and clears next_action_suggestion below.
            if (!query_running_.load() && screen_state_->next_action_suggestion &&
                screen_state_->next_action_suggestion->front() != '/') {
                const auto& text = *screen_state_->next_action_suggestion;
                add_suggestion(text, "Suggested next action", text,
                               0, 0, /*submit_on_return=*/false,
                               /*id=*/"__next_action_suggestion__");
                restore_index();
            }
            return;
        }
        // SL-11: user typed something — retire the next-action suggestion.
        screen_state_->next_action_suggestion.reset();

        // INF-05: honor a previous Esc dismissal — if the user closed the
        // popup for this exact input, don't reopen until the input changes.
        // (Suggestions were already cleared at the top of this function.)
        if (input == screen_state_->dismissed_autocomplete_for_input) {
            return;
        }
        screen_state_->dismissed_autocomplete_for_input.clear();

        // SL-03: derive inline argument hint for "/cmd ..." inputs (shown by
        // TextInputImpl after the prompt). Faithful to TS useTypeahead's
        // commandArgumentHint (src/hooks/useTypeahead.tsx:729-770).
        screen_state_->pending_argument_hint.clear();
        if (input.starts_with('/') && cmd_registry_) {
            const auto sp = input.find(' ');
            if (sp != std::string::npos && sp > 1) {
                const std::string cmd_name = input.substr(1, sp - 1);
                if (const auto* def = cmd_registry_->find_definition(cmd_name)) {
                    if (!def->argument_hint.empty()) {
                        screen_state_->pending_argument_hint = def->argument_hint;
                    }
                }
            }
        }

        // SL-05: mid-input slash ghost text — a "/prefix" token appearing
        // mid-input (input doesn't start with '/') completes inline to the
        // shortest matching command name. Faithful to TS findMidInputSlashCommand
        // + getBestCommandMatch (src/utils/commandSuggestions.ts:114-195).
        screen_state_->pending_ghost_text.clear();
        if (!input.starts_with('/') && token.text.starts_with('/') &&
            cmd_registry_) {
            const std::string partial = token.text.substr(1);
            if (!(partial.empty() || partial.find(' ') != std::string::npos)) {
                const CommandDefinition* best = nullptr;
                for (const auto* def : cmd_registry_->visible_commands()) {
                    if (def->name.size() >= partial.size() &&
                        def->name.compare(0, partial.size(), partial) == 0) {
                        if (!best || def->name.size() < best->name.size()) best = def;
                    }
                }
                if (best && best->name.size() > partial.size()) {
                    screen_state_->pending_ghost_text = best->name.substr(partial.size());
                }
            }
        }

        auto add_directory_suggestions = [&](std::string_view partial) {
            std::filesystem::path base = screen_state_->cwd.empty()
                ? std::filesystem::current_path()
                : std::filesystem::path(screen_state_->cwd);
            std::filesystem::path raw{std::string(partial)};
            std::filesystem::path parent = raw.has_parent_path()
                ? base / raw.parent_path()
                : base;
            const auto prefix = raw.has_parent_path()
                ? raw.parent_path().string() + "/"
                : std::string{};
            const auto leaf = raw.filename().string();

            std::error_code ec;
            if (!std::filesystem::is_directory(parent, ec)) return;

            struct DirCandidate { std::string display; std::string insert; int rank; };
            std::vector<DirCandidate> dirs;
            for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
                if (ec) break;
                if (!entry.is_directory(ec)) continue;
                const auto name = entry.path().filename().string();
                if (!frn::fuzzy_match_nucleo(name, leaf)) continue;
                auto insert = prefix + name + "/";
                dirs.push_back(DirCandidate{
                    .display = insert,
                    .insert = insert,
                    .rank = frn::fuzzy_rank_nucleo(name, leaf),
                });
                if (dirs.size() >= 50) break;
            }
            std::ranges::sort(dirs, [](const auto& a, const auto& b) {
                if (a.rank != b.rank) return a.rank < b.rank;
                return a.display < b.display;
            });
            for (auto& dir : dirs) {
                add_suggestion(
                    std::move(dir.display),
                    "Directory",
                    std::move(dir.insert),
                    token.start,
                    token.end,
                    false);
            }
        };

        auto add_session_suggestions = [&](std::string_view partial) {
            if (!storage_) return;
            auto sessions = storage_->list_sessions(50);
            if (!sessions) return;
            for (const auto& session : *sessions) {
                const auto& id = session.metadata.id;
                const auto& title = session.metadata.title;
                if (!frn::fuzzy_match_nucleo(id, partial) &&
                    !frn::fuzzy_match_nucleo(title, partial)) {
                    continue;
                }
                add_suggestion(
                    id.substr(0, std::min<std::size_t>(id.size(), 8)),
                    title.empty() ? "Session" : title,
                    id,
                    token.start,
                    token.end,
                    true);
            }
        };

        const auto before_cursor = input.substr(0, cursor);
        if (before_cursor.starts_with("/add-dir ") ||
            before_cursor.starts_with("/add-dir\t")) {
            add_directory_suggestions(token.text);
            restore_index();
            return;
        }
        if (before_cursor.starts_with("/resume ") ||
            before_cursor.starts_with("/resume\t") ||
            before_cursor.starts_with("/r ") ||
            before_cursor.starts_with("/r\t")) {
            add_session_suggestions(token.text);
            restore_index();
            return;
        }

        if (input.starts_with('/') &&
            cursor <= input.size() &&
            before_cursor.find_first_of(" \t\n") != std::string::npos &&
            cmd_registry_) {
            auto completions = cmd_registry_->complete(before_cursor);
            for (auto& completion : completions) {
                const bool whole_command = completion.starts_with('/');
                add_suggestion(
                    completion,
                    "Command argument",
                    whole_command ? completion + " " : completion,
                    whole_command ? 0 : token.start,
                    whole_command ? cursor : token.end,
                    true);
            }
            if (!screen_state_->autocomplete_suggestions.empty()) {
                restore_index();
                return;
            }
        }

        if (token.text.starts_with('/')) {
            const auto query = std::string_view(token.text).substr(1);
            struct SlashCandidate {
                std::string display;
                std::string description;
                std::string insert;
                int rank = 0;
                bool submit = true;
                std::string id;  // INF-02: source-aware stable id
            };
            std::vector<SlashCandidate> candidates;

            if (cmd_registry_) {
                for (const auto* def : cmd_registry_->visible_commands()) {
                    if (!def) continue;
                    // SL-02: multi-key match — name (exact/prefix/substring/subseq)
                    // outranks a description-word match, so commands are still
                    // surfaced when the user types a description term (TS Fuse
                    // weights descriptionKey×0.5; cpp previously matched name only).
                    int cmd_rank = -1;
                    if (frn::fuzzy_match_nucleo(def->name, query)) {
                        cmd_rank = frn::fuzzy_rank_nucleo(def->name, query);
                    } else {
                        const auto d = lowercase_ascii(def->description);
                        const auto q = lowercase_ascii(query);
                        if (!q.empty() && d.find(q) != std::string::npos) {
                            cmd_rank = 10;  // description match, lower priority
                        }
                    }
                    if (cmd_rank >= 0) {
                        // SL-06: only auto-execute (submit on Enter) commands that
                        // don't require arguments — commands with required args
                        // expand to "/cmd " so the user can type them. Faithful
                        // to TS shouldExecute gated on argNames.length.
                        bool needs_args = false;
                        for (const auto& a : def->args) {
                            if (a.required) { needs_args = true; break; }
                        }
                        // SL-07: TS shows the matched alias as a parenthetical on
                        // the CANONICAL row (commandSuggestions.ts:265-287
                        // createCommandSuggestionItem — aliasText = ` (${alias})`).
                        std::string matched_alias;
                        if (!query.empty() && !def->aliases.empty()) {
                            const auto q = lowercase_ascii(query);
                            for (const auto& alias : def->aliases) {
                                if (lowercase_ascii(alias).starts_with(q)) {
                                    matched_alias = alias;
                                    break;
                                }
                            }
                        }
                        std::string display = "/" + def->name;
                        if (!matched_alias.empty()) display += " (" + matched_alias + ")";
                        candidates.push_back(SlashCandidate{
                            .display = std::move(display),
                            .description = def->description,
                            .insert = "/" + def->name + " ",
                            .rank = cmd_rank,
                            .submit = !needs_args,
                            .id = "cmd:" + def->name,
                        });
                    }
                    for (const auto& alias : def->aliases) {
                        if (!frn::fuzzy_match_nucleo(alias, query)) continue;
                        candidates.push_back(SlashCandidate{
                            .display = "/" + alias,
                            .description = "Alias for /" + def->name,
                            .insert = "/" + alias + " ",
                            .rank = frn::fuzzy_rank_nucleo(alias, query) + 1,
                            .submit = true,
                            .id = "alias:" + alias,
                        });
                    }
                }
            }

            for (const auto& skill : acsrc::collect_skill_suggestions(screen_state_->cwd)) {
                // SL-02: name match outranks a description-word match (Fuse
                // keeps descriptionKey at lower weight; cpp matched name only).
                int skill_rank = -1;
                if (frn::fuzzy_match_nucleo(skill.name, query)) {
                    skill_rank = frn::fuzzy_rank_nucleo(skill.name, query) + 4;
                } else {
                    const auto d = lowercase_ascii(skill.description);
                    const auto q = lowercase_ascii(query);
                    if (!q.empty() && d.find(q) != std::string::npos) skill_rank = 14;
                }
                if (skill_rank < 0) continue;
                // SL-04: recency boost — recently-used skills rank higher
                // (TS getSkillUsageScore). Bounded so fuzzy relevance still wins
                // on non-empty queries; on empty '/' all skills tie on fuzzy rank
                // so recency dominates, surfacing recent skills first.
                const int recency_bonus = static_cast<int>(
                    std::min(cc::utils::skill_usage::get_skill_usage_score(skill.name), 3.0));
                candidates.push_back(SlashCandidate{
                    .display = "/" + skill.name,
                    .description = (skill.kind == "workflow")
                        ? std::format("[workflow] {} skill · {}", skill.source, skill.description)
                        : std::format("{} skill · {}", skill.source, skill.description),
                    .insert = "/" + skill.name + " ",
                    .rank = skill_rank - recency_bonus,
                    .submit = true,
                    .id = "skill:" + skill.name + ":" + skill.source,
                });
            }

            for (const auto& plugin_command : acsrc::collect_plugin_commands(screen_state_->cwd)) {
                int plugin_rank = -1;
                if (frn::fuzzy_match_nucleo(plugin_command.command, query)) {
                    plugin_rank = frn::fuzzy_rank_nucleo(plugin_command.command, query) + 6;
                } else {
                    const auto d = lowercase_ascii(plugin_command.plugin_name);
                    const auto q = lowercase_ascii(query);
                    if (!q.empty() && d.find(q) != std::string::npos) plugin_rank = 16;
                }
                if (plugin_rank < 0) continue;
                candidates.push_back(SlashCandidate{
                    .display = "/" + plugin_command.command,
                    .description = "Plugin command · " + plugin_command.plugin_name,
                    .insert = "/" + plugin_command.command + " ",
                    .rank = plugin_rank,
                    .submit = false,
                    .id = "plugin:" + plugin_command.command + ":" + plugin_command.plugin_name,
                });
            }

            // SL-01: hidden-command exact-name escape hatch — if the user typed
            // the full name of a hidden command, surface it at the top (TS
            // commandSuggestions.ts:391-401 hiddenExact). visible_commands()
            // otherwise hides them entirely.
            if (auto* hidden = cmd_registry_->hidden_command_if_exact(query)) {
                candidates.push_back(SlashCandidate{
                    .display = "/" + hidden->name,
                    .description = hidden->description,
                    .insert = "/" + hidden->name + " ",
                    .rank = -1000,  // force top (rank ascending = smaller first)
                    .submit = true,
                    .id = "hidden-cmd:" + hidden->name,
                });
            }

            std::ranges::sort(candidates, [](const auto& a, const auto& b) {
                if (a.rank != b.rank) return a.rank < b.rank;
                return a.display < b.display;
            });
            // INF-03: precompute stable name-column width over ALL candidates
            // (not just the visible window) so the description column doesn't
            // jitter as the user filters. Slash names are ASCII, so size() is a
            // faithful width measure here. Mirrors TS maxColumnWidth over
            // visible commands (src/hooks/useTypeahead.tsx:380-386).
            {
                int widest = 0;
                for (const auto& c : candidates) {
                    widest = std::max(widest, static_cast<int>(c.display.size()));
                }
                screen_state_->autocomplete_stable_name_width = widest;
            }
            const std::size_t limit = std::min<std::size_t>(candidates.size(), 80);
            for (std::size_t i = 0; i < limit; ++i) {
                add_suggestion(
                    std::move(candidates[i].display),
                    std::move(candidates[i].description),
                    std::move(candidates[i].insert),
                    token.start,
                    token.end,
                    candidates[i].submit,
                    std::move(candidates[i].id));
            }
            restore_index();
            return;
        }

        // INF-01/AT-08: @ mentions are suppressed in bash mode. Faithful to TS
        // useTypeahead (the @DM/@file branches are gated on mode !== 'bash');
        // in bash mode we fall through to the $PATH shell-command scan below.
        if (token.text.starts_with('@') &&
            screen_state_->input_mode != repl::InputMode::Bash) {
            const auto query = std::string_view(token.text).substr(1);
            std::filesystem::path base = screen_state_->cwd.empty()
                ? std::filesystem::current_path()
                : std::filesystem::path(screen_state_->cwd);
            std::filesystem::path raw{std::string(query)};
            std::filesystem::path parent = raw.has_parent_path()
                ? base / raw.parent_path()
                : base;
            const auto prefix = raw.has_parent_path()
                ? raw.parent_path().string() + "/"
                : std::string{};
            const auto leaf = raw.filename().string();

            // AT-01: bare @-queries (no path separator) fuzzy-match the whole
            // repo via the file index — TS searches the index, so "@readme"
            // finds src/readme.md. The directory_iterator block below still
            // handles explicit path browsing (@src/...).
            const bool bare_query = query.find('/') == std::string_view::npos &&
                                    query.find('\\') == std::string_view::npos;

            // AT-05: bare-@ teammate DM precedence — when the query has no path
            // separator, teammate (native-agent) DM matches are shown EXCLUSIVELY
            // before file/agent/mcp, mirroring TS useTypeahead @DM
            // (src/hooks/useTypeahead.tsx:596-637, startsWith on lowercased name).
            // With a match we return immediately so the popup is DM-only.
            if (bare_query) {
                auto starts_with_ci = [](std::string_view name, std::string_view q) {
                    if (name.size() < q.size()) return false;
                    for (std::size_t i = 0; i < q.size(); ++i) {
                        if (std::tolower(static_cast<unsigned char>(name[i])) !=
                            std::tolower(static_cast<unsigned char>(q[i]))) {
                            return false;
                        }
                    }
                    return true;
                };
                struct DmCandidate { std::string display; std::string insert; std::string desc; };
                std::vector<DmCandidate> dms;
                for (const auto& record : agent_runtime::load_all_native_agent_records()) {
                    const auto name = record.name.value_or(record.agent_id);
                    if (!starts_with_ci(name, query)) continue;
                    dms.push_back(DmCandidate{
                        .display = "@" + std::string(name),
                        .insert = "@" + std::string(name) + " ",
                        .desc = "Teammate · " +
                                std::string(agent_runtime::native_agent_status_name(record.status)),
                    });
                }
                if (!dms.empty()) {
                    for (auto& d : dms) {
                        add_suggestion(std::move(d.display), std::move(d.desc),
                                       std::move(d.insert), token.start, token.end, false);
                    }
                    restore_index();
                    return;  // exclusive: bare-@ with teammate match shows DMs only
                }
            }

            std::error_code ec;
            // AT-06: skip the file index when the query is empty (bare "@") —
            // TS shows teammates/MCP/agents on an empty @ query, not every file
            // in the repo. With a non-empty query the index fuzzy-matches.
            if (bare_query && !query.empty()) {
                struct RepoCandidate { std::string display; std::string insert; std::string desc; int rank; };
                std::vector<RepoCandidate> repos;
                for (const auto& rel : fidx::collect_repo_files(screen_state_->cwd)) {
                    const std::size_t slash = rel.find_last_of("/\\");
                    const std::string base = (slash == std::string::npos)
                        ? rel : rel.substr(slash + 1);
                    const bool match_base = frn::fuzzy_match_nucleo(base, query);
                    const bool match_path = !match_base && frn::fuzzy_match_nucleo(rel, query);
                    if (!match_base && !match_path) continue;
                    repos.push_back(RepoCandidate{
                        .display = "@" + rel,
                        .insert = "@" + rel + " ",
                        .desc = "File",
                        .rank = frn::fuzzy_rank_nucleo(base, query) + (match_base ? 0 : 2),
                    });
                    if (repos.size() >= 60) break;
                }
                std::ranges::sort(repos, [](const auto& a, const auto& b) {
                    if (a.rank != b.rank) return a.rank < b.rank;
                    return a.display < b.display;
                });
                for (auto& r : repos) {
                    add_suggestion(std::move(r.display), std::move(r.desc),
                                   std::move(r.insert), token.start, token.end, false);
                }
            } else if (std::filesystem::is_directory(parent, ec)) {
                struct FileCandidate { std::string display; std::string insert; std::string desc; int rank; };
                std::vector<FileCandidate> files;
                for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
                    if (ec) break;
                    const auto name = entry.path().filename().string();
                    if (name.starts_with(".")) continue;
                    if (!frn::fuzzy_match_nucleo(name, leaf)) continue;
                    const bool is_dir = entry.is_directory(ec);
                    auto rel = prefix + name + (is_dir ? "/" : "");
                    files.push_back(FileCandidate{
                        .display = "@" + rel,
                        .insert = "@" + rel,
                        .desc = is_dir ? "Directory" : "File",
                        .rank = frn::fuzzy_rank_nucleo(name, leaf),
                    });
                    if (files.size() >= 60) break;
                }
                std::ranges::sort(files, [](const auto& a, const auto& b) {
                    if (a.rank != b.rank) return a.rank < b.rank;
                    return a.display < b.display;
                });
                for (auto& file : files) {
                    add_suggestion(
                        std::move(file.display),
                        std::move(file.desc),
                        std::move(file.insert),
                        token.start,
                        token.end,
                        false);
                }
            }

            std::optional<std::filesystem::path> cwd_path;
            if (!screen_state_->cwd.empty()) cwd_path = std::filesystem::path(screen_state_->cwd);
            for (const auto& agent : agent_runtime::get_all_agent_definitions(cwd_path)) {
                if (!frn::fuzzy_match_nucleo(agent.agent_type, query)) continue;
                add_suggestion(
                    "@" + agent.agent_type,
                    "Agent · " + summarize_agent_description(agent.when_to_use),
                    "@" + agent.agent_type + " ",
                    token.start,
                    token.end,
                    false);
            }
            for (const auto& record : agent_runtime::load_all_native_agent_records()) {
                const auto name = record.name.value_or(record.agent_id);
                if (!frn::fuzzy_match_nucleo(name, query)) continue;
                add_suggestion(
                    "@" + name,
                    "Teammate · " + std::string(agent_runtime::native_agent_status_name(record.status)),
                    "@" + name + " ",
                    token.start,
                    token.end,
                    false);
            }
            for (const auto& resource : acsrc::collect_mcp_resource_suggestions()) {
                if (!frn::fuzzy_match_nucleo(resource.display, query) &&
                    !frn::fuzzy_match_nucleo(resource.insert_text, query)) {
                    continue;
                }
                add_suggestion(
                    "@" + resource.display,
                    resource.description,
                    "@" + resource.insert_text + " ",
                    token.start,
                    token.end,
                    false);
            }
            restore_index();
            return;
        }

        // INF-01/AT-08: # channels are suppressed in bash mode. Faithful to TS
        // useTypeahead (the #slack branch is gated on mode === 'prompt'); in
        // bash mode we fall through to the $PATH shell-command scan below.
        if (token.text.starts_with('#') &&
            screen_state_->input_mode != repl::InputMode::Bash) {
            const auto query = std::string_view(token.text).substr(1);
            for (const auto& resource : acsrc::collect_mcp_resource_suggestions()) {
                if (!resource.channel_like) continue;
                const auto display = resource.display.starts_with("#")
                    ? resource.display
                    : "#" + resource.display;
                if (!frn::fuzzy_match_nucleo(display, token.text) &&
                    !frn::fuzzy_match_nucleo(resource.insert_text, query)) {
                        continue;
                    }
                add_suggestion(
                    display,
                    resource.description,
                    display + " ",
                    token.start,
                    token.end,
                    false);
            }
            restore_index();
            return;
        }

        if (screen_state_->input_mode == repl::InputMode::Bash && !token.text.empty()) {
            std::unordered_set<std::string> seen;
            if (const char* path_env = std::getenv("PATH")) {
                std::string_view paths(path_env);
                while (!paths.empty()) {
                    auto sep = paths.find(':');
                    auto current = sep == std::string_view::npos
                        ? paths
                        : paths.substr(0, sep);
                    if (sep == std::string_view::npos) paths = {};
                    else paths.remove_prefix(sep + 1);

                    std::error_code ec;
                    std::filesystem::path dir{std::string(current)};
                    if (!std::filesystem::is_directory(dir, ec)) continue;
                    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                        if (ec) break;
                        auto name = entry.path().filename().string();
                        if (seen.contains(name) || !frn::fuzzy_match_nucleo(name, token.text)) continue;
                        seen.insert(name);
                        add_suggestion(
                            name,
                            "Shell command",
                            name + " ",
                            token.start,
                            token.end,
                            false);
                        if (screen_state_->autocomplete_suggestions.size() >= 50) break;
                    }
                    if (screen_state_->autocomplete_suggestions.size() >= 50) break;
                }
            }
            restore_index();
        }
    }

    [[nodiscard]] static bool is_built_in_agent(
        const agent_cards::AgentCardData& agent) {
        return agent.source == "built-in";
    }

    [[nodiscard]] static std::vector<std::size_t> selectable_agent_indices(
        const std::vector<agent_cards::AgentCardData>& agents) {
        std::vector<std::size_t> out;
        out.reserve(agents.size());
        for (std::size_t i = 0; i < agents.size(); ++i) {
            if (!is_built_in_agent(agents[i])) out.push_back(i);
        }
        return out;
    }

    [[nodiscard]] static std::string agent_model_label(
        const agent_cards::AgentCardData& agent) {
        if (agent.model_override && !agent.model_override->empty()) {
            return *agent.model_override;
        }
        return is_built_in_agent(agent) ? "inherit" : "";
    }

    [[nodiscard]] static std::string FormatAgentsMenuOutput(
        const std::vector<agent_cards::AgentCardData>& agents,
        int selected_position) {
        const auto selectable = selectable_agent_indices(agents);
        const int selectable_count = static_cast<int>(selectable.size());
        const int display_count = static_cast<int>(agents.size());
        const int item_count = std::max(1, selectable_count + 1);
        selected_position = std::clamp(selected_position, 0, item_count - 1);

        std::string out;
        out += "Agents\n";
        if (selectable_count == 0) {
            out += "No agents found\n";
        } else {
            out += std::format(
                "{} agent{}\n",
                display_count,
                display_count == 1 ? "" : "s");
        }

        out += "\n";
        out += selected_position == 0 ? "› Create new agent\n"
                                      : "  Create new agent\n";

        if (selectable_count == 0) {
            out += "\n";
            out += "No agents found. Create specialized subagents that Claude can delegate to.\n";
            out += "Each subagent has its own context window, custom system prompt, and specific tools.\n";
            out += "Try creating: Code Reviewer, Code Simplifier, Security Reviewer, Tech Lead, or UX Reviewer.\n";
        } else {
            int position = 1;
            for (const auto& group : agent_display::agent_source_groups()) {
                if (group.source == "built-in") continue;

                bool has_group = false;
                for (const auto& agent : agents) {
                    if (agent.source == group.source) {
                        has_group = true;
                        break;
                    }
                }
                if (!has_group) continue;

                out += "\n";
                out += group.label;
                out += "\n";
                for (const auto& agent : agents) {
                    if (agent.source != group.source) continue;
                    out += position == selected_position ? "› " : "  ";
                    out += agent.name;
                    const auto model = agent_model_label(agent);
                    if (!model.empty()) {
                        out += " · ";
                        out += model;
                    }
                    out += "\n";
                    ++position;
                }
            }
        }

        bool has_built_in = false;
        for (const auto& agent : agents) {
            if (is_built_in_agent(agent)) {
                has_built_in = true;
                break;
            }
        }
        if (has_built_in) {
            out += "\n────────────────────────────────────────────────────────────────\n\n";
            out += selectable_count == 0
                ? "Built-in (always available):\n"
                : "Built-in agents (always available)\n";
            for (const auto& agent : agents) {
                if (!is_built_in_agent(agent)) continue;
                out += agent.name;
                const auto model = agent_model_label(agent);
                if (!model.empty()) {
                    out += " · ";
                    out += model;
                }
                out += "\n";
            }
        }

        out += "\nPress ↑↓ to navigate · Enter to select · Esc to go back";
        return out;
    }

    void RefreshAgentsMenuOutput() {
        screen_state_->active_local_jsx_content = FormatAgentsMenuOutput(
            screen_state_->agent_cards,
            screen_state_->active_agents_selection_position);
    }

    void LoadAgentCardsForMenu() {
        std::optional<std::filesystem::path> cwd;
        if (!screen_state_->cwd.empty()) {
            cwd = std::filesystem::path(screen_state_->cwd);
        }

        auto definitions = agent_runtime::get_all_agent_definitions(std::move(cwd));
        std::ranges::sort(definitions, agent_display::CompareAgentsByName{});

        screen_state_->agent_cards.clear();
        screen_state_->agent_cards.reserve(definitions.size());
        for (const auto& definition : definitions) {
            screen_state_->agent_cards.push_back(
                project_agent_definition_card(definition));
        }
        screen_state_->agents_component.reset();
    }

    void OpenAgentsMenu() {
        LoadAgentCardsForMenu();
        screen_state_->mode = repl::ReplMode::Normal;
        screen_state_->active_local_jsx_command = true;
        screen_state_->active_local_jsx_command_name = "agents";
        screen_state_->active_local_jsx_command_args.clear();
        screen_state_->active_agents_selection_position = 0;
        RefreshAgentsMenuOutput();
        screen_state_->scroll_offset = 0;
        screen_state_->scroll_pinned_to_bottom = false;
        PostRenderEvent();
    }

    bool HandleLocalJsxEvent(const Event& ev) {
        if (!screen_state_->active_local_jsx_command ||
            screen_state_->active_local_jsx_command_name != "agents") {
            return false;
        }

        const auto selectable = selectable_agent_indices(screen_state_->agent_cards);
        const int item_count = 1 + static_cast<int>(selectable.size());
        if (item_count <= 0) return false;

        auto refresh_selection = [&] {
            RefreshAgentsMenuOutput();
            PostRenderEvent();
        };

        if (ev == Event::ArrowDown || ev == Event::Character('j')) {
            screen_state_->active_agents_selection_position =
                (screen_state_->active_agents_selection_position + 1) % item_count;
            refresh_selection();
            return true;
        }
        if (ev == Event::ArrowUp || ev == Event::Character('k')) {
            screen_state_->active_agents_selection_position =
                (screen_state_->active_agents_selection_position - 1 + item_count) %
                item_count;
            refresh_selection();
            return true;
        }
        if (ev == Event::Return) {
            const int selected = std::clamp(
                screen_state_->active_agents_selection_position,
                0,
                item_count - 1);
            std::string command = "/agents create";
            if (selected > 0) {
                const auto agent_index =
                    selectable[static_cast<std::size_t>(selected - 1)];
                command = "/agents configure " +
                    screen_state_->agent_cards[agent_index].id;
            }
            ClearActiveLocalJsxCommand();
            screen_state_->scroll_offset = 0;
            screen_state_->scroll_pinned_to_bottom = true;
            HandleCommand(command);
            PostRenderEvent();
            return true;
        }
        return false;
    }

    [[nodiscard]] static int skill_source_order(std::string_view source) {
        if (source == "project") return 0;
        if (source == "user") return 1;
        if (source == "plugin") return 2;
        if (source == "mcp") return 3;
        return 4;
    }

    [[nodiscard]] static bool is_visible_skills_menu_source(
        std::string_view source) {
        return source == "project" ||
               source == "user" ||
               source == "plugin" ||
               source == "mcp";
    }

    [[nodiscard]] static bool utf8_continuation(unsigned char ch) {
        return (ch & 0xC0) == 0x80;
    }

    [[nodiscard]] static std::size_t utf16_code_unit_count(
        std::string_view value) {
        std::size_t count = 0;
        for (std::size_t i = 0; i < value.size();) {
            const auto c0 = static_cast<unsigned char>(value[i]);
            std::uint32_t codepoint = c0;
            std::size_t length = 1;

            if (c0 < 0x80) {
                codepoint = c0;
            } else if ((c0 & 0xE0) == 0xC0 &&
                       i + 1 < value.size() &&
                       utf8_continuation(static_cast<unsigned char>(value[i + 1]))) {
                codepoint =
                    (static_cast<std::uint32_t>(c0 & 0x1F) << 6) |
                    static_cast<std::uint32_t>(
                        static_cast<unsigned char>(value[i + 1]) & 0x3F);
                length = 2;
            } else if ((c0 & 0xF0) == 0xE0 &&
                       i + 2 < value.size() &&
                       utf8_continuation(static_cast<unsigned char>(value[i + 1])) &&
                       utf8_continuation(static_cast<unsigned char>(value[i + 2]))) {
                codepoint =
                    (static_cast<std::uint32_t>(c0 & 0x0F) << 12) |
                    (static_cast<std::uint32_t>(
                         static_cast<unsigned char>(value[i + 1]) & 0x3F) << 6) |
                    static_cast<std::uint32_t>(
                        static_cast<unsigned char>(value[i + 2]) & 0x3F);
                length = 3;
            } else if ((c0 & 0xF8) == 0xF0 &&
                       i + 3 < value.size() &&
                       utf8_continuation(static_cast<unsigned char>(value[i + 1])) &&
                       utf8_continuation(static_cast<unsigned char>(value[i + 2])) &&
                       utf8_continuation(static_cast<unsigned char>(value[i + 3]))) {
                codepoint =
                    (static_cast<std::uint32_t>(c0 & 0x07) << 18) |
                    (static_cast<std::uint32_t>(
                         static_cast<unsigned char>(value[i + 1]) & 0x3F) << 12) |
                    (static_cast<std::uint32_t>(
                         static_cast<unsigned char>(value[i + 2]) & 0x3F) << 6) |
                    static_cast<std::uint32_t>(
                        static_cast<unsigned char>(value[i + 3]) & 0x3F);
                length = 4;
            }

            count += codepoint > 0xFFFF ? 2 : 1;
            i += length;
        }
        return count;
    }

    [[nodiscard]] static std::size_t rough_js_token_count(
        std::string_view value) {
        return static_cast<std::size_t>(
            std::llround(static_cast<double>(utf16_code_unit_count(value)) / 4.0));
    }

    [[nodiscard]] static std::size_t skills_menu_token_estimate(
        const acsrc::SkillSuggestionData& skill) {
        std::string frontmatter = skill.name;
        if (!skill.description.empty()) {
            frontmatter.push_back(' ');
            frontmatter += skill.description;
        }
        return rough_js_token_count(frontmatter);
    }

    [[nodiscard]] static std::string collapse_home_path(std::string path) {
        if (const char* home = std::getenv("HOME"); home && *home) {
            const std::string home_path(home);
            if (path == home_path) return "~";
            if (path.starts_with(home_path + "/")) {
                return "~" + path.substr(home_path.size());
            }
        }
        return path;
    }

    [[nodiscard]] static std::string skill_source_group_title(
        const acsrc::SkillSuggestionData& skill) {
        if (skill.source == "project") {
            return skill.source_detail.empty()
                ? "Project skills"
                : "Project skills (" + collapse_home_path(skill.source_detail) + ")";
        }
        if (skill.source == "user") return "User skills (~/.claude/skills)";
        if (skill.source == "plugin") {
            return skill.source_detail.empty()
                ? "Plugin skills"
                : "Plugin skills (" + skill.source_detail + ")";
        }
        if (skill.source == "mcp") return "MCP skills";
        return "Other skills";
    }

    [[nodiscard]] static std::string FormatSkillsMenuOutput(
        std::vector<acsrc::SkillSuggestionData> skills) {
        std::erase_if(skills, [](const auto& skill) {
            return !is_visible_skills_menu_source(skill.source);
        });

        std::ranges::sort(skills, [](const auto& a, const auto& b) {
            const int ao = skill_source_order(a.source);
            const int bo = skill_source_order(b.source);
            if (ao != bo) return ao < bo;
            if (a.source_detail != b.source_detail) {
                return a.source_detail < b.source_detail;
            }
            return a.name < b.name;
        });

        std::string out;
        out += "Skills\n";
        out += std::format(
            "{} skill{}\n",
            skills.size(),
            skills.size() == 1 ? "" : "s");

        if (skills.empty()) {
            out += "\nNo skills found.\n";
            out += "Create skills under `.claude/skills` or `~/.claude/skills`.\n";
            return out;
        }

        std::string current_group;
        bool first_group = true;
        for (const auto& skill : skills) {
            const std::string group = skill_source_group_title(skill);
            if (group != current_group) {
                if (!first_group) out += "\n";
                first_group = false;
                current_group = group;
                out += "\n" + current_group + "\n";
            }

            out += skill.name;
            out += std::format(
                " · ~{} description tokens",
                skills_menu_token_estimate(skill));
            out += "\n";
        }
        return out;
    }

    void OpenSkillsMenu() {
        auto skills = acsrc::collect_skill_suggestions(screen_state_->cwd);
        screen_state_->mode = repl::ReplMode::Normal;
        screen_state_->active_local_jsx_command = true;
        screen_state_->active_local_jsx_command_name = "skills";
        screen_state_->active_local_jsx_command_args.clear();
        screen_state_->active_local_jsx_content =
            FormatSkillsMenuOutput(std::move(skills));
        screen_state_->scroll_offset = 0;
        screen_state_->scroll_pinned_to_bottom = false;
        PostRenderEvent();
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
               cc::hooks::LifecycleHookRegistry* lifecycle_hooks,
               cc::commands::AppCommandRegistry* cmd_registry,
               utils::SessionStorage* storage,
               std::function<void()> on_exit)
        : engine_(engine),
          lifecycle_hooks_(lifecycle_hooks),
          cmd_registry_(cmd_registry),
          storage_(storage),
          on_exit_(std::move(on_exit)),
          screen_state_(std::make_shared<repl::ReplScreenState>()) {

        // ── M7: Register default dialog renderers in the registry ────
        // Overlay (ToolPermission), plus any future default slots.
        cc::ui::dialogs::default_renderers::register_default_renderers(
            screen_state_->dialog_renderers);

        // ── SL-11: deterministic next-action suggestion on QueryEnd ──
        // Impl lives in app_prompt_suggestion_wiring.cpp (impl unit) so the
        // heavy cc.services.prompt_suggestion import stays out of this thin
        // module (clang 2GB source-location budget).
        if (lifecycle_hooks_) {
            wire_prompt_suggestion_hook(*lifecycle_hooks_, engine_, screen_state_);
        }

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

        // ── Load settings from disk and project into screen state ──────────
        // Settings come from ~/.claude/settings.json (user),
        // ./.claude/settings.json (project), etc. — merged by SettingsManager.
        // We project the subset the renderer needs (model, statusLine, …) into
        // screen_state_ so repl_screen can read it without depending on the
        // settings manager directly.
        settings_manager_ = std::make_unique<cc::utils::settings_manager::SettingsManager>();
        settings_manager_->initialize();
        this->ProjectSettingsToScreenState();
        this->ProjectRuntimeMetadataToScreenState();

        // Re-project settings whenever they change on disk (e.g. user edits
        // settings.json from another terminal, or the /config command saves).
        settings_unsubscribe_ = settings_manager_->on_change(
            [this](cc::utils::settings_manager::SettingSource) {
                this->ProjectSettingsToScreenState();
                // Settings change → statusline may need re-run (command changed).
                this->TriggerStatuslineUpdate();
                PostRenderEvent();
            });

        repl::ReplScreenCallbacks cbs;
        cbs.on_submit = [this](const std::string& text, repl::InputMode) {
            this->HandleSubmit(text);
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

        // ── IDE at_mentioned responder (AT-09) ───────────────────────────
        // Faithful to TS useIdeAtMentioned.ts: on an inbound MCP
        // "at_mentioned" notification we insert "@<relpath>#L<a>-<b>" at the
        // prompt cursor. The MCP receive thread calls us via
        // McpConnectionManager::handle_server_notification -> dispatch_at_mention,
        // so we only stage the token on ReplScreenState and let Render() drain
        // it (see DrainPendingAtMentionInserts) to keep input_text mutations
        // on the render thread.
        cc::services::mcp::set_at_mention_responder(
            [this](const cc::services::mcp::AtMentionNotification& n)
                -> void
            {
                if (n.file_path.empty()) return;
                // TS inserts "@<relpath>#L<a>-<b>"; relpath is the file_path
                // verbatim (IDE provides it relative to the workspace root).
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

        this->SyncState();

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
                auto input = this->BuildStatuslineInput();

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
        this->TriggerStatuslineUpdate();
        StartUiAnimationTicker();
    }

    void HandleSubmit(const std::string& text) {
        if (text.empty()) return;

        if (text.starts_with('/')) {
            this->HandleCommand(text);
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

            // AT-02: materialize @-mention file references into content blocks
            // so the model sees file contents, not the literal "@path" string.
            auto materialized = atatt::materialize_at_mentions(text, screen_state_->cwd);
            opts.attachments = std::move(materialized.blocks);

            engine_->stream_query(materialized.text, opts);

            query_running_.store(false);
            PostRenderEvent();
        });
    }

    void HandleCommand(std::string_view cmd) {
        std::string command = trim_ascii_copy(cmd);
        std::string_view normalized = command;
        if (normalized.empty()) return;

        if (normalized == "/exit" || normalized == "/quit") {
            if (on_exit_) on_exit_();
            return;
        }
        if (normalized == "/clear") {
            engine_->clear_conversation();
            local_command_messages_.clear();
            this->SyncState();
            return;
        }
        if (normalized == "/compact") {
            auto result = engine_->compact_conversation();
            if (result) this->SyncState();
            return;
        }
        if (normalized == "/cost") {
            auto usage = engine_->get_usage();
            auto cost = engine_->budget_tracker().current_spend_usd;
            screen_state_->spinner_tip = std::format(
                "Cost: ${:.4f} | In: {} | Out: {} | Ctx: {:.0f}%",
                cost, usage.input_tokens, usage.output_tokens,
                engine_->context_utilization() * 100.0);
            return;
        }
        if (normalized.starts_with("/model")) {
            auto args_start = normalized.find(' ');
            if (args_start != std::string_view::npos) {
                auto new_model = normalized.substr(args_start + 1);
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
            this->SyncState();
            return;
        }
        if (normalized.starts_with("/vim")) {
            auto args_start = normalized.find(' ');
            if (args_start == std::string_view::npos)
                vim_enabled_ = !vim_enabled_;
            else {
                auto arg = normalized.substr(args_start + 1);
                vim_enabled_ = (arg == "on" || arg == "1");
            }
            if (vim_enabled_) cc::vim::enable_vim_mode();
            else cc::vim::disable_vim_mode();
            return;
        }

        if (normalized == "/config" || normalized == "/settings") {
            screen_state_->mode = repl::ReplMode::SettingsView;
            // Note: if the engine later exposes a ConfigManager accessor,
            // wire it here.  For now the dialog uses its internal fallback.
            screen_state_->settings_config = nullptr;
            return;
        }

        if (normalized == "/agents") {
            this->OpenAgentsMenu();
            return;
        }

        if (normalized == "/skills") {
            this->OpenSkillsMenu();
            return;
        }

        if (auto parsed = cc::core::CommandRegistry::parse(normalized)) {
            const bool known_command =
                cmd_registry_ && cmd_registry_->has_command(parsed->name);
            if (!known_command) {
                if (auto skill =
                        acsrc::find_skill_suggestion(screen_state_->cwd, parsed->name)) {
                    // SL-10: reject skills marked user_invocable=false (model-only).
                    if (!skill->user_invocable) {
                        AppendLocalCommandInputMessage(std::string(normalized));
                        AppendLocalCommandMessage(
                            std::format("This skill can only be invoked by Claude, not "
                                        "directly by users. Ask Claude to use the \"{}\" skill.",
                                        skill->name),
                            true);
                        return;
                    }
                    std::string user_text;
                    const auto args_start = normalized.find(' ');
                    if (args_start != std::string_view::npos) {
                        user_text = trim_ascii_copy(normalized.substr(args_start + 1));
                    }
                    cc::utils::skill_usage::record_skill_usage(skill->name);  // SL-04
                    this->HandleSubmit(acsrc::skill_invocation_prompt(*skill, user_text));
                    return;
                }
                // SL-09: unknown-command file-path disambiguation (TS
                // processSlashCommand.tsx:332-361). When the name looks like a
                // command (only [a-zA-Z0-9:-_]) but isn't one, and a same-named
                // path exists in cwd, surface that hint instead of a bare error.
                const auto looks_like_command = [](std::string_view name) {
                    return !name.empty() &&
                        name.find_first_not_of(
                            "abcdefghijklmnopqrstuvwxyz"
                            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                            "0123456789:-_") == std::string_view::npos;
                };
                if (looks_like_command(parsed->name)) {
                    std::error_code ec;
                    const std::filesystem::path candidate{"./" + parsed->name};
                    if (std::filesystem::exists(candidate, ec) && !ec) {
                        AppendLocalCommandInputMessage(std::string(normalized));
                        AppendLocalCommandMessage(
                            std::format("\"/{}\" is not a command or skill, but \"{}\" "
                                        "exists in the current directory. If you meant to "
                                        "reference the file, drop the leading slash.",
                                        parsed->name, candidate.string()),
                            false);
                        return;
                    }
                }
            }
        }

        if (cmd_registry_) {
            auto result = cmd_registry_->execute(
                command,
                command_context_for_engine(engine_, screen_state_->cwd));
            if (result) {
                if (result->status == CommandStatus::Injected) {
                    this->HandleSubmit(result->message);
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
                            this->HandleCommand(c);
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
                AppendCommandResult(*result);
                return;
            }
            AppendLocalCommandMessage(result.error().message, true);
            return;
        } else {
            AppendLocalCommandMessage("Command registry is not available.", true);
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

        screen_state_->cwd = engine_->working_directory();
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
        const auto cwd = engine_->working_directory();
        input.workspace.current_dir = cwd;
        input.workspace.project_dir = cwd;
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
        AppendLocalMessagesToScreenState();
        // Restore chronological order: local-command rows are created at
        // dismiss/submit time but held in a side vector; merge them with the
        // engine rows by timestamp. Stable sort preserves intra-source order
        // (engine conversation order, /skills-before-dismissed pair). TS uses
        // ONE chronological array (REPL.tsx onQuery [...prev, ...newMessages]);
        // cpp's engine-first/local-appended split inverted chronology.
        std::stable_sort(
            screen_state_->messages.begin(),
            screen_state_->messages.end(),
            [](const repl::MessageDisplayEntry& a,
               const repl::MessageDisplayEntry& b) {
                return a.timestamp < b.timestamp;
            });

        this->ProjectRuntimeMetadataToScreenState();

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

        this->SyncState();
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
        this->TriggerStatuslineUpdate();
    }

    Element Render() override {
        this->ProjectRuntimeMetadataToScreenState();
        ConsumePendingResult();
        // AT-09: apply any inbound IDE at_mentioned tokens that landed since
        // the last frame (drained on the render thread for input_text safety).
        repl::DrainPendingAtMentionInserts(screen_state_);

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
            AppendLocalMessagesToScreenState();
            // Restore chronological order (see SyncState). Applied BEFORE the
            // in-flight streaming projection so streaming rows stay last.
            std::stable_sort(
                screen_state_->messages.begin(),
                screen_state_->messages.end(),
                [](const repl::MessageDisplayEntry& a,
                   const repl::MessageDisplayEntry& b) {
                    return a.timestamp < b.timestamp;
                });

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
        // Any active declared_cursor decorator on descendant elements can
        // override this with a physical cursor anchor at the declared position.
        // This enables IME preedit text to appear inline at the insertion
        // point and lets screen readers / magnifiers follow the input.
        namespace dc = cc::ui::common::declared_cursor;
        return repl_component_->Render() | dc::cursor_reset();
    }

    bool OnEvent(Event event) override {
        const bool handled = repl_component_->OnEvent(event);
        if (handled && event != Event::Escape) {
            // SL-11: any accepted keystroke that fills input retires the
            // next-action suggestion for this turn (RefreshAutocompleteSuggestions
            // also clears on non-empty input, but this is the unambiguous
            // reset for the accept-on-Return path).
            if (!screen_state_->input_text.empty()) {
                screen_state_->next_action_suggestion.reset();
            }
            RefreshAutocompleteSuggestions();
        }
        return handled;
    }

    Component ActiveChild() override {
        return repl_component_;
    }

    void set_screen(ScreenInteractive* screen) {
        screen_.store(screen, std::memory_order_release);
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

    // Debug/testing: snapshot screen_state_->messages as "label:preview" rows
    // to verify transcript ordering (local-command vs user vs assistant).
    [[nodiscard]] std::vector<std::string> messages_for_testing() const {
        std::vector<std::string> out;
        out.reserve(screen_state_->messages.size());
        for (const auto& m : screen_state_->messages) {
            std::string label = m.role;
            if (m.is_local_command_input) label = "lc-input";
            else if (m.is_local_command_output) label = "lc-output";
            else if (m.is_thinking) label = "thinking";
            std::string pv = m.content_preview.substr(
                0, std::min<std::size_t>(30, m.content_preview.size()));
            out.push_back(label + ":" + pv);
        }
        return out;
    }

    [[nodiscard]] std::string input_text_for_testing() const {
        return screen_state_->input_text;
    }

    [[nodiscard]] bool is_agents_view_for_testing() const noexcept {
        return screen_state_->mode == repl::ReplMode::AgentsView;
    }

    [[nodiscard]] bool is_local_jsx_command_for_testing(
        std::string_view command_name) const noexcept {
        return screen_state_->active_local_jsx_command &&
               screen_state_->active_local_jsx_command_name == command_name;
    }

    [[nodiscard]] int active_agents_selection_position_for_testing() const noexcept {
        return screen_state_->active_agents_selection_position;
    }

    [[nodiscard]] std::size_t agent_card_count_for_testing() const noexcept {
        return screen_state_->agent_cards.size();
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
    cc::hooks::ToolPermissionHook* permission_hook = nullptr,
    cc::hooks::LifecycleHookRegistry* lifecycle_hooks = nullptr
) {
    // Use the alternate-screen fullscreen like TS (AlternateScreen) - the REPL owns the terminal.
    auto screen = ScreenInteractive::Fullscreen();

    bool should_exit = false;

    auto app = Make<AppAdapter>(
        &engine,
        lifecycle_hooks,
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
    cc::hooks::LifecycleHookRegistry* lifecycle_hooks,
    cc::commands::AppCommandRegistry* cmd_registry,
    cc::utils::SessionStorage* storage,
    cc::hooks::ToolPermissionHook* permission_hook
) {
    return cc::ui::RunApp(*engine, *cmd_registry, *storage, permission_hook, lifecycle_hooks);
}
