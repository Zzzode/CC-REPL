module;
#include <string>
#include <chrono>
#include <mutex>

export module cc.bridge.poll_config;

export namespace cc::bridge {

// Configuration for bridge polling behavior
struct PollConfig {
    std::chrono::seconds interval;
    std::chrono::seconds timeout;
    int max_retries;
    std::string endpoint;
};

namespace detail {
    // Global poll configuration state
    inline PollConfig& get_poll_config_ref() {
        static PollConfig config{
            .interval = std::chrono::seconds{5},
            .timeout = std::chrono::seconds{30},
            .max_retries = 3,
            .endpoint = ""
        };
        return config;
    }

    inline std::mutex& get_poll_mutex() {
        static std::mutex mutex;
        return mutex;
    }
}

// Get the current poll configuration
PollConfig get_poll_config() {
    std::lock_guard lock(detail::get_poll_mutex());
    return detail::get_poll_config_ref();
}

// Set the poll configuration
void set_poll_config(PollConfig config) {
    std::lock_guard lock(detail::get_poll_mutex());
    detail::get_poll_config_ref() = std::move(config);
}

// Determine if polling should be active based on current configuration
bool should_poll() {
    std::lock_guard lock(detail::get_poll_mutex());
    const auto& config = detail::get_poll_config_ref();

    // Polling requires a valid endpoint and positive interval
    return !config.endpoint.empty() && config.interval.count() > 0;
}

} // namespace cc::bridge
