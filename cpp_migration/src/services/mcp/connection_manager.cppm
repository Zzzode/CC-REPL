// MCP Connection Manager - Manages multiple MCP server connections
module;

#include <memory>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <expected>
#include <utility>
#include <chrono>
#include <mutex>
#include <functional>
#include <filesystem>
#include <atomic>
#include <thread>
#include <algorithm>
#include <cctype>
#include <unordered_map>

export module cc.services.mcp.connection_manager;

import cc.services.mcp.types;
import cc.services.mcp.client;
import cc.services.mcp.config;
import cc.services.mcp.headers_helper;
import cc.services.mcp.auth;
import cc.services.mcp.at_mention_handler;
import cc.services.mcp.channel_notification;
import cc.utils.json;

export namespace cc::services::mcp {

using namespace cc::utils::json;

// Helper: convert McpClientError to string
[[nodiscard]] inline std::string error_to_string(McpClientError err) {
    switch (err) {
        case McpClientError::ConnectionFailed: return "connection failed";
        case McpClientError::NotConnected: return "not connected";
        case McpClientError::AlreadyConnected: return "already connected";
        case McpClientError::TransportError: return "transport error";
        case McpClientError::ServerClosed: return "server closed";
        case McpClientError::Timeout: return "timeout";
        case McpClientError::InvalidResponse: return "invalid response";
        case McpClientError::InitializationFailed: return "initialization failed";
        case McpClientError::ProtocolError: return "protocol error";
        case McpClientError::ServerNotFound: return "server not found";
        case McpClientError::ToolNotFound: return "tool not found";
        case McpClientError::Unauthorized: return "authentication required";
    }
    return "unknown error";
}

// Info about a connected MCP server (for callbacks)
struct ConnectedMcpServer {
    std::string name;
    ConnectionStatus status{ConnectionStatus::Disconnected};
    std::optional<std::string> server_info;
    std::optional<std::string> capabilities;
    std::vector<std::string> tools;
    std::vector<std::string> resources;
    std::vector<std::string> prompts;
    std::chrono::steady_clock::time_point connected_at;
};

struct McpServerSnapshot {
    std::string name;
    ConnectionStatus status{ConnectionStatus::Disconnected};
    std::optional<std::string> last_error;
    std::optional<std::string> endpoint;
    std::optional<std::string> server_info;
    std::optional<std::string> capabilities;
    std::vector<McpTool> tools;
    std::vector<McpResource> resources;
    std::vector<McpPrompt> prompts;
};

// Connection manager configuration
struct ConnectionManagerConfig {
    std::filesystem::path config_directory;
    std::chrono::milliseconds connection_timeout{30000};
    bool auto_connect_on_start{true};
};

// Server connection state
struct ServerConnection {
    std::string name;
    ServerConfig config;
    ConnectionStatus status{ConnectionStatus::Disconnected};
    std::unique_ptr<McpClient> client;
    std::optional<std::string> last_error;
    std::chrono::steady_clock::time_point last_connection_attempt;
    int retry_count{0};
};

// MCP Connection Manager
class McpConnectionManager {
public:
    explicit McpConnectionManager(ConnectionManagerConfig config)
        : config_(std::move(config)) {}
    
    ~McpConnectionManager() {
        shutdown();
    }
    
    // Initialize manager lifecycle
    McpResult<void> initialize() {
        shutting_down_.store(false);
        // Load configuration
        auto config_result = load_configuration();
        if (!config_result) {
            return std::unexpected(config_result.error());
        }
        
        // Auto-connect to servers
        if (config_.auto_connect_on_start) {
            connect_all_auto_servers();
        }
        
        return {};
    }
    
    void shutdown() {
        shutting_down_.store(true);
        // Disconnect all servers
        disconnect_all_servers();
        join_notification_workers();
    }
    
    // Configuration management
    McpResult<void> load_configuration() {
        ConfigLoader loader(config_.config_directory);
        auto config = loader.load();
        if (!config) {
            return std::unexpected(McpClientError::InitializationFailed);
        }
        mcp_config_ = *config;
        return {};
    }
    
    McpResult<void> reload_configuration() {
        shutting_down_.store(true);
        // Disconnect all first
        disconnect_all_servers();
        join_notification_workers();
        
        // Reload config
        auto result = load_configuration();
        if (!result) {
            return result;
        }

        shutting_down_.store(false);
        
        // Reconnect
        if (config_.auto_connect_on_start) {
            connect_all_auto_servers();
        }
        
        return {};
    }

    void set_configuration(McpConfig config) {
        shutting_down_.store(true);
        disconnect_all_servers();
        join_notification_workers();
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_.clear();
        mcp_config_ = std::move(config);
        shutting_down_.store(false);
    }
    
    // Server connection management
    McpResult<void> connect_server(const std::string& server_name) {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        
        // Find server config
        auto server_config = mcp_config_.get_server(server_name);
        if (!server_config) {
            return std::unexpected(McpClientError::ServerNotFound);
        }
        
        // Check if already connected/connecting
        auto it = connections_.find(server_name);
        if (it == connections_.end()) {
            // Create new connection entry
            ServerConnection conn;
            conn.name = server_name;
            conn.config = *server_config;
            conn.status = ConnectionStatus::Connecting;
            connections_[server_name] = std::move(conn);
        } else {
            if (it->second.status == ConnectionStatus::Connected || 
                it->second.status == ConnectionStatus::Connecting) {
                return std::unexpected(McpClientError::AlreadyConnected);
            }
            it->second.status = ConnectionStatus::Connecting;
            it->second.config = *server_config;
        }
        
        // Connect in background (simplified - in real impl would be async)
        return connect_server_internal(server_name, *server_config);
    }
    
    void disconnect_server(const std::string& server_name) {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        
        auto it = connections_.find(server_name);
        if (it != connections_.end()) {
            if (it->second.client) {
                it->second.client->shutdown();
                it->second.client.reset();
            }
            it->second.status = ConnectionStatus::Disconnected;
            
            if (server_disconnected_callback_) {
                server_disconnected_callback_(server_name);
            }
        }
    }
    
    void disconnect_all_servers() {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        
        for (auto& [name, conn] : connections_) {
            if (conn.client) {
                conn.client->shutdown();
                conn.client.reset();
            }
            conn.status = ConnectionStatus::Disconnected;
        }
    }
    
    // Server access
    std::optional<std::reference_wrapper<ServerConnection>> get_server(const std::string& server_name) {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        
        auto it = connections_.find(server_name);
        if (it == connections_.end()) {
            return std::nullopt;
        }
        return std::ref(it->second);
    }
    
    std::vector<std::reference_wrapper<ServerConnection>> get_all_servers() {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        
        std::vector<std::reference_wrapper<ServerConnection>> result;
        for (auto& [name, conn] : connections_) {
            result.push_back(std::ref(conn));
        }
        return result;
    }
    
    std::vector<McpTool> get_all_tools() {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        
        std::vector<McpTool> all_tools;
        for (auto& [name, conn] : connections_) {
            if (conn.status == ConnectionStatus::Connected && conn.client) {
                for (const auto& tool : conn.client->cached_tools()) {
                    // Optionally prefix tool name with server name
                    McpTool prefixed = tool;
                    prefixed.name = name + "::" + tool.name;
                    all_tools.push_back(prefixed);
                }
            }
        }
        return all_tools;
    }

    std::optional<McpServerSnapshot> snapshot_server(const std::string& server_name) {
        std::lock_guard<std::mutex> lock(connections_mutex_);

        auto config_it = mcp_config_.servers.find(server_name);
        auto conn_it = connections_.find(server_name);
        if (config_it == mcp_config_.servers.end() && conn_it == connections_.end()) {
            return std::nullopt;
        }

        McpServerSnapshot snapshot;
        snapshot.name = server_name;
        if (config_it != mcp_config_.servers.end()) {
            const auto& cfg = config_it->second;
            snapshot.endpoint = cfg.transport == TransportType::Stdio
                ? cfg.command
                : cfg.url;
        }

        if (conn_it == connections_.end()) {
            snapshot.status = ConnectionStatus::Disconnected;
            return snapshot;
        }

        const auto& conn = conn_it->second;
        snapshot.status = conn.status;
        snapshot.last_error = conn.last_error;
        if (conn.client) {
            const auto& info = conn.client->server_info();
            if (!info.name.empty() || !info.version.empty()) {
                snapshot.server_info = info.name + "@" + info.version;
            }
            const auto& caps = conn.client->server_capabilities();
            snapshot.capabilities = std::format("tools={},resources={},prompts={},logging={}",
                caps.tools, caps.resources, caps.prompts, caps.logging);
            snapshot.tools = conn.client->cached_tools();
            snapshot.resources = conn.client->cached_resources();
            snapshot.prompts = conn.client->cached_prompts();
        }
        return snapshot;
    }

    std::vector<McpServerSnapshot> snapshot_all_servers() {
        std::lock_guard<std::mutex> lock(connections_mutex_);

        std::vector<McpServerSnapshot> snapshots;
        for (const auto& [name, cfg] : mcp_config_.servers) {
            McpServerSnapshot snapshot;
            snapshot.name = name;
            snapshot.status = ConnectionStatus::Disconnected;
            snapshot.endpoint = cfg.transport == TransportType::Stdio ? cfg.command : cfg.url;

            auto conn_it = connections_.find(name);
            if (conn_it != connections_.end()) {
                const auto& conn = conn_it->second;
                snapshot.status = conn.status;
                snapshot.last_error = conn.last_error;
                if (conn.client) {
                    const auto& info = conn.client->server_info();
                    if (!info.name.empty() || !info.version.empty()) {
                        snapshot.server_info = info.name + "@" + info.version;
                    }
                    const auto& caps = conn.client->server_capabilities();
                    snapshot.capabilities = std::format("tools={},resources={},prompts={},logging={}",
                        caps.tools, caps.resources, caps.prompts, caps.logging);
                    snapshot.tools = conn.client->cached_tools();
                    snapshot.resources = conn.client->cached_resources();
                    snapshot.prompts = conn.client->cached_prompts();
                }
            }
            snapshots.push_back(std::move(snapshot));
        }
        return snapshots;
    }
    
    // Call tool from any connected server
    McpResult<ToolCallResult> call_tool(const std::string& server_name, const ToolCallRequest& request) {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        
        auto it = connections_.find(server_name);
        if (it == connections_.end()) {
            return std::unexpected(McpClientError::NotConnected);
        }
        
        if (it->second.status != ConnectionStatus::Connected || !it->second.client) {
            return std::unexpected(McpClientError::NotConnected);
        }
        
        return it->second.client->call_tool(request);
    }

    McpResult<ListResourcesResult> list_resources(const std::string& server_name) {
        std::lock_guard<std::mutex> lock(connections_mutex_);

        auto it = connections_.find(server_name);
        if (it == connections_.end() || it->second.status != ConnectionStatus::Connected || !it->second.client) {
            return std::unexpected(McpClientError::NotConnected);
        }
        return it->second.client->list_resources();
    }

    McpResult<ResourceReadResult> read_resource(const std::string& server_name, std::string_view uri) {
        std::lock_guard<std::mutex> lock(connections_mutex_);

        auto it = connections_.find(server_name);
        if (it == connections_.end() || it->second.status != ConnectionStatus::Connected || !it->second.client) {
            return std::unexpected(McpClientError::NotConnected);
        }
        return it->second.client->read_resource(uri);
    }
    
    // Tool with qualified name (server::tool
    McpResult<ToolCallResult> call_qualified_tool(const std::string& qualified_name, const std::string& arguments_json) {
        // Parse qualified name
        auto pos = qualified_name.find("::");
        if (pos == std::string::npos) {
            return std::unexpected(McpClientError::ToolNotFound);
        }
        
        std::string server_name = qualified_name.substr(0, pos);
        std::string tool_name = qualified_name.substr(pos + 2);
        
        ToolCallRequest request;
        request.name = tool_name;
        request.arguments_json = arguments_json;
        
        return call_tool(server_name, request);
    }
    
    // Event callbacks
    void set_server_connected_callback(ServerConnectedCallback callback) {
        server_connected_callback_ = std::move(callback);
    }
    
    void set_server_disconnected_callback(ServerDisconnectedCallback callback) {
        server_disconnected_callback_ = std::move(callback);
    }
    
    void set_server_error_callback(ServerErrorCallback callback) {
        server_error_callback_ = std::move(callback);
    }
    
    void set_tools_updated_callback(ToolsUpdatedCallback callback) {
        tools_updated_callback_ = std::move(callback);
    }
    
private:
    // Connect to a server internally
    McpResult<void> connect_server_internal(const std::string& server_name, const ServerConfig& server_config) {
        // Create client config
        McpClient::Config client_config;
        client_config.name = server_name;
        client_config.transport_type = server_config.transport;
        client_config.request_timeout = config_.connection_timeout;
        client_config.client_info.name = "claude-code";
        client_config.client_info.version = "1.0.0";
        client_config.capabilities.roots = true;
        
        // Create client
        auto client = std::make_unique<McpClient>(std::move(client_config));
        
        // Set up roots handler
        client->set_roots_handler([this]() {
            return get_default_roots();
        });

        client->set_notification_callback([this, server_name](const JsonRpcNotification& notification) {
            handle_server_notification(server_name, notification);
        });
        
        // Connect based on transport type
        McpResult<void> connect_result;
        auto current_epoch_seconds = [] {
            return std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        };
        auto mark_auth_failure = [&](std::string message) -> McpResult<void> {
            auto it = connections_.find(server_name);
            if (it != connections_.end()) {
                it->second.status = ConnectionStatus::NeedsAuth;
                it->second.last_error = std::move(message);
                it->second.retry_count++;
                if (server_error_callback_) {
                    server_error_callback_(server_name, *it->second.last_error);
                }
            }
            return std::unexpected(McpClientError::Unauthorized);
        };
        auto remote_headers = [&]() -> std::expected<HeaderMap, std::string> {
            auto headers = get_mcp_server_headers(
                server_name,
                server_config.headers,
                server_config.url,
                server_config.headers_helper
            );
            McpServerConfig auth_config;
            switch (server_config.transport) {
                case TransportType::Sse:
                    auth_config.type = "sse";
                    break;
                case TransportType::Http:
                case TransportType::StreamableHttp:
                    auth_config.type = "http";
                    break;
                case TransportType::Stdio:
                default:
                    auth_config.type = "stdio";
                    break;
            }
            auth_config.url = server_config.url;
            for (const auto& [key, value] : server_config.headers) {
                auth_config.headers[key] = value;
            }
            auth_config.oauth = server_config.oauth;

            if (has_mcp_discovery_but_no_token(server_name, auth_config)) {
                return std::unexpected(std::string{
                    "MCP OAuth authentication required: no stored token"});
            }

            auto token = load_server_tokens_from_local_storage(server_name, auth_config);
            if (!token || token->access_token.empty()) {
                return headers;
            }

            const auto expires_at = token->expires_at;
            const bool is_expired_or_expiring =
                expires_at > 0 && expires_at <= current_epoch_seconds() + 300;
            if (!is_expired_or_expiring) {
                headers["Authorization"] = "Bearer " + token->access_token;
                return headers;
            }

            if (token->refresh_token.empty()) {
                return std::unexpected(std::string{
                    "OAuth token refresh failed: stored token has no refresh_token"});
            }
            auto refreshed = refresh_server_tokens_from_local_storage(server_name, auth_config);
            if (!refreshed) {
                return std::unexpected("OAuth token refresh failed: " + refreshed.error().message());
            }
            if (refreshed->access_token.empty()) {
                return std::unexpected(std::string{"OAuth token refresh failed: empty access_token"});
            }
            headers["Authorization"] = "Bearer " + refreshed->access_token;
            return headers;
        };
        
        switch (server_config.transport) {
            case TransportType::Stdio:
                connect_result = client->connect_stdio(
                    server_config.command, 
                    server_config.args, 
                    server_config.env
                );
                break;
                
            case TransportType::Sse:
                if (auto headers = remote_headers()) {
                    connect_result = client->connect_sse(
                        server_config.url,
                        *headers
                    );
                } else {
                    return mark_auth_failure(headers.error());
                }
                break;

            case TransportType::Http:
            case TransportType::StreamableHttp:
                if (auto headers = remote_headers()) {
                    connect_result = client->connect_streamable_http(
                        server_config.url,
                        *headers
                    );
                } else {
                    return mark_auth_failure(headers.error());
                }
                break;
                
            default:
                return std::unexpected(McpClientError::ConnectionFailed);
        }
        
        if (!connect_result) {
            // Update connection status
            auto it = connections_.find(server_name);
            if (it != connections_.end()) {
                it->second.status = connect_result.error() == McpClientError::Unauthorized
                    ? ConnectionStatus::NeedsAuth
                    : ConnectionStatus::Error;
                it->second.last_error = error_to_string(connect_result.error());
                it->second.retry_count++;
                
                if (server_error_callback_) {
                    server_error_callback_(server_name, it->second.last_error.value());
                }
            }
            return connect_result;
        }
        
        // Store client and update status
        auto it = connections_.find(server_name);
        if (it != connections_.end()) {
            it->second.client = std::move(client);
            it->second.status = ConnectionStatus::Connected;
            it->second.retry_count = 0;
            
            // Load tools, resources, prompts
            refresh_server_state(server_name, *it->second.client);
            
            // Notify connection
            if (server_connected_callback_) {
                ConnectedMcpServer info;
                info.name = server_name;
                info.status = ConnectionStatus::Connected;
                const auto& server_info = it->second.client->server_info();
                info.server_info = server_info.name + "@" + server_info.version;
                const auto& capabilities = it->second.client->server_capabilities();
                info.capabilities = std::format("tools={},resources={},prompts={},logging={}",
                    capabilities.tools, capabilities.resources, capabilities.prompts, capabilities.logging);
                for (const auto& tool : it->second.client->cached_tools()) {
                    info.tools.push_back(tool.name);
                }
                for (const auto& resource : it->second.client->cached_resources()) {
                    info.resources.push_back(resource.uri);
                }
                for (const auto& prompt : it->second.client->cached_prompts()) {
                    info.prompts.push_back(prompt.name);
                }
                info.connected_at = std::chrono::steady_clock::now();
                
                server_connected_callback_(server_name);
            }
        }
        
        return {};
    }
    
    // Connect all auto-start servers
    void connect_all_auto_servers() {
        for (const auto& [name, config] : mcp_config_.servers) {
            if (config.auto_start && config.enabled) {
                // Connect in background (simplified)
                connect_server(name);
            }
        }
    }
    
    // Refresh server state after connection
    void refresh_server_state(const std::string& server_name, McpClient& client) {
        // Load tools
        if (client.server_capabilities().tools) {
            auto tools_result = client.list_tools();
            if (tools_result) {
                if (tools_updated_callback_) {
                    std::vector<std::string> tool_names;
                    tool_names.reserve(tools_result->tools.size());
                    for (const auto& tool : tools_result->tools) {
                        tool_names.push_back(tool.name);
                    }
                    tools_updated_callback_(server_name, tool_names);
                }
            }
        }
        
        // Load resources
        if (client.server_capabilities().resources) {
            (void)client.list_resources();
        }
        
        // Load prompts
        if (client.server_capabilities().prompts) {
            (void)client.list_prompts();
        }
    }

    enum class ListChangedKind {
        Tools,
        Resources,
        Prompts,
    };

    void handle_server_notification(const std::string& server_name, const JsonRpcNotification& notification) {
        if (notification.method == "notifications/tools/list_changed") {
            start_notification_refresh(server_name, ListChangedKind::Tools);
        } else if (notification.method == "notifications/resources/list_changed") {
            start_notification_refresh(server_name, ListChangedKind::Resources);
        } else if (notification.method == "notifications/prompts/list_changed") {
            start_notification_refresh(server_name, ListChangedKind::Prompts);
        } else if (notification.method == "at_mentioned") {
            // IDE at-mention: forward the raw params to whichever UI responder
            // has registered (see cc.services.mcp.at_mention_handler). This is
            // the JSON-RPC inbound dispatch point that useIdeAtMentioned.ts
            // hooks via client.setNotificationHandler on the TS side.
            dispatch_at_mention(server_name, notification.params_json);
        } else if (notification.method == "notifications/claude/channel") {
            // TS REF: src/services/mcp/channelNotification.ts:37-47
            // Channel server pushed an inbound message (e.g. user typed in
            // Slack). Parse params, wrap in <channel> tag, emit to the
            // channel notification bus so subscribers (query engine, UI)
            // can enqueue the message.
            auto params = parse_channel_message_params(
                notification.params_json.value_or("{}")
            );
            if (params) {
                emit_channel_message(server_name, params->content, params->meta);
            }
        } else if (notification.method == "notifications/claude/channel/permission") {
            // TS REF: src/services/mcp/channelNotification.ts:62-72
            // Channel server sent a structured permission reply (the human
            // approved/denied a tool call via the channel). Parse and emit
            // to the bus — subscribers match request_id against pending
            // permission maps.
            auto params = parse_channel_permission_params(
                notification.params_json.value_or("{}")
            );
            if (params) {
                emit_channel_permission(
                    server_name, params->request_id, params->behavior
                );
            }
        }
    }

    void start_notification_refresh(std::string server_name, ListChangedKind kind) {
        if (shutting_down_.load()) return;
        std::lock_guard lock(notification_workers_mutex_);
        if (shutting_down_.load()) return;
        notification_workers_.emplace_back([this, server_name = std::move(server_name), kind] {
            refresh_after_list_changed(server_name, kind);
        });
    }

    void refresh_after_list_changed(const std::string& server_name, ListChangedKind kind) {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(server_name);
        if (it == connections_.end() || it->second.status != ConnectionStatus::Connected || !it->second.client) {
            return;
        }

        auto& client = *it->second.client;
        const auto& caps = client.server_capabilities();
        switch (kind) {
            case ListChangedKind::Tools: {
                if (!caps.tools || !caps.tools_list_changed) return;
                auto tools_result = client.list_tools();
                if (tools_result && tools_updated_callback_) {
                    std::vector<std::string> tool_names;
                    tool_names.reserve(tools_result->tools.size());
                    for (const auto& tool : tools_result->tools) {
                        tool_names.push_back(tool.name);
                    }
                    tools_updated_callback_(server_name, tool_names);
                }
                break;
            }
            case ListChangedKind::Resources:
                if (!caps.resources || !caps.resources_list_changed) return;
                (void)client.list_resources();
                break;
            case ListChangedKind::Prompts:
                if (!caps.prompts || !caps.prompts_list_changed) return;
                (void)client.list_prompts();
                break;
        }
    }

    void join_notification_workers() {
        std::vector<std::thread> workers;
        {
            std::lock_guard lock(notification_workers_mutex_);
            workers.swap(notification_workers_);
        }
        for (auto& worker : workers) {
            if (worker.joinable()) worker.join();
        }
    }
    
    // Get default roots for this system
    std::vector<Root> get_default_roots() {
        std::vector<Root> roots;
        
        // Add current working directory
        try {
            auto cwd = std::filesystem::current_path();
            Root root;
            root.uri = "file://" + cwd.string();
            root.name = "Current Directory";
            roots.push_back(root);
        } catch (...) {
            // Ignore
        }
        
        return roots;
    }
    
    ConnectionManagerConfig config_;
    McpConfig mcp_config_;
    
    std::map<std::string, ServerConnection> connections_;
    std::mutex connections_mutex_;
    std::atomic<bool> shutting_down_{false};
    std::mutex notification_workers_mutex_;
    std::vector<std::thread> notification_workers_;
    
    ServerConnectedCallback server_connected_callback_;
    ServerDisconnectedCallback server_disconnected_callback_;
    ServerErrorCallback server_error_callback_;
    ToolsUpdatedCallback tools_updated_callback_;
};

} // namespace cc::services::mcp
