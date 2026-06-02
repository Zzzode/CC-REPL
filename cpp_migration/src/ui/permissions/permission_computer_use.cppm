/// @file permission_computer_use.cppm
/// @brief Computer use permission UI for screen/mouse/keyboard control
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <format>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.permissions.permission_computer_use;

import cc.types.types;

export namespace cc::ui::permissions {

using namespace ftxui;

/// Computer use action types
enum class ComputerUseAction {
    Screenshot,
    Click,
    Type,
    Scroll,
    DragDrop,
    OpenApp,
};

/// Computer use permission options
struct ComputerUsePermissionOptions {
    ComputerUseAction action{ComputerUseAction::Screenshot};
    std::optional<std::string> target_app;
    std::optional<std::string> coordinates;
    std::optional<std::string> text_to_type;
    bool first_use_in_session{false};
};

/// Get action description
[[nodiscard]] inline std::string_view action_description(ComputerUseAction action) {
    switch (action) {
        case ComputerUseAction::Screenshot: return "Take a screenshot";
        case ComputerUseAction::Click: return "Click on screen";
        case ComputerUseAction::Type: return "Type text";
        case ComputerUseAction::Scroll: return "Scroll";
        case ComputerUseAction::DragDrop: return "Drag and drop";
        case ComputerUseAction::OpenApp: return "Open application";
    }
    return "Unknown action";
}

/// Render computer use permission request
[[nodiscard]] inline Element render_computer_use_permission(const ComputerUsePermissionOptions& opts) {
    std::vector<Element> elements;

    elements.push_back(hbox({
        text("COMPUTER USE") | bold | color(Color::Magenta),
        text(": "),
        text(std::string(action_description(opts.action))),
    }));

    if (opts.target_app) {
        elements.push_back(text(std::format("  App: {}", *opts.target_app)) | dim);
    }
    if (opts.coordinates) {
        elements.push_back(text(std::format("  Position: {}", *opts.coordinates)) | dim);
    }
    if (opts.text_to_type) {
        auto preview = opts.text_to_type->substr(0, 50);
        elements.push_back(text(std::format("  Text: \"{}\"", preview)) | dim);
    }

    if (opts.first_use_in_session) {
        elements.push_back(separator());
        elements.push_back(text("  First computer use in this session") | color(Color::Yellow));
    }

    return vbox(elements);
}

/// Create computer use permission component
[[nodiscard]] inline Component computer_use_permission_dialog(
    const ComputerUsePermissionOptions& opts,
    std::function<void(bool)> on_decision) {
    return Renderer([opts, on_decision = std::move(on_decision)] {
        return vbox({
            text("Allow computer use?") | bold,
            separator(),
            render_computer_use_permission(opts),
            separator(),
            hbox({text("[Y]es") | bold, text(" / "), text("[N]o") | bold}),
        }) | border;
    });
}

} // namespace cc::ui::permissions
