/// @file dialog_all_renderers.cppm
/// @brief Registry wiring for all remaining dialog types.
///
/// MODULE:   cc.ui.dialogs.all_renderers
/// LICENCE:  Exported.  Imported by app initialization to register
///           the complete set of dialog renderers.
///
/// Bridges existing standalone dialog implementations into the
/// DialogRendererRegistry so they work through the DialogQueue system.
/// For dialogs without a simple Element render function, provides
/// a stub renderer showing the dialog name.
///
/// Renderers provided here (M7.5 — partial upgrade from stubs):
///   - MessageSelector (bottom, band 1)
///   - SettingsPanel (modal)
///   - TasksView (modal) — stub
///   - TeamsView (modal) — stub
///   - HelpView (modal)
///   - QuickOpen (modal)
///   - PluginDialog (modal) — real (main menu view)
///   - MCPDialog (modal)
///   - DiffDialog (modal) — real (structured diff viewer)
///   - ConfigDialog (modal)
///   - ExportDialog (modal) — real (basic)
///   - GlobalSearch (modal) — real
///   - HistorySearch (modal) — real
///   - FeedbackSurvey (modal) — real
///   - ManagedSettingsSecurity (modal) — real
///   - TrustDialog (modal) — stub (full integration TBD)
///   - Onboarding (modal) — stub
///   - InstallGitHubAppWizard (modal) — stub
///   - InstallSlackAppWizard (modal) — stub
///   - CreateAgentWizard (modal) — stub
///   - EditAgentWizard (modal) — stub
module;

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <chrono>
#include <algorithm>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.all_renderers;

import cc.ui.dialogs.system;
import cc.ui.dialogs.frame;
import cc.ui.dialogs.settings_view;
import cc.ui.dialogs.help_view;
import cc.ui.dialogs.about;
import cc.ui.dialogs.mcp_dialog;
import cc.ui.dialogs.config_dialog;
import cc.ui.dialogs.quick_open;
import cc.ui.dialogs.export_dialog;
import cc.ui.dialogs.plugin_dialog;
import cc.ui.dialogs.diff_dialog;
import cc.ui.trust_dialog;
import cc.ui.trust_utils;
import cc.ui.task_list_ui;
import cc.ui.teams.teams_overview;
import cc.ui.dialogs.managed_settings_security;
import cc.ui.dialogs.global_search_dialog;
import cc.ui.dialogs.history_search_dialog;
import cc.ui.dialogs.onboarding;
import cc.ui.dialogs.install_github_app_wizard;
import cc.ui.dialogs.install_slack_app_wizard;
import cc.ui.agents.agent_wizard;  // namespace: cc::ui::agents::wizard
import cc.ui.agents.agent_cards;   // namespace: cc::ui::agents::cards
import cc.ui.structured_diff;
import cc.utils.file_edit;
import cc.ui.feedback_survey;
import cc.ui.design.theme;
import cc.ui.design.tokens;
import cc.tools.task;
import cc.history;

export namespace cc::ui::dialogs::all_renderers {

using namespace ftxui;
namespace dsys = cc::ui::dialogs::system;
namespace dframe = cc::ui::dialogs::frame;
namespace sd = cc::ui::structured_diff;
using Theme = cc::ui::design::theme::Theme;
using Role = cc::ui::design::tokens::Role;

// ============================================================
// Helpers
// ============================================================

namespace detail {

/// Render a generic stub dialog for types not yet fully implemented.
[[nodiscard]] inline Element RenderStubDialog(
    std::string_view title,
    std::string_view description,
    const Theme& theme)
{
    dframe::DialogFrameProps props;
    props.title = std::string{title};
    props.style = dframe::FrameStyle::Info;
    props.full_border = true;
    props.inner_padding_x = 2;
    props.inner_padding_y = 1;

    props.content = vbox({
        text(std::string{description}) | dim | center,
        text(""),
        text("[Esc to close]") | dim | center,
    });

    return dframe::DialogFrame(props, theme);
}

/// Map system feedback state to feedback_survey module's SurveyState.
[[nodiscard]] inline auto map_feedback_state(dsys::FeedbackSurveyState s)
    -> cc::ui::feedback_survey::SurveyState
{
    using Dst = cc::ui::feedback_survey::SurveyState;
    switch (s) {
        case dsys::FeedbackSurveyState::Closed:    return Dst::closed;
        case dsys::FeedbackSurveyState::Open:      return Dst::open;
        case dsys::FeedbackSurveyState::Thanks:    return Dst::thanks;
        case dsys::FeedbackSurveyState::TranscriptPrompt:
            return Dst::transcript_prompt;
        case dsys::FeedbackSurveyState::Submitting:return Dst::submitting;
        case dsys::FeedbackSurveyState::Submitted: return Dst::submitted;
    }
    return Dst::closed;
}

} // namespace detail

// ============================================================
// MessageSelector (bottom slot, band 1)
// ============================================================

[[nodiscard]] inline Element RenderMessageSelector(
    const dsys::MessageSelectorPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    dframe::DialogFrameProps props;
    props.style = dframe::FrameStyle::Info;
    props.full_border = false;
    props.inner_padding_x = 2;
    props.inner_padding_y = 0;

    Elements options;
    for (size_t i = 0; i < p.options.size(); ++i) {
        bool selected = static_cast<int>(i) == p.selected_index;
        auto opt = text(p.options[i]);
        if (selected) opt = opt | inverted | bold;
        else opt = opt | dim;
        if (i > 0) options.push_back(text("  "));
        options.push_back(opt);
    }

    auto content = vbox({
        hbox({
            text("▼ ") | color(ctx.theme.color_for(Role::Info)),
            text(p.placeholder.empty() ? "Select an option" : p.placeholder) | bold,
            filler(),
            text("↑↓ navigate · Enter select") | dim,
        }),
        hbox(std::move(options)),
    });

    props.content = content;
    return dframe::DialogFrame(props, ctx.theme);
}

inline bool HandleMessageSelectorEvent(
    dsys::MessageSelectorPayload& p,
    const Event& event)
{
    if (event == Event::ArrowUp) {
        p.selected_index = std::max(0, p.selected_index - 1);
        return true;
    }
    if (event == Event::ArrowDown) {
        p.selected_index = std::min(
            static_cast<int>(p.options.size()) - 1,
            p.selected_index + 1);
        return true;
    }
    if (event == Event::Return) {
        if (p.on_select && !p.options.empty()) p.on_select(p.selected_index);
        return true;
    }
    if (event == Event::Escape) {
        if (p.on_select) p.on_select(-1);
        return true;
    }
    return false;
}

// ============================================================
// SettingsPanel (modal)
// ============================================================

[[nodiscard]] inline Element RenderSettingsPanel(
    const dsys::SettingsPanelPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    // If a component is provided (high-fidelity mode), use it directly
    if (p.component) {
        auto comp = std::static_pointer_cast<Component>(p.component);
        if (comp) return (*comp)->Render();
    }
    // Fallback: simple stateless render
    namespace sv = cc::ui::dialogs::settings_view;
    sv::SettingsViewProps props;
    sv::SettingsViewState state;
    return sv::RenderSettingsView(props, state, ctx.theme);
}

inline bool HandleSettingsPanelEvent(
    dsys::SettingsPanelPayload& p,
    const Event& event)
{
    // If a component is provided, forward events to it
    if (p.component) {
        auto comp = std::static_pointer_cast<Component>(p.component);
        if (comp && (*comp)->OnEvent(event)) return true;
    }
    if (event == Event::Escape) {
        if (p.on_close) p.on_close();
        return true;
    }
    return false;
}

// ============================================================
// TasksView (modal) — task list with navigation
// ============================================================

namespace tasks_detail {

namespace TL = cc::ui::task_list_ui;
namespace ttools = cc::tools;

/// Convert a tool Task to a BackgroundTaskEntry for display.
[[nodiscard]] inline TL::BackgroundTaskEntry task_to_entry(const ttools::Task& t) {
    TL::BackgroundTaskEntry e;
    e.id = t.id;
    e.name = t.description;

    // Type mapping: agent tasks map to AsyncAgent
    switch (t.agent_type) {
        case ttools::AgentType::Search:
        case ttools::AgentType::GeneralPurpose:
            e.type = TL::BackgroundTaskType::AsyncAgent;
            break;
        default:
            e.type = TL::BackgroundTaskType::AsyncAgent;
            break;
    }

    // Status mapping
    switch (t.status) {
        case ttools::TaskStatus::Pending:
            e.status = TL::RuntimeTaskStatus::Queued;
            break;
        case ttools::TaskStatus::Running:
            e.status = TL::RuntimeTaskStatus::Running;
            break;
        case ttools::TaskStatus::Completed:
            e.status = TL::RuntimeTaskStatus::Completed;
            break;
        case ttools::TaskStatus::Failed:
            e.status = TL::RuntimeTaskStatus::Failed;
            e.display_opts.has_error = true;
            break;
        case ttools::TaskStatus::Cancelled:
            e.status = TL::RuntimeTaskStatus::Killed;
            break;
    }

    // Activity info
    TL::TaskActivity act;
    if (t.status == ttools::TaskStatus::Running && !t.output.empty()) {
        // Use last line of output as progress description
        auto nl = t.output.find_last_of('\n');
        act.description = (nl != std::string::npos)
            ? t.output.substr(nl + 1)
            : t.output;
        if (act.description.size() > 80) {
            act.description = act.description.substr(0, 77) + "...";
        }
    } else if (t.status == ttools::TaskStatus::Completed && t.result) {
        act.description = "Completed";
    } else if (t.status == ttools::TaskStatus::Failed && t.error_message) {
        act.description = *t.error_message;
    } else if (t.status == ttools::TaskStatus::Pending) {
        act.description = "Waiting to start…";
    }
    if (!act.description.empty()) {
        e.activity = std::move(act);
    }

    e.started_at = t.started_at.value_or(t.created_at);
    e.ended_at = t.completed_at;

    return e;
}

/// Fetch all tasks from the global task store as display entries.
[[nodiscard]] inline std::vector<TL::BackgroundTaskEntry> fetch_tasks() {
    std::vector<TL::BackgroundTaskEntry> entries;
    const auto& store = ttools::global_task_store();
    auto tasks = store.list();
    for (const auto* t : tasks) {
        entries.push_back(task_to_entry(*t));
    }
    // Sort by created_at descending (newest first)
    std::sort(entries.begin(), entries.end(),
        [](const TL::BackgroundTaskEntry& a, const TL::BackgroundTaskEntry& b) {
            return a.started_at > b.started_at;
        });
    return entries;
}

} // namespace tasks_detail

[[nodiscard]] inline Element RenderTasksView(
    const dsys::TasksViewPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    namespace tui = cc::ui::task_list_ui;

    auto tasks = tasks_detail::fetch_tasks();
    int count = static_cast<int>(tasks.size());
    int sel = count > 0 ? std::min(p.selected_index, count - 1) : 0;

    tui::TaskListUIOptions opts;
    opts.tasks = std::move(tasks);
    opts.selected_index = sel;
    opts.on_close = p.on_close;

    auto content = tui::RenderTaskListUI(opts);

    dframe::DialogFrameProps fprops;
    fprops.title = "Tasks";
    fprops.style = dframe::FrameStyle::Info;
    fprops.full_border = true;
    fprops.inner_padding_x = 1;
    fprops.inner_padding_y = 0;
    fprops.content = content;

    return dframe::DialogFrame(fprops, ctx.theme)
        | size(WIDTH, EQUAL, std::min(70, ctx.term_cols - 4))
        | size(HEIGHT, EQUAL, std::min(24, ctx.term_rows - 4));
}

inline bool HandleTasksViewEvent(dsys::TasksViewPayload& p, const Event& event) {
    if (event == Event::Escape && p.on_close) { p.on_close(); return true; }

    auto tasks = tasks_detail::fetch_tasks();
    int count = static_cast<int>(tasks.size());
    if (count == 0) return false;

    // Navigate list
    if (event == Event::ArrowUp || event == Event::Character('k')) {
        p.selected_index = std::max(0, p.selected_index - 1);
        return true;
    }
    if (event == Event::ArrowDown || event == Event::Character('j')) {
        p.selected_index = std::min(count - 1, p.selected_index + 1);
        return true;
    }
    if (event == Event::Return && p.on_select) {
        if (p.selected_index >= 0 && p.selected_index < count) {
            p.on_select(tasks[p.selected_index].id);
        }
        return true;
    }
    return false;
}

// ============================================================
// TeamsView (modal) — teams overview
// ============================================================

namespace teams_detail {

namespace to = cc::ui::teams::overview;

[[nodiscard]] inline to::TeamsOverviewOptions sample_teams() {
    to::TeamsOverviewOptions opts;
    opts.teams = {
        to::TeamSummary{"team_alpha", "Alpha Team", 5},
        to::TeamSummary{"team_beta", "Beta Squad", 3},
    };
    opts.current_team_index = 0;
    opts.viewer_role = to::ViewerRole::Member;
    opts.members = {
        {"m1", "Alice", "@alice", to::MembershipRole::Owner,
         to::MemberPresence::Online, std::chrono::seconds(30), "cyan"},
        {"m2", "Bob", "@bob", to::MembershipRole::Maintainer,
         to::MemberPresence::Online, std::chrono::seconds(60), "green"},
        {"m3", "Charlie", "@charlie", to::MembershipRole::Member,
         to::MemberPresence::Idle, std::chrono::seconds(600), "yellow"},
        {"m4", "Diana", "@diana", to::MembershipRole::Member,
         to::MemberPresence::Offline, std::chrono::seconds(7200), "magenta"},
        {"m5", "Eve", "@eve", to::MembershipRole::Guest,
         to::MemberPresence::Offline, std::chrono::seconds(86400), "gray"},
    };
    opts.selected_member_index = 0;
    opts.activity = {
        {"a1", std::chrono::seconds(120), "Alice", "cyan",
         to::ActivityKind::CreateAgent, "created agent deploy-bot", {}},
        {"a2", std::chrono::seconds(600), "Bob", "green",
         to::ActivityKind::Build, "pushed v2.1.0", "12 targets rebuilt"},
        {"a3", std::chrono::seconds(1800), "Charlie", "yellow",
         to::ActivityKind::Join, "joined the team", {}},
    };
    return opts;
}

} // namespace teams_detail

[[nodiscard]] inline Element RenderTeamsView(
    const dsys::TeamsViewPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    auto opts = teams_detail::sample_teams();

    auto content = teams_detail::to::RenderTeamsOverview(
        opts, std::min(80, ctx.term_cols - 4));

    dframe::DialogFrameProps fprops;
    fprops.title = "Teams";
    fprops.style = dframe::FrameStyle::Info;
    fprops.full_border = true;
    fprops.inner_padding_x = 1;
    fprops.inner_padding_y = 0;
    fprops.content = content;

    return dframe::DialogFrame(fprops, ctx.theme)
        | size(WIDTH, EQUAL, std::min(75, ctx.term_cols - 4))
        | size(HEIGHT, EQUAL, std::min(26, ctx.term_rows - 4));
}

inline bool HandleTeamsViewEvent(dsys::TeamsViewPayload& p, const Event& event) {
    if (event == Event::Escape && p.on_close) { p.on_close(); return true; }
    // Tab / Shift+Tab cycle members
    if (event == Event::Tab) {
        p.selected_index = (p.selected_index + 1) % 5;
        return true;
    }
    if (event == Event::ArrowRight || event == Event::Character('l')) {
        p.selected_index = std::min(4, p.selected_index + 1);
        return true;
    }
    if (event == Event::ArrowLeft || event == Event::Character('h')) {
        p.selected_index = std::max(0, p.selected_index - 1);
        return true;
    }
    return false;
}

// ============================================================
// HelpView (modal)
// ============================================================

[[nodiscard]] inline Element RenderHelpView(
    const dsys::HelpViewPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    // If a component is provided (high-fidelity mode), use it directly
    if (p.component) {
        auto comp = std::static_pointer_cast<Component>(p.component);
        if (comp) return (*comp)->Render();
    }
    // Fallback: simple stateless render
    namespace hv = cc::ui::dialogs::help_view;
    hv::HelpViewProps props;
    hv::HelpViewState state;
    return hv::RenderHelpView(props, state, ctx.theme);
}

inline bool HandleHelpViewEvent(dsys::HelpViewPayload& p, const Event& event) {
    // If a component is provided, forward events to it
    if (p.component) {
        auto comp = std::static_pointer_cast<Component>(p.component);
        if (comp && (*comp)->OnEvent(event)) return true;
    }
    if (event == Event::Escape && p.on_close) { p.on_close(); return true; }
    return false;
}

// ============================================================
// AboutDialog (modal)
// ============================================================

[[nodiscard]] inline Element RenderAboutDialog(
    const dsys::AboutDialogPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    // If a component is provided (high-fidelity mode), use it directly
    if (p.component) {
        auto comp = std::static_pointer_cast<Component>(p.component);
        if (comp) return (*comp)->Render();
    }
    // Fallback: simple stateless render
    namespace av = cc::ui::dialogs::about;
    av::AboutDialogProps props;
    props.app_version = p.version;
    props.build_date = p.build_date;
    return av::RenderAboutDialog(props, ctx.theme);
}

inline bool HandleAboutDialogEvent(dsys::AboutDialogPayload& p, const Event& event) {
    // If a component is provided, forward events to it
    if (p.component) {
        auto comp = std::static_pointer_cast<Component>(p.component);
        if (comp && (*comp)->OnEvent(event)) return true;
    }
    if (event == Event::Escape && p.on_close) { p.on_close(); return true; }
    return false;
}

// ============================================================
// QuickOpen (modal)
// ============================================================

[[nodiscard]] inline Element RenderQuickOpen(
    const dsys::QuickOpenPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    namespace qo = cc::ui::dialogs::quick_open;
    return qo::RenderQuickOpen(p, ctx);
}

inline bool HandleQuickOpenEvent(
    dsys::QuickOpenPayload& p,
    const Event& event)
{
    namespace qo = cc::ui::dialogs::quick_open;
    return qo::HandleQuickOpenEvent(p, event);
}

// ============================================================
// PluginDialog (modal)
// ============================================================

[[nodiscard]] inline Element RenderPluginDialog(
    const dsys::PluginDialogPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    namespace pd = cc::ui::dialogs::plugin_dialog;

    auto sub = pd::RenderMainMenu(p.menu_selected);

    dframe::DialogFrameProps fprops;
    fprops.title = "Plugins";
    fprops.style = dframe::FrameStyle::Info;
    fprops.full_border = true;
    fprops.inner_padding_x = 2;
    fprops.inner_padding_y = 1;

    auto footer = hbox({
        text(" ↑/↓") | bold | color(Color::Cyan),
        text(" navigate  "),
        text("⏎") | bold | color(Color::Green),
        text(" select  "),
        text("Esc") | bold | color(Color::Red),
        text(" close"),
        filler(),
        text("5 items") | dim,
    }) | dim;

    fprops.content = vbox({
        sub | flex,
        separator(),
        footer,
    });

    return dframe::DialogFrame(fprops, ctx.theme);
}

inline bool HandlePluginDialogEvent(dsys::PluginDialogPayload& p, const Event& event) {
    if (event == Event::Escape && p.on_close) { p.on_close(); return true; }
    if (event == Event::ArrowUp || event == Event::Character('k')) {
        p.menu_selected = std::max(0, p.menu_selected - 1);
        return true;
    }
    if (event == Event::ArrowDown || event == Event::Character('j')) {
        p.menu_selected = std::min(4, p.menu_selected + 1); // 5 menu cards
        return true;
    }
    if (event == Event::Return) {
        // Enter — for now just close (sub-views TBD in M7.5 follow-up)
        if (p.on_close) p.on_close();
        return true;
    }
    return false;
}

// ============================================================
// MCPDialog (modal)
// ============================================================

[[nodiscard]] inline Element RenderMCPDialog(
    const dsys::MCPDialogPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    // High-fidelity mode: use pre-built component if provided
    if (p.component) {
        auto comp = std::static_pointer_cast<Component>(p.component);
        if (comp) return (*comp)->Render();
    }
    // Fallback: simple stateless render
    namespace md = cc::ui::dialogs::mcp_dialog;
    md::McpDialogOptions opts;
    return md::RenderMcpDialog(opts);
}

inline bool HandleMCPDialogEvent(dsys::MCPDialogPayload& p, const Event& event) {
    // High-fidelity mode: forward events to component
    if (p.component) {
        auto comp = std::static_pointer_cast<Component>(p.component);
        if (comp && (*comp)->OnEvent(event)) return true;
    }
    if (event == Event::Escape && p.on_close) { p.on_close(); return true; }
    return false;
}

// ============================================================
// DiffDialog (modal)
// ============================================================

namespace diff_detail {

[[nodiscard]] inline std::vector<sd::StructuredPatchHunk>
convert_hunks(const std::vector<cc::utils::file_edit::PatchHunk>& phunks) {
    std::vector<sd::StructuredPatchHunk> shunks;
    for (const auto& ph : phunks) {
        sd::StructuredPatchHunk sh;
        sh.old_start = ph.old_start;
        sh.old_lines = ph.old_lines;
        sh.new_start = ph.new_start;
        sh.new_lines = ph.new_lines;
        int ol = ph.old_start, nl = ph.new_start;
        for (const auto& l : ph.lines) {
            if (l.empty()) continue;
            sd::StructuredDiffLine sdl;
            char p = l[0];
            std::string content = l.size() > 1 ? l.substr(1) : "";
            if (p == '+') {
                sdl.type = sd::StructuredDiffLine::Type::Added;
                sdl.new_line_num = nl++;
            } else if (p == '-') {
                sdl.type = sd::StructuredDiffLine::Type::Removed;
                sdl.old_line_num = ol++;
            } else {
                sdl.type = sd::StructuredDiffLine::Type::Context;
                sdl.old_line_num = ol++;
                sdl.new_line_num = nl++;
            }
            sdl.content = std::move(content);
            sh.lines.push_back(std::move(sdl));
        }
        shunks.push_back(std::move(sh));
    }
    return shunks;
}

} // namespace diff_detail

[[nodiscard]] inline Element RenderDiffDialog(
    const dsys::DiffDialogPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    auto phunks = cc::utils::file_edit::compute_structured_patch(
        p.before_text, p.after_text, 3);
    auto hunks = diff_detail::convert_hunks(phunks);

    sd::DiffDetailViewProps diff_props;
    diff_props.file_path = p.file_path.empty() ? "changes" : p.file_path;
    diff_props.hunks = hunks;

    sd::StructuredDiffOptions opts;
    opts.diff = diff_props;
    opts.show_line_numbers = true;
    opts.scroll_offset = p.scroll_offset;
    opts.visible_height = std::max(10, ctx.term_rows - 10);

    auto body = sd::RenderStructuredDiff(opts);

    auto footer = hbox({
        text(" ↑/↓") | bold | color(Color::Cyan),
        text(" scroll  "),
        text("⏎") | bold | color(Color::Green),
        text(" accept  "),
        text("Esc") | bold | color(Color::Red),
        text(" cancel"),
        filler(),
    }) | dim;

    dframe::DialogFrameProps fprops;
    fprops.title = p.title.empty() ? "Diff" : p.title;
    fprops.style = dframe::FrameStyle::Info;
    fprops.full_border = true;
    fprops.inner_padding_x = 1;
    fprops.inner_padding_y = 1;
    fprops.content = vbox({
        body | flex,
        separator(),
        footer,
    });

    return dframe::DialogFrame(fprops, ctx.theme);
}

inline bool HandleDiffDialogEvent(
    dsys::DiffDialogPayload& p,
    const Event& event)
{
    // Accept
    if (event == Event::Return || event == Event::Character('y') ||
        event == Event::Character('Y'))
    {
        if (p.on_response) p.on_response(true);
        if (p.on_close) p.on_close();
        return true;
    }
    // Cancel / close
    if (event == Event::Escape || event == Event::Character('n') ||
        event == Event::Character('N'))
    {
        if (p.on_response) p.on_response(false);
        if (p.on_close) p.on_close();
        return true;
    }
    // Scroll
    if (event == Event::ArrowDown || event == Event::Character('j')) {
        p.scroll_offset += 1;
        return true;
    }
    if (event == Event::ArrowUp || event == Event::Character('k')) {
        p.scroll_offset = std::max(0, p.scroll_offset - 1);
        return true;
    }
    if (event == Event::PageDown) {
        p.scroll_offset += 20;
        return true;
    }
    if (event == Event::PageUp) {
        p.scroll_offset = std::max(0, p.scroll_offset - 20);
        return true;
    }
    return false;
}

// ============================================================
// ConfigDialog (modal)
// ============================================================

[[nodiscard]] inline Element RenderConfigDialog(
    const dsys::ConfigDialogPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    // High-fidelity mode: use pre-built component if provided
    if (p.component) {
        auto comp = std::static_pointer_cast<Component>(p.component);
        if (comp) return (*comp)->Render();
    }
    // Fallback: simple stateless render
    namespace cd = cc::ui::dialogs::config_dialog;
    cd::ConfigDialogOptions opts;
    return cd::RenderConfigDialog(opts);
}

inline bool HandleConfigDialogEvent(dsys::ConfigDialogPayload& p, const Event& event) {
    // High-fidelity mode: forward events to component
    if (p.component) {
        auto comp = std::static_pointer_cast<Component>(p.component);
        if (comp && (*comp)->OnEvent(event)) return true;
    }
    if (event == Event::Escape && p.on_close) { p.on_close(); return true; }
    return false;
}

// ============================================================
// ExportDialog (modal)
// ============================================================

[[nodiscard]] inline Element RenderExportDialog(
    const dsys::ExportDialogPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    namespace ed = cc::ui::dialogs;
    ed::ExportOptions opts;
    if (p.format == "json") opts.format = ed::ExportFormat::Json;
    else if (p.format == "html") opts.format = ed::ExportFormat::Html;
    else opts.format = ed::ExportFormat::Markdown;
    return ed::render_export_dialog(opts);
}

inline bool HandleExportDialogEvent(
    dsys::ExportDialogPayload& p,
    const Event& event)
{
    if (event == Event::Return || event == Event::Character('y') ||
        event == Event::Character('Y'))
    {
        if (p.on_response) p.on_response(true);
        return true;
    }
    if (event == Event::Escape || event == Event::Character('n') ||
        event == Event::Character('N'))
    {
        if (p.on_response) p.on_response(false);
        return true;
    }
    return false;
}

// ============================================================
// GlobalSearch (modal)
// ============================================================

[[nodiscard]] inline Element RenderGlobalSearch(
    const dsys::GlobalSearchPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    namespace gs = cc::ui::dialogs;
    std::vector<gs::SearchResult> results;

    // Build sample results based on query
    if (!p.query.empty()) {
        results.push_back({p.query + " — in files", "12 matching files",
                           gs::SearchCategory::Files});
        results.push_back({p.query + " — in commands", "3 matching commands",
                           gs::SearchCategory::Commands});
        results.push_back({p.query + " — in sessions", "8 matching sessions",
                           gs::SearchCategory::Sessions});
    } else {
        results.push_back({"recent-file.txt", "2 hours ago",
                           gs::SearchCategory::Files});
        results.push_back({"/help", "Show help message",
                           gs::SearchCategory::Commands});
        results.push_back({"Project brainstorming", "Yesterday",
                           gs::SearchCategory::Sessions});
    }

    auto content = vbox({
        hbox({
            text("❯ ") | color(Color::Cyan),
            text(p.query.empty() ? "Search..." : p.query) |
                (p.query.empty() ? dim : bold),
            filler(),
            text(std::to_string(results.size()) + " results") | dim,
        }),
        text(""),
        gs::render_search_results(results, 0),
    });

    dframe::DialogFrameProps props;
    props.title = "Search";
    props.subtitle = "Find files, commands, and sessions";
    props.style = dframe::FrameStyle::Default;
    props.content = content;
    props.inner_padding_x = 1;
    props.inner_padding_y = 1;
    props.full_border = true;
    props.rounded = true;

    return dframe::DialogFrame(props, ctx.theme)
        | size(WIDTH, EQUAL, std::min(60, ctx.term_cols - 4))
        | size(HEIGHT, EQUAL, std::min(18, ctx.term_rows - 4));
}

inline bool HandleGlobalSearchEvent(
    dsys::GlobalSearchPayload& p, const Event& event)
{
    // Character input adds to query
    if (event.is_character()) {
        char c = event.character()[0];
        if (std::isprint(static_cast<unsigned char>(c))) {
            p.query += c;
            return true;
        }
    }
    // Backspace removes last char
    if (event == Event::Backspace || event == Event::Delete) {
        if (!p.query.empty()) {
            p.query.pop_back();
            return true;
        }
    }
    // Escape closes
    if (event == Event::Escape) {
        if (p.on_close) p.on_close();
        return true;
    }
    // Enter selects
    if (event == Event::Return) {
        if (p.on_close) p.on_close();
        return true;
    }
    return false;
}

// ============================================================
// HistorySearch (modal)
// ============================================================

namespace history_detail {

/// Format a relative timestamp from milliseconds since epoch.
[[nodiscard]] inline std::string format_relative_time(int64_t ts_ms) {
    using namespace std::chrono;
    auto now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    auto diff_ms = now - ts_ms;

    if (diff_ms < 60 * 1000) return "Just now";
    if (diff_ms < 60 * 60 * 1000) {
        int mins = static_cast<int>(diff_ms / (60 * 1000));
        return std::format("{} min{} ago", mins, mins == 1 ? "" : "s");
    }
    if (diff_ms < 24 * 60 * 60 * 1000) {
        int hours = static_cast<int>(diff_ms / (60 * 60 * 1000));
        return std::format("{} hour{} ago", hours, hours == 1 ? "" : "s");
    }
    if (diff_ms < 7 * 24 * 60 * 60 * 1000) {
        int days = static_cast<int>(diff_ms / (24 * 60 * 60 * 1000));
        return std::format("{} day{} ago", days, days == 1 ? "" : "s");
    }
    // Older — format as date
    auto ts = system_clock::time_point(milliseconds(ts_ms));
    std::time_t t = system_clock::to_time_t(ts);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%b %d, %Y", &tm_buf);
    return std::string(buf);
}

/// Get the first user message as a conversation title.
[[nodiscard]] inline std::string get_first_message(const cc::history::SessionHistory& h) {
    for (const auto& m : h.messages) {
        if (m.role == cc::history::Role::User && !m.content_json.empty()) {
            std::string s = m.content_json;
            // Strip quotes if it's a JSON string
            if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
                s = s.substr(1, s.size() - 2);
            }
            if (s.size() > 60) {
                s = s.substr(0, 57) + "...";
            }
            return s.empty() ? "(empty)" : s;
        }
    }
    return "(no messages)";
}

/// Fetch and filter sessions from HistoryManager.
/// `selected_index` is clamped to the result range.
[[nodiscard]] inline std::vector<cc::ui::dialogs::HistoryEntry>
fetch_sessions(std::string_view query, int& selected_index) {
    std::vector<cc::ui::dialogs::HistoryEntry> entries;

    auto& mgr = cc::history::HistoryManager::instance();
    auto session_ids = mgr.list_sessions();

    for (const auto& sid : session_ids) {
        auto* session = mgr.get_session(sid);
        if (!session) continue;

        std::string title = get_first_message(*session);

        // Filter by query (case-insensitive substring match)
        if (!query.empty()) {
            std::string lower_title, lower_query, lower_id;
            lower_title.reserve(title.size());
            for (char c : title) lower_title += std::tolower(static_cast<unsigned char>(c));
            lower_query.reserve(query.size());
            for (char c : query) lower_query += std::tolower(static_cast<unsigned char>(c));
            lower_id.reserve(sid.size());
            for (char c : sid) lower_id += std::tolower(static_cast<unsigned char>(c));

            if (lower_title.find(lower_query) == std::string::npos &&
                lower_id.find(lower_query) == std::string::npos) {
                continue;
            }
        }

        cc::ui::dialogs::HistoryEntry e;
        e.session_id = session->session_id;
        e.first_message = title;
        e.timestamp = format_relative_time(session->last_active_ms);
        e.project_name = std::nullopt;
        entries.push_back(std::move(e));
    }

    // Sort by session_id descending (rough proxy for recency since ids are often time-based)
    // TODO: sort by last_active_ms once HistoryEntry carries numeric timestamps
    std::sort(entries.begin(), entries.end(),
        [](const cc::ui::dialogs::HistoryEntry& a, const cc::ui::dialogs::HistoryEntry& b) {
            return a.session_id > b.session_id;
        });

    // Clamp selection
    if (selected_index < 0) selected_index = 0;
    if (!entries.empty() && selected_index >= static_cast<int>(entries.size())) {
        selected_index = static_cast<int>(entries.size()) - 1;
    }

    return entries;
}

} // namespace history_detail

[[nodiscard]] inline Element RenderHistorySearch(
    const dsys::HistorySearchPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    namespace hs = cc::ui::dialogs;

    int sel = p.selected_index;
    auto entries = history_detail::fetch_sessions(p.query, sel);

    auto content = vbox({
        hbox({
            text("❯ ") | color(Color::Cyan),
            text(p.query.empty() ? "Search history..." : p.query) |
                (p.query.empty() ? dim : bold),
            filler(),
            text(std::to_string(entries.size()) + " sessions") | dim,
        }),
        text(""),
        hs::render_history_search(entries, static_cast<std::size_t>(sel)),
    });

    dframe::DialogFrameProps props;
    props.title = "History Search";
    props.subtitle = "Find past conversations";
    props.style = dframe::FrameStyle::Default;
    props.content = content;
    props.inner_padding_x = 1;
    props.inner_padding_y = 1;
    props.full_border = true;
    props.rounded = true;

    return dframe::DialogFrame(props, ctx.theme)
        | size(WIDTH, EQUAL, std::min(60, ctx.term_cols - 4))
        | size(HEIGHT, EQUAL, std::min(18, ctx.term_rows - 4));
}

inline bool HandleHistorySearchEvent(
    dsys::HistorySearchPayload& p, const Event& event)
{
    // Character input adds to query
    if (event.is_character()) {
        char c = event.character()[0];
        if (std::isprint(static_cast<unsigned char>(c))) {
            p.query += c;
            p.selected_index = 0;
            return true;
        }
    }
    // Backspace removes last char
    if (event == Event::Backspace || event == Event::Delete) {
        if (!p.query.empty()) {
            p.query.pop_back();
            p.selected_index = 0;
            return true;
        }
    }
    // Navigation
    if (event == Event::ArrowUp || event == Event::Character('k')) {
        p.selected_index = std::max(0, p.selected_index - 1);
        return true;
    }
    if (event == Event::ArrowDown || event == Event::Character('j')) {
        p.selected_index = p.selected_index + 1;  // clamped on next render
        return true;
    }
    // Escape cancels
    if (event == Event::Escape) {
        if (p.on_select) p.on_select({});
        return true;
    }
    // Enter selects
    if (event == Event::Return) {
        if (p.on_select) {
            int sel = p.selected_index;
            auto entries = history_detail::fetch_sessions(p.query, sel);
            if (sel >= 0 && sel < static_cast<int>(entries.size())) {
                p.on_select(entries[sel].session_id);
            }
        }
        return true;
    }
    return false;
}

// ============================================================
// FeedbackSurvey (modal)
// ============================================================

[[nodiscard]] inline Element RenderFeedbackSurvey(
    const dsys::FeedbackSurveyPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    namespace fb = cc::ui::feedback_survey;
    auto content = fb::RenderFeedbackSurvey(
        detail::map_feedback_state(p.state),
        p.message);

    dframe::DialogFrameProps props;
    props.title = "Feedback";
    props.subtitle = "How is Claude doing?";
    props.style = dframe::FrameStyle::Info;
    props.content = content;
    props.inner_padding_x = 2;
    props.inner_padding_y = 1;
    props.full_border = true;
    props.rounded = true;

    return dframe::DialogFrame(props, ctx.theme)
        | size(WIDTH, EQUAL, std::min(60, ctx.term_cols - 4));
}

inline bool HandleFeedbackSurveyEvent(
    dsys::FeedbackSurveyPayload& p, const Event& event)
{
    if (p.state != dsys::FeedbackSurveyState::Open) {
        // Non-interactive states — any key dismisses
        if (event == Event::Escape || event == Event::Return ||
            event.is_character())
        {
            if (p.on_close) p.on_close();
            return true;
        }
        return false;
    }

    // Open state — handle digit input for ratings
    if (event.is_character() && event.character().size() == 1) {
        char ch = event.character()[0];
        if (ch == '1' || ch == '2' || ch == '3') {
            int rating = ch - '0';
            p.state = dsys::FeedbackSurveyState::Thanks;
            if (p.on_submit) p.on_submit(rating);
            return true;
        }
        if (ch == '0') {
            p.state = dsys::FeedbackSurveyState::Closed;
            if (p.on_submit) p.on_submit(0);
            if (p.on_close) p.on_close();
            return true;
        }
    }
    // Escape to dismiss
    if (event == Event::Escape) {
        p.state = dsys::FeedbackSurveyState::Closed;
        if (p.on_close) p.on_close();
        return true;
    }
    return false;
}

// ============================================================
// ManagedSettingsSecurity (modal)
// ============================================================

/// Render the ManagedSettingsSecurity dialog using the real implementation.
[[nodiscard]] inline Element RenderManagedSettingsSecurity(
    const dsys::ManagedSettingsSecurityPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    namespace mss = cc::ui::dialogs;
    mss::ManagedSettingsSecurityProps props;
    props.categories = mss::default_managed_categories();
    props.organization_name = p.organization_name.value_or("");
    props.dialog_width = std::min(72, ctx.term_cols - 4);
    return mss::RenderManagedSettingsSecurityDialog(
        props.organization_name,
        props.categories,
        p.selected_index,
        props.dialog_width);
}

inline bool HandleManagedSettingsSecurityEvent(
    dsys::ManagedSettingsSecurityPayload& p, const Event& event)
{
    namespace mss = cc::ui::dialogs;
    auto cats = mss::default_managed_categories();
    auto cat_count = static_cast<int>(cats.size());

    // Navigation
    if (event == Event::ArrowUp || event == Event::Character('k') ||
        event == Event::Character('K'))
    {
        p.selected_index = std::max(0, p.selected_index - 1);
        return true;
    }
    if (event == Event::ArrowDown || event == Event::Character('j') ||
        event == Event::Character('J'))
    {
        p.selected_index = std::max(0,
            std::min(cat_count - 1, p.selected_index + 1));
        return true;
    }

    // Enter / Esc to acknowledge and close
    if (event == Event::Return || event == Event::Escape) {
        if (p.on_close) p.on_close();
        return true;
    }
    return false;
}

// ============================================================
// TrustDialog (modal) — real implementation
// ============================================================

namespace trust_detail {

using trust_dialog::DialogState;
using trust_dialog::TrustChoice;
using RL = trust_dialog::RiskLevel;

/// Replicates the navigation + gate logic from MakeTrustDialogComponent's
/// CatchEvent lambda, operating directly on the shared state.
inline bool HandleTrustEvent(DialogState& s, const Event& event)
{
    const auto level = s.props.forced_level;
    const int btn_count = trust_dialog::tier_button_count(level);

    // Navigation
    if (event == Event::ArrowUp || event == Event::Character('k')) {
        s.selected = std::max(0, s.selected - 1);
        return true;
    }
    if (event == Event::ArrowDown || event == Event::Character('j')) {
        s.selected = std::min(btn_count - 1, s.selected + 1);
        return true;
    }
    // Toggle details panel
    if (event == Event::Character('d') || event == Event::Character('D')) {
        s.show_details = !s.show_details;
        return true;
    }
    // Toggle current-tier checkbox(es)
    if (event == Event::Character(' ')) {
        trust_dialog::toggle_selected_gate(s);
        return true;
    }
    // Critical tier: typed input for YES confirmation
    if (level == RL::Critical &&
        (s.understand_risk || s.show_second_confirm))
    {
        if (event.is_character()) {
            const char c = event.character()[0];
            if (s.yes_input.size() < 3) {
                s.yes_input.push_back(c);
            }
            return true;
        }
        if (event == Event::Backspace) {
            if (!s.yes_input.empty()) s.yes_input.pop_back();
            return true;
        }
    }
    // Confirm
    if (event == Event::Return) {
        if (level == RL::High && s.countdown_remaining > 0) {
            return true; // gated
        }
        if (level == RL::Critical) {
            if (!s.understand_risk) return true;
            if (s.yes_input != std::string{trust_dialog::kCriticalConfirmWord}) return true;
        }

        TrustChoice choice = TrustChoice::Cancel;
        switch (level) {
            case RL::Low:
                choice = (s.selected == 0) ? TrustChoice::AllowOnce
                                           : TrustChoice::Cancel;
                break;
            case RL::Medium:
                if (s.selected == 0)       choice = TrustChoice::AllowOnce;
                else if (s.selected == 1)  choice = TrustChoice::AlwaysAllow;
                else                       choice = TrustChoice::Cancel;
                break;
            case RL::High:
                if (s.selected == 0) {
                    choice = s.always_allow ? TrustChoice::AlwaysAllow
                                            : TrustChoice::AllowOnce;
                } else if (s.selected == 1) {
                    choice = TrustChoice::AlwaysAllow;
                } else if (s.selected == 2) {
                    choice = TrustChoice::ViewFile;
                } else {
                    choice = TrustChoice::Cancel;
                }
                break;
            case RL::Critical:
                choice = (s.selected == 0) ? TrustChoice::EnableAnyway
                                           : TrustChoice::Cancel;
                break;
        }
        if (s.props.on_done) {
            auto cb = std::move(s.props.on_done);
            cb(choice);
        }
        return true;
    }
    // Cancel
    if (event == Event::Escape) {
        if (s.props.on_done) {
            auto cb = std::move(s.props.on_done);
            cb(TrustChoice::Cancel);
        }
        return true;
    }
    return false;
}

} // namespace trust_detail

[[nodiscard]] inline Element RenderTrustDialog(
    const dsys::TrustDialogPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    if (!p.state) {
        return detail::RenderStubDialog(
            "Trust", "Trust dialog (no state)", ctx.theme);
    }
    auto state = std::static_pointer_cast<
        cc::ui::trust_dialog::DialogState>(p.state);
    return trust_dialog::RenderTrustDialogFull(state);
}

inline bool HandleTrustDialogEvent(
    dsys::TrustDialogPayload& p,
    const Event& event)
{
    if (!p.state) return false;
    auto state = std::static_pointer_cast<
        cc::ui::trust_dialog::DialogState>(p.state);
    return trust_detail::HandleTrustEvent(*state, event);
}

// ============================================================
// Onboarding (standalone) — real FTXUI implementation
// ============================================================

namespace onboarding_detail {

using Step = cc::ui::dialogs::OnboardingStep;

[[nodiscard]] inline std::vector<Step> default_steps() {
    return cc::ui::dialogs::get_onboarding_steps();
}

/// Render a progress dot row with coloured dots for each step.
[[nodiscard]] inline Element RenderProgressRow(
    const std::vector<Step>& steps, int current)
{
    Elements dots;
    const int n = static_cast<int>(steps.size());
    for (int i = 0; i < n; ++i) {
        if (steps[i].completed) {
            dots.push_back(text("●") | color(Color::Green));
        } else if (i == current) {
            dots.push_back(text("◉") | color(Color::Cyan));
        } else {
            dots.push_back(text("○") | dim);
        }
        if (i + 1 < n) dots.push_back(text("─") | dim);
    }
    return hbox(std::move(dots));
}

/// Render the onboarding dialog body (step content + nav hints).
[[nodiscard]] inline Element RenderOnboardingBody(
    const std::vector<Step>& steps, int current)
{
    const int n = static_cast<int>(steps.size());
    const auto& step = steps[static_cast<std::size_t>(current)];

    Elements body;
    body.push_back(text(""));
    body.push_back(hbox({
        text("Step ") | dim,
        text(std::to_string(current + 1)) | bold,
        text("/") | dim,
        text(std::to_string(n)) | dim,
        text(": ") | dim,
        text(step.title) | bold,
    }));
    body.push_back(text(""));
    body.push_back(paragraph(step.description));
    if (step.action.has_value()) {
        body.push_back(text(""));
        body.push_back(hbox({
            text("→ ") | color(Color::Cyan),
            text(*step.action) | color(Color::Cyan),
        }));
    }
    body.push_back(text(""));

    body.push_back(separator());
    body.push_back(hbox({
        text("[←/→] ") | bold,
        text("navigate ") | dim,
        text("  [Enter] ") | bold,
        text("continue ") | dim,
        text("  [Esc] ") | bold,
        text("skip") | dim,
    }) | dim);

    return vbox(std::move(body));
}

} // namespace onboarding_detail

[[nodiscard]] inline Element RenderOnboarding(
    const dsys::OnboardingPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    namespace ob = onboarding_detail;
    auto steps = ob::default_steps();
    const int step = std::clamp(p.step, 0, static_cast<int>(steps.size()) - 1);

    Elements content;
    content.push_back(hbox({
        text("🎉 ") | bold,
        text("Welcome to Claude Code") | bold | color(Color::Cyan),
    }));
    content.push_back(separator());
    content.push_back(ob::RenderProgressRow(steps, step));
    content.push_back(ob::RenderOnboardingBody(steps, step));

    return dframe::DialogFrame(dframe::DialogFrameProps{
        .title = "Onboarding",
        .style = dframe::FrameStyle::Info,
        .content = vbox(std::move(content)),
    }, ctx.theme) | size(WIDTH, LESS_THAN, 64);
}

inline bool HandleOnboardingEvent(
    dsys::OnboardingPayload& p,
    const Event& event)
{
    constexpr int kTotalSteps = 5;

    if (event == Event::ArrowLeft || event == Event::Character('h')) {
        p.step = std::max(0, p.step - 1);
        return true;
    }
    if (event == Event::ArrowRight || event == Event::Character('l')) {
        p.step = std::min(kTotalSteps - 1, p.step + 1);
        return true;
    }
    if (event == Event::Return) {
        if (p.step >= kTotalSteps - 1) {
            if (p.on_complete) p.on_complete(true);
        } else {
            p.step = std::min(kTotalSteps - 1, p.step + 1);
        }
        return true;
    }
    if (event == Event::Escape) {
        if (p.on_complete) p.on_complete(false);
        return true;
    }
    return false;
}

// ============================================================
// InstallGitHubAppWizard (standalone) — real implementation
// ============================================================

namespace github_wiz_detail {

using Options = cc::ui::dialogs::install_github_app_wizard::
    InstallGitHubAppWizardOptions;
using WizPtr = std::shared_ptr<Component>;

/// Ensure the component is created; returns it.
[[nodiscard]] inline WizPtr ensure_component(
    const std::shared_ptr<void>& handle,
    std::shared_ptr<void>& slot,
    const std::function<void(bool)>& on_complete)
{
    if (handle) {
        return std::static_pointer_cast<Component>(handle);
    }
    Options opts;
    opts.on_complete = [on_complete](auto) {
        if (on_complete) on_complete(true);
    };
    opts.on_cancel = [on_complete] {
        if (on_complete) on_complete(false);
    };
    auto comp = std::make_shared<Component>(
        cc::ui::dialogs::install_github_app_wizard::
            MakeInstallGitHubAppWizard(std::move(opts)));
    slot = comp;
    return comp;
}

} // namespace github_wiz_detail

[[nodiscard]] inline Element RenderInstallGitHubAppWizard(
    const dsys::InstallGitHubAppWizardPayload& p,
    const dsys::DialogRenderContext&)
{
    if (!p.component) return text("Install GitHub App…");
    auto comp = std::static_pointer_cast<Component>(p.component);
    return (*comp)->Render();
}

inline bool HandleInstallGitHubAppWizardEvent(
    dsys::InstallGitHubAppWizardPayload& p,
    const Event& event)
{
    // Lazy-create the component on first event.
    if (!p.component) {
        using namespace cc::ui::dialogs::install_github_app_wizard;
        InstallGitHubAppWizardOptions opts;
        opts.on_complete = [cb = p.on_complete](auto) {
            if (cb) cb(true);
        };
        opts.on_cancel = [cb = p.on_complete] {
            if (cb) cb(false);
        };
        p.component = std::make_shared<Component>(
            MakeInstallGitHubAppWizard(std::move(opts)));
    }
    auto comp = std::static_pointer_cast<Component>(p.component);
    return (*comp)->OnEvent(event);
}

// ============================================================
// InstallSlackAppWizard (standalone) — real implementation
// ============================================================

[[nodiscard]] inline Element RenderInstallSlackAppWizard(
    const dsys::InstallSlackAppWizardPayload& p,
    const dsys::DialogRenderContext&)
{
    if (!p.component) return text("Install Slack App…");
    auto comp = std::static_pointer_cast<Component>(p.component);
    return (*comp)->Render();
}

inline bool HandleInstallSlackAppWizardEvent(
    dsys::InstallSlackAppWizardPayload& p,
    const Event& event)
{
    if (!p.component) {
        using namespace cc::ui::dialogs::install_slack_app_wizard;
        InstallSlackAppWizardOptions opts;
        opts.on_complete = [cb = p.on_complete] {
            if (cb) cb(true);
        };
        opts.on_cancel = [cb = p.on_complete] {
            if (cb) cb(false);
        };
        p.component = std::make_shared<Component>(
            MakeInstallSlackAppWizard(std::move(opts)));
    }
    auto comp = std::static_pointer_cast<Component>(p.component);
    return (*comp)->OnEvent(event);
}

// ============================================================
// CreateAgentWizard (standalone) — real implementation
// ============================================================

[[nodiscard]] inline Element RenderCreateAgentWizard(
    const dsys::CreateAgentWizardPayload& p,
    const dsys::DialogRenderContext&)
{
    if (!p.component) return text("Create Agent…");
    auto comp = std::static_pointer_cast<Component>(p.component);
    return (*comp)->Render();
}

inline bool HandleCreateAgentWizardEvent(
    dsys::CreateAgentWizardPayload& p,
    const Event& event)
{
    if (!p.component) {
        namespace aw = cc::ui::agents::wizard;
        aw::AgentWizardOptions opts;
        opts.on_save = [cb = p.on_complete](const auto& agent) {
            if (cb) cb(true, agent.name);
        };
        opts.on_cancel = [cb = p.on_complete] {
            if (cb) cb(false, "");
        };
        p.component = std::make_shared<Component>(
            aw::AgentWizard(std::move(opts)));
    }
    auto comp = std::static_pointer_cast<Component>(p.component);
    return (*comp)->OnEvent(event);
}

// ============================================================
// EditAgentWizard (standalone) — real implementation
// ============================================================

[[nodiscard]] inline Element RenderEditAgentWizard(
    const dsys::EditAgentWizardPayload& p,
    const dsys::DialogRenderContext&)
{
    if (!p.component) return text("Edit Agent…");
    auto comp = std::static_pointer_cast<Component>(p.component);
    return (*comp)->Render();
}

inline bool HandleEditAgentWizardEvent(
    dsys::EditAgentWizardPayload& p,
    const Event& event)
{
    if (!p.component) {
        namespace aw = cc::ui::agents::wizard;
        namespace cards = cc::ui::agents::cards;
        aw::AgentWizardOptions opts;
        cards::AgentCardData card;
        card.name = p.agent_name;
        opts.edit_agent = std::move(card);
        opts.on_save = [cb = p.on_complete](const auto&) {
            if (cb) cb(true);
        };
        opts.on_cancel = [cb = p.on_complete] {
            if (cb) cb(false);
        };
        p.component = std::make_shared<Component>(
            aw::AgentWizard(std::move(opts)));
    }
    auto comp = std::static_pointer_cast<Component>(p.component);
    return (*comp)->OnEvent(event);
}

// ============================================================
// Registry setup — register all remaining dialog renderers
// ============================================================

/// Register all remaining dialog renderers into the registry.
/// Completes the dialog inventory: all DialogType values get
/// a renderer (real or stub) after this call + the other
/// renderer registration modules.
void register_all_renderers(dsys::DialogRendererRegistry& registry) {
    // Helper macro-ish pattern — register each dialog type

    // --- Bottom slot ---

    // MessageSelector (band 1)
    registry.register_dialog(
        dsys::DialogType::MessageSelector,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::MessageSelectorPayload>(&payload);
            if (!p) return text("");
            return RenderMessageSelector(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::MessageSelectorPayload>(&payload);
            if (!p) return false;
            return HandleMessageSelectorEvent(*p, event);
        }
    );

    // --- Modal slot ---

    // SettingsPanel
    registry.register_dialog(
        dsys::DialogType::SettingsPanel,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::SettingsPanelPayload>(&payload);
            if (!p) return text("");
            return RenderSettingsPanel(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::SettingsPanelPayload>(&payload);
            if (!p) return false;
            return HandleSettingsPanelEvent(*p, event);
        }
    );

    // TasksView
    registry.register_dialog(
        dsys::DialogType::TasksView,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::TasksViewPayload>(&payload);
            if (!p) return text("");
            return RenderTasksView(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::TasksViewPayload>(&payload);
            if (!p) return false;
            return HandleTasksViewEvent(*p, event);
        }
    );

    // TeamsView
    registry.register_dialog(
        dsys::DialogType::TeamsView,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::TeamsViewPayload>(&payload);
            if (!p) return text("");
            return RenderTeamsView(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::TeamsViewPayload>(&payload);
            if (!p) return false;
            return HandleTeamsViewEvent(*p, event);
        }
    );

    // HelpView
    registry.register_dialog(
        dsys::DialogType::HelpView,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::HelpViewPayload>(&payload);
            if (!p) return text("");
            return RenderHelpView(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::HelpViewPayload>(&payload);
            if (!p) return false;
            return HandleHelpViewEvent(*p, event);
        }
    );

    // AboutDialog
    registry.register_dialog(
        dsys::DialogType::AboutDialog,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::AboutDialogPayload>(&payload);
            if (!p) return text("");
            return RenderAboutDialog(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::AboutDialogPayload>(&payload);
            if (!p) return false;
            return HandleAboutDialogEvent(*p, event);
        }
    );

    // QuickOpen
    registry.register_dialog(
        dsys::DialogType::QuickOpen,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::QuickOpenPayload>(&payload);
            if (!p) return text("");
            return RenderQuickOpen(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::QuickOpenPayload>(&payload);
            if (!p) return false;
            return HandleQuickOpenEvent(*p, event);
        }
    );

    // PluginDialog
    registry.register_dialog(
        dsys::DialogType::PluginDialog,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::PluginDialogPayload>(&payload);
            if (!p) return text("");
            return RenderPluginDialog(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::PluginDialogPayload>(&payload);
            if (!p) return false;
            return HandlePluginDialogEvent(*p, event);
        }
    );

    // MCPDialog
    registry.register_dialog(
        dsys::DialogType::MCPDialog,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::MCPDialogPayload>(&payload);
            if (!p) return text("");
            return RenderMCPDialog(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::MCPDialogPayload>(&payload);
            if (!p) return false;
            return HandleMCPDialogEvent(*p, event);
        }
    );

    // DiffDialog
    registry.register_dialog(
        dsys::DialogType::DiffDialog,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::DiffDialogPayload>(&payload);
            if (!p) return text("");
            return RenderDiffDialog(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::DiffDialogPayload>(&payload);
            if (!p) return false;
            return HandleDiffDialogEvent(*p, event);
        }
    );

    // ConfigDialog
    registry.register_dialog(
        dsys::DialogType::ConfigDialog,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::ConfigDialogPayload>(&payload);
            if (!p) return text("");
            return RenderConfigDialog(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::ConfigDialogPayload>(&payload);
            if (!p) return false;
            return HandleConfigDialogEvent(*p, event);
        }
    );

    // ExportDialog
    registry.register_dialog(
        dsys::DialogType::ExportDialog,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::ExportDialogPayload>(&payload);
            if (!p) return text("");
            return RenderExportDialog(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::ExportDialogPayload>(&payload);
            if (!p) return false;
            return HandleExportDialogEvent(*p, event);
        }
    );

    // GlobalSearch
    registry.register_dialog(
        dsys::DialogType::GlobalSearch,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::GlobalSearchPayload>(&payload);
            if (!p) return text("");
            return RenderGlobalSearch(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::GlobalSearchPayload>(&payload);
            if (!p) return false;
            return HandleGlobalSearchEvent(*p, event);
        }
    );

    // HistorySearch
    registry.register_dialog(
        dsys::DialogType::HistorySearch,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::HistorySearchPayload>(&payload);
            if (!p) return text("");
            return RenderHistorySearch(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::HistorySearchPayload>(&payload);
            if (!p) return false;
            return HandleHistorySearchEvent(*p, event);
        }
    );

    // FeedbackSurvey
    registry.register_dialog(
        dsys::DialogType::FeedbackSurvey,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::FeedbackSurveyPayload>(&payload);
            if (!p) return text("");
            return RenderFeedbackSurvey(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::FeedbackSurveyPayload>(&payload);
            if (!p) return false;
            return HandleFeedbackSurveyEvent(*p, event);
        }
    );

    // ManagedSettingsSecurity
    registry.register_dialog(
        dsys::DialogType::ManagedSettingsSecurity,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::ManagedSettingsSecurityPayload>(&payload);
            if (!p) return text("");
            return RenderManagedSettingsSecurity(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::ManagedSettingsSecurityPayload>(&payload);
            if (!p) return false;
            return HandleManagedSettingsSecurityEvent(*p, event);
        }
    );

    // --- Standalone ---

    // TrustDialog
    registry.register_dialog(
        dsys::DialogType::TrustDialog,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::TrustDialogPayload>(&payload);
            if (!p) return text("");
            return RenderTrustDialog(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::TrustDialogPayload>(&payload);
            if (!p) return false;
            return HandleTrustDialogEvent(*p, event);
        }
    );

    // Onboarding
    registry.register_dialog(
        dsys::DialogType::Onboarding,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::OnboardingPayload>(&payload);
            if (!p) return text("");
            return RenderOnboarding(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::OnboardingPayload>(&payload);
            if (!p) return false;
            return HandleOnboardingEvent(*p, event);
        }
    );

    // InstallGitHubAppWizard
    registry.register_dialog(
        dsys::DialogType::InstallGitHubAppWizard,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::InstallGitHubAppWizardPayload>(&payload);
            if (!p) return text("");
            return RenderInstallGitHubAppWizard(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::InstallGitHubAppWizardPayload>(&payload);
            if (!p) return false;
            return HandleInstallGitHubAppWizardEvent(*p, event);
        }
    );

    // InstallSlackAppWizard
    registry.register_dialog(
        dsys::DialogType::InstallSlackAppWizard,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::InstallSlackAppWizardPayload>(&payload);
            if (!p) return text("");
            return RenderInstallSlackAppWizard(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::InstallSlackAppWizardPayload>(&payload);
            if (!p) return false;
            return HandleInstallSlackAppWizardEvent(*p, event);
        }
    );

    // CreateAgentWizard
    registry.register_dialog(
        dsys::DialogType::CreateAgentWizard,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::CreateAgentWizardPayload>(&payload);
            if (!p) return text("");
            return RenderCreateAgentWizard(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::CreateAgentWizardPayload>(&payload);
            if (!p) return false;
            return HandleCreateAgentWizardEvent(*p, event);
        }
    );

    // EditAgentWizard
    registry.register_dialog(
        dsys::DialogType::EditAgentWizard,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::EditAgentWizardPayload>(&payload);
            if (!p) return text("");
            return RenderEditAgentWizard(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::EditAgentWizardPayload>(&payload);
            if (!p) return false;
            return HandleEditAgentWizardEvent(*p, event);
        }
    );
}

} // namespace cc::ui::dialogs::all_renderers
