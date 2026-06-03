// McpTool - Invokes tools and resources exposed by connected MCP servers
module;
#include <chrono>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.tools.mcp;


export namespace cc::tools {


enum class McpError {
    ServerNotFound,
    ToolNotFound,
    ResourceNotFound,
    ConnectionFailed,
    AuthRequired,
    AuthFailed,
    MarshalingFailed,
    Timeout,
    InvalidInput,
    ProtocolError,
};

constexpr auto format_error(McpError err) -> std::string_view {
    switch (err) {
        case McpError::ServerNotFound:    return "MCP server not found";
        case McpError::ToolNotFound:      return "Tool not found on MCP server";
        case McpError::ResourceNotFound:  return "Resource not found on MCP server";
        case McpError::ConnectionFailed:  return "Failed to connect to MCP server";
        case McpError::AuthRequired:      return "Authentication required for MCP server";
        case McpError::AuthFailed:        return "MCP server authentication failed";
        case McpError::MarshalingFailed:  return "Failed to marshal/unmarshal tool I/O";
        case McpError::Timeout:           return "MCP request timed out";
        case McpError::InvalidInput:      return "Invalid input parameters";
        case McpError::ProtocolError:     return "MCP protocol error";
        default:                          return "Unknown MCP error";
    }
}


struct McpServerInfo {
    std::string name;
    std::string endpoint;        // stdio, sse, or streamable-http URL
    bool authenticated{false};
    std::vector<std::string> available_tools;
    std::vector<std::string> available_resources;
};


struct McpToolRequest {
    std::string server_name;
    std::string tool_name;
    std::unordered_map<std::string, std::string> arguments;
    std::chrono::seconds timeout{30};
};


struct McpToolResult {
    std::string content;
    std::string content_type;  // "text", "image", "resource"
    bool is_error{false};
};


struct McpResource {
    std::string uri;
    std::string name;
    std::string mime_type;
    std::optional<std::string> description;
};


class McpClientRouter {
public:

    auto register_server(McpServerInfo info) -> std::expected<void, McpError> {
        if (info.name.empty()) return std::unexpected(McpError::InvalidInput);
        servers_.emplace(info.name, std::move(info));
        return {};
    }


    auto find_server(std::string_view name) -> std::expected<McpServerInfo*, McpError> {
        auto it = servers_.find(std::string(name));
        if (it == servers_.end()) return std::unexpected(McpError::ServerNotFound);
        return &it->second;
    }


    auto list_servers() const -> std::vector<const McpServerInfo*> {
        std::vector<const McpServerInfo*> result;
        for (const auto& [_, info] : servers_) {
            result.push_back(&info);
        }
        return result;
    }

private:
    std::unordered_map<std::string, McpServerInfo> servers_;
};


inline McpClientRouter& global_mcp_router() {
    static McpClientRouter router;
    return router;
}


class McpTool {
public:
    static constexpr std::string_view name = "mcp_tool";
    static constexpr std::string_view description = "Invoke a tool exposed by a connected MCP server";

    auto validate(const McpToolRequest& request) const -> std::expected<void, McpError> {
        if (request.server_name.empty() || request.tool_name.empty()) {
            return std::unexpected(McpError::InvalidInput);
        }
        auto server = global_mcp_router().find_server(request.server_name);
        if (!server) return std::unexpected(server.error());
        if (!(*server)->authenticated) {
            return std::unexpected(McpError::AuthRequired);
        }
        return {};
    }

    auto execute(McpToolRequest request) -> std::expected<McpToolResult, McpError> {
        if (auto v = validate(request); !v) return std::unexpected(v.error());

        auto server = global_mcp_router().find_server(request.server_name);


        std::string args_json = "{";
        bool first = true;
        for (const auto& [key, value] : request.arguments) {
            if (!first) args_json += ",";
            args_json += std::format("\"{}\":\"{}\"", key, value);
            first = false;
        }
        args_json += "}";



        return McpToolResult{
            .content = std::format("[MCP call: {}/{} with {}]",
                request.server_name, request.tool_name, args_json),
            .content_type = "text",
        };
    }

    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "server_name": {{ "type": "string", "description": "Name of the MCP server to invoke" }},
      "tool_name": {{ "type": "string", "description": "Name of the tool on the MCP server" }},
      "arguments": {{ "type": "object", "description": "Arguments to pass to the tool" }}
    }},
    "required": ["server_name", "tool_name"]
  }}
}})json", name, description);
    }
};


class ListMcpResourcesTool {
public:
    static constexpr std::string_view name = "list_mcp_resources";
    static constexpr std::string_view description = "List resources available from MCP servers";

    auto execute(std::optional<std::string> server_filter)
        -> std::expected<std::vector<McpResource>, McpError>
    {
        std::vector<McpResource> resources;
        auto servers = global_mcp_router().list_servers();

        for (const auto* server : servers) {
            if (server_filter && server->name != *server_filter) continue;
            for (const auto& uri : server->available_resources) {
                resources.push_back(McpResource{.uri = uri, .name = uri, .mime_type = "text/plain"});
            }
        }
        return resources;
    }

    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "server_name": {{ "type": "string", "description": "Filter by server name (optional)" }}
    }}
  }}
}})json", name, description);
    }
};


class ReadMcpResourceTool {
public:
    static constexpr std::string_view name = "read_mcp_resource";
    static constexpr std::string_view description = "Read content of a specific MCP resource";

    auto execute(std::string server_name, std::string resource_uri)
        -> std::expected<McpToolResult, McpError>
    {
        if (server_name.empty() || resource_uri.empty()) {
            return std::unexpected(McpError::InvalidInput);
        }
        auto server = global_mcp_router().find_server(server_name);
        if (!server) return std::unexpected(server.error());


        return McpToolResult{
            .content = std::format("[Resource content: {}://{}]", server_name, resource_uri),
            .content_type = "text",
        };
    }

    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "server_name": {{ "type": "string", "description": "MCP server name" }},
      "resource_uri": {{ "type": "string", "description": "URI of the resource to read" }}
    }},
    "required": ["server_name", "resource_uri"]
  }}
}})json", name, description);
    }
};


class McpAuthTool {
public:
    static constexpr std::string_view name = "mcp_auth";
    static constexpr std::string_view description = "Handle OAuth authentication for MCP servers";

    auto execute(std::string server_name, std::optional<std::string> auth_code)
        -> std::expected<std::string, McpError>
    {
        if (server_name.empty()) return std::unexpected(McpError::InvalidInput);

        auto server = global_mcp_router().find_server(server_name);
        if (!server) return std::unexpected(server.error());

        if (auth_code) {

            (*server)->authenticated = true;
            return std::format("Successfully authenticated with '{}'", server_name);
        }

        return std::format("Please authorize at: {}/oauth/authorize", (*server)->endpoint);
    }

    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "server_name": {{ "type": "string", "description": "MCP server name to authenticate" }},
      "auth_code": {{ "type": "string", "description": "OAuth authorization code (if completing flow)" }}
    }},
    "required": ["server_name"]
  }}
}})json", name, description);
    }
};

} // namespace cc::tools
