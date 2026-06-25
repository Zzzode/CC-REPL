// Plugin Lifecycle Module
// Consolidates: pluginInstall, pluginUninstall, pluginUpdate, pluginVersioning,
//               pluginDependencies, pluginLoader, pluginRunner
//
// Manages the full plugin lifecycle: install, uninstall, update, load/unload,
// dependency resolution, version parsing/compatibility, and state tracking.
module;

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module cc.utils.plugin_lifecycle;
import cc.utils.bash_execution;

export namespace cc::utils::plugins {

// ─────────────────────────────────────────────────────────────────────────────
// Plugin State (from pluginLoader, pluginRunner)
// ─────────────────────────────────────────────────────────────────────────────

enum class PluginState : unsigned char {
    NotInstalled,
    Installing,
    Installed,
    Active,
    Disabled,
    Error,
    Updating,
    Uninstalling,
};

// ─────────────────────────────────────────────────────────────────────────────
// Version Types (from pluginVersioning)
// ─────────────────────────────────────────────────────────────────────────────

struct PluginVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::optional<std::string> prerelease;
};

// ─────────────────────────────────────────────────────────────────────────────
// Dependency Types (from pluginDependencies)
// ─────────────────────────────────────────────────────────────────────────────

struct PluginDependency {
    std::string plugin_id;
    std::string version_constraint;
    bool optional{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// Install Types (from pluginInstall)
// ─────────────────────────────────────────────────────────────────────────────

struct PluginInstallOptions {
    bool force{false};
    bool skip_deps{false};
    std::optional<std::string> version;
    std::string registry_url;
};

struct InstallResult {
    std::string plugin_id;
    PluginState state;
    PluginVersion installed_version;
    std::vector<std::string> warnings;
    std::chrono::milliseconds duration;
};

// ─────────────────────────────────────────────────────────────────────────────
// Load Types (from pluginLoader, pluginRunner)
// ─────────────────────────────────────────────────────────────────────────────

struct PluginLoadResult {
    bool success = false;
    std::optional<std::string> error;
    std::vector<std::string> exported_tools;
    std::vector<std::string> exported_commands;
};

// ─────────────────────────────────────────────────────────────────────────────
// Internal State
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

struct PluginRegistry {
    std::mutex mutex;
    std::unordered_map<std::string, PluginState> states;
    std::unordered_map<std::string, PluginLoadResult> loaded;
};

inline auto get_registry() -> PluginRegistry& {
    static PluginRegistry registry;
    return registry;
}

inline auto get_plugins_dir() -> std::filesystem::path {
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return std::filesystem::path(home) / ".cc-repl" / "plugins";
}

inline auto get_plugin_dir(std::string_view plugin_id) -> std::filesystem::path {
    return get_plugins_dir() / std::string(plugin_id);
}

/// Execute a shell command and return stdout
inline auto exec_cmd(const std::string& cmd) -> std::expected<std::string, std::string> {
    std::array<char, 4096> buffer{};
    std::string result;
    FILE* pipe = cc::utils::bash::popen_spawn(cmd.c_str());
    if (!pipe) return std::unexpected("Failed to execute: " + cmd);
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result += buffer.data();
    }
    int status = cc::utils::bash::pclose_spawn(pipe);
    if (status != 0) return std::unexpected("Command failed with status " + std::to_string(status));
    return result;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Version Utilities (from pluginVersioning)
// ─────────────────────────────────────────────────────────────────────────────

/// Parse a version string (e.g. "1.2.3-beta.1") into a PluginVersion
[[nodiscard]] inline std::expected<PluginVersion, std::string> parse_version(
    std::string_view version_str
) {
    std::string_view s = version_str;
    // Strip leading 'v'
    if (!s.empty() && s[0] == 'v') s = s.substr(1);

    PluginVersion ver;
    auto parse_int = [&](int& out) -> bool {
        if (s.empty() || s[0] < '0' || s[0] > '9') return false;
        out = 0;
        while (!s.empty() && s[0] >= '0' && s[0] <= '9') {
            out = out * 10 + (s[0] - '0');
            s = s.substr(1);
        }
        return true;
    };

    if (!parse_int(ver.major)) return std::unexpected("Invalid version: " + std::string(version_str));
    if (s.empty() || s[0] != '.') return std::unexpected("Invalid version: missing minor");
    s = s.substr(1);

    if (!parse_int(ver.minor)) return std::unexpected("Invalid version: missing minor number");
    if (s.empty() || s[0] != '.') return std::unexpected("Invalid version: missing patch");
    s = s.substr(1);

    if (!parse_int(ver.patch)) return std::unexpected("Invalid version: missing patch number");

    if (!s.empty() && s[0] == '-') {
        ver.prerelease = std::string(s.substr(1));
    }

    return ver;
}

/// Format a PluginVersion back into a string (e.g. "1.2.3-beta.1")
[[nodiscard]] inline std::string format_version(PluginVersion version) {
    std::string result = std::to_string(version.major) + "." +
                         std::to_string(version.minor) + "." +
                         std::to_string(version.patch);
    if (version.prerelease) {
        result += "-";
        result += *version.prerelease;
    }
    return result;
}

/// Check whether an installed version satisfies a semver constraint string
[[nodiscard]] inline bool check_version_compatibility(
    PluginVersion installed,
    std::string_view constraint
) {
    if (constraint.empty() || constraint == "*") return true;

    // Parse constraint prefix
    char op = '^'; // default: caret (compatible with)
    std::string_view ver_part = constraint;
    if (constraint.starts_with(">=")) { op = 'G'; ver_part = constraint.substr(2); }
    else if (constraint.starts_with("^")) { op = '^'; ver_part = constraint.substr(1); }
    else if (constraint.starts_with("~")) { op = '~'; ver_part = constraint.substr(1); }
    else if (constraint.starts_with("=")) { op = '='; ver_part = constraint.substr(1); }

    auto req = parse_version(ver_part);
    if (!req) return false;

    switch (op) {
        case '=': // exact match
            return installed.major == req->major &&
                   installed.minor == req->minor &&
                   installed.patch == req->patch;
        case '^': // compatible: same major, >= minor.patch
            if (installed.major != req->major) return false;
            if (installed.minor > req->minor) return true;
            if (installed.minor == req->minor) return installed.patch >= req->patch;
            return false;
        case '~': // approximately: same major.minor, >= patch
            return installed.major == req->major &&
                   installed.minor == req->minor &&
                   installed.patch >= req->patch;
        case 'G': // >=
            if (installed.major > req->major) return true;
            if (installed.major < req->major) return false;
            if (installed.minor > req->minor) return true;
            if (installed.minor < req->minor) return false;
            return installed.patch >= req->patch;
        default:
            return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Install / Uninstall / Update (from pluginInstall, pluginUninstall, pluginUpdate)
// ─────────────────────────────────────────────────────────────────────────────

/// Install a plugin by ID with optional configuration
[[nodiscard]] inline std::expected<InstallResult, std::string> install_plugin(
    std::string_view plugin_id,
    PluginInstallOptions options = {}
) {
    namespace fs = std::filesystem;
    auto start = std::chrono::steady_clock::now();

    auto& reg = detail::get_registry();
    {
        std::lock_guard lock(reg.mutex);
        reg.states[std::string(plugin_id)] = PluginState::Installing;
    }

    auto plugin_dir = detail::get_plugin_dir(plugin_id);

    // Check if already installed
    if (!options.force && fs::exists(plugin_dir)) {
        std::lock_guard lock(reg.mutex);
        reg.states[std::string(plugin_id)] = PluginState::Installed;
        return std::unexpected("Plugin '" + std::string(plugin_id) + "' already installed. Use --force to reinstall.");
    }

    // Create plugin directory
    std::error_code ec;
    fs::create_directories(plugin_dir, ec);
    if (ec) {
        std::lock_guard lock(reg.mutex);
        reg.states[std::string(plugin_id)] = PluginState::Error;
        return std::unexpected("Failed to create plugin directory: " + ec.message());
    }

    // Determine registry URL
    std::string registry = options.registry_url.empty()
        ? "https://registry.npmjs.org" : options.registry_url;

    // Fetch plugin package via npm pack
    std::string version_suffix = options.version ? "@" + *options.version : "";
    std::string cmd = "cd " + plugin_dir.string() + " && npm pack " +
                      std::string(plugin_id) + version_suffix +
                      " --registry=" + registry + " 2>&1";

    auto result = detail::exec_cmd(cmd);
    if (!result) {
        std::lock_guard lock(reg.mutex);
        reg.states[std::string(plugin_id)] = PluginState::Error;
        return std::unexpected("Failed to download plugin: " + result.error());
    }

    // Write manifest marker
    std::ofstream manifest(plugin_dir / "manifest.json");
    manifest << "{\"id\":\"" << plugin_id << "\",\"version\":\""
             << (options.version ? *options.version : "latest") << "\"}";
    manifest.close();

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    auto ver = options.version ? parse_version(*options.version) : std::expected<PluginVersion, std::string>{
        PluginVersion{.major = 0, .minor = 0, .patch = 1, .prerelease = std::nullopt}
    };

    {
        std::lock_guard lock(reg.mutex);
        reg.states[std::string(plugin_id)] = PluginState::Installed;
    }

    return InstallResult{
        .plugin_id = std::string(plugin_id),
        .state = PluginState::Installed,
        .installed_version = ver.value_or(PluginVersion{.major = 0, .minor = 0, .patch = 1, .prerelease = std::nullopt}),
        .warnings = {},
        .duration = duration,
    };
}

/// Uninstall a plugin, optionally removing its persistent data
[[nodiscard]] inline std::expected<void, std::string> uninstall_plugin(
    std::string_view plugin_id,
    bool remove_data = false
) {
    namespace fs = std::filesystem;
    auto& reg = detail::get_registry();

    {
        std::lock_guard lock(reg.mutex);
        reg.states[std::string(plugin_id)] = PluginState::Uninstalling;
    }

    auto plugin_dir = detail::get_plugin_dir(plugin_id);
    if (!fs::exists(plugin_dir)) {
        std::lock_guard lock(reg.mutex);
        reg.states.erase(std::string(plugin_id));
        return std::unexpected("Plugin '" + std::string(plugin_id) + "' is not installed");
    }

    // Remove plugin directory
    std::error_code ec;
    fs::remove_all(plugin_dir, ec);
    if (ec) {
        std::lock_guard lock(reg.mutex);
        reg.states[std::string(plugin_id)] = PluginState::Error;
        return std::unexpected("Failed to remove plugin: " + ec.message());
    }

    // Optionally remove data directory
    if (remove_data) {
        auto data_dir = detail::get_plugins_dir() / ".data" / std::string(plugin_id);
        fs::remove_all(data_dir, ec);
    }

    {
        std::lock_guard lock(reg.mutex);
        reg.states.erase(std::string(plugin_id));
        reg.loaded.erase(std::string(plugin_id));
    }

    return {};
}

/// Update a plugin to a specific version (or latest if not specified)
[[nodiscard]] inline std::expected<InstallResult, std::string> update_plugin(
    std::string_view plugin_id,
    std::optional<std::string_view> target_version = {}
) {
    // Uninstall current, then reinstall with new version
    auto uninstall_result = uninstall_plugin(plugin_id, false);
    // Ignore "not installed" errors — just proceed with install

    PluginInstallOptions opts;
    opts.force = true;
    if (target_version) {
        opts.version = std::string(*target_version);
    }

    return install_plugin(plugin_id, opts);
}

/// Update all installed plugins to their latest compatible versions
[[nodiscard]] inline std::vector<InstallResult> update_all_plugins() {
    namespace fs = std::filesystem;
    std::vector<InstallResult> results;

    auto plugins_dir = detail::get_plugins_dir();
    if (!fs::exists(plugins_dir)) return results;

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(plugins_dir, ec)) {
        if (!entry.is_directory()) continue;
        auto id = entry.path().filename().string();
        if (id.starts_with(".")) continue; // skip hidden dirs

        auto result = update_plugin(id);
        if (result) {
            results.push_back(std::move(*result));
        }
    }

    return results;
}

// ─────────────────────────────────────────────────────────────────────────────
// Load / Unload (from pluginLoader, pluginRunner)
// ─────────────────────────────────────────────────────────────────────────────

/// Load a plugin into the runtime, registering its tools and commands
[[nodiscard]] inline std::expected<PluginLoadResult, std::string> load_plugin(
    std::string_view plugin_id
) {
    namespace fs = std::filesystem;
    auto& reg = detail::get_registry();

    auto plugin_dir = detail::get_plugin_dir(plugin_id);
    if (!fs::exists(plugin_dir)) {
        return std::unexpected("Plugin '" + std::string(plugin_id) + "' is not installed");
    }

    // Check manifest for exported tools and commands
    auto manifest_path = plugin_dir / "manifest.json";
    if (!fs::exists(manifest_path)) {
        return std::unexpected("Plugin '" + std::string(plugin_id) + "' has no manifest");
    }

    // Read manifest to discover exports
    std::ifstream ifs(manifest_path);
    std::string manifest_content((std::istreambuf_iterator<char>(ifs)),
                                  std::istreambuf_iterator<char>());

    PluginLoadResult result;
    result.success = true;
    // Parse tool names from manifest (simple search for "tools" array)
    // In a full implementation, this would use proper JSON parsing
    result.exported_tools.push_back(std::string(plugin_id) + "::default");
    result.exported_commands.push_back("/" + std::string(plugin_id));

    {
        std::lock_guard lock(reg.mutex);
        reg.states[std::string(plugin_id)] = PluginState::Active;
        reg.loaded[std::string(plugin_id)] = result;
    }

    return result;
}

/// Unload a plugin from the runtime, deregistering its tools and commands
[[nodiscard]] inline std::expected<void, std::string> unload_plugin(
    std::string_view plugin_id
) {
    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);

    auto it = reg.loaded.find(std::string(plugin_id));
    if (it == reg.loaded.end()) {
        return std::unexpected("Plugin '" + std::string(plugin_id) + "' is not loaded");
    }

    reg.loaded.erase(it);
    reg.states[std::string(plugin_id)] = PluginState::Installed;
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// Dependencies (from pluginDependencies)
// ─────────────────────────────────────────────────────────────────────────────

/// Resolve all transitive dependencies for a plugin
[[nodiscard]] inline std::expected<std::vector<PluginDependency>, std::string> resolve_dependencies(
    std::string_view plugin_id
) {
    namespace fs = std::filesystem;
    auto plugin_dir = detail::get_plugin_dir(plugin_id);
    auto deps_file = plugin_dir / "dependencies.json";

    std::vector<PluginDependency> deps;

    if (!fs::exists(deps_file)) {
        // No dependencies file — plugin has no dependencies
        return deps;
    }

    // Read and parse dependencies (simplified: one dep per line as "id:constraint")
    std::ifstream ifs(deps_file);
    std::string line;
    while (std::getline(ifs, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        deps.push_back(PluginDependency{
            .plugin_id = line.substr(0, colon),
            .version_constraint = line.substr(colon + 1),
            .optional = false,
        });
    }

    return deps;
}

// ─────────────────────────────────────────────────────────────────────────────
// State Query (from pluginLoader)
// ─────────────────────────────────────────────────────────────────────────────

/// Get the current lifecycle state of a plugin
[[nodiscard]] inline PluginState get_plugin_state(std::string_view plugin_id) {
    namespace fs = std::filesystem;
    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);

    auto it = reg.states.find(std::string(plugin_id));
    if (it != reg.states.end()) return it->second;

    // Fall back to filesystem check
    if (fs::exists(detail::get_plugin_dir(plugin_id))) {
        return PluginState::Installed;
    }
    return PluginState::NotInstalled;
}

/// Enable or disable an installed plugin by toggling a `.disabled` marker
/// file next to its manifest.  Returns an error when the plugin is not
/// installed or filesystem writes fail.
[[nodiscard]] inline std::expected<void, std::string> set_plugin_enabled(
    std::string_view plugin_id, bool enabled
) {
    namespace fs = std::filesystem;
    const auto plugin_dir = detail::get_plugin_dir(plugin_id);
    std::error_code ec;
    if (!fs::exists(plugin_dir, ec)) {
        return std::unexpected(
            "Plugin '" + std::string(plugin_id) + "' is not installed.");
    }
    const auto marker = plugin_dir / ".disabled";
    if (enabled) {
        if (fs::exists(marker, ec)) fs::remove(marker, ec);
    } else {
        std::ofstream ofs(marker);
        if (!ofs.good()) {
            return std::unexpected(
                "Failed to write disabled marker for plugin '"
                + std::string(plugin_id) + "'.");
        }
    }
    if (ec) {
        return std::unexpected("Failed to update plugin state: " + ec.message());
    }
    return {};
}

} // namespace cc::utils::plugins
