/// @file permission_rules_ui.cppm
/// @brief Top-level permissions panel: 4-tab container (All Rules / Recent
///        Denials / Workspaces / Create Rule).  Each tab delegates to the
///        matching builder in cc::ui::permissions::rule_list.
module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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

export module cc.ui.permissions.permission_rules_ui;

import cc.types.types;
import cc.ui.permissions.rule_list;
import cc.ui.design.tokens;
import cc.ui.permissions.components;

export namespace cc::ui::permissions {

using namespace ftxui;
namespace rl   = cc::ui::permissions::rule_list;
namespace dt   = cc::ui::design::tokens;
namespace pc   = cc::ui::permissions::components;
namespace peng = cc::utils::permissions_engine;

// Panel model/callbacks and PermTab are owned by cc.ui.permissions.rule_list
// (exported) so BuildPermissionsTabs can consume them without a circular
// import; alias them into this namespace.
using rl::PermissionsPanelCallbacks;
using rl::PermissionsPanelModel;
using rl::PermTab;

inline constexpr std::array<const char*, 4> kPermTabNames = {
    "All Rules", "Recent Denials", "Workspaces", "Create Rule",
};
inline const std::array<Color, 4> kPermTabAccents = {
    Color::Cyan, Color::Red, Color::Blue, Color::Green,
};
inline constexpr std::array<const char*, 4> kPermTabIcons = {
    "#",      // shield placeholder
    "!",      // warning placeholder
    "F",      // flag placeholder
    "+",      // plus
};

// --------------------------------------------------------------------------
// Rendering helpers
// --------------------------------------------------------------------------

namespace detail {

/// Render the 4-tab header with active-tab + count-badge styling.
[[nodiscard]] inline Element RenderPermTabBar(PermTab active,
                                              std::size_t denials_count,
                                              std::size_t workspace_count)
{
    Elements bits;
    for (std::size_t i = 0; i < kPermTabNames.size(); ++i) {
        const bool sel = (i == static_cast<std::size_t>(active));
        const Color accent = kPermTabAccents[i];

        Elements label_parts = {
            text(std::string{kPermTabIcons[i]}) | color(accent),
            text(" "),
            text(std::string{kPermTabNames[i]}),
        };

        if (i == 1 && denials_count > 0) {
            label_parts.push_back(text(" "));
            label_parts.push_back(text(std::format("{}", denials_count))
                                  | bold | color(Color::White)
                                  | bgcolor(Color::Red));
        }
        if (i == 2 && workspace_count > 0) {
            label_parts.push_back(text(" "));
            label_parts.push_back(text(std::format("{}", workspace_count))
                                  | dim | color(Color::BlueLight));
        }

        auto label = hbox(std::move(label_parts));
        auto cell = hbox({text(" "), std::move(label), text(" ")})
                  | (sel ? (bold | color(accent) |
                            bgcolor(Color::RGB(20, 30, 55)) | underlined)
                         : dim);
        bits.push_back(std::move(cell));
        if (i + 1 < kPermTabNames.size())
            bits.push_back(text("|") | dim);
    }
    return hbox(std::move(bits));
}

} // namespace detail

// --------------------------------------------------------------------------
// Top-level public factory
// --------------------------------------------------------------------------

/// Build the 4-tab permissions panel.  Delegates per-tab content to
/// rule_list::BuildPermissionsTabs; this wrapper owns only the tab bar UI,
/// the active-tab state cell, and tab-navigation event handling.
[[nodiscard]] inline Component BuildPermissionsPanel(
    PermissionsPanelModel model,
    PermissionsPanelCallbacks cbs)
{
    auto active = std::make_shared<PermTab>(PermTab::AllRules);

    rl::PermissionsTabsState tabs_state;
    tabs_state.model      = std::move(model);
    tabs_state.callbacks  = std::move(cbs);

    auto inner = rl::BuildPermissionsTabs(tabs_state, active);

    return Renderer(inner, [active, inner] {
        const auto denials_vec =
            peng::recent_denials(50);
        const auto workspaces_vec =
            peng::workspace_directories();
        const std::size_t dc = denials_vec.size();
        const std::size_t wc = workspaces_vec.size();

        auto header = vbox({
            hbox({
                text(" Permission Rules ")
                    | bold | color(dt::palette::dark.primary),
                filler(),
                text("[1/2/3/4] jump  [Left/Right] switch tab  [Esc] close")
                    | dim,
            }),
            pc::ThinDivider(),
            detail::RenderPermTabBar(*active, dc, wc),
        });

        return vbox({
            header,
            pc::ThinDivider(),
            inner->Render() | yflex_grow,
        }) | yflex_grow | borderLight | color(dt::palette::dark.primary);
    }) | CatchEvent([active](Event e) -> bool {
        // Global tab-switching keys.
        if (e == Event::ArrowRight || e == Event::Character('l')
                                   || e == Event::Character('L')) {
            *active = static_cast<PermTab>(
                (static_cast<std::size_t>(*active) + 1) % kPermTabNames.size());
            return true;
        }
        if (e == Event::ArrowLeft || e == Event::Character('h')
                                  || e == Event::Character('H')) {
            *active = static_cast<PermTab>(
                (static_cast<std::size_t>(*active) + kPermTabNames.size() - 1)
                % kPermTabNames.size());
            return true;
        }
        // Hotkeys: 1/2/3/4 jump directly.
        if (e.is_character()) {
            const char c = e.character().front();
            if (c >= '1' && c <= '4') {
                *active = static_cast<PermTab>(
                    static_cast<std::size_t>(c - '1'));
                return true;
            }
        }
        return false;
    });
}

} // namespace cc::ui::permissions
