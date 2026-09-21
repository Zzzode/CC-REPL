module;
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
export module cc.services.tips.tip_scheduler;

export namespace cc::services::tips {

// Tip data for scheduler (redefined to avoid module dependency)
struct ScheduledTip {
    std::string id;
    std::string message;
    std::string category;
    int priority{0};
};

namespace detail {
    inline std::chrono::minutes tip_interval{30};
    inline std::chrono::steady_clock::time_point last_tip_time;
    inline bool initialized = false;
} // namespace detail

// Check if enough time has passed to show another tip
auto should_show_tip() -> bool {
    auto now = std::chrono::steady_clock::now();
    if (!detail::initialized) {
        detail::initialized = true;
        detail::last_tip_time = now;
        return false; // Don't show on first check
    }
    return (now - detail::last_tip_time) >= detail::tip_interval;
}

// Get the next tip to display (if conditions are met)
auto get_next_tip() -> std::optional<ScheduledTip> {
    if (!should_show_tip()) {
        return std::nullopt;
    }
    // No scheduler-owned tips are registered in this module.
    return std::nullopt;
}

// Configure minimum interval between tips
auto set_tip_frequency(std::chrono::minutes min_interval) -> void {
    detail::tip_interval = min_interval;
}

// Dismiss a tip so it won't be shown again
auto dismiss_tip(std::string_view tip_id) -> void {
    (void)tip_id;
    // Dismissal updates throttle timing; persistence is handled by callers.
    detail::last_tip_time = std::chrono::steady_clock::now();
}

} // namespace cc::services::tips
