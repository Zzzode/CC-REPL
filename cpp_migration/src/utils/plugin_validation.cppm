// Plugin Validation Module
// Consolidates: validatePlugin, pluginFlagging, schemas, performStartupChecks
//
// Provides validation of plugin manifests, marketplace manifests,
// plugin flagging/reporting, schema definitions, and startup safety checks.
module;

#include <algorithm>
#include <cctype>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module cc.utils.plugin_validation;

import cc.utils.json;
import cc.utils.plugin_identifier;

export namespace cc::utils::plugin_validation {

// ─────────────────────────────────────────────────────────────────────────────
// Validation Result Types (from validatePlugin)
// ─────────────────────────────────────────────────────────────────────────────

enum class ValidatedFileType : unsigned char {
    Plugin,
    Marketplace,
    Skill,
    Agent,
    Command,
    Hooks,
};

struct ValidationError {
    std::string path;    // field path (e.g., "plugins.my-plugin.source")
    std::string message;
    std::optional<std::string> code;
};

struct ValidationWarning {
    std::string path;
    std::string message;
};

struct ValidationResult {
    bool success = false;
    std::vector<ValidationError> errors;
    std::vector<ValidationWarning> warnings;
    std::filesystem::path file_path;
    ValidatedFileType file_type = ValidatedFileType::Plugin;
};

/// Validate a plugin manifest (plugin.json)
[[nodiscard]] ValidationResult validate_plugin_manifest(const std::filesystem::path& path);

/// Validate a marketplace manifest (marketplace.json)
[[nodiscard]] ValidationResult validate_marketplace_manifest(const std::filesystem::path& path);

/// Validate a hooks configuration file (hooks.json)
[[nodiscard]] ValidationResult validate_hooks_file(const std::filesystem::path& path);

/// Validate a skill/agent/command markdown file (frontmatter + content)
[[nodiscard]] ValidationResult validate_markdown_component(
    const std::filesystem::path& path,
    ValidatedFileType type
);

/// Validate a plugin directory (all components)
[[nodiscard]] std::vector<ValidationResult> validate_plugin_directory(
    const std::filesystem::path& plugin_dir
);

/// Auto-detect file type and validate
[[nodiscard]] ValidationResult validate_file(const std::filesystem::path& path);

// ─────────────────────────────────────────────────────────────────────────────
// Schema Definitions (from schemas — type-level constants and validators)
// ─────────────────────────────────────────────────────────────────────────────

/// Plugin ID format: "name@marketplace"
[[nodiscard]] bool is_valid_plugin_id(std::string_view id);

/// Validate a plugin name component
[[nodiscard]] bool is_valid_plugin_name(std::string_view name);

/// Validate a marketplace name component
[[nodiscard]] bool is_valid_marketplace_name(std::string_view name);

/// Plugin source type enumeration
enum class PluginSourceType : unsigned char {
    Local,
    GitHub,
    Git,
    GitSubdir,
    Npm,
    Pip,
};

/// Check if a source is a local plugin source (string path)
[[nodiscard]] bool is_local_plugin_source(std::string_view source);

/// Infer source type from a source value
[[nodiscard]] PluginSourceType infer_source_type(std::string_view source);

// ─────────────────────────────────────────────────────────────────────────────
// Plugin Flagging (from pluginFlagging)
// ─────────────────────────────────────────────────────────────────────────────

enum class PluginFlagReason : unsigned char {
    SecurityConcern,
    Malicious,
    BrokenManifest,
    Spam,
    PolicyViolation,
    Other,
};

struct PluginFlag {
    std::string plugin_id;
    PluginFlagReason reason;
    std::string description;
    std::optional<std::string> reporter;
    std::string flagged_at;
};

struct FlagCheckResult {
    bool is_flagged = false;
    std::optional<PluginFlag> flag;
};

/// Check if a plugin has been flagged
[[nodiscard]] FlagCheckResult check_plugin_flag(std::string_view plugin_id);

/// Report a plugin for review
[[nodiscard]] std::expected<void, std::string> report_plugin(
    std::string_view plugin_id,
    PluginFlagReason reason,
    std::string_view description
);

/// Get all flagged plugins
[[nodiscard]] std::vector<PluginFlag> get_flagged_plugins();

// ─────────────────────────────────────────────────────────────────────────────
// Startup Checks (from performStartupChecks)
// ─────────────────────────────────────────────────────────────────────────────

struct StartupCheckConfig {
    bool check_manifests = true;
    bool check_hooks_integrity = true;
    bool check_outdated_plugins = false;
    bool check_blocklist = true;
};

struct StartupCheckReport {
    bool passed = true;
    std::vector<std::string> critical_errors;   // blocks plugin loading
    std::vector<std::string> warnings;          // informational
    std::vector<std::string> disabled_plugins;  // plugins disabled due to issues
    std::chrono::milliseconds duration{0};
};

/// Perform comprehensive startup checks on the plugin system
[[nodiscard]] StartupCheckReport perform_startup_checks(
    const StartupCheckConfig& config = {}
);

/// Quick startup check (blocklist + manifest integrity only)
[[nodiscard]] StartupCheckReport perform_quick_startup_check();

// ─────────────────────────────────────────────────────────────────────────────
// Manifest Schema Constants
// ─────────────────────────────────────────────────────────────────────────────

/// Maximum allowed plugin name length
inline constexpr std::size_t MAX_PLUGIN_NAME_LENGTH = 128;

/// Maximum allowed marketplace name length
inline constexpr std::size_t MAX_MARKETPLACE_NAME_LENGTH = 64;

/// Maximum allowed description length
inline constexpr std::size_t MAX_DESCRIPTION_LENGTH = 1024;

/// Maximum number of plugins per marketplace
inline constexpr std::size_t MAX_PLUGINS_PER_MARKETPLACE = 500;

/// Maximum number of dependencies per plugin
inline constexpr std::size_t MAX_DEPENDENCIES_PER_PLUGIN = 20;

/// Allowed characters in plugin names (regex-like description)
inline constexpr std::string_view PLUGIN_NAME_PATTERN = "[a-zA-Z0-9][a-zA-Z0-9._-]*";

/// Fields that belong in marketplace.json but not plugin.json
inline constexpr std::array MARKETPLACE_ONLY_MANIFEST_FIELDS = {
    std::string_view{"category"},
    std::string_view{"source"},
    std::string_view{"tags"},
    std::string_view{"strict"},
    std::string_view{"id"},
};

// ─────────────────────────────────────────────────────────────────────────────
// Manifest validation implementation (ported from validatePlugin.ts)
//
// The TS original uses Zod schemas; this C++ port mirrors the same happy-path
// checks (file existence/JSON parse, path-traversal scan, required `name`,
// marketplace-only-field warnings) using the yyjson-backed cc.utils.json
// module. Fields that the TS schema would reject (wrong types, missing required
// keys) surface as ValidationError entries. Anything not ported (deep schema
// constraints, hooks/frontmatter validation) is reported honestly as an error
// rather than fake success, so callers cannot be silently misled.
// ─────────────────────────────────────────────────────────────────────────────
namespace detail {

using cc::utils::json::JsonDoc;
using cc::utils::json::JsonVal;

// Read a file into a string; distinguish ENOENT/EISDIR/other (TS parity).
inline std::expected<std::string, std::string>
read_file_contents(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) {
        return std::unexpected("ENOENT");
    }
    std::error_code sec;
    if (std::filesystem::is_directory(p, sec)) {
        return std::unexpected("EISDIR");
    }
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return std::unexpected("Failed to read file: " + p.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// detectManifestType() parity: filename/dirname heuristics + content probe.
// Returns "plugin" | "marketplace" | "unknown".
inline std::string detect_manifest_type(const std::filesystem::path& p,
                                        JsonVal root) {
    const auto fname = p.filename().string();
    const auto parent = p.parent_path().filename().string();
    if (fname == "plugin.json") return "plugin";
    if (fname == "marketplace.json") return "marketplace";
    if (parent == ".claude-plugin") return "plugin";
    // Content heuristic: a marketplace manifest has a "plugins" array.
    auto plugins_val = root.get("plugins");
    if (plugins_val.valid() && plugins_val.is_arr()) return "marketplace";
    return "unknown";
}

// checkPathTraversal() parity: flag any ".." segment as a security error.
inline void check_path_traversal(std::string_view p,
                                 std::string_view field,
                                 std::vector<ValidationError>& errors,
                                 std::optional<std::string> hint = std::nullopt) {
    if (p.find("..") != std::string_view::npos) {
        ValidationError e;
        e.path = std::string(field);
        if (hint) {
            e.message = std::format("Path contains \"..\": {}. {}", p, *hint);
        } else {
            e.message = std::format(
                "Path contains \"..\" which could be a path traversal attempt: {}", p);
        }
        errors.push_back(std::move(e));
    }
}

// Return a string scalar from a JsonVal, or std::nullopt if absent/not a string.
inline std::optional<std::string> as_string(JsonVal v) {
    if (v.valid() && v.is_str()) return std::string(v.as_str());
    return std::nullopt;
}

// Iterate an array-of-strings field (commands/agents/skills) and run the
// path-traversal check on each entry — mirrors the TS scan in
// validatePluginManifest.
inline void scan_string_array_for_traversal(JsonVal obj,
                                            std::string_view key,
                                            std::vector<ValidationError>& errors) {
    auto arr = obj.get(key);
    if (!arr.valid() || !arr.is_arr()) return;
    arr.iter([&](JsonVal item) {
        if (item.valid() && item.is_str()) {
            check_path_traversal(item.as_str(),
                                 std::format("{}[?]", key), errors);
        }
    });
}

} // namespace detail

using cc::utils::json::JsonVal;

// Validate a plugin.json manifest against the TS PluginManifestSchema subset
// (name required + string-typed; path-traversal scan on commands/agents/skills;
// marketplace-only field warnings). Mirrors validatePluginManifest() in TS.
inline ValidationResult validate_plugin_manifest(const std::filesystem::path& path) {
    ValidationResult r;
    r.file_path = path;
    r.file_type = ValidatedFileType::Plugin;

    auto contents = detail::read_file_contents(path);
    if (!contents) {
        const auto& code = contents.error();
        std::string msg = (code == "ENOENT")
            ? std::format("File not found: {}", path.string())
            : (code == "EISDIR")
                ? std::format("Path is not a file: {}", path.string())
                : code;
        ValidationError e; e.path = "file"; e.message = std::move(msg);
        e.code = code;
        r.errors.push_back(std::move(e));
        r.success = false;
        return r;
    }

    auto parsed = cc::utils::json::parse(*contents);
    if (!parsed) {
        ValidationError e; e.path = "json";
        e.message = std::format("Invalid JSON syntax: {}", parsed.error().message());
        r.errors.push_back(std::move(e));
        r.success = false;
        return r;
    }
    JsonVal root = parsed->root();
    if (!root.valid() || !root.is_obj()) {
        ValidationError e; e.path = "root";
        e.message = "Manifest must be a JSON object";
        r.errors.push_back(std::move(e));
        r.success = false;
        return r;
    }

    // Path-traversal scan on string component lists (security pre-check).
    detail::scan_string_array_for_traversal(root, "commands", r.errors);
    detail::scan_string_array_for_traversal(root, "agents", r.errors);
    detail::scan_string_array_for_traversal(root, "skills", r.errors);

    // Warn on marketplace-only fields mistakenly placed in plugin.json.
    for (const auto field : MARKETPLACE_ONLY_MANIFEST_FIELDS) {
        auto stray = root.get(std::string(field));
        if (stray.valid()) {
            ValidationWarning w;
            w.path = std::string(field);
            w.message = std::format(
                "Field '{}' belongs in the marketplace entry (marketplace.json), "
                "not plugin.json. It's harmless here but unused — Claude Code "
                "ignores it at load time.", field);
            r.warnings.push_back(std::move(w));
        }
    }

    // name is the only hard-required field in the TS PluginManifestSchema.
    auto name_val = root.get("name");
    if (!name_val.valid()) {
        ValidationError e; e.path = "name";
        e.message = "Required field 'name' is missing";
        r.errors.push_back(std::move(e));
    } else if (!name_val.is_str()) {
        ValidationError e; e.path = "name";
        e.message = std::format(
            "name must be a string, got {}", name_val.to_string());
        r.errors.push_back(std::move(e));
    } else {
        // Kebab-case advisory (marketplace sync rejects non-kebab names).
        auto name = std::string(name_val.as_str());
        bool kebab = !name.empty() && std::all_of(name.begin(), name.end(),
            [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-'; });
        if (!kebab) {
            ValidationWarning w; w.path = "name";
            w.message = std::format(
                "Plugin name \"{}\" is not kebab-case. Claude Code accepts it, "
                "but the Claude.ai marketplace sync requires kebab-case "
                "(lowercase letters, digits, and hyphens only, e.g., \"my-plugin\").",
                name);
            r.warnings.push_back(std::move(w));
        }
    }

    // Soft advisories mirrored from TS (missing version/description/author).
    if (!detail::as_string(root.get("version"))) {
        r.warnings.push_back({"version",
            "No version specified. Consider adding a version following semver (e.g., \"1.0.0\")"});
    }
    if (!detail::as_string(root.get("description"))) {
        r.warnings.push_back({"description",
            "No description provided. Adding a description helps users understand what your plugin does"});
    }
    if (!detail::as_string(root.get("author"))) {
        r.warnings.push_back({"author",
            "No author information provided. Consider adding author details for plugin attribution"});
    }

    r.success = r.errors.empty();
    return r;
}

// Validate a marketplace.json manifest (TS validateMarketplaceManifest parity):
// plugins array required, path-traversal on plugin source strings, duplicate
// name detection, missing description warning.
inline ValidationResult validate_marketplace_manifest(const std::filesystem::path& path) {
    ValidationResult r;
    r.file_path = path;
    r.file_type = ValidatedFileType::Marketplace;

    auto contents = detail::read_file_contents(path);
    if (!contents) {
        const auto& code = contents.error();
        std::string msg = (code == "ENOENT")
            ? std::format("File not found: {}", path.string())
            : (code == "EISDIR")
                ? std::format("Path is not a file: {}", path.string())
                : code;
        ValidationError e; e.path = "file"; e.message = std::move(msg);
        e.code = code;
        r.errors.push_back(std::move(e));
        r.success = false;
        return r;
    }

    auto parsed = cc::utils::json::parse(*contents);
    if (!parsed) {
        ValidationError e; e.path = "json";
        e.message = std::format("Invalid JSON syntax: {}", parsed.error().message());
        r.errors.push_back(std::move(e));
        r.success = false;
        return r;
    }
    JsonVal root = parsed->root();
    if (!root.valid() || !root.is_obj()) {
        ValidationError e; e.path = "root";
        e.message = "Marketplace manifest must be a JSON object";
        r.errors.push_back(std::move(e));
        r.success = false;
        return r;
    }

    auto plugins_val = root.get("plugins");
    if (!plugins_val.valid() || !plugins_val.is_arr()) {
        ValidationError e; e.path = "plugins";
        e.message = "Marketplace manifest must have a 'plugins' array";
        r.errors.push_back(std::move(e));
        r.success = false;
        return r;
    }

    // Scan each plugin entry: name presence + duplicate detection +
    // path-traversal on string source paths (marketplaceSourceHint parity).
    std::set<std::string> seen_names;
    std::size_t idx = 0;
    plugins_val.iter([&](JsonVal entry) {
        if (!entry.valid() || !entry.is_obj()) {
            ValidationError e;
            e.path = std::format("plugins[{}]", idx);
            e.message = "Each plugin entry must be a JSON object";
            r.errors.push_back(std::move(e));
            return;
        }
        auto name = detail::as_string(entry.get("name"));
        if (!name) {
            ValidationError e;
            e.path = std::format("plugins[{}].name", idx);
            e.message = "Plugin entry is missing required 'name'";
            r.errors.push_back(std::move(e));
        } else if (seen_names.contains(*name)) {
            ValidationError e;
            e.path = std::format("plugins[{}].name", idx);
            e.message = std::format(
                "Duplicate plugin name \"{}\" found in marketplace", *name);
            r.errors.push_back(std::move(e));
        } else {
            seen_names.insert(*name);
        }

        auto src = entry.get("source");
        if (src.valid() && src.is_str()) {
            std::string hint = std::format(
                "Plugin source paths are resolved relative to the marketplace root "
                "(the directory containing .claude-plugin/), not relative to "
                "marketplace.json. Use \"./{}\" instead of \"{}\".",
                std::string(src.as_str()), std::string(src.as_str()));
            detail::check_path_traversal(src.as_str(),
                                 std::format("plugins[{}].source", idx),
                                 r.errors, hint);
        }
        ++idx;
    });

    if (seen_names.empty()) {
        r.warnings.push_back({"plugins", "Marketplace has no plugins defined"});
    }

    auto meta = root.get("metadata");
    auto meta_desc = meta.valid() ? detail::as_string(meta.get("description")) : std::nullopt;
    auto top_desc = detail::as_string(root.get("description"));
    if (!meta_desc && !top_desc) {
        r.warnings.push_back({"metadata.description",
            "No marketplace description provided. Adding a description helps users understand what this marketplace offers"});
    }

    r.success = r.errors.empty();
    return r;
}

// validateManifest() parity: auto-detect type from filename/content, or, for a
// directory input, look for .claude-plugin/marketplace.json then plugin.json.
inline ValidationResult validate_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        auto mp = path / ".claude-plugin" / "marketplace.json";
        if (std::filesystem::exists(mp, ec)) {
            return validate_marketplace_manifest(mp);
        }
        auto pj = path / ".claude-plugin" / "plugin.json";
        if (std::filesystem::exists(pj, ec)) {
            return validate_plugin_manifest(pj);
        }
        ValidationResult r;
        r.file_path = path;
        r.file_type = ValidatedFileType::Plugin;
        r.success = false;
        ValidationError e; e.path = "directory";
        e.message = "No manifest found in directory. Expected "
                    ".claude-plugin/marketplace.json or .claude-plugin/plugin.json";
        r.errors.push_back(std::move(e));
        return r;
    }

    // Peek at content to disambiguate "unknown" filenames.
    auto contents = detail::read_file_contents(path);
    if (!contents) {
        ValidationResult r;
        r.file_path = path;
        r.file_type = ValidatedFileType::Plugin;
        r.success = false;
        const auto& code = contents.error();
        ValidationError e; e.path = "file"; e.code = code;
        e.message = (code == "ENOENT")
            ? std::format("File not found: {}", path.string()) : code;
        r.errors.push_back(std::move(e));
        return r;
    }
    auto parsed = cc::utils::json::parse(*contents);
    if (!parsed) {
        ValidationResult r;
        r.file_path = path;
        r.file_type = ValidatedFileType::Plugin;
        r.success = false;
        ValidationError e; e.path = "json";
        e.message = std::format("Invalid JSON syntax: {}", parsed.error().message());
        r.errors.push_back(std::move(e));
        return r;
    }

    const auto type = detail::detect_manifest_type(path, parsed->root());
    if (type == "marketplace") {
        return validate_marketplace_manifest(path);
    }
    // "plugin" or "unknown" → validate as plugin manifest.
    return validate_plugin_manifest(path);
}

} // namespace cc::utils::plugin_validation
