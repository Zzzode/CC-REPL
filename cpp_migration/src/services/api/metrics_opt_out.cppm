module;
#include <cstdlib>
#include <string>
export module cc.services.api.metrics_opt_out;

export namespace cc::services::api {

// Privacy level for metrics collection
enum class PrivacyLevel { Full, Limited, Minimal };

namespace detail {
    inline bool metrics_opted_out = false;
} // namespace detail

// Check if user has opted out of metrics collection
auto is_metrics_opted_out() -> bool {
    // Check environment variables first
    if (std::getenv("DO_NOT_TRACK") || std::getenv("CLAUDE_NO_METRICS")) {
        return true;
    }
    return detail::metrics_opted_out;
}

// Set metrics opt-out preference
auto set_metrics_opt_out(bool opt_out) -> void {
    detail::metrics_opted_out = opt_out;
}

// Get current privacy level based on settings
auto get_privacy_level() -> PrivacyLevel {
    if (is_metrics_opted_out()) {
        return PrivacyLevel::Minimal;
    }
    // Check for limited mode
    if (std::getenv("CLAUDE_LIMITED_METRICS")) {
        return PrivacyLevel::Limited;
    }
    return PrivacyLevel::Full;
}

} // namespace cc::services::api
