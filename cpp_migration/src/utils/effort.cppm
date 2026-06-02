module;
#include <string>

export module cc.utils.effort;

export namespace cc::utils {

enum class EffortLevel {
    Low,
    Medium,
    High,
    Max
};

namespace detail {
    inline EffortLevel& current_effort() {
        static EffortLevel level = EffortLevel::High;
        return level;
    }
} // namespace detail

// Get current effort level
EffortLevel get_effort_level() {
    return detail::current_effort();
}

// Set effort level
void set_effort_level(EffortLevel level) {
    detail::current_effort() = level;
}

// Convert effort level to a budget multiplier
float effort_to_budget_multiplier(EffortLevel level) {
    switch (level) {
        case EffortLevel::Low:    return 0.5f;
        case EffortLevel::Medium: return 1.0f;
        case EffortLevel::High:   return 1.5f;
        case EffortLevel::Max:    return 2.0f;
    }
    return 1.0f;
}

} // namespace cc::utils
