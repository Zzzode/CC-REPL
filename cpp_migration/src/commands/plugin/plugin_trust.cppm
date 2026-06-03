module;
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <map>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

export module cc.commands.plugin.plugin_trust;

export namespace cc::commands {


enum class TrustLevel {
    Untrusted,
    Trusted,
    Verified
};

auto trust_config_path() -> std::filesystem::path {
    if (const char* home = std::getenv("HOME")) return std::filesystem::path{home} / ".cc-repl" / "plugin-trust.txt";
    return std::filesystem::path{".cc-repl"} / "plugin-trust.txt";
}

auto trust_to_string(TrustLevel level) -> std::string;
auto string_to_trust(std::string_view value) -> TrustLevel;
auto read_trust_config() -> std::map<std::string, TrustLevel>;
auto write_trust_config(const std::map<std::string, TrustLevel>& config) -> void;


auto get_trust_level(std::string_view plugin_id) -> TrustLevel {
    if (plugin_id.empty()) {
        return TrustLevel::Untrusted;
    }
    auto config = read_trust_config();
    auto found = config.find(std::string(plugin_id));
    return found == config.end() ? TrustLevel::Untrusted : found->second;
}


auto set_trust_level(std::string_view plugin_id, TrustLevel level) -> void {
    if (plugin_id.empty()) {
        return;
    }
    auto config = read_trust_config();
    config[std::string(plugin_id)] = level;
    write_trust_config(config);
}


auto prompt_trust_dialog(std::string_view plugin_id, std::string_view permissions) -> bool {
    if (plugin_id.empty()) {
        return false;
    }
    auto level = get_trust_level(plugin_id);
    if (level == TrustLevel::Verified) return true;
    if (level == TrustLevel::Trusted && permissions.find("unsafe") == std::string_view::npos) return true;
    return false;
}


auto get_plugin_permissions(std::string_view id) -> std::vector<std::string> {
    if (id.empty()) {
        return {};
    }
    auto manifest_path = std::filesystem::path{std::string(id)} / "manifest.txt";
    std::ifstream input{manifest_path};
    std::vector<std::string> permissions;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.starts_with("permission=")) continue;
        permissions.push_back(line.substr(std::string{"permission="}.size()));
    }
    return permissions;
}

auto trust_to_string(TrustLevel level) -> std::string {
    switch (level) {
        case TrustLevel::Verified: return "verified";
        case TrustLevel::Trusted: return "trusted";
        case TrustLevel::Untrusted: return "untrusted";
    }
    return "untrusted";
}

auto string_to_trust(std::string_view value) -> TrustLevel {
    if (value == "verified") return TrustLevel::Verified;
    if (value == "trusted") return TrustLevel::Trusted;
    return TrustLevel::Untrusted;
}

auto read_trust_config() -> std::map<std::string, TrustLevel> {
    std::map<std::string, TrustLevel> config;
    std::ifstream input{trust_config_path()};
    std::string line;
    while (std::getline(input, line)) {
        auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        config[line.substr(0, separator)] = string_to_trust(line.substr(separator + 1));
    }
    return config;
}

auto write_trust_config(const std::map<std::string, TrustLevel>& config) -> void {
    auto path = trust_config_path();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::trunc};
    for (const auto& [id, level] : config) output << id << '=' << trust_to_string(level) << '\n';
}

} // namespace cc::commands
