/// @file agent_list.cppm
/// @brief Full agent-list view: Grid/List tabs, search+filter+sort toolbar,
/// hover context menu, j/k + r/e/d/a + / keyboard nav, summary footer.
///
/// Replaces the old skeleton (same filename) and consolidates migration of:
///   - src/components/agents/AgentsList.tsx   (~1,300 lines, grid+list)
///   - src/components/agents/AgentsMenu.tsx   (pane toolbar)
///   - src/components/agents/AgentNavigationFooter.tsx
///   - quick-action hover menu helpers inside AgentsMenu
///
/// Two views (Tab toggled):
///   * Grid view — LargeCard layout, fixed 2 columns per row
///                 (adaptive 1-2-3-4 based on width deferred to FTXUI resize support)
///   * List view — vertical MiniCard list
///
/// Top toolbar: search box (+New Agent big-green) + Filter dropdown + Sort.
/// Bottom summary: "N agents · M running · avg cost per run: $0.0X".
///
/// Reuses:
///   - cc.ui.agents.agent_cards       (Mini/Card/Large)
///   - cc.ui.agents.shared_widgets    (RunStatsBar, SharedAnimState)
///   - ui.components.tag_tabs         (Grid/List tab switcher)
///   - cc.ui.custom_select            (Filter / Sort dropdowns)
module;

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.agents.agent_list;

import cc.ui.agents.shared_widgets;
import cc.ui.agents.agent_cards;
import cc.ui.components.tag_tabs;
import cc.ui.custom_select;

export namespace cc::ui::agents::list_view {
using namespace ftxui;

using cards::AgentCardData;
using cards::CardSize;
using cards::AgentCardComponent;
using cards::AgentCardCallbacks;
using shared::AgentStatus;
using shared::RunStats;
using shared::RunStatsBar;
using shared::SharedAnimState;
using ui::components::TagTabsOptions;
using ui::components::Tab;
using ui::components::TagTabsComponent;
using cc::ui::custom_select::SelectMode;
using cc::ui::custom_select::CustomSelectOptions;
using cc::ui::custom_select::SelectOption;

// ============================================================
// Enums
// ============================================================

/// Which layout is active.
enum class ViewMode : std::uint8_t {
    Grid,  // LargeCard 2-col
    List,  // MiniCard vertical
};

/// Quick status filter.
enum class FilterPreset : std::uint8_t {
    All,
    Running,
    Idle,
    Errored,
    Disabled,
};

/// Sort order for cards.
enum class SortKey : std::uint8_t {
    Name,
    LastRun,
    Cost,
};

// ============================================================
// Data Model (thin; full types live in tools/agent_tool)
// ============================================================

/// Aggregated input for the agent list view.
struct AgentListData {
    std::vector<AgentCardData> agents;
    RunStats totals{};           // pre-computed overall summary
    std::string filter_query;    // search box contents
    FilterPreset filter = FilterPreset::All;
    SortKey sort = SortKey::Name;
};

// ============================================================
// Search + Filter + Sort helpers
// ============================================================

/// Lowercase an ASCII string (case-insensitive search).
[[nodiscard]] inline std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

/// Return true if the agent matches the free-text query (name / tag / desc).
[[nodiscard]] inline bool matches_query(
    const AgentCardData& a, std::string_view query)
{
    if (query.empty()) return true;
    std::string q = to_lower(query);
    if (to_lower(a.name).find(q) != std::string::npos) return true;
    if (to_lower(a.description).find(q) != std::string::npos) return true;
    if (to_lower(a.description_long).find(q) != std::string::npos) return true;
    for (const auto& t : a.role_tags)
        if (to_lower(t).find(q) != std::string::npos) return true;
    for (const auto& t : a.tools)
        if (to_lower(t).find(q) != std::string::npos) return true;
    return false;
}

/// Return true if the agent passes the status filter preset.
[[nodiscard]] inline bool matches_status(
    const AgentCardData& a, FilterPreset f)
{
    switch (f) {
        case FilterPreset::All:      return true;
        case FilterPreset::Running:  return a.status == AgentStatus::Running;
        case FilterPreset::Idle:     return a.status == AgentStatus::Idle;
        case FilterPreset::Errored:  return a.status == AgentStatus::Errored;
        case FilterPreset::Disabled: return a.status == AgentStatus::Disabled;
    }
    return true;
}

/// Apply query + status + sort; returns a filtered/sorted copy.
[[nodiscard]] inline std::vector<AgentCardData> apply_view_options(
    const AgentListData& data)
{
    std::vector<AgentCardData> out;
    out.reserve(data.agents.size());
    for (const auto& a : data.agents) {
        if (!matches_query(a, data.filter_query)) continue;
        if (!matches_status(a, data.filter)) continue;
        out.push_back(a);
    }

    switch (data.sort) {
        case SortKey::Name:
            std::ranges::sort(out, [](const AgentCardData& a, const AgentCardData& b) {
                return to_lower(a.name) < to_lower(b.name);
            });
            break;
        case SortKey::LastRun:
            std::ranges::sort(out, [](const AgentCardData& a, const AgentCardData& b) {
                return (a.last_run_at.value_or("")) > (b.last_run_at.value_or(""));
            });
            break;
        case SortKey::Cost:
            std::ranges::sort(out, [](const AgentCardData& a, const AgentCardData& b) {
                return a.stats.avg_cost_usd > b.stats.avg_cost_usd;
            });
            break;
    }

    return out;
}

// ============================================================
// Summary footer
// ============================================================

/// Render: "N agents · M running · avg cost per run: $0.0XX"
[[nodiscard]] inline Element SummaryFooter(const AgentListData& data) {
    int total = static_cast<int>(data.agents.size());
    int running = 0;
    double total_cost = 0.0;
    int total_runs = 0;
    for (const auto& a : data.agents) {
        if (a.status == AgentStatus::Running) ++running;
        total_cost += a.stats.avg_cost_usd * a.stats.run_count;
        total_runs += a.stats.run_count;
    }
    double avg = (total_runs > 0) ? (total_cost / total_runs) : 0.0;

    return hbox({
        text(std::format(" {} agents", total)) | bold,
        text(" · ") | dim,
        text(std::format("{} running", running)) | color(Color::Green),
        text(" · ") | dim,
        text(std::format("avg cost per run: ${:.3f}", avg)) | color(Color::Yellow),
        filler(),
        text("[a]") | color(Color::Cyan), text(" new  ") | dim,
        text("[/]") | color(Color::Cyan), text(" search  ") | dim,
        text("[Tab]") | color(Color::Cyan), text(" view") | dim,
    });
}

// ============================================================
// Context Menu (hover popover on a card)
// ============================================================

/// A hover-style context menu rendered as a bordered popover.
/// Actions: Run / Duplicate / Export JSON / Delete
[[nodiscard]] inline Element HoverContextMenu(const AgentCardData& agent) {
    auto row = [](std::string_view key, std::string_view label, Color c) {
        return hbox({
            text(" [") | dim,
            text(std::string(key)) | color(c) | bold,
            text("] ") | dim,
            text(std::string(label)) | color(c),
        });
    };

    return vbox({
        text(" Quick actions for: " + agent.name) | bold | color(Color::Cyan),
        separator() | dim,
        row("r", "Run agent",      Color::Green),
        row("D", "Duplicate",      Color::Yellow),
        row("x", "Export JSON",    Color::Cyan),
        row("d", "Delete agent",   Color::Red),
    }) | border | color(Color::Cyan);
}

// ============================================================
// Top Toolbar: search + New button + Filter + Sort
// ============================================================

/// Options struct for the toolbar renderer.
struct ToolbarOptions {
    std::string search_query;
    FilterPreset filter = FilterPreset::All;
    SortKey sort = SortKey::Name;
    bool search_focused = false;

    std::function<void()> on_new_agent;
};

/// Convert a FilterPreset to its display label.
[[nodiscard]] inline std::string_view filter_label(FilterPreset f) {
    switch (f) {
        case FilterPreset::All:      return "All";
        case FilterPreset::Running:  return "Running";
        case FilterPreset::Idle:     return "Idle";
        case FilterPreset::Errored:  return "Errored";
        case FilterPreset::Disabled: return "Disabled";
    }
    return "?";
}

/// Convert a SortKey to its display label.
[[nodiscard]] inline std::string_view sort_label(SortKey s) {
    switch (s) {
        case SortKey::Name:    return "Name";
        case SortKey::LastRun: return "Last run";
        case SortKey::Cost:    return "Cost";
    }
    return "?";
}

/// Render the top toolbar. Search input placeholder/value + new-agent green
/// button + filter pill + sort pill.
[[nodiscard]] inline Element Toolbar(const ToolbarOptions& opts) {
    // Search box (display-only; input is handled by the container).
    std::string query_display = opts.search_query;
    if (query_display.empty()) query_display = "search name / tag / tools…";
    auto search_color = opts.search_focused ? Color::Cyan : Color::GrayDark;

    auto search_box = hbox({
        text(" 🔍 ") | color(search_color),
        text(opts.search_query.empty() ? query_display : opts.search_query)
            | (opts.search_query.empty() ? dim : color(Color::White)),
        filler() | nothing,
    }) | border | color(search_color) | flex;

    // Big green "+ New Agent" button.
    auto new_btn = hbox({
        text(" + "),
        text("New Agent") | bold,
        text(" "),
    }) | color(Color::White) | bgcolor(Color::RGB(40, 160, 70)) | borderRounded;

    // Filter + sort pills.
    auto filter_pill = hbox({
        text(" Filter: ") | dim,
        text(std::string(filter_label(opts.filter)))
            | color(Color::MagentaLight) | bold,
        text(" ▾") | dim,
    }) | borderLight;

    auto sort_pill = hbox({
        text(" Sort: ") | dim,
        text(std::string(sort_label(opts.sort)))
            | color(Color::YellowLight) | bold,
        text(" ▾") | dim,
    }) | borderLight;

    return hbox({
        search_box,
        text("  "),
        new_btn,
        text("  "),
        filter_pill,
        text("  "),
        sort_pill,
    });
}

// ============================================================
// Grid / List Layouts
// ============================================================

/// Render the Grid view: LargeCard 2 columns (adaptive column count
/// requires FTXUI terminal-width query at render time; using fixed 2 for now).
[[nodiscard]] inline Element GridView(
    const std::vector<AgentCardData>& agents,
    int selected_idx,
    std::shared_ptr<SharedAnimState> anim)
{
    if (agents.empty()) {
        return text("(no agents match the current filters)") | dim | center;
    }

    uint32_t f = anim ? anim->frame : 0;
    Elements rows;

    // Fixed 2-column layout.  Adaptive 1-2-3-4 columns require querying
    // the terminal width at render time (FTXUI does not expose width inside
    // a Renderer lambda); deferred until resize-callback infrastructure lands.
    for (size_t i = 0; i < agents.size(); i += 2) {
        Elements cells;
        for (size_t j = i; j < std::min(i + 2, agents.size()); ++j) {
            bool sel = (static_cast<int>(j) == selected_idx);
            cells.push_back(
                cards::LargeCard(agents[j], sel, f) | flex);
            if (j + 1 < std::min(i + 2, agents.size()))
                cells.push_back(text("  "));
        }
        rows.push_back(hbox(std::move(cells)));
        if (i + 2 < agents.size()) rows.push_back(text(" "));
    }

    return vbox(std::move(rows)) | vscroll_indicator | yframe | flex;
}

/// Render the List view: MiniCard vertical stack.
[[nodiscard]] inline Element ListView(
    const std::vector<AgentCardData>& agents,
    int selected_idx,
    std::shared_ptr<SharedAnimState> anim)
{
    if (agents.empty()) {
        return text("(no agents match the current filters)") | dim | center;
    }

    uint32_t f = anim ? anim->frame : 0;
    Elements rows;
    for (size_t i = 0; i < agents.size(); ++i) {
        bool sel = (static_cast<int>(i) == selected_idx);
        rows.push_back(cards::MiniCard(agents[i], sel, f));
        if (i + 1 < agents.size()) rows.push_back(separator() | dim);
    }
    return vbox(std::move(rows)) | vscroll_indicator | yframe | flex;
}

// ============================================================
// Full interactive AgentList component
// ============================================================

/// Props for AgentListComponent.
struct AgentListOptions {
    std::vector<AgentCardData> initial_agents;
    RunStats initial_totals{};

    // Callbacks
    std::function<void(const std::string& id)> on_run;
    std::function<void(const std::string& id)> on_edit;
    std::function<void(const std::string& id)> on_delete;
    std::function<void(const std::string& id)> on_duplicate;
    std::function<void(const std::string& id)> on_open_detail;
    std::function<void()> on_new_agent;
    std::function<void(const AgentCardData&)> on_export_json;
};

/// Build the top-level agent list interactive component.
/// Keyboard:
///   j / ↓       next
///   k / ↑       prev
///   Enter       open detail
///   r           run selected
///   e           edit selected
///   d           delete selected  (with confirmation via callback)
///   D           duplicate
///   a           create new agent
///   /           focus search
///   Esc         defocus search (if focused)
///   Tab         toggle Grid/List view
///   f           cycle filter preset
///   s           cycle sort
[[nodiscard]] inline Component AgentListComponent(AgentListOptions opts) {
    struct State {
        AgentListData data;
        ViewMode view = ViewMode::Grid;
        int selected = 0;
        bool search_focused = false;
        bool show_hover_menu = false;
        std::shared_ptr<SharedAnimState> anim;
        AgentListOptions cb;
    };

    auto s = std::make_shared<State>();
    s->data.agents = std::move(opts.initial_agents);
    s->data.totals = opts.initial_totals;
    s->anim = std::make_shared<SharedAnimState>();
    s->cb = std::move(opts);

    auto renderer = Renderer([s] {
        s->anim->tick();

        // Compute visible set.
        auto visible = apply_view_options(s->data);
        int safe_sel = std::clamp(
            s->selected, 0, std::max(0, static_cast<int>(visible.size()) - 1));
        s->selected = safe_sel;

        // Grid/List tab bar.
        std::vector<Tab> tabs = {
            {.label = "Grid", .id = "grid", .is_all_tab = true},
            {.label = "List", .id = "list", .is_all_tab = true},
        };
        TagTabsOptions tto;
        tto.tabs = tabs;
        tto.active_tab = (s->view == ViewMode::Grid) ? 0 : 1;
        tto.show_resume_label = false;
        auto tabs_el = TagTabsComponent(tto);

        // Toolbar.
        ToolbarOptions tb;
        tb.search_query = s->data.filter_query;
        tb.filter = s->data.filter;
        tb.sort = s->data.sort;
        tb.search_focused = s->search_focused;
        tb.on_new_agent = s->cb.on_new_agent;

        // Body.
        Element body;
        if (s->view == ViewMode::Grid) {
            body = GridView(visible, safe_sel, s->anim);
        } else {
            body = ListView(visible, safe_sel, s->anim);
        }

        // Optional hover menu next to body (if shown).
        Element main_body = body;
        if (s->show_hover_menu && !visible.empty()) {
            int idx = std::clamp(safe_sel, 0, static_cast<int>(visible.size()) - 1);
            main_body = hbox({
                body | flex,
                text(" "),
                HoverContextMenu(visible[idx])
                    | size(WIDTH, LESS_THAN, 30) | align_right,
            });
        }

        return vbox({
            Toolbar(tb),
            text(" "),
            tabs_el->Render(),
            text(" "),
            main_body,
            separator(),
            SummaryFooter(s->data),
        }) | flex;
    });

    return renderer | CatchEvent([s](Event event) -> bool {
        auto visible = apply_view_options(s->data);
        int count = static_cast<int>(visible.size());
        int safe_sel = std::clamp(s->selected, 0, std::max(0, count - 1));

        // --- Search mode capture ---
        if (s->search_focused) {
            if (event == Event::Escape) {
                s->search_focused = false;
                s->data.filter_query.clear();
                return true;
            }
            if (event == Event::Return || event == Event::Tab) {
                s->search_focused = false;
                return true;
            }
            if (event == Event::Backspace) {
                if (!s->data.filter_query.empty()) {
                    s->data.filter_query.pop_back();
                    s->selected = 0;
                }
                return true;
            }
            if (event.is_character()) {
                s->data.filter_query.push_back(event.character()[0]);
                s->selected = 0;
                return true;
            }
            return false;
        }

        // --- Global keys ---
        if (event == Event::Character('/')) {
            s->search_focused = true;
            return true;
        }
        if (event == Event::Character('a')) {
            if (s->cb.on_new_agent) s->cb.on_new_agent();
            return true;
        }
        if (event == Event::Tab) {
            s->view = (s->view == ViewMode::Grid) ? ViewMode::List : ViewMode::Grid;
            return true;
        }
        if (event == Event::Character('f')) {
            // Cycle filter preset.
            using U = std::underlying_type_t<FilterPreset>;
            auto cur = static_cast<U>(s->data.filter);
            auto next = static_cast<FilterPreset>((cur + 1) % 5);
            s->data.filter = next;
            s->selected = 0;
            return true;
        }
        if (event == Event::Character('s')) {
            // Cycle sort.
            using U = std::underlying_type_t<SortKey>;
            auto cur = static_cast<U>(s->data.sort);
            auto next = static_cast<SortKey>((cur + 1) % 3);
            s->data.sort = next;
            return true;
        }
        if (event == Event::Character('m')) {
            // Toggle hover menu.
            s->show_hover_menu = !s->show_hover_menu;
            return true;
        }

        // --- Navigation ---
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            if (count > 0) s->selected = (safe_sel + 1) % count;
            return true;
        }
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            if (count > 0)
                s->selected = (safe_sel - 1 + count) % count;
            return true;
        }

        if (count == 0) return false;
        const auto& agent = visible[safe_sel];

        // --- Actions on selected ---
        if (event == Event::Return) {
            if (s->cb.on_open_detail) s->cb.on_open_detail(agent.id);
            return true;
        }
        if (event == Event::Character('r')) {
            if (s->cb.on_run) s->cb.on_run(agent.id);
            return true;
        }
        if (event == Event::Character('e')) {
            if (s->cb.on_edit) s->cb.on_edit(agent.id);
            return true;
        }
        if (event == Event::Character('d')) {
            if (s->cb.on_delete) s->cb.on_delete(agent.id);
            return true;
        }
        if (event == Event::Character('D')) {
            if (s->cb.on_duplicate) s->cb.on_duplicate(agent.id);
            return true;
        }
        if (event == Event::Character('x')) {
            if (s->cb.on_export_json) s->cb.on_export_json(agent);
            return true;
        }

        return false;
    });
}

} // namespace cc::ui::agents::list_view
