// Plugin Cache Module
// Consolidates: cacheUtils, zipCache, zipCacheAdapters, fetchTelemetry
//
// Manages plugin caching (both directory and ZIP formats), cache cleanup,
// orphaned version handling, zip cache extraction, and fetch telemetry.
module;

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.plugin_cache;

export namespace cc::utils::plugin_cache {

// ─────────────────────────────────────────────────────────────────────────────
// Cache Utilities (from cacheUtils)
// ─────────────────────────────────────────────────────────────────────────────

/// Clear all plugin-related memoization caches
void clear_all_plugin_caches();

/// Clear all caches (plugins + commands + agents + skills)
void clear_all_caches();

/// Mark a plugin version as orphaned (sets .orphaned_at timestamp file)
[[nodiscard]] std::expected<void, std::string> mark_plugin_version_orphaned(
    const std::filesystem::path& version_path
);

/// Clean up orphaned plugin versions older than retention period (7 days)
/// Runs as a background operation — does not block startup.
[[nodiscard]] std::expected<void, std::string> cleanup_orphaned_plugin_versions_in_background();

/// Retention period for orphaned plugin versions before cleanup
inline constexpr auto CLEANUP_AGE = std::chrono::hours{7 * 24};  // 7 days

/// Filename used to mark orphaned versions
inline constexpr std::string_view ORPHANED_AT_FILENAME = ".orphaned_at";

// ─────────────────────────────────────────────────────────────────────────────
// Zip Cache (from zipCache)
// ─────────────────────────────────────────────────────────────────────────────

/// Check if the plugin zip cache mode is enabled
[[nodiscard]] bool is_plugin_zip_cache_enabled();

/// Get the path to the zip cache directory (CLAUDE_CODE_PLUGIN_CACHE_DIR)
[[nodiscard]] std::optional<std::filesystem::path> get_zip_cache_dir();

/// Get the session-local plugin cache path for extracted ZIPs
[[nodiscard]] std::filesystem::path get_session_plugin_cache_path();

/// Convert a plugin directory to a ZIP archive in-place
/// Creates the ZIP at zip_path and removes the original directory.
[[nodiscard]] std::expected<void, std::string> convert_directory_to_zip_in_place(
    const std::filesystem::path& directory_path,
    const std::filesystem::path& zip_path
);

/// Extract a ZIP archive to a directory
[[nodiscard]] std::expected<std::filesystem::path, std::string> extract_zip_to_directory(
    const std::filesystem::path& zip_path,
    const std::filesystem::path& target_dir
);

/// Ensure a plugin ZIP is extracted to the session-local cache
/// Returns the path to the extracted directory (may be cached from prior extraction).
[[nodiscard]] std::expected<std::filesystem::path, std::string> ensure_zip_extracted(
    const std::filesystem::path& zip_path
);

/// Clean up the session-local plugin cache (called at exit)
void cleanup_session_plugin_cache();

// ─────────────────────────────────────────────────────────────────────────────
// Zip Cache Adapters (from zipCacheAdapters)
// ─────────────────────────────────────────────────────────────────────────────

/// Resolve a plugin install path, extracting from ZIP cache if necessary.
/// Transparent to callers — returns a directory path whether the plugin is
/// stored as a directory or a ZIP.
[[nodiscard]] std::expected<std::filesystem::path, std::string> resolve_install_path(
    const std::filesystem::path& raw_install_path
);

/// Check if a path points to a ZIP-cached plugin
[[nodiscard]] bool is_zip_cache_path(const std::filesystem::path& path);

/// Get the known marketplaces config path (respects zip cache override)
[[nodiscard]] std::filesystem::path get_known_marketplaces_config_path_for_zip_cache();

/// Get installed plugins file path (respects zip cache override)
[[nodiscard]] std::filesystem::path get_installed_plugins_file_path_for_zip_cache();

// ─────────────────────────────────────────────────────────────────────────────
// Fetch Telemetry (from fetchTelemetry)
// ─────────────────────────────────────────────────────────────────────────────

/// Classification of fetch/clone errors for telemetry
enum class FetchErrorClass : unsigned char {
    None,
    NetworkTimeout,
    DnsResolution,
    AuthFailure,
    NotFound,
    RateLimited,
    ServerError,
    SslError,
    Unknown,
};

/// Classify a fetch/clone error stderr into a telemetry category
[[nodiscard]] FetchErrorClass classify_fetch_error(std::string_view stderr_output);

/// Log a plugin fetch event for telemetry
/// @param operation - "plugin_clone", "marketplace_url", "marketplace_gcs", etc.
/// @param target - URL/repo being fetched
/// @param status - "success" or "failure"
/// @param duration_ms - elapsed time in milliseconds
/// @param error_class - classification of the error (only for failures)
void log_plugin_fetch(
    std::string_view operation,
    std::string_view target,
    std::string_view status,
    double duration_ms,
    FetchErrorClass error_class = FetchErrorClass::None
);

// ─────────────────────────────────────────────────────────────────────────────
// Cache Statistics
// ─────────────────────────────────────────────────────────────────────────────

struct CacheStats {
    std::size_t total_plugins = 0;
    std::size_t total_versions = 0;
    std::size_t orphaned_versions = 0;
    std::size_t zip_cached_plugins = 0;
    std::size_t total_size_bytes = 0;
};

/// Get cache statistics
[[nodiscard]] std::expected<CacheStats, std::string> get_cache_stats();

} // namespace cc::utils::plugin_cache
