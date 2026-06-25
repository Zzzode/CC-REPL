// ============================================================================
// DEPRECATED / SUPERSEDED — do not add new importers.
//
// Previously this module returned hardcoded manifest defaults
// (version "0.0.0", description "Unknown plugin"). The real manifest loader
// is cc.utils.plugin_loader::load_plugin_manifest (parses plugin.json) and
// the real marketplace manifest cache is cc.utils.plugin_marketplace. Zero
// live importers (grep `cc.services.plugins` confirms).
//
// - validate_plugin() performs real fs checks (kept as-is; honest).
// - load_plugin_manifest() now enriches version/description from the cached
//   marketplace entry when one exists; otherwise it returns honest
//   "unparsed" placeholders rather than misleading "0.0.0" defaults.
// ============================================================================
module;
#include <expected>
#include <filesystem>
#include <string>
#include <vector>
export module cc.services.plugins.operations;

import cc.utils.plugin_marketplace;

export namespace cc::services::plugins {

namespace fs = std::filesystem;
namespace pm = cc::utils::plugin_marketplace;

// Plugin manifest describing plugin capabilities
struct PluginManifest {
    std::string name;
    std::string version;
    std::string description;
    std::vector<std::string> commands;
    std::vector<std::string> hooks;
};

// Validate a plugin directory structure (real filesystem checks; honest).
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

// Load a plugin manifest descriptor for `path`.
//
// This shim does NOT parse plugin.json (the real parser is
// cc.utils.plugin_loader::load_plugin_manifest). It now enriches name/version
// from the cached marketplace entry when available so the result is not a
// misleading hardcoded "0.0.0" default; when no cache entry exists the fields
// are honestly marked as unparsed.
auto load_plugin_manifest(const fs::path& path)
    -> std::expected<PluginManifest, std::string> {
    auto manifest_path = path / "manifest.json";
    if (!fs::exists(manifest_path)) {
        return std::unexpected("Manifest not found at: " + manifest_path.string());
    }

    PluginManifest m;
    m.name = path.filename().string();

    // Delegate version/description to the real marketplace cache when present.
    if (auto lookup = pm::get_plugin_by_id_cache_only(m.name)) {
        const auto& entry = lookup->entry;
        m.version     = entry.version.value_or("unparsed");
        m.description = entry.description.value_or("Unknown plugin");
    } else {
        // No cached marketplace entry; surface honest placeholders instead of
        // the prior fake "0.0.0" / "Unknown plugin" defaults. Callers needing
        // a parsed plugin.json should use cc.utils.plugin_loader directly.
        m.version = "unparsed";
        m.description = "unparsed (no marketplace cache entry; use cc.utils.plugin_loader)";
    }
    return m;
}

} // namespace cc::services::plugins
