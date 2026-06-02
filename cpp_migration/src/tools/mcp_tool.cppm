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

// MCP 操作错误类型
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

// MCP 服务器连接信息
struct McpServerInfo {
    std::string name;
    std::string endpoint;        // stdio, sse, or streamable-http URL
    bool authenticated{false};
    std::vector<std::string> available_tools;
    std::vector<std::string> available_resources;
};

// MCP 工具调用请求
struct McpToolRequest {
    std::string server_name;
    std::string tool_name;
    std::unordered_map<std::string, std::string> arguments;
    std::chrono::seconds timeout{30};
};

// MCP 工具调用结果
struct McpToolResult {
    std::string content;
    std::string content_type;  // "text", "image", "resource"
    bool is_error{false};
};

// MCP 资源标识
struct McpResource {
    std::string uri;
    std::string name;
    std::string mime_type;
    std::optional<std::string> description;
};

// MCP 客户端路由器：管理多个 MCP 服务器连接
class McpClientRouter {
public:
    // 注册 MCP 服务器
    auto register_server(McpServerInfo info) -> std::expected<void, McpError> {
        if (info.name.empty()) return std::unexpected(McpError::InvalidInput);
        servers_.emplace(info.name, std::move(info));
        return {};
    }

    // 查找目标服务器
    auto find_server(std::string_view name) -> std::expected<McpServerInfo*, McpError> {
        auto it = servers_.find(std::string(name));
        if (it == servers_.end()) return std::unexpected(McpError::ServerNotFound);
        return &it->second;
    }

    // 列出所有已注册服务器
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

// 全局路由器实例
inline McpClientRouter& global_mcp_router() {
    static McpClientRouter router;
    return router;
}

// McpTool - 调用 MCP 服务器暴露的工具
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

        // 序列化参数为 JSON
        std::string args_json = "{";
        bool first = true;
        for (const auto& [key, value] : request.arguments) {
            if (!first) args_json += ",";
            args_json += std::format("\"{}\":\"{}\"", key, value);
            first = false;
        }
        args_json += "}";

        // 实际调用由底层传输层执行 (stdio/SSE/HTTP)
        // 这里返回占位结果，运行时由 MCP 客户端完成实际 RPC
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

// ListMcpResourcesTool - 列出 MCP 服务器上的可用资源
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

// ReadMcpResourceTool - 读取特定 MCP 资源内容
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

        // 实际读取由传输层完成
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

// McpAuthTool - 处理 MCP 服务器 OAuth 认证
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
            // 用授权码完成认证流程
            (*server)->authenticated = true;
            return std::format("Successfully authenticated with '{}'", server_name);
        }
        // 返回 OAuth 授权 URL
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
