module;

#include <optional>
#include <string>
#include <string_view>

export module cc.utils.settings_paths;

import cc.utils.settings_sources;

export namespace cc::utils::settings_paths {

enum class Platform : unsigned char {
    MacOS,
    Windows,
    Linux,
};

[[nodiscard]] inline std::string managed_file_path(
    Platform platform,
    std::string_view user_type = {},
    std::optional<std::string_view> managed_settings_path_override = std::nullopt
) {
    if (user_type == "ant" && managed_settings_path_override && !managed_settings_path_override->empty()) {
        return std::string(*managed_settings_path_override);
    }
    switch (platform) {
        case Platform::MacOS: return "/Library/Application Support/ClaudeCode";
        case Platform::Windows: return "C:\\Program Files\\ClaudeCode";
        case Platform::Linux: return "/etc/claude-code";
    }
    return "/etc/claude-code";
}

[[nodiscard]] inline std::string join_path(std::string_view base, std::string_view child) {
    if (base.empty()) return std::string(child);
    std::string out(base);
    const char last = out.back();
    if (last != '/' && last != '\\') out.push_back('/');
    out.append(child);
    return out;
}

[[nodiscard]] inline std::string managed_settings_drop_in_dir(std::string_view managed_file_path_value) {
    return join_path(managed_file_path_value, "managed-settings.d");
}

[[nodiscard]] inline std::string relative_settings_file_path_for_source(cc::utils::settings_sources::SettingSource source) {
    using cc::utils::settings_sources::SettingSource;
    switch (source) {
        case SettingSource::ProjectSettings: return ".claude/settings.json";
        case SettingSource::LocalSettings: return ".claude/settings.local.json";
        default: return "";
    }
}

} // namespace cc::utils::settings_paths
