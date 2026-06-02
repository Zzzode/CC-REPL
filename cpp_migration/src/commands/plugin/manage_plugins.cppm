module;
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <map>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <sstream>

export module cc.commands.plugin.manage_plugins;

export namespace cc::commands {

using std::filesystem::path;

// 已安装的插件信息
struct InstalledPlugin {
    std::string id;
    std::string version;
    bool enabled;
    path install_dir;
};

auto plugins_dir() -> path;
auto read_manifest(const path& manifest_path) -> std::map<std::string, std::string>;
auto write_manifest(const InstalledPlugin& plugin) -> std::expected<void, std::string>;
auto set_plugin_enabled(std::string_view id, bool enabled) -> std::expected<void, std::string>;

// 获取所有已安装插件列表
auto get_installed_plugins() -> std::vector<InstalledPlugin> {
    std::vector<InstalledPlugin> plugins;
    auto dir = plugins_dir();
    if (!std::filesystem::exists(dir)) return plugins;

    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_directory()) continue;
        auto manifest = read_manifest(entry.path() / "manifest.txt");
        auto id = manifest.contains("id") ? manifest["id"] : entry.path().filename().string();
        plugins.push_back(InstalledPlugin{
            .id = id,
            .version = manifest.contains("version") ? manifest["version"] : "local",
            .enabled = manifest.contains("enabled") ? manifest["enabled"] != "false" : true,
            .install_dir = entry.path(),
        });
    }
    return plugins;
}

// 启用指定插件
auto enable_plugin(std::string_view id) -> std::expected<void, std::string> {
    if (id.empty()) {
        return std::unexpected("Plugin ID cannot be empty");
    }
    return set_plugin_enabled(id, true);
}

// 禁用指定插件
auto disable_plugin(std::string_view id) -> std::expected<void, std::string> {
    if (id.empty()) {
        return std::unexpected("Plugin ID cannot be empty");
    }
    return set_plugin_enabled(id, false);
}

// 获取指定插件的详细信息
auto get_plugin_info(std::string_view id) -> std::expected<InstalledPlugin, std::string> {
    if (id.empty()) {
        return std::unexpected("Plugin ID cannot be empty");
    }
    for (const auto& plugin : get_installed_plugins()) {
        if (plugin.id == id) return plugin;
    }
    return std::unexpected("Plugin not found: " + std::string(id));
}

auto plugins_dir() -> path {
    if (const char* home = std::getenv("HOME")) {
        return path{home} / ".cc-repl" / "plugins";
    }
    return path{".cc-repl"} / "plugins";
}

auto read_manifest(const path& manifest_path) -> std::map<std::string, std::string> {
    std::map<std::string, std::string> values;
    std::ifstream input{manifest_path};
    std::string line;
    while (std::getline(input, line)) {
        auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        values[line.substr(0, separator)] = line.substr(separator + 1);
    }
    return values;
}

auto write_manifest(const InstalledPlugin& plugin) -> std::expected<void, std::string> {
    std::filesystem::create_directories(plugin.install_dir);
    std::ofstream output{plugin.install_dir / "manifest.txt", std::ios::trunc};
    if (!output) return std::unexpected("Cannot write plugin manifest: " + plugin.id);
    output << "id=" << plugin.id << '\n'
           << "version=" << plugin.version << '\n'
           << "enabled=" << (plugin.enabled ? "true" : "false") << '\n';
    return {};
}

auto set_plugin_enabled(std::string_view id, bool enabled) -> std::expected<void, std::string> {
    for (auto plugin : get_installed_plugins()) {
        if (plugin.id == id) {
            plugin.enabled = enabled;
            return write_manifest(plugin);
        }
    }
    return std::unexpected("Plugin not found: " + std::string(id));
}

} // namespace cc::commands
