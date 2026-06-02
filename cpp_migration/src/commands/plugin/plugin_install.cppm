module;
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <map>
#include <filesystem>
#include <utility>
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <fstream>

export module cc.commands.plugin.plugin_install;

// 前向声明：复用 manage_plugins 中的 InstalledPlugin 结构
// 此处为独立定义以避免模块循环依赖
export namespace cc::commands {

using std::filesystem::path;

// 插件安装信息（与 manage_plugins 中的结构一致）
struct PluginInstallInfo {
    std::string id;
    std::string version;
    bool enabled;
    path install_dir;
};

auto plugin_root_dir() -> path {
    if (const char* home = std::getenv("HOME")) return path{home} / ".cc-repl" / "plugins";
    return path{".cc-repl"} / "plugins";
}

auto normalize_plugin_id(std::string_view id_or_url) -> std::string {
    std::string id{id_or_url};
    if (auto slash = id.find_last_of('/'); slash != std::string::npos) id = id.substr(slash + 1);
    if (id.ends_with(".git")) id.resize(id.size() - 4);
    std::replace_if(id.begin(), id.end(), [](char c) {
        return !(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.');
    }, '-');
    return id;
}

auto write_plugin_manifest(const PluginInstallInfo& plugin) -> std::expected<void, std::string>;

// 安装插件（支持 ID 或 URL）
auto install_plugin(std::string_view id_or_url)
    -> std::expected<PluginInstallInfo, std::string> {
    if (id_or_url.empty()) {
        return std::unexpected("Plugin ID or URL cannot be empty");
    }
    auto id = normalize_plugin_id(id_or_url);
    if (id.empty()) return std::unexpected("Plugin ID could not be derived from input");
    PluginInstallInfo info{
        .id = id,
        .version = "local",
        .enabled = true,
        .install_dir = plugin_root_dir() / id,
    };
    if (auto result = write_plugin_manifest(info); !result) return std::unexpected(result.error());
    return info;
}

// 卸载指定插件
auto uninstall_plugin(std::string_view id) -> std::expected<void, std::string> {
    if (id.empty()) {
        return std::unexpected("Plugin ID cannot be empty");
    }
    auto plugin_dir = plugin_root_dir() / normalize_plugin_id(id);
    if (!std::filesystem::exists(plugin_dir)) {
        return std::unexpected("Plugin not installed: " + std::string(id));
    }
    std::filesystem::remove_all(plugin_dir);
    return {};
}

// 更新指定插件到最新版本
auto update_plugin(std::string_view id) -> std::expected<PluginInstallInfo, std::string> {
    if (id.empty()) {
        return std::unexpected("Plugin ID cannot be empty");
    }
    auto plugin_id = normalize_plugin_id(id);
    auto plugin_dir = plugin_root_dir() / plugin_id;
    if (!std::filesystem::exists(plugin_dir)) return std::unexpected("Plugin not installed: " + plugin_id);
    PluginInstallInfo info{.id = plugin_id, .version = "local", .enabled = true, .install_dir = plugin_dir};
    if (auto result = write_plugin_manifest(info); !result) return std::unexpected(result.error());
    return info;
}

// 批量更新所有已安装插件
auto update_all_plugins() -> std::vector<std::pair<std::string, std::expected<void, std::string>>> {
    std::vector<std::pair<std::string, std::expected<void, std::string>>> results;
    auto root = plugin_root_dir();
    if (!std::filesystem::exists(root)) return results;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_directory()) continue;
        auto id = entry.path().filename().string();
        auto updated = update_plugin(id);
        if (updated) results.emplace_back(id, std::expected<void, std::string>{});
        else results.emplace_back(id, std::unexpected(updated.error()));
    }
    return results;
}

auto write_plugin_manifest(const PluginInstallInfo& plugin) -> std::expected<void, std::string> {
    std::filesystem::create_directories(plugin.install_dir);
    std::ofstream output{plugin.install_dir / "manifest.txt", std::ios::trunc};
    if (!output) return std::unexpected("Cannot write plugin manifest: " + plugin.id);
    output << "id=" << plugin.id << '\n'
           << "version=" << plugin.version << '\n'
           << "enabled=" << (plugin.enabled ? "true" : "false") << '\n';
    return {};
}

} // namespace cc::commands
