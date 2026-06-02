/// @file message_plan_approval.cppm
/// @brief Plan approval message rendering
module;

#include <string>
#include <vector>
#include <optional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.messages.message_plan_approval;

import cc.types.types;

export namespace cc::ui::messages {

using namespace ftxui;

/// Plan approval state
enum class PlanApprovalState {
    Pending,
    Approved,
    Rejected,
    Modified,
};

/// Plan step for display
struct PlanStep {
    std::string description;
    std::optional<std::string> tool_name;
    bool is_risky{false};
};

/// Plan approval message options
struct PlanApprovalOptions {
    std::string plan_title;
    std::vector<PlanStep> steps;
    PlanApprovalState state{PlanApprovalState::Pending};
    std::optional<std::string> rejection_reason;
};

/// Render plan approval message
[[nodiscard]] inline Element render_plan_approval(const PlanApprovalOptions& opts) {
    std::vector<Element> elements;

    auto state_indicator = [&]() -> Element {
        switch (opts.state) {
            case PlanApprovalState::Pending: return text("[PENDING]") | color(Color::Yellow);
            case PlanApprovalState::Approved: return text("[APPROVED]") | color(Color::Green);
            case PlanApprovalState::Rejected: return text("[REJECTED]") | color(Color::Red);
            case PlanApprovalState::Modified: return text("[MODIFIED]") | color(Color::Cyan);
        }
        return text("");
    }();

    elements.push_back(hbox({text(opts.plan_title) | bold, text(" "), state_indicator}));
    elements.push_back(separator());

    for (std::size_t i = 0; i < opts.steps.size(); ++i) {
        const auto& step = opts.steps[i];
        auto prefix = std::format("{}. ", i + 1);
        auto style = step.is_risky ? color(Color::Yellow) : nothing;
        elements.push_back(text(prefix + step.description) | style);
    }

    if (opts.rejection_reason) {
        elements.push_back(separator());
        elements.push_back(text("Reason: " + *opts.rejection_reason) | color(Color::Red));
    }

    return vbox(elements) | border;
}

} // namespace cc::ui::messages
