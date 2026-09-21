/// @file tool_limits.cppm
/// @brief Tool result size limits.
/// Migrated from src/constants/toolLimits.ts
module;

#include <cstddef>

export module cc.constants.tool_limits;

export namespace cc::constants::tool_limits {

/// Default max size in characters for tool results before disk persistence
inline constexpr std::size_t DEFAULT_MAX_RESULT_SIZE_CHARS = 50'000;

/// Maximum size for tool results in tokens
inline constexpr std::size_t MAX_TOOL_RESULT_TOKENS = 100'000;

/// Bytes per token estimate
inline constexpr std::size_t BYTES_PER_TOKEN = 4;

/// Maximum size for tool results in bytes
inline constexpr std::size_t MAX_TOOL_RESULT_BYTES = MAX_TOOL_RESULT_TOKENS * BYTES_PER_TOKEN;

/// Max aggregate size in characters for tool_result blocks within a single user message
inline constexpr std::size_t MAX_TOOL_RESULTS_PER_MESSAGE_CHARS = 200'000;

/// Maximum character length for tool summary strings in compact views
inline constexpr std::size_t TOOL_SUMMARY_MAX_LENGTH = 50;

} // namespace cc::constants::tool_limits
