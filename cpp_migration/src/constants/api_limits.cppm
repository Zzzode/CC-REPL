/// @file api_limits.cppm
/// @brief Anthropic API server-side limits.
/// Migrated from src/constants/apiLimits.ts
module;

#include <cstddef>
#include <cstdint>

export module cc.constants.api_limits;

export namespace cc::constants::api_limits {

// Image limits
inline constexpr std::size_t API_IMAGE_MAX_BASE64_SIZE = 5 * 1024 * 1024;  // 5 MB
inline constexpr std::size_t IMAGE_TARGET_RAW_SIZE = (API_IMAGE_MAX_BASE64_SIZE * 3) / 4;  // 3.75 MB
inline constexpr int IMAGE_MAX_WIDTH = 2000;
inline constexpr int IMAGE_MAX_HEIGHT = 2000;

// PDF limits
inline constexpr std::size_t PDF_TARGET_RAW_SIZE = 20 * 1024 * 1024;  // 20 MB
inline constexpr int API_PDF_MAX_PAGES = 100;
inline constexpr std::size_t PDF_EXTRACT_SIZE_THRESHOLD = 3 * 1024 * 1024;  // 3 MB
inline constexpr std::size_t PDF_MAX_EXTRACT_SIZE = 100 * 1024 * 1024;  // 100 MB
inline constexpr int PDF_MAX_PAGES_PER_READ = 20;
inline constexpr int PDF_AT_MENTION_INLINE_THRESHOLD = 10;

// Media limits
inline constexpr int API_MAX_MEDIA_PER_REQUEST = 100;

} // namespace cc::constants::api_limits
