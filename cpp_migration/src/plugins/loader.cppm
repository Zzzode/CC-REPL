/// @file loader.cppm
/// @brief Plugin loader - discovery, validation, process management, and caching.
/// Discovers plugins from filesystem, validates manifests, manages lifecycle.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <expected>
#include <format>
#include <ranges>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <algorithm>

#include <uv.h>

export module cc.plugins.loader;

import cc.types.types;
import cc.plugins.plugin;
import cc.utils.json;

export namespace cc::plugins {

using cc::core::Result;
using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::VoidResult;

// ============================================================
// Version Compatibility
// ============================================================

/// Semantic version parsed into components
struct SemVer {
    std::uint16_t major = 0;
    std::uint16_t minor = 0;
    std::uint16_t patch = 0;

    /// Parse a "major.minor.patch" string
    [[nodiscard]] static std::optional<SemVer> parse(std::string_view str) {
        SemVer v{};
        auto dot1 = str.find('.');
        if (dot1 == std::string_view::npos) return std::nullopt;
        auto dot2 = str.find('.', dot1 + 1);
        if (dot2 == std::string_view::npos) return std::nullopt;

        auto to_uint = [](std::string_view s) -> std::optional<uint16_t> {
            uint16_t val = 0;
            for (char c : s) {
                if (c < '0' || c > '9') return std::nullopt;
                val = val * 10 + (c - '0');
            }
            return val;
        };

        auto maj = to_uint(str.substr(0, dot1));
        auto min = to_uint(str.substr(dot1 + 1, dot2 - dot1 - 1));
        auto pat = to_uint(str.substr(dot2 + 1));
        if (!maj || !min || !pat) return std::nullopt;

        v.major = *maj; v.minor = *min; v.patch = *pat;
        return v;
    }

    /// Check if this version satisfies a "^major.minor.patch" constraint
    [[nodiscard]] bool satisfies_caret(const SemVer& constraint) const noexcept {
        if (major != constraint.major) return false;
        if (major == 0) {
            // Pre-1.0: minor must match exactly
            return minor == constraint.minor && patch >= constraint.patch;
        }
        // Post-1.0: minor.patch must be >=
        if (minor < constraint.minor) return false;
        if (minor == constraint.minor && patch < constraint.patch) return false;
        return true;
    }

    /// Format as string
    [[nodiscard]] std::string to_string() const {
        return std::format("{}.{}.{}", major, minor, patch);
    }
};

// ============================================================
// Plugin Manifest
// ============================================================

/// Parsed plugin manifest file (plugin.json)
struct PluginManifest {
    PluginDefinition definition;
    PluginCapabilities capabilities;
    std::filesystem::path plugin_dir;
    std::vector<std::filesystem::path> agents_paths;
    std::vector<std::filesystem::path> skills_paths;
    std::string min_host_version;              // Minimum CLI version required
    std::unordered_map<std::string, std::string> env; // Environment variables
    std::optional<std::string> sandbox_policy;  // Sandbox configuration
};

struct PluginComponentPaths {
    std::string plugin_name;
    std::filesystem::path plugin_dir;
    std::vector<std::filesystem::path> agents_paths;
    std::vector<std::filesystem::path> skills_paths;
};

// ============================================================
// Plugin Cache Entry
// ============================================================

/// Cached plugin validation result to avoid re-parsing
struct PluginCacheEntry {
    std::string plugin_name;
    std::filesystem::path path;
    std::uint64_t last_modified;    // File modification timestamp
    bool is_valid = false;          // Whether manifest was valid
    std::optional<PluginManifest> manifest;
};

// ============================================================
// PluginLoader - discovers and validates plugins
// ============================================================

/// Discovers plugins from filesystem, validates manifests,
/// manages process lifecycle, and handles dependency resolution.
class PluginLoader {
    std::vector<std::filesystem::path> search_paths_;
    std::unordered_map<std::string, PluginCacheEntry> cache_;
    std::string host_version_ = "1.0.0";  // Current CLI version for compat checks
    uv_loop_t* loop_;

public:
    explicit PluginLoader(uv_loop_t* loop) : loop_(loop) {
        // Default plugin search paths
        if (auto home = std::getenv("HOME")) {
            search_paths_.emplace_back(
                std::filesystem::path(home) / ".claude" / "plugins");
        }
        search_paths_.emplace_back(
            std::filesystem::current_path() / ".claude" / "plugins");
    }

    /// Add a custom search path
    void add_search_path(std::filesystem::path path) {
        search_paths_.push_back(std::move(path));
    }

    /// Set the host CLI version for compatibility checking
    void set_host_version(std::string version) {
        host_version_ = std::move(version);
    }

    /// Discover all plugins from configured search paths
    [[nodiscard]] Result<std::vector<PluginManifest>> discover_all() {
        std::vector<PluginManifest> manifests;

        for (const auto& search_path : search_paths_) {
            if (!std::filesystem::exists(search_path)) continue;

            for (const auto& entry : std::filesystem::directory_iterator(search_path)) {
                if (!entry.is_directory()) continue;

                auto manifest_path = entry.path() / "plugin.json";
                if (!std::filesystem::exists(manifest_path)) continue;

                auto result = load_manifest(manifest_path);
                if (result) {
                    manifests.push_back(std::move(*result));
                }
            }
        }
        return manifests;
    }

    /// Load and validate a single plugin manifest
    [[nodiscard]] Result<PluginManifest> load_manifest(
        const std::filesystem::path& manifest_path) {

        // Check cache first
        auto cache_key = manifest_path.string();
        if (auto it = cache_.find(cache_key); it != cache_.end()) {
            auto mod_time = std::filesystem::last_write_time(manifest_path)
                .time_since_epoch().count();
            if (static_cast<std::uint64_t>(mod_time) == it->second.last_modified
                && it->second.is_valid) {
                return *it->second.manifest;
            }
        }

        // Read manifest file
        std::ifstream file(manifest_path);
        if (!file.is_open()) {
            return std::unexpected(Error::make(ErrorCode::ConfigNotFound,
                std::format("Cannot open manifest: {}", manifest_path.string())));
        }

        std::stringstream buf;
        buf << file.rdbuf();
        std::string json_str = buf.str();

        auto result = parse_manifest_json(json_str, manifest_path.parent_path());
        if (!result) return result;

        // Validate the parsed manifest
        auto validation = validate_manifest(*result);
        if (!validation) return std::unexpected(validation.error());

        // Update cache
        auto mod_time = std::filesystem::last_write_time(manifest_path)
            .time_since_epoch().count();
        cache_[cache_key] = PluginCacheEntry{
            .plugin_name = result->definition.name,
            .path = manifest_path,
            .last_modified = static_cast<std::uint64_t>(mod_time),
            .is_valid = true,
            .manifest = *result,
        };

        return result;
    }

    /// Validate version compatibility between plugin and host
    [[nodiscard]] VoidResult check_compatibility(const PluginManifest& manifest) const {
        if (manifest.min_host_version.empty()) return {};

        auto required = SemVer::parse(manifest.min_host_version);
        auto current = SemVer::parse(host_version_);
        if (!required || !current) return {};

        if (!current->satisfies_caret(*required)) {
            return std::unexpected(Error::make(ErrorCode::InternalError,
                std::format("Plugin '{}' requires host >= {}, but current is {}",
                    manifest.definition.name, manifest.min_host_version, host_version_)));
        }
        return {};
    }

    /// Resolve plugin dependencies, returning load order or error on cycles
    [[nodiscard]] Result<std::vector<std::string>> resolve_dependencies(
        const std::vector<PluginManifest>& manifests) const {

        // Build adjacency map
        std::unordered_map<std::string, std::vector<std::string>> deps;
        std::unordered_map<std::string, std::uint32_t> in_degree;

        for (const auto& m : manifests) {
            deps[m.definition.name]; // Ensure all nodes exist
            in_degree.try_emplace(m.definition.name, 0);
        }

        for (const auto& m : manifests) {
            for (const auto& dep : m.definition.dependencies) {
                deps[dep].push_back(m.definition.name);
                in_degree[m.definition.name]++;
            }
        }

        // Topological sort (Kahn's algorithm)
        std::queue<std::string> queue;
        for (const auto& [name, degree] : in_degree) {
            if (degree == 0) queue.push(name);
        }

        std::vector<std::string> order;
        while (!queue.empty()) {
            auto curr = std::move(queue.front());
            queue.pop();
            order.push_back(curr);

            for (const auto& dependent : deps[curr]) {
                if (--in_degree[dependent] == 0) {
                    queue.push(dependent);
                }
            }
        }

        if (order.size() != manifests.size()) {
            return std::unexpected(Error::make(ErrorCode::InternalError,
                "Circular dependency detected in plugins"));
        }
        return order;
    }

    /// Clear the manifest cache
    void clear_cache() { cache_.clear(); }

    /// Invalidate a specific cache entry
    void invalidate(std::string_view plugin_name) {
        std::erase_if(cache_, [plugin_name](const auto& pair) {
            return pair.second.plugin_name == plugin_name;
        });
    }

    /// Get all cached entries
    [[nodiscard]] std::vector<const PluginCacheEntry*> cached_entries() const {
        std::vector<const PluginCacheEntry*> entries;
        entries.reserve(cache_.size());
        for (const auto& [_, entry] : cache_) {
            entries.push_back(&entry);
        }
        return entries;
    }

private:
    /// Parse plugin.json into a PluginManifest
    [[nodiscard]] Result<PluginManifest> parse_manifest_json(
        const std::string& json_str,
        const std::filesystem::path& plugin_dir) const {

        auto doc_result = cc::utils::json::parse(json_str);
        if (!doc_result) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigParseError,
                "Invalid JSON in plugin manifest: " + doc_result.error().message()));
        }

        auto root = doc_result->root();
        PluginManifest manifest{};
        manifest.plugin_dir = plugin_dir;

        // Required fields
        auto name_val = root.get("name");
        auto version_val = root.get("version");
        auto entry_val = root.get("entry_point");

        if (!root || !root.is_obj() || !name_val.is_str() || !version_val.is_str() || !entry_val.is_str()) {
            return std::unexpected(Error::make(ErrorCode::ConfigParseError,
                "Plugin manifest missing required fields (name, version, entry_point)"));
        }

        manifest.definition.name = std::string(name_val.as_str());
        manifest.definition.version = std::string(version_val.as_str());
        manifest.definition.entry_point =
            (plugin_dir / std::string(entry_val.as_str())).string();

        // Optional fields
        if (auto v = root.get("author"); v && v.is_str())
            manifest.definition.author = std::string(v.as_str());
        if (auto v = root.get("description"); v && v.is_str())
            manifest.definition.description = std::string(v.as_str());
        if (auto v = root.get("license"); v && v.is_str())
            manifest.definition.license = std::string(v.as_str());
        if (auto v = root.get("min_host_version"); v && v.is_str())
            manifest.min_host_version = std::string(v.as_str());

        auto append_component_path = [&](cc::utils::json::JsonVal value,
                                         std::vector<std::filesystem::path>& out) {
            auto append_one = [&](std::string_view raw) {
                if (raw.empty()) return;
                std::filesystem::path path{std::string(raw)};
                if (path.is_relative()) path = plugin_dir / path;
                if (std::filesystem::exists(path)) out.push_back(std::move(path));
            };
            if (value.is_str()) {
                append_one(value.as_str());
            } else if (value.is_arr()) {
                value.iter([&](cc::utils::json::JsonVal item) {
                    if (item.is_str()) append_one(item.as_str());
                });
            }
        };

        const auto default_agents = plugin_dir / "agents";
        if (std::filesystem::exists(default_agents)) {
            manifest.agents_paths.push_back(default_agents);
        }
        if (auto agents = root.get("agents"); agents.valid()) {
            append_component_path(agents, manifest.agents_paths);
        }

        const auto default_skills = plugin_dir / "skills";
        if (std::filesystem::exists(default_skills)) {
            manifest.skills_paths.push_back(default_skills);
        }
        if (auto skills = root.get("skills"); skills.valid()) {
            append_component_path(skills, manifest.skills_paths);
        }

        // Capabilities
        if (auto caps = root.get("capabilities"); caps && caps.is_obj()) {
            if (auto tools = caps.get("tools"); tools && tools.is_arr()) {
                tools.iter([&](cc::utils::json::JsonVal item) {
                    if (item.is_str()) manifest.capabilities.tools.emplace_back(item.as_str());
                });
            }
            if (auto cmds = caps.get("commands"); cmds && cmds.is_arr()) {
                cmds.iter([&](cc::utils::json::JsonVal item) {
                    if (item.is_str()) manifest.capabilities.commands.emplace_back(item.as_str());
                });
            }
            if (auto hooks = caps.get("hooks"); hooks && hooks.is_arr()) {
                hooks.iter([&](cc::utils::json::JsonVal item) {
                    if (item.is_str()) manifest.capabilities.hooks.emplace_back(item.as_str());
                });
            }
        }

        // Dependencies
        if (auto deps = root.get("dependencies"); deps && deps.is_arr()) {
            deps.iter([&](cc::utils::json::JsonVal item) {
                if (item.is_str()) manifest.definition.dependencies.emplace_back(item.as_str());
            });
        }

        return manifest;
    }

    /// Validate a parsed manifest for correctness
    [[nodiscard]] VoidResult validate_manifest(const PluginManifest& manifest) const {
        // Name must be non-empty and alphanumeric with hyphens
        if (manifest.definition.name.empty()) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigParseError, "Plugin name cannot be empty"));
        }

        for (char c : manifest.definition.name) {
            if (!std::isalnum(c) && c != '-' && c != '_') {
                return std::unexpected(Error::make(ErrorCode::ConfigParseError,
                    std::format("Invalid character '{}' in plugin name", c)));
            }
        }

        // Version must be valid semver
        if (!SemVer::parse(manifest.definition.version)) {
            return std::unexpected(Error::make(ErrorCode::ConfigParseError,
                std::format("Invalid version '{}' in plugin manifest",
                    manifest.definition.version)));
        }

        // Entry point must exist (if absolute) or be resolvable
        std::filesystem::path entry(manifest.definition.entry_point);
        if (entry.is_absolute() && !std::filesystem::exists(entry)) {
            return std::unexpected(Error::make(ErrorCode::ConfigNotFound,
                std::format("Plugin entry point not found: {}",
                    manifest.definition.entry_point)));
        }

        // Check version compatibility with host
        return check_compatibility(manifest);
    }
};

[[nodiscard]] inline Result<std::vector<PluginComponentPaths>> discover_plugin_component_paths() {
    PluginLoader loader(uv_default_loop());
    auto manifests = loader.discover_all();
    if (!manifests) return std::unexpected(manifests.error());

    std::vector<PluginComponentPaths> paths;
    paths.reserve(manifests->size());
    for (const auto& manifest : *manifests) {
        if (manifest.agents_paths.empty() && manifest.skills_paths.empty()) continue;
        paths.push_back(PluginComponentPaths{
            .plugin_name = manifest.definition.name,
            .plugin_dir = manifest.plugin_dir,
            .agents_paths = manifest.agents_paths,
            .skills_paths = manifest.skills_paths,
        });
    }
    return paths;
}

} // namespace cc::plugins
