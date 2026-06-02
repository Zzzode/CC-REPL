module;

#include <algorithm>
#include <array>
#include <cctype>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

export module cc.utils.plugin_identifier;

import cc.utils.settings_sources;

export namespace cc::utils::plugin_identifier {

struct ParsedPluginIdentifier {
    std::string name;
    std::optional<std::string> marketplace;
};

enum class PluginScope : unsigned char {
    User,
    Project,
    Local,
    Managed,
};

[[nodiscard]] inline std::string lower_copy(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

[[nodiscard]] inline ParsedPluginIdentifier parse_plugin_identifier(std::string_view plugin) {
    const auto at = plugin.find('@');
    if (at == std::string_view::npos) return {.name = std::string(plugin), .marketplace = std::nullopt};
    const auto next_at = plugin.find('@', at + 1);
    const auto marketplace_end = next_at == std::string_view::npos ? plugin.size() : next_at;
    return {
        .name = std::string(plugin.substr(0, at)),
        .marketplace = std::string(plugin.substr(at + 1, marketplace_end - at - 1)),
    };
}

[[nodiscard]] inline std::string build_plugin_id(std::string_view name, std::optional<std::string_view> marketplace = std::nullopt) {
    if (!marketplace || marketplace->empty()) return std::string(name);
    std::string out(name);
    out.push_back('@');
    out.append(*marketplace);
    return out;
}

[[nodiscard]] inline bool is_official_marketplace_name(std::string_view marketplace) {
    static constexpr std::array<std::string_view, 8> allowed = {
        "claude-code-marketplace",
        "claude-code-plugins",
        "claude-plugins-official",
        "anthropic-marketplace",
        "anthropic-plugins",
        "agent-skills",
        "life-sciences",
        "knowledge-work-plugins",
    };
    const auto normalized = lower_copy(marketplace);
    return std::find(allowed.begin(), allowed.end(), normalized) != allowed.end();
}

[[nodiscard]] inline std::expected<cc::utils::settings_sources::SettingSource, std::string> scope_to_setting_source(PluginScope scope) {
    using cc::utils::settings_sources::SettingSource;
    switch (scope) {
        case PluginScope::User: return SettingSource::UserSettings;
        case PluginScope::Project: return SettingSource::ProjectSettings;
        case PluginScope::Local: return SettingSource::LocalSettings;
        case PluginScope::Managed: return std::unexpected("Cannot install plugins to managed scope");
    }
    return std::unexpected("Unknown plugin scope");
}

[[nodiscard]] inline PluginScope setting_source_to_scope(cc::utils::settings_sources::SettingSource source) {
    using cc::utils::settings_sources::SettingSource;
    switch (source) {
        case SettingSource::UserSettings: return PluginScope::User;
        case SettingSource::ProjectSettings: return PluginScope::Project;
        case SettingSource::LocalSettings: return PluginScope::Local;
        case SettingSource::PolicySettings: return PluginScope::Managed;
        case SettingSource::FlagSettings: return PluginScope::User;
    }
    return PluginScope::User;
}

} // namespace cc::utils::plugin_identifier
