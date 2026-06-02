module;
#include <chrono>
#include <string>
export module cc.services.analytics.config;

export namespace cc::services::analytics {

// Analytics system configuration
struct AnalyticsConfig {
    std::string endpoint;
    bool enabled{true};
    size_t batch_size{50};
    std::chrono::seconds flush_interval{30};
};

namespace detail {
    inline AnalyticsConfig current_config{
        .endpoint = "https://api.anthropic.com/v1/analytics",
        .enabled = true,
        .batch_size = 50,
        .flush_interval = std::chrono::seconds{30}
    };
} // namespace detail

// Get the current analytics configuration
auto get_analytics_config() -> AnalyticsConfig {
    return detail::current_config;
}

// Update the analytics configuration
auto update_analytics_config(AnalyticsConfig config) -> void {
    detail::current_config = std::move(config);
}

} // namespace cc::services::analytics
