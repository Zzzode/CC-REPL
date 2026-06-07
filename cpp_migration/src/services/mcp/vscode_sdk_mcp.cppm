module;
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.services.mcp.vscode_sdk_mcp;

import cc.utils.json;

export namespace cc::services::mcp {

/// VS Code MCP SDK integration config
struct VSCodeMCPConfig {
    std::string extension_id;
    std::string workspace_path;
    std::optional<std::string> socket_path;
    bool auto_discover{true};
};

/// Discovered VS Code MCP server
struct VSCodeMCPServer {
    std::string name;
    std::string transport_type;
    std::string connection_string;
    std::vector<std::string> capabilities;
};

namespace detail {

[[nodiscard]] inline std::optional<std::string> json_string(
    cc::utils::json::JsonVal object,
    std::string_view key) {
    if (!object.is_obj()) return std::nullopt;
    auto value = object.get(key);
    if (!value.is_str()) return std::nullopt;
    return std::string(value.as_str());
}

[[nodiscard]] inline std::string normalize_transport(
    cc::utils::json::JsonVal config,
    const std::string& connection_string) {
    auto transport = json_string(config, "type");
    if (!transport) transport = json_string(config, "transport");
    if (!transport && connection_string.starts_with("ws://")) return "ws";
    if (!transport && (connection_string.starts_with("http://") ||
                       connection_string.starts_with("https://"))) return "sse";
    return transport.value_or("stdio");
}

inline void add_server_from_config(
    std::vector<VSCodeMCPServer>& servers,
    std::string name,
    cc::utils::json::JsonVal config) {
    if (!config.is_obj()) return;

    if (auto explicit_name = json_string(config, "name"); explicit_name && !explicit_name->empty()) {
        name = std::move(*explicit_name);
    }

    std::string connection_string;
    if (auto command = json_string(config, "command"); command && !command->empty()) {
        connection_string = std::move(*command);
    } else if (auto url = json_string(config, "url"); url && !url->empty()) {
        connection_string = std::move(*url);
    } else if (auto socket = json_string(config, "socketPath"); socket && !socket->empty()) {
        connection_string = std::move(*socket);
    } else if (auto socket = json_string(config, "socket_path"); socket && !socket->empty()) {
        connection_string = std::move(*socket);
    }

    if (name.empty() || connection_string.empty()) return;

    VSCodeMCPServer server;
    server.name = std::move(name);
    server.transport_type = normalize_transport(config, connection_string);
    server.connection_string = std::move(connection_string);
    server.capabilities = {"tools"};
    servers.push_back(std::move(server));
}

inline void add_servers_from_object(
    std::vector<VSCodeMCPServer>& servers,
    cc::utils::json::JsonVal object) {
    if (!object.is_obj()) return;
    object.iter_obj([&servers](cc::utils::json::JsonVal key, cc::utils::json::JsonVal value) {
        if (!key.is_str()) return;
        add_server_from_config(servers, std::string(key.as_str()), value);
    });
}

inline void add_servers_from_document(
    std::vector<VSCodeMCPServer>& servers,
    cc::utils::json::JsonVal root) {
    if (!root.is_obj()) return;

    const auto before_count = servers.size();
    add_servers_from_object(servers, root.get("servers"));
    add_servers_from_object(servers, root.get("mcpServers"));

    auto contributes = root.get("contributes");
    if (contributes.is_obj()) {
        add_servers_from_object(servers, contributes.get("mcpServers"));
        auto mcp = contributes.get("mcp");
        if (mcp.is_obj()) {
            add_servers_from_object(servers, mcp.get("servers"));
            add_servers_from_object(servers, mcp.get("mcpServers"));
        }
    }

    if (servers.size() == before_count && root.get("name").is_str()) {
        add_server_from_config(servers, std::string(root.get("name").as_str()), root);
    }
}

} // namespace detail

/// Discover MCP servers from VS Code extensions
inline std::vector<VSCodeMCPServer> discover_vscode_mcp_servers(
    std::string_view workspace_path) {
    namespace fs = std::filesystem;
    std::vector<VSCodeMCPServer> servers;

    if (workspace_path.empty()) return servers;

    // Check .vscode/mcp.json in the workspace
    auto mcp_config = fs::path(std::string(workspace_path)) / ".vscode" / "mcp.json";
    if (fs::exists(mcp_config)) {
        std::ifstream ifs(mcp_config);
        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());

        auto doc = cc::utils::json::parse(content);
        if (doc) {
            detail::add_servers_from_document(servers, doc->root());
        }
    }

    // Check VS Code extensions directory for MCP-capable extensions
    const char* home = std::getenv("HOME");
    if (home) {
        auto extensions_dir = fs::path(home) / ".vscode" / "extensions";
        if (fs::exists(extensions_dir)) {
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(extensions_dir, ec)) {
                if (!entry.is_directory()) continue;
                auto package_json = entry.path() / "package.json";
                if (!fs::exists(package_json)) continue;

                std::ifstream pf(package_json);
                std::string pkg((std::istreambuf_iterator<char>(pf)),
                                 std::istreambuf_iterator<char>());

                // Check if extension contributes MCP servers
                if (pkg.find("\"mcp\"") != std::string::npos ||
                    pkg.find("\"mcpServers\"") != std::string::npos) {
                    const auto before_count = servers.size();
                    auto doc = cc::utils::json::parse(pkg);
                    if (doc) {
                        detail::add_servers_from_document(servers, doc->root());
                    }
                    if (servers.size() == before_count) {
                        VSCodeMCPServer server;
                        server.name = entry.path().filename().string();
                        if (doc) {
                            if (auto name = detail::json_string(doc->root(), "name");
                                name && !name->empty()) {
                                server.name = std::move(*name);
                            }
                        }
                        server.transport_type = "stdio";
                        server.connection_string = entry.path().string();
                        server.capabilities = {"tools", "resources"};
                        servers.push_back(std::move(server));
                    }
                }
            }
        }
    }

    return servers;
}

/// Connect to a VS Code SDK MCP server
inline bool connect_vscode_mcp(const VSCodeMCPServer& server) {
    if (server.name.empty() || server.connection_string.empty()) {
        return false;
    }

    // For stdio transport, verify the command/path exists
    if (server.transport_type == "stdio") {
        namespace fs = std::filesystem;
        // If connection_string is a path, check it exists
        if (fs::exists(server.connection_string)) {
            return true;
        }
        // Otherwise treat as a command name — assume available
        return !server.connection_string.empty();
    }

    // For socket transport, check socket path
    if (server.transport_type == "socket") {
        namespace fs = std::filesystem;
        return fs::exists(server.connection_string);
    }

    if (server.transport_type == "sse" ||
        server.transport_type == "http" ||
        server.transport_type == "streamable_http" ||
        server.transport_type == "ws" ||
        server.transport_type == "websocket") {
        return server.connection_string.starts_with("http://") ||
               server.connection_string.starts_with("https://") ||
               server.connection_string.starts_with("ws://") ||
               server.connection_string.starts_with("wss://");
    }

    return false;
}

} // namespace cc::services::mcp
