/// @file test_services.cpp
/// @brief Service layer smoke tests aligned with current C++ module APIs.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
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
#include <sys/time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif

#include <gtest/gtest.h>
#include <httplib.h>

import cc.cli.ccr_client;
import cc.config.config;
import cc.services.api.client;
import cc.services.api.errors;
import cc.services.api.session_ingress;
import cc.services.api.streaming;
import cc.services.compact.api_microcompact;
import cc.services.lsp.LSPServerManager;
import cc.services.lsp.client;
import cc.services.mcp.client;
import cc.services.mcp.auth;
import cc.services.mcp.channel_permissions;
import cc.services.mcp.config;
import cc.services.mcp.connection_manager;
import cc.services.mcp.elicitation_handler;
import cc.services.mcp.headers_helper;
import cc.services.mcp.vscode_sdk_mcp;
import cc.services.memory.sessionMemory;
import cc.services.mcp.types;
import cc.services.rate_limit;
import cc.services.telemetry;
import cc.services.token_estimation;
import cc.services.voice.voice;
import cc.services.prompt_suggestion;
import cc.server.server_routes;
import cc.server.server_main;
import cc.session.storage;
import cc.session.history;
import cc.query.query_engine;
import cc.remote.remote_session;
import cc.tools.agent_runtime;
import cc.tools.team;
import cc.tools.tool;
import cc.types.types;
import cc.utils.error;
import cc.utils.ide_integration;
import cc.utils.json;
import cc.utils.team_helpers;

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

struct EnvironmentUnsetGuard {
    std::string name;
    std::optional<std::string> previous;

    explicit EnvironmentUnsetGuard(std::string key) : name(std::move(key)) {
        if (const char* existing = std::getenv(name.c_str())) {
            previous = existing;
        }
        unsetenv(name.c_str());
    }

    ~EnvironmentUnsetGuard() {
        if (previous) {
            setenv(name.c_str(), previous->c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
    }
};

class DefinitionOnlyTool final : public cc::core::ITool {
public:
    explicit DefinitionOnlyTool(cc::core::ToolDefinition definition)
        : definition_(std::move(definition)) {}

    [[nodiscard]] const cc::core::ToolDefinition& definition() const override {
        return definition_;
    }

    [[nodiscard]] cc::core::Result<cc::core::ToolResult> execute(
        const cc::core::ToolInput& /*input*/) override {
        return cc::core::ToolResult::success("unused");
    }

    [[nodiscard]] bool check_permission(const cc::core::ToolInput& /*input*/) const override {
        return true;
    }

private:
    cc::core::ToolDefinition definition_;
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

int test_hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
    return -1;
}

std::string test_url_decode(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            out.push_back(' ');
        } else if (value[i] == '%' && i + 2 < value.size()) {
            const auto hi = test_hex_value(value[i + 1]);
            const auto lo = test_hex_value(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(value[i]);
            }
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

std::optional<std::string> test_query_param(std::string_view url, std::string_view key) {
    auto query_start = url.find('?');
    if (query_start == std::string_view::npos) return std::nullopt;
    auto fragment_start = url.find('#', query_start);
    auto query = url.substr(
        query_start + 1,
        fragment_start == std::string_view::npos ? std::string_view::npos : fragment_start - query_start - 1);
    std::string pattern(key);
    pattern.push_back('=');
    std::size_t pos = 0;
    while (pos < query.size()) {
        auto next = query.find('&', pos);
        auto part = query.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos);
        if (part.starts_with(pattern)) return test_url_decode(part.substr(pattern.size()));
        if (next == std::string_view::npos) break;
        pos = next + 1;
    }
    return std::nullopt;
}

std::optional<int> localhost_url_port(std::string_view url) {
    constexpr std::string_view prefix = "http://localhost:";
    if (!url.starts_with(prefix)) return std::nullopt;
    auto port_start = prefix.size();
    auto path_start = url.find('/', port_start);
    if (path_start == std::string_view::npos) return std::nullopt;
    try {
        return std::stoi(std::string(url.substr(port_start, path_start - port_start)));
    } catch (...) {
        return std::nullopt;
    }
}

class LocalAnthropicMessagesServer {
public:
    explicit LocalAnthropicMessagesServer(
        std::vector<std::string> response_bodies = {},
        std::vector<int> response_statuses = {})
        : response_bodies_(std::move(response_bodies)),
          response_statuses_(std::move(response_statuses)) {
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

    [[nodiscard]] std::optional<std::string> wait_for_headers(
        std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this] { return request_headers_.has_value(); })) {
            return std::nullopt;
        }
        return request_headers_;
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

    struct RawRequest {
        std::string headers;
        std::string body;
    };

    static RawRequest read_request(int fd) {
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
        return RawRequest{header, request.substr(body_start, content_length)};
    }

    void handle_client(int fd) {
        auto request = read_request(fd);
        auto body = request.body;
        std::size_t request_index = 0;
        {
            std::lock_guard lock(mutex_);
            if (!request_body_) request_body_ = body;
            if (!request_headers_) request_headers_ = request.headers;
            request_bodies_.push_back(body);
            request_index = request_bodies_.size() - 1;
        }
        cv_.notify_all();

        std::string response_body =
            R"({"id":"msg_test","type":"message","role":"assistant","model":"claude-test","content":[{"type":"text","text":"ok"}],"stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":1}})";
        if (!response_bodies_.empty()) {
            response_body = response_bodies_[std::min(request_index, response_bodies_.size() - 1)];
        }
        int response_status = 200;
        std::string response_reason = "OK";
        if (!response_statuses_.empty()) {
            response_status = response_statuses_[std::min(request_index, response_statuses_.size() - 1)];
            if (response_status == 413) {
                response_reason = "Payload Too Large";
            } else if (response_status >= 400) {
                response_reason = "Error";
            }
        }
        const auto response = std::format(
            "HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            response_status,
            response_reason,
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
    std::optional<std::string> request_headers_;
    std::vector<std::string> request_bodies_;
    std::vector<std::string> response_bodies_;
    std::vector<int> response_statuses_;
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
        if (request->method == "POST" && request->path == "/v1/sessions/session_1/events") {
            send_response(fd, 201, "Created", R"({"ok":true})");
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

class LocalReconnectSseStreamServer {
public:
    LocalReconnectSseStreamServer() {
        server_.Get("/sse", [this](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard lock(mutex_);
                stream_last_event_ids_.push_back(req.get_header_value("Last-Event-ID"));
            }
            cv_.notify_all();
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "close");
            res.set_content(
                "id: 1\n"
                "event: endpoint\n"
                "data: /messages\n\n",
                "text/event-stream");
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] {
            server_.listen_after_bind();
        });
        server_.wait_until_ready();
    }

    ~LocalReconnectSseStreamServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] bool ready() const {
        return port_ > 0;
    }

    [[nodiscard]] std::string url() const {
        return std::format("http://127.0.0.1:{}/sse", port_);
    }

    [[nodiscard]] std::vector<std::string> stream_last_event_ids() const {
        std::lock_guard lock(mutex_);
        return stream_last_event_ids_;
    }

    [[nodiscard]] bool wait_for_stream_requests(
        std::size_t count,
        std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, count] {
            return stream_last_event_ids_.size() >= count;
        });
    }

private:
    httplib::Server server_;
    int port_{0};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::string> stream_last_event_ids_;
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

struct DirectConnectHttpResponse {
    int status = 0;
    std::string body;
};

struct DirectConnectWsFrame {
    std::uint8_t opcode = 0;
    std::string payload;
};

void direct_connect_set_timeouts(int fd) {
    timeval timeout{};
    timeout.tv_sec = 3;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

int direct_connect_open_socket(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    direct_connect_set_timeouts(fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::optional<DirectConnectHttpResponse> direct_connect_http_request(
    std::uint16_t port,
    std::string_view method,
    std::string_view target,
    std::string_view body = {}
) {
    const int fd = direct_connect_open_socket(port);
    if (fd < 0) return std::nullopt;

    std::ostringstream request;
    request << method << " " << target << " HTTP/1.1\r\n"
            << "Host: 127.0.0.1:" << port << "\r\n"
            << "Connection: close\r\n";
    if (!body.empty() || method == "POST") {
        request << "Content-Type: application/json\r\n"
                << "Content-Length: " << body.size() << "\r\n";
    }
    request << "\r\n" << body;
    if (!send_all(fd, request.str())) {
        ::close(fd);
        return std::nullopt;
    }

    std::string raw;
    std::array<char, 4096> buffer{};
    while (true) {
        auto n = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (n <= 0) break;
        raw.append(buffer.data(), static_cast<std::size_t>(n));
    }
    ::close(fd);

    const auto header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return std::nullopt;

    DirectConnectHttpResponse response;
    std::istringstream first_line(raw.substr(0, raw.find("\r\n")));
    std::string http_version;
    first_line >> http_version >> response.status;
    response.body = raw.substr(header_end + 4);
    return response;
}

bool direct_connect_read_exact(int fd, char* data, std::size_t size) {
    while (size > 0) {
        auto n = ::recv(fd, data, size, 0);
        if (n <= 0) return false;
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

std::optional<DirectConnectWsFrame> direct_connect_read_ws_frame(int fd) {
    unsigned char header[2]{};
    if (!direct_connect_read_exact(fd, reinterpret_cast<char*>(header), 2)) return std::nullopt;

    DirectConnectWsFrame frame;
    frame.opcode = header[0] & 0x0f;
    const bool masked = (header[1] & 0x80) != 0;
    std::uint64_t len = header[1] & 0x7f;
    if (len == 126) {
        unsigned char ext[2]{};
        if (!direct_connect_read_exact(fd, reinterpret_cast<char*>(ext), 2)) return std::nullopt;
        len = (static_cast<std::uint64_t>(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
        unsigned char ext[8]{};
        if (!direct_connect_read_exact(fd, reinterpret_cast<char*>(ext), 8)) return std::nullopt;
        len = 0;
        for (unsigned char byte : ext) len = (len << 8) | byte;
    }

    std::array<unsigned char, 4> mask{};
    if (masked && !direct_connect_read_exact(fd, reinterpret_cast<char*>(mask.data()), mask.size())) {
        return std::nullopt;
    }

    frame.payload.resize(static_cast<std::size_t>(len));
    if (len > 0 && !direct_connect_read_exact(fd, frame.payload.data(), frame.payload.size())) {
        return std::nullopt;
    }
    if (masked) {
        for (std::size_t i = 0; i < frame.payload.size(); ++i) {
            frame.payload[i] = static_cast<char>(frame.payload[i] ^ mask[i % 4]);
        }
    }
    return frame;
}

bool direct_connect_send_client_text_frame(int fd, std::string_view payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    const auto len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<char>(0x80 | len));
    } else if (len <= 0xffff) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((len >> 8) & 0xff));
        frame.push_back(static_cast<char>(len & 0xff));
    } else {
        frame.push_back(static_cast<char>(0x80 | 127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>((len >> shift) & 0xff));
        }
    }

    constexpr std::array<unsigned char, 4> mask{0x11, 0x22, 0x33, 0x44};
    for (auto byte : mask) frame.push_back(static_cast<char>(byte));
    for (std::size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(payload[i] ^ mask[i % mask.size()]));
    }
    return send_all(fd, frame);
}

std::optional<int> direct_connect_open_websocket(std::uint16_t port, std::string_view path) {
    const int fd = direct_connect_open_socket(port);
    if (fd < 0) return std::nullopt;

    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const auto request = std::format(
        "GET {} HTTP/1.1\r\n"
        "Host: 127.0.0.1:{}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: {}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n",
        path,
        port,
        key);
    if (!send_all(fd, request)) {
        ::close(fd);
        return std::nullopt;
    }

    std::string headers;
    std::array<char, 1024> buffer{};
    while (headers.find("\r\n\r\n") == std::string::npos) {
        auto n = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (n <= 0) {
            ::close(fd);
            return std::nullopt;
        }
        headers.append(buffer.data(), static_cast<std::size_t>(n));
        if (headers.size() > 64 * 1024) {
            ::close(fd);
            return std::nullopt;
        }
    }
    if (headers.find("101 Switching Protocols") == std::string::npos ||
        headers.find(remote_ws_accept_key(key)) == std::string::npos) {
        ::close(fd);
        return std::nullopt;
    }
    return fd;
}

std::string direct_connect_trim_json_line(std::string payload) {
    while (!payload.empty() && std::isspace(static_cast<unsigned char>(payload.back()))) {
        payload.pop_back();
    }
    return payload;
}

bool direct_connect_send_permission_response(
    int fd,
    std::string_view request_id,
    std::string_view behavior,
    std::string_view extra_response_fields_json = {}
) {
    const auto payload = std::format(
        R"({{"type":"control_response","response":{{"subtype":"success","request_id":"{}","response":{{"behavior":"{}"{}}}}}}})",
        request_id,
        behavior,
        extra_response_fields_json);
    return direct_connect_send_client_text_frame(fd, payload);
}

bool direct_connect_send_permission_error_response(
    int fd,
    std::string_view request_id,
    std::string_view error
) {
    const auto payload = std::format(
        R"({{"type":"control_response","response":{{"subtype":"error","request_id":"{}","error":"{}"}}}})",
        request_id,
        error);
    return direct_connect_send_client_text_frame(fd, payload);
}

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

class LocalRefreshingStreamableHttpMcpServer {
public:
    explicit LocalRefreshingStreamableHttpMcpServer(bool fail_refresh = false)
        : fail_refresh_(fail_refresh) {
        server_.Get("/metadata", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(std::format(R"({{
              "authorization_endpoint": "{0}/authorize",
              "token_endpoint": "{0}/token",
              "scope": "tools"
            }})", base_url()), "application/json");
        });

        server_.Post("/token", [this](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard lock(mutex_);
                token_request_body_ = req.body;
                ++token_request_count_;
            }
            cv_.notify_all();
            if (req.body.find("grant_type=refresh_token") == std::string::npos ||
                req.body.find("refresh_token=old-refresh") == std::string::npos ||
                req.body.find("client_id=client-1") == std::string::npos) {
                res.status = 400;
                res.set_content(R"({"error":"invalid_request"})", "application/json");
                return;
            }
            if (fail_refresh_) {
                res.status = 400;
                res.set_content(R"({"error":"invalid_grant"})", "application/json");
                return;
            }
            res.set_content(R"({
              "access_token": "fresh-access",
              "refresh_token": "fresh-refresh",
              "expires_in": 3600,
              "scope": "tools"
            })", "application/json");
        });

        server_.Post("/mcp", [this](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard lock(mutex_);
                mcp_authorization_headers_.push_back(req.get_header_value("Authorization"));
                mcp_request_bodies_.push_back(req.body);
            }
            cv_.notify_all();

            if (req.get_header_value("Authorization") != "Bearer fresh-access") {
                res.status = 401;
                res.set_content("", "text/plain");
                return;
            }

            auto parsed = cc::utils::json::parse(req.body);
            if (!parsed) {
                res.status = 400;
                res.set_content("", "text/plain");
                return;
            }

            auto root = parsed->root();
            const auto method = std::string(root.get("method").as_str());
            if (method == "notifications/initialized" || method == "notifications/cancelled") {
                res.status = 202;
                res.set_content("", "text/plain");
                return;
            }

            const auto id = json_id_literal(root.get("id"));
            if (method == "initialize") {
                res.set_content(std::format(
                    R"({{"jsonrpc":"2.0","id":{},"result":{{"protocolVersion":"2024-11-05","capabilities":{{"tools":{{}}}},"serverInfo":{{"name":"refresh-fixture","version":"1.0.0"}}}}}})",
                    id), "application/json");
                return;
            }
            if (method == "tools/list") {
                res.set_content(std::format(
                    R"({{"jsonrpc":"2.0","id":{},"result":{{"tools":[{{"name":"refresh_lookup","description":"Lookup after refresh","inputSchema":{{"type":"object"}}}}]}}}})",
                    id), "application/json");
                return;
            }

            res.status = 404;
            res.set_content("", "text/plain");
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] {
            server_.listen_after_bind();
        });
        server_.wait_until_ready();
    }

    ~LocalRefreshingStreamableHttpMcpServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] bool ready() const {
        return port_ > 0;
    }

    [[nodiscard]] std::string base_url() const {
        return std::format("http://127.0.0.1:{}", port_);
    }

    [[nodiscard]] std::string mcp_url() const {
        return base_url() + "/mcp";
    }

    [[nodiscard]] std::string metadata_url() const {
        return base_url() + "/metadata";
    }

    [[nodiscard]] std::string token_request_body() const {
        std::lock_guard lock(mutex_);
        return token_request_body_;
    }

    [[nodiscard]] std::vector<std::string> mcp_authorization_headers() const {
        std::lock_guard lock(mutex_);
        return mcp_authorization_headers_;
    }

    [[nodiscard]] bool wait_for_token_request(std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return token_request_count_ > 0; });
    }

private:
    httplib::Server server_;
    int port_{0};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    int token_request_count_{0};
    std::string token_request_body_;
    std::vector<std::string> mcp_authorization_headers_;
    std::vector<std::string> mcp_request_bodies_;
    bool fail_refresh_{false};
};

class LocalOAuthRevocationServer {
public:
    LocalOAuthRevocationServer() {
        server_.Get("/metadata", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(std::format(R"({{
              "authorization_endpoint": "{0}/authorize",
              "token_endpoint": "{0}/token",
              "revocation_endpoint": "{0}/revoke",
              "revocation_endpoint_auth_methods_supported": ["client_secret_post"],
              "scope": "tools"
            }})", base_url()), "application/json");
        });

        server_.Post("/token", [this](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard lock(mutex_);
                token_request_body_ = req.body;
                ++token_request_count_;
            }
            cv_.notify_all();
            if (req.body.find("grant_type=authorization_code") == std::string::npos ||
                req.body.find("code=callback-code") == std::string::npos ||
                req.body.find("client_id=client-1") == std::string::npos ||
                req.body.find("code_verifier=") == std::string::npos ||
                req.body.find("redirect_uri=http%3A%2F%2Flocalhost%3A") == std::string::npos) {
                res.status = 400;
                res.set_content(R"({"error":"invalid_request"})", "application/json");
                return;
            }
            res.set_content(R"({
              "access_token": "callback-access",
              "refresh_token": "callback-refresh",
              "expires_in": 3600,
              "scope": "tools"
            })", "application/json");
        });

        server_.Post("/revoke", [this](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard lock(mutex_);
                revoke_request_bodies_.push_back(req.body);
                revoke_authorization_headers_.push_back(req.get_header_value("Authorization"));
            }
            cv_.notify_all();
            res.status = 200;
            res.set_content(R"({"ok":true})", "application/json");
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] {
            server_.listen_after_bind();
        });
        server_.wait_until_ready();
    }

    ~LocalOAuthRevocationServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] bool ready() const {
        return port_ > 0;
    }

    [[nodiscard]] std::string base_url() const {
        return std::format("http://127.0.0.1:{}", port_);
    }

    [[nodiscard]] std::string metadata_url() const {
        return base_url() + "/metadata";
    }

    [[nodiscard]] std::string token_request_body() const {
        std::lock_guard lock(mutex_);
        return token_request_body_;
    }

    [[nodiscard]] std::vector<std::string> revoke_request_bodies() const {
        std::lock_guard lock(mutex_);
        return revoke_request_bodies_;
    }

    [[nodiscard]] std::vector<std::string> revoke_authorization_headers() const {
        std::lock_guard lock(mutex_);
        return revoke_authorization_headers_;
    }

    [[nodiscard]] bool wait_for_revoke_requests(
        std::size_t count,
        std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, count] {
            return revoke_request_bodies_.size() >= count;
        });
    }

    [[nodiscard]] bool wait_for_token_request(std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return token_request_count_ > 0; });
    }

private:
    httplib::Server server_;
    int port_{0};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    int token_request_count_{0};
    std::string token_request_body_;
    std::vector<std::string> revoke_request_bodies_;
    std::vector<std::string> revoke_authorization_headers_;
};

class LocalXaaIdpServer {
public:
    LocalXaaIdpServer() {
        // PRM discovery (RFC 9728): GET /mcp/.well-known/oauth-protected-resource
        server_.Get("/mcp/.well-known/oauth-protected-resource",
            [this](const httplib::Request& req, httplib::Response& res) {
            (void)req;
            {
                std::lock_guard lock(mutex_);
                ++prm_request_count_;
            }
            cv_.notify_all();
            auto base = base_url();
            auto body = std::format(R"({{
                "resource": "{}/mcp",
                "authorization_servers": ["{}"]
            }})", base, base);
            res.set_content(body, "application/json");
        });

        // AS metadata (RFC 8414): GET /.well-known/oauth-authorization-server
        server_.Get("/.well-known/oauth-authorization-server",
            [this](const httplib::Request& req, httplib::Response& res) {
            (void)req;
            {
                std::lock_guard lock(mutex_);
                ++as_metadata_request_count_;
            }
            cv_.notify_all();
            auto base = base_url();
            auto body = std::format(R"({{
                "issuer": "{}",
                "token_endpoint": "{}/token",
                "grant_types_supported": [
                    "urn:ietf:params:oauth:grant-type:jwt-bearer",
                    "urn:ietf:params:oauth:grant-type:token-exchange"
                ],
                "token_endpoint_auth_methods_supported": ["client_secret_basic"]
            }})", base, base);
            res.set_content(body, "application/json");
        });

        // Token endpoint: handles both RFC 8693 (token-exchange) and RFC 7523 (jwt-bearer)
        server_.Post("/token",
            [this](const httplib::Request& req, httplib::Response& res) {
            auto body = req.body;
            {
                std::lock_guard lock(mutex_);
                if (body.find("grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Atoken-exchange")
                    != std::string::npos) {
                    // RFC 8693 Token Exchange (id_token → ID-JAG)
                    ++token_exchange_count_;
                    token_exchange_body_ = body;
                    cv_.notify_all();

                    // Verify expected fields
                    if (body.find("subject_token=fake-id-token-for-testing") == std::string::npos ||
                        body.find("client_id=idp-client-1") == std::string::npos) {
                        res.status = 400;
                        res.set_content(R"({"error":"invalid_request"})", "application/json");
                        return;
                    }

                    res.set_content(R"({
                        "access_token": "fake-id-jag-for-testing",
                        "issued_token_type": "urn:ietf:params:oauth:token-type:id-jag",
                        "expires_in": 3600,
                        "scope": "openid profile mcp"
                    })", "application/json");
                } else if (body.find("grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer")
                    != std::string::npos) {
                    // RFC 7523 JWT Bearer Grant (ID-JAG → access_token)
                    ++jwt_bearer_count_;
                    jwt_bearer_body_ = body;
                    jwt_bearer_auth_header_ = req.get_header_value("Authorization");
                    cv_.notify_all();

                    res.set_content(R"({
                        "access_token": "xaa-access",
                        "refresh_token": "xaa-refresh",
                        "token_type": "Bearer",
                        "expires_in": 3600,
                        "scope": "openid profile mcp"
                    })", "application/json");
                } else {
                    res.status = 400;
                    res.set_content(R"({"error":"unsupported_grant_type"})", "application/json");
                }
            }
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] {
            server_.listen_after_bind();
        });
        server_.wait_until_ready();
    }

    ~LocalXaaIdpServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] bool ready() const {
        return port_ > 0;
    }

    [[nodiscard]] std::string base_url() const {
        return std::format("http://127.0.0.1:{}", port_);
    }

    // ── Request counters and bodies for test assertions ──────────────────

    [[nodiscard]] int prm_request_count() const {
        std::lock_guard lock(mutex_);
        return prm_request_count_;
    }

    [[nodiscard]] int as_metadata_request_count() const {
        std::lock_guard lock(mutex_);
        return as_metadata_request_count_;
    }

    [[nodiscard]] int token_exchange_count() const {
        std::lock_guard lock(mutex_);
        return token_exchange_count_;
    }

    [[nodiscard]] int jwt_bearer_count() const {
        std::lock_guard lock(mutex_);
        return jwt_bearer_count_;
    }

    [[nodiscard]] std::string token_exchange_body() const {
        std::lock_guard lock(mutex_);
        return token_exchange_body_;
    }

    [[nodiscard]] std::string jwt_bearer_body() const {
        std::lock_guard lock(mutex_);
        return jwt_bearer_body_;
    }

    [[nodiscard]] std::string jwt_bearer_auth_header() const {
        std::lock_guard lock(mutex_);
        return jwt_bearer_auth_header_;
    }

    [[nodiscard]] bool wait_for_jwt_bearer(std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] {
            return jwt_bearer_count_ > 0;
        });
    }

private:
    httplib::Server server_;
    int port_{0};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;

    int prm_request_count_{0};
    int as_metadata_request_count_{0};
    int token_exchange_count_{0};
    int jwt_bearer_count_{0};
    std::string token_exchange_body_;
    std::string jwt_bearer_body_;
    std::string jwt_bearer_auth_header_;
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

TEST(SessionIngress, PostsSessionEventsWithBearerAuth) {
    LocalCcrHttpServer server;
    ASSERT_TRUE(server.ready());

    auto created = cc::services::api::create_ingress(cc::services::api::IngressConfig{
        .endpoint = server.base_url(),
        .session_id = "session_1",
        .auth_token = "session-jwt-token",
        .organization_uuid = std::nullopt,
    });
    ASSERT_TRUE(created.has_value()) << created.error();

    auto sent = cc::services::api::send_ingress_message(
        R"({"type":"control_response","response":{"request_id":"permission-1","subtype":"success"}})");
    ASSERT_TRUE(sent.has_value()) << sent.error();

    auto requests = server.wait_for_requests(1);
    ASSERT_TRUE(requests.has_value());
    ASSERT_EQ(requests->size(), 1u);
    EXPECT_EQ((*requests)[0].method, "POST");
    EXPECT_EQ((*requests)[0].path, "/v1/sessions/session_1/events");
    EXPECT_NE((*requests)[0].headers.find("Authorization: Bearer session-jwt-token"), std::string::npos);
    EXPECT_NE((*requests)[0].body.find(R"("events":[)"), std::string::npos);
    EXPECT_NE((*requests)[0].body.find(R"("type":"control_response")"), std::string::npos);
    EXPECT_NE((*requests)[0].body.find(R"("request_id":"permission-1")"), std::string::npos);

    cc::services::api::close_ingress();
}

TEST(SessionIngress, PostsSessionEventsWithSessionCookieAuth) {
    LocalCcrHttpServer server;
    ASSERT_TRUE(server.ready());

    auto created = cc::services::api::create_ingress(cc::services::api::IngressConfig{
        .endpoint = server.base_url(),
        .session_id = "session_1",
        .auth_token = "sk-ant-sid01-test",
        .organization_uuid = "org-uuid-1",
    });
    ASSERT_TRUE(created.has_value()) << created.error();

    auto sent = cc::services::api::send_ingress_message(R"({"type":"progress","value":0.5})");
    ASSERT_TRUE(sent.has_value()) << sent.error();

    auto requests = server.wait_for_requests(1);
    ASSERT_TRUE(requests.has_value());
    ASSERT_EQ(requests->size(), 1u);
    EXPECT_EQ((*requests)[0].method, "POST");
    EXPECT_EQ((*requests)[0].path, "/v1/sessions/session_1/events");
    EXPECT_NE((*requests)[0].headers.find("Cookie: sessionKey=sk-ant-sid01-test"), std::string::npos);
    EXPECT_NE((*requests)[0].headers.find("X-Organization-Uuid: org-uuid-1"), std::string::npos);
    EXPECT_EQ((*requests)[0].headers.find("Authorization:"), std::string::npos);
    EXPECT_NE((*requests)[0].body.find(R"("type":"progress")"), std::string::npos);

    cc::services::api::close_ingress();
}

TEST(SessionIngress, CreatesIngressFromDaemonEnvironmentAndSendsLifecycleEvent) {
    EnvironmentGuard endpoint("CLAUDE_CODE_REMOTE_API_BASE_URL", "placeholder");
    EnvironmentGuard session("CC_REMOTE_SESSION_ID", "session_1");
    EnvironmentGuard token("CLAUDE_CODE_SESSION_ACCESS_TOKEN", "env-session-token");
    EnvironmentUnsetGuard compat_endpoint("CC_REPL_REMOTE_API_BASE_URL");
    EnvironmentUnsetGuard ingress_endpoint("CLAUDE_CODE_SESSION_INGRESS_URL");
    EnvironmentUnsetGuard compat_ingress_endpoint("CC_REPL_SESSION_INGRESS_URL");
    EnvironmentUnsetGuard compat_session("CLAUDE_CODE_REMOTE_SESSION_ID");

    LocalCcrHttpServer server;
    ASSERT_TRUE(server.ready());
    setenv("CLAUDE_CODE_REMOTE_API_BASE_URL", server.base_url().c_str(), 1);

    cc::services::api::close_ingress();
    auto created = cc::services::api::create_ingress_from_environment();
    ASSERT_TRUE(created.has_value()) << created.error();
    EXPECT_TRUE(*created);
    EXPECT_TRUE(cc::services::api::is_ingress_active());

    auto sent = cc::services::api::send_ingress_lifecycle_event("started", std::string_view{"work_1"});
    ASSERT_TRUE(sent.has_value()) << sent.error();

    auto requests = server.wait_for_requests(1);
    ASSERT_TRUE(requests.has_value());
    ASSERT_EQ(requests->size(), 1u);
    EXPECT_EQ((*requests)[0].method, "POST");
    EXPECT_EQ((*requests)[0].path, "/v1/sessions/session_1/events");
    EXPECT_NE((*requests)[0].headers.find("Authorization: Bearer env-session-token"), std::string::npos);
    EXPECT_NE((*requests)[0].body.find(R"("type":"session_lifecycle")"), std::string::npos);
    EXPECT_NE((*requests)[0].body.find(R"("status":"started")"), std::string::npos);
    EXPECT_NE((*requests)[0].body.find(R"("bridge_work_id":"work_1")"), std::string::npos);

    cc::services::api::close_ingress();
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

TEST(QueryEngine, SnipMetadataProjectsRemovedMessagesFromAnthropicRequest) {
    LocalAnthropicMessagesServer server;
    ASSERT_NE(server.port(), 0);

    auto root = fs::temp_directory_path() / "cc-repl-query-engine-snip-projection-test";
    fs::remove_all(root);
    fs::create_directories(root);

    cc::core::ToolRegistry registry;
    cc::core::QueryEngineConfig config;
    config.api_key = "test-key";
    config.base_url = server.base_url();
    config.cwd = root.string();
    config.retry_policy.max_retries = 0;
    config.thinking_config.mode = cc::core::ThinkingConfig::Mode::Disabled;
    cc::core::QueryEngine engine(std::move(config), registry);

    cc::core::UserMessage old_user{};
    old_user.id.value = "snipped-user-id";
    old_user.timestamp = std::chrono::system_clock::now();
    old_user.content.push_back(cc::core::TextBlock{"SNIPPED_USER_PAYLOAD_DO_NOT_SEND"});
    engine.append_message_for_testing(cc::core::Message{std::move(old_user)});

    cc::core::AssistantMessage old_assistant{};
    old_assistant.id.value = "snipped-assistant-id";
    old_assistant.timestamp = std::chrono::system_clock::now();
    old_assistant.content.push_back(cc::core::TextBlock{"SNIPPED_ASSISTANT_PAYLOAD_DO_NOT_SEND"});
    engine.append_message_for_testing(cc::core::Message{std::move(old_assistant)});

    cc::core::SystemMessage snip_boundary{};
    snip_boundary.id.value = "snip-boundary-id";
    snip_boundary.timestamp = std::chrono::system_clock::now();
    snip_boundary.subtype = "snip_boundary";
    snip_boundary.content.push_back(cc::core::TextBlock{"Conversation snipped."});
    snip_boundary.snip_metadata = cc::core::SnipMetadata{
        .removed_uuids = {"snipped-user-id", "snipped-assistant-id"},
    };
    engine.append_message_for_testing(cc::core::Message{std::move(snip_boundary)});

    cc::core::UserMessage survivor{};
    survivor.id.value = "survivor-user-id";
    survivor.timestamp = std::chrono::system_clock::now();
    survivor.content.push_back(cc::core::TextBlock{"SURVIVOR_PAYLOAD_SHOULD_SEND"});
    engine.append_message_for_testing(cc::core::Message{std::move(survivor)});

    auto response = engine.query("fresh prompt after snip");
    ASSERT_TRUE(response.has_value()) << response.error().message;

    auto request_body = server.wait_for_body();
    ASSERT_TRUE(request_body.has_value());
    EXPECT_EQ(request_body->find("SNIPPED_USER_PAYLOAD_DO_NOT_SEND"), std::string::npos)
        << *request_body;
    EXPECT_EQ(request_body->find("SNIPPED_ASSISTANT_PAYLOAD_DO_NOT_SEND"), std::string::npos)
        << *request_body;
    EXPECT_EQ(request_body->find("snipped-user-id"), std::string::npos) << *request_body;
    EXPECT_EQ(request_body->find("snipped-assistant-id"), std::string::npos) << *request_body;
    EXPECT_NE(request_body->find("SURVIVOR_PAYLOAD_SHOULD_SEND"), std::string::npos)
        << *request_body;
    EXPECT_NE(request_body->find("fresh prompt after snip"), std::string::npos)
        << *request_body;

    auto conversation = engine.get_conversation();
    auto contains_message_id = [&](std::string_view id) {
        return std::ranges::any_of(conversation, [&](const cc::core::Message& msg) {
            return std::visit([&](const auto& value) {
                return value.id.value == id;
            }, msg);
        });
    };
    EXPECT_FALSE(contains_message_id("snipped-user-id"));
    EXPECT_FALSE(contains_message_id("snipped-assistant-id"));
    EXPECT_TRUE(contains_message_id("snip-boundary-id"));
    EXPECT_TRUE(contains_message_id("survivor-user-id"));

    fs::remove_all(root);
}

TEST(ApiMicrocompact, BuildsThinkingAndToolContextManagementStrategies) {
    EnvironmentGuard user_type_guard("USER_TYPE", "ant");
    EnvironmentGuard clear_results_guard("USE_API_CLEAR_TOOL_RESULTS", "1");
    EnvironmentGuard clear_uses_guard("USE_API_CLEAR_TOOL_USES", "true");
    EnvironmentGuard max_tokens_guard("API_MAX_INPUT_TOKENS", "1000");
    EnvironmentGuard target_tokens_guard("API_TARGET_INPUT_TOKENS", "250");

    auto context = cc::services::compact::get_api_context_management({
        .has_thinking = true,
        .is_redact_thinking_active = false,
        .clear_all_thinking = true,
    });

    ASSERT_TRUE(context.has_value());
    ASSERT_EQ(context->edits.size(), 3u);

    EXPECT_EQ(context->edits[0].type, "clear_thinking_20251015");
    EXPECT_TRUE(context->edits[0].has_thinking_keep);
    ASSERT_TRUE(context->edits[0].keep_thinking_turns.has_value());
    EXPECT_EQ(*context->edits[0].keep_thinking_turns, 1u);

    EXPECT_EQ(context->edits[1].type, "clear_tool_uses_20250919");
    ASSERT_TRUE(context->edits[1].trigger_input_tokens.has_value());
    ASSERT_TRUE(context->edits[1].clear_at_least_input_tokens.has_value());
    EXPECT_EQ(*context->edits[1].trigger_input_tokens, 1000u);
    EXPECT_EQ(*context->edits[1].clear_at_least_input_tokens, 750u);
    EXPECT_TRUE(std::ranges::contains(context->edits[1].clear_tool_inputs, std::string{"Bash"}));
    EXPECT_TRUE(std::ranges::contains(context->edits[1].clear_tool_inputs, std::string{"Read"}));

    EXPECT_EQ(context->edits[2].type, "clear_tool_uses_20250919");
    EXPECT_TRUE(std::ranges::contains(context->edits[2].exclude_tools, std::string{"Edit"}));
    EXPECT_TRUE(std::ranges::contains(context->edits[2].exclude_tools, std::string{"Write"}));
    EXPECT_TRUE(std::ranges::contains(context->edits[2].exclude_tools, std::string{"NotebookEdit"}));
}

TEST(QueryEngine, SerializesTaskBudgetAndApiContextManagementRequestConfig) {
    EnvironmentUnsetGuard disable_thinking_guard("CLAUDE_CODE_DISABLE_THINKING");
    EnvironmentGuard user_type_guard("USER_TYPE", "ant");
    EnvironmentGuard clear_results_guard("USE_API_CLEAR_TOOL_RESULTS", "1");
    EnvironmentGuard clear_uses_guard("USE_API_CLEAR_TOOL_USES", "1");
    EnvironmentGuard max_tokens_guard("API_MAX_INPUT_TOKENS", "1000");
    EnvironmentGuard target_tokens_guard("API_TARGET_INPUT_TOKENS", "250");

    LocalAnthropicMessagesServer server;
    ASSERT_NE(server.port(), 0);

    auto root = fs::temp_directory_path() / "cc-repl-query-engine-task-budget-context-management-test";
    fs::remove_all(root);
    fs::create_directories(root);

    cc::core::ToolRegistry registry;
    cc::core::QueryEngineConfig config;
    config.api_key = "test-key";
    config.base_url = server.base_url();
    config.cwd = root.string();
    config.retry_policy.max_retries = 0;
    config.thinking_config.mode = cc::core::ThinkingConfig::Mode::Adaptive;
    config.task_budget = cc::core::QueryEngineConfig::TaskBudget{
        .total = 12'000,
        .remaining = 6'000,
    };

    cc::core::QueryEngine engine(std::move(config), registry);
    auto response = engine.query("hello");
    ASSERT_TRUE(response.has_value()) << response.error().message;

    auto request_body = server.wait_for_body();
    ASSERT_TRUE(request_body.has_value());
    auto parsed = cc::utils::json::parse(*request_body);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();

    auto output_config = parsed->root().get("output_config");
    ASSERT_TRUE(output_config.is_obj()) << *request_body;
    auto task_budget = output_config.get("task_budget");
    ASSERT_TRUE(task_budget.is_obj()) << *request_body;
    EXPECT_EQ(task_budget.get("type").as_str(), "tokens");
    EXPECT_EQ(task_budget.get("total").as_int(), 12'000);
    EXPECT_EQ(task_budget.get("remaining").as_int(), 6'000);

    auto context_management = parsed->root().get("context_management");
    ASSERT_TRUE(context_management.is_obj()) << *request_body;
    auto edits = context_management.get("edits");
    ASSERT_TRUE(edits.is_arr()) << *request_body;
    ASSERT_EQ(edits.size(), 3u) << *request_body;

    EXPECT_EQ(edits.at(0).get("type").as_str(), "clear_thinking_20251015");
    EXPECT_EQ(edits.at(0).get("keep").as_str(), "all");
    EXPECT_EQ(edits.at(1).get("trigger").get("value").as_int(), 1000);
    EXPECT_EQ(edits.at(1).get("clear_at_least").get("value").as_int(), 750);
    EXPECT_TRUE(edits.at(1).get("clear_tool_inputs").is_arr());
    EXPECT_TRUE(edits.at(2).get("exclude_tools").is_arr());

    auto headers = server.wait_for_headers();
    ASSERT_TRUE(headers.has_value());
    EXPECT_NE(headers->find("task-budgets-2026-03-13"), std::string::npos) << *headers;
    EXPECT_NE(headers->find("context-management-2025-06-27"), std::string::npos) << *headers;

    fs::remove_all(root);
}

TEST(QueryEngine, DisableThinkingEnvSuppressesThinkingAndClearThinkingContextManagement) {
    EnvironmentGuard disable_thinking_guard("CLAUDE_CODE_DISABLE_THINKING", "1");
    EnvironmentUnsetGuard user_type_guard("USER_TYPE");
    EnvironmentUnsetGuard clear_results_guard("USE_API_CLEAR_TOOL_RESULTS");
    EnvironmentUnsetGuard clear_uses_guard("USE_API_CLEAR_TOOL_USES");

    LocalAnthropicMessagesServer server;
    ASSERT_NE(server.port(), 0);

    auto root = fs::temp_directory_path() / "cc-repl-query-engine-disable-thinking-test";
    fs::remove_all(root);
    fs::create_directories(root);

    cc::core::ToolRegistry registry;
    cc::core::QueryEngineConfig config;
    config.api_key = "test-key";
    config.base_url = server.base_url();
    config.cwd = root.string();
    config.retry_policy.max_retries = 0;
    config.thinking_config.mode = cc::core::ThinkingConfig::Mode::Adaptive;

    cc::core::QueryEngine engine(std::move(config), registry);
    auto response = engine.query("hello");
    ASSERT_TRUE(response.has_value()) << response.error().message;

    auto request_body = server.wait_for_body();
    ASSERT_TRUE(request_body.has_value());
    auto parsed = cc::utils::json::parse(*request_body);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
    EXPECT_FALSE(parsed->root().get("thinking").valid()) << *request_body;
    EXPECT_FALSE(parsed->root().get("context_management").valid()) << *request_body;

    auto headers = server.wait_for_headers();
    ASSERT_TRUE(headers.has_value());
    EXPECT_EQ(headers->find("context-management-2025-06-27"), std::string::npos)
        << *headers;

    fs::remove_all(root);
}

TEST(QueryEngine, InjectsPendingNativeAgentTaskNotificationsIntoRequest) {
    LocalAnthropicMessagesServer server;
    ASSERT_NE(server.port(), 0);

    auto root = fs::temp_directory_path() / "cc-repl-query-engine-agent-notification-test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
        .agent_id = "query-notify-agent",
        .agent_type = "general-purpose",
        .description = "Query notify agent",
        .background = true,
        .status = cc::tools::agent_runtime::NativeAgentStatus::Completed,
        .output = std::string("native agent completed with useful context"),
    });

    cc::core::ToolRegistry registry;
    cc::core::QueryEngineConfig config;
    config.api_key = "test-key";
    config.base_url = server.base_url();
    config.cwd = root.string();
    config.retry_policy.max_retries = 0;
    config.thinking_config.mode = cc::core::ThinkingConfig::Mode::Disabled;

    cc::core::QueryEngine engine(std::move(config), registry);
    auto first_response = engine.query("hello");
    ASSERT_TRUE(first_response.has_value()) << first_response.error().message;

    auto first_body = server.wait_for_body();
    ASSERT_TRUE(first_body.has_value());
    auto first_json = cc::utils::json::parse(*first_body);
    ASSERT_TRUE(first_json.has_value()) << first_json.error().message();

    auto message_text = [](cc::utils::json::JsonVal message) {
        auto content = message.get("content");
        if (content.is_str()) return std::string(content.as_str());
        std::string text;
        if (content.is_arr()) {
            content.iter([&](cc::utils::json::JsonVal block) {
                auto block_text = block.get("text");
                if (block_text.is_str()) text += block_text.as_str();
            });
        }
        return text;
    };
    auto notification_count = [&](cc::utils::json::JsonVal messages) {
        std::size_t count = 0;
        messages.iter([&](cc::utils::json::JsonVal message) {
            if (message_text(message).find("<task_notification>") != std::string::npos) {
                ++count;
            }
        });
        return count;
    };

    auto first_messages = first_json->root().get("messages");
    ASSERT_TRUE(first_messages.is_arr()) << *first_body;
    ASSERT_EQ(notification_count(first_messages), 1u) << *first_body;

    bool saw_user_notification = false;
    first_messages.iter([&](cc::utils::json::JsonVal message) {
        const auto text = message_text(message);
        if (text.find("<task_notification>") == std::string::npos) return;
        EXPECT_EQ(std::string(message.get("role").as_str()), "user");
        EXPECT_NE(text.find("<task_id>query-notify-agent</task_id>"), std::string::npos);
        EXPECT_NE(text.find("<status>completed</status>"), std::string::npos);
        EXPECT_NE(text.find("native agent completed with useful context"), std::string::npos);
        saw_user_notification = true;
    });
    EXPECT_TRUE(saw_user_notification);

    auto delivered_record = cc::tools::agent_runtime::native_agent_store().get("query-notify-agent");
    ASSERT_TRUE(delivered_record.has_value());
    EXPECT_TRUE(delivered_record->notification_delivered);
    EXPECT_TRUE(cc::tools::agent_runtime::native_agent_store().take_pending_task_notifications().empty());

    auto second_response = engine.query("follow up");
    ASSERT_TRUE(second_response.has_value()) << second_response.error().message;
    auto request_bodies = server.wait_for_bodies(2);
    ASSERT_TRUE(request_bodies.has_value());
    ASSERT_EQ(request_bodies->size(), 2u);

    auto second_json = cc::utils::json::parse(request_bodies->back());
    ASSERT_TRUE(second_json.has_value()) << second_json.error().message();
    auto second_messages = second_json->root().get("messages");
    ASSERT_TRUE(second_messages.is_arr()) << request_bodies->back();
    EXPECT_EQ(notification_count(second_messages), 1u) << request_bodies->back();

    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
    fs::remove_all(root);
}

TEST(QueryEngine, PersistsTranscriptToSessionStorage) {
    auto dir = fs::temp_directory_path() / "cc_repl_qe_session_persist_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    cc::core::ToolRegistry registry;
    cc::core::QueryEngineConfig config;
    config.context_window.auto_compact = false;
    config.cwd = fs::temp_directory_path().string();
    config.model_params.model = "test-model";
    cc::core::QueryEngine engine(std::move(config), registry);
    engine.set_session_storage(dir);

    auto make_user = [](std::string text) {
        cc::core::UserMessage msg{};
        msg.content.push_back(cc::core::TextBlock{std::move(text)});
        return cc::core::Message{std::move(msg)};
    };
    auto make_assistant = [](std::string text) {
        cc::core::AssistantMessage msg{};
        msg.content.push_back(cc::core::TextBlock{std::move(text)});
        return cc::core::Message{std::move(msg)};
    };

    engine.append_message_for_testing(make_user("hello world"));
    engine.append_message_for_testing(make_assistant("hi there"));

    const auto msgs_path = dir / engine.session_id().str() / "messages.jsonl";
    ASSERT_TRUE(fs::exists(msgs_path)) << msgs_path;

    std::vector<std::string> lines;
    {
        std::ifstream ifs(msgs_path);
        std::string line;
        while (std::getline(ifs, line)) if (!line.empty()) lines.push_back(line);
    }
    // Two non-system messages -> two transcript lines.
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_NE(lines[0].find("hello world"), std::string::npos);
    EXPECT_NE(lines[0].find("user"), std::string::npos);
    EXPECT_NE(lines[1].find("hi there"), std::string::npos);
    EXPECT_NE(lines[1].find("assistant"), std::string::npos);

    // Metadata discoverable via list_recent_sessions.
    auto sessions = cc::session::list_recent_sessions(dir);
    EXPECT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions.front().session_id, engine.session_id().str());
    EXPECT_EQ(sessions.front().model, "test-model");

    // flush_session refreshes metadata message_count.
    engine.flush_session();
    auto sessions2 = cc::session::list_recent_sessions(dir);
    ASSERT_EQ(sessions2.size(), 1u);
    EXPECT_EQ(sessions2.front().message_count, 2);

    fs::remove_all(dir);
}

TEST(QueryEngine, StructuredOutputInjectsResponseSchema) {
    cc::core::ToolRegistry registry;
    cc::core::QueryEngineConfig config;
    config.context_window.auto_compact = false;
    config.response_schema = cc::core::QueryEngineConfig::ResponseSchema{
        .name = "result",
        .schema_json = R"({"type":"object","properties":{"answer":{"type":"string"}},"required":["answer"]})",
    };
    cc::core::QueryEngine engine(std::move(config), registry);

    const auto out = engine.build_output_config_json_for_testing();
    EXPECT_NE(out.find("output_config"), std::string::npos);
    EXPECT_NE(out.find("json_schema"), std::string::npos);
    EXPECT_NE(out.find("\"result\""), std::string::npos);
    EXPECT_NE(out.find("answer"), std::string::npos);

    // Without a schema and without a budget, output_config is omitted.
    cc::core::QueryEngineConfig bare;
    bare.context_window.auto_compact = false;
    cc::core::QueryEngine bare_engine(std::move(bare), registry);
    EXPECT_EQ(bare_engine.build_output_config_json_for_testing(), "{}");
}

TEST(QueryEngine, TracksInvokedSkillsInLoop) {
    // In-loop skill dispatch: when the skill tool is invoked, the engine
    // records the skill name in discovered_skills_ (previously a dead field).
    struct StubSkillTool final : cc::core::ITool {
        cc::core::ToolDefinition definition_{};
        StubSkillTool() {
            definition_.name = "skill";
            definition_.permission = cc::core::ToolPermission::ReadOnly;
        }
        [[nodiscard]] const cc::core::ToolDefinition& definition() const override { return definition_; }
        [[nodiscard]] cc::core::Result<cc::core::ToolResult> execute(const cc::core::ToolInput&) override {
            return cc::core::ToolResult::success("ok");
        }
        [[nodiscard]] bool check_permission(const cc::core::ToolInput&) const override { return true; }
    };

    cc::core::ToolRegistry registry;
    registry.register_tool(std::make_unique<StubSkillTool>());
    cc::core::QueryEngineConfig config;
    config.context_window.auto_compact = false;
    config.cwd = fs::temp_directory_path().string();
    cc::core::QueryEngine engine(std::move(config), registry);

    cc::core::ToolUseBlock tu{
        .id = cc::core::ToolUseId{.value = "tu-1"},
        .name = "skill",
        .input_json = R"({"name":"my-test-skill"})",
    };
    (void)engine.execute_single_tool_for_testing(tu);

    const auto skills = engine.discovered_skills();
    EXPECT_NE(std::find(skills.begin(), skills.end(), "my-test-skill"), skills.end())
        << "skill invocation should be tracked in discovered_skills_";
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

TEST(QueryEngine, CompactConversationCarriesTaskBudgetRemainingIntoNextRequest) {
    LocalAnthropicMessagesServer server;
    ASSERT_NE(server.port(), 0);

    auto root = fs::temp_directory_path() / "cc-repl-query-task-budget-compact-carry-test";
    fs::remove_all(root);
    fs::create_directories(root);

    cc::core::ToolRegistry registry;
    cc::core::QueryEngineConfig config;
    config.api_key = "test-key";
    config.base_url = server.base_url();
    config.context_window.auto_compact = false;
    config.cwd = root.string();
    config.retry_policy.max_retries = 0;
    config.thinking_config.mode = cc::core::ThinkingConfig::Mode::Disabled;
    config.task_budget = cc::core::QueryEngineConfig::TaskBudget{
        .total = 10'000,
        .remaining = std::nullopt,
    };

    cc::core::QueryEngine engine(std::move(config), registry);

    auto make_user = [](std::string text) {
        cc::core::UserMessage msg{};
        msg.id.value = "user-" + text.substr(0, 12);
        msg.timestamp = std::chrono::system_clock::now();
        msg.content.push_back(cc::core::TextBlock{std::move(text)});
        return cc::core::Message{std::move(msg)};
    };
    auto make_assistant = [](std::string text) {
        cc::core::AssistantMessage msg{};
        msg.id.value = "assistant-" + text.substr(0, 12);
        msg.timestamp = std::chrono::system_clock::now();
        msg.content.push_back(cc::core::TextBlock{std::move(text)});
        return cc::core::Message{std::move(msg)};
    };

    for (int i = 0; i < 5; ++i) {
        engine.append_message_for_testing(make_user("legacy user context " + std::to_string(i) + std::string(160, 'u')));
        engine.append_message_for_testing(make_assistant("legacy assistant context " + std::to_string(i) + std::string(160, 'a')));
    }

    auto compacted = engine.compact_conversation("auto");
    ASSERT_TRUE(compacted.has_value());

    auto conversation = engine.get_conversation();
    auto boundary_it = std::ranges::find_if(conversation, [](const cc::core::Message& message) {
        const auto* system = std::get_if<cc::core::SystemMessage>(&message);
        return system && system->subtype && *system->subtype == "compact_boundary";
    });
    ASSERT_NE(boundary_it, conversation.end());
    const auto* boundary = std::get_if<cc::core::SystemMessage>(&*boundary_it);
    ASSERT_NE(boundary, nullptr);
    ASSERT_TRUE(boundary->compact_metadata.has_value());
    const auto pre_tokens = boundary->compact_metadata->pre_tokens;
    ASSERT_GT(pre_tokens, 0u);
    ASSERT_LT(pre_tokens, 10'000u);

    auto response = engine.query("continue after compact");
    ASSERT_TRUE(response.has_value()) << response.error().message;

    auto request_body = server.wait_for_body();
    ASSERT_TRUE(request_body.has_value());
    auto parsed = cc::utils::json::parse(*request_body);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
    auto task_budget = parsed->root().get("output_config").get("task_budget");
    ASSERT_TRUE(task_budget.is_obj()) << *request_body;
    EXPECT_EQ(task_budget.get("total").as_int(), 10'000);
    EXPECT_EQ(task_budget.get("remaining").as_int(), 10'000 - pre_tokens);

    fs::remove_all(root);
}

TEST(QueryEngine, RestoreConversationDerivesTaskBudgetRemainingFromCompactBoundaryMetadata) {
    auto root = fs::temp_directory_path() / "cc-repl-query-task-budget-restore-test";
    fs::remove_all(root);
    fs::create_directories(root);

    cc::core::ToolRegistry registry;
    cc::core::QueryEngineConfig config;
    config.context_window.auto_compact = false;
    config.cwd = root.string();
    config.thinking_config.mode = cc::core::ThinkingConfig::Mode::Disabled;
    config.task_budget = cc::core::QueryEngineConfig::TaskBudget{
        .total = 10'000,
        .remaining = std::nullopt,
    };

    cc::core::QueryEngine engine(std::move(config), registry);

    auto make_user = [](std::string text) {
        cc::core::UserMessage msg{};
        msg.id.value = "restore-user-" + text.substr(0, 12);
        msg.timestamp = std::chrono::system_clock::now();
        msg.content.push_back(cc::core::TextBlock{std::move(text)});
        return cc::core::Message{std::move(msg)};
    };
    auto make_assistant = [](std::string text) {
        cc::core::AssistantMessage msg{};
        msg.id.value = "restore-assistant-" + text.substr(0, 12);
        msg.timestamp = std::chrono::system_clock::now();
        msg.content.push_back(cc::core::TextBlock{std::move(text)});
        return cc::core::Message{std::move(msg)};
    };

    for (int i = 0; i < 5; ++i) {
        engine.append_message_for_testing(make_user("restored legacy user context " + std::to_string(i) + std::string(160, 'u')));
        engine.append_message_for_testing(make_assistant("restored legacy assistant context " + std::to_string(i) + std::string(160, 'a')));
    }

    auto compacted = engine.compact_conversation("auto");
    ASSERT_TRUE(compacted.has_value());

    auto restored_messages = engine.get_conversation();
    auto boundary_it = std::ranges::find_if(restored_messages, [](const cc::core::Message& message) {
        const auto* system = std::get_if<cc::core::SystemMessage>(&message);
        return system && system->subtype && *system->subtype == "compact_boundary";
    });
    ASSERT_NE(boundary_it, restored_messages.end());
    const auto* boundary = std::get_if<cc::core::SystemMessage>(&*boundary_it);
    ASSERT_NE(boundary, nullptr);
    ASSERT_TRUE(boundary->compact_metadata.has_value());
    const auto pre_tokens = boundary->compact_metadata->pre_tokens;
    ASSERT_GT(pre_tokens, 0u);
    ASSERT_LT(pre_tokens, 10'000u);

    LocalAnthropicMessagesServer resumed_server;
    ASSERT_NE(resumed_server.port(), 0);
    cc::core::ToolRegistry restored_registry;
    cc::core::QueryEngineConfig restored_config;
    restored_config.api_key = "test-key";
    restored_config.base_url = resumed_server.base_url();
    restored_config.context_window.auto_compact = false;
    restored_config.cwd = root.string();
    restored_config.retry_policy.max_retries = 0;
    restored_config.thinking_config.mode = cc::core::ThinkingConfig::Mode::Disabled;
    restored_config.task_budget = cc::core::QueryEngineConfig::TaskBudget{
        .total = 10'000,
        .remaining = std::nullopt,
    };

    cc::core::QueryEngine restored_engine(std::move(restored_config), restored_registry);
    restored_engine.restore_conversation(std::move(restored_messages));

    auto response = restored_engine.query("continue after restore");
    ASSERT_TRUE(response.has_value()) << response.error().message;

    auto request_body = resumed_server.wait_for_body();
    ASSERT_TRUE(request_body.has_value());
    auto parsed = cc::utils::json::parse(*request_body);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
    auto task_budget = parsed->root().get("output_config").get("task_budget");
    ASSERT_TRUE(task_budget.is_obj()) << *request_body;
    EXPECT_EQ(task_budget.get("total").as_int(), 10'000);
    EXPECT_EQ(task_budget.get("remaining").as_int(), 10'000 - pre_tokens);

    fs::remove_all(root);
}

TEST(QueryEngine, RepeatedCompactDoesNotSummarizePriorCompactBoundaries) {
    cc::core::ToolRegistry registry;
    cc::core::QueryEngineConfig config;
    config.context_window.auto_compact = false;
    config.cwd = fs::temp_directory_path().string();

    cc::core::QueryEngine engine(std::move(config), registry);

    auto make_user = [](std::string text) {
        cc::core::UserMessage msg{};
        msg.id.value = "repeat-user-" + text;
        msg.timestamp = std::chrono::system_clock::now();
        msg.content.push_back(cc::core::TextBlock{std::move(text)});
        return cc::core::Message{std::move(msg)};
    };
    auto make_assistant = [](std::string text) {
        cc::core::AssistantMessage msg{};
        msg.id.value = "repeat-assistant-" + text;
        msg.timestamp = std::chrono::system_clock::now();
        msg.content.push_back(cc::core::TextBlock{std::move(text)});
        return cc::core::Message{std::move(msg)};
    };

    for (int i = 0; i < 10; ++i) {
        engine.append_message_for_testing(i % 2 == 0
            ? make_user("initial " + std::to_string(i))
            : make_assistant("initial " + std::to_string(i)));
    }

    auto first = engine.compact_conversation();
    ASSERT_TRUE(first.has_value());

    for (int i = 0; i < 6; ++i) {
        engine.append_message_for_testing(i % 2 == 0
            ? make_user("second wave " + std::to_string(i))
            : make_assistant("second wave " + std::to_string(i)));
    }

    auto second = engine.compact_conversation();
    ASSERT_TRUE(second.has_value());

    auto conversation = engine.get_conversation();
    auto boundary_count = std::ranges::count_if(conversation, [](const cc::core::Message& message) {
        const auto* system = std::get_if<cc::core::SystemMessage>(&message);
        return system && system->subtype && *system->subtype == "compact_boundary";
    });
    EXPECT_EQ(boundary_count, 1);

    const auto* marker = std::get_if<cc::core::UserMessage>(&conversation.at(2));
    ASSERT_NE(marker, nullptr);
    ASSERT_EQ(marker->content.size(), 1u);
    const auto* summary = std::get_if<cc::core::TextBlock>(&marker->content.front());
    ASSERT_NE(summary, nullptr);
    EXPECT_EQ(summary->text.find("Conversation compacted by manual compact."), std::string::npos);
    EXPECT_NE(summary->text.find("initial 0"), std::string::npos);

    bool kept_second_wave = false;
    for (const auto& message : conversation) {
        const auto* user = std::get_if<cc::core::UserMessage>(&message);
        if (!user || user->content.empty()) continue;
        const auto* text = std::get_if<cc::core::TextBlock>(&user->content.front());
        if (text && text->text == "second wave 0") {
            kept_second_wave = true;
            break;
        }
    }
    EXPECT_TRUE(kept_second_wave);
}

TEST(QueryEngine, AutoCompactWritesBoundaryMetadataAndKeepsRecentTail) {
    cc::core::ToolRegistry registry;
    cc::core::QueryEngineConfig config;
    config.context_window.auto_compact = true;
    config.context_window.max_context_tokens = 2000;
    config.context_window.compaction_threshold = 0.05;
    config.cwd = fs::temp_directory_path().string();

    cc::core::QueryEngine engine(std::move(config), registry);

    auto make_user = [](int index) {
        cc::core::UserMessage msg{};
        msg.id.value = "auto-user-" + std::to_string(index);
        msg.timestamp = std::chrono::system_clock::now();
        msg.content.push_back(cc::core::TextBlock{
            "auto compact payload " + std::to_string(index) + " " + std::string(900, 'x')});
        return cc::core::Message{std::move(msg)};
    };

    for (int i = 0; i < 12; ++i) {
        engine.append_message_for_testing(make_user(i));
    }

    auto conversation = engine.get_conversation();
    auto boundary_count = std::ranges::count_if(conversation, [](const cc::core::Message& message) {
        const auto* system = std::get_if<cc::core::SystemMessage>(&message);
        return system && system->subtype && *system->subtype == "compact_boundary";
    });
    ASSERT_EQ(boundary_count, 1);
    ASSERT_LE(conversation.size(), 9u);

    auto boundary_it = std::ranges::find_if(conversation, [](const cc::core::Message& message) {
        const auto* system = std::get_if<cc::core::SystemMessage>(&message);
        return system && system->subtype && *system->subtype == "compact_boundary";
    });
    ASSERT_NE(boundary_it, conversation.end());
    const auto boundary_index = static_cast<std::size_t>(std::distance(conversation.begin(), boundary_it));

    const auto* boundary = std::get_if<cc::core::SystemMessage>(&*boundary_it);
    ASSERT_NE(boundary, nullptr);
    ASSERT_TRUE(boundary->compact_metadata.has_value());
    EXPECT_EQ(boundary->compact_metadata->trigger, "auto");
    EXPECT_GT(boundary->compact_metadata->pre_tokens, 0u);
    ASSERT_TRUE(boundary->compact_metadata->preserved_segment.has_value());

    ASSERT_LT(boundary_index + 1, conversation.size());
    const auto* marker = std::get_if<cc::core::UserMessage>(&conversation[boundary_index + 1]);
    ASSERT_NE(marker, nullptr);
    EXPECT_EQ(boundary->compact_metadata->preserved_segment->anchor_uuid, marker->id.value);

    const auto last_id = std::visit([](const auto& msg) { return msg.id.value; }, conversation.back());
    EXPECT_EQ(boundary->compact_metadata->preserved_segment->tail_uuid, last_id);

    ASSERT_EQ(marker->content.size(), 1u);
    const auto* summary = std::get_if<cc::core::TextBlock>(&marker->content.front());
    ASSERT_NE(summary, nullptr);
    EXPECT_NE(summary->text.find("Preserve these details"), std::string::npos);
    EXPECT_NE(summary->text.find("auto compact payload"), std::string::npos);
}

TEST(QueryEngine, ReactiveCompactRetriesPromptTooLongAfterWritingBoundary) {
    LocalAnthropicMessagesServer server(
        {
            R"({"error":{"type":"invalid_request_error","message":"prompt_too_long"}})",
            R"({"id":"msg_reactive","type":"message","role":"assistant","model":"claude-test","content":[{"type":"text","text":"reactive-ok"}],"stop_reason":"end_turn","usage":{"input_tokens":2,"output_tokens":3}})",
        },
        {413, 200});
    ASSERT_NE(server.port(), 0);

    cc::core::ToolRegistry registry;
    cc::core::QueryEngineConfig config;
    config.api_key = "test-key";
    config.base_url = server.base_url();
    config.context_window.auto_compact = false;
    config.cwd = fs::temp_directory_path().string();
    config.retry_policy.max_retries = 0;
    config.thinking_config.mode = cc::core::ThinkingConfig::Mode::Disabled;

    cc::core::QueryEngine engine(std::move(config), registry);

    for (int i = 0; i < 10; ++i) {
        cc::core::UserMessage msg{};
        msg.id.value = "reactive-user-" + std::to_string(i);
        msg.timestamp = std::chrono::system_clock::now();
        msg.content.push_back(cc::core::TextBlock{
            "reactive legacy " + std::to_string(i) + " " + std::string(600, 'r')});
        engine.append_message_for_testing(cc::core::Message{std::move(msg)});
    }

    auto response = engine.query("trigger reactive compact");
    ASSERT_TRUE(response.has_value()) << response.error().message;
    ASSERT_FALSE(response->message.content.empty());
    const auto* response_text = std::get_if<cc::core::TextBlock>(&response->message.content.front());
    ASSERT_NE(response_text, nullptr);
    EXPECT_EQ(response_text->text, "reactive-ok");

    auto request_bodies = server.wait_for_bodies(2);
    ASSERT_TRUE(request_bodies.has_value());
    ASSERT_EQ(request_bodies->size(), 2u);

    auto count_messages = [](std::string_view body) {
        auto parsed = cc::utils::json::parse(body);
        if (!parsed) return std::size_t{0};
        std::size_t count = 0;
        auto messages = parsed->root().get("messages");
        if (messages.is_arr()) {
            messages.iter([&](cc::utils::json::JsonVal) { ++count; });
        }
        return count;
    };

    const auto first_count = count_messages(request_bodies->front());
    const auto second_count = count_messages(request_bodies->back());
    EXPECT_GT(first_count, second_count);
    EXPECT_NE(request_bodies->back().find("Preserve these details"), std::string::npos);
    EXPECT_EQ(request_bodies->back().find("compact_boundary"), std::string::npos);

    auto conversation = engine.get_conversation();
    auto boundary_it = std::ranges::find_if(conversation, [](const cc::core::Message& message) {
        const auto* system = std::get_if<cc::core::SystemMessage>(&message);
        return system && system->subtype && *system->subtype == "compact_boundary";
    });
    ASSERT_NE(boundary_it, conversation.end());
    const auto* boundary = std::get_if<cc::core::SystemMessage>(&*boundary_it);
    ASSERT_NE(boundary, nullptr);
    ASSERT_TRUE(boundary->compact_metadata.has_value());
    EXPECT_EQ(boundary->compact_metadata->trigger, "reactive");
}

TEST(QueryEngine, AppliesMainThreadToolResultBudgetBeforeModelRequest) {
    LocalAnthropicMessagesServer server;
    ASSERT_NE(server.port(), 0);

    auto root = fs::temp_directory_path() / "cc-repl-query-tool-result-budget-test";
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard runtime_dir_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "runtime").string());

    cc::core::ToolRegistry registry;
    registry.register_tool(std::make_unique<DefinitionOnlyTool>(cc::core::ToolDefinition{
        .name = "Bash",
        .description = "Execute shell",
        .input_schema = {},
        .permission = cc::core::ToolPermission::Execute,
        .max_result_size_chars = 30'000,
    }));
    registry.register_tool(std::make_unique<DefinitionOnlyTool>(cc::core::ToolDefinition{
        .name = "Read",
        .description = "Read file",
        .input_schema = {},
        .permission = cc::core::ToolPermission::ReadOnly,
        .max_result_size_chars = 0,
        .max_result_size_unbounded = true,
    }));

    cc::core::QueryEngineConfig config;
    config.api_key = "test-key";
    config.base_url = server.base_url();
    config.context_window.auto_compact = false;
    config.cwd = root.string();
    config.retry_policy.max_retries = 0;
    config.thinking_config.mode = cc::core::ThinkingConfig::Mode::Disabled;

    cc::core::QueryEngine engine(std::move(config), registry);

    cc::core::AssistantMessage assistant{};
    assistant.id.value = "assistant-tool-uses";
    assistant.timestamp = std::chrono::system_clock::now();
    assistant.content.push_back(cc::core::ToolUseBlock{
        .id = cc::core::ToolUseId{"bash-large-1"},
        .name = "Bash",
        .input_json = R"({"command":"one"})",
    });
    assistant.content.push_back(cc::core::ToolUseBlock{
        .id = cc::core::ToolUseId{"bash-large-2"},
        .name = "Bash",
        .input_json = R"({"command":"two"})",
    });
    assistant.content.push_back(cc::core::ToolUseBlock{
        .id = cc::core::ToolUseId{"read-unbounded"},
        .name = "Read",
        .input_json = R"({"file_path":"huge.txt"})",
    });
    engine.append_message_for_testing(cc::core::Message{std::move(assistant)});

    auto append_tool_result = [&](std::string id, std::string text) {
        cc::core::ToolResultMessage result{};
        result.id.value = "result-" + id;
        result.timestamp = std::chrono::system_clock::now();
        result.tool_use_id = cc::core::ToolUseId{std::move(id)};
        result.content.push_back(cc::core::TextBlock{std::move(text)});
        engine.append_message_for_testing(cc::core::Message{std::move(result)});
    };

    append_tool_result("bash-large-1", std::string(170'000, 'b'));
    append_tool_result("bash-large-2", std::string(160'000, 'c'));
    append_tool_result("read-unbounded", std::string(250'000, 'r'));

    auto response = engine.query("continue after tools");
    ASSERT_TRUE(response.has_value()) << response.error().message;

    auto request_body = server.wait_for_body();
    ASSERT_TRUE(request_body.has_value());
    EXPECT_NE(request_body->find("<persisted-output>"), std::string::npos);
    EXPECT_NE(request_body->find("Output too large (170000 bytes)"), std::string::npos);
    EXPECT_EQ(request_body->find(std::string(2'500, 'b')), std::string::npos);
    EXPECT_NE(request_body->find(std::string(2'500, 'c')), std::string::npos);
    EXPECT_NE(request_body->find(std::string(2'500, 'r')), std::string::npos);

    auto persisted_dir = root / "runtime" / "tool-results";
    ASSERT_TRUE(fs::exists(persisted_dir));
    bool saw_persisted_file = false;
    for (const auto& entry : fs::directory_iterator(persisted_dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().filename().string().find("bash-large-1") != std::string::npos) {
            saw_persisted_file = true;
            EXPECT_EQ(fs::file_size(entry.path()), 170'000u);
        }
    }
    EXPECT_TRUE(saw_persisted_file);

    const auto conversation = engine.get_conversation();
    auto replacement_for = [](const std::vector<cc::core::Message>& messages, std::string_view tool_use_id) {
        for (const auto& message : messages) {
            const auto* tool_result = std::get_if<cc::core::ToolResultMessage>(&message);
            if (!tool_result || tool_result->tool_use_id.value != tool_use_id) continue;
            if (tool_result->content.empty()) return std::optional<std::string>{};
            const auto* text = std::get_if<cc::core::TextBlock>(&tool_result->content.front());
            if (!text) return std::optional<std::string>{};
            return std::optional<std::string>{text->text};
        }
        return std::optional<std::string>{};
    };
    auto original_replacement = replacement_for(conversation, "bash-large-1");
    ASSERT_TRUE(original_replacement.has_value());
    ASSERT_NE(original_replacement->find("<persisted-output>"), std::string::npos);

    auto storage_path = root / "history.json";
    {
        cc::core::ConversationStore store(storage_path.string());
        auto* stored = store.create_conversation();
        for (const auto& message : conversation) {
            stored->add_message(message);
        }
        ASSERT_TRUE(store.save_all().has_value());
    }

    cc::core::ConversationStore loaded(storage_path.string());
    ASSERT_TRUE(loaded.load_all().has_value());
    auto restored_messages = loaded.get_active_conversation()->get_messages();
    auto restored_replacement = replacement_for(restored_messages, "bash-large-1");
    ASSERT_TRUE(restored_replacement.has_value());
    EXPECT_EQ(*restored_replacement, *original_replacement);

    LocalAnthropicMessagesServer resumed_server;
    ASSERT_NE(resumed_server.port(), 0);
    cc::core::ToolRegistry restored_registry;
    restored_registry.register_tool(std::make_unique<DefinitionOnlyTool>(cc::core::ToolDefinition{
        .name = "Bash",
        .description = "Execute shell",
        .input_schema = {},
        .permission = cc::core::ToolPermission::Execute,
        .max_result_size_chars = 30'000,
    }));
    restored_registry.register_tool(std::make_unique<DefinitionOnlyTool>(cc::core::ToolDefinition{
        .name = "Read",
        .description = "Read file",
        .input_schema = {},
        .permission = cc::core::ToolPermission::ReadOnly,
        .max_result_size_chars = 0,
        .max_result_size_unbounded = true,
    }));

    cc::core::QueryEngineConfig restored_config;
    restored_config.api_key = "test-key";
    restored_config.base_url = resumed_server.base_url();
    restored_config.context_window.auto_compact = false;
    restored_config.cwd = root.string();
    restored_config.retry_policy.max_retries = 0;
    restored_config.thinking_config.mode = cc::core::ThinkingConfig::Mode::Disabled;

    cc::core::QueryEngine restored_engine(std::move(restored_config), restored_registry);
    restored_engine.restore_conversation(std::move(restored_messages));
    auto resumed_response = restored_engine.query("after resume");
    ASSERT_TRUE(resumed_response.has_value()) << resumed_response.error().message;

    auto resumed_body = resumed_server.wait_for_body();
    ASSERT_TRUE(resumed_body.has_value());
    EXPECT_NE(resumed_body->find("<persisted-output>"), std::string::npos);
    EXPECT_NE(resumed_body->find("Output too large (170000 bytes)"), std::string::npos);
    EXPECT_EQ(resumed_body->find(std::string(2'500, 'b')), std::string::npos);
    EXPECT_NE(resumed_body->find(std::string(2'500, 'c')), std::string::npos);
    EXPECT_NE(resumed_body->find(std::string(2'500, 'r')), std::string::npos);

    fs::remove_all(root);
}

TEST(QueryEngine, TimeBasedMicrocompactClearsOldCompactableToolResultsBeforeRequest) {
    EnvironmentGuard enable_guard("CC_REPL_TIME_BASED_MICROCOMPACT", "1");
    EnvironmentGuard gap_guard("CC_REPL_TIME_BASED_MICROCOMPACT_GAP_MINUTES", "30");
    EnvironmentGuard keep_guard("CC_REPL_TIME_BASED_MICROCOMPACT_KEEP_RECENT", "1");

    LocalAnthropicMessagesServer server;
    ASSERT_NE(server.port(), 0);

    auto root = fs::temp_directory_path() / "cc-repl-query-time-based-microcompact-test";
    fs::remove_all(root);
    fs::create_directories(root);

    cc::core::ToolRegistry registry;
    cc::core::QueryEngineConfig config;
    config.api_key = "test-key";
    config.base_url = server.base_url();
    config.context_window.auto_compact = false;
    config.cwd = root.string();
    config.retry_policy.max_retries = 0;
    config.thinking_config.mode = cc::core::ThinkingConfig::Mode::Disabled;

    cc::core::QueryEngine engine(std::move(config), registry);

    const auto old_time = std::chrono::system_clock::now() - std::chrono::hours(2);
    cc::core::AssistantMessage assistant{};
    assistant.id.value = "assistant-old-tool-uses";
    assistant.timestamp = old_time;
    assistant.content.push_back(cc::core::ToolUseBlock{
        .id = cc::core::ToolUseId{"read-old"},
        .name = "Read",
        .input_json = R"({"file_path":"old.txt"})",
    });
    assistant.content.push_back(cc::core::ToolUseBlock{
        .id = cc::core::ToolUseId{"bash-old"},
        .name = "Bash",
        .input_json = R"({"command":"old"})",
    });
    assistant.content.push_back(cc::core::ToolUseBlock{
        .id = cc::core::ToolUseId{"task-noncompact"},
        .name = "Task",
        .input_json = R"({"description":"noncompact"})",
    });
    assistant.content.push_back(cc::core::ToolUseBlock{
        .id = cc::core::ToolUseId{"edit-recent"},
        .name = "Edit",
        .input_json = R"({"file_path":"recent.txt"})",
    });
    engine.append_message_for_testing(cc::core::Message{std::move(assistant)});

    auto append_tool_result = [&](std::string id, std::string text) {
        cc::core::ToolResultMessage result{};
        result.id.value = "result-" + id;
        result.timestamp = old_time;
        result.tool_use_id = cc::core::ToolUseId{std::move(id)};
        result.content.push_back(cc::core::TextBlock{std::move(text)});
        engine.append_message_for_testing(cc::core::Message{std::move(result)});
    };

    append_tool_result("read-old", "OLD_READ_RESULT_SHOULD_CLEAR");
    append_tool_result("bash-old", "OLD_BASH_RESULT_SHOULD_CLEAR");
    append_tool_result("task-noncompact", "NONCOMPACT_TASK_RESULT_SHOULD_STAY");
    append_tool_result("edit-recent", "RECENT_EDIT_RESULT_SHOULD_STAY");

    auto response = engine.query("continue after cold cache");
    ASSERT_TRUE(response.has_value()) << response.error().message;

    auto request_body = server.wait_for_body();
    ASSERT_TRUE(request_body.has_value());
    EXPECT_EQ(request_body->find("OLD_READ_RESULT_SHOULD_CLEAR"), std::string::npos)
        << *request_body;
    EXPECT_EQ(request_body->find("OLD_BASH_RESULT_SHOULD_CLEAR"), std::string::npos)
        << *request_body;
    EXPECT_NE(request_body->find("[Old tool result content cleared]"), std::string::npos)
        << *request_body;
    EXPECT_NE(request_body->find("NONCOMPACT_TASK_RESULT_SHOULD_STAY"), std::string::npos)
        << *request_body;
    EXPECT_NE(request_body->find("RECENT_EDIT_RESULT_SHOULD_STAY"), std::string::npos)
        << *request_body;

    auto conversation = engine.get_conversation();
    auto tool_result_text = [&](std::string_view tool_use_id) -> std::optional<std::string> {
        for (const auto& message : conversation) {
            const auto* result = std::get_if<cc::core::ToolResultMessage>(&message);
            if (!result || result->tool_use_id.value != tool_use_id) continue;
            if (result->content.empty()) return std::nullopt;
            const auto* text = std::get_if<cc::core::TextBlock>(&result->content.front());
            if (!text) return std::nullopt;
            return text->text;
        }
        return std::nullopt;
    };
    EXPECT_EQ(tool_result_text("read-old"), std::optional<std::string>{"[Old tool result content cleared]"});
    EXPECT_EQ(tool_result_text("bash-old"), std::optional<std::string>{"[Old tool result content cleared]"});
    EXPECT_EQ(tool_result_text("task-noncompact"), std::optional<std::string>{"NONCOMPACT_TASK_RESULT_SHOULD_STAY"});
    EXPECT_EQ(tool_result_text("edit-recent"), std::optional<std::string>{"RECENT_EDIT_RESULT_SHOULD_STAY"});

    fs::remove_all(root);
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

TEST(ApiClient, RequestSerializerSerializesEffortConfig) {
    cc::services::api::CreateMessageRequest request;
    request.model = "claude-test";
    request.messages.push_back(cc::services::api::Message::from_text("user", "hello"));
    request.output_effort = "high";
    request.task_budget = cc::services::api::TaskBudget{
        .total = 12000,
        .remaining = 3456,
    };
    request.internal_effort_override = 77;

    auto serialized = cc::services::api::RequestSerializer::serialize(request);
    auto parsed = cc::utils::json::parse(serialized);

    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
    auto output_config = parsed->root().get("output_config");
    ASSERT_TRUE(output_config.is_obj());
    EXPECT_EQ(output_config.get("effort").as_str(), "high");
    auto task_budget = output_config.get("task_budget");
    ASSERT_TRUE(task_budget.is_obj());
    EXPECT_EQ(task_budget.get("type").as_str(), "tokens");
    EXPECT_EQ(task_budget.get("total").as_int(), 12000);
    EXPECT_EQ(task_budget.get("remaining").as_int(), 3456);
    auto anthropic_internal = parsed->root().get("anthropic_internal");
    ASSERT_TRUE(anthropic_internal.is_obj());
    EXPECT_EQ(anthropic_internal.get("effort_override").as_int(), 77);
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

TEST(IdeIntegration, DiscoversVSCodeWorkspaceMcpServersFromObjectConfig) {
    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("cc_repl_vscode_mcp_workspace_" + std::to_string(suffix));
    const auto workspace = root / "workspace";
    const auto home = root / "home";
    fs::create_directories(workspace / ".vscode");
    fs::create_directories(home);
    EnvironmentGuard home_guard("HOME", home.string());

    {
        std::ofstream config(workspace / ".vscode" / "mcp.json");
        config << R"JSON({
  "servers": {
    "workspace-stdio": {
      "type": "stdio",
      "command": "node",
      "args": ["server.js"]
    },
    "workspace-sse": {
      "type": "sse",
      "url": "http://127.0.0.1:9012/sse"
    }
  },
  "mcpServers": {
    "legacy-stdio": {
      "command": "python"
    }
  }
})JSON";
    }

    auto servers = cc::services::mcp::discover_vscode_mcp_servers(workspace.string());
    auto find_server = [&servers](std::string_view name) {
        return std::find_if(servers.begin(), servers.end(), [name](const auto& server) {
            return server.name == name;
        });
    };

    auto stdio = find_server("workspace-stdio");
    ASSERT_NE(stdio, servers.end());
    EXPECT_EQ(stdio->transport_type, "stdio");
    EXPECT_EQ(stdio->connection_string, "node");
    EXPECT_TRUE(cc::services::mcp::connect_vscode_mcp(*stdio));

    auto sse = find_server("workspace-sse");
    ASSERT_NE(sse, servers.end());
    EXPECT_EQ(sse->transport_type, "sse");
    EXPECT_EQ(sse->connection_string, "http://127.0.0.1:9012/sse");
    EXPECT_TRUE(cc::services::mcp::connect_vscode_mcp(*sse));

    auto legacy = find_server("legacy-stdio");
    ASSERT_NE(legacy, servers.end());
    EXPECT_EQ(legacy->transport_type, "stdio");
    EXPECT_EQ(legacy->connection_string, "python");

    fs::remove_all(root);
}

TEST(IdeIntegration, DiscoversVSCodeExtensionContributedMcpServers) {
    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("cc_repl_vscode_mcp_extension_" + std::to_string(suffix));
    const auto workspace = root / "workspace";
    const auto home = root / "home";
    const auto extension = home / ".vscode" / "extensions" / "publisher.fixture-1.0.0";
    fs::create_directories(workspace);
    fs::create_directories(extension);
    EnvironmentGuard home_guard("HOME", home.string());

    {
        std::ofstream package(extension / "package.json");
        package << R"JSON({
  "name": "fixture-extension",
  "contributes": {
    "mcp": {
      "servers": {
        "extension-stdio": {
          "command": "node"
        },
        "extension-ws": {
          "url": "ws://127.0.0.1:8020/mcp"
        }
      }
    }
  }
})JSON";
    }

    auto servers = cc::services::mcp::discover_vscode_mcp_servers(workspace.string());
    auto find_server = [&servers](std::string_view name) {
        return std::find_if(servers.begin(), servers.end(), [name](const auto& server) {
            return server.name == name;
        });
    };

    auto stdio = find_server("extension-stdio");
    ASSERT_NE(stdio, servers.end());
    EXPECT_EQ(stdio->transport_type, "stdio");
    EXPECT_EQ(stdio->connection_string, "node");
    EXPECT_TRUE(cc::services::mcp::connect_vscode_mcp(*stdio));

    auto ws = find_server("extension-ws");
    ASSERT_NE(ws, servers.end());
    EXPECT_EQ(ws->transport_type, "ws");
    EXPECT_EQ(ws->connection_string, "ws://127.0.0.1:8020/mcp");
    EXPECT_TRUE(cc::services::mcp::connect_vscode_mcp(*ws));

    fs::remove_all(root);
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

TEST(McpClient, SseReconnectResumesWithLastEventId) {
    LocalReconnectSseStreamServer server;
    ASSERT_TRUE(server.ready());

    cc::services::mcp::SseTransport::ReconnectPolicy policy{
        .initial_delay = std::chrono::milliseconds{10},
        .max_delay = std::chrono::milliseconds{20},
        .backoff_multiplier = 2.0,
        .jitter_factor = 0.0,
        .max_retries = 5,
        .liveness_timeout = std::chrono::seconds{1},
    };
    cc::services::mcp::SseTransport transport(server.url(), {}, policy);

    auto started = transport.start();
    ASSERT_TRUE(started.has_value()) << static_cast<int>(started.error());
    ASSERT_TRUE(server.wait_for_stream_requests(2));
    transport.close();

    const auto last_event_ids = server.stream_last_event_ids();
    ASSERT_GE(last_event_ids.size(), 2u);
    EXPECT_TRUE(last_event_ids.front().empty());
    EXPECT_EQ(last_event_ids[1], "1");
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

TEST(McpConnectionManager, RefreshesExpiredOAuthTokenBeforeRemoteConnection) {
    LocalRefreshingStreamableHttpMcpServer server;
    ASSERT_TRUE(server.ready());

    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("cc_repl_mcp_refresh_" + std::to_string(suffix));
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard xdg_config_guard("XDG_CONFIG_HOME", root.string());

    cc::services::mcp::ServerConfig server_config;
    server_config.name = "refresh-fixture";
    server_config.transport = cc::services::mcp::TransportType::StreamableHttp;
    server_config.url = server.mcp_url();
    server_config.enabled = true;
    server_config.auto_start = true;
    server_config.oauth = cc::services::mcp::McpOAuthConfig{
        .auth_server_metadata_url = server.metadata_url(),
        .client_id = "client-1",
    };

    cc::services::mcp::McpServerConfig auth_config;
    auth_config.type = "http";
    auth_config.url = server_config.url;
    auth_config.oauth = server_config.oauth;
    const auto server_key = cc::services::mcp::get_server_key(server_config.name, auth_config);
    auto sanitize_key = [](std::string_view key) {
        std::string sanitized;
        sanitized.reserve(key.size());
        for (unsigned char ch : key) {
            sanitized.push_back(std::isalnum(ch) ? static_cast<char>(ch) : '_');
        }
        return sanitized;
    };
    const auto token_path = root / "cc-repl" / "mcp" / (sanitize_key(server_key) + ".json");
    fs::create_directories(token_path.parent_path());
    const auto expired_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 60;
    {
        std::ofstream token_file(token_path);
        token_file << std::format(R"({{
          "server_name": "refresh-fixture",
          "server_url": "{}",
          "access_token": "old-access",
          "refresh_token": "old-refresh",
          "expires_at": {},
          "scope": "tools",
          "client_id": "client-1",
          "client_secret": "",
          "discovery_state": {{}}
        }})", server.mcp_url(), expired_at);
    }

    cc::services::mcp::ConnectionManagerConfig manager_config;
    manager_config.config_directory = root;
    manager_config.connection_timeout = std::chrono::milliseconds{2000};
    manager_config.auto_connect_on_start = false;

    cc::services::mcp::McpConnectionManager manager(std::move(manager_config));

    cc::services::mcp::McpConfig mcp_config;
    mcp_config.servers.emplace(server_config.name, std::move(server_config));
    manager.set_configuration(std::move(mcp_config));

    auto connected = manager.connect_server("refresh-fixture");
    ASSERT_TRUE(connected.has_value()) << static_cast<int>(connected.error());
    ASSERT_TRUE(server.wait_for_token_request());
    EXPECT_NE(server.token_request_body().find("grant_type=refresh_token"), std::string::npos);
    EXPECT_NE(server.token_request_body().find("refresh_token=old-refresh"), std::string::npos);

    auto snapshot = manager.snapshot_server("refresh-fixture");
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->status, cc::services::mcp::ConnectionStatus::Connected);
    ASSERT_EQ(snapshot->tools.size(), 1u);
    EXPECT_EQ(snapshot->tools.front().name, "refresh_lookup");

    const auto auth_headers = server.mcp_authorization_headers();
    ASSERT_FALSE(auth_headers.empty());
    EXPECT_TRUE(std::ranges::all_of(auth_headers, [](const auto& header) {
        return header == "Bearer fresh-access";
    }));

    auto persisted = cc::utils::json::parse_file(token_path);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(persisted->root().get_string("access_token"), "fresh-access");
    EXPECT_EQ(persisted->root().get_string("refresh_token"), "fresh-refresh");

    manager.shutdown();
    fs::remove_all(root);
}

TEST(McpConnectionManager, MarksRefreshFailureAsNeedsAuthWithoutRemoteConnect) {
    LocalRefreshingStreamableHttpMcpServer server(/*fail_refresh=*/true);
    ASSERT_TRUE(server.ready());

    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("cc_repl_mcp_refresh_failure_" + std::to_string(suffix));
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard xdg_config_guard("XDG_CONFIG_HOME", root.string());

    cc::services::mcp::ServerConfig server_config;
    server_config.name = "refresh-failure-fixture";
    server_config.transport = cc::services::mcp::TransportType::StreamableHttp;
    server_config.url = server.mcp_url();
    server_config.enabled = true;
    server_config.auto_start = true;
    server_config.oauth = cc::services::mcp::McpOAuthConfig{
        .auth_server_metadata_url = server.metadata_url(),
        .client_id = "client-1",
    };

    cc::services::mcp::McpServerConfig auth_config;
    auth_config.type = "http";
    auth_config.url = server_config.url;
    auth_config.oauth = server_config.oauth;
    const auto server_key = cc::services::mcp::get_server_key(server_config.name, auth_config);
    auto sanitize_key = [](std::string_view key) {
        std::string sanitized;
        sanitized.reserve(key.size());
        for (unsigned char ch : key) {
            sanitized.push_back(std::isalnum(ch) ? static_cast<char>(ch) : '_');
        }
        return sanitized;
    };
    const auto token_path = root / "cc-repl" / "mcp" / (sanitize_key(server_key) + ".json");
    fs::create_directories(token_path.parent_path());
    const auto expired_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 60;
    {
        std::ofstream token_file(token_path);
        token_file << std::format(R"({{
          "server_name": "refresh-failure-fixture",
          "server_url": "{}",
          "access_token": "old-access",
          "refresh_token": "old-refresh",
          "expires_at": {},
          "scope": "tools",
          "client_id": "client-1",
          "client_secret": "",
          "discovery_state": {{}}
        }})", server.mcp_url(), expired_at);
    }

    cc::services::mcp::ConnectionManagerConfig manager_config;
    manager_config.config_directory = root;
    manager_config.connection_timeout = std::chrono::milliseconds{2000};
    manager_config.auto_connect_on_start = false;

    cc::services::mcp::McpConnectionManager manager(std::move(manager_config));

    cc::services::mcp::McpConfig mcp_config;
    mcp_config.servers.emplace(server_config.name, std::move(server_config));
    manager.set_configuration(std::move(mcp_config));

    auto connected = manager.connect_server("refresh-failure-fixture");
    ASSERT_FALSE(connected.has_value());
    EXPECT_EQ(connected.error(), cc::services::mcp::McpClientError::Unauthorized);
    ASSERT_TRUE(server.wait_for_token_request());
    EXPECT_NE(server.token_request_body().find("grant_type=refresh_token"), std::string::npos);
    EXPECT_NE(server.token_request_body().find("refresh_token=old-refresh"), std::string::npos);

    auto snapshot = manager.snapshot_server("refresh-failure-fixture");
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->status, cc::services::mcp::ConnectionStatus::NeedsAuth);
    ASSERT_TRUE(snapshot->last_error.has_value());
    EXPECT_NE(snapshot->last_error->find("OAuth token refresh failed"), std::string::npos);
    EXPECT_TRUE(server.mcp_authorization_headers().empty());

    auto persisted = cc::utils::json::parse_file(token_path);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(persisted->root().get_string("access_token"), "old-access");
    EXPECT_EQ(persisted->root().get_string("refresh_token"), "old-refresh");

    manager.shutdown();
    fs::remove_all(root);
}

TEST(McpConnectionManager, MarksDiscoveryServerWithoutTokenAsNeedsAuthThenReconnectsWithStoredToken) {
    LocalRefreshingStreamableHttpMcpServer server;
    ASSERT_TRUE(server.ready());

    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("cc_repl_mcp_auth_needed_" + std::to_string(suffix));
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard xdg_config_guard("XDG_CONFIG_HOME", root.string());

    cc::services::mcp::ServerConfig server_config;
    server_config.name = "auth-needed-fixture";
    server_config.transport = cc::services::mcp::TransportType::StreamableHttp;
    server_config.url = server.mcp_url();
    server_config.enabled = true;
    server_config.auto_start = true;
    server_config.oauth = cc::services::mcp::McpOAuthConfig{
        .auth_server_metadata_url = server.metadata_url(),
        .client_id = "client-1",
    };

    cc::services::mcp::McpServerConfig auth_config;
    auth_config.type = "http";
    auth_config.url = server_config.url;
    auth_config.oauth = server_config.oauth;
    const auto server_key = cc::services::mcp::get_server_key(server_config.name, auth_config);
    auto sanitize_key = [](std::string_view key) {
        std::string sanitized;
        sanitized.reserve(key.size());
        for (unsigned char ch : key) {
            sanitized.push_back(std::isalnum(ch) ? static_cast<char>(ch) : '_');
        }
        return sanitized;
    };
    const auto token_path = root / "cc-repl" / "mcp" / (sanitize_key(server_key) + ".json");

    cc::services::mcp::ConnectionManagerConfig manager_config;
    manager_config.config_directory = root;
    manager_config.connection_timeout = std::chrono::milliseconds{2000};
    manager_config.auto_connect_on_start = false;

    cc::services::mcp::McpConnectionManager manager(std::move(manager_config));

    cc::services::mcp::McpConfig mcp_config;
    mcp_config.servers.emplace(server_config.name, std::move(server_config));
    manager.set_configuration(std::move(mcp_config));

    auto auth_needed = manager.connect_server("auth-needed-fixture");
    ASSERT_FALSE(auth_needed.has_value());
    EXPECT_EQ(auth_needed.error(), cc::services::mcp::McpClientError::Unauthorized);
    EXPECT_TRUE(server.mcp_authorization_headers().empty());
    EXPECT_TRUE(server.token_request_body().empty());

    auto snapshot = manager.snapshot_server("auth-needed-fixture");
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->status, cc::services::mcp::ConnectionStatus::NeedsAuth);
    ASSERT_TRUE(snapshot->last_error.has_value());
    EXPECT_NE(snapshot->last_error->find("MCP OAuth authentication required"), std::string::npos);

    fs::create_directories(token_path.parent_path());
    const auto expires_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() + 3600;
    {
        std::ofstream token_file(token_path);
        token_file << std::format(R"({{
          "server_name": "auth-needed-fixture",
          "server_url": "{}",
          "access_token": "fresh-access",
          "refresh_token": "",
          "expires_at": {},
          "scope": "tools",
          "client_id": "client-1",
          "client_secret": "",
          "discovery_state": {{}}
        }})", server.mcp_url(), expires_at);
    }

    auto connected = manager.connect_server("auth-needed-fixture");
    ASSERT_TRUE(connected.has_value()) << static_cast<int>(connected.error());

    snapshot = manager.snapshot_server("auth-needed-fixture");
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->status, cc::services::mcp::ConnectionStatus::Connected);
    ASSERT_EQ(snapshot->tools.size(), 1u);
    EXPECT_EQ(snapshot->tools.front().name, "refresh_lookup");

    const auto auth_headers = server.mcp_authorization_headers();
    ASSERT_FALSE(auth_headers.empty());
    EXPECT_TRUE(std::ranges::all_of(auth_headers, [](const auto& header) {
        return header == "Bearer fresh-access";
    }));

    manager.shutdown();
    fs::remove_all(root);
}

TEST(McpAuth, RevokesOAuthTokensViaMetadataEndpointAndClearsLocalStorage) {
    LocalOAuthRevocationServer server;
    ASSERT_TRUE(server.ready());

    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("cc_repl_mcp_revoke_" + std::to_string(suffix));
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard xdg_config_guard("XDG_CONFIG_HOME", root.string());

    cc::services::mcp::McpServerConfig auth_config;
    auth_config.type = "http";
    auth_config.url = "https://mcp.example.test/mcp";
    auth_config.oauth = cc::services::mcp::McpOAuthConfig{
        .auth_server_metadata_url = server.metadata_url(),
        .client_id = "client-1",
    };
    const auto server_key = cc::services::mcp::get_server_key("revoke-fixture", auth_config);
    auto sanitize_key = [](std::string_view key) {
        std::string sanitized;
        sanitized.reserve(key.size());
        for (unsigned char ch : key) {
            sanitized.push_back(std::isalnum(ch) ? static_cast<char>(ch) : '_');
        }
        return sanitized;
    };
    const auto token_path = root / "cc-repl" / "mcp" / (sanitize_key(server_key) + ".json");
    fs::create_directories(token_path.parent_path());
    const auto expires_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() + 3600;
    {
        std::ofstream token_file(token_path);
        token_file << std::format(R"({{
          "server_name": "revoke-fixture",
          "server_url": "{}",
          "access_token": "old-access",
          "refresh_token": "old-refresh",
          "expires_at": {},
          "scope": "tools",
          "client_id": "client-1",
          "client_secret": "secret-1",
          "discovery_state": {{}}
        }})", auth_config.url, expires_at);
    }

    auto revoked = cc::services::mcp::revoke_server_tokens("revoke-fixture", auth_config);
    ASSERT_TRUE(revoked.has_value()) << revoked.error().message();
    ASSERT_TRUE(server.wait_for_revoke_requests(2));

    const auto bodies = server.revoke_request_bodies();
    ASSERT_EQ(bodies.size(), 2u);
    EXPECT_NE(bodies[0].find("token=old-refresh"), std::string::npos) << bodies[0];
    EXPECT_NE(bodies[0].find("token_type_hint=refresh_token"), std::string::npos) << bodies[0];
    EXPECT_NE(bodies[0].find("client_id=client-1"), std::string::npos) << bodies[0];
    EXPECT_NE(bodies[0].find("client_secret=secret-1"), std::string::npos) << bodies[0];
    EXPECT_NE(bodies[1].find("token=old-access"), std::string::npos) << bodies[1];
    EXPECT_NE(bodies[1].find("token_type_hint=access_token"), std::string::npos) << bodies[1];

    const auto auth_headers = server.revoke_authorization_headers();
    ASSERT_EQ(auth_headers.size(), 2u);
    EXPECT_TRUE(auth_headers[0].empty());
    EXPECT_TRUE(auth_headers[1].empty());
    EXPECT_FALSE(fs::exists(token_path));

    fs::remove_all(root);
}

TEST(McpAuth, CompletesOAuthBrowserCallbackFlowAndStoresTokens) {
    LocalOAuthRevocationServer server;
    ASSERT_TRUE(server.ready());

    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("cc_repl_mcp_oauth_callback_" + std::to_string(suffix));
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard xdg_config_guard("XDG_CONFIG_HOME", root.string());

    cc::services::mcp::McpServerConfig auth_config;
    auth_config.type = "http";
    auth_config.url = "https://mcp.example.test/mcp";
    auth_config.oauth = cc::services::mcp::McpOAuthConfig{
        .auth_server_metadata_url = server.metadata_url(),
        .client_id = "client-1",
    };

    std::mutex mutex;
    std::condition_variable cv;
    std::optional<std::string> auth_url;
    std::optional<std::string> flow_error;
    bool flow_done = false;
    std::jthread flow_thread([&](std::stop_token) {
        auto result = cc::services::mcp::perform_mcp_oauth_flow(
            "callback-fixture",
            auth_config,
            [&](const std::string& url) {
                {
                    std::lock_guard lock(mutex);
                    auth_url = url;
                }
                cv.notify_all();
            },
            std::nullopt,
            true);
        {
            std::lock_guard lock(mutex);
            if (!result) flow_error = result.error().message();
            flow_done = true;
        }
        cv.notify_all();
    });

    std::string captured_url;
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&] {
            return auth_url.has_value() || flow_done;
        }));
        ASSERT_TRUE(auth_url.has_value()) << flow_error.value_or("OAuth flow ended before authorization URL");
        captured_url = *auth_url;
    }

    auto redirect_uri = test_query_param(captured_url, "redirect_uri");
    auto state = test_query_param(captured_url, "state");
    ASSERT_TRUE(redirect_uri.has_value()) << captured_url;
    ASSERT_TRUE(state.has_value()) << captured_url;
    EXPECT_NE(captured_url.find("response_type=code"), std::string::npos) << captured_url;
    EXPECT_NE(captured_url.find("client_id=client-1"), std::string::npos) << captured_url;
    EXPECT_NE(captured_url.find("code_challenge_method=S256"), std::string::npos) << captured_url;
    EXPECT_NE(captured_url.find("scope=tools"), std::string::npos) << captured_url;

    auto callback_port = localhost_url_port(*redirect_uri);
    ASSERT_TRUE(callback_port.has_value()) << *redirect_uri;
    httplib::Client callback_client(std::format("http://localhost:{}", *callback_port));
    auto callback_response = callback_client.Get(
        std::format("/oauth/callback?code=callback-code&state={}", *state));
    ASSERT_TRUE(callback_response);
    EXPECT_EQ(callback_response->status, 200);

    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return flow_done; }));
        ASSERT_FALSE(flow_error.has_value()) << *flow_error;
    }
    ASSERT_TRUE(server.wait_for_token_request());
    const auto token_body = server.token_request_body();
    EXPECT_NE(token_body.find("grant_type=authorization_code"), std::string::npos) << token_body;
    EXPECT_NE(token_body.find("code=callback-code"), std::string::npos) << token_body;
    EXPECT_NE(token_body.find("client_id=client-1"), std::string::npos) << token_body;
    EXPECT_NE(token_body.find("code_verifier="), std::string::npos) << token_body;

    const auto server_key = cc::services::mcp::get_server_key("callback-fixture", auth_config);
    auto sanitize_key = [](std::string_view key) {
        std::string sanitized;
        sanitized.reserve(key.size());
        for (unsigned char ch : key) {
            sanitized.push_back(std::isalnum(ch) ? static_cast<char>(ch) : '_');
        }
        return sanitized;
    };
    const auto token_path = root / "cc-repl" / "mcp" / (sanitize_key(server_key) + ".json");
    auto persisted = cc::utils::json::parse_file(token_path);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(persisted->root().get_string("server_name"), "callback-fixture");
    EXPECT_EQ(persisted->root().get_string("server_url"), auth_config.url);
    EXPECT_EQ(persisted->root().get_string("access_token"), "callback-access");
    EXPECT_EQ(persisted->root().get_string("refresh_token"), "callback-refresh");
    EXPECT_EQ(persisted->root().get_string("scope"), "tools");
    EXPECT_EQ(persisted->root().get_string("client_id"), "client-1");

    fs::remove_all(root);
}

TEST(McpAuth, PerformsXaaIdpLoginAndStoresTokens) {
    LocalXaaIdpServer server;
    ASSERT_TRUE(server.ready());

    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("cc_repl_mcp_xaa_idp_" + std::to_string(suffix));
    fs::remove_all(root);
    fs::create_directories(root / ".cc-repl");
    EnvironmentGuard home_guard("HOME", root.string());
    EnvironmentGuard xdg_config_guard("XDG_CONFIG_HOME", root.string());
    EnvironmentGuard xaa_enabled_guard("CLAUDE_CODE_ENABLE_XAA", "1");
    {
        std::ofstream idp_config(root / ".cc-repl" / "xaa-idp.txt");
        idp_config << "idp_url=" << server.base_url() << "\n";
        idp_config << "idp_issuer=" << server.base_url() << "\n";
        idp_config << "idp_token_endpoint=" << server.base_url() << "/token\n";
        idp_config << "idp_id_token=fake-id-token-for-testing\n";
        idp_config << "client_id=idp-client-1\n";
        idp_config << "client_secret=as-secret-1\n";
        idp_config << "idp_client_id=idp-client-1\n";
        idp_config << "scope=openid profile mcp\n";
    }

    cc::services::mcp::McpServerConfig auth_config;
    auth_config.type = "http";
    // Point to mock server so PRM discovery succeeds
    auth_config.url = server.base_url() + "/mcp";
    auth_config.oauth = cc::services::mcp::McpOAuthConfig{
        .client_id = "as-client-1",
        .xaa = true,
    };

    bool authorization_url_called = false;
    auto result = cc::services::mcp::perform_mcp_oauth_flow(
        "xaa-fixture",
        auth_config,
        [&](const std::string&) {
            authorization_url_called = true;
        },
        std::nullopt,
        true);
    ASSERT_TRUE(result.has_value()) << result.error().message();

    // No browser consent screen should have been shown — id_token was provided
    EXPECT_FALSE(authorization_url_called);

    // Verify the full XAA pipeline executed
    EXPECT_GE(server.prm_request_count(), 1)
        << "PRM discovery should have been called";
    EXPECT_GE(server.as_metadata_request_count(), 1)
        << "AS metadata discovery should have been called";
    EXPECT_GE(server.token_exchange_count(), 1)
        << "IdP token exchange (RFC 8693) should have been called";
    EXPECT_GE(server.jwt_bearer_count(), 1)
        << "AS jwt-bearer grant (RFC 7523) should have been called";

    // Verify the token exchange sent our fake id_token and correct client_id
    const auto exchange_body = server.token_exchange_body();
    EXPECT_NE(exchange_body.find("subject_token=fake-id-token-for-testing"), std::string::npos)
        << exchange_body;
    EXPECT_NE(exchange_body.find("client_id=idp-client-1"), std::string::npos)
        << exchange_body;
    EXPECT_NE(exchange_body.find("requested_token_type=urn%3Aietf%3Aparams%3Aoauth%3Atoken-type%3Aid-jag"),
        std::string::npos) << exchange_body;

    // Verify the jwt-bearer grant used HTTP Basic auth with AS credentials
    const auto auth_header = server.jwt_bearer_auth_header();
    EXPECT_NE(auth_header.find("Basic "), std::string::npos) << auth_header;

    // Verify token persistence
    const auto server_key = cc::services::mcp::get_server_key("xaa-fixture", auth_config);
    auto sanitize_key = [](std::string_view key) {
        std::string sanitized;
        sanitized.reserve(key.size());
        for (unsigned char ch : key) {
            sanitized.push_back(std::isalnum(ch) ? static_cast<char>(ch) : '_');
        }
        return sanitized;
    };
    const auto token_path = root / "cc-repl" / "mcp" / (sanitize_key(server_key) + ".json");
    auto persisted = cc::utils::json::parse_file(token_path);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(persisted->root().get_string("server_name"), "xaa-fixture");
    EXPECT_EQ(persisted->root().get_string("server_url"), auth_config.url);
    EXPECT_EQ(persisted->root().get_string("access_token"), "xaa-access");
    EXPECT_EQ(persisted->root().get_string("refresh_token"), "xaa-refresh");
    EXPECT_EQ(persisted->root().get_string("scope"), "openid profile mcp");
    EXPECT_EQ(persisted->root().get_string("client_id"), "idp-client-1");

    fs::remove_all(root);
}

TEST(McpAuth, XaaEnabledServerRequiresConfiguredIdpConnection) {
    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("cc_repl_mcp_xaa_missing_idp_" + std::to_string(suffix));
    fs::remove_all(root);
    fs::create_directories(root);
    EnvironmentGuard home_guard("HOME", root.string());
    EnvironmentGuard xdg_config_guard("XDG_CONFIG_HOME", root.string());
    EnvironmentGuard xaa_enabled_guard("CLAUDE_CODE_ENABLE_XAA", "1");

    cc::services::mcp::McpServerConfig auth_config;
    auth_config.type = "http";
    auth_config.url = "https://mcp.example.test/mcp";
    auth_config.oauth = cc::services::mcp::McpOAuthConfig{
        .client_id = "as-client-1",
        .xaa = true,
    };

    auto result = cc::services::mcp::perform_mcp_oauth_flow(
        "xaa-missing-idp",
        auth_config,
        [](const std::string&) {},
        std::nullopt,
        true);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message().find("configured IdP connection"), std::string::npos);

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

TEST(ServerRoutes, MessageRoutePublishesAssistantAndResultIngressEvents) {
    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("cc_repl_server_routes_ingress_test_" + std::to_string(suffix));
    const auto sessions_dir = root / "sessions";
    fs::remove_all(root);
    fs::create_directories(sessions_dir);

    LocalAnthropicMessagesServer anthropic;
    ASSERT_NE(anthropic.port(), 0);
    LocalCcrHttpServer ccr;
    ASSERT_TRUE(ccr.ready());

    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", anthropic.base_url());
    EnvironmentGuard model_guard("CLAUDE_MODEL", "direct-ingress-test-model");
    CurrentPathGuard cwd_guard(root);

    const auto now = std::chrono::system_clock::now();
    ASSERT_TRUE(cc::session::save_session_metadata(
        sessions_dir,
        cc::session::SessionMetadata{
            .session_id = "session_1",
            .model = "direct-ingress-test-model",
            .cwd = root,
            .created_at = now,
            .last_active = now,
            .message_count = 0,
            .title = std::string("Ingress route test"),
            .is_archived = false,
        }));

    cc::services::api::close_ingress();
    auto created = cc::services::api::create_ingress(cc::services::api::IngressConfig{
        .endpoint = ccr.base_url(),
        .session_id = "session_1",
        .auth_token = "session-route-token",
        .organization_uuid = std::nullopt,
    });
    ASSERT_TRUE(created.has_value()) << created.error();

    cc::server::reset_route_state_for_testing();
    cc::server::set_sessions_dir_for_testing(sessions_dir);
    auto routes = cc::server::get_default_routes();
    auto it = std::ranges::find_if(routes, [](const auto& route) {
        return route.method == "POST" && route.path == "/message";
    });
    ASSERT_NE(it, routes.end());

    auto response = it->handler({
        {"session_id", "session_1"},
        {"content", "hello ingress route"}
    });
    auto parsed = cc::utils::json::parse(response);
    ASSERT_TRUE(parsed.has_value()) << response;
    EXPECT_EQ(parsed->root().get_string("status"), "completed");
    EXPECT_EQ(parsed->root().get_string("session_id"), "session_1");
    EXPECT_EQ(parsed->root().get_string("response"), "ok");

    auto requests = ccr.wait_for_requests(2, std::chrono::seconds(5));
    ASSERT_TRUE(requests.has_value());
    ASSERT_EQ(requests->size(), 2u);
    EXPECT_EQ((*requests)[0].method, "POST");
    EXPECT_EQ((*requests)[0].path, "/v1/sessions/session_1/events");
    EXPECT_NE((*requests)[0].headers.find("Authorization: Bearer session-route-token"), std::string::npos);
    EXPECT_NE((*requests)[0].body.find(R"("events":[)"), std::string::npos);
    EXPECT_NE((*requests)[0].body.find(R"("type":"assistant")"), std::string::npos);
    EXPECT_NE((*requests)[0].body.find(R"("role":"assistant")"), std::string::npos);
    EXPECT_NE((*requests)[0].body.find(R"("session_id":"session_1")"), std::string::npos);
    EXPECT_NE((*requests)[0].body.find(R"("text":"ok")"), std::string::npos);

    EXPECT_EQ((*requests)[1].method, "POST");
    EXPECT_EQ((*requests)[1].path, "/v1/sessions/session_1/events");
    EXPECT_NE((*requests)[1].headers.find("Authorization: Bearer session-route-token"), std::string::npos);
    EXPECT_NE((*requests)[1].body.find(R"("type":"result")"), std::string::npos);
    EXPECT_NE((*requests)[1].body.find(R"("subtype":"success")"), std::string::npos);
    EXPECT_NE((*requests)[1].body.find(R"("result":"ok")"), std::string::npos);
    EXPECT_NE((*requests)[1].body.find(R"("session_id":"session_1")"), std::string::npos);

    auto metadata = cc::session::load_session_metadata(sessions_dir, "session_1");
    ASSERT_TRUE(metadata.has_value());
    EXPECT_EQ(metadata->message_count, 2);

    std::ifstream messages_file(cc::session::get_messages_path(sessions_dir, "session_1"));
    ASSERT_TRUE(messages_file.is_open());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(messages_file, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_NE(lines[0].find(R"("role":"user")"), std::string::npos);
    EXPECT_NE(lines[1].find(R"("role":"assistant")"), std::string::npos);

    cc::services::api::close_ingress();
    cc::server::reset_route_state_for_testing();
    fs::remove_all(root);
}

TEST(ServerMain, DirectConnectSessionsAndWebSocketUsePersistentRoutes) {
    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("cc_repl_server_main_test_" + std::to_string(suffix));
    const auto sessions_dir = root / "sessions";
    fs::remove_all(root);
    fs::create_directories(sessions_dir);

    LocalAnthropicMessagesServer anthropic;
    ASSERT_NE(anthropic.port(), 0);
    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", anthropic.base_url());
    EnvironmentGuard model_guard("CLAUDE_MODEL", "direct-server-test-model");
    EnvironmentGuard sessions_guard("CC_REPL_SERVER_SESSIONS_DIR", sessions_dir.string());
    CurrentPathGuard cwd_guard(root);
    cc::server::reset_route_state_for_testing();

    cc::server::HttpServer direct_server;
    auto started = direct_server.start(cc::server::ServerConfig{
        .port = 0,
        .host = "127.0.0.1",
        .cors = false,
        .auth_token = std::nullopt,
    });
    ASSERT_TRUE(started.has_value()) << started.error();
    const auto server_port = direct_server.get_config().port;
    ASSERT_NE(server_port, 0);

    const auto create_body = std::format(R"({{"cwd":"{}"}})", root.string());
    auto create_response = direct_connect_http_request(server_port, "POST", "/sessions", create_body);
    ASSERT_TRUE(create_response.has_value());
    ASSERT_EQ(create_response->status, 200) << create_response->body;
    auto create_json = cc::utils::json::parse(create_response->body);
    ASSERT_TRUE(create_json.has_value()) << create_response->body;
    const auto session_id = std::string(create_json->root().get_string("session_id"));
    ASSERT_FALSE(session_id.empty());
    EXPECT_NE(
        std::string(create_json->root().get_string("ws_url")).find("/sessions/ws/" + session_id),
        std::string::npos);
    EXPECT_EQ(
        std::string(create_json->root().get_string("work_dir")),
        fs::weakly_canonical(root).string());

    auto initial_metadata = cc::session::load_session_metadata(sessions_dir, session_id);
    ASSERT_TRUE(initial_metadata.has_value());
    EXPECT_EQ(initial_metadata->message_count, 0);

    auto ws_fd = direct_connect_open_websocket(server_port, "/sessions/ws/" + session_id);
    ASSERT_TRUE(ws_fd.has_value());

    const std::vector<std::string> prompts{
        "hello direct connect",
        "follow up direct connect 1",
        "follow up direct connect 2",
        "follow up direct connect 3",
        "follow up direct connect 4",
    };
    for (const auto& prompt : prompts) {
        const auto user_payload = std::format(
            R"({{"type":"user","message":{{"role":"user","content":"{}"}},"parent_tool_use_id":null,"session_id":""}})",
            prompt);
        ASSERT_TRUE(direct_connect_send_client_text_frame(*ws_fd, user_payload));

        auto assistant_frame = direct_connect_read_ws_frame(*ws_fd);
        ASSERT_TRUE(assistant_frame.has_value());
        ASSERT_EQ(assistant_frame->opcode, 0x1);
        auto assistant_json = cc::utils::json::parse(direct_connect_trim_json_line(assistant_frame->payload));
        ASSERT_TRUE(assistant_json.has_value()) << assistant_frame->payload;
        EXPECT_EQ(assistant_json->root().get_string("type"), "assistant");
        EXPECT_EQ(assistant_json->root().get_string("session_id"), session_id);
        auto assistant_content = assistant_json->root().get("message").get("content");
        ASSERT_TRUE(assistant_content.is_arr()) << assistant_frame->payload;
        ASSERT_GE(assistant_content.size(), 1u);
        EXPECT_EQ(assistant_content.at(0).get_string("text"), "ok");

        auto result_frame = direct_connect_read_ws_frame(*ws_fd);
        ASSERT_TRUE(result_frame.has_value());
        ASSERT_EQ(result_frame->opcode, 0x1);
        auto result_json = cc::utils::json::parse(direct_connect_trim_json_line(result_frame->payload));
        ASSERT_TRUE(result_json.has_value()) << result_frame->payload;
        EXPECT_EQ(result_json->root().get_string("type"), "result");
        EXPECT_EQ(result_json->root().get_string("subtype"), "success");
        EXPECT_EQ(result_json->root().get_string("result"), "ok");
        EXPECT_EQ(result_json->root().get_string("session_id"), session_id);
    }

    const std::string interrupt_payload =
        R"({"type":"control_request","request_id":"interrupt-test","request":{"subtype":"interrupt"}})";
    ASSERT_TRUE(direct_connect_send_client_text_frame(*ws_fd, interrupt_payload));
    auto control_frame = direct_connect_read_ws_frame(*ws_fd);
    ASSERT_TRUE(control_frame.has_value());
    auto control_json = cc::utils::json::parse(direct_connect_trim_json_line(control_frame->payload));
    ASSERT_TRUE(control_json.has_value()) << control_frame->payload;
    EXPECT_EQ(control_json->root().get_string("type"), "control_response");
    auto control_response = control_json->root().get("response");
    EXPECT_EQ(control_response.get_string("subtype"), "success");
    EXPECT_EQ(control_response.get_string("request_id"), "interrupt-test");

    ::shutdown(*ws_fd, SHUT_RDWR);
    ::close(*ws_fd);

    auto request_bodies = anthropic.wait_for_bodies(prompts.size());
    ASSERT_TRUE(request_bodies.has_value());
    auto request_json = cc::utils::json::parse(request_bodies->back());
    ASSERT_TRUE(request_json.has_value()) << request_bodies->back();
    auto request_messages = request_json->root().get("messages");
    ASSERT_TRUE(request_messages.is_arr()) << request_bodies->back();
    ASSERT_EQ(request_messages.size(), 9u);
    EXPECT_NE(
        std::string(request_messages.at(0).get_string("content")).find("hello direct connect"),
        std::string::npos);
    EXPECT_NE(
        std::string(request_messages.at(8).get_string("content")).find("follow up direct connect 4"),
        std::string::npos);

    auto sessions_response = direct_connect_http_request(server_port, "GET", "/sessions?limit=5");
    ASSERT_TRUE(sessions_response.has_value());
    ASSERT_EQ(sessions_response->status, 200) << sessions_response->body;
    auto sessions_json = cc::utils::json::parse(sessions_response->body);
    ASSERT_TRUE(sessions_json.has_value()) << sessions_response->body;
    EXPECT_EQ(sessions_json->root().get("total").as_int(), 1);
    auto sessions = sessions_json->root().get("sessions");
    ASSERT_TRUE(sessions.is_arr());
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions.at(0).get_string("session_id"), session_id);
    EXPECT_EQ(sessions.at(0).get("message_count").as_int(), 10);

    const auto compact_body = std::format(R"({{"session_id":"{}"}})", session_id);
    auto compact_response = direct_connect_http_request(server_port, "POST", "/compact", compact_body);
    ASSERT_TRUE(compact_response.has_value());
    ASSERT_EQ(compact_response->status, 200) << compact_response->body;
    auto compact_json = cc::utils::json::parse(compact_response->body);
    ASSERT_TRUE(compact_json.has_value()) << compact_response->body;
    EXPECT_EQ(compact_json->root().get_string("status"), "compacted");
    EXPECT_EQ(compact_json->root().get_string("session_id"), session_id);
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
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(messages_file, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    ASSERT_EQ(lines.size(), 7u);
    auto boundary_json = cc::utils::json::parse(lines.front());
    ASSERT_TRUE(boundary_json.has_value()) << lines.front();
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
    EXPECT_NE(boundary_content.find("hello direct connect"), std::string::npos);
    EXPECT_NE(boundary_content.find("follow up direct connect 1"), std::string::npos);
    EXPECT_NE(boundary_content.find("Preserve these details"), std::string::npos);
    EXPECT_NE(lines.back().find("ok"), std::string::npos);

    direct_server.stop();
	cc::server::reset_route_state_for_testing();
	fs::remove_all(root);
}

TEST(ServerMain, DirectConnectInterruptCancelsActiveMessageRoute) {
    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("cc_repl_server_interrupt_cancel_test_" + std::to_string(suffix));
    const auto sessions_dir = root / "sessions";
    fs::remove_all(root);
    fs::create_directories(sessions_dir);

    EnvironmentGuard sessions_guard("CC_REPL_SERVER_SESSIONS_DIR", sessions_dir.string());
    CurrentPathGuard cwd_guard(root);
    cc::server::reset_route_state_for_testing();

    std::mutex executor_mutex;
    std::condition_variable executor_cv;
    bool executor_started = false;
    std::atomic<bool> executor_saw_cancel{false};
    cc::server::set_query_executor_for_testing(
        [&](const cc::server::detail::DirectQueryRequest& request)
            -> std::expected<cc::server::detail::DirectQueryResult, std::string> {
            {
                std::lock_guard lock(executor_mutex);
                executor_started = true;
            }
            executor_cv.notify_all();

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < deadline) {
                if (request.cancel_flag && request.cancel_flag->load()) {
                    executor_saw_cancel.store(true);
                    return std::unexpected(std::string("Query interrupted"));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return cc::server::detail::DirectQueryResult{
                .assistant_id = "msg_not_cancelled",
                .content = "not cancelled",
                .model = "test-model",
                .input_tokens = 1,
                .output_tokens = 1,
                .tool_rounds = 0,
                .elapsed_ms = 3000,
            };
        });

    cc::server::HttpServer direct_server;
    auto started = direct_server.start(cc::server::ServerConfig{
        .port = 0,
        .host = "127.0.0.1",
        .cors = false,
        .auth_token = std::nullopt,
    });
    ASSERT_TRUE(started.has_value()) << started.error();
    const auto server_port = direct_server.get_config().port;
    ASSERT_NE(server_port, 0);

    const auto create_body = std::format(R"({{"cwd":"{}"}})", root.string());
    auto create_response = direct_connect_http_request(server_port, "POST", "/sessions", create_body);
    ASSERT_TRUE(create_response.has_value());
    ASSERT_EQ(create_response->status, 200) << create_response->body;
    auto create_json = cc::utils::json::parse(create_response->body);
    ASSERT_TRUE(create_json.has_value()) << create_response->body;
    const auto session_id = std::string(create_json->root().get_string("session_id"));
    ASSERT_FALSE(session_id.empty());

    auto ws_fd = direct_connect_open_websocket(server_port, "/sessions/ws/" + session_id);
    ASSERT_TRUE(ws_fd.has_value());
    const auto user_payload = R"({"type":"user","message":{"role":"user","content":"start long direct route"},"parent_tool_use_id":null,"session_id":""})";
    ASSERT_TRUE(direct_connect_send_client_text_frame(*ws_fd, user_payload));

    {
        std::unique_lock lock(executor_mutex);
        ASSERT_TRUE(executor_cv.wait_for(lock, std::chrono::seconds(1), [&] {
            return executor_started;
        }));
    }

    const std::string interrupt_payload =
        R"({"type":"control_request","request_id":"active-interrupt","request":{"subtype":"interrupt"}})";
    ASSERT_TRUE(direct_connect_send_client_text_frame(*ws_fd, interrupt_payload));

    auto control_frame = direct_connect_read_ws_frame(*ws_fd);
    ASSERT_TRUE(control_frame.has_value());
    auto control_json = cc::utils::json::parse(direct_connect_trim_json_line(control_frame->payload));
    ASSERT_TRUE(control_json.has_value()) << control_frame->payload;
    EXPECT_EQ(control_json->root().get_string("type"), "control_response");
    auto control_response = control_json->root().get("response");
    EXPECT_EQ(control_response.get_string("subtype"), "success");
    EXPECT_EQ(control_response.get_string("request_id"), "active-interrupt");
    EXPECT_TRUE(control_response.get("response").get("interrupted").as_bool());

    auto error_frame = direct_connect_read_ws_frame(*ws_fd);
    ASSERT_TRUE(error_frame.has_value());
    auto error_json = cc::utils::json::parse(direct_connect_trim_json_line(error_frame->payload));
    ASSERT_TRUE(error_json.has_value()) << error_frame->payload;
    EXPECT_EQ(error_json->root().get_string("type"), "result");
    EXPECT_EQ(error_json->root().get_string("subtype"), "error_during_execution");
    EXPECT_TRUE(error_json->root().get("is_error").as_bool());
    auto errors = error_json->root().get("errors");
    ASSERT_TRUE(errors.is_arr());
    ASSERT_GE(errors.size(), 1u);
    EXPECT_NE(std::string(errors.at(0).as_str()).find("Query interrupted"), std::string::npos);
    EXPECT_TRUE(executor_saw_cancel.load());

    ::shutdown(*ws_fd, SHUT_RDWR);
    ::close(*ws_fd);
    direct_server.stop();
	cc::server::reset_route_state_for_testing();
	fs::remove_all(root);
}

TEST(ServerMain, DirectConnectPermissionControlCanAllowAndDenyToolUse) {
    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("cc_repl_direct_permission_test_" + std::to_string(suffix));
    const auto sessions_dir = root / "sessions";
    fs::remove_all(root);
    fs::create_directories(root);
    const auto allowed_file = root / "allowed.txt";
    {
        std::ofstream output(allowed_file);
        output << "permission bridge content\n";
    }
    const auto original_file = root / "original.txt";
    {
        std::ofstream output(original_file);
        output << "original input content\n";
    }
    const auto updated_file = root / "updated.txt";
    {
        std::ofstream output(updated_file);
        output << "updated permission content\n";
    }
    const auto denied_file = root / "denied.txt";
    {
        std::ofstream output(denied_file);
        output << "denied permission content\n";
    }
    const auto errored_file = root / "errored.txt";
    {
        std::ofstream output(errored_file);
        output << "errored permission content\n";
    }
    const auto allowed_dir = root / "allowed-dir";
    fs::create_directories(allowed_dir);
    const auto directory_file = allowed_dir / "directory.txt";
    {
        std::ofstream output(directory_file);
        output << "directory permission content\n";
    }

    LocalAnthropicMessagesServer anthropic({
        std::format(
            R"({{"id":"msg_read_allow_tool","type":"message","role":"assistant","model":"claude-test","content":[{{"type":"tool_use","id":"toolu_read_allow","name":"Read","input":{{"file_path":"{}"}}}}],"stop_reason":"tool_use","usage":{{"input_tokens":1,"output_tokens":1}}}})",
            allowed_file.string()),
        R"({"id":"msg_read_allow_done","type":"message","role":"assistant","model":"claude-test","content":[{"type":"text","text":"read allowed"}],"stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":1}})",
        std::format(
            R"({{"id":"msg_read_update_tool","type":"message","role":"assistant","model":"claude-test","content":[{{"type":"tool_use","id":"toolu_read_update","name":"Read","input":{{"file_path":"{}"}}}}],"stop_reason":"tool_use","usage":{{"input_tokens":1,"output_tokens":1}}}})",
            original_file.string()),
        R"({"id":"msg_read_update_done","type":"message","role":"assistant","model":"claude-test","content":[{"type":"text","text":"read updated"}],"stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":1}})",
        std::format(
            R"({{"id":"msg_read_cached_tool","type":"message","role":"assistant","model":"claude-test","content":[{{"type":"tool_use","id":"toolu_read_cached","name":"Read","input":{{"file_path":"{}"}}}}],"stop_reason":"tool_use","usage":{{"input_tokens":1,"output_tokens":1}}}})",
            original_file.string()),
        R"({"id":"msg_read_cached_done","type":"message","role":"assistant","model":"claude-test","content":[{"type":"text","text":"read cached"}],"stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":1}})",
        std::format(
            R"({{"id":"msg_read_directory_tool","type":"message","role":"assistant","model":"claude-test","content":[{{"type":"tool_use","id":"toolu_read_directory","name":"Read","input":{{"file_path":"{}"}}}}],"stop_reason":"tool_use","usage":{{"input_tokens":1,"output_tokens":1}}}})",
            directory_file.string()),
        R"({"id":"msg_read_directory_done","type":"message","role":"assistant","model":"claude-test","content":[{"type":"text","text":"read directory cached"}],"stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":1}})",
        std::format(
            R"({{"id":"msg_read_deny_tool","type":"message","role":"assistant","model":"claude-test","content":[{{"type":"tool_use","id":"toolu_read_deny","name":"Read","input":{{"file_path":"{}"}}}}],"stop_reason":"tool_use","usage":{{"input_tokens":1,"output_tokens":1}}}})",
            denied_file.string()),
        R"({"id":"msg_read_deny_done","type":"message","role":"assistant","model":"claude-test","content":[{"type":"text","text":"read denied"}],"stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":1}})",
        std::format(
            R"({{"id":"msg_read_error_tool","type":"message","role":"assistant","model":"claude-test","content":[{{"type":"tool_use","id":"toolu_read_error","name":"Read","input":{{"file_path":"{}"}}}}],"stop_reason":"tool_use","usage":{{"input_tokens":1,"output_tokens":1}}}})",
            errored_file.string()),
        R"({"id":"msg_read_error_done","type":"message","role":"assistant","model":"claude-test","content":[{"type":"text","text":"read permission error"}],"stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":1}})",
    });
    ASSERT_NE(anthropic.port(), 0);

    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", anthropic.base_url());
    EnvironmentGuard sessions_guard("CC_REPL_SERVER_SESSIONS_DIR", sessions_dir.string());
    CurrentPathGuard cwd_guard(root);
    cc::server::reset_route_state_for_testing();

    cc::server::HttpServer direct_server;
    auto started = direct_server.start(cc::server::ServerConfig{
        .port = 0,
        .host = "127.0.0.1",
        .cors = false,
        .auth_token = std::nullopt,
    });
    ASSERT_TRUE(started.has_value()) << started.error();
    const auto server_port = direct_server.get_config().port;
    ASSERT_NE(server_port, 0);

    const auto create_body = std::format(R"({{"cwd":"{}"}})", root.string());
    auto create_response = direct_connect_http_request(server_port, "POST", "/sessions", create_body);
    ASSERT_TRUE(create_response.has_value());
    ASSERT_EQ(create_response->status, 200) << create_response->body;
    auto create_json = cc::utils::json::parse(create_response->body);
    ASSERT_TRUE(create_json.has_value()) << create_response->body;
    const auto session_id = std::string(create_json->root().get_string("session_id"));
    ASSERT_FALSE(session_id.empty());

    auto ws_fd = direct_connect_open_websocket(server_port, "/sessions/ws/" + session_id);
    ASSERT_TRUE(ws_fd.has_value());

    auto send_prompt_with_permission = [&](std::string_view prompt,
                                           std::string_view behavior,
                                           std::string_view expected_tool_use_id,
                                           std::string_view expected_file_path,
                                           std::string_view expected_text,
                                           std::string_view extra_response_fields_or_error_message = {}) {
        const auto user_payload = std::format(
            R"({{"type":"user","message":{{"role":"user","content":"{}"}},"parent_tool_use_id":null,"session_id":""}})",
            prompt);
        ASSERT_TRUE(direct_connect_send_client_text_frame(*ws_fd, user_payload));

        std::optional<cc::utils::json::JsonDoc> permission_json;
        for (int attempt = 0; attempt < 4; ++attempt) {
            auto permission_frame = direct_connect_read_ws_frame(*ws_fd);
            ASSERT_TRUE(permission_frame.has_value());
            auto parsed = cc::utils::json::parse(direct_connect_trim_json_line(permission_frame->payload));
            ASSERT_TRUE(parsed.has_value()) << permission_frame->payload;
            if (parsed->root().get_string("type") == "control_request") {
                permission_json.emplace(std::move(*parsed));
                break;
            }
        }
        ASSERT_TRUE(permission_json.has_value());
        EXPECT_EQ(permission_json->root().get_string("type"), "control_request");
        ASSERT_TRUE(permission_json->root().get("request_id").is_str());
        const auto request_id = std::string(permission_json->root().get_string("request_id"));
        auto request = permission_json->root().get("request");
        ASSERT_TRUE(request.is_obj());
        EXPECT_EQ(request.get_string("subtype"), "can_use_tool");
        EXPECT_EQ(request.get_string("tool_name"), "Read");
        EXPECT_EQ(request.get_string("tool_use_id"), expected_tool_use_id);
        EXPECT_EQ(request.get("input").get_string("file_path"), expected_file_path);
        if (behavior == std::string_view("error")) {
            ASSERT_TRUE(direct_connect_send_permission_error_response(
                *ws_fd,
                request_id,
                extra_response_fields_or_error_message));
        } else {
            ASSERT_TRUE(direct_connect_send_permission_response(
                *ws_fd,
                request_id,
                behavior,
                extra_response_fields_or_error_message));
        }

        auto assistant_frame = direct_connect_read_ws_frame(*ws_fd);
        ASSERT_TRUE(assistant_frame.has_value());
        auto assistant_json = cc::utils::json::parse(direct_connect_trim_json_line(assistant_frame->payload));
        ASSERT_TRUE(assistant_json.has_value()) << assistant_frame->payload;
        EXPECT_EQ(assistant_json->root().get_string("type"), "assistant");
        auto assistant_content = assistant_json->root().get("message").get("content");
        ASSERT_TRUE(assistant_content.is_arr()) << assistant_frame->payload;
        ASSERT_GE(assistant_content.size(), 1u);
        EXPECT_EQ(assistant_content.at(0).get_string("text"), expected_text);

        auto result_frame = direct_connect_read_ws_frame(*ws_fd);
        ASSERT_TRUE(result_frame.has_value());
        auto result_json = cc::utils::json::parse(direct_connect_trim_json_line(result_frame->payload));
        ASSERT_TRUE(result_json.has_value()) << result_frame->payload;
        EXPECT_EQ(result_json->root().get_string("type"), "result");
        EXPECT_EQ(result_json->root().get_string("subtype"), "success");
	        EXPECT_EQ(result_json->root().get_string("result"), expected_text);
	    };

    auto send_prompt_without_permission = [&](std::string_view prompt,
                                              std::string_view expected_text) {
        const auto user_payload = std::format(
            R"({{"type":"user","message":{{"role":"user","content":"{}"}},"parent_tool_use_id":null,"session_id":""}})",
            prompt);
        ASSERT_TRUE(direct_connect_send_client_text_frame(*ws_fd, user_payload));

        auto assistant_frame = direct_connect_read_ws_frame(*ws_fd);
        ASSERT_TRUE(assistant_frame.has_value());
        auto assistant_json = cc::utils::json::parse(direct_connect_trim_json_line(assistant_frame->payload));
        ASSERT_TRUE(assistant_json.has_value()) << assistant_frame->payload;
        EXPECT_EQ(assistant_json->root().get_string("type"), "assistant");
        auto assistant_content = assistant_json->root().get("message").get("content");
        ASSERT_TRUE(assistant_content.is_arr()) << assistant_frame->payload;
        ASSERT_GE(assistant_content.size(), 1u);
        EXPECT_EQ(assistant_content.at(0).get_string("text"), expected_text);

        auto result_frame = direct_connect_read_ws_frame(*ws_fd);
        ASSERT_TRUE(result_frame.has_value());
        auto result_json = cc::utils::json::parse(direct_connect_trim_json_line(result_frame->payload));
        ASSERT_TRUE(result_json.has_value()) << result_frame->payload;
        EXPECT_EQ(result_json->root().get_string("type"), "result");
        EXPECT_EQ(result_json->root().get_string("subtype"), "success");
        EXPECT_EQ(result_json->root().get_string("result"), expected_text);
    };

    send_prompt_with_permission(
        "read with permission allow",
        "allow",
        "toolu_read_allow",
        allowed_file.string(),
        "read allowed");
    send_prompt_with_permission(
        "read with permission updated input",
        "allow",
        "toolu_read_update",
        original_file.string(),
	        "read updated",
	        std::format(
	            R"(,"updatedInput":{{"file_path":"{}"}},"updatedPermissions":[{{"type":"addRules","rules":[{{"toolName":"Read","ruleContent":"{}"}}],"behavior":"allow","destination":"session"}},{{"type":"addDirectories","directories":["{}"],"destination":"session"}}])",
	            updated_file.string(),
	            original_file.string(),
	            allowed_dir.string()));
	    send_prompt_without_permission("read with cached permission update", "read cached");
    send_prompt_without_permission("read with cached directory permission", "read directory cached");
	    send_prompt_with_permission(
	        "read with permission deny",
        "deny",
        "toolu_read_deny",
        denied_file.string(),
        "read denied",
        R"(,"message":"denied by direct permission test")");
    send_prompt_with_permission(
        "read with permission error response",
        "error",
        "toolu_read_error",
        errored_file.string(),
        "read permission error",
        "client callback failed direct permission test");

    auto request_bodies = anthropic.wait_for_bodies(12);
    ASSERT_TRUE(request_bodies.has_value());
    ASSERT_EQ(request_bodies->size(), 12u);
    EXPECT_NE((*request_bodies)[1].find("permission bridge content"), std::string::npos);
    EXPECT_NE((*request_bodies)[3].find("updated permission content"), std::string::npos);
    EXPECT_EQ((*request_bodies)[3].find("original input content"), std::string::npos);
    EXPECT_NE((*request_bodies)[5].find("original input content"), std::string::npos);
    EXPECT_NE((*request_bodies)[7].find("directory permission content"), std::string::npos);
    EXPECT_NE((*request_bodies)[9].find("denied by direct permission test"), std::string::npos);
    EXPECT_EQ((*request_bodies)[9].find("Permission denied for tool: Read"), std::string::npos);
    EXPECT_NE((*request_bodies)[11].find("client callback failed direct permission test"), std::string::npos);
    EXPECT_EQ((*request_bodies)[11].find("Permission denied for tool: Read"), std::string::npos);

    ::shutdown(*ws_fd, SHUT_RDWR);
    ::close(*ws_fd);
    direct_server.stop();
    cc::server::reset_route_state_for_testing();
    fs::remove_all(root);
}

TEST(ServerMain, DirectConnectToolLoopPersistsTeamCreateAndSendMessage) {
    const auto suffix = std::chrono::system_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("cc_repl_direct_team_tool_test_" + std::to_string(suffix));
    const auto sessions_dir = root / "sessions";
    fs::remove_all(root);
    fs::create_directories(root);

    LocalAnthropicMessagesServer anthropic({
        R"({"id":"msg_team_create_tool","type":"message","role":"assistant","model":"claude-test","content":[{"type":"tool_use","id":"toolu_team_create","name":"team_create","input":{"team_id":"direct-team-id","team_name":"Direct Team","members":[{"agent_id":"reviewer-one","role":"reviewer"},{"agent_id":"researcher-one","role":"worker"}],"task_list":[{"id":"direct-task","description":"Inspect direct connect team migration","assigned_to":"reviewer-one"}]}}],"stop_reason":"tool_use","usage":{"input_tokens":1,"output_tokens":1}})",
        R"({"id":"msg_team_create_done","type":"message","role":"assistant","model":"claude-test","content":[{"type":"text","text":"direct team created"}],"stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":1}})",
        R"({"id":"msg_send_message_tool","type":"message","role":"assistant","model":"claude-test","content":[{"type":"tool_use","id":"toolu_send_message","name":"send_message","input":{"target_agent":"reviewer-one","team_name":"Direct Team","content":"Please review direct connect team output","summary":"direct team follow-up"}}],"stop_reason":"tool_use","usage":{"input_tokens":1,"output_tokens":1}})",
        R"({"id":"msg_send_message_done","type":"message","role":"assistant","model":"claude-test","content":[{"type":"text","text":"direct team message delivered"}],"stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":1}})",
    });
    ASSERT_NE(anthropic.port(), 0);

    EnvironmentGuard api_key_guard("ANTHROPIC_API_KEY", "test-key");
    EnvironmentGuard base_url_guard("ANTHROPIC_BASE_URL", anthropic.base_url());
    EnvironmentGuard sessions_guard("CC_REPL_SERVER_SESSIONS_DIR", sessions_dir.string());
    EnvironmentGuard team_dir_guard("CC_REPL_TEAM_RUNTIME_DIR", (root / "teams").string());
    EnvironmentGuard agent_runtime_guard("CC_REPL_AGENT_RUNTIME_DIR", (root / "agents").string());
    CurrentPathGuard cwd_guard(root);
    cc::server::reset_route_state_for_testing();
    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();

    cc::server::HttpServer direct_server;
    auto started = direct_server.start(cc::server::ServerConfig{
        .port = 0,
        .host = "127.0.0.1",
        .cors = false,
        .auth_token = std::nullopt,
    });
    ASSERT_TRUE(started.has_value()) << started.error();
    const auto server_port = direct_server.get_config().port;
    ASSERT_NE(server_port, 0);

    const auto create_body = std::format(R"({{"cwd":"{}"}})", root.string());
    auto create_response = direct_connect_http_request(server_port, "POST", "/sessions", create_body);
    ASSERT_TRUE(create_response.has_value());
    ASSERT_EQ(create_response->status, 200) << create_response->body;
    auto create_json = cc::utils::json::parse(create_response->body);
    ASSERT_TRUE(create_json.has_value()) << create_response->body;
    const auto session_id = std::string(create_json->root().get_string("session_id"));
    ASSERT_FALSE(session_id.empty());

    auto ws_fd = direct_connect_open_websocket(server_port, "/sessions/ws/" + session_id);
    ASSERT_TRUE(ws_fd.has_value());

    auto read_non_permission_frame = [&]() -> std::optional<DirectConnectWsFrame> {
        while (true) {
            auto frame = direct_connect_read_ws_frame(*ws_fd);
            if (!frame) return std::nullopt;
            auto parsed = cc::utils::json::parse(direct_connect_trim_json_line(frame->payload));
            if (!parsed || !parsed->root().is_obj()) return frame;
            if (parsed->root().get_string("type") == "control_request") {
                auto request_id_value = parsed->root().get("request_id");
                auto request = parsed->root().get("request");
                if (request_id_value.is_str() &&
                    request.is_obj() &&
                    request.get_string("subtype") == "can_use_tool") {
                    EXPECT_TRUE(direct_connect_send_permission_response(
                        *ws_fd,
                        std::string(request_id_value.as_str()),
                        "allow"));
                    continue;
                }
            }
            return frame;
        }
    };

    auto send_user_prompt = [&](std::string_view prompt, std::string_view expected_text) {
        const auto user_payload = std::format(
            R"({{"type":"user","message":{{"role":"user","content":"{}"}},"parent_tool_use_id":null,"session_id":""}})",
            prompt);
        ASSERT_TRUE(direct_connect_send_client_text_frame(*ws_fd, user_payload));

        auto assistant_frame = read_non_permission_frame();
        ASSERT_TRUE(assistant_frame.has_value());
        auto assistant_json = cc::utils::json::parse(direct_connect_trim_json_line(assistant_frame->payload));
        ASSERT_TRUE(assistant_json.has_value()) << assistant_frame->payload;
        EXPECT_EQ(assistant_json->root().get_string("type"), "assistant");
        EXPECT_EQ(assistant_json->root().get_string("session_id"), session_id);
        auto assistant_content = assistant_json->root().get("message").get("content");
        ASSERT_TRUE(assistant_content.is_arr()) << assistant_frame->payload;
        ASSERT_GE(assistant_content.size(), 1u);
        EXPECT_EQ(assistant_content.at(0).get_string("text"), expected_text);

        auto result_frame = read_non_permission_frame();
        ASSERT_TRUE(result_frame.has_value());
        auto result_json = cc::utils::json::parse(direct_connect_trim_json_line(result_frame->payload));
        ASSERT_TRUE(result_json.has_value()) << result_frame->payload;
        EXPECT_EQ(result_json->root().get_string("type"), "result");
        EXPECT_EQ(result_json->root().get_string("subtype"), "success");
        EXPECT_EQ(result_json->root().get_string("result"), expected_text);
    };

    send_user_prompt("create a direct connect team", "direct team created");
    send_user_prompt("message reviewer-one on the direct team", "direct team message delivered");

    ::shutdown(*ws_fd, SHUT_RDWR);
    ::close(*ws_fd);

    auto request_bodies = anthropic.wait_for_bodies(4);
    ASSERT_TRUE(request_bodies.has_value());
    ASSERT_EQ(request_bodies->size(), 4u);
    auto first_request = cc::utils::json::parse(request_bodies->front());
    ASSERT_TRUE(first_request.has_value()) << request_bodies->front();
    auto tools = first_request->root().get("tools");
    ASSERT_TRUE(tools.is_arr()) << request_bodies->front();
    bool exposed_team_create = false;
    bool exposed_send_message = false;
    tools.iter([&](cc::utils::json::JsonVal tool) {
        const auto name = std::string(tool.get_string("name"));
        if (name == "team_create") exposed_team_create = true;
        if (name == "send_message") exposed_send_message = true;
    });
    EXPECT_TRUE(exposed_team_create);
    EXPECT_TRUE(exposed_send_message);

    auto team = cc::tools::global_team_store().get("direct-team-id");
    ASSERT_TRUE(team.has_value()) << std::string(cc::tools::format_error(team.error()));
    EXPECT_EQ((*team)->name, "Direct Team");
    ASSERT_EQ((*team)->members.size(), 2u);
    ASSERT_EQ((*team)->task_list.size(), 1u);
    EXPECT_EQ((*team)->task_list.front().id, "direct-task");
    ASSERT_TRUE((*team)->task_list.front().assigned_to.has_value());
    EXPECT_EQ(*(*team)->task_list.front().assigned_to, "reviewer-one");

    const auto team_dir = root / "teams" / "direct-team";
    EXPECT_TRUE(fs::exists(root / "teams" / "direct-team-id.json"));
    EXPECT_TRUE(fs::exists(team_dir / "config.json"));
    EXPECT_TRUE(fs::exists(team_dir / "tasks.json"));
    EXPECT_TRUE(fs::exists(team_dir / "inboxes" / "reviewer-one.json"));

    auto reviewer_record = cc::tools::agent_runtime::native_agent_store().get("reviewer-one");
    ASSERT_TRUE(reviewer_record.has_value());
    ASSERT_TRUE(reviewer_record->team_name.has_value());
    EXPECT_EQ(*reviewer_record->team_name, "Direct Team");
    ASSERT_EQ(reviewer_record->pending_messages.size(), 2u);
    EXPECT_NE(reviewer_record->pending_messages.front().find("Inspect direct connect team migration"), std::string::npos);
    EXPECT_NE(reviewer_record->pending_messages.back().find("Please review direct connect team output"), std::string::npos);

    auto inbox = cc::utils::read_inbox("reviewer-one", std::optional<std::string_view>{"Direct Team"});
    ASSERT_TRUE(inbox.has_value()) << inbox.error();
    ASSERT_EQ(inbox->size(), 1u);
    EXPECT_EQ(inbox->front().from, "team-lead");
    EXPECT_EQ(inbox->front().text, "Please review direct connect team output");
    ASSERT_TRUE(inbox->front().summary.has_value());
    EXPECT_EQ(*inbox->front().summary, "direct team follow-up");

    auto metadata = cc::session::load_session_metadata(sessions_dir, session_id);
    ASSERT_TRUE(metadata.has_value());
    EXPECT_EQ(metadata->message_count, 4);

    direct_server.stop();
    cc::server::reset_route_state_for_testing();
    cc::tools::global_team_store().clear_for_testing();
    cc::tools::agent_runtime::native_agent_store().clear_for_testing();
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

// ---------------------------------------------------------------------------
// LSP response parsers — exercised against canned JSON fixtures (no server).
// These cover the previously-stubbed parsers that dropped most response data.
// ---------------------------------------------------------------------------

namespace {
cc::services::lsp::LspClient make_lsp_for_parsing() {
    return cc::services::lsp::LspClient(cc::services::lsp::LspClient::Config{});
}
} // namespace

TEST(LspClientParser, ParseHoverExtractsMarkupStringAndRange) {
    auto client = make_lsp_for_parsing();
    auto hover = client.parse_hover(
        R"({"contents":{"kind":"markdown","value":"fn doc"},"range":{"start":{"line":1,"character":2},"end":{"line":1,"character":4}}})");
    ASSERT_TRUE(hover.has_value()) << static_cast<int>(hover.error());
    EXPECT_EQ(std::get<std::string>(hover->contents), "fn doc");
    ASSERT_TRUE(hover->range.has_value());
    EXPECT_EQ(hover->range->start.line, 1);
    EXPECT_EQ(hover->range->end.character, 4);
}

TEST(LspClientParser, ParseLocationsReadsUriAndRange) {
    auto client = make_lsp_for_parsing();
    auto locs = client.parse_locations(
        R"json([{"uri":"file:///a.cpp","range":{"start":{"line":0,"character":3},"end":{"line":0,"character":7}}}])json");
    ASSERT_TRUE(locs.has_value()) << static_cast<int>(locs.error());
    ASSERT_EQ(locs->size(), 1u);
    EXPECT_EQ((*locs)[0].uri, "file:///a.cpp");
    EXPECT_EQ((*locs)[0].range.start.character, 3);
    EXPECT_EQ((*locs)[0].range.end.character, 7);
}

TEST(LspClientParser, ParseDocumentSymbolsHandlesHierarchy) {
    auto client = make_lsp_for_parsing();
    auto syms = client.parse_document_symbols(
        R"([{"name":"main","kind":12,"range":{"start":{"line":0,"character":0},"end":{"line":2,"character":0}},"selectionRange":{"start":{"line":0,"character":0},"end":{"line":0,"character":4}},"children":[{"name":"x","kind":13,"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":1}},"selectionRange":{"start":{"line":1,"character":0},"end":{"line":1,"character":1}}}]}])");
    ASSERT_TRUE(syms.has_value());
    ASSERT_EQ(syms->size(), 1u);
    EXPECT_EQ((*syms)[0].name, "main");
    ASSERT_TRUE((*syms)[0].children.has_value());
    EXPECT_EQ((*syms)[0].children->size(), 1u);
    EXPECT_EQ((*syms)[0].children->front().name, "x");
}

TEST(LspClientParser, ParseCodeActionsReadsTitleKindPreferred) {
    auto client = make_lsp_for_parsing();
    auto actions = client.parse_code_actions(
        R"([{"title":"Fix me","kind":"quickfix","isPreferred":true}])");
    ASSERT_TRUE(actions.has_value());
    ASSERT_EQ(actions->size(), 1u);
    EXPECT_EQ((*actions)[0].title, "Fix me");
    ASSERT_TRUE((*actions)[0].kind.has_value());
    EXPECT_EQ((*actions)[0].kind.value(), "quickfix");
    EXPECT_TRUE((*actions)[0].is_preferred.value_or(false));
}

TEST(LspClientParser, ParseTextEditsReadsRangeAndNewText) {
    auto client = make_lsp_for_parsing();
    auto edits = client.parse_text_edits(
        R"([{"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":5}},"newText":"hello"}])");
    ASSERT_TRUE(edits.has_value());
    ASSERT_EQ(edits->size(), 1u);
    EXPECT_EQ((*edits)[0].new_text, "hello");
    EXPECT_EQ((*edits)[0].range.end.character, 5);
}

TEST(LspClientParser, ParseCompletionListEnrichesItems) {
    auto client = make_lsp_for_parsing();
    auto list = client.parse_completion_list(
        R"json({"isIncomplete":true,"items":[{"label":"foo","kind":3,"detail":"(int)","documentation":"doc","insertText":"foo()","sortText":"a"}]})json");
    ASSERT_TRUE(list.has_value());
    EXPECT_TRUE(list->is_incomplete);
    ASSERT_EQ(list->items.size(), 1u);
    const auto& item = list->items.front();
    EXPECT_EQ(item.label, "foo");
    ASSERT_TRUE(item.detail.has_value());  EXPECT_EQ(*item.detail, "(int)");
    ASSERT_TRUE(item.documentation.has_value()); EXPECT_EQ(*item.documentation, "doc");
    ASSERT_TRUE(item.insert_text.has_value());  EXPECT_EQ(*item.insert_text, "foo()");
}

TEST(LspClientParser, ParseInitializeResultPopulatesCapabilities) {
    auto client = make_lsp_for_parsing();
    auto init = client.parse_initialize_result(
        R"({"capabilities":{"hoverProvider":true,"definitionProvider":true,"completionProvider":{"triggerCharacters":["."]}},"serverInfo":{"name":"clangd","version":"17.0"}})");
    ASSERT_TRUE(init.has_value());
    EXPECT_TRUE(init->capabilities.hover_provider.value_or(false));
    EXPECT_TRUE(init->capabilities.definition_provider.value_or(false));
    ASSERT_TRUE(init->capabilities.completion_provider.has_value());
    ASSERT_TRUE(init->server_info.has_value());
    EXPECT_NE(init->server_info->find("clangd"), std::string::npos);
}

// ─── P2-07: WorkerRegistry + Server types smoke tests ───────────────────────

import cc.daemon.worker_registry;
import cc.server.types;

namespace {

TEST(WorkerRegistry, ExpiresStale) {
    auto& r = cc::daemon::WorkerRegistry::instance();
    r.clear();

    cc::daemon::WorkerInfo w;
    w.kind = cc::daemon::WorkerKind::InProcess;
    w.hostname = "localhost";
    w.capabilities = {"query"};
    w.max_concurrent_tasks = 1;
    auto id_r = r.register_worker(std::move(w));
    ASSERT_TRUE(id_r.has_value());
    const std::string id = *id_r;

    // Advance heartbeat to a known timestamp then manually expire it.
    (void)r.heartbeat(id, 0.0, 0, 0, cc::daemon::WorkerHealth::Healthy);
    // Use a very short TTL (1ms) + a 20ms sleep so the worker is older than TTL.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const size_t expired = r.expire_stale(std::chrono::milliseconds(1));
    EXPECT_EQ(expired, 1u);
    EXPECT_FALSE(r.lookup(id).has_value());
}

TEST(WorkerRegistry, PickBest) {
    auto& r = cc::daemon::WorkerRegistry::instance();
    r.clear();

    auto mk = [&](int cur, int max, uint64_t mem_used, uint64_t mem_limit,
                  std::string suffix) -> std::string {
        cc::daemon::WorkerInfo w;
        w.kind = cc::daemon::WorkerKind::Subprocess;
        w.hostname = "host" + suffix;
        w.capabilities = {"query"};
        w.current_tasks = cur;
        w.max_concurrent_tasks = max;
        w.memory_used_bytes = mem_used;
        w.memory_limit_bytes = mem_limit;
        return *r.register_worker(std::move(w));
    };

    const std::string id_a = mk(0, 1, 100, 1000, "A");   // ratio 0/1, free mem 900
    const std::string id_b = mk(1, 4, 200, 1000, "B");   // ratio 0.25
    const std::string id_c = mk(7, 8, 50,  1000, "C");   // ratio 0.875
    (void)id_c;

    cc::daemon::WorkerQueryFilters f;
    f.capability_required = "query";
    f.min_free_tasks = 0;          // do not filter on free slots here
    f.require_heartbeat_within_ms = 0;
    auto best = r.pick_best(f);
    ASSERT_TRUE(best.has_value());
    // Lowest load ratio = id_a (0/1).  If tie, most free memory (A still wins here).
    EXPECT_EQ(best->id, id_a);
}

TEST(WorkerRegistry, Cordon) {
    auto& r = cc::daemon::WorkerRegistry::instance();
    r.clear();

    cc::daemon::WorkerInfo w;
    w.kind = cc::daemon::WorkerKind::InProcess;
    w.hostname = "cordon";
    w.capabilities = {"query"};
    w.max_concurrent_tasks = 2;
    const std::string id = *r.register_worker(std::move(w));

    cc::daemon::WorkerQueryFilters f;
    f.capability_required = "query";
    f.min_free_tasks = 0;
    f.require_heartbeat_within_ms = 0;

    f.include_cordoned = false;
    EXPECT_EQ(r.find_matching(f).size(), 1u);

    EXPECT_TRUE(r.set_cordon(id, true));
    EXPECT_EQ(r.find_matching(f).size(), 0u);

    f.include_cordoned = true;
    EXPECT_EQ(r.find_matching(f).size(), 1u);
    EXPECT_TRUE(r.find_matching(f).front().cordoned);
}

TEST(ServerTypes, RoundtripSerde) {
    cc::server::ServerSession s;
    s.id = "session-1";
    s.token = "tok-abcdef";
    s.role = cc::server::Role::Admin;
    s.user_id = "u-42";
    s.user_agent = "test-agent/1.0";
    s.client_ip = "127.0.0.1";
    s.scopes = {"read", "write", "query"};
    s.created_ms = 1'000'000;
    s.expires_ms = 2'000'000;
    s.last_active_ms = 1'500'000;
    s.request_count = 17;
    s.revoked = false;

    const std::string json = cc::server::to_json(s);
    auto parsed = cc::server::ServerSession_from_json(json);
    ASSERT_TRUE(parsed.has_value()) << "parse error: " << (parsed.has_value() ? std::string{} : parsed.error());
    const auto& p = *parsed;
    EXPECT_EQ(p.id, s.id);
    EXPECT_EQ(p.token, s.token);
    EXPECT_EQ(p.role, s.role);
    EXPECT_EQ(p.user_id, s.user_id);
    EXPECT_EQ(p.user_agent, s.user_agent);
    EXPECT_EQ(p.client_ip, s.client_ip);
    EXPECT_EQ(p.scopes, s.scopes);
    EXPECT_EQ(p.created_ms, s.created_ms);
    EXPECT_EQ(p.expires_ms, s.expires_ms);
    EXPECT_EQ(p.last_active_ms, s.last_active_ms);
    EXPECT_EQ(p.request_count, s.request_count);
    EXPECT_EQ(p.revoked, s.revoked);
}

TEST(ServerTypes, RolesScopes) {
    cc::server::ServerSession s;
    s.role = cc::server::Role::Admin;
    s.scopes = {"read", "write"};
    s.expires_ms = 0;   // never expires
    s.revoked = false;

    EXPECT_TRUE(s.has_scope("read"));
    EXPECT_FALSE(s.has_scope("delete"));
    // Trivially satisfied empty scope query.
    EXPECT_TRUE(s.has_scope(""));
    // Not revoked and no expiry wall clock → not expired.
    EXPECT_FALSE(s.is_expired());
    // Manually inject a "now" that is in the future after a hypothetical expire_ms.
    s.expires_ms = 1000;
    EXPECT_TRUE(s.is_expired(2000));
    EXPECT_FALSE(s.is_expired(500));
    // Revoked always-expired semantics.
    s.revoked = true;
    EXPECT_TRUE(s.is_expired());
}

// ── Speculation suggestion engine (port of TS speculation.ts) ────────────────
// The deterministic ranker replaces the former single hardcoded suggestion.
// These tests pin the ranker's contract without a live LLM.

TEST(SpeculationSuggestion, EmptyTurnsReturnsNothing) {
    using cc::services::prompt_suggestion::rank_candidate_suggestions;
    using cc::services::prompt_suggestion::SuggestionRequest;
    SuggestionRequest req;
    EXPECT_TRUE(rank_candidate_suggestions(req).empty());
}

TEST(SpeculationSuggestion, NoAssistantTurnReturnsNothing) {
    using namespace cc::services::prompt_suggestion;
    SuggestionRequest req;
    req.recent_turns.push_back({.role = "user", .content = "implement the login flow"});
    // Early gate: needs >= 1 assistant turn before suggesting.
    EXPECT_TRUE(rank_candidate_suggestions(req).empty());
}

TEST(SpeculationSuggestion, AssistantTurnYieldsRankedCandidates) {
    using namespace cc::services::prompt_suggestion;
    SuggestionRequest req;
    req.recent_turns.push_back({.role = "user", .content = "implement the login flow"});
    req.recent_turns.push_back({.role = "assistant", .content = "I implemented the login flow with tests."});
    const auto r = rank_candidate_suggestions(req);
    EXPECT_FALSE(r.empty());
    for (const auto& s : r) {
        EXPECT_GE(s.confidence, 0.0);
        EXPECT_LE(s.confidence, 1.0);
        EXPECT_FALSE(s.text.empty());
    }
}

TEST(SpeculationSuggestion, ResultsSortedByConfidenceDesc) {
    using namespace cc::services::prompt_suggestion;
    SuggestionRequest req;
    req.recent_turns.push_back({.role = "user", .content = "refactor the module"});
    req.recent_turns.push_back({.role = "assistant", .content = "I refactored it and added tests."});
    const auto r = rank_candidate_suggestions(req);
    for (std::size_t i = 1; i < r.size(); ++i) {
        EXPECT_GE(r[i - 1].confidence, r[i].confidence);
    }
}

TEST(SpeculationSuggestion, RespectsMaxSuggestions) {
    using namespace cc::services::prompt_suggestion;
    SuggestionRequest req;
    req.max_suggestions = 2;
    req.recent_turns.push_back({.role = "user", .content = "ship the feature"});
    req.recent_turns.push_back({.role = "assistant", .content = "Shipped with full test coverage."});
    const auto r = rank_candidate_suggestions(req);
    EXPECT_LE(r.size(), 2u);
}

TEST(SpeculationSuggestion, QualityFilterRejectsEmpty) {
    using cc::services::prompt_suggestion::should_filter_suggestion;
    EXPECT_TRUE(should_filter_suggestion(""));
    EXPECT_TRUE(should_filter_suggestion("   "));
}

// ============================================================================
// ChannelPermission — short request ID generation
// ============================================================================
// TS REF: src/services/mcp/channelPermissions.ts:140-152

TEST(ChannelPermission, ShortRequestIdIsFiveLetters) {
    using namespace cc::services::mcp;
    auto id = short_request_id("toolu_01ABC123def456GHI789jkl");
    EXPECT_EQ(id.size(), 5u);
    for (char c : id) {
        EXPECT_GE(c, 'a');
        EXPECT_LE(c, 'z');
        EXPECT_NE(c, 'l');  // 'l' excluded from alphabet
    }
}

TEST(ChannelPermission, ShortRequestIdIsDeterministic) {
    using namespace cc::services::mcp;
    auto id1 = short_request_id("toolu_01ABC123def456GHI789jkl");
    auto id2 = short_request_id("toolu_01ABC123def456GHI789jkl");
    EXPECT_EQ(id1, id2);
}

TEST(ChannelPermission, ShortRequestIdDifferentInputsDiffer) {
    using namespace cc::services::mcp;
    auto id1 = short_request_id("toolu_01ABC123def456GHI789jkl");
    auto id2 = short_request_id("toolu_99XYZ999xyz999ABC999mno");
    EXPECT_NE(id1, id2);
}

TEST(ChannelPermission, ShortRequestIdAvoidsBlockedSubstrings) {
    using namespace cc::services::mcp;
    // The re-hash with salt should avoid producing IDs containing
    // blocklisted substrings. We test a few inputs that might hash to
    // problematic outputs.
    for (int i = 0; i < 100; ++i) {
        std::string tool_use_id = "toolu_test_" + std::to_string(i);
        auto id = short_request_id(tool_use_id);
        // Verify no blocked substring is present
        constexpr std::array<std::string_view, 24> blocked = {
            "fuck",  "shit",  "cunt",  "cock",  "dick",  "twat",  "piss",
            "crap",  "bitch", "whore", "ass",   "tit",   "cum",   "fag",
            "dyke",  "nig",   "kike",  "rape",  "nazi",  "damn",  "poo",
            "pee",   "wank",  "anus",
        };
        for (auto bad : blocked) {
            EXPECT_EQ(id.find(bad), std::string::npos)
                << "ID '" << id << "' contains blocked substring '" << bad << "'";
        }
    }
}

// ============================================================================
// ChannelPermission — truncate_for_preview
// ============================================================================
// TS REF: src/services/mcp/channelPermissions.ts:160-167

TEST(ChannelPermission, TruncateForPreviewShort) {
    using namespace cc::services::mcp;
    auto result = truncate_for_preview(R"({"cmd":"ls"})");
    EXPECT_EQ(result, R"({"cmd":"ls"})");
}

TEST(ChannelPermission, TruncateForPreviewLong) {
    using namespace cc::services::mcp;
    std::string long_str(300, 'x');
    auto result = truncate_for_preview(long_str);
    EXPECT_EQ(result.size(), 201u);  // 200 chars + "…"
    EXPECT_EQ(result.back(), char(0xE2));  // first byte of UTF-8 ellipsis …
}

TEST(ChannelPermission, TruncateForPreviewEmpty) {
    using namespace cc::services::mcp;
    auto result = truncate_for_preview("");
    EXPECT_EQ(result, "(unserializable)");
}

// ============================================================================
// ChannelPermission — parse_permission_reply
// ============================================================================
// TS REF: src/services/mcp/channelPermissions.ts:75

TEST(ChannelPermission, ParseReplyYesLowercase) {
    using namespace cc::services::mcp;
    auto parsed = parse_permission_reply("yes tbxkq");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->request_id, "tbxkq");
    EXPECT_EQ(parsed->behavior, ChannelPermissionBehavior::Allow);
}

TEST(ChannelPermission, ParseReplyNoLowercase) {
    using namespace cc::services::mcp;
    auto parsed = parse_permission_reply("no tbxkq");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->request_id, "tbxkq");
    EXPECT_EQ(parsed->behavior, ChannelPermissionBehavior::Deny);
}

TEST(ChannelPermission, ParseReplyYShortForm) {
    using namespace cc::services::mcp;
    auto parsed = parse_permission_reply("y tbxkq");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->behavior, ChannelPermissionBehavior::Allow);
}

TEST(ChannelPermission, ParseReplyNShortForm) {
    using namespace cc::services::mcp;
    auto parsed = parse_permission_reply("n tbxkq");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->behavior, ChannelPermissionBehavior::Deny);
}

TEST(ChannelPermission, ParseReplyCaseInsensitive) {
    using namespace cc::services::mcp;
    auto parsed = parse_permission_reply("YES TBXKQ");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->request_id, "tbxkq");  // lowercased
    EXPECT_EQ(parsed->behavior, ChannelPermissionBehavior::Allow);
}

TEST(ChannelPermission, ParseReplyWithWhitespacePadding) {
    using namespace cc::services::mcp;
    auto parsed = parse_permission_reply("  yes   tbxkq  ");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->request_id, "tbxkq");
}

TEST(ChannelPermission, ParseReplyRejectsBareYes) {
    using namespace cc::services::mcp;
    EXPECT_FALSE(parse_permission_reply("yes").has_value());
}

TEST(ChannelPermission, ParseReplyRejectsIdWithL) {
    using namespace cc::services::mcp;
    // 'l' is excluded from the alphabet (looks like 1/I)
    EXPECT_FALSE(parse_permission_reply("yes tblkq").has_value());
}

TEST(ChannelPermission, ParseReplyRejectsExtraText) {
    using namespace cc::services::mcp;
    EXPECT_FALSE(parse_permission_reply("yes tbxkq please").has_value());
}

TEST(ChannelPermission, ParseReplyRejectsWrongIdLength) {
    using namespace cc::services::mcp;
    EXPECT_FALSE(parse_permission_reply("yes tbxk").has_value());   // 4 chars
    EXPECT_FALSE(parse_permission_reply("yes tbxkqq").has_value()); // 6 chars
}

// ============================================================================
// ChannelPermission — ChannelPermissionCallbacks
// ============================================================================
// TS REF: src/services/mcp/channelPermissions.ts:46-61, 209-240

TEST(ChannelPermission, CallbacksResolveAllow) {
    using namespace cc::services::mcp;
    auto cbs = create_channel_permission_callbacks();
    bool called = false;
    ChannelPermissionBehavior received_behavior{};
    std::string received_server;

    auto unsub = cbs->on_response("tbxkq", [&](const ChannelPermissionResponse& resp) {
        called = true;
        received_behavior = resp.behavior;
        received_server = resp.from_server;
    });

    bool resolved = cbs->resolve("tbxkq", ChannelPermissionBehavior::Allow, "plugin:telegram:tg");
    EXPECT_TRUE(resolved);
    EXPECT_TRUE(called);
    EXPECT_EQ(received_behavior, ChannelPermissionBehavior::Allow);
    EXPECT_EQ(received_server, "plugin:telegram:tg");
}

TEST(ChannelPermission, CallbacksResolveDeny) {
    using namespace cc::services::mcp;
    auto cbs = create_channel_permission_callbacks();
    ChannelPermissionBehavior received{};
    cbs->on_response("abcde", [&](const ChannelPermissionResponse& resp) {
        received = resp.behavior;
    });
    cbs->resolve("abcde", ChannelPermissionBehavior::Deny, "test");
    EXPECT_EQ(received, ChannelPermissionBehavior::Deny);
}

TEST(ChannelPermission, CallbacksResolveReturnsFalseForUnknown) {
    using namespace cc::services::mcp;
    auto cbs = create_channel_permission_callbacks();
    EXPECT_FALSE(cbs->resolve("zzzzz", ChannelPermissionBehavior::Allow, "test"));
}

TEST(ChannelPermission, CallbacksUnsubscribePreventsResolve) {
    using namespace cc::services::mcp;
    auto cbs = create_channel_permission_callbacks();
    bool called = false;
    auto unsub = cbs->on_response("tbxkq", [&](const ChannelPermissionResponse&) {
        called = true;
    });
    unsub();  // unsubscribe
    EXPECT_FALSE(cbs->resolve("tbxkq", ChannelPermissionBehavior::Allow, "test"));
    EXPECT_FALSE(called);
}

TEST(ChannelPermission, CallbacksCaseInsensitiveMatching) {
    using namespace cc::services::mcp;
    auto cbs = create_channel_permission_callbacks();
    bool called = false;
    cbs->on_response("TBXKQ", [&](const ChannelPermissionResponse&) {
        called = true;
    });
    // Resolve with different case
    EXPECT_TRUE(cbs->resolve("tbxkq", ChannelPermissionBehavior::Allow, "test"));
    EXPECT_TRUE(called);
}

TEST(ChannelPermission, CallbacksResolveDeletesBeforeCalling) {
    using namespace cc::services::mcp;
    auto cbs = create_channel_permission_callbacks();
    int call_count = 0;
    cbs->on_response("tbxkq", [&](const ChannelPermissionResponse&) {
        ++call_count;
    });
    // First resolve succeeds
    EXPECT_TRUE(cbs->resolve("tbxkq", ChannelPermissionBehavior::Allow, "test"));
    // Second resolve on same ID fails (already consumed)
    EXPECT_FALSE(cbs->resolve("tbxkq", ChannelPermissionBehavior::Allow, "test"));
    EXPECT_EQ(call_count, 1);
}

TEST(ChannelPermission, CallbacksPendingCount) {
    using namespace cc::services::mcp;
    auto cbs = create_channel_permission_callbacks();
    EXPECT_EQ(cbs->pending_count(), 0u);
    auto u1 = cbs->on_response("aaaaa", [](auto){});
    EXPECT_EQ(cbs->pending_count(), 1u);
    auto u2 = cbs->on_response("bbbbb", [](auto){});
    EXPECT_EQ(cbs->pending_count(), 2u);
    u1();
    EXPECT_EQ(cbs->pending_count(), 1u);
    cbs->resolve("bbbbb", ChannelPermissionBehavior::Allow, "test");
    EXPECT_EQ(cbs->pending_count(), 0u);
}

// ============================================================================
// ChannelPermission — filter_permission_relay_clients
// ============================================================================
// TS REF: src/services/mcp/channelPermissions.ts:177-194

namespace {
struct TestMcpClient {
    std::string name;
    cc::services::mcp::ServerState state = cc::services::mcp::ServerState::Ready;
    cc::services::mcp::ServerCapabilities capabilities;
};
} // anonymous namespace

TEST(ChannelPermission, FilterRelayRequiresConnected) {
    using namespace cc::services::mcp;
    std::vector<TestMcpClient> clients = {
        {"telegram", ServerState::Ready, {}},
        {"discord", ServerState::Error, {}},
    };
    clients[0].capabilities.experimental["claude/channel"] = "true";
    clients[0].capabilities.experimental["claude/channel/permission"] = "true";
    clients[1].capabilities.experimental["claude/channel"] = "true";
    clients[1].capabilities.experimental["claude/channel/permission"] = "true";

    auto filtered = filter_permission_relay_clients<TestMcpClient>(
        clients, [](auto) { return true; });
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].name, "telegram");
}

TEST(ChannelPermission, FilterRelayRequiresAllowlist) {
    using namespace cc::services::mcp;
    std::vector<TestMcpClient> clients = {
        {"telegram", ServerState::Ready, {}},
        {"discord", ServerState::Ready, {}},
    };
    for (auto& c : clients) {
        c.capabilities.experimental["claude/channel"] = "true";
        c.capabilities.experimental["claude/channel/permission"] = "true";
    }

    auto filtered = filter_permission_relay_clients<TestMcpClient>(
        clients, [](auto name) { return name == "telegram"; });
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].name, "telegram");
}

TEST(ChannelPermission, FilterRelayRequiresBothCapabilities) {
    using namespace cc::services::mcp;
    std::vector<TestMcpClient> clients = {
        {"both", ServerState::Ready, {}},
        {"channel_only", ServerState::Ready, {}},
        {"permission_only", ServerState::Ready, {}},
        {"neither", ServerState::Ready, {}},
    };
    clients[0].capabilities.experimental["claude/channel"] = "true";
    clients[0].capabilities.experimental["claude/channel/permission"] = "true";
    clients[1].capabilities.experimental["claude/channel"] = "true";
    clients[2].capabilities.experimental["claude/channel/permission"] = "true";

    auto filtered = filter_permission_relay_clients<TestMcpClient>(
        clients, [](auto) { return true; });
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].name, "both");
}

TEST(ChannelPermission, FilterRelayEmptyInput) {
    using namespace cc::services::mcp;
    std::vector<TestMcpClient> clients;
    auto filtered = filter_permission_relay_clients<TestMcpClient>(
        clients, [](auto) { return true; });
    EXPECT_TRUE(filtered.empty());
}

// ============================================================================
// ChannelPermission — ChannelPermissionStore
// ============================================================================
// TS REF: conceptual extension (persistent permission rules)

TEST(ChannelPermission, StoreDefaultIsPrompt) {
    using namespace cc::services::mcp;
    ChannelPermissionStore store;
    // No rules loaded → default to Prompt
    EXPECT_EQ(store.check_permission("any_server", "any_tool"),
              ChannelPermission::Prompt);
}

TEST(ChannelPermission, StoreGlobalRuleApplies) {
    using namespace cc::services::mcp;
    ChannelPermissionStore store;
    store.set_permission(ChannelPermissionStore::make_global_rule(
        ChannelPermission::Allowed));
    EXPECT_EQ(store.check_permission("server_a", "tool_x"),
              ChannelPermission::Allowed);
    EXPECT_EQ(store.check_permission("server_b", "tool_y"),
              ChannelPermission::Allowed);
}

TEST(ChannelPermission, StoreServerRuleOverridesGlobal) {
    using namespace cc::services::mcp;
    ChannelPermissionStore store;
    store.set_permission(ChannelPermissionStore::make_global_rule(
        ChannelPermission::Prompt));
    store.set_permission(ChannelPermissionStore::make_server_rule(
        "trusted_server", ChannelPermission::Allowed));
    EXPECT_EQ(store.check_permission("trusted_server", "any_tool"),
              ChannelPermission::Allowed);
    EXPECT_EQ(store.check_permission("other_server", "any_tool"),
              ChannelPermission::Prompt);
}

TEST(ChannelPermission, StoreToolRuleOverridesServer) {
    using namespace cc::services::mcp;
    ChannelPermissionStore store;
    store.set_permission(ChannelPermissionStore::make_server_rule(
        "my_server", ChannelPermission::Allowed));
    store.set_permission(ChannelPermissionStore::make_tool_rule(
        "my_server", "dangerous_tool", ChannelPermission::Denied));
    EXPECT_EQ(store.check_permission("my_server", "safe_tool"),
              ChannelPermission::Allowed);
    EXPECT_EQ(store.check_permission("my_server", "dangerous_tool"),
              ChannelPermission::Denied);
}

TEST(ChannelPermission, StoreMostSpecificWins) {
    using namespace cc::services::mcp;
    ChannelPermissionStore store;
    store.set_permission(ChannelPermissionStore::make_global_rule(
        ChannelPermission::Prompt));
    store.set_permission(ChannelPermissionStore::make_server_rule(
        "srv", ChannelPermission::Allowed));
    store.set_permission(ChannelPermissionStore::make_tool_rule(
        "srv", "tool", ChannelPermission::Denied));
    // Tool rule (score 3) > Server rule (score 2) > Global rule (score 1)
    EXPECT_EQ(store.check_permission("srv", "tool"),
              ChannelPermission::Denied);
}

TEST(ChannelPermission, StoreRemoveRule) {
    using namespace cc::services::mcp;
    ChannelPermissionStore store;
    store.set_permission(ChannelPermissionStore::make_server_rule(
        "srv", ChannelPermission::Allowed));
    EXPECT_EQ(store.check_permission("srv", "tool"),
              ChannelPermission::Allowed);
    bool removed = store.remove_rule(ChannelPermissionScope::Server, "srv", "");
    EXPECT_TRUE(removed);
    EXPECT_EQ(store.check_permission("srv", "tool"),
              ChannelPermission::Prompt);
}

TEST(ChannelPermission, StoreRemoveNonexistentReturnsFalse) {
    using namespace cc::services::mcp;
    ChannelPermissionStore store;
    EXPECT_FALSE(store.remove_rule(ChannelPermissionScope::Server, "nope", ""));
}

TEST(ChannelPermission, StoreUpsertSameIdentity) {
    using namespace cc::services::mcp;
    ChannelPermissionStore store;
    store.set_permission(ChannelPermissionStore::make_server_rule(
        "srv", ChannelPermission::Allowed));
    // Setting same identity again should update, not duplicate
    store.set_permission(ChannelPermissionStore::make_server_rule(
        "srv", ChannelPermission::Denied));
    EXPECT_EQ(store.rule_count(), 1u);
    EXPECT_EQ(store.check_permission("srv", "tool"),
              ChannelPermission::Denied);
}

TEST(ChannelPermission, StoreGetAllRules) {
    using namespace cc::services::mcp;
    ChannelPermissionStore store;
    store.set_permission(ChannelPermissionStore::make_global_rule(
        ChannelPermission::Prompt));
    store.set_permission(ChannelPermissionStore::make_server_rule(
        "srv", ChannelPermission::Allowed));
    auto rules = store.get_all_rules();
    EXPECT_EQ(rules.size(), 2u);
}

TEST(ChannelPermission, StorePersistenceRoundtrip) {
    using namespace cc::services::mcp;
    // Use a temp file path for testing
    auto original_path = ChannelPermissionStore::file_path();
    // Create a store, set rules, save
    {
        ChannelPermissionStore store;
        store.set_permission(ChannelPermissionStore::make_server_rule(
            "test_server", ChannelPermission::Allowed));
        store.set_permission(ChannelPermissionStore::make_tool_rule(
            "test_server", "dangerous", ChannelPermission::Denied));
        store.save();
    }
    // Load into a new store and verify
    {
        ChannelPermissionStore store;
        store.load();
        EXPECT_EQ(store.check_permission("test_server", "safe"),
                  ChannelPermission::Allowed);
        EXPECT_EQ(store.check_permission("test_server", "dangerous"),
                  ChannelPermission::Denied);
    }
    // Cleanup: remove the test file
    std::error_code ec;
    fs::remove(original_path, ec);
}

TEST(ChannelPermission, StoreFactoryCreatesLoaded) {
    using namespace cc::services::mcp;
    // Clean slate
    auto path = ChannelPermissionStore::file_path();
    std::error_code ec;
    fs::remove(path, ec);

    auto store = create_channel_permission_store();
    ASSERT_NE(store, nullptr);
    // Should have loaded from disk (empty rules → default Prompt)
    EXPECT_EQ(store->check_permission("any", "tool"),
              ChannelPermission::Prompt);
    EXPECT_EQ(store->rule_count(), 0u);
}

// ============================================================================
// ChannelPermission — string conversion utilities
// ============================================================================

TEST(ChannelPermission, PermissionToString) {
    using namespace cc::services::mcp;
    EXPECT_EQ(channel_permission_to_string(ChannelPermission::Allowed), "Allowed");
    EXPECT_EQ(channel_permission_to_string(ChannelPermission::Denied), "Denied");
    EXPECT_EQ(channel_permission_to_string(ChannelPermission::Prompt), "Prompt");
}

TEST(ChannelPermission, ScopeToString) {
    using namespace cc::services::mcp;
    EXPECT_EQ(channel_permission_scope_to_string(ChannelPermissionScope::Global), "Global");
    EXPECT_EQ(channel_permission_scope_to_string(ChannelPermissionScope::Server), "Server");
    EXPECT_EQ(channel_permission_scope_to_string(ChannelPermissionScope::Tool), "Tool");
}

// ============================================================================
// ChannelPermission — feature gate
// ============================================================================

TEST(ChannelPermission, FeatureGateDefaultsToFalse) {
    using namespace cc::services::mcp;
    // Stub returns false until GrowthBook integration exists
    EXPECT_FALSE(is_channel_permission_relay_enabled());
}

}  // namespace
