/// @file mcp_types.cppm
/// @brief Canonical MCP (Model Context Protocol) configuration data types.
/// Rank-1 leaf: defines McpOAuthConfig/McpServerConfig with zero cc.* imports;
/// cc.config.config re-exports it so existing importers stay unchanged.
export module cc.config.mcp_types;

import std;

export namespace cc::core {

/// MCP (Model Context Protocol) server configuration
struct McpOAuthConfig {
    std::optional<std::string> auth_server_metadata_url = std::nullopt;
    std::optional<int> callback_port = std::nullopt;
    std::optional<std::string> client_id = std::nullopt;
    bool xaa = false;
    std::optional<std::string> issuer = std::nullopt;
};

struct McpServerConfig {
    std::string name;                              // Server identifier
    std::string command;                           // Launch command
    std::vector<std::string> args;                 // Command arguments
    std::unordered_map<std::string, std::string> env;  // Environment variables
    std::string transport = "stdio";               // stdio, sse, or http
    std::optional<std::string> url = std::nullopt;  // Remote MCP endpoint
    std::unordered_map<std::string, std::string> headers = {}; // Static HTTP headers
    std::optional<std::string> headers_helper = std::nullopt; // Command that emits dynamic headers JSON
    std::optional<McpOAuthConfig> oauth = std::nullopt; // Remote OAuth/XAA settings
    std::optional<bool> disabled = std::nullopt;    // Optional explicit enable/disable
    std::string config_scope{"project"};               // Where this config is stored: local/user/project
};

} // namespace cc::core
