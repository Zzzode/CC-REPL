module;
#include <chrono>
#include <string>
export module cc.services.rate_limit.messages;

export namespace cc::services::rate_limit {

// Format a rate limit status message for the user
auto get_rate_limit_message(int remaining, int total, std::chrono::seconds reset_in)
    -> std::string {
    std::string msg = "Rate limit: " + std::to_string(remaining) + "/" +
                      std::to_string(total) + " requests remaining. ";
    auto minutes = std::chrono::duration_cast<std::chrono::minutes>(reset_in).count();
    if (minutes > 0) {
        msg += "Resets in " + std::to_string(minutes) + " minute(s).";
    } else {
        msg += "Resets in " + std::to_string(reset_in.count()) + " second(s).";
    }
    return msg;
}

// Format an overage warning message
auto format_overage_warning(double overage_pct) -> std::string {
    int pct = static_cast<int>(overage_pct * 100);
    return "Warning: You have exceeded your rate limit by " +
           std::to_string(pct) + "%. Requests may be throttled or rejected.";
}

// Get upgrade suggestion message
auto get_upgrade_suggestion() -> std::string {
    return "To increase your rate limits, consider upgrading your plan at "
           "https://console.anthropic.com/settings/billing";
}

} // namespace cc::services::rate_limit
