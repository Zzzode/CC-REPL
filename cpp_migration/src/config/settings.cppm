/// @file settings.cppm
/// @brief User settings management.
/// Supplements existing config.cppm with settings-specific logic.
module;

#include <string>
#include <string_view>
#include <optional>
#include <functional>
#include <filesystem>
#include <unordered_map>
#include <variant>
#include <vector>

import cc.utils.json;

export module cc.config.settings;

export namespace cc::config {

/// Result of applying a --settings flag payload.
/// Tracks which top-level keys were applied and which were deferred so the
/// caller can surface honest feedback (matches TS which logs unknown keys).
struct FlagSettingsResult {
    std::optional<std::string> model;        // from `model` (string) if present
    std::optional<std::string> api_key;      // from `apiKey` (string) if present
    std::vector<std::string> applied_env_keys; // env vars set (in iteration order)
    std::vector<std::string> deferred_keys;    // recognized-but-unhandled or unknown keys
};

/// EnvSetter abstraction: lets callers (tests) inject a recorder instead of
/// mutating the process environment. Defaults to setenv for production use.
using EnvSetter = std::function<void(std::string_view name, std::string_view value)>;

/// Apply a parsed --settings JSON object to the active environment.
///
/// Mirrors the TS `loadSettingsFromFlag` semantics for the priority subset of
/// keys that the C++ port currently understands:
///   - `env`:     object of name->string; each is set via the env_setter
///                (production: setenv). This is the key path for provider
///                credentials (ANTHROPIC_API_KEY / ANTHROPIC_BASE_URL / ...).
///   - `apiKey`:  string; reported back so the caller can set ANTHROPIC_API_KEY.
///   - `model`:   string; reported back so the caller can override the default.
///
/// Keys the C++ port does NOT yet apply (permissions merge, hooks, mcpServers,
/// managed/MDM settings, etc.) are recorded in `deferred_keys` for honest
/// feedback rather than silently dropped.
[[nodiscard]] inline FlagSettingsResult apply_flag_settings(
    const cc::utils::json::JsonVal& root,
    const EnvSetter& env_setter
) {
    FlagSettingsResult out;

    if (!root.is_obj()) {
        out.deferred_keys.emplace_back("<root-not-object>");
        return out;
    }

    // env: object of name -> string value
    if (auto env = root.get("env"); env.is_obj()) {
        env.iter_obj([&](auto key, auto value) {
            if (!key.is_str() || !value.is_str()) return;
            const auto name = std::string(key.as_str());
            const auto value_str = std::string(value.as_str());
            if (env_setter) env_setter(name, value_str);
            out.applied_env_keys.push_back(name);
        });
    }

    // apiKey: string -> reported back (caller sets ANTHROPIC_API_KEY)
    if (auto api_key = root.get("apiKey"); api_key.is_str()) {
        out.api_key = std::string(api_key.as_str());
    }

    // model: string -> reported back (caller overrides default model)
    if (auto model = root.get("model"); model.is_str()) {
        out.model = std::string(model.as_str());
    }

    // Record keys we recognize-but-do-not-yet-apply or do not understand, so
    // callers get honest feedback rather than silent drops.
    root.iter_obj([&](auto key, auto /*value*/) {
        if (!key.is_str()) return;
        const auto name = std::string(key.as_str());
        if (name == "env" || name == "apiKey" || name == "model") return;
        out.deferred_keys.push_back(name);
    });

    return out;
}

/// Setting value types
using SettingValue = std::variant<std::string, int, double, bool, std::vector<std::string>>;

/// Settings scope
enum class SettingsScope : std::uint8_t {
    User,       // ~/.claude/settings.json
    Project,    // .claude/settings.json in project root
    Local,      // .claude/settings.local.json (gitignored)
};

/// A single settings entry
struct SettingsEntry {
    std::string key;
    SettingValue value;
    SettingsScope scope;
};

/// Get settings file path for a scope
[[nodiscard]] inline std::filesystem::path get_settings_path(
    SettingsScope scope,
    const std::filesystem::path& home_dir,
    const std::filesystem::path& project_root
) {
    switch (scope) {
        case SettingsScope::User:
            return home_dir / ".claude" / "settings.json";
        case SettingsScope::Project:
            return project_root / ".claude" / "settings.json";
        case SettingsScope::Local:
            return project_root / ".claude" / "settings.local.json";
    }
    return {};
}

/// Known setting keys
namespace keys {
    inline constexpr std::string_view MODEL = "model";
    inline constexpr std::string_view PERMISSION_MODE = "permissionMode";
    inline constexpr std::string_view ALLOWED_TOOLS = "allowedTools";
    inline constexpr std::string_view THEME = "theme";
    inline constexpr std::string_view VERBOSE = "verbose";
    inline constexpr std::string_view SPINNER_VERBS = "spinnerVerbs";
    inline constexpr std::string_view BUDDY_ENABLED = "buddyEnabled";
    inline constexpr std::string_view VIM_MODE = "vimMode";
    inline constexpr std::string_view PREFERRED_NOT_EMPTY_RESPONSES = "preferNotEmptyResponses";
} // namespace keys

} // namespace cc::config
