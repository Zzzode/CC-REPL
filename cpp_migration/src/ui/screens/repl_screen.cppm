/// @file repl_screen.cppm
/// @brief Main REPL screen skeleton: enums, state, layout orchestration,
///        dialog routing, and event binding.  Rendering sub-modules delegate
///        to dedicated UIx agents (see ownership matrix below).
/// Migrated from src/screens/REPL.tsx (5005 lines)
///
/// =========================================================
/// PHASE 4 COMPONENT MATRIX — Sub-Component -> Agent Ownership
/// =========================================================
/// UI2  PromptInput full              UI11 Onboarding / wizards
/// UI3  Settings / model picker      UI12 Tasks panel UI
/// UI4  Assistant msg (markdown)     UI13 Agents panel / editor
/// UI5  User msg / attachments       UI14 Teams panel
/// UI6  CustomSelect / dropdowns     UI15 Install wizards
/// UI7  Structured diff viewer       UI16 Plugin dialogs / recs
/// UI8  Trust + sandbox dialogs      UI17 Code rendering (hl/md/term)
/// UI9  Permissions system           UI18 Feedback surveys
/// UI10 MCP dialogs (elicit/OAuth)   UI19 Spinner animations + tree
/// =========================================================
///
/// RENDERING DELEGATION (this file does NOT contain render bodies):
///   status bar   -> cc.ui.prompt.prompt_input_footer (UI1, user-configurable command output)
///   spinner      -> cc.ui.components.spinner_widget (UI19)
///   msg list     -> cc.ui.messages (RenderMessages wrapper, UI4/5)
///   prompt input -> cc.ui.prompt.prompt_input_full (UI2)
///   dialogs      -> cc.ui.dialogs.* (DialogQueue 4-slot system, UI8-UI11/UI16)
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include <optional>
#include <variant>
#include <format>
#include <cstdint>
#include <chrono>
#include <algorithm>
#include <deque>
#include <random>
#include <array>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/screen/string.hpp>  // for string_width

export module cc.ui.repl_screen;

// Core engine types (Role, Message, ContentBlock, ImageBlock, etc.)
import cc.types.types;

// --- Sub-modules we DEPEND ON (skeleton-wired, bodies delegated) ---
import cc.ui.task_list_ui;
import cc.ui.team_status;
import cc.ui.messages.message_row;
import cc.ui.messages.message_image;
import cc.ui.messages.messages_list;
import cc.ui.messages.user_text_message;
import cc.ui.messages.assistant_text_message;
import cc.ui.messages.system_text_message;
import cc.ui.messages.thinking_message;
import cc.ui.messages.tool_use_message;
import cc.ui.messages.message_tool_result;
import cc.ui.messages.local_command_output_message;
import cc.ui.dialogs.permission_dialog;
import cc.ui.dialogs.system;
import cc.ui.dialogs.cost_threshold_dialog;
import cc.ui.dialogs.idle_return_dialog;
import cc.ui.dialogs.settings_dialog;
import cc.config.config;
import cc.ui.trust_dialog;
import cc.ui.sandbox_dialog;
import cc.ui.agents.agent_cards;
import cc.ui.agents.agent_wizard;
import cc.tools.agent_display;
import cc.ui.dialogs.install_github_app_wizard;
import cc.ui.dialogs.install_slack_app_wizard;
// Welcome header: Clawd mark + animated asterisk, wired into RenderReplScreen
// for fresh sessions.
import cc.ui.design.logo;
// M2: theme::current_theme() for the clawd_body colour used by the banner.
import cc.ui.design.theme;
// M8 (P0-1 glyph unification): shared figures/constants + palette tokens.
import cc.ui.design.figures;
import cc.ui.design.tokens;
// M2: format_welcome_message + kWelcomeTips feed for the welcome header.
import cc.ui.logo;
// P0-4: LogoV2 3-mode dispatch + WelcomeV2 58-col static card + full
// notice stack (Voice/Opus1m/Channels/Debug/Emergency/Tmux/Org/Sandbox/
// StatusNotices ×6 / GuestPasses / OverageCredit).  Condensed mode is
// the default (no changelog/onboarding/force_full_logo available yet);
// Compact & Horizontal modes are used when the caller opts in via the
// force_full_logo = true flag (see RenderWelcomeHeader below).
import cc.ui.logo_v2;
// Terminal size probe for adaptive layout (welcome header centering + future
// message-scroll height clamping).
import cc.ui.ink_utils;
// Runtime terminal feature detection (fullscreen mode, mouse tracking, etc.)
import cc.utils.terminal_helpers;
// M1: FullscreenLayout slot-system — faithful port of TS FullscreenLayout's
// region model (scrollable / bottom / overlay / modal / bottomFloat).  The
// shell is now composed via this slot composer instead of a flat vbox.
import cc.ui.layout.fullscreen;
// M3: the REAL text editor component (ui::components::TextInputImpl).  This is
// the faithful counterpart of TS BaseTextInput.tsx's inputState — it owns
// cursor tracking, multi-line layout, selection rendering, and the suggestions
// dropdown.  Previously the prompt was rendered by a hand-rolled ~90-line
// RenderPromptInput body that IGNORED this component ("shelfware").  M3 wires
// it in: each render we sync a TextInputImpl from ReplScreenState and delegate
// the caret/multiline/selection painting to it (mirroring TS
// useDeclaredCursor, which parks the terminal cursor at the insertion point).
import ui.components.text_input;
// M5: Declared cursor support — parks the real terminal cursor at the text
// input's insertion point so IME preedit renders inline and screen readers /
// magnifiers can follow the input.  Faithful port of TS useDeclaredCursor +
// CursorDeclarationContext.
import cc.ui.common.declared_cursor;
// M3: vim mode badge / mode_display helper (Normal/Insert/Visual/VisualLine/
// Command).  Faithful to TS VimInput's -- INSERT -- / -- NORMAL -- / -- VISUAL
// indicator driven by vim_input state.
import cc.ui.prompt.vim_input;
// M5: PromptInputFooter — faithful port of TS PromptInputFooter.tsx +
// PromptInputFooterLeftSide.tsx.  Renders the area below the prompt input
// with left/right columns: ModeIndicator, tasks, teams, hints, bridge status.
import cc.ui.prompt.prompt_input_footer;
// M7: Core dialog framework — DialogQueue, DialogRendererRegistry, DialogFrame.
// Faithful port of TS dialog system architecture (DialogType, DialogSlot, priority bands).
import cc.ui.dialogs.system;
import cc.ui.dialogs.frame;
// M7: Faithful HelpView dialog — 3-tab help panel (general / commands / custom).
import cc.ui.dialogs.help_view;
// M7: Faithful SettingsView dialog — read-only settings panel.
import cc.ui.dialogs.settings_view;
// M7: Faithful About dialog.
import cc.ui.dialogs.about;
// Spinner verb list (TS src/constants/spinnerVerbs.ts) used by RenderSpinner
// to pick a random playful loading label per request.
import cc.constants.spinner_verbs;
// P0-3: VirtualMessageList — O(viewport) windowed rendering for transcripts
// with >80 rows.  Exports VirtualListHandle / VirtualListState types used
// below in ReplScreenState and ScrollTranscript.
import cc.ui.messages.virtual_list;

// M6: Faithful permission panels — bash / file_edit / file_write.
// These replace the old crude permission_dialog.cppm string renderer.
import cc.ui.permissions.permission_bash;
import cc.ui.permissions.permission_file_edit;
import cc.ui.permissions.permission_file_write;
import cc.ui.permissions.single_prompt;

// Forward imports (implement bodies in owning agent modules):
//   cc.ui.dialogs.{permission_prompts,mcp_dialogs,trust_dialog,
//                  sandbox_dialog,settings_dialog,model_picker,
//                  ide_dialogs,teleport_dialogs,plugin_dialog,
//                  feedback_survey,config_dialog,mcp_dialogs}
//   cc.ui.prompt.{prompt_input_full,autocomplete,vim_input}
//   cc.ui.messages.{assistant_message,user_message,structured_diff}
//   cc.ui.components.{custom_select,diff_view,spinner_widget,
//                     file_tree,text_input_widget,notification,
//                     cost_display,dev_bar,status_line}
//   cc.ui.{design.dialog,hooks.hooks_ui,permissions.permission_views,
//          agents.agent_editor,tasks.task_list_ui,markdown,terminal}

export namespace cc::ui::repl_screen {
using namespace ftxui;

// M7: pull the dialog framework types up into convenient aliases.
namespace dsys_fw   = cc::ui::dialogs::system;
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
    // UI15 (Phase 4) — install-command wizard overlays.
    InstallGitHubApp,   // 12-step /install-github-app  FTXUI wizard
    InstallSlackApp,    // 3-step  /install-slack-app   FTXUI wizard
    // UI13 — agent wizard (create/edit).
    CreateAgent,        // 4-step new-agent wizard
    EditAgent,          // 4-step edit-agent wizard
    // UI8  — trust dialog (standalone takeover; 4-tier risk UX).
    TrustDialog,        // 'trust-dialog'
};

/// Input modes.  TS PromptInputMode (textInputTypes.ts:265) + VimMode (tI:222).
enum class InputMode : std::uint8_t {
    Prompt, Bash, SlashCommand, HistorySearch, PlanMode,
    VimInsert, VimNormal, VimVisual,
    OrphanedPermission, TaskNotification,
};

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
};/// Lean orchestration state.  Full app state lives in services/; the
/// engine writes computed projections into this struct between frames.
struct ReplScreenState {
    ReplMode mode = ReplMode::Normal;
    InputMode input_mode = InputMode::Prompt;
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
    // Input
    std::string input_text;
    // TS-style contextual placeholder rather than a generic default.
    std::string input_placeholder = "Try \"write a test\", \"/help\", or ask anything...";
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
    // M2: oauthAccount.displayName analogue — drives formatWelcomeMessage
    // ("Welcome back {user}!" vs "Welcome back!").  Empty for new users.
    std::string user_display_name;
    // P0 Gap logov2-render-modes-missing: pre-rendered feed content for the
    // full-logo horizontal mode's right column. Populated by the engine layer
    // from session storage (recent activity) and changelog parser (what's
    // new). Empty vectors fall back to placeholders defined in RenderWelcomeHeader.
    std::vector<std::string> recent_activity_lines;
    std::vector<std::string> changelog_lines;
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
        // INF-02: stable identity for selection preservation across refreshes
        // (TS getPreservedSelection-by-id, src/hooks/useTypeahead.tsx:52-74).
        // Defaults to display_text when a caller doesn't supply a richer id,
        // so same-display items from different sources can be distinguished.
        // `{}` in-class init so existing partial designated initializers (e.g.
        // in tests/test_ui.cpp) don't trip -Wmissing-designated-field-initializers.
        std::string id{};
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
    bool status_line_enabled = false;       // User-configurable status line
    std::string status_line_command;        // Shell command for status line
    int status_line_padding = 0;            // Horizontal padding for status line
    std::string status_line_text;           // Cached output of the status line command (may contain ANSI)
    // Counts
    int background_task_count = 0, teammate_count = 0;
    bool teams_footer_selected = false;
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
    std::shared_ptr<void> wizard_install_github_app;
    std::shared_ptr<void> wizard_install_slack_app;
    // UI13: agent wizard component handle (lazily created by
    // dialog_router::get_agent_wizard()).
    std::shared_ptr<void> wizard_agent;
    std::vector<cc::ui::agents::cards::AgentCardData> agent_cards;
    std::shared_ptr<void> agents_component;
    // UI8: trust dialog component handle (lazy-created; opaque).
    std::shared_ptr<void> wizard_trust;
    // UI3: settings dialog component (lazy-created).  Opaque so state
    // doesn't need to import the settings dialog module types.
    std::shared_ptr<void> settings_component;
    // Optional external ConfigManager reference.  When set, settings_dialog
    // reads/writes against this engine-owned instance; otherwise it uses a
    // thread-local fallback (snapshot-only).
    void* settings_config = nullptr;

    // M7 Dialog Framework (Task #124): dialog queue (4 slots) +
    // renderer/event-handler registry.  The engine pushes payloads into
    // dialog_queue between frames; ReplScreen dispatches render + events
    // through dialog_renderers at priority Standalone > Modal > Overlay
    // > Bottom.
    DialogQueue dialog_queue;
    DialogRendererRegistry dialog_renderers;
};

/// Engine-facing callbacks (TS ReplScreen external prop callbacks).
struct ReplScreenCallbacks {
    std::function<void(const std::string&, InputMode)> on_submit;
    std::function<void()> on_interrupt;                 // Ctrl+C
    std::function<void()> on_exit;                      // Ctrl+D or /exit
    // Legacy simple permission response (allow/deny + always flag)
    std::function<void(bool, std::optional<bool>)> on_permission_response;
    // M6: Rich permission response — decision kind, scope, and feedback text.
    // Mirrors TS onPermissionRequestDecision callback.
    std::function<void(
        std::string_view decision,  // "allow_once", "allow_always", "deny", "abort"
        std::string_view scope,     // "session", "global", "project", "claude_folder", etc.
        std::string_view feedback   // user feedback text, may be empty
    )> on_permission_decision;
    std::function<void(ReplMode, int)> on_dialog_action;
    std::function<void(ReplMode)> on_mode_change;
    // UI15 Slack wizard "Launch /slack now" shortcut -> REPL engine.
    std::function<void(const std::string& command)> enqueue_slash_command;
    std::function<std::optional<cc::ui::agents::cards::AgentCardData>(
        std::string_view agent_id)> load_agent_for_wizard;
    std::function<void(const cc::ui::agents::wizard::WizardDraft& draft)> save_agent_from_wizard;
    std::function<void()> on_local_jsx_cancel;
    std::function<bool(Event)> on_local_jsx_event;
};

// =========================================================
// Rendering helpers — DELEGATED (owning agent: see UIx tags)
// =========================================================

// UI1: status bar — delegates to cc.ui.components.status_line.
// For now we provide a semantic assembler that status_line will style.
[[nodiscard]] inline Element RenderStatusBar(const StatusBarData& d) {
    Elements L = { text(" ") };
    L.push_back(text(d.model_name) | bold | color(Color::Cyan));
    if (d.is_fast_mode)  L.push_back(text(" fast") | color(Color::Yellow) | dim);
    if (d.is_auto_mode)  L.push_back(text(" [auto]") | color(Color::Magenta));
    if (d.agent_name)   { L.push_back(text(" @") | dim);
        L.push_back(text(*d.agent_name) | color(Color::Blue)); }
    if (d.effort_level) L.push_back(text(" [" + *d.effort_level + "]") | dim);
    if (d.is_brief_mode)L.push_back(text(" [brief]") | dim | color(Color::GrayLight));
    Elements R;
    if (d.context_token_count > 0) {
        R.push_back(text(std::format("ctx:{}", d.context_token_count)) | dim);
        R.push_back(text(" | ") | dim); }
    if (d.input_tokens || d.output_tokens) R.push_back(
        text(std::format("{}v {}^", d.input_tokens, d.output_tokens)) | dim);
    if (d.cost_usd) { R.push_back(text(" ") | dim); R.push_back(
        text(std::format("${:.4f}", *d.cost_usd)) | dim | color(Color::Green)); }
    if (d.swarm_session_count > 0) {
        R.push_back(text(" | ") | dim);
        R.push_back(text(std::format("{} swarm", d.swarm_session_count))
                        | dim | color(Color::Magenta)); }
    if (d.session_name) { R.push_back(text(" | ") | dim);
        R.push_back(text(*d.session_name) | dim); }
    if (d.bridge_connected) {
        R.push_back(text(" | ") | dim);
        R.push_back(text("<-> bridge") | color(Color::Cyan) | dim); }
    R.push_back(text(" "));
    return hbox({ hbox(L), filler(), hbox(R) })
         | bgcolor(Color::RGB(20, 20, 22));
}

// UI19: spinner line shell.  Faithful port of TS Spinner.tsx + BriefSpinner.
// Single claude-gold theme, cycling TEARDROP_ASTERISK glyph, random playful
// verb sampled from spinner_verbs list, 3-dot blink cadence.
[[nodiscard]] inline Element RenderSpinner(
    SpinnerMode m, const std::optional<std::string>& verb,
    const std::optional<std::string>& tip,
    int frame = 0) {
    if (m == SpinnerMode::Hidden) return text("");
    // TS Spinner.tsx: defaultColor='claude' (amber/gold), shimmer animation.
    // Single theme token regardless of mode — no per-mode color switch.
    const Color kClaudeGold = Color::RGB(217, 154, 56);  // ~claude token

    // Pick a random playful verb once "per mount".  We don't track mount
    // state here, so hash the mode + pid and sample the verbs list.
    using cc::constants::spinner_verbs::SPINNER_VERBS;
    static std::mt19937 rng{std::random_device{}()};
    static thread_local std::uniform_int_distribution<std::size_t> dist(
        0, SPINNER_VERBS.size() - 1);
    static thread_local std::string_view cached_verb = SPINNER_VERBS[dist(rng)];
    // Re-sample when mode transitions from Hidden -> something (frame==0
    // from caller signals first visible frame of a new request).
    if (frame == 1) cached_verb = SPINNER_VERBS[dist(rng)];

    // 3-dot blink cycle: Math.floor(time/300)%3 per TS BriefSpinner.
    // Frame increments ~per render at ~60Hz; use frame/18 as ~300ms tick.
    const int dot_idx = std::max(0, frame / 18) % 3;
    const std::string dots = std::string(static_cast<std::size_t>(dot_idx + 1), '.') +
                             std::string(static_cast<std::size_t>(3 - dot_idx - 1), ' ');

    // SpinnerGlyph cycle (frames of TEARDROP_ASTERISK animation); 8 frames.
    constexpr std::array<std::string_view, 8> kGlyphs = {
        "✻", "❋", "✦", "✧", "✶", "✷", "✸", "✹"
    };
    const auto glyph = kGlyphs[static_cast<std::size_t>(std::max(0, frame / 6)) % kGlyphs.size()];

    std::string_view selected_verb = verb ? std::string_view(*verb) : cached_verb;
    std::string label = std::string(selected_verb) + "\xE2\x80\xA6";  // …
    Elements p = {
        text("  ") | size(WIDTH, EQUAL, 2),  // paddingLeft=2
        text(std::string(glyph)) | color(kClaudeGold),
        text(" "),
        text(label) | color(kClaudeGold),
        text(dots)  | color(kClaudeGold) | dim,
    };
    if (tip) p.push_back(text("  -- " + *tip) | dim);
    return hbox(p);
}

[[nodiscard]] inline std::string truncate_columns(std::string text, int max_cols) {
    if (max_cols <= 0) return {};
    if (string_width(text) <= max_cols) return text;
    while (!text.empty() && string_width(text + "…") > max_cols) {
        text.pop_back();
    }
    return text + "…";
}

[[nodiscard]] inline std::string repeat_welcome_segment(std::string_view text, int count) {
    std::string out;
    for (int i = 0; i < count; ++i) out += text;
    return out;
}

[[nodiscard]] inline std::vector<MessageDisplayEntry> BuildVisibleMessages(
    const ReplScreenState& s) {
    auto entries = s.messages;
    if (!s.active_local_jsx_command) return entries;

    MessageDisplayEntry command;
    command.role = "user";
    command.is_local_command_input = true;
    command.content_preview = "/" + s.active_local_jsx_command_name;
    if (!s.active_local_jsx_command_args.empty()) {
        command.content_preview += " " + s.active_local_jsx_command_args;
    }
    command.timestamp = std::chrono::system_clock::now();
    entries.push_back(std::move(command));

    std::string content = s.active_local_jsx_content;
    if (!content.empty() && content.back() != '\n') content.push_back('\n');
    if (content.find("Esc to close") == std::string::npos &&
        content.find("Esc to go back") == std::string::npos) {
        content += "\nEsc to close";
    }
    MessageDisplayEntry local_jsx;
    local_jsx.role = "system";
    local_jsx.is_local_jsx_output = true;
    local_jsx.content_preview = std::move(content);
    local_jsx.timestamp = std::chrono::system_clock::now();
    entries.push_back(std::move(local_jsx));

    return entries;
}

// UI4/UI5: message list.  Delegates to messages_list.cppm (UI21).
// `spinner_frame` drives the tool-use header spinner animation (fix #10).
// `unseen_divider` is the optional in-transcript "N new messages" anchor
// set by useUnseenDivider (FullscreenLayout.tsx L86-190); nullopt when
// pinned to bottom or the session has no scroll-away yet.
[[nodiscard]] inline Element RenderMessages(
    const std::vector<MessageDisplayEntry>& entries,
    int sel = -1, int vlines = 40,
    int offs = 0, bool pinned = true,
    int spinner_frame = 0,
    std::optional<cc::ui::messages_list::UnseenDivider> unseen_divider =
        std::nullopt) {
    if (entries.empty()) {
        // Empty transcript: reserve zero height (do NOT return filler()/flex).
        // Returning a flex filler here caused the sibling WelcomeHeader above
        // to be compressed to 0 rows inside the outer scroll_rows vbox under a
        // nested flex distribution (see repl_screen.cppm Compose call site).
        return text("");
    }

    namespace ml = cc::ui::messages_list;
    namespace image = cc::ui::messages::image;
    ml::MessagesListInput input;
    input.rows.reserve(entries.size());
    input.shapes.reserve(entries.size());
    input.uuids.reserve(entries.size());
    input.unseen_divider = std::move(unseen_divider);

    for (const auto& m : entries) {
        // TS REF: Messages.tsx L549-553  uuid → 24-char prefix anchor.
        // Populated parallel to rows/shapes; empty strings are harmless
        // (find_divider_before_visible_index skips them).
        input.uuids.push_back(m.id);
        if (m.is_local_command_input) {
            input.shapes.push_back(messages::MessageShape::UserCommand);
            input.rows.push_back(messages::UserTextMessageData{
                .content = m.content_preview,
                .timestamp = m.timestamp,
                .quoted_reply = std::nullopt,
                .is_transcript_mode = false,
                .command_name = std::nullopt});
        } else if (m.is_local_jsx_output) {
            input.shapes.push_back(messages::MessageShape::UserLocalJsxOutput);
            input.rows.push_back(messages::UserTextMessageData{
                .content = m.content_preview,
                .timestamp = m.timestamp,
                .quoted_reply = std::nullopt,
                .is_transcript_mode = false,
                .command_name = std::nullopt});
        } else if (m.is_local_command_output) {
            input.shapes.push_back(messages::MessageShape::UserLocalCommandOutput);
            messages::local_cmd::LocalCommandOptions opts;
            opts.show_line_numbers = false;
            opts.data.exit_code = m.is_error ? 1 : 0;

            std::size_t start = 0;
            while (start <= m.content_preview.size()) {
                auto nl = m.content_preview.find('\n', start);
                std::string line = nl == std::string::npos
                    ? m.content_preview.substr(start)
                    : m.content_preview.substr(start, nl - start);
                opts.data.lines.push_back(messages::local_cmd::OutputLine{
                    .kind = m.is_error
                        ? messages::local_cmd::StreamKind::Stderr
                        : messages::local_cmd::StreamKind::Stdout,
                    .text = std::move(line),
                });
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
            input.rows.push_back(std::move(opts));
        } else if (m.role == "user") {
            if (m.is_image && m.image_block) {
                // TS parity: each user-attached image is its own UserImage row.
                // See TS src/components/UserImageMessage.tsx full file.
                // The data flow is: project_messages() → is_image=true +
                // image_block; we translate the block metadata into
                // image::ImageMessageData which message_image.cppm already
                // knows how to render.
                using image::ImageMessageData;
                using image::ImageSource;
                ImageMessageData d;
                const auto& ib = *m.image_block;
                d.timestamp = m.timestamp;
                d.media_type = ib.media_type;
                if (ib.file_name)        d.file_name = *ib.file_name;
                if (ib.source_path)      d.source = *ib.source_path;
                if (ib.width)            d.width = *ib.width;
                if (ib.height)           d.height = *ib.height;
                if (ib.size_bytes)       d.file_size = *ib.size_bytes;
                switch (ib.source) {
                    using IS = ::cc::core::ImageBlockSource;
                    case IS::Clipboard: d.source_type = ImageSource::Clipboard; break;
                    case IS::File:      d.source_type = ImageSource::File; break;
                    case IS::Base64:    d.source_type = ImageSource::Base64; break;
                    case IS::Unknown:
                    default:
                        // Heuristic: empty source + non-empty data means a
                        // raw inline paste (no file path known).
                        d.source_type = d.source.empty()
                            ? ImageSource::Clipboard
                            : ImageSource::File;
                        break;
                }
                // Pass the raw base64 payload through via the alt_text field
                // so the renderer can generate a deterministic ASCII-art
                // thumbnail from the data (without the need for a full PNG
                // decoder).  The renderer falls back to the filename if
                // alt_text is unset.
                if (!ib.data.empty())
                    d.alt_text = ib.data.substr(0, 256);
                input.shapes.push_back(messages::MessageShape::UserImage);
                input.rows.push_back(std::move(d));
            } else {
                input.shapes.push_back(messages::MessageShape::UserText);
                input.rows.push_back(messages::UserTextMessageData{
                    .content = m.content_preview,
                    .timestamp = m.timestamp,
                    .quoted_reply = std::nullopt,
                    .command_name = std::nullopt});
            }
        } else if (m.role == "assistant") {
            if (m.is_thinking) {
                input.shapes.push_back(messages::MessageShape::AssistantThinking);
                messages::thinking_message::ThinkingMessageOptions opts;
                opts.data.raw_text = m.content_preview;
                // Static / unselected view must render the collapsed "Thinking"
                // label (TS: AssistantThinkingMessage.tsx collapsed state)
                // rather than hiding the row.  The messages-list fast path
                // hides rows where thinking is neither selected nor "active".
                if (m.thinking_active) {
                    opts.data.state =
                        messages::thinking_message::ThinkingState::Active;
                }
                input.rows.push_back(std::move(opts));
            } else if (m.is_tool_use) {
                input.shapes.push_back(messages::MessageShape::AssistantToolUse);
                messages::tool_use_message::ToolUseRenderOptions opts;
                opts.call.tool_name = m.tool_name.value_or("tool");
                // Fix #8/#9: thread the real parsed tool input + status from
                // the shared projection contract into the renderer (previously
                // raw_parameters fell back to content_preview and status was
                // hard-coded Pending).
                opts.call.raw_parameters =
                    m.tool_input_json.value_or(m.content_preview);
                // M6 result_preview: thread live streaming result preview into
                // the call data so ToolUIRegistry.progress() can format a
                // dynamic progress line (e.g. last output line for Bash tools).
                if (m.tool_result_preview) {
                    opts.call.result_preview = *m.tool_result_preview;
                }
                opts.call.parameters_language = "json";
                opts.call.status = messages::tool_use_message::parse_tool_status(
                    m.tool_status.value_or("pending"));
                input.rows.push_back(std::move(opts));
            } else {
                input.shapes.push_back(messages::MessageShape::AssistantText);
                input.rows.push_back(messages::AssistantTextMessageData{
                    .content = m.content_preview,
                    .timestamp = m.timestamp,
                    .model_name = std::nullopt,
                    .is_streaming = m.is_streaming});
            }
        } else if (m.role == "tool") {
            input.shapes.push_back(messages::MessageShape::UserToolResult);
            input.rows.push_back(messages::ToolResultOptions{
                .tool_name = m.tool_name.value_or("tool"),
                .status = m.is_error
                    ? messages::ToolResultStatus::Error
                    : messages::ToolResultStatus::Success,
                .output = m.content_preview,
                .error_message = std::nullopt,
                .duration_ms = std::nullopt});
        } else {
            input.shapes.push_back(messages::MessageShape::SystemText);
            // Bridge the system-row subtype so the LIVE faithful renderer
            // (RenderSystemTextMessageFaithful dispatches per subtype) shows
            // the right glyph (※ away_summary / ✻ event / ⏺ generic).  When
            // the engine hasn't set system_subtype we derive a best-effort
            // subtype from the preview text — same labels TS SystemTextMessage
            // keys on (turn_duration / memory_saved / etc.).
            auto derive_subtype = [](const std::string& s,
                const std::optional<std::string>& hint) {
                using ST = messages::SystemMessageSubtype;
                if (hint) {
                    if (*hint == "away_summary")    return ST::AwaySummary;
                    if (*hint == "turn_duration")   return ST::TurnDuration;
                    if (*hint == "memory_saved")    return ST::MemorySaved;
                    if (*hint == "bridge_status")   return ST::BridgeStatus;
                    if (*hint == "thinking_summary")return ST::ThinkingSummary;
                    if (*hint == "hook_summary")    return ST::HookSummary;
                    if (*hint == "model_switch")    return ST::ModelSwitch;
                    if (*hint == "background_task") return ST::BackgroundTask;
                }
                // Heuristic fallback: scan preview text for TS-style keywords.
                if (s.find("away for") != std::string::npos ||
                    s.find("Welcome back") != std::string::npos)
                    return ST::AwaySummary;
                if (s.find("took") != std::string::npos ||
                    s.find("duration") != std::string::npos ||
                    s.find("seconds") != std::string::npos)
                    return ST::TurnDuration;
                if (s.find("memory") != std::string::npos ||
                    s.find("saved") != std::string::npos)
                    return ST::MemorySaved;
                if (s.find("bridge") != std::string::npos ||
                    s.find("IDE") != std::string::npos)
                    return ST::BridgeStatus;
                if (s.find("model") != std::string::npos &&
                    (s.find("switch") != std::string::npos ||
                     s.find("changed") != std::string::npos))
                    return ST::ModelSwitch;
                return ST::Plain;
            };
            input.rows.push_back(messages::SystemTextMessageData{
                .subtype = derive_subtype(m.content_preview, m.system_subtype),
                .summary = m.content_preview,
                .detail = {},
                .timestamp = m.timestamp});
        }
    }

    if (sel >= 0)
        input.selected_row_idx = static_cast<std::size_t>(sel);

    const auto N = entries.size();
    bool has_streaming = !entries.empty() && entries.back().is_streaming;
    input.streaming_tail_row = has_streaming ? N - 1 : N;
    input.pin_to_bottom = pinned;
    input.scroll_offset = std::max(0, offs);
    input.viewport_rows = std::max(1, vlines);

    return ml::render_messages_list_view(std::move(input),
                                         static_cast<std::size_t>(spinner_frame)) | flex;
}

[[nodiscard]] inline int CountTextLines(std::string_view text) {
    if (text.empty()) return 1;
    return static_cast<int>(std::count(text.begin(), text.end(), '\n')) + 1;
}

[[nodiscard]] inline int EstimateTranscriptRows(
    const std::vector<MessageDisplayEntry>& entries) {
    int rows = 0;
    for (const auto& entry : entries) {
        rows += CountTextLines(entry.content_preview);
        // Message list inserts one empty separator after each rendered row.
        rows += 1;
    }
    return rows;
}

inline bool ScrollTranscript(const std::shared_ptr<ReplScreenState>& state,
                             int delta) {
    if (!state || delta == 0) return false;

    const int viewport_rows = std::max(1, state->viewport_height_lines);

    // P0-3 path: if the VirtualMessageList is active for this frame, use
    // its JumpHandle (prefix-sum table of exact visual lines) for O(log N)
    // scroll bounds instead of the crude EstimateTranscriptRows heuristic.
    if (state->virtual_list_active) {
        namespace vl = cc::ui::messages::virtual_list;
        const vl::JumpHandle& jh = state->virtual_jh;
        const int total_lines = jh.total();
        if (total_lines <= viewport_rows) return false;
        const int max_top = total_lines - viewport_rows;

        const int old_top = std::clamp(state->scroll_offset, 0, max_top);
        int target = old_top + delta;
        // Guarantee at least one row moves on PageUp/PageDown style deltas:
        // if target equals old_top, step by one row in the requested
        // direction using binary search.
        if (delta > 0 && target <= old_top) {
            size_t cur = jh.find_row_at_visual_line(old_top);
            if (cur + 1 < jh.size()) target = jh.find_visual_top_for_row(cur + 1);
        } else if (delta < 0 && target >= old_top) {
            size_t cur = jh.find_row_at_visual_line(old_top);
            if (cur > 0) target = jh.find_visual_top_for_row(cur - 1);
        }
        target = std::clamp(target, 0, max_top);
        if (target == old_top) return false;

        state->scroll_offset = target;
        state->scroll_pinned_to_bottom = (target >= max_top);

        // If we are maintaining a live VirtualListState (Component-mode
        // wiring), also update its scroll_top so Render() reuses it.
        if (state->virtual_list_state) {
            state->virtual_list_state->scroll_top = target;
            vl::update_sticky_after_scroll(*state->virtual_list_state,
                                            old_top);
        }
        return true;
    }

    const auto visible_messages = BuildVisibleMessages(*state);
    if (visible_messages.empty()) return false;
    const int max_offset =
        std::max(0, EstimateTranscriptRows(visible_messages) - viewport_rows);
    if (max_offset == 0) return false;

    const int next =
        std::clamp(state->scroll_offset + delta, 0, max_offset);
    state->scroll_offset = next;
    state->scroll_pinned_to_bottom = next >= max_offset;
    return true;
}

[[nodiscard]] inline Element RenderClawdMark(Color body, Color bg) {
    return vbox({
        hbox({
            text(" ▐") | color(body),
            text("▛███▜") | color(body) | bgcolor(bg),
            text("▌") | color(body),
        }),
        hbox({
            text("▝▜") | color(body),
            text("█████") | color(body) | bgcolor(bg),
            text("▛▘") | color(body),
        }),
        text("  ▘▘ ▝▝  ") | color(body),
    });
}

[[nodiscard]] inline Element RenderWelcomeFeed(std::string title,
                                               std::string message,
                                               int width,
                                               Color accent,
                                               Color muted) {
    width = std::max(width, 10);
    Elements rows;
    rows.push_back(text(std::move(title)) | bold | color(accent));
    rows.push_back(text(truncate_columns(std::move(message), width)) | color(muted) | dim);
    return vbox(std::move(rows)) | size(WIDTH, EQUAL, width);
}

[[nodiscard]] inline Element RenderWelcomeFeedColumn(int width,
                                                     Color accent,
                                                     Color muted) {
    width = std::max(width, 30);
    return vbox({
        RenderWelcomeFeed(
            "Recent activity",
            "No recent activity",
            width,
            accent,
            muted),
        separator() | color(accent),
        RenderWelcomeFeed(
            "What's new",
            "Check the Claude Code changelog for updates",
            width,
            accent,
            muted),
    }) | size(WIDTH, EQUAL, width);
}

[[nodiscard]] inline Element RenderWelcomeWindow(Element title,
                                                 Element body,
                                                 int width,
                                                 int title_width,
                                                 Color accent) {
    width = std::max(width, 4);
    const int title_offset = std::min(3, std::max(0, width - 2));
    const int right_rule_width =
        std::max(0, width - 2 - title_offset - title_width);

    Element top = hbox({
        text("╭") | color(accent),
        text(repeat_welcome_segment("─", title_offset)) | color(accent),
        std::move(title),
        text(repeat_welcome_segment("─", right_rule_width)) | color(accent),
        text("╮") | color(accent),
    });
    Element middle = hbox({
        separator() | color(accent),
        std::move(body) | size(WIDTH, EQUAL, width - 2),
        separator() | color(accent),
    });
    Element bottom = hbox({
        text("╰") | color(accent),
        text(repeat_welcome_segment("─", width - 2)) | color(accent),
        text("╯") | color(accent),
    });
    return vbox({
        std::move(top),
        std::move(middle),
        std::move(bottom),
    }) | size(WIDTH, EQUAL, width);
}

[[nodiscard]] inline Element RenderWelcomeLeftPanel(const ReplScreenState& s,
                                                    int spinner_frame,
                                                    int width,
                                                    Color accent,
                                                    Color muted,
                                                    Color text_color,
                                                    Color bg) {
    const std::string welcome = cc::ui::logo::format_welcome_message(
        s.user_display_name);
    const std::string model_line = !s.model_display_name.empty()
        ? s.model_display_name
        : s.settings_model;
    std::string cwd_line = s.cwd;
    if (!cwd_line.empty()) {
        cwd_line = truncate_columns(std::move(cwd_line), std::max(10, width - 2));
    }

    Elements meta;
    if (!model_line.empty()) {
        meta.push_back(text(truncate_columns(model_line, std::max(10, width - 2)))
            | color(muted) | dim);
    }
    if (!cwd_line.empty()) {
        meta.push_back(text(std::move(cwd_line)) | color(muted) | dim);
    }
    if (meta.empty()) meta.push_back(text(""));

    return vbox({
        text(""),
        hbox({
            cc::ui::design::logo::welcome_animated_asterisk(spinner_frame),
            text(" "),
            text(welcome) | bold | color(text_color),
        }) | center,
        text(""),
        RenderClawdMark(accent, bg) | center,
        text(""),
        vbox(std::move(meta)) | center,
    }) | size(WIDTH, EQUAL, width) | size(HEIGHT, GREATER_THAN, 9);
}

// UI0: welcome header.  Faithful to TS LogoV2/CondensedLogo + Opus1mMergeNotice:
//   Row 1: orange AnimatedClawd + "Claude Code" bold + "vX.X.X" dim
//   Row 2: model · billing_type dim
//   Row 3: [@agent · ] cwd dim
//   Row 4: ↑ "Opus now defaults to 1M context" banner
// Shown only on a fresh idle session (messages empty + spinner hidden).
// The old ASCII-art bordered card / left-panel / FeedColumn helpers are
// preserved in this file but no longer called by this renderer.
//
// P0-4 Faithful dispatch (TS LogoV2.tsx return paths):
//   is_condensed_mode (default)  → CondensedLogo + 10 notices flat stack
//   force_full_logo + cols<70    → Compact round card + flat notice stack
//   force_full_logo + cols>=70   → Horizontal left|divider|feed card + stack
// The caller may set s.debug_* / s.tmux_* / s.sandboxing_enabled fields to
// drive notice activation; they default to off so the header renders the
// same minimal 4-row look the TS default-condensed branch produces.
[[nodiscard]] inline Element RenderWelcomeHeader(const ReplScreenState& s,
                                                 int /*spinner_frame*/ = 0,
                                                 int term_cols = 80,
                                                 bool force_full_logo = false) {
    namespace lv2 = cc::ui::logo_v2;

    const std::string model_line = !s.model_display_name.empty()
        ? s.model_display_name
        : s.settings_model;
    const std::string version = s.app_version.empty()
        ? std::string("0.0.0") : s.app_version;

    // Build the LogoV2Options. Defaults mirror the TS LogoV2 component's
    // initial props (no onboarding, no release-notes → condensed branch).
    lv2::LogoV2Options opts;
    opts.version              = version;
    opts.cwd                  = s.cwd;
    opts.billing_type         = s.billing_type;
    opts.agent_name           = std::nullopt;  // TODO(engine-wire): populate
    opts.model_display_name   = model_line;
    opts.username             = s.user_display_name.empty()
        ? std::nullopt
        : std::make_optional(s.user_display_name);
    opts.org_name             = std::nullopt;
    opts.is_condensed_mode    = !force_full_logo;   // TS early-return gate
    opts.show_sandbox_status  = false;               // TODO(engine-wire)
    opts.show_guest_passes    = false;               // TODO(engine-wire)
    opts.show_overage_credit  = false;               // TODO(engine-wire)
    opts.is_debug_mode        = false;               // TODO(engine-wire)
    opts.tmux_session         = std::nullopt;        // TODO(engine-wire)
    opts.company_announcement = std::nullopt;        // TODO(engine-wire)
    opts.emergency_tip        = std::nullopt;        // TODO(engine-wire)
    // StatusNotices: 6 TS definitions (memory/agent/subscriber/apikey/both/
    // jetbrains). All stubs inactive until engine wiring provides data.
    opts.status_notices       = {};

    // When force_full_logo is set and term_cols >= 70 (horizontal threshold),
    // build the default [RecentActivity, What'sNew] feed pair (matches the TS
    // default branch of the 4-armed ternary in LogoV2.tsx L421). Callers that
    // wire real engine data (session storage / changelog parser) can override
    // by passing pre-populated FeedConfigs.
    std::vector<lv2::FeedConfig> feeds;
    if (force_full_logo) {
      {
        lv2::FeedConfig recent;
        recent.title = "Recent activity";
        if (s.recent_activity_lines.empty()) {
          recent.empty_message =
              "No recent conversations — start a new chat above";
        } else {
          recent.lines.reserve(s.recent_activity_lines.size());
          for (const auto& a : s.recent_activity_lines) {
            recent.lines.push_back(lv2::FeedLine{
                /*text=*/std::string(a),
                /*timestamp=*/std::nullopt});
          }
        }
        feeds.push_back(std::move(recent));
      }
      {
        lv2::FeedConfig whats_new;
        whats_new.title = "What's new";
        if (s.changelog_lines.empty()) {
          whats_new.lines = {
            lv2::FeedLine{/*text=*/"Paste images into the prompt with Ctrl+V",
                          /*timestamp=*/std::nullopt},
            lv2::FeedLine{/*text=*/"3-mode logo: condensed / compact / horizontal",
                          /*timestamp=*/std::nullopt},
            lv2::FeedLine{/*text=*/"Bash sandboxing via /sandbox toggle",
                          /*timestamp=*/std::nullopt},
          };
        } else {
          whats_new.lines.reserve(s.changelog_lines.size());
          for (const auto& c : s.changelog_lines) {
            whats_new.lines.push_back(lv2::FeedLine{
                /*text=*/std::string(c), /*timestamp=*/std::nullopt});
          }
        }
        whats_new.footer = "Full changelog at /changelog";
        feeds.push_back(std::move(whats_new));
      }
    }

    return lv2::render_logo_v2(opts, term_cols, std::move(feeds));
  }

// UI2: prompt input shell.  Full feature parity in prompt_input_full.cppm.
//
// M3 — WIRED TO THE REAL COMPONENT.  Previously this was a ~90-line
// hand-rolled body that IGNORED the real ui::components::TextInputImpl
// (cursor tracking, selection, vim modes, autocomplete, multiline, masking)
// and was flagged 0/25 faithful by the 1:1 audit.  We now delegate the
// caret/multiline/selection painting to a TextInputImpl that is SYNCED from
// ReplScreenState each render — mirroring TS BaseTextInput.tsx's
// useDeclaredCursor (which parks the real terminal cursor at the insertion
// point and lets screen readers follow the input).
//
// The pure-function signature `Element RenderPromptInput(const
// ReplScreenState&)` is PRESERVED so app.cppm's input handling (which writes
// s.input_text / s.input_mode / s.autocomplete_* between frames and forwards
// keystrokes via ReplScreen's CatchEvent) is untouched.  The TextInputImpl
// is used purely as a render primitive here — it is rebuilt per-frame from
// the projection, never as the interactive event target.
//
// Rendered faithful to TS BaseTextInput.tsx:
//   * TS prompt glyph figures.pointer "❯" (green) for normal mode,
//     "!" (red) for bash, "❮" (yellow/magenta) for vim Normal/Visual —
//     driven by s.input_mode.
//   * DECLARED CARET at the insertion point: TextInputImpl.Render() draws an
//     inverted glyph at the cursor offset (TS parks the real terminal cursor
//     there via useDeclaredCursor; we render a visible caret that lands on
//     the same byte offset).  Multi-line content lays out as a vbox.
//   * Contextual placeholder when empty (TS renderPlaceholder), styled dim.
//   * Selection highlight (TS HighlightedInput path) — provided by the real
//     impl when a selection range is set.
//   * Vim-mode badge (-- INSERT -- / -- NORMAL -- / -- VISUAL --) like TS,
//     driven by the existing vim_input::mode_display() helper.
//   * Prompt chrome uses top and bottom horizontal rules, matching TS
//     borderStyle="round" with left/right borders disabled.
[[nodiscard]] inline bool is_utf8_continuation_byte(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

[[nodiscard]] inline std::size_t clamp_input_cursor(
    const std::string& text,
    std::size_t pos) {
    if (pos == std::string::npos || pos > text.size()) return text.size();
    while (pos > 0 &&
           pos < text.size() &&
           is_utf8_continuation_byte(static_cast<unsigned char>(text[pos]))) {
        --pos;
    }
    return pos;
}

[[nodiscard]] inline std::size_t input_cursor_or_end(
    const ReplScreenState& s) {
    return clamp_input_cursor(s.input_text, s.input_cursor);
}

[[nodiscard]] inline std::size_t previous_utf8_boundary(
    const std::string& text,
    std::size_t pos) {
    pos = clamp_input_cursor(text, pos);
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 &&
           is_utf8_continuation_byte(static_cast<unsigned char>(text[pos]))) {
        --pos;
    }
    return pos;
}

[[nodiscard]] inline std::size_t next_utf8_boundary(
    const std::string& text,
    std::size_t pos) {
    pos = clamp_input_cursor(text, pos);
    if (pos >= text.size()) return text.size();
    ++pos;
    while (pos < text.size() &&
           is_utf8_continuation_byte(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
    return pos;
}

inline void set_prompt_input_text(
    const std::shared_ptr<ReplScreenState>& state,
    std::string value,
    std::size_t cursor) {
    state->input_text = std::move(value);
    state->input_cursor = clamp_input_cursor(state->input_text, cursor);
    state->is_prompt_input_active = true;
    state->last_keystroke = std::chrono::steady_clock::now();
}

inline void insert_prompt_text(
    const std::shared_ptr<ReplScreenState>& state,
    std::string_view value) {
    auto cursor = input_cursor_or_end(*state);
    state->input_text.insert(cursor, value);
    set_prompt_input_text(state, std::move(state->input_text), cursor + value.size());
}

// AT-09: drain inbound IDE at_mentioned tokens staged by the MCP receive
// thread. MUST be called on the render thread (it mutates input_text/cursor).
// Returns the number of tokens inserted. Empty under the lock fast-path when
// nothing is pending so per-frame cost is negligible (keeps app.cppm thin).
inline std::size_t DrainPendingAtMentionInserts(
    const std::shared_ptr<ReplScreenState>& state) {
    std::vector<std::string> batch;
    {
        std::lock_guard<std::mutex> lk(state->pending_at_mention_mutex);
        if (state->pending_at_mention_inserts.empty()) return 0;
        batch.swap(state->pending_at_mention_inserts);
    }
    for (const auto& token : batch) {
        if (!token.empty()) insert_prompt_text(state, token);
    }
    return batch.size();
}

inline bool backspace_prompt_text(const std::shared_ptr<ReplScreenState>& state) {
    auto cursor = input_cursor_or_end(*state);
    if (cursor == 0) return false;
    const auto prev = previous_utf8_boundary(state->input_text, cursor);
    state->input_text.erase(prev, cursor - prev);
    set_prompt_input_text(state, std::move(state->input_text), prev);
    return true;
}

inline bool delete_prompt_text(const std::shared_ptr<ReplScreenState>& state) {
    auto cursor = input_cursor_or_end(*state);
    if (cursor >= state->input_text.size()) return false;
    const auto next = next_utf8_boundary(state->input_text, cursor);
    state->input_text.erase(cursor, next - cursor);
    set_prompt_input_text(state, std::move(state->input_text), cursor);
    return true;
}

inline void move_prompt_cursor_left(const std::shared_ptr<ReplScreenState>& state) {
    state->input_cursor = previous_utf8_boundary(
        state->input_text,
        input_cursor_or_end(*state));
    state->is_prompt_input_active = true;
    state->last_keystroke = std::chrono::steady_clock::now();
}

inline void move_prompt_cursor_right(const std::shared_ptr<ReplScreenState>& state) {
    state->input_cursor = next_utf8_boundary(
        state->input_text,
        input_cursor_or_end(*state));
    state->is_prompt_input_active = true;
    state->last_keystroke = std::chrono::steady_clock::now();
}

[[nodiscard]] inline Element RenderPromptInput(const ReplScreenState& s,
                                                  int term_cols) {
    namespace uic   = ::ui::components;
    namespace vim   = cc::ui::prompt::vim_input;
    namespace figs  = cc::ui::design::figures;

    // --- 1. Prompt glyph + accent colour (TS faithfulness, unified) ---------
    //
    // REFERENCE (TS files):
    //   PromptInputModeIndicator.tsx + inputModes.ts + theme.ts
    //
    // SEMANTICS (simplified from TS — the CPP InputMode enum is kept
    // intact for backward compat with autocomplete gates in app.cppm,
    // but the PREFIX GLYPH COLLAPSES to exactly TWO visual variants per TS:
    //
    //   VARIANT A  — mode == Bash:        glyph = kBashGlyph   "!"
    //                                        color = bashBorder  rgb(255,0,135)
    //   VARIANT B  — ALL OTHER modes:     glyph = kPointer     "❯"
    //                                        color = palette.text (white)
    //                 OR: if a teammate/agent conversation is active and the
    //                     engine supplies `teammate_prefix_color`, use that
    //                     instead of palette.text (TS AGENT_COLOR_TO_THEME_COLOR).
    //
    // The old CPP-only per-mode glyphs (Slash "/", History "?", Plan "▣",
    // VimNormal "❮", VimVisual "❮", Permission "!", Task "*") are ELIMINATED
    // from the prefix position per TS:
    //   - Slash / History are routing semantics, not visual glyphs — TS's
    //     PromptInputModeIndicator falls through to ❯ even for those.
    //   - Vim mode is shown as a SEPARATE badge below the prefix (the
    //     "-- INSERT --" / "-- NORMAL --" row rendered later in this fn).
    //   - Plan mode is shown as a badge in the footer (StatusLine) or as a
    //     bubble marker, never as a replacement prefix glyph.
    //   - OrphanedPermission / TaskNotification fall through to the default
    //     "❯" pointer per TS.
    //
    // Fetch the currently active palette via the theme provider (TS ThemeContext
    // equivalent).  This respects ThemeVariant::Dark / Light / Daltonized /
    // Monochrome plus the force_monochrome a11y flag.  theme::current_theme()
    // is a cheap value copy (2 pointers + 3 booleans) with a short mutex grab.
    namespace thm = cc::ui::design::theme;
    namespace tok = cc::ui::design::tokens;
    const tok::Palette& pal = *thm::current_theme().palette;
    const bool is_bash_mode = (s.input_mode == InputMode::Bash);
    std::string prefix_str;     // passed into TextInputOptions.prefix;
    Color       prefix_color;   // applied to the prefix inside renderInputArea.

    // Step 1a: pick glyph.
    prefix_str += is_bash_mode
        ? std::string(figs::kBashGlyph)
        : std::string(figs::kPointer);
    prefix_str += " ";   // trailing NBSP/space — 2 display cells total (TS).

    // Step 1b: pick color.
    //
    // Bash mode always uses bashBorder (TS: dark rgb(255,0,135), daltonized blue
    // variants, light same).  All other modes: use the teammate color if the
    // engine has supplied one via s.teammate_prefix_color (TS
    // AGENT_COLOR_TO_THEME_COLOR map in agentColorManager.ts), otherwise fall
    // through to palette.text (dark: pure white, light: pure black).
    if (is_bash_mode) {
        prefix_color = pal.bash_border;
    } else if (s.teammate_prefix_color.has_value()) {
        prefix_color = *s.teammate_prefix_color;
    } else {
        prefix_color = pal.text;
    }
    // (Prefix rendering happens INSIDE TextInputImpl via opts.prefix — see
    // below — so cursor_display_col returns correct values automatically
    // and the declared cursor lands at the right screen column.)

    // --- 2. Sync a TextInputImpl from the projection --------------------
    uic::TextInputOptions opts;
    opts.placeholder  = s.input_placeholder;
    opts.prefix       = std::move(prefix_str);   // ← RENDERED INSIDE now (BUG-2 fix)
    opts.prefix_color = prefix_color;            // ← new field: explicit color for prefix
    opts.multiline    = true;
    opts.show_line_numbers = false;
    opts.enable_undo_redo   = false;
    opts.cursor_blink_ms    = 0;   // deterministic snapshot (no flicker)
    opts.show_history       = false;
    opts.argument_hint      = s.pending_argument_hint;  // SL-03
    opts.inline_ghost_text = s.pending_ghost_text;      // SL-05
    auto impl = std::make_shared<uic::TextInputImpl>(std::move(opts));
    impl->set_text(s.input_text);  // parks cursor at end of buffer
    const auto cursor = input_cursor_or_end(s);
    const auto end = s.input_text.size();
    if (cursor < end) {
        impl->move_cursor(
            -static_cast<int>(end - cursor),
            /*extend_selection=*/false);
    }

    // --- 3. Render the input area from the REAL component ---------------
    Element input_area = impl->RenderInputAreaPub();

    // --- 3b. Declared cursor (IME / accessibility) ----------------------
    // Faithful port of TS useDeclaredCursor: park the real terminal cursor at
    // the insertion point so IME preedit renders inline and screen readers /
    // magnifiers can follow the input.
    //
    // NOTE on cursor-display math (BUG-2 FIXED):
    //   We previously rendered the prefix OUTSIDE TextInputImpl in a separate
    //   hbox, but left opts.prefix empty.  declared_cursor then used
    //   `string_width(opts.prefix) = 0` as the prefix width, so the native
    //   cursor parked 3-4 display columns to the LEFT of actual text start.
    //
    //   After this commit: opts.prefix contains the rendered 2-cell glyph,
    //   impl->cursor_display_col() includes prefix width in its return, and
    //   declared_cursor below positions the terminal cursor EXACTLY over the
    //   character where the next keystroke will insert.  No arithmetic tricks
    //   are required — TextInputImpl's own prefix logic (see RenderInputArea
    //   inside text_input.cppm) already accounts for it.
    //
    //   We ALSO add 1 column for the left-side " " padding space rendered in
    //   hbox #5 (the `text(" ")` before the text area hbox row — this is a
    //   pure layout margin that TextInputImpl does NOT know about so we
    //   account for it here manually).
    {
        // Native terminal cursor parks at the screen bottom-right (Hidden) via
        // the root CursorResetNode in app.cppm.  We intentionally do NOT declare
        // the prompt caret position here: FTXUI's ScreenInteractive emits a
        // cursor-MOVE sequence every frame (from bottom-right to the declared
        // position) even when nothing else changed, and many terminals render
        // those hidden-cursor moves as visible flicker during the ~20Hz idle
        // re-render.  Leaving the cursor at bottom-right makes the move delta
        // zero, so FTXUI emits no move and the idle frame is flicker-free.
        // The visible caret is still drawn by TextInputImpl (inverted glyph), so
        // the user sees their caret; only the hidden native cursor (IME/a11y
        // anchor) parks at the corner instead of over the caret.
    }

    Elements box_body;

    // --- 4. Vim-mode badge (-- INSERT -- / -- NORMAL -- / -- VISUAL --) -
    // Faithful to TS: drawn as a separate row (NOT a prefix glyph swap),
    // dim+bold, per-mode color (see vim_input.cppm mode_display).
    std::optional<std::pair<std::string, Color>> vim_badge;
    if (s.input_mode == InputMode::VimInsert)
        vim_badge = {"-- INSERT --", vim::mode_display(vim::VimMode::Insert).second};
    else if (s.input_mode == InputMode::VimNormal)
        vim_badge = {"-- NORMAL --", vim::mode_display(vim::VimMode::Normal).second};
    else if (s.input_mode == InputMode::VimVisual)
        vim_badge = {"-- VISUAL --", vim::mode_display(vim::VimMode::Visual).second};
    if (vim_badge) {
        box_body.push_back(hbox({
            text("  "),
            text(vim_badge->first) | color(vim_badge->second) | bold | dim,
        }));
    }

    // --- 5. Compose -----------------------------------------------------
    // Per TS layout: a leading space (`text(" ")`) followed by the
    // TextInputImpl's rendered output (which itself is `prefix_glyph + space
    // + text`).  The leading space was originally introduced so the glyph
    // doesn't hug the left edge; we keep it for visual parity.
    box_body.push_back(hbox({
        text(" "),
        input_area,
    }));

    auto content = vbox(std::move(box_body));

    // TS: Box with borderStyle="round", borderLeft={false}, borderRight={false}.
    // Ink renders this as TWO plain horizontal rules with no side borders and
    // no corners.  Color = theme.promptBorder (TS dark: ansi:whiteBright ≈
    // rgb(136,136,136); light: ansi:whiteBright ≈ rgb(153,153,153)).  Use the
    // palette value we just added instead of the inline hardcoded 153,153,153
    // so daltonized/monochrome/light variants get the correct frame color.
    const Color frame_color = pal.prompt_border;

    Element top_rule    = separator() | color(frame_color);
    Element bottom_rule = separator() | color(frame_color);

    return vbox({
        std::move(top_rule),
        content,
        std::move(bottom_rule),
    }) | size(WIDTH, EQUAL, std::max(term_cols, 40));
}

[[nodiscard]] inline std::string pad_to_columns(std::string text, int width) {
    const int pad = width - string_width(text);
    if (pad > 0) text.append(static_cast<std::size_t>(pad), ' ');
    return text;
}

[[nodiscard]] inline Element RenderPromptSuggestions(const ReplScreenState& s,
                                                     int term_cols) {
    if (s.autocomplete_suggestions.empty()) return Element{};

    constexpr int kMaxVisibleItems = 5;
    const int total = static_cast<int>(s.autocomplete_suggestions.size());
    const int selected = std::clamp(
        s.autocomplete_index < 0 ? 0 : s.autocomplete_index,
        0,
        total - 1);
    const int visible = std::min(kMaxVisibleItems, total);
    const int start = std::max(
        0,
        std::min(selected - visible / 2, total - visible));
    const int end = std::min(start + visible, total);

    int widest = 0;
    if (s.autocomplete_stable_name_width > 0) {
        // INF-03: stable precomputed width (slash-command path) — no jitter.
        widest = s.autocomplete_stable_name_width;
    } else {
        for (int i = start; i < end; ++i) {
            widest = std::max(
                widest,
                string_width(s.autocomplete_suggestions[static_cast<std::size_t>(i)].display_text));
        }
    }
    const int max_name_width = std::max(10, term_cols * 2 / 5);
    const int name_width = std::min(widest + 5, max_name_width);
    const int desc_width = std::max(0, term_cols - name_width - 4);

    Elements rows;
    rows.reserve(static_cast<std::size_t>(visible));
    for (int i = start; i < end; ++i) {
        const auto& item = s.autocomplete_suggestions[static_cast<std::size_t>(i)];
        const bool is_selected = i == selected;
        auto display = truncate_columns(item.display_text, name_width - 2);
        auto desc = truncate_columns(item.description, desc_width);
        Element name = text(pad_to_columns(std::move(display), name_width));
        Element detail = text(std::move(desc));
        if (is_selected) {
            name = name | color(Color::Cyan) | bold;
            detail = detail | color(Color::Cyan);
        } else {
            name = name | dim;
            detail = detail | dim;
        }
        rows.push_back(hbox({
            text("  "),
            std::move(name),
            std::move(detail),
            filler(),
        }));
    }

    return vbox(std::move(rows));
}

[[nodiscard]] inline std::optional<std::string> accept_selected_prompt_suggestion(
    const std::shared_ptr<ReplScreenState>& state) {
    if (state->autocomplete_suggestions.empty()) return std::nullopt;

    const int total = static_cast<int>(state->autocomplete_suggestions.size());
    const int selected = std::clamp(
        state->autocomplete_index < 0 ? 0 : state->autocomplete_index,
        0,
        total - 1);
    std::string accepted =
        state->autocomplete_suggestions[static_cast<std::size_t>(selected)].insert_text;
    if (accepted.empty()) {
        accepted =
            state->autocomplete_suggestions[static_cast<std::size_t>(selected)].display_text;
    }

    const auto& suggestion =
        state->autocomplete_suggestions[static_cast<std::size_t>(selected)];
    std::size_t start = suggestion.replacement_start;
    std::size_t end = suggestion.replacement_end;
    if (start == std::string::npos || end == std::string::npos ||
        start > end || end > state->input_text.size()) {
        start = 0;
        end = state->input_text.size();
    }

    state->input_text.replace(start, end - start, accepted);
    state->input_cursor = clamp_input_cursor(state->input_text, start + accepted.size());
    state->autocomplete_suggestions.clear();
    state->autocomplete_index = -1;
    state->is_prompt_input_active = true;
    state->last_keystroke = std::chrono::steady_clock::now();
    return state->input_text;
}

// =========================================================
// M7: Queue-based dialog rendering
// =========================================================
//
// The legacy dialog routing system (ReplMode → RouteDialog → dialog_stubs
// SimpleDialog builders) was REMOVED in M7 Task #124.
//
// ALL dialogs now flow through the DialogQueue:
//   - Engine side calls PushXxx() triggers in app.cppm / query_engine.cppm
//     (there are ZERO writes to a "DialogContext" bridge struct anywhere).
//   - Engine sets ReplMode for some legacy mode-aware UI chrome (mode opts,
//     vim indicator, etc.) but no longer uses it to determine dialog content.
//   - SyncReplModeToQueue was also REMOVED — audit confirmed there are ZERO
//     ReplMode→PushXxx() gaps in the engine call path; every engine-side
//     ReplMode assignment is paired with a matching DialogQueue push.
//
// Render path for queue:
//   RenderStandaloneDialog() (highest — fullscreen takeover)
//   RenderModalDialog()       (panel overlay — /settings, /tasks...)
//   RenderOverlayDialog()     (floating ToolPermission in scroll area)
//   RenderBottomDialog()      (prompt-affixed focused input dialogs)
//
// Event path:
//   HandleDialogQueueEvent() dispatches in priority order:
//     standalone > modal > overlay > bottom
//   with appropriate typing/animation suppression checks.
//
// Suppression rules (mirror TS REPL.tsx):
//   - is_prompt_input_active = typing in progress → Bands 2..6 hidden
//   - is_tool_animation_active = JSX tool animation running → Band3 hidden

// M7: Queue-based dialog rendering
// =========================================================

// =========================================================
// M7 Dialog Framework: dialog_queue renderer dispatchers
// =========================================================
//
// Four slots (DialogSlot enum) in priority order:
//   Standalone > Modal > Overlay > Bottom
//
// The Render* functions return Element (wrapped in optional or
// Element{} to signal "nothing to render").  Handle* events return
// true if they consumed the event.
namespace dialog_queue_render {

namespace dsys = dsys_fw;

/// Build the render-time width/height context for the registry.
[[nodiscard]] inline dsys::DialogRenderContext MakeContext(
    int term_w = 120, int term_h = 40)
{
    dsys::DialogRenderContext c;
    c.term_cols  = term_w;
    c.term_rows  = term_h;
    return c;
}

/// Standalone dialog: full-takeover render (no chrome).
[[nodiscard]] inline Element RenderStandaloneDialog(ReplScreenState& s,
                                                    int w = 120, int h = 40) {
    auto peek = s.dialog_queue.peek_standalone_mut();
    if (!peek) return Element{};
    dsys::DialogPayloadVariant& payload = peek->get();
    if (std::holds_alternative<std::monostate>(payload)) return Element{};
    return s.dialog_renderers.render(payload, MakeContext(w, h));
}

/// Modal dialog (stack top): rendered full-width dbox above the rest.
[[nodiscard]] inline Element RenderModalDialog(ReplScreenState& s,
                                               int w = 120, int h = 40) {
    auto peek = s.dialog_queue.peek_modal_mut();
    if (!peek) return Element{};
    dsys::DialogPayloadVariant& payload = peek->get();
    if (std::holds_alternative<std::monostate>(payload)) return Element{};
    auto el = s.dialog_renderers.render(payload, MakeContext(w, h));
    if (!el) return Element{};
    return dbox({
        vbox({ filler(),
               hbox({ filler(), el, filler() }) | flex_shrink,
               filler() }) | flex,
    });
}

/// Overlay dialog (ToolPermission, Band3): centered floating dbox
/// clamped to 3/5 of terminal height so it never blocks the prompt.
/// Suppressed when prompt has input active or when allow_animation
/// dialogs are disabled (mid-tool-animation).
[[nodiscard]] inline Element RenderOverlayDialog(
    ReplScreenState& s,
    bool is_prompt_input_active,
    bool allow_dialogs_with_animation = true,
    int w = 120, int h = 40)
{
    auto peek = s.dialog_queue.peek_overlay_mut();
    if (!peek) return Element{};
    dsys::DialogPayloadVariant& payload = peek->get();
    if (std::holds_alternative<std::monostate>(payload)) return Element{};
    if (!dsys::should_show_dialog(payload, is_prompt_input_active,
                                   allow_dialogs_with_animation)) {
        return Element{};
    }
    auto el = s.dialog_renderers.render(payload, MakeContext(w, h));
    if (!el) return Element{};
    // Size clamp: 60% of rows max.
    const int max_h = std::max(12, h * 3 / 5);
    el = std::move(el) | size(HEIGHT, LESS_THAN, max_h);
    // Inject inside a centered dbox above the messages area.
    return dbox({
        vbox({ filler(),
               hbox({ filler(), std::move(el) | flex_shrink, filler() })
                   | flex_shrink,
               filler() }) | flex,
    });
}

/// Bottom slot: banner-style dialogs pushed to the bottom of the screen.
[[nodiscard]] inline Element RenderBottomDialog(
    ReplScreenState& s,
    bool is_prompt_input_active,
    bool allow_dialogs_with_animation,
    int w = 120)
{
    (void)w;
    auto peek = s.dialog_queue.peek_bottom_mut(is_prompt_input_active,
                                               allow_dialogs_with_animation);
    if (!peek) return Element{};
    dsys::DialogPayloadVariant& payload = peek->get();
    if (std::holds_alternative<std::monostate>(payload)) return Element{};
    if (!dsys::should_show_dialog(payload, is_prompt_input_active,
                                   allow_dialogs_with_animation)) {
        return Element{};
    }
    auto el = s.dialog_renderers.render(payload, MakeContext(w, 40));
    if (!el) return Element{};
    return std::move(el) | size(WIDTH, EQUAL, w);
}

// ── Event dispatch: priority Standalone > Modal > Overlay > Bottom ──
inline bool DispatchDialogQueueEvents(ReplScreenState& s,
                                      const ftxui::Event& ev,
                                      bool is_prompt_input_active,
                                      bool allow_dialogs_with_animation) {
    namespace dsys = dsys_fw;

    // Standalone always takes every event.
    {
        auto peek = s.dialog_queue.peek_standalone_mut();
        if (peek) {
            dsys::DialogPayloadVariant& payload = peek->get();
            if (!std::holds_alternative<std::monostate>(payload)) {
                if (s.dialog_renderers.handle_event(payload, ev)) return true;
                // Standalone Escape fallback — closes as Abort.
                if (ev == ftxui::Event::Escape) {
                    s.dialog_queue.pop_standalone();
                    return true;
                }
            }
        }
    }

    // Modal stack top.
    {
        auto peek = s.dialog_queue.peek_modal_mut();
        if (peek) {
            dsys::DialogPayloadVariant& payload = peek->get();
            if (!std::holds_alternative<std::monostate>(payload)) {
                if (s.dialog_renderers.handle_event(payload, ev)) return true;
                if (ev == ftxui::Event::Escape) {
                    s.dialog_queue.pop_modal();
                    return true;
                }
            }
        }
    }
    // Overlay (Band3).  Skip when suppressed so typing can continue.
    {
        auto peek = s.dialog_queue.peek_overlay_mut();
        if (peek) {
            dsys::DialogPayloadVariant& payload = peek->get();
            if (!std::holds_alternative<std::monostate>(payload)) {
                if (dsys::should_show_dialog(payload, is_prompt_input_active,
                                             allow_dialogs_with_animation)) {
                    if (s.dialog_renderers.handle_event(payload, ev)) return true;
                }
            }
        }
    }

    // Bottom.
    {
        auto peek = s.dialog_queue.peek_bottom_mut(is_prompt_input_active,
                                                   allow_dialogs_with_animation);
        if (peek) {
            dsys::DialogPayloadVariant& payload = peek->get();
            if (!std::holds_alternative<std::monostate>(payload)) {
                if (dsys::should_show_dialog(payload, is_prompt_input_active,
                                             allow_dialogs_with_animation)) {
                    if (s.dialog_renderers.handle_event(payload, ev)) return true;
                }
            }
        }
    }
    return false;
}

/// Convenience: combine Overlay + Modal + Bottom into a single dbox
/// that callers can overlay onto the base chrome.  Standalone is handled
/// separately (full-takeover, replaces the entire render).
[[nodiscard]] inline Element LayerAllDialogs(
    Element base_chrome,
    ReplScreenState& s,
    bool is_prompt_input_active,
    bool allow_dialogs_with_animation,
    int w = 120, int h = 40)
{
    Elements layers;
    layers.push_back(std::move(base_chrome));
    Element bottom = RenderBottomDialog(
        s, is_prompt_input_active, allow_dialogs_with_animation, w);
    if (bottom) {
        layers.push_back(vbox({
            filler(),
            std::move(bottom),
        }) | flex);
    }
    Element overlay = RenderOverlayDialog(
        s, is_prompt_input_active, allow_dialogs_with_animation, w, h);
    if (overlay) layers.push_back(std::move(overlay));
    Element modal = RenderModalDialog(s, w, h);
    if (modal)   layers.push_back(std::move(modal));
    if (layers.size() == 1) return layers.front();
    return dbox(std::move(layers));
}

} // namespace dialog_queue_render

/// Top-level dialog router: ReplMode -> overlay Element.
/// Panel modes (Normal / Tasks / Teams / Help / QuickOpen) return nullopt.
/// Priority matches TS getFocusedInputDialog() (REPL.tsx:2013):
///   Exit > message-selector > sandbox > permissions/hook/elicit >
///   cost/idle/ultraplan > onboarding > recs > panels.
[[nodiscard]] inline std::optional<Element> RouteDialog(
    ReplMode m, const ReplScreenState& s) {
    (void)s;
    // M7: All legacy ReplMode dialog routing is retired.  Dialogs now flow
    // exclusively via the 4-slot DialogQueue with priority
    // Standalone > Modal > Overlay > Bottom.  Any remaining ReplMode-driven
    // chrome (IdleReturn return-from-idle prompt, onboarding, etc.) is
    // rendered by the owning slot in dialog_queue_render.  This stub
    // preserves the file's historical RouteDialog() API so downstream
    // callers (RenderReplScreen) don't need another rewrite.
    switch (m) {
      default: break; }
    return std::nullopt;
}

// =========================================================
// Full layout composition
// =========================================================

/// M1: Composed via the FullscreenLayout slot-system (faithful port of TS
/// FullscreenLayout.tsx).  The previously-flat top-to-bottom vbox is now
/// slotted:
///   scrollable slot (flexGrow region) =
///     [WelcomeHeader (fresh session)] | Messages | Spinner | Tasks/Teams
///   bottom slot (pinned, flexShrink=0) =
    ///     PromptInput | Footer
///   modal slot (dbox overlay, bottom-anchored) =
///     RouteDialog() when non-null (with MODAL_TRANSCRIPT_PEEK peek)
///   overlay slot =
///     (reserved — engine wires PermissionRequest here in a future milestone;
///      currently permission flows through RouteDialog as the modal slot,
///      matching how it rendered before.)
///
    /// All existing render functions are PRESERVED (RenderWelcomeHeader,
    /// RenderMessages, RenderSpinner, RenderPromptInput,
/// RouteDialog) — only HOW they are composed changed.  Visual order is
/// preserved: messages scroll above, status/prompt pinned below.
///
/// Fix #1: welcome header atop the list on a fresh session.
    /// Fix #7: StatusLine lives inside the prompt footer, matching TS.
/// Fix #11: terminal size probed once per frame for adaptive clamping.
[[nodiscard]] inline Element RenderReplScreen(ReplScreenState& s) {
    // Probe terminal size once per frame for adaptive layout (fix #11).
    auto [term_cols, term_rows] = cc::ui::ink_utils::query_terminal_size();
    if (term_cols <= 0) term_cols = 80;
    if (term_rows <= 0) term_rows = 24;
    s.viewport_height_lines = std::max(1, term_rows - 5);

    // Spinner frame tick: monotonically increments per render call so the
    // tool-use header spinner animates.  (The interactive ToolUseMessage
    // component also drives its own counter; this feeds the static render
    // path used by message_list row dispatch.)
    static int spinner_frame = 0;
    ++spinner_frame;

    namespace fl = cc::ui::layout::fullscreen;
    fl::FullscreenLayoutSlots slots;
    slots.term_cols = term_cols;
    slots.term_rows = term_rows;

    // ── scrollable slot (flexGrow region) ───────────────────────────────
    // Builds the same top→bottom order the old flat vbox had for the
    // message transcript area: welcome header (fresh session), messages.
    // Spinner is NOT a scroll row — it lives in the pinned chrome between
    // the messages list and the prompt input (TS BriefSpinner marginTop=1).
    Elements L;
    Elements scroll_rows; scroll_rows.reserve(4);
    const auto visible_messages = BuildVisibleMessages(s);
    // ── Pinned Logo header (R2 fix — TS Messages.tsx:679) ─────────────
    // Faithful to TS Messages.tsx line 679: LogoHeader is rendered OUTSIDE
    // the VirtualMessageList — scrolling messages never scroll it out of
    // view (IMG#18 fix).  We therefore place it in the new `slots.header`
    // pinned slot (drawn above scrollwrap) rather than inside scroll_rows.
    //
    // Height is pinned to HEIGHT EQUAL so nested yframe/flex never compresses
    // the strip to 0 rows, and pinned width fills TERM_COLS so the strip has
    // correct horizontal anchor.
    slots.header = RenderWelcomeHeader(s, spinner_frame, term_cols)
                 | size(HEIGHT, EQUAL, 4)
                 | size(WIDTH,  EQUAL, term_cols);
    scroll_rows.push_back(RenderMessages(visible_messages, s.selected_message_idx,
                                         s.viewport_height_lines, s.scroll_offset,
                                         s.scroll_pinned_to_bottom, spinner_frame,
                                         s.unseen_divider));
    // Spinner lives in the chrome BETWEEN messages list and prompt input
    // (TS BriefSpinner marginTop=1, NOT a message row inside scroll content).
    Element spinner_chrome = text("");
    if (s.spinner_mode != SpinnerMode::Hidden)
        spinner_chrome = RenderSpinner(s.spinner_mode, s.spinner_verb,
                                       s.spinner_tip, spinner_frame);
    // M7.5: Panel views (Tasks/Teams/Help/Settings/About/QuickOpen) are
    // now rendered as modal dialogs via DialogQueue — no longer inlined
    // in the scrollable slot.
    slots.scrollable = vbox(std::move(scroll_rows));

    if (!s.active_local_jsx_command) {
        // ── bottom slot (pinned, flexShrink=0) ──────────────────────────────
        // Chrome order: [spinner (marginTop=1)] → [suggestions overlay?] →
        //               [prompt input] → [footer]
        //
        // Faithful to TS PromptInputFooter structure:
        //   suggestions overlay?  →  prompt input  →  footer (left/right columns)
        //
        // The footer contains StatusLine (optional, user-configurable) +
        // PromptInputFooterLeftSide (mode indicator, tasks, teams, hints) on
        // the left, and bridge/notifications on the right.
        //
        // STABLE HEIGHT: The LeftSide row is always exactly 1 row so scroll
        // content never shifts when hints change.  StatusLine adds a row when
        // present but is conditionally shown only in prompt mode + not short.
        namespace pif = cc::ui::prompt::footer;

        // Build StatusLine options (user-configurable command-driven status).
        //
        // Faithful to TS StatusLine.tsx:
        //   - Configured by settings.statusLine, rendered only in prompt mode
        //     and hidden in short fullscreen layouts
        //   - content comes from executing the user's shell command
        //   - In fullscreen, reserves a row even while loading (stable height)
        //   - Text may contain ANSI escape codes for coloring
        const bool is_fullscreen = cc::utils::is_fullscreen_enabled();
        const bool is_short = is_fullscreen && term_rows < 24;
        const bool status_line_configured =
            s.status_line_enabled && !s.status_line_command.empty();
        pif::StatusLineOptions status_line_opts;
        status_line_opts.content = s.status_line_text;
        status_line_opts.should_display =
            status_line_configured &&
            s.input_mode == InputMode::Prompt &&
            !is_short;
        status_line_opts.is_fullscreen = is_fullscreen;
        status_line_opts.padding_x = s.status_line_padding;

        // Map InputMode to footer PromptInputMode
        pif::PromptInputMode footer_mode = pif::PromptInputMode::Prompt;
        switch (s.input_mode) {
            case InputMode::Prompt:       footer_mode = pif::PromptInputMode::Prompt; break;
            case InputMode::Bash:         footer_mode = pif::PromptInputMode::Bash; break;
            case InputMode::SlashCommand: footer_mode = pif::PromptInputMode::SlashCommand; break;
            case InputMode::HistorySearch:footer_mode = pif::PromptInputMode::HistorySearch; break;
            case InputMode::PlanMode:     footer_mode = pif::PromptInputMode::PlanMode; break;
            default: break;
        }
        // ── Assemble the bottom slot ──
        L.reserve(4);
        if (s.spinner_mode != SpinnerMode::Hidden) {
            L.push_back(hbox({spinner_chrome, filler()}) | flex_shrink);
        }
        if (!s.autocomplete_suggestions.empty()) {
            L.push_back(RenderPromptSuggestions(s, term_cols));
        }
        L.push_back(RenderPromptInput(s, term_cols));
        // PromptInputFooter: LeftSide carries mode/tasks/teams via
        // ModeIndicatorOptions; StatusLine is its own nested struct.
        pif::LeftSideOptions left_opts;
        left_opts.mode_indicator.mode                 = footer_mode;
        left_opts.mode_indicator.background_task_count = s.background_task_count;
        left_opts.mode_indicator.teammate_count        = s.teammate_count;
        left_opts.mode_indicator.teams_selected        = s.teams_footer_selected;
        if (status_line_configured) {
            left_opts.mode_indicator.show_hint = false;
        }
        pif::FooterOptions footer_opts;
        footer_opts.status_line = std::move(status_line_opts);
        footer_opts.left_side   = std::move(left_opts);
        footer_opts.is_fullscreen = is_fullscreen;
        footer_opts.is_narrow = term_cols < 80;
        // M4 faithful: outer chrome.
        //   * Clipboard image hint — stays nullopt until the engine wires up
        //     platform clipboard-image detection; no visual regression while
        //     empty.
        // NOTE: TS upstream does NOT render a brand pill in the footer
        // (PromptInputFooter.tsx has zero occurrences of "CC-REPL" /
        // "Claude Code" text).  Branding is rendered by CondensedLogo only
        // in the top header.
        L.push_back(pif::RenderPromptInputFooter(footer_opts));

        slots.bottom = vbox(std::move(L)) | flex_shrink;
    }

    // M7: Standalone slot (trust dialog, first-run onboarding) takes over
    // the entire terminal — no chrome, no prompt, no messages rendered.
    if (s.dialog_queue.has_standalone()) {
        return dialog_queue_render::RenderStandaloneDialog(s, 120, 40);
    }

    // Legacy RouteDialog path — only used when dialog_queue has no
    // overlay/bottom/modal slots.  Eventually this will be phased out
    // in favour of the queue for all dialogs.

    // ── M1 FullscreenLayout: 3-state sticky prompt chrome ─────────────
    // TS REF: FullscreenLayout.tsx lines 339-351 (3-state discriminant,
    //        padCollapsed resolution, headerPrompt guard).
    slots.sticky_prompt         = s.sticky_prompt;
    slots.sticky_clicked        = s.sticky_prompt_clicked;
    slots.hide_sticky           = false;
    slots.pill_visible          = s.pill_visible;
    slots.hide_pill             = false;
    slots.new_message_count     = s.unseen_message_count;

    // on_sticky_click: the TS pattern "onClick={headerPrompt.scrollTo}"
    // (line 344) sets stickyPrompt='clicked' (the literal sentinel) via a
    // stable setState that reacts before scrollTo side-effects fire.  We
    // match that order in C++: (1) flip sticky_prompt_clicked to hide the
    // header + keep padCollapsed=true; (2) compute the delta between the
    // prompt's visual line and current scroll_top and ask ScrollTranscript
    // to jump there.
    //
    // Captures: `&s` is a ReplScreenState& whose lifetime is bound to the
    // outer `std::shared_ptr<ReplScreenState>` in MakeReplScreen; it is
    // stable across renders.  The callback is only invoked from within
    // FTXUI event dispatch (same thread), so no data races.
    slots.on_sticky_click = [&s](const fl::StickyPrompt& sp) {
        s.sticky_prompt_clicked = true;
        // Jump so the target visual line is at the TOP of the viewport.
        // scroll_target_row is measured from scroll_top=0 (content
        // coordinates); ScrollTranscript(delta) is relative — so delta =
        // target - current.  Clamp against viewport_rows to avoid
        // overshooting below min-scroll.
        int current = std::max(0, s.scroll_offset);
        int delta   = static_cast<int>(sp.scroll_target_row) - current;
        if (delta != 0) {
            // We need to call ScrollTranscript which takes
            // shared_ptr<ReplScreenState>.  Here we only have a bare ref;
            // but this callback is dispatched from FTXUI's event loop from
            // within the outer MakeReplScreen Component's OnEvent chain
            // which owns the shared_ptr.  For a purely visual change
            // (clicking the header is a scroll, not engine-state mutation),
            // a direct offset mutation achieves the same effect without
            // requiring the shared_ptr here.
            int viewport = std::max(1, s.viewport_height_lines);
            int total;
            if (s.virtual_list_active) {
                namespace vl = cc::ui::messages::virtual_list;
                total = s.virtual_jh.total();
            } else {
                const auto vm = BuildVisibleMessages(s);
                total = EstimateTranscriptRows(vm);
            }
            int max_top = std::max(0, total - viewport);
            int old_top = std::clamp(current, 0, max_top);
            int target  = std::clamp(old_top + delta, 0, max_top);
            if (target != old_top) {
                s.scroll_offset = target;
                s.scroll_pinned_to_bottom = (target >= max_top);
                if (s.virtual_list_state) {
                    namespace vl = cc::ui::messages::virtual_list;
                    s.virtual_list_state->scroll_top = target;
                    vl::update_sticky_after_scroll(*s.virtual_list_state,
                                                    old_top);
                }
            }
        }
        // TS note: after the click, stickyPrompt stays at 'clicked' until
        // the NEXT scroll event re-emits a fresh {text,scrollTo} from
        // StickyTracker.  Any movement (wheel, PageUp, click-to-select)
        // that moves the viewport will write a new sticky_prompt and
        // clear sticky_prompt_clicked.  We therefore do NOT clear the
        // flag ourselves here.
    };

    // on_pill_click: TS lines 371-381 — clicking the "N new messages" pill
    // re-pins to the bottom.  Same lifetime reasoning as on_sticky_click.
    slots.on_pill_click = [&s] {
        int viewport = std::max(1, s.viewport_height_lines);
        int total;
        if (s.virtual_list_active) {
            namespace vl = cc::ui::messages::virtual_list;
            total = s.virtual_jh.total();
        } else {
            const auto vm = BuildVisibleMessages(s);
            total = EstimateTranscriptRows(vm);
        }
        int max_top = std::max(0, total - viewport);
        int old_top = std::clamp(s.scroll_offset, 0, max_top);
        if (max_top != old_top) {
            s.scroll_offset = max_top;
            s.scroll_pinned_to_bottom = true;
            if (s.virtual_list_state) {
                namespace vl = cc::ui::messages::virtual_list;
                s.virtual_list_state->scroll_top = max_top;
                vl::update_sticky_after_scroll(*s.virtual_list_state, old_top);
            }
            // Clear the pill + unseen count on repin (mirrors TS onRepin
            // setting dividerIndex=null — the pill only shows while
            // pill_visible=true AND a divider snapshot exists.)
            s.pill_visible = false;
            s.unseen_message_count = 0;
        }
    };

    Element base = fl::ComposeFullscreen(std::move(slots));
    auto dlg = RouteDialog(s.mode, s);
    if (dlg) base = dbox({
        std::move(base) | dim,
        vbox({ filler(),
               hbox({ filler(), std::move(*dlg) | flex_shrink, filler() })
               | flex_shrink, filler() }) | flex });

    // M7: Layer Bottom + Overlay + Modal dialogs from the dialog_queue.
    bool tool_animating = s.spinner_mode != SpinnerMode::Hidden;
    return dialog_queue_render::LayerAllDialogs(
        std::move(base), s, s.is_prompt_input_active,
        /*allow_dialogs_with_animation=*/!tool_animating, 120, 40);
}

// =========================================================
// FTXUI Component factory
// =========================================================

// UI13 agent_wizard is fully implemented and wired here: the wizard Component
// is lazily created on first entry to CreateAgent / EditAgent mode, and
// events are forwarded to it via forward_agent().
namespace dialog_router {

namespace github_wizard = cc::ui::dialogs::install_github_app_wizard;
namespace slack_wizard = cc::ui::dialogs::install_slack_app_wizard;
namespace trust_ns = cc::ui::trust_dialog;

[[nodiscard]] inline std::shared_ptr<Component> get_install_github_wizard(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    if (!s->wizard_install_github_app) {
        github_wizard::InstallGitHubAppWizardOptions opts;
        opts.on_complete = [s, cb](auto) {
            s->mode = ReplMode::Normal;
            s->wizard_install_github_app.reset();
            if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        };
        opts.on_cancel = [s, cb] {
            s->mode = ReplMode::Normal;
            s->wizard_install_github_app.reset();
            if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        };
        s->wizard_install_github_app = std::make_shared<Component>(
            github_wizard::MakeInstallGitHubAppWizard(std::move(opts)));
    }
    return std::static_pointer_cast<Component>(s->wizard_install_github_app);
}

[[nodiscard]] inline std::shared_ptr<Component> get_install_slack_wizard(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    if (!s->wizard_install_slack_app) {
        slack_wizard::InstallSlackAppWizardOptions opts;
        opts.on_complete = [s, cb] {
            s->mode = ReplMode::Normal;
            s->wizard_install_slack_app.reset();
            if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        };
        opts.on_cancel = [s, cb] {
            s->mode = ReplMode::Normal;
            s->wizard_install_slack_app.reset();
            if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        };
        s->wizard_install_slack_app = std::make_shared<Component>(
            slack_wizard::MakeInstallSlackAppWizard(std::move(opts)));
    }
    return std::static_pointer_cast<Component>(s->wizard_install_slack_app);
}

[[nodiscard]] inline Element render_install_wizard(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    auto wiz = s->mode == ReplMode::InstallGitHubApp
        ? get_install_github_wizard(s, cb)
        : get_install_slack_wizard(s, cb);
    return wiz ? (*wiz)->Render() : text("");
}

inline bool forward_install_wizard(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb,
    Event ev) {
    auto wiz = s->mode == ReplMode::InstallGitHubApp
        ? get_install_github_wizard(s, cb)
        : get_install_slack_wizard(s, cb);
    return wiz && (*wiz)->OnEvent(std::move(ev));
}

// -------------------------------------------------------------------
// Agent wizard helpers
// -------------------------------------------------------------------

namespace wizard_ns = cc::ui::agents::wizard;

using wizard_ns::AgentWizardOptions;
using wizard_ns::WizardDraft;

/// Lazily create (or re-create) the agent wizard component.
/// The mode (create vs edit) and agent_id are read from state.
[[nodiscard]] inline std::shared_ptr<Component> get_agent_wizard(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    if (!s->wizard_agent) {
        AgentWizardOptions opts;
        // M7: Read agent_id from the Standalone-slot EditAgentWizardPayload
        // in dialog_queue (instead of legacy DialogContext bridge struct).
        namespace dsys_gw = cc::ui::dialogs::system;
        auto& q_gw = s->dialog_queue;
        if (cb->load_agent_for_wizard &&
            q_gw.contains_type(dsys_gw::DialogType::EditAgentWizard)) {
            auto st = q_gw.peek_standalone();
            if (st) {
                // peek_standalone() returns optional<reference_wrapper<const V>>.
                // Unwrap with .get() so std::get_if<T> sees a const V*.
                const auto& variant = st->get();
                auto* p = std::get_if<dsys_gw::EditAgentWizardPayload>(&variant);
                if (p && !p->agent_name.empty()) {
                    opts.edit_agent = cb->load_agent_for_wizard(p->agent_name);
                }
            }
        }
        opts.on_save = [s, cb](const WizardDraft& draft) {
            if (cb->save_agent_from_wizard) cb->save_agent_from_wizard(draft);
            s->mode = ReplMode::Normal;
            if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        };
        opts.on_cancel = [s, cb] {
            s->mode = ReplMode::Normal;
            if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        };
        s->wizard_agent = std::make_shared<Component>(
            wizard_ns::AgentWizard(std::move(opts)));
    }
    return std::static_pointer_cast<Component>(s->wizard_agent);
}

/// Forward an event to the agent wizard component.
inline bool forward_agent(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb,
    Event ev) {
    auto wiz = get_agent_wizard(s, cb);
    return wiz && (*wiz)->OnEvent(std::move(ev));
}

/// Render the agent wizard content as an Element.
[[nodiscard]] inline Element render_agent_wizard(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    auto wiz = get_agent_wizard(s, cb);
    return wiz ? (*wiz)->Render() : text("");
}

/// Reset (destroy) the agent wizard so the next entry starts fresh.
inline void reset_agent_wizard(const std::shared_ptr<ReplScreenState>& s) {
    s->wizard_agent.reset();
}

// -------------------------------------------------------------------
// Agents menu helpers (UI13)
// -------------------------------------------------------------------

namespace agents_menu {

namespace cards = cc::ui::agents::cards;
namespace agent_display = cc::tools::agent_display;

struct AgentMenuOptions {
    std::vector<cards::AgentCardData> agents;
    std::function<void()> on_create_new;
    std::function<void(const std::string& id)> on_select;
    std::function<void()> on_cancel;
};

[[nodiscard]] inline bool is_built_in(const cards::AgentCardData& agent) {
    return agent.source == "built-in";
}

[[nodiscard]] inline std::string resolved_model_label(
    const cards::AgentCardData& agent) {
    if (agent.model_override && !agent.model_override->empty()) {
        return *agent.model_override;
    }
    return is_built_in(agent) ? "inherit" : "";
}

[[nodiscard]] inline std::vector<std::size_t> selectable_indices(
    const std::vector<cards::AgentCardData>& agents) {
    std::vector<std::size_t> out;
    out.reserve(agents.size());
    for (std::size_t i = 0; i < agents.size(); ++i) {
        if (!is_built_in(agents[i])) out.push_back(i);
    }
    return out;
}

class AgentMenuListBase : public ComponentBase {
public:
    explicit AgentMenuListBase(AgentMenuOptions opts)
        : opts_(std::move(opts)) {}

    Element Render() override {
        auto selectable = selectable_indices(opts_.agents);
        const int item_count = 1 + static_cast<int>(selectable.size());
        if (selected_position_ < 0 || selected_position_ >= item_count) {
            selected_position_ = 0;
        }

        const auto theme = cc::ui::design::theme::current_theme();
        const auto accent = theme.palette->primary;
        const auto muted = theme.palette->muted;
        const auto suggestion = theme.palette->suggestion;
        const int active_count = static_cast<int>(opts_.agents.size());

        Elements rows;
        rows.push_back(render_create_row(selected_position_ == 0, suggestion));

        for (const auto& group : agent_display::agent_source_groups()) {
            if (group.source == "built-in") continue;
            Elements group_rows;
            for (std::size_t i = 0; i < opts_.agents.size(); ++i) {
                const auto& agent = opts_.agents[i];
                if (agent.source != group.source) continue;
                group_rows.push_back(render_agent_row(
                    agent,
                    selected_position_for_index(selectable, i) == selected_position_,
                    /*selectable=*/true,
                    suggestion,
                    muted));
            }
            if (group_rows.empty()) continue;
            rows.push_back(text(""));
            rows.push_back(text("  " + group.label) | bold | color(muted) | dim);
            for (auto& row : group_rows) rows.push_back(std::move(row));
        }

        Elements built_in;
        for (const auto& agent : opts_.agents) {
            if (!is_built_in(agent)) continue;
            built_in.push_back(render_agent_row(
                agent,
                /*selected=*/false,
                /*selectable=*/false,
                suggestion,
                muted));
        }
        if (!built_in.empty()) {
            rows.push_back(text(""));
            rows.push_back(hbox({
                text("  Built-in agents") | bold | color(muted) | dim,
                text(" (always available)") | color(muted) | dim,
            }));
            for (auto& row : built_in) rows.push_back(std::move(row));
        }

        Element body = vbox(std::move(rows))
            | vscroll_indicator
            | yframe
            | size(HEIGHT, LESS_THAN, 26);

        Element title = vbox({
            text("Agents") | bold | color(accent),
            text(std::format("{} agents", active_count)) | color(muted) | dim,
        });

        return vbox({
            title,
            text(""),
            std::move(body),
        }) | flex;
    }

    bool OnEvent(Event ev) override {
        if (ev == Event::Escape) {
            if (opts_.on_cancel) opts_.on_cancel();
            return true;
        }

        auto selectable = selectable_indices(opts_.agents);
        const int item_count = 1 + static_cast<int>(selectable.size());
        if (item_count <= 0) return false;

        if (ev == Event::ArrowDown || ev == Event::Character('j')) {
            selected_position_ = (selected_position_ + 1) % item_count;
            return true;
        }
        if (ev == Event::ArrowUp || ev == Event::Character('k')) {
            selected_position_ = (selected_position_ - 1 + item_count) % item_count;
            return true;
        }
        if (ev == Event::Return) {
            if (selected_position_ == 0) {
                if (opts_.on_create_new) opts_.on_create_new();
                return true;
            }
            const int idx = selected_position_ - 1;
            if (idx >= 0 && idx < static_cast<int>(selectable.size())) {
                const auto& agent = opts_.agents[selectable[static_cast<std::size_t>(idx)]];
                if (opts_.on_select) opts_.on_select(agent.id);
            }
            return true;
        }
        return false;
    }

private:
    [[nodiscard]] static int selected_position_for_index(
        const std::vector<std::size_t>& selectable,
        std::size_t index) {
        for (std::size_t i = 0; i < selectable.size(); ++i) {
            if (selectable[i] == index) return static_cast<int>(i) + 1;
        }
        return -1;
    }

    [[nodiscard]] static Element render_create_row(bool selected, Color suggestion) {
        const auto c = selected ? suggestion : Color::Default;
        return hbox({
            text(selected ? "  › " : "    ") | color(c) | bold,
            text("Create new agent") | color(c),
        });
    }

    [[nodiscard]] static Element render_agent_row(
        const cards::AgentCardData& agent,
        bool selected,
        bool selectable,
        Color suggestion,
        Color muted) {
        const bool dimmed = !selectable;
        const auto c = selected ? suggestion : Color::Default;
        const auto model = resolved_model_label(agent);
        Elements parts;
        parts.push_back(text(selectable ? (selected ? "  › " : "    ") : "    ")
            | color(c) | bold);
        Element name = text(agent.name) | color(c);
        if (dimmed) name = name | dim;
        parts.push_back(std::move(name));
        if (!model.empty()) {
            parts.push_back(text(" · " + model)
                | color(selected ? c : muted)
                | dim);
        }
        return hbox(std::move(parts));
    }

    AgentMenuOptions opts_;
    int selected_position_ = 0;  // 0 is "Create new agent".
};

[[nodiscard]] inline Component AgentMenuList(AgentMenuOptions opts) {
    return Make<AgentMenuListBase>(std::move(opts));
}

} // namespace agents_menu

inline void close_agents_menu(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    s->mode = ReplMode::Normal;
    s->agents_component.reset();
    if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
}

[[nodiscard]] inline std::shared_ptr<Component> get_agents_component(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    if (!s->agents_component) {
        agents_menu::AgentMenuOptions opts;
        opts.agents = s->agent_cards;
        opts.on_create_new = [s, cb] {
            close_agents_menu(s, cb);
            if (cb->enqueue_slash_command) cb->enqueue_slash_command("/agents create");
        };
        opts.on_select = [s, cb](const std::string& id) {
            close_agents_menu(s, cb);
            if (cb->enqueue_slash_command) cb->enqueue_slash_command("/agents configure " + id);
        };
        opts.on_cancel = [s, cb] {
            close_agents_menu(s, cb);
        };
        s->agents_component = std::make_shared<Component>(
            agents_menu::AgentMenuList(std::move(opts)));
    }
    return std::static_pointer_cast<Component>(s->agents_component);
}

[[nodiscard]] inline Element render_agents_menu(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    auto comp = get_agents_component(s, cb);
    Element footer = text("  Press ↑↓ to navigate · Enter to select · Esc to go back")
        | color(cc::ui::design::theme::current_theme().palette->muted)
        | dim;
    return comp ? vbox({(*comp)->Render(), std::move(footer)}) | flex : text("");
}

inline bool forward_agents_menu(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb,
    Event ev) {
    auto comp = get_agents_component(s, cb);
    if (comp && (*comp)->OnEvent(ev)) return true;
    if (ev == Event::Escape) {
        close_agents_menu(s, cb);
        return true;
    }
    return false;
}

// -------------------------------------------------------------------
// Settings dialog helpers (UI3)
// -------------------------------------------------------------------

namespace settings_ns = cc::ui::dialogs::settings_dialog;

using settings_ns::CommandResultDisplay;
using settings_ns::SettingsDialogOptions;
using settings_ns::SettingsTabId;
using settings_ns::MakeSettingsDialog;

/// Lazily create (or re-create) the settings dialog component.
/// If `state->settings_config` is non-null it is used for reads/writes,
/// otherwise a fresh internal ConfigManager is used (snapshot only).
[[nodiscard]] inline std::shared_ptr<Component> get_settings_component(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    if (!s->settings_component) {
        // Use the supplied config manager, otherwise manufacture a default.
        static thread_local cc::core::ConfigManager fallback_config;
        cc::core::ConfigManager* cfg = s->settings_config
                                            ? static_cast<cc::core::ConfigManager*>(
                                                  s->settings_config)
                                            : &fallback_config;
        SettingsDialogOptions opts;
        opts.initial_tab = SettingsTabId::General;
        opts.on_close = [s, cb](std::optional<std::string>, CommandResultDisplay) {
            s->mode = ReplMode::Normal;
            s->settings_component.reset();
            if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        };
        s->settings_component = std::make_shared<Component>(
            MakeSettingsDialog(*cfg, std::move(opts)));
    }
    return std::static_pointer_cast<Component>(s->settings_component);
}

/// Reset (destroy) the settings component so the next entry starts fresh
/// (empty dirty flag, pristine snapshot, tab=General).
inline void reset_settings_component(const std::shared_ptr<ReplScreenState>& s) {
    s->settings_component.reset();
}

/// Render the settings dialog content as an Element.
[[nodiscard]] inline Element render_settings(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    auto comp = get_settings_component(s, cb);
    return comp ? (*comp)->Render() : text("");
}

/// Forward an event to the settings dialog component.
inline bool forward_settings(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb,
    Event ev) {
    auto comp = get_settings_component(s, cb);
    return comp && (*comp)->OnEvent(std::move(ev));
}

// -------------------------------------------------------------------
// Trust dialog helpers (UI8)
// -------------------------------------------------------------------
//
// Trust dialog lives in the STANDALONE slot (full-takeover).  We also
// keep a ReplMode-driven path as a bridge for callers that haven't
// migrated to the queue yet.  The Component is lazy-created via
// MakeWorkspaceTrustDialog() and stored as an opaque handle so
// ReplScreenState doesn't need to import the trust_dialog types.

namespace trust_ns = cc::ui::trust_dialog;
using trust_ns::TrustChoice;
using trust_ns::WorkspaceTrustProps;
using trust_ns::SecuritySources;
using trust_ns::MakeWorkspaceTrustDialog;

/// Lazily create (or re-create) the trust dialog component.
[[nodiscard]] inline std::shared_ptr<Component> get_trust_dialog(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    if (!s->wizard_trust) {
        WorkspaceTrustProps props;
        props.workspace_path = s->cwd.empty() ? "." : s->cwd;
        props.on_done = [s, cb](TrustChoice choice) {
            // Bridge choices onto the permission + exit callbacks.  The
            // enum actually ships 5 values (AllowOnce/AlwaysAllow/ViewFile/
            // Cancel/EnableAnyway) — the "Exit" variant is handled by the
            // on_exit callback directly inside the trust dialog component
            // when the user chooses the corresponding option, not via
            // choice enum here.
            using C = TrustChoice;
            if (cb->on_permission_response) {
                switch (choice) {
                  case C::AllowOnce:    cb->on_permission_response(true, false); break;
                  case C::AlwaysAllow:  cb->on_permission_response(true, true);  break;
                  case C::EnableAnyway: cb->on_permission_response(true, true);  break;
                  case C::ViewFile:     cb->on_permission_response(true, false); break;
                  case C::Cancel:
                  default:              cb->on_permission_response(false, std::nullopt); break;
                }
            }
            s->mode = ReplMode::Normal;
            if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
            s->wizard_trust.reset();
        };
        s->wizard_trust = std::make_shared<Component>(
            MakeWorkspaceTrustDialog(std::move(props)));
    }
    return std::static_pointer_cast<Component>(s->wizard_trust);
}

/// Render the trust dialog content as an Element.
[[nodiscard]] inline Element render_trust_dialog(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    auto d = get_trust_dialog(s, cb);
    return d ? (*d)->Render() : text("(trust dialog unavailable)");
}

/// Forward an event to the trust dialog component.
inline bool forward_trust_dialog(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb,
    Event ev) {
    auto d = get_trust_dialog(s, cb);
    return d && (*d)->OnEvent(std::move(ev));
}

} // namespace dialog_router

/// Build the REPL screen as an FTXUI Component.
/// Engine updates the externally-held state between frames.
/// Event tiers (TS global+command keybindings):
///   Dialog(Esc/y/n/a/c/r/q) > Global(Ctrl+C/D/L/O) > Input(Enter/
///     Ctrl+J/Tab/Shift+Tab/Up/Down/Esc/printable/Backspace)
[[nodiscard]] inline Component ReplScreen(
    std::shared_ptr<ReplScreenState> state,
    ReplScreenCallbacks cbs) {
    auto cb = std::make_shared<ReplScreenCallbacks>(std::move(cbs));
    return Renderer([state, cb]() -> Element {
        // M7.5: All dialogs flow through DialogQueue — no legacy bridge needed.
        // Engine pushes via PushXxx() (app.cppm / query_engine.cppm).

        // M6: Faithful permission panels — bash / file_edit / file_write.
        // Rendered as a dbox overlay (matching TS overlay slot).
        if (state->mode == ReplMode::ToolPermission &&
            state->permission_request) {
            // Convert repl_screen PermissionRequestInfo →
            // cc::ui::dialogs::PermissionRequest (lives in the dialogs
            // namespace, NOT a nested permission_dialog sub-namespace).
            using pd_pr = cc::ui::dialogs::PermissionRequest;
            pd_pr r{};
            r.tool_name   = state->permission_request->tool_name;
            r.description = state->permission_request->description;
            r.risk_level  = state->permission_request->risk_labels.empty()
                          ? "medium"
                          : state->permission_request->risk_labels.front();
            if (state->permission_request->file_path)
                r.affected_paths.push_back(*state->permission_request->file_path);
            Element base = RenderReplScreen(*state);
            Element panel = paragraph(
                cc::ui::dialogs::render_permission_dialog(std::move(r), 80));
            return dbox({
                base | dim,
                vbox({ filler(),
                       hbox({ filler(), panel | flex_shrink, filler() })
                           | flex_shrink,
                       filler() }) | flex });
        }
        // UI3: SettingsView modal — render the tabbed settings dialog
        // over the dimmed REPL background.
        if (state->mode == ReplMode::SettingsView) {
            Element base = RenderReplScreen(*state);
            Element settings_content = dialog_router::render_settings(state, cb);
            return dbox({
                base | dim,
                vbox({ filler(),
                       hbox({ filler(), settings_content | flex_shrink, filler() })
                           | flex_shrink,
                       filler() }) | flex });
        }
        if (state->mode == ReplMode::AgentsView) {
            Element agents_content = dialog_router::render_agents_menu(state, cb);
            return vbox({ filler(),
                          separator() |
                              color(cc::ui::design::theme::current_theme().palette->muted),
                          hbox({ text("  "),
                                 agents_content | flex,
                                 text("  ") }) | flex_shrink }) | flex;
        }
        // UI8: TrustDialog is a STANDALONE slot — it takes over the full
        // terminal.  No chrome, no prompt, no messages show behind it.
        if (state->mode == ReplMode::TrustDialog) {
            return vbox({
                filler(),
                hbox({ filler(),
                       dialog_router::render_trust_dialog(state, cb) | flex_shrink,
                       filler() }) | flex_shrink,
                filler() }) | flex;
        }
        return RenderReplScreen(*state);
    })
         | CatchEvent([state, cb](Event ev) -> bool {
    // --- M7: dialog_queue event dispatch (priority 0) ---
    // Standalone > Modal > Overlay > Bottom.  This block runs FIRST
    // so that a ToolPermission overlay consumes y/n/a before the
    // legacy in_dialog / input paths see it.
    bool tool_animating = state->spinner_mode != SpinnerMode::Hidden;
    if (dialog_queue_render::DispatchDialogQueueEvents(
            *state, ev, state->is_prompt_input_active,
            /*allow_dialogs_with_animation=*/!tool_animating)) {
        return true;
    }

    // --- Panel-mode predicate ---
    auto is_panel = [](ReplMode m){ return
        m==ReplMode::Normal || m==ReplMode::TasksView
        || m==ReplMode::TeamsView || m==ReplMode::HelpView
        || m==ReplMode::QuickOpen; };
    const bool in_dialog = !is_panel(state->mode);

    // 0) Dialog queue — takes priority over legacy ReplMode dialogs
    //    (M7.5: migration path from ReplMode to DialogQueue).
    //    NOTE: has_standalone/modal/overlay/bottom are checked inline
    //    inside DispatchDialogQueueEvents() (called above), so this
    //    block is intentionally empty — the dedicated per-slot helper
    //    namespace functions are not re-exposed as free predicates here
    //    to avoid a duplicate definition with dialog_queue_render.
    (void)state;

    // 1) Dialog-context events
    if (in_dialog) {
        // UI8 TrustDialog: highest priority (standalone slot), takes every
        // event so that the 4-tier selection / countdown / YES-typing
        // gating can work reliably.
        if (state->mode == ReplMode::TrustDialog) {
            return dialog_router::forward_trust_dialog(state, cb, ev);
        }
        // UI13 agent wizard: forward every event to the wizard component
        // (it manages Esc/Enter/buttons internally).
        if (state->mode == ReplMode::CreateAgent ||
            state->mode == ReplMode::EditAgent) {
            return dialog_router::forward_agent(state, cb, ev);
        }
        if (state->mode == ReplMode::AgentsView) {
            return dialog_router::forward_agents_menu(state, cb, ev);
        }
        // UI15 wizard modes: forward every event to the wizard
        // component (they manage Esc/Enter/buttons internally).
        if (state->mode == ReplMode::InstallGitHubApp ||
            state->mode == ReplMode::InstallSlackApp) {
            return dialog_router::forward_install_wizard(state, cb, ev);
        }
        if (ev == Event::Escape) {
            // Critical dialogs defer to y/n/a/c/r/q handlers — EXCEPT
            // CostThreshold where Esc MUST ACKNOWLEDGE (never quit / data-loss).
            switch (state->mode) {
              case ReplMode::ToolPermission:
              case ReplMode::SandboxPermission:
              case ReplMode::WorkerSandboxPermission:
                break;
              case ReplMode::CostThreshold: {
                namespace ct = cc::ui::dialogs::cost_threshold;
                ct::CostThresholdState st;
                st.on_done = [&state, cb] {
                    state->mode = ReplMode::Normal;
                    if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
                    if (cb->on_dialog_action)
                        cb->on_dialog_action(ReplMode::CostThreshold, 0);
                };
                ct::HandleCostThresholdEvent(st, Event::Escape);
                return true; }
              default:
                state->mode = ReplMode::Normal;
                if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
                return true; } }
        // CostThreshold: Enter / Space / shortcuts all fire on_done().
        // Delegate to the unified HandleCostThresholdEvent.
        if (state->mode == ReplMode::CostThreshold) {
            if (ev == Event::Return ||
                (ev.is_character() && ev.character() == " ")) {
                namespace ct = cc::ui::dialogs::cost_threshold;
                ct::CostThresholdState st;
                st.on_done = [&state, cb] {
                    state->mode = ReplMode::Normal;
                    if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
                    if (cb->on_dialog_action)
                        cb->on_dialog_action(ReplMode::CostThreshold, 0);
                };
                ct::HandleCostThresholdEvent(st, ev);
                return true;
            }
        }
        if (ev.is_character()) {
            char c = ev.character()[0];
            const bool is_perm =
                state->mode==ReplMode::ToolPermission ||
                state->mode==ReplMode::SandboxPermission ||
                state->mode==ReplMode::WorkerSandboxPermission ||
                state->mode==ReplMode::Elicitation ||
                state->mode==ReplMode::PromptHook;
            if (is_perm) {
                if (c=='y'||c=='Y'){
                    if (cb->on_permission_response)
                        cb->on_permission_response(true,false);
                    return true;
                }
                if (c=='n'||c=='N'){
                    if (cb->on_permission_response)
                        cb->on_permission_response(false,std::nullopt);
                    return true;
                }
                if (c=='a'||c=='A'){
                    if (cb->on_permission_response)
                        cb->on_permission_response(true,true);
                    return true;
                }
            }
            // CostThreshold: ALL characters are swallowed by the unified
            // handler.  Shortcuts (g/y/o/k) will fire on_done(); any other
            // character is silently consumed to prevent prompt-injection.
            if (state->mode == ReplMode::CostThreshold) {
                namespace ct = cc::ui::dialogs::cost_threshold;
                ct::CostThresholdState st;
                st.on_done = [&state, cb] {
                    state->mode = ReplMode::Normal;
                    if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
                    if (cb->on_dialog_action)
                        cb->on_dialog_action(ReplMode::CostThreshold, 0);
                };
                (void)ct::HandleCostThresholdEvent(st, ev);
                return true;
            }
        }
    }

    // 2) Global shortcuts
    if (ev == Event::Character('\x03'))
        { if (cb->on_interrupt) cb->on_interrupt(); return true; }
    if (ev == Event::Character('\x04'))
        { if (cb->on_exit) cb->on_exit(); return true; }
    if (ev == Event::Character('\x0C')) {
        state->input_text.clear();
        state->input_cursor = std::string::npos;
        state->autocomplete_suggestions.clear();
        state->autocomplete_index = -1; return true; }
    if (ev == Event::Character('\x0F')) return true;  // Ctrl+O transcript

    if (!in_dialog &&
        state->active_local_jsx_command &&
        cb->on_local_jsx_event &&
        cb->on_local_jsx_event(ev)) {
        return true;
    }

    if (!in_dialog && state->active_local_jsx_command && ev == Event::Escape) {
        if (cb->on_local_jsx_cancel) {
            cb->on_local_jsx_cancel();
        } else {
            state->active_local_jsx_command = false;
            state->active_local_jsx_command_name.clear();
            state->active_local_jsx_command_args.clear();
            state->active_local_jsx_content.clear();
        }
        return true;
    }

    if (!in_dialog && ev.is_mouse()) {
        if (ev.mouse().button == Mouse::WheelUp) {
            return ScrollTranscript(state, -3);
        }
        if (ev.mouse().button == Mouse::WheelDown) {
            return ScrollTranscript(state, 3);
        }
    }
    if (!in_dialog && state->autocomplete_suggestions.empty()) {
        const int page = std::max(1, state->viewport_height_lines / 2);
        if (ev == Event::PageUp) {
            return ScrollTranscript(state, -page);
        }
        if (ev == Event::PageDown) {
            return ScrollTranscript(state, page);
        }
    }

    // 3) Input-context events
    const bool accept_input = !in_dialog;
    if (accept_input) {
        const int asn = static_cast<int>(state->autocomplete_suggestions.size());
        // Enter
        if (ev == Event::Return && asn > 0) {
            const int selected = std::clamp(
                state->autocomplete_index < 0 ? 0 : state->autocomplete_index,
                0,
                asn - 1);
            const bool submit =
                state->autocomplete_suggestions[static_cast<std::size_t>(selected)]
                    .submit_on_return;
            auto accepted = accept_selected_prompt_suggestion(state);
            if (submit && accepted && cb->on_submit) {
                cb->on_submit(*accepted, state->input_mode);
                state->input_history.push_back(*accepted);
                if (state->input_history.size() > 1000) state->input_history.pop_front();
                state->history_index = std::string::npos;
                state->input_text.clear();
                state->input_cursor = std::string::npos;
                state->is_prompt_input_active = false;
            }
            return true;
        }
        if (ev == Event::Return && !state->input_text.empty()) {
            if (cb->on_submit) cb->on_submit(state->input_text, state->input_mode);
            state->input_history.push_back(state->input_text);
            if (state->input_history.size() > 1000) state->input_history.pop_front();
            state->history_index = std::string::npos;
            state->input_text.clear();
            state->input_cursor = std::string::npos;
            state->autocomplete_suggestions.clear();
            state->autocomplete_index = -1;
            state->is_prompt_input_active = false; return true; }
        // Ctrl+Enter -> newline (Ctrl+J in terminals)
        if (ev == Event::Character('\x0A')) {
            insert_prompt_text(state, "\n");
            return true; }
        // Tab / Shift+Tab -> autocomplete
        if (ev == Event::Tab && asn > 0) {
            // AT-07: complete to the common prefix of all visible suggestion
            // insert_texts when it strictly extends what's typed (e.g. "@sr"
            // with {@src/readme, @src/main} → "@src/"); otherwise accept the
            // selected suggestion. Faithful to TS typeahead Tab behavior.
            const auto& sugg = state->autocomplete_suggestions;
            std::string common = sugg[0].insert_text;
            for (int k = 1; k < asn && !common.empty(); ++k) {
                const auto& ins = sugg[k].insert_text;
                std::size_t j = 0;
                while (j < common.size() && j < ins.size() && common[j] == ins[j]) ++j;
                common.resize(j);
            }
            const int idx = std::clamp(state->autocomplete_index, 0, asn - 1);
            const auto& sel = sugg[idx];
            const std::size_t rs = (sel.replacement_start == std::string::npos)
                ? 0 : sel.replacement_start;
            const std::size_t re = (sel.replacement_end == std::string::npos)
                ? state->input_text.size() : sel.replacement_end;
            const std::string typed = (rs <= re && re <= state->input_text.size())
                ? state->input_text.substr(rs, re - rs) : std::string{};
            if (!common.empty() && common.size() > typed.size() &&
                common.compare(0, typed.size(), typed) == 0) {
                state->input_text.replace(rs, re - rs, common);
                state->input_cursor = rs + common.size();
                state->is_prompt_input_active = true;
                state->last_keystroke = std::chrono::steady_clock::now();
            } else {
                (void)accept_selected_prompt_suggestion(state);
            }
            return true;
        }
        // Shift+Tab (ISO backtab) = \x1B[Z
        if (ev.input() == "\x1B[Z" && asn > 0) {
            state->autocomplete_index = state->autocomplete_index < 0 ? asn - 1
                : (state->autocomplete_index - 1 + asn) % asn; return true; }
        // Up / Down navigate visible autocomplete suggestions before history.
        if (ev == Event::ArrowUp && asn > 0) {
            state->autocomplete_index = state->autocomplete_index <= 0 ? asn - 1
                : state->autocomplete_index - 1; return true; }
        if (ev == Event::ArrowDown && asn > 0) {
            state->autocomplete_index = state->autocomplete_index < 0 ||
                state->autocomplete_index >= asn - 1
                ? 0 : state->autocomplete_index + 1; return true; }
        // Up (history back) / Down (history forward)
        if (ev == Event::ArrowUp && state->input_text.empty()
            && !state->input_history.empty()) {
            state->history_index = state->history_index == std::string::npos
                ? state->input_history.size() - 1
                : std::max<std::size_t>(0, state->history_index - 1);
            state->input_text = state->input_history[state->history_index];
            state->input_cursor = state->input_text.size();
            state->is_prompt_input_active = true;
            state->last_keystroke = std::chrono::steady_clock::now(); return true; }
        if (ev == Event::ArrowDown
            && state->history_index != std::string::npos) {
            if (state->history_index + 1 >= state->input_history.size()) {
                state->history_index = std::string::npos;
                state->input_text.clear();
                state->input_cursor = std::string::npos;
            } else {
                state->input_text = state->input_history[++state->history_index];
                state->input_cursor = state->input_text.size(); }
            state->is_prompt_input_active = true;
            state->last_keystroke = std::chrono::steady_clock::now(); return true; }
        // Esc
        if (ev == Event::Escape) {
            if (!state->autocomplete_suggestions.empty()) {
                // INF-05: remember the dismissed input so a later non-mutating
                // keystroke (e.g. arrow keys) doesn't reopen the popup.
                state->dismissed_autocomplete_for_input = state->input_text;
                state->autocomplete_suggestions.clear();
                state->autocomplete_index = -1; return true; }
            if (state->selected_message_idx >= 0)
                { state->selected_message_idx = -1; return true; }
            if (!state->input_text.empty()) {
                state->input_text.clear();
                state->input_cursor = std::string::npos;
                state->history_index = std::string::npos; return true; } }
        if (ev == Event::ArrowLeft) {
            move_prompt_cursor_left(state);
            return true;
        }
        if (ev == Event::ArrowRight) {
            move_prompt_cursor_right(state);
            return true;
        }
        if (ev == Event::Home) {
            state->input_cursor = 0;
            state->is_prompt_input_active = true;
            state->last_keystroke = std::chrono::steady_clock::now();
            return true;
        }
        if (ev == Event::End) {
            state->input_cursor = state->input_text.size();
            state->is_prompt_input_active = true;
            state->last_keystroke = std::chrono::steady_clock::now();
            return true;
        }
        if (ev == Event::Delete) {
            if (delete_prompt_text(state)) return true;
        }
        // Printable chars — accepts both ASCII and multi-byte UTF-8 (CJK).
        // FTXUI delivers composed IME characters as a single character event
        // containing the full UTF-8 byte sequence in ev.character().
        if (ev.is_character()) {
            const std::string& ch = ev.character();
            if (!ch.empty()) {
                unsigned char first = static_cast<unsigned char>(ch[0]);
                // Accept printable ASCII (0x20..0x7E) and UTF-8 multi-byte
                // start bytes (0xC0..0xFF).  Continuation bytes (0x80..0xBF)
                // should never appear as the first byte of a character event.
                const bool is_printable =
                    (first >= 0x20 && first < 0x7F) || first >= 0xC0;
                if (is_printable) {
                    namespace figs = cc::ui::design::figures;
                    // ── P0-1: TS-equivalent single-char mode interception ──
                    //
                    // TS PromptInput.tsx lines 869-901: when the user types a
                    // single '!' with cursor at offset 0 into an EMPTY input
                    // buffer, that's a MODE TRANSITION — NOT a character to
                    // store.  The '!' is swallowed, InputMode flips, and the
                    // prefix glyph changes without the text ever landing in
                    // the input state (so history persists cleanly).
                    //
                    // All other cases (multi-byte paste of "!cmd", cursor
                    // nonzero, typing '!' into existing text) → fall through
                    // to insert_prompt_text as normal; HandleSubmit will
                    // strip the prefix on submit for those.
                    const bool cursor_at_zero =
                        state->input_cursor == std::string::npos ||
                        state->input_cursor == 0;
                    if (state->input_text.empty() && cursor_at_zero &&
                        figs::is_mode_character(ch) &&
                        ch.size() == 1) {
                        // Swallow the char, flip mode.  Toggle Prompt↔Bash.
                        state->input_mode =
                            (state->input_mode == InputMode::Bash)
                                ? InputMode::Prompt
                                : InputMode::Bash;
                        state->is_prompt_input_active = true;
                        state->last_keystroke =
                            std::chrono::steady_clock::now();
                        // Also wipe any dismissed-suggestion memory since
                        // mode change implicitly changes the autocomplete
                        // provider context.
                        state->dismissed_autocomplete_for_input.clear();
                        return true;
                    }
                    insert_prompt_text(state, ch);
                    return true;
                }
            }
        }
        // Backspace — handle multi-byte UTF-8 correctly by erasing a full
        // codepoint, not just the last byte.  CJK characters are 3 bytes in
        // UTF-8, so a plain pop_back() would leave a partial/invalid sequence.
        if (ev == Event::Backspace && !state->input_text.empty()) {
            (void)backspace_prompt_text(state);
            return true;
        }
    }
    return false; });
}

/// Convenience: self-owned state (demos/tests only).
/// Production: use externally-held shared_ptr<ReplScreenState> overload.
[[nodiscard]] inline Component ReplScreen(ReplScreenCallbacks cbs) {
    return ReplScreen(std::make_shared<ReplScreenState>(), std::move(cbs));
}

} // namespace cc::ui::repl_screen
