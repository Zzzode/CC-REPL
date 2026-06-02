/// @file mcp_server.cppm
/// @brief MCP Server module - exposes CLI tools to external clients via
/// JSON-RPC 2.0 over stdio or HTTP SSE transport (using libuv).
module;

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <optional>
#include <expected>
#include <chrono>
#include <format>
#include <ranges>
#include <algorithm>
#include <deque>
#include <mutex>

#include <uv.h>

export module cc.services.mcp.mcp_server;

import cc.types.types;
import cc.services.mcp.types;

export namespace cc::core {

using namespace cc::services::mcp;

// ============================================================
// Server transport concept
// ============================================================

/// Concept constraining valid server transport implementations
template <typename T>
concept ServerTransport = requires(T t, std::string_view msg) {
    { t.start() } -> std::same_as<VoidResult>;
    { t.stop() } -> std::same_as<void>;
    { t.send(msg) } -> std::same_as<VoidResult>;
    { t.receive() } -> std::same_as<Result<std::string>>;
    { t.is_running() } -> std::convertible_to<bool>;
};

// ============================================================
// StdioServerTransport - reads JSON-RPC from stdin, writes to stdout
// ============================================================

/// Stdio-based server transport: reads newline-delimited JSON-RPC from stdin,
/// writes responses to stdout. Suitable for single-client IPC.
class StdioServerTransport {
public:
    StdioServerTransport() = default;

    /// Start listening on stdin (non-blocking via libuv)
    VoidResult start() {
        if (running_) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError, "Transport already running"));
        }
        // Initialize libuv stdin pipe
        uv_loop_ = uv_default_loop();
        uv_pipe_init(uv_loop_, &stdin_pipe_, 0);
        uv_pipe_open(&stdin_pipe_, 0); // fd 0 = stdin
        stdin_pipe_.data = this;
        running_ = true;
        return {};
    }

    /// Stop the transport and close handles
    void stop() {
        if (!running_) return;
        running_ = false;
        uv_close(reinterpret_cast<uv_handle_t*>(&stdin_pipe_), nullptr);
    }

    /// Send a JSON-RPC response to stdout
    VoidResult send(std::string_view message) {
        if (!running_) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError, "Transport not running"));
        }
        // Write message + newline to stdout
        auto output = std::string(message) + "\n";
        auto written = fwrite(output.c_str(), 1, output.size(), stdout);
        fflush(stdout);
        if (written != output.size()) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError, "Failed to write to stdout"));
        }
        return {};
    }

    /// Receive a line from the internal buffer (populated by libuv read callbacks)
    Result<std::string> receive() {
        std::lock_guard lock(buffer_mutex_);
        if (message_queue_.empty()) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError, "No messages available"));
        }
        auto msg = std::move(message_queue_.front());
        message_queue_.pop_front();
        return msg;
    }

    [[nodiscard]] bool is_running() const noexcept { return running_; }

    /// Push a received line into the message queue (called from libuv callback)
    void enqueue_message(std::string message) {
        std::lock_guard lock(buffer_mutex_);
        message_queue_.push_back(std::move(message));
    }

private:
    uv_loop_t* uv_loop_ = nullptr;
    uv_pipe_t stdin_pipe_{};
    bool running_ = false;
    std::mutex buffer_mutex_;
    std::deque<std::string> message_queue_;
};

// ============================================================
// SseServerTransport - HTTP SSE server mode using libuv TCP
// ============================================================

/// HTTP Server-Sent Events transport: accepts TCP connections and serves
/// JSON-RPC over SSE. Supports multiple concurrent clients.
class SseServerTransport {
public:
    struct Config {
        std::string host = "127.0.0.1";
        uint16_t port = 8080;
        std::string path = "/mcp";           // SSE endpoint path
        std::string message_path = "/message"; // POST endpoint for client messages
    };

    explicit SseServerTransport(Config config) : config_(std::move(config)) {}

    /// Start the HTTP SSE server
    VoidResult start() {
        if (running_) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError, "SSE transport already running"));
        }
        uv_loop_ = uv_default_loop();
        uv_tcp_init(uv_loop_, &server_);
        server_.data = this;

        struct sockaddr_in addr{};
        uv_ip4_addr(config_.host.c_str(), config_.port, &addr);

        int r = uv_tcp_bind(&server_, reinterpret_cast<const sockaddr*>(&addr), 0);
        if (r != 0) {
            return std::unexpected(Error::make(
                ErrorCode::ConnectionFailed,
                std::format("Failed to bind: {}", uv_strerror(r))));
        }

        r = uv_listen(reinterpret_cast<uv_stream_t*>(&server_), 128, on_new_connection);
        if (r != 0) {
            return std::unexpected(Error::make(
                ErrorCode::ConnectionFailed,
                std::format("Failed to listen: {}", uv_strerror(r))));
        }
        running_ = true;
        return {};
    }

    /// Stop the SSE server and close all client connections
    void stop() {
        if (!running_) return;
        running_ = false;
        uv_close(reinterpret_cast<uv_handle_t*>(&server_), nullptr);
        clients_.clear();
    }

    /// Broadcast an SSE event to all connected clients
    VoidResult send(std::string_view message) {
        if (!running_) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError, "SSE transport not running"));
        }
        // Format as SSE event: "data: <json>\n\n"
        auto sse_frame = std::format("data: {}\n\n", message);
        for (auto& client : clients_) {
            // Write to each connected client stream
            uv_buf_t buf = uv_buf_init(
                const_cast<char*>(sse_frame.c_str()), sse_frame.size());
            uv_write_t* req = new uv_write_t;
            uv_write(req, reinterpret_cast<uv_stream_t*>(&client),
                     &buf, 1, on_write_complete);
        }
        return {};
    }

    /// Receive a message from any client
    Result<std::string> receive() {
        std::lock_guard lock(queue_mutex_);
        if (incoming_queue_.empty()) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError, "No incoming messages"));
        }
        auto msg = std::move(incoming_queue_.front());
        incoming_queue_.pop_front();
        return msg;
    }

    [[nodiscard]] bool is_running() const noexcept { return running_; }
    [[nodiscard]] uint16_t port() const noexcept { return config_.port; }
    [[nodiscard]] size_t client_count() const noexcept { return clients_.size(); }

private:
    /// libuv callback for new client connections
    static void on_new_connection(uv_stream_t* server, int status) {
        if (status < 0) return;
        auto* self = static_cast<SseServerTransport*>(server->data);
        self->clients_.emplace_back();
        auto& client = self->clients_.back();
        uv_tcp_init(self->uv_loop_, &client);
        uv_accept(server, reinterpret_cast<uv_stream_t*>(&client));
    }

    /// libuv callback after write completes
    static void on_write_complete(uv_write_t* req, int /*status*/) {
        delete req;
    }

    Config config_;
    uv_loop_t* uv_loop_ = nullptr;
    uv_tcp_t server_{};
    bool running_ = false;
    std::vector<uv_tcp_t> clients_;
    std::mutex queue_mutex_;
    std::deque<std::string> incoming_queue_;
};

// ============================================================
// TransportConfig - variant selecting which transport to use
// ============================================================

/// Configuration variant: choose stdio or SSE transport at startup
struct TransportConfig {
    enum class Type { Stdio, Sse };
    Type type = Type::Stdio;
    SseServerTransport::Config sse_config; // Only used if type == Sse
};

// ============================================================
// McpServer - the main server orchestrator
// ============================================================

/// MCP Server: registers tools/resources/prompts and handles incoming
/// JSON-RPC requests, dispatching to the appropriate handler.
class McpServer {
public:
    struct ServerConfig {
        std::string name = "claude-code";
        std::string version = "1.0.0";
        ServerCapabilities capabilities;
    };

    explicit McpServer(ServerConfig config) : config_(std::move(config)) {}

    /// Start listening using the specified transport
    VoidResult start(const TransportConfig& transport_config) {
        if (running_) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError, "Server already running"));
        }
        transport_type_ = transport_config.type;

        if (transport_config.type == TransportConfig::Type::Stdio) {
            stdio_transport_ = std::make_unique<StdioServerTransport>();
            auto r = stdio_transport_->start();
            if (!r) return r;
        } else {
            sse_transport_ = std::make_unique<SseServerTransport>(transport_config.sse_config);
            auto r = sse_transport_->start();
            if (!r) return r;
        }
        running_ = true;
        return {};
    }

    /// Graceful shutdown
    void stop() {
        if (!running_) return;
        running_ = false;
        if (stdio_transport_) stdio_transport_->stop();
        if (sse_transport_) sse_transport_->stop();
    }

    /// Register a tool to be exposed via MCP
    void register_tool(McpTool tool) {
        tools_[tool.name] = std::move(tool);
        config_.capabilities.tools = true;
    }

    /// Register a resource to serve
    void register_resource(McpResource resource) {
        resources_[resource.uri] = std::move(resource);
        config_.capabilities.resources = true;
    }

    /// Register a prompt template
    void register_prompt(McpPrompt prompt) {
        prompts_[prompt.name] = std::move(prompt);
        config_.capabilities.prompts = true;
    }

    /// Set the tool invocation handler callback
    using ToolHandler = std::function<ToolCallResult(const ToolCallRequest&)>;
    void set_tool_handler(ToolHandler handler) { tool_handler_ = std::move(handler); }

    /// Set the resource read handler callback
    using ResourceHandler = std::function<ResourceReadResult(std::string_view uri)>;
    void set_resource_handler(ResourceHandler handler) { resource_handler_ = std::move(handler); }

    /// Process one incoming JSON-RPC request and return the response
    Result<std::string> handle_request(std::string_view raw_json) {
        auto method = extract_method(raw_json);
        auto id = extract_id(raw_json);

        // Initialization handshake
        if (method == "initialize") {
            return handle_initialize(id);
        }
        if (method == "notifications/initialized") {
            initialized_ = true;
            return std::string{}; // Notifications don't get responses
        }
        // Tool operations
        if (method == "tools/list") return handle_tools_list(id);
        if (method == "tools/call") return handle_tools_call(id, raw_json);
        // Resource operations
        if (method == "resources/list") return handle_resources_list(id);
        if (method == "resources/read") return handle_resources_read(id, raw_json);
        // Prompt operations
        if (method == "prompts/list") return handle_prompts_list(id);
        if (method == "prompts/get") return handle_prompts_get(id, raw_json);

        // Method not found
        return make_error_response(id, JsonRpcErrorCode::MethodNotFound,
                                   std::format("Method not found: {}", method));
    }

    [[nodiscard]] bool is_running() const noexcept { return running_; }
    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }
    [[nodiscard]] size_t tool_count() const noexcept { return tools_.size(); }
    [[nodiscard]] size_t resource_count() const noexcept { return resources_.size(); }
    [[nodiscard]] size_t prompt_count() const noexcept { return prompts_.size(); }

private:
    // --- Request handlers ---

    Result<std::string> handle_initialize(std::string_view id) {
        auto result = std::format(
            R"({{"jsonrpc":"2.0","id":{},"result":{{)"
            R"("protocolVersion":"2024-11-05",)"
            R"("capabilities":{{"tools":{},"resources":{},"prompts":{}}},)"
            R"("serverInfo":{{"name":"{}","version":"{}"}})"
            R"(}}}})",
            id, config_.capabilities.tools ? "true" : "false",
            config_.capabilities.resources ? "true" : "false",
            config_.capabilities.prompts ? "true" : "false",
            config_.name, config_.version);
        return result;
    }

    Result<std::string> handle_tools_list(std::string_view id) {
        std::string tools_json = "[";
        bool first = true;
        for (const auto& [name, tool] : tools_) {
            if (!first) tools_json += ",";
            tools_json += std::format(
                R"({{"name":"{}","description":"{}","inputSchema":{}}})",
                tool.name, tool.description, tool.input_schema_json);
            first = false;
        }
        tools_json += "]";
        return std::format(
            R"({{"jsonrpc":"2.0","id":{},"result":{{"tools":{}}}}})", id, tools_json);
    }

    Result<std::string> handle_tools_call(std::string_view id, std::string_view raw_json) {
        auto tool_name = extract_nested_field(raw_json, "name");
        auto args_json = extract_nested_field(raw_json, "arguments");

        if (!tools_.contains(tool_name)) {
            return make_error_response(id, JsonRpcErrorCode::InvalidParams,
                                       std::format("Tool not found: {}", tool_name));
        }
        if (!tool_handler_) {
            return make_error_response(id, JsonRpcErrorCode::InternalError,
                                       "No tool handler registered");
        }
        ToolCallRequest req{.name = tool_name, .arguments_json = args_json};
        auto result = tool_handler_(req);

        std::string content_json = "[";
        for (size_t i = 0; i < result.content.size(); ++i) {
            if (i > 0) content_json += ",";
            content_json += std::format(
                R"({{"type":"{}","text":"{}"}})",
                result.content[i].type, result.content[i].text);
        }
        content_json += "]";

        return std::format(
            R"({{"jsonrpc":"2.0","id":{},"result":{{"content":{},"isError":{}}}}})",
            id, content_json, result.is_error ? "true" : "false");
    }

    Result<std::string> handle_resources_list(std::string_view id) {
        std::string res_json = "[";
        bool first = true;
        for (const auto& [uri, res] : resources_) {
            if (!first) res_json += ",";
            res_json += std::format(
                R"({{"uri":"{}","name":"{}","description":"{}","mimeType":"{}"}})",
                res.uri, res.name, res.description, res.mime_type);
            first = false;
        }
        res_json += "]";
        return std::format(
            R"({{"jsonrpc":"2.0","id":{},"result":{{"resources":{}}}}})", id, res_json);
    }

    Result<std::string> handle_resources_read(std::string_view id, std::string_view raw_json) {
        auto uri = extract_nested_field(raw_json, "uri");
        if (!resource_handler_) {
            return make_error_response(id, JsonRpcErrorCode::InternalError,
                                       "No resource handler registered");
        }
        auto result = resource_handler_(uri);
        std::string contents_json = "[";
        for (size_t i = 0; i < result.contents.size(); ++i) {
            if (i > 0) contents_json += ",";
            contents_json += std::format(
                R"({{"uri":"{}","mimeType":"{}","text":"{}"}})",
                result.contents[i].uri, result.contents[i].mime_type, result.contents[i].text);
        }
        contents_json += "]";
        return std::format(
            R"({{"jsonrpc":"2.0","id":{},"result":{{"contents":{}}}}})", id, contents_json);
    }

    Result<std::string> handle_prompts_list(std::string_view id) {
        std::string prompts_json = "[";
        bool first = true;
        for (const auto& [name, prompt] : prompts_) {
            if (!first) prompts_json += ",";
            prompts_json += std::format(
                R"({{"name":"{}","description":"{}"}})", prompt.name, prompt.description);
            first = false;
        }
        prompts_json += "]";
        return std::format(
            R"({{"jsonrpc":"2.0","id":{},"result":{{"prompts":{}}}}})", id, prompts_json);
    }

    Result<std::string> handle_prompts_get(std::string_view id, std::string_view raw_json) {
        auto name = extract_nested_field(raw_json, "name");
        auto it = prompts_.find(name);
        if (it == prompts_.end()) {
            return make_error_response(id, JsonRpcErrorCode::InvalidParams,
                                       std::format("Prompt not found: {}", name));
        }
        return std::format(
            R"({{"jsonrpc":"2.0","id":{},"result":{{"description":"{}","messages":[]}}}})",
            id, it->second.description);
    }

    // --- Helper utilities ---

    static std::string make_error_response(std::string_view id, JsonRpcErrorCode code,
                                           std::string_view message) {
        return std::format(
            R"({{"jsonrpc":"2.0","id":{},"error":{{"code":{},"message":"{}"}}}})",
            id, static_cast<int>(code), message);
    }

    static std::string extract_method(std::string_view json) {
        auto pos = json.find("\"method\":\"");
        if (pos == std::string_view::npos) return "";
        pos += 10;
        auto end = json.find('"', pos);
        return std::string(json.substr(pos, end - pos));
    }

    static std::string extract_id(std::string_view json) {
        auto pos = json.find("\"id\":");
        if (pos == std::string_view::npos) return "null";
        pos += 5;
        auto end = json.find_first_of(",}", pos);
        return std::string(json.substr(pos, end - pos));
    }

    static std::string extract_nested_field(std::string_view json, std::string_view key) {
        auto pattern = std::format("\"{}\":\"", key);
        auto pos = json.find(pattern);
        if (pos == std::string_view::npos) return "";
        pos += pattern.size();
        auto end = json.find('"', pos);
        return (end != std::string_view::npos) ? std::string(json.substr(pos, end - pos)) : "";
    }

    ServerConfig config_;
    TransportConfig::Type transport_type_ = TransportConfig::Type::Stdio;
    bool running_ = false;
    bool initialized_ = false;

    std::unique_ptr<StdioServerTransport> stdio_transport_;
    std::unique_ptr<SseServerTransport> sse_transport_;

    std::unordered_map<std::string, McpTool> tools_;
    std::unordered_map<std::string, McpResource> resources_;
    std::unordered_map<std::string, McpPrompt> prompts_;

    ToolHandler tool_handler_;
    ResourceHandler resource_handler_;
};

} // namespace cc::core
