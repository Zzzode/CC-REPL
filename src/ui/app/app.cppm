/// @file app.cppm
/// @brief Application entry point — thin adapter that drives repl_screen from
///        the production QueryEngine.
module;

#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <cstdio>
#include <memory>
#include <optional>
#include <expected>
#include <functional>
#include <chrono>
#include <format>
#include <fstream>
#include <initializer_list>
#include <deque>
#include <map>
#include <set>
#include <unordered_set>
#include <unordered_map>
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

export module cc.ui.app.app;

import cc.types.types;
import cc.ui.widgets.components;
import cc.ui.widgets.all_components;
import cc.ui.visual.markdown;
import cc.ui.screens.repl_state;
// FooterVoiceState for the voice-processing animation gate below.
import cc.ui.prompt.voice_indicator;
import cc.ui.prompt.autocomplete_sources;
// P0-2: 7-stage message pipeline utilities (dedup / tag filter / tool augment).
import cc.ui.messages.message_pipeline;
import cc.ui.features.teams.live_teammates;
import cc.ui.dialogs.system;

export namespace cc::ui {

// PIMPL backing type, defined in the internal partition cc.ui.app.app:impl.
// Forward-declared here so the interface can hold a unique_ptr without
// importing the heavy modules its member objects require.
struct AppImpl;
// Deleter whose call operator is defined in the :impl partition (where
// AppImpl is complete). This lets unique_ptr<AppImpl> be destroyed from any
// translation unit — including the out-of-line constructor's implicit
// cleanup — without AppImpl being complete there.
struct AppImplDeleter {
    void operator()(AppImpl* p) const noexcept;
};

// Nested PIMPL for the teammate inbox/permission cluster. The backing struct
// lives entirely in app_team.cpp (a plain impl unit), keeping swarm_helpers /
// team_helpers / swarm_backends out of both this interface and the :impl
// partition.
struct TeammateState;
struct TeammateStateDeleter {
    void operator()(TeammateState* p) const noexcept;
};

// Nested PIMPL for settings (disk load + file-watch). Defined in
// app_settings.cpp; keeps cc.utils.settings_manager out of the :impl BMI.
struct SettingsState;
struct SettingsStateDeleter {
    void operator()(SettingsState* p) const noexcept;
};

// Defined in app_store_bridge.cpp; type-erased shared_ptr factory so :impl
// never imports cc.state.{store,app_state}.
[[nodiscard]] std::shared_ptr<void> create_typed_app_store();

// Plain-data projection of the AppState bridge fields, returned by
// AppAdapter::bridge_state() so callers need not import cc.state.app_state.
struct BridgeState {
    bool enabled = false;
    bool explicit_remote = false;
    bool connected = false;
    bool session_active = false;
    bool reconnecting = false;
};

// SL-11: defined in app_prompt_suggestion_wiring.cpp (impl unit) to keep the
// heavy cc.services.prompt_suggestion import out of this thin module (clang
// 2GB source-location budget).
void wire_prompt_suggestion_hook(void* hooks, void* engine,
                                 std::shared_ptr<cc::ui::repl_screen::ReplScreenState> state);

using namespace ftxui;
using namespace cc::ui::components;
using namespace cc::core;

namespace repl = cc::ui::repl_screen;
namespace acsrc = cc::ui::autocomplete_sources;

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

    // TS REF: src/hooks/useTypeahead.tsx:272-286 — quoted @ mention detection.
    // If the token starts with @", extend end to include the full quoted content
    // (up to closing quote or end of input). This allows @"path with spaces"
    // to be treated as a single token for autocomplete.
    std::string text_before = std::string(input.substr(start, cursor - start));
    std::size_t token_end = cursor;
    if (text_before.starts_with("@\"")) {
        // Find the closing quote after cursor, or end of input.
        std::size_t close = input.find('"', cursor);
        if (close != std::string_view::npos) {
            token_end = close + 1;  // include the closing quote
        } else {
            token_end = input.size();  // unterminated quote — extend to end
        }
    }

    return AutocompleteToken{
        .start = start,
        .end = token_end,
        .text = std::string(input.substr(start, cursor - start)),
    };
}

// AT-12: fuzzy_match_ascii / fuzzy_rank_ascii removed — all autocomplete
// ranking now delegates to cc::ui::prompt::fuzzy_rank_nucleo (frn::), which
// ports the nucleo/fzf-v2 scorer (boundary/camel/consecutive/gap/path bonuses)
// while preserving the exact {0..3} base range so the tier offsets (alias +1,
// skill +4, plugin +6) and the rank-ascending sort stay unchanged. See
// ui/prompt/fuzzy_rank_nucleo.cppm. lowercase_ascii() above is retained.

// ============================================================
// Projection: Engine state -> ReplScreenState
// ============================================================

[[nodiscard]] repl::MessageDisplayEntry project_message(const Message& msg);

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
[[nodiscard]] std::vector<repl::MessageDisplayEntry>
project_messages(const Message& msg);

// ============================================================
// Convenience: render a single core Message to an Element.
// Used by tests and callers that want a quick rendering of one message.
// ============================================================

[[nodiscard]] Element RenderMessage(const Message& msg);

// Defined out-of-line (app_constructor.cpp) to keep the
// cc.ui.foundation.theme_provider closure out of this interface.
[[nodiscard]] bool reduced_motion_enabled();

// ============================================================
// App Adapter Component
// ============================================================

class AppAdapter : public ComponentBase {
private:
    std::unique_ptr<AppImpl, AppImplDeleter> impl_;
    std::unique_ptr<TeammateState, TeammateStateDeleter> teammate_;
    std::unique_ptr<SettingsState, SettingsStateDeleter> settings_;
    // Defined in the :impl partition where AppImpl is complete. The out-of-line
    // constructor body calls this; teardown goes through AppImplDeleter, so
    // neither impl unit needs AppImpl's layout.
    // Stored type-erased as void* in AppImpl; each impl unit casts back
    // after importing the owning module (cc.query/cc.hooks/cc.commands/
    // cc.utils.session_storage), keeping those closures out of this BMI.
    void construct_impl(void* engine, void* lifecycle_hooks,
                        void* cmd_registry, void* storage);
    void construct_teammate();
    void construct_settings();
    [[nodiscard]] void* engine_raw() const noexcept;
    [[nodiscard]] void* lifecycle_hooks_raw() const noexcept;
    [[nodiscard]] void* cmd_registry_raw() const noexcept;
    [[nodiscard]] void* storage_raw() const noexcept;

    std::function<void()> on_exit_;

    std::shared_ptr<repl::ReplScreenState> screen_state_;
    Component repl_component_;
    std::vector<repl::MessageDisplayEntry> local_command_messages_;

    std::string current_session_id_;

    // Ctrl-C double-press ExitHandler moved into AppImpl (:impl partition);
    // access via set_exit_message_impl/reset_exit_handler/handle_ctrl_c.

    // Session start time for duration tracking (statusline cost.total_duration_ms)
    std::chrono::steady_clock::time_point session_start_time_;

    // Async query state
    std::jthread query_thread_;
    std::jthread spinner_thread_;
    // Local '!' bash command worker (TS processBashCommand.tsx). Runs the
    // command outside the LLM turn (shouldQuery:false), so it uses its own
    // thread rather than query_thread_ and never sets query_running_.
    std::jthread bash_thread_;
    std::atomic<bool> bash_running_{false};
    std::atomic<bool> query_running_{false};

    // ── Teammate inbox worker state moved into AppImpl (:impl/:team). ─────

    // ── Live-teammate projection (leader UI) ───────────────────────────────
    // Event-driven pane observer: background callbacks only flag + post an
    // FTXUI event; projection merges snapshots on the UI thread.
    std::uint64_t pane_observer_token_ = 0;
    // Held for the subscription's lifetime; releasing it on destruction makes
    // an in-flight observer callback a no-op (no use-after-free on `this`).
    std::shared_ptr<void> pane_observer_sub_guard_;
    std::atomic<bool> pane_snapshot_dirty_{false};
    // Last serialized projection; suppresses no-op state replacement.
    std::string projected_teams_signature_;

    // ── Leader-side teammate permission requests ──────────────────────────
    // Pending-permission deque + mutex moved into AppImpl (:impl/:team).

    // ── Leader inbox poller (team-lead mailbox) ────────────────────────────
    // Mirrors the pane-teammate inbox worker: a background jthread polls
    // $ROOT/<team>/inboxes/team-lead.json for stage-A permission_request
    // envelopes, enqueues them (mutex + PostRenderEvent), and the UI thread
    // drains them into the existing ToolPermission dialog. Timed mailbox
    // polling is allowed; there is still no FTXUI render ticker.
    std::jthread leader_inbox_thread_;
    std::unordered_set<std::string> seen_leader_permission_ids_;
    /// P2 gap api-error-retry: last user-submitted message text.  Used by
    /// the Retry button on SystemAPIError cards to re-send the same query.
    /// TS REF: SystemAPIErrorMessage.tsx onRetry → re-submits last prompt.
    std::string last_submitted_text_;
    // Cached autocomplete data (loaded once at startup to avoid repeated disk I/O
    // on every keystroke — TS memoizes these in useTypeahead).
    std::vector<acsrc::SkillSuggestionData> cached_skills_;
    std::vector<acsrc::PluginCommandSuggestionData> cached_plugin_commands_;
    std::atomic<std::uint64_t> ui_animation_tick_count_{0};
    std::mutex result_mutex_;
    std::optional<std::string> pending_error_;
    // Pasted clipboard images keyed by paste-id (TS pastedContents: Record).
    // Each ctrl+v image paste assigns a monotonically-increasing id and
    // inserts "[Image #N]" into input_text at the cursor.  Orphan cleanup
    // (see OnEvent + HandleSubmit) prunes entries whose placeholder is no
    // longer in the text.  TS REF: PromptInput.tsx L144 + L1151-1200.
    std::unordered_map<int, ImageBlock> pasted_contents_;
    // Pasted clipboard TEXT content keyed by paste-id.  When a >10K char
    // text paste is truncated, the middle (elided) content is stored here
    // so that HandleSubmit can expand [...Truncated text #N] refs back to
    // the full text before sending to the model.
    // TS REF: inputPaste.ts maybeTruncateInput — stores {id, type: 'text',
    //   content: placeholderContent} in pastedContents.
    std::unordered_map<int, std::string> pasted_text_contents_;
    int next_paste_id_ = 1;
    // Async paste: the placeholder "[Image #N]" is inserted into input_text
    // immediately on Ctrl+v (instant UI feedback), and the actual clipboard
    // read (osascript + PNG encode) runs on a background thread.  Results
    // are posted back via these queues and drained on the next OnEvent.
    std::mutex paste_mutex_;
    std::unordered_map<int, ImageBlock> pending_paste_results_;  // bg→UI
    std::unordered_set<int> pending_paste_failures_;             // bg→UI
    // Async text paste results: raw clipboard text keyed by paste-id.
    // Posted by SpawnPasteWorker when the clipboard has no image but does
    // have text.  ProcessCompletedPastes replaces the "[Image #N]"
    // placeholder with the text (truncating if >10K).
    std::unordered_map<int, std::string> pending_paste_text_results_;  // bg→UI
    // Track ids whose SpawnPasteWorker thread is still in flight (read hasn't
    // posted to pending_paste_results_/failures_ yet). HandleSubmit waits on
    // these so a fast Ctrl+V→Enter doesn't submit before the image data lands
    // (which would send a text-only message with [Image #N] refs but no PNG).
    std::unordered_set<int> in_flight_pastes_;
    // Local '!' bash command output posted back from bash_thread_ (bg→UI),
    // drained on the render thread in ConsumePendingResult().  Mirrors the
    // pending_paste_results_ handoff pattern so local_command_messages_ /
    // SyncState are only ever mutated on the render thread.
    std::mutex bash_result_mutex_;
    struct PendingBashResult {
        std::string output;   // combined stdout+stderr (already trimmed)
        bool is_error = false;
    };
    std::optional<PendingBashResult> pending_bash_result_;  // bg→UI
    std::string streaming_text_;
    /// TS REF: Markdown.tsx L186-235 — StreamingMarkdown stable-prefix cache
    /// for the streaming-text tail row.  Reset alongside streaming_text_ so
    /// each new model response starts with a fresh stable prefix.  Used by
    /// RenderAssistantTextMessageFaithful via MessagesListInput.streaming_md.
    ::cc::ui::StreamingMarkdown streaming_markdown_;
    struct StreamingToolPreview {
        std::string tool_name;
        std::string tool_use_id;  ///< M6: matches ToolExecution* events
        std::string input_json;
        std::string result_preview;  ///< M6: live streaming result preview
        // P0-2 Stage 5: Augmented tool result fields (lazy computed once on
        // ToolExecutionEnd — used by collapsed tool card + transcript.
        std::string compact_preview;   /// 200-char one-liner
        int         error_code      = 0;  /// 0=none, >0 shell exit/HTTP code
        bool        truncated       = false;/// result > 4 KiB threshold
        bool complete = false;       ///< ContentBlockStop: input_json fully streamed
        bool exec_done = false;      ///< ToolExecutionEnd: tool has finished executing
        bool is_error = false;
    };
    // TS REF: src/utils/messages.ts L2921-2925  StreamingThinking type
    //   { thinking, isStreaming, streamingEndedAt }
    // streaming_ended_at enables the 30s grace period after thinking stops
    // (TS REF: Messages.tsx L382-389  isStreamingThinkingVisible).
    struct StreamingThinkingPreview {
        std::string text;
        bool complete = false;
        std::optional<std::chrono::steady_clock::time_point> streaming_ended_at;
    };
    std::map<std::uint32_t, StreamingToolPreview> streaming_tools_;
    std::map<std::uint32_t, StreamingThinkingPreview> streaming_thinking_;

    // TS REF: Messages.tsx L382-389  isStreamingThinkingVisible useMemo.
    // Returns true when any streaming thinking block is still being streamed,
    // OR when a recently-completed thinking block is within the 30-second
    // grace period (TS: Date.now() - streamingEndedAt < 30000).
    // Drives G3 (hide all completed thinking when streaming visible) and
    // keeps the tail visible after ContentBlockStop fires.
    bool is_streaming_thinking_visible() const {
        auto now = std::chrono::steady_clock::now();
        for (const auto& [idx, stp] : streaming_thinking_) {
            if (!stp.complete) return true;
            if (stp.streaming_ended_at &&
                std::chrono::duration_cast<std::chrono::seconds>(
                    now - *stp.streaming_ended_at).count() < 30)
                return true;
        }
        return false;
    }
    // P0-2 Stage 1: per-turn dedup tracker for ContentBlock index transitions.
    // One per App (one instantiation per repl lifetime; cleared on each turn start.
    cc::ui::messages::pipeline::DedupTracker event_dedup_;
    std::atomic<ScreenInteractive*> screen_{nullptr};

    // Permission confirmation
    std::mutex permission_mutex_;
    std::condition_variable permission_cv_;
    std::optional<bool> permission_response_;
    std::set<std::string> always_allowed_tools_;
    /// Whether any computer-use action has been approved/asked this session —
    /// drives the "first computer use in this session" warning in the panel.
    bool computer_use_seen_in_session_ = false;

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

    // Vim mode — state lives in AppImpl (:impl partition). Accessors keep the
    // VimMode/VimStateMachine types out of this interface.
    bool vim_enabled() const noexcept;
    void set_vim_enabled(bool on);
    // Returns the statusline mode label ("NORMAL"/"INSERT"/…) or nullopt when
    // vim mode is off.
    std::optional<std::string> vim_statusline_label() const;

    // Exit handler (Ctrl-C double-press) — state in AppImpl. Accessors keep
    // ExitHandler/ExitReason types out of this interface.
    void set_exit_message_impl(std::string_view msg);
    void reset_exit_handler();
    bool handle_ctrl_c();

    // AppStore bridge — state in AppImpl.
    bool has_app_store() const noexcept;
    [[nodiscard]] void* app_store_raw() const noexcept;
    BridgeState bridge_state() const;

    // Settings manager — state in AppImpl.
    void init_settings_manager();
    void subscribe_settings_changed(std::function<void()> cb);
    std::optional<std::string> setting_string(std::string_view key) const;
    std::optional<std::string> statusline_setting(std::string_view key) const;
    std::string output_style_setting() const;

    // Settings manager moved into AppImpl (:impl partition).
    std::function<void()> skills_changed_unsubscribe_;  // SkillRegistry dynamic discovery

    // Cost threshold hook — listener ID + shown guard to avoid re-prompting.
    int cost_listener_id_ = -1;
    bool cost_threshold_shown_ = false;

    // Redux-like AppState store moved into AppImpl (:impl partition).
    // Access via has_app_store/app_store_raw/bridge_state.

    // Statusline runner — async execution of user-configurable shell command.
    // Triggered on mount, after messages change, and when settings change.
    // Faithful to TS StatusLine.tsx's debounced doUpdate() pattern.
    std::jthread statusline_thread_;
    std::atomic<bool> statusline_dirty_{false};
    std::atomic<bool> statusline_running_{false};
    std::mutex statusline_mutex_;
    std::condition_variable statusline_cv_;
    int statusline_debounce_ms_ = 300;  // TS: 300ms debounce
    // Memo cache: skip re-exec when command + JSON input are identical and
    // last run was < 30s ago.  Matches TS StatusLine.tsx memo dependency tuple
    // (lastAssistantMessageId, permissionMode, vimMode, mainLoopModel).
    std::string statusline_last_cmd_;
    std::string statusline_last_input_json_;
    std::chrono::steady_clock::time_point statusline_last_run_{};
    // P0-6 builtin statusline: git branch detection cache.  We only re-run
    // `git rev-parse --abbrev-ref HEAD` when the cwd changes (cd events are
    // rare).  This avoids spawning a subprocess on every render tick.
    std::string last_branch_cwd_;
    std::string cached_git_branch_;

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
                // Voice "Voice: processing…" pulse must keep repainting at
                // 50ms (TS useAnimationFrame(50) in ProcessingShimmer) even
                // when no query is running — processing follows mic release
                // and typically overlaps no running query.  Listening is
                // static dim text and Idle renders nothing: neither ticks,
                // preserving the static-idle fast path below.
                // TS REF: VoiceIndicator.tsx:96 useAnimationFrame(
                //   reducedMotion ? null : 50).
                const bool voice_processing =
                    screen_state_ &&
                    screen_state_->voice_enabled &&
                    screen_state_->voice_footer_status ==
                        cc::ui::prompt::FooterVoiceState::Processing;
                // TS VoiceIndicator.tsx:96 useAnimationFrame(reducedMotion ?
                // null : 50): under reduced motion the processing shimmer is
                // static, so do not keep repainting just for voice.
                const bool voice_animates =
                    voice_processing &&
                    !reduced_motion_enabled();
                const bool welcome_active =
                    screen_state_ &&
                    screen_state_->messages.empty() &&
                    screen_state_->spinner_mode == repl::SpinnerMode::Hidden;
                if (!welcome_active) welcome_render_ticks = 0;

                // Re-render only while an animation is actually advancing:
                // an active query (spinner), the voice-processing pulse, or
                // the welcome-intro sweep.  At static idle we skip — no
                // animation to drive.
                if (query_active || voice_animates) {
                    // spinner / voice-pulse animation: keep ticking
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

    // ── Teammate inbox worker ───────────────────────────────────────────────

    // Teammate inbox worker methods used by other shards are declared here;
    // their bodies (and the partition-only helpers) live in the :team
    // partition (app_team.cppm), keeping the TeammateMessage/swarm closure out
    // of this interface BMI.
    [[nodiscard]] bool running_as_pane_teammate() const;
    bool drain_one_teammate_prompt();
    void start_teammate_inbox_worker();
    // Leader-side poller; body in the :team partition.
    void start_leader_inbox_worker();
    void enqueue_teammate_prompt(std::string prompt);
    void poll_teammate_inbox_once(const std::string& agent, const std::string& team);

    void AppendLocalMessagesToScreenState() {
        // Ensure local-command entries have a synthetic 24-char uuids so the
        // UnseenDivider anchor match still lands consistently.  Each local
        // command row is self-contained (not part of any source Message) so
        // each gets its own unique prefix.  A monotonically counter ensures
        // no collisions.
        static std::uint64_t s_local_seq = 0;
        for (auto it = local_command_messages_.begin();
             it != local_command_messages_.end(); ++it) {
            if (it->id.empty()) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "loc_%016llx",
                              (unsigned long long)s_local_seq++);
                it->id = std::string(buf, 24);
            }
        }
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


    // TS REF: src/utils/processUserInput/processBashCommand.tsx
    //
    // Run a user-initiated `!` command LOCALLY (never an LLM turn).  TS does
    // BashTool.call({command, dangerouslyDisableSandbox:true}) with
    // shouldQuery:false, renders a <bash-input> user row plus a <bash-stdout>/
    // <bash-stderr> output row, and NEVER sends the command to the model.
    //
    // We mirror that: append the input row immediately (like TS's initial
    // setToolJSX(<BashModeProgress>)), then run `/bin/sh -c` on a worker thread
    // (combined stdout+stderr via popen_spawn, run in the session cwd) and post
    // the output back to the render thread via pending_bash_result_.  The
    // engine / query path is never touched, so no Bash *tool-use* card and no
    // assistant summary are produced — matching the TS transcript exactly.
    void RunLocalBashCommand(std::string command);

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

    void RefreshAutocompleteSuggestions();


    // Agents menu methods are defined in app_agent_menu.cpp so the
    // agent_cards closure stays out of this interface BMI.
    void RefreshAgentsMenuOutput();
    void LoadAgentCardsForMenu();
    void OpenAgentsMenu();
    void OpenTeamsOverview();
    bool HandleLocalJsxEvent(const Event& ev);

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
        if (skill.source == "user") return "User skills (~/.loom/skills)";
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
            out += "Create skills under `.loom/skills` or `~/.loom/skills`.\n";
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
        const auto& skills = cached_skills_;
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
    ~AppAdapter() override;

    AppAdapter(void* engine, void* lifecycle_hooks,
               void* cmd_registry, void* storage,
               std::function<void()> on_exit);

    void HandleSubmit(const std::string& text,
                      repl::InputMode submit_mode = repl::InputMode::Normal);

    void HandleCommand(std::string_view cmd);

    void ProjectRuntimeMetadataToScreenState();

    /// Project settings from SettingsManager into screen_state_.
    /// Mirrors how the TS engine projects AppState.settings into the REPL
    /// screen's model/status-line fields.  Only the subset needed by the
    /// renderer is projected — the engine owns the full settings object.
    void ProjectSettingsToScreenState();

    /// Trigger an async statusline update (debounced).
    /// Faithful to TS scheduleUpdate() — sets a dirty flag and wakes the
    /// worker thread; the actual command runs after the debounce period.
    void TriggerStatuslineUpdate() {
        if (screen_state_->status_line_command.empty()) return;
        statusline_dirty_.store(true);
        statusline_cv_.notify_one();
    }

    // Build the statusline JSON payload / execute the user command.
    // Out-of-line in app_constructor.cpp so cc.utils.statusline_runner,
    // cc.utils.model and cc.constants stay out of this interface's BMI.
    [[nodiscard]] std::string BuildStatuslineInputJson();
    bool ExecuteStatuslineCommand(std::string_view command,
                                  std::string json_input,
                                  int timeout_ms,
                                  std::string& output);

    // TS REF: src/components/Messages.tsx L519-520 — the render `useMemo`
    // applies a chain of collapse passes to the message list before projecting
    // rows:
    //   collapseBackgroundBashNotifications(collapseHookSummaries(
    //     collapseTeammateShutdowns(collapseReadSearchGroups(grouped, tools))))
    //
    // We run the same chain here, on the raw conversation, before the
    // per-message projection loop in SyncState()/Render().  Only the passes
    // that have a faithful CPP port are wired so far:
    //   * collapseBackgroundBashNotifications — DONE (this call).
    //   * collapseHookSummaries / collapseTeammateShutdowns / collapseReadSearch
    //     — pending (need richer SystemMessage / AttachmentMessage types).
    // As each pass lands it slots in here, preserving the TS ordering.
    //
    // `fullscreen=true`: the CPP transcript is always the fullscreen-equivalent
    // view (TS gates collapse on isFullscreenEnvEnabled()).  `verbose=false`:
    // there is no ctrl+O verbose transcript toggle at this layer yet, so we use
    // the default collapsed presentation (TS shows each item only in verbose).
    [[nodiscard]] std::vector<Message> ApplyMessageCollapsePipeline(
        std::vector<Message> messages) const;

    void SyncState();

    // Rebuild screen_state_->live_teammates from the native agent store +
    // pane-observer snapshot. Defined in app_team_projection.cpp; callers own
    // the render wake.
    void ProjectLiveTeammatesToScreenState();

    // Leader-side: drain one queued teammate permission_request into the
    // existing ToolPermission dialog (Band3 overlay). Defined in
    // app_team_projection.cpp. Returns true when a dialog was pushed.
    bool drain_one_teammate_permission();

    void ConsumePendingResult();

    Element Render() override;

    bool OnEvent(Event event) override;

    Component ActiveChild() override;

    void set_screen(ScreenInteractive* screen) {
        screen_.store(screen, std::memory_order_release);
    }

    // ── Async clipboard paste worker ──────────────────────────────────────
    // Spawns a detached thread that reads the clipboard image.  On success
    // the ImageBlock is posted to pending_paste_results_; on failure the id
    // is posted to pending_paste_failures_.  ProcessCompletedPastes() drains
    // both queues on the render/event thread.
    //
    // Why async?  std::system() + osascript fork + PNG-to-file + base64
    // encode takes 100-500ms on macOS.  Doing that synchronously in OnEvent
    // blocks the FTXUI render loop, causing visible UI freeze and (worse)
    // terminal raw-mode state corruption that can take seconds to recover
    // from.  The placeholder "[Image #N]" is inserted synchronously so the
    // user gets instant feedback; the image data fills in shortly after.
    void SpawnPasteWorker(int id);

    /// Drain background paste results onto pasted_contents_ (render thread).
    /// Called at the top of every OnEvent so results are picked up as soon as
    /// possible without blocking.  Failed pastes have their "[Image #N]"
    /// placeholder removed from input_text.  Text pastes replace "[Image #N]"
    /// with the actual text (truncating if >10K chars).
    void ProcessCompletedPastes();

    /// Block (main thread, brief) until every [Image #N] referenced in `text`
    /// that still has an in-flight paste worker has either landed in
    /// pasted_contents_ / pending_paste_results_ / pending_paste_failures_.
    /// This closes the Ctrl+V→Enter race where a fast submit would snapshot
    /// pasted_contents_ before the PNG data arrived.
    ///
    /// Bounded wait (default ~3s) so a stuck/leaked worker never wedges the UI.
    /// Drains completed results on each tick so pasted_contents_ is fresh when
    /// HandleSubmit reads it immediately after this returns.
    void WaitForInFlightPastes(const std::string& text);

    // ── Teammate inbox test seams (bodies in the :team partition) ─────────
    void configure_teammate_for_testing(std::string agent_name, std::string team);
    void poll_teammate_inbox_once_for_testing();
    [[nodiscard]] std::size_t teammate_pending_count_for_testing();
    [[nodiscard]] std::string pop_teammate_prompt_for_testing();

    [[nodiscard]] std::function<bool(std::string_view, std::string_view)> get_permission_callback();

    [[nodiscard]] bool is_query_running_for_testing() const noexcept {        return query_running_.load();
    }

    // Drive a prompt submission through the full HandleSubmit path (slash /
    // bash / LLM routing) exactly as the Enter key would.
    void submit_for_testing(const std::string& text) {
        this->HandleSubmit(text);
    }

    // True while a local '!' bash command worker is still running.
    [[nodiscard]] bool is_local_bash_running_for_testing() const noexcept {
        return bash_running_.load();
    }

    // Block until the local '!' bash worker finishes, then drain its output
    // into the transcript (mirrors what the render loop does each frame).
    void wait_for_local_bash_for_testing() {
        if (bash_thread_.joinable()) bash_thread_.join();
        this->ConsumePendingResult();
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

    /// Number of entries in pasted_contents_ (for testing orphan cleanup).
    [[nodiscard]] std::size_t pasted_contents_size_for_testing() const noexcept {
        return pasted_contents_.size();
    }

    /// Check if a specific paste-id is still in pasted_contents_ (for testing
    /// orphan cleanup after placeholder deletion).
    [[nodiscard]] bool has_pasted_content_for_testing(int id) const noexcept {
        return pasted_contents_.contains(id);
    }

    /// Inject a pasted image directly (bypasses clipboard read — for testing
    /// HandleSubmit's referenced-ids filter and empty-text+images guard).
    void inject_pasted_image_for_testing(int id, ImageBlock ib) {
        pasted_contents_[id] = std::move(ib);
    }

    /// When true, SpawnPasteWorker injects a tiny fake PNG synchronously into
    /// pending_paste_results_ instead of spawning a detached thread that reads
    /// the real clipboard. Lets tests exercise the Ctrl+V → placeholder →
    /// submit path without the lifetime hazard of a detached thread outliving
    /// the test's AppAdapter.
    bool no_real_paste_worker_for_testing_ = false;
    void set_no_real_paste_worker_for_testing(bool v) {
        no_real_paste_worker_for_testing_ = v;
    }

    /// Set input_text directly (for testing orphan cleanup and submit guards
    /// without going through the text input component).
    void set_input_text_for_testing(std::string text) {
        screen_state_->input_text = std::move(text);
        screen_state_->input_cursor = screen_state_->input_text.size();
    }

    /// Expose HandleSubmit for direct test invocation (the real submit path
    /// goes through the text input component's on_submit callback).
    void handle_submit_for_testing(std::string text) {
        this->HandleSubmit(text);
    }

    /// Run the orphan-cleanup logic (TS PromptInput.tsx L1185-1200 useEffect)
    /// against the current screen_state_->input_text.  For testing only.
    void trigger_orphan_cleanup_for_testing();

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

    // Out-of-line in the :team partition so the live_teammates /
    // dialogs.system closures stay out of this interface.
    void set_live_teammates_for_testing(void* v);
    [[nodiscard]] bool teams_overview_open_for_testing() const;

    [[nodiscard]] int teams_overview_count_for_testing() const {
        return static_cast<int>(screen_state_->live_teammates.size());
    }

    // Enqueue a stage-A permission_request as if the leader inbox poll found
    // it (exercises the ToolPermission dialog + PermissionSync reply path
    // without a real tmux worker mailbox).
    void enqueue_teammate_permission_for_testing(void* request, std::string team);
    [[nodiscard]] std::size_t pending_teammate_permission_count_for_testing();
};

// ============================================================
// Main Application Runner
// ============================================================

} // namespace cc::ui
