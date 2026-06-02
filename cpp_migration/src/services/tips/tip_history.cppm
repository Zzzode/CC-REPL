module;
#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
export module cc.services.tips.tip_history;

export namespace cc::services::tips {

namespace detail {
    inline std::mutex history_mutex;
    inline std::vector<std::string> shown_tips;
} // namespace detail

// Record that a tip was shown to the user
auto record_tip_shown(std::string_view tip_id) -> void {
    std::lock_guard lock(detail::history_mutex);
    if (std::ranges::find(detail::shown_tips, tip_id) == detail::shown_tips.end()) {
        detail::shown_tips.emplace_back(tip_id);
    }
}

// Check if a tip has been shown before
auto was_tip_shown(std::string_view tip_id) -> bool {
    std::lock_guard lock(detail::history_mutex);
    return std::ranges::find(detail::shown_tips, tip_id) != detail::shown_tips.end();
}

// Get all previously shown tip IDs
auto get_shown_tips() -> std::vector<std::string> {
    std::lock_guard lock(detail::history_mutex);
    return detail::shown_tips;
}

// Clear tip history
auto clear_tip_history() -> void {
    std::lock_guard lock(detail::history_mutex);
    detail::shown_tips.clear();
}

} // namespace cc::services::tips
