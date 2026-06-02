module;
#include <atomic>
#include <cstdlib>
export module cc.services.analytics.sink_killswitch;

export namespace cc::services::analytics {

namespace detail {
    inline std::atomic<bool> analytics_disabled{false};
} // namespace detail

// Check if analytics is disabled (via env vars or explicit disable)
auto is_analytics_disabled() -> bool {
    // Check environment variables: DO_NOT_TRACK and CLAUDE_NO_ANALYTICS
    if (std::getenv("DO_NOT_TRACK")) {
        return true;
    }
    if (std::getenv("CLAUDE_NO_ANALYTICS")) {
        return true;
    }
    return detail::analytics_disabled.load();
}

// Programmatically disable analytics
auto disable_analytics() -> void {
    detail::analytics_disabled.store(true);
}

// Re-enable analytics (env vars still override)
auto enable_analytics() -> void {
    detail::analytics_disabled.store(false);
}

} // namespace cc::services::analytics
