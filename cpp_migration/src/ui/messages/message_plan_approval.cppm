/// @file message_plan_approval.cppm
/// @brief Plan approval message rendering  (+ Approve / Modify / Reject buttons)
///
/// Mirrors PlanApprovalMessage.tsx (221 lines):
///   ┌ Proposed plan: Refactor billing module ─────── [PENDING] ┐
///   │ 1. Read the existing billing code                        │
///   │ 2. Extract interfaces (Invoice, Payment)                 │
///   │ 3. ⚠️ Replace DB schema (risky)                          │
///   │ 4. Add unit tests                                        │
///   ├─────────────────────────────────────────────────────────┤
///   │ [✓ Approve]  [✎ Modify]  [✕ Reject]                     │
///   └─────────────────────────────────────────────────────────┘
module;

#include <functional>
#include <string>
#include <vector>
#include <optional>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

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

/// ─── Stateless element renderer (kept for non-interactive contexts) ───
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

// ─── Interactive component: 3-button approval panel ────────────────────

class PlanApprovalComponent : public ComponentBase {
  public:
    using OnApproveFn = std::function<void()>;
    using OnModifyFn  = std::function<void(std::vector<PlanStep> proposed)>;
    using OnRejectFn  = std::function<void(std::string reason)>;

    explicit PlanApprovalComponent(PlanApprovalOptions opts,
                                   OnApproveFn on_approve = nullptr,
                                   OnModifyFn  on_modify  = nullptr,
                                   OnRejectFn  on_reject  = nullptr)
        : opts_(std::move(opts)),
          on_approve_(std::move(on_approve)),
          on_modify_(std::move(on_modify)),
          on_reject_(std::move(on_reject)) {}

    Element Render() override {
        // State badge
        auto state_badge = [&]() -> Element {
            switch (opts_.state) {
                case PlanApprovalState::Pending:
                    return hbox({ text("⏳ ") | color(Color::Yellow),
                                  text(" PENDING ") | color(Color::Yellow) | bold });
                case PlanApprovalState::Approved:
                    return hbox({ text("✓ ") | color(Color::Green),
                                  text(" APPROVED ") | color(Color::Green) | bold });
                case PlanApprovalState::Rejected:
                    return hbox({ text("✕ ") | color(Color::Red),
                                  text(" REJECTED ") | color(Color::Red) | bold });
                case PlanApprovalState::Modified:
                    return hbox({ text("✎ ") | color(Color::Cyan),
                                  text(" MODIFIED ") | color(Color::Cyan) | bold });
            }
            return text("");
        }();

        auto header = hbox({
            text("📋 ") | color(Color::Cyan),
            text(opts_.plan_title) | bold,
            filler(),
            std::move(state_badge),
        });

        // Bulleted step list
        Elements steps_rows;
        for (std::size_t i = 0; i < opts_.steps.size(); ++i) {
            const auto& step = opts_.steps[i];
            auto bullet = std::format("{}. ", i + 1);
            auto risky = step.is_risky ? text(" ⚠️") | color(Color::Yellow) : text("");
            auto tool_chip = step.tool_name
                ? text("  [" + *step.tool_name + "]") | dim | color(Color::GrayDark)
                : text("");
            Decorator style = step.is_risky ? color(Color::YellowLight) : nothing;
            steps_rows.push_back(hbox({
                text(bullet) | bold | color(Color::Cyan),
                text(step.description) | style,
                tool_chip,
                risky,
            }));
        }

        // Risk warning banner (if any steps are risky)
        Elements risk_rows;
        bool has_risk = false;
        for (const auto& s : opts_.steps) if (s.is_risky) { has_risk = true; break; }
        if (has_risk) {
            risk_rows.push_back(hbox({
                text("⚠️ ") | color(Color::Yellow),
                text("This plan contains risky operations. Please review carefully.")
                    | color(Color::Yellow) | dim,
            }));
        }

        // Rejection reason (after rejection)
        Elements rejection_rows;
        if (opts_.rejection_reason) {
            rejection_rows.push_back(text("Rejection reason: " + *opts_.rejection_reason)
                                         | color(Color::Red) | dim);
        }

        // Buttons (only in Pending state)
        Elements actions;
        if (opts_.state == PlanApprovalState::Pending) {
            auto approve_btn = button("  ✓ Approve  ", [this] {
                if (on_approve_) {
                    opts_.state = PlanApprovalState::Approved;
                    on_approve_();
                }
            }) | color(Color::Green) | bold;
            auto modify_btn = button("  ✎ Modify  ", [this] {
                if (on_modify_) {
                    opts_.state = PlanApprovalState::Modified;
                    on_modify_(opts_.steps);
                }
            }) | color(Color::Cyan);
            auto reject_btn = button("  ✕ Reject  ", [this] {
                if (on_reject_) {
                    opts_.state = PlanApprovalState::Rejected;
                    opts_.rejection_reason = opts_.rejection_reason
                        ? opts_.rejection_reason
                        : std::optional<std::string>("User rejected plan");
                    on_reject_(*opts_.rejection_reason);
                }
            }) | color(Color::Red);
            actions.push_back(hbox({
                std::move(approve_btn), text(" "),
                std::move(modify_btn), text(" "),
                std::move(reject_btn),
            }));
        }

        Elements all;
        all.push_back(std::move(header));
        all.push_back(separator());
        for (auto& r : steps_rows) all.push_back(std::move(r));
        if (!risk_rows.empty()) {
            all.push_back(separatorEmpty());
            for (auto& r : risk_rows) all.push_back(std::move(r));
        }
        if (!rejection_rows.empty()) {
            all.push_back(separatorEmpty());
            for (auto& r : rejection_rows) all.push_back(std::move(r));
        }
        if (!actions.empty()) {
            all.push_back(separator());
            all.push_back(hbox(std::move(actions)) | center);
        }

        return vbox(std::move(all)) | borderStyled(BorderStyle::ROUNDED, Color::Cyan);
    }

    bool OnEvent(Event event) override {
        if (opts_.state != PlanApprovalState::Pending)
            return ComponentBase::OnEvent(event);
        if (event == Event::Character('y') || event == Event::Character('Y')) {
            if (on_approve_) {
                opts_.state = PlanApprovalState::Approved;
                on_approve_();
                return true;
            }
        }
        if (event == Event::Character('m') || event == Event::Character('M')) {
            if (on_modify_) {
                opts_.state = PlanApprovalState::Modified;
                on_modify_(opts_.steps);
                return true;
            }
        }
        if (event == Event::Character('n') || event == Event::Escape) {
            if (on_reject_) {
                opts_.state = PlanApprovalState::Rejected;
                opts_.rejection_reason = opts_.rejection_reason
                    ? opts_.rejection_reason
                    : std::optional<std::string>("User rejected plan");
                on_reject_(*opts_.rejection_reason);
                return true;
            }
        }
        return ComponentBase::OnEvent(event);
    }

  private:
    PlanApprovalOptions opts_;
    OnApproveFn on_approve_;
    OnModifyFn  on_modify_;
    OnRejectFn  on_reject_;
};

[[nodiscard]] inline Component MakePlanApprovalMessage(
    PlanApprovalOptions opts,
    PlanApprovalComponent::OnApproveFn on_approve = nullptr,
    PlanApprovalComponent::OnModifyFn  on_modify  = nullptr,
    PlanApprovalComponent::OnRejectFn  on_reject  = nullptr)
{
    return Make<PlanApprovalComponent>(
        std::move(opts),
        std::move(on_approve), std::move(on_modify), std::move(on_reject));
}

} // namespace cc::ui::messages
