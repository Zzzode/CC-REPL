// repl_state.cppm — plain-data REPL screen state.
//
// Split from repl_screen.cppm so importers that only need the state struct
// (cc.ui.app.app and its impl shards) do not pull the full rendering closure
// (message/dialog/widget renderers — over a hundred modules) into their BMI.
// repl_screen.cppm re-exports this module, so every existing importer keeps
// seeing these names unchanged.
module;

#include <cstdint>

#include <ftxui/screen/color.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.screens.repl_state;

import std;

import cc.types.types;
import cc.ui.foundation.ui_types;                 // cc::ui::common::PromptInputMode
import cc.ui.dialogs.system;                     // DialogQueue / payloads
import cc.ui.chrome.fullscreen_layout;           // StickyPrompt
import cc.ui.prompt.prompt_input_footer;         // footer::* projection types
import cc.ui.prompt.voice_indicator;             // FooterVoiceState
import cc.ui.messages.messages_list;             // UnseenDivider
import cc.ui.messages.virtual_list;              // JumpHandle / VirtualListState
import cc.ui.features.agents.agent_cards;        // AgentCardData
import cc.ui.features.agents.agent_wizard;       // WizardDraft (callback sig)
import cc.ui.features.teams.live_teammates;      // LiveTeammate
import cc.ui.visual.markdown;                    // StreamingMarkdown (ptr field)

export namespace cc::ui::repl_screen {
using namespace ftxui;

// M7: pull the dialog framework types up into convenient aliases.
namespace dsys_fw   = cc::ui::dialogs::system;
namespace cards     = cc::ui::agents::cards;
using DialogQueue            = dsys_fw::DialogQueue;
using DialogRendererRegistry = dsys_fw::DialogRendererRegistry;
using DialogRenderContext    = dsys_fw::DialogRenderContext;
using DialogPayloadVariant   = dsys_fw::DialogPayloadVariant;

// =========================================================
// Enums
// =========================================================

/// Top-level mode.  1:1 with TS focusedInputDialog union (REPL.tsx:2017)
/// plus contextual panel modes.  TS names in trailing comments.
enum class ReplMode : std::uint8_t {
    Normal,                   // (none) — base layout
    // Dialogs (overlay via dbox)
    MessageSelector,          // 'message-selector'
    SandboxPermission,        // 'sandbox-permission'
    ToolPermission,           // 'tool-permission'
    PromptHook,               // 'prompt' (was: HookPrompt)
    WorkerSandboxPermission,  // 'worker-sandbox-permission'
    Elicitation,              // 'elicitation'
    CostThreshold,            // 'cost'
    IdleReturn,               // 'idle-return'
    UltraplanChoice,          // 'ultraplan-choice'  (ULTRAPLAN)
    UltraplanLaunch,          // 'ultraplan-launch'  (ULTRAPLAN)
    IdeOnboarding,            // 'ide-onboarding'
    InitOnboarding,           // 'init-onboarding'
    ModelSwitch,              // 'model-switch'        (ant-only)
    UndercoverCallout,        // 'undercover-callout'  (ant-only)
    EffortCallout,            // 'effort-callout'
    RemoteCallout,            // 'remote-callout'
    DesktopUpsell,            // 'desktop-upsell'
    LspRecommendation,        // 'lsp-recommendation'
    PluginHint,               // 'plugin-hint'
    // Panels (rendered inline — no overlay)
    TasksView, TeamsView, AgentsView, SettingsView, HelpView, AboutView, QuickOpen,
    // UI13 — agent wizard (create/edit).
    CreateAgent,        // 4-step new-agent wizard
    EditAgent,          // 4-step edit-agent wizard
    // UI8  — trust dialog (standalone takeover; 4-tier risk UX).
    TrustDialog,        // 'trust-dialog'
};

/// Input modes — unified canonical enum from cc::ui::common.
/// Previously this file defined a local 10-value InputMode:
///   {Prompt, Bash, SlashCommand, HistorySearch, PlanMode,
///    VimInsert, VimNormal, VimVisual, OrphanedPermission, TaskNotification}
/// Old→new mapping:
///   InputMode::Prompt → PromptInputMode::Normal  (TS: 'prompt')
/// All other values retain their names in the unified enum.
/// TS REF: src/types/textInputTypes.ts:265 (PromptInputMode type)
using InputMode = cc::ui::common::PromptInputMode;

/// Spinner modes.  TS SpinnerMode (SpinnerAnimationRow.tsx switch cases +
/// SpinnerWithVerb usage in REPL.tsx): requesting/thinking/responding/
/// tool-input/tool-use + briefing/idle for KAIROS brief mode.
enum class SpinnerMode : std::uint8_t {
    Hidden, Requesting, Thinking, Responding,
    ToolInput, ToolUse, Briefing, IdleBrief,
};

// =========================================================
// Data structures (lean projections — engine owns full state)
// =========================================================

/// Status bar projection.  Mirrors TS REPL top status line.
struct StatusBarData {
    std::string model_name;
    std::optional<std::string> session_name, agent_name;
    std::optional<double> cost_usd;
    int input_tokens = 0, output_tokens = 0, context_token_count = 0;
    bool is_fast_mode = false, is_auto_mode = false, is_brief_mode = false;
    std::optional<std::string> effort_level;
    int swarm_session_count = 0;
    bool bridge_connected = false;
    std::optional<std::string> current_path;
};

/// Minimal Message projection for orchestration.
/// Per-row rendering delegated to message_row.cppm (UI4/UI5).
struct MessageDisplayEntry {
    std::string id, role, content_preview;
    bool is_streaming = false, is_thinking = false, is_tool_use = false;
    /// True when the thinking block is still being streamed (or the entry
    /// is a static projection that should render the collapsed "Thinking"
    /// label instead of being hidden entirely by the Complete-state fast
    /// path in the faithful renderer).  Messages-list uses this to decide
    /// whether the row contributes vertical space when unselected.
    bool thinking_active = false;
    bool is_local_command_input = false;
    bool is_local_command_output = false;
    bool is_local_jsx_output = false;
    bool is_compact_boundary = false, is_error = false;
    /// True when the entry corresponds to a user-attached ImageBlock and
    /// should be routed through MessageShape::UserImage (instead of
    /// UserText).  Populated by project_messages when splitting a single
    /// UserMessage with mixed text+image content into multiple sibling
    /// display rows (TS parity: each <UserImageMessage/> is its own row).
    bool is_image = false;
    std::optional<::cc::core::ImageBlock> image_block;
    /// Display ID for user-attached images (shown as "[Image #N]").
    /// Populated by project_messages when splitting a UserMessage with
    /// image content blocks into individual display rows (TS parity:
    /// Message.tsx assigns imageIds from message.imagePasteIds).
    std::optional<int> image_display_id;
    std::optional<std::string> tool_name, tool_status;
    /// Parsed tool input JSON for tool-use entries.  Threaded into
    /// ToolUseRenderOptions.raw_parameters (shared G1/G2 contract) instead of
    /// the previous content_preview fallback.  G2 (app.cppm) populates this
    /// from ToolUseBlock.input_json / ToolUseMessage.tool_input_json.
    std::optional<std::string> tool_input_json;
    /// M6 result_preview: live streaming result preview for tool-use entries.
    /// Populated by App::UpdateScreen from streaming tool state, consumed by
    /// BuildMessagesList to drive ToolUIRegistry.progress() and the
    /// result-preview block in faithful tool-use renderers.
    std::optional<std::string> tool_result_preview;
    /// TS PARITY (2026-07-04): structured content items from tool results
    /// (e.g. MCP tools returning mixed text+image).  When present, the
    /// faithful tool-result renderer iterates these instead of the
    /// flattened content_preview string.
    std::optional<std::vector<::cc::core::ToolResultContentItem>> tool_result_content_items;
    std::optional<std::string> agent_display_name, agent_color_name;
    std::chrono::system_clock::time_point timestamp;
    int estimated_height_lines = 3;
    /// M4 live-path: system-row subtype hint.  When set on a `system` entry,
    /// RenderMessages routes the row through the matching faithful
    /// RenderSystemTextMessageFaithful branch (away_summary / teardrop event /
    /// generic dot).  When unset the projection falls back to a heuristic
    /// derived from content_preview (see RenderMessages).  The engine / G2
    /// app.cppm may populate this from the upstream SystemMessage tag later.
    std::optional<std::string> system_subtype;

    // ── P2 gap api-error-retry: retry metadata for SystemAPIError cards ──
    /// TS REF: SystemAPIErrorMessage.tsx — retryInMs.  Backoff duration in
    /// milliseconds before the next auto-retry.  Used for the live countdown.
    std::optional<double> retry_after_ms;
    /// TS REF: SystemAPIErrorMessage.tsx — retryAttempt.  Current attempt
    /// number (1-based, shown as "attempt N/M").
    std::optional<int> retry_attempt;
    /// TS REF: SystemAPIErrorMessage.tsx — maxRetries.  Total allowed attempts.
    std::optional<int> max_retries;
    /// TS REF: SystemAPIErrorMessage.tsx — sessionExpired.  When true, the
    /// auth session has expired; show "Clear session" button instead of Retry.
    bool session_expired{false};
};

/// Tool kind for permission prompt dispatch.
/// Determines which faithful permission panel renderer to use.
enum class PermissionToolKind : std::uint8_t {
    Generic,        ///< Fallback — old simple dialog
    Bash,           ///< Bash / shell command
    FileEdit,       ///< File edit (with diff)
    FileWrite,      ///< File write / create
    FileRead,       ///< File read
    Glob,           ///< Glob search
    Grep,           ///< Grep search
    WebFetch,       ///< Web fetch
    WebSearch,      ///< Web search
    Skill,          ///< Skill execution
    PlanMode,       ///< Enter/exit plan mode
    MCP,            ///< MCP tool
    LSP,            ///< LSP tool
    Agent,          ///< Agent spawn
    NotebookEdit,   ///< Notebook cell edit
    _COUNT,
};

/// Permission prompt subset (TS ToolUseConfirm).  Full shape: UI8/UI9.
struct PermissionRequestInfo {
    std::string tool_name, description;
    std::optional<std::string> file_path;
    std::vector<std::string> risk_labels;
    bool can_always_allow = true;

/// Transient dialog payloads for RouteDialog dispatch.
struct DialogContext {
    std::optional<std::string> sandbox_host_pattern, sandbox_worker_request_id;
    std::optional<std::string> elicitation_server_name;
    std::optional<std::uint64_t> elicitation_request_id;
    std::optional<double> cost_threshold_usd;
    std::optional<std::uint32_t> idle_return_minutes;
    std::optional<std::uint64_t> idle_return_total_tokens;
    int idle_return_selected_index{0};
    std::optional<std::string> ide_installation_status, model_switch_alias;
    std::optional<std::string> plugin_hint_name, plugin_hint_description;
    std::optional<std::string> lsp_rec_extension, lsp_rec_file_ext;
    std::optional<std::string> ultraplan_blurb;
    // UI13: agent wizard — name of agent being edited (empty = create new).
    std::optional<std::string> agent_wizard_agent_id;
    // UI8: TrustDialog payload (workspace path + user-facing decision).
    std::optional<std::string> trust_workspace_path;
};

    // -- Tool-specific payload (populated only for matching tool_kind) --

    // Bash
    std::optional<std::string> bash_command;
    std::optional<std::string> bash_working_dir;
    bool bash_is_destructive = false;
    std::string bash_destructive_reason;

    // FileEdit / FileWrite / FileRead (shared path fields)
    std::optional<std::string> file_old_content;    // edit: old_string / write: old_content
    std::optional<std::string> file_new_content;    // edit: new_string / write: content
    bool file_replace_all = false;                  // edit only
    bool file_exists = false;                       // write only (overwrite vs create)
    std::optional<std::string> file_language;       // syntax highlight hint
    std::optional<std::string> file_relative_path;
    std::optional<std::string> file_filename;       // basename

    // Web
    std::optional<std::string> web_url;

    // Skill
    std::optional<std::string> skill_name;
    std::optional<std::string> skill_source;  // "bundled" / "user" / "plugin"
};/// Screen projection of the TS voice context.
/// TS REF: src/context/voice.tsx — voiceState is 'idle' | 'recording' |
/// 'processing'.  The TS 'recording' value maps to FooterVoiceState::
/// Listening (matching cc::context::VoiceState::Listening).  The future
/// voice service (cc::hooks::voice) is the ONLY writer, via the
/// ProjectVoiceFooterStatus seam below; the renderer is read-only.
/// voiceError is a separate TS Notifications path and is not represented
/// here.
///
/// Lean orchestration state.  Full app state lives in services/; the
/// engine writes computed projections into this struct between frames.
struct ReplScreenState {
    ReplMode mode = ReplMode::Normal;
    InputMode input_mode = InputMode::Normal;
    SpinnerMode spinner_mode = SpinnerMode::Hidden;
    // Messages
    std::vector<MessageDisplayEntry> messages;
    bool active_local_jsx_command = false;
    std::string active_local_jsx_command_name;
    std::string active_local_jsx_command_args;
    std::string active_local_jsx_content;
    int active_agents_selection_position = 0;
    int scroll_offset = 0, selected_message_idx = -1;
    int viewport_height_lines = 40;
    bool scroll_pinned_to_bottom = true;
    /// Index into messages[] where the unseen divider anchor sits.
    /// Set on FIRST scroll-away from bottom (TS REF: useUnseenDivider
    /// dividerIndex).  nullopt = pinned to bottom (no divider).  Cleared
    /// on repin (scroll-to-bottom, submit, or /clear).
    std::optional<std::size_t> divider_index;
    /// Snapshot of messages.size() at the time of first scroll-away.
    /// Used to detect when new messages arrive past the divider.
    std::size_t message_count_at_scroll_away = 0;
    // P0-3 VirtualMessageList — the imperative handle exposed by the windowed
    // renderer when the current visible count crosses kVirtualThreshold.
    // `virtual_list_active` is set per-frame by RenderMessages; when true,
    // ScrollTranscript uses JumpToVisualLine() which runs O(log N) binary
    // search against the JumpHandle instead of the crude EstimateTranscriptRows.
    bool virtual_list_active = false;
    cc::ui::messages::virtual_list::JumpHandle virtual_jh;
    std::shared_ptr<cc::ui::messages::virtual_list::VirtualListState>
        virtual_list_state;
    // M1 (FullscreenLayout slot-system chrome): scroll-derived chrome state.
    //   sticky_prompt — std::nullopt = at bottom (TS null).  Value present =
    //     scrolled up; the inner .text is shown as a breadcrumb header and
    //     .scroll_target_row is the absolute transcript row to jump to on
    //     click (maps to TS `stickyPrompt = {text, scrollTo}`).
    //   sticky_prompt_clicked — set to true by the header's click handler
    //     BEFORE the actual scroll happens; corresponds to the TS literal
    //     sentinel stickyPrompt === 'clicked'.  It hides the header this
    //     frame so the prompt line rises to absolute row 0 of the scroll
    //     viewport (the padCollapsed mechanic already strips paddingTop=1
    //     whenever sticky_prompt is set, so the only delta is hiding the
    //     header row).  Reset to false by any subsequent scroll event that
    //     writes a fresh sticky_prompt (StickyTracker emits every
    //     viewport-top change while unpinned).
    //   unseen_message_count — count of assistant turns added below the fold
    //     while unpinned; drives the "N new messages" pill label (TS
    //     `newMessageCount` + `useUnseenDivider`).  The pill itself only
    //     renders when pill_visible is true (engine snapshots the divider).
    //   pill_visible — TS `pillVisible` (useSyncExternalStore against the
    //     scroll handle).  Defaults false so chrome stays dormant until the
    //     engine wires real scroll-observe state.
    // TS REF: FullscreenLayout.tsx lines 293 (useState) + 339-351 (3-state
    //        discriminant + padCollapsed logic).
    std::optional<::cc::ui::layout::fullscreen::StickyPrompt> sticky_prompt;
    bool sticky_prompt_clicked = false;
    int unseen_message_count = 0;
    bool pill_visible = false;
    // TS REF: Messages.tsx L240 (unseenDivider prop) +
    //        FullscreenLayout.tsx L224-256 (UnseenDivider + computeUnseenDivider).
    // Populated by App::UpdateScreen from scroll state + messages[].  The
    // engine sets `first_unseen_uuid_prefix` to the 24-char prefix (or full
    // uuid) of messages[dividerIndex] after skipping progress + null-rendering
    // attachments (CC-724).  `count` is Math.max(1, countUnseenAssistantTurns(…)).
    // nullopt = no divider (pinned to bottom, dividerIndex=null, empty session).
    std::optional<::cc::ui::messages_list::UnseenDivider> unseen_divider;

    // TS REF: Messages.tsx expandedKeys (L563) — Set of expand keys the user
    // has toggled open via Enter/Space on clickable rows.  Tool results and
    // thinking blocks render with verbose=true when their expand key is in
    // this set, showing full content instead of truncated summary.
    std::unordered_set<std::string> expanded_keys;

    /// TS REF: Messages.tsx isBriefOnly prop (L236, L510-514).
    /// When true, only brief-tool calls + their results + real user input
    /// are shown; assistant text, thinking, and non-brief tools are hidden.
    /// Toggled by the user (e.g., via /brief command or status bar click).
    bool is_brief_mode = false;

    /// TS REF: Messages.tsx L382-389 + L395-419  isStreamingThinkingVisible.
    /// When true, ALL completed thinking blocks are hidden (TS:
    /// lastThinkingBlockId = 'streaming').  Set by app.cppm when any streaming
    /// thinking entry is active or within its 30s grace period.
    bool streaming_thinking_globally_visible = false;

    /// TS REF: Messages.tsx isTranscriptMode (L459, screen === 'transcript').
    /// When true, the message list shows the FULL transcript (all message
    /// types visible, bypassing brief/dropText filters).  Capped at last 30
    /// messages unless show_all_in_transcript is also true.
    /// Toggled by Ctrl+O (TS: app:toggleTranscript global shortcut).
    bool is_transcript_mode = false;

    /// TS REF: Messages.tsx showAllInTranscript prop (L223, L467, L515-516).
    /// When true AND is_transcript_mode is true, the 30-message cap is lifted
    /// and ALL messages are rendered.  Toggled by Ctrl+E while in transcript
    /// mode (TS: transcript:toggleShowAll shortcut, Transcript context).
    bool show_all_in_transcript = false;

    // Input
    std::string input_text;
    // GAP 2: stashed-prompt-restore-logic-missing
    // TS REF: src/screens/REPL.tsx L1373-1377 — stashedPrompt state:
    //   {text, cursorOffset, pastedContents}.  When the user sends a message
    //   while a background agent is running (or when permission interrupts),
    //   the current input is stashed and can be restored after the request
    //   completes.  Restore at:
    //     - TS L3251-3255 (after local-jsx result returns)
    //     - TS L3344-3348 (on submit when not slash-command)
    //     - TS L3527-3531 (after handlePromptSubmit for slash/loading)
    //   The stash notice (PromptInputStashNotice.tsx) renders
    //   "{figures.pointerSmall} Stashed (auto-restores after submit)" when
    //   hasStash is true.
    // TS REF: src/screens/REPL.tsx L1373-1377 — stashedPrompt state:
    //   {text, cursorOffset, pastedContents}.  pastedContents carries the
    //   image/text paste records so that [Image #N] / [...Truncated text #N]
    //   refs in the stashed text resolve correctly after restore.
    struct StashedPrompt {
        std::string text;
        std::size_t cursor_offset = std::string::npos;
        // TS REF: pastedContents: Record<number, PastedContent> — image pastes.
        std::unordered_map<int, ::cc::core::ImageBlock> pasted_images;
        // TS REF: pastedContents also holds text-type entries for truncated
        // text pastes (inputPaste.ts maybeTruncateInput → type:'text').
        std::unordered_map<int, std::string> pasted_texts;
    };
    std::optional<StashedPrompt> stashed_prompt;
    // TS-style contextual placeholder rather than a generic default.
    // NOTE: This is the FALLBACK only.  The actual displayed placeholder is
    // computed dynamically by ComputePlaceholder() at render time from the
    // cascade (teammate hint > queue hint > onboarding example > AI suggestion
    // override).  This string is used only when all cascade conditions fail
    // AND the caller explicitly wants a static default (e.g. standalone
    // TextInputImpl usage outside the REPL).
    std::string input_placeholder = "Try \"write a test\", \"/help\", or ask anything...";
    // ── TS usePromptInputPlaceholder cascade state ──────────────────────
    // Viewing agent/teammate name.  When set and input is empty, the
    // placeholder becomes "Message @{name}..." (TS REF: usePromptInputPlaceholder.ts
    // viewingAgentName branch).  Truncated to 20 chars in ComputePlaceholder().
    std::optional<std::string> viewing_agent_name;
    // Number of user submissions (messages sent).  Drives the onboarding
    // example placeholder: shown only when submit_count < 1 (TS REF:
    // usePromptInputPlaceholder.ts submitCount < 1 guard).
    int submit_count = 0;
    // How many times the "Press up to edit queued messages" hint has been
    // shown.  Capped at 3 (NUM_TIMES_QUEUE_HINT_SHOWN in TS).  Incremented
    // by the renderer each time the hint is displayed.
    int queued_command_hint_shown_count = 0;
    // Whether prompt suggestions (AI next-action hints) are enabled.
    // Maps to TS AppState.promptSuggestionEnabled.
    bool prompt_suggestion_enabled = true;
    // True when the command queue holds user-editable pending commands
    // (TS REF: isQueuedCommandEditable check).  Populated by the engine
    // from the command queue state.
    bool has_editable_queued_commands = false;
    // TS AGENT_COLOR_TO_THEME_COLOR teammate prefix color.  Empty = use
    // palette.text (the default prompt prefix color).  Populated from the
    // engine's active-teammate lookup.  Rendered as the prefix glyph's
    // foreground in RenderPromptInput (replaces the old plain white).
    std::optional<ftxui::Color> teammate_prefix_color;
    // Welcome-header data (shown when messages is empty on a fresh session).
    std::string app_version = "0.0.0";
    std::string model_display_name;
    // TS LogoV2/CondensedLogo row 2 separator billing_type token (e.g.
    // "API Usage Billing" / "Team Seat" / "Rate Limited").  Empty = row 2
    // shows only the model name without " · <billing>" suffix.
    std::string billing_type;
    std::string cwd;
    // P0-6 builtin statusline: detected git branch for cwd (empty = not a git
    // repo or detection failed).  Populated by AppAdapter from
    // cc::utils::git::get_branch(), cached per-cwd-change to avoid spawning
    // `git` on every render tick.
    std::string git_branch;
    // M2: oauthAccount.displayName analogue — drives formatWelcomeMessage
    // ("Welcome back {user}!" vs "Welcome back!").  Empty for new users.
    std::string user_display_name;
    // P0 Gap logov2-render-modes-missing: pre-rendered feed content for the
    // full-logo horizontal mode's right column. Populated by the engine layer
    // from session storage (recent activity) and changelog parser (what's
    // new). Empty vectors fall back to placeholders defined in RenderWelcomeHeader.
    std::vector<std::string> recent_activity_lines;
    std::vector<std::string> changelog_lines;
    // TS REF: LogoV2.tsx L56 shouldShowProjectOnboarding() — when true, the
    // horizontal-layout feed column shows [ProjectOnboarding, RecentActivity]
    // instead of [RecentActivity, What'sNew].  Also prevents condensed mode
    // (TS isCondensedMode = !hasReleaseNotes && !showOnboarding && !forceFull).
    bool show_onboarding = false;
    // TS REF: LogoV2.tsx L70 useShowGuestPassesUpsell() — when true and no
    // onboarding, feed shows [RecentActivity, GuestPasses] instead of default.
    bool show_guest_passes_upsell = false;
    // TS REF: LogoV2.tsx L71 useShowOverageCreditUpsell() — when true and no
    // onboarding/guest-passes, feed shows [RecentActivity, OverageCredit].
    bool show_overage_credit_upsell = false;

    // ── P1 Footer notifications ──────────────────────────────────────
    // TS REF: src/components/PromptInput/Notifications.tsx
    // These fields drive the right-column notification area.
    // RenderNotifications() picks the highest-priority active item.
    cc::ui::prompt::footer::ApiKeyStatus api_key_status =
        cc::ui::prompt::footer::ApiKeyStatus::Unknown;
    bool is_remote_session = false;   // LOOM_REMOTE → changes error text
    bool debug_mode = false;          // "Debug mode" pill
    bool verbose = false;             // show token count when valid + verbose
    // IDE selection indicator (TS IdeStatusIndicator.tsx)
    bool ide_connected = false;
    std::optional<std::string> ide_file_path;
    std::optional<int> ide_selected_lines;
    // Dynamic notification (env-hook, external-editor hint, etc.)
    std::optional<std::string> footer_dynamic_text;
    std::optional<std::string> footer_dynamic_color;  // "error" / "warning" / empty=dim
    // P1: Notification queue — rotating carousel of up to 12 status notices.
    // TS REF: src/context/notifications.tsx (useNotifications hook)
    // Items are added via hooks (env-hook, rate-limit warnings, etc.) and
    // rotate through the footer's notification slot on a timeout basis.
    cc::ui::prompt::footer::NotificationQueue footer_notification_queue;
    // Stable per-session welcome-tip index (seeded once from the session id in
    // app.cppm). The renderer mods this by kWelcomeTips.size(). Previously the
    // tip used spinner_frame, which cycled the tip on every mouse-move re-render.
    std::size_t welcome_tip_index{0};
    struct AutocompleteSuggestion {
        std::string display_text;
        std::string description;
        std::string insert_text;
        std::size_t replacement_start = std::string::npos;
        std::size_t replacement_end = std::string::npos;
        bool submit_on_return = false;
        /// Optional icon prefix (e.g. "📄" for files, "📁" for dirs).
        /// TS REF: src/components/PromptInput/PromptInputFooterSuggestions.tsx:24
        ///          (getIcon — + for files, ◇ for MCP, * for agents).
        std::string icon{};
        // INF-02: stable identity for selection preservation across refreshes
        // (TS getPreservedSelection-by-id, src/hooks/useTypeahead.tsx:52-74).
        // Defaults to display_text when a caller doesn't supply a richer id,
        // so same-display items from different sources can be distinguished.
        // `{}` in-class init so existing partial designated initializers (e.g.
        // in tests/test_ui.cpp) don't trip -Wmissing-designated-field-initializers.
        std::string id{};
        /// Optional color name for a colored dot prefix (e.g. "red", "blue").
        /// TS REF: src/hooks/unifiedSuggestions.ts:77-108 — agent defs include
        ///          a color field used to tint the avatar dot in the picker.
        std::string color_name{};
    };
    std::vector<AutocompleteSuggestion> autocomplete_suggestions;
    // INF-05: input text at which the user dismissed the popup with Esc.
    // RefreshAutocompleteSuggestions stays dismissed until the input changes
    // (TS dismissedForInputRef, src/hooks/useTypeahead.tsx:893-908).
    std::string dismissed_autocomplete_for_input;
    // INF-03: precomputed stable name-column width for slash suggestions (max
    // over ALL candidates) so the description column doesn't jitter while
    // filtering. 0 = derive dynamically per visible row (@, #, ...).
    // Mirrors TS maxColumnWidth (src/hooks/useTypeahead.tsx:380-386).
    int autocomplete_stable_name_width = 0;
    // SL-03: pending inline argument hint for the slash command currently being
    // typed (e.g. "set <key> <value>"). Populated by RefreshAutocompleteSuggestions
    // when input matches "/cmd "; rendered by TextInputImpl.
    std::string pending_argument_hint;
    // SL-05: pending ghost-text completion suffix for a mid-input "/prefix"
    // command token (e.g. input "text /com" → ghost "mit"). Rendered inline
    // by TextInputImpl inline_ghost_text.
    std::string pending_ghost_text;
    // SL-11: deterministic next-action suggestion produced by
    // PromptSuggestionService on QueryEnd (no LLM). When set and the prompt
    // input is empty, RefreshAutocompleteSuggestions surfaces it as the lone
    // AutocompleteSuggestion; any non-empty input clears it. Acts as the
    // shared hand-off between the engine-side QueryEndHook (writer) and the
    // renderer (reader). Cleared on accept / first keystroke.
    std::optional<std::string> next_action_suggestion;
    int autocomplete_index = -1;
    std::size_t input_cursor = std::string::npos;
    std::deque<std::string> input_history;
    std::size_t history_index = std::string::npos;
    // AT-09: inbound IDE at_mentioned notifications deliver a fully-formed
    // "@<relpath>#L<a>-<b>" token to insert at the prompt cursor. They arrive
    // on the MCP receive thread, so they are staged here under mutex and
    // drained on the render thread (DrainPendingAtMentionInserts). Faithful
    // to TS useIdeAtMentioned.ts -> inputState.insert at cursor.
    std::mutex pending_at_mention_mutex;
    std::vector<std::string> pending_at_mention_inserts;
    // Status / spinner / permission / context
    StatusBarData status_bar;
    std::optional<std::string> spinner_verb, spinner_tip;
    std::optional<PermissionRequestInfo> permission_request;    // Settings-driven UI configuration (mirrors AppState.settings subset
    // that the renderer needs — populated by the engine/app layer).
    std::string settings_model;             // Configured default model
    std::string settings_agent_name;        // Configured settings.agent (TS getInitialSettings().agent)

    // Bridge / remote-control footer projection (TS replBridge* AppState
    // fields; projected from AppStore in SyncState).
    bool bridge_enabled = false;
    bool bridge_explicit_remote = false;
    bool bridge_connected = false;
    bool bridge_session_active = false;
    bool bridge_reconnecting = false;
    bool bridge_selected = false;           // footer item focused (IDE selection)

    // Footer pasting indicator (TS usePasteHandler.ts isPasting +
    // PASTE_COMPLETION_TIMEOUT_MS = 100ms). A multi-char event (terminal
    // paste arrives as one batch) stamps this point; RenderLeftSide shows
    // "Pasting text…" for 100ms after it. Event-driven — no ticker.
    std::optional<std::chrono::steady_clock::time_point> pasting_since;

    // Voice footer indicator (TS REF: src/context/voice.tsx voiceState
    // 'idle'|'recording'|'processing'; VoiceIndicator.tsx).  Written only
    // via ProjectVoiceFooterStatus.  voice_processing_since stamps the
    // Processing transition so the renderer can derive wall-clock elapsed
    // seconds for the 2s sine pulse (same by-value timestamp pattern as
    // pasting_since — no pointer lifetime).
    cc::ui::prompt::FooterVoiceState voice_footer_status =
        cc::ui::prompt::FooterVoiceState::Idle;
    // TS Notifications.tsx:283 gates the indicator on voiceEnabled. The seam
    // defaults to false (no voice service wired in production yet); the
    // future voice service sets both this and the status together.
    bool voice_enabled = false;
    std::optional<std::chrono::steady_clock::time_point> voice_processing_since;

    // TS REF: src/hooks/useTextInput.ts:126-153 handleEscape via
    // src/hooks/useDoublePress.ts:6 DOUBLE_PRESS_TIMEOUT_MS = 800.
    // First Esc on non-empty input arms (notification "Esc again to clear");
    // a second Esc within 800ms persists to prompt history and clears.
    // Event-driven — no timer (same pattern as pasting_since).
    std::optional<std::chrono::steady_clock::time_point> escape_pending_since;

    // TS REF: src/hooks/useTextInput.ts:108-120 handleCtrlC double-press —
    // useExitOnCtrlCD exitState projected for
    // LeftSideOptions.exit_message_show. Presence of the timestamp = show;
    // expiry is event-driven at render (pasting_since pattern). The app
    // layer owns the exit decision; the screen only renders the window.
    std::optional<std::chrono::steady_clock::time_point> exit_message_until;
    std::string exit_message_key = "Ctrl-C";

    bool status_line_enabled = false;       // User-configurable status line
    std::string status_line_command;        // Shell command for status line
    int status_line_padding = 0;            // Horizontal padding for status line
    std::string status_line_text;           // Cached output of the status line command (may contain ANSI)
    // Counts
    int background_task_count = 0, teammate_count = 0;
    bool teams_footer_selected = false;

    // ── Live teams (leader view) ─────────────────────────────────────────
    // TS REF: src/components/teams/TeamStatus.tsx (footer count) +
    // TeamsDialog.tsx (roster) + CoordinatorAgentStatus.tsx AgentLine
    // (per-teammate live status + output tail). Projected by AppAdapter from
    // (a) agent_runtime::native_agent_store() for in-process teammates and
    // (b) cc::utils::swarm_pane_observer for tmux pane teammates.
    // Event-driven: the observer posts a refresh when pane content changes;
    // there is no render ticker (see app_team_projection.cpp).
    std::vector<teams::live::LiveTeammate> live_teammates;
    // Selection cursor for the TeamsView modal (TeamsViewPayload.selected_index
    // mirrors this when the dialog is open).
    int teams_overview_selected_index = 0;
    // Permission mode (cycled via shift+tab; TS REF: getNextPermissionMode.ts)
    cc::ui::prompt::footer::PermissionMode permission_mode =
        cc::ui::prompt::footer::PermissionMode::Default;
    // Dialog-suppression flag (typing -> suppress interrupt dialogs)
    bool is_prompt_input_active = false;
    // M7: True when a JSX tool result is currently rendering an animation in
    // the message stream.  When set, Band3 dialogs (ToolPermission overlay,
    // PromptDialog, Elicitation) are suppressed to avoid visual clash.
    bool is_tool_animation_active = false;
    std::chrono::steady_clock::time_point last_keystroke;

    // UI15: opaque wizard component handles (lazily created by
    // dialog_router::get_install_*_wizard(), stored as shared_ptr<void>
    // so ReplScreenState doesn't need to import their types).
    // UI13: agent wizard component handle (lazily created by
    // dialog_router::get_agent_wizard()).
    std::shared_ptr<void> wizard_agent;
    std::vector<cc::ui::agents::cards::AgentCardData> agent_cards;
    std::shared_ptr<void> agents_component;
    // UI8: trust dialog component handle (lazy-created; opaque).
    std::shared_ptr<void> wizard_trust;
    // dlg-permission-legacy: rich permission panel for the dormant
    // ReplMode::ToolPermission branch.  State-owned (wizard_trust
    // pattern) so focus/PromptState survive repaint; keyed on request
    // identity.  TS REF: PermissionRequest.tsx:47-82 (tool dispatch).
    std::shared_ptr<void> tool_permission_component;
    std::string tool_permission_key;
    // UI3: settings dialog component (lazy-created).  Opaque so state
    // doesn't need to import the settings dialog module types.
    std::shared_ptr<void> settings_component;
    // Optional external ConfigManager reference.  When set, settings_dialog
    // reads/writes against this engine-owned instance; otherwise it uses a
    // thread-local fallback (snapshot-only).
    void* settings_config = nullptr;
    // Initial tab when the settings dialog is opened.  Defaults to General;
    // commands like /permissions may set this to Permissions before opening.
    // Typed as int to keep ReplScreenState free of settings_dialog type deps.
    int settings_initial_tab = 0;  // matches SettingsTabId::General = 0

    // M7 Dialog Framework (Task #124): dialog queue (4 slots) +
    // renderer/event-handler registry.  The engine pushes payloads into
    // dialog_queue between frames; ReplScreen dispatches render + events
    // through dialog_renderers at priority Standalone > Modal > Overlay
    // > Bottom.
    DialogQueue dialog_queue;
    DialogRendererRegistry dialog_renderers;
};

/// Single write seam for the voice footer projection.  The future voice
/// service (cc::hooks::voice on_state_change / cc::context::VoiceState)
/// calls this between frames; TS Error and Idle both project to Idle here.
/// No voice_hooks/service wiring exists in this gap — only the seam and
/// the read-only renderer.  Entering Processing stamps the wall-clock
/// anchor used by the 2s sine pulse (TS ProcessingShimmer elapsedSec);
/// leaving Processing clears it.  The renderer never mutates state.
// TS REF: src/context/voice.tsx (voiceState transitions) and
// VoiceIndicator.tsx:107 elapsedSec = time / 1000.
inline void ProjectVoiceFooterStatus(
    ReplScreenState& s, cc::ui::prompt::FooterVoiceState next) {
    if (next == cc::ui::prompt::FooterVoiceState::Processing &&
        s.voice_footer_status != cc::ui::prompt::FooterVoiceState::Processing) {
        s.voice_processing_since = std::chrono::steady_clock::now();
    } else if (next != cc::ui::prompt::FooterVoiceState::Processing) {
        s.voice_processing_since.reset();
    }
    // Projecting an active state means a voice service is driving the UI,
    // which implies voiceEnabled (TS Notifications.tsx:283). Idle does not
    // disable — the service may simply not be recording.
    if (next != cc::ui::prompt::FooterVoiceState::Idle) {
        s.voice_enabled = true;
    }
    s.voice_footer_status = next;
}

/// Engine-facing callbacks (TS ReplScreen external prop callbacks).
struct ReplScreenCallbacks {
    std::function<void(const std::string&, InputMode)> on_submit;
    std::function<void()> on_interrupt;                 // Ctrl+C
    std::function<void()> on_exit;                      // Ctrl+D or /exit
    /// TS REF: src/keybindings/defaultBindings.ts:42 'ctrl+l':'app:redraw'
    /// (Global context) + useGlobalKeybindings.tsx handleRedraw ->
    /// ink forceRedraw (ERASE_SCREEN '\x1b[2J' + CURSOR_HOME '\x1b[H', then
    /// repaint). The engine forces a terminal repaint WITHOUT mutating the
    /// input text or autocomplete state.
    std::function<void()> on_redraw;
    /// TS REF: src/hooks/useTextInput.ts:142-150 — Esc double-press clear
    /// persists the original value via addToHistory before clearing. The
    /// engine owns the session id / project cwd for the history append.
    std::function<void(const std::string&)> on_save_to_history;
    // Legacy simple permission response (allow/deny + always flag)
    std::function<void(bool, std::optional<bool>)> on_permission_response;
    // M6: Rich permission response — decision kind, scope, and feedback text.
    // Mirrors TS onPermissionRequestDecision callback.
    std::function<void(
        std::string_view decision,  // "allow_once", "allow_always", "deny", "abort"
        std::string_view scope,     // "session", "global", "project", "loom_folder", etc.
        std::string_view feedback   // user feedback text, may be empty
    )> on_permission_decision;
    std::function<void(ReplMode, int)> on_dialog_action;
    std::function<void(ReplMode)> on_mode_change;
    std::function<void(const std::string& command)> enqueue_slash_command;
    std::function<std::optional<cc::ui::agents::cards::AgentCardData>(
        std::string_view agent_id)> load_agent_for_wizard;
    std::function<void(const cc::ui::agents::wizard::WizardDraft& draft)> save_agent_from_wizard;
    std::function<void()> on_local_jsx_cancel;
    std::function<bool(Event)> on_local_jsx_event;
    /// Called when user cycles permission mode (shift+tab).  TS REF:
    /// PromptInput.tsx:1409 handleCycleMode → cyclePermissionMode().
    std::function<void(cc::ui::prompt::footer::PermissionMode)> on_permission_cycle;
    /// GAP 3: msg-system-api-error-retry — called when user clicks "Retry"
    /// on an API error message.  Re-sends the last user message.
    /// TS REF: src/components/messages/SystemAPIErrorMessage.tsx — the
    ///   retry button re-triggers the last user submission.
    std::function<void()> on_retry;
    /// P2 gap api-error-retry: called when user clicks "Clear session"
    /// on a session-expired API error card.  Resets the conversation so
    /// the user can re-authenticate.
    /// TS REF: SystemAPIErrorMessage.tsx — onClearSession prop.
    std::function<void()> on_clear_session;
    /// TS REF: Messages.tsx L703-712 + Markdown.tsx L186-235 — shared
    /// StreamingMarkdown instance for the streaming-text tail row.
    /// When non-null, RenderMessages threads it to the messages list
    /// so is_streaming rows use stable-prefix caching.
    ::cc::ui::StreamingMarkdown* streaming_md = nullptr;
};

}  // namespace cc::ui
