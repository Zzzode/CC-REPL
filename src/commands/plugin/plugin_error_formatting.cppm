/// @file plugin_error_formatting.cppm
/// @brief Plugin error formatting and guidance helpers.
///
/// Pure-logic extraction from src/commands/plugin/PluginErrors.tsx.
/// Converts structured PluginError variants to human-readable messages
/// and user-facing troubleshooting guidance. No React/FTXUI dependencies.

module;

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <format>
#include <algorithm>
#include <ranges>

export module cc.commands.plugin_error_formatting;

export namespace cc::commands::plugin {

// ---------------------------------------------------------------------------
// Plugin error type enumeration
// ---------------------------------------------------------------------------

/// Mirrors the TS PluginError discriminated-union `type` field.
enum class ErrorType : std::uint8_t {
    PathNotFound,
    GitAuthFailed,
    GitTimeout,
    NetworkError,
    ManifestParseError,
    ManifestValidationError,
    PluginNotFound,
    MarketplaceNotFound,
    MarketplaceLoadFailed,
    McpConfigInvalid,
    McpServerSuppressedDuplicate,
    HookLoadFailed,
    ComponentLoadFailed,
    McpbDownloadFailed,
    McpbExtractFailed,
    McpbInvalidManifest,
    MarketplaceBlockedByPolicy,
    DependencyUnsatisfied,
    LspConfigInvalid,
    LspServerStartFailed,
    LspServerCrashed,
    LspRequestTimeout,
    LspRequestFailed,
    PluginCacheMiss,
    GenericError,
};

/// Optional fields used by specific error variants. Fields not relevant
/// to a given variant are left as empty/nullopt.
struct PluginErrorDetail {
    // PathNotFound / ComponentLoadFailed / HookLoadFailed
    std::string component;
    std::string path;
    std::string hook_path;

    // Git* variants
    std::string auth_type;   // "ssh" | "https" | ...
    std::string git_url;
    std::string operation;   // clone | fetch | push

    // NetworkError
    std::string url;
    std::string details;

    // Manifest* variants
    std::string manifest_path;
    std::string parse_error;
    std::vector<std::string> validation_errors;

    // PluginNotFound / Marketplace* variants / Dependency*
    std::string plugin_id;
    std::string plugin;     // cache miss
    std::string marketplace;
    std::string reason;     // marketplace-load / hook/component/dep fail
    std::vector<std::string> available_marketplaces;
    std::vector<std::string> allowed_sources;
    bool blocked_by_blocklist = false;

    // Mcp variants
    std::string server_name;
    std::string validation_error;
    std::string duplicate_of;  // "plugin:name:srv" or existing config key

    // Mcpb variants
    std::string mcpb_path;

    // DependencyUnsatisfied
    std::string dependency;

    // Lsp variants
    std::string method;
    std::uint64_t timeout_ms = 0;
    int exit_code = 0;
    std::string signal;
    std::string lsp_error;

    // GenericError
    std::string error;

    // Install-path
    std::string install_path;
};

// ---------------------------------------------------------------------------
// formatErrorMessage
// ---------------------------------------------------------------------------

/// Render a PluginError as a single-line (or multi-line) user-readable string.
/// Mirrors TS: formatErrorMessage(error: PluginError): string
[[nodiscard]] inline std::string format_error_message(
    ErrorType type, const PluginErrorDetail& d)
{
    using enum ErrorType;
    switch (type) {
        case PathNotFound:
            return std::format("{} path not found: {}", d.component, d.path);
        case GitAuthFailed: {
            std::string upper{d.auth_type};
            std::ranges::transform(upper, upper.begin(), ::toupper);
            return std::format("Git {} authentication failed for {}", upper, d.git_url);
        }
        case GitTimeout:
            return std::format("Git {} timed out for {}", d.operation, d.git_url);
        case NetworkError:
            if (!d.details.empty())
                return std::format("Network error accessing {}: {}", d.url, d.details);
            return std::format("Network error accessing {}", d.url);
        case ManifestParseError:
            return std::format("Failed to parse manifest at {}: {}", d.manifest_path, d.parse_error);
        case ManifestValidationError: {
            std::string joined;
            for (std::size_t i = 0; i < d.validation_errors.size(); ++i) {
                if (i) joined += ", ";
                joined += d.validation_errors[i];
            }
            return std::format("Invalid manifest at {}: {}", d.manifest_path, joined);
        }
        case PluginNotFound:
            return std::format("Plugin \"{}\" not found in marketplace \"{}\"", d.plugin_id, d.marketplace);
        case MarketplaceNotFound:
            return std::format("Marketplace \"{}\" not found", d.marketplace);
        case MarketplaceLoadFailed:
            return std::format("Failed to load marketplace \"{}\": {}", d.marketplace, d.reason);
        case McpConfigInvalid:
            return std::format("Invalid MCP server config for \"{}\": {}", d.server_name, d.validation_error);
        case McpServerSuppressedDuplicate: {
            std::string dup;
            if (d.duplicate_of.starts_with("plugin:")) {
                std::string_view sv = d.duplicate_of;
                auto colon = sv.find(':', 7);
                auto name = (colon != std::string_view::npos)
                    ? sv.substr(7, colon - 7)
                    : sv.substr(7);
                dup = std::format("server provided by plugin \"{}\"", name.empty() ? "?" : name);
            } else {
                dup = std::format("already-configured \"{}\"", d.duplicate_of);
            }
            return std::format("MCP server \"{}\" skipped \u2014 same command/URL as {}", d.server_name, dup);
        }
        case HookLoadFailed:
            return std::format("Failed to load hooks from {}: {}", d.hook_path, d.reason);
        case ComponentLoadFailed:
            return std::format("Failed to load {} from {}: {}", d.component, d.path, d.reason);
        case McpbDownloadFailed:
            return std::format("Failed to download MCPB from {}: {}", d.url, d.reason);
        case McpbExtractFailed:
            return std::format("Failed to extract MCPB {}: {}", d.mcpb_path, d.reason);
        case McpbInvalidManifest:
            return std::format("MCPB manifest invalid at {}: {}", d.mcpb_path, d.validation_error);
        case MarketplaceBlockedByPolicy:
            if (d.blocked_by_blocklist)
                return std::format("Marketplace \"{}\" is blocked by enterprise policy", d.marketplace);
            return std::format("Marketplace \"{}\" is not in the allowed marketplace list", d.marketplace);
        case DependencyUnsatisfied:
            if (d.reason == "not-enabled")
                return std::format("Dependency \"{}\" is disabled", d.dependency);
            return std::format("Dependency \"{}\" is not installed", d.dependency);
        case LspConfigInvalid:
            return std::format("Invalid LSP server config for \"{}\": {}", d.server_name, d.validation_error);
        case LspServerStartFailed:
            return std::format("LSP server \"{}\" failed to start: {}", d.server_name, d.reason);
        case LspServerCrashed:
            if (!d.signal.empty())
                return std::format("LSP server \"{}\" crashed with signal {}", d.server_name, d.signal);
            return std::format("LSP server \"{}\" crashed with exit code {}", d.server_name,
                               d.exit_code != 0 ? std::to_string(d.exit_code) : "unknown");
        case LspRequestTimeout:
            return std::format("LSP server \"{}\" timed out on {} after {}ms", d.server_name, d.method, d.timeout_ms);
        case LspRequestFailed:
            return std::format("LSP server \"{}\" {} failed: {}", d.server_name, d.method, d.lsp_error);
        case PluginCacheMiss:
            return std::format("Plugin \"{}\" not cached at {}", d.plugin, d.install_path);
        case GenericError:
            return d.error;
    }
    return {};
}

// ---------------------------------------------------------------------------
// getErrorGuidance
// ---------------------------------------------------------------------------

/// Return user-facing troubleshooting text for an error, or nullopt when
/// no guidance applies. Mirrors TS: getErrorGuidance(error: PluginError)
[[nodiscard]] inline std::optional<std::string> get_error_guidance(
    ErrorType type, const PluginErrorDetail& d)
{
    using enum ErrorType;
    switch (type) {
        case PathNotFound:
            return "Check that the path in your manifest or marketplace config is correct";
        case GitAuthFailed:
            if (d.auth_type == "ssh")
                return "Configure SSH keys or use HTTPS URL instead";
            return "Configure credentials or use SSH URL instead";
        case GitTimeout:
        case NetworkError:
            return "Check your internet connection and try again";
        case ManifestParseError:
            return "Check manifest file syntax in the plugin directory";
        case ManifestValidationError:
            return "Check manifest file follows the required schema";
        case PluginNotFound:
            return std::format("Plugin may not exist in marketplace \"{}\"", d.marketplace);
        case MarketplaceNotFound:
            if (!d.available_marketplaces.empty()) {
                std::string joined;
                for (std::size_t i = 0; i < d.available_marketplaces.size(); ++i) {
                    if (i) joined += ", ";
                    joined += d.available_marketplaces[i];
                }
                return std::format("Available marketplaces: {}", joined);
            }
            return "Add the marketplace first using /plugin marketplace add";
        case McpConfigInvalid:
            return "Check MCP server configuration in .mcp.json or manifest";
        case McpServerSuppressedDuplicate: {
            if (d.duplicate_of.starts_with("plugin:")) {
                std::string_view sv = d.duplicate_of;
                auto colon = sv.find(':', 7);
                auto name = (colon != std::string_view::npos)
                    ? sv.substr(7, colon - 7)
                    : sv.substr(7);
                std::string winner{name.empty() ? "the other plugin" : name};
                return std::format("Disable plugin \"{}\" if you want this plugin's version instead", winner);
            }
            return std::format("Remove \"{}\" from your MCP config if you want the plugin's version instead",
                               d.duplicate_of);
        }
        case HookLoadFailed:
            return "Check hooks.json file syntax and structure";
        case ComponentLoadFailed:
            return std::format("Check {} directory structure and file permissions", d.component);
        case McpbDownloadFailed:
            return "Check your internet connection and URL accessibility";
        case McpbExtractFailed:
            return "Verify the MCPB file is valid and not corrupted";
        case McpbInvalidManifest:
            return "Contact the plugin author about the invalid manifest";
        case MarketplaceBlockedByPolicy:
            if (d.blocked_by_blocklist)
                return "This marketplace source is explicitly blocked by your administrator";
            if (!d.allowed_sources.empty()) {
                std::string joined;
                for (std::size_t i = 0; i < d.allowed_sources.size(); ++i) {
                    if (i) joined += ", ";
                    joined += d.allowed_sources[i];
                }
                return std::format("Allowed sources: {}", joined);
            }
            return "Contact your administrator to configure allowed marketplace sources";
        case DependencyUnsatisfied:
            if (d.reason == "not-enabled")
                return std::format("Enable \"{}\" or uninstall \"{}\"", d.dependency, d.plugin);
            return std::format("Install \"{}\" or uninstall \"{}\"", d.dependency, d.plugin);
        case LspConfigInvalid:
            return "Check LSP server configuration in the plugin manifest";
        case LspServerStartFailed:
        case LspServerCrashed:
        case LspRequestTimeout:
        case LspRequestFailed:
            return "Check LSP server logs with --debug for details";
        case PluginCacheMiss:
            return "Run /plugins to refresh the plugin cache";
        case MarketplaceLoadFailed:
        case GenericError:
            return std::nullopt;
    }
    return std::nullopt;
}

} // namespace cc::commands::plugin
