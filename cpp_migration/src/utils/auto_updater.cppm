/// @file auto_updater.cppm
/// @brief Auto-update checking, version comparison, update download/apply orchestration
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <chrono>
#include <cstdint>
#include <string_view>
#include <filesystem>

export module cc.utils.auto_updater;

export namespace cc::utils::auto_updater {

// ---------------------------------------------------------------------------
// Install status and result types
// ---------------------------------------------------------------------------

/// Status of a package installation attempt
enum class InstallStatus : std::uint8_t {
    Success,
    NoPermissions,
    InstallFailed,
    InProgress,
};

/// Result of an auto-update check/install
struct AutoUpdaterResult {
    std::optional<std::string> version;
    InstallStatus status{InstallStatus::Success};
    std::vector<std::string> notifications;
};

/// Release channel for updates
enum class ReleaseChannel : std::uint8_t {
    Latest,
    Stable,
};

/// Npm dist-tags (latest and stable versions)
struct NpmDistTags {
    std::optional<std::string> latest;
    std::optional<std::string> stable;
};

// ---------------------------------------------------------------------------
// Max version configuration (server-side kill switch)
// ---------------------------------------------------------------------------

/// Server-driven max version configuration for pausing auto-updates
struct MaxVersionConfig {
    std::optional<std::string> external;
    std::optional<std::string> ant;
    std::optional<std::string> external_message;
    std::optional<std::string> ant_message;
};

// ---------------------------------------------------------------------------
// Version assertion and queries
// ---------------------------------------------------------------------------

/// Checks if the current version meets the minimum required version.
/// Terminates the process with an error if the version is too old.
[[nodiscard]] auto assert_min_version(std::string_view current_version)
    -> std::expected<void, std::string>;

/// Returns the maximum allowed version for the current user type.
/// Used as a server-side kill switch to pause auto-updates during incidents.
[[nodiscard]] auto get_max_version(bool is_ant_user)
    -> std::expected<std::optional<std::string>, std::string>;

/// Returns the server-driven message explaining the known issue.
[[nodiscard]] auto get_max_version_message(bool is_ant_user)
    -> std::expected<std::optional<std::string>, std::string>;

/// Checks if a target version should be skipped due to user's minimumVersion setting.
[[nodiscard]] auto should_skip_version(
    std::string_view target_version,
    std::optional<std::string_view> minimum_version) -> bool;

// ---------------------------------------------------------------------------
// Version comparison (semver)
// ---------------------------------------------------------------------------

/// Compare two semver strings. Returns true if a >= b.
/// Build metadata (+SHA) is ignored per SemVer spec.
[[nodiscard]] auto semver_gte(std::string_view a, std::string_view b) -> bool;

/// Compare two semver strings. Returns true if a < b.
[[nodiscard]] auto semver_lt(std::string_view a, std::string_view b) -> bool;

// ---------------------------------------------------------------------------
// Lock file management
// ---------------------------------------------------------------------------

/// Get the path to the update lock file
[[nodiscard]] auto get_lock_file_path(const std::filesystem::path& config_home)
    -> std::filesystem::path;

// ---------------------------------------------------------------------------
// Permission checking
// ---------------------------------------------------------------------------

/// Result of checking global install permissions
struct PermissionCheckResult {
    bool has_permissions{false};
    std::optional<std::string> npm_prefix;
};

/// Check if the user has permissions for global npm/bun install
[[nodiscard]] auto check_global_install_permissions()
    -> std::expected<PermissionCheckResult, std::string>;

// ---------------------------------------------------------------------------
// Version fetching
// ---------------------------------------------------------------------------

/// Get the latest version from npm registry for a given channel
[[nodiscard]] auto get_latest_version(ReleaseChannel channel)
    -> std::expected<std::optional<std::string>, std::string>;

/// Get npm dist-tags (latest and stable versions) from the registry
[[nodiscard]] auto get_npm_dist_tags()
    -> std::expected<NpmDistTags, std::string>;

/// Get the latest version from GCS bucket for a given release channel
[[nodiscard]] auto get_latest_version_from_gcs(ReleaseChannel channel)
    -> std::expected<std::optional<std::string>, std::string>;

/// Get available versions from GCS bucket (for native installations)
[[nodiscard]] auto get_gcs_dist_tags()
    -> std::expected<NpmDistTags, std::string>;

/// Get version history from registry (limited to N newest versions)
[[nodiscard]] auto get_version_history(int limit)
    -> std::expected<std::vector<std::string>, std::string>;

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

/// Install/update the global package to a specific or latest version
[[nodiscard]] auto install_global_package(
    std::optional<std::string_view> specific_version = std::nullopt)
    -> std::expected<InstallStatus, std::string>;

/// Remove claude aliases from shell configuration files
[[nodiscard]] auto remove_claude_aliases_from_shell_configs()
    -> std::expected<void, std::string>;

} // namespace cc::utils::auto_updater
