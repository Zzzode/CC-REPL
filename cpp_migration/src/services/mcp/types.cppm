// MCP Types Module
module;
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

export module cc.services.mcp.types;

import cc.utils.error;
import cc.utils.json;

export namespace cc::services::mcp {

using cc::utils::Result;
using cc::utils::json::JsonDoc;
using cc::utils::json::JsonMutDoc;
using cc::utils::json::JsonVal;

// =========================================================================
// MCP Client Error types
// =========================================================================

enum class McpClientError {
    ConnectionFailed,
    NotConnected,
    AlreadyConnected,
    TransportError,
    ServerClosed,
    Timeout,
    InvalidResponse,
    InitializationFailed,
    ProtocolError,
    ServerNotFound,
    ToolNotFound,
    Unauthorized,
};

// Result type for MCP operations
template<typename T>
using McpResult = std::expected<T, McpClientError>;

// =========================================================================
// Connection Status
// =========================================================================

enum class ConnectionStatus {
    Disconnected,
    Connecting,
    Connected,
    NeedsAuth,
    Error,
};

// Callback types for connection manager events
using ServerConnectedCallback = std::function<void(const std::string& server_name)>;
using ServerDisconnectedCallback = std::function<void(const std::string& server_name)>;
using ServerErrorCallback = std::function<void(const std::string& server_name, const std::string& error)>;
using ToolsUpdatedCallback = std::function<void(const std::string& server_name, const std::vector<std::string>& tools)>;

// =========================================================================
// JSON-RPC types
// =========================================================================

using RequestId = std::variant<int64_t, std::string>;

struct JsonRpcNotification {
    std::string method;
    std::optional<std::string> params_json;
};

struct JsonRpcRequest {
    RequestId id;
    std::string method;
    std::optional<std::string> params_json;
};

// Helper to make a request
inline JsonRpcRequest make_request(RequestId id, std::string method, std::optional<std::string> params) {
    return JsonRpcRequest{.id = std::move(id), .method = std::move(method), .params_json = std::move(params)};
}

// Helper to make a notification
inline JsonRpcNotification make_notification(std::string method, std::optional<std::string> params) {
    return JsonRpcNotification{.method = std::move(method), .params_json = std::move(params)};
}

// Serialize request to JSON string
inline std::string serialize_request(const JsonRpcRequest& req) {
    JsonMutDoc doc;
    auto root = doc.object();
    root.add("jsonrpc", doc.string("2.0"));
    if (std::holds_alternative<int64_t>(req.id)) {
        root.add("id", doc.number(std::get<int64_t>(req.id)));
    } else {
        root.add("id", doc.string(std::get<std::string>(req.id)));
    }
    root.add("method", doc.string(req.method));
    if (req.params_json && !req.params_json->empty()) {
        auto params = doc.raw_json(*req.params_json);
        if (params.valid()) {
            root.add("params", params);
        }
    }
    doc.set_root(root);
    return doc.to_string();
}

// Serialize notification to JSON string
inline std::string serialize_notification(const JsonRpcNotification& notif) {
    JsonMutDoc doc;
    auto root = doc.object();
    root.add("jsonrpc", doc.string("2.0"));
    root.add("method", doc.string(notif.method));
    if (notif.params_json && !notif.params_json->empty()) {
        auto params = doc.raw_json(*notif.params_json);
        if (params.valid()) {
            root.add("params", params);
        }
    }
    doc.set_root(root);
    return doc.to_string();
}

// =========================================================================
// Transport types
// =========================================================================

enum class TransportType {
    Stdio,
    Sse,
    Http,
    StreamableHttp,
};

// =========================================================================
// JSON-RPC Error Codes (per spec)
// =========================================================================

enum class JsonRpcErrorCode : int32_t {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
};

// =========================================================================
// Server/Client info and capabilities
// =========================================================================

struct ClientInfo {
    std::string name = "claude-code";
    std::string version = "1.0.0";
};

struct RootsCapabilities {
    bool list_changed = true;
};

struct ClientCapabilities {
    bool roots = true;
    RootsCapabilities roots_capabilities;
};

struct ServerInfo {
    std::string name;
    std::string version;
};

struct ServerCapabilities {
    bool tools = false;
    bool tools_list_changed = false;
    bool resources = false;
    bool resources_list_changed = false;
    bool resources_subscribe = false;
    bool prompts = false;
    bool prompts_list_changed = false;
    bool logging = false;
};

enum class ServerState {
    NotStarted,
    Starting,
    Initializing,
    Ready,
    ShuttingDown,
    Stopped,
    Error,
};

// =========================================================================
// Root type
// =========================================================================

struct Root {
    std::string uri;
    std::optional<std::string> name;
};

// =========================================================================
// Tool types
// =========================================================================

struct McpTool {
    std::string name;
    std::string description;
    std::string input_schema_json;
};

struct ToolCallRequest {
    std::string name;
    std::string arguments_json;
};

struct ContentItem {
    std::string type = "text";
    std::string text;
    std::optional<std::string> media_type;  ///< for type="image" (TS: source.media_type)
    std::optional<std::string> data;        ///< base64 for type="image" (TS: source.data)
};

struct ToolCallResult {
    bool is_error = false;
    std::vector<ContentItem> content;
};

struct ListToolsResult {
    std::vector<McpTool> tools;
};

// =========================================================================
// Resource types
// =========================================================================

struct McpResource {
    std::string uri;
    std::string name;
    std::string description;
    std::string mime_type;
};

struct ResourceContent {
    std::string uri;
    std::string mime_type;
    std::string text;
    std::string blob;
};

struct ListResourcesResult {
    std::vector<McpResource> resources;
};

struct ResourceReadResult {
    std::vector<ResourceContent> contents;
};

// =========================================================================
// Prompt types
// =========================================================================

enum class PromptRole {
    User,
    Assistant,
};

struct McpPromptArgument {
    std::string name;
    std::string description;
    bool required = false;
};

struct McpPrompt {
    std::string name;
    std::string description;
    std::vector<McpPromptArgument> arguments;
};

struct McpPromptMessage {
    PromptRole role;
    std::string content;
};

struct ListPromptsResult {
    std::vector<McpPrompt> prompts;
};

struct PromptGetResult {
    std::string description;
    std::vector<McpPromptMessage> messages;
};

// =========================================================================
// Initialize result
// =========================================================================

struct InitializeResult {
    ServerInfo server_info;
    ServerCapabilities capabilities;
};

// Parse initialize result from JSON response
inline std::optional<InitializeResult> parse_initialize_result(const std::string& json_str) {
    auto doc = cc::utils::json::parse(json_str);
    if (!doc) return std::nullopt;
    
    auto root = doc->root();
    auto result_node = root.get("result");
    if (!result_node.is_obj()) return std::nullopt;
    
    InitializeResult result;
    auto info_node = result_node.get("serverInfo");
    if (info_node.is_obj()) {
        result.server_info.name = std::string(info_node.get("name").as_str());
        result.server_info.version = std::string(info_node.get("version").as_str());
    }
    
    auto caps_node = result_node.get("capabilities");
    if (caps_node.is_obj()) {
        auto tools_node = caps_node.get("tools");
        result.capabilities.tools = tools_node.valid();
        if (tools_node.is_obj()) {
            result.capabilities.tools_list_changed = tools_node.get("listChanged").as_bool();
        }
        auto resources_node = caps_node.get("resources");
        result.capabilities.resources = resources_node.valid();
        if (resources_node.is_obj()) {
            result.capabilities.resources_list_changed = resources_node.get("listChanged").as_bool();
            result.capabilities.resources_subscribe = resources_node.get("subscribe").as_bool();
        }
        auto prompts_node = caps_node.get("prompts");
        result.capabilities.prompts = prompts_node.valid();
        if (prompts_node.is_obj()) {
            result.capabilities.prompts_list_changed = prompts_node.get("listChanged").as_bool();
        }
        result.capabilities.logging = caps_node.get("logging").valid();
    }
    
    return result;
}

// Parse list tools result from JSON response
inline std::optional<ListToolsResult> parse_list_tools_result(const std::string& json_str) {
    auto doc = cc::utils::json::parse(json_str);
    if (!doc) return std::nullopt;
    
    auto root = doc->root();
    auto result_node = root.get("result");
    if (!result_node.is_obj()) return std::nullopt;
    
    ListToolsResult result;
    auto tools_node = result_node.get("tools");
    if (tools_node.is_arr()) {
        tools_node.iter([&result](JsonVal tool_val) {
            if (tool_val.is_obj()) {
                McpTool tool;
                tool.name = std::string(tool_val.get("name").as_str());
                tool.description = std::string(tool_val.get("description").as_str());
                result.tools.push_back(std::move(tool));
            }
        });
    }
    
    return result;
}

// Parse tool call result from JSON response
inline std::optional<ToolCallResult> parse_tool_call_result(const std::string& json_str) {
    auto doc = cc::utils::json::parse(json_str);
    if (!doc) return std::nullopt;
    
    auto root = doc->root();
    auto result_node = root.get("result");
    if (!result_node.is_obj()) return std::nullopt;
    
    ToolCallResult result;
    result.is_error = result_node.get("isError").as_bool();
    auto content_node = result_node.get("content");
    if (content_node.is_arr()) {
        content_node.iter([&result](JsonVal item) {
            if (!item.is_obj()) return;
            std::string type_str;
            if (auto t = item.get("type"); t.is_str()) type_str = t.as_str();

            if (type_str == "text") {
                ContentItem ci;
                ci.type = "text";
                ci.text = std::string(item.get("text").as_str());
                result.content.push_back(std::move(ci));
            } else if (type_str == "image") {
                // TS REF: transformResultContent for image — extracts
                // source.media_type and source.data from the block.
                ContentItem ci;
                ci.type = "image";
                // Try direct fields first (some MCP servers use flat format)
                if (auto mt = item.get("mimeType"); mt.is_str()) ci.media_type = mt.as_str();
                else if (auto mt = item.get("mediaType"); mt.is_str()) ci.media_type = mt.as_str();
                else if (auto mt = item.get("media_type"); mt.is_str()) ci.media_type = mt.as_str();
                if (auto d = item.get("data"); d.is_str()) ci.data = d.as_str();
                // Try nested "source" object (Anthropic API format)
                if (!ci.data) {
                    if (auto src = item.get("source"); src.is_obj()) {
                        if (auto mt = src.get("media_type"); mt.is_str()) ci.media_type = mt.as_str();
                        if (auto d = src.get("data"); d.is_str()) ci.data = d.as_str();
                    }
                }
                if (!ci.media_type) ci.media_type = "image/png";
                if (!ci.data) ci.data = "";
                result.content.push_back(std::move(ci));
            }
            // Other types (audio, resource, resource_link) are ignored for now
        });
    }
    
    return result;
}

// =========================================================================
// Server configuration types
// =========================================================================

struct McpOAuthConfig {
    std::optional<std::string> auth_server_metadata_url = std::nullopt;
    std::optional<int> callback_port = std::nullopt;
    std::optional<std::string> client_id = std::nullopt;
    bool xaa = false;
};

struct McpServerConfig {
    std::string type; // "stdio", "sse", "http"
    std::string url;
    std::unordered_map<std::string, std::string> headers = {};
    std::optional<McpOAuthConfig> oauth = std::nullopt;
};

// Common types for MCP protocol (legacy aliases)
struct Tool {
    std::string name;
    std::string description;
};

struct Resource {
    std::string uri;
    std::string name;
    std::string description;
};

struct Prompt {
    std::string name;
    std::string description;
};

// Build redirect URI
std::string build_redirect_uri(int port) {
    return "http://127.0.0.1:" + std::to_string(port) + "/callback";
}

// Find available port
int find_available_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 3000;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return 3000;
    }

    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        close(fd);
        return 3000;
    }
    auto port = ntohs(addr.sin_port);
    close(fd);
    return static_cast<int>(port);
}

// Get logging-safe MCP base URL
std::optional<std::string> get_logging_safe_mcp_base_url(const McpServerConfig& config) {
    if (config.url.empty()) {
        return std::nullopt;
    }
    try {
        return config.url;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace cc::services::mcp
