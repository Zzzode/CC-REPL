/// @file plugin_marketplace_browse.cppm
/// @brief Marketplace browser: two sub-tabs (Browse / Discover), per-market
///        dropdown, simple vertical plugin list with radio-select indicator,
///        name + marketplace, install count, and truncated description.
///
/// Data comes from plugin_ui_data C1:
///   - MarketplaceInfo    (marketplace list with counts + warnings)
///   - InstallablePlugin  (individual plugin entries with metadata)
///   - PaginationUtil     (windowing for both browse and discover lists)
module;

#include <algorithm>
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

export module cc.ui.plugins.plugin_marketplace_browse;

import cc.types.types;
import cc.commands.plugin_ui_data;
import cc.commands.plugin_helpers;
import cc.commands.plugin_pagination_util;
import cc.ui.custom_select;

export namespace cc::ui::plugins::plugin_marketplace_browse {
using namespace ftxui;

namespace ui = cc::commands::plugin_ui;
namespace pf = cc::commands::plugin_helpers;
namespace pp = cc::commands::plugin;
namespace cs = cc::ui::custom_select;

using ui::MarketplaceInfo;
using ui::ViewKind;
using ui::BrowseView;
using pf::InstallablePlugin;

// =========================================================================
// Types
// =========================================================================

enum class SubTab : unsigned char {
    Browse,     // per-marketplace list
    Discover,   // global popularity-sorted list
};

/// Extended installable-plugin data for rendering (pure augmentation).
struct PluginCardData {
    InstallablePlugin base;

    // Display-only augmentations (deterministic, from C1 data)
    std::string          name;
    std::string          author;
    std::string          description;
    std::string          version;
    std::string          size_kb_str;           // e.g. "42KB"
    std::vector<std::string> tags;
    float                rating = 0.0f;
    int                  rating_count = 0;
    int                  install_count = 0;
    bool                 has_homepage = false;
    bool                 has_github = false;
};

/// Inputs — all from C1 data prep.
struct BrowsePanelInputs {
    SubTab                      initial_subtab = SubTab::Browse;
    std::vector<MarketplaceInfo> marketplaces;
    int                          default_marketplace = 0;
    std::vector<PluginCardData>  discover_cards;   // global, pre-sorted
    bool                         discover_empty = false;
    std::optional<std::string>   discover_empty_reason;
    // Per-marketplace cards (index matches marketplaces[] index)
    std::vector<std::vector<PluginCardData>> marketplace_cards;

    // Routed hints
    std::optional<std::string> target_marketplace;
    std::optional<std::string> target_plugin;

    // Callbacks
    std::function<void(std::string_view plugin_id)> on_install;
    std::function<void()>                           on_close;
};

// =========================================================================
// Rendering helpers
// =========================================================================

namespace detail {

/// Render a single plugin row (simple vertical list, matching TS
/// DiscoverPlugins.tsx).  Each row shows a radio-select indicator, the
/// plugin name + marketplace, install count, and a truncated description.
[[nodiscard]] inline Element RenderPluginRow(
    const PluginCardData& card,
    bool selected,
    bool is_installing)
{
    // Radio indicator: ◉ selected for install, ○ not selected, … installing.
    std::string radio_utf8;
    if (is_installing) {
        radio_utf8 = "…";  // …
    } else if (card.base.is_installed) {
        radio_utf8 = "◉";  // ◉
    } else {
        radio_utf8 = "○";  // ○
    }

    // Selection pointer (▶ when selected, space otherwise).
    std::string pointer = selected ? "❯ " : "  ";  // ❯ or spaces

    // Name + marketplace + install count (same line).
    Elements name_line{
        text(pointer) | color(selected ? Color::Cyan : Color::GrayDark),
        text(radio_utf8 + " ") | color(Color::GrayLight),
        text(card.name) | bold | color(selected ? Color::Cyan : Color::White),
    };
    if (!card.author.empty()) {
        name_line.push_back(text(" · ") | dim | color(Color::GrayLight));  // ·
        name_line.push_back(text(card.author) | dim | color(Color::GrayLight));
    }
    if (card.install_count > 0) {
        name_line.push_back(text(" · ") | dim | color(Color::GrayLight));
        name_line.push_back(text(std::to_string(card.install_count) + " installs")
                           | dim | color(Color::GrayLight));
    }

    // Description on next line (indented 4, dim, truncated to ~60 chars).
    Element desc_el;
    std::string desc = card.description;
    if (desc.empty()) {
        desc_el = text("");
    } else {
        const std::size_t max_len = 60;
        if (desc.size() > max_len) {
            desc = desc.substr(0, max_len - 1) + "…";  // …
        }
        desc_el = text("    " + desc) | dim | color(Color::GrayLight);
    }

    auto row = vbox({
        hbox(std::move(name_line)),
        desc_el,
    });

    if (selected) {
        row = row | bgcolor(Color::RGB(20, 30, 50));
    }
    return row;
}

/// Sub-tab bar (Browse / Discover).
[[nodiscard]] inline Element RenderSubTabBar(SubTab active) {
    auto browse = text(" 🔍 Browse ")
        | (active == SubTab::Browse
               ? (bold | color(Color::Cyan) | bgcolor(Color::RGB(20, 30, 50)))
               : dim);
    auto discover = text(" ✨ Discover ")
        | (active == SubTab::Discover
               ? (bold | color(Color::Yellow) | bgcolor(Color::RGB(40, 30, 15)))
               : dim);
    return hbox({
        std::move(browse),
        text("│") | dim,
        std::move(discover),
        filler(),
    });
}

/// Marketplace selector row.
[[nodiscard]] inline Element RenderMarketSelector(
    const std::vector<MarketplaceInfo>& markets, int selected)
{
    Elements bits;
    if (markets.empty()) {
        bits.push_back(text(" No marketplaces configured — add one in Settings")
                       | color(Color::Yellow) | dim);
    } else {
        const int safe = std::clamp(selected, 0, (int)markets.size() - 1);
        const auto& m = markets[safe];
        bits.push_back(text(" 🛒  ") | dim);
        bits.push_back(text(m.name) | bold | color(Color::Magenta));
        bits.push_back(text(std::format(" ({} plugins, {} installed)",
                                       m.total_plugins, m.installed_count))
                       | dim);
        if (m.source_display) {
            bits.push_back(text("  ·  ") | dim);
            bits.push_back(text(*m.source_display) | color(Color::BlueLight) | dim);
        }
        if (m.warning) {
            bits.push_back(text(""));
            bits.push_back(hbox({
                text("  ⚠ "),
                text(*m.warning) | color(Color::Red) | dim,
            }));
        }
    }

    // ←/→ arrows for multi-marketplace browsing
    if (markets.size() > 1) {
        bits.push_back(filler());
        bits.push_back(text(std::format("[← {}/{} →]", selected + 1, markets.size()))
                       | dim | color(Color::Cyan));
    }
    return hbox(std::move(bits));
}

} // namespace detail

// =========================================================================
// Interactive component
// =========================================================================

struct BrowseState {
    BrowsePanelInputs inputs;
    SubTab            active_subtab;
    BrowseView        inner_view = BrowseView::PluginList;

    // Browse state
    int               mp_selected = 0;
    // Discover state
    pp::Paginator     discover_paginator{0, 8};   // 8 items per page
    pp::Paginator     browse_paginator{0, 8};

    // Selected item index within the current page
    int               selected_index = 0;
};

/// Build a simple vertical plugin list for the current page (matching TS
/// DiscoverPlugins.tsx layout: radio indicator, name + marketplace,
/// install count, truncated description).
[[nodiscard]] inline Element RenderPluginList(
    BrowseState& s)
{
    const bool is_discover = (s.active_subtab == SubTab::Discover);
    const auto& cards = is_discover
        ? s.inputs.discover_cards
        : (s.mp_selected < (int)s.inputs.marketplace_cards.size()
              ? s.inputs.marketplace_cards[s.mp_selected]
              : s.inputs.marketplace_cards.front());
    auto& paginator = is_discover ? s.discover_paginator : s.browse_paginator;
    paginator = pp::Paginator{cards.size(), 8};

    // Empty-state handling (Discover only)
    if (is_discover && s.inputs.discover_empty) {
        return vbox({
            text("") | size(HEIGHT, EQUAL, 3),
            hbox({filler(),
                text(" 🔍 ") | color(Color::Yellow) | dim,
                text(s.inputs.discover_empty_reason.value_or(
                         "No plugins to show right now.")) | dim,
                filler()}),
        }) | flex;
    }
    if (cards.empty()) {
        return vbox({
            text("") | size(HEIGHT, EQUAL, 3),
            hbox({filler(),
                text(" (no plugins in this marketplace) ") | dim,
                filler()}),
        }) | flex;
    }

    const auto snap = paginator.snapshot();

    // Scroll up indicator
    Elements list_rows;
    if (snap.scroll_position.can_scroll_up) {
        list_rows.push_back(text(" ↑ more above") | dim | color(Color::GrayLight));
    }

    // Plugin rows
    for (std::size_t r = snap.start_index; r < snap.end_index; ++r) {
        int actual_index = (int)(r - snap.start_index);
        const bool sel = (actual_index == s.selected_index);
        Element row = detail::RenderPluginRow(cards[r], sel, false);
        list_rows.push_back(std::move(row));
    }

    // Scroll down indicator
    if (snap.scroll_position.can_scroll_down) {
        list_rows.push_back(text(" ↓ more below") | dim | color(Color::GrayLight));
    }

    // Footer: pagination + keyboard hints
    auto footer = hbox({
        text(std::format(" {}/{} plugin{}  ",
                         (int)cards.size(),
                         (int)cards.size(),
                         cards.size() == 1 ? "" : "s")) | dim,
        filler(),
        text(" ↑↓/hjkl select  ") | dim,
        text("⏎") | bold | color(Color::Cyan) | dim,
        text(" install  ") | dim,
        text("Tab") | bold | color(Color::Cyan) | dim,
        text(" subtab  ") | dim,
        text("i") | bold | color(Color::Cyan) | dim,
        text("nstall") | dim,
    });

    return vbox({
        vbox(std::move(list_rows)) | flex | vscroll_indicator,
        separator() | dim,
        footer,
    });
}

[[nodiscard]] inline Component MakeBrowsePanel(BrowsePanelInputs inputs) {
    auto state = std::make_shared<BrowseState>();
    state->inputs = std::move(inputs);
    state->active_subtab = state->inputs.initial_subtab;

    // Resolve default marketplace from target_marketplace hint
    if (state->inputs.target_marketplace) {
        for (int i = 0; i < (int)state->inputs.marketplaces.size(); ++i) {
            if (state->inputs.marketplaces[i].name == *state->inputs.target_marketplace) {
                state->mp_selected = i;
                break;
            }
        }
    }

    return Renderer([state]() -> Element {
        auto sub_tab_bar = detail::RenderSubTabBar(state->active_subtab);
        Element market_header;
        if (state->active_subtab == SubTab::Browse) {
            market_header = detail::RenderMarketSelector(
                state->inputs.marketplaces, state->mp_selected);
        } else {
            std::size_t total = state->inputs.discover_cards.size();
            market_header = hbox({
                text(" ✨  All Marketplaces") | bold | color(Color::Yellow),
                filler(),
                text(std::format("{} trending plugin{}", total, total == 1 ? "" : "s")) | dim,
            });
        }

        auto list = RenderPluginList(*state);

        return vbox({
            std::move(sub_tab_bar),
            separator() | dim,
            std::move(market_header),
            separator() | dim,
            std::move(list) | flex,
        });
    }) | CatchEvent([state](Event event) -> bool {
        // Sub-tab switch (Tab)
        if (event == Event::Tab) {
            state->active_subtab = (state->active_subtab == SubTab::Browse)
                ? SubTab::Discover : SubTab::Browse;
            state->selected_index = 0;
            return true;
        }

        // Marketplace left/right (browse subtab only)
        if (state->active_subtab == SubTab::Browse &&
            state->inputs.marketplaces.size() > 1)
        {
            if (event == Event::Character('[') || event == Event::Character('{')) {
                state->mp_selected = (state->mp_selected +
                    (int)state->inputs.marketplaces.size() - 1)
                    % (int)state->inputs.marketplaces.size();
                state->selected_index = 0;
                return true;
            }
            if (event == Event::Character(']') || event == Event::Character('}')) {
                state->mp_selected = (state->mp_selected + 1)
                    % (int)state->inputs.marketplaces.size();
                state->selected_index = 0;
                return true;
            }
        }

        const bool is_discover = (state->active_subtab == SubTab::Discover);
        const auto& cards = is_discover
            ? state->inputs.discover_cards
            : (state->mp_selected < (int)state->inputs.marketplace_cards.size()
                  ? state->inputs.marketplace_cards[state->mp_selected]
                  : state->inputs.marketplace_cards.front());
        const std::size_t total_cards = cards.size();
        if (total_cards == 0) return false;

        auto& paginator = is_discover
            ? state->discover_paginator : state->browse_paginator;
        const std::size_t page_start = paginator.start_index();
        const std::size_t page_end = paginator.snapshot().end_index;
        const std::size_t current_global = page_start + (std::size_t)state->selected_index;

        // Up: move selection up, scroll page if needed
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            if (state->selected_index > 0) {
                state->selected_index--;
                return true;
            }
            if (paginator.snapshot().scroll_position.can_scroll_up) {
                paginator.prev_page();
                state->selected_index = (int)(paginator.snapshot().end_index -
                    paginator.start_index()) - 1;
                return true;
            }
            return false;
        }

        // Down: move selection down, scroll page if needed
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            std::size_t next_global = current_global + 1;
            if (next_global < total_cards) {
                if ((std::size_t)(state->selected_index + 1) <
                    (page_end - page_start)) {
                    state->selected_index++;
                } else {
                    paginator.next_page();
                    state->selected_index = 0;
                }
                return true;
            }
            return false;
        }

        // Page keys
        if (event == Event::PageUp) {
            paginator.prev_page();
            state->selected_index = 0;
            return true;
        }
        if (event == Event::PageDown) {
            paginator.next_page();
            state->selected_index = 0;
            return true;
        }

        // Enter / i → install
        if (event == Event::Return ||
            event == Event::Character('i') || event == Event::Character('I')) {
            if (current_global < total_cards) {
                const auto& card = cards[current_global];
                if (state->inputs.on_install)
                    state->inputs.on_install(card.base.plugin_id);
                return true;
            }
        }

        return false;
    });
}

} // namespace cc::ui::plugins::plugin_marketplace_browse
