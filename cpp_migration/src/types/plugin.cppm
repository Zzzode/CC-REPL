/// @file plugin.cppm
/// @brief Plugin system types: definitions, errors, and load results.
/// Migrated from: src/types/plugin.ts
module;

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

export module cc.types.plugin;

export namespace cc::types::plugin {

// ============================================================
// Plugin component enumeration
// ============================================================

/// Plugin component types that can be loaded
enum class PluginComponent {
    commands,
    agents,
    skills,
    hooks,
    output_styles,
};

// ============================================================
// Plugin repository and config
// ============================================================

/// Repository source information for a plugin
struct PluginRepository {
    std::string url;
    std::string branch;
    std::optional<std::string> last_updated;
    std::optional<std::string> commit_sha;
};

/// Plugin configuration with repository mappings
struct PluginConfig {
    std::unordered_map<std::string, PluginRepository> repositories;
};

// ============================================================
// Loaded plugin definition
// ============================================================

/// A fully loaded plugin instance
struct LoadedPlugin {
    std::string name;
    std::string path;
    std::string source;
    std::string repository;
    std::optional<bool> enabled;
    std::optional<bool> is_builtin;
    std::optional<std::string> sha;
    std::optional<std::string> commands_path;
    std::optional<std::vector<std::string>> commands_paths;
    std::optional<std::string> agents_path;
    std::optional<std::vector<std::string>> agents_paths;
    std::optional<std::string> skills_path;
    std::optional<std::vector<std::string>> skills_paths;
    std::optional<std::string> output_styles_path;
    std::optional<std::vector<std::string>> output_styles_paths;
};

/// Built-in plugin definition that ships with the CLI
struct BuiltinPluginDefinition {
    std::string name;
    std::string description;
    std::optional<std::string> version;
    std::optional<bool> default_enabled;
};

// ============================================================
// Plugin error types (discriminated union)
// ============================================================

/// Git authentication type
enum class GitAuthType {
    ssh,
    https,
};

/// Git operation type
enum class GitOperation {
    clone_op,
    pull,
};

/// Dependency unsatisfied reason
enum class DependencyReason {
    not_enabled,
    not_found,
};

// Individual error structs for each error type
struct PathNotFoundError {
    std::string source;
    std::optional<std::string> plugin;
    std::string path;
    PluginComponent component;
};

struct GitAuthFailedError {
    std::string source;
    std::optional<std::string> plugin;
    std::string git_url;
    GitAuthType auth_type;
};

struct GitTimeoutError {
    std::string source;
    std::optional<std::string> plugin;
    std::string git_url;
    GitOperation operation;
};

struct NetworkError {
    std::string source;
    std::optional<std::string> plugin;
    std::string url;
    std::optional<std::string> details;
};

struct ManifestParseError {
    std::string source;
    std::optional<std::string> plugin;
    std::string manifest_path;
    std::string parse_error;
};

struct ManifestValidationError {
    std::string source;
    std::optional<std::string> plugin;
    std::string manifest_path;
    std::vector<std::string> validation_errors;
};

struct PluginNotFoundError {
    std::string source;
    std::string plugin_id;
    std::string marketplace;
};

struct MarketplaceNotFoundError {
    std::string source;
    std::string marketplace;
    std::vector<std::string> available_marketplaces;
};

struct MarketplaceLoadFailedError {
    std::string source;
    std::string marketplace;
    std::string reason;
};

struct McpConfigInvalidError {
    std::string source;
    std::string plugin;
    std::string server_name;
    std::string validation_error;
};

struct McpServerSuppressedDuplicateError {
    std::string source;
    std::string plugin;
    std::string server_name;
    std::string duplicate_of;
};

struct LspConfigInvalidError {
    std::string source;
    std::string plugin;
    std::string server_name;
    std::string validation_error;
};

struct HookLoadFailedError {
    std::string source;
    std::string plugin;
    std::string hook_path;
    std::string reason;
};

struct ComponentLoadFailedError {
    std::string source;
    std::string plugin;
    PluginComponent component;
    std::string path;
    std::string reason;
};

struct McpbDownloadFailedError {
    std::string source;
    std::string plugin;
    std::string url;
    std::string reason;
};

struct McpbExtractFailedError {
    std::string source;
    std::string plugin;
    std::string mcpb_path;
    std::string reason;
};

struct McpbInvalidManifestError {
    std::string source;
    std::string plugin;
    std::string mcpb_path;
    std::string validation_error;
};

struct LspServerStartFailedError {
    std::string source;
    std::string plugin;
    std::string server_name;
    std::string reason;
};

struct LspServerCrashedError {
    std::string source;
    std::string plugin;
    std::string server_name;
    std::optional<int32_t> exit_code;
    std::optional<std::string> signal;
};

struct LspRequestTimeoutError {
    std::string source;
    std::string plugin;
    std::string server_name;
    std::string method;
    int64_t timeout_ms;
};

struct LspRequestFailedError {
    std::string source;
    std::string plugin;
    std::string server_name;
    std::string method;
    std::string error;
};

struct MarketplaceBlockedByPolicyError {
    std::string source;
    std::optional<std::string> plugin;
    std::string marketplace;
    std::optional<bool> blocked_by_blocklist;
    std::vector<std::string> allowed_sources;
};

struct DependencyUnsatisfiedError {
    std::string source;
    std::string plugin;
    std::string dependency;
    DependencyReason reason;
};

struct PluginCacheMissError {
    std::string source;
    std::string plugin;
    std::string install_path;
};

struct GenericError {
    std::string source;
    std::optional<std::string> plugin;
    std::string error;
};

/// Discriminated union of all plugin error types
using PluginError = std::variant<
    PathNotFoundError,
    GitAuthFailedError,
    GitTimeoutError,
    NetworkError,
    ManifestParseError,
    ManifestValidationError,
    PluginNotFoundError,
    MarketplaceNotFoundError,
    MarketplaceLoadFailedError,
    McpConfigInvalidError,
    McpServerSuppressedDuplicateError,
    LspConfigInvalidError,
    HookLoadFailedError,
    ComponentLoadFailedError,
    McpbDownloadFailedError,
    McpbExtractFailedError,
    McpbInvalidManifestError,
    LspServerStartFailedError,
    LspServerCrashedError,
    LspRequestTimeoutError,
    LspRequestFailedError,
    MarketplaceBlockedByPolicyError,
    DependencyUnsatisfiedError,
    PluginCacheMissError,
    GenericError
>;

// ============================================================
// Plugin load result
// ============================================================

/// Result of loading plugins: enabled, disabled, and any errors
struct PluginLoadResult {
    std::vector<LoadedPlugin> enabled;
    std::vector<LoadedPlugin> disabled;
    std::vector<PluginError> errors;
};

// ============================================================
// Error message helper
// ============================================================

/// Get a human-readable error message from any PluginError variant
[[nodiscard]] inline std::string get_plugin_error_message(const PluginError& error) {
    return std::visit([](const auto& e) -> std::string {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, GenericError>) {
            return e.error;
        } else if constexpr (std::is_same_v<T, PathNotFoundError>) {
            return "Path not found: " + e.path;
        } else if constexpr (std::is_same_v<T, GitAuthFailedError>) {
            return "Git authentication failed: " + e.git_url;
        } else if constexpr (std::is_same_v<T, GitTimeoutError>) {
            return "Git timeout: " + e.git_url;
        } else if constexpr (std::is_same_v<T, NetworkError>) {
            return "Network error: " + e.url;
        } else if constexpr (std::is_same_v<T, ManifestParseError>) {
            return "Manifest parse error: " + e.parse_error;
        } else if constexpr (std::is_same_v<T, ManifestValidationError>) {
            return "Manifest validation failed";
        } else if constexpr (std::is_same_v<T, PluginNotFoundError>) {
            return "Plugin " + e.plugin_id + " not found in marketplace " + e.marketplace;
        } else if constexpr (std::is_same_v<T, MarketplaceNotFoundError>) {
            return "Marketplace " + e.marketplace + " not found";
        } else if constexpr (std::is_same_v<T, MarketplaceLoadFailedError>) {
            return "Marketplace " + e.marketplace + " failed to load: " + e.reason;
        } else if constexpr (std::is_same_v<T, McpConfigInvalidError>) {
            return "MCP server " + e.server_name + " invalid: " + e.validation_error;
        } else if constexpr (std::is_same_v<T, McpServerSuppressedDuplicateError>) {
            return "MCP server \"" + e.server_name + "\" skipped — duplicate of " + e.duplicate_of;
        } else if constexpr (std::is_same_v<T, LspConfigInvalidError>) {
            return "LSP server " + e.server_name + " invalid: " + e.validation_error;
        } else if constexpr (std::is_same_v<T, HookLoadFailedError>) {
            return "Hook load failed: " + e.reason;
        } else if constexpr (std::is_same_v<T, ComponentLoadFailedError>) {
            return "Component load failed from " + e.path + ": " + e.reason;
        } else if constexpr (std::is_same_v<T, McpbDownloadFailedError>) {
            return "Failed to download MCPB from " + e.url + ": " + e.reason;
        } else if constexpr (std::is_same_v<T, McpbExtractFailedError>) {
            return "Failed to extract MCPB " + e.mcpb_path + ": " + e.reason;
        } else if constexpr (std::is_same_v<T, McpbInvalidManifestError>) {
            return "MCPB manifest invalid at " + e.mcpb_path + ": " + e.validation_error;
        } else if constexpr (std::is_same_v<T, LspServerStartFailedError>) {
            return "LSP server \"" + e.server_name + "\" failed to start: " + e.reason;
        } else if constexpr (std::is_same_v<T, LspServerCrashedError>) {
            return "LSP server \"" + e.server_name + "\" crashed";
        } else if constexpr (std::is_same_v<T, LspRequestTimeoutError>) {
            return "LSP server \"" + e.server_name + "\" timed out on " + e.method;
        } else if constexpr (std::is_same_v<T, LspRequestFailedError>) {
            return "LSP server \"" + e.server_name + "\" " + e.method + " failed: " + e.error;
        } else if constexpr (std::is_same_v<T, MarketplaceBlockedByPolicyError>) {
            return "Marketplace '" + e.marketplace + "' is blocked by policy";
        } else if constexpr (std::is_same_v<T, DependencyUnsatisfiedError>) {
            return "Dependency \"" + e.dependency + "\" unsatisfied";
        } else if constexpr (std::is_same_v<T, PluginCacheMissError>) {
            return "Plugin \"" + e.plugin + "\" not cached at " + e.install_path;
        } else {
            return "Unknown plugin error";
        }
    }, error);
}

} // namespace cc::types::plugin
