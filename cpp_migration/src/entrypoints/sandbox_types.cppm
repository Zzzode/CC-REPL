/// @file sandbox_types.cppm
/// @brief Sandbox types for the Claude Code Agent SDK.
/// Migrated from src/entrypoints/sandboxTypes.ts
///
/// This file is the single source of truth for sandbox configuration types.
/// Both the SDK and the settings validation import from here.
module;

#include <string>
#include <vector>
#include <optional>
#include <unordered_map>

export module cc.entrypoints.sandbox_types;

export namespace cc::entrypoints::sandbox {

// ============================================================================
// Network Configuration
// ============================================================================

/// Network configuration for sandbox
struct SandboxNetworkConfig {
    /// Domains allowed for network access
    std::optional<std::vector<std::string>> allowed_domains;

    /// When true (managed settings), only allowedDomains and WebFetch allow rules
    /// from managed settings are respected. User/project/local/flag domains are ignored.
    std::optional<bool> allow_managed_domains_only;

    /// macOS only: Unix socket paths to allow.
    /// Ignored on Linux (seccomp cannot filter by path).
    std::optional<std::vector<std::string>> allow_unix_sockets;

    /// If true, allow all Unix sockets (disables blocking on both platforms)
    std::optional<bool> allow_all_unix_sockets;

    /// Allow binding to local ports
    std::optional<bool> allow_local_binding;

    /// HTTP proxy port for network interception
    std::optional<int> http_proxy_port;

    /// SOCKS proxy port
    std::optional<int> socks_proxy_port;
};

// ============================================================================
// Filesystem Configuration
// ============================================================================

/// Filesystem configuration for sandbox
struct SandboxFilesystemConfig {
    /// Additional paths to allow writing within the sandbox.
    /// Merged with paths from Edit(...) allow permission rules.
    std::optional<std::vector<std::string>> allow_write;

    /// Additional paths to deny writing within the sandbox.
    /// Merged with paths from Edit(...) deny permission rules.
    std::optional<std::vector<std::string>> deny_write;

    /// Additional paths to deny reading within the sandbox.
    /// Merged with paths from Read(...) deny permission rules.
    std::optional<std::vector<std::string>> deny_read;

    /// Paths to re-allow reading within denyRead regions.
    /// Takes precedence over deny_read for matching paths.
    std::optional<std::vector<std::string>> allow_read;

    /// When true (managed settings), only allowRead paths from policySettings are used.
    std::optional<bool> allow_managed_read_paths_only;
};

// ============================================================================
// Ripgrep Configuration
// ============================================================================

/// Custom ripgrep configuration for bundled ripgrep support
struct RipgrepConfig {
    std::string command;
    std::optional<std::vector<std::string>> args;
};

// ============================================================================
// Sandbox Settings
// ============================================================================

/// Complete sandbox settings
struct SandboxSettings {
    /// Whether sandbox is enabled
    std::optional<bool> enabled;

    /// Exit with error at startup if sandbox cannot start.
    /// When false (default), a warning is shown and commands run unsandboxed.
    /// Intended for managed-settings deployments requiring sandboxing as a hard gate.
    std::optional<bool> fail_if_unavailable;

    /// Auto-allow bash commands when sandbox is active
    std::optional<bool> auto_allow_bash_if_sandboxed;

    /// Allow commands to run outside the sandbox via dangerouslyDisableSandbox parameter.
    /// When false, the parameter is completely ignored. Default: true.
    std::optional<bool> allow_unsandboxed_commands;

    /// Network configuration
    std::optional<SandboxNetworkConfig> network;

    /// Filesystem configuration
    std::optional<SandboxFilesystemConfig> filesystem;

    /// Map of violation categories to patterns to ignore
    /// Key: violation category, Value: list of patterns
    std::optional<std::unordered_map<std::string, std::vector<std::string>>> ignore_violations;

    /// Enable weaker nested sandbox for compatibility
    std::optional<bool> enable_weaker_nested_sandbox;

    /// macOS only: Allow access to com.apple.trustd.agent in the sandbox.
    /// Needed for Go-based CLI tools to verify TLS certificates
    /// when using httpProxyPort with a MITM proxy and custom CA.
    /// **Reduces security** - opens a potential data exfiltration vector. Default: false
    std::optional<bool> enable_weaker_network_isolation;

    /// Commands excluded from sandboxing
    std::optional<std::vector<std::string>> excluded_commands;

    /// Custom ripgrep configuration
    std::optional<RipgrepConfig> ripgrep;
};

/// Type alias for the ignore violations map
using SandboxIgnoreViolations = std::unordered_map<std::string, std::vector<std::string>>;

// ============================================================================
// Validation
// ============================================================================

/// Validate sandbox network configuration
[[nodiscard]] inline bool validate_network_config(const SandboxNetworkConfig& config) {
    // http_proxy_port and socks_proxy_port must be valid port numbers if present
    if (config.http_proxy_port && (*config.http_proxy_port < 0 || *config.http_proxy_port > 65535)) {
        return false;
    }
    if (config.socks_proxy_port && (*config.socks_proxy_port < 0 || *config.socks_proxy_port > 65535)) {
        return false;
    }
    return true;
}

/// Validate sandbox settings
[[nodiscard]] inline bool validate_sandbox_settings(const SandboxSettings& settings) {
    if (settings.network) {
        if (!validate_network_config(*settings.network)) {
            return false;
        }
    }
    return true;
}

} // namespace cc::entrypoints::sandbox
