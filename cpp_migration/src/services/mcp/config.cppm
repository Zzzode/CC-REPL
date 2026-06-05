// MCP Server Configuration - Parsing, validation, and merging of MCP server configs
module;
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.services.mcp.config;

import cc.services.mcp.types;

export namespace cc::services::mcp {

// Error types for configuration operations
enum class ConfigError {
    FileNotFound,
    ParseError,
    ValidationError,
    PermissionDenied,
    InvalidSchema,
};

// Configuration scope hierarchy (later scopes override earlier)
enum class ConfigScope {
    Global,   // ~/.config/claude/mcp_servers.json
    User,     // ~/.claude/mcp_servers.json
    Project,  // .claude/mcp_servers.json (project root)
    Local,    // .claude/mcp_servers.local.json (gitignored)
};

// Single MCP server configuration entry
struct ServerConfig {
    std::string name;                // Unique server identifier
    TransportType transport = TransportType::Stdio;

    // Stdio transport settings
    std::string command;             // Executable path or command
    std::vector<std::string> args;   // Command arguments
    std::map<std::string, std::string> env; // Environment variables

    // SSE/HTTP transport settings
    std::string url;                 // Server URL for SSE/HTTP
    std::map<std::string, std::string> headers; // Custom HTTP headers
    std::string headers_helper;      // Optional command that prints JSON headers

    // Behavior settings
    std::chrono::milliseconds timeout{30000};
    bool auto_start = true;          // Start automatically on client launch
    bool enabled = true;             // Whether server is active
    ConfigScope scope = ConfigScope::Project;

    // Validation: check that required fields are present
    [[nodiscard]] std::expected<void, std::string> validate() const;
};

// Complete MCP configuration (merged from all scopes)
struct McpConfig {
    std::map<std::string, ServerConfig> servers;
    std::string config_version = "1.0";

    // Get servers that should auto-start
    [[nodiscard]] std::vector<const ServerConfig*> auto_start_servers() const;

    // Get a specific server config by name
    [[nodiscard]] std::optional<std::reference_wrapper<const ServerConfig>> 
    get_server(std::string_view name) const;
};

// Configuration file path resolver
class ConfigPaths {
public:
    // Get global config path (~/.config/claude/mcp_servers.json)
    [[nodiscard]] static std::filesystem::path global_config();

    // Get user config path (~/.claude/mcp_servers.json)
    [[nodiscard]] static std::filesystem::path user_config();

    // Get project config path (relative to project root)
    [[nodiscard]] static std::filesystem::path project_config(const std::filesystem::path& project_root);

    // Get local (gitignored) config path
    [[nodiscard]] static std::filesystem::path local_config(const std::filesystem::path& project_root);

    // Get all config paths in priority order (lowest to highest)
    [[nodiscard]] static std::vector<std::pair<ConfigScope, std::filesystem::path>> 
    all_config_paths(const std::filesystem::path& project_root);

private:
    [[nodiscard]] static std::filesystem::path home_directory();
};

// Configuration parser - reads and validates config files
class ConfigParser {
public:
    // Parse a single config file
    [[nodiscard]] static std::expected<std::map<std::string, ServerConfig>, ConfigError>
    parse_file(const std::filesystem::path& path, ConfigScope scope);

    // Parse JSON content into server configs
    [[nodiscard]] static std::expected<std::map<std::string, ServerConfig>, ConfigError>
    parse_json(const std::string& json, ConfigScope scope);
};

// Configuration loader - reads and merges configs from all scopes
class ConfigLoader {
public:
    explicit ConfigLoader(std::filesystem::path project_root);

    // Load and merge all configuration files
    [[nodiscard]] std::expected<McpConfig, ConfigError> load() const;

    // Load only a specific scope
    [[nodiscard]] std::expected<McpConfig, ConfigError> load_scope(ConfigScope scope) const;

    // Write a server config to the specified scope
    [[nodiscard]] std::expected<void, ConfigError> save_server(
        const ServerConfig& server, ConfigScope scope) const;

private:
    // Serialize a server config to JSON
    [[nodiscard]] static std::string serialize_server(const ServerConfig& server);

    std::filesystem::path project_root_;
};

} // namespace cc::services::mcp
