/// @file permission_plan_mode.cppm
/// @brief Plan mode permission UI - shows plan approval interface
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.permissions.permission_plan_mode;

import cc.types.types;

export namespace cc::ui::permissions {

using namespace ftxui;

/// Plan step entry for display
struct PlanStepDisplay {
    std::string description;
    bool is_completed{false};
    bool is_current{false};
};

/// Plan mode permission options
struct PlanModePermissionOptions {
    std::string plan_summary;
    std::vector<PlanStepDisplay> steps;
    std::optional<std::string> current_action;
    bool requires_explicit_approval{true};
};

/// Render plan step
[[nodiscard]] inline Element render_plan_step(const PlanStepDisplay& step) {
    auto prefix = step.is_completed ? "[x] " : step.is_current ? "[>] " : "[ ] ";
    auto style = step.is_completed ? dim : (step.is_current ? bold : nothing);
    return text(prefix + step.description) | style;
}

/// Render plan mode permission request
[[nodiscard]] inline Element render_plan_mode_permission(const PlanModePermissionOptions& opts) {
    std::vector<Element> elements;
    elements.push_back(text("Plan: " + opts.plan_summary) | bold);
    elements.push_back(separator());

    for (const auto& step : opts.steps) {
        elements.push_back(render_plan_step(step));
    }

    if (opts.current_action) {
        elements.push_back(separator());
        elements.push_back(text("Next: " + *opts.current_action) | color(Color::Cyan));
    }

    return vbox(elements);
}

/// Create plan mode approval component
[[nodiscard]] inline Component plan_mode_approval_dialog(
    const PlanModePermissionOptions& opts,
    std::function<void(bool)> on_decision) {
    return Renderer([opts, on_decision = std::move(on_decision)] {
        return vbox({
            text("Approve plan execution?") | bold,
            separator(),
            render_plan_mode_permission(opts),
            separator(),
            hbox({text("[A]pprove") | bold, text(" / "), text("[R]eject") | bold}),
        }) | border;
    });
}

} // namespace cc::ui::permissions
