/// @file test_services.cpp
/// @brief Service layer smoke tests aligned with current C++ module APIs.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif

#include <gtest/gtest.h>

import cc.cli.ccr_client;
import cc.config.config;
import cc.services.api.client;
import cc.services.api.errors;
import cc.services.api.streaming;
import cc.services.lsp.LSPServerManager;
import cc.services.mcp.client;
import cc.services.mcp.config;
import cc.services.mcp.connection_manager;
import cc.services.mcp.elicitation_handler;
import cc.services.mcp.headers_helper;
import cc.services.memory.sessionMemory;
import cc.services.mcp.types;
import cc.services.rate_limit;
import cc.services.telemetry;
import cc.services.token_estimation;
import cc.services.voice.voice;
import cc.server.server_routes;
import cc.session.storage;
import cc.query.query_engine;
import cc.remote.remote_session;
import cc.tools.tool;
import cc.types.types;
import cc.utils.error;
import cc.utils.ide_integration;
import cc.utils.json;

namespace fs = std::filesystem;

namespace {

struct CurrentPathGuard {
    fs::path previous;

    explicit CurrentPathGuard(const fs::path& next) : previous(fs::current_path()) {
        fs::current_path(next);
    }

    ~CurrentPathGuard() {
        std::error_code ec;
        fs::current_path(previous, ec);
    }
};

struct EnvironmentGuard {
    std::string name;
    std::optional<std::string> previous;

    EnvironmentGuard(std::string key, const std::string& value) : name(std::move(key)) {
        if (const char* existing = std::getenv(name.c_str())) {
            previous = existing;
        }
        setenv(name.c_str(), value.c_str(), 1);
    }

    ~EnvironmentGuard() {
        if (previous) {
            setenv(name.c_str(), previous->c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
    }
};

bool send_all(int fd, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        auto n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

std::string json_id_literal(cc::utils::json::JsonVal id) {
    if (id.is_num()) return std::to_string(id.as_int());
    if (id.is_str()) return "\"" + std::string(id.as_str()) + "\"";
    return "null";
}

class LocalAnthropicMessagesServer {
public:
    LocalAnthropicMessagesServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return;
        int yes = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        if (::listen(listen_fd_, 4) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }

        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            port_ = ntohs(addr.sin_port);
        }

        running_.store(true);
        accept_thread_ = std::jthread([this](std::stop_token stop) {
            accept_loop(stop);
        });
    }

    ~LocalAnthropicMessagesServer() {
        running_.store(false);
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    [[nodiscard]] std::string base_url() const {
        return std::format("http://127.0.0.1:{}", port_);
    }

    [[nodiscard]] std::optional<std::string> wait_for_body(
        std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this] { return request_body_.has_value(); })) {
            return std::nullopt;
        }
        return request_body_;
    }

    [[nodiscard]] std::optional<std::vector<std::string>> wait_for_bodies(
        std::size_t count,
        std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this, count] { return request_bodies_.size() >= count; })) {
            return std::nullopt;
        }
        return request_bodies_;
    }

private:
    void accept_loop(std::stop_token stop) {
        while (!stop.stop_requested() && running_.load()) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_fd < 0) {
                if (!running_.load()) break;
                continue;
            }
            handle_client(client_fd);
            ::close(client_fd);
        }
    }

    static std::string read_request_body(int fd) {
        std::string request;
        char buffer[4096];
        std::size_t header_end = std::string::npos;
        while ((header_end = request.find("\r\n\r\n")) == std::string::npos) {
            auto n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) return {};
            request.append(buffer, buffer + n);
        }

        auto header = request.substr(0, header_end + 4);
        std::size_t content_length = 0;
        auto length_pos = header.find("Content-Length:");
        if (length_pos == std::string::npos) {
            length_pos = header.find("content-length:");
        }
        if (length_pos != std::string::npos) {
            auto value_start = header.find(':', length_pos);
            auto value_end = header.find("\r\n", value_start);
            if (value_start != std::string::npos && value_end != std::string::npos) {
                content_length = static_cast<std::size_t>(
                    std::stoul(header.substr(value_start + 1, value_end - value_start - 1)));
            }
        }

        const std::size_t body_start = header_end + 4;
        while (request.size() - body_start < content_length) {
            auto n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            request.append(buffer, buffer + n);
        }
        return request.substr(body_start, content_length);
    }

    void handle_client(int fd) {
        auto body = read_request_body(fd);
        {
            std::lock_guard lock(mutex_);
            if (!request_body_) request_body_ = body;
            request_bodies_.push_back(body);
        }
        cv_.notify_all();

        const std::string response_body =
            R"({"id":"msg_test","type":"message","role":"assistant","model":"claude-test","content":[{"type":"text","text":"ok"}],"stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":1}})";
        const auto response = std::format(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            response_body.size(),
            response_body);
        send_all(fd, response);
    }

    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::jthread accept_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<std::string> request_body_;
    std::vector<std::string> request_bodies_;
};

struct LocalCcrHttpRequest {
    std::string method;
    std::string path;
    std::string headers;
    std::string body;
};

class LocalCcrHttpServer {
public:
    LocalCcrHttpServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return;
        int yes = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        if (::listen(listen_fd_, 8) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }

        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            port_ = ntohs(addr.sin_port);
        }

        running_.store(true);
        accept_thread_ = std::jthread([this](std::stop_token stop) {
            accept_loop(stop);
        });
    }

    ~LocalCcrHttpServer() {
        running_.store(false);
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    [[nodiscard]] bool ready() const noexcept { return listen_fd_ >= 0 && port_ != 0; }

    [[nodiscard]] std::string base_url() const {
        return std::format("http://127.0.0.1:{}", port_);
    }

    [[nodiscard]] std::optional<std::vector<LocalCcrHttpRequest>> wait_for_requests(
        std::size_t count,
        std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this, count] { return requests_.size() >= count; })) {
            return std::nullopt;
        }
        return requests_;
    }

private:
    void accept_loop(std::stop_token stop) {
        while (!stop.stop_requested() && running_.load()) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_fd < 0) {
                if (!running_.load()) break;
                continue;
            }
            handle_client(client_fd);
            ::close(client_fd);
        }
    }

    static std::optional<LocalCcrHttpRequest> read_request(int fd) {
        std::string request;
        char buffer[4096];
        std::size_t header_end = std::string::npos;
        while ((header_end = request.find("\r\n\r\n")) == std::string::npos) {
            auto n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) return std::nullopt;
            request.append(buffer, buffer + n);
            if (request.size() > 64 * 1024) return std::nullopt;
        }

        const auto headers = request.substr(0, header_end + 4);
        std::size_t content_length = 0;
        auto length_pos = headers.find("Content-Length:");
        if (length_pos == std::string::npos) {
            length_pos = headers.find("content-length:");
        }
        if (length_pos != std::string::npos) {
            auto value_start = headers.find(':', length_pos);
            auto value_end = headers.find("\r\n", value_start);
            if (value_start != std::string::npos && value_end != std::string::npos) {
                content_length = static_cast<std::size_t>(
                    std::stoul(headers.substr(value_start + 1, value_end - value_start - 1)));
            }
        }

        const std::size_t body_start = header_end + 4;
        while (request.size() - body_start < content_length) {
            auto n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            request.append(buffer, buffer + n);
        }

        auto first_line_end = headers.find("\r\n");
        if (first_line_end == std::string::npos) return std::nullopt;
        std::istringstream first_line(headers.substr(0, first_line_end));
        LocalCcrHttpRequest parsed;
        first_line >> parsed.method >> parsed.path;
        parsed.headers = headers;
        parsed.body = request.substr(body_start, content_length);
        return parsed;
    }

    static bool send_response(int fd, int status, std::string_view reason, std::string_view body) {
        const auto response = std::format(
            "HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            status,
            reason,
            body.size(),
            body);
        return send_all(fd, response);
    }

    static bool send_sse_response(int fd, std::string_view body) {
        const auto response = std::format(
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            body.size(),
            body);
        return send_all(fd, response);
    }

    void handle_client(int fd) {
        auto request = read_request(fd);
        if (!request) return;
        {
            std::lock_guard lock(mutex_);
            requests_.push_back(*request);
        }
        cv_.notify_all();

        if (request->method == "POST" && request->path == "/api/sessions") {
            send_response(fd, 201, "Created", R"({"id":"remote-session-1"})");
            return;
        }
        if (request->method == "POST" && request->path == "/api/sessions/remote-session-1/messages") {
            send_response(fd, 200, "OK", R"({"content":"remote-ok"})");
            return;
        }
        if (request->method == "POST" && request->path == "/api/sessions/remote-session-1/messages?stream=true") {
            send_sse_response(fd,
                "event: message\r\n"
                "data: {\"delta\":\"one\"}\r\n"
                "\r\n"
                "data: {\"delta\":\"two\"}\r\n"
                "\r\n"
                "data: [DONE]\r\n"
                "\r\n");
            return;
        }
        if (request->method == "DELETE" && request->path == "/api/sessions/remote-session-1") {
            send_response(fd, 204, "No Content", "");
            return;
        }
        send_response(fd, 404, "Not Found", R"({"error":"not found"})");
    }

    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::jthread accept_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<LocalCcrHttpRequest> requests_;
};

class LocalWebSocketMcpServer {
public:
    LocalWebSocketMcpServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return;
        int yes = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        if (::listen(listen_fd_, 4) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }

        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            port_ = ntohs(addr.sin_port);
        }

        running_.store(true);
        accept_thread_ = std::jthread([this](std::stop_token stop) {
            accept_loop(stop);
        });
    }

    ~LocalWebSocketMcpServer() {
        running_.store(false);
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    [[nodiscard]] bool ready() const noexcept { return listen_fd_ >= 0 && port_ != 0; }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    [[nodiscard]] std::vector<std::string> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }

    [[nodiscard]] std::string handshake_headers() const {
        std::lock_guard lock(mutex_);
        return handshake_headers_;
    }

    [[nodiscard]] bool wait_for_tool_call(
        std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return saw_tool_call_; });
    }

private:
    void accept_loop(std::stop_token stop) {
        while (!stop.stop_requested() && running_.load()) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_fd < 0) {
                if (!running_.load()) break;
                continue;
            }
            handle_client(client_fd);
            ::close(client_fd);
            break;
        }
    }

    static std::optional<std::string> read_http_headers(int fd) {
        std::string request;
        char buffer[1024];
        while (request.find("\r\n\r\n") == std::string::npos) {
            auto n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) return std::nullopt;
            request.append(buffer, buffer + n);
            if (request.size() > 64 * 1024) return std::nullopt;
        }
        return request;
    }

    struct Frame {
        std::uint8_t opcode = 0;
        std::string payload;
    };

    static bool read_exact(int fd, char* data, std::size_t size) {
        while (size > 0) {
            auto n = ::recv(fd, data, size, 0);
            if (n <= 0) return false;
            data += n;
            size -= static_cast<std::size_t>(n);
        }
        return true;
    }

    static std::optional<Frame> read_frame(int fd) {
        unsigned char header[2]{};
        if (!read_exact(fd, reinterpret_cast<char*>(header), 2)) return std::nullopt;

        Frame frame;
        frame.opcode = header[0] & 0x0f;
        const bool masked = (header[1] & 0x80) != 0;
        std::uint64_t len = header[1] & 0x7f;
        if (len == 126) {
            unsigned char ext[2]{};
            if (!read_exact(fd, reinterpret_cast<char*>(ext), 2)) return std::nullopt;
            len = (static_cast<std::uint64_t>(ext[0]) << 8) | ext[1];
        } else if (len == 127) {
            unsigned char ext[8]{};
            if (!read_exact(fd, reinterpret_cast<char*>(ext), 8)) return std::nullopt;
            len = 0;
            for (unsigned char byte : ext) len = (len << 8) | byte;
        }

        std::array<unsigned char, 4> mask{};
        if (masked && !read_exact(fd, reinterpret_cast<char*>(mask.data()), mask.size())) {
            return std::nullopt;
        }

        frame.payload.resize(static_cast<std::size_t>(len));
        if (len > 0 && !read_exact(fd, frame.payload.data(), frame.payload.size())) {
            return std::nullopt;
        }
        if (masked) {
            for (std::size_t i = 0; i < frame.payload.size(); ++i) {
                frame.payload[i] = static_cast<char>(frame.payload[i] ^ mask[i % 4]);
            }
        }
        return frame;
    }

    static bool send_text_frame(int fd, std::string_view payload) {
        std::string frame;
        frame.push_back(static_cast<char>(0x81));
        const auto len = payload.size();
        if (len < 126) {
            frame.push_back(static_cast<char>(len));
        } else if (len <= 0xffff) {
            frame.push_back(static_cast<char>(126));
            frame.push_back(static_cast<char>((len >> 8) & 0xff));
            frame.push_back(static_cast<char>(len & 0xff));
        } else {
            frame.push_back(static_cast<char>(127));
            for (int shift = 56; shift >= 0; shift -= 8) {
                frame.push_back(static_cast<char>((len >> shift) & 0xff));
            }
        }
        frame.append(payload);
        return send_all(fd, frame);
    }

    void handle_client(int fd) {
        auto headers = read_http_headers(fd);
        if (!headers) return;
        {
            std::lock_guard lock(mutex_);
            handshake_headers_ = *headers;
        }

        const std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: local-test\r\n"
            "Sec-WebSocket-Protocol: mcp\r\n\r\n";
        if (!send_all(fd, response)) return;

        while (running_.load()) {
            auto frame = read_frame(fd);
            if (!frame) break;
            if (frame->opcode == 0x8) break;
            if (frame->opcode != 0x1 && frame->opcode != 0x2) continue;

            {
                std::lock_guard lock(mutex_);
                requests_.push_back(frame->payload);
            }

            auto doc = cc::utils::json::parse(frame->payload);
            if (!doc) continue;
            auto root = doc->root();
            const auto method = std::string(root.get("method").as_str());
            auto id = root.get("id");
            if (method == "initialize") {
                auto payload = std::format(
                    R"({{"jsonrpc":"2.0","id":{},"result":{{"protocolVersion":"2024-11-05","capabilities":{{"tools":{{}}}},"serverInfo":{{"name":"ide-ws-test","version":"1.0.0"}}}}}})",
                    json_id_literal(id));
                send_text_frame(fd, payload);
            } else if (method == "tools/call") {
                const auto params = root.get("params");
                const auto name = std::string(params.get("name").as_str());
                auto payload = std::format(
                    R"({{"jsonrpc":"2.0","id":{},"result":{{"content":[{{"type":"text","text":"called:{}"}}],"isError":false}}}})",
                    json_id_literal(id),
                    name);
                send_text_frame(fd, payload);
                {
                    std::lock_guard lock(mutex_);
                    saw_tool_call_ = true;
                }
                cv_.notify_all();
                break;
            }
        }
    }

    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::jthread accept_thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string handshake_headers_;
    std::vector<std::string> requests_;
    bool saw_tool_call_ = false;
};

class LocalSseMcpServer {
public:
    LocalSseMcpServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return;
        int yes = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        if (::listen(listen_fd_, 8) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }

        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            port_ = ntohs(addr.sin_port);
        }

        running_.store(true);
        accept_thread_ = std::jthread([this](std::stop_token stop) {
            accept_loop(stop);
        });
    }

    ~LocalSseMcpServer() {
        running_.store(false);
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        {
            std::lock_guard lock(sse_mutex_);
            if (sse_fd_ >= 0) {
                ::shutdown(sse_fd_, SHUT_RDWR);
            }
        }
        if (accept_thread_.joinable()) {
            accept_thread_.request_stop();
            accept_thread_.join();
        }
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }

    [[nodiscard]] bool ready() const {
        return listen_fd_ >= 0 && port_ != 0;
    }

    [[nodiscard]] std::string url() const {
        return std::format("http://127.0.0.1:{}/sse", port_);
    }

    [[nodiscard]] uint16_t port() const {
        return port_;
    }

    [[nodiscard]] std::vector<std::string> post_bodies() const {
        std::lock_guard lock(posts_mutex_);
        return post_bodies_;
    }

private:
    void accept_loop(std::stop_token stop) {
        while (!stop.stop_requested() && running_.load()) {
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                if (!running_.load()) return;
                continue;
            }
            workers_.emplace_back([this, fd](std::stop_token) {
                handle_connection(fd);
            });
        }
    }

    static std::string read_http_request(int fd) {
        std::string request;
        char c;
        while (request.size() < 16384) {
            if (::recv(fd, &c, 1, 0) <= 0) return request;
            request += c;
            if (request.find("\r\n\r\n") != std::string::npos) break;
        }

        const auto content_length_pos = request.find("Content-Length:");
        if (content_length_pos == std::string::npos) return request;
        auto value_start = content_length_pos + std::string_view("Content-Length:").size();
        while (value_start < request.size() && request[value_start] == ' ') ++value_start;
        auto value_end = request.find("\r\n", value_start);
        const auto body_len = static_cast<std::size_t>(std::atoi(request.substr(value_start, value_end - value_start).c_str()));
        const auto body_start = request.find("\r\n\r\n") + 4;
        while (request.size() < body_start + body_len) {
            char buf[4096];
            auto n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            request.append(buf, static_cast<std::size_t>(n));
        }
        return request;
    }

    void handle_connection(int fd) {
        auto request = read_http_request(fd);
        if (request.starts_with("GET /sse ")) {
            handle_sse(fd);
            return;
        }
        if (request.starts_with("POST /messages ")) {
            handle_post(fd, request);
            return;
        }
        send_all(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        ::close(fd);
    }

    void handle_sse(int fd) {
        {
            std::lock_guard lock(sse_mutex_);
            sse_fd_ = fd;
        }
        send_all(fd,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n\r\n"
            "event: endpoint\n"
            "data: /messages\n\n");

        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        ::close(fd);
        {
            std::lock_guard lock(sse_mutex_);
            if (sse_fd_ == fd) sse_fd_ = -1;
        }
    }

    void handle_post(int fd, const std::string& request) {
        const auto body_start = request.find("\r\n\r\n");
        const auto body = body_start == std::string::npos ? std::string{} : request.substr(body_start + 4);
        {
            std::lock_guard lock(posts_mutex_);
            post_bodies_.push_back(body);
        }

        send_all(fd, "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        ::close(fd);

        auto parsed = cc::utils::json::parse(body);
        if (!parsed) return;
        auto root = parsed->root();
        const auto method = std::string(root.get("method").as_str());
        const auto id = json_id_literal(root.get("id"));
        if (method == "initialize") {
            send_sse_json(std::format(
                R"({{"jsonrpc":"2.0","id":{},"result":{{"protocolVersion":"2024-11-05","capabilities":{{"tools":{{}}}},"serverInfo":{{"name":"sse-fixture","version":"1.0.0"}}}}}})",
                id));
        } else if (method == "tools/list") {
            send_sse_json(std::format(
                R"({{"jsonrpc":"2.0","id":{},"result":{{"tools":[{{"name":"sse_lookup","description":"Lookup through SSE","inputSchema":{{"type":"object"}}}}]}}}})",
                id));
        } else if (method == "tools/call") {
            const auto params = root.get("params");
            const auto name = params.is_obj() ? std::string(params.get("name").as_str()) : std::string{};
            send_sse_json(std::format(
                R"({{"jsonrpc":"2.0","id":{},"result":{{"content":[{{"type":"text","text":"called:{}"}}],"isError":false}}}})",
                id,
                name));
        }
    }

    void send_sse_json(const std::string& json) {
        std::lock_guard lock(sse_mutex_);
        if (sse_fd_ < 0) return;
        send_all(sse_fd_, "data: " + json + "\n\n");
    }

    int listen_fd_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::jthread accept_thread_;
    std::vector<std::jthread> workers_;
    mutable std::mutex sse_mutex_;
    int sse_fd_ = -1;
    mutable std::mutex posts_mutex_;
    std::vector<std::string> post_bodies_;
};

std::string remote_ws_base64_encode(const unsigned char* data, std::size_t len) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<std::uint32_t>(data[i + 2]);
        result.push_back(table[(n >> 18) & 0x3f]);
        result.push_back(table[(n >> 12) & 0x3f]);
        result.push_back((i + 1 < len) ? table[(n >> 6) & 0x3f] : '=');
        result.push_back((i + 2 < len) ? table[n & 0x3f] : '=');
    }
    return result;
}

std::string remote_ws_accept_key(std::string_view key) {
    const std::string input = std::string(key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::array<unsigned char, 20> hash{};
#ifdef __APPLE__
    CC_SHA1(input.data(), static_cast<CC_LONG>(input.size()), hash.data());
#else
    SHA1(reinterpret_cast<const unsigned char*>(input.data()), input.size(), hash.data());
#endif
    return remote_ws_base64_encode(hash.data(), hash.size());
}

class LocalRemoteSessionWebSocketServer {
public:
    LocalRemoteSessionWebSocketServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return;
        int yes = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        if (::listen(listen_fd_, 4) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }

        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            port_ = ntohs(addr.sin_port);
        }

        running_.store(true);
        accept_thread_ = std::jthread([this](std::stop_token stop) {
            accept_loop(stop);
        });
    }

    ~LocalRemoteSessionWebSocketServer() {
        running_.store(false);
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    [[nodiscard]] bool ready() const noexcept { return listen_fd_ >= 0 && port_ != 0; }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    [[nodiscard]] std::string handshake_headers() const {
        std::lock_guard lock(mutex_);
        return handshake_headers_;
    }

    [[nodiscard]] std::string handshake_path() const {
        std::lock_guard lock(mutex_);
        return handshake_path_;
    }

    [[nodiscard]] std::optional<std::vector<std::string>> wait_for_text_frames(
        std::size_t count,
        std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this, count] { return text_frames_.size() >= count; })) {
            return std::nullopt;
        }
        return text_frames_;
    }

private:
    struct Frame {
        std::uint8_t opcode = 0;
        std::string payload;
    };

    void accept_loop(std::stop_token stop) {
        while (!stop.stop_requested() && running_.load()) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_fd < 0) {
                if (!running_.load()) break;
                continue;
            }
            handle_client(client_fd);
            ::close(client_fd);
            break;
        }
    }

    static std::optional<std::string> read_http_headers(int fd) {
        std::string request;
        char buffer[1024];
        while (request.find("\r\n\r\n") == std::string::npos) {
            auto n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) return std::nullopt;
            request.append(buffer, buffer + n);
            if (request.size() > 64 * 1024) return std::nullopt;
        }
        return request;
    }

    static std::string request_path(std::string_view headers) {
        auto first_line_end = headers.find("\r\n");
        if (first_line_end == std::string_view::npos) return {};
        std::istringstream line(std::string(headers.substr(0, first_line_end)));
        std::string method;
        std::string path;
        line >> method >> path;
        return path;
    }

    static std::string header_value(std::string_view headers, std::string_view name) {
        auto pos = headers.find(name);
        if (pos == std::string_view::npos) return {};
        auto value_start = headers.find(':', pos);
        if (value_start == std::string_view::npos) return {};
        ++value_start;
        while (value_start < headers.size() && headers[value_start] == ' ') ++value_start;
        auto value_end = headers.find("\r\n", value_start);
        if (value_end == std::string_view::npos) return {};
        return std::string(headers.substr(value_start, value_end - value_start));
    }

    static bool read_exact(int fd, char* data, std::size_t size) {
        while (size > 0) {
            auto n = ::recv(fd, data, size, 0);
            if (n <= 0) return false;
            data += n;
            size -= static_cast<std::size_t>(n);
        }
        return true;
    }

    static std::optional<Frame> read_frame(int fd) {
        unsigned char header[2]{};
        if (!read_exact(fd, reinterpret_cast<char*>(header), 2)) return std::nullopt;

        Frame frame;
        frame.opcode = header[0] & 0x0f;
        const bool masked = (header[1] & 0x80) != 0;
        std::uint64_t len = header[1] & 0x7f;
        if (len == 126) {
            unsigned char ext[2]{};
            if (!read_exact(fd, reinterpret_cast<char*>(ext), 2)) return std::nullopt;
            len = (static_cast<std::uint64_t>(ext[0]) << 8) | ext[1];
        } else if (len == 127) {
            unsigned char ext[8]{};
            if (!read_exact(fd, reinterpret_cast<char*>(ext), 8)) return std::nullopt;
            len = 0;
            for (unsigned char byte : ext) len = (len << 8) | byte;
        }

        std::array<unsigned char, 4> mask{};
        if (masked && !read_exact(fd, reinterpret_cast<char*>(mask.data()), mask.size())) {
            return std::nullopt;
        }

        frame.payload.resize(static_cast<std::size_t>(len));
        if (len > 0 && !read_exact(fd, frame.payload.data(), frame.payload.size())) {
            return std::nullopt;
        }
        if (masked) {
            for (std::size_t i = 0; i < frame.payload.size(); ++i) {
                frame.payload[i] = static_cast<char>(frame.payload[i] ^ mask[i % 4]);
            }
        }
        return frame;
    }

    static bool send_text_frame(int fd, std::string_view payload) {
        std::string frame;
        frame.push_back(static_cast<char>(0x81));
        const auto len = payload.size();
        if (len < 126) {
            frame.push_back(static_cast<char>(len));
        } else if (len <= 0xffff) {
            frame.push_back(static_cast<char>(126));
            frame.push_back(static_cast<char>((len >> 8) & 0xff));
            frame.push_back(static_cast<char>(len & 0xff));
        } else {
            frame.push_back(static_cast<char>(127));
            for (int shift = 56; shift >= 0; shift -= 8) {
                frame.push_back(static_cast<char>((len >> shift) & 0xff));
            }
        }
        frame.append(payload);
        return send_all(fd, frame);
    }

    void handle_client(int fd) {
        auto headers = read_http_headers(fd);
        if (!headers) return;
        const auto key = header_value(*headers, "Sec-WebSocket-Key");
        {
            std::lock_guard lock(mutex_);
            handshake_headers_ = *headers;
            handshake_path_ = request_path(*headers);
        }

        const auto response = std::format(
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: {}\r\n\r\n",
            remote_ws_accept_key(key));
        if (!send_all(fd, response)) return;

        while (running_.load()) {
            auto frame = read_frame(fd);
            if (!frame) break;
            if (frame->opcode == 0x8) break;
            if (frame->opcode != 0x1) continue;
            {
                std::lock_guard lock(mutex_);
                text_frames_.push_back(frame->payload);
            }
            cv_.notify_all();

            if (frame->payload.find(R"("type":"request")") != std::string::npos) {
                send_text_frame(fd,
                    R"({"type":"response","id":"server-response","method":"agent.message","payload":{"ok":true}})");
            }
        }
    }

    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::jthread accept_thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string handshake_headers_;
    std::string handshake_path_;
    std::vector<std::string> text_frames_;
};

class LocalStreamableHttpMcpServer {
public:
    LocalStreamableHttpMcpServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return;
        int yes = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        if (::listen(listen_fd_, 8) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }

        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            port_ = ntohs(addr.sin_port);
        }

        running_.store(true);
        accept_thread_ = std::jthread([this](std::stop_token stop) {
            accept_loop(stop);
        });
    }

    ~LocalStreamableHttpMcpServer() {
        running_.store(false);
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) {
            accept_thread_.request_stop();
            accept_thread_.join();
        }
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }

    [[nodiscard]] bool ready() const {
        return listen_fd_ >= 0 && port_ != 0;
    }

    [[nodiscard]] std::string url() const {
        return std::format("http://127.0.0.1:{}/mcp", port_);
    }

    [[nodiscard]] std::vector<std::string> requests() const {
        std::lock_guard lock(requests_mutex_);
        return requests_;
    }

    [[nodiscard]] std::vector<std::string> post_bodies() const {
        std::lock_guard lock(requests_mutex_);
        return post_bodies_;
    }

private:
    void accept_loop(std::stop_token stop) {
        while (!stop.stop_requested() && running_.load()) {
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                if (!running_.load()) return;
                continue;
            }
            workers_.emplace_back([this, fd](std::stop_token) {
                handle_connection(fd);
            });
        }
    }

    static std::string read_http_request(int fd) {
        std::string request;
        char c;
        while (request.size() < 16384) {
            if (::recv(fd, &c, 1, 0) <= 0) return request;
            request += c;
            if (request.find("\r\n\r\n") != std::string::npos) break;
        }

        const auto content_length_pos = request.find("Content-Length:");
        if (content_length_pos == std::string::npos) return request;
        auto value_start = content_length_pos + std::string_view("Content-Length:").size();
        while (value_start < request.size() && request[value_start] == ' ') ++value_start;
        auto value_end = request.find("\r\n", value_start);
        const auto body_len = static_cast<std::size_t>(std::atoi(request.substr(value_start, value_end - value_start).c_str()));
        const auto body_start = request.find("\r\n\r\n") + 4;
        while (request.size() < body_start + body_len) {
            char buf[4096];
            auto n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            request.append(buf, static_cast<std::size_t>(n));
        }
        return request;
    }

    void handle_connection(int fd) {
        auto request = read_http_request(fd);
        if (request.starts_with("POST /mcp ")) {
            handle_post(fd, request);
            return;
        }
        send_all(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        ::close(fd);
    }

    void handle_post(int fd, const std::string& request) {
        const auto body_start = request.find("\r\n\r\n");
        const auto body = body_start == std::string::npos ? std::string{} : request.substr(body_start + 4);
        {
            std::lock_guard lock(requests_mutex_);
            requests_.push_back(request);
            post_bodies_.push_back(body);
        }

        auto parsed = cc::utils::json::parse(body);
        if (!parsed) {
            send_empty(fd, "400 Bad Request");
            return;
        }

        auto root = parsed->root();
        const auto method = std::string(root.get("method").as_str());
        if (method == "notifications/initialized" || method == "notifications/cancelled") {
            send_empty(fd, "202 Accepted");
            return;
        }

        const auto id = json_id_literal(root.get("id"));
        if (method == "initialize") {
            send_json(fd, std::format(
                R"({{"jsonrpc":"2.0","id":{},"result":{{"protocolVersion":"2024-11-05","capabilities":{{"tools":{{}}}},"serverInfo":{{"name":"http-fixture","version":"1.0.0"}}}}}})",
                id));
            return;
        }
        if (method == "tools/list") {
            send_json(fd, std::format(
                R"({{"jsonrpc":"2.0","id":{},"result":{{"tools":[{{"name":"http_lookup","description":"Lookup through HTTP","inputSchema":{{"type":"object"}}}}]}}}})",
                id));
            return;
        }

        send_empty(fd, "404 Not Found");
    }

    static void send_empty(int fd, std::string_view status) {
        send_all(fd, std::format(
            "HTTP/1.1 {}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
            status));
        ::close(fd);
    }

    static void send_json(int fd, const std::string& body) {
        send_all(fd, std::format(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            body.size(),
            body));
        ::close(fd);
    }

    int listen_fd_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::jthread accept_thread_;
    std::vector<std::jthread> workers_;
    mutable std::mutex requests_mutex_;
    std::vector<std::string> requests_;
    std::vector<std::string> post_bodies_;
};

class LocalUnauthorizedStreamableHttpMcpServer {
public:
    LocalUnauthorizedStreamableHttpMcpServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return;
        int yes = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        if (::listen(listen_fd_, 8) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }

        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            port_ = ntohs(addr.sin_port);
        }

        running_.store(true);
        accept_thread_ = std::jthread([this](std::stop_token stop) {
            accept_loop(stop);
        });
    }

    ~LocalUnauthorizedStreamableHttpMcpServer() {
        running_.store(false);
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) {
            accept_thread_.request_stop();
            accept_thread_.join();
        }
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }

    [[nodiscard]] bool ready() const {
        return listen_fd_ >= 0 && port_ != 0;
    }

    [[nodiscard]] std::string url() const {
        return std::format("http://127.0.0.1:{}/mcp", port_);
    }

    [[nodiscard]] std::vector<std::string> requests() const {
        std::lock_guard lock(requests_mutex_);
        return requests_;
    }

private:
    void accept_loop(std::stop_token stop) {
        while (!stop.stop_requested() && running_.load()) {
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                if (!running_.load()) return;
                continue;
            }
            workers_.emplace_back([this, fd](std::stop_token) {
                handle_connection(fd);
            });
        }
    }

    static std::string read_http_request(int fd) {
        std::string request;
        char c;
        while (request.size() < 16384) {
            if (::recv(fd, &c, 1, 0) <= 0) return request;
            request += c;
            if (request.find("\r\n\r\n") != std::string::npos) break;
        }

        const auto content_length_pos = request.find("Content-Length:");
        if (content_length_pos == std::string::npos) return request;
        auto value_start = content_length_pos + std::string_view("Content-Length:").size();
        while (value_start < request.size() && request[value_start] == ' ') ++value_start;
        const auto value_end = request.find("\r\n", value_start);
        const auto body_len = static_cast<std::size_t>(std::atoi(request.substr(value_start, value_end - value_start).c_str()));
        const auto body_start = request.find("\r\n\r\n") + 4;
        while (request.size() < body_start + body_len) {
            char buf[4096];
            auto n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            request.append(buf, static_cast<std::size_t>(n));
        }
        return request;
    }

    void handle_connection(int fd) {
        auto request = read_http_request(fd);
        {
            std::lock_guard lock(requests_mutex_);
            requests_.push_back(std::move(request));
        }
        send_all(fd,
            "HTTP/1.1 401 Unauthorized\r\n"
            "WWW-Authenticate: Bearer realm=\"mcp\"\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n");
        ::close(fd);
    }

    int listen_fd_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::jthread accept_thread_;
    std::vector<std::jthread> workers_;
    mutable std::mutex requests_mutex_;
    std::vector<std::string> requests_;
};

} // namespace

TEST(LspConfig, LoadsPluginLspServersFromManifestAndRoutesExtension) {
    auto root = fs::weakly_canonical(fs::temp_directory_path()) / "cc_repl_plugin_lsp_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude");
    EnvironmentGuard home_guard("HOME", root.string());
    EnvironmentGuard plugin_cache_guard(
        "CLAUDE_CODE_PLUGIN_CACHE_DIR",
        (root / ".claude" / "plugins").string()
    );

    const auto plugin_root = root / ".claude" / "plugins" / "lsp-fixture";
    fs::create_directories(plugin_root / "workspace");
    const auto log_path = root / "lsp-log.jsonl";
    const auto server_path = plugin_root / "server.js";
    {
        std::ofstream server(server_path);
        server << R"JS(
const fs = require('node:fs');
const logPath = process.env.PLUGIN_LSP_LOG;
let buffer = Buffer.alloc(0);

function send(message) {
  const body = JSON.stringify(message);
  process.stdout.write(`Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`);
}

function handle(message) {
  if (logPath) {
    fs.appendFileSync(logPath, `${JSON.stringify(message)}\n`);
  }
  if (message.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: message.id,
      result: { capabilities: { textDocumentSync: 1 } },
    });
  }
  if (message.method === 'exit') {
    process.exit(0);
  }
}

process.stdin.on('data', chunk => {
  buffer = Buffer.concat([buffer, chunk]);
  while (true) {
    const headerEnd = buffer.indexOf('\r\n\r\n');
    if (headerEnd === -1) return;
    const header = buffer.subarray(0, headerEnd).toString();
    const match = /Content-Length:\s*(\d+)/i.exec(header);
    if (!match) process.exit(2);
    const length = Number(match[1]);
    const bodyStart = headerEnd + 4;
    if (buffer.length < bodyStart + length) return;
    const body = buffer.subarray(bodyStart, bodyStart + length).toString();
    buffer = buffer.subarray(bodyStart + length);
    handle(JSON.parse(body));
  }
});
process.stdin.resume();
)JS";
    }
    {
        std::ofstream settings(root / ".claude" / "settings.json");
        settings << R"JSON({
  "pluginConfigs": {
    "lsp-fixture": {
      "options": {
        "mode": "configured"
      }
    }
  }
})JSON";
    }
    {
        std::ofstream defaults(plugin_root / ".lsp.json");
        defaults << R"JSON({
  "fixture": {
    "command": "missing-lsp-command",
    "extensionToLanguage": {".foo": "foo-default"}
  }
})JSON";
    }
    {
        std::ofstream manifest(plugin_root / "plugin.json");
        manifest << R"JSON({
  "name": "lsp-fixture",
  "version": "1.0.0",
  "entry_point": "plugin.js",
  "lspServers": {
    "fixture": {
      "command": "node",
      "args": [
        "${CLAUDE_PLUGIN_ROOT}/server.js",
        "${user_config.mode}",
        "${PLUGIN_LSP_MISSING:-fallback}"
      ],
      "extensionToLanguage": {".foo": "foo-plugin"},
      "env": {
        "PLUGIN_LSP_MODE": "${user_config.mode}",
        "PLUGIN_LSP_ROOT": "${CLAUDE_PLUGIN_ROOT}",
        "PLUGIN_LSP_LOG": ")JSON" << log_path.string() << R"JSON("
      },
      "workspaceFolder": "${CLAUDE_PLUGIN_ROOT}/workspace",
      "initializationOptions": {"mode": "configured", "feature": true}
    }
  }
})JSON";
    }

    {
        CurrentPathGuard cwd(root);
        auto servers = cc::services::lsp::discover_plugin_lsp_servers();
        auto it = std::ranges::find_if(servers, [](const auto& server) {
            return server.name == "plugin:lsp-fixture:fixture";
        });
        ASSERT_NE(it, servers.end());
        EXPECT_EQ(it->config.command, "node");
        ASSERT_EQ(it->config.args.size(), 3u);
        EXPECT_EQ(it->config.args[0], server_path.string());
        EXPECT_EQ(it->config.args[1], "configured");
        EXPECT_EQ(it->config.args[2], "fallback");
        EXPECT_EQ(it->config.env.at("PLUGIN_LSP_MODE"), "configured");
        EXPECT_EQ(it->config.env.at("PLUGIN_LSP_ROOT"), plugin_root.string());
        EXPECT_EQ(it->config.env.at("PLUGIN_LSP_LOG"), log_path.string());
        EXPECT_EQ(it->config.env.at("CLAUDE_PLUGIN_ROOT"), plugin_root.string());
        EXPECT_EQ(
            it->config.env.at("CLAUDE_PLUGIN_DATA"),
            (root / ".claude" / "plugins" / "data" / "lsp-fixture").string()
        );
        ASSERT_TRUE(it->config.workspace_folder.has_value());
        EXPECT_EQ(*it->config.workspace_folder, (plugin_root / "workspace").string());
        EXPECT_NE(it->config.initialization_options_json.find("\"mode\":\"configured\""), std::string::npos);
        EXPECT_NE(it->config.initialization_options_json.find("\"feature\":true"), std::string::npos);
        EXPECT_EQ(it->config.extension_to_language.at("foo"), "foo-plugin");

        auto manager = cc::services::lsp::create_lsp_server_manager();
        auto initialized = manager->initialize();
        ASSERT_TRUE(initialized.has_value()) << initialized.error().message();
        auto* routed = manager->get_server_for_file((root / "sample.foo").string());
        ASSERT_NE(routed, nullptr);
        EXPECT_EQ(routed->name, "plugin:lsp-fixture:fixture");
        EXPECT_EQ(routed->config.extension_to_language.at("foo"), "foo-plugin");

        auto opened = manager->open_file((root / "sample.foo").string(), "let x = 1;");
        ASSERT_TRUE(opened.has_value()) << opened.error().message();

        std::string log;
        for (int attempt = 0; attempt < 50; ++attempt) {
            std::ifstream input(log_path);
            if (input) {
                std::stringstream buffer;
                buffer << input.rdbuf();
                log = buffer.str();
                if (log.find("\"method\":\"textDocument/didOpen\"") != std::string::npos) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }

        const auto initialize_pos = log.find("\"method\":\"initialize\"");
        const auto initialized_pos = log.find("\"method\":\"initialized\"");
        const auto did_open_pos = log.find("\"method\":\"textDocument/didOpen\"");
        EXPECT_NE(initialize_pos, std::string::npos) << log;
        EXPECT_NE(initialized_pos, std::string::npos) << log;
        EXPECT_NE(did_open_pos, std::string::npos) << log;
        if (initialize_pos != std::string::npos &&
            initialized_pos != std::string::npos &&
            did_open_pos != std::string::npos) {
            EXPECT_LT(initialize_pos, initialized_pos);
            EXPECT_LT(initialized_pos, did_open_pos);
        }
        EXPECT_NE(log.find("\"initializationOptions\":{\"mode\":\"configured\",\"feature\":true}"), std::string::npos) << log;
        EXPECT_NE(log.find("\"rootPath\":\"" + (plugin_root / "workspace").string() + "\""), std::string::npos) << log;
        EXPECT_NE(log.find("\"languageId\":\"foo-plugin\""), std::string::npos) << log;

        auto shutdown = manager->shutdown();
        EXPECT_TRUE(shutdown.has_value());
    }

    fs::remove_all(root);
}

TEST(CcrClient, UsesDefaultHttpTransportForRemoteSessionLifecycle) {
    LocalCcrHttpServer server;
    ASSERT_TRUE(server.ready());

    cc::cli::CcrClient client;
    auto connected = client.connect(server.base_url() + "/api", "ccr-token");
    ASSERT_TRUE(connected.has_value()) << connected.error();

    auto message = client.send_message("hello \"remote\"\nline");
    ASSERT_TRUE(message.has_value()) << message.error();
    EXPECT_EQ(*message, R"({"content":"remote-ok"})");

    auto info = client.get_session_info();
    EXPECT_EQ(info.id, "remote-session-1");
    EXPECT_EQ(info.status, "connected");
    EXPECT_EQ(info.messages_sent, 1u);
    EXPECT_EQ(info.messages_received, 1u);

    client.disconnect();

    auto requests = server.wait_for_requests(3);
    ASSERT_TRUE(requests.has_value());
    ASSERT_GE(requests->size(), 3u);

    EXPECT_EQ((*requests)[0].method, "POST");
    EXPECT_EQ((*requests)[0].path, "/api/sessions");
    EXPECT_NE((*requests)[0].headers.find("Authorization: Bearer ccr-token"), std::string::npos);
    EXPECT_NE((*requests)[0].body.find(R"("token":"ccr-token")"), std::string::npos);

    EXPECT_EQ((*requests)[1].method, "POST");
    EXPECT_EQ((*requests)[1].path, "/api/sessions/remote-session-1/messages");
    EXPECT_NE((*requests)[1].headers.find("Authorization: Bearer ccr-token"), std::string::npos);
    EXPECT_NE((*requests)[1].body.find(R"("session_id":"remote-session-1")"), std::string::npos);
    EXPECT_NE((*requests)[1].body.find(R"(hello \"remote\"\nline)"), std::string::npos);

    EXPECT_EQ((*requests)[2].method, "DELETE");
    EXPECT_EQ((*requests)[2].path, "/api/sessions/remote-session-1");
    EXPECT_NE((*requests)[2].headers.find("Authorization: Bearer ccr-token"), std::string::npos);
}

TEST(RemoteSession, ConnectsAuthenticatesSubscribesRequestsAndUnsubscribesOverWebSocket) {
    LocalRemoteSessionWebSocketServer server;
    ASSERT_TRUE(server.ready());

    cc::remote::RemoteSession session;
    std::vector<cc::remote::SessionMessage> messages;
    std::vector<std::string> errors;
    session.on_message([&](const cc::remote::SessionMessage& message) {
        messages.push_back(message);
    });
    session.on_error([&](std::string_view error) {
        errors.emplace_back(error);
    });

    auto connected = session.connect(cc::remote::RemoteSessionConfig{
        .host = "127.0.0.1",
        .port = server.port(),
        .token = "remote-token",
        .session_id = std::string{"remote-session-1"},
        .api_version = "v1",
        .ping_interval = std::chrono::seconds{30},
        .connect_timeout = std::chrono::seconds{3},
        .use_tls = false,
    });
    ASSERT_TRUE(connected.has_value()) << connected.error();
    EXPECT_EQ(session.status(), cc::remote::SessionStatus::Subscribed);

    auto sent = session.send_request("agent.message", R"({"content":"hello"})");
    ASSERT_TRUE(sent.has_value()) << sent.error();

    for (int attempt = 0; attempt < 30 && messages.empty(); ++attempt) {
        session.poll(std::chrono::milliseconds{50});
    }
    ASSERT_FALSE(messages.empty());
    EXPECT_EQ(messages.front().type, cc::remote::SessionMessageType::Response);
    EXPECT_EQ(messages.front().method, "agent.message");
    EXPECT_NE(messages.front().payload.find(R"("ok":true)"), std::string::npos);
    EXPECT_TRUE(errors.empty());

    session.disconnect();
    EXPECT_EQ(session.status(), cc::remote::SessionStatus::Disconnected);

    auto frames = server.wait_for_text_frames(4);
    ASSERT_TRUE(frames.has_value());
    ASSERT_EQ(frames->size(), 4u);

    EXPECT_EQ(server.handshake_path(), "/v1/sessions/ws/remote-session-1/subscribe");
    const auto headers = server.handshake_headers();
    EXPECT_NE(headers.find("Authorization: Bearer remote-token"), std::string::npos);
    EXPECT_NE(headers.find("X-Api-Version: v1"), std::string::npos);

    EXPECT_NE((*frames)[0].find(R"("type":"auth")"), std::string::npos);
    EXPECT_NE((*frames)[0].find(R"("token":"remote-token")"), std::string::npos);
    EXPECT_NE((*frames)[1].find(R"("type":"subscribe")"), std::string::npos);
    EXPECT_NE((*frames)[1].find(R"("session_id":"remote-session-1")"), std::string::npos);
    EXPECT_NE((*frames)[2].find(R"("type":"request")"), std::string::npos);
    EXPECT_NE((*frames)[2].find(R"("method":"agent.message")"), std::string::npos);
    EXPECT_NE((*frames)[2].find(R"("content":"hello")"), std::string::npos);
    EXPECT_NE((*frames)[3].find(R"("type":"unsubscribe")"), std::string::npos);
    EXPECT_NE((*frames)[3].find(R"("session_id":"remote-session-1")"), std::string::npos);
}

TEST(CcrClient, StreamsMessagesThroughDefaultHttpTransport) {
    LocalCcrHttpServer server;
    ASSERT_TRUE(server.ready());

    cc::cli::CcrClient client;
    auto connected = client.connect(server.base_url() + "/api", "ccr-token");
    ASSERT_TRUE(connected.has_value()) << connected.error();

    std::vector<std::pair<std::string, bool>> chunks;
    auto streamed = client.send_message_streaming("stream \"remote\"", [&](std::string_view chunk, bool is_final) {
        chunks.emplace_back(std::string(chunk), is_final);
    });
    ASSERT_TRUE(streamed.has_value()) << streamed.error();

    ASSERT_EQ(chunks.size(), 3u);
    EXPECT_EQ(chunks[0].first, R"({"delta":"one"})");
    EXPECT_FALSE(chunks[0].second);
    EXPECT_EQ(chunks[1].first, R"({"delta":"two"})");
    EXPECT_FALSE(chunks[1].second);
    EXPECT_EQ(chunks[2].first, "[DONE]");
    EXPECT_TRUE(chunks[2].second);

    auto info = client.get_session_info();
    EXPECT_EQ(info.id, "remote-session-1");
    EXPECT_EQ(info.messages_sent, 1u);
    EXPECT_EQ(info.messages_received, 1u);

    client.disconnect();

    auto requests = server.wait_for_requests(3);
    ASSERT_TRUE(requests.has_value());
    ASSERT_GE(requests->size(), 3u);

    EXPECT_EQ((*requests)[1].method, "POST");
    EXPECT_EQ((*requests)[1].path, "/api/sessions/remote-session-1/messages?stream=true");
    EXPECT_NE((*requests)[1].headers.find("Authorization: Bearer ccr-token"), std::string::npos);
    EXPECT_NE((*requests)[1].headers.find("Accept: text/event-stream"), std::string::npos);
    EXPECT_NE((*requests)[1].body.find(R"("session_id":"remote-session-1")"), std::string::npos);
    EXPECT_NE((*requests)[1].body.find(R"(stream \"remote\")"), std::string::npos);

    EXPECT_EQ((*requests)[2].method, "DELETE");
    EXPECT_EQ((*requests)[2].path, "/api/sessions/remote-session-1");
}

TEST(CcrClient, DoesNotInventSessionWhenRemoteHandshakeFails) {
    cc::cli::CcrClient client;
    client.set_http_transport([](const cc::cli::CcrHttpRequest&)
        -> std::expected<cc::cli::CcrHttpResponse, std::string> {
        return std::unexpected("network down");
    });

    auto connected = client.connect("https://remote.example/api", "ccr-token");
    ASSERT_FALSE(connected.has_value());
    EXPECT_NE(connected.error().find("Remote session handshake failed: network down"), std::string::npos);
    EXPECT_FALSE(client.is_connected());
}

TEST(ApiErrors, ClassifiesHttpStatusCodes) {
    using cc::services::api::errors::ApiErrorCategory;
    using cc::services::api::errors::ErrorClassifier;

    EXPECT_EQ(ErrorClassifier::classify_status(401), ApiErrorCategory::Authentication);
    EXPECT_EQ(ErrorClassifier::classify_status(429), ApiErrorCategory::RateLimited);
    EXPECT_EQ(ErrorClassifier::classify_status(529), ApiErrorCategory::Overloaded);
    EXPECT_EQ(ErrorClassifier::classify_status(500), ApiErrorCategory::ServerError);
    EXPECT_EQ(ErrorClassifier::classify_status(400), ApiErrorCategory::InvalidRequest);
}

TEST(ApiErrors, RetryDecisionUsesRetryableCategories) {
    using cc::services::api::errors::ApiErrorCategory;
    using cc::services::api::errors::ApiErrorDetails;
    using cc::services::api::errors::ErrorClassifier;

    ApiErrorDetails rate_limited{};
    rate_limited.category = ApiErrorCategory::RateLimited;
    rate_limited.http_status = 429;
    ApiErrorDetails bad_request{};
    bad_request.category = ApiErrorCategory::InvalidRequest;
    bad_request.http_status = 400;

    EXPECT_TRUE(ErrorClassifier::is_retryable(rate_limited));
    EXPECT_FALSE(ErrorClassifier::is_retryable(bad_request));
}

TEST(ApiErrors, ClientMapsJsonHttpErrorsToStructuredMessages) {
    auto error = cc::services::api::AnthropicClient::error_from_http_response(
        400,
        R"({"type":"error","error":{"type":"invalid_request_error","message":"prompt is too long"}})",
        std::optional<std::string>{"req_123"});

    EXPECT_EQ(error.code(), cc::utils::ErrorCode::invalid_argument);
    EXPECT_NE(error.message().find("HTTP 400 invalid_request_error: prompt is too long"), std::string::npos);
    EXPECT_NE(error.message().find("req_123"), std::string::npos);
}

TEST(ApiErrors, ClientPreservesRetryAfterFromJsonHttpErrors) {
    auto error = cc::services::api::AnthropicClient::error_from_http_response(
        429,
        R"({"error":{"type":"rate_limit_error","message":"too many requests","retry_after_seconds":7}})");

    EXPECT_EQ(error.code(), cc::utils::ErrorCode::resource_exhausted);
    EXPECT_NE(error.message().find("rate_limit_error: too many requests"), std::string::npos);
    EXPECT_NE(error.message().find("retry after: 7s"), std::string::npos);
}

TEST(ApiErrors, ClientErrorDetailsDriveRetryClassification) {
    using cc::services::api::errors::ApiErrorCategory;
    using cc::services::api::errors::ErrorClassifier;

    auto invalid_error = cc::services::api::AnthropicClient::error_from_http_response(
        400,
        R"({"error":{"type":"invalid_request_error","message":"bad tool schema"}})");
    auto invalid_details = cc::services::api::AnthropicClient::error_details_from_error(invalid_error);

    EXPECT_EQ(invalid_details.category, ApiErrorCategory::InvalidRequest);
    EXPECT_EQ(invalid_details.http_status, 400);
    EXPECT_EQ(invalid_details.error_type, "invalid_request_error");
    EXPECT_FALSE(ErrorClassifier::is_retryable(invalid_details));

    auto rate_limit_error = cc::services::api::AnthropicClient::error_from_http_response(
        429,
        R"({"error":{"type":"rate_limit_error","message":"too many requests","retry_after_seconds":7}})");
    auto rate_limit_details = cc::services::api::AnthropicClient::error_details_from_error(rate_limit_error);

    EXPECT_EQ(rate_limit_details.category, ApiErrorCategory::RateLimited);
    EXPECT_EQ(rate_limit_details.retry_after_seconds, std::optional<int>{7});
    EXPECT_TRUE(ErrorClassifier::is_retryable(rate_limit_details));
}

TEST(QueryEngine, AppliesPerQueryEnabledToolsToAnthropicRequest) {
    LocalAnthropicMessagesServer server;
    ASSERT_NE(server.port(), 0);

    auto root = fs::temp_directory_path() / "cc-repl-query-engine-tool-filter-test";
    fs::remove_all(root);
    fs::create_directories(root);

    cc::core::ToolRegistry registry;
    cc::core::QueryEngineConfig config;
    config.api_key = "test-key";
    config.base_url = server.base_url();
    config.cwd = root.string();
    config.retry_policy.max_retries = 0;
    config.thinking_config.mode = cc::core::ThinkingConfig::Mode::Disabled;
    config.tools = {
        cc::core::ToolDefinition{
            .name = "Read",
            .description = "Read a file",
            .input_schema = cc::core::InputSchema{
                .properties = {
                    cc::core::SchemaProperty{
                        .name = "file_path",
                        .type = "string",
                        .description = "File path",
                        .required = true,
                    },
                },
            },
            .permission = cc::core::ToolPermission::ReadOnly,
        },
        cc::core::ToolDefinition{
            .name = "Write",
            .description = "Write a file",
            .input_schema = cc::core::InputSchema{
                .properties = {
                    cc::core::SchemaProperty{
                        .name = "file_path",
                        .type = "string",
                        .description = "File path",
                        .required = true,
                    },
                },
            },
            .permission = cc::core::ToolPermission::Write,
        },
    };

    cc::core::QueryEngine engine(std::move(config), registry);
    cc::core::QueryOptions options;
    options.enabled_tools = {"Read"};

    auto response = engine.query("hello", options);
    ASSERT_TRUE(response.has_value()) << response.error().message;
    EXPECT_EQ(response->message.model, "claude-test");

    auto request_body = server.wait_for_body();
    ASSERT_TRUE(request_body.has_value());
    auto parsed = cc::utils::json::parse(*request_body);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();

    auto tools = parsed->root().get("tools");
    ASSERT_TRUE(tools.valid());
    ASSERT_TRUE(tools.is_arr());

    std::vector<std::string> tool_names;
    tools.iter([&](cc::utils::json::JsonVal tool) {
        tool_names.emplace_back(tool.get("name").as_str());
    });

    ASSERT_EQ(tool_names.size(), 1u) << *request_body;
    EXPECT_EQ(tool_names.front(), "Read");
    EXPECT_FALSE(parsed->root().get("stream").as_bool());

    fs::remove_all(root);
}

TEST(QueryEngine, CompactConversationPreservesSummarizedHistoryDetails) {
    cc::core::ToolRegistry registry;
    cc::core::QueryEngineConfig config;
    config.context_window.auto_compact = false;
    config.cwd = fs::temp_directory_path().string();

    cc::core::QueryEngine engine(std::move(config), registry);

    auto make_user = [](std::string text) {
        cc::core::UserMessage msg{};
        msg.id.value = "user-" + text;
        msg.timestamp = std::chrono::system_clock::now();
        msg.content.push_back(cc::core::TextBlock{std::move(text)});
        return cc::core::Message{std::move(msg)};
    };
    auto make_assistant = [](std::string text) {
        cc::core::AssistantMessage msg{};
        msg.id.value = "assistant-" + text;
        msg.timestamp = std::chrono::system_clock::now();
        msg.content.push_back(cc::core::TextBlock{std::move(text)});
        return cc::core::Message{std::move(msg)};
    };

    engine.append_message_for_testing(make_user("legacy requirement alpha"));
    engine.append_message_for_testing(make_assistant("assistant decision beta"));
    engine.append_message_for_testing(make_user("tool context gamma"));
    engine.append_message_for_testing(make_assistant("design constraint delta"));
    engine.append_message_for_testing(make_user("recent one"));
    engine.append_message_for_testing(make_assistant("recent two"));
    engine.append_message_for_testing(make_user("recent three"));
    engine.append_message_for_testing(make_assistant("recent four"));
    engine.append_message_for_testing(make_user("recent five"));
    engine.append_message_for_testing(make_assistant("recent six"));

    auto compacted = engine.compact_conversation();
    ASSERT_TRUE(compacted.has_value());

    auto conversation = engine.get_conversation();
    ASSERT_EQ(conversation.size(), 9u);

    const auto* boundary = std::get_if<cc::core::SystemMessage>(&conversation[1]);
    ASSERT_NE(boundary, nullptr);
    ASSERT_TRUE(boundary->subtype.has_value());
    EXPECT_EQ(*boundary->subtype, "compact_boundary");
    ASSERT_TRUE(boundary->compact_metadata.has_value());
    EXPECT_EQ(boundary->compact_metadata->trigger, "manual");
    EXPECT_GT(boundary->compact_metadata->pre_tokens, 0u);
    ASSERT_TRUE(boundary->compact_metadata->preserved_segment.has_value());
    EXPECT_EQ(boundary->compact_metadata->preserved_segment->head_uuid, "user-recent one");
    EXPECT_EQ(boundary->compact_metadata->preserved_segment->tail_uuid, "assistant-recent six");

    const auto* marker = std::get_if<cc::core::UserMessage>(&conversation[2]);
    ASSERT_NE(marker, nullptr);
    ASSERT_EQ(marker->content.size(), 1u);
    const auto* summary = std::get_if<cc::core::TextBlock>(&marker->content.front());
    ASSERT_NE(summary, nullptr);
    EXPECT_EQ(boundary->compact_metadata->preserved_segment->anchor_uuid, marker->id.value);

    EXPECT_NE(summary->text.find("legacy requirement alpha"), std::string::npos);
    EXPECT_NE(summary->text.find("assistant decision beta"), std::string::npos);
    EXPECT_NE(summary->text.find("Preserve these details"), std::string::npos);

    const auto* last = std::get_if<cc::core::AssistantMessage>(&conversation.back());
    ASSERT_NE(last, nullptr);
    const auto* last_text = std::get_if<cc::core::TextBlock>(&last->content.front());
    ASSERT_NE(last_text, nullptr);
    EXPECT_EQ(last_text->text, "recent six");
}

TEST(ApiClient, MessageFromTextCreatesSingleTextBlock) {
    auto message = cc::services::api::Message::from_text("user", "hello");

    ASSERT_EQ(message.role, "user");
    ASSERT_EQ(message.content.size(), 1u);
    EXPECT_EQ(message.content.front().type, cc::services::api::ContentBlockType::Text);
    EXPECT_EQ(message.content.front().text, "hello");
}

TEST(ApiClient, ResponseCombinesTextContentAndTokenUsage) {
    cc::services::api::CreateMessageResponse response;
    cc::services::api::ContentBlock first;
    first.type = cc::services::api::ContentBlockType::Text;
    first.text = "hello ";
    response.content.push_back(first);
    cc::services::api::ContentBlock second;
    second.type = cc::services::api::ContentBlockType::Text;
    second.text = "world";
    response.content.push_back(second);
    response.usage.input_tokens = 3;
    response.usage.output_tokens = 5;
    response.usage.cache_creation_tokens = 7;
    response.usage.cache_read_tokens = 11;

    EXPECT_EQ(response.get_text_content(), "hello world");
    EXPECT_EQ(response.usage.total(), 8);
    EXPECT_EQ(response.usage.total_with_cache(), 26);
}

TEST(VoiceService, TranscribesStreamThroughProvider) {
    bool called = false;
    cc::services::voice::VoiceService service(
        [&](std::span<const std::uint8_t> audio) -> cc::utils::Result<std::string> {
            called = true;
            EXPECT_EQ(audio.size(), 3u);
            EXPECT_EQ(audio[0], static_cast<std::uint8_t>('a'));
            return std::string("hello transcript");
        });

    std::istringstream input("abc");
    auto result = service.transcribe_stream(input);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(called);
    EXPECT_TRUE(result->success);
    EXPECT_EQ(result->text, "hello transcript");
    EXPECT_FALSE(result->error.has_value());
}

TEST(VoiceService, ReportsMissingTranscriptionProvider) {
    cc::services::voice::VoiceService service(cc::services::voice::VoiceService::TranscriptionProvider{});
    std::istringstream input("abc");

    auto result = service.transcribe_stream(input);

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->success);
    ASSERT_TRUE(result->error.has_value());
    EXPECT_EQ(*result->error, "No voice transcription provider is configured");
    EXPECT_EQ(result->text, "");
}

TEST(ApiClient, RequestSerializerPreservesToolUseInputJson) {
    cc::services::api::CreateMessageRequest request;
    request.model = "claude-test";
    request.messages.push_back(cc::services::api::Message{
        .role = "assistant",
        .content = {
            cc::services::api::ContentBlock{
                .type = cc::services::api::ContentBlockType::Text,
                .text = "I will read a file."
            },
            cc::services::api::ContentBlock{
                .type = cc::services::api::ContentBlockType::ToolUse,
                .tool_use_id = "toolu_1",
                .tool_name = "Read",
                .tool_input_json = R"({"file_path":"README.md","limit":20})"
            }
        }
    });

    auto serialized = cc::services::api::RequestSerializer::serialize(request);
    auto parsed = cc::utils::json::parse(serialized);

    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
    auto content = parsed->root().get("messages").at(0).get("content");
    ASSERT_TRUE(content.is_arr());
    auto tool_use = content.at(1);
    EXPECT_EQ(tool_use.get("type").as_str(), "tool_use");
    EXPECT_EQ(tool_use.get("id").as_str(), "toolu_1");
    EXPECT_EQ(tool_use.get("name").as_str(), "Read");
    auto input = tool_use.get("input");
    ASSERT_TRUE(input.is_obj());
    EXPECT_EQ(input.get("file_path").as_str(), "README.md");
    EXPECT_EQ(input.get("limit").as_int(), 20);
}

TEST(ApiClient, RequestSerializerPreservesImageAndDocumentBlocks) {
    cc::services::api::CreateMessageRequest request;
    request.model = "claude-test";
    request.messages.push_back(cc::services::api::Message{
        .role = "user",
        .content = {
            cc::services::api::ContentBlock{
                .type = cc::services::api::ContentBlockType::Image,
                .media_type = "image/png",
                .image_data = "iVBORw0KGgo="
            },
            cc::services::api::ContentBlock{
                .type = cc::services::api::ContentBlockType::Document,
                .media_type = "application/pdf",
                .image_data = "JVBERi0xLjQ="
            }
        }
    });

    auto serialized = cc::services::api::RequestSerializer::serialize(request);
    auto parsed = cc::utils::json::parse(serialized);

    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
    auto content = parsed->root().get("messages").at(0).get("content");
    ASSERT_TRUE(content.is_arr());

    auto image = content.at(0);
    EXPECT_EQ(image.get("type").as_str(), "image");
    EXPECT_EQ(image.get("source").get("type").as_str(), "base64");
    EXPECT_EQ(image.get("source").get("media_type").as_str(), "image/png");
    EXPECT_EQ(image.get("source").get("data").as_str(), "iVBORw0KGgo=");

    auto document = content.at(1);
    EXPECT_EQ(document.get("type").as_str(), "document");
    EXPECT_EQ(document.get("source").get("type").as_str(), "base64");
    EXPECT_EQ(document.get("source").get("media_type").as_str(), "application/pdf");
    EXPECT_EQ(document.get("source").get("data").as_str(), "JVBERi0xLjQ=");
}

TEST(ApiClient, ResponseParserPreservesToolUseInputJson) {
    const auto response = cc::services::api::ResponseParser::parse(R"({
      "id": "msg_1",
      "model": "claude-test",
      "role": "assistant",
      "content": [
        {
          "type": "tool_use",
          "id": "toolu_1",
          "name": "Bash",
          "input": {"command": "pwd", "timeout": 1000}
        }
      ],
      "stop_reason": "tool_use",
      "usage": {"input_tokens": 1, "output_tokens": 2}
    })");

    ASSERT_TRUE(response.has_value()) << response.error().message();
    ASSERT_EQ(response->content.size(), 1u);
    const auto& block = response->content.front();
    EXPECT_EQ(block.type, cc::services::api::ContentBlockType::ToolUse);
    EXPECT_EQ(block.tool_use_id, "toolu_1");
    EXPECT_EQ(block.tool_name, "Bash");

    auto input = cc::utils::json::parse(block.tool_input_json);
    ASSERT_TRUE(input.has_value()) << input.error().message();
    EXPECT_EQ(input->root().get("command").as_str(), "pwd");
    EXPECT_EQ(input->root().get("timeout").as_int(), 1000);
}

TEST(ApiStreaming, SseBufferExtractsCompleteEvents) {
    cc::services::api::SseBuffer buffer;
    buffer.append("event: ping\ndata: {}\n\n");
    auto event = buffer.next_event();
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->first, "ping");
    EXPECT_EQ(event->second, "{}");
}

TEST(ApiStreaming, StreamParserAccumulatesTextDeltas) {
    cc::services::api::StreamParser parser;
    parser.start();
    parser.feed("event: content_block_delta\n");
    parser.feed("data: {\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n");

    auto event = parser.next_event();
    ASSERT_TRUE(event.has_value());
    ASSERT_TRUE(event->has_value());
    EXPECT_EQ((*event)->type, cc::services::api::StreamEventType::ContentBlockDelta);
    EXPECT_EQ(parser.full_text(), "hi");
    EXPECT_EQ(parser.statistics().total_events, 1);
}

TEST(McpElicitationHandler, UsesRegisteredResponderAndPolicy) {
    cc::services::mcp::clear_elicitation_policy();
    cc::services::mcp::clear_elicitation_responder();

    auto missing = cc::services::mcp::handle_elicitation(cc::services::mcp::ElicitationRequest{
        .server_name = "linear",
        .message = "Pick a workspace",
        .schema = {{"workspace", "string"}},
    });
    ASSERT_FALSE(missing.has_value());
    EXPECT_NE(missing.error().find("No MCP elicitation responder"), std::string::npos);

    std::optional<cc::services::mcp::ElicitationRequest> captured;
    cc::services::mcp::set_elicitation_responder([&](const cc::services::mcp::ElicitationRequest& request)
        -> std::expected<std::map<std::string, std::string>, std::string> {
        captured = request;
        return std::map<std::string, std::string>{{"workspace", "eng"}};
    });

    auto response = cc::services::mcp::handle_elicitation(cc::services::mcp::ElicitationRequest{
        .server_name = "linear",
        .message = "Pick a workspace",
        .schema = {{"workspace", "string"}},
    });
    ASSERT_TRUE(response.has_value()) << response.error();
    EXPECT_EQ(response->at("workspace"), "eng");
    ASSERT_TRUE(captured.has_value());
    EXPECT_EQ(captured->server_name, "linear");
    EXPECT_EQ(captured->message, "Pick a workspace");
    EXPECT_EQ(captured->schema.at("workspace"), "string");

    cc::services::mcp::set_elicitation_allowed("linear", false);
    auto denied = cc::services::mcp::handle_elicitation(cc::services::mcp::ElicitationRequest{
        .server_name = "linear",
        .message = "Pick a workspace",
        .schema = {},
    });
    ASSERT_FALSE(denied.has_value());
    EXPECT_NE(denied.error().find("not allowed"), std::string::npos);

    cc::services::mcp::clear_elicitation_policy();
    cc::services::mcp::clear_elicitation_responder();
}

TEST(McpConfigParser, ParsesJsonFieldsAndExplicitTransports) {
    const auto parsed = cc::services::mcp::ConfigParser::parse_json(R"JSON({
      "mcpServers": {
        "stdio_fixture": {
          "type": "stdio",
          "command": "node",
          "args": ["server.js", "--flag"],
          "env": {"FOO": "bar"},
          "timeout": 1234,
          "autoStart": false,
          "enabled": false
        },
	"sse_fixture": {
	  "type": "sse",
	  "url": "http://127.0.0.1:8123/events",
	  "headers": {"Authorization": "Bearer token"},
	  "headersHelper": "node helper.js",
	  "oauth": {
	    "authServerMetadataUrl": "https://auth.example.com/.well-known/oauth-authorization-server",
	    "callbackPort": 19485,
	    "clientId": "client-1",
	    "xaa": true
	  }
	},
        "http_fixture": {
          "type": "http",
          "url": "http://127.0.0.1:8124/mcp",
          "headers": {"X-Test": "present"},
          "disabled": true
        },
        "inferred_http": {
          "url": "http://127.0.0.1:8125/mcp"
        },
        "unsupported_ws": {
          "type": "ws",
          "url": "ws://127.0.0.1:8126/mcp"
        },
        "invalid_stdio": {
          "type": "stdio",
          "args": ["missing-command"]
        }
      }
    })JSON", cc::services::mcp::ConfigScope::Project);

    ASSERT_TRUE(parsed.has_value()) << static_cast<int>(parsed.error());
    ASSERT_EQ(parsed->size(), 4u);

    const auto& stdio = parsed->at("stdio_fixture");
    EXPECT_EQ(stdio.transport, cc::services::mcp::TransportType::Stdio);
    EXPECT_EQ(stdio.command, "node");
    ASSERT_EQ(stdio.args.size(), 2u);
    EXPECT_EQ(stdio.args[0], "server.js");
    EXPECT_EQ(stdio.args[1], "--flag");
    EXPECT_EQ(stdio.env.at("FOO"), "bar");
    EXPECT_EQ(stdio.timeout, std::chrono::milliseconds{1234});
    EXPECT_FALSE(stdio.auto_start);
    EXPECT_FALSE(stdio.enabled);
    EXPECT_EQ(stdio.scope, cc::services::mcp::ConfigScope::Project);

	const auto& sse = parsed->at("sse_fixture");
	EXPECT_EQ(sse.transport, cc::services::mcp::TransportType::Sse);
	EXPECT_EQ(sse.url, "http://127.0.0.1:8123/events");
	EXPECT_EQ(sse.headers.at("Authorization"), "Bearer token");
	EXPECT_EQ(sse.headers_helper, "node helper.js");
	ASSERT_TRUE(sse.oauth.has_value());
	ASSERT_TRUE(sse.oauth->auth_server_metadata_url.has_value());
	EXPECT_EQ(*sse.oauth->auth_server_metadata_url, "https://auth.example.com/.well-known/oauth-authorization-server");
	ASSERT_TRUE(sse.oauth->callback_port.has_value());
	EXPECT_EQ(*sse.oauth->callback_port, 19485);
	ASSERT_TRUE(sse.oauth->client_id.has_value());
	EXPECT_EQ(*sse.oauth->client_id, "client-1");
	EXPECT_TRUE(sse.oauth->xaa);

    const auto& http = parsed->at("http_fixture");
    EXPECT_EQ(http.transport, cc::services::mcp::TransportType::StreamableHttp);
    EXPECT_EQ(http.url, "http://127.0.0.1:8124/mcp");
    EXPECT_EQ(http.headers.at("X-Test"), "present");
    EXPECT_FALSE(http.enabled);

    const auto& inferred = parsed->at("inferred_http");
    EXPECT_EQ(inferred.transport, cc::services::mcp::TransportType::StreamableHttp);
    EXPECT_EQ(inferred.url, "http://127.0.0.1:8125/mcp");
    EXPECT_FALSE(parsed->contains("unsupported_ws"));
    EXPECT_FALSE(parsed->contains("invalid_stdio"));
}

TEST(McpConfigParser, SupportsServersAliasAndRejectsInvalidJson) {
    const auto parsed = cc::services::mcp::ConfigParser::parse_json(R"JSON({
      "servers": {
        "alias_fixture": {
          "transport": "streamable-http",
          "url": "http://127.0.0.1:8127/mcp"
        }
      }
    })JSON", cc::services::mcp::ConfigScope::User);

    ASSERT_TRUE(parsed.has_value()) << static_cast<int>(parsed.error());
    ASSERT_EQ(parsed->size(), 1u);
    const auto& alias = parsed->at("alias_fixture");
    EXPECT_EQ(alias.transport, cc::services::mcp::TransportType::StreamableHttp);
    EXPECT_EQ(alias.scope, cc::services::mcp::ConfigScope::User);

    const auto invalid = cc::services::mcp::ConfigParser::parse_json(
        "{",
        cc::services::mcp::ConfigScope::User
    );
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error(), cc::services::mcp::ConfigError::ParseError);
}

TEST(McpHeadersHelper, ParsesAndMergesDynamicHeaders) {
    const auto parsed = cc::services::mcp::parse_header_helper_json(R"JSON({
      "Authorization": "Bearer dynamic",
      "X-Helper": "present"
    })JSON");

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->at("Authorization"), "Bearer dynamic");
    EXPECT_EQ(parsed->at("X-Helper"), "present");

    const auto invalid = cc::services::mcp::parse_header_helper_json(R"JSON({
      "X-Bad": 7
    })JSON");
    EXPECT_FALSE(invalid.has_value());

    const auto merged = cc::services::mcp::get_mcp_server_headers(
        "server",
        {
            {"Authorization", "Bearer static"},
            {"X-Keep", "static"},
        },
        "http://127.0.0.1:8123/mcp",
        ""
    );
    EXPECT_EQ(merged.at("Authorization"), "Bearer static");
    EXPECT_EQ(merged.at("X-Keep"), "static");
}

TEST(McpTypes, JsonRpcSerializationIncludesParams) {
    auto request = cc::services::mcp::make_request(
        int64_t{7},
        "tools/call",
        std::optional<std::string>{R"({"name":"echo","arguments":{"value":"hello"}})"});

    const auto serialized = cc::services::mcp::serialize_request(request);

    EXPECT_NE(serialized.find(R"("method":"tools/call")"), std::string::npos);
    EXPECT_NE(serialized.find(R"("params":{"name":"echo","arguments":{"value":"hello"}})"), std::string::npos);

    auto notification = cc::services::mcp::make_notification(
        "notifications/initialized",
        std::optional<std::string>{R"({"ready":true})"});

    const auto serialized_notification = cc::services::mcp::serialize_notification(notification);
    EXPECT_NE(serialized_notification.find(R"("params":{"ready":true})"), std::string::npos);
}

TEST(McpClient, SendsSseRequestsViaDiscoveredPostEndpoint) {
    LocalSseMcpServer server;
    ASSERT_TRUE(server.ready());

    cc::services::mcp::McpClient::Config config;
    config.name = "sse-fixture";
    config.request_timeout = std::chrono::milliseconds{2000};
    config.init_timeout = std::chrono::milliseconds{2000};

    cc::services::mcp::McpClient client(std::move(config));
    auto connected = client.connect_sse(server.url(), {{"X-Test-Header", "present"}});
    ASSERT_TRUE(connected.has_value());

    auto tools = client.list_tools();
    ASSERT_TRUE(tools.has_value());
    ASSERT_EQ(tools->tools.size(), 1u);
    EXPECT_EQ(tools->tools.front().name, "sse_lookup");
    EXPECT_EQ(tools->tools.front().description, "Lookup through SSE");

    const auto posts = server.post_bodies();
    std::string joined;
    for (const auto& body : posts) joined += body + "\n";
    EXPECT_NE(joined.find(R"("method":"initialize")"), std::string::npos) << joined;
    EXPECT_NE(joined.find(R"("method":"notifications/initialized")"), std::string::npos) << joined;
    EXPECT_NE(joined.find(R"("method":"tools/list")"), std::string::npos) << joined;

    client.shutdown();
}

TEST(IdeIntegration, ReadsLockfileAndCallsIdeMcpTool) {
    LocalSseMcpServer server;
    ASSERT_TRUE(server.ready());

    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    const auto temp_home = fs::temp_directory_path() / ("cc_repl_ide_home_" + std::to_string(suffix));
    const auto ide_dir = temp_home / ".claude" / "ide";
    fs::create_directories(ide_dir);
    EnvironmentGuard home("HOME", temp_home.string());

    const auto lockfile_path = ide_dir / (std::to_string(server.port()) + ".lock");
    {
        std::ofstream lockfile(lockfile_path);
        lockfile << std::format(
            R"({{"workspaceFolders":["{}"],"pid":{},"ideName":"VS Code","transport":"sse"}})",
            fs::current_path().string(),
            static_cast<int>(::getpid()));
    }

    cc::utils::ide::IdeLockfileScanner scanner;
    auto lockfiles = scanner.scan();
    ASSERT_EQ(lockfiles.size(), 1u);
    EXPECT_EQ(lockfiles.front().port, server.port());
    EXPECT_EQ(lockfiles.front().name, "VS Code");
    ASSERT_EQ(lockfiles.front().workspace_folders.size(), 1u);
    EXPECT_EQ(lockfiles.front().workspace_folders.front(), fs::current_path());

    auto response = cc::utils::ide::callIdeRpc(
        "openFile",
        R"({"filePath":"/tmp/example.ts","preview":false})");
    EXPECT_TRUE(response.success) << response.error.value_or(response.result);
    EXPECT_NE(response.result.find("called:openFile"), std::string::npos) << response.result;

    const auto posts = server.post_bodies();
    std::string joined;
    for (const auto& body : posts) joined += body + "\n";
    EXPECT_NE(joined.find(R"("method":"initialize")"), std::string::npos) << joined;
    EXPECT_NE(joined.find(R"("method":"tools/call")"), std::string::npos) << joined;
    EXPECT_NE(joined.find(R"("name":"openFile")"), std::string::npos) << joined;

    fs::remove_all(temp_home);
}

TEST(IdeIntegration, CallsIdeWebSocketMcpToolFromLockfile) {
    LocalWebSocketMcpServer server;
    ASSERT_TRUE(server.ready());

    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    const auto temp_home = fs::temp_directory_path() / ("cc_repl_ide_ws_home_" + std::to_string(suffix));
    const auto ide_dir = temp_home / ".claude" / "ide";
    fs::create_directories(ide_dir);
    EnvironmentGuard home("HOME", temp_home.string());

    const auto lockfile_path = ide_dir / (std::to_string(server.port()) + ".lock");
    {
        std::ofstream lockfile(lockfile_path);
        lockfile << std::format(
            R"({{"workspaceFolders":["{}"],"pid":{},"ideName":"Cursor","transport":"ws","authToken":"test-token"}})",
            fs::current_path().string(),
            static_cast<int>(::getpid()));
    }

    auto response = cc::utils::ide::callIdeRpc(
        "openFile",
        R"({"filePath":"/tmp/example.ts","preview":false})");
    EXPECT_TRUE(response.success) << response.error.value_or(response.result);
    EXPECT_NE(response.result.find("called:openFile"), std::string::npos) << response.result;
    EXPECT_TRUE(server.wait_for_tool_call());

    const auto headers = server.handshake_headers();
    EXPECT_NE(headers.find("Sec-WebSocket-Protocol: mcp"), std::string::npos) << headers;
    EXPECT_NE(headers.find("X-Claude-Code-Ide-Authorization: test-token"), std::string::npos) << headers;

    const auto requests = server.requests();
    std::string joined;
    for (const auto& request : requests) joined += request + "\n";
    EXPECT_NE(joined.find(R"("method":"initialize")"), std::string::npos) << joined;
    EXPECT_NE(joined.find(R"("method":"notifications/initialized")"), std::string::npos) << joined;
    EXPECT_NE(joined.find(R"("method":"tools/call")"), std::string::npos) << joined;
    EXPECT_NE(joined.find(R"("name":"openFile")"), std::string::npos) << joined;

    fs::remove_all(temp_home);
}

TEST(McpClient, MapsSseUnauthorizedToUnauthorizedError) {
    LocalUnauthorizedStreamableHttpMcpServer server;
    ASSERT_TRUE(server.ready());

    cc::services::mcp::McpClient::Config config;
    config.name = "sse-auth-fixture";
    config.request_timeout = std::chrono::milliseconds{500};
    config.init_timeout = std::chrono::milliseconds{500};

    cc::services::mcp::McpClient client(std::move(config));
    auto connected = client.connect_sse(server.url());
    ASSERT_FALSE(connected.has_value());
    EXPECT_EQ(connected.error(), cc::services::mcp::McpClientError::Unauthorized);

    const auto requests = server.requests();
    ASSERT_FALSE(requests.empty());
    EXPECT_NE(requests.front().find("GET /mcp HTTP/1.1"), std::string::npos) << requests.front();

    client.shutdown();
}

TEST(McpConnectionManager, ConnectsStreamableHttpServerWithDirectPostTransport) {
    LocalStreamableHttpMcpServer server;
    ASSERT_TRUE(server.ready());

    cc::services::mcp::ConnectionManagerConfig manager_config;
    manager_config.config_directory = fs::temp_directory_path() / "cc_repl_streamable_http_mcp_config";
    manager_config.connection_timeout = std::chrono::milliseconds{2000};
    manager_config.auto_connect_on_start = false;

    cc::services::mcp::McpConnectionManager manager(std::move(manager_config));

    cc::services::mcp::ServerConfig server_config;
    server_config.name = "http-fixture";
    server_config.transport = cc::services::mcp::TransportType::StreamableHttp;
    server_config.url = server.url();
    server_config.headers = {{"X-Test-Header", "present"}};
    server_config.enabled = true;
    server_config.auto_start = true;

    cc::services::mcp::McpConfig mcp_config;
    mcp_config.servers.emplace(server_config.name, std::move(server_config));
    manager.set_configuration(std::move(mcp_config));

    auto connected = manager.connect_server("http-fixture");
    ASSERT_TRUE(connected.has_value()) << static_cast<int>(connected.error());

    auto snapshot = manager.snapshot_server("http-fixture");
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->status, cc::services::mcp::ConnectionStatus::Connected);
    ASSERT_EQ(snapshot->tools.size(), 1u);
    EXPECT_EQ(snapshot->tools.front().name, "http_lookup");
    EXPECT_EQ(snapshot->tools.front().description, "Lookup through HTTP");

    const auto posts = server.post_bodies();
    std::string joined_bodies;
    for (const auto& body : posts) joined_bodies += body + "\n";
    EXPECT_NE(joined_bodies.find(R"("method":"initialize")"), std::string::npos) << joined_bodies;
    EXPECT_NE(joined_bodies.find(R"("method":"notifications/initialized")"), std::string::npos) << joined_bodies;
    EXPECT_NE(joined_bodies.find(R"("method":"tools/list")"), std::string::npos) << joined_bodies;

    const auto requests = server.requests();
    std::string joined_requests;
    for (const auto& request : requests) joined_requests += request + "\n";
    EXPECT_NE(joined_requests.find("POST /mcp HTTP/1.1"), std::string::npos) << joined_requests;
    EXPECT_NE(joined_requests.find("Accept: application/json, text/event-stream"), std::string::npos) << joined_requests;
    EXPECT_NE(joined_requests.find("X-Test-Header: present"), std::string::npos) << joined_requests;

    manager.shutdown();
}

TEST(McpConnectionManager, MarksRemoteHttpUnauthorizedAsNeedsAuth) {
    LocalUnauthorizedStreamableHttpMcpServer server;
    ASSERT_TRUE(server.ready());

    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    cc::services::mcp::ConnectionManagerConfig manager_config;
    manager_config.config_directory = fs::temp_directory_path() / ("cc_repl_mcp_unauthorized_" + std::to_string(suffix));
    manager_config.connection_timeout = std::chrono::milliseconds{2000};
    manager_config.auto_connect_on_start = false;

    cc::services::mcp::McpConnectionManager manager(std::move(manager_config));

    cc::services::mcp::ServerConfig server_config;
    server_config.name = "auth-fixture";
    server_config.transport = cc::services::mcp::TransportType::StreamableHttp;
    server_config.url = server.url();
    server_config.enabled = true;
    server_config.auto_start = true;

    cc::services::mcp::McpConfig mcp_config;
    mcp_config.servers.emplace(server_config.name, std::move(server_config));
    manager.set_configuration(std::move(mcp_config));

    auto connected = manager.connect_server("auth-fixture");
    ASSERT_FALSE(connected.has_value());
    EXPECT_EQ(connected.error(), cc::services::mcp::McpClientError::Unauthorized);

    auto snapshot = manager.snapshot_server("auth-fixture");
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->status, cc::services::mcp::ConnectionStatus::NeedsAuth);
    ASSERT_TRUE(snapshot->last_error.has_value());
    EXPECT_EQ(*snapshot->last_error, "authentication required");

    const auto requests = server.requests();
    ASSERT_FALSE(requests.empty());
    EXPECT_NE(requests.front().find(R"("method":"initialize")"), std::string::npos) << requests.front();

    manager.shutdown();
}

TEST(McpConnectionManager, AppliesHeadersHelperBeforeRemoteConnection) {
    LocalStreamableHttpMcpServer server;
    ASSERT_TRUE(server.ready());

    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("cc_repl_mcp_headers_helper_" + std::to_string(suffix));
    fs::create_directories(root);
    const auto helper_path = root / "headers-helper.sh";
    {
        std::ofstream helper(helper_path);
        helper << R"SH(
printf '{"X-Test-Header":"dynamic","X-Helper-Server":"%s","X-Helper-Url":"%s"}\n' "$CLAUDE_CODE_MCP_SERVER_NAME" "$CLAUDE_CODE_MCP_SERVER_URL"
)SH";
    }

    cc::services::mcp::ConnectionManagerConfig manager_config;
    manager_config.config_directory = root;
    manager_config.connection_timeout = std::chrono::milliseconds{2000};
    manager_config.auto_connect_on_start = false;

    cc::services::mcp::McpConnectionManager manager(std::move(manager_config));

    cc::services::mcp::ServerConfig server_config;
    server_config.name = "helper-fixture";
    server_config.transport = cc::services::mcp::TransportType::StreamableHttp;
    server_config.url = server.url();
    server_config.headers = {
        {"X-Test-Header", "static"},
        {"X-Static", "present"},
    };
    server_config.headers_helper = "sh '" + helper_path.string() + "'";

    cc::services::mcp::McpConfig mcp_config;
    mcp_config.servers.emplace(server_config.name, std::move(server_config));
    manager.set_configuration(std::move(mcp_config));

    auto connected = manager.connect_server("helper-fixture");
    ASSERT_TRUE(connected.has_value()) << static_cast<int>(connected.error());

    const auto requests = server.requests();
    std::string joined_requests;
    for (const auto& request : requests) joined_requests += request + "\n";
    EXPECT_NE(joined_requests.find("X-Test-Header: dynamic"), std::string::npos) << joined_requests;
    EXPECT_EQ(joined_requests.find("X-Test-Header: static"), std::string::npos) << joined_requests;
    EXPECT_NE(joined_requests.find("X-Static: present"), std::string::npos) << joined_requests;
    EXPECT_NE(joined_requests.find("X-Helper-Server: helper-fixture"), std::string::npos) << joined_requests;
    EXPECT_NE(joined_requests.find("X-Helper-Url: " + server.url()), std::string::npos) << joined_requests;

    manager.shutdown();
    fs::remove_all(root);
}

TEST(McpConnectionManager, RefreshesCachedListsAfterListChangedNotifications) {
    auto root = fs::temp_directory_path() / "cc_repl_mcp_list_changed_test";
    fs::remove_all(root);
    fs::create_directories(root);
    const auto server_path = root / "server.js";
    {
        std::ofstream server(server_path);
        server << R"JS(
const readline = require('node:readline');
const rl = readline.createInterface({ input: process.stdin });

let toolListCount = 0;
let resourceListCount = 0;
let promptListCount = 0;
let notificationsSent = false;

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\n');
}

function maybeSendListChangedNotifications() {
  if (notificationsSent || promptListCount === 0) return;
  notificationsSent = true;
  setTimeout(() => {
    send({ jsonrpc: '2.0', method: 'notifications/tools/list_changed', params: {} });
    send({ jsonrpc: '2.0', method: 'notifications/resources/list_changed', params: {} });
    send({ jsonrpc: '2.0', method: 'notifications/prompts/list_changed', params: {} });
  }, 25);
}

rl.on('line', line => {
  const message = JSON.parse(line);
  if (message.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: message.id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: {
          tools: { listChanged: true },
          resources: { listChanged: true },
          prompts: { listChanged: true }
        },
        serverInfo: { name: 'list-changed-fixture', version: '1.0.0' }
      }
    });
    return;
  }
  if (message.method === 'tools/list') {
    toolListCount += 1;
    const suffix = toolListCount === 1 ? 'initial' : 'updated';
    send({
      jsonrpc: '2.0',
      id: message.id,
      result: { tools: [{ name: `${suffix}_tool`, description: `${suffix} tool`, inputSchema: { type: 'object' } }] }
    });
    maybeSendListChangedNotifications();
    return;
  }
  if (message.method === 'resources/list') {
    resourceListCount += 1;
    const suffix = resourceListCount === 1 ? 'initial' : 'updated';
    send({
      jsonrpc: '2.0',
      id: message.id,
      result: { resources: [{ uri: `file:///${suffix}.txt`, name: `${suffix} resource`, description: `${suffix} resource`, mimeType: 'text/plain' }] }
    });
    maybeSendListChangedNotifications();
    return;
  }
  if (message.method === 'prompts/list') {
    promptListCount += 1;
    const suffix = promptListCount === 1 ? 'initial' : 'updated';
    send({
      jsonrpc: '2.0',
      id: message.id,
      result: { prompts: [{ name: `${suffix}_prompt`, description: `${suffix} prompt`, arguments: [] }] }
    });
    maybeSendListChangedNotifications();
  }
});
)JS";
    }

    cc::services::mcp::ConnectionManagerConfig manager_config;
    manager_config.config_directory = root;
    manager_config.connection_timeout = std::chrono::milliseconds{2000};
    manager_config.auto_connect_on_start = false;

    cc::services::mcp::McpConnectionManager manager(std::move(manager_config));

    cc::services::mcp::ServerConfig server_config;
    server_config.name = "list-changed-fixture";
    server_config.transport = cc::services::mcp::TransportType::Stdio;
    server_config.command = "node";
    server_config.args = {server_path.string()};

    cc::services::mcp::McpConfig mcp_config;
    mcp_config.servers.emplace(server_config.name, std::move(server_config));
    manager.set_configuration(std::move(mcp_config));

    auto connected = manager.connect_server("list-changed-fixture");
    ASSERT_TRUE(connected.has_value()) << static_cast<int>(connected.error());

    auto initial = manager.snapshot_server("list-changed-fixture");
    ASSERT_TRUE(initial.has_value());
    ASSERT_EQ(initial->tools.size(), 1u);
    ASSERT_EQ(initial->resources.size(), 1u);
    ASSERT_EQ(initial->prompts.size(), 1u);
    EXPECT_EQ(initial->tools.front().name, "initial_tool");
    EXPECT_EQ(initial->resources.front().uri, "file:///initial.txt");
    EXPECT_EQ(initial->prompts.front().name, "initial_prompt");

    bool refreshed = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto snapshot = manager.snapshot_server("list-changed-fixture");
        if (snapshot &&
            snapshot->tools.size() == 1 &&
            snapshot->resources.size() == 1 &&
            snapshot->prompts.size() == 1 &&
            snapshot->tools.front().name == "updated_tool" &&
            snapshot->resources.front().uri == "file:///updated.txt" &&
            snapshot->prompts.front().name == "updated_prompt") {
            refreshed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    EXPECT_TRUE(refreshed);

    manager.shutdown();
    fs::remove_all(root);
}

TEST(McpClient, HandlesServerRootsRequestsAndNotificationParams) {
    auto root = fs::temp_directory_path() / "cc_repl_mcp_client_requests_test";
    fs::remove_all(root);
    fs::create_directories(root);
    const auto server_path = root / "server.js";
    const auto log_path = root / "mcp-log.jsonl";
    {
        std::ofstream server(server_path);
        server << R"JS(
const fs = require('node:fs');
const readline = require('node:readline');
const rl = readline.createInterface({ input: process.stdin });
const logPath = process.env.MCP_LOG;

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\n');
}

function log(message) {
  fs.appendFileSync(logPath, `${JSON.stringify(message)}\n`);
}

rl.on('line', line => {
  const message = JSON.parse(line);
  if (message.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: message.id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: { tools: {} },
        serverInfo: { name: 'roots-fixture', version: '1.0.0' }
      }
    });
    return;
  }
  if (message.method === 'notifications/initialized') {
    send({ jsonrpc: '2.0', id: 'roots-1', method: 'roots/list', params: {} });
    send({
      jsonrpc: '2.0',
      method: 'notifications/progress',
      params: { progressToken: 'tok', progress: 0.5 }
    });
    return;
  }
  if (message.id === 'roots-1') {
    log(message);
    return;
  }
  if (message.method === 'tools/list') {
    send({
      jsonrpc: '2.0',
      id: message.id,
      result: { tools: [{ name: 'noop', description: 'Noop', inputSchema: { type: 'object' } }] }
    });
  }
});
)JS";
    }

    cc::services::mcp::McpClient::Config config;
    config.name = "roots-fixture";
    config.request_timeout = std::chrono::milliseconds{2000};
    config.init_timeout = std::chrono::milliseconds{2000};

    cc::services::mcp::McpClient client(std::move(config));
    client.set_roots_handler([] {
        return std::vector<cc::services::mcp::Root>{
            cc::services::mcp::Root{
                .uri = "file:///workspace",
                .name = std::string{"workspace"},
            },
        };
    });

    std::mutex notification_mutex;
    std::optional<cc::services::mcp::JsonRpcNotification> notification;
    client.set_notification_callback([&](const cc::services::mcp::JsonRpcNotification& value) {
        std::lock_guard lock(notification_mutex);
        notification = value;
    });

    auto connected = client.connect_stdio(
        "node",
        {server_path.string()},
        std::map<std::string, std::string>{{"MCP_LOG", log_path.string()}});
    ASSERT_TRUE(connected.has_value());

    auto tools = client.list_tools();
    ASSERT_TRUE(tools.has_value());
    ASSERT_EQ(tools->tools.size(), 1u);
    EXPECT_EQ(tools->tools.front().name, "noop");

    std::string log;
    for (int attempt = 0; attempt < 50; ++attempt) {
        {
            std::ifstream input(log_path);
            if (input) {
                std::stringstream buffer;
                buffer << input.rdbuf();
                log = buffer.str();
            }
        }
        bool has_notification = false;
        {
            std::lock_guard lock(notification_mutex);
            has_notification = notification.has_value();
        }
        if (log.find("\"id\":\"roots-1\"") != std::string::npos && has_notification) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }

    EXPECT_NE(log.find("\"id\":\"roots-1\""), std::string::npos) << log;
    EXPECT_NE(log.find("\"roots\":[{\"uri\":\"file:///workspace\",\"name\":\"workspace\"}]"), std::string::npos) << log;

    std::optional<cc::services::mcp::JsonRpcNotification> captured;
    {
        std::lock_guard lock(notification_mutex);
        captured = notification;
    }
    ASSERT_TRUE(captured.has_value());
    EXPECT_EQ(captured->method, "notifications/progress");
    ASSERT_TRUE(captured->params_json.has_value());
    auto params = cc::utils::json::parse(*captured->params_json);
    ASSERT_TRUE(params.has_value()) << params.error().message();
    EXPECT_EQ(params->root().get("progressToken").as_str(), "tok");
    EXPECT_EQ(params->root().get("progress").as_double(), 0.5);
    EXPECT_FALSE(params->root().has("jsonrpc"));
    EXPECT_FALSE(params->root().has("method"));

    client.shutdown();
    fs::remove_all(root);
}

TEST(McpClient, ParsesPromptMessageContentObjects) {
    auto root = fs::temp_directory_path() / "cc_repl_mcp_prompt_content_test";
    fs::remove_all(root);
    fs::create_directories(root);
    const auto server_path = root / "server.js";
    {
        std::ofstream server(server_path);
        server << R"JS(
const readline = require('node:readline');
const rl = readline.createInterface({ input: process.stdin });

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\n');
}

rl.on('line', line => {
  const message = JSON.parse(line);
  if (message.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: message.id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: { prompts: {} },
        serverInfo: { name: 'prompt-fixture', version: '1.0.0' }
      }
    });
    return;
  }
  if (message.method === 'prompts/list') {
    send({
      jsonrpc: '2.0',
      id: message.id,
      result: {
        prompts: [{
          name: 'review',
          description: 'Review a topic',
          arguments: [{ name: 'topic', description: 'Topic to review', required: true }]
        }]
      }
    });
    return;
  }
  if (message.method === 'prompts/get') {
    send({
      jsonrpc: '2.0',
      id: message.id,
      result: {
        description: 'Prompt with rich content',
        messages: [
          {
            role: 'user',
            content: { type: 'text', text: 'Review ' + message.params.arguments.topic }
          },
          {
            role: 'assistant',
            content: {
              type: 'resource',
              resource: { uri: 'file:///notes.md', mimeType: 'text/markdown', text: 'notes body' }
            }
          },
          {
            role: 'user',
            content: {
              type: 'resource_link',
              name: 'notes',
              uri: 'file:///notes.md',
              description: 'Reference notes'
            }
          },
          {
            role: 'assistant',
            content: { type: 'image', mimeType: 'image/png', data: 'iVBORw0KGgo=' }
          }
        ]
      }
    });
  }
});
)JS";
    }

    cc::services::mcp::McpClient::Config config;
    config.name = "prompt-fixture";
    config.request_timeout = std::chrono::milliseconds{2000};
    config.init_timeout = std::chrono::milliseconds{2000};

    cc::services::mcp::McpClient client(std::move(config));
    auto connected = client.connect_stdio("node", {server_path.string()}, {});
    ASSERT_TRUE(connected.has_value());

    auto prompts = client.list_prompts();
    ASSERT_TRUE(prompts.has_value());
    ASSERT_EQ(prompts->prompts.size(), 1u);
    EXPECT_EQ(prompts->prompts.front().name, "review");
    ASSERT_EQ(prompts->prompts.front().arguments.size(), 1u);
    EXPECT_EQ(prompts->prompts.front().arguments.front().name, "topic");
    EXPECT_TRUE(prompts->prompts.front().arguments.front().required);

    auto prompt = client.get_prompt("review", {{"topic", "migration"}});
    ASSERT_TRUE(prompt.has_value());
    ASSERT_EQ(prompt->messages.size(), 4u);
    EXPECT_EQ(prompt->messages[0].role, cc::services::mcp::PromptRole::User);
    EXPECT_EQ(prompt->messages[0].content, "Review migration");
    EXPECT_EQ(prompt->messages[1].role, cc::services::mcp::PromptRole::Assistant);
    EXPECT_EQ(prompt->messages[1].content, "[Resource from prompt-fixture at file:///notes.md] notes body");
    EXPECT_EQ(prompt->messages[2].content, "[Resource link: notes] file:///notes.md (Reference notes)");
    EXPECT_NE(prompt->messages[3].content.find("[Image from prompt-fixture] Binary content (image/png"), std::string::npos);

    client.shutdown();
    fs::remove_all(root);
}

TEST(ConfigManager, PersistsMcpServerSettings) {
    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("cc_repl_config_test_" + std::to_string(suffix));
    fs::create_directories(root);

    cc::core::ConfigManager manager(root / "global.json", root / "project.json");
    auto& settings = manager.settings_mut();
    settings.mcp_servers.push_back(cc::core::McpServerConfig{
        .name = "echo",
        .command = "node",
        .args = {"server.js", "--flag"},
        .env = {{"FOO", "bar"}},
    });

    ASSERT_TRUE(manager.save(cc::core::ConfigSource::ProjectConfig).has_value());

    cc::core::ConfigManager loaded(root / "global.json", root / "project.json");
    ASSERT_TRUE(loaded.load().has_value());
    ASSERT_EQ(loaded.settings().mcp_servers.size(), 1u);
    EXPECT_EQ(loaded.settings().mcp_servers.front().name, "echo");
    EXPECT_EQ(loaded.settings().mcp_servers.front().command, "node");
    ASSERT_EQ(loaded.settings().mcp_servers.front().args.size(), 2u);
    EXPECT_EQ(loaded.settings().mcp_servers.front().args[0], "server.js");
    EXPECT_EQ(loaded.settings().mcp_servers.front().args[1], "--flag");
	EXPECT_EQ(loaded.settings().mcp_servers.front().env.at("FOO"), "bar");

	fs::remove_all(root);
}

TEST(ConfigManager, PreservesRemoteMcpServerAuthSettings) {
    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("cc_repl_remote_mcp_config_test_" + std::to_string(suffix));
    fs::create_directories(root);

    cc::core::ConfigManager manager(root / "global.json", root / "project.json");
    auto& settings = manager.settings_mut();
    settings.mcp_servers.push_back(cc::core::McpServerConfig{
        .name = "remote",
        .command = {},
        .args = {},
        .env = {},
        .transport = "http",
        .url = "https://mcp.example.com/mcp",
        .headers = {{"X-Test", "present"}},
        .headers_helper = "node headers.js",
        .oauth = cc::core::McpOAuthConfig{
            .auth_server_metadata_url = "https://auth.example.com/.well-known/oauth-authorization-server",
            .callback_port = 19485,
            .client_id = "client-1",
            .xaa = true,
        },
    });

    ASSERT_TRUE(manager.save(cc::core::ConfigSource::ProjectConfig).has_value());

    cc::core::ConfigManager loaded(root / "global.json", root / "project.json");
    ASSERT_TRUE(loaded.load().has_value());
    ASSERT_EQ(loaded.settings().mcp_servers.size(), 1u);
    const auto& server = loaded.settings().mcp_servers.front();
    EXPECT_EQ(server.name, "remote");
    EXPECT_EQ(server.transport, "http");
    ASSERT_TRUE(server.url.has_value());
    EXPECT_EQ(*server.url, "https://mcp.example.com/mcp");
    EXPECT_EQ(server.headers.at("X-Test"), "present");
    ASSERT_TRUE(server.headers_helper.has_value());
    EXPECT_EQ(*server.headers_helper, "node headers.js");
    ASSERT_TRUE(server.oauth.has_value());
    ASSERT_TRUE(server.oauth->auth_server_metadata_url.has_value());
    EXPECT_EQ(*server.oauth->auth_server_metadata_url, "https://auth.example.com/.well-known/oauth-authorization-server");
    ASSERT_TRUE(server.oauth->callback_port.has_value());
    EXPECT_EQ(*server.oauth->callback_port, 19485);
    ASSERT_TRUE(server.oauth->client_id.has_value());
    EXPECT_EQ(*server.oauth->client_id, "client-1");
    EXPECT_TRUE(server.oauth->xaa);

    fs::remove_all(root);
}

TEST(ServerRoutes, MessageSessionsAndCompactUsePersistentState) {
    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("cc_repl_server_routes_test_" + std::to_string(suffix));
    const auto sessions_dir = root / "sessions";
    fs::remove_all(root);
    fs::create_directories(sessions_dir);

    LocalAnthropicMessagesServer server;
    ASSERT_NE(server.port(), 0);
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", server.base_url());
    EnvironmentGuard model_guard("CLAUDE_MODEL", "direct-route-test-model");
    CurrentPathGuard cwd_guard(root);

    cc::server::reset_route_state_for_testing();
    cc::server::set_sessions_dir_for_testing(sessions_dir);
    auto routes = cc::server::get_default_routes();
    auto find_route = [&](std::string_view method, std::string_view path) -> const cc::server::Route* {
        auto it = std::ranges::find_if(routes, [&](const auto& route) {
            return route.method == method && route.path == path;
        });
        return it == routes.end() ? nullptr : &*it;
    };

    const auto* message_route = find_route("POST", "/message");
    const auto* sessions_route = find_route("GET", "/sessions");
    const auto* compact_route = find_route("POST", "/compact");
    ASSERT_NE(message_route, nullptr);
    ASSERT_NE(sessions_route, nullptr);
    ASSERT_NE(compact_route, nullptr);

    auto first_response = message_route->handler({{"content", "hello server route"}, {"model", "test-model"}});
    auto first_json = cc::utils::json::parse(first_response);
    ASSERT_TRUE(first_json.has_value()) << first_response;
    auto session_id_value = first_json->root().get("session_id");
    ASSERT_TRUE(session_id_value.is_str());
    std::string session_id(session_id_value.as_str());
    EXPECT_EQ(first_json->root().get_string("status"), "completed");
    EXPECT_EQ(first_json->root().get_string("response"), "ok");
    EXPECT_EQ(first_json->root().get_string("model"), "claude-test");

    for (int i = 0; i < 4; ++i) {
        auto response = message_route->handler({
            {"session_id", session_id},
            {"content", "follow up " + std::to_string(i)}
        });
        auto parsed = cc::utils::json::parse(response);
        ASSERT_TRUE(parsed.has_value()) << response;
        EXPECT_EQ(parsed->root().get_string("session_id"), session_id);
        EXPECT_EQ(parsed->root().get_string("response"), "ok");
    }

    auto request_bodies = server.wait_for_bodies(5);
    ASSERT_TRUE(request_bodies.has_value());
    auto last_request_json = cc::utils::json::parse(request_bodies->back());
    ASSERT_TRUE(last_request_json.has_value()) << request_bodies->back();
    auto request_messages = last_request_json->root().get("messages");
    ASSERT_TRUE(request_messages.is_arr()) << request_bodies->back();
    ASSERT_EQ(request_messages.size(), 9u) << request_bodies->back();
    std::string request_history_text;
    request_messages.iter([&](cc::utils::json::JsonVal message) {
        auto content = message.get("content");
        if (content.is_str()) {
            request_history_text += std::string(content.as_str()) + "\n";
        } else if (content.is_arr()) {
            content.iter([&](cc::utils::json::JsonVal block) {
                auto text = block.get("text");
                if (text.valid() && text.is_str()) {
                    request_history_text += std::string(text.as_str()) + "\n";
                }
            });
        }
    });
    EXPECT_NE(request_history_text.find("hello server route"), std::string::npos);
    EXPECT_NE(request_history_text.find("follow up 0"), std::string::npos);
    EXPECT_NE(request_history_text.find("follow up 3"), std::string::npos);
    EXPECT_NE(request_history_text.find("ok"), std::string::npos);

    auto sessions_response = sessions_route->handler({{"limit", "5"}});
    auto sessions_json = cc::utils::json::parse(sessions_response);
    ASSERT_TRUE(sessions_json.has_value()) << sessions_response;
    EXPECT_EQ(sessions_json->root().get("total").as_int(), 1);
    auto sessions = sessions_json->root().get("sessions");
    ASSERT_TRUE(sessions.is_arr());
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions.at(0).get_string("session_id"), session_id);
    EXPECT_EQ(sessions.at(0).get("message_count").as_int(), 10);

    auto compact_response = compact_route->handler({{"session_id", session_id}});
    auto compact_json = cc::utils::json::parse(compact_response);
    ASSERT_TRUE(compact_json.has_value()) << compact_response;
    EXPECT_EQ(compact_json->root().get_string("status"), "compacted");
    EXPECT_EQ(compact_json->root().get("messages_before").as_int(), 10);
    EXPECT_EQ(compact_json->root().get("messages_after").as_int(), 7);
    EXPECT_EQ(compact_json->root().get("messages_removed").as_int(), 3);
    EXPECT_EQ(compact_json->root().get("messages_summarized").as_int(), 4);
    ASSERT_TRUE(compact_json->root().get("compact_boundary_id").is_str());

    auto metadata = cc::session::load_session_metadata(sessions_dir, session_id);
    ASSERT_TRUE(metadata.has_value());
    EXPECT_EQ(metadata->message_count, 7);

    std::ifstream messages_file(cc::session::get_messages_path(sessions_dir, session_id));
    ASSERT_TRUE(messages_file.is_open());
    std::vector<std::string> compacted_lines;
    std::string line;
    while (std::getline(messages_file, line)) {
        if (!line.empty()) compacted_lines.push_back(line);
    }
    ASSERT_EQ(compacted_lines.size(), 7u);
    auto boundary_json = cc::utils::json::parse(compacted_lines.front());
    ASSERT_TRUE(boundary_json.has_value()) << compacted_lines.front();
    EXPECT_EQ(boundary_json->root().get_string("role"), "system");
    EXPECT_EQ(boundary_json->root().get_string("subtype"), "compact_boundary");
    EXPECT_EQ(
        boundary_json->root().get_string("id"),
        compact_json->root().get_string("compact_boundary_id"));
    auto boundary_metadata = boundary_json->root().get("compact_metadata");
    ASSERT_TRUE(boundary_metadata.is_obj());
    EXPECT_EQ(boundary_metadata.get("messages_before").as_int(), 10);
    EXPECT_EQ(boundary_metadata.get("messages_after").as_int(), 7);
    EXPECT_EQ(boundary_metadata.get("messages_removed").as_int(), 3);
    EXPECT_EQ(boundary_metadata.get("messages_summarized").as_int(), 4);
    EXPECT_EQ(boundary_metadata.get("preserved_recent_messages").as_int(), 6);
    auto boundary_content = boundary_json->root().get_string("content");
    EXPECT_NE(boundary_content.find("hello server route"), std::string::npos);
    EXPECT_NE(boundary_content.find("follow up 0"), std::string::npos);
    EXPECT_NE(boundary_content.find("Preserve these details"), std::string::npos);

    cc::server::reset_route_state_for_testing();
    fs::remove_all(root);
}

TEST(RateLimitManager, UpdatesStateFromHeadersAndWarnsNearLimits) {
    cc::services::RateLimitManager manager;
    manager.update_from_headers({
        {"x-ratelimit-remaining-requests", "3"},
        {"x-ratelimit-remaining-tokens", "9000"},
        {"retry-after", "2"},
    });

    const auto& state = manager.get_state();
    EXPECT_EQ(state.current_info.requests_remaining, 3);
    EXPECT_EQ(state.current_info.tokens_remaining, 9000);
    EXPECT_EQ(state.current_info.retry_after, std::chrono::milliseconds(2000));
    EXPECT_TRUE(manager.get_warning_message().has_value());
}

TEST(RateLimitManager, MockRateLimitControlsLimitedState) {
    cc::services::RateLimitManager manager;
    manager.mock_rate_limit({.simulate_429 = true});
    EXPECT_TRUE(manager.is_rate_limited());

    manager.clear_mock();
    EXPECT_FALSE(manager.is_rate_limited());
}

TEST(SessionMemoryService, StoresSearchesAndDeletesMemoryItems) {
    cc::services::memory::SessionMemoryService service;
    const auto now = std::chrono::system_clock::now();
    cc::services::memory::MemoryItem item{
        .id = "mem-1",
        .content = "remember project migration details",
        .type = "note",
        .created_at = now,
        .updated_at = now,
        .importance = 7,
    };

    ASSERT_TRUE(service.add_memory(item).has_value());

    auto loaded = service.get_memory("mem-1");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->content, item.content);

    auto matches = service.search_memories("migration");
    ASSERT_TRUE(matches.has_value());
    ASSERT_EQ(matches->size(), 1u);
    EXPECT_EQ(matches->front().id, "mem-1");

    ASSERT_TRUE(service.delete_memory("mem-1").has_value());
    EXPECT_FALSE(service.get_memory("mem-1").has_value());
}

TEST(TokenEstimator, EstimatesTextImagesToolsAndModelLimits) {
    using cc::services::ImageDetail;
    using cc::services::TokenEstimator;

    EXPECT_GT(TokenEstimator::estimate_text("Hello world"), 0u);
    EXPECT_EQ(TokenEstimator::estimate_text(""), 1u);
    EXPECT_EQ(TokenEstimator::estimate_image(512, 512, ImageDetail::low), 85u);
    EXPECT_GT(TokenEstimator::estimate_tool_use("Read", R"({"file_path":"main.cpp"})"), 50u);
    EXPECT_EQ(TokenEstimator::get_model_limit("claude-3-5-sonnet"), 200000u);
    EXPECT_TRUE(TokenEstimator::fits_in_context(1000, "claude-3-5-sonnet"));
}

TEST(TelemetryManager, TracksEventsAndFlushesConfiguredEndpoint) {
    cc::services::TelemetryConfig config;
    config.enabled = true;
    config.send_to_server = true;
    config.endpoint = "https://telemetry.example.test";
    config.max_buffer_size = 2;
    cc::services::TelemetryManager telemetry(config);
    telemetry.set_session("session-1");

    telemetry.track_command("help");
    EXPECT_EQ(telemetry.get_event_count(), 1u);
    EXPECT_EQ(telemetry.get_buffer_size(), 1u);

    telemetry.track_tool_use("Read", 12.5);
    EXPECT_EQ(telemetry.get_buffer_size(), 0u);
    EXPECT_EQ(telemetry.get_last_flush_endpoint(), "https://telemetry.example.test");
    EXPECT_EQ(telemetry.get_last_flush_count(), 2u);
}

TEST(TelemetryManager, SpanGuardRecordsCompletedSpanOnDestruction) {
    cc::services::TelemetryManager telemetry;
    {
        auto span = telemetry.start_span("compile", "trace-1");
        span.set_attribute("target", "test_services");
    }

    ASSERT_EQ(telemetry.get_spans().size(), 1u);
    EXPECT_EQ(telemetry.get_spans().front().name, "compile");
    EXPECT_EQ(telemetry.get_spans().front().trace_id, "trace-1");
    EXPECT_TRUE(telemetry.get_spans().front().end_time.has_value());
}
