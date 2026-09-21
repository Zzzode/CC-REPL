/// @file time_based_config.cppm
/// @brief Time-based compaction configuration
module;
#include <string>
#include <chrono>
#include <cstdint>
export module cc.services.compact.time_based_config;
export namespace cc::services::compact {
struct TimeBasedConfig { std::chrono::minutes idle_compact_after{30}; std::chrono::minutes force_compact_after{120}; uint64_t min_tokens_to_compact{50000}; };
inline constexpr TimeBasedConfig kDefaultTimeConfig{};
[[nodiscard]] inline bool should_compact_by_time(std::chrono::minutes idle_time, const TimeBasedConfig& config = kDefaultTimeConfig) {
    return idle_time >= config.idle_compact_after;
}
} // namespace
