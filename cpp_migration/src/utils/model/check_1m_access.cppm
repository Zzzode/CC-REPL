module;
#include <cstddef>
#include <cstdlib>
#include <string_view>

export module cc.utils.model.check_1m_access;

export namespace cc::utils {

// Check if user has access to 1M token context window
bool has_1m_context_access() {
    const char* plan = std::getenv("CLAUDE_PLAN");
    if (!plan) return false;

    std::string_view plan_sv(plan);
    // Pro and Team plans have 1M context access
    return plan_sv == "pro" || plan_sv == "team" || plan_sv == "enterprise";
}

// Return the maximum context window size based on current plan
std::size_t get_max_context_for_plan() {
    if (has_1m_context_access()) {
        return 1'000'000; // 1M tokens
    }
    return 200'000; // 200K tokens (default)
}

// Check if user is eligible to upgrade to 1M context
bool check_context_upgrade_eligibility() {
    const char* plan = std::getenv("CLAUDE_PLAN");
    if (!plan) return true; // No plan info means might be eligible

    std::string_view plan_sv(plan);
    // Free tier users can upgrade
    return plan_sv == "free" || plan_sv == "basic";
}

} // namespace cc::utils
