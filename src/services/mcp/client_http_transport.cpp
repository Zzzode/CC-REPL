// Implementation unit for cc.services.mcp.client — StreamableHttpTransport
// bodies. This is the ONLY implementation unit that imports cc.utils.http,
// so the textual <httplib.h> closure never enters the module interface BMI
// nor the other transport units.
module;

#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

module cc.services.mcp.client;

import std;

import cc.services.mcp.types;
import cc.utils.http;

namespace cc::services::mcp {

StreamableHttpTransport::StreamableHttpTransport(
    std::string url, std::map<std::string, std::string> headers)
    : url_(std::move(url))
    , headers_(std::move(headers)) {}

StreamableHttpTransport::~StreamableHttpTransport() {
    close();
}

McpResult<void> StreamableHttpTransport::start() {
    if (connected_.load()) return {};
    auto parsed = parse_url_parts(url_);
    if (!parsed) {
        return std::unexpected(McpClientError::ConnectionFailed);
    }
    parts_ = std::move(*parsed);
    connected_.store(true);
    return {};
}

McpResult<void> StreamableHttpTransport::send(std::string_view message) {
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

McpResult<std::string> StreamableHttpTransport::receive() {
    using namespace std::chrono_literals;
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

void StreamableHttpTransport::close() {
    connected_.store(false);
    recv_cv_.notify_all();
}

std::optional<StreamableHttpTransport::UrlParts>
StreamableHttpTransport::parse_url_parts(std::string_view sv) {
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

std::string StreamableHttpTransport::lower_ascii(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string StreamableHttpTransport::trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

bool StreamableHttpTransport::send_all(int fd, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        auto n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

McpResult<StreamableHttpTransport::HttpResponse>
StreamableHttpTransport::send_post_request(const std::string& body) {
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

McpResult<StreamableHttpTransport::HttpResponse>
StreamableHttpTransport::send_post_request_with_http_client(const std::string& body) {
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

std::optional<StreamableHttpTransport::HttpResponse>
StreamableHttpTransport::read_http_response(int fd) {
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

int StreamableHttpTransport::tcp_connect(const UrlParts& parts) {
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

void StreamableHttpTransport::enqueue_response(const HttpResponse& response) {
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

void StreamableHttpTransport::enqueue_sse_body(std::string_view body) {
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

void StreamableHttpTransport::parse_sse_field(const std::string& line, std::string& event,
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

void StreamableHttpTransport::enqueue_message(std::string message) {
    {
        std::lock_guard lock(recv_mutex_);
        receive_queue_.push_back(std::move(message));
    }
    recv_cv_.notify_one();
}

} // namespace cc::services::mcp
