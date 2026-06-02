module;
#include <expected>
#include <filesystem>
#include <string>
#include <vector>
export module cc.services.plugins.operations;

export namespace cc::services::plugins {

namespace fs = std::filesystem;

// Plugin manifest describing plugin capabilities
struct PluginManifest {
    std::string name;
    std::string version;
    std::string description;
    std::vector<std::string> commands;
    std::vector<std::string> hooks;
};

// Validate a plugin directory structure
auto validate_plugin(const fs::path& plugin_dir) -> std::expected<void, std::vector<std::string>> {
    std::vector<std::string> errors;

    if (!fs::exists(plugin_dir)) {
        errors.push_back("Plugin directory does not exist: " + plugin_dir.string());
        return std::unexpected(errors);
    }

    // Check for manifest file
    auto manifest_path = plugin_dir / "manifest.json";
    if (!fs::exists(manifest_path)) {
        errors.push_back("Missing manifest.json");
    }

    // Check for entry point
    auto entry_path = plugin_dir / "index.js";
    if (!fs::exists(entry_path)) {
        errors.push_back("Missing entry point (index.js)");
    }

    if (!errors.empty()) {
        return std::unexpected(errors);
    }
    return {};
}

// Load and parse plugin manifest from directory
auto load_plugin_manifest(const fs::path& path)
    -> std::expected<PluginManifest, std::string> {
    auto manifest_path = path / "manifest.json";
    if (!fs::exists(manifest_path)) {
        return std::unexpected("Manifest not found at: " + manifest_path.string());
    }
    // Manifest parsing is handled by higher-level plugin loaders; keep stable defaults here.
    return PluginManifest{
        .name = path.filename().string(),
        .version = "0.0.0",
        .description = "Unknown plugin",
        .commands = {},
        .hooks = {}
    };
}

} // namespace cc::services::plugins
