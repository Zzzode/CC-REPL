// Implementation unit for cc.services.mcp.client — SseTransport bodies.
// Out-of-line so the raw-socket POSIX GMF below stays out of the interface
// BMI. All bodies are cold-path reconnect/SSE parsing.
module;

#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

module cc.services.mcp.client;

import std;

import cc.services.mcp.types;

namespace cc::services::mcp {

SseTransport::ReconnectPolicy SseTransport::default_policy() {
    return ReconnectPolicy{
        .initial_delay = std::chrono::milliseconds{1000},
        .max_delay = std::chrono::milliseconds{30000},
        .backoff_multiplier = 2.0,
        .jitter_factor = 0.25,
        .max_retries = 60,
        .liveness_timeout = std::chrono::seconds{45},
    };
}

SseTransport::SseTransport(std::string url, std::map<std::string, std::string> headers,
                           ReconnectPolicy policy)
    : url_(std::move(url))
    , headers_(std::move(headers))
    , policy_(policy) {}

SseTransport::~SseTransport() {
    close();
}

McpResult<void> SseTransport::start() {
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
    using namespace std::chrono_literals;
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

McpResult<void> SseTransport::send(std::string_view message) {
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

McpResult<std::string> SseTransport::receive() {
    using namespace std::chrono_literals;
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

void SseTransport::close() {
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

std::string SseTransport::get_post_url() const {
    std::lock_guard lock(post_mutex_);
    return post_url_;
}

std::optional<SseTransport::UrlParts>
SseTransport::parse_url_parts(std::string_view sv) {
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

bool SseTransport::parse_url(const std::string& url) {
    auto parsed = parse_url_parts(url);
    if (!parsed) return false;
    parts_ = std::move(*parsed);
    return true;
}

std::optional<SseTransport::UrlParts>
SseTransport::resolve_post_endpoint(std::string_view endpoint) const {
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
        const auto base = slash == std::string_view::npos ? std::string{"/"} : parts_.path.substr(0, slash + 1);
        target.path = base + std::string(endpoint);
    }
    return target;
}

std::optional<SseTransport::UrlParts>
SseTransport::wait_for_post_target() {
    using namespace std::chrono_literals;
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

bool SseTransport::send_all(int fd, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        auto n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

McpResult<void> SseTransport::send_post_request(const UrlParts& target, const std::string& body) {
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

McpResult<void> SseTransport::read_http_success_headers(int fd) {
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

int SseTransport::tcp_connect(const UrlParts& parts) {
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

void SseTransport::connection_loop(std::stop_token stop) {
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

int SseTransport::tcp_connect() {
    if (parts_.https) return -1;
    return tcp_connect(parts_);
}

bool SseTransport::send_sse_request(int fd) {
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

McpResult<void> SseTransport::read_headers(int fd) {
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

void SseTransport::stream_events(int fd, std::stop_token& stop) {
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

void SseTransport::parse_field(const std::string& line, std::string& event,
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

void SseTransport::sleep_with_backoff(std::stop_token& stop, std::chrono::milliseconds delay) {
    using namespace std::chrono_literals;
    auto end = std::chrono::steady_clock::now() + delay;
    while (std::chrono::steady_clock::now() < end) {
        if (stop.stop_requested() || !should_run_.load()) return;
        std::this_thread::sleep_for(50ms);
    }
}

} // namespace cc::services::mcp
