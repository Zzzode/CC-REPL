module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <chrono>
#include <expected>
#include <functional>
#include <mutex>
#include <unordered_set>

export module cc.hooks.remaining_notifs;

export namespace cc::hooks::notifs {

namespace detail {

struct DismissState {
    std::mutex mutex;
    std::unordered_set<std::string> dismissed_migrations;
    std::unordered_set<std::string> dismissed_plugin_updates;
    std::unordered_set<std::string> dismissed_teammate_shutdowns;
    std::unordered_set<std::string> dismissed_settings_errors;
    bool npm_deprecation_dismissed{false};
};

inline auto get_state() -> DismissState& {
    static DismissState state;
    return state;
}

} // namespace detail

// --------------------------------------------------------------------------
// 1. SubscriptionSwitch (useCanSwitchToExistingSubscription)
// --------------------------------------------------------------------------

struct SubscriptionSwitch {
    std::string current_plan;
    std::string available_plan;
    bool can_switch;
};

inline auto can_switch_to_existing_subscription() -> std::optional<SubscriptionSwitch> {
    return std::nullopt;
}

// --------------------------------------------------------------------------
// 2. McpConnectivity (useMcpConnectivityStatus)
// --------------------------------------------------------------------------

enum class McpServerStatus {
    Connected,
    Disconnected,
    Error,
    Starting
};

struct McpConnectivityInfo {
    std::string server_name;
    McpServerStatus status;
    std::optional<std::string> error;
};

inline auto get_mcp_connectivity_status() -> std::vector<McpConnectivityInfo> {
    return {};
}

inline auto has_mcp_connectivity_issues() -> bool {
    return false;
}

// --------------------------------------------------------------------------
// 3. ModelMigration (useModelMigrationNotifications)
// --------------------------------------------------------------------------

struct ModelMigrationNotif {
    std::string old_model;
    std::string new_model;
    std::string reason;
    bool auto_migrated;
};

inline auto get_pending_model_migrations() -> std::vector<ModelMigrationNotif> {
    return {};
}

inline auto acknowledge_migration(std::string_view old_model) -> void {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.dismissed_migrations.insert(std::string(old_model));
}

// --------------------------------------------------------------------------
// 4. NpmDeprecation (useNpmDeprecationNotification)
// --------------------------------------------------------------------------

struct NpmDeprecationInfo {
    std::string package_name;
    std::string deprecated_version;
    std::string recommended_version;
};

inline auto check_npm_deprecation() -> std::optional<NpmDeprecationInfo> {
    return std::nullopt;
}

inline auto dismiss_npm_deprecation() -> void {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.npm_deprecation_dismissed = true;
}

// --------------------------------------------------------------------------
// 5. PluginAutoupdate (usePluginAutoupdateNotification)
// --------------------------------------------------------------------------

struct PluginUpdateInfo {
    std::string plugin_id;
    std::string current_version;
    std::string available_version;
    bool auto_updated;
};

inline auto get_plugin_updates() -> std::vector<PluginUpdateInfo> {
    return {};
}

inline auto acknowledge_plugin_update(std::string_view plugin_id) -> void {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.dismissed_plugin_updates.insert(std::string(plugin_id));
}

// --------------------------------------------------------------------------
// 6. PluginInstallation (usePluginInstallationStatus)
// --------------------------------------------------------------------------

enum class PluginInstallStatus {
    Pending,
    Installing,
    Installed,
    Failed
};

struct PluginInstallInfo {
    std::string plugin_id;
    PluginInstallStatus status;
    std::optional<std::string> error;
};

inline auto get_plugin_installation_status() -> std::vector<PluginInstallInfo> {
    return {};
}

// --------------------------------------------------------------------------
// 7. SettingsErrors (useSettingsErrors)
// --------------------------------------------------------------------------

struct SettingsError {
    std::string setting_path;
    std::string error_message;
    std::string suggested_fix;
};

inline auto get_settings_errors() -> std::vector<SettingsError> {
    return {};
}

inline auto has_settings_errors() -> bool {
    return false;
}

inline auto dismiss_settings_error(std::string_view path) -> void {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.dismissed_settings_errors.insert(std::string(path));
}

// --------------------------------------------------------------------------
// 8. TeammateShutdown (useTeammateShutdownNotification)
// --------------------------------------------------------------------------

struct TeammateShutdownInfo {
    std::string teammate_id;
    std::string reason;
    std::chrono::system_clock::time_point shutdown_at;
};

inline auto get_teammate_shutdowns() -> std::vector<TeammateShutdownInfo> {
    return {};
}

inline auto acknowledge_teammate_shutdown(std::string_view teammate_id) -> void {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.dismissed_teammate_shutdowns.insert(std::string(teammate_id));
}

} // namespace cc::hooks::notifs
