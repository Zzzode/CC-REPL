module;
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <cstdlib>
#include <cstdio>
#include <array>
#include <algorithm>
#include <chrono>

export module cc.cli.handlers.plugins_handler;

export namespace cc::cli::handlers {

// Plugin installation source type
enum class PluginSource {
    Npm,
    Git,
    Local,
    Unknown
};

// Plugin manifest read from a plugin directory
struct PluginManifest {
    std::string name;
    std::string version;
    std::string description;
    std::string author;
    std::string entry_point;
    std::vector<std::string> skills;
    std::vector<std::string> tools;
};

// Plugin state information
struct PluginInfo {
    std::string name;
    std::string version;
    std::string path;
    PluginSource source;
    bool enabled{true};
    std::optional<std::string> error;
};

namespace detail {

inline std::filesystem::path get_plugins_dir() {
    const char* home = std::getenv("HOME");
    if (!home) home = std::getenv("USERPROFILE");
    if (home) {
        return std::filesystem::path(home) / ".config" / "claude-code" / "plugins";
    }
    return std::filesystem::temp_directory_path() / "claude-code-plugins";
}

inline std::filesystem::path get_project_plugins_dir() {
    // Check for .claude/plugins in the project root
    auto cwd = std::filesystem::current_path();
    auto dir = cwd;
    while (true) {
        if (std::filesystem::exists(dir / ".claude" / "plugins")) {
            return dir / ".claude" / "plugins";
        }
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return cwd / ".claude" / "plugins";
}

inline PluginSource detect_source(std::string_view spec) {
    if (spec.starts_with("http://") || spec.starts_with("https://") ||
        spec.starts_with("git@") || spec.starts_with("git://")) {
        return PluginSource::Git;
    }
    if (spec.starts_with("./") || spec.starts_with("../") || spec.starts_with("/")) {
        return PluginSource::Local;
    }
    // Check if it looks like a scoped npm package or plain name
    if (spec.starts_with("@") || spec.find('/') == std::string_view::npos) {
        return PluginSource::Npm;
    }
    // Could be an org/repo format — treat as git
    if (spec.find('/') != std::string_view::npos && !spec.starts_with(".")) {
        return PluginSource::Git;
    }
    return PluginSource::Unknown;
}

inline std::string exec_command(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};
    std::string output;
    std::array<char, 4096> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    pclose(pipe);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
}

inline std::optional<PluginManifest> read_manifest(const std::filesystem::path& plugin_dir) {
    namespace fs = std::filesystem;
    // Look for manifest in order of preference
    std::vector<std::string> manifest_names = {
        "plugin.json", "manifest.json", "package.json"
    };

    for (const auto& name : manifest_names) {
        auto path = plugin_dir / name;
        if (!fs::exists(path)) continue;

        std::ifstream ifs(path);
        std::string content((std::istreambuf_iterator<char>(ifs)),
                           std::istreambuf_iterator<char>());

        PluginManifest manifest;

        // Simple JSON field extraction
        auto extract_field = [&](const std::string& key) -> std::string {
            auto pos = content.find("\"" + key + "\"");
            if (pos == std::string::npos) return {};
            auto colon = content.find(':', pos);
            if (colon == std::string::npos) return {};
            auto q1 = content.find('"', colon);
            if (q1 == std::string::npos) return {};
            auto q2 = content.find('"', q1 + 1);
            if (q2 == std::string::npos) return {};
            return content.substr(q1 + 1, q2 - q1 - 1);
        };

        manifest.name = extract_field("name");
        manifest.version = extract_field("version");
        manifest.description = extract_field("description");
        manifest.author = extract_field("author");
        manifest.entry_point = extract_field("main");
        if (manifest.entry_point.empty()) {
            manifest.entry_point = extract_field("entry");
        }

        if (!manifest.name.empty()) {
            return manifest;
        }
    }
    return std::nullopt;
}

inline std::vector<PluginInfo> scan_plugins_directory(const std::filesystem::path& plugins_dir) {
    namespace fs = std::filesystem;
    std::vector<PluginInfo> plugins;

    if (!fs::exists(plugins_dir) || !fs::is_directory(plugins_dir)) {
        return plugins;
    }

    for (const auto& entry : fs::directory_iterator(plugins_dir)) {
        if (!entry.is_directory()) continue;

        PluginInfo info;
        info.path = entry.path().string();
        info.name = entry.path().filename().string();

        auto manifest = read_manifest(entry.path());
        if (manifest) {
            info.name = manifest->name.empty() ? info.name : manifest->name;
            info.version = manifest->version;
        } else {
            info.error = "No valid manifest found";
            info.enabled = false;
        }

        // Detect source from a marker file
        if (fs::exists(entry.path() / ".git")) {
            info.source = PluginSource::Git;
        } else if (fs::is_symlink(entry.path())) {
            info.source = PluginSource::Local;
        } else {
            info.source = PluginSource::Npm;
        }

        plugins.push_back(std::move(info));
    }

    return plugins;
}

} // namespace detail

// Forward declarations
std::expected<void, std::string> install_plugin_cli(std::string_view spec);
std::string list_plugins_cli();
std::expected<void, std::string> uninstall_plugin_cli(std::string_view name);

// Handle plugin management CLI commands
std::expected<std::string, std::string> handle_plugins_command(std::span<std::string> args) {
    if (args.empty()) {
        return std::string(list_plugins_cli());
    }

    std::string subcommand(args[0]);

    if (subcommand == "list" || subcommand == "ls") {
        return std::string(list_plugins_cli());
    }

    if (subcommand == "install" || subcommand == "add") {
        if (args.size() < 2) {
            return std::unexpected("Usage: plugins install <plugin_spec>\n"
                "  Spec formats:\n"
                "    package-name       (npm registry)\n"
                "    @scope/package     (scoped npm)\n"
                "    https://...git     (git URL)\n"
                "    ./local/path       (local directory)");
        }
        auto result = install_plugin_cli(args[1]);
        if (result.has_value()) {
            return std::string("Plugin '" + std::string(args[1]) + "' installed successfully.");
        }
        return std::unexpected(result.error());
    }

    if (subcommand == "uninstall" || subcommand == "remove" || subcommand == "rm") {
        if (args.size() < 2) {
            return std::unexpected("Usage: plugins uninstall <plugin_name>");
        }
        auto result = uninstall_plugin_cli(args[1]);
        if (result.has_value()) {
            return std::string("Plugin '" + std::string(args[1]) + "' uninstalled.");
        }
        return std::unexpected(result.error());
    }

    if (subcommand == "info") {
        if (args.size() < 2) {
            return std::unexpected("Usage: plugins info <plugin_name>");
        }
        auto plugins_dir = detail::get_plugins_dir();
        auto plugin_path = plugins_dir / std::string(args[1]);
        if (!std::filesystem::exists(plugin_path)) {
            return std::unexpected("Plugin '" + std::string(args[1]) + "' is not installed.");
        }
        auto manifest = detail::read_manifest(plugin_path);
        if (!manifest) {
            return std::unexpected("Plugin found but has no valid manifest.");
        }
        std::string info = "Plugin: " + manifest->name + "\n";
        info += "  Version: " + manifest->version + "\n";
        info += "  Description: " + manifest->description + "\n";
        info += "  Author: " + manifest->author + "\n";
        info += "  Entry: " + manifest->entry_point + "\n";
        info += "  Path: " + plugin_path.string() + "\n";
        return info;
    }

    if (subcommand == "enable") {
        if (args.size() < 2) {
            return std::unexpected("Usage: plugins enable <plugin_name>");
        }
        auto plugins_dir = detail::get_plugins_dir();
        auto disabled_marker = plugins_dir / std::string(args[1]) / ".disabled";
        std::filesystem::remove(disabled_marker);
        return std::string("Plugin '" + std::string(args[1]) + "' enabled.");
    }

    if (subcommand == "disable") {
        if (args.size() < 2) {
            return std::unexpected("Usage: plugins disable <plugin_name>");
        }
        auto plugins_dir = detail::get_plugins_dir();
        auto plugin_path = plugins_dir / std::string(args[1]);
        if (!std::filesystem::exists(plugin_path)) {
            return std::unexpected("Plugin '" + std::string(args[1]) + "' is not installed.");
        }
        auto disabled_marker = plugin_path / ".disabled";
        std::ofstream(disabled_marker) << "disabled\n";
        return std::string("Plugin '" + std::string(args[1]) + "' disabled.");
    }

    return std::unexpected("Unknown plugins subcommand: " + subcommand +
        "\nAvailable: list, install, uninstall, info, enable, disable");
}

// Install a plugin from a spec string (npm package, git URL, or local path)
std::expected<void, std::string> install_plugin_cli(std::string_view spec) {
    namespace fs = std::filesystem;

    if (spec.empty()) {
        return std::unexpected("Plugin spec cannot be empty");
    }

    auto source = detail::detect_source(spec);
    auto plugins_dir = detail::get_plugins_dir();

    // Ensure plugins directory exists
    std::error_code ec;
    fs::create_directories(plugins_dir, ec);
    if (ec) {
        return std::unexpected("Failed to create plugins directory: " + ec.message());
    }

    std::string spec_str(spec);

    switch (source) {
    case PluginSource::Git: {
        // Clone the git repository into plugins directory
        std::string repo_name;
        // Extract repo name from URL
        auto last_slash = spec_str.rfind('/');
        if (last_slash != std::string::npos) {
            repo_name = spec_str.substr(last_slash + 1);
            // Remove .git suffix
            if (repo_name.ends_with(".git")) {
                repo_name = repo_name.substr(0, repo_name.size() - 4);
            }
        } else {
            repo_name = spec_str;
        }

        auto target_dir = plugins_dir / repo_name;
        if (fs::exists(target_dir)) {
            // Update existing plugin
            std::string cmd = "cd " + target_dir.string() + " && git pull 2>&1";
            auto output = detail::exec_command(cmd);
            if (output.find("fatal") != std::string::npos ||
                output.find("error") != std::string::npos) {
                return std::unexpected("Failed to update plugin: " + output);
            }
        } else {
            std::string cmd = "git clone " + spec_str + " " + target_dir.string() + " 2>&1";
            auto output = detail::exec_command(cmd);
            if (output.find("fatal") != std::string::npos) {
                return std::unexpected("Failed to clone plugin: " + output);
            }
        }

        // Validate manifest exists
        auto manifest = detail::read_manifest(target_dir);
        if (!manifest) {
            fs::remove_all(target_dir, ec);
            return std::unexpected(
                "Cloned repository has no valid plugin manifest (plugin.json or package.json)");
        }

        // Run install hook if present
        if (fs::exists(target_dir / "package.json")) {
            std::string install_cmd = "cd " + target_dir.string() + " && npm install --production 2>&1";
            detail::exec_command(install_cmd);
        }
        return {};
    }

    case PluginSource::Local: {
        // Create symlink to local directory
        auto source_path = fs::absolute(fs::path(spec_str));
        if (!fs::exists(source_path)) {
            return std::unexpected("Local plugin path does not exist: " + source_path.string());
        }
        if (!fs::is_directory(source_path)) {
            return std::unexpected("Local plugin path is not a directory: " + source_path.string());
        }

        // Validate manifest
        auto manifest = detail::read_manifest(source_path);
        if (!manifest) {
            return std::unexpected("Local directory has no valid plugin manifest");
        }

        auto link_name = source_path.filename();
        auto link_path = plugins_dir / link_name;

        if (fs::exists(link_path)) {
            fs::remove(link_path, ec);
        }
        fs::create_symlink(source_path, link_path, ec);
        if (ec) {
            return std::unexpected("Failed to create symlink: " + ec.message());
        }
        return {};
    }

    case PluginSource::Npm: {
        // Install via npm to the plugins directory
        auto target_dir = plugins_dir / spec_str;
        std::string cmd = "npm install --prefix " + plugins_dir.string() +
            " " + spec_str + " 2>&1";
        auto output = detail::exec_command(cmd);
        if (output.find("ERR!") != std::string::npos) {
            return std::unexpected("npm install failed: " + output);
        }

        // npm puts packages in node_modules
        auto nm_path = plugins_dir / "node_modules" / spec_str;
        if (!fs::exists(nm_path)) {
            return std::unexpected("Package installed but not found at expected path");
        }

        // Move to top-level plugins dir for simpler management
        auto final_path = plugins_dir / spec_str;
        if (fs::exists(final_path) && final_path != nm_path) {
            fs::remove_all(final_path, ec);
        }
        if (nm_path != final_path) {
            fs::rename(nm_path, final_path, ec);
            if (ec) {
                // Fallback: just leave it in node_modules
            }
        }
        return {};
    }

    default:
        return std::unexpected("Cannot determine plugin source type for: " + spec_str);
    }
}

// Uninstall a plugin by name
std::expected<void, std::string> uninstall_plugin_cli(std::string_view name) {
    namespace fs = std::filesystem;

    if (name.empty()) {
        return std::unexpected("Plugin name cannot be empty");
    }

    auto plugins_dir = detail::get_plugins_dir();
    auto plugin_path = plugins_dir / std::string(name);

    if (!fs::exists(plugin_path)) {
        return std::unexpected("Plugin '" + std::string(name) + "' is not installed");
    }

    std::error_code ec;
    fs::remove_all(plugin_path, ec);
    if (ec) {
        return std::unexpected("Failed to remove plugin: " + ec.message());
    }

    return {};
}

// List all installed plugins with their status
std::string list_plugins_cli() {
    auto global_plugins = detail::scan_plugins_directory(detail::get_plugins_dir());
    auto project_plugins = detail::scan_plugins_directory(detail::get_project_plugins_dir());

    if (global_plugins.empty() && project_plugins.empty()) {
        return "No plugins installed.\n\n"
               "To install a plugin:\n"
               "  plugins install <package_name>    (from npm)\n"
               "  plugins install <git_url>         (from git)\n"
               "  plugins install ./local/path      (local directory)\n";
    }

    std::string output;

    if (!global_plugins.empty()) {
        output += "Global plugins (~/.config/claude-code/plugins/):\n";
        for (const auto& plugin : global_plugins) {
            output += "  " + plugin.name;
            if (!plugin.version.empty()) output += " v" + plugin.version;
            if (!plugin.enabled) output += " [disabled]";
            if (plugin.error) output += " [error: " + *plugin.error + "]";
            output += "\n";
        }
    }

    if (!project_plugins.empty()) {
        if (!output.empty()) output += "\n";
        output += "Project plugins (.claude/plugins/):\n";
        for (const auto& plugin : project_plugins) {
            output += "  " + plugin.name;
            if (!plugin.version.empty()) output += " v" + plugin.version;
            if (!plugin.enabled) output += " [disabled]";
            if (plugin.error) output += " [error: " + *plugin.error + "]";
            output += "\n";
        }
    }

    return output;
}

} // namespace cc::cli::handlers
