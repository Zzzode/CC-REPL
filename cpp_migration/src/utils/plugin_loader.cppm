// Plugin Loader Module
// Consolidates: pluginLoader, loadPluginAgents, loadPluginCommands,
//               loadPluginHooks, loadPluginOutputStyles, walkPluginMarkdown
//
// Responsible for discovering, loading, and validating plugins from various
// sources including marketplaces, git repositories, and local paths.
module;

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module cc.utils.plugin_loader;

import cc.utils.plugin_identifier;
import cc.utils.plugin_versioning;

export namespace cc::utils::plugin_loader {

// ─────────────────────────────────────────────────────────────────────────────
// Plugin Source Types
// ─────────────────────────────────────────────────────────────────────────────

struct GitHubSource {
    std::string repo;
    std::optional<std::string> ref;
    std::optional<std::string> sha;
};

struct GitUrlSource {
    std::string url;
    std::optional<std::string> ref;
    std::optional<std::string> sha;
};

struct GitSubdirSource {
    std::string url;
    std::string path;
    std::optional<std::string> ref;
    std::optional<std::string> sha;
};

struct NpmSource {
    std::string package_name;
    std::optional<std::string> registry;
    std::optional<std::string> version;
};

struct LocalSource {
    std::string path;
};

using PluginSource = std::variant<LocalSource, GitHubSource, GitUrlSource, GitSubdirSource, NpmSource>;

// ─────────────────────────────────────────────────────────────────────────────
// Plugin Manifest & Components
// ─────────────────────────────────────────────────────────────────────────────

struct PluginAuthor {
    std::string name;
    std::optional<std::string> email;
    std::optional<std::string> url;
};

struct CommandMetadata {
    std::optional<std::string> source;
    std::optional<std::string> content;
    std::optional<std::string> description;
    bool hidden = false;
};

struct PluginManifest {
    std::string name;
    std::optional<std::string> version;
    std::optional<std::string> description;
    std::optional<PluginAuthor> author;
    std::optional<std::vector<std::string>> keywords;
    std::optional<std::string> homepage;
    std::optional<std::vector<std::string>> dependencies;
    // hooks can be path strings or inline objects
    std::optional<std::vector<std::string>> hooks;
    // commands can be paths or metadata map
    std::optional<std::vector<std::string>> commands;
    std::optional<std::vector<std::string>> agents;
    std::optional<std::vector<std::string>> skills;
    std::optional<std::vector<std::string>> output_styles;
};

enum class PluginComponent : unsigned char {
    Commands,
    Agents,
    Skills,
    Hooks,
    OutputStyles,
};

struct PluginError {
    std::string type;  // "path-not-found", "hook-load-failed", "marketplace-blocked-by-policy"
    std::string source;
    std::string plugin;
    std::optional<std::string> path;
    std::optional<PluginComponent> component;
    std::optional<std::string> hook_path;
    std::optional<std::string> reason;
    std::optional<std::string> marketplace;
};

// ─────────────────────────────────────────────────────────────────────────────
// Loaded Plugin
// ─────────────────────────────────────────────────────────────────────────────

struct LoadedPlugin {
    std::string name;
    PluginManifest manifest;
    std::filesystem::path path;
    std::string source;
    std::string repository;
    bool enabled = false;

    // Component paths
    std::optional<std::filesystem::path> commands_path;
    std::optional<std::vector<std::filesystem::path>> commands_paths;
    std::optional<std::map<std::string, CommandMetadata>> commands_metadata;
    std::optional<std::filesystem::path> agents_path;
    std::optional<std::vector<std::filesystem::path>> agents_paths;
    std::optional<std::filesystem::path> skills_path;
    std::optional<std::vector<std::filesystem::path>> skills_paths;
    std::optional<std::filesystem::path> output_styles_path;
    std::optional<std::vector<std::filesystem::path>> output_styles_paths;

    // Hooks configuration (simplified — full hooks config is a nested map)
    std::optional<std::map<std::string, std::vector<std::map<std::string, std::string>>>> hooks_config;

    // Plugin settings (allowlisted keys only)
    std::optional<std::map<std::string, std::string>> settings;
};

struct PluginLoadResult {
    std::vector<LoadedPlugin> plugins;
    std::vector<PluginError> errors;
};

// ─────────────────────────────────────────────────────────────────────────────
// Cache Path Utilities
// ─────────────────────────────────────────────────────────────────────────────

/// Get the base plugin cache directory path
[[nodiscard]] std::filesystem::path get_plugin_cache_path();

/// Compute versioned cache path: cache/{marketplace}/{plugin}/{version}/
[[nodiscard]] std::filesystem::path get_versioned_cache_path(
    std::string_view plugin_id,
    std::string_view version
);

/// Compute versioned cache path under a specific base directory
[[nodiscard]] std::filesystem::path get_versioned_cache_path_in(
    const std::filesystem::path& base_dir,
    std::string_view plugin_id,
    std::string_view version
);

/// Get versioned ZIP cache path: versioned_path + ".zip"
[[nodiscard]] std::filesystem::path get_versioned_zip_cache_path(
    std::string_view plugin_id,
    std::string_view version
);

/// Get legacy (non-versioned) cache path for backward compatibility
[[nodiscard]] std::filesystem::path get_legacy_cache_path(std::string_view plugin_name);

/// Resolve plugin path with fallback to legacy location
[[nodiscard]] std::expected<std::filesystem::path, std::string> resolve_plugin_path(
    std::string_view plugin_id,
    std::optional<std::string_view> version = std::nullopt
);

/// Probe seed directories for a populated cache at this plugin version
[[nodiscard]] std::optional<std::filesystem::path> probe_seed_cache(
    std::string_view plugin_id,
    std::string_view version
);

/// Probe seed cache for any version (first-boot chicken-and-egg resolution)
[[nodiscard]] std::optional<std::filesystem::path> probe_seed_cache_any_version(
    std::string_view plugin_id
);

// ─────────────────────────────────────────────────────────────────────────────
// Plugin Installation from Source
// ─────────────────────────────────────────────────────────────────────────────

/// Validate a git URL (https, http, file, or git@ssh)
[[nodiscard]] std::expected<std::string, std::string> validate_git_url(std::string_view url);

/// Clone a git repository
[[nodiscard]] std::expected<void, std::string> git_clone(
    std::string_view git_url,
    const std::filesystem::path& target_path,
    std::optional<std::string_view> ref = std::nullopt,
    std::optional<std::string_view> sha = std::nullopt
);

/// Install a plugin from npm
[[nodiscard]] std::expected<void, std::string> install_from_npm(
    std::string_view package_name,
    const std::filesystem::path& target_path,
    std::optional<std::string_view> registry = std::nullopt,
    std::optional<std::string_view> version = std::nullopt
);

/// Install a plugin from a git subdirectory (sparse-checkout)
[[nodiscard]] std::expected<std::optional<std::string>, std::string> install_from_git_subdir(
    std::string_view url,
    const std::filesystem::path& target_path,
    std::string_view subdir_path,
    std::optional<std::string_view> ref = std::nullopt,
    std::optional<std::string_view> sha = std::nullopt
);

/// Generate a temporary cache name for a plugin
[[nodiscard]] std::string generate_temporary_cache_name(const PluginSource& source);

struct CachePluginResult {
    std::filesystem::path path;
    PluginManifest manifest;
    std::optional<std::string> git_commit_sha;
};

/// Cache a plugin from an external source
[[nodiscard]] std::expected<CachePluginResult, std::string> cache_plugin(
    const PluginSource& source,
    std::optional<PluginManifest> manifest_override = std::nullopt
);

/// Copy plugin files to versioned cache directory
[[nodiscard]] std::expected<std::filesystem::path, std::string> copy_plugin_to_versioned_cache(
    const std::filesystem::path& source_path,
    std::string_view plugin_id,
    std::string_view version
);

// ─────────────────────────────────────────────────────────────────────────────
// Plugin Loading & Discovery
// ─────────────────────────────────────────────────────────────────────────────

/// Load and validate a plugin manifest from a JSON file
[[nodiscard]] std::expected<PluginManifest, std::string> load_plugin_manifest(
    const std::filesystem::path& manifest_path,
    std::string_view plugin_name,
    std::string_view source
);

/// Create a LoadedPlugin from a plugin directory path
[[nodiscard]] std::expected<std::pair<LoadedPlugin, std::vector<PluginError>>, std::string>
create_plugin_from_path(
    const std::filesystem::path& plugin_path,
    std::string_view source,
    bool enabled,
    std::string_view fallback_name,
    bool strict = true
);

/// Load all plugins (marketplace-based discovery with full fetch)
[[nodiscard]] PluginLoadResult load_all_plugins();

/// Load all plugins using cache only (no network fetches)
[[nodiscard]] PluginLoadResult load_all_plugins_cache_only();

/// Clear the memoized plugin load cache
void clear_plugin_cache();

// ─────────────────────────────────────────────────────────────────────────────
// Component Loaders (from loadPluginAgents, loadPluginCommands, etc.)
// ─────────────────────────────────────────────────────────────────────────────

struct PluginCommand {
    std::string name;
    std::string content;
    std::string plugin_name;
    std::string source_path;
    std::optional<CommandMetadata> metadata;
};

struct PluginAgent {
    std::string name;
    std::string content;
    std::string plugin_name;
    std::string source_path;
};

struct PluginOutputStyle {
    std::string name;
    std::string content;
    std::string plugin_name;
    std::string source_path;
};

/// Load commands from all enabled plugins
[[nodiscard]] std::vector<PluginCommand> load_plugin_commands();

/// Load agents from all enabled plugins
[[nodiscard]] std::vector<PluginAgent> load_plugin_agents();

/// Load output styles from all enabled plugins
[[nodiscard]] std::vector<PluginOutputStyle> load_plugin_output_styles();

/// Clear individual component caches
void clear_plugin_command_cache();
void clear_plugin_agent_cache();
void clear_plugin_output_style_cache();
void clear_plugin_hook_cache();

// ─────────────────────────────────────────────────────────────────────────────
// Markdown Walker (from walkPluginMarkdown)
// ─────────────────────────────────────────────────────────────────────────────

struct MarkdownFile {
    std::string name;        // filename without extension (used as command/agent name)
    std::string content;     // full file content
    std::filesystem::path path;
    std::map<std::string, std::string> frontmatter;
};

/// Walk a directory for markdown files, parsing frontmatter
[[nodiscard]] std::vector<MarkdownFile> walk_plugin_markdown(
    const std::filesystem::path& dir_path
);

/// Walk multiple directories for markdown files
[[nodiscard]] std::vector<MarkdownFile> walk_plugin_markdown_paths(
    const std::vector<std::filesystem::path>& paths
);

} // namespace cc::utils::plugin_loader
