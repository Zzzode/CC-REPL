module;
#include <atomic>
#include <mutex>
export module cc.services.api.usage;

export namespace cc::services::api {

// Token usage tracking data
struct UsageData {
    int input_tokens{0};
    int output_tokens{0};
    int cache_creation_tokens{0};
    int cache_read_tokens{0};
    double cost{0.0};
};

namespace detail {
    inline std::mutex usage_mutex;
    inline UsageData current_usage;
    inline UsageData session_usage;
} // namespace detail

// Get current request usage
auto get_current_usage() -> UsageData {
    std::lock_guard lock(detail::usage_mutex);
    return detail::current_usage;
}

// Get accumulated session usage
auto get_session_usage() -> UsageData {
    std::lock_guard lock(detail::usage_mutex);
    return detail::session_usage;
}

// Reset current usage counters
auto reset_usage() -> void {
    std::lock_guard lock(detail::usage_mutex);
    detail::current_usage = UsageData{};
}

} // namespace cc::services::api
