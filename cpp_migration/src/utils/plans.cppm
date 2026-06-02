module;
#include <string>

export module cc.utils.plans;

export namespace cc::utils {

enum class PlanMode {
    Normal,
    Plan,
    Act
};

namespace detail {
    inline PlanMode& current_mode() {
        static PlanMode mode = PlanMode::Normal;
        return mode;
    }
} // namespace detail

PlanMode get_plan_mode() {
    return detail::current_mode();
}

void set_plan_mode(PlanMode mode) {
    detail::current_mode() = mode;
}

bool is_plan_mode_active() {
    return detail::current_mode() != PlanMode::Normal;
}

// Get instructions for the LLM based on current plan mode
std::string get_plan_mode_instructions() {
    switch (detail::current_mode()) {
        case PlanMode::Plan:
            return "You are in PLAN mode. Analyze the task and create a detailed plan. "
                   "Do NOT make any changes or execute commands. Only describe what you would do.";
        case PlanMode::Act:
            return "You are in ACT mode. Execute the plan that was previously created. "
                   "Make changes and run commands as needed to implement the plan.";
        case PlanMode::Normal:
            return "";
    }
    return "";
}

} // namespace cc::utils
