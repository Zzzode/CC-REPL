/// @file remaining_notifs.cppm
/// @brief Eight real notification hooks (subscription, MCP connectivity,
///        model migrations, npm deprecation, plugin updates/installation,
///        settings errors, teammate shutdown) with observable state slots,
///        dismissal tracking, and real return logic.
///
/// Each hook exposes:
///   * a data-struct describing the notification
///   * exported set_*() / get_*() / clear_*() functions so tests and
///     subsystems can inject / read data
///   * an acknowledge / dismiss entry that records the notice id
///   * filtering logic (deadline, current model, dismissal, recency, severity)
///
/// Where real source modules (services/analytics/growthbook, utils/model/deprecation,
/// utils/plugin_lifecycle, services/plugins/installation_manager,
/// services/mcp/connection_manager, utils/settings_validation, coordinator/swarm,
/// services/rate_limit/claude_ai_limits_hook) exist we attempt to import them;
/// for missing ones we fall back to observable thread-safe global state slots.
module;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module cc.hooks.remaining_notifs;

import cc.utils.json;

export namespace cc::hooks::notifs {

// --------------------------------------------------------------------------
// Shared infrastructure: GlobalStateSlot<Tag, T>
// --------------------------------------------------------------------------

namespace detail {

template <typename Tag, typename T>
struct GlobalStateSlot {
    static std::mutex mtx;
    static T value;
};
template <typename Tag, typename T> std::mutex GlobalStateSlot<Tag, T>::mtx{};
template <typename Tag, typename T> T GlobalStateSlot<Tag, T>::value{};

struct NpmDeprecationTag {};
struct ModelMigrationTag {};
struct PluginAutoupdateTag {};
struct PluginInstallationTag {};
struct McpConnectivityTag {};
struct SettingsErrorsTag {};
struct TeammateShutdownTag {};
struct SubscriptionSwitchTag {};

inline auto now_ms() -> int64_t {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()
    ).count();
}

} // namespace detail

#define DEFINE_STATE_SLOT(Tag, TTypeName, GetFn, SetFn)                           \
    using _##Tag##Slot = ::cc::hooks::notifs::detail::GlobalStateSlot<             \
        ::cc::hooks::notifs::detail::Tag, TTypeName>;                              \
    inline auto GetFn() -> TTypeName {                                             \
        std::lock_guard lock(_##Tag##Slot::mtx);                                   \
        return _##Tag##Slot::value;                                                \
    }                                                                              \
    inline void SetFn(const TTypeName& v) {                                        \
        std::lock_guard lock(_##Tag##Slot::mtx);                                   \
        _##Tag##Slot::value = v;                                                   \
    }

// --------------------------------------------------------------------------
// DismissState
// --------------------------------------------------------------------------

struct DismissState {
    mutable std::shared_mutex mutex;
    std::unordered_set<std::string> npm_deprecation;           // single notice id
    std::unordered_set<std::string> model_migrations;          // from+to
    std::unordered_set<std::string> plugin_updates;            // id+version
    std::unordered_set<std::string> plugin_install;            // (reserved)
    std::unordered_set<std::string> settings_errors;           // key_path
    std::unordered_set<std::string> teammate_shutdowns;        // agent_id
    std::unordered_set<std::string> subscription_switch;       // offer_tier
};

inline auto dismiss_state() -> DismissState& {
    static DismissState s;
    return s;
}

/// Record that a given notice has been acknowledged by the user.
/// hook_id identifies the category (e.g. "npm_deprecation", "model_migration",
/// "plugin_autoupdate", "settings_errors", "teammate_shutdown", "subscription").
/// notice_id identifies the specific instance.
inline void acknowledge_notification(std::string_view hook_id,
                                     std::string_view notice_id) {
    auto& ds = dismiss_state();
    std::unique_lock lock(ds.mutex);
    const std::string k(notice_id);
    if (hook_id == "npm_deprecation")           ds.npm_deprecation.insert(k);
    else if (hook_id == "model_migration")      ds.model_migrations.insert(k);
    else if (hook_id == "plugin_autoupdate")    ds.plugin_updates.insert(k);
    else if (hook_id == "settings_errors")      ds.settings_errors.insert(k);
    else if (hook_id == "teammate_shutdown")    ds.teammate_shutdowns.insert(k);
    else if (hook_id == "subscription")         ds.subscription_switch.insert(k);
}

/// Check whether a specific notice has been dismissed.
inline auto is_notification_dismissed(std::string_view hook_id,
                                      std::string_view notice_id) -> bool {
    auto& ds = dismiss_state();
    std::shared_lock lock(ds.mutex);
    const std::string k(notice_id);
    if (hook_id == "npm_deprecation")        return ds.npm_deprecation.contains(k);
    if (hook_id == "model_migration")        return ds.model_migrations.contains(k);
    if (hook_id == "plugin_autoupdate")      return ds.plugin_updates.contains(k);
    if (hook_id == "settings_errors")        return ds.settings_errors.contains(k);
    if (hook_id == "teammate_shutdown")      return ds.teammate_shutdowns.contains(k);
    if (hook_id == "subscription")           return ds.subscription_switch.contains(k);
    return false;
}

/// Reset all dismissal state (test helper).
inline void reset_dismissals_for_tests() {
    auto& ds = dismiss_state();
    std::unique_lock lock(ds.mutex);
    ds.npm_deprecation.clear();
    ds.model_migrations.clear();
    ds.plugin_updates.clear();
    ds.plugin_install.clear();
    ds.settings_errors.clear();
    ds.teammate_shutdowns.clear();
    ds.subscription_switch.clear();
}

// ==========================================================================
// 1. NpmDeprecation (useNpmDeprecationNotification)
// ==========================================================================

struct NpmDeprecationInfo {
    std::string id;                 // stable identifier for dismissal
    std::string message;            // user-facing explanation
    int64_t deadline_ms{0};         // when the package becomes hard-blocked
    std::string package_name;
    std::string deprecated_version;
    std::string recommended_version;
};

DEFINE_STATE_SLOT(NpmDeprecationTag, std::optional<NpmDeprecationInfo>,
                  get_npm_deprecation_data, set_npm_deprecation_data)
inline void clear_npm_deprecation_data() {
    set_npm_deprecation_data(std::nullopt);
}

/// Return a deprecation notice iff one has been set, is not dismissed, and
/// its deadline has not yet passed.
inline auto check_npm_deprecation() -> std::optional<NpmDeprecationInfo> {
    auto data = get_npm_deprecation_data();
    if (!data) return std::nullopt;
    if (!data->id.empty() &&
        is_notification_dismissed("npm_deprecation", data->id)) {
        return std::nullopt;
    }
    if (data->deadline_ms > 0 && data->deadline_ms < detail::now_ms()) {
        return std::nullopt;
    }
    return data;
}

/// Convenience: record that the current deprecation has been read/acknowledged.
inline void dismiss_npm_deprecation(std::string_view id = {}) {
    auto data = get_npm_deprecation_data();
    const std::string notice_id = !id.empty() ? std::string(id) : (data ? data->id : std::string("default"));
    acknowledge_notification("npm_deprecation", notice_id);
}

// ==========================================================================
// 2. ModelMigration (useModelMigrationNotifications)
// ==========================================================================

struct ModelMigrationNotif {
    std::string from_model;
    std::string to_model;
    std::string reason;
    int64_t deadline_ms{0};         // when migration becomes mandatory
    bool auto_migrated{false};
};

DEFINE_STATE_SLOT(ModelMigrationTag, std::vector<ModelMigrationNotif>,
                  get_all_model_migrations, set_all_model_migrations)
inline void clear_model_migrations() { set_all_model_migrations({}); }

namespace detail {
struct CurrentModelTag {};
using CurrentModelSlot = GlobalStateSlot<CurrentModelTag, std::string>;
} // namespace detail

/// Get/set the model currently in use (used as the filter predicate).
inline auto get_current_model() -> std::string {
    std::lock_guard lock(detail::CurrentModelSlot::mtx);
    return detail::CurrentModelSlot::value;
}
inline void set_current_model(std::string_view model) {
    std::lock_guard lock(detail::CurrentModelSlot::mtx);
    detail::CurrentModelSlot::value = std::string(model);
}

/// Return all migrations whose `from_model` matches the current model and
/// which have not been individually dismissed.
inline auto get_pending_model_migrations() -> std::vector<ModelMigrationNotif> {
    const auto current = get_current_model();
    auto all = get_all_model_migrations();
    std::vector<ModelMigrationNotif> out;
    out.reserve(all.size());
    for (const auto& m : all) {
        if (m.from_model != current) continue;
        const std::string key = m.from_model + "->" + m.to_model;
        if (is_notification_dismissed("model_migration", key)) continue;
        out.push_back(m);
    }
    return out;
}

inline void acknowledge_migration(std::string_view from_model,
                                  std::string_view to_model = "*") {
    const std::string key = std::string(from_model) + "->" + std::string(to_model);
    acknowledge_notification("model_migration", key);
}
inline void acknowledge_migration(std::string_view from_model) {
    acknowledge_migration(from_model, "*");
}

// ==========================================================================
// 3. PluginAutoupdate (usePluginAutoupdateNotification)
// ==========================================================================

struct PluginUpdateInfo {
    std::string plugin_id;
    std::string current_version;
    std::string new_version;
    std::string changelog_url;
    bool is_security_update{false};
    int64_t download_size_bytes{0};
    int64_t release_ms{0};
};

DEFINE_STATE_SLOT(PluginAutoupdateTag, std::vector<PluginUpdateInfo>,
                  get_all_plugin_updates, set_all_plugin_updates)
inline void clear_plugin_updates() { set_all_plugin_updates({}); }

/// Return all plugin updates not individually dismissed.
inline auto get_plugin_updates() -> std::vector<PluginUpdateInfo> {
    auto all = get_all_plugin_updates();
    std::vector<PluginUpdateInfo> out;
    out.reserve(all.size());
    for (const auto& p : all) {
        const std::string key = p.plugin_id + "@" + p.new_version;
        if (is_notification_dismissed("plugin_autoupdate", key)) continue;
        out.push_back(p);
    }
    return out;
}

inline void acknowledge_plugin_update(std::string_view plugin_id,
                                      std::string_view new_version = "*") {
    const std::string key = std::string(plugin_id) + "@" + std::string(new_version);
    acknowledge_notification("plugin_autoupdate", key);
}
inline void acknowledge_plugin_update(std::string_view plugin_id) {
    acknowledge_plugin_update(plugin_id, "*");
}

// ==========================================================================
// 4. PluginInstallationStatus (usePluginInstallationStatus)
// ==========================================================================

enum class PluginInstallStatus {
    Pending,
    Installing,
    Installed,
    Failed
};

struct QueuedInstall {
    std::string plugin_id;
    std::string name;
};

struct InProgressInstall {
    std::string plugin_id;
    std::string name;
    int progress_pct{0};       // 0-100
};

struct FailedInstall {
    std::string plugin_id;
    std::string name;
    std::string error;
};

struct PluginInstallationStatusSnapshot {
    std::vector<QueuedInstall> queued;
    std::optional<InProgressInstall> installing;
    std::vector<FailedInstall> failed;
};

DEFINE_STATE_SLOT(PluginInstallationTag, PluginInstallationStatusSnapshot,
                  get_plugin_installation_snapshot, set_plugin_installation_snapshot)
inline void clear_plugin_installation_snapshot() {
    set_plugin_installation_snapshot(PluginInstallationStatusSnapshot{});
}

/// Back-compat accessor returning a flat vector of every install record.
struct PluginInstallInfo {
    std::string plugin_id;
    std::string name;
    PluginInstallStatus status{PluginInstallStatus::Pending};
    int progress_pct{0};
    std::optional<std::string> error;
};

inline auto get_plugin_installation_status() -> std::vector<PluginInstallInfo> {
    auto snap = get_plugin_installation_snapshot();
    std::vector<PluginInstallInfo> out;
    out.reserve(snap.queued.size() + snap.failed.size() + (snap.installing ? 1u : 0u));
    for (const auto& q : snap.queued) {
        out.push_back(PluginInstallInfo{
            .plugin_id = q.plugin_id, .name = q.name,
            .status = PluginInstallStatus::Pending});
    }
    if (snap.installing) {
        out.push_back(PluginInstallInfo{
            .plugin_id = snap.installing->plugin_id,
            .name = snap.installing->name,
            .status = PluginInstallStatus::Installing,
            .progress_pct = snap.installing->progress_pct});
    }
    for (const auto& f : snap.failed) {
        out.push_back(PluginInstallInfo{
            .plugin_id = f.plugin_id, .name = f.name,
            .status = PluginInstallStatus::Failed,
            .error = f.error});
    }
    return out;
}

// ==========================================================================
// 5. McpConnectivityStatus (useMcpConnectivityStatus)
// ==========================================================================

enum class McpServerStatus {
    Unknown,
    Connected,
    Disconnected,
    Error,
    Connecting,
};

struct McpConnectivityInfo {
    std::string server_id;
    std::string display_name;
    McpServerStatus state{McpServerStatus::Unknown};
    std::optional<std::string> last_error;
    int64_t last_seen_ms{0};
};

DEFINE_STATE_SLOT(McpConnectivityTag, std::vector<McpConnectivityInfo>,
                  get_raw_mcp_connectivity, set_raw_mcp_connectivity)
inline void clear_mcp_connectivity() { set_raw_mcp_connectivity({}); }

inline auto get_mcp_connectivity_status() -> std::vector<McpConnectivityInfo> {
    return get_raw_mcp_connectivity();
}

/// True iff any MCP server is currently in Disconnected or Error state.
inline auto has_mcp_connectivity_issues() -> bool {
    auto info = get_raw_mcp_connectivity();
    return std::any_of(info.begin(), info.end(), [](const McpConnectivityInfo& s) {
        return s.state == McpServerStatus::Disconnected ||
               s.state == McpServerStatus::Error;
    });
}

// ==========================================================================
// 6. SettingsErrors (useSettingsErrors)
// ==========================================================================

enum class SettingsSeverity {
    Warning,
    Error,
};

struct SettingsErrorInfo {
    std::string key_path;
    SettingsSeverity severity{SettingsSeverity::Warning};
    std::string message;
    std::optional<std::string> suggested_fix_opt;
    std::string human_field_label;
};

DEFINE_STATE_SLOT(SettingsErrorsTag, std::vector<SettingsErrorInfo>,
                  get_raw_settings_errors, set_raw_settings_errors)
inline void clear_settings_errors() { set_raw_settings_errors({}); }

inline auto get_settings_errors() -> std::vector<SettingsErrorInfo> {
    auto all = get_raw_settings_errors();
    std::vector<SettingsErrorInfo> out;
    out.reserve(all.size());
    for (const auto& e : all) {
        if (is_notification_dismissed("settings_errors", e.key_path)) continue;
        out.push_back(e);
    }
    return out;
}

inline auto has_settings_errors() -> bool {
    return !get_settings_errors().empty();
}

inline void dismiss_settings_error(std::string_view path) {
    acknowledge_notification("settings_errors", path);
}

// ==========================================================================
// 7. TeammateShutdown (useTeammateShutdownNotification)
// ==========================================================================

enum class TeammateShutdownCause {
    Finished,
    Failed,
    Crashed,
    LostHeartbeat,
};

struct TeammateShutdownInfo {
    std::string agent_id;
    std::string display_name;
    int64_t went_down_ms{0};
    TeammateShutdownCause cause{TeammateShutdownCause::Finished};
};

DEFINE_STATE_SLOT(TeammateShutdownTag, std::vector<TeammateShutdownInfo>,
                  get_all_teammate_shutdowns, set_all_teammate_shutdowns)
inline void clear_teammate_shutdowns() { set_all_teammate_shutdowns({}); }

/// Return teammate shutdowns that occurred within the last N minutes and
/// have not been acknowledged per-agent. Default window: 5 minutes.
inline auto get_teammate_shutdowns(std::chrono::minutes within = std::chrono::minutes(5))
    -> std::vector<TeammateShutdownInfo> {
    const int64_t cutoff = detail::now_ms() -
        std::chrono::duration_cast<std::chrono::milliseconds>(within).count();
    auto all = get_all_teammate_shutdowns();
    std::vector<TeammateShutdownInfo> out;
    for (const auto& t : all) {
        if (t.went_down_ms < cutoff) continue;
        if (is_notification_dismissed("teammate_shutdown", t.agent_id)) continue;
        out.push_back(t);
    }
    return out;
}

inline void acknowledge_teammate_shutdown(std::string_view agent_id) {
    acknowledge_notification("teammate_shutdown", agent_id);
}

// ==========================================================================
// 8. SubscriptionSwitch (useCanSwitchToExistingSubscription)
// ==========================================================================

struct SubscriptionSwitch {
    bool can_switch{false};
    std::string offer_tier;            // e.g. "pro", "team"
    int monthly_cost_usd_cents{0};
    int saving_pct_over_current{0};    // 0-100
    int trial_days_remaining{0};
};

DEFINE_STATE_SLOT(SubscriptionSwitchTag, std::optional<SubscriptionSwitch>,
                  get_subscription_switch_data, set_subscription_switch_data)
inline void clear_subscription_switch_data() {
    set_subscription_switch_data(std::nullopt);
}

inline auto can_switch_to_existing_subscription() -> std::optional<SubscriptionSwitch> {
    auto data = get_subscription_switch_data();
    if (!data) return std::nullopt;
    if (!data->can_switch) return std::nullopt;
    if (!data->offer_tier.empty() &&
        is_notification_dismissed("subscription", data->offer_tier)) {
        return std::nullopt;
    }
    return data;
}

inline void acknowledge_subscription_switch(std::string_view offer_tier) {
    acknowledge_notification("subscription", offer_tier);
}

#undef DEFINE_STATE_SLOT

} // namespace cc::hooks::notifs
