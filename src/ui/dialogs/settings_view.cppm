/// @file settings_view.cppm
/// @brief Faithful Settings read-only view panel — port of TS Settings.tsx
///        read-only summary view.  Shows key configuration at a glance.
///
/// MODULE:   cc.ui.dialogs.settings_view
/// LICENCE:  Exported.  Imported by repl_screen (SettingsView panel mode).
///
/// TS REFERENCE:
///   src/components/Settings/Settings.tsx (read-only summary view)
///   src/components/Settings/General.tsx
///
/// VISUAL STRUCTURE (faithful to TS):
///   ┌─ Settings ──────────────────────────────────────────┐
///   │  Quick overview of your current configuration.       │
///   ├──────────────────────────────────────────────────────┤
///   │                                                      │
///   │  General                                             │
///   │    Theme         Dark (system)                       │
///   │    Vim mode      off                                 │
///   │    Brief mode    off                                 │
///   │                                                      │
///   │  Model                                               │
///   │    Model         claude-sonnet-4-20250514            │
///   │    Max output    8192 tokens                         │
///   │    Temperature  0.7                                  │
///   │                                                      │
///   │  API                                                 │
///   │    Endpoint      api.anthropic.com                   │
///   │    Status        ● connected                         │
///   │                                                      │
///   ├──────────────────────────────────────────────────────┤
///   │  For full settings: /config                           │
///   │  esc to close                                        │
///   └──────────────────────────────────────────────────────┘
///
/// KEYBOARD:
///   Esc  — close dialog
///   c    — open full config (handled by caller)
module;

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.settings_view;

import cc.ui.dialogs.frame;
import cc.ui.design.theme;
import cc.ui.design.tokens;

export namespace cc::ui::dialogs::settings_view {

using namespace ftxui;
namespace dframe = cc::ui::dialogs::frame;
using Theme = cc::ui::design::theme::Theme;
using Role = cc::ui::design::tokens::Role;

// ============================================================
// Types
// ============================================================

/// A single setting row in the view.
struct SettingRow {
    std::string label;
    std::string value;
    bool is_enabled = true;
    std::optional<std::string> hint;  ///< dim hint text below value
};

/// A section of settings.
struct SettingSection {
    std::string title;
    std::vector<SettingRow> rows;
};

/// Settings view properties — all the data to display.
struct SettingsViewProps {
    std::string app_version = "0.0.0";
    std::string model_name;
    std::string default_model;
    std::string theme_name = "Dark";
    bool vim_mode = false;
    bool brief_mode = false;
    bool auto_mode = false;
    bool fast_mode = false;
    std::string api_endpoint = "api.anthropic.com";
    bool api_connected = true;
    int mcp_server_count = 0;
    std::string permission_default = "ask";
    /// Called when user wants to open full config (c key).
    std::function<void()> on_open_config;
    /// Called when dialog is dismissed (Esc / close).
    std::function<void()> on_close;
};

/// Mutable state for the interactive settings view.
struct SettingsViewState {
    int selected_section = 0;
    int scroll_offset = 0;
};

// ============================================================
// Helpers
// ============================================================

namespace detail {

/// Render a section title with an underline-style divider.
[[nodiscard]] inline Element RenderSectionTitle(const std::string& title,
                                                  const Theme& theme) {
    return hbox({
        text(" "),
        text(title) | bold | color(theme.color_for(Role::Info)),
        filler(),
    });
}

/// Render a single setting row: label | value | (hint).
[[nodiscard]] inline Element RenderSettingRow(const SettingRow& row,
                                               const Theme& theme) {
    auto value_el = text(row.value);
    if (!row.is_enabled) {
        value_el = value_el | dim;
    }

    auto label_el = text("  " + row.label) | dim | size(WIDTH, EQUAL, 22);

    Elements rows;
    rows.push_back(hbox({
        label_el,
        value_el,
        filler(),
    }));

    if (row.hint && !row.hint->empty()) {
        rows.push_back(hbox({
            text("    "),
            text(*row.hint) | dim | color(theme.color_for(Role::Muted)),
            filler(),
        }));
    }

    return vbox(std::move(rows));
}

/// Render the footer: hint about full config + dismiss hint.
[[nodiscard]] inline Element RenderFooter(const Theme& theme) {
    return vbox({
        hbox({
            text("  For full settings: ") | dim,
            text("/config") | color(theme.color_for(Role::Info)),
            filler(),
        }),
        hbox({
            text("  "),
            text("esc") | dim,
            text(" to close") | dim,
            filler(),
        }),
    });
}

/// Build default sections from props.
[[nodiscard]] inline std::vector<SettingSection> build_sections(
    const SettingsViewProps& props)
{
    std::vector<SettingSection> sections;

    // General section
    sections.push_back({"General", {
        {"Theme", props.theme_name, true, {}},
        {"Vim mode", props.vim_mode ? "on" : "off", true, {}},
        {"Brief mode", props.brief_mode ? "on" : "off", true, {}},
        {"Auto mode", props.auto_mode ? "on" : "off", true, {}},
        {"Fast mode", props.fast_mode ? "on" : "off", true, {}},
    }});

    // Model section
    sections.push_back({"Model", {
        {"Current", props.model_name.empty() ? "default" : props.model_name, true, {}},
        {"Default", props.default_model.empty() ? "(not set)" : props.default_model, true, {}},
        {"MCP servers", std::to_string(props.mcp_server_count), true, {}},
    }});

    // API section
    sections.push_back({"API", {
        {"Endpoint", props.api_endpoint, true, {}},
        {"Status", props.api_connected ? "● connected" : "● disconnected",
         props.api_connected,
         props.api_connected ? std::nullopt
                             : std::optional<std::string>("Check your API key")},
    }});

    // Permissions section
    sections.push_back({"Permissions", {
        {"Default", props.permission_default, true, {}},
    }});

    // About section
    sections.push_back({"About", {
        {"Version", props.app_version, true, {}},
    }});

    return sections;
}

} // namespace detail

// ============================================================
// Main SettingsView renderer (pure Element version)
// ============================================================

/// Render the settings view (read-only panel version).
/// Faithful to TS Settings.tsx read-only summary.
[[nodiscard]] inline Element RenderSettingsView(
    const SettingsViewProps& props,
    const SettingsViewState& /*state*/,
    const Theme& theme)
{
    using namespace detail;

    auto sections = build_sections(props);

    // Build content
    Elements content_els;

    // Intro text
    content_els.push_back(text(" Quick overview of your current configuration.") | dim);
    content_els.push_back(text(""));

    // Render each section
    for (const auto& section : sections) {
        content_els.push_back(RenderSectionTitle(section.title, theme));
        content_els.push_back(text(""));
        for (const auto& row : section.rows) {
            content_els.push_back(RenderSettingRow(row, theme));
        }
        content_els.push_back(text(""));
    }

    // Footer
    content_els.push_back(RenderFooter(theme));

    auto content = vbox(std::move(content_els));

    // Wrap in DialogFrame
    dframe::DialogFrameProps frame_props;
    frame_props.title = "Settings";
    frame_props.subtitle = "Read-only overview";
    frame_props.style = dframe::FrameStyle::Permission;
    frame_props.content = content;
    frame_props.full_border = true;
    frame_props.inner_padding_x = 1;
    frame_props.inner_padding_y = 0;
    frame_props.pane_variant = dframe::PaneVariant::ModalMinimal;

    return dframe::DialogFrame(frame_props, theme);
}

// ============================================================
// Interactive SettingsView Component
// ============================================================

/// Create an interactive SettingsView component (with keyboard navigation).
/// Faithful to TS Settings panel behavior.
[[nodiscard]] inline Component MakeSettingsView(
    std::shared_ptr<SettingsViewState> state,
    SettingsViewProps props,
    const Theme& theme)
{
    using namespace detail;

    auto renderer = Renderer([state, props = std::move(props), &theme]() -> Element {
        return RenderSettingsView(props, *state, theme);
    });

    auto with_events = CatchEvent([state, props = std::move(props)](Event event) -> bool {
        // Close
        if (event == Event::Escape) {
            if (props.on_close) props.on_close();
            return true;
        }

        // Open full config
        if (event == Event::Character('c') || event == Event::Character('C')) {
            if (props.on_open_config) props.on_open_config();
            return true;
        }

        // Scroll
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            state->scroll_offset += 1;
            return true;
        }
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            state->scroll_offset = std::max(0, state->scroll_offset - 1);
            return true;
        }

        return false;
    });

    return renderer | with_events;
}

} // namespace cc::ui::dialogs::settings_view
