/// @file permission_ask_user.cppm
/// @brief AskUser tool permission UI
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.permissions.permission_ask_user;

import cc.types.types;

export namespace cc::ui::permissions {

using namespace ftxui;

/// Ask user permission display options
struct AskUserPermissionOptions {
    std::string question;
    std::vector<std::string> options;
    bool allow_free_text{true};
    std::optional<std::string> context;
};

/// Render ask user permission request
[[nodiscard]] inline Element render_ask_user_permission(const AskUserPermissionOptions& opts) {
    std::vector<Element> elements;
    elements.push_back(text(opts.question) | bold);

    if (!opts.options.empty()) {
        elements.push_back(separator());
        for (std::size_t i = 0; i < opts.options.size(); ++i) {
            elements.push_back(text(std::format("  {}. {}", i + 1, opts.options[i])));
        }
    }

    if (opts.context) {
        elements.push_back(separator());
        elements.push_back(text("Context: " + *opts.context) | dim);
    }

    return vbox(elements);
}

/// Create ask user permission component
[[nodiscard]] inline Component ask_user_permission_dialog(
    const AskUserPermissionOptions& opts,
    std::function<void(bool)> on_decision) {
    return Renderer([opts, on_decision = std::move(on_decision)] {
        return vbox({
            text("Claude wants to ask you a question:") | bold,
            separator(),
            render_ask_user_permission(opts),
            separator(),
            hbox({text("[A]llow") | bold, text(" / "), text("[D]eny") | bold}),
        }) | border;
    });
}

} // namespace cc::ui::permissions
