// MCP Client - Model Context Protocol client with JSON-RPC 2.0 transport.
//
// This is the declaration-only module interface. All non-trivial bodies live
// in module implementation units (client_*_transport.cpp / client_protocol.cpp
// / client_requests.cpp, all `module cc.services.mcp.client;`) so that the
// heavy textual third-party closures used by the implementations
// (<yyjson.h> via cc.utils.json, <httplib.h> via cc.utils.http, and the raw
// socket POSIX headers) never enter this interface's BMI. A body edit
// recompiles one object instead of the importer fan-out.
module;

#include <sys/types.h> // pid_t member in StdioTransport layout
#include <cstddef>
#include <cstdint>

export module cc.services.mcp.client;

import std;

import cc.services.mcp.types;

export namespace cc::services::mcp {

// Callback types
using RequestCallback = std::function<void(const std::string& response, std::optional<McpClientError> error)>;
using NotificationCallback = std::function<void(const JsonRpcNotification& notification)>;
using RootsHandler = std::function<std::vector<Root>()>;

// =========================================================================
// Transport Interface
// =========================================================================

class IMcpTransport {
public:
    virtual ~IMcpTransport() = default;

    [[nodiscard]] virtual McpResult<void> start() = 0;
    [[nodiscard]] virtual McpResult<void> send(std::string_view message) = 0;
    [[nodiscard]] virtual McpResult<std::string> receive() = 0;
    [[nodiscard]] virtual bool is_connected() const = 0;
    virtual void close() = 0;
};

// =========================================================================
// Stdio Transport
// =========================================================================

class StdioTransport : public IMcpTransport {
public:
    StdioTransport(std::string command, std::vector<std::string> args,
                   std::map<std::string, std::string> env = {});

    // Out-of-line destructor (defined in client_stdio_transport.cpp) so the
    // destructor body is emitted once as a strong symbol instead of weakly in
    // every importer. (Under clang named modules the polymorphic-class
    // vtable/typeinfo itself is owned by this interface unit.)
    ~StdioTransport() override;

    [[nodiscard]] McpResult<void> start() override;
    [[nodiscard]] McpResult<void> send(std::string_view message) override;
    [[nodiscard]] McpResult<std::string> receive() override;

    [[nodiscard]] bool is_connected() const override {
        return connected_ && read_fd_ >= 0 && write_fd_ >= 0;
    }

    void close() override;

private:
    std::string command_;
    std::vector<std::string> args_;
    std::map<std::string, std::string> env_;
    int read_fd_ = -1;
    int write_fd_ = -1;
    pid_t child_pid_ = -1;
    bool connected_ = false;
};

// =========================================================================
// SSE Transport — Full implementation with reconnect
// =========================================================================

class SseTransport : public IMcpTransport {
public:
    struct ReconnectPolicy {
        std::chrono::milliseconds initial_delay;
        std::chrono::milliseconds max_delay;
        double backoff_multiplier;
        double jitter_factor;
        uint32_t max_retries;
        std::chrono::seconds liveness_timeout;
    };

    static ReconnectPolicy default_policy();

    explicit SseTransport(std::string url, std::map<std::string, std::string> headers = {},
                          ReconnectPolicy policy = default_policy());

    // Out-of-line destructor (client_sse_transport.cpp); see StdioTransport.
    ~SseTransport() override;

    [[nodiscard]] McpResult<void> start() override;
    [[nodiscard]] McpResult<void> send(std::string_view message) override;
    [[nodiscard]] McpResult<std::string> receive() override;

    [[nodiscard]] bool is_connected() const override {
        return connected_.load();
    }

    void close() override;

    [[nodiscard]] std::string get_post_url() const;

private:
    // URL components
    struct UrlParts { bool https; std::string host; uint16_t port; std::string path; };

    static std::optional<UrlParts> parse_url_parts(std::string_view sv);
    bool parse_url(const std::string& url);
    [[nodiscard]] std::optional<UrlParts> resolve_post_endpoint(std::string_view endpoint) const;
    [[nodiscard]] std::optional<UrlParts> wait_for_post_target();
    static bool send_all(int fd, std::string_view data);
    [[nodiscard]] McpResult<void> send_post_request(const UrlParts& target, const std::string& body);
    [[nodiscard]] McpResult<void> read_http_success_headers(int fd);
    int tcp_connect(const UrlParts& parts);
    void connection_loop(std::stop_token stop);
    int tcp_connect();
    bool send_sse_request(int fd);
    [[nodiscard]] McpResult<void> read_headers(int fd);
    void stream_events(int fd, std::stop_token& stop);
    static void parse_field(const std::string& line, std::string& event,
                            std::string& data, std::string& id);
    void sleep_with_backoff(std::stop_token& stop, std::chrono::milliseconds delay);

    std::string url_;
    std::string post_url_;  // Discovered POST endpoint from SSE "endpoint" event
    UrlParts parts_{};
    std::map<std::string, std::string> headers_;
    ReconnectPolicy policy_;
    std::string last_event_id_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> should_run_{false};
    std::atomic<bool> unauthorized_{false};
    std::atomic<int> socket_fd_{-1};
    std::jthread reader_thread_;

    std::mutex send_mutex_;
    mutable std::mutex post_mutex_;
    std::condition_variable post_cv_;

    mutable std::mutex recv_mutex_;
    std::condition_variable recv_cv_;
    std::deque<std::string> receive_queue_;
};

// =========================================================================
// Streamable HTTP Transport
// =========================================================================

class StreamableHttpTransport : public IMcpTransport {
public:
    explicit StreamableHttpTransport(std::string url, std::map<std::string, std::string> headers = {});

    // Out-of-line destructor (client_http_transport.cpp); see StdioTransport.
    ~StreamableHttpTransport() override;

    [[nodiscard]] McpResult<void> start() override;
    [[nodiscard]] McpResult<void> send(std::string_view message) override;
    [[nodiscard]] McpResult<std::string> receive() override;

    [[nodiscard]] bool is_connected() const override {
        return connected_.load();
    }

    void close() override;

private:
    struct UrlParts {
        bool https = false;
        std::string host;
        uint16_t port = 0;
        std::string path;
    };

    struct HttpResponse {
        int status_code = 0;
        std::map<std::string, std::string> headers;
        std::string body;
    };

    static std::optional<UrlParts> parse_url_parts(std::string_view sv);
    static std::string lower_ascii(std::string value);
    static std::string trim(std::string_view value);
    static bool send_all(int fd, std::string_view data);
    [[nodiscard]] McpResult<HttpResponse> send_post_request(const std::string& body);
    [[nodiscard]] McpResult<HttpResponse> send_post_request_with_http_client(const std::string& body);
    [[nodiscard]] static std::optional<HttpResponse> read_http_response(int fd);
    int tcp_connect(const UrlParts& parts);
    void enqueue_response(const HttpResponse& response);
    void enqueue_sse_body(std::string_view body);
    static void parse_sse_field(const std::string& line, std::string& event,
                                std::string& data, std::string& id);
    void enqueue_message(std::string message);

    std::string url_;
    UrlParts parts_{};
    std::map<std::string, std::string> headers_;
    std::atomic<bool> connected_{false};
    std::mutex send_mutex_;
    mutable std::mutex recv_mutex_;
    std::condition_variable recv_cv_;
    std::deque<std::string> receive_queue_;
};

// =========================================================================
// Pending Request
// =========================================================================

struct PendingRequest {
    RequestId id;
    std::chrono::steady_clock::time_point sent_at;
    std::chrono::milliseconds timeout;
    RequestCallback callback;
    bool completed = false;
};

// =========================================================================
// MCP Client
// =========================================================================

class McpClient {
public:
    struct Config {
        std::string name;
        TransportType transport_type = TransportType::Stdio;
        std::chrono::milliseconds request_timeout{30000};
        std::chrono::milliseconds init_timeout{60000};
        ClientInfo client_info;
        ClientCapabilities capabilities;
    };

    explicit McpClient(Config config);
    ~McpClient();

    // Connect to server using stdio transport
    [[nodiscard]] McpResult<void> connect_stdio(
        std::string command, std::vector<std::string> args,
        std::map<std::string, std::string> env = {});

    // Connect to server using SSE transport
    [[nodiscard]] McpResult<void> connect_sse(
        std::string url, std::map<std::string, std::string> headers = {});

    // Connect to server using streamable HTTP transport
    [[nodiscard]] McpResult<void> connect_streamable_http(
        std::string url, std::map<std::string, std::string> headers = {});

    // Set roots handler for listRoots requests
    void set_roots_handler(RootsHandler handler) {
        roots_handler_ = std::move(handler);
    }

    // Set notification callback
    void set_notification_callback(NotificationCallback callback) {
        notification_callback_ = std::move(callback);
    }

    // List available tools from the server
    [[nodiscard]] McpResult<ListToolsResult> list_tools();

    // Call a tool on the server
    [[nodiscard]] McpResult<ToolCallResult> call_tool(const ToolCallRequest& request);

    // List available resources
    [[nodiscard]] McpResult<ListResourcesResult> list_resources();

    // Read a specific resource
    [[nodiscard]] McpResult<ResourceReadResult> read_resource(std::string_view uri);

    // List available prompts
    [[nodiscard]] McpResult<ListPromptsResult> list_prompts();

    // Get a specific prompt with arguments
    [[nodiscard]] McpResult<PromptGetResult> get_prompt(
        std::string_view name,
        const std::map<std::string, std::string>& arguments = {});

    // Graceful shutdown
    void shutdown();

    // State accessors
    [[nodiscard]] ServerState state() const { return state_; }
    [[nodiscard]] bool is_ready() const { return state_ == ServerState::Ready; }
    [[nodiscard]] const ServerCapabilities& server_capabilities() const { return server_caps_; }
    [[nodiscard]] const ServerInfo& server_info() const { return server_info_; }
    [[nodiscard]] const std::vector<McpTool>& cached_tools() const { return cached_tools_; }
    [[nodiscard]] const std::vector<McpResource>& cached_resources() const { return cached_resources_; }
    [[nodiscard]] const std::vector<McpPrompt>& cached_prompts() const { return cached_prompts_; }

private:
    // Initialize the MCP connection (handshake)
    [[nodiscard]] McpResult<void> initialize();

    // Send a JSON-RPC request and wait for response (synchronous)
    [[nodiscard]] McpResult<std::string> send_request_sync(
        std::string_view method, std::optional<std::string> params);

    // Send a JSON-RPC request asynchronously
    void send_request_async(
        std::string_view method,
        std::optional<std::string> params,
        RequestCallback callback);

    // Send a JSON-RPC notification (no response expected)
    void send_notification(std::string_view method, std::optional<std::string> params);

    // Transport layer abstraction
    [[nodiscard]] McpResult<void> transport_send(std::string_view message);

    void send_error_response(const RequestId& id, JsonRpcErrorCode code, std::string_view message);

    // Receive loop (runs in background thread)
    void receive_loop();

    // Handle incoming message
    void handle_incoming_message(const std::string& message);

    // Handle roots/list request from server
    void handle_roots_list_request(const RequestId& id);

    // Check for timed-out pending requests
    void check_pending_timeouts();

    Config config_;
    ServerState state_;
    int64_t next_request_id_;
    ServerCapabilities server_caps_;
    ServerInfo server_info_;
    std::vector<McpTool> cached_tools_;
    std::vector<McpResource> cached_resources_;
    std::vector<McpPrompt> cached_prompts_;

    std::unique_ptr<IMcpTransport> transport_;

    std::mutex pending_mutex_;
    std::map<RequestId, PendingRequest> pending_requests_;

    std::atomic<bool> running_;
    std::thread receive_thread_;

    RootsHandler roots_handler_;
    NotificationCallback notification_callback_;
};

} // namespace cc::services::mcp
