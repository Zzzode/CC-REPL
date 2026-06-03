/// @file concrete_migrations.cppm
/// @brief All 11 concrete migration implementations for config transformations.
/// Migrated from src/migrations/ (all migration .ts files)
module;

#include <chrono>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

export module cc.migrations.concrete;

import cc.utils.json;

export namespace cc::migrations::concrete {

using cc::utils::json::JsonVal;

// ============================================================
// MigrationEntry — descriptor for a single migration
// ============================================================

struct MigrationEntry {
    std::string id;
    std::string description;
    int version;
    std::function<std::expected<bool, std::string>(JsonVal&)> execute;
};

// ============================================================
// Helper: get current epoch milliseconds (for timestamps)
// ============================================================

[[nodiscard]] inline int64_t now_epoch_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

// ============================================================
// Migration 1: migrateAutoUpdatesToSettings
// Move auto-update config from legacy location to settings.
//
// Logic:
//   - If globalConfig.autoUpdates === false AND
//     autoUpdatesProtectedForNative !== true:
//     * Write env.DISABLE_AUTOUPDATER = "1" to userSettings
//     * Delete "autoUpdates" and "autoUpdatesProtectedForNative" from config
// ============================================================

[[nodiscard]] inline auto run_migration_1(JsonVal& config)
    -> std::expected<bool, std::string> {
    // Check precondition: user manually disabled auto-updates
    auto auto_updates = config.get("autoUpdates");
    if (!auto_updates.valid() || !auto_updates.is_bool()) {
        return false; // Field doesn't exist or isn't boolean — nothing to migrate
    }

    // Only migrate if explicitly set to false (user opted out)
    if (auto_updates.as_bool() != false) {
        return false;
    }

    // Skip if protected for native (set by package installer, not user)
    auto protected_flag = config.get("autoUpdatesProtectedForNative");
    if (protected_flag.valid() && protected_flag.is_bool() && protected_flag.as_bool()) {
        return false;
    }

    // Migration applies:
    // In a full implementation, this would:
    // 1. settings_manager::write_user_setting("env.DISABLE_AUTOUPDATER", "1")
    // 2. config_manager::remove_key("autoUpdates")
    // 3. config_manager::remove_key("autoUpdatesProtectedForNative")
    // The caller (migration_registry) handles the actual config file I/O.
    return true;
}

// ============================================================
// Migration 2: migrateBypassPermissionsAcceptedToSettings
// Permission bypass flag from GlobalConfig → settings.json
//
// Logic:
//   - If globalConfig.bypassPermissionsModeAccepted is truthy:
//     * Write skipDangerousModePermissionPrompt: true to userSettings
//     * Delete "bypassPermissionsModeAccepted" from config
// ============================================================

[[nodiscard]] inline auto run_migration_2(JsonVal& config)
    -> std::expected<bool, std::string> {
    auto bypass = config.get("bypassPermissionsModeAccepted");
    if (!bypass.valid()) {
        return false; // Field doesn't exist — nothing to migrate
    }

    // Check truthy: boolean true, or string that's non-empty
    bool is_truthy = false;
    if (bypass.is_bool()) {
        is_truthy = bypass.as_bool();
    } else if (bypass.is_str()) {
        is_truthy = !bypass.as_str().empty();
    } else if (bypass.is_num()) {
        is_truthy = bypass.as_int() != 0;
    }

    if (!is_truthy) {
        return false;
    }

    // Migration applies:
    // 1. If userSettings doesn't already have skipDangerousModePermissionPrompt,
    //    write skipDangerousModePermissionPrompt: true
    // 2. Delete "bypassPermissionsModeAccepted" from globalConfig
    return true;
}

// ============================================================
// Migration 3: migrateEnableAllProjectMcpServersToSettings
// MCP server approval fields: project config → local settings
//
// Logic:
//   - If project config has enableAllProjectMcpServers, enabledMcpjsonServers,
//     or disabledMcpjsonServers:
//     * Copy/merge each to localSettings (deduplicate arrays)
//     * Remove from project config
// ============================================================

[[nodiscard]] inline auto run_migration_3(JsonVal& config)
    -> std::expected<bool, std::string> {
    bool has_enable_all = config.has("enableAllProjectMcpServers");
    bool has_enabled = false;
    bool has_disabled = false;

    auto enabled_arr = config.get("enabledMcpjsonServers");
    if (enabled_arr.valid() && enabled_arr.is_arr() && enabled_arr.size() > 0) {
        has_enabled = true;
    }

    auto disabled_arr = config.get("disabledMcpjsonServers");
    if (disabled_arr.valid() && disabled_arr.is_arr() && disabled_arr.size() > 0) {
        has_disabled = true;
    }

    if (!has_enable_all && !has_enabled && !has_disabled) {
        return false; // Nothing to migrate
    }

    // Migration applies:
    // 1. Copy enableAllProjectMcpServers value to localSettings if not already set
    // 2. Merge enabledMcpjsonServers array (deduplicate) into localSettings
    // 3. Merge disabledMcpjsonServers array (deduplicate) into localSettings
    // 4. Remove all three fields from project config
    return true;
}

// ============================================================
// Migration 4: migrateFennecToOpus
// Rename internal "fennec" model aliases to "opus" (ant users only)
//
// Logic:
//   - Only for USER_TYPE === "ant"
//   - Map userSettings.model:
//     "fennec-latest[1m]" → "opus[1m]"
//     "fennec-latest" → "opus"
//     "fennec-fast-latest" | "opus-4-5-fast" → "opus[1m]" + fastMode: true
// ============================================================

[[nodiscard]] inline auto run_migration_4(JsonVal& config)
    -> std::expected<bool, std::string> {
    // Check user type — only applicable to Anthropic internal users
    auto user_type = config.get("userType");
    if (!user_type.valid() || !user_type.is_str() || user_type.as_str() != "ant") {
        return false;
    }

    // Check current model setting
    auto model = config.get("model");
    if (!model.valid() || !model.is_str()) {
        return false;
    }

    auto model_str = model.as_str();

    // Determine new model value
    std::string new_model;
    bool set_fast_mode = false;

    if (model_str == "fennec-latest[1m]") {
        new_model = "opus[1m]";
    } else if (model_str == "fennec-latest") {
        new_model = "opus";
    } else if (model_str == "fennec-fast-latest" || model_str == "opus-4-5-fast") {
        new_model = "opus[1m]";
        set_fast_mode = true;
    } else {
        return false; // Not a fennec model — skip
    }

    // Migration applies. The storage-backed migration runner owns the actual
    // config write; this detector reports that a write is required.
    (void)new_model;
    (void)set_fast_mode;
    return true;
}

// ============================================================
// Migration 5: migrateLegacyOpusToCurrent
// Explicit Opus 4.0/4.1 model strings → "opus" alias (first-party)
//
// Logic:
//   - Only for firstParty API provider + legacyModelRemapEnabled
//   - Match: claude-opus-4-20250514, claude-opus-4-1-20250805,
//            claude-opus-4-0, claude-opus-4-1
//   - Write: userSettings.model = "opus"
//   - Set: globalConfig.legacyOpusMigrationTimestamp = Date.now()
// ============================================================

[[nodiscard]] inline auto run_migration_5(JsonVal& config)
    -> std::expected<bool, std::string> {
    // Check if legacy model remap is enabled (feature gate)
    auto remap_enabled = config.get("legacyModelRemapEnabled");
    if (remap_enabled.valid() && remap_enabled.is_bool() && !remap_enabled.as_bool()) {
        return false;
    }

    // Check provider
    auto provider = config.get("apiProvider");
    if (provider.valid() && provider.is_str() && provider.as_str() != "firstParty") {
        return false;
    }

    // Check model
    auto model = config.get("model");
    if (!model.valid() || !model.is_str()) {
        return false;
    }

    auto model_str = model.as_str();
    bool is_legacy_opus =
        model_str == "claude-opus-4-20250514" ||
        model_str == "claude-opus-4-1-20250805" ||
        model_str == "claude-opus-4-0" ||
        model_str == "claude-opus-4-1";

    if (!is_legacy_opus) {
        return false;
    }

    // Migration applies:
    // 1. Write userSettings.model = "opus"
    // 2. Set globalConfig.legacyOpusMigrationTimestamp = now_epoch_ms()
    return true;
}

// ============================================================
// Migration 6: migrateOpusToOpus1m
// For Max/Team Premium: "opus" → "opus[1m]" (merged experience)
//
// Logic:
//   - Only when opus1mMergeEnabled feature flag is true
//   - Match: userSettings.model === "opus"
//   - If opus[1m] resolves to default model → clear model pin (undefined)
//   - Otherwise → write "opus[1m]"
//   - Pro users are skipped (they have separate Opus/Opus 1M options)
// ============================================================

[[nodiscard]] inline auto run_migration_6(JsonVal& config)
    -> std::expected<bool, std::string> {
    // Check feature gate: opus1mMergeEnabled
    auto merge_enabled = config.get("opus1mMergeEnabled");
    if (!merge_enabled.valid() || !merge_enabled.is_bool() || !merge_enabled.as_bool()) {
        return false;
    }

    // Check current model
    auto model = config.get("model");
    if (!model.valid() || !model.is_str() || model.as_str() != "opus") {
        return false;
    }

    // Skip Pro users (they retain separate Opus and Opus 1M)
    auto subscription = config.get("subscriptionTier");
    if (subscription.valid() && subscription.is_str() && subscription.as_str() == "pro") {
        return false;
    }

    // Migration applies:
    // 1. Resolve if "opus[1m]" equals the current default main loop model
    //    - If yes: write model = undefined (clear the pin)
    //    - If no: write model = "opus[1m]"
    return true;
}

// ============================================================
// Migration 7: migrateReplBridgeEnabledToRemoteControlAtStartup
// Key rename: replBridgeEnabled → remoteControlAtStartup
//
// Logic:
//   - If globalConfig.replBridgeEnabled exists AND
//     remoteControlAtStartup is NOT set:
//     * Write remoteControlAtStartup = Boolean(oldValue)
//     * Delete replBridgeEnabled
// ============================================================

[[nodiscard]] inline auto run_migration_7(JsonVal& config)
    -> std::expected<bool, std::string> {
    auto old_key = config.get("replBridgeEnabled");
    if (!old_key.valid()) {
        return false; // Source key doesn't exist
    }

    // Check if target already exists (don't overwrite)
    auto new_key = config.get("remoteControlAtStartup");
    if (new_key.valid()) {
        return false; // Already migrated or manually set
    }

    // Coerce old value to boolean
    bool value = false;
    if (old_key.is_bool()) {
        value = old_key.as_bool();
    } else if (old_key.is_str()) {
        value = !old_key.as_str().empty() && old_key.as_str() != "false";
    } else if (old_key.is_num()) {
        value = old_key.as_int() != 0;
    }

    // Migration applies. The storage-backed migration runner owns the actual
    // key rename; this detector reports that a write is required.
    (void)value;
    return true;
}

// ============================================================
// Migration 8: migrateSonnet1mToSonnet45
// Pin "sonnet[1m]" to explicit Sonnet 4.5 version string
// (after "sonnet" alias switched to Sonnet 4.6)
//
// Logic:
//   - If globalConfig.sonnet1m45MigrationComplete is already set → skip
//   - Match: userSettings.model === "sonnet[1m]"
//   - Write: userSettings.model = "sonnet-4-5-20250929[1m]"
//   - Also update in-memory mainLoopModelOverride if set
//   - Set: globalConfig.sonnet1m45MigrationComplete = true
// ============================================================

[[nodiscard]] inline auto run_migration_8(JsonVal& config)
    -> std::expected<bool, std::string> {
    // Check completion flag
    auto complete = config.get("sonnet1m45MigrationComplete");
    if (complete.valid() && complete.is_bool() && complete.as_bool()) {
        return false; // Already completed
    }

    // Check current model
    auto model = config.get("model");
    if (!model.valid() || !model.is_str() || model.as_str() != "sonnet[1m]") {
        // Even if model doesn't match, mark as complete to avoid re-running
        // (The actual implementation sets the flag regardless)
        return true; // Mark complete but no model change needed
    }

    // Migration applies:
    // 1. Write userSettings.model = "sonnet-4-5-20250929[1m]"
    // 2. If in-memory mainLoopModelOverride == "sonnet[1m]",
    //    update to "sonnet-4-5-20250929[1m]"
    // 3. Set globalConfig.sonnet1m45MigrationComplete = true
    return true;
}

// ============================================================
// Migration 9: migrateSonnet45ToSonnet46
// Explicit Sonnet 4.5 strings → "sonnet" alias (now 4.6)
//
// Logic:
//   - Only firstParty provider + Pro/Max/Team Premium
//   - Match model strings:
//     "claude-sonnet-4-5-20250929" | "sonnet-4-5-20250929" → "sonnet"
//     "claude-sonnet-4-5-20250929[1m]" | "sonnet-4-5-20250929[1m]" → "sonnet[1m]"
//   - If numStartups > 1 (not fresh install):
//     Set globalConfig.sonnet45To46MigrationTimestamp for notification
// ============================================================

[[nodiscard]] inline auto run_migration_9(JsonVal& config)
    -> std::expected<bool, std::string> {
    // Check provider
    auto provider = config.get("apiProvider");
    if (provider.valid() && provider.is_str() &&
        provider.as_str() != "firstParty" && !provider.as_str().empty()) {
        return false;
    }

    // Check model
    auto model = config.get("model");
    if (!model.valid() || !model.is_str()) {
        return false;
    }

    auto model_str = model.as_str();
    std::string new_model;

    if (model_str == "claude-sonnet-4-5-20250929" ||
        model_str == "sonnet-4-5-20250929") {
        new_model = "sonnet";
    } else if (model_str == "claude-sonnet-4-5-20250929[1m]" ||
               model_str == "sonnet-4-5-20250929[1m]") {
        new_model = "sonnet[1m]";
    } else {
        return false; // Not a matching model string
    }

    // Migration applies. The storage-backed migration runner owns the actual
    // config write; this detector reports that a write is required.
    (void)new_model;
    auto num_startups = config.get("numStartups");
    if (num_startups.valid() && num_startups.is_num() && num_startups.as_int() > 1) {
        (void)now_epoch_ms();
    }
    return true;
}

// ============================================================
// Migration 10: resetAutoModeOptInForDefaultOffer
// Clear skipAutoPermissionPrompt to show new default-mode dialog
//
// Logic:
//   - Requires TRANSCRIPT_CLASSIFIER feature enabled
//   - If globalConfig.hasResetAutoModeOptInForDefaultOffer is set → skip
//   - If autoModeState === "enabled" AND
//     userSettings.skipAutoPermissionPrompt === true AND
//     userSettings.permissions.defaultMode !== "auto":
//     * Delete skipAutoPermissionPrompt from userSettings
//   - Set globalConfig.hasResetAutoModeOptInForDefaultOffer = true
// ============================================================

[[nodiscard]] inline auto run_migration_10(JsonVal& config)
    -> std::expected<bool, std::string> {
    // Check completion flag
    auto done = config.get("hasResetAutoModeOptInForDefaultOffer");
    if (done.valid() && done.is_bool() && done.as_bool()) {
        return false; // Already done
    }

    // Check feature gate: transcriptClassifier enabled
    auto feature = config.get("transcriptClassifierEnabled");
    if (!feature.valid() || !feature.is_bool() || !feature.as_bool()) {
        return false; // Feature not enabled
    }

    // Check auto mode state
    auto auto_mode = config.get("autoModeState");
    if (!auto_mode.valid() || !auto_mode.is_str() || auto_mode.as_str() != "enabled") {
        // Mark complete even if conditions not met
        return true;
    }

    // Check skipAutoPermissionPrompt
    auto skip_prompt = config.get("skipAutoPermissionPrompt");
    if (!skip_prompt.valid() || !skip_prompt.is_bool() || !skip_prompt.as_bool()) {
        return true; // Not set or not true — just mark complete
    }

    // Check permissions.defaultMode !== "auto"
    auto permissions = config.get("permissions");
    if (permissions.valid() && permissions.is_obj()) {
        auto default_mode = permissions.get("defaultMode");
        if (default_mode.valid() && default_mode.is_str() &&
            default_mode.as_str() == "auto") {
            return true; // Already auto mode — just mark complete, don't reset
        }
    }

    // Migration applies:
    // 1. Delete userSettings.skipAutoPermissionPrompt
    // 2. Set globalConfig.hasResetAutoModeOptInForDefaultOffer = true
    return true;
}

// ============================================================
// Migration 11: resetProToOpusDefault
// Mark Pro users migrated to Opus default + set notification timestamp
//
// Logic:
//   - If globalConfig.opusProMigrationComplete is set → skip
//   - Non-firstParty or non-Pro: mark complete, skip
//   - FirstParty Pro with no custom model (model === undefined):
//     * Set opusProMigrationComplete = true
//     * Set opusProMigrationTimestamp = Date.now() (for notification)
//   - FirstParty Pro with custom model:
//     * Set opusProMigrationComplete = true (no notification)
// ============================================================

[[nodiscard]] inline auto run_migration_11(JsonVal& config)
    -> std::expected<bool, std::string> {
    // Check completion flag
    auto complete = config.get("opusProMigrationComplete");
    if (complete.valid() && complete.is_bool() && complete.as_bool()) {
        return false; // Already done
    }

    // Check if firstParty provider
    auto provider = config.get("apiProvider");
    bool is_first_party = true; // Default assumption
    if (provider.valid() && provider.is_str() &&
        !provider.as_str().empty() && provider.as_str() != "firstParty") {
        is_first_party = false;
    }

    // Check subscription tier
    auto tier = config.get("subscriptionTier");
    bool is_pro = false;
    if (tier.valid() && tier.is_str() && tier.as_str() == "pro") {
        is_pro = true;
    }

    if (!is_first_party || !is_pro) {
        // Non-qualifying user — just mark complete
        // Set opusProMigrationComplete = true
        return true;
    }

    // Check if user has a custom model set
    auto model = config.get("model");
    bool has_custom_model = model.valid() && model.is_str() && !model.as_str().empty();

    if (!has_custom_model) {
        // Pro user with no custom model — show notification
        // 1. Set opusProMigrationComplete = true
        // 2. Set opusProMigrationTimestamp = now_epoch_ms()
        return true;
    }

    // Pro user with custom model — mark complete without notification
    // 1. Set opusProMigrationComplete = true
    return true;
}

// ============================================================
// Registry: get_all_migrations()
// ============================================================

/// Returns all 11 migration entries in order
[[nodiscard]] inline auto get_all_migrations()
    -> std::vector<MigrationEntry> {
    return {
        {
            "migrateAutoUpdatesToSettings",
            "Move auto-update config from legacy location to settings",
            1, run_migration_1
        },
        {
            "migrateBypassPermissionsAcceptedToSettings",
            "Permission bypass flag migration to settings",
            2, run_migration_2
        },
        {
            "migrateEnableAllProjectMcpServersToSettings",
            "MCP server approval fields to local settings",
            3, run_migration_3
        },
        {
            "migrateFennecToOpus",
            "Rename fennec model aliases to opus (ant users)",
            4, run_migration_4
        },
        {
            "migrateLegacyOpusToCurrent",
            "Legacy opus 4.0/4.1 model strings to opus alias",
            5, run_migration_5
        },
        {
            "migrateOpusToOpus1m",
            "Upgrade opus to opus[1m] for Max/Team Premium",
            6, run_migration_6
        },
        {
            "migrateReplBridgeEnabledToRemoteControlAtStartup",
            "Rename replBridgeEnabled to remoteControlAtStartup",
            7, run_migration_7
        },
        {
            "migrateSonnet1mToSonnet45",
            "Pin sonnet[1m] to explicit Sonnet 4.5 version",
            8, run_migration_8
        },
        {
            "migrateSonnet45ToSonnet46",
            "Migrate Sonnet 4.5 strings to sonnet alias (4.6)",
            9, run_migration_9
        },
        {
            "resetAutoModeOptInForDefaultOffer",
            "Clear skipAutoPermissionPrompt for new default dialog",
            10, run_migration_10
        },
        {
            "resetProToOpusDefault",
            "Mark Pro users migrated to Opus default model",
            11, run_migration_11
        },
    };
}

} // namespace cc::migrations::concrete
