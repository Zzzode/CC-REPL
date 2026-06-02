module;
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.services.compact_helpers;

export namespace cc::services::compact_helpers {

struct MicrocompactConfig {
    std::uint32_t target_token_count;
    double compression_ratio{0.5};
    bool preserve_tool_calls{true};
};

struct CompactWarning {
    std::string message;
    std::string severity;
    std::optional<std::chrono::system_clock::time_point> shown_at;
};

struct TimeBasedMCConfig {
    std::chrono::minutes interval;
    std::uint32_t max_messages_before_compact;
    bool enabled{true};
};

inline std::expected<std::string, std::string> run_api_microcompact(
    std::string_view content, const MicrocompactConfig& config) {
    (void)config;
    return std::string(content);
}

inline CompactWarning create_compact_warning(std::string_view reason) {
    return {std::string(reason), "info", std::chrono::system_clock::now()};
}

inline void register_compact_warning_hook() {
    // Register stderr warning output when compaction occurs
    std::fprintf(stderr, "[compact] Warning hook registered\n");
}

inline TimeBasedMCConfig get_time_based_config() {
    return {std::chrono::minutes{30}, 100, true};
}

inline bool should_compact_now(const TimeBasedMCConfig& config, std::uint32_t message_count) {
    return message_count > config.max_messages_before_compact;
}

} // namespace cc::services::compact_helpers
