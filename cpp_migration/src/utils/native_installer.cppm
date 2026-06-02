// C++23 Native Installer Module
// Provides native package download, installation, PID lock management,
// and package manager detection functionality.
module;

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.native_installer;

export namespace cc::utils::native_installer {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Number of old versions to retain during cleanup
inline constexpr std::size_t VERSION_RETENTION_COUNT = 2;

/// Default stall timeout for downloads (60 seconds)
inline constexpr std::chrono::milliseconds DEFAULT_STALL_TIMEOUT{60000};

/// Maximum download retry count
inline constexpr int MAX_DOWNLOAD_RETRIES = 3;

// ---------------------------------------------------------------------------
// Platform & Package Manager Detection
// ---------------------------------------------------------------------------

/// Supported platform identifiers (os-arch, e.g. "darwin-arm64")
enum class Platform {
    DarwinArm64,
    DarwinX64,
    LinuxArm64,
    LinuxX64,
    LinuxArm64Musl,
    LinuxX64Musl,
    Win32Arm64,
    Win32X64,
    Unknown
};

/// Detected package manager that installed the current binary
enum class PackageManager {
    Homebrew,
    Winget,
    Pacman,
    Deb,
    Rpm,
    Apk,
    Mise,
    Asdf,
    Unknown
};

/// OS release info parsed from /etc/os-release
struct OsRelease {
    std::string id;
    std::vector<std::string> id_like;
};

/// Get the current platform string (e.g. "darwin-arm64", "linux-x64-musl")
[[nodiscard]] std::expected<std::string, std::string> get_platform();

/// Get the binary executable name for a given platform string
[[nodiscard]] inline std::string get_binary_name(std::string_view platform) {
    if (platform.starts_with("win32")) return "claude.exe";
    return "claude";
}

/// Parse /etc/os-release for distro identification
[[nodiscard]] std::expected<OsRelease, std::string> get_os_release();

/// Check if the current process was installed via a specific package manager
[[nodiscard]] bool detect_homebrew(std::string_view exec_path);
[[nodiscard]] bool detect_winget(std::string_view exec_path);
[[nodiscard]] bool detect_mise(std::string_view exec_path);
[[nodiscard]] bool detect_asdf(std::string_view exec_path);

/// Async package manager detectors (shell out to query package databases)
using AsyncDetectFn = std::function<std::expected<bool, std::string>(std::string_view exec_path)>;

/// Detect the package manager that installed the current binary
[[nodiscard]] PackageManager get_package_manager(std::string_view exec_path);

// ---------------------------------------------------------------------------
// Release Channel
// ---------------------------------------------------------------------------

/// Supported release channels
enum class ReleaseChannel {
    Latest,
    Stable
};

/// Parse a channel-or-version string into either a resolved version
/// or an error. Direct versions (e.g. "1.2.3") are returned as-is.
[[nodiscard]] std::expected<std::string, std::string> resolve_version(
    std::string_view channel_or_version);

// ---------------------------------------------------------------------------
// Download
// ---------------------------------------------------------------------------

/// Authentication config for binary repositories
struct AuthConfig {
    std::optional<std::string> username;
    std::optional<std::string> password;
    std::vector<std::pair<std::string, std::string>> headers;
};

/// Download source type
enum class DownloadSource {
    Npm,       // Artifactory npm package
    Binary     // Direct binary from GCS/bucket
};

/// Download result
struct DownloadResult {
    DownloadSource source;
    std::filesystem::path staging_path;
};

/// Get the latest version from an npm registry (Artifactory)
[[nodiscard]] std::expected<std::string, std::string> get_latest_version_from_artifactory(
    std::string_view tag = "latest");

/// Get the latest version from a binary repository (GCS bucket)
[[nodiscard]] std::expected<std::string, std::string> get_latest_version_from_binary_repo(
    ReleaseChannel channel,
    std::string_view base_url);

/// Get the latest version from a binary repository with auth
[[nodiscard]] std::expected<std::string, std::string> get_latest_version_from_binary_repo(
    ReleaseChannel channel,
    std::string_view base_url,
    const AuthConfig& auth);

/// Download a version from Artifactory npm registry
[[nodiscard]] std::expected<void, std::string> download_version_from_artifactory(
    std::string_view version,
    const std::filesystem::path& staging_path);

/// Download a version from a binary repository (GCS)
[[nodiscard]] std::expected<void, std::string> download_version_from_binary_repo(
    std::string_view version,
    const std::filesystem::path& staging_path,
    std::string_view base_url);

/// Download a version from a binary repository with auth config
[[nodiscard]] std::expected<void, std::string> download_version_from_binary_repo(
    std::string_view version,
    const std::filesystem::path& staging_path,
    std::string_view base_url,
    const AuthConfig& auth);

/// Download the specified version using the appropriate source.
/// Returns the download source type used.
[[nodiscard]] std::expected<DownloadSource, std::string> download_version(
    std::string_view version,
    const std::filesystem::path& staging_path);

// ---------------------------------------------------------------------------
// PID-Based Version Locking
// ---------------------------------------------------------------------------

/// Content stored in a version lock file
struct VersionLockContent {
    int pid = 0;
    std::string version;
    std::string exec_path;
    std::chrono::system_clock::time_point acquired_at;
};

/// Diagnostic information about a lock
struct LockInfo {
    std::string version;
    int pid = 0;
    bool is_process_running = false;
    std::string exec_path;
    std::chrono::system_clock::time_point acquired_at;
    std::filesystem::path lock_file_path;
};

/// Check if PID-based version locking is enabled
[[nodiscard]] bool is_pid_based_locking_enabled();

/// Check if a process with the given PID is currently running
[[nodiscard]] bool is_process_running(int pid);

/// Read and parse a lock file's content
[[nodiscard]] std::optional<VersionLockContent> read_lock_content(
    const std::filesystem::path& lock_file_path);

/// Check if a lock file represents an active lock (process still running)
[[nodiscard]] bool is_lock_active(const std::filesystem::path& lock_file_path);

/// Try to acquire a lock on a version file.
/// Returns a release function on success, or nullopt if lock is held.
using ReleaseFn = std::function<void()>;
[[nodiscard]] std::optional<ReleaseFn> try_acquire_lock(
    const std::filesystem::path& version_path,
    const std::filesystem::path& lock_file_path);

/// Acquire a lock held for the process lifetime (until exit/signal)
[[nodiscard]] bool acquire_process_lifetime_lock(
    const std::filesystem::path& version_path,
    const std::filesystem::path& lock_file_path);

/// Execute a callback while holding a lock.
/// Returns true if the callback executed, false if lock couldn't be acquired.
[[nodiscard]] std::expected<bool, std::string> with_lock(
    const std::filesystem::path& version_path,
    const std::filesystem::path& lock_file_path,
    std::function<std::expected<void, std::string>()> callback);

/// Get diagnostic info for all version locks in a directory
[[nodiscard]] std::vector<LockInfo> get_all_lock_info(
    const std::filesystem::path& locks_dir);

/// Clean up stale locks where the process is no longer running.
/// Returns the number of locks cleaned.
[[nodiscard]] int cleanup_stale_locks(const std::filesystem::path& locks_dir);

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

/// Setup message reported during installation checks
struct SetupMessage {
    std::string message;
    bool user_action_required = false;
    enum class Type { Path, Alias, Info, Error } type = Type::Info;
};

/// Result of an install attempt
struct InstallResult {
    std::optional<std::string> latest_version;
    bool was_updated = false;
    bool lock_failed = false;
    std::optional<int> lock_holder_pid;
};

/// Base directories used by the native installer
struct BaseDirectories {
    std::filesystem::path versions;
    std::filesystem::path staging;
    std::filesystem::path locks;
    std::filesystem::path executable;
};

/// Get the base directories for native installer operations
[[nodiscard]] BaseDirectories get_base_directories();

/// Check installation status and return any setup messages
[[nodiscard]] std::expected<std::vector<SetupMessage>, std::string> check_install(
    bool force = false);

/// Install or update to the latest version of the specified channel.
/// Single-flight: concurrent calls for the same channel share results.
[[nodiscard]] std::expected<InstallResult, std::string> install_latest(
    std::string_view channel_or_version,
    bool force_reinstall = false);

/// Lock the currently running version to prevent cleanup deletion
[[nodiscard]] std::expected<void, std::string> lock_current_version();

/// Clean up old versions beyond the retention count
[[nodiscard]] std::expected<void, std::string> cleanup_old_versions();

/// Remove the installed symlink from the executable directory.
/// Only removes native binary symlinks, not npm-managed ones.
[[nodiscard]] std::expected<void, std::string> remove_installed_symlink();

/// Clean up old Claude aliases from shell configuration files
[[nodiscard]] std::expected<std::vector<SetupMessage>, std::string> cleanup_shell_aliases();

/// Clean up npm global installations of Claude packages
struct NpmCleanupResult {
    int removed = 0;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};
[[nodiscard]] std::expected<NpmCleanupResult, std::string> cleanup_npm_installations();

// ---------------------------------------------------------------------------
// Utility Functions
// ---------------------------------------------------------------------------

/// Remove a directory only if it is empty (used for symlink target cleanup)
[[nodiscard]] std::expected<void, std::string> remove_directory_if_empty(
    const std::filesystem::path& path);

/// Check if a file looks like a valid Claude binary (exists, non-empty, executable)
[[nodiscard]] bool is_possible_claude_binary(const std::filesystem::path& path);

} // namespace cc::utils::native_installer
