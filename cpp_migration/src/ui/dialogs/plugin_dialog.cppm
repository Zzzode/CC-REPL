/// @file plugin_dialog.cppm
/// @brief Plugin management interface — full FTXUI dialog with ViewState
///        routing.  Consumes pure data-prep from Phase 2 C1/A1 only.
///
/// View states covered:
///   - ViewKind::Menu                 5-card main dashboard
///   - ViewKind::ManagePlugins        (delegates → plugin_manage_panel)
///   - ViewKind::BrowseMarketplace    (delegates → plugin_marketplace_browse)
///   - ViewKind::DiscoverPlugins      (delegates → plugin_marketplace_browse)
///   - ViewKind::ManageMarketplaces   marketplace-list variant of browse
///   - ViewKind::AddMarketplace       URL input form
///   - ViewKind::Validate             manifest-validator result screen
///   - ViewKind::Help                 static help text (from k_plugin_help_text)
///   - ViewKind::Errors               error-tab rows (from ErrorRow model)
///   - TargetAction::InstallFlow      (delegates → plugin_install_flow)
///
/// Settings are handled by plugin_settings_dialog.cppm (per-plugin toggle +
/// keybindings editor).
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

export module cc.ui.dialogs.plugin_dialog;

import cc.types.types;
import cc.commands.plugin_ui_data;
import cc.commands.plugin_helpers;
import cc.commands.plugin_error_formatting;
import cc.commands.plugin_details_helpers;
import cc.commands.plugin_pagination_util;
import cc.commands.plugin_trust_text;

export namespace cc::ui::dialogs::plugin_dialog {
using namespace ftxui;

namespace ui = cc::commands::plugin_ui;
namespace pf = cc::commands::plugin_helpers;
namespace pe = cc::commands::plugin::error_formatting;
namespace pd = cc::commands::plugin;
namespace pp = cc::commands::plugin;
namespace pt = cc::commands::plugin;

using ui::ViewKind;
using ui::ViewState;
using ui::TabId;
using ui::TargetAction;
using ui::ErrorRow;
using ui::ErrorActionKind;
using ui::InstalledCellData;
using ui::SortedDiscoverListResult;
using ui::ConfigStep;
using ui::TrustWarningInfo;
using ui::TrustWarningLevel;
using ui::BrowseView;
using ui::MarketplaceInfo;

using pf::InstallablePlugin;
using pf::Pagination;
using pf::DisplayPluginError;
using pf::ErrorKind;

// =========================================================================
// 1.  Type bridge — Phase 2 data → display-ready aliases
// =========================================================================

/// Main-menu card definition (5 cards total).
struct MenuCard {
    std::string icon;
    std::string title;
    std::string subtitle;
    Color       accent;
    ViewKind    target;
    std::string hotkey;
};

/// Top-level plugin dialog options (inputs come from C1 data-prep).
struct PluginDialogInputs {
    ViewState initial_view;

    // ── ManagePlugins data ───────────────────────────────────────────────
    std::vector<InstalledCellData> installed_plugins;

    // ── Browse / Discover data ──────────────────────────────────────────
    std::vector<MarketplaceInfo>   marketplaces;
    std::vector<InstallablePlugin> discover_plugins;   // flattened across markets
    bool                           discover_empty = false;
    std::optional<std::string>     discover_empty_reason;

    // ── Errors-tab data ─────────────────────────────────────────────────
    std::vector<ErrorRow> error_rows;

    // ── Validate data ───────────────────────────────────────────────────
    std::optional<std::string> validate_path;
    bool                        validate_ok = true;
    std::vector<std::string>    validate_messages;

    // ── Add-marketplace data ────────────────────────────────────────────
    std::optional<std::string> add_marketplace_initial;

    // ── Callbacks (consumer-wired, never called from rendering code) ────
    std::function<void(const ViewState& next)> on_navigate;
    std::function<void()>                       on_close;
};

// =========================================================================
// 2.  Helpers — card / chip / tag rendering
// =========================================================================

namespace detail {

/// 5 dashboard cards — mirror of TS PluginSettings.tsx main menu.
inline std::vector<MenuCard> k_menu_cards = {
    {"📦", "Installed",    "Manage, enable, disable",      Color::Cyan,    ViewKind::ManagePlugins,      "1"},
    {"🛒", "Marketplace",  "Browse plugins in markets",    Color::Magenta, ViewKind::BrowseMarketplace,  "2"},
    {"✨", "Discover",     "Trending and recommended",    Color::Yellow,  ViewKind::DiscoverPlugins,    "3"},
    {"⚙️", "Settings",     "Global plugin settings",       Color::Blue,    ViewKind::ManageMarketplaces, "4"},
    {"✅", "Validate",     "Check a manifest",             Color::Green,   ViewKind::Validate,           "5"},
};

/// Render a single menu card (selected when highlighted).
[[nodiscard]] inline Element RenderMenuCard(const MenuCard& c, bool selected) {
    auto body = vbox({
        hbox({
            text(c.icon),
            text(" "),
            text(c.title) | bold | color(c.accent),
            filler(),
            text("[" + c.hotkey + "]") | dim | color(c.accent),
        }),
        text("  " + c.subtitle) | dim,
    }) | padding(1, 0, 1, 0);

    if (selected) {
        body = body | border | bgcolor(Color::RGB(25, 30, 45)) | color(c.accent);
    } else {
        body = body | borderLight | dim;
    }
    return body;
}

/// Render a chip/tag pill.
[[nodiscard]] inline Element Chip(std::string_view text, Color c = Color::Cyan) {
    return hbox({
        text(" "),
        text(std::string{text}) | color(c) | dim,
        text(" "),
    }) | borderLight | size(WIDTH, EQUAL, (int)text.size() + 2);
}

/// Render a small status badge (enabled / disabled / error).
[[nodiscard]] inline Element StatusBadge(std::string_view label, Color c) {
    return hbox({
        text("● ") | color(c),
        text(std::string{label}) | color(c) | dim,
    });
}

/// Truncate text to N characters with ellipsis.
[[nodiscard]] inline std::string truncate(std::string_view s, std::size_t n) {
    if (s.size() <= n) return std::string{s};
    return std::string{s.substr(0, n - 1)} + "…";
}

/// Pick a deterministic colour for a plugin name (for the letter-icon).
[[nodiscard]] inline Color name_color(std::string_view name) {
    static constexpr Color palette[] = {
        Color::Cyan, Color::Magenta, Color::Yellow, Color::Green,
        Color::Blue, Color::Orange1, Color::RedLight,
    };
    std::uint32_t h = 2166136261u;
    for (char c : name) { h ^= (std::uint8_t)c; h *= 16777619u; }
    return palette[h % std::size(palette)];
}

/// Coloured letter-icon (first char, uppercase).
[[nodiscard]] inline Element LetterIcon(std::string_view name) {
    const char c = name.empty() ? '?' : (char)std::toupper((unsigned char)name[0]);
    return hbox({
        text(" "),
        text(std::string{1, c}) | bold | color(name_color(name))
             | bgcolor(Color::RGB(20, 20, 35)),
        text(" "),
    });
}

} // namespace detail

// =========================================================================
// 3.  Sub-view renderers (render functions only; interactive components
//     live in ui/plugins/* which import this module)
// =========================================================================

// ── 3a.  Main menu (5 cards) ─────────────────────────────────────────────

/// Render the 5-card dashboard.  `selected` is the 0..4 card index.
[[nodiscard]] inline Element RenderMainMenu(int selected) {
    Elements rows;
    rows.push_back(hbox({
        text(" 🧩  Plugin Manager ") | bold | color(Color::Magenta),
        filler(),
        text("Select a section below") | dim,
    }));
    rows.push_back(separator());

    // Top row: 3 cards
    Elements top_row;
    for (int i = 0; i < 3; ++i) {
        top_row.push_back(detail::RenderMenuCard(detail::k_menu_cards[i], i == selected) | flex);
        if (i < 2) top_row.push_back(text("  "));
    }
    rows.push_back(hbox(std::move(top_row)));

    rows.push_back(text(""));

    // Bottom row: 2 cards, centered
    Elements bot_row;
    bot_row.push_back(filler());
    for (int i = 3; i < 5; ++i) {
        bot_row.push_back(detail::RenderMenuCard(detail::k_menu_cards[i], i == selected) | size(WIDTH, GREATER_THAN, 30));
        if (i < 4) bot_row.push_back(text("  "));
    }
    bot_row.push_back(filler());
    rows.push_back(hbox(std::move(bot_row)));

    rows.push_back(text(""));
    rows.push_back(hbox({
        text(" ↑/↓") | bold | color(Color::Cyan), text(" navigate  "),
        text("⏎") | bold | color(Color::Cyan), text(" open  "),
        text("1-5") | bold | color(Color::Cyan), text(" quick-jump  "),
        text("Esc") | bold | color(Color::Cyan), text(" close"),
        filler(),
        text("/plugin help") | dim,
    }) | dim);

    return vbox(std::move(rows));
}

// ── 3b.  Help view ───────────────────────────────────────────────────────

/// Render the static help block (verbatim k_plugin_help_text).
[[nodiscard]] inline Element RenderHelp() {
    return vbox({
        hbox({
            text(" ❔  Plugin Command Help ") | bold | color(Color::Cyan),
            filler(),
            text("Esc to go back") | dim,
        }),
        separator(),
        paragraph(std::string{ui::k_plugin_help_text}) | color(Color::GrayLight),
    }) | borderLight;
}

// ── 3c.  Errors tab ──────────────────────────────────────────────────────

/// Render a single error row.
[[nodiscard]] inline Element RenderErrorRow(const ErrorRow& row, bool selected) {
    Color head_color = Color::Red;
    if (row.action_kind == ErrorActionKind::ManagedOnly) head_color = Color::Yellow;
    if (row.action_kind == ErrorActionKind::Navigate)    head_color = Color::Orange1;

    Elements parts{
        text(selected ? "› " : "  ") | color(head_color),
        text(row.label) | bold | color(selected ? Color::White : Color::GrayLight),
        filler(),
    };
    if (row.scope) {
        parts.push_back(text("[" + *row.scope + "] ") | dim | color(Color::Cyan));
    }
    auto line = hbox(std::move(parts));
    if (selected) line = line | bgcolor(Color::RGB(30, 20, 20));

    Elements sub{line};
    if (!row.message.empty()) {
        sub.push_back(paragraph("   " + row.message) | dim | color(Color::GrayLight));
    }
    if (row.guidance) {
        sub.push_back(hbox({
            text("   💡 "),
            paragraph(*row.guidance) | dim | color(Color::Yellow),
        }));
    }
    return vbox(std::move(sub));
}

/// Render the full errors-tab list.
[[nodiscard]] inline Element RenderErrorsTab(
    const std::vector<ErrorRow>& rows, int selected)
{
    Elements items;
    if (rows.empty()) {
        items.push_back(text(" 🎉  No errors to report. Everything looks good!")
                        | color(Color::Green) | dim | center);
    } else {
        for (int i = 0; i < (int)rows.size(); ++i) {
            items.push_back(RenderErrorRow(rows[i], i == selected));
            if (i + 1 < (int)rows.size()) items.push_back(separator() | dim);
        }
    }

    return vbox({
        hbox({
            text(" ⚠  Errors & Warnings ") | bold | color(Color::Red),
            filler(),
            text(std::format("{} item{}", (int)rows.size(), rows.size() == 1 ? "" : "s")) | dim,
        }),
        separator(),
        vbox(std::move(items)) | vscroll_indicator | yframe | flex,
        separator(),
        text(" ↑/↓ navigate  ⏎ act  Esc back") | dim,
    });
}

// ── 3d.  Validate view ───────────────────────────────────────────────────

[[nodiscard]] inline Element RenderValidate(
    const std::optional<std::string>& path, bool ok,
    const std::vector<std::string>& messages)
{
    Elements body;
    body.push_back(hbox({
        text(" ✅  Manifest Validation ") | bold | color(ok ? Color::Green : Color::Red),
        filler(),
        path ? text(*path) | color(Color::Cyan) | underlined : text("(no path)") | dim,
    }));
    body.push_back(separator());

    if (ok) {
        body.push_back(hbox({
            text("  ✓ ") | color(Color::Green) | bold,
            text("Manifest is valid.") | color(Color::Green),
        }));
    } else {
        body.push_back(hbox({
            text("  ✗ ") | color(Color::Red) | bold,
            text("Validation failed:") | color(Color::Red),
        }));
    }

    Elements msgs;
    for (const auto& m : messages) {
        msgs.push_back(hbox({
            text("    • ") | dim,
            paragraph(m) | (ok ? color(Color::GrayLight) : color(Color::RedLight) | dim),
        }));
    }
    body.push_back(vbox(std::move(msgs)) | borderEmpty);

    body.push_back(separator());
    body.push_back(text(" Esc back to menu") | dim);
    return vbox(std::move(body));
}

// ── 3e.  Add-marketplace form ────────────────────────────────────────────

/// Render the AddMarketplace input form skeleton (the actual interactive
/// Input widget is built in MakeAddMarketplaceComponent below).
[[nodiscard]] inline Element RenderAddMarketplace(
    std::string_view current_value,
    bool value_is_valid,
    std::string_view validation_hint)
{
    Color c = value_is_valid ? Color::Green : Color::Yellow;
    return vbox({
        hbox({
            text(" ➕  Add Marketplace ") | bold | color(Color::Blue),
            filler(),
            text("Esc to cancel") | dim,
        }),
        separator(),
        text("") | size(HEIGHT, EQUAL, 1),
        hbox({
            text("  Marketplace URL or path: ") | bold,
            text(std::string{current_value.empty() ? "(type here)" : current_value})
                | color(c),
            text("│") | blink,
        }) | borderLight | color(value_is_valid ? Color::Green : Color::GrayDark),
        text("") | size(HEIGHT, EQUAL, 1),
        paragraph(std::string{validation_hint.empty()
            ? "Paste a marketplace.json URL, a local directory path, or a "
              "GitHub repo URL (owner/repo)."
            : std::string{validation_hint}}) | dim,
        text(""),
        hbox({
            detail::Chip("file:", Color::Cyan), text(" "),
            detail::Chip("http(s)://", Color::Green), text(" "),
            detail::Chip("github:owner/repo", Color::Blue), text(" "),
            detail::Chip("local path", Color::Magenta),
        }) | dim,
    });
}

// ── 3f.  Empty-state renderer (discover) ─────────────────────────────────

[[nodiscard]] inline Element RenderDiscoverEmpty(
    bool needs_empty,
    const std::optional<std::string>& reason)
{
    if (!needs_empty) return text("");
    return vbox({
        text("") | size(HEIGHT, EQUAL, 2),
        hbox({filler(),
            text(" 🔍 ") | color(Color::Yellow) | dim,
            text(reason.value_or("No plugins match your filters.")) | dim,
            filler(),
        }),
        hbox({filler(),
            text(" Try removing filters or adding a marketplace. ") | dim,
            filler(),
        }),
    }) | flex;
}

// =========================================================================
// 4.  Public: top-level renderer + component
// =========================================================================

/// Render the full dialog chrome — wraps the active sub-view.
[[nodiscard]] inline Element RenderPluginDialogChrome(
    Element sub_view,
    ViewKind active_kind,
    TabId active_tab)
{
    // Top tab bar (4 tabs)
    static constexpr std::array<std::pair<const char*, TabId>, 4> k_tabs = {{
        {"Discover",     TabId::Discover},
        {"Installed",    TabId::Installed},
        {"Marketplaces", TabId::Marketplaces},
        {"Errors",       TabId::Errors},
    }};
    Elements tab_bits;
    for (const auto& [label, id] : k_tabs) {
        const bool active = (id == active_tab);
        auto bit = text(" " + std::string{label} + " ");
        if (active) bit = bit | bold | bgcolor(Color::RGB(30, 40, 60)) | color(Color::Cyan);
        else        bit = bit | dim;
        tab_bits.push_back(std::move(bit));
    }

    auto top_bar = hbox({
        text(" 🧩 ") | color(Color::Magenta),
        hbox(std::move(tab_bits)),
        filler(),
        text(std::format("view: {}", ui::view_kind_name(active_kind))) | dim,
    });

    return vbox({
        top_bar,
        separator() | color(Color::Magenta),
        std::move(sub_view) | flex,
    }) | borderDouble | bgcolor(Color::RGB(12, 12, 20));
}

// =========================================================================
// 5.  Factory: interactive PluginDialog component
// =========================================================================

struct PluginDialogState {
    PluginDialogInputs inputs;
    ViewKind           active_kind;
    TabId              active_tab;

    // ── Menu navigation ──────────────────────────────────────────────────
    int menu_selected = 0;

    // ── Errors list ─────────────────────────────────────────────────────
    int errors_selected = 0;

    // ── Add-marketplace ─────────────────────────────────────────────────
    std::string add_mp_value;
    bool        add_mp_open = false;

    // ── Install-flow routing ────────────────────────────────────────────
    bool install_flow_active = false;
};

/// Build the top-level plugin dialog component.
/// Sub-views (Manage / Browse / Install / Settings) are produced by sibling
/// factory functions in cc::ui::plugins:: — this router composes them.
[[nodiscard]] inline Component MakePluginDialog(PluginDialogInputs inputs) {
    auto state = std::make_shared<PluginDialogState>();
    state->inputs = std::move(inputs);
    state->active_kind = state->inputs.initial_view.kind;
    state->active_tab  = ui::initial_tab_for(state->active_kind);

    // Pre-fill add-marketplace if routed with an initial value.
    if (state->inputs.add_marketplace_initial) {
        state->add_mp_value = *state->inputs.add_marketplace_initial;
    }

    return Renderer([state] {
        Element sub;
        switch (state->active_kind) {
            case ViewKind::Menu:
            case ViewKind::MarketplaceMenu:
                sub = RenderMainMenu(state->menu_selected);
                break;

            case ViewKind::Help:
                sub = RenderHelp();
                break;

            case ViewKind::DiscoverPlugins:
            case ViewKind::BrowseMarketplace:
            case ViewKind::ManagePlugins:
            case ViewKind::ManageMarketplaces:
                // Rendered by delegated panels.  We keep a fallback here so
                // the router never draws a blank screen.
                sub = vbox({
                    text(std::format(" ⏳  {} view — pending panel mount",
                                     ui::view_kind_name(state->active_kind)))
                         | color(Color::Yellow) | dim | center,
                    text(" (cc::ui::plugins panel not yet composed)") | dim | center,
                }) | border;
                break;

            case ViewKind::Validate:
                sub = RenderValidate(state->inputs.validate_path,
                                     state->inputs.validate_ok,
                                     state->inputs.validate_messages);
                break;

            case ViewKind::Errors:
                sub = RenderErrorsTab(state->inputs.error_rows, state->errors_selected);
                break;

            case ViewKind::AddMarketplace: {
                const bool valid = !state->add_mp_value.empty() &&
                                   state->add_mp_value.size() >= 4;
                sub = RenderAddMarketplace(state->add_mp_value, valid, "");
                break;
            }

            case ViewKind::MarketplaceList:
                sub = vbox({
                    hbox({
                        text(" 📋  Marketplace List ") | bold | color(Color::Blue),
                        filler(),
                        text("Esc back") | dim,
                    }),
                    separator(),
                    text(" (delegated to marketplace-browse panel)") | dim | center,
                }) | border;
                break;
        }

        return RenderPluginDialogChrome(std::move(sub), state->active_kind, state->active_tab);

    }) | CatchEvent([state](Event event) -> bool {
        // Global Esc → close / back
        if (event == Event::Escape) {
            if (state->active_kind == ViewKind::Menu) {
                if (state->inputs.on_close) state->inputs.on_close();
                return true;
            }
            // Go back to main menu
            state->active_kind = ViewKind::Menu;
            state->active_tab  = ui::initial_tab_for(state->active_kind);
            return true;
        }

        // Global: h (help)
        if (event == Event::Character('h') || event == Event::Character('H')) {
            state->active_kind = ViewKind::Help;
            return true;
        }

        // Global: digit 1-5 → menu card quick-jump
        if (event.is_character()) {
            const char ch = event.character()[0];
            if (ch >= '1' && ch <= '5') {
                int idx = ch - '1';
                if (idx < (int)detail::k_menu_cards.size()) {
                    auto target = detail::k_menu_cards[idx].target;
                    state->active_kind = target;
                    state->active_tab  = ui::initial_tab_for(target);
                    if (state->inputs.on_navigate) {
                        ViewState vs; vs.kind = target;
                        state->inputs.on_navigate(vs);
                    }
                    return true;
                }
            }
        }

        // ── View-specific handlers ────────────────────────────────────────
        switch (state->active_kind) {
            case ViewKind::Menu:
            case ViewKind::MarketplaceMenu: {
                if (event == Event::ArrowUp || event == Event::Character('k')) {
                    state->menu_selected = std::max(0, state->menu_selected - 1);
                    return true;
                }
                if (event == Event::ArrowDown || event == Event::Character('j')) {
                    state->menu_selected = std::min(
                        (int)detail::k_menu_cards.size() - 1, state->menu_selected + 1);
                    return true;
                }
                if (event == Event::ArrowLeft || event == Event::ArrowRight) {
                    int delta = (event == Event::ArrowRight) ? 1 : -1;
                    state->menu_selected = std::clamp(
                        state->menu_selected + delta, 0,
                        (int)detail::k_menu_cards.size() - 1);
                    return true;
                }
                if (event == Event::Return) {
                    auto target = detail::k_menu_cards[state->menu_selected].target;
                    state->active_kind = target;
                    state->active_tab  = ui::initial_tab_for(target);
                    if (state->inputs.on_navigate) {
                        ViewState vs; vs.kind = target;
                        state->inputs.on_navigate(vs);
                    }
                    return true;
                }
                return false;
            }

            case ViewKind::Errors: {
                const int n = (int)state->inputs.error_rows.size();
                if (event == Event::ArrowUp || event == Event::Character('k')) {
                    state->errors_selected = std::max(0, state->errors_selected - 1);
                    return true;
                }
                if (event == Event::ArrowDown || event == Event::Character('j')) {
                    state->errors_selected = std::min(std::max(0, n - 1),
                                                       state->errors_selected + 1);
                    return true;
                }
                if (event == Event::Return && n > 0) {
                    const auto& row = state->inputs.error_rows[state->errors_selected];
                    if (row.action_kind == ErrorActionKind::Navigate &&
                        row.nav_view.has_value())
                    {
                        state->active_kind = *row.nav_view;
                        state->active_tab  = row.nav_tab.value_or(
                            ui::initial_tab_for(*row.nav_view));
                        if (state->inputs.on_navigate) {
                            ViewState vs;
                            vs.kind = *row.nav_view;
                            vs.target_plugin = row.nav_target_plugin;
                            vs.target_marketplace = row.nav_target_marketplace;
                            vs.action = row.nav_action;
                            state->inputs.on_navigate(vs);
                        }
                    }
                    return true;
                }
                return false;
            }

            case ViewKind::AddMarketplace: {
                if (event.is_character()) {
                    state->add_mp_value.push_back(event.character()[0]);
                    return true;
                }
                if (event == Event::Backspace && !state->add_mp_value.empty()) {
                    state->add_mp_value.pop_back();
                    return true;
                }
                if (event == Event::Return && !state->add_mp_value.empty()) {
                    if (state->inputs.on_navigate) {
                        ViewState vs;
                        vs.kind = ViewKind::ManageMarketplaces;
                        vs.add_initial_value = state->add_mp_value;
                        state->inputs.on_navigate(vs);
                    }
                    state->active_kind = ViewKind::ManageMarketplaces;
                    return true;
                }
                return false;
            }

            default:
                return false;
        }
        return false;
    });
}

} // namespace cc::ui::dialogs::plugin_dialog
