// MCP Server Configuration - Parsing, validation, and merging of MCP server configs
module;
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
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

    // Behavior settings
    std::chrono::milliseconds timeout{30000};
    bool auto_start = true;          // Start automatically on client launch
    bool enabled = true;             // Whether server is active
    ConfigScope scope = ConfigScope::Project;

    // Validation: check that required fields are present
    [[nodiscard]] std::expected<void, std::string> validate() const {
        if (name.empty()) {
            return std::unexpected("Server name is required");
        }
        if (transport == TransportType::Stdio && command.empty()) {
            return std::unexpected(std::format("Server '{}': command is required for stdio transport", name));
        }
        if ((transport == TransportType::Sse || transport == TransportType::StreamableHttp) && url.empty()) {
            return std::unexpected(std::format("Server '{}': url is required for SSE/HTTP transport", name));
        }
        return {};
    }
};

// Complete MCP configuration (merged from all scopes)
struct McpConfig {
    std::map<std::string, ServerConfig> servers;
    std::string config_version = "1.0";

    // Get servers that should auto-start
    [[nodiscard]] std::vector<const ServerConfig*> auto_start_servers() const {
        std::vector<const ServerConfig*> result;
        for (const auto& [_, server] : servers) {
            if (server.auto_start && server.enabled) {
                result.push_back(&server);
            }
        }
        return result;
    }

    // Get a specific server config by name
    [[nodiscard]] std::optional<std::reference_wrapper<const ServerConfig>> 
    get_server(std::string_view name) const {
        auto it = servers.find(std::string(name));
        if (it != servers.end()) return std::cref(it->second);
        return std::nullopt;
    }
};

// Configuration file path resolver
class ConfigPaths {
public:
    // Get global config path (~/.config/claude/mcp_servers.json)
    [[nodiscard]] static std::filesystem::path global_config() {
        auto home = home_directory();
        return home / ".config" / "claude" / "mcp_servers.json";
    }

    // Get user config path (~/.claude/mcp_servers.json)
    [[nodiscard]] static std::filesystem::path user_config() {
        auto home = home_directory();
        return home / ".claude" / "mcp_servers.json";
    }

    // Get project config path (relative to project root)
    [[nodiscard]] static std::filesystem::path project_config(const std::filesystem::path& project_root) {
        return project_root / ".claude" / "mcp_servers.json";
    }

    // Get local (gitignored) config path
    [[nodiscard]] static std::filesystem::path local_config(const std::filesystem::path& project_root) {
        return project_root / ".claude" / "mcp_servers.local.json";
    }

    // Get all config paths in priority order (lowest to highest)
    [[nodiscard]] static std::vector<std::pair<ConfigScope, std::filesystem::path>> 
    all_config_paths(const std::filesystem::path& project_root) {
        return {
            {ConfigScope::Global, global_config()},
            {ConfigScope::User, user_config()},
            {ConfigScope::Project, project_config(project_root)},
            {ConfigScope::Local, local_config(project_root)},
        };
    }

private:
    [[nodiscard]] static std::filesystem::path home_directory() {
        if (auto* home = std::getenv("HOME")) {
            return std::filesystem::path(home);
        }
        return std::filesystem::path("/tmp");
    }
};

// Configuration parser - reads and validates config files
class ConfigParser {
public:
    // Parse a single config file
    [[nodiscard]] static std::expected<std::map<std::string, ServerConfig>, ConfigError>
    parse_file(const std::filesystem::path& path, ConfigScope scope) {
        if (!std::filesystem::exists(path)) {
            return std::unexpected(ConfigError::FileNotFound);
        }

        // Read file content
        std::ifstream file(path);
        if (!file.is_open()) {
            return std::unexpected(ConfigError::PermissionDenied);
        }

        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

        return parse_json(content, scope);
    }

    // Parse JSON content into server configs
    [[nodiscard]] static std::expected<std::map<std::string, ServerConfig>, ConfigError>
    parse_json(const std::string& json, ConfigScope scope) {
        std::map<std::string, ServerConfig> servers;

        // Find "mcpServers" object (simplified JSON parsing)
        auto servers_pos = json.find("\"mcpServers\"");
        if (servers_pos == std::string::npos) {
            // Try alternative key
            servers_pos = json.find("\"servers\"");
        }
        if (servers_pos == std::string::npos) {
            return servers; // Empty config is valid
        }

        // Extract server entries (simplified - production uses proper JSON)
        auto entries = extract_server_entries(json, servers_pos);
        for (auto& [name, config] : entries) {
            config.scope = scope;
            auto validation = config.validate();
            if (validation) {
                servers[name] = std::move(config);
            }
        }
        return servers;
    }

private:
    // Extract individual server entries from JSON
    [[nodiscard]] static std::vector<std::pair<std::string, ServerConfig>>
    extract_server_entries(const std::string& json, size_t start_pos) {
        std::vector<std::pair<std::string, ServerConfig>> entries;

        // Simplified extraction - find key-value pairs within the servers object
        size_t pos = json.find('{', start_pos);
        if (pos == std::string::npos) return entries;

        // Scan for server name keys
        size_t depth = 0;
        size_t scan = pos;
        while (scan < json.size()) {
            if (json[scan] == '{') ++depth;
            else if (json[scan] == '}') {
                if (--depth == 0) break;
            }
            else if (json[scan] == '"' && depth == 1) {
                // Found a server name key
                auto name_end = json.find('"', scan + 1);
                if (name_end == std::string::npos) break;
                auto name = json.substr(scan + 1, name_end - scan - 1);

                // Parse this server's config
                auto config = parse_single_server(json, name_end, name);
                if (config) {
                    entries.emplace_back(name, std::move(*config));
                }
                scan = name_end + 1;
            }
            ++scan;
        }
        return entries;
    }

    // Parse a single server configuration block
    [[nodiscard]] static std::optional<ServerConfig>
    parse_single_server(const std::string& json, size_t after_name, const std::string& name) {
        ServerConfig config;
        config.name = name;

        // Find the server's object body
        auto obj_start = json.find('{', after_name);
        if (obj_start == std::string::npos) return std::nullopt;

        auto obj_end = find_matching_brace(json, obj_start);
        if (obj_end == std::string::npos) return std::nullopt;

        auto block = json.substr(obj_start, obj_end - obj_start + 1);

        // Extract fields
        config.command = extract_string(block, "command");
        config.url = extract_string(block, "url");

        // Determine transport type from available fields
        if (!config.url.empty()) {
            config.transport = config.url.contains("sse") ? TransportType::Sse : TransportType::StreamableHttp;
        } else {
            config.transport = TransportType::Stdio;
        }

        // Extract args array (simplified)
        auto args_str = extract_string(block, "args");
        if (!args_str.empty()) {
            config.args.push_back(args_str);
        }

        return config;
    }

    // Find matching closing brace
    [[nodiscard]] static size_t find_matching_brace(const std::string& json, size_t open_pos) {
        int depth = 0;
        for (size_t i = open_pos; i < json.size(); ++i) {
            if (json[i] == '{') ++depth;
            else if (json[i] == '}') {
                if (--depth == 0) return i;
            }
        }
        return std::string::npos;
    }

    // Extract a string value for a given key
    [[nodiscard]] static std::string extract_string(const std::string& json, std::string_view key) {
        auto pattern = std::format("\"{}\":", key);
        auto pos = json.find(pattern);
        if (pos == std::string::npos) return "";
        pos += pattern.size();
        // Skip whitespace
        while (pos < json.size() && std::isspace(json[pos])) ++pos;
        if (pos >= json.size() || json[pos] != '"') return "";
        auto end = json.find('"', pos + 1);
        if (end == std::string::npos) return "";
        return json.substr(pos + 1, end - pos - 1);
    }
};

// Configuration loader - reads and merges configs from all scopes
class ConfigLoader {
public:
    explicit ConfigLoader(std::filesystem::path project_root)
        : project_root_(std::move(project_root)) {}

    // Load and merge all configuration files
    [[nodiscard]] std::expected<McpConfig, ConfigError> load() const {
        McpConfig merged;

        // Load configs in priority order (later overrides earlier)
        for (const auto& [scope, path] : ConfigPaths::all_config_paths(project_root_)) {
            auto result = ConfigParser::parse_file(path, scope);
            if (result) {
                // Merge: later scope entries override earlier ones with same name
                for (auto& [name, config] : *result) {
                    merged.servers[name] = std::move(config);
                }
            }
            // FileNotFound is acceptable - just skip that scope
        }
        return merged;
    }

    // Load only a specific scope
    [[nodiscard]] std::expected<McpConfig, ConfigError> load_scope(ConfigScope scope) const {
        McpConfig config;
        std::filesystem::path path;

        switch (scope) {
            case ConfigScope::Global: path = ConfigPaths::global_config(); break;
            case ConfigScope::User: path = ConfigPaths::user_config(); break;
            case ConfigScope::Project: path = ConfigPaths::project_config(project_root_); break;
            case ConfigScope::Local: path = ConfigPaths::local_config(project_root_); break;
        }

        auto result = ConfigParser::parse_file(path, scope);
        if (!result) return std::unexpected(result.error());

        for (auto& [name, server] : *result) {
            config.servers[name] = std::move(server);
        }
        return config;
    }

    // Write a server config to the specified scope
    [[nodiscard]] std::expected<void, ConfigError> save_server(
        const ServerConfig& server, ConfigScope scope) const {
        std::filesystem::path path;
        switch (scope) {
            case ConfigScope::Global: path = ConfigPaths::global_config(); break;
            case ConfigScope::User: path = ConfigPaths::user_config(); break;
            case ConfigScope::Project: path = ConfigPaths::project_config(project_root_); break;
            case ConfigScope::Local: path = ConfigPaths::local_config(project_root_); break;
        }

        // Ensure parent directory exists
        std::filesystem::create_directories(path.parent_path());

        // Serialize and write (simplified)
        auto json = serialize_server(server);
        std::ofstream file(path, std::ios::app);
        if (!file.is_open()) return std::unexpected(ConfigError::PermissionDenied);
        file << json;
        return {};
    }

private:
    // Serialize a server config to JSON
    [[nodiscard]] static std::string serialize_server(const ServerConfig& server) {
        std::string json = std::format(R"("{}":{{)", server.name);
        if (server.transport == TransportType::Stdio) {
            json += std::format(R"("command":"{}")", server.command);
            if (!server.args.empty()) {
                json += R"(,"args":[)";
                for (size_t i = 0; i < server.args.size(); ++i) {
                    if (i > 0) json += ",";
                    json += std::format(R"("{}")", server.args[i]);
                }
                json += "]";
            }
        } else {
            json += std::format(R"("url":"{}")", server.url);
        }
        json += "}";
        return json;
    }

    std::filesystem::path project_root_;
};

} // namespace cc::services::mcp
