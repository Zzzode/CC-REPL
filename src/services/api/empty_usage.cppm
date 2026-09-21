/// @file empty_usage.cppm
/// @brief Empty/zero usage constants and helpers
module;
#include <string>
#include <cstdint>
export module cc.services.api.empty_usage;
export namespace cc::services::api {
struct EmptyUsage { uint64_t input_tokens{0}; uint64_t output_tokens{0}; uint64_t cache_read{0}; uint64_t cache_write{0}; };
inline constexpr EmptyUsage kZeroUsage{};
[[nodiscard]] inline bool is_empty_usage(const EmptyUsage& u) { return u.input_tokens == 0 && u.output_tokens == 0; }
} // namespace
