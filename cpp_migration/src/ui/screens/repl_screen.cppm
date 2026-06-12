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
///   status bar   -> cc.ui.components.status_line  (UI1)
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
import cc.ui.agents.agent_wizard;
import cc.ui.dialogs.install_github_app_wizard;
import cc.ui.dialogs.install_slack_app_wizard;

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
    TasksView, TeamsView, SettingsView, HelpView, QuickOpen,
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
    std::optional<std::string> agent_display_name, agent_color_name;
    std::chrono::system_clock::time_point timestamp;
    int estimated_height_lines = 3;
};

/// Permission prompt subset (TS ToolUseConfirm).  Full shape: UI8/UI9.
struct PermissionRequestInfo {
    std::string tool_name, description;
    std::optional<std::string> file_path;
    std::vector<std::string> risk_labels;
    bool can_always_allow = true;
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
    // Input
    std::string input_text;
    std::string input_placeholder = "Ask anything...";
    std::vector<std::string> autocomplete_suggestions;
    int autocomplete_index = -1;
    std::deque<std::string> input_history;
    std::size_t history_index = std::string::npos;
    // Status / spinner / permission / context
    StatusBarData status_bar;
    std::optional<std::string> spinner_verb, spinner_tip;
    std::optional<PermissionRequestInfo> permission_request;
    DialogContext dialog_ctx;
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
};

/// Engine-facing callbacks (TS ReplScreen external prop callbacks).
struct ReplScreenCallbacks {
    std::function<void(const std::string&, InputMode)> on_submit;
    std::function<void()> on_interrupt;                 // Ctrl+C
    std::function<void()> on_exit;                      // Ctrl+D or /exit
    std::function<void(bool, std::optional<bool>)> on_permission_response;
    std::function<void(ReplMode, int)> on_dialog_action;
    std::function<void(ReplMode)> on_mode_change;
    // UI15 Slack wizard "Launch /slack now" shortcut -> REPL engine.
    std::function<void(const std::string& command)> enqueue_slash_command;
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
[[nodiscard]] inline Element RenderMessages(
    const std::vector<MessageDisplayEntry>& entries,
    int sel = -1, int vlines = 40,
    [[maybe_unused]] int offs = 0, bool pinned = true) {
    if (entries.empty())
        return vbox({ filler(),
            vbox({ text(" Welcome! Type a message to begin.") | dim | center,
                   text(""), text("  /help    -- list commands") | dim | center,
                   text("  /model   -- change model") | dim | center,
                   text("  /config  -- open settings") | dim | center }) | center,
            filler() }) | flex;

    namespace ml = cc::ui::messages_list;
    ml::MessagesListInput input;
    input.rows.reserve(entries.size());
    input.shapes.reserve(entries.size());

    for (const auto& m : entries) {
        if (m.role == "user") {
            input.shapes.push_back(messages::MessageShape::UserText);
            input.rows.push_back(messages::UserTextMessageData{
                .content = m.content_preview,
                .timestamp = m.timestamp});
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
                opts.call.raw_parameters = m.content_preview;
                input.rows.push_back(std::move(opts));
            } else {
                input.shapes.push_back(messages::MessageShape::AssistantText);
                input.rows.push_back(messages::AssistantTextMessageData{
                    .content = m.content_preview,
                    .timestamp = m.timestamp,
                    .is_streaming = m.is_streaming});
            }
        } else if (m.role == "tool") {
            input.shapes.push_back(messages::MessageShape::UserToolResult);
            input.rows.push_back(messages::ToolResultOptions{
                .tool_name = m.tool_name.value_or("tool"),
                .status = m.is_error
                    ? messages::ToolResultStatus::Error
                    : messages::ToolResultStatus::Success,
                .output = m.content_preview});
        } else {
            input.shapes.push_back(messages::MessageShape::SystemText);
            input.rows.push_back(messages::SystemTextMessageData{
                .summary = m.content_preview,
                .timestamp = m.timestamp});
        }
    }

    if (sel >= 0)
        input.selected_row_idx = static_cast<std::size_t>(sel);

    const auto N = entries.size();
    bool has_streaming = !entries.empty() && entries.back().is_streaming;
    input.streaming_tail_row = has_streaming ? N - 1 : N;

    static int frame_count = 0;
    ++frame_count;

    (void)vlines; (void)pinned;
    return ml::render_messages_list_view(std::move(input), frame_count) | flex;
}

// UI2: prompt input shell.  Full feature parity in prompt_input_full.cppm.
[[nodiscard]] inline Element RenderPromptInput(const ReplScreenState& s) {
    std::string mp; Color mc = Color::White;
    switch (s.input_mode) {
      case InputMode::Prompt:       mp=">";   mc=Color::Green;    break;
      case InputMode::Bash:         mp="!";   mc=Color::Red;      break;
      case InputMode::SlashCommand: mp="/";   mc=Color::Cyan;     break;
      case InputMode::HistorySearch:mp="?";   mc=Color::Magenta;  break;
      case InputMode::PlanMode:     mp="S";   mc=Color::Yellow;   break;
      case InputMode::VimInsert:    mp="[I]"; mc=Color::Green;    break;
      case InputMode::VimNormal:    mp="[N]"; mc=Color::Yellow;   break;
      case InputMode::VimVisual:    mp="[V]"; mc=Color::Magenta;  break;
      case InputMode::OrphanedPermission: mp="[!]"; mc=Color::Red;break;
      case InputMode::TaskNotification: mp="[*]"; mc=Color::Cyan; break; }
    Elements line = { text(" "), text(mp + " ") | color(mc) | bold };
    line.push_back(s.input_text.empty()
        ? text(s.input_placeholder) | dim
        : text(s.input_text) | color(Color::White));
    Element ac_row = text("");
    if (!s.autocomplete_suggestions.empty()) {
        Elements pills;
        std::size_t n = s.autocomplete_suggestions.size();
        for (std::size_t i = 0; i < std::min(n, std::size_t{5}); ++i) {
            bool sel = static_cast<int>(i) == s.autocomplete_index;
            auto pill = text(" " + s.autocomplete_suggestions[i] + " ");
            if (sel) pill = pill | color(Color::Black) | bgcolor(Color::Cyan) | bold;
            else     pill = pill | dim | borderLight;
            pills.push_back(std::move(pill));
            pills.push_back(text(" "));
        }
        ac_row = hbox({ text("  "), hbox(pills), filler() });
    }
    return vbox({ ac_row, hbox(line), text("") });
}

// =========================================================
// Dialog routing (delegates each ReplMode to owning UIx agent)
// =========================================================

namespace dialog_stubs {
using Builder = std::function<Element(const ReplScreenState&)>;

/// Dispatch table.  Each ReplMode dialog mode maps to a builder.
[[nodiscard]] inline Builder get_builder(ReplMode m) {
    using P = std::pair<ReplMode, Builder>;
    static const P kTable[] = {
      // UI6  MessageSelector — full toolbar in messages_interactions.cppm
      P{ReplMode::MessageSelector, [](const auto&){
         return window(text(" Message Selector "),
             vbox({
                 text("Select messages to rewind, edit, or summarize") | center,
                 separator(),
                 hbox({
                     text(" [Up/Down]") | bold | color(Color::Cyan), text(" nav "),
                     text("[Enter]") | bold | color(Color::Cyan), text(" edit "),
                     text("[s]") | bold | color(Color::Cyan), text(" summarize "),
                     text("[Esc]") | bold | color(Color::Cyan), text(" close"),
                 }) | dim,
             }) | size(WIDTH, GREATER_THAN, 50)); }},
      // UI8  Sandbox (leader + worker) — styled permission panels
      P{ReplMode::SandboxPermission, [](const auto& s){
         auto host = s.dialog_ctx.sandbox_host_pattern.value_or("unknown");
         return window(text(" Sandbox Permission ") | color(Color::Yellow),
             vbox({
                 hbox({text("Network access requested") | bold}),
                 separator(),
                 text("  Host: " + host) | dim,
                 text(""),
                 hbox({
                     text(" [y]") | bold | color(Color::Green), text(" allow "),
                     text("[n]") | bold | color(Color::Red), text(" deny "),
                     text("[a]") | bold | color(Color::Cyan), text(" always allow"),
                 }) | dim,
             }) | size(WIDTH, GREATER_THAN, 50)) | color(Color::Yellow); }},
      P{ReplMode::WorkerSandboxPermission, [](const auto& s){
         auto req_id = s.dialog_ctx.sandbox_worker_request_id.value_or("worker");
         return window(text(" Worker Sandbox Permission ") | color(Color::Yellow),
             vbox({
                 hbox({text("Worker requests network access") | bold}),
                 separator(),
                 text("  Request: " + req_id) | dim,
                 text(""),
                 hbox({
                     text(" [y]") | bold | color(Color::Green), text(" allow "),
                     text("[n]") | bold | color(Color::Red), text(" deny "),
                     text("[a]") | bold | color(Color::Cyan), text(" always allow"),
                 }) | dim,
             }) | size(WIDTH, GREATER_THAN, 50)) | color(Color::Yellow); }},
      // UI9  ToolPermission — delegates to permission_dialog.cppm base style
      P{ReplMode::ToolPermission, [](const auto& s){
         if (!s.permission_request) return Element{};
         dialogs::PermissionRequest r{};
         r.tool_name   = s.permission_request->tool_name;
         r.description = s.permission_request->description;
         r.risk_level  = s.permission_request->risk_labels.empty()
                       ? "medium" : s.permission_request->risk_labels.front();
         if (s.permission_request->file_path)
             r.affected_paths.push_back(*s.permission_request->file_path);
         return paragraph(dialogs::render_permission_dialog(std::move(r),80)); }},
      // UI2  Hook prompt — input request from a pre/post hook
      P{ReplMode::PromptHook, [](const auto&){
         return window(text(" Hook Prompt ") | color(Color::Magenta),
             vbox({
                 text("A hook requires user input") | center,
                 separator(),
                 hbox({
                     text(" [Enter]") | bold | color(Color::Green), text(" submit "),
                     text("[Esc]") | bold | color(Color::Red), text(" cancel"),
                 }) | dim,
             }) | size(WIDTH, GREATER_THAN, 44)) | color(Color::Magenta); }},
      // UI10 Elicitation (MCP) — server requesting structured input
      P{ReplMode::Elicitation, [](const auto& s){
         auto server = s.dialog_ctx.elicitation_server_name.value_or("MCP Server");
         return window(text(" MCP Elicitation ") | color(Color::Blue),
             vbox({
                 hbox({text(server) | bold, text(" requests permission") | dim}),
                 separator(),
                 text("  The server needs additional input to proceed.") | dim,
                 text(""),
                 hbox({
                     text(" [Enter]") | bold | color(Color::Green), text(" approve "),
                     text("[Esc]") | bold | color(Color::Red), text(" deny"),
                 }) | dim,
             }) | size(WIDTH, GREATER_THAN, 50)) | color(Color::Blue); }},
      // UI1  Cost threshold — session cost exceeded
      P{ReplMode::CostThreshold, [](const auto& s){
         auto cost = s.dialog_ctx.cost_threshold_usd.value_or(0.0);
         auto current = s.status_bar.cost_usd.value_or(0.0);
         return window(text(" Cost Threshold ") | color(Color::Yellow),
             vbox({
                 text("Session cost exceeds threshold") | bold | color(Color::Yellow),
                 separator(),
                 text(std::format("  Current:   ${:.2f}", current)),
                 text(std::format("  Threshold: ${:.2f}", cost)),
                 text("  Model: " + s.status_bar.model_name) | dim,
                 separator(),
                 hbox({
                     text(" [c]") | bold | color(Color::Green), text(" continue "),
                     text("[r]") | bold | color(Color::Cyan), text(" reset "),
                     text("[q]") | bold | color(Color::Red), text(" quit"),
                 }) | dim,
             }) | size(WIDTH, GREATER_THAN, 44)) | color(Color::Yellow); }},
      // UI1  Idle return — session was idle
      P{ReplMode::IdleReturn, [](const auto& s){
         auto mins = s.dialog_ctx.idle_return_minutes.value_or(0);
         return window(text(" Welcome Back "),
             vbox({
                 text("Session has been idle") | bold | center,
                 separator(),
                 text(mins > 0 ? std::format("  Idle for {} minutes", mins) : "") | dim,
                 text(""),
                 hbox({
                     text(" [Enter]") | bold | color(Color::Green), text(" resume "),
                     text("[n]") | bold | color(Color::Cyan), text(" start new"),
                 }) | dim,
             }) | size(WIDTH, GREATER_THAN, 40)); }},
      // UI11 Ultraplan choice — plan proposal
      P{ReplMode::UltraplanChoice, [](const auto& s){
         auto blurb = s.dialog_ctx.ultraplan_blurb.value_or(
             "A structured plan has been generated.");
         return window(text(" Ultraplan ") | color(Color::Cyan),
             vbox({
                 text("Plan Proposal") | bold | color(Color::Cyan),
                 separator(),
                 text("  " + blurb) | dim,
                 text(""),
                 hbox({
                     text(" [Enter]") | bold | color(Color::Green), text(" accept "),
                     text("[Esc]") | bold | color(Color::Red), text(" cancel"),
                 }) | dim,
             }) | size(WIDTH, GREATER_THAN, 50)) | color(Color::Cyan); }},
      // UI11 Ultraplan launch
      P{ReplMode::UltraplanLaunch, [](const auto&){
         return window(text(" Ultraplan Launch ") | color(Color::Cyan),
             vbox({
                 text("Launch background plan execution?") | bold | center,
                 separator(),
                 text("  Workers will execute the plan steps in parallel.") | dim,
                 text(""),
                 hbox({
                     text(" [Enter]") | bold | color(Color::Green), text(" launch "),
                     text("[Esc]") | bold | color(Color::Red), text(" cancel"),
                 }) | dim,
             }) | size(WIDTH, GREATER_THAN, 50)) | color(Color::Cyan); }},
      // UI11 IDE Onboarding
      P{ReplMode::IdeOnboarding, [](const auto&){
         return window(text(" IDE Integration ") | color(Color::Blue),
             vbox({
                 text("IDE Extension Available") | bold | center,
                 separator(),
                 text("  Install the IDE extension for enhanced editing,") | dim,
                 text("  inline completions, and project awareness.") | dim,
                 text(""),
                 hbox({
                     text(" [Enter]") | bold | color(Color::Green), text(" install "),
                     text("[Esc]") | bold | color(Color::GrayLight), text(" dismiss"),
                 }) | dim,
             }) | size(WIDTH, GREATER_THAN, 50)) | color(Color::Blue); }},
      // UI11 Init onboarding (first-run wizard)
      P{ReplMode::InitOnboarding, [](const auto&){
         return window(text(" Welcome ") | color(Color::Green),
             vbox({
                 text("Welcome to CC-REPL") | bold | center | color(Color::Green),
                 separator(),
                 text("  Let's get you set up with a quick walkthrough.") | dim,
                 text("  Configure your API key, model, and preferences.") | dim,
                 text(""),
                 hbox({
                     text(" [Enter]") | bold | color(Color::Green), text(" begin setup "),
                     text("[Esc]") | bold | color(Color::GrayLight), text(" skip"),
                 }) | dim,
             }) | size(WIDTH, GREATER_THAN, 50)) | color(Color::Green); }},
      // UI11 Model switch suggestion
      P{ReplMode::ModelSwitch, [](const auto& s){
         auto model = s.dialog_ctx.model_switch_alias.value_or("a new model");
         return window(text(" Model Switch ") | color(Color::Cyan),
             vbox({
                 text("Try " + model + "?") | bold | center,
                 separator(),
                 text("  A better model may be available for this task.") | dim,
                 text(""),
                 hbox({
                     text(" [Enter]") | bold | color(Color::Green), text(" switch "),
                     text("[Esc]") | bold | color(Color::GrayLight), text(" dismiss"),
                 }) | dim,
             }) | size(WIDTH, GREATER_THAN, 44)) | color(Color::Cyan); }},
      // UI11 Auto mode callout
      P{ReplMode::UndercoverCallout, [](const auto&){
         return window(text(" Auto Mode ") | color(Color::Magenta),
             vbox({
                 text("Enable Auto-Approve?") | bold | center,
                 separator(),
                 text("  Automatically approve low-risk tool calls") | dim,
                 text("  (file reads, searches, safe commands).") | dim,
                 text(""),
                 hbox({
                     text(" [Enter]") | bold | color(Color::Green), text(" enable "),
                     text("[Esc]") | bold | color(Color::GrayLight), text(" dismiss"),
                 }) | dim,
             }) | size(WIDTH, GREATER_THAN, 48)) | color(Color::Magenta); }},
      // UI11 Effort callout
      P{ReplMode::EffortCallout, [](const auto&){
         return window(text(" Effort Level ") | color(Color::Yellow),
             vbox({
                 text("Adjust Response Effort") | bold | center,
                 separator(),
                 text("  Higher effort = deeper reasoning, more tokens.") | dim,
                 text("  Lower effort = faster, cheaper responses.") | dim,
                 text(""),
                 hbox({
                     text(" [Enter]") | bold | color(Color::Green), text(" configure "),
                     text("[Esc]") | bold | color(Color::GrayLight), text(" dismiss"),
                 }) | dim,
             }) | size(WIDTH, GREATER_THAN, 48)) | color(Color::Yellow); }},
      // UI11 Remote control callout
      P{ReplMode::RemoteCallout, [](const auto&){
         return window(text(" Remote Control ") | color(Color::Cyan),
             vbox({
                 text("Enable IDE Bridge?") | bold | center,
                 separator(),
                 text("  Connect to your IDE for remote editing,") | dim,
                 text("  file sync, and collaborative features.") | dim,
                 text(""),
                 hbox({
                     text(" [Enter]") | bold | color(Color::Green), text(" enable "),
                     text("[Esc]") | bold | color(Color::GrayLight), text(" dismiss"),
                 }) | dim,
             }) | size(WIDTH, GREATER_THAN, 48)) | color(Color::Cyan); }},
      // UI11 Desktop upsell
      P{ReplMode::DesktopUpsell, [](const auto&){
         return window(text(" Desktop App ") | color(Color::Cyan),
             vbox({
                 text("Try the Desktop App") | bold | center | color(Color::Cyan),
                 separator(),
                 text("  Rich UI, native notifications, multi-window,") | dim,
                 text("  and file drag-and-drop support.") | dim,
                 text(""),
                 hbox({
                     text(" [Enter]") | bold | color(Color::Green), text(" learn more "),
                     text("[Esc]") | bold | color(Color::GrayLight), text(" dismiss"),
                 }) | dim,
             }) | size(WIDTH, GREATER_THAN, 48)) | color(Color::Cyan); }},
      // UI16 LSP Recommendation
      P{ReplMode::LspRecommendation, [](const auto& s){
         auto ext = s.dialog_ctx.lsp_rec_extension.value_or("language-server");
         auto fext = s.dialog_ctx.lsp_rec_file_ext.value_or("");
         Elements body;
         body.push_back(text("LSP Server Recommended") | bold | center);
         body.push_back(separator());
         body.push_back(text("  Extension: " + ext));
         if (!fext.empty())
             body.push_back(text("  For files: *." + fext) | dim);
         body.push_back(text(""));
         body.push_back(hbox({
             text(" [i]") | bold | color(Color::Green), text(" install "),
             text("[d]") | bold | color(Color::GrayLight), text(" dismiss"),
         }) | dim);
         return window(text(" LSP Recommendation ") | color(Color::Blue),
             vbox(std::move(body)) | size(WIDTH, GREATER_THAN, 48))
             | color(Color::Blue); }},
      // UI16 Plugin hint
      P{ReplMode::PluginHint, [](const auto& s){
         auto name = s.dialog_ctx.plugin_hint_name.value_or("plugin");
         auto desc = s.dialog_ctx.plugin_hint_description.value_or("");
         Elements body;
         body.push_back(text("Plugin Suggestion") | bold | center);
         body.push_back(separator());
         body.push_back(text("  " + name) | bold);
         if (!desc.empty())
             body.push_back(text("  " + desc) | dim);
         body.push_back(text(""));
         body.push_back(hbox({
             text(" [i]") | bold | color(Color::Green), text(" install "),
             text("[d]") | bold | color(Color::GrayLight), text(" dismiss"),
         }) | dim);
         return window(text(" Plugin Hint ") | color(Color::Magenta),
             vbox(std::move(body)) | size(WIDTH, GREATER_THAN, 48))
             | color(Color::Magenta); }},
      // UI3  Settings panel
      P{ReplMode::SettingsView, [](const auto&){
         return window(text(" Settings ") | color(Color::Cyan),
             vbox({
                 text("Settings") | bold | center,
                 separator(),
                 text("  /config        Open configuration") | dim,
                 text("  /model         Change model") | dim,
                 text("  /permissions   View permissions") | dim,
                 text("  /mcp           MCP servers") | dim,
                 text(""),
                 hbox({
                     text(" [Esc]") | bold | color(Color::Red), text(" close"),
                 }) | dim,
             }) | size(WIDTH, GREATER_THAN, 44)) | color(Color::Cyan); }},
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
      case ReplMode::QuickOpen: return std::nullopt;

      // UI15: install wizard overlays (banners; the live FTXUI Component
      // is owned by ReplScreen's dialog_router and composited on top).
      case ReplMode::InstallGitHubApp:
        return vbox({
            text("") | size(HEIGHT, EQUAL, 1),
            hbox({
                text("  Install GitHub App  ") | bold | color(Color::Cyan),
                text("12-step wizard") | dim,
            }),
            separator() | color(Color::Cyan),
            vbox({
                text(""),
                text(" Enter = Next / Complete")     | color(Color::GrayLight),
                text(" Esc   = Back / Cancel")       | color(Color::GrayLight),
                text(" Tab   = Focus next field")    | color(Color::GrayLight),
            }) | center | flex,
        }) | border | color(Color::Cyan);

      case ReplMode::InstallSlackApp:
        return vbox({
            text("") | size(HEIGHT, EQUAL, 1),
            hbox({
                text("  Install Slack App  ") | bold | color(Color::Magenta),
                text("3-step wizard") | dim,
            }),
            separator() | color(Color::Magenta),
            vbox({
                text(""),
                text(" Enter = Next / Complete")     | color(Color::GrayLight),
                text(" Esc   = Back / Cancel")       | color(Color::GrayLight),
            }) | center | flex,
        }) | border | color(Color::Magenta);

      // UI13: agent wizard — the full FTXUI Component is owned by
      // dialog_router and composited by ReplScreen's Renderer.
      // RouteDialog returns nullopt here to avoid double-overlay.
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
// Full layout composition
// =========================================================

/// Top->bottom: StatusBar | Messages | Spinner | Tasks/Teams (opt)
///               | PromptInput | Footer (counts).
/// Non-null RouteDialog() overlays via dbox() with dim background.
[[nodiscard]] inline Element RenderReplScreen(const ReplScreenState& s) {
    Elements L; L.reserve(8);
    L.push_back(RenderStatusBar(s.status_bar));
    L.push_back(separator());
    L.push_back(RenderMessages(s.messages, s.selected_message_idx,
                               s.viewport_height_lines, s.scroll_offset,
                               s.scroll_pinned_to_bottom));
    if (s.spinner_mode != SpinnerMode::Hidden)
        L.push_back(RenderSpinner(s.spinner_mode, s.spinner_verb, s.spinner_tip));
    if (s.mode == ReplMode::TasksView && s.background_task_count > 0) {
        L.push_back(separator());
        L.push_back(hbox({
            text(" ") ,
            text(std::format("{} background task{}", s.background_task_count,
                 s.background_task_count == 1 ? "" : "s")) | bold | color(Color::Cyan),
            text(" active") | dim,
            filler(),
            text("[t] toggle ") | dim,
            text("[Esc] close ") | dim,
        })); }
    if (s.mode == ReplMode::TeamsView && s.teammate_count > 0) {
        L.push_back(separator());
        L.push_back(hbox({
            text(" "),
            text(std::format("{} team member{}", s.teammate_count,
                 s.teammate_count == 1 ? "" : "s")) | bold | color(Color::Magenta),
            text(" connected") | dim,
            filler(),
            text("[Esc] close ") | dim,
        })); }
    L.push_back(separator());
    L.push_back(RenderPromptInput(s));
    if (s.background_task_count || s.teammate_count) {
        Elements f = { text(" ") };
        if (s.background_task_count) f.push_back(
            text(std::format("{} tasks ", s.background_task_count))
                | dim | color(Color::Cyan));
        if (s.teammate_count) f.push_back(
            text(std::format("{} team ", s.teammate_count))
                | dim | color(Color::Magenta));
        L.push_back(hbox(std::move(f)));
    }
    Element base = vbox(std::move(L));
    auto dlg = RouteDialog(s.mode, s);
    if (dlg) return dbox({
        std::move(base) | dim,
        vbox({ filler(),
               hbox({ filler(), std::move(*dlg) | flex_shrink, filler() })
               | flex_shrink, filler() }) | flex });
    return base;
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
        // TODO: when editing, load the AgentCardData from agent runtime.
        // For now, create mode is always used (edit_agent = nullopt).
        opts.on_save = [s, cb](const WizardDraft& draft) {
            // Wizard completed — save the agent and close.
            // TODO: persist via agent_runtime API.
            (void)draft;
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
        // For agent wizard modes, ensure the wizard component exists
        // and render the real wizard content (not the placeholder banner).
        if (state->mode == ReplMode::CreateAgent ||
            state->mode == ReplMode::EditAgent) {
            Element base = RenderReplScreen(*state);
            Element wizard_content = dialog_router::render_agent_wizard(state, cb);
            // Replace the placeholder dialog with the real wizard content.
            return dbox({
                base | dim,
                vbox({ filler(),
                       hbox({ filler(), wizard_content | flex_shrink, filler() })
                           | flex_shrink,
                       filler() }) | flex });
        }
        if (state->mode == ReplMode::InstallGitHubApp ||
            state->mode == ReplMode::InstallSlackApp) {
            Element base = RenderReplScreen(*state);
            Element wizard_content = dialog_router::render_install_wizard(state, cb);
            return dbox({
                base | dim,
                vbox({ filler(),
                       hbox({ filler(), wizard_content | flex_shrink, filler() })
                           | flex_shrink,
                       filler() }) | flex });
        }
        return RenderReplScreen(*state);
    })
         | CatchEvent([state, cb](Event ev) -> bool {
    // --- Panel-mode predicate ---
    auto is_panel = [](ReplMode m){ return
        m==ReplMode::Normal || m==ReplMode::TasksView
        || m==ReplMode::TeamsView || m==ReplMode::HelpView
        || m==ReplMode::QuickOpen; };
    const bool in_dialog = !is_panel(state->mode);

    // 1) Dialog-context events
    if (in_dialog) {
        // UI13 agent wizard: forward every event to the wizard component
        // (it manages Esc/Enter/buttons internally).
        if (state->mode == ReplMode::CreateAgent ||
            state->mode == ReplMode::EditAgent) {
            return dialog_router::forward_agent(state, cb, ev);
        }
        // UI15 wizard modes: forward every event to the wizard
        // component (they manage Esc/Enter/buttons internally).
        if (state->mode == ReplMode::InstallGitHubApp ||
            state->mode == ReplMode::InstallSlackApp) {
            return dialog_router::forward_install_wizard(state, cb, ev);
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
                if (c=='y'||c=='Y'){ if(cb->on_permission_response)
                    cb->on_permission_response(true,false); return true; }
                if (c=='n'||c=='N'){ if(cb->on_permission_response)
                    cb->on_permission_response(false,std::nullopt); return true; }
                if (c=='a'||c=='A'){ if(cb->on_permission_response)
                    cb->on_permission_response(true,true); return true; } }
            if (state->mode == ReplMode::CostThreshold) {
                if (c=='c'||c=='C'){ if(cb->on_dialog_action)
                    cb->on_dialog_action(ReplMode::CostThreshold,0); return true; }
                if (c=='r'||c=='R'){ if(cb->on_dialog_action)
                    cb->on_dialog_action(ReplMode::CostThreshold,1); return true; }
                if (c=='q'||c=='Q'){ if(cb->on_dialog_action)
                    cb->on_dialog_action(ReplMode::CostThreshold,2); return true; } } } }

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
        // Printable chars
        if (ev.is_character()) {
            char c = ev.character()[0];
            if (c >= 0x20 && c < 0x7F) {
                state->input_text += c;
                state->is_prompt_input_active = true;
                state->last_keystroke = std::chrono::steady_clock::now();
                return true; } }
        // Backspace
        if (ev == Event::Backspace && !state->input_text.empty()) {
            state->input_text.pop_back();
            state->is_prompt_input_active = true;
            state->last_keystroke = std::chrono::steady_clock::now();
            return true; }
    }
    return false; });
}

/// Convenience: self-owned state (demos/tests only).
/// Production: use externally-held shared_ptr<ReplScreenState> overload.
[[nodiscard]] inline Component ReplScreen(ReplScreenCallbacks cbs) {
    return ReplScreen(std::make_shared<ReplScreenState>(), std::move(cbs));
}

} // namespace cc::ui::repl_screen
