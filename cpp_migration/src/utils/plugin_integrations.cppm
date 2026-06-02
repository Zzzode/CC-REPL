// Plugin Integrations Module
// Consolidates: mcpPluginIntegration, mcpbHandler, lspPluginIntegration,
//               lspRecommendation, hintRecommendation, gitAvailability,
//               addDirPluginSettings, orphanedPluginFilter
//
// Provides integration points between the plugin system and external systems:
// MCP servers, LSP servers, hint recommendations, git availability checks,
// --add-dir plugin settings, and orphaned plugin filtering.
module;

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module cc.utils.plugin_integrations;

import cc.utils.plugin_loader;
import cc.utils.plugin_identifier;

export namespace cc::utils::plugin_integrations {

// ─────────────────────────────────────────────────────────────────────────────
// MCP Plugin Integration (from mcpPluginIntegration)
// ─────────────────────────────────────────────────────────────────────────────

/// MCP server transport types
enum class McpTransportType : unsigned char {
    Stdio,
    Sse,
    StreamableHttp,
};

/// MCP server configuration (mirrors services/mcp/types)
struct McpServerConfig {
    McpTransportType transport = McpTransportType::Stdio;
    std::string command;
    std::vector<std::string> args;
    std::optional<std::map<std::string, std::string>> env;
    std::optional<std::string> url;  // for SSE/HTTP transport
    std::optional<std::filesystem::path> cwd;
};

/// Scoped MCP server configuration (plugin-associated)
struct ScopedMcpServerConfig {
    McpServerConfig config;
    std::string plugin_name;
    std::string plugin_source;
    bool is_plugin_managed = true;
};

/// Load MCP server configurations from all enabled plugins
[[nodiscard]] std::expected<std::map<std::string, ScopedMcpServerConfig>, std::string>
load_plugin_mcp_servers(const std::vector<cc::utils::plugin_loader::LoadedPlugin>& plugins);

/// Load MCP servers from a single plugin
[[nodiscard]] std::expected<std::map<std::string, McpServerConfig>, std::vector<cc::utils::plugin_loader::PluginError>>
load_mcp_servers_for_plugin(const cc::utils::plugin_loader::LoadedPlugin& plugin);

// ─────────────────────────────────────────────────────────────────────────────
// MCPB Handler (from mcpbHandler)
// ─────────────────────────────────────────────────────────────────────────────

/// DXT (Desktop Extension) manifest structure
struct DxtManifest {
    std::string name;
    std::string version;
    std::optional<std::string> description;
    std::optional<std::string> author;
};

/// Result of loading an MCPB file
struct McpbLoadResult {
    DxtManifest manifest;
    McpServerConfig mcp_config;
    std::filesystem::path extracted_path;
};

/// Status returned when MCPB needs user configuration
struct McpbNeedsConfig {
    std::string status = "needs-config";
    std::vector<std::string> required_fields;
};

using McpbResult = std::variant<McpbLoadResult, McpbNeedsConfig>;

/// Check if a path points to an MCPB source
[[nodiscard]] bool is_mcpb_source(const std::filesystem::path& path);

/// Load an MCPB file (download, extract, convert DXT manifest to MCP config)
[[nodiscard]] std::expected<McpbResult, std::string> load_mcpb_file(
    const std::filesystem::path& mcpb_path,
    const std::filesystem::path& plugin_path,
    std::string_view plugin_id,
    std::function<void(std::string_view)> status_callback = nullptr
);

/// User configuration schema for MCPB plugins
struct UserConfigField {
    std::string name;
    std::string type;  // "string", "number", "boolean"
    std::optional<std::string> description;
    std::optional<std::string> default_value;
    bool required = false;
};

using UserConfigSchema = std::vector<UserConfigField>;
using UserConfigValues = std::map<std::string, std::string>;

/// Load user configuration for an MCP server plugin
[[nodiscard]] std::optional<UserConfigValues> load_mcp_server_user_config(std::string_view plugin_id);

/// Validate user configuration against schema
[[nodiscard]] std::expected<void, std::string> validate_user_config(
    const UserConfigValues& values,
    const UserConfigSchema& schema
);

// ─────────────────────────────────────────────────────────────────────────────
// LSP Plugin Integration (from lspPluginIntegration)
// ─────────────────────────────────────────────────────────────────────────────

/// LSP server configuration from a plugin
struct LspServerConfig {
    std::string language_id;
    std::string command;
    std::vector<std::string> args;
    std::optional<std::map<std::string, std::string>> env;
    std::optional<std::filesystem::path> root_dir;
    std::optional<std::map<std::string, std::string>> initialization_options;
};

/// Load LSP server configurations from all enabled plugins
[[nodiscard]] std::vector<LspServerConfig> load_plugin_lsp_servers(
    const std::vector<cc::utils::plugin_loader::LoadedPlugin>& plugins
);

// ─────────────────────────────────────────────────────────────────────────────
// LSP Recommendation (from lspRecommendation)
// ─────────────────────────────────────────────────────────────────────────────

struct LspRecommendation {
    std::string plugin_id;
    std::string plugin_name;
    std::string language_id;
    std::string reason;
};

/// Get LSP plugin recommendations based on workspace file types
[[nodiscard]] std::vector<LspRecommendation> get_lsp_recommendations(
    const std::filesystem::path& workspace_root
);

/// Check if any LSP recommendations are available for the current workspace
[[nodiscard]] bool has_lsp_recommendations(const std::filesystem::path& workspace_root);

// ─────────────────────────────────────────────────────────────────────────────
// Hint Recommendation (from hintRecommendation)
// ─────────────────────────────────────────────────────────────────────────────

struct PluginHint {
    std::string plugin_id;
    std::string plugin_name;
    std::string marketplace_name;
    std::string reason;         // Why this plugin is recommended
    double confidence = 0.0;    // 0.0 - 1.0
};

/// Get plugin recommendations based on current context (workspace, history, etc.)
[[nodiscard]] std::vector<PluginHint> get_plugin_hints(
    const std::filesystem::path& workspace_root,
    std::size_t max_hints = 3
);

/// Record that a hint was shown (for analytics and de-duplication)
void record_hint_shown(std::string_view plugin_id);

/// Record that a hint was accepted (plugin was installed from a hint)
void record_hint_accepted(std::string_view plugin_id);

/// Record that a hint was dismissed
void record_hint_dismissed(std::string_view plugin_id);

/// Check if a hint has already been shown recently (suppress re-display)
[[nodiscard]] bool was_hint_recently_shown(std::string_view plugin_id);

// ─────────────────────────────────────────────────────────────────────────────
// Git Availability (from gitAvailability)
// ─────────────────────────────────────────────────────────────────────────────

/// Check if git is available on the system PATH
[[nodiscard]] bool check_git_available();

/// Get the git version string (e.g., "2.43.0")
[[nodiscard]] std::optional<std::string> get_git_version();

/// Check if the git version supports sparse-checkout cone mode (>= 2.25)
[[nodiscard]] bool supports_sparse_checkout();

// ─────────────────────────────────────────────────────────────────────────────
// Add-Dir Plugin Settings (from addDirPluginSettings)
// ─────────────────────────────────────────────────────────────────────────────

/// Get enabled plugins contributed by --add-dir flags and SDK plugins option
[[nodiscard]] std::map<std::string, bool> get_add_dir_enabled_plugins();

/// Get extra marketplace declarations from --add-dir flags
[[nodiscard]] std::map<std::string, std::string> get_add_dir_extra_marketplaces();

/// Register a plugin directory from --add-dir (session-only)
void register_add_dir_plugin(
    std::string_view plugin_id,
    const std::filesystem::path& dir_path,
    bool enabled = true
);

/// Clear all --add-dir registrations (for testing)
void clear_add_dir_plugins();

// ─────────────────────────────────────────────────────────────────────────────
// Orphaned Plugin Filter (from orphanedPluginFilter)
// ─────────────────────────────────────────────────────────────────────────────

/// Filter out plugins that are orphaned (installed but marketplace removed)
[[nodiscard]] std::vector<cc::utils::plugin_loader::LoadedPlugin> filter_orphaned_plugins(
    const std::vector<cc::utils::plugin_loader::LoadedPlugin>& plugins
);

/// Get the list of orphaned plugin IDs
[[nodiscard]] std::vector<std::string> get_orphaned_plugin_ids(
    const std::vector<cc::utils::plugin_loader::LoadedPlugin>& plugins
);

/// Check if a specific plugin is orphaned
[[nodiscard]] bool is_plugin_orphaned(std::string_view plugin_id);

} // namespace cc::utils::plugin_integrations
