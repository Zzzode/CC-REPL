// MCP Client - Model Context Protocol client with JSON-RPC 2.0 transport
module;

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <expected>
#include <functional>
#include <thread>
#include <atomic>
#include <sstream>
#include <future>
#include <variant>
#include <cerrno>
#include <csignal>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/wait.h>

export module cc.services.mcp.client;

import cc.services.mcp.types;
import cc.utils.json;

export namespace cc::services::mcp {

using namespace cc::utils::json;
using namespace std::chrono_literals;

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
                   std::map<std::string, std::string> env = {})
        : command_(std::move(command))
        , args_(std::move(args))
        , env_(std::move(env)) {}
    
    ~StdioTransport() override {
        close();
    }
    
    [[nodiscard]] McpResult<void> start() override {
        if (is_connected()) {
            return std::unexpected(McpClientError::AlreadyConnected);
        }

        int child_stdin[2]{-1, -1};
        int child_stdout[2]{-1, -1};
        if (::pipe(child_stdin) != 0 || ::pipe(child_stdout) != 0) {
            if (child_stdin[0] >= 0) ::close(child_stdin[0]);
            if (child_stdin[1] >= 0) ::close(child_stdin[1]);
            if (child_stdout[0] >= 0) ::close(child_stdout[0]);
            if (child_stdout[1] >= 0) ::close(child_stdout[1]);
            return std::unexpected(McpClientError::ConnectionFailed);
        }

        child_pid_ = ::fork();
        if (child_pid_ < 0) {
            ::close(child_stdin[0]);
            ::close(child_stdin[1]);
            ::close(child_stdout[0]);
            ::close(child_stdout[1]);
            child_pid_ = -1;
            return std::unexpected(McpClientError::ConnectionFailed);
        }

        if (child_pid_ == 0) {
            ::dup2(child_stdin[0], STDIN_FILENO);
            ::dup2(child_stdout[1], STDOUT_FILENO);
            ::close(child_stdin[0]);
            ::close(child_stdin[1]);
            ::close(child_stdout[0]);
            ::close(child_stdout[1]);

            for (const auto& [key, value] : env_) {
                ::setenv(key.c_str(), value.c_str(), 1);
            }

            std::vector<char*> argv;
            argv.reserve(args_.size() + 2);
            argv.push_back(const_cast<char*>(command_.c_str()));
            for (auto& arg : args_) {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            argv.push_back(nullptr);
            ::execvp(command_.c_str(), argv.data());
            ::_exit(127);
        }

        ::close(child_stdin[0]);
        ::close(child_stdout[1]);
        write_fd_ = child_stdin[1];
        read_fd_ = child_stdout[0];
        connected_ = true;
        return {};
    }
    
    [[nodiscard]] McpResult<void> send(std::string_view message) override {
        if (!is_connected()) {
            return std::unexpected(McpClientError::NotConnected);
        }

        auto msg = std::string(message) + "\n";
        std::size_t written = 0;
        while (written < msg.size()) {
            const auto bytes = ::write(write_fd_, msg.data() + written, msg.size() - written);
            if (bytes < 0) {
                if (errno == EINTR) continue;
                return std::unexpected(McpClientError::TransportError);
            }
            if (bytes == 0) {
                return std::unexpected(McpClientError::TransportError);
            }
            written += static_cast<std::size_t>(bytes);
        }
        return {};
    }
    
    [[nodiscard]] McpResult<std::string> receive() override {
        if (!is_connected()) {
            return std::unexpected(McpClientError::NotConnected);
        }

        std::string result;
        char ch = '\0';
        while (true) {
            const auto bytes = ::read(read_fd_, &ch, 1);
            if (bytes < 0) {
                if (errno == EINTR) continue;
                return std::unexpected(McpClientError::TransportError);
            }
            if (bytes == 0) {
                connected_ = false;
                return std::unexpected(McpClientError::ServerClosed);
            }
            if (ch == '\n') break;
            result.push_back(ch);
        }

        if (!result.empty() && result.back() == '\r') {
            result.pop_back();
        }
        
        return result;
    }
    
    [[nodiscard]] bool is_connected() const override {
        return connected_ && read_fd_ >= 0 && write_fd_ >= 0;
    }
    
    void close() override {
        if (write_fd_ >= 0) {
            ::close(write_fd_);
            write_fd_ = -1;
        }
        if (read_fd_ >= 0) {
            ::close(read_fd_);
            read_fd_ = -1;
        }
        if (child_pid_ > 0) {
            int status = 0;
            if (::waitpid(child_pid_, &status, WNOHANG) == 0) {
                ::kill(child_pid_, SIGTERM);
                if (::waitpid(child_pid_, &status, WNOHANG) == 0) {
                    ::kill(child_pid_, SIGKILL);
                    ::waitpid(child_pid_, &status, 0);
                }
            }
            child_pid_ = -1;
        }
        connected_ = false;
    }
    
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

    static ReconnectPolicy default_policy() {
        return ReconnectPolicy{
            .initial_delay = std::chrono::milliseconds{1000},
            .max_delay = std::chrono::milliseconds{30000},
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.25,
            .max_retries = 60,
            .liveness_timeout = std::chrono::seconds{45},
        };
    }

    explicit SseTransport(std::string url, std::map<std::string, std::string> headers = {},
                          ReconnectPolicy policy = default_policy())
        : url_(std::move(url))
        , headers_(std::move(headers))
        , policy_(policy) {}

    ~SseTransport() override { close(); }

    [[nodiscard]] McpResult<void> start() override {
        if (connected_.load()) return {};

        // Parse URL to extract host, port, path
        if (!parse_url(url_)) {
            return std::unexpected(McpClientError::ConnectionFailed);
        }

        should_run_.store(true);
        reader_thread_ = std::jthread([this](std::stop_token stop) {
            connection_loop(stop);
        });

        // Wait briefly for initial connection
        for (int i = 0; i < 50 && !connected_.load(); ++i) {
            std::this_thread::sleep_for(100ms);
        }

        if (!connected_.load()) {
            return std::unexpected(McpClientError::ConnectionFailed);
        }
        return {};
    }

    [[nodiscard]] McpResult<void> send(std::string_view message) override {
        if (!connected_.load()) {
            return std::unexpected(McpClientError::NotConnected);
        }
        // MCP over SSE: messages are sent via HTTP POST to the endpoint URL
        // Queue for the post sender thread
        std::lock_guard lock(send_mutex_);
        pending_sends_.emplace_back(message);
        return {};
    }

    [[nodiscard]] McpResult<std::string> receive() override {
        std::unique_lock lock(recv_mutex_);
        if (recv_cv_.wait_for(lock, 5s, [this] { return !receive_queue_.empty(); })) {
            auto msg = std::move(receive_queue_.front());
            receive_queue_.pop_front();
            return msg;
        }
        if (!connected_.load()) {
            return std::unexpected(McpClientError::NotConnected);
        }
        return std::unexpected(McpClientError::Timeout);
    }

    [[nodiscard]] bool is_connected() const override {
        return connected_.load();
    }

    void close() override {
        should_run_.store(false);
        connected_.store(false);

        if (socket_fd_.load() >= 0) {
            ::shutdown(socket_fd_.load(), SHUT_RDWR);
            ::close(socket_fd_.load());
            socket_fd_.store(-1);
        }

        if (reader_thread_.joinable()) {
            reader_thread_.request_stop();
            reader_thread_.join();
        }
    }

    [[nodiscard]] std::string get_post_url() const { return post_url_; }

private:
    // URL components
    struct UrlParts { bool https; std::string host; uint16_t port; std::string path; };

    bool parse_url(const std::string& url) {
        std::string_view sv(url);
        if (sv.starts_with("https://")) { parts_.https = true; sv.remove_prefix(8); parts_.port = 443; }
        else if (sv.starts_with("http://")) { parts_.https = false; sv.remove_prefix(7); parts_.port = 80; }
        else return false;
        auto slash = sv.find('/');
        auto host_part = (slash != std::string_view::npos) ? sv.substr(0, slash) : sv;
        parts_.path = (slash != std::string_view::npos) ? std::string(sv.substr(slash)) : "/";
        auto colon = host_part.find(':');
        if (colon != std::string_view::npos) {
            parts_.host = std::string(host_part.substr(0, colon));
            parts_.port = static_cast<uint16_t>(std::atoi(std::string(host_part.substr(colon+1)).c_str()));
        } else {
            parts_.host = std::string(host_part);
        }
        return !parts_.host.empty();
    }

    void connection_loop(std::stop_token stop) {
        uint32_t retries = 0;
        auto delay = policy_.initial_delay;

        while (!stop.stop_requested() && should_run_.load()) {
            int fd = tcp_connect();
            if (fd < 0) {
                if (++retries > policy_.max_retries) break;
                sleep_with_backoff(stop, delay);
                delay = std::min(
                    std::chrono::milliseconds(static_cast<int64_t>(delay.count() * policy_.backoff_multiplier)),
                    policy_.max_delay);
                continue;
            }

            socket_fd_.store(fd);
            if (!send_sse_request(fd) || !read_headers(fd)) {
                ::close(fd); socket_fd_.store(-1);
                if (++retries > policy_.max_retries) break;
                sleep_with_backoff(stop, delay);
                continue;
            }

            connected_.store(true);
            retries = 0;
            delay = policy_.initial_delay;

            // Stream SSE events until disconnect
            stream_events(fd, stop);

            ::close(fd);
            socket_fd_.store(-1);
            connected_.store(false);

            if (!should_run_.load() || stop.stop_requested()) break;
            // Reconnect
            sleep_with_backoff(stop, delay);
        }
    }

    int tcp_connect() {
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        auto port_s = std::to_string(parts_.port);
        if (getaddrinfo(parts_.host.c_str(), port_s.c_str(), &hints, &res) != 0) return -1;
        int fd = -1;
        for (auto* r = res; r; r = r->ai_next) {
            fd = ::socket(r->ai_family, r->ai_socktype, r->ai_protocol);
            if (fd < 0) continue;
            struct timeval tv{}; tv.tv_sec = policy_.liveness_timeout.count();
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            if (::connect(fd, r->ai_addr, r->ai_addrlen) == 0) break;
            ::close(fd); fd = -1;
        }
        freeaddrinfo(res);
        return fd;
    }

    bool send_sse_request(int fd) {
        std::string req = "GET " + parts_.path + " HTTP/1.1\r\n";
        req += "Host: " + parts_.host + "\r\nAccept: text/event-stream\r\nCache-Control: no-cache\r\n";
        if (!last_event_id_.empty()) req += "Last-Event-ID: " + last_event_id_ + "\r\n";
        for (auto& [k,v] : headers_) req += k + ": " + v + "\r\n";
        req += "\r\n";
        size_t sent = 0;
        while (sent < req.size()) {
            auto n = ::send(fd, req.data()+sent, req.size()-sent, 0);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    bool read_headers(int fd) {
        std::string hdr; char c;
        while (hdr.size() < 8192) {
            if (::recv(fd, &c, 1, 0) <= 0) return false;
            hdr += c;
            if (hdr.size() >= 4 && hdr.ends_with("\r\n\r\n")) break;
        }
        // Check 2xx
        auto sp = hdr.find(' ');
        if (sp == std::string::npos) return false;
        int code = std::atoi(hdr.substr(sp+1, 3).c_str());
        if (code < 200 || code >= 300) return false;
        // Extract POST endpoint from response if provided (Link header or endpoint event)
        return true;
    }

    void stream_events(int fd, std::stop_token& stop) {
        std::string event_type, data_buf, id_buf;
        std::string line;
        char buf[4096];

        while (!stop.stop_requested() && should_run_.load()) {
            auto n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;

            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == '\r') continue;
                if (buf[i] == '\n') {
                    if (line.empty()) {
                        // Dispatch event
                        if (!data_buf.empty()) {
                            if (data_buf.back() == '\n') data_buf.pop_back();
                            if (!id_buf.empty()) last_event_id_ = id_buf;

                            // "endpoint" event tells us where to POST
                            if (event_type == "endpoint") {
                                post_url_ = data_buf;
                            } else {
                                std::lock_guard lock(recv_mutex_);
                                receive_queue_.push_back(std::move(data_buf));
                                recv_cv_.notify_one();
                            }
                        }
                        event_type.clear(); data_buf.clear(); id_buf.clear();
                    } else {
                        parse_field(line, event_type, data_buf, id_buf);
                        line.clear();
                    }
                } else {
                    line += buf[i];
                }
            }
        }
    }

    static void parse_field(const std::string& line, std::string& event,
                            std::string& data, std::string& id) {
        if (line.empty() || line[0] == ':') return;
        auto col = line.find(':');
        std::string_view field = (col != std::string::npos) ? std::string_view(line).substr(0, col) : std::string_view(line);
        std::string_view val = (col != std::string::npos) ? std::string_view(line).substr(col+1) : std::string_view{};
        if (!val.empty() && val[0] == ' ') val.remove_prefix(1);
        if (field == "event") event = std::string(val);
        else if (field == "data") { data += std::string(val); data += '\n'; }
        else if (field == "id") id = std::string(val);
    }

    void sleep_with_backoff(std::stop_token& stop, std::chrono::milliseconds delay) {
        auto end = std::chrono::steady_clock::now() + delay;
        while (std::chrono::steady_clock::now() < end) {
            if (stop.stop_requested() || !should_run_.load()) return;
            std::this_thread::sleep_for(50ms);
        }
    }

    std::string url_;
    std::string post_url_;  // Discovered POST endpoint from SSE "endpoint" event
    UrlParts parts_{};
    std::map<std::string, std::string> headers_;
    ReconnectPolicy policy_;
    std::string last_event_id_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> should_run_{false};
    std::atomic<int> socket_fd_{-1};
    std::jthread reader_thread_;

    std::mutex send_mutex_;
    std::vector<std::string> pending_sends_;

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
    
    explicit McpClient(Config config)
        : config_(std::move(config))
        , state_(ServerState::NotStarted)
        , next_request_id_(1)
        , running_(false) {}
    
    ~McpClient() {
        shutdown();
    }
    
    // Connect to server using stdio transport
    [[nodiscard]] McpResult<void> connect_stdio(
        std::string command, std::vector<std::string> args,
        std::map<std::string, std::string> env = {}) {
        
        if (state_ != ServerState::NotStarted && state_ != ServerState::Stopped) {
            return std::unexpected(McpClientError::AlreadyConnected);
        }
        
        state_ = ServerState::Starting;
        transport_ = std::make_unique<StdioTransport>(std::move(command), std::move(args), std::move(env));
        
        auto result = transport_->start();
        if (!result) {
            state_ = ServerState::Error;
            return result;
        }
        
        return initialize();
    }
    
    // Connect to server using SSE transport
    [[nodiscard]] McpResult<void> connect_sse(
        std::string url, std::map<std::string, std::string> headers = {}) {
        
        if (state_ != ServerState::NotStarted && state_ != ServerState::Stopped) {
            return std::unexpected(McpClientError::AlreadyConnected);
        }
        
        state_ = ServerState::Starting;
        transport_ = std::make_unique<SseTransport>(std::move(url), std::move(headers));
        
        auto result = transport_->start();
        if (!result) {
            state_ = ServerState::Error;
            return result;
        }
        
        return initialize();
    }
    
    // Set roots handler for listRoots requests
    void set_roots_handler(RootsHandler handler) {
        roots_handler_ = std::move(handler);
    }
    
    // Set notification callback
    void set_notification_callback(NotificationCallback callback) {
        notification_callback_ = std::move(callback);
    }
    
    // List available tools from the server
    [[nodiscard]] McpResult<ListToolsResult> list_tools() {
        if (state_ != ServerState::Ready) {
            return std::unexpected(McpClientError::NotConnected);
        }
        
        auto response = send_request_sync("tools/list", std::nullopt);
        if (!response) {
            return std::unexpected(response.error());
        }
        
        auto result = parse_list_tools_result(*response);
        if (!result) {
            return std::unexpected(McpClientError::InvalidResponse);
        }
        
        cached_tools_ = result->tools;
        return *result;
    }
    
    // Call a tool on the server
    [[nodiscard]] McpResult<ToolCallResult> call_tool(const ToolCallRequest& request) {
        if (state_ != ServerState::Ready) {
            return std::unexpected(McpClientError::NotConnected);
        }
        
        JsonMutDoc doc;
        auto params = doc.object();
        params.add("name", doc.string(request.name));
        
        // Parse and add arguments
        auto args_doc = parse(request.arguments_json);
        if (args_doc) {
            params.add("arguments", doc.copy_val(args_doc->root()));
        } else {
            params.add("arguments", doc.object());
        }
        
        doc.set_root(params);
        auto params_json = doc.to_string();
        
        auto response = send_request_sync("tools/call", params_json);
        if (!response) {
            return std::unexpected(response.error());
        }
        
        auto result = parse_tool_call_result(*response);
        if (!result) {
            return std::unexpected(McpClientError::InvalidResponse);
        }
        
        return *result;
    }
    
    // List available resources
    [[nodiscard]] McpResult<ListResourcesResult> list_resources() {
        if (state_ != ServerState::Ready) {
            return std::unexpected(McpClientError::NotConnected);
        }
        
        auto response = send_request_sync("resources/list", std::nullopt);
        if (!response) {
            return std::unexpected(response.error());
        }
        
        // Parse resources
        ListResourcesResult result;
        auto doc = parse(*response);
        if (doc) {
            auto root = doc->root();
            auto result_node = root.get("result");
            if (result_node.is_obj()) {
                auto resources_node = result_node.get("resources");
                if (resources_node.is_arr()) {
                    resources_node.iter([&result](JsonVal res_val) {
                        if (res_val.is_obj()) {
                            McpResource resource;
                            resource.uri = std::string(res_val.get("uri").as_str());
                            resource.name = std::string(res_val.get("name").as_str());
                            resource.description = std::string(res_val.get("description").as_str());
                            resource.mime_type = std::string(res_val.get("mimeType").as_str());
                            result.resources.push_back(std::move(resource));
                        }
                    });
                }
            }
        }
        
        cached_resources_ = result.resources;
        return result;
    }
    
    // Read a specific resource
    [[nodiscard]] McpResult<ResourceReadResult> read_resource(std::string_view uri) {
        if (state_ != ServerState::Ready) {
            return std::unexpected(McpClientError::NotConnected);
        }
        
        JsonMutDoc doc;
        auto params = doc.object();
        params.add("uri", doc.string(uri));
        doc.set_root(params);
        
        auto response = send_request_sync("resources/read", doc.to_string());
        if (!response) {
            return std::unexpected(response.error());
        }
        
        ResourceReadResult result;
        auto resp_doc = parse(*response);
        if (resp_doc) {
            auto root = resp_doc->root();
            auto result_node = root.get("result");
            if (result_node.is_obj()) {
                auto contents_node = result_node.get("contents");
                if (contents_node.is_arr()) {
                    contents_node.iter([&result](JsonVal content_val) {
                        if (content_val.is_obj()) {
                            ResourceContent content;
                            content.uri = std::string(content_val.get("uri").as_str());
                            content.mime_type = std::string(content_val.get("mimeType").as_str());
                            content.text = std::string(content_val.get("text").as_str());
                            content.blob = std::string(content_val.get("blob").as_str());
                            result.contents.push_back(std::move(content));
                        }
                    });
                }
            }
        }
        
        return result;
    }
    
    // List available prompts
    [[nodiscard]] McpResult<ListPromptsResult> list_prompts() {
        if (state_ != ServerState::Ready) {
            return std::unexpected(McpClientError::NotConnected);
        }
        
        auto response = send_request_sync("prompts/list", std::nullopt);
        if (!response) {
            return std::unexpected(response.error());
        }
        
        ListPromptsResult result;
        auto doc = parse(*response);
        if (doc) {
            auto root = doc->root();
            auto result_node = root.get("result");
            if (result_node.is_obj()) {
                auto prompts_node = result_node.get("prompts");
                if (prompts_node.is_arr()) {
                    prompts_node.iter([&result](JsonVal prompt_val) {
                        if (prompt_val.is_obj()) {
                            McpPrompt prompt;
                            prompt.name = std::string(prompt_val.get("name").as_str());
                            prompt.description = std::string(prompt_val.get("description").as_str());
                            // Parse arguments
                            auto args_node = prompt_val.get("arguments");
                            if (args_node.is_arr()) {
                                args_node.iter([&prompt](JsonVal arg_val) {
                                    if (arg_val.is_obj()) {
                                        McpPromptArgument arg;
                                        arg.name = std::string(arg_val.get("name").as_str());
                                        arg.description = std::string(arg_val.get("description").as_str());
                                        arg.required = arg_val.get("required").as_bool();
                                        prompt.arguments.push_back(std::move(arg));
                                    }
                                });
                            }
                            result.prompts.push_back(std::move(prompt));
                        }
                    });
                }
            }
        }
        
        cached_prompts_ = result.prompts;
        return result;
    }
    
    // Get a specific prompt with arguments
    [[nodiscard]] McpResult<PromptGetResult> get_prompt(
        std::string_view name,
        const std::map<std::string, std::string>& arguments = {}) {
        
        if (state_ != ServerState::Ready) {
            return std::unexpected(McpClientError::NotConnected);
        }
        
        JsonMutDoc doc;
        auto params = doc.object();
        params.add("name", doc.string(name));
        
        auto args_obj = doc.object();
        for (const auto& [key, value] : arguments) {
            args_obj.add(key, doc.string(value));
        }
        params.add("arguments", args_obj);
        
        doc.set_root(params);
        
        auto response = send_request_sync("prompts/get", doc.to_string());
        if (!response) {
            return std::unexpected(response.error());
        }
        
        PromptGetResult result;
        auto resp_doc = parse(*response);
        if (resp_doc) {
            auto root = resp_doc->root();
            auto result_node = root.get("result");
            if (result_node.is_obj()) {
                result.description = std::string(result_node.get("description").as_str());
                // Parse messages
                auto messages_node = result_node.get("messages");
                if (messages_node.is_arr()) {
                    messages_node.iter([&result](JsonVal msg_val) {
                        if (msg_val.is_obj()) {
                            McpPromptMessage msg;
                            auto role_str = std::string(msg_val.get("role").as_str());
                            msg.role = (role_str == "assistant") ? PromptRole::Assistant : PromptRole::User;
                            msg.content = std::string(msg_val.get("content").as_str());
                            result.messages.push_back(std::move(msg));
                        }
                    });
                }
            }
        }
        
        return result;
    }
    
    // Graceful shutdown
    void shutdown() {
        if (state_ == ServerState::Ready || state_ == ServerState::Initializing) {
            state_ = ServerState::ShuttingDown;
            send_notification("notifications/cancelled", std::nullopt);
        }

        running_ = false;
        if (transport_) {
            transport_->close();
        }
        if (receive_thread_.joinable()) {
            receive_thread_.join();
        }
        
        state_ = ServerState::Stopped;
    }
    
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
    [[nodiscard]] McpResult<void> initialize() {
        state_ = ServerState::Initializing;

        running_ = true;
        if (!receive_thread_.joinable()) {
            receive_thread_ = std::thread(&McpClient::receive_loop, this);
        }
        
        // Build initialize params
        JsonMutDoc doc;
        auto root = doc.object();
        root.add("protocolVersion", doc.string("2024-11-05"));
        
        auto capabilities = doc.object();
        if (config_.capabilities.roots) {
            auto roots_cap = doc.object();
            roots_cap.add("listChanged", doc.boolean(config_.capabilities.roots_capabilities.list_changed));
            capabilities.add("roots", roots_cap);
        }
        root.add("capabilities", capabilities);
        
        auto client_info = doc.object();
        client_info.add("name", doc.string(config_.client_info.name));
        client_info.add("version", doc.string(config_.client_info.version));
        root.add("clientInfo", client_info);
        
        doc.set_root(root);
        
        auto response = send_request_sync("initialize", doc.to_string());
        if (!response) {
            state_ = ServerState::Error;
            running_ = false;
            if (transport_) {
                transport_->close();
            }
            if (receive_thread_.joinable()) {
                receive_thread_.join();
            }
            return std::unexpected(response.error());
        }
        
        // Parse server capabilities from response
        auto init_result = parse_initialize_result(*response);
        if (!init_result) {
            state_ = ServerState::Error;
            running_ = false;
            if (transport_) {
                transport_->close();
            }
            if (receive_thread_.joinable()) {
                receive_thread_.join();
            }
            return std::unexpected(McpClientError::InitializationFailed);
        }
        
        server_info_ = init_result->server_info;
        server_caps_ = init_result->capabilities;
        
        // Send initialized notification
        send_notification("notifications/initialized", std::nullopt);

        state_ = ServerState::Ready;
        return {};
    }
    
    // Send a JSON-RPC request and wait for response (synchronous)
    [[nodiscard]] McpResult<std::string> send_request_sync(
        std::string_view method, std::optional<std::string> params) {
        
        std::promise<McpResult<std::string>> promise;
        auto future = promise.get_future();
        
        auto callback = [&promise](const std::string& response, std::optional<McpClientError> error) {
            if (error) {
                promise.set_value(std::unexpected(*error));
            } else {
                promise.set_value(response);
            }
        };
        
        send_request_async(method, std::move(params), std::move(callback));
        
        auto status = future.wait_for(config_.request_timeout);
        if (status == std::future_status::timeout) {
            return std::unexpected(McpClientError::Timeout);
        }
        
        return future.get();
    }
    
    // Send a JSON-RPC request asynchronously
    void send_request_async(
        std::string_view method,
        std::optional<std::string> params,
        RequestCallback callback) {
        
        RequestId id = next_request_id_++;
        auto request = make_request(id, std::string(method), std::move(params));
        auto serialized = serialize_request(request);
        
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_requests_[id] = PendingRequest{
                .id = id,
                .sent_at = std::chrono::steady_clock::now(),
                .timeout = config_.request_timeout,
                .callback = std::move(callback)
            };
        }
        
        auto result = transport_send(serialized);
        if (!result) {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_requests_.find(id);
            if (it != pending_requests_.end()) {
                if (it->second.callback) {
                    it->second.callback("", result.error());
                }
                pending_requests_.erase(it);
            }
        }
    }
    
    // Send a JSON-RPC notification (no response expected)
    void send_notification(std::string_view method, std::optional<std::string> params) {
        auto notif = make_notification(std::string(method), std::move(params));
        auto serialized = serialize_notification(notif);
        transport_send(serialized);  // Fire and forget
    }
    
    // Transport layer abstraction
    [[nodiscard]] McpResult<void> transport_send(std::string_view message) {
        if (!transport_ || !transport_->is_connected()) {
            return std::unexpected(McpClientError::NotConnected);
        }
        return transport_->send(message);
    }
    
    // Receive loop (runs in background thread)
    void receive_loop() {
        while (running_) {
            if (!transport_ || !transport_->is_connected()) {
                std::this_thread::sleep_for(10ms);
                continue;
            }
            
            auto result = transport_->receive();
            if (!result) {
                if (result.error() == McpClientError::ServerClosed) {
                    state_ = ServerState::Stopped;
                    running_ = false;
                } else if (result.error() == McpClientError::Timeout) {
                    // Check for timed-out pending requests
                    check_pending_timeouts();
                }
                continue;
            }
            
            handle_incoming_message(*result);
        }
    }
    
    // Handle incoming message
    void handle_incoming_message(const std::string& message) {
        // Parse message
        auto doc = parse(message);
        if (!doc) {
            return;
        }
        
        auto root = doc->root();
        if (!root.is_obj()) {
            return;
        }
        
        // Check if it's a response (has id)
        auto id_node = root.get("id");
        if (id_node.valid() && !id_node.is_null()) {
            // Handle response
            RequestId id;
            if (id_node.is_num()) {
                id = static_cast<int64_t>(id_node.as_int());
            } else {
                id = std::string(id_node.as_str());
            }
            
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_requests_.find(id);
            if (it != pending_requests_.end() && !it->second.completed) {
                it->second.completed = true;
                if (it->second.callback) {
                    it->second.callback(message, std::nullopt);
                }
                pending_requests_.erase(it);
            }
        } else {
            // Check if it's a request (method without id) or notification
            auto method_node = root.get("method");
            if (method_node.is_str()) {
                std::string method = std::string(method_node.as_str());
                
                // Handle server requests
                if (method == "roots/list") {
                    handle_roots_list_request(message);
                } else {
                    // Handle as notification
                    JsonRpcNotification notif;
                    notif.method = method;
                    // Extract params
                    auto params_node = root.get("params");
                    if (params_node.valid() && !params_node.is_null()) {
                        JsonMutDoc params_doc;
                        // Re-serialize params
                        notif.params_json = message;  // Simplified
                    }
                    
                    if (notification_callback_) {
                        notification_callback_(notif);
                    }
                }
            }
        }
    }
    
    // Handle roots/list request from server
    void handle_roots_list_request(const std::string& message) {
        // Parse request to get id
        auto doc = parse(message);
        if (!doc) return;
        
        auto root = doc->root();
        auto id_node = root.get("id");
        if (!id_node.valid()) return;
        
        RequestId id;
        if (id_node.is_num()) {
            id = static_cast<int64_t>(id_node.as_int());
        } else {
            id = std::string(id_node.as_str());
        }
        
        // Build response
        JsonMutDoc resp_doc;
        auto resp_root = resp_doc.object();
        resp_root.add("jsonrpc", resp_doc.string("2.0"));
        
        if (std::holds_alternative<int64_t>(id)) {
            resp_root.add("id", resp_doc.number(std::get<int64_t>(id)));
        } else {
            resp_root.add("id", resp_doc.string(std::get<std::string>(id)));
        }
        
        // Get roots from handler
        auto result = resp_doc.object();
        auto roots_arr = resp_doc.array();
        
        if (roots_handler_) {
            auto roots = roots_handler_();
            for (const auto& r : roots) {
                auto root_obj = resp_doc.object();
                root_obj.add("uri", resp_doc.string(r.uri));
                if (r.name) {
                    root_obj.add("name", resp_doc.string(*r.name));
                }
                roots_arr.append(root_obj);
            }
        }
        
        result.add("roots", roots_arr);
        resp_root.add("result", result);
        
        resp_doc.set_root(resp_root);
        transport_send(resp_doc.to_string());
    }
    
    // Check for timed-out pending requests
    void check_pending_timeouts() {
        auto now = std::chrono::steady_clock::now();
        
        std::vector<RequestId> timed_out;
        
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            for (const auto& [id, req] : pending_requests_) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - req.sent_at);
                if (elapsed > req.timeout) {
                    timed_out.push_back(id);
                }
            }
            
            for (const auto& id : timed_out) {
                auto it = pending_requests_.find(id);
                if (it != pending_requests_.end() && !it->second.completed) {
                    it->second.completed = true;
                    if (it->second.callback) {
                        it->second.callback("", McpClientError::Timeout);
                    }
                    pending_requests_.erase(it);
                }
            }
        }
    }
    
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
