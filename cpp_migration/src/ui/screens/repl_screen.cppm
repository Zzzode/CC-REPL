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
///   dialogs      -> cc.ui.dialogs.* (RouteDialog dispatches, UI8-UI11/UI16)
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <variant>
#include <format>
#include <cstdint>
#include <chrono>
#include <algorithm>
#include <deque>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/string.hpp>  // for string_width

export module cc.ui.repl_screen;

// --- Sub-modules we DEPEND ON (skeleton-wired, bodies delegated) ---
import cc.ui.task_list_ui;
import cc.ui.team_status;
import cc.ui.messages.message_row;
import cc.ui.messages.messages_list;
import cc.ui.messages.user_text_message;
import cc.ui.messages.assistant_text_message;
import cc.ui.messages.system_text_message;
import cc.ui.messages.thinking_message;
import cc.ui.messages.tool_use_message;
import cc.ui.messages.message_tool_result;
import cc.ui.dialogs.permission_dialog;
import cc.ui.agents.agent_cards;
import cc.ui.agents.agent_wizard;
import cc.ui.dialogs.install_github_app_wizard;
import cc.ui.dialogs.install_slack_app_wizard;
// Welcome header: Clawd mark + animated asterisk (logo_v2) — was dead code,
// now wired into RenderReplScreen for fresh sessions.
import cc.ui.design.logo;
// M2: theme::current_theme() for the clawd_body colour used by the banner.
import cc.ui.design.theme;
// M2: format_welcome_message + kWelcomeTips feed for the welcome header.
import cc.ui.logo;
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
    TasksView, TeamsView, SettingsView, HelpView, AboutView, QuickOpen,
    // UI15 (Phase 4) — install-command wizard overlays.
    InstallGitHubApp,   // 12-step /install-github-app  FTXUI wizard
    InstallSlackApp,    // 3-step  /install-slack-app   FTXUI wizard
    // UI13 — agent wizard (create/edit).
    CreateAgent,        // 4-step new-agent wizard
    EditAgent,          // 4-step edit-agent wizard
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
    bool is_compact_boundary = false, is_error = false;
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

    // M6: tool kind for dispatching to the correct faithful panel
    PermissionToolKind tool_kind = PermissionToolKind::Generic;

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
};

/// Transient dialog payloads for RouteDialog dispatch.
struct DialogContext {
    std::optional<std::string> sandbox_host_pattern, sandbox_worker_request_id;
    std::optional<std::string> elicitation_server_name;
    std::optional<std::uint64_t> elicitation_request_id;
    std::optional<double> cost_threshold_usd;
    std::optional<std::uint32_t> idle_return_minutes;
    std::optional<std::string> ide_installation_status, model_switch_alias;
    std::optional<std::string> plugin_hint_name, plugin_hint_description;
    std::optional<std::string> lsp_rec_extension, lsp_rec_file_ext;
    std::optional<std::string> ultraplan_blurb;
    // UI13: agent wizard — name of agent being edited (empty = create new).
    std::optional<std::string> agent_wizard_agent_id;
};

/// Lean orchestration state.  Full app state lives in services/; the
/// engine writes computed projections into this struct between frames.
struct ReplScreenState {
    ReplMode mode = ReplMode::Normal;
    InputMode input_mode = InputMode::Prompt;
    SpinnerMode spinner_mode = SpinnerMode::Hidden;
    // Messages
    std::vector<MessageDisplayEntry> messages;
    int scroll_offset = 0, selected_message_idx = -1;
    int viewport_height_lines = 40;
    bool scroll_pinned_to_bottom = true;
    // M1 (FullscreenLayout slot-system chrome): scroll-derived chrome state.
    //   sticky_prompt_text — text of the sticky header shown while scrolled up
    //     into history (TS `stickyPrompt.text` via ScrollChromeContext).
    //     Empty => no header.
    //   unseen_message_count — count of assistant turns added below the fold
    //     while unpinned; drives the "N new messages" pill label (TS
    //     `newMessageCount` + `useUnseenDivider`).  The pill itself only
    //     renders when pill_visible is true (engine snapshots the divider).
    //   pill_visible — TS `pillVisible` (useSyncExternalStore against the
    //     scroll handle).  Defaults false so chrome stays dormant until the
    //     engine wires real scroll-observe state.
    std::string sticky_prompt_text;
    int unseen_message_count = 0;
    bool pill_visible = false;
    // Input
    std::string input_text;
    // TS-style contextual placeholder rather than a generic default.
    std::string input_placeholder = "Try \"write a test\", \"/help\", or ask anything...";
    // Welcome-header data (shown when messages is empty on a fresh session).
    std::string app_version = "0.0.0";
    std::string model_display_name;
    std::string cwd;
    // M2: oauthAccount.displayName analogue — drives formatWelcomeMessage
    // ("Welcome back {user}!" vs "Welcome back!").  Empty for new users.
    std::string user_display_name;
    // Stable per-session welcome-tip index (seeded once from the session id in
    // app.cppm). The renderer mods this by kWelcomeTips.size(). Previously the
    // tip used spinner_frame, which cycled the tip on every mouse-move re-render.
    std::size_t welcome_tip_index{0};
    std::vector<std::string> autocomplete_suggestions;
    int autocomplete_index = -1;
    std::deque<std::string> input_history;
    std::size_t history_index = std::string::npos;
    // Status / spinner / permission / context
    StatusBarData status_bar;
    std::optional<std::string> spinner_verb, spinner_tip;
    std::optional<PermissionRequestInfo> permission_request;
    DialogContext dialog_ctx;
    // Settings-driven UI configuration (mirrors AppState.settings subset
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
    std::chrono::steady_clock::time_point last_keystroke;

    // UI15: opaque wizard component handles (lazily created by
    // dialog_router::get_install_*_wizard(), stored as shared_ptr<void>
    // so ReplScreenState doesn't need to import their types).
    std::shared_ptr<void> wizard_install_github_app;
    std::shared_ptr<void> wizard_install_slack_app;
    // UI13: agent wizard component handle (lazily created by
    // dialog_router::get_agent_wizard()).
    std::shared_ptr<void> wizard_agent;
    // M6: permission panel component handle (lazily created by
    // dialog_router::get_permission_panel()).  Cache avoids rebuilding
    // the component on every frame (matches agent_wizard pattern).
    std::shared_ptr<void> permission_panel;
    // Tracks which tool_kind the cached permission_panel was built for.
    // When tool_kind changes, we rebuild the panel.
    PermissionToolKind permission_panel_kind = PermissionToolKind::Generic;
    // Cache key for content-level invalidation (tool_name + description).
    // Ensures the panel rebuilds when request content changes, even for
    // the same tool_kind (e.g. two consecutive Generic requests).
    std::string permission_panel_cache_key;

    // M7: Core dialog framework
    // dialog_queue holds all pending dialogs across all slots.
    // Engine pushes dialog requests into this queue; renderer peeks from it.
    cc::ui::dialogs::system::DialogQueue dialog_queue;
    // dialog_renderers is the registry of all dialog renderers + event handlers.
    // Registered once at startup by the app layer.
    cc::ui::dialogs::system::DialogRendererRegistry dialog_renderers;

    // M7: HelpView dialog state (panel mode).
    // Lazy-cached Component so state (selected tab, scroll position)
    // persists across renders.  Created on first entry to HelpView mode.
    std::shared_ptr<void> help_view_component;
    std::shared_ptr<cc::ui::dialogs::help_view::HelpViewState> help_view_state;

    // M7: SettingsView dialog state (panel mode — read-only view).
    std::shared_ptr<void> settings_view_component;
    std::shared_ptr<cc::ui::dialogs::settings_view::SettingsViewState> settings_view_state;

    // M7: AboutView dialog state (panel mode).
    std::shared_ptr<void> about_view_component;
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

// UI19: spinner line shell.  Animations/shimmer in spinner_widget.cppm.
[[nodiscard]] inline Element RenderSpinner(
    SpinnerMode m, const std::optional<std::string>& verb,
    const std::optional<std::string>& tip) {
    if (m == SpinnerMode::Hidden) return text("");
    struct S { std::string_view p, l; Color c; };
    S s{};
    switch (m) {
      case SpinnerMode::Requesting: s={"^ ","Requesting",Color::Blue}; break;
      case SpinnerMode::Thinking:   s={"(?) ","Thinking",Color::Cyan}; break;
      case SpinnerMode::Responding: s={"v ","Responding",Color::Cyan}; break;
      case SpinnerMode::ToolInput:  s={"(o) ","Preparing",Color::Yellow}; break;
      case SpinnerMode::ToolUse:    s={"(o) ","Running",Color::Yellow}; break;
      case SpinnerMode::Briefing:   s={"... ","Idle",Color::GrayLight}; break;
      case SpinnerMode::IdleBrief:  s={"  ","",Color::GrayDark}; break;
      default: return text(""); }
    std::string label(s.l);
    if (verb) label += ": " + *verb;
    Elements p = { text(" "), text(std::string(s.p)) | color(s.c),
                   text(label) | color(s.c) };
    if (tip) p.push_back(text("  -- " + *tip) | dim);
    return hbox(p);
}

// UI4/UI5: message list.  Delegates to messages_list.cppm (UI21).
// `spinner_frame` drives the tool-use header spinner animation (fix #10).
[[nodiscard]] inline Element RenderMessages(
    const std::vector<MessageDisplayEntry>& entries,
    int sel = -1, int vlines = 40,
    [[maybe_unused]] int offs = 0, bool pinned = true,
    int spinner_frame = 0) {
    if (entries.empty()) {
        // Empty-list placeholder kept minimal; the rich welcome header is
        // rendered by RenderWelcomeHeader() above the messages list.
        return vbox({ filler(),
            vbox({ text("  Type a message to begin.") | dim | center,
                   text("  /help    -- list commands") | dim | center,
                   text("  /model   -- change model") | dim | center,
                   text("  /config  -- open settings") | dim | center }) | center,
            filler() }) | flex;
    }

    namespace ml = cc::ui::messages_list;
    ml::MessagesListInput input;
    input.rows.reserve(entries.size());
    input.shapes.reserve(entries.size());

    for (const auto& m : entries) {
        if (m.role == "user") {
            input.shapes.push_back(messages::MessageShape::UserText);
            input.rows.push_back(messages::UserTextMessageData{
                .content = m.content_preview,
                .timestamp = m.timestamp,
                .quoted_reply = std::nullopt,
                .command_name = std::nullopt});
        } else if (m.role == "assistant") {
            if (m.is_thinking) {
                input.shapes.push_back(messages::MessageShape::AssistantThinking);
                messages::thinking_message::ThinkingMessageOptions opts;
                opts.data.raw_text = m.content_preview;
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

    (void)vlines; (void)pinned;
    return ml::render_messages_list_view(std::move(input),
                                         static_cast<std::size_t>(spinner_frame)) | flex;
}

// UI0: welcome header.  M2 faithful port of the TS LogoV2/WelcomeV2 welcome.
// Renders:
//   * the 58-col WelcomeV2 ASCII banner (Clawd mascot + scattered asterisks
//     + embedded clawd mark) — verbatim from WelcomeV2.tsx dark-theme branch;
//   * the animated hue-sweeping teardrop asterisk "✻" driven by the M1
//     per-frame `spinner_frame` tick (no Component tree required);
//   * formatWelcomeMessage parity ("Welcome back!" / "Welcome back {user}!");
//   * model display name + cwd metadata lines (dim, like TS);
//   * a single deterministic welcome tip below the banner.
// Shown only on a fresh idle session (messages empty + spinner hidden) — the
// gate lives in RenderReplScreen.
[[nodiscard]] inline Element RenderWelcomeHeader(const ReplScreenState& s,
                                                 int spinner_frame = 0) {
    namespace logo  = cc::ui::design::logo;
    namespace lgo   = cc::ui::logo;
    namespace thm   = cc::ui::design::theme;
    // clawd_body colour from the active theme (TS "clawd_body" role).
    const auto clawd = thm::current_theme().palette->primary;

    Elements head;
    // 2-space left indent so the banner sits roughly where the TS layout
    // centers it (the surrounding FullscreenLayout slot adds more chrome).
    head.push_back(text("  "));
    head.push_back(logo::welcome_v2_banner(s.app_version, clawd));

    // Welcome-message line: animated ✻ + formatWelcomeMessage.
    // New users (no display name known) fall back to the WelcomeV2 banner
    // caption that's already drawn; the message line mirrors the TS
    // LogoV2.tsx compact/horizontal welcome block.
    Elements wm;
    wm.push_back(text("  "));
    wm.push_back(logo::welcome_animated_asterisk(spinner_frame));
    wm.push_back(text(" "));
    wm.push_back(text(lgo::format_welcome_message(s.user_display_name)) | bold);
    head.push_back(hbox(std::move(wm)));

    // Metadata lines (dim, TS-style): model display name, cwd.
    // Priority: model_display_name (friendly name) > settings_model (config ID).
    if (!s.model_display_name.empty()) {
        head.push_back(hbox({
            text("  "),
            text(s.model_display_name) | dim,
        }));
    } else if (!s.settings_model.empty()) {
        head.push_back(hbox({
            text("  "),
            text(s.settings_model) | dim,
        }));
    }
    if (!s.cwd.empty()) {
        std::string cwd_disp = s.cwd;
        if (cwd_disp.size() > 56)
            cwd_disp = "…" + cwd_disp.substr(cwd_disp.size() - 55);
        head.push_back(hbox({
            text("  "),
            text(cwd_disp) | dim,
        }));
    }

    // One deterministic tip from the kWelcomeTips feed.  The TS EmergencyTip
    // is growthbook-driven and effectively empty in this port, so we surface
    // the in-repo kWelcomeTips list instead (frame-stable pick).
    if (!lgo::kWelcomeTips.empty()) {
        // Stable per-session pick: s.welcome_tip_index is seeded once from the
        // session id (app.cppm) - NOT spinner_frame, which would flicker the tip
        // on every mouse-move-triggered re-render.
        const auto idx = s.welcome_tip_index % lgo::kWelcomeTips.size();
        head.push_back(text(""));
        head.push_back(hbox({
            text("  "),
            text(std::string(lgo::kWelcomeTips[idx])) | dim,
        }));
    }

    return vbox(std::move(head));
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
//     "!" (red) for bash, "/" (cyan) for slash, "❮" (yellow/magenta) for
//     vim Normal/Visual — driven by s.input_mode.
//   * DECLARED CARET at the insertion point: TextInputImpl.Render() draws an
//     inverted glyph at the cursor offset (TS parks the real terminal cursor
//     there via useDeclaredCursor; we render a visible caret that lands on
//     the same byte offset).  Multi-line content lays out as a vbox.
//   * Contextual placeholder when empty (TS renderPlaceholder), styled dim.
//   * Selection highlight (TS HighlightedInput path) — provided by the real
//     impl when a selection range is set.
//   * Vim-mode badge (-- INSERT -- / -- NORMAL -- / -- VISUAL --) like TS,
//     driven by the existing vim_input::mode_display() helper.
//   * Autocomplete dropdown surfaced inline below the input (faithful to TS
//     ContextSuggestions) when s.autocomplete_suggestions is non-empty.
[[nodiscard]] inline Element RenderPromptInput(const ReplScreenState& s) {
    namespace uic = ::ui::components;
    namespace vim = cc::ui::prompt::vim_input;

    // --- 1. Prompt glyph (figures.pointer in TS) + accent colour ---------
    std::string mp; Color mc = Color::White;
    switch (s.input_mode) {
      case InputMode::Prompt:       mp="❯";   mc=Color::Green;    break;
      case InputMode::Bash:         mp="!";   mc=Color::Red;      break;
      case InputMode::SlashCommand: mp="/";   mc=Color::Cyan;     break;
      case InputMode::HistorySearch:mp="?";   mc=Color::Magenta;  break;
      case InputMode::PlanMode:     mp="▣";   mc=Color::Yellow;   break;
      case InputMode::VimInsert:    mp="❯";  mc=Color::Green;    break;
      case InputMode::VimNormal:    mp="❮";  mc=Color::Yellow;   break;
      case InputMode::VimVisual:    mp="❮";  mc=Color::Magenta;  break;
      case InputMode::OrphanedPermission: mp="!"; mc=Color::Red;  break;
      case InputMode::TaskNotification: mp="*"; mc=Color::Cyan;   break; }

    // --- 2. Sync a TextInputImpl from the projection --------------------
    // Built per render (cheap: only the buffer text + cursor move).  We do
    // NOT push_undo so the undo stack stays empty here — the engine remains
    // the source of truth for input_text; the impl is purely a render
    // primitive for caret/multiline/selection fidelity.
    uic::TextInputOptions opts;
    opts.placeholder  = s.input_placeholder;
    opts.prefix       = " ";   // glyph drawn separately (coloured per mode)
    opts.multiline    = true;
    opts.show_line_numbers = false;
    opts.enable_undo_redo   = false;
    opts.cursor_blink_ms    = 0;   // deterministic snapshot (no flicker)
    opts.show_history       = false;
    auto impl = std::make_shared<uic::TextInputImpl>(opts);
    impl->set_text(s.input_text);  // parks cursor at end of buffer
    // If the projection carries a caret offset in the future, an
    // impl->move_cursor(...) call here would honour it.  Today the engine
    // treats input as append-only, so end-of-buffer is the faithful caret.

    // --- 3. Render the input area from the REAL component ---------------
    // RenderInputArea paints: prefix, multi-line vbox, the declared caret
    // (inverted glyph at the cursor offset — TS useDeclaredCursor parity),
    // and selection highlight.  We strip the impl's own green prefix below
    // and prepend the mode-coloured TS glyph instead.
    Element input_area = impl->RenderInputAreaPub();

    // --- 3b. Declared cursor (IME / accessibility) ----------------------
    // Faithful port of TS useDeclaredCursor: park the real terminal cursor
    // at the insertion point so IME preedit renders inline and screen
    // readers / magnifiers can follow the input.
    //
    // The cursor's display column = prefix width + text display width up to
    // the cursor.  The line is the zero-indexed line within the input area.
    {
        namespace dc = cc::ui::common::declared_cursor;
        using Screen = ftxui::Screen;
        const int prefix_w = string_width(opts.prefix);
        const int rel_x = prefix_w + impl->cursor_display_col();
        const int rel_y = impl->cursor_line();
        // Steady bar shape for IME compatibility.  Blinking cursors can
        // interfere with some IME preedit rendering; a steady bar gives
        // the IME a stable anchor point for the composition window.
        // The prompt input is the primary focus target in the REPL screen,
        // so we always declare the cursor position (mirrors TS where
        // `props.focus` is true for the prompt when no dialog is active).
        // TODO: once dialog focus management is wired, gate this on
        // "prompt has focus" instead of always-on.
        input_area = input_area | dc::declared_cursor(
            /*active=*/true, rel_x, rel_y,
            Screen::Cursor::Shape::Bar);
    }

    // --- 4. Autocomplete dropdown (TS ContextSuggestions parity) --------
    Elements box_body;
    if (!s.autocomplete_suggestions.empty()) {
        uic::TextInputOptions ac_opts;
        ac_opts.multiline = false;
        ac_opts.show_line_numbers = false;
        ac_opts.enable_undo_redo = false;
        ac_opts.cursor_blink_ms = 0;
        auto ac_impl = std::make_shared<uic::TextInputImpl>(ac_opts);
        std::vector<uic::Suggestion> sugs;
        sugs.reserve(s.autocomplete_suggestions.size());
        for (const auto& raw : s.autocomplete_suggestions) {
            uic::Suggestion sg;
            sg.text = raw;
            sg.display_text = raw;
            sugs.push_back(std::move(sg));
        }
        // Drive the impl's dropdown renderer by injecting the suggestions
        // list directly — RenderSuggestionsDropdown paints the
        // header/rows/footer faithful to TS ContextSuggestions.
        Element ac_dropdown = ac_impl->RenderSuggestionsFromListPub(
            sugs, std::clamp(s.autocomplete_index, 0,
                             static_cast<int>(sugs.size()) - 1));
        box_body.push_back(hbox({ text("  "), ac_dropdown, filler() }));
    }

    // --- 5. Vim-mode badge (-- INSERT -- / -- NORMAL -- / -- VISUAL --) -
    // Faithful to TS: drawn alongside the prompt, dim+bold, mode-coloured.
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

    // --- 6. Compose -----------------------------------------------------
    box_body.push_back(hbox({
        text(" "),
        text(mp + " ") | color(mc) | bold,
        input_area,
    }));
    box_body.push_back(text(""));

    auto content = vbox(std::move(box_body));
    // TS: borderStyle="round" borderLeft={false} borderRight={false} borderBottom
    // (PromptInput.tsx).  Only the bottom border is visible, colored with
    // promptBorder (medium gray, rgb(153,153,153)).
    return vbox({
        content,
        separator() | color(Color::RGB(153, 153, 153)),
    });
}

// =========================================================
// Dialog routing (delegates each ReplMode to owning UIx agent)
// =========================================================

namespace dialog_stubs {
using Builder = std::function<Element(const ReplScreenState&)>;
namespace dframe = cc::ui::dialogs::frame;
using FrameStyle = dframe::FrameStyle;
using Theme = cc::ui::design::theme::Theme;

/// Build a simple dialog with title + content using DialogFrame.
/// Common pattern for callout/info dialogs.
[[nodiscard]] inline Element SimpleDialog(
    std::string_view title,
    std::string_view subtitle,
    Element content,
    FrameStyle style,
    const Theme& theme)
{
    dframe::DialogFrameProps props;
    props.title = std::string{title};
    if (!subtitle.empty()) props.subtitle = std::string{subtitle};
    props.style = style;
    props.content = std::move(content);
    props.full_border = true;
    props.inner_padding_x = 1;
    props.inner_padding_y = 0;
    return dframe::DialogFrame(props, theme);
}

/// Keyboard hint row (standard pattern: [key] action [key] action ...).
[[nodiscard]] inline Element KeyHintRow(
    std::initializer_list<std::pair<std::string, std::string>> hints,
    Color key_color = Color::Green)
{
    Elements row;
    bool first = true;
    for (const auto& [key, action] : hints) {
        if (!first) row.push_back(text("  "));
        first = false;
        row.push_back(text("[" + key + "]") | bold | color(key_color));
        row.push_back(text(" " + action) | dim);
    }
    return hbox(std::move(row));
}

/// Dispatch table.  Each ReplMode dialog mode maps to a builder.
/// All renders use DialogFrame for faithful TS PermissionDialog visual language.
[[nodiscard]] inline Builder get_builder(ReplMode m) {
    using P = std::pair<ReplMode, Builder>;
    static const P kTable[] = {
      // UI6  MessageSelector — full toolbar in messages_interactions.cppm
      P{ReplMode::MessageSelector, [](const auto&){
         auto theme = cc::ui::design::theme::current_theme();
         auto content = vbox({
             text("Select messages to rewind, edit, or summarize") | center,
             text(""),
             KeyHintRow({
                 {"Up/Down", "navigate"},
                 {"Enter", "edit"},
                 {"s", "summarize"},
             }, Color::Cyan),
             text(""),
             hbox({
                 text("[Esc]") | bold | color(Color::Red),
                 text(" close") | dim,
             }),
         });
         return SimpleDialog("Message Selector", "Rewind / Edit / Summarize",
                             std::move(content), FrameStyle::Info, theme);
      }},
      // UI8  Sandbox (leader + worker) — styled permission panels
      P{ReplMode::SandboxPermission, [](const auto& s){
         auto theme = cc::ui::design::theme::current_theme();
         auto host = s.dialog_ctx.sandbox_host_pattern.value_or("unknown");
         auto content = vbox({
             hbox({
                 text("Host: ") | dim,
                 text(host) | bold,
             }),
             text(""),
             paragraph("Allow network access to this host?"),
             text(""),
             KeyHintRow({
                 {"y", "Allow"},
                 {"a", "Always"},
                 {"n", "Deny"},
             }),
             text(""),
             hbox({
                 text("[Esc]") | bold | color(Color::Red),
                 text(" cancel") | dim,
             }),
         });
         return SimpleDialog("Network Access Request",
                             "Claude wants to access the network",
                             std::move(content),
                             FrameStyle::Warning, theme);
      }},
      P{ReplMode::WorkerSandboxPermission, [](const auto& s){
         auto theme = cc::ui::design::theme::current_theme();
         auto req_id = s.dialog_ctx.sandbox_worker_request_id.value_or("worker");
         auto content = vbox({
             hbox({
                 text("Worker: ") | dim,
                 text(req_id) | bold,
             }),
             text(""),
             paragraph("This worker requests network access."),
             text(""),
             KeyHintRow({
                 {"y", "Allow"},
                 {"a", "Always"},
                 {"n", "Deny"},
             }),
             text(""),
             hbox({
                 text("[Esc]") | bold | color(Color::Red),
                 text(" cancel") | dim,
             }),
         });
         return SimpleDialog("Worker Network Access",
                             "Worker \"" + req_id + "\" requests network access",
                             std::move(content),
                             FrameStyle::Warning, theme);
      }},
      // UI9  ToolPermission — handled by Component path (faithful permission panels)
      P{ReplMode::ToolPermission, [](const auto& s){
         if (!s.permission_request) return Element{};
         namespace sp = cc::ui::permissions::single_prompt;
         sp::SinglePromptProps props;
         props.tool_name = s.permission_request->tool_name;
         props.description = s.permission_request->description;
         if (s.permission_request->file_path)
             props.affected_paths.push_back(*s.permission_request->file_path);
         auto state = std::make_shared<sp::PromptState>();
         state->props = std::move(props);
         return sp::RenderSinglePrompt(state);
      }},
      // UI2  Hook prompt — input request from a pre/post hook
      P{ReplMode::PromptHook, [](const auto&){
         auto theme = cc::ui::design::theme::current_theme();
         auto content = vbox({
             text("A hook requires user input.") | center,
             text(""),
             KeyHintRow({
                 {"Enter", "Submit"},
             }),
             text(""),
             hbox({
                 text("[Esc]") | bold | color(Color::Red),
                 text(" cancel") | dim,
             }),
         });
         return SimpleDialog("Input Required", "Hook prompt",
                             std::move(content),
                             FrameStyle::Info, theme);
      }},
      // UI10 Elicitation (MCP) — server requesting structured input
      P{ReplMode::Elicitation, [](const auto& s){
         auto theme = cc::ui::design::theme::current_theme();
         auto server = s.dialog_ctx.elicitation_server_name.value_or("MCP Server");
         auto content = vbox({
             hbox({
                 text("Server: ") | dim,
                 text(server) | bold,
             }),
             text(""),
             paragraph("The server needs additional input to proceed."),
             text(""),
             KeyHintRow({
                 {"Enter", "Approve"},
             }),
             text(""),
             hbox({
                 text("[Esc]") | bold | color(Color::Red),
                 text(" deny") | dim,
             }),
         });
         return SimpleDialog("MCP Server Request",
                             std::string{server} + " needs additional input",
                             std::move(content),
                             FrameStyle::Info, theme);
      }},
      // UI1  Cost threshold — session cost exceeded
      P{ReplMode::CostThreshold, [](const auto& s){
         auto theme = cc::ui::design::theme::current_theme();
         auto cost = s.dialog_ctx.cost_threshold_usd.value_or(0.0);
         auto current = s.status_bar.cost_usd.value_or(0.0);
         auto content = vbox({
             hbox({
                 text("Current: ") | dim,
                 text(std::format("${:.2f}", current)) | bold
                     | color(Color::Yellow),
             }),
             hbox({
                 text("Threshold: ") | dim,
                 text(std::format("${:.2f}", cost)),
             }),
             hbox({
                 text("Model: ") | dim,
                 text(s.status_bar.model_name),
             }),
             text(""),
             KeyHintRow({
                 {"c", "Continue"},
                 {"r", "Reset counter"},
                 {"q", "Quit"},
             }),
         });
         return SimpleDialog("Cost Threshold Reached",
                             "Session cost has exceeded your configured threshold",
                             std::move(content),
                             FrameStyle::Warning, theme);
      }},
      // UI1  Idle return — session was idle
      P{ReplMode::IdleReturn, [](const auto& s){
         auto theme = cc::ui::design::theme::current_theme();
         auto mins = s.dialog_ctx.idle_return_minutes.value_or(0);
         auto subtitle = mins > 0
             ? std::format("Your session has been idle for {} minutes.", mins)
             : std::string{"Your session has been idle."};
         auto content = vbox({
             text("Would you like to resume where you left off?") | center,
             text(""),
             hbox({
                 text("[Enter]") | bold | color(Color::Green),
                 text(" Resume") | dim,
                 text("  "),
                 text("[n]") | bold | color(Color::Cyan),
                 text(" Start new") | dim,
             }) | center,
         });
         return SimpleDialog("Welcome Back", subtitle, std::move(content),
                             FrameStyle::Info, theme);
      }},
      // UI11 Ultraplan choice — plan proposal
      P{ReplMode::UltraplanChoice, [](const auto& s){
         auto theme = cc::ui::design::theme::current_theme();
         auto blurb = s.dialog_ctx.ultraplan_blurb.value_or(
             "A structured plan has been generated.");
         auto content = vbox({
             paragraph(blurb),
             text(""),
             KeyHintRow({
                 {"Enter", "Accept"},
             }),
             text(""),
             hbox({
                 text("[Esc]") | bold | color(Color::Red),
                 text(" cancel") | dim,
             }),
         });
         return SimpleDialog("Plan Proposal", "Ultraplan",
                             std::move(content),
                             FrameStyle::Info, theme);
      }},
      // UI11 Ultraplan launch
      P{ReplMode::UltraplanLaunch, [](const auto&){
         auto theme = cc::ui::design::theme::current_theme();
         auto content = vbox({
             text("Launch background plan execution?") | center,
             text(""),
             text("Workers will execute the plan steps in parallel.") | dim | center,
             text(""),
             KeyHintRow({
                 {"Enter", "Launch"},
             }),
             text(""),
             hbox({
                 text("[Esc]") | bold | color(Color::Red),
                 text(" cancel") | dim,
             }),
         });
         return SimpleDialog("Ultraplan Launch", "Background execution",
                             std::move(content),
                             FrameStyle::Info, theme);
      }},
      // UI11 IDE Onboarding
      P{ReplMode::IdeOnboarding, [](const auto&){
         auto theme = cc::ui::design::theme::current_theme();
         auto content = vbox({
             text("Install the IDE extension for enhanced editing,") | center,
             text("inline completions, and project awareness.") | center,
             text(""),
             KeyHintRow({
                 {"Enter", "Install"},
             }),
             text(""),
             hbox({
                 text("[Esc]") | bold,
                 text(" dismiss") | dim,
             }),
         });
         return SimpleDialog("IDE Extension Available", "Enhanced editing experience",
                             std::move(content),
                             FrameStyle::Info, theme);
      }},
      // UI11 Init onboarding (first-run wizard)
      P{ReplMode::InitOnboarding, [](const auto&){
         auto theme = cc::ui::design::theme::current_theme();
         auto content = vbox({
             text("Let's get you set up with a quick walkthrough.") | center,
             text("Configure your API key, model, and preferences.") | center,
             text(""),
             KeyHintRow({
                 {"Enter", "Begin setup"},
             }),
             text(""),
             hbox({
                 text("[Esc]") | bold,
                 text(" skip") | dim,
             }),
         });
         return SimpleDialog("Welcome to CC-REPL", "First-time setup",
                             std::move(content),
                             FrameStyle::Success, theme);
      }},
      // UI11 Model switch suggestion
      P{ReplMode::ModelSwitch, [](const auto& s){
         auto theme = cc::ui::design::theme::current_theme();
         auto model = s.dialog_ctx.model_switch_alias.value_or("a new model");
         auto content = vbox({
             text("A better model may be available for this task.") | center,
             text(""),
             KeyHintRow({
                 {"Enter", "Switch"},
             }),
             text(""),
             hbox({
                 text("[Esc]") | bold,
                 text(" dismiss") | dim,
             }),
         });
         return SimpleDialog("Try " + std::string{model} + "?",
                             "Model recommendation",
                             std::move(content),
                             FrameStyle::Info, theme);
      }},
      // UI11 Auto mode callout
      P{ReplMode::UndercoverCallout, [](const auto&){
         auto theme = cc::ui::design::theme::current_theme();
         auto content = vbox({
             text("Automatically approve low-risk tool calls") | center,
             text("(file reads, searches, safe commands).") | center,
             text(""),
             KeyHintRow({
                 {"Enter", "Enable"},
             }),
             text(""),
             hbox({
                 text("[Esc]") | bold,
                 text(" dismiss") | dim,
             }),
         });
         return SimpleDialog("Enable Auto-Approve?", "Auto mode",
                             std::move(content),
                             FrameStyle::Info, theme);
      }},
      // UI11 Effort callout
      P{ReplMode::EffortCallout, [](const auto&){
         auto theme = cc::ui::design::theme::current_theme();
         auto content = vbox({
             text("Higher effort = deeper reasoning, more tokens.") | center,
             text("Lower effort = faster, cheaper responses.") | center,
             text(""),
             KeyHintRow({
                 {"Enter", "Configure"},
             }),
             text(""),
             hbox({
                 text("[Esc]") | bold,
                 text(" dismiss") | dim,
             }),
         });
         return SimpleDialog("Adjust Response Effort", "Effort level",
                             std::move(content),
                             FrameStyle::Warning, theme);
      }},
      // UI11 Remote control callout
      P{ReplMode::RemoteCallout, [](const auto&){
         auto theme = cc::ui::design::theme::current_theme();
         auto content = vbox({
             text("Connect to your IDE for remote editing,") | center,
             text("file sync, and collaborative features.") | center,
             text(""),
             KeyHintRow({
                 {"Enter", "Enable"},
             }),
             text(""),
             hbox({
                 text("[Esc]") | bold,
                 text(" dismiss") | dim,
             }),
         });
         return SimpleDialog("Enable IDE Bridge?", "Remote control",
                             std::move(content),
                             FrameStyle::Info, theme);
      }},
      // UI11 Desktop upsell
      P{ReplMode::DesktopUpsell, [](const auto&){
         auto theme = cc::ui::design::theme::current_theme();
         auto content = vbox({
             text("Rich UI, native notifications, multi-window,") | center,
             text("and file drag-and-drop support.") | center,
             text(""),
             KeyHintRow({
                 {"Enter", "Learn more"},
             }),
             text(""),
             hbox({
                 text("[Esc]") | bold,
                 text(" dismiss") | dim,
             }),
         });
         return SimpleDialog("Try the Desktop App", "Enhanced experience",
                             std::move(content),
                             FrameStyle::Info, theme);
      }},
      // UI16 LSP Recommendation
      P{ReplMode::LspRecommendation, [](const auto& s){
         auto theme = cc::ui::design::theme::current_theme();
         auto ext = s.dialog_ctx.lsp_rec_extension.value_or("language-server");
         auto fext = s.dialog_ctx.lsp_rec_file_ext.value_or("");
         Elements body;
         body.push_back(hbox({
             text("Extension: ") | dim,
             text(ext) | bold,
         }));
         if (!fext.empty())
             body.push_back(hbox({
                 text("For files: ") | dim,
                 text("*." + fext),
             }));
         body.push_back(text(""));
         body.push_back(KeyHintRow({
             {"i", "Install"},
             {"d", "Dismiss"},
         }, Color::Cyan));
         return SimpleDialog("LSP Server Recommended", "Language server suggestion",
                             vbox(std::move(body)),
                             FrameStyle::Info, theme);
      }},
      // UI16 Plugin hint
      P{ReplMode::PluginHint, [](const auto& s){
         auto theme = cc::ui::design::theme::current_theme();
         auto name = s.dialog_ctx.plugin_hint_name.value_or("plugin");
         auto desc = s.dialog_ctx.plugin_hint_description.value_or("");
         Elements body;
         body.push_back(text(name) | bold);
         if (!desc.empty()) body.push_back(text(desc) | dim);
         body.push_back(text(""));
         body.push_back(KeyHintRow({
             {"i", "Install"},
             {"d", "Dismiss"},
         }, Color::Cyan));
         return SimpleDialog("Plugin Suggestion", "Recommended plugin",
                             vbox(std::move(body)),
                             FrameStyle::Info, theme);
      }},
    };
    for (const auto& p : kTable) if (p.first == m) return p.second;
    return nullptr;
}
} // namespace dialog_stubs

/// Top-level dialog router: ReplMode -> overlay Element.
/// Panel modes (Normal / Tasks / Teams / Help / QuickOpen) return nullopt.
/// Priority matches TS getFocusedInputDialog() (REPL.tsx:2013):
///   Exit > message-selector > sandbox > permissions/hook/elicit >
///   cost/idle/ultraplan > onboarding > recs > panels.
[[nodiscard]] inline std::optional<Element> RouteDialog(
    ReplMode m, const ReplScreenState& s) {
    using namespace dialog_stubs;
    switch (m) {
      case ReplMode::Normal: case ReplMode::TasksView:
      case ReplMode::TeamsView: case ReplMode::HelpView:
      case ReplMode::SettingsView: case ReplMode::AboutView:
      case ReplMode::QuickOpen: return std::nullopt;

      case ReplMode::InstallGitHubApp:
      case ReplMode::InstallSlackApp:
      case ReplMode::CreateAgent:
      case ReplMode::EditAgent:
        return std::nullopt;

      default: break; }
    auto b = get_builder(m);
    if (!b) return std::nullopt;
    Element e = b(s);
    if (!e) return std::nullopt;
    return e;
}

// =========================================================
// M7: Queue-based dialog rendering
// =========================================================

namespace dialog_queue_render {

using namespace cc::ui::dialogs::system;
namespace frame = cc::ui::dialogs::frame;

/// Build a DialogRenderContext from REPL screen state.
[[nodiscard]] inline DialogRenderContext make_context(const ReplScreenState& s,
                                                        int term_cols,
                                                        int term_rows) {
    DialogRenderContext ctx;
    ctx.term_cols = term_cols;
    ctx.term_rows = term_rows;
    ctx.theme = cc::ui::design::theme::current_theme();
    ctx.is_fullscreen = false;
    ctx.repl_state = &s;
    return ctx;
}

/// Render the overlay-slot dialog (inside ScrollBox).
/// This is the ToolPermission dialog (band 3).
[[nodiscard]] inline std::optional<Element> RenderOverlayDialog(
    const ReplScreenState& s,
    int term_cols,
    int term_rows)
{
    const auto& queue = s.dialog_queue;
    if (!queue.has_overlay()) return std::nullopt;

    // Suppress while user is typing (matches TS: tool-permission is band 3)
    if (s.is_prompt_input_active) return std::nullopt;

    auto payload = queue.peek_overlay();
    if (!payload) return std::nullopt;

    auto ctx = make_context(s, term_cols, term_rows);
    auto el = s.dialog_renderers.render(payload->get(), ctx);
    if (!el) return std::nullopt;

    // Clamp height to ~60% of terminal (overlay dialog floating in scroll area)
    return el | size(HEIGHT, LESS_THAN, std::max(5, term_rows * 3 / 5));
}

/// Render the bottom-slot dialog (pinned below prompt).
/// These are the "focused input dialogs" per TS getFocusedInputDialog().
[[nodiscard]] inline std::optional<Element> RenderBottomDialog(
    const ReplScreenState& s,
    int term_cols,
    int term_rows)
{
    const auto& queue = s.dialog_queue;
    if (!queue.has_any_bottom()) return std::nullopt;

    auto payload = queue.peek_bottom(s.is_prompt_input_active);
    if (!payload) return std::nullopt;

    auto ctx = make_context(s, term_cols, term_rows);
    auto el = s.dialog_renderers.render(payload->get(), ctx);
    if (!el) return std::nullopt;

    // Bottom dialog sits above the prompt — clamp to ~40% of terminal
    return el | size(HEIGHT, LESS_THAN, std::max(4, term_rows * 2 / 5));
}

/// Render the modal-slot dialog (full-width dbox overlay with ▔ divider).
/// These are the panel dialogs: /settings, /tasks, /help, /plugins, etc.
///
/// NOTE: For backward compatibility, the modal slot is still primarily driven
/// by ReplMode + RouteDialog.  The queue-based modal dialogs are additive —
/// if the queue has a modal dialog, it takes precedence over RouteDialog.
[[nodiscard]] inline std::optional<Element> RenderModalDialog(
    const ReplScreenState& s,
    int term_cols,
    int term_rows)
{
    const auto& queue = s.dialog_queue;
    if (!queue.has_modal()) return std::nullopt;

    auto payload = queue.peek_modal();
    if (!payload) return std::nullopt;

    auto ctx = make_context(s, term_cols, term_rows);
    auto el = s.dialog_renderers.render(payload->get(), ctx);
    if (!el) return std::nullopt;

    // Modal dialog takes most of the screen (typical panel: ~70% height)
    return el | size(HEIGHT, LESS_THAN, std::max(5, term_rows - 4));
}

/// Dispatch an event to the currently active dialog in the queue.
/// Checks slots in priority order: modal > overlay > bottom.
/// Returns true if the event was handled by a queue dialog.
[[nodiscard]] inline bool HandleDialogQueueEvent(
    ReplScreenState& s, const Event& ev) {
    auto& queue = s.dialog_queue;
    if (queue.empty()) return false;

    // Modal slot — highest priority
    if (queue.has_modal()) {
        auto payload = queue.peek_modal_mut();
        if (payload) {
            return s.dialog_renderers.handle_event(payload->get(), ev);
        }
    }

    // Overlay slot (ToolPermission etc.)
    if (queue.has_overlay()) {
        auto payload = queue.peek_overlay_mut();
        if (payload) {
            return s.dialog_renderers.handle_event(payload->get(), ev);
        }
    }

    // Bottom slot — only if prompt not actively typing (matches render suppression rules)
    auto payload = queue.peek_bottom_mut(s.is_prompt_input_active);
    if (payload) {
        return s.dialog_renderers.handle_event(payload->get(), ev);
    }

    return false;
}

/// True if the dialog queue is showing an active dialog.
/// Used by event dispatch logic.
[[nodiscard]] inline bool HasActiveQueueDialog(const ReplScreenState& s) {
    auto& q = s.dialog_queue;
    if (q.has_modal()) return true;
    if (q.has_overlay()) return true;
    if (q.has_any_bottom()) {
        // Bottom dialog may be suppressed by typing; check active band
        auto peek = q.peek_bottom(s.is_prompt_input_active);
        return peek.has_value();
    }
    return false;
}

// ---- ReplMode → DialogQueue bridge (M7.5) ----

/// Returns true if the given ReplMode is bridged to the DialogQueue
/// (i.e. its rendering is handled through queue slots, not RouteDialog
/// or legacy dbox overlays).
///
/// NOTE: ToolPermission is NOT bridged yet — it uses the higher-quality
/// faithful permission panel implementation (bash/file_edit/file_write)
/// via the legacy dbox path.  Upgrade to queue-based rendering is
/// planned for M7.6.
[[nodiscard]] inline bool IsModeBridgedToQueue(ReplMode m) {
    switch (m) {
      case ReplMode::MessageSelector:
      case ReplMode::SandboxPermission:
      case ReplMode::WorkerSandboxPermission:
      case ReplMode::PromptHook:
      case ReplMode::Elicitation:
      case ReplMode::CostThreshold:
      case ReplMode::IdleReturn:
      case ReplMode::UltraplanChoice:
      case ReplMode::UltraplanLaunch:
      case ReplMode::IdeOnboarding:
      case ReplMode::InitOnboarding:
      case ReplMode::ModelSwitch:
      case ReplMode::UndercoverCallout:
      case ReplMode::EffortCallout:
      case ReplMode::RemoteCallout:
      case ReplMode::DesktopUpsell:
      case ReplMode::LspRecommendation:
      case ReplMode::PluginHint:
      case ReplMode::InstallGitHubApp:
      case ReplMode::InstallSlackApp:
      case ReplMode::CreateAgent:
      case ReplMode::EditAgent:
      case ReplMode::TasksView:
      case ReplMode::TeamsView:
      case ReplMode::HelpView:
      case ReplMode::SettingsView:
      case ReplMode::AboutView:
      case ReplMode::QuickOpen:
        return true;
      default:
        return false;
    }
}

/// Synchronize ReplScreenState::mode with the DialogQueue.
///
/// When `s.mode` is set to a dialog mode, this function pushes the
/// corresponding dialog into the queue (if not already present).  The
/// dialog's response callbacks are wrapped so that resolving the
/// dialog also resets `s.mode = Normal` and pops the dialog from the
/// queue.
///
/// Panel modes (TasksView, HelpView, etc.) are bridged to the modal slot
/// as of M7.5 — they use pre-built FTXUI components stored in the payload
/// so state (selected tab, scroll position) persists across renders.
///
/// This is a migration aid: engine code can continue to set `mode`
/// while the UI gradually migrates to queue-based dialog dispatch.
inline void SyncReplModeToQueue(ReplScreenState& s,
                                const std::shared_ptr<ReplScreenCallbacks>& cb)
{
    namespace dsys = cc::ui::dialogs::system;

    // Helper: check if mode should be handled by legacy code paths
    // (Normal is a no-op; ToolPermission uses high-fidelity panels).
    auto is_legacy_only = [](ReplMode m) {
        switch (m) {
          case ReplMode::Normal:
          case ReplMode::ToolPermission:
            return true;
          default: return false;
        }
    };

    if (is_legacy_only(s.mode)) {
        return;
    }

    // For each dialog mode, push the corresponding payload if not
    // already in the queue.
    //
    // Each callback wrapper:
    //   1. Forwards to the original engine callback
    //   2. Pops the dialog from the correct queue slot
    //   3. Resets s.mode = Normal
    //   4. Fires on_mode_change

    auto set_mode_normal = [&]() {
        s.mode = ReplMode::Normal;
        if (cb && cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
    };

    switch (s.mode) {
      // ==== Band 1: MessageSelector (never suppressed by typing) ====
      case ReplMode::MessageSelector: {
        if (s.dialog_queue.contains_type(dsys::DialogType::MessageSelector)) break;
        dsys::MessageSelectorPayload p;
        p.id = "msgsel_bridge";
        p.placeholder = "Select a message";
        p.on_select = [&, set_mode_normal](int idx) {
            (void)idx;
            s.dialog_queue.remove(p.id);
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }

      // ==== Band 2: SandboxPermission ====
      case ReplMode::SandboxPermission: {
        if (s.dialog_queue.contains_type(dsys::DialogType::SandboxPermission)) break;
        dsys::SandboxPermissionPayload p;
        p.id = "sandbox_bridge";
        p.host_pattern = s.dialog_ctx.sandbox_host_pattern.value_or("unknown");
        p.is_worker = false;
        p.on_response = [&, set_mode_normal, cb_p = cb](bool allow, bool always) {
            if (cb_p && cb_p->on_permission_response) {
                cb_p->on_permission_response(allow, always ? std::optional{true} : std::nullopt);
            }
            s.dialog_queue.remove("sandbox_bridge");
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }

      // ==== Band 3: WorkerSandboxPermission ====
      case ReplMode::WorkerSandboxPermission: {
        if (s.dialog_queue.contains_type(dsys::DialogType::WorkerSandboxPermission)) break;
        dsys::WorkerSandboxPermissionPayload p;
        p.id = "worker_sandbox_bridge";
        p.worker_id = s.dialog_ctx.sandbox_worker_request_id.value_or("worker");
        p.tool_name = "worker";
        p.description = "Worker network access request";
        p.on_response = [&, set_mode_normal, cb_p = cb](bool allow, bool always) {
            if (cb_p && cb_p->on_permission_response) {
                cb_p->on_permission_response(allow, always ? std::optional{true} : std::nullopt);
            }
            s.dialog_queue.remove("worker_sandbox_bridge");
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }

      // ==== Band 3: ToolPermission (overlay slot) ====
      case ReplMode::ToolPermission: {
        if (s.dialog_queue.contains_type(dsys::DialogType::ToolPermission)) break;
        if (!s.permission_request) break;
        dsys::ToolPermissionPayload p;
        p.id = "toolperm_bridge";
        p.tool_name = s.permission_request->tool_name;
        p.description = s.permission_request->description;
        p.can_always_allow = true;
        p.on_response = [&, set_mode_normal, cb_p = cb](
            dsys::ToolPermissionPayload::Decision decision, bool sandbox)
        {
            bool allow = (decision == dsys::ToolPermissionPayload::Decision::AllowOnce
                       || decision == dsys::ToolPermissionPayload::Decision::AlwaysAllow);
            bool always = (decision == dsys::ToolPermissionPayload::Decision::AlwaysAllow);
            (void)sandbox;
            if (cb_p && cb_p->on_permission_response) {
                cb_p->on_permission_response(allow, always ? std::optional{true} : std::nullopt);
            }
            s.dialog_queue.remove("toolperm_bridge");
            set_mode_normal();
        };
        p.on_abort = [&, set_mode_normal]() {
            s.dialog_queue.remove("toolperm_bridge");
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }

      // ==== Band 3: PromptDialog (hook prompt) ====
      case ReplMode::PromptHook: {
        if (s.dialog_queue.contains_type(dsys::DialogType::PromptDialog)) break;
        dsys::PromptDialogPayload p;
        p.id = "prompt_bridge";
        p.title = "Input Required";
        p.prompt_text = "A hook requires user input.";
        p.on_response = [&, set_mode_normal, cb_p = cb](std::optional<std::string> value) {
            (void)value;
            if (cb_p && cb_p->on_permission_response) {
                // Map prompt submit → allow (approximate for bridge)
                cb_p->on_permission_response(value.has_value(), std::nullopt);
            }
            s.dialog_queue.remove("prompt_bridge");
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }

      // ==== Band 3: Elicitation (MCP) ====
      case ReplMode::Elicitation: {
        if (s.dialog_queue.contains_type(dsys::DialogType::Elicitation)) break;
        dsys::ElicitationPayload p;
        p.id = "elicit_bridge";
        p.server_name = s.dialog_ctx.elicitation_server_name.value_or("MCP Server");
        p.request_description = "Server needs additional input";
        p.request_id = s.dialog_ctx.elicitation_request_id.value_or(0);
        p.on_response = [&, set_mode_normal, cb_p = cb](bool approve) {
            if (cb_p && cb_p->on_permission_response) {
                cb_p->on_permission_response(approve, std::nullopt);
            }
            s.dialog_queue.remove("elicit_bridge");
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }

      // ==== Band 4: CostThreshold ====
      case ReplMode::CostThreshold: {
        if (s.dialog_queue.contains_type(dsys::DialogType::CostThreshold)) break;
        dsys::CostThresholdPayload p;
        p.id = "cost_bridge";
        p.cost_threshold_usd = s.dialog_ctx.cost_threshold_usd.value_or(0.0);
        p.current_cost_usd = s.status_bar.cost_usd.value_or(0.0);
        p.model_name = s.status_bar.model_name;
        p.on_response = [&, set_mode_normal, cb_p = cb](bool continue_, bool reset) {
            if (cb_p && cb_p->on_dialog_action) {
                // action 0 = continue, 1 = reset, 2 = quit
                int action = continue_ ? 0 : (reset ? 1 : 2);
                cb_p->on_dialog_action(ReplMode::CostThreshold, action);
            }
            s.dialog_queue.remove("cost_bridge");
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }

      // ==== Band 4: IdleReturn ====
      case ReplMode::IdleReturn: {
        if (s.dialog_queue.contains_type(dsys::DialogType::IdleReturn)) break;
        dsys::IdleReturnPayload p;
        p.id = "idle_bridge";
        p.idle_minutes = s.dialog_ctx.idle_return_minutes.value_or(0);
        p.on_response = [&, set_mode_normal](bool resume) {
            (void)resume;
            s.dialog_queue.remove("idle_bridge");
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }

      // ==== Band 4: UltraplanChoice ====
      case ReplMode::UltraplanChoice: {
        if (s.dialog_queue.contains_type(dsys::DialogType::UltraplanChoice)) break;
        dsys::UltraplanChoicePayload p;
        p.id = "ultrachoice_bridge";
        p.plan_name = "Ultraplan";
        p.price_text = s.dialog_ctx.ultraplan_blurb.value_or("");
        p.on_response = [&, set_mode_normal](bool upgrade) {
            (void)upgrade;
            s.dialog_queue.remove("ultrachoice_bridge");
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }

      // ==== Band 4: UltraplanLaunch ====
      case ReplMode::UltraplanLaunch: {
        if (s.dialog_queue.contains_type(dsys::DialogType::UltraplanLaunch)) break;
        dsys::UltraplanLaunchPayload p;
        p.id = "ultralaunch_bridge";
        p.plan_name = "Ultraplan";
        p.on_response = [&, set_mode_normal](bool activate) {
            (void)activate;
            s.dialog_queue.remove("ultralaunch_bridge");
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }

      // ==== Band 5: Onboarding / callouts ====
      case ReplMode::IdeOnboarding: {
        if (s.dialog_queue.contains_type(dsys::DialogType::IdeOnboarding)) break;
        dsys::IdeOnboardingPayload p;
        p.id = "ideonboard_bridge";
        p.ide_name = "IDE Extension";
        p.on_response = [&, set_mode_normal](bool dismiss) {
            (void)dismiss;
            s.dialog_queue.remove("ideonboard_bridge");
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }

      case ReplMode::InitOnboarding: {
        if (s.dialog_queue.contains_type(dsys::DialogType::InitOnboarding)) break;
        dsys::InitOnboardingPayload p;
        p.id = "initonboard_bridge";
        p.on_response = [&, set_mode_normal](bool dismiss) {
            (void)dismiss;
            s.dialog_queue.remove("initonboard_bridge");
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }

      case ReplMode::ModelSwitch: {
        if (s.dialog_queue.contains_type(dsys::DialogType::ModelSwitch)) break;
        dsys::ModelSwitchPayload p;
        p.id = "modelswitch_bridge";
        p.from_model = s.status_bar.model_name;
        p.to_model = s.dialog_ctx.model_switch_alias.value_or("new model");
        p.on_response = [&, set_mode_normal](bool confirm) {
            (void)confirm;
            s.dialog_queue.remove("modelswitch_bridge");
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }

      case ReplMode::UndercoverCallout: {
        if (s.dialog_queue.contains_type(dsys::DialogType::UndercoverCallout)) break;
        dsys::UndercoverCalloutPayload p;
        p.id = "undercover_bridge";
        p.is_active = false;
        p.on_dismiss = [&, set_mode_normal]() {
            s.dialog_queue.remove("undercover_bridge");
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }

      case ReplMode::EffortCallout: {
        if (s.dialog_queue.contains_type(dsys::DialogType::EffortCallout)) break;
        dsys::EffortCalloutPayload p;
        p.id = "effort_bridge";
        p.effort_level = s.status_bar.effort_level.value_or("medium");
        p.on_dismiss = [&, set_mode_normal]() {
            s.dialog_queue.remove("effort_bridge");
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }

      case ReplMode::RemoteCallout: {
        if (s.dialog_queue.contains_type(dsys::DialogType::RemoteCallout)) break;
        dsys::RemoteCalloutPayload p;
        p.id = "remote_bridge";
        p.host = s.status_bar.bridge_connected ? "connected" : "disconnected";
        p.is_connected = s.status_bar.bridge_connected;
        p.on_dismiss = [&, set_mode_normal]() {
            s.dialog_queue.remove("remote_bridge");
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }

      // ==== Band 6: Upsell / Recommendations ====
      case ReplMode::DesktopUpsell: {
        if (s.dialog_queue.contains_type(dsys::DialogType::DesktopUpsell)) break;
        dsys::DesktopUpsellPayload p;
        p.id = "desktop_bridge";
        p.on_response = [&, set_mode_normal](bool upgrade) {
            (void)upgrade;
            s.dialog_queue.remove("desktop_bridge");
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }

      case ReplMode::LspRecommendation: {
        if (s.dialog_queue.contains_type(dsys::DialogType::LspRecommendation)) break;
        dsys::LspRecommendationPayload p;
        p.id = "lsprec_bridge";
        p.server_name = s.dialog_ctx.lsp_rec_extension.value_or("language-server");
        p.on_response = [&, set_mode_normal](bool install) {
            (void)install;
            s.dialog_queue.remove("lsprec_bridge");
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }

      case ReplMode::PluginHint: {
        if (s.dialog_queue.contains_type(dsys::DialogType::PluginHint)) break;
        dsys::PluginHintPayload p;
        p.id = "pluginhint_bridge";
        p.plugin_name = s.dialog_ctx.plugin_hint_name.value_or("plugin");
        p.hint_text = s.dialog_ctx.plugin_hint_description.value_or("");
        p.on_response = [&, set_mode_normal](bool enable) {
            (void)enable;
            s.dialog_queue.remove("pluginhint_bridge");
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }

      // ==== Modal: InstallGitHubAppWizard ====
      case ReplMode::InstallGitHubApp: {
        if (s.dialog_queue.contains_type(dsys::DialogType::InstallGitHubAppWizard)) break;
        dsys::InstallGitHubAppWizardPayload p;
        p.id = "github_wiz_bridge";
        // Pre-create the component so first render shows the real wizard
        // (not a placeholder — matches legacy render-on-first-frame behavior).
        namespace gh_wiz = cc::ui::dialogs::install_github_app_wizard;
        gh_wiz::InstallGitHubAppWizardOptions opts;
        opts.on_complete = [&, set_mode_normal](auto&&) {
            s.dialog_queue.remove("github_wiz_bridge");
            set_mode_normal();
        };
        opts.on_cancel = [&, set_mode_normal]() {
            s.dialog_queue.remove("github_wiz_bridge");
            set_mode_normal();
        };
        p.component = std::make_shared<Component>(
            gh_wiz::MakeInstallGitHubAppWizard(std::move(opts)));
        p.on_complete = [](bool) {};  // handled by component callbacks above
        s.dialog_queue.push(std::move(p));
        break;
      }

      // ==== Modal: InstallSlackAppWizard ====
      case ReplMode::InstallSlackApp: {
        if (s.dialog_queue.contains_type(dsys::DialogType::InstallSlackAppWizard)) break;
        dsys::InstallSlackAppWizardPayload p;
        p.id = "slack_wiz_bridge";
        namespace sk_wiz = cc::ui::dialogs::install_slack_app_wizard;
        sk_wiz::InstallSlackAppWizardOptions opts;
        opts.on_complete = [&, set_mode_normal]() {
            s.dialog_queue.remove("slack_wiz_bridge");
            set_mode_normal();
        };
        opts.on_cancel = [&, set_mode_normal]() {
            s.dialog_queue.remove("slack_wiz_bridge");
            set_mode_normal();
        };
        p.component = std::make_shared<Component>(
            sk_wiz::MakeInstallSlackAppWizard(std::move(opts)));
        p.on_complete = [](bool) {};  // handled by component callbacks above
        s.dialog_queue.push(std::move(p));
        break;
      }

      // ==== Modal: CreateAgentWizard ====
      case ReplMode::CreateAgent: {
        if (s.dialog_queue.contains_type(dsys::DialogType::CreateAgentWizard)) break;
        dsys::CreateAgentWizardPayload p;
        p.id = "create_agent_wiz_bridge";
        namespace aw = cc::ui::agents::wizard;
        aw::AgentWizardOptions opts;
        opts.on_save = [&, set_mode_normal, cb_p = cb](const aw::WizardDraft& draft) {
            if (cb_p && cb_p->save_agent_from_wizard) {
                cb_p->save_agent_from_wizard(draft);
            }
            s.dialog_queue.remove("create_agent_wiz_bridge");
            set_mode_normal();
        };
        opts.on_cancel = [&, set_mode_normal]() {
            s.dialog_queue.remove("create_agent_wiz_bridge");
            set_mode_normal();
        };
        p.component = std::make_shared<Component>(
            aw::AgentWizard(std::move(opts)));
        p.on_complete = [](bool, std::string_view) {};  // handled by component callbacks
        s.dialog_queue.push(std::move(p));
        break;
      }

      // ==== Modal: EditAgentWizard ====
      case ReplMode::EditAgent: {
        if (s.dialog_queue.contains_type(dsys::DialogType::EditAgentWizard)) break;
        dsys::EditAgentWizardPayload p;
        p.id = "edit_agent_wiz_bridge";
        p.agent_name = s.dialog_ctx.agent_wizard_agent_id.value_or("");
        namespace aw = cc::ui::agents::wizard;
        namespace cards = cc::ui::agents::cards;
        aw::AgentWizardOptions opts;
        if (s.dialog_ctx.agent_wizard_agent_id && cb && cb->load_agent_for_wizard) {
            opts.edit_agent = cb->load_agent_for_wizard(*s.dialog_ctx.agent_wizard_agent_id);
        } else {
            cards::AgentCardData card;
            card.name = p.agent_name;
            opts.edit_agent = std::move(card);
        }
        opts.on_save = [&, set_mode_normal, cb_p = cb](const aw::WizardDraft& draft) {
            if (cb_p && cb_p->save_agent_from_wizard) {
                cb_p->save_agent_from_wizard(draft);
            }
            s.dialog_queue.remove("edit_agent_wiz_bridge");
            set_mode_normal();
        };
        opts.on_cancel = [&, set_mode_normal]() {
            s.dialog_queue.remove("edit_agent_wiz_bridge");
            set_mode_normal();
        };
        p.component = std::make_shared<Component>(
            aw::AgentWizard(std::move(opts)));
        p.on_complete = [](bool) {};  // handled by component callbacks
        s.dialog_queue.push(std::move(p));
        break;
      }

      // ==== Panel views (modal slot) ====
      case ReplMode::TasksView: {
        if (s.dialog_queue.contains_type(dsys::DialogType::TasksView)) break;
        dsys::TasksViewPayload p;
        p.id = "tasks_view_bridge";
        p.on_close = [&, set_mode_normal]() {
            s.dialog_queue.remove("tasks_view_bridge");
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }
      case ReplMode::TeamsView: {
        if (s.dialog_queue.contains_type(dsys::DialogType::TeamsView)) break;
        dsys::TeamsViewPayload p;
        p.id = "teams_view_bridge";
        p.on_close = [&, set_mode_normal]() {
            s.dialog_queue.remove("teams_view_bridge");
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }
      case ReplMode::HelpView: {
        if (s.dialog_queue.contains_type(dsys::DialogType::HelpView)) break;
        namespace hv = cc::ui::dialogs::help_view;
        dsys::HelpViewPayload p;
        p.id = "help_view_bridge";
        // Pre-create component so state persists and first render is real
        hv::HelpViewProps props;
        props.app_version = s.app_version;
        props.builtin_commands = hv::default_builtin_commands();
        props.on_close = [&, set_mode_normal]() {
            s.dialog_queue.remove("help_view_bridge");
            set_mode_normal();
        };
        props.on_run_command = [&, set_mode_normal, cb](std::string_view cmd_name) {
            if (cb && cb->enqueue_slash_command) {
                cb->enqueue_slash_command(std::string{"/"} + std::string{cmd_name});
            }
            s.dialog_queue.remove("help_view_bridge");
            set_mode_normal();
        };
        auto state = std::make_shared<hv::HelpViewState>();
        s.help_view_state = state;  // keep state ref in screen state
        auto theme = cc::ui::design::theme::current_theme();
        p.component = std::make_shared<Component>(
            hv::MakeHelpView(std::move(state), std::move(props), theme));
        p.on_close = []() {};  // handled by component callbacks
        s.dialog_queue.push(std::move(p));
        break;
      }
      case ReplMode::SettingsView: {
        if (s.dialog_queue.contains_type(dsys::DialogType::SettingsPanel)) break;
        namespace sv = cc::ui::dialogs::settings_view;
        dsys::SettingsPanelPayload p;
        p.id = "settings_view_bridge";
        // Pre-create component so state persists and first render is real
        sv::SettingsViewProps props;
        props.app_version = s.app_version;
        props.model_name = s.status_bar.model_name;
        props.default_model = s.settings_model;
        props.vim_mode = s.input_mode == InputMode::VimNormal ||
                         s.input_mode == InputMode::VimInsert ||
                         s.input_mode == InputMode::VimVisual;
        props.brief_mode = s.status_bar.is_brief_mode;
        props.auto_mode = s.status_bar.is_auto_mode;
        props.fast_mode = s.status_bar.is_fast_mode;
        props.on_close = [&, set_mode_normal]() {
            s.dialog_queue.remove("settings_view_bridge");
            set_mode_normal();
        };
        props.on_open_config = [&, set_mode_normal, cb]() {
            if (cb && cb->enqueue_slash_command) {
                cb->enqueue_slash_command("/config");
            }
            s.dialog_queue.remove("settings_view_bridge");
            set_mode_normal();
        };
        auto state = std::make_shared<sv::SettingsViewState>();
        s.settings_view_state = state;  // keep state ref in screen state
        auto theme = cc::ui::design::theme::current_theme();
        p.component = std::make_shared<Component>(
            sv::MakeSettingsView(std::move(state), std::move(props), theme));
        p.on_close = []() {};  // handled by component callbacks
        s.dialog_queue.push(std::move(p));
        break;
      }
      case ReplMode::AboutView: {
        if (s.dialog_queue.contains_type(dsys::DialogType::AboutDialog)) break;
        namespace av = cc::ui::dialogs::about;
        dsys::AboutDialogPayload p;
        p.id = "about_view_bridge";
        p.version = s.app_version;
        // Pre-create component so first render is real
        av::AboutDialogProps props;
        props.app_version = s.app_version;
        props.on_close = [&, set_mode_normal]() {
            s.dialog_queue.remove("about_view_bridge");
            set_mode_normal();
        };
        auto theme = cc::ui::design::theme::current_theme();
        p.component = std::make_shared<Component>(
            av::MakeAboutDialog(std::move(props), theme));
        p.on_close = []() {};  // handled by component callbacks
        s.dialog_queue.push(std::move(p));
        break;
      }
      case ReplMode::QuickOpen: {
        if (s.dialog_queue.contains_type(dsys::DialogType::QuickOpen)) break;
        dsys::QuickOpenPayload p;
        p.id = "quick_open_bridge";
        p.on_result = [&, set_mode_normal](int idx, bool confirmed) {
            (void)idx; (void)confirmed;
            s.dialog_queue.remove("quick_open_bridge");
            set_mode_normal();
        };
        s.dialog_queue.push(std::move(p));
        break;
      }

      default:
        break;
    }
}

} // namespace dialog_queue_render

// =========================================================
// Full layout composition
// =========================================================

/// M1: Composed via the FullscreenLayout slot-system (faithful port of TS
/// FullscreenLayout.tsx).  The previously-flat top-to-bottom vbox is now
/// slotted:
///   scrollable slot (flexGrow region) =
///     [WelcomeHeader (fresh session)] | Messages | Spinner | Tasks/Teams
///   bottom slot (pinned, flexShrink=0) =
///     StatusBar | PromptInput | Footer (counts)
///   modal slot (dbox overlay, bottom-anchored) =
///     RouteDialog() when non-null (with MODAL_TRANSCRIPT_PEEK peek)
///   overlay slot =
///     (reserved — engine wires PermissionRequest here in a future milestone;
///      currently permission flows through RouteDialog as the modal slot,
///      matching how it rendered before.)
///
/// All existing render functions are PRESERVED (RenderWelcomeHeader,
/// RenderMessages, RenderSpinner, RenderStatusBar, RenderPromptInput,
/// RouteDialog) — only HOW they are composed changed.  Visual order is
/// preserved: messages scroll above, status/prompt pinned below.
///
/// Fix #1: welcome header atop the list on a fresh session.
/// Fix #7: StatusBar just above prompt input (TS bottom status line).
/// Fix #11: terminal size probed once per frame for adaptive clamping.
[[nodiscard]] inline Element RenderReplScreen(const ReplScreenState& s) {
    // Probe terminal size once per frame for adaptive layout (fix #11).
    auto [term_cols, term_rows] = cc::ui::ink_utils::query_terminal_size();
    if (term_cols <= 0) term_cols = 80;
    if (term_rows <= 0) term_rows = 24;

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
    // message transcript area: welcome header (fresh session), messages,
    // spinner, tasks/teams panel.
    Elements scroll_rows; scroll_rows.reserve(6);
    if (s.messages.empty() && s.spinner_mode == SpinnerMode::Hidden)
        scroll_rows.push_back(RenderWelcomeHeader(s, spinner_frame));
    scroll_rows.push_back(RenderMessages(s.messages, s.selected_message_idx,
                                         s.viewport_height_lines, s.scroll_offset,
                                         s.scroll_pinned_to_bottom, spinner_frame));
    if (s.spinner_mode != SpinnerMode::Hidden)
        scroll_rows.push_back(RenderSpinner(s.spinner_mode, s.spinner_verb, s.spinner_tip));
    // M7.5: Panel views (Tasks/Teams/Help/Settings/About/QuickOpen) are
    // now rendered as modal dialogs via DialogQueue — no longer inlined
    // in the scrollable slot.
    slots.scrollable = vbox(std::move(scroll_rows));

    // ── bottom slot (pinned, flexShrink=0) ──────────────────────────────
    // Faithful to TS PromptInputFooter structure:
    //   separator  →  prompt input  →  footer (left/right columns)
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
    //   - Shown when settings.statusLine is configured (status_line_enabled)
    //   - content comes from executing the user's shell command
    //   - In fullscreen, reserves a row even while loading (stable height)
    //   - Text may contain ANSI escape codes for coloring
    pif::StatusLineOptions status_line_opts;
    status_line_opts.content = s.status_line_text;
    status_line_opts.should_display =
        s.status_line_enabled && !s.status_line_command.empty();
    status_line_opts.is_fullscreen = cc::utils::is_fullscreen_enabled();
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

    // Map to VimMode
    pif::VimMode vim_mode = pif::VimMode::None;
    bool vim_enabled = false;
    if (s.input_mode == InputMode::VimInsert) {
        vim_mode = pif::VimMode::Insert; vim_enabled = true;
    } else if (s.input_mode == InputMode::VimNormal) {
        vim_mode = pif::VimMode::Normal; vim_enabled = true;
    } else if (s.input_mode == InputMode::VimVisual) {
        vim_mode = pif::VimMode::Visual; vim_enabled = true;
    }

    // Build ModeIndicator options
    pif::ModeIndicatorOptions mode_opts;
    mode_opts.mode = footer_mode;
    // TODO: wire real permission mode from engine (default for now)
    mode_opts.permission_mode = pif::PermissionMode::Default;
    mode_opts.is_loading = s.spinner_mode != SpinnerMode::Hidden;
    mode_opts.tasks_selected = s.mode == ReplMode::TasksView;
    mode_opts.teams_selected = s.mode == ReplMode::TeamsView;
    mode_opts.background_task_count = s.background_task_count;
    mode_opts.teammate_count = s.teammate_count;
    mode_opts.show_hint = true;
    mode_opts.esc_shortcut = "esc";
    mode_opts.todos_shortcut = "ctrl+t";

    // Build LeftSide options
    pif::LeftSideOptions left_opts;
    left_opts.vim_mode = vim_mode;
    left_opts.vim_enabled = vim_enabled;
    left_opts.is_searching = s.input_mode == InputMode::HistorySearch;
    // TODO: wire history query from engine
    left_opts.history_query = "";
    left_opts.history_failed_match = false;
    left_opts.mode_indicator = std::move(mode_opts);

    // Build Bridge options
    pif::BridgeOptions bridge_opts;
    if (s.status_bar.bridge_connected) {
        bridge_opts.status = pif::BridgeStatus::Connected;
        bridge_opts.selected = false;  // TODO: wire bridge selection
        bridge_opts.explicit_remote = false;  // TODO: wire from config
    }

    // Assemble full footer options
    pif::FooterOptions footer_opts;
    footer_opts.status_line = std::move(status_line_opts);
    footer_opts.left_side = std::move(left_opts);
    footer_opts.bridge = std::move(bridge_opts);
    footer_opts.is_fullscreen = cc::utils::is_fullscreen_enabled();
    footer_opts.is_narrow = false;  // TODO: probe term width

    Elements bottom_rows; bottom_rows.reserve(6);

    // M7: Bottom-slot dialogs (focusedInputDialog set) render ABOVE the prompt
    // when present, similar to TS focusedInputDialog behavior.
    namespace dqr = dialog_queue_render;
    auto bottom_dlg = dqr::RenderBottomDialog(s, term_cols, term_rows);
    if (bottom_dlg) {
        bottom_rows.push_back(*bottom_dlg);
    }

    bottom_rows.push_back(separator());
    bottom_rows.push_back(RenderPromptInput(s));
    bottom_rows.push_back(pif::RenderPromptInputFooter(footer_opts));
    slots.bottom = vbox(std::move(bottom_rows));

    // ── overlay slot (inside ScrollBox) ────────────────────────────────
    // M7: Queue-based overlay dialogs (e.g. ToolPermission).
    // Rendered inside the ScrollBox, bottom-aligned, floating over messages.
    auto overlay_dlg = dqr::RenderOverlayDialog(s, term_cols, term_rows);
    if (overlay_dlg) {
        slots.overlay = std::move(*overlay_dlg);
    }

    // ── modal slot (dbox overlay) ───────────────────────────────────────
    // M7: Queue-based modal dialogs take priority over legacy RouteDialog.
    // RouteDialog() still works for legacy dialog modes (backward compat).
    auto modal_dlg = dqr::RenderModalDialog(s, term_cols, term_rows);
    if (modal_dlg) {
        slots.modal = std::move(*modal_dlg);
    } else {
        // M7.5: Skip RouteDialog for modes that are bridged to the queue
        // (they render via bottom/overlay slots instead of modal overlay).
        namespace dqr2 = dialog_queue_render;
        if (!dqr2::IsModeBridgedToQueue(s.mode)) {
            auto legacy_dlg = RouteDialog(s.mode, s);
            if (legacy_dlg) {
                // Fix #11: clamp the dialog overlay height so it cannot overflow.
                slots.modal = std::move(*legacy_dlg)
                    | size(HEIGHT, LESS_THAN, std::max(4, term_rows - 2));
            }
        }
    }

    // ── chrome-driver inputs (scroll-derived) ──────────────────────────
    // Wired to state fields added in M1; dormant by default (pill_visible
    // false, sticky_prompt_text empty) until the engine drives them.
    slots.sticky_prompt_text = s.sticky_prompt_text;
    slots.pill_visible       = s.pill_visible && !s.scroll_pinned_to_bottom;
    slots.new_message_count  = s.unseen_message_count;
    slots.pill_actionable    = slots.pill_visible;

    return fl::ComposeFullscreen(std::move(slots));
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
        if (s->dialog_ctx.agent_wizard_agent_id && cb->load_agent_for_wizard) {
            opts.edit_agent = cb->load_agent_for_wizard(*s->dialog_ctx.agent_wizard_agent_id);
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
// Permission panel helpers (M6 — faithful port)
// -------------------------------------------------------------------

namespace bash_ns = cc::ui::permissions::bash_prompt;
namespace fedit_ns = cc::ui::permissions::file_edit;
namespace fwrite_ns = cc::ui::permissions::file_write;

/// Build a BashPromptProps from a PermissionRequestInfo.
[[nodiscard]] inline bash_ns::BashPromptProps build_bash_props(
    const PermissionRequestInfo& info)
{
    bash_ns::BashPromptProps p;
    p.command = info.bash_command.value_or(info.description);
    p.description = info.description;
    p.working_dir = info.bash_working_dir;
    p.is_destructive = info.bash_is_destructive;
    p.destructive_reason = info.bash_destructive_reason;
    if (!info.risk_labels.empty()) {
        p.matched_rules = info.risk_labels;
        p.rule_explanation = info.risk_labels.front();
    }
    p.show_always_allow = info.can_always_allow;
    return p;
}

/// Build a FileEditPermissionProps from a PermissionRequestInfo.
[[nodiscard]] inline fedit_ns::FileEditPermissionProps build_file_edit_props(
    const PermissionRequestInfo& info)
{
    fedit_ns::FileEditPermissionProps p;
    p.file_path = info.file_path.value_or("");
    p.old_string = info.file_old_content.value_or("");
    p.new_string = info.file_new_content.value_or("");
    p.replace_all = info.file_replace_all;
    if (info.file_filename) p.filename = *info.file_filename;
    if (info.file_relative_path) p.relative_path = *info.file_relative_path;
    p.max_visible_lines = 20;
    p.language = info.file_language;
    p.in_allowed_path = true;
    return p;
}

/// Build a FileWritePermissionProps from a PermissionRequestInfo.
[[nodiscard]] inline fwrite_ns::FileWritePermissionProps build_file_write_props(
    const PermissionRequestInfo& info)
{
    fwrite_ns::FileWritePermissionProps p;
    p.file_path = info.file_path.value_or("");
    p.content = info.file_new_content.value_or("");
    p.old_content = info.file_old_content.value_or("");
    p.file_exists = info.file_exists;
    if (info.file_filename) p.filename = *info.file_filename;
    if (info.file_relative_path) p.relative_path = *info.file_relative_path;
    p.max_visible_lines = 20;
    p.language = info.file_language;
    p.in_allowed_path = true;
    return p;
}

/// Lazily create (or re-create) the permission panel component.
/// Dispatches to the correct faithful panel based on tool_kind.
/// Rebuilds the component if tool_kind changes.
[[nodiscard]] inline std::shared_ptr<Component> get_permission_panel(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb)
{
    if (!s->permission_request) return nullptr;
    const auto& info = *s->permission_request;

    // Build a cache key from core identifying fields.
    // This ensures we rebuild when content changes, even for the same tool_kind
    // (e.g. two consecutive Generic/Bash requests with different details).
    std::string cache_key = info.tool_name + "|" + info.description + "|" +
        (info.file_path.value_or("")) + "|" +
        std::to_string(static_cast<int>(info.tool_kind));

    // Rebuild if kind changed, or content changed, or panel doesn't exist
    bool needs_rebuild = !s->permission_panel ||
                         s->permission_panel_kind != info.tool_kind ||
                         s->permission_panel_cache_key != cache_key;

    if (needs_rebuild) {
        s->permission_panel_kind = info.tool_kind;
        s->permission_panel_cache_key = std::move(cache_key);

        switch (info.tool_kind) {
          case PermissionToolKind::Bash: {
            auto props = build_bash_props(info);
            // Wire decision callback
            props.on_decide = [s, cb](bash_ns::Decision d,
                                       std::string_view /*prefix*/,
                                       std::string_view feedback) {
                std::string decision_str = "allow_once";
                std::string scope_str = "session";
                switch (d) {
                  case bash_ns::Decision::AllowOnce:
                    decision_str = "allow_once"; break;
                  case bash_ns::Decision::AllowWithPrefix:
                    decision_str = "allow_always";
                    scope_str = "prefix"; break;
                  case bash_ns::Decision::Deny:
                    decision_str = "deny"; break;
                  case bash_ns::Decision::Abort:
                    decision_str = "abort"; break;
                }
                if (cb->on_permission_decision) {
                    cb->on_permission_decision(decision_str, scope_str, feedback);
                } else if (cb->on_permission_response) {
                    // Fallback to legacy callback
                    bool allow = (d != bash_ns::Decision::Deny &&
                                  d != bash_ns::Decision::Abort);
                    std::optional<bool> always =
                        (d == bash_ns::Decision::AllowWithPrefix)
                            ? std::optional<bool>(true) : std::nullopt;
                    cb->on_permission_response(allow, always);
                }
                s->mode = ReplMode::Normal;
                if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
            };
            props.on_abort = [s, cb] {
                if (cb->on_permission_response)
                    cb->on_permission_response(false, std::nullopt);
                s->mode = ReplMode::Normal;
                if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
            };
            s->permission_panel = std::make_shared<Component>(
                bash_ns::MakeBashPermissionPrompt(std::move(props)));
            break;
          }
          case PermissionToolKind::FileEdit: {
            auto props = build_file_edit_props(info);
            props.on_decide = [s, cb](fedit_ns::Decision d,
                                       fedit_ns::SessionScope scope,
                                       std::string_view feedback) {
                std::string decision_str = "allow_once";
                std::string scope_str = "session";
                switch (d) {
                  case fedit_ns::Decision::AllowOnce:
                    decision_str = "allow_once"; break;
                  case fedit_ns::Decision::AllowSession:
                    decision_str = "allow_always";
                    switch (scope) {
                      case fedit_ns::SessionScope::Default:
                        scope_str = "session"; break;
                      case fedit_ns::SessionScope::ClaudeFolder:
                        scope_str = "claude_folder"; break;
                      case fedit_ns::SessionScope::GlobalClaudeFolder:
                        scope_str = "global_claude_folder"; break;
                    }
                    break;
                  case fedit_ns::Decision::Deny:
                    decision_str = "deny"; break;
                  case fedit_ns::Decision::Abort:
                    decision_str = "abort"; break;
                }
                if (cb->on_permission_decision) {
                    cb->on_permission_decision(decision_str, scope_str, feedback);
                } else if (cb->on_permission_response) {
                    bool allow = (d == fedit_ns::Decision::AllowOnce ||
                                  d == fedit_ns::Decision::AllowSession);
                    std::optional<bool> always =
                        (d == fedit_ns::Decision::AllowSession)
                            ? std::optional<bool>(true) : std::nullopt;
                    cb->on_permission_response(allow, always);
                }
                s->mode = ReplMode::Normal;
                if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
            };
            s->permission_panel = std::make_shared<Component>(
                fedit_ns::MakeFileEditPermissionPrompt(std::move(props)));
            break;
          }
          case PermissionToolKind::FileWrite: {
            auto props = build_file_write_props(info);
            props.on_decide = [s, cb](fwrite_ns::Decision d,
                                       fwrite_ns::SessionScope scope,
                                       std::string_view feedback) {
                std::string decision_str = "allow_once";
                std::string scope_str = "session";
                switch (d) {
                  case fwrite_ns::Decision::AllowOnce:
                    decision_str = "allow_once"; break;
                  case fwrite_ns::Decision::AllowSession:
                    decision_str = "allow_always";
                    switch (scope) {
                      case fwrite_ns::SessionScope::Default:
                        scope_str = "session"; break;
                      case fwrite_ns::SessionScope::ClaudeFolder:
                        scope_str = "claude_folder"; break;
                      case fwrite_ns::SessionScope::GlobalClaudeFolder:
                        scope_str = "global_claude_folder"; break;
                    }
                    break;
                  case fwrite_ns::Decision::Deny:
                    decision_str = "deny"; break;
                  case fwrite_ns::Decision::Abort:
                    decision_str = "abort"; break;
                }
                if (cb->on_permission_decision) {
                    cb->on_permission_decision(decision_str, scope_str, feedback);
                } else if (cb->on_permission_response) {
                    bool allow = (d == fwrite_ns::Decision::AllowOnce ||
                                  d == fwrite_ns::Decision::AllowSession);
                    std::optional<bool> always =
                        (d == fwrite_ns::Decision::AllowSession)
                            ? std::optional<bool>(true) : std::nullopt;
                    cb->on_permission_response(allow, always);
                }
                s->mode = ReplMode::Normal;
                if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
            };
            s->permission_panel = std::make_shared<Component>(
                fwrite_ns::MakeFileWritePermissionPrompt(std::move(props)));
            break;
          }
          default: {
            // Generic fallback — SinglePrompt with DetailGeneric.
            // Handles any tool that doesn't have a specialized panel.
            namespace sp = cc::ui::permissions::single_prompt;
            sp::SinglePromptProps props;
            props.tool_name = info.tool_name;
            props.description = info.description;
            props.action_kind = sp::ActionKind::Other;
            props.risk_level = sp::RiskLevel::Medium;
            if (info.file_path)
                props.affected_paths.push_back(*info.file_path);
            props.detail = sp::DetailGeneric{info.description};

            props.on_decide = [s, cb](sp::Decision d,
                                        bool /*sandbox*/) {
                if (cb->on_permission_response) {
                    bool allow = (d == sp::Decision::AllowOnce ||
                                  d == sp::Decision::AlwaysAllow);
                    std::optional<bool> always =
                        (d == sp::Decision::AlwaysAllow)
                            ? std::optional<bool>(true) : std::nullopt;
                    cb->on_permission_response(allow, always);
                }
                s->mode = ReplMode::Normal;
                if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
            };
            props.on_abort = [s, cb] {
                if (cb->on_permission_response)
                    cb->on_permission_response(false, std::nullopt);
                s->mode = ReplMode::Normal;
                if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
            };
            s->permission_panel = std::make_shared<Component>(
                sp::MakeSinglePromptDialog(std::move(props)));
            break;
          }
        }
    }
    return std::static_pointer_cast<Component>(s->permission_panel);
}

/// Render the permission panel content as an Element.
[[nodiscard]] inline Element render_permission_panel(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    auto panel = get_permission_panel(s, cb);
    return panel ? (*panel)->Render() : text("");
}

/// Forward an event to the permission panel component.
inline bool forward_permission_panel(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb,
    Event ev) {
    auto panel = get_permission_panel(s, cb);
    if (panel && (*panel)->OnEvent(std::move(ev))) return true;

    // ESC dismisses for generic fallback and as a safety net
    if (ev == Event::Escape) {
        if (cb->on_permission_response)
            cb->on_permission_response(false, std::nullopt);
        s->mode = ReplMode::Normal;
        if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        return true;
    }
    return false;
}

/// Reset (destroy) the permission panel so the next entry starts fresh.
inline void reset_permission_panel(const std::shared_ptr<ReplScreenState>& s) {
    s->permission_panel.reset();
    s->permission_panel_kind = PermissionToolKind::Generic;
    s->permission_panel_cache_key.clear();
}

// ============================================================
// HelpView dialog (M7 — faithful port)
// ============================================================

namespace hv = cc::ui::dialogs::help_view;

/// Lazily create (or return) the HelpView component.
/// Faithful to TS HelpV2.tsx with 3 tabs: general / commands / custom-commands.
[[nodiscard]] inline std::shared_ptr<Component> get_help_view(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb)
{
    if (!s->help_view_component) {
        // Create state
        auto state = std::make_shared<hv::HelpViewState>();
        s->help_view_state = state;

        hv::HelpViewProps props;
        props.app_version = s->app_version;
        props.builtin_commands = hv::default_builtin_commands();
        props.on_close = [s, cb]() {
            s->mode = ReplMode::Normal;
            s->help_view_component.reset();
            if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        };
        props.on_run_command = [s, cb](std::string_view cmd_name) {
            // Enqueue the slash command
            if (cb->enqueue_slash_command) {
                cb->enqueue_slash_command(std::string{"/"} + std::string{cmd_name});
            }
            // Close help view
            s->mode = ReplMode::Normal;
            s->help_view_component.reset();
            if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        };

        auto theme = cc::ui::design::theme::current_theme();
        s->help_view_component = std::make_shared<Component>(
            hv::MakeHelpView(std::move(state), std::move(props), theme));
    }
    return std::static_pointer_cast<Component>(s->help_view_component);
}

/// Render the help view component.
[[nodiscard]] inline Element render_help_view(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb)
{
    auto panel = get_help_view(s, cb);
    return panel ? (*panel)->Render() : text("");
}

/// Forward an event to the help view component.
inline bool forward_help_view(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb,
    Event ev)
{
    auto panel = get_help_view(s, cb);
    if (panel && (*panel)->OnEvent(std::move(ev))) return true;

    // ESC as fallback
    if (ev == Event::Escape) {
        s->mode = ReplMode::Normal;
        s->help_view_component.reset();
        if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        return true;
    }
    return false;
}

/// Reset (destroy) the help view so the next entry starts fresh.
inline void reset_help_view(const std::shared_ptr<ReplScreenState>& s) {
    s->help_view_component.reset();
    s->help_view_state.reset();
}

// ============================================================
// SettingsView dialog (M7 — faithful port)
// ============================================================

namespace sv = cc::ui::dialogs::settings_view;

/// Lazily create (or return) the SettingsView component.
[[nodiscard]] inline std::shared_ptr<Component> get_settings_view(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb)
{
    if (!s->settings_view_component) {
        auto state = std::make_shared<sv::SettingsViewState>();
        s->settings_view_state = state;

        sv::SettingsViewProps props;
        props.app_version = s->app_version;
        props.model_name = s->status_bar.model_name;
        props.default_model = s->settings_model;
        props.on_close = [s, cb]() {
            s->mode = ReplMode::Normal;
            s->settings_view_component.reset();
            if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        };
        props.on_open_config = [s, cb]() {
            if (cb->enqueue_slash_command) {
                cb->enqueue_slash_command("/config");
            }
            s->mode = ReplMode::Normal;
            s->settings_view_component.reset();
            if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        };

        auto theme = cc::ui::design::theme::current_theme();
        s->settings_view_component = std::make_shared<Component>(
            sv::MakeSettingsView(std::move(state), std::move(props), theme));
    }
    return std::static_pointer_cast<Component>(s->settings_view_component);
}

/// Render the settings view component.
[[nodiscard]] inline Element render_settings_view(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb)
{
    auto panel = get_settings_view(s, cb);
    return panel ? (*panel)->Render() : text("");
}

/// Forward an event to the settings view component.
inline bool forward_settings_view(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb,
    Event ev)
{
    auto panel = get_settings_view(s, cb);
    if (panel && (*panel)->OnEvent(std::move(ev))) return true;

    if (ev == Event::Escape) {
        s->mode = ReplMode::Normal;
        s->settings_view_component.reset();
        if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        return true;
    }
    return false;
}

/// Reset (destroy) the settings view so the next entry starts fresh.
inline void reset_settings_view(const std::shared_ptr<ReplScreenState>& s) {
    s->settings_view_component.reset();
    s->settings_view_state.reset();
}

// ============================================================
// AboutView dialog (M7 — faithful port)
// ============================================================

namespace av = cc::ui::dialogs::about;

/// Lazily create (or return) the AboutView component.
[[nodiscard]] inline std::shared_ptr<Component> get_about_view(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb)
{
    if (!s->about_view_component) {
        av::AboutDialogProps props;
        props.app_version = s->app_version;
        props.on_close = [s, cb]() {
            s->mode = ReplMode::Normal;
            s->about_view_component.reset();
            if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        };

        auto theme = cc::ui::design::theme::current_theme();
        s->about_view_component = std::make_shared<Component>(
            av::MakeAboutDialog(std::move(props), theme));
    }
    return std::static_pointer_cast<Component>(s->about_view_component);
}

/// Render the about view component.
[[nodiscard]] inline Element render_about_view(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb)
{
    auto panel = get_about_view(s, cb);
    return panel ? (*panel)->Render() : text("");
}

/// Forward an event to the about view component.
inline bool forward_about_view(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb,
    Event ev)
{
    auto panel = get_about_view(s, cb);
    if (panel && (*panel)->OnEvent(std::move(ev))) return true;

    if (ev == Event::Escape) {
        s->mode = ReplMode::Normal;
        s->about_view_component.reset();
        if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        return true;
    }
    return false;
}

/// Reset (destroy) the about view so the next entry starts fresh.
inline void reset_about_view(const std::shared_ptr<ReplScreenState>& s) {
    s->about_view_component.reset();
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
        // M7.5: Sync ReplMode → DialogQueue bridge.
        // Converts legacy mode-based dialogs into queue-based dialogs so
        // they render through the priority-band system (faithful to TS).
        namespace dqr = dialog_queue_render;
        dqr::SyncReplModeToQueue(*state, cb);

        // M6: Faithful permission panels — bash / file_edit / file_write.
        // Rendered as a dbox overlay (matching TS overlay slot).
        if (state->mode == ReplMode::ToolPermission &&
            state->permission_request) {
            Element base = RenderReplScreen(*state);
            Element panel = dialog_router::render_permission_panel(state, cb);
            return dbox({
                base | dim,
                vbox({ filler(),
                       hbox({ filler(), panel | flex_shrink, filler() })
                           | flex_shrink,
                       filler() }) | flex });
        }
        return RenderReplScreen(*state);
    })
         | CatchEvent([state, cb](Event ev) -> bool {
    // M7.5: Sync ReplMode → DialogQueue bridge before event dispatch
    // (events fire before render in FTXUI, so we sync here too).
    namespace dqr = dialog_queue_render;
    dqr::SyncReplModeToQueue(*state, cb);

    // --- Panel-mode predicate (modes that show inline content, not dialogs) ---
    // M7.5: Tasks/Teams/Help/Settings/About/QuickOpen are now modal dialogs
    // via DialogQueue, so they're no longer "panel" (inline) modes.
    auto is_panel = [](ReplMode m){ return m == ReplMode::Normal; };
    const bool in_dialog = !is_panel(state->mode);

    // 0) Dialog queue — takes priority over legacy ReplMode dialogs
    //    (M7.5: migration path from ReplMode to DialogQueue)
    namespace dqr = dialog_queue_render;
    if (dqr::HasActiveQueueDialog(*state)) {
        if (dqr::HandleDialogQueueEvent(*state, ev)) {
            return true;
        }
    }

    // 1) Dialog-context events
    if (in_dialog) {
        // M6: Faithful permission panels — forward events to the panel
        // component (manages up/down/Enter/Esc/input internally).
        if (state->mode == ReplMode::ToolPermission &&
            state->permission_request) {
            return dialog_router::forward_permission_panel(state, cb, ev);
        }
        if (ev == Event::Escape) {
            // Critical dialogs defer to y/n/a/c/r/q handlers
            switch (state->mode) {
              case ReplMode::ToolPermission:
              case ReplMode::SandboxPermission:
              case ReplMode::WorkerSandboxPermission:
              case ReplMode::CostThreshold: break;
              default:
                state->mode = ReplMode::Normal;
                if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
                return true; } }
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
            if (state->mode == ReplMode::CostThreshold) {
                if (c=='c'||c=='C'){
                    if (cb->on_dialog_action)
                        cb->on_dialog_action(ReplMode::CostThreshold,0);
                    return true;
                }
                if (c=='r'||c=='R'){
                    if (cb->on_dialog_action)
                        cb->on_dialog_action(ReplMode::CostThreshold,1);
                    return true;
                }
                if (c=='q'||c=='Q'){
                    if (cb->on_dialog_action)
                        cb->on_dialog_action(ReplMode::CostThreshold,2);
                    return true;
                }
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
        state->autocomplete_suggestions.clear();
        state->autocomplete_index = -1; return true; }
    if (ev == Event::Character('\x0F')) return true;  // Ctrl+O transcript

    // 3) Input-context events
    const bool accept_input = !in_dialog;
    if (accept_input) {
        // Enter
        if (ev == Event::Return && !state->input_text.empty()) {
            if (cb->on_submit) cb->on_submit(state->input_text, state->input_mode);
            state->input_history.push_back(state->input_text);
            if (state->input_history.size() > 1000) state->input_history.pop_front();
            state->history_index = std::string::npos;
            state->input_text.clear();
            state->autocomplete_suggestions.clear();
            state->autocomplete_index = -1;
            state->is_prompt_input_active = false; return true; }
        // Ctrl+Enter -> newline (Ctrl+J in terminals)
        if (ev == Event::Character('\x0A')) {
            state->input_text += '\n';
            state->is_prompt_input_active = true;
            state->last_keystroke = std::chrono::steady_clock::now(); return true; }
        // Tab / Shift+Tab -> autocomplete
        const int asn = static_cast<int>(state->autocomplete_suggestions.size());
        if (ev == Event::Tab && asn > 0) {
            state->autocomplete_index = state->autocomplete_index < 0 ? 0
                : (state->autocomplete_index + 1) % asn; return true; }
        // Shift+Tab (ISO backtab) = \x1B[Z
        if (ev.input() == "\x1B[Z" && asn > 0) {
            state->autocomplete_index = state->autocomplete_index < 0 ? asn - 1
                : (state->autocomplete_index - 1 + asn) % asn; return true; }
        // Up (history back) / Down (history forward)
        if (ev == Event::ArrowUp && state->input_text.empty()
            && !state->input_history.empty()) {
            state->history_index = state->history_index == std::string::npos
                ? state->input_history.size() - 1
                : std::max<std::size_t>(0, state->history_index - 1);
            state->input_text = state->input_history[state->history_index];
            state->is_prompt_input_active = true;
            state->last_keystroke = std::chrono::steady_clock::now(); return true; }
        if (ev == Event::ArrowDown
            && state->history_index != std::string::npos) {
            if (state->history_index + 1 >= state->input_history.size()) {
                state->history_index = std::string::npos;
                state->input_text.clear();
            } else {
                state->input_text = state->input_history[++state->history_index]; }
            state->is_prompt_input_active = true;
            state->last_keystroke = std::chrono::steady_clock::now(); return true; }
        // Esc
        if (ev == Event::Escape) {
            if (!state->autocomplete_suggestions.empty()) {
                state->autocomplete_suggestions.clear();
                state->autocomplete_index = -1; return true; }
            if (state->selected_message_idx >= 0)
                { state->selected_message_idx = -1; return true; }
            if (!state->input_text.empty()) {
                state->input_text.clear();
                state->history_index = std::string::npos; return true; } }
        // Slash auto-detection
        if (!state->input_text.empty() && state->input_text[0] == '/'
            && state->input_mode == InputMode::Prompt)
            state->input_mode = InputMode::SlashCommand;
        if (state->input_text.empty()
            && state->input_mode == InputMode::SlashCommand)
            state->input_mode = InputMode::Prompt;
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
                if ((first >= 0x20 && first < 0x7F) || first >= 0xC0) {
                    state->input_text += ch;
                    state->is_prompt_input_active = true;
                    state->last_keystroke = std::chrono::steady_clock::now();
                    return true;
                }
            }
        }
        // Backspace — handle multi-byte UTF-8 correctly by erasing a full
        // codepoint, not just the last byte.  CJK characters are 3 bytes in
        // UTF-8, so a plain pop_back() would leave a partial/invalid sequence.
        if (ev == Event::Backspace && !state->input_text.empty()) {
            // Back up from the end to the previous UTF-8 character boundary.
            // UTF-8 continuation bytes have the top bit set and second bit
            // clear (0b10xxxxxx).  We skip them to find the start byte.
            std::size_t pos = state->input_text.size();
            while (pos > 0) {
                --pos;
                unsigned char c = static_cast<unsigned char>(state->input_text[pos]);
                if ((c & 0xC0) != 0x80) break;  // not a continuation byte
            }
            state->input_text.erase(pos);
            state->is_prompt_input_active = true;
            state->last_keystroke = std::chrono::steady_clock::now();
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
