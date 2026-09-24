/// @file permission_computer_use.cppm
/// @brief Computer use permission UI for screen/mouse/keyboard control
module;

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.permissions.permission_computer_use;

import std;

import cc.types.types;
import cc.utils.json;

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

/// Map an Anthropic computer_20241022 action name to the UI enum.
[[nodiscard]] inline std::optional<ComputerUseAction> action_from_wire(
    std::string_view action) {
    if (action == "screenshot" || action == "cursor_position") return ComputerUseAction::Screenshot;
    if (action == "left_click" || action == "right_click" ||
        action == "middle_click" || action == "double_click" ||
        action == "triple_click" || action == "left_mouse_down" ||
        action == "left_mouse_up" || action == "click") {
        return ComputerUseAction::Click;
    }
    if (action == "type" || action == "key" || action == "hold_key" ||
        action == "press") {
        return ComputerUseAction::Type;
    }
    if (action == "scroll") return ComputerUseAction::Scroll;
    if (action == "left_click_drag" || action == "drag") return ComputerUseAction::DragDrop;
    if (action == "open_app" || action == "open_application") return ComputerUseAction::OpenApp;
    return std::nullopt;
}

/// Build the panel options from a raw computer tool input JSON object.
/// Returns nullopt when the input is not recognizably a computer action, so
/// callers can fall back to the generic panel instead of mislabeling.
[[nodiscard]] inline std::optional<ComputerUsePermissionOptions>
options_from_tool_input(std::string_view input_json) {
    auto parsed = cc::utils::json::parse(std::string(input_json));
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    const auto root = parsed->root();

    auto action_name = root.get_string("action");
    auto kind = action_from_wire(action_name);
    if (!kind) return std::nullopt;

    ComputerUsePermissionOptions opts;
    opts.action = *kind;
    if (auto app = root.get("app"); app.is_str() && app.as_str().size() > 0) {
        opts.target_app = std::string(app.as_str());
    }
    // Native coordinates arrive as {"coordinate":[x,y]}.
    if (const auto coord = root.get("coordinate");
        coord.is_arr() && coord.size() >= 2) {
        opts.coordinates = std::format("({}, {})",
            coord.at(0).as_int(), coord.at(1).as_int());
    } else if (root.get("x").is_num() && root.get("y").is_num()) {
        opts.coordinates = std::format("({}, {})",
            root.get("x").as_int(), root.get("y").as_int());
    }
    if (const auto text = root.get("text"); text.is_str()) {
        opts.text_to_type = std::string(text.as_str());
    } else if (const auto key = root.get("key"); key.is_str()) {
        opts.text_to_type = std::string(key.as_str());
    }
    return opts;
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
