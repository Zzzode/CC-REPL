/// @file plugin_marketplace_browse.cppm
/// @brief Marketplace browser: two sub-tabs (Browse / Discover), per-market
///        dropdown, 2-column plugin card grid with icon / rating / install
///        button / collapsible description / tags.
///
/// Data comes from plugin_ui_data C1:
///   - MarketplaceInfo    (marketplace list with counts + warnings)
///   - InstallablePlugin  (individual plugin entries with metadata)
///   - PaginationUtil     (windowing for both browse and discover lists)
module;

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <random>
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
    bool                 description_expanded = false;
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

// Pick a colour for the coloured-letter icon.
[[nodiscard]] inline Color card_name_color(std::string_view name) {
    static const Color palette[] = {
        Color::Cyan, Color::Magenta, Color::Yellow, Color::GreenLight,
        Color::BlueLight, Color::Orange1, Color::RedLight, Color::CyanLight,
    };
    std::uint32_t h = 0x811c9dc5u;
    for (char c : name) { h ^= (std::uint8_t)c; h *= 0x01000193u; }
    return palette[h % std::size(palette)];
}

[[nodiscard]] inline Element CardIcon(std::string_view name) {
    const char c = name.empty() ? '?' : (char)std::toupper((unsigned char)name[0]);
    return vbox({
        text(""),
        hbox({
            text(" "),
            text(std::string{1, c}) | bold | color(card_name_color(name))
                 | bgcolor(Color::RGB(18, 18, 30)) | size(WIDTH, EQUAL, 3),
            text(" "),
        }) | size(WIDTH, EQUAL, 5) | center,
        text(""),
    }) | size(WIDTH, EQUAL, 5) | size(HEIGHT, EQUAL, 3);
}

[[nodiscard]] inline Element Tag(std::string_view t, Color c = Color::Cyan) {
    return hbox({
        text(" "),
        text(std::string{t}) | color(c) | dim,
        text(" "),
    }) | borderLight;
}

/// Render a single plugin card (as part of a 2-column grid).
[[nodiscard]] inline Element RenderPluginCard(
    const PluginCardData& card,
    bool selected,
    int)   // 0 or 1 for 2-col grid
{
    Color accent = card_name_color(card.name);
    auto name_el = text(card.name) | bold | color(accent);

    // Rating line
    std::string stars;
    int full = (int)card.rating;
    bool half = (std::round(card.rating * 10.0f) / 10.0f) - (float)full >= 0.5f;
    for (int i = 0; i < full; ++i) stars += "★";
    if (half) stars += "☆";
    int empty = 5 - full - (half ? 1 : 0);
    for (int i = 0; i < empty; ++i) stars += "·";
    std::string rating_str = card.rating_count > 0
        ? std::format("{} ({})", card.rating > 0 ? std::format("{:.1f}", card.rating) : "★",
                      card.rating_count)
        : "—";

    // Header row: icon + name + author + rating + install button
    auto header = hbox({
        CardIcon(card.name),
        text(" "),
        vbox({
            hbox({
                std::move(name_el),
                filler(),
                text(stars) | color(Color::Yellow),
                text(" " + rating_str) | dim,
                text("  "),
                text(selected ? "[ Install ▶ ]" : "  Install  ")
                     | bold | color(selected ? Color::White : Color::Green)
                     | bgcolor(selected ? Color::Green : Color::RGB(10, 30, 15)),
            }),
            hbox({
                text(card.author.empty() ? "Unknown author" : card.author) | dim | color(Color::GrayLight),
                filler(),
                text(card.install_count > 0
                         ? std::format("{} installs", card.install_count)
                         : "new"),
            }) | dim,
        }) | flex,
    });

    // Description (collapsible)
    std::string desc = card.description.empty()
        ? "(no description provided)" : card.description;
    Element desc_el;
    const std::size_t collapse_at = 140;
    if (!card.description_expanded && desc.size() > collapse_at) {
        desc_el = paragraph(desc.substr(0, collapse_at) + "…")
                | dim | color(Color::GrayLight);
    } else {
        desc_el = paragraph(desc) | dim | color(Color::GrayLight);
    }
    auto hint_more = desc.size() > collapse_at && !card.description_expanded
        ? text(" [↕ expand]") | dim | color(Color::Cyan)
        : (card.description_expanded ? text(" [↕ collapse]") | dim | color(Color::Cyan)
                                     : text(""));

    // Tags + version + size footer
    Elements tag_els;
    for (std::size_t i = 0; i < card.tags.size() && i < 4; ++i) {
        tag_els.push_back(Tag(card.tags[i], i == 0 ? Color::Magenta
                                                    : (i == 1 ? Color::Yellow
                                                              : Color::Cyan)));
        if (i + 1 < card.tags.size() && i + 1 < 4) tag_els.push_back(text(" "));
    }

    auto footer = hbox({
        hbox(std::move(tag_els)) | flex,
        text(" "),
        text("v" + card.version) | dim,
        text("  "),
        text(card.size_kb_str.empty() ? "—" : card.size_kb_str) | dim | color(Color::BlueLight),
    });

    auto body = vbox({
        std::move(header),
        separator() | dim,
        hbox({
            text("   "),
            vbox({
                std::move(desc_el),
                hint_more,
            }) | flex,
        }),
        separator() | dim,
        std::move(footer),
    });

    if (selected) {
        body = body | border | color(Color::Green) | bgcolor(Color::RGB(12, 22, 35));
    } else {
        body = body | borderLight | dim;
    }
    return body;
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
    pp::Paginator     discover_paginator{0, 4};   // 4 rows = 8 cards (2-col grid)
    pp::Paginator     browse_paginator{0, 4};

    // Card focus (in 2-col grid): row+col within current page
    int               focus_row = 0;
    int               focus_col = 0;
    static constexpr int k_cols = 2;
};

/// Build a 2-column card grid for the current page.
[[nodiscard]] inline Element RenderCardGrid(
    BrowseState& s)
{
    const bool is_discover = (s.active_subtab == SubTab::Discover);
    const auto& cards = is_discover
        ? s.inputs.discover_cards
        : (s.mp_selected < (int)s.inputs.marketplace_cards.size()
              ? s.inputs.marketplace_cards[s.mp_selected]
              : s.inputs.marketplace_cards.front());
    auto& paginator = is_discover ? s.discover_paginator : s.browse_paginator;
    paginator = pp::Paginator{cards.size(), 4};

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

    // Build 2-column rows
    Elements grid_rows;
    for (std::size_t r = snap.start_index; r < snap.end_index; ++r) {
        Elements row_els;
        int actual_row = (int)(r - snap.start_index);
        for (int c = 0; c < BrowseState::k_cols; ++c) {
            const std::size_t card_idx = r * BrowseState::k_cols + c;
            const bool sel = (actual_row == s.focus_row && c == s.focus_col);

            Element card;
            if (card_idx < cards.size()) {
                card = detail::RenderPluginCard(cards[card_idx], sel, c);
            } else {
                card = text("") | size(WIDTH, EQUAL, 50) | size(HEIGHT, EQUAL, 8);
            }
            row_els.push_back(std::move(card) | flex);
            if (c + 1 < BrowseState::k_cols) row_els.push_back(text(" "));
        }
        grid_rows.push_back(hbox(std::move(row_els)));
    }

    // Footer: pagination + keyboard hints
    auto footer = hbox({
        text(std::format(" Showing {}-{} of {} plugin{}  ",
                         snap.scroll_position.current == 0 ? 0 : (int)snap.start_index * BrowseState::k_cols + 1,
                         std::min((int)snap.end_index * BrowseState::k_cols, (int)cards.size()),
                         (int)cards.size(),
                         cards.size() == 1 ? "" : "s")) | dim,
        filler(),
        text(" ↑↓←→/hjkl select  ") | dim,
        text("⏎") | bold | color(Color::Cyan) | dim,
        text(" install  ") | dim,
        text("Tab") | bold | color(Color::Cyan) | dim,
        text(" subtab  ") | dim,
        text("E") | bold | color(Color::Cyan) | dim,
        text("xpand desc") | dim,
    });

    return vbox({
        vbox(std::move(grid_rows)) | flex,
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

        auto grid = RenderCardGrid(*state);

        return vbox({
            std::move(sub_tab_bar),
            separator() | dim,
            std::move(market_header),
            separator() | dim,
            std::move(grid) | flex,
        });
    }) | CatchEvent([state](Event event) -> bool {
        // Sub-tab switch (Tab)
        if (event == Event::Tab) {
            state->active_subtab = (state->active_subtab == SubTab::Browse)
                ? SubTab::Discover : SubTab::Browse;
            state->focus_row = 0;
            state->focus_col = 0;
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
                state->focus_row = 0; state->focus_col = 0;
                return true;
            }
            if (event == Event::Character(']') || event == Event::Character('}')) {
                state->mp_selected = (state->mp_selected + 1)
                    % (int)state->inputs.marketplaces.size();
                state->focus_row = 0; state->focus_col = 0;
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
        const int rows_per_page = 4;
        if (total_cards == 0) return false;

        const std::size_t current_linear = (std::size_t)(
            (is_discover ? state->discover_paginator.start_index()
                         : state->browse_paginator.start_index()) + state->focus_row)
            * BrowseState::k_cols + state->focus_col;

        auto try_move = [&](int drow, int dcol) -> bool {
            int nr = state->focus_row + drow;
            int nc = state->focus_col + dcol;
            // Same page?
            if (nr >= 0 && nr < rows_per_page) {
                if (nc >= 0 && nc < BrowseState::k_cols) {
                    const std::size_t target = (
                        (is_discover ? state->discover_paginator.start_index()
                                     : state->browse_paginator.start_index()) + nr)
                        * BrowseState::k_cols + nc;
                    if (target < total_cards) {
                        state->focus_row = nr;
                        state->focus_col = nc;
                        return true;
                    }
                }
            }
            // Off-page: move paginator
            auto& paginator = is_discover
                ? state->discover_paginator : state->browse_paginator;
            if (drow < 0) {
                if (paginator.snapshot().scroll_position.can_scroll_up) {
                    paginator.prev_page();
                    state->focus_row = rows_per_page - 1;
                    return true;
                }
            }
            if (drow > 0) {
                if (paginator.snapshot().scroll_position.can_scroll_down) {
                    paginator.next_page();
                    state->focus_row = 0;
                    return true;
                }
            }
            return false;
        };

        // Card grid navigation
        if (event == Event::ArrowUp   || event == Event::Character('k')) { return try_move(-1, 0); }
        if (event == Event::ArrowDown || event == Event::Character('j')) { return try_move( 1, 0); }
        if (event == Event::ArrowLeft || event == Event::Character('h')) { return try_move( 0,-1); }
        if (event == Event::ArrowRight|| event == Event::Character('l')) { return try_move( 0, 1); }

        // Page keys
        if (event == Event::PageUp) {
            auto& p = is_discover ? state->discover_paginator : state->browse_paginator;
            p.prev_page(); state->focus_row = 0; state->focus_col = 0; return true;
        }
        if (event == Event::PageDown) {
            auto& p = is_discover ? state->discover_paginator : state->browse_paginator;
            p.next_page(); state->focus_row = 0; state->focus_col = 0; return true;
        }

        // Expand/collapse description (E)
        if (event == Event::Character('e') || event == Event::Character('E')) {
            if (current_linear < cards.size()) {
                auto& mutable_cards = is_discover
                    ? state->inputs.discover_cards
                    : state->inputs.marketplace_cards[state->mp_selected];
                mutable_cards[current_linear].description_expanded =
                    !mutable_cards[current_linear].description_expanded;
                return true;
            }
        }

        // Enter → install
        if (event == Event::Return) {
            if (current_linear < cards.size()) {
                const auto& card = cards[current_linear];
                if (state->inputs.on_install)
                    state->inputs.on_install(card.base.plugin_id);
                return true;
            }
        }

        return false;
    });
}

} // namespace cc::ui::plugins::plugin_marketplace_browse
