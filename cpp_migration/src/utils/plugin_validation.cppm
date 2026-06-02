// Plugin Validation Module
// Consolidates: validatePlugin, pluginFlagging, schemas, performStartupChecks
//
// Provides validation of plugin manifests, marketplace manifests,
// plugin flagging/reporting, schema definitions, and startup safety checks.
module;

#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module cc.utils.plugin_validation;

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

} // namespace cc::utils::plugin_validation
