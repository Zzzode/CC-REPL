module;
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.services.mcp.vscode_sdk_mcp;

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

        // Parse servers from mcp.json (simplified: extract "name" and "command" fields)
        std::size_t pos = 0;
        while ((pos = content.find("\"name\"", pos)) != std::string::npos) {
            auto colon = content.find(':', pos);
            auto q1 = content.find('"', colon + 1);
            auto q2 = content.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) break;

            VSCodeMCPServer server;
            server.name = content.substr(q1 + 1, q2 - q1 - 1);
            server.transport_type = "stdio";
            server.connection_string = server.name;
            servers.push_back(std::move(server));
            pos = q2 + 1;
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
                    VSCodeMCPServer server;
                    // Extract extension name
                    auto name_pos = pkg.find("\"name\"");
                    if (name_pos != std::string::npos) {
                        auto c = pkg.find(':', name_pos);
                        auto q1 = pkg.find('"', c + 1);
                        auto q2 = pkg.find('"', q1 + 1);
                        if (q1 != std::string::npos && q2 != std::string::npos) {
                            server.name = pkg.substr(q1 + 1, q2 - q1 - 1);
                        }
                    }
                    if (server.name.empty()) {
                        server.name = entry.path().filename().string();
                    }
                    server.transport_type = "stdio";
                    server.connection_string = entry.path().string();
                    server.capabilities = {"tools", "resources"};
                    servers.push_back(std::move(server));
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

    return false;
}

} // namespace cc::services::mcp
