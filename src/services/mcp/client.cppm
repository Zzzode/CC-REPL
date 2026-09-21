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
#include <cctype>
#include <algorithm>
#include <array>
#include <unordered_map>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/wait.h>

export module cc.services.mcp.client;

import cc.services.mcp.types;
import cc.utils.json;
import cc.utils.http;

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

        unauthorized_.store(false);
        should_run_.store(true);
        reader_thread_ = std::jthread([this](std::stop_token stop) {
            connection_loop(stop);
        });

        // Wait briefly for initial connection
        for (int i = 0; i < 50 && !connected_.load() && !unauthorized_.load(); ++i) {
            std::this_thread::sleep_for(100ms);
        }

        if (unauthorized_.load()) {
            return std::unexpected(McpClientError::Unauthorized);
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

        auto target = wait_for_post_target();
        if (!target) {
            return std::unexpected(McpClientError::TransportError);
        }

        std::lock_guard lock(send_mutex_);
        return send_post_request(*target, std::string(message));
    }

    [[nodiscard]] McpResult<std::string> receive() override {
        std::unique_lock lock(recv_mutex_);
        if (recv_cv_.wait_for(lock, 5s, [this] { return !receive_queue_.empty() || !connected_.load(); })) {
            if (receive_queue_.empty()) {
                return std::unexpected(McpClientError::NotConnected);
            }
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
        post_cv_.notify_all();
        recv_cv_.notify_all();

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

    [[nodiscard]] std::string get_post_url() const {
        std::lock_guard lock(post_mutex_);
        return post_url_;
    }

private:
    // URL components
    struct UrlParts { bool https; std::string host; uint16_t port; std::string path; };

    static std::optional<UrlParts> parse_url_parts(std::string_view sv) {
        UrlParts parts{};
        if (sv.starts_with("https://")) {
            parts.https = true;
            sv.remove_prefix(8);
            parts.port = 443;
        } else if (sv.starts_with("http://")) {
            parts.https = false;
            sv.remove_prefix(7);
            parts.port = 80;
        } else {
            return std::nullopt;
        }
        auto slash = sv.find('/');
        auto host_part = (slash != std::string_view::npos) ? sv.substr(0, slash) : sv;
        parts.path = (slash != std::string_view::npos) ? std::string(sv.substr(slash)) : "/";
        auto colon = host_part.find(':');
        if (colon != std::string_view::npos) {
            parts.host = std::string(host_part.substr(0, colon));
            parts.port = static_cast<uint16_t>(std::atoi(std::string(host_part.substr(colon + 1)).c_str()));
        } else {
            parts.host = std::string(host_part);
        }
        if (parts.host.empty() || parts.port == 0) return std::nullopt;
        return parts;
    }

    bool parse_url(const std::string& url) {
        auto parsed = parse_url_parts(url);
        if (!parsed) return false;
        parts_ = std::move(*parsed);
        return true;
    }

    [[nodiscard]] std::optional<UrlParts> resolve_post_endpoint(std::string_view endpoint) const {
        if (endpoint.starts_with("http://") || endpoint.starts_with("https://")) {
            return parse_url_parts(endpoint);
        }

        UrlParts target = parts_;
        if (endpoint.empty()) {
            target.path = parts_.path.empty() ? "/" : parts_.path;
        } else if (endpoint.front() == '/') {
            target.path = std::string(endpoint);
        } else {
            const auto slash = parts_.path.rfind('/');
            const auto base = slash == std::string::npos ? std::string{"/"} : parts_.path.substr(0, slash + 1);
            target.path = base + std::string(endpoint);
        }
        return target;
    }

    [[nodiscard]] std::optional<UrlParts> wait_for_post_target() {
        std::string endpoint;
        {
            std::unique_lock lock(post_mutex_);
            post_cv_.wait_for(lock, 5s, [this] {
                return !post_url_.empty() || !connected_.load() || !should_run_.load();
            });
            endpoint = post_url_;
        }
        return resolve_post_endpoint(endpoint);
    }

    static bool send_all(int fd, std::string_view data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            auto n = ::send(fd, data.data() + sent, data.size() - sent, 0);
            if (n <= 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    [[nodiscard]] McpResult<void> send_post_request(const UrlParts& target, const std::string& body) {
        if (target.https) return std::unexpected(McpClientError::ConnectionFailed);

        int fd = tcp_connect(target);
        if (fd < 0) return std::unexpected(McpClientError::ConnectionFailed);

        std::string host = target.host;
        if ((target.port != 80 && !target.https) || (target.port != 443 && target.https)) {
            host += ":" + std::to_string(target.port);
        }

        std::string req = "POST " + (target.path.empty() ? std::string{"/"} : target.path) + " HTTP/1.1\r\n";
        req += "Host: " + host + "\r\n";
        req += "Content-Type: application/json\r\n";
        req += "Accept: application/json\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        req += "Connection: close\r\n";
        for (auto& [k, v] : headers_) req += k + ": " + v + "\r\n";
        req += "\r\n";
        req += body;

        auto result = send_all(fd, req)
            ? read_http_success_headers(fd)
            : McpResult<void>{std::unexpected(McpClientError::TransportError)};
        ::close(fd);
        return result;
    }

    [[nodiscard]] McpResult<void> read_http_success_headers(int fd) {
        std::string hdr;
        char c;
        while (hdr.size() < 8192) {
            if (::recv(fd, &c, 1, 0) <= 0) return std::unexpected(McpClientError::TransportError);
            hdr += c;
            if (hdr.size() >= 4 && hdr.ends_with("\r\n\r\n")) break;
        }
        auto sp = hdr.find(' ');
        if (sp == std::string::npos || sp + 4 > hdr.size()) return std::unexpected(McpClientError::TransportError);
        int code = std::atoi(hdr.substr(sp + 1, 3).c_str());
        if (code == 401) return std::unexpected(McpClientError::Unauthorized);
        if (code < 200 || code >= 300) return std::unexpected(McpClientError::TransportError);
        return {};
    }

    int tcp_connect(const UrlParts& parts) {
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        auto port_s = std::to_string(parts.port);
        if (getaddrinfo(parts.host.c_str(), port_s.c_str(), &hints, &res) != 0) return -1;
        int fd = -1;
        for (auto* r = res; r; r = r->ai_next) {
            fd = ::socket(r->ai_family, r->ai_socktype, r->ai_protocol);
            if (fd < 0) continue;
            struct timeval tv{};
            tv.tv_sec = policy_.liveness_timeout.count();
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (::connect(fd, r->ai_addr, r->ai_addrlen) == 0) break;
            ::close(fd);
            fd = -1;
        }
        freeaddrinfo(res);
        return fd;
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
            auto header_result = send_sse_request(fd)
                ? read_headers(fd)
                : McpResult<void>{std::unexpected(McpClientError::TransportError)};
            if (!header_result) {
                if (header_result.error() == McpClientError::Unauthorized) {
                    unauthorized_.store(true);
                    ::close(fd);
                    socket_fd_.store(-1);
                    break;
                }
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
        if (parts_.https) return -1;
        return tcp_connect(parts_);
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

    [[nodiscard]] McpResult<void> read_headers(int fd) {
        std::string hdr; char c;
        while (hdr.size() < 8192) {
            if (::recv(fd, &c, 1, 0) <= 0) return std::unexpected(McpClientError::TransportError);
            hdr += c;
            if (hdr.size() >= 4 && hdr.ends_with("\r\n\r\n")) break;
        }
        // Check 2xx
        auto sp = hdr.find(' ');
        if (sp == std::string::npos) return std::unexpected(McpClientError::TransportError);
        int code = std::atoi(hdr.substr(sp+1, 3).c_str());
        if (code == 401) return std::unexpected(McpClientError::Unauthorized);
        if (code < 200 || code >= 300) return std::unexpected(McpClientError::TransportError);
        // Extract POST endpoint from response if provided (Link header or endpoint event)
        return {};
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
                                {
                                    std::lock_guard lock(post_mutex_);
                                    post_url_ = data_buf;
                                }
                                post_cv_.notify_all();
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
    explicit StreamableHttpTransport(std::string url, std::map<std::string, std::string> headers = {})
        : url_(std::move(url))
        , headers_(std::move(headers)) {}

    ~StreamableHttpTransport() override { close(); }

    [[nodiscard]] McpResult<void> start() override {
        if (connected_.load()) return {};
        auto parsed = parse_url_parts(url_);
        if (!parsed) {
            return std::unexpected(McpClientError::ConnectionFailed);
        }
        parts_ = std::move(*parsed);
        connected_.store(true);
        return {};
    }

    [[nodiscard]] McpResult<void> send(std::string_view message) override {
        if (!connected_.load()) {
            return std::unexpected(McpClientError::NotConnected);
        }

        std::lock_guard lock(send_mutex_);
        auto response = send_post_request(std::string(message));
        if (!response) {
            return std::unexpected(response.error());
        }
        enqueue_response(*response);
        return {};
    }

    [[nodiscard]] McpResult<std::string> receive() override {
        std::unique_lock lock(recv_mutex_);
        if (recv_cv_.wait_for(lock, 5s, [this] {
            return !receive_queue_.empty() || !connected_.load();
        })) {
            if (receive_queue_.empty()) {
                return std::unexpected(McpClientError::NotConnected);
            }
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
        connected_.store(false);
        recv_cv_.notify_all();
    }

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

    static std::optional<UrlParts> parse_url_parts(std::string_view sv) {
        UrlParts parts{};
        if (sv.starts_with("https://")) {
            parts.https = true;
            sv.remove_prefix(8);
            parts.port = 443;
        } else if (sv.starts_with("http://")) {
            parts.https = false;
            sv.remove_prefix(7);
            parts.port = 80;
        } else {
            return std::nullopt;
        }

        const auto slash = sv.find('/');
        const auto host_part = (slash != std::string_view::npos) ? sv.substr(0, slash) : sv;
        parts.path = (slash != std::string_view::npos) ? std::string(sv.substr(slash)) : "/";

        const auto colon = host_part.find(':');
        if (colon != std::string_view::npos) {
            parts.host = std::string(host_part.substr(0, colon));
            parts.port = static_cast<uint16_t>(std::atoi(std::string(host_part.substr(colon + 1)).c_str()));
        } else {
            parts.host = std::string(host_part);
        }
        if (parts.host.empty() || parts.port == 0) return std::nullopt;
        return parts;
    }

    static std::string lower_ascii(std::string value) {
        std::ranges::transform(value, value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    static std::string trim(std::string_view value) {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
            value.remove_prefix(1);
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.remove_suffix(1);
        }
        return std::string(value);
    }

    static bool send_all(int fd, std::string_view data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            auto n = ::send(fd, data.data() + sent, data.size() - sent, 0);
            if (n <= 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    [[nodiscard]] McpResult<HttpResponse> send_post_request(const std::string& body) {
        if (parts_.https) return send_post_request_with_http_client(body);

        const int fd = tcp_connect(parts_);
        if (fd < 0) return std::unexpected(McpClientError::ConnectionFailed);

        std::string host = parts_.host;
        if (parts_.port != 80) {
            host += ":" + std::to_string(parts_.port);
        }

        std::string req = "POST " + (parts_.path.empty() ? std::string{"/"} : parts_.path) + " HTTP/1.1\r\n";
        req += "Host: " + host + "\r\n";
        req += "Content-Type: application/json\r\n";
        req += "Accept: application/json, text/event-stream\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        req += "Connection: close\r\n";
        for (const auto& [key, value] : headers_) {
            req += key + ": " + value + "\r\n";
        }
        req += "\r\n";
        req += body;

        auto response = send_all(fd, req) ? read_http_response(fd) : std::optional<HttpResponse>{};
        ::close(fd);
        if (!response) {
            return std::unexpected(McpClientError::TransportError);
        }
        if (response->status_code == 401) {
            return std::unexpected(McpClientError::Unauthorized);
        }
        if (response->status_code < 200 || response->status_code >= 300) {
            return std::unexpected(McpClientError::TransportError);
        }
        return *response;
    }

    [[nodiscard]] McpResult<HttpResponse> send_post_request_with_http_client(const std::string& body) {
        std::unordered_map<std::string, std::string> request_headers;
        request_headers["Content-Type"] = "application/json";
        request_headers["Accept"] = "application/json, text/event-stream";
        for (const auto& [key, value] : headers_) {
            request_headers[key] = value;
        }

        cc::utils::HttpConfig config;
        config.max_retries = 0;
        cc::utils::HttpClient client(std::move(config));
        auto response = client.post(url_, body, request_headers);
        if (!response) {
            switch (response.error().code) {
                case cc::utils::HttpError::timeout:
                    return std::unexpected(McpClientError::Timeout);
                case cc::utils::HttpError::ssl_error:
                case cc::utils::HttpError::dns_error:
                case cc::utils::HttpError::connection_failed:
                case cc::utils::HttpError::cancelled:
                    return std::unexpected(McpClientError::ConnectionFailed);
            }
            return std::unexpected(McpClientError::ConnectionFailed);
        }

        if (response->status == 401) {
            return std::unexpected(McpClientError::Unauthorized);
        }
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected(McpClientError::TransportError);
        }

        HttpResponse converted;
        converted.status_code = response->status;
        converted.body = std::move(response->body);
        for (const auto& [key, value] : response->headers) {
            converted.headers[lower_ascii(key)] = value;
        }
        return converted;
    }

    [[nodiscard]] static std::optional<HttpResponse> read_http_response(int fd) {
        std::string buffer;
        std::array<char, 4096> chunk{};
        std::size_t header_end = std::string::npos;
        while (buffer.size() < 65536) {
            header_end = buffer.find("\r\n\r\n");
            if (header_end != std::string::npos) break;
            auto n = ::recv(fd, chunk.data(), chunk.size(), 0);
            if (n <= 0) return std::nullopt;
            buffer.append(chunk.data(), static_cast<std::size_t>(n));
        }
        if (header_end == std::string::npos) {
            header_end = buffer.find("\r\n\r\n");
        }
        if (header_end == std::string::npos) return std::nullopt;

        HttpResponse response;
        const auto header_block = buffer.substr(0, header_end);
        response.body = buffer.substr(header_end + 4);

        const auto status_end = header_block.find("\r\n");
        const auto status_line = header_block.substr(0, status_end);
        const auto first_space = status_line.find(' ');
        if (first_space == std::string::npos || first_space + 4 > status_line.size()) {
            return std::nullopt;
        }
        response.status_code = std::atoi(status_line.substr(first_space + 1, 3).c_str());

        std::size_t line_start = status_end == std::string::npos ? header_block.size() : status_end + 2;
        while (line_start < header_block.size()) {
            const auto line_end = header_block.find("\r\n", line_start);
            const auto line = header_block.substr(line_start, line_end == std::string::npos ? std::string::npos : line_end - line_start);
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                response.headers[lower_ascii(line.substr(0, colon))] = trim(std::string_view(line).substr(colon + 1));
            }
            if (line_end == std::string::npos) break;
            line_start = line_end + 2;
        }

        std::size_t content_length = 0;
        if (auto it = response.headers.find("content-length"); it != response.headers.end()) {
            content_length = static_cast<std::size_t>(std::strtoull(it->second.c_str(), nullptr, 10));
        }
        while (content_length > 0 && response.body.size() < content_length) {
            auto n = ::recv(fd, chunk.data(), chunk.size(), 0);
            if (n <= 0) break;
            response.body.append(chunk.data(), static_cast<std::size_t>(n));
        }
        if (content_length > 0 && response.body.size() > content_length) {
            response.body.resize(content_length);
        }
        return response;
    }

    int tcp_connect(const UrlParts& parts) {
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        const auto port = std::to_string(parts.port);
        if (getaddrinfo(parts.host.c_str(), port.c_str(), &hints, &res) != 0) return -1;

        int fd = -1;
        for (auto* r = res; r; r = r->ai_next) {
            fd = ::socket(r->ai_family, r->ai_socktype, r->ai_protocol);
            if (fd < 0) continue;
            struct timeval tv{};
            tv.tv_sec = 5;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (::connect(fd, r->ai_addr, r->ai_addrlen) == 0) break;
            ::close(fd);
            fd = -1;
        }
        freeaddrinfo(res);
        return fd;
    }

    void enqueue_response(const HttpResponse& response) {
        if (trim(response.body).empty()) return;
        const auto content_type = [&]() {
            auto it = response.headers.find("content-type");
            return it == response.headers.end() ? std::string{} : lower_ascii(it->second);
        }();
        if (content_type.contains("text/event-stream")) {
            enqueue_sse_body(response.body);
            return;
        }
        enqueue_message(response.body);
    }

    void enqueue_sse_body(std::string_view body) {
        std::string event_type, data_buf, id_buf, line;
        auto dispatch = [&]() {
            if (data_buf.empty()) return;
            if (data_buf.back() == '\n') data_buf.pop_back();
            if (event_type.empty() || event_type == "message") {
                enqueue_message(data_buf);
            }
        };

        for (char ch : body) {
            if (ch == '\r') continue;
            if (ch == '\n') {
                if (line.empty()) {
                    dispatch();
                    event_type.clear();
                    data_buf.clear();
                    id_buf.clear();
                } else {
                    parse_sse_field(line, event_type, data_buf, id_buf);
                    line.clear();
                }
            } else {
                line.push_back(ch);
            }
        }
        if (!line.empty()) {
            parse_sse_field(line, event_type, data_buf, id_buf);
        }
        dispatch();
    }

    static void parse_sse_field(const std::string& line, std::string& event,
                                std::string& data, std::string& id) {
        if (line.empty() || line[0] == ':') return;
        auto col = line.find(':');
        std::string_view field = (col != std::string::npos) ? std::string_view(line).substr(0, col) : std::string_view(line);
        std::string_view val = (col != std::string::npos) ? std::string_view(line).substr(col + 1) : std::string_view{};
        if (!val.empty() && val[0] == ' ') val.remove_prefix(1);
        if (field == "event") event = std::string(val);
        else if (field == "data") { data += std::string(val); data += '\n'; }
        else if (field == "id") id = std::string(val);
    }

    void enqueue_message(std::string message) {
        {
            std::lock_guard lock(recv_mutex_);
            receive_queue_.push_back(std::move(message));
        }
        recv_cv_.notify_one();
    }

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

    // Connect to server using streamable HTTP transport
    [[nodiscard]] McpResult<void> connect_streamable_http(
        std::string url, std::map<std::string, std::string> headers = {}) {

        if (state_ != ServerState::NotStarted && state_ != ServerState::Stopped) {
            return std::unexpected(McpClientError::AlreadyConnected);
        }

        state_ = ServerState::Starting;
        transport_ = std::make_unique<StreamableHttpTransport>(std::move(url), std::move(headers));

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
                    messages_node.iter([&result, this](JsonVal msg_val) {
                        if (msg_val.is_obj()) {
                            McpPromptMessage msg;
                            auto role_str = std::string(msg_val.get("role").as_str());
                            msg.role = (role_str == "assistant") ? PromptRole::Assistant : PromptRole::User;
                            msg.content = prompt_message_content_to_text(msg_val.get("content"));
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
        (void)transport_send(serialized);
    }
    
    // Transport layer abstraction
    [[nodiscard]] McpResult<void> transport_send(std::string_view message) {
        if (!transport_ || !transport_->is_connected()) {
            return std::unexpected(McpClientError::NotConnected);
        }
        return transport_->send(message);
    }

    [[nodiscard]] std::string prompt_message_content_to_text(JsonVal content) const {
        if (content.is_str()) {
            return std::string(content.as_str());
        }
        if (content.is_arr()) {
            std::string joined;
            content.iter([&](JsonVal item) {
                auto text = prompt_message_content_to_text(item);
                if (text.empty()) return;
                if (!joined.empty()) joined += "\n";
                joined += std::move(text);
            });
            return joined;
        }
        if (!content.is_obj()) return {};

        const auto type = std::string(content.get("type").as_str());
        if (type == "text") {
            return std::string(content.get("text").as_str());
        }
        if (type == "resource") {
            auto resource = content.get("resource");
            if (!resource.is_obj()) return {};
            const auto uri = std::string(resource.get("uri").as_str());
            std::string prefix = std::format("[Resource from {} at {}] ", config_.name, uri);
            if (auto text = resource.get("text"); text.is_str()) {
                return prefix + std::string(text.as_str());
            }
            if (auto blob = resource.get("blob"); blob.is_str()) {
                const auto mime_type = std::string(resource.get("mimeType").as_str());
                const auto mime_label = mime_type.empty() ? std::string{"unknown type"} : mime_type;
                return prefix + std::format(
                    "Binary content ({}, {} base64 characters)",
                    mime_label,
                    blob.as_str().size());
            }
            return prefix;
        }
        if (type == "resource_link") {
            const auto name = std::string(content.get("name").as_str());
            const auto uri = std::string(content.get("uri").as_str());
            std::string text = std::format("[Resource link: {}] {}", name.empty() ? uri : name, uri);
            if (auto description = content.get("description"); description.is_str() && !description.as_str().empty()) {
                text += std::format(" ({})", description.as_str());
            }
            return text;
        }
        if (type == "image" || type == "audio") {
            const auto mime_type = std::string(content.get("mimeType").as_str());
            const auto mime_label = mime_type.empty() ? std::string{"unknown type"} : mime_type;
            const auto data = content.get("data");
            return std::format(
                "[{} from {}] Binary content ({}, {} base64 characters)",
                type == "image" ? "Image" : "Audio",
                config_.name,
                mime_label,
                data.is_str() ? data.as_str().size() : 0);
        }

        return serialize_json_value(content).value_or(std::string{});
    }

    [[nodiscard]] static std::optional<std::string> serialize_json_value(JsonVal value) {
        if (!value.valid() || value.is_null()) return std::nullopt;
        JsonMutDoc doc;
        auto copy = doc.copy_val(value);
        if (!copy.valid()) return std::nullopt;
        doc.set_root(copy);
        return doc.to_string();
    }

    [[nodiscard]] static std::optional<RequestId> parse_request_id(JsonVal id_node) {
        if (!id_node.valid() || id_node.is_null()) return std::nullopt;
        if (id_node.is_num()) return RequestId{static_cast<int64_t>(id_node.as_int())};
        if (id_node.is_str()) return RequestId{std::string(id_node.as_str())};
        return std::nullopt;
    }

    void send_error_response(const RequestId& id, JsonRpcErrorCode code, std::string_view message) {
        JsonMutDoc doc;
        auto root = doc.object();
        root.add("jsonrpc", doc.string("2.0"));
        if (std::holds_alternative<int64_t>(id)) {
            root.add("id", doc.number(std::get<int64_t>(id)));
        } else {
            root.add("id", doc.string(std::get<std::string>(id)));
        }
        auto error = doc.object();
        error.add("code", doc.number(static_cast<int64_t>(code)));
        error.add("message", doc.string(message));
        root.add("error", error);
        doc.set_root(root);
        (void)transport_send(doc.to_string());
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
        
        auto method_node = root.get("method");
        auto id_node = root.get("id");

        if (method_node.is_str()) {
            std::string method = std::string(method_node.as_str());
            auto request_id = parse_request_id(id_node);

            if (request_id) {
                if (method == "roots/list") {
                    handle_roots_list_request(*request_id);
                } else {
                    send_error_response(
                        *request_id,
                        JsonRpcErrorCode::MethodNotFound,
                        "Unsupported server request");
                }
                return;
            }

            JsonRpcNotification notif;
            notif.method = std::move(method);
            notif.params_json = serialize_json_value(root.get("params"));

            if (notification_callback_) {
                notification_callback_(notif);
            }
            return;
        }

        if (auto response_id = parse_request_id(id_node)) {
            RequestId id = std::move(*response_id);
            
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_requests_.find(id);
            if (it != pending_requests_.end() && !it->second.completed) {
                it->second.completed = true;
                if (it->second.callback) {
                    it->second.callback(message, std::nullopt);
                }
                pending_requests_.erase(it);
            }
        }
    }
    
    // Handle roots/list request from server
    void handle_roots_list_request(const RequestId& id) {
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
        (void)transport_send(resp_doc.to_string());
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
