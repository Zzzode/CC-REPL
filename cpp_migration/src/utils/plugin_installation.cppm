// Plugin Installation Module
// Consolidates: pluginInstallationHelpers, headlessPluginInstall, installCounts,
//               pluginDirectories, pluginOptionsStorage
//
// Provides shared utilities for plugin installation, directory management,
// headless install flows, and plugin-specific option storage.
module;

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module cc.utils.plugin_installation;

import cc.utils.plugin_identifier;
import cc.utils.plugin_loader;

export namespace cc::utils::plugin_installation {

// ─────────────────────────────────────────────────────────────────────────────
// Plugin Directories (from pluginDirectories)
// ─────────────────────────────────────────────────────────────────────────────

/// Get the full path to the plugins directory (~/.claude/plugins or override)
[[nodiscard]] std::filesystem::path get_plugins_directory();

/// Get seed directories for pre-populated plugin caches
[[nodiscard]] std::vector<std::filesystem::path> get_plugin_seed_dirs();

/// Get the plugin data directory for a specific plugin
[[nodiscard]] std::filesystem::path get_plugin_data_dir(std::string_view plugin_id);

/// Delete the plugin data directory
void delete_plugin_data_dir(std::string_view plugin_id);

/// Get the plugins directory name based on current mode (plugins vs cowork_plugins)
[[nodiscard]] std::string get_plugins_directory_name();

struct PluginDirectoryStats {
    std::size_t total_size_bytes = 0;
    std::size_t file_count = 0;
    std::size_t plugin_count = 0;
};

/// Get statistics about the plugins directory
[[nodiscard]] std::expected<PluginDirectoryStats, std::string> get_plugins_directory_stats();

// ─────────────────────────────────────────────────────────────────────────────
// Installation Helpers (from pluginInstallationHelpers)
// ─────────────────────────────────────────────────────────────────────────────

/// Validate that a resolved path stays within a base directory (prevent path traversal)
[[nodiscard]] std::expected<std::filesystem::path, std::string> validate_path_within_base(
    const std::filesystem::path& base_path,
    std::string_view relative_path
);

/// Get current ISO timestamp
[[nodiscard]] std::string get_current_timestamp();

struct PluginInstallationInfo {
    std::string plugin_id;
    std::filesystem::path install_path;
    std::optional<std::string> version;
};

/// Register a plugin installation without caching (for local plugins already on disk)
void register_plugin_installation(
    const PluginInstallationInfo& info,
    cc::utils::plugin_identifier::PluginScope scope = cc::utils::plugin_identifier::PluginScope::User,
    std::optional<std::filesystem::path> project_path = std::nullopt
);

/// Cache and register a plugin (download/copy + add to installed_plugins.json)
[[nodiscard]] std::expected<std::filesystem::path, std::string> cache_and_register_plugin(
    std::string_view plugin_id,
    const cc::utils::plugin_loader::PluginSource& source,
    cc::utils::plugin_identifier::PluginScope scope = cc::utils::plugin_identifier::PluginScope::User,
    std::optional<std::filesystem::path> project_path = std::nullopt,
    std::optional<std::filesystem::path> local_source_path = std::nullopt
);

/// Parse a plugin ID into name and marketplace components
struct ParsedPluginId {
    std::string name;
    std::string marketplace;
};
[[nodiscard]] std::optional<ParsedPluginId> parse_plugin_id(std::string_view plugin_id);

// ─────────────────────────────────────────────────────────────────────────────
// Install Core Result Types
// ─────────────────────────────────────────────────────────────────────────────

enum class InstallFailureReason : unsigned char {
    LocalSourceNoLocation,
    SettingsWriteFailed,
    ResolutionFailed,
    BlockedByPolicy,
    DependencyBlockedByPolicy,
};

struct InstallCoreSuccess {
    std::vector<std::string> closure;
    std::string dep_note;
};

struct InstallCoreFailure {
    InstallFailureReason reason;
    std::string plugin_name;
    std::optional<std::string> message;
    std::optional<std::string> blocked_dependency;
};

using InstallCoreResult = std::expected<InstallCoreSuccess, InstallCoreFailure>;

/// Format a resolution failure into a user-facing message
[[nodiscard]] std::string format_resolution_error(InstallFailureReason reason, std::string_view detail);

/// Core plugin install logic (shared by CLI and interactive UI paths)
[[nodiscard]] InstallCoreResult install_resolved_plugin(
    std::string_view plugin_id,
    std::string_view scope,
    std::optional<std::filesystem::path> marketplace_install_location = std::nullopt
);

// ─────────────────────────────────────────────────────────────────────────────
// Interactive Install Result
// ─────────────────────────────────────────────────────────────────────────────

struct InstallPluginResult {
    bool success = false;
    std::string message;
};

struct InstallPluginParams {
    std::string plugin_id;
    std::string marketplace_name;
    std::string scope = "user";  // "user" | "project" | "local"
    std::string trigger = "user"; // "hint" | "user"
};

/// Install a single plugin from a marketplace (interactive UI wrapper)
[[nodiscard]] InstallPluginResult install_plugin_from_marketplace(const InstallPluginParams& params);

// ─────────────────────────────────────────────────────────────────────────────
// Headless Plugin Install (from headlessPluginInstall)
// ─────────────────────────────────────────────────────────────────────────────

struct HeadlessInstallOptions {
    std::vector<std::string> plugin_ids;
    bool force = false;
    bool quiet = false;
};

struct HeadlessInstallResult {
    bool success = true;
    std::size_t installed_count = 0;
    std::size_t skipped_count = 0;
    std::vector<std::string> errors;
};

/// Install plugins in headless mode (non-interactive, for CI/automation)
[[nodiscard]] HeadlessInstallResult headless_plugin_install(const HeadlessInstallOptions& options);

// ─────────────────────────────────────────────────────────────────────────────
// Install Counts (from installCounts)
// ─────────────────────────────────────────────────────────────────────────────

/// Record a plugin install event for counting
void record_install_event(std::string_view plugin_id, std::string_view marketplace_name);

/// Get the install count for a specific plugin
[[nodiscard]] std::size_t get_install_count(std::string_view plugin_id);

/// Get top installed plugins (sorted by count)
[[nodiscard]] std::vector<std::pair<std::string, std::size_t>> get_top_installed_plugins(std::size_t limit = 10);

// ─────────────────────────────────────────────────────────────────────────────
// Plugin Options Storage (from pluginOptionsStorage)
// ─────────────────────────────────────────────────────────────────────────────

/// Get the storage ID for a plugin (used as filesystem-safe key)
[[nodiscard]] std::string get_plugin_storage_id(std::string_view plugin_id);

/// Load plugin options from storage
[[nodiscard]] std::map<std::string, std::string> load_plugin_options(std::string_view plugin_id);

/// Save plugin options to storage
void save_plugin_options(std::string_view plugin_id, const std::map<std::string, std::string>& options);

/// Delete plugin options from storage
void delete_plugin_options(std::string_view plugin_id);

/// Clear the plugin options cache
void clear_plugin_options_cache();

/// Substitute plugin variables in a string (e.g., ${plugin_data_dir})
[[nodiscard]] std::string substitute_plugin_variables(
    std::string_view input,
    std::string_view plugin_id,
    const std::filesystem::path& plugin_path
);

/// Substitute user config variables in a string
[[nodiscard]] std::string substitute_user_config_variables(
    std::string_view input,
    const std::map<std::string, std::string>& user_config
);

} // namespace cc::utils::plugin_installation
