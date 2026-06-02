/// @file settings_dialog.cppm
/// @brief Settings interface with tabbed navigation (Status, Config, Usage, Gates)
/// Migrated from Settings.tsx — tab-based settings panel with keybinding support.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <variant>
#include <algorithm>
#include <expected>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.settings_dialog;

export namespace cc::ui::settings_dialog {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Available tabs in the settings dialog
enum class SettingsTab {
    status,
    config,
    usage,
    gates,
};

/// Convert tab enum to display label
[[nodiscard]] inline std::string tab_label(SettingsTab tab) {
    switch (tab) {
        case SettingsTab::status: return "Status";
        case SettingsTab::config: return "Config";
        case SettingsTab::usage: return "Usage";
        case SettingsTab::gates: return "Gates";
    }
    return "?";
}

/// Setting value types
using SettingValue = std::variant<
    bool,
    int,
    std::string,
    std::vector<std::string>
>;

/// A single setting entry
struct SettingEntry {
    std::string key;
    std::string label;
    std::string description;
    SettingValue value;
    SettingValue default_value;
    std::optional<std::vector<std::string>> enum_options;
    std::optional<int> min_value;
    std::optional<int> max_value;
    bool is_modified = false;
};

/// Category grouping for settings
struct SettingCategory {
    std::string id;
    std::string label;
    std::string icon;
    std::vector<SettingEntry> settings;
};

/// Diagnostic entry for status tab
struct DiagnosticEntry {
    std::string label;
    std::string value;
    bool is_ok = true;
    std::optional<std::string> error_detail;
};

/// Command result display mode (matches TS CommandResultDisplay)
enum class CommandResultDisplay {
    normal,
    system,
    skip,
};

/// Props for the settings dialog
struct SettingsDialogProps {
    std::function<void(std::optional<std::string>, CommandResultDisplay)> on_close;
    SettingsTab default_tab = SettingsTab::status;
    std::vector<DiagnosticEntry> diagnostics;
    std::vector<SettingCategory> categories;
    int content_height = 25;
};

// ============================================================
// Rendering Helpers
// ============================================================

/// Render a setting value as display string
[[nodiscard]] inline std::string value_display(const SettingValue& val) {
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>) {
            return v ? "On" : "Off";
        } else if constexpr (std::is_same_v<T, int>) {
            return std::to_string(v);
        } else if constexpr (std::is_same_v<T, std::string>) {
            return v;
        } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            return std::format("[{} items]", v.size());
        }
        return "?";
    }, val);
}

/// Get color for a setting value
[[nodiscard]] inline Color value_color(const SettingEntry& entry) {
    if (entry.is_modified) return Color::Yellow;
    return std::visit([](const auto& v) -> Color {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>) {
            return v ? Color::Green : Color::GrayDark;
        }
        return Color::Cyan;
    }, entry.value);
}

// ============================================================
// Tab Rendering
// ============================================================

/// Render the status/diagnostics tab
[[nodiscard]] inline Element RenderStatusTab(
    const std::vector<DiagnosticEntry>& diagnostics) {

    if (diagnostics.empty()) {
        return text("  Loading diagnostics...") | dim;
    }

    Elements items;
    for (const auto& diag : diagnostics) {
        auto indicator = diag.is_ok
            ? text(" \u2713 ") | color(Color::Green)
            : text(" \u2717 ") | color(Color::Red);
        auto line = hbox({
            indicator,
            text(diag.label) | bold,
            text(": "),
            text(diag.value) | (diag.is_ok ? dim : color(Color::Red)),
        });
        items.push_back(line);
        if (diag.error_detail.has_value()) {
            items.push_back(hbox({
                text("     "),
                text(diag.error_detail.value()) | dim | color(Color::Red),
            }));
        }
    }
    return vbox(items);
}

/// Render a config settings list
[[nodiscard]] inline Element RenderConfigTab(
    const std::vector<SettingCategory>& categories,
    int selected_category, int selected_setting, bool editing) {

    if (categories.empty()) {
        return text("  No configuration available") | dim;
    }

    Elements elements;
    const auto& cat = categories[std::min(
        selected_category, static_cast<int>(categories.size()) - 1)];

    elements.push_back(hbox({
        text(" " + cat.icon + " " + cat.label + " ") | bold,
        filler(),
    }));
    elements.push_back(separator());

    for (int i = 0; i < static_cast<int>(cat.settings.size()); ++i) {
        const auto& entry = cat.settings[i];
        bool selected = (i == selected_setting);
        auto label_el = text(" " + entry.label)
            | (selected ? bold : nothing);
        auto val_str = value_display(entry.value);
        auto val_el = text(" " + val_str + " ")
                      | color(value_color(entry));
        if (editing && selected) val_el = val_el | inverted;

        auto mod = entry.is_modified
            ? text(" *") | color(Color::Yellow)
            : text("  ");

        auto line = hbox({mod, label_el | flex, val_el, text(" ")});
        if (selected) line = line | bgcolor(Color::RGB(30, 40, 55));
        elements.push_back(line);
    }

    return vbox(elements);
}

/// Render the usage tab
[[nodiscard]] inline Element RenderUsageTab() {
    return vbox({
        text("  API Usage") | bold,
        text(""),
        hbox({text("  Tokens used: "), text("—") | dim}),
        hbox({text("  Requests:    "), text("—") | dim}),
        text(""),
        text("  (usage stats populated at runtime)") | dim,
    });
}

/// Render the tab header bar
[[nodiscard]] inline Element RenderTabHeader(
    SettingsTab selected, bool hidden) {

    if (hidden) return text("");

    const std::vector<SettingsTab> tabs = {
        SettingsTab::status, SettingsTab::config,
        SettingsTab::usage, SettingsTab::gates,
    };

    Elements tab_elements;
    for (const auto& tab : tabs) {
        auto label = tab_label(tab);
        auto el = text(" " + label + " ");
        if (tab == selected) {
            el = el | bold | inverted;
        } else {
            el = el | dim;
        }
        tab_elements.push_back(el);
    }

    return hbox(tab_elements);
}

/// Render the full settings dialog
[[nodiscard]] inline Element RenderSettingsDialog(
    SettingsTab selected_tab,
    const SettingsDialogProps& props,
    int selected_setting,
    bool editing,
    bool tabs_hidden) {

    auto header = RenderTabHeader(selected_tab, tabs_hidden);

    Element content;
    switch (selected_tab) {
        case SettingsTab::status:
            content = RenderStatusTab(props.diagnostics);
            break;
        case SettingsTab::config:
            content = RenderConfigTab(props.categories, 0, selected_setting, editing);
            break;
        case SettingsTab::usage:
            content = RenderUsageTab();
            break;
        case SettingsTab::gates:
            content = text("  Feature gates (internal)") | dim;
            break;
    }

    auto body = vbox({
        header,
        separator(),
        content | flex | size(HEIGHT, LESS_THAN, props.content_height),
    });

    return window(
        text(" Settings ") | bold | color(Color::Magenta),
        body
    ) | color(Color::Magenta);
}

// ============================================================
// Interactive Component
// ============================================================

/// Create the settings dialog component
[[nodiscard]] inline Component SettingsDialog(SettingsDialogProps props) {
    struct State {
        SettingsDialogProps props;
        SettingsTab selected_tab;
        int selected_setting = 0;
        bool editing = false;
        bool tabs_hidden = false;
        bool config_owns_esc = false;
    };

    auto state = std::make_shared<State>();
    state->selected_tab = props.default_tab;
    state->props = std::move(props);

    return Renderer([state] {
        return RenderSettingsDialog(
            state->selected_tab,
            state->props,
            state->selected_setting,
            state->editing,
            state->tabs_hidden);
    }) | CatchEvent([state](Event event) -> bool {
        // Tab switching with left/right when not editing
        if (!state->editing && !state->tabs_hidden) {
            if (event == Event::ArrowLeft) {
                auto idx = static_cast<int>(state->selected_tab);
                if (idx > 0) {
                    state->selected_tab = static_cast<SettingsTab>(idx - 1);
                    state->selected_setting = 0;
                }
                return true;
            }
            if (event == Event::ArrowRight) {
                auto idx = static_cast<int>(state->selected_tab);
                if (idx < 3) {
                    state->selected_tab = static_cast<SettingsTab>(idx + 1);
                    state->selected_setting = 0;
                }
                return true;
            }
        }

        // Navigation within config tab
        if (state->selected_tab == SettingsTab::config) {
            if (event == Event::ArrowUp || event == Event::Character('k')) {
                state->selected_setting = std::max(0, state->selected_setting - 1);
                return true;
            }
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                state->selected_setting++;
                return true;
            }
            if (event == Event::Return) {
                state->editing = !state->editing;
                return true;
            }
        }

        // Escape handling
        if (event == Event::Escape) {
            if (state->editing) {
                state->editing = false;
                return true;
            }
            if (state->tabs_hidden) {
                return true;
            }
            if (state->props.on_close) {
                state->props.on_close("Status dialog dismissed",
                                      CommandResultDisplay::system);
            }
            return true;
        }

        return false;
    });
}

/// Create settings dialog with default tab (overload)
[[nodiscard]] inline Component SettingsDialog(
    std::function<void(std::optional<std::string>, CommandResultDisplay)> on_close,
    SettingsTab default_tab) {

    SettingsDialogProps props;
    props.on_close = std::move(on_close);
    props.default_tab = default_tab;
    return SettingsDialog(std::move(props));
}

} // namespace cc::ui::settings_dialog
