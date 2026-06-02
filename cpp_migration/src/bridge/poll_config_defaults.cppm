module;
#include <chrono>

export module cc.bridge.poll_config_defaults;

import cc.bridge.poll_config;

export namespace cc::bridge {

// Default polling interval: 5 seconds between polls
inline constexpr auto DEFAULT_POLL_INTERVAL = std::chrono::seconds{5};

// Default polling timeout: 30 seconds per request
inline constexpr auto DEFAULT_POLL_TIMEOUT = std::chrono::seconds{30};

// Default maximum retry count before giving up
inline constexpr int DEFAULT_MAX_RETRIES = 3;

// Get a PollConfig with all default values
PollConfig get_default_poll_config() {
    return PollConfig{
        .interval = DEFAULT_POLL_INTERVAL,
        .timeout = DEFAULT_POLL_TIMEOUT,
        .max_retries = DEFAULT_MAX_RETRIES,
        .endpoint = ""
    };
}

} // namespace cc::bridge
