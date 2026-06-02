/// @file settings.cppm
/// @brief User settings management.
/// Supplements existing config.cppm with settings-specific logic.
module;

#include <string>
#include <optional>
#include <filesystem>
#include <unordered_map>
#include <variant>
#include <vector>

export module cc.config.settings;

export namespace cc::config {

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
