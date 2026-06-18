// Plugin Manager Module
// Consolidates: installedPluginsManager, managedPlugins, pluginAutoupdate,
//               pluginBlocklist, pluginPolicy, pluginStartupCheck, reconciler, refresh
//
// Manages the lifecycle of installed plugins: installation state, auto-updates,
// policy enforcement, startup checks, marketplace reconciliation, and refresh.
module;

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.plugin_manager;

import cc.utils.plugin_identifier;
import cc.utils.plugin_loader;

export namespace cc::utils::plugin_manager {

// ─────────────────────────────────────────────────────────────────────────────
// Plugin Scope & Installation Types
// ─────────────────────────────────────────────────────────────────────────────

enum class PluginScope : unsigned char {
    User,
    Project,
    Local,
    Managed,
};

struct PluginInstallationEntry {
    PluginScope scope = PluginScope::User;
    std::filesystem::path install_path;
    std::optional<std::string> version;
    std::string installed_at;
    std::optional<std::string> last_updated;
    std::optional<std::string> git_commit_sha;
    std::optional<std::filesystem::path> project_path;
};

struct InstalledPluginsFile {
    int version = 2;
    std::map<std::string, std::vector<PluginInstallationEntry>> plugins;
};

struct InstalledPlugin {
    std::string version;
    std::string installed_at;
    std::optional<std::string> last_updated;
    std::filesystem::path install_path;
    std::optional<std::string> git_commit_sha;
};

// ─────────────────────────────────────────────────────────────────────────────
// Installed Plugins Manager
// ─────────────────────────────────────────────────────────────────────────────

/// Get path to installed_plugins.json
[[nodiscard]] std::filesystem::path get_installed_plugins_file_path();

/// Clear the installed plugins cache (forces reload from disk)
void clear_installed_plugins_cache();

/// Migrate to single plugin file format (V1→V2 consolidation)
void migrate_to_single_plugin_file();

/// Load installed plugins in V2 format
[[nodiscard]] InstalledPluginsFile load_installed_plugins_v2();

/// Load installed plugins directly from disk (bypasses cache)
[[nodiscard]] InstalledPluginsFile load_installed_plugins_from_disk();

/// Get the in-memory session-level snapshot of installed plugins
[[nodiscard]] const InstalledPluginsFile& get_in_memory_installed_plugins();

/// Add or update a plugin installation entry
void add_plugin_installation(
    std::string_view plugin_id,
    PluginScope scope,
    const std::filesystem::path& install_path,
    const PluginInstallationEntry& metadata,
    std::optional<std::filesystem::path> project_path = std::nullopt
);

/// Remove a plugin installation entry at a specific scope
void remove_plugin_installation(
    std::string_view plugin_id,
    PluginScope scope,
    std::optional<std::filesystem::path> project_path = std::nullopt
);

/// Add or update a plugin's installation metadata (high-level)
void add_installed_plugin(
    std::string_view plugin_id,
    const InstalledPlugin& metadata,
    PluginScope scope = PluginScope::User,
    std::optional<std::filesystem::path> project_path = std::nullopt
);

/// Remove a plugin from the installed plugins registry
[[nodiscard]] std::optional<InstalledPlugin> remove_installed_plugin(std::string_view plugin_id);

/// Delete a plugin's cache directory from disk
void delete_plugin_cache(const std::filesystem::path& install_path);

/// Check if a plugin is installed (relevant to current project)
[[nodiscard]] bool is_plugin_installed(std::string_view plugin_id);

/// Check if a plugin has a user or managed scope installation (globally installed)
[[nodiscard]] bool is_plugin_globally_installed(std::string_view plugin_id);

/// Update a plugin's install path on disk without modifying in-memory state
void update_installation_path_on_disk(
    std::string_view plugin_id,
    PluginScope scope,
    std::optional<std::filesystem::path> project_path,
    const std::filesystem::path& new_path,
    std::string_view new_version,
    std::optional<std::string_view> git_commit_sha = std::nullopt
);

/// Check if there are pending updates (disk differs from memory)
[[nodiscard]] bool has_pending_updates();

/// Get count of pending updates
[[nodiscard]] std::size_t get_pending_update_count();

struct PendingUpdate {
    std::string plugin_id;
    std::string scope;
    std::string old_version;
    std::string new_version;
};

/// Get details about pending updates
[[nodiscard]] std::vector<PendingUpdate> get_pending_updates_details();

/// Remove all plugin entries belonging to a specific marketplace
struct MarketplaceRemovalResult {
    std::vector<std::filesystem::path> orphaned_paths;
    std::vector<std::string> removed_plugin_ids;
};
[[nodiscard]] MarketplaceRemovalResult remove_all_plugins_for_marketplace(std::string_view marketplace_name);

/// Initialize the versioned plugins system (migration + session state)
std::expected<void, std::string> initialize_versioned_plugins();

/// Reset the in-memory session state (for testing)
void reset_in_memory_state();

// ─────────────────────────────────────────────────────────────────────────────
// Managed Plugins
// ─────────────────────────────────────────────────────────────────────────────

/// Get the set of plugin names managed by org policy
[[nodiscard]] std::set<std::string> get_managed_plugin_names();

/// Check if a plugin is managed by org policy
[[nodiscard]] bool is_managed_plugin(std::string_view plugin_id);

// ─────────────────────────────────────────────────────────────────────────────
// Plugin Policy (from pluginPolicy)
// ─────────────────────────────────────────────────────────────────────────────

/// Check if a plugin is force-disabled by org policy (managed-settings.json)
[[nodiscard]] bool is_plugin_blocked_by_policy(std::string_view plugin_id);

// ─────────────────────────────────────────────────────────────────────────────
// Plugin Blocklist
// ─────────────────────────────────────────────────────────────────────────────

struct BlocklistEntry {
    std::string plugin_id;
    std::string reason;
    std::optional<std::string> blocked_at;
};

/// Get the current plugin blocklist
[[nodiscard]] std::vector<BlocklistEntry> get_plugin_blocklist();

/// Check if a plugin is on the blocklist
[[nodiscard]] bool is_plugin_blocklisted(std::string_view plugin_id);

// ─────────────────────────────────────────────────────────────────────────────
// Plugin Auto-update (from pluginAutoupdate)
// ─────────────────────────────────────────────────────────────────────────────

enum class AutoUpdateResult : unsigned char {
    Updated,
    AlreadyUpToDate,
    Skipped,
    Failed,
};

struct AutoUpdateReport {
    std::string plugin_id;
    AutoUpdateResult result;
    std::optional<std::string> old_version;
    std::optional<std::string> new_version;
    std::optional<std::string> error_message;
};

/// Run auto-update check for a single plugin
[[nodiscard]] AutoUpdateReport auto_update_plugin(std::string_view plugin_id);

/// Run auto-update for all eligible plugins (background operation)
[[nodiscard]] std::vector<AutoUpdateReport> auto_update_all_plugins();

/// Check if a plugin is eligible for auto-update
[[nodiscard]] bool is_auto_update_eligible(std::string_view plugin_id);

// ─────────────────────────────────────────────────────────────────────────────
// Plugin Startup Check (from pluginStartupCheck)
// ─────────────────────────────────────────────────────────────────────────────

struct StartupCheckResult {
    bool all_healthy = true;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    std::vector<std::string> demoted_plugins;
};

/// Perform startup health checks on all installed plugins
[[nodiscard]] StartupCheckResult perform_plugin_startup_check();

// ─────────────────────────────────────────────────────────────────────────────
// Marketplace Reconciler (from reconciler)
// ─────────────────────────────────────────────────────────────────────────────

struct MarketplaceDiffEntry {
    std::string name;
    std::string declared_source;
    std::string materialized_source;
};

struct MarketplaceDiff {
    std::vector<std::string> missing;               // declared in settings, absent from known_marketplaces.json
    std::vector<MarketplaceDiffEntry> source_changed; // present but source differs
    std::vector<std::string> up_to_date;            // present and matching
};

/// Compare declared intent (settings) against materialized state (JSON)
[[nodiscard]] MarketplaceDiff diff_marketplaces();

/// Reconcile marketplaces: bundled diff + install (idempotent, additive)
[[nodiscard]] std::expected<MarketplaceDiff, std::string> reconcile_marketplaces();

// ─────────────────────────────────────────────────────────────────────────────
// Plugin Refresh (from refresh)
// ─────────────────────────────────────────────────────────────────────────────

struct RefreshResult {
    bool success = true;
    std::size_t plugins_loaded = 0;
    std::vector<std::string> errors;
};

/// Refresh all plugins (reload from sources, clear caches)
[[nodiscard]] RefreshResult refresh_plugins();

/// Refresh a single plugin by ID
[[nodiscard]] std::expected<void, std::string> refresh_plugin(std::string_view plugin_id);

// ── Stub implementations ─────────────────────────────────────────────────────
//
// These mirror src/utils/plugins/installedPluginsManager.ts isPluginInstalled /
// removeInstalledPlugin signatures, but the bodies are intentionally stubs
// because the TS implementations depend on subsystems that are NOT yet migrated
// to C++:
//
//   - loadInstalledPluginsV2() / load_installed_plugins_v2(): declared in this
//     header but never DEFINED — the V2 disk-format reader is missing.
//   - isInstallationRelevantToCurrentProject(): needs getOriginalCwd (cwd
//     bootstrap is not migrated).
//   - getSettings_DEPRECATED().enabledPlugins: the enabledPlugins settings
//     field is not migrated.
//   - plugin_marketplace.cppm also has no definition for its marketplace
//     queries.
//
// Porting these would mean porting the entire installed-plugins disk layer,
// the settings field, and the cwd bootstrap — not a quick stub fix. Until then
// these return conservative values (not installed / nothing removed) so callers
// fail safe.
inline bool is_plugin_installed(std::string_view) { return false; }

inline std::optional<InstalledPlugin> remove_installed_plugin(std::string_view) {
    return std::nullopt;
}

} // namespace cc::utils::plugin_manager
