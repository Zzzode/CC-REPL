/// @file plugin_manage_panel.cppm
/// @brief Installed-plugins management panel: toolbar (search + filter + sort
///        + Install button) + scrollable installed list + detail side-panel.
///
/// Data is consumed exclusively from plugin_ui_data::InstalledCellData
/// (Phase 2 C1).  No direct plugin engine reads.
module;

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

export module cc.ui.plugins.plugin_manage_panel;

import cc.types.types;
import cc.commands.plugin_ui_data;
import cc.commands.plugin_pagination_util;
import cc.ui.custom_select;
import cc.ui.dialogs.plugin_dialog;

export namespace cc::ui::plugins::plugin_manage_panel {
using namespace ftxui;

namespace ui = cc::commands::plugin_ui;
namespace pp = cc::commands::plugin;
namespace cs = cc::ui::custom_select;

using ui::InstalledCellData;
using ui::TargetAction;

// =========================================================================
// Filter / sort enums
// =========================================================================

enum class StatusFilter : unsigned char {
    All,
    Enabled,
    Disabled,
    ByAuthor,
};

enum class SortKey : unsigned char {
    Name,
    InstalledTime,
    Scope,
    Author,
};

// =========================================================================
// Inputs (all come from C1 data prep)
// =========================================================================

struct ManagePanelInputs {
    std::vector<InstalledCellData> installed;

    // Routed-action hints
    std::optional<std::string> target_plugin;
    TargetAction               preselect_action = TargetAction::None;

    // Callbacks (consumer-provided; never invoked from render code)
    std::function<void(std::string_view id, bool enable)> on_toggle;
    std::function<void(std::string_view id)>               on_uninstall;
    std::function<void(std::string_view id)>               on_update;
    std::function<void(std::string_view id)>               on_configure;
    std::function<void()>                                  on_install_new;   // → install flow
    std::function<void(const ui::ViewState& next)>         on_navigate;
};

// =========================================================================
// Render: toolbar
// =========================================================================

namespace detail {

[[nodiscard]] inline Element RenderToolbar(
    std::string_view search_text,
    StatusFilter filter,
    SortKey sort,
    std::size_t visible_count,
    std::size_t total_count)
{
    static constexpr const char* filter_labels[] = {
        "All", "Enabled", "Disabled", "By Author"
    };
    static constexpr const char* sort_labels[] = {
        "Name", "Installed", "Scope", "Author"
    };
    auto filter_label = filter_labels[(unsigned)filter];
    auto sort_label   = sort_labels[(unsigned)sort];

    return hbox({
        text(" 🔍 ") | dim,
        text(std::string{search_text.empty() ? "Search installed plugins… (press /)"
                                             : search_text})
             | color(search_text.empty() ? Color::GrayDark : Color::White),
        text("│") | blink | (search_text.empty() ? dim : color(Color::Cyan)),
        filler(),
        text("Filter:") | dim,
        text(" " + std::string{filter_label} + " ") | color(Color::Cyan) | borderLight | dim,
        text("  Sort:") | dim,
        text(" " + std::string{sort_label} + " ") | color(Color::Magenta) | borderLight | dim,
        text("  "),
        text(" [+] Install ") | bold | color(Color::Green) | bgcolor(Color::RGB(20, 40, 20)),
        text(" "),
        text(std::format("{}/{}", visible_count, total_count)) | dim,
    }) | padding(0, 1, 0, 0);
}

// Render a single installed plugin row.
[[nodiscard]] inline Element RenderInstalledRow(
    const InstalledCellData& p, bool selected, bool is_target)
{
    auto name_color = p.enabled ? Color::White : Color::GrayDark;
    auto scope_color = [](std::string_view s) -> Color {
        if (s == "user")    return Color::Cyan;
        if (s == "project") return Color::Green;
        if (s == "local")   return Color::Yellow;
        if (s == "managed") return Color::Magenta;
        return Color::GrayDark;
    }(p.scope);

    Elements left;
    left.push_back(text(selected ? "› " : (is_target ? "★ " : "  "))
                   | color(is_target ? Color::Yellow : Color::Cyan));
    // Status dot
    left.push_back(text(p.enabled ? "● " : "○ ")
                   | color(p.has_error ? Color::Red : (p.enabled ? Color::Green : Color::GrayDark)));
    // Name + marketplace
    left.push_back(text(p.name) | bold | color(selected ? Color::White : name_color));
    left.push_back(text(" @") | dim);
    left.push_back(text(p.marketplace) | dim | color(Color::Cyan) | dim);
    if (p.version) {
        left.push_back(text("  v" + *p.version) | dim);
    }
    if (p.has_pending_update) {
        left.push_back(text(" ⬆") | color(Color::Yellow) | bold);
    }
    if (p.has_error) {
        left.push_back(text(" ✗") | color(Color::Red));
    }

    Elements right;
    right.push_back(text(" ") | filler());
    // Tool/command counts
    if (p.tool_count || p.command_count) {
        right.push_back(text(std::format("🔧{} ⌘{}", p.tool_count, p.command_count)) | dim);
        right.push_back(text(" "));
    }
    // Scope chip
    right.push_back(text(" " + p.scope + " ") | color(scope_color) | dim | borderLight);

    auto line = hbox({
        hbox(std::move(left)),
        text(" ") | filler(),
        hbox(std::move(right)),
    });

    if (selected) {
        line = line | bgcolor(Color::RGB(25, 35, 55));
    }
    return line;
}

// Render the detail side-panel for the focused plugin.
[[nodiscard]] inline Element RenderDetailPane(const InstalledCellData& p) {
    Elements rows;
    rows.push_back(hbox({
        text(" " + p.name + " ") | bold | color(Color::Cyan),
        filler(),
        p.version ? text("v" + *p.version) | dim : text(""),
    }));
    rows.push_back(separator());

    if (p.description) {
        rows.push_back(paragraph(" " + *p.description) | dim);
        rows.push_back(text(""));
    }

    rows.push_back(hbox({
        text("  Marketplace: ") | dim,
        text(p.marketplace) | color(Color::Blue),
    }));
    rows.push_back(hbox({
        text("  Scope:       ") | dim,
        text(p.scope) | color(Color::Magenta),
    }));
    rows.push_back(hbox({
        text("  Status:      ") | dim,
        text(p.enabled ? "● ENABLED" : "○ DISABLED")
             | color(p.enabled ? Color::Green : Color::GrayDark),
    }));
    if (p.last_updated) {
        rows.push_back(hbox({
            text("  Updated:     ") | dim,
            text(*p.last_updated),
        }));
    }
    rows.push_back(text(""));

    rows.push_back(hbox({
        text("  Tools: ") | dim,
        text(std::format("{}", p.tool_count)) | color(Color::Yellow),
        text("   Commands: ") | dim,
        text(std::format("{}", p.command_count)) | color(Color::Yellow),
        p.has_config_options
            ? text("   • Has options") | color(Color::Cyan) | dim
            : text(""),
    }));

    if (p.has_pending_update) {
        rows.push_back(text(""));
        rows.push_back(hbox({
            text("  ⬆ ") | color(Color::Yellow) | bold,
            text("Update available") | color(Color::Yellow),
        }));
    }
    if (p.has_error) {
        rows.push_back(text(""));
        rows.push_back(hbox({
            text("  ✗ ") | color(Color::Red) | bold,
            text("Plugin has load errors — check the Errors tab") | color(Color::Red) | dim,
        }));
    }

    // Action shortcut bar
    rows.push_back(text(""));
    rows.push_back(text(" Actions:") | dim);
    rows.push_back(hbox({
        text(" [") | dim,
        text("Space") | bold | color(Color::Cyan),
        text("]") | dim,
        text(p.enabled ? " Disable " : " Enable "),
        text(" [") | dim,
        text("c") | bold | color(Color::Cyan),
        text("]") | dim,
        text(" Config  "),
        text("[") | dim,
        text("u") | bold | color(Color::Cyan),
        text("]") | dim,
        text(" Update  "),
        text("[") | dim,
        text("x") | bold | color(Color::Cyan),
        text("]") | dim,
        text(" Uninstall"),
    }) | dim);

    return vbox(std::move(rows)) | borderLight | color(Color::Cyan);
}

} // namespace detail

// =========================================================================
// Factory — full interactive manage panel
// =========================================================================

struct ManageState {
    ManagePanelInputs inputs;

    // ── List state ──────────────────────────────────────────────────────
    std::string              search_text;
    StatusFilter             filter = StatusFilter::All;
    SortKey                  sort   = SortKey::Name;
    std::vector<std::size_t> filtered_indices;   // → into inputs.installed
    pp::Paginator            paginator{0, 8};

    // ── Selection ───────────────────────────────────────────────────────
    int  selected_page_index = 0;    // index within filtered page (0..7)
    bool searching = false;
};

/// Apply filter + sort to the installed list.
inline void RecomputeFiltered(ManageState& s) {
    const auto& all = s.inputs.installed;
    s.filtered_indices.clear();
    s.filtered_indices.reserve(all.size());

    auto q = s.search_text;
    std::ranges::transform(q, q.begin(), ::tolower);

    for (std::size_t i = 0; i < all.size(); ++i) {
        const auto& p = all[i];

        // Status filter
        switch (s.filter) {
            case StatusFilter::Enabled:  if (!p.enabled) continue; break;
            case StatusFilter::Disabled: if (p.enabled)  continue; break;
            case StatusFilter::ByAuthor: /* all authors, show grouped */ break;
            case StatusFilter::All:      break;
        }

        // Text filter (case-insensitive contains on name/marketplace/desc)
        if (!q.empty()) {
            std::string name_l = p.name;
            std::ranges::transform(name_l, name_l.begin(), ::tolower);
            if (name_l.find(q) == std::string::npos) {
                std::string mp_l = p.marketplace;
                std::ranges::transform(mp_l, mp_l.begin(), ::tolower);
                if (mp_l.find(q) == std::string::npos) {
                    std::string desc_l = p.description.value_or("");
                    std::ranges::transform(desc_l, desc_l.begin(), ::tolower);
                    if (desc_l.find(q) == std::string::npos) continue;
                }
            }
        }
        s.filtered_indices.push_back(i);
    }

    // Sort
    switch (s.sort) {
        case SortKey::Name:
            std::ranges::sort(s.filtered_indices, [&](std::size_t a, std::size_t b) {
                return all[a].name < all[b].name;
            });
            break;
        case SortKey::InstalledTime:
            std::ranges::sort(s.filtered_indices, [&](std::size_t a, std::size_t b) {
                return all[a].last_updated.value_or("") > all[b].last_updated.value_or("");
            });
            break;
        case SortKey::Scope:
            std::ranges::sort(s.filtered_indices, [&](std::size_t a, std::size_t b) {
                if (all[a].scope != all[b].scope) return all[a].scope < all[b].scope;
                return all[a].name < all[b].name;
            });
            break;
        case SortKey::Author:
            // Author not in InstalledCellData — fallback to scope then name
            std::ranges::sort(s.filtered_indices, [&](std::size_t a, std::size_t b) {
                return all[a].name < all[b].name;
            });
            break;
    }

    s.paginator = pp::Paginator{s.filtered_indices.size(), 8};
    if (s.selected_page_index >= (int)s.paginator.max_visible())
        s.selected_page_index = (int)s.paginator.max_visible() - 1;

    // Auto-focus target_plugin if routed with one
    if (s.inputs.target_plugin) {
        for (std::size_t i = 0; i < s.filtered_indices.size(); ++i) {
            if (all[s.filtered_indices[i]].name == *s.inputs.target_plugin ||
                (all[s.filtered_indices[i]].name + "@" +
                 all[s.filtered_indices[i]].marketplace) == *s.inputs.target_plugin)
            {
                s.paginator.set_selected_index(i);
                const auto page_start = s.paginator.start_index();
                s.selected_page_index = (int)(i - page_start);
                break;
            }
        }
    }
}

[[nodiscard]] inline Component MakeManagePanel(ManagePanelInputs inputs) {
    auto state = std::make_shared<ManageState>();
    state->inputs = std::move(inputs);
    RecomputeFiltered(*state);

    // Sub-component: search input (toggled via '/')
    Component search_input = Input(&state->search_text,
                                   "Search installed plugins…");
    auto search_with_filter = search_input | CatchEvent([state](Event) {
        RecomputeFiltered(*state);
        return false;
    });

    return Renderer([state] {
        const auto snap = state->paginator.snapshot();
        auto toolbar = detail::RenderToolbar(
            state->search_text, state->filter, state->sort,
            snap.end_index - snap.start_index, state->filtered_indices.size());

        // Build list
        Elements list_rows;
        if (state->filtered_indices.empty()) {
            list_rows.push_back(text("  No plugins match the current filters.")
                                | color(Color::Yellow) | dim | center);
        } else {
            for (std::size_t i = snap.start_index; i < snap.end_index; ++i) {
                const int page_i = (int)(i - snap.start_index);
                const bool selected = (page_i == state->selected_page_index);
                const bool is_target = state->inputs.target_plugin.has_value() &&
                    (state->inputs.installed[state->filtered_indices[i]].name ==
                     *state->inputs.target_plugin);
                list_rows.push_back(detail::RenderInstalledRow(
                    state->inputs.installed[state->filtered_indices[i]],
                    selected, is_target));
                if (page_i + 1 < (int)(snap.end_index - snap.start_index))
                    list_rows.push_back(separator() | dim);
            }
        }

        // Pagination footer
        Elements page_bits{
            text(std::format(" {}-{} / {}  ",
                             snap.scroll_position.current == 0 ? 0 : (int)snap.start_index + 1,
                             (int)snap.end_index, (int)snap.scroll_position.total))
                | dim,
        };
        if (snap.scroll_position.can_scroll_up)
            page_bits.push_back(text("PgUp ") | color(Color::Cyan) | dim);
        page_bits.push_back(text("|") | dim);
        if (snap.scroll_position.can_scroll_down)
            page_bits.push_back(text(" PgDn") | color(Color::Cyan) | dim);

        auto list_panel = vbox({
            toolbar,
            separator(),
            state->searching
                ? hbox({search_with_filter->Render()})
                : text(""),
            state->searching ? separator() : text(""),
            vbox(std::move(list_rows)) | flex | yframe | vscroll_indicator,
            separator(),
            hbox(std::move(page_bits)),
        }) | border | size(WIDTH, EQUAL, 58);

        // Detail panel
        Element detail;
        if (!state->filtered_indices.empty()) {
            const auto page_idx = (std::size_t)std::max(0, state->selected_page_index);
            const auto actual_i = state->paginator.start_index() + page_idx;
            if (actual_i < state->filtered_indices.size()) {
                detail = detail::RenderDetailPane(
                    state->inputs.installed[state->filtered_indices[actual_i]]);
            }
        }
        if (!detail) {
            detail = text(" No plugin selected\n\n\n Use ↑/↓ or j/k to browse.\n"
                          " Press / to search, F to filter, S to sort.")
                | dim | center | borderLight;
        }

        return hbox({
            std::move(list_panel),
            text(" "),
            std::move(detail) | flex,
        });
    }) | CatchEvent([state](Event event) -> bool {
        // Search mode
        if (state->searching) {
            if (event == Event::Escape) {
                state->searching = false;
                state->search_text.clear();
                RecomputeFiltered(*state);
                return true;
            }
            if (event == Event::Return) {
                state->searching = false;
                return true;
            }
            // Forward to search input
            return search_input->OnEvent(event);
        }

        if (event == Event::Character('/')) {
            state->searching = true;
            search_input->TakeFocus();
            return true;
        }

        // Filter cycle: F → All → Enabled → Disabled → ByAuthor
        if (event == Event::Character('f') || event == Event::Character('F')) {
            state->filter = (StatusFilter)(((unsigned)state->filter + 1) % 4);
            RecomputeFiltered(*state);
            return true;
        }

        // Sort cycle: S → Name → Installed → Scope → Author
        if (event == Event::Character('s') || event == Event::Character('S')) {
            state->sort = (SortKey)(((unsigned)state->sort + 1) % 4);
            RecomputeFiltered(*state);
            return true;
        }

        // Install new → call on_install_new
        if (event == Event::Character('+') || event == Event::Character('i') ||
            event == Event::Character('I'))
        {
            if (state->inputs.on_install_new) state->inputs.on_install_new();
            return true;
        }

        // Nav: up/down within the filtered page
        const auto page_size = (int)state->paginator.max_visible();
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            if (state->selected_page_index > 0) {
                --state->selected_page_index;
            } else if (state->paginator.snapshot().scroll_position.can_scroll_up) {
                state->paginator.prev_page();
                state->selected_page_index = page_size - 1;
            }
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            if (state->selected_page_index + 1 <
                (int)(state->paginator.end_index() - state->paginator.start_index()))
            {
                ++state->selected_page_index;
            } else if (state->paginator.snapshot().scroll_position.can_scroll_down) {
                state->paginator.next_page();
                state->selected_page_index = 0;
            }
            return true;
        }
        if (event == Event::PageUp)   { state->paginator.prev_page(); state->selected_page_index = 0; return true; }
        if (event == Event::PageDown) { state->paginator.next_page(); state->selected_page_index = 0; return true; }

        if (state->filtered_indices.empty()) return false;

        const auto page_idx = (std::size_t)std::max(0, state->selected_page_index);
        const auto actual_i = state->paginator.start_index() + page_idx;
        if (actual_i >= state->filtered_indices.size()) return false;
        const auto& p = state->inputs.installed[state->filtered_indices[actual_i]];
        std::string_view id = p.name + "@" + p.marketplace;

        // Space: toggle enable
        if (event == Event::Character(' ')) {
            if (state->inputs.on_toggle) state->inputs.on_toggle(id, !p.enabled);
            // Mutate local state so re-render reflects the change immediately
            // (consumer will re-supply updated data on next frame)
            const_cast<InstalledCellData&>(p).enabled = !p.enabled;
            return true;
        }

        // c: configure
        if (event == Event::Character('c') || event == Event::Character('C')) {
            if (state->inputs.on_configure) state->inputs.on_configure(id);
            return true;
        }

        // u: update
        if (event == Event::Character('u') || event == Event::Character('U')) {
            if (state->inputs.on_update) state->inputs.on_update(id);
            return true;
        }

        // x: uninstall
        if (event == Event::Character('x') || event == Event::Character('X')) {
            if (state->inputs.on_uninstall) state->inputs.on_uninstall(id);
            return true;
        }

        return false;
    });
}

} // namespace cc::ui::plugins::plugin_manage_panel
