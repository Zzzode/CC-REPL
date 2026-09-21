module;

#include <algorithm>
#include <array>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.settings_sources;

export namespace cc::utils::settings_sources {

enum class SettingSource : unsigned char {
    UserSettings,
    ProjectSettings,
    LocalSettings,
    FlagSettings,
    PolicySettings,
};

inline constexpr std::array<SettingSource, 5> setting_sources = {
    SettingSource::UserSettings,
    SettingSource::ProjectSettings,
    SettingSource::LocalSettings,
    SettingSource::FlagSettings,
    SettingSource::PolicySettings,
};

[[nodiscard]] inline std::string_view to_string(SettingSource source) noexcept {
    switch (source) {
        case SettingSource::UserSettings: return "userSettings";
        case SettingSource::ProjectSettings: return "projectSettings";
        case SettingSource::LocalSettings: return "localSettings";
        case SettingSource::FlagSettings: return "flagSettings";
        case SettingSource::PolicySettings: return "policySettings";
    }
    return "userSettings";
}

[[nodiscard]] inline std::string_view get_setting_source_name(SettingSource source) noexcept {
    switch (source) {
        case SettingSource::UserSettings: return "user";
        case SettingSource::ProjectSettings: return "project";
        case SettingSource::LocalSettings: return "project, gitignored";
        case SettingSource::FlagSettings: return "cli flag";
        case SettingSource::PolicySettings: return "managed";
    }
    return "user";
}

[[nodiscard]] inline std::string_view get_source_display_name(SettingSource source) noexcept {
    switch (source) {
        case SettingSource::UserSettings: return "User";
        case SettingSource::ProjectSettings: return "Project";
        case SettingSource::LocalSettings: return "Local";
        case SettingSource::FlagSettings: return "Flag";
        case SettingSource::PolicySettings: return "Managed";
    }
    return "User";
}

[[nodiscard]] inline std::string_view get_source_display_name(std::string_view source) noexcept {
    if (source == "plugin") return "Plugin";
    if (source == "built-in") return "Built-in";
    return "";
}

[[nodiscard]] inline std::string_view get_display_name_lowercase(std::string_view source) noexcept {
    if (source == "userSettings") return "user settings";
    if (source == "projectSettings") return "shared project settings";
    if (source == "localSettings") return "project local settings";
    if (source == "flagSettings") return "command line arguments";
    if (source == "policySettings") return "enterprise managed settings";
    if (source == "cliArg") return "CLI argument";
    if (source == "command") return "command configuration";
    if (source == "session") return "current session";
    return "";
}

[[nodiscard]] inline std::string_view get_display_name_capitalized(std::string_view source) noexcept {
    if (source == "userSettings") return "User settings";
    if (source == "projectSettings") return "Shared project settings";
    if (source == "localSettings") return "Project local settings";
    if (source == "flagSettings") return "Command line arguments";
    if (source == "policySettings") return "Enterprise managed settings";
    if (source == "cliArg") return "CLI argument";
    if (source == "command") return "Command configuration";
    if (source == "session") return "Current session";
    return "";
}

[[nodiscard]] inline std::string trim_copy(std::string_view value) {
    auto first = value.begin();
    auto last = value.end();
    while (first != last && (*first == ' ' || *first == '\t' || *first == '\r' || *first == '\n')) ++first;
    while (first != last) {
        auto prev = last;
        --prev;
        if (*prev != ' ' && *prev != '\t' && *prev != '\r' && *prev != '\n') break;
        last = prev;
    }
    return std::string(first, last);
}

[[nodiscard]] inline std::expected<std::vector<SettingSource>, std::string> parse_setting_sources_flag(std::string_view flag) {
    if (flag.empty()) return std::vector<SettingSource>{};

    std::vector<SettingSource> result;
    std::size_t start = 0;
    while (start <= flag.size()) {
        const auto comma = flag.find(',', start);
        const auto end = comma == std::string_view::npos ? flag.size() : comma;
        const auto name = trim_copy(flag.substr(start, end - start));

        if (name == "user") result.push_back(SettingSource::UserSettings);
        else if (name == "project") result.push_back(SettingSource::ProjectSettings);
        else if (name == "local") result.push_back(SettingSource::LocalSettings);
        else return std::unexpected("Invalid setting source: " + name + ". Valid options are: user, project, local");

        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return result;
}

} // namespace cc::utils::settings_sources
