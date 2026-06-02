module;

#include <string>
#include <string_view>
#include <expected>
#include <optional>
#include <chrono>
#include <cstdint>

export module cc.utils.cron_helpers;

export namespace cc::utils::cron_helpers {

struct JitterConfig {
    std::chrono::seconds base_interval;
    std::chrono::seconds max_jitter;
    uint32_t seed{0};
};

struct CronLock {
    std::string lock_id;
    std::string holder;
    std::chrono::system_clock::time_point acquired_at;
    std::chrono::seconds ttl;
};

inline std::chrono::seconds compute_jittered_interval(const JitterConfig& config) {
    return config.base_interval;
}

inline std::expected<CronLock, std::string> acquire_cron_lock(std::string_view task_id, std::chrono::seconds ttl) {
    return CronLock{std::string(task_id), "self", std::chrono::system_clock::now(), ttl};
}

inline std::expected<void, std::string> release_cron_lock([[maybe_unused]] std::string_view lock_id) {
    return {};
}

inline bool is_lock_held([[maybe_unused]] std::string_view task_id) {
    return false;
}

inline std::optional<CronLock> get_lock_info([[maybe_unused]] std::string_view task_id) {
    return std::nullopt;
}

} // namespace cc::utils::cron_helpers
