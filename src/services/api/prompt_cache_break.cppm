/// @file prompt_cache_break.cppm
/// @brief Prompt cache break detection and handling
module;
#include <string>
#include <optional>
#include <cstdint>
export module cc.services.api.prompt_cache_break;
export namespace cc::services::api {
struct CacheBreakInfo { bool cache_broken{false}; uint64_t tokens_lost{0}; std::optional<std::string> reason; };
[[nodiscard]] inline CacheBreakInfo detect_cache_break(uint64_t expected_cached, uint64_t actual_cached) {
    if (actual_cached < expected_cached / 2) return {true, expected_cached - actual_cached, "significant cache miss"};
    return {};
}
} // namespace
