/// @file managed_settings_security.cppm
/// @brief Managed settings security dialog — displays organization-managed
/// settings warnings with a list of managed categories and acknowledge button.
module;
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <algorithm>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.managed_settings_security;

export namespace cc::ui::dialogs {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// A single managed setting category entry
struct ManagedSettingCategory {
    std::string id;           // e.g. "permissions", "tools", "mcp"
    std::string label;        // Display name
    std::string description;  // Short description of what is managed
    bool is_locked = true;    // Whether the setting is locked (non-editable)
};

/// Props for the managed settings security dialog component
struct ManagedSettingsSecurityProps {
    std::function<void()> on_acknowledge;
    std::string organization_name;  // Name of the managing org (optional)
    std::vector<ManagedSettingCategory> categories;
    int dialog_width = 72;
};

// ============================================================
// Default Data
// ============================================================

/// Default managed setting categories (example set)
[[nodiscard]] inline std::vector<ManagedSettingCategory>
default_managed_categories() {
    return {
        {"permissions",   "Tool Permissions",
         "Allowed and denied tool configurations", true},
        {"tools",         "Tool Access",
         "Which tools are available in the session", true},
        {"mcp",           "MCP Servers",
         "Connected MCP server allowlists and configurations", true},
        {"sandbox",       "Sandbox Policy",
         "Sandbox mode and override rules", false},
        {"api",           "API Endpoints",
         "Allowed API endpoints and rate limits", true},
    };
}

// ============================================================
// Element Rendering
// ============================================================

/// Render the warning header with icon
[[nodiscard]] inline Element RenderWarningHeader(
    std::string_view organization_name) {

    auto org_text = organization_name.empty()
        ? std::string{"your organization"}
        : std::string{organization_name};

    return vbox({
        hbox({
            text(" ⚠️ ") | color(Color::Yellow) | bold,
            text("The following settings are managed by ") | bold,
            text(org_text) | bold | color(Color::Yellow),
            text(".") | bold,
        }),
        text(""),
        paragraph(
            "Some configuration options have been set by your organization "
            "and cannot be changed locally. This is to ensure compliance "
            "with your team's security policies.") | dim,
    });
}

/// Render a single managed setting category row
[[nodiscard]] inline Element RenderCategoryRow(
    const ManagedSettingCategory& cat, bool selected) {

    auto lock_icon = cat.is_locked
        ? text(" \U0001F512 ") | color(Color::Yellow)
        : text(" \U0001F513 ") | color(Color::Green);

    auto label_el = text(" " + cat.label)
        | (selected ? bold : nothing);
    auto desc_el = text(" — " + cat.description) | dim;

    auto row = hbox({lock_icon, label_el | flex, desc_el});
    if (selected) {
        row = row | bgcolor(Color::RGB(35, 40, 55));
    }
    return row;
}

/// Render the full list of managed categories
[[nodiscard]] inline Element RenderCategoryList(
    const std::vector<ManagedSettingCategory>& categories,
    int selected_index) {

    Elements items;
    items.push_back(text(" Managed Setting Categories:") | bold);
    items.push_back(separator());

    if (categories.empty()) {
        items.push_back(text("  No managed settings found.") | dim);
        return vbox(items);
    }

    for (int i = 0; i < static_cast<int>(categories.size()); ++i) {
        items.push_back(RenderCategoryRow(categories[i], i == selected_index));
    }

    items.push_back(text(""));
    items.push_back(hbox({
        text("  "),
        text(std::to_string(categories.size())),
        text(" categories managed · "),
        text("Arrow keys") | bold | dim,
        text(" to navigate") | dim,
    }));

    return vbox(items);
}

/// Render the acknowledge button bar
[[nodiscard]] inline Element RenderAcknowledgeButton(bool focused) {
    auto btn_label = text(" Acknowledge ");

    auto btn = focused
        ? btn_label | bold | inverted | color(Color::Yellow)
        : btn_label | color(Color::Yellow);

    return hbox({
        text("  "),
        btn | size(WIDTH, GREATER_THAN, 16),
        filler(),
        text("Enter") | bold | dim,
        text(" to acknowledge · ") | dim,
        text("Esc") | bold | dim,
        text(" to dismiss") | dim,
    });
}

/// Render the full managed settings security dialog
[[nodiscard]] inline Element RenderManagedSettingsSecurityDialog(
    const std::string& organization_name,
    const std::vector<ManagedSettingCategory>& categories,
    int selected_index,
    int width) {

    auto body = vbox({
        RenderWarningHeader(organization_name),
        text(""),
        RenderCategoryList(categories, selected_index) | flex,
        text(""),
        separator(),
        RenderAcknowledgeButton(true),
    });

    return window(
        text(" Security: Managed Settings ") | bold | color(Color::Yellow),
        body | size(WIDTH, LESS_THAN, width)
    ) | color(Color::Yellow);
}

// ============================================================
// Interactive Component
// ============================================================

/// Create the managed settings security dialog component
[[nodiscard]] inline Component ManagedSettingsSecurityDialog(
    ManagedSettingsSecurityProps props) {

    struct State {
        ManagedSettingsSecurityProps props;
        int selected_index = 0;
    };

    auto state = std::make_shared<State>();
    state->props = std::move(props);

    // Populate default categories if none provided
    if (state->props.categories.empty()) {
        state->props.categories = default_managed_categories();
    }

    return Renderer([state] {
        return RenderManagedSettingsSecurityDialog(
            state->props.organization_name,
            state->props.categories,
            state->selected_index,
            state->props.dialog_width);
    }) | CatchEvent([state](Event event) -> bool {
        auto cat_count = static_cast<int>(state->props.categories.size());

        // Up / 'k' to navigate list
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            state->selected_index =
                std::max(0, state->selected_index - 1);
            return true;
        }
        // Down / 'j' to navigate list
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            state->selected_index =
                std::min(cat_count - 1, state->selected_index + 1);
            return true;
        }

        // Enter to acknowledge
        if (event == Event::Return) {
            if (state->props.on_acknowledge) {
                state->props.on_acknowledge();
            }
            return true;
        }

        // Escape to dismiss (same as acknowledge)
        if (event == Event::Escape) {
            if (state->props.on_acknowledge) {
                state->props.on_acknowledge();
            }
            return true;
        }

        return false;
    });
}

/// Create managed settings security dialog with simple callback (overload)
[[nodiscard]] inline Component ManagedSettingsSecurityDialog(
    std::function<void()> on_acknowledge) {

    ManagedSettingsSecurityProps props;
    props.on_acknowledge = std::move(on_acknowledge);
    return ManagedSettingsSecurityDialog(std::move(props));
}

} // namespace cc::ui::dialogs
