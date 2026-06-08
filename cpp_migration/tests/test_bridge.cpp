/// @file test_bridge.cpp
/// @brief Bridge module smoke tests aligned with current C++ module APIs.

#include <gtest/gtest.h>
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
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <openssl/sha.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <signal.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

import cc.bridge.api;
import cc.bridge.config;
import cc.bridge.messages;
import cc.bridge.session_id_compat;
import cc.bridge.transport;
import cc.bridge.work_secret;
import cc.daemon.daemon_client;
import cc.daemon.daemon_server;

namespace {

class ScopedEnvVar {
public:
    explicit ScopedEnvVar(const char* name) : name_(name) {
        if (auto* value = std::getenv(name)) previous_ = value;
        unsetenv(name);
    }
    ScopedEnvVar(const char* name, std::string_view value) : name_(name) {
        if (auto* previous = std::getenv(name)) previous_ = previous;
        setenv(name_, std::string(value).c_str(), 1);
    }
    ~ScopedEnvVar() {
        if (previous_) setenv(name_, previous_->c_str(), 1);
        else unsetenv(name_);
    }
    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
    const char* name_;
    std::optional<std::string> previous_;
};

std::filesystem::path unique_temp_file(std::string_view suffix) {
    return std::filesystem::temp_directory_path() / (cc::bridge::generate_session_id() + std::string(suffix));
}

std::string extract_json_string_field(std::string_view json, std::string_view key) {
    const auto pattern = std::string("\"") + std::string(key) + "\":\"";
    auto pos = json.find(pattern);
    if (pos == std::string_view::npos) return {};
    pos += pattern.size();
    auto end = json.find('"', pos);
    if (end == std::string_view::npos) return {};
    return std::string(json.substr(pos, end - pos));
}

bool bridge_test_send_all(int fd, std::string_view data) {
    while (!data.empty()) {
        auto n = ::send(fd, data.data(), data.size(), 0);
        if (n <= 0) return false;
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

std::string bridge_test_base64_encode(const unsigned char* data, std::size_t len) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(table[(n >> 18) & 0x3f]);
        out.push_back(table[(n >> 12) & 0x3f]);
        out.push_back((i + 1 < len) ? table[(n >> 6) & 0x3f] : '=');
        out.push_back((i + 2 < len) ? table[n & 0x3f] : '=');
    }
    return out;
}

std::string bridge_test_base64url_encode(std::string_view data) {
    auto encoded = bridge_test_base64_encode(
        reinterpret_cast<const unsigned char*>(data.data()),
        data.size());
    for (char& ch : encoded) {
        if (ch == '+') ch = '-';
        else if (ch == '/') ch = '_';
    }
    while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
    return encoded;
}

std::string bridge_test_work_secret_json() {
    return R"({"version":1,"session_ingress_token":"session-token-from-secret","api_base_url":"http://session-ingress.local","use_code_sessions":true,"code_session_mode":"code-session","sources":[{"type":"github","id":"source-1"}],"auth":{"records":[{"type":"oauth","name":"console"}]},"mcp_config":{"servers":{"linear":{"transport":"http","url":"https://mcp.example"}}},"environment_variables":{"REMOTE_FLAG":"enabled","REMOTE_COUNT":42}})";
}

std::string bridge_test_encoded_work_secret() {
    return bridge_test_base64url_encode(bridge_test_work_secret_json());
}

std::string bridge_test_encoded_work_secret(std::string_view api_base_url) {
    return bridge_test_base64url_encode(std::format(
        R"({{"version":1,"session_ingress_token":"session-token-from-secret","api_base_url":"{}","use_code_sessions":true,"code_session_mode":"code-session","sources":[{{"type":"github","id":"source-1"}}],"auth":{{"records":[{{"type":"oauth","name":"console"}}]}},"mcp_config":{{"servers":{{"linear":{{"transport":"http","url":"https://mcp.example"}}}}}},"environment_variables":{{"REMOTE_FLAG":"enabled","REMOTE_COUNT":42}}}})",
        api_base_url));
}

std::filesystem::path native_cc_repl_binary_path() {
    if (const char* path = std::getenv("CC_REPL_TEST_NATIVE_BINARY"); path && *path) {
        return path;
    }
#ifdef CC_REPL_NATIVE_BINARY_PATH
    return std::filesystem::path{CC_REPL_NATIVE_BINARY_PATH};
#else
    return {};
#endif
}

std::string bridge_test_v1_work_secret_json() {
    return R"({"version":1,"session_ingress_token":"session-token-from-secret","api_base_url":"http://127.0.0.1:19191","use_code_sessions":false})";
}

std::string bridge_test_encoded_v1_work_secret() {
    return bridge_test_base64url_encode(bridge_test_v1_work_secret_json());
}

std::string bridge_test_accept_key(std::string_view key) {
    const std::string input = std::string(key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::array<unsigned char, 20> hash{};
    SHA1(reinterpret_cast<const unsigned char*>(input.data()), input.size(), hash.data());
    return bridge_test_base64_encode(hash.data(), hash.size());
}

class LocalBridgeWebSocketServer {
public:
    LocalBridgeWebSocketServer() {
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

    ~LocalBridgeWebSocketServer() {
        running_.store(false);
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        const int client_fd = client_fd_.exchange(-1);
        if (client_fd >= 0) {
            ::shutdown(client_fd, SHUT_RDWR);
            ::close(client_fd);
        }
        if (accept_thread_.joinable()) {
            accept_thread_.request_stop();
            accept_thread_.join();
        }
    }

    [[nodiscard]] bool ready() const noexcept { return listen_fd_ >= 0 && port_ != 0; }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    [[nodiscard]] std::optional<std::vector<std::string>> wait_for_frames(
        std::size_t count,
        std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this, count] { return frames_.size() >= count; })) {
            return std::nullopt;
        }
        return frames_;
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
            int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (fd < 0) {
                if (!running_.load()) break;
                continue;
            }
            client_fd_.store(fd);
            handle_client(fd);
            const int owned_fd = client_fd_.exchange(-1);
            if (owned_fd >= 0) ::close(owned_fd);
            break;
        }
    }

    static std::optional<std::string> read_http_headers(int fd) {
        std::string headers;
        std::array<char, 1024> buffer{};
        while (headers.find("\r\n\r\n") == std::string::npos) {
            auto n = ::recv(fd, buffer.data(), buffer.size(), 0);
            if (n <= 0) return std::nullopt;
            headers.append(buffer.data(), static_cast<std::size_t>(n));
            if (headers.size() > 64 * 1024) return std::nullopt;
        }
        return headers;
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
        return bridge_test_send_all(fd, frame);
    }

    void handle_client(int fd) {
        auto headers = read_http_headers(fd);
        if (!headers) return;
        const auto key = header_value(*headers, "Sec-WebSocket-Key");
        const auto response = std::format(
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: {}\r\n\r\n",
            bridge_test_accept_key(key));
        if (!bridge_test_send_all(fd, response)) return;

        bool sent_inbound = false;
        while (running_.load()) {
            auto frame = read_frame(fd);
            if (!frame) break;
            if (frame->opcode == 0x8) break;
            if (frame->opcode != 0x1 && frame->opcode != 0x2) continue;
            {
                std::lock_guard lock(mutex_);
                frames_.push_back(frame->payload);
            }
            cv_.notify_all();
            if (!sent_inbound) {
                sent_inbound = true;
                send_text_frame(fd,
                    R"({"id":"srv-1","type":"event","method":"server/pong","payload":{"ok":true},"priority":"normal"})");
            }
        }
    }

    int listen_fd_ = -1;
    std::atomic<int> client_fd_{-1};
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::jthread accept_thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::string> frames_;
};

struct LocalBridgeApiRequest {
    std::string method;
    std::string path;
    std::string headers;
    std::string body;
};

class LocalBridgeApiHttpServer {
public:
    LocalBridgeApiHttpServer() {
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

    ~LocalBridgeApiHttpServer() {
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
    }

    [[nodiscard]] bool ready() const noexcept { return listen_fd_ >= 0 && port_ != 0; }
    [[nodiscard]] std::string base_url() const { return std::format("http://127.0.0.1:{}", port_); }

    [[nodiscard]] std::optional<std::vector<LocalBridgeApiRequest>> wait_for_requests(
        std::size_t count,
        std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this, count] { return requests_.size() >= count; })) {
            return std::nullopt;
        }
        return requests_;
    }

    void fail_next_work_polls(int count, int status, std::string reason, std::string body) {
        std::lock_guard lock(mutex_);
        work_poll_failures_remaining_ = count;
        work_poll_failure_status_ = status;
        work_poll_failure_reason_ = std::move(reason);
        work_poll_failure_body_ = std::move(body);
    }

    void fail_next_heartbeats(int count, int status, std::string reason, std::string body) {
        std::lock_guard lock(mutex_);
        heartbeat_failures_remaining_ = count;
        heartbeat_failure_status_ = status;
        heartbeat_failure_reason_ = std::move(reason);
        heartbeat_failure_body_ = std::move(body);
    }

    void set_work_secret(std::string secret) {
        std::lock_guard lock(mutex_);
        work_secret_ = std::move(secret);
    }

private:
    void accept_loop(std::stop_token stop) {
        while (!stop.stop_requested() && running_.load()) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (fd < 0) {
                if (!running_.load()) break;
                continue;
            }
            handle_client(fd);
            ::close(fd);
        }
    }

    static std::optional<LocalBridgeApiRequest> read_request(int fd) {
        std::string request;
        std::array<char, 4096> buffer{};
        std::size_t header_end = std::string::npos;
        while ((header_end = request.find("\r\n\r\n")) == std::string::npos) {
            auto n = ::recv(fd, buffer.data(), buffer.size(), 0);
            if (n <= 0) return std::nullopt;
            request.append(buffer.data(), static_cast<std::size_t>(n));
            if (request.size() > 64 * 1024) return std::nullopt;
        }

        const auto headers = request.substr(0, header_end + 4);
        std::size_t content_length = 0;
        auto length_pos = headers.find("Content-Length:");
        if (length_pos == std::string::npos) length_pos = headers.find("content-length:");
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
            auto n = ::recv(fd, buffer.data(), buffer.size(), 0);
            if (n <= 0) break;
            request.append(buffer.data(), static_cast<std::size_t>(n));
        }

        auto first_line_end = headers.find("\r\n");
        if (first_line_end == std::string::npos) return std::nullopt;
        std::istringstream first_line(headers.substr(0, first_line_end));
        LocalBridgeApiRequest parsed;
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
        return bridge_test_send_all(fd, response);
    }

    static bool send_sse_response(int fd, std::string_view body) {
        const auto response = std::format(
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            body.size(),
            body);
        return bridge_test_send_all(fd, response);
    }

    [[nodiscard]] std::string work_secret() const {
        std::lock_guard lock(mutex_);
        return work_secret_;
    }

    bool consume_work_poll_failure(int& status, std::string& reason, std::string& body) {
        std::lock_guard lock(mutex_);
        if (work_poll_failures_remaining_ <= 0) return false;
        --work_poll_failures_remaining_;
        status = work_poll_failure_status_;
        reason = work_poll_failure_reason_;
        body = work_poll_failure_body_;
        return true;
    }

    bool consume_heartbeat_failure(int& status, std::string& reason, std::string& body) {
        std::lock_guard lock(mutex_);
        if (heartbeat_failures_remaining_ <= 0) return false;
        --heartbeat_failures_remaining_;
        status = heartbeat_failure_status_;
        reason = heartbeat_failure_reason_;
        body = heartbeat_failure_body_;
        return true;
    }

    void handle_client(int fd) {
        auto request = read_request(fd);
        if (!request) return;
        {
            std::lock_guard lock(mutex_);
            requests_.push_back(*request);
        }
        cv_.notify_all();

        if (request->method == "POST" && request->path == "/v1/environments/bridge") {
            send_response(fd, 201, "Created",
                R"({"environment_id":"env_backend_1","environment_secret":"env_secret_1"})");
            return;
        }
        if (request->method == "POST" && request->path == "/bridge/messages") {
            send_response(fd, 202, "Accepted", R"({"ok":true})");
            return;
        }
        if (request->method == "GET" && request->path == "/bridge/poll") {
            send_response(fd, 200, "OK",
                R"({"messages":[{"id":"poll-1","type":"event","method":"server/poll","payload":{"ok":true},"priority":"high","correlation_id":"corr-1"}]})");
            return;
        }
        if (request->method == "GET" &&
            request->path.rfind("/v1/environments/env_backend_1/work/poll", 0) == 0) {
            int status = 0;
            std::string reason;
            std::string body;
            if (consume_work_poll_failure(status, reason, body)) {
                send_response(fd, status, reason, body);
                return;
            }
        }
        if (request->method == "GET" &&
            request->path == "/v1/environments/env_backend_1/work/poll?reclaim_older_than_ms=12345") {
            send_response(fd, 200, "OK",
                std::format(
                    R"({{"id":"work_1","secret":"{}","data":{{"type":"session","id":"session_1"}}}})",
                    work_secret()));
            return;
        }
        if (request->method == "GET" && request->path == "/v1/environments/env_backend_1/work/poll") {
            send_response(fd, 200, "OK",
                std::format(
                    R"({{"id":"work_1","secret":"{}","data":{{"type":"session","id":"session_1"}}}})",
                    work_secret()));
            return;
        }
        if (request->method == "POST" && request->path == "/v1/environments/env_backend_1/work/work_1/ack") {
            send_response(fd, 200, "OK", R"({"ok":true})");
            return;
        }
        if (request->method == "POST" && request->path == "/v1/code/sessions/session_1/worker/register") {
            send_response(fd, 200, "OK", R"({"worker_epoch":"42"})");
            return;
        }
        if (request->method == "POST" && request->path == "/v1/code/sessions/session_1/worker/events") {
            send_response(fd, 201, "Created", R"({"ok":true})");
            return;
        }
        if (request->method == "GET" && request->path == "/v1/code/sessions/session_1/worker/events/stream") {
            const int count = next_sse_stream_count();
            if (count == 1) {
                send_sse_response(fd,
                    "id: 1\n"
                    "event: client_event\n"
                    "data: {\"event_id\":\"evt_daemon_product_1\",\"sequence_num\":1,\"event_type\":\"user\",\"source\":\"test\",\"created_at\":\"2026-06-07T00:00:01Z\",\"payload\":{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"run native daemon bridge product e2e\"},\"parent_tool_use_id\":null,\"session_id\":\"session_1\"}}\n\n");
                return;
            }
            send_sse_response(fd,
                "id: 2\n"
                "event: client_event\n"
                "data: {\"event_id\":\"evt_daemon_product_keepalive\",\"sequence_num\":2,\"event_type\":\"keep_alive\",\"source\":\"test\",\"created_at\":\"2026-06-07T00:00:02Z\",\"payload\":{\"type\":\"keep_alive\",\"session_id\":\"session_1\"}}\n\n");
            return;
        }
        if (request->method == "PUT" && request->path == "/v1/code/sessions/session_1/worker") {
            send_response(fd, 200, "OK", R"({"ok":true})");
            return;
        }
        if (request->method == "POST" && request->path == "/v1/code/sessions/session_1/worker/heartbeat") {
            send_response(fd, 200, "OK", R"({"ok":true})");
            return;
        }
        if (request->method == "POST" && request->path == "/v1/code/sessions/session_1/worker/events/delivery") {
            send_response(fd, 200, "OK", R"({"ok":true})");
            return;
        }
        if (request->method == "POST" && request->path == "/v1/messages") {
            send_response(fd, 200, "OK",
                R"({"id":"msg_bridge_daemon_product","type":"message","role":"assistant","model":"claude-test","content":[{"type":"text","text":"native daemon bridge product reply"}],"stop_reason":"end_turn","usage":{"input_tokens":6,"output_tokens":8}})");
            return;
        }
        if (request->method == "POST" && request->path == "/v1/environments/env_backend_1/work/work_1/heartbeat") {
            int status = 0;
            std::string reason;
            std::string body;
            if (consume_heartbeat_failure(status, reason, body)) {
                send_response(fd, status, reason, body);
                return;
            }
            send_response(fd, 200, "OK",
                R"({"lease_extended":true,"state":"active","last_heartbeat":"2026-06-07T00:00:00Z","ttl_seconds":30})");
            return;
        }
        if (request->method == "POST" && request->path == "/v1/sessions/session_1/events") {
            send_response(fd, 201, "Created", R"({"ok":true})");
            return;
        }
        if (request->method == "POST" && request->path == "/v1/sessions/session_1/archive") {
            send_response(fd, 200, "OK", R"({"ok":true})");
            return;
        }
        if (request->method == "POST" && request->path == "/v1/environments/env_backend_1/work/work_1/stop") {
            send_response(fd, 200, "OK", R"({"ok":true})");
            return;
        }
        if (request->method == "DELETE" && request->path == "/v1/environments/bridge/env_backend_1") {
            send_response(fd, 204, "No Content", "");
            return;
        }
        send_response(fd, 404, "Not Found", R"({"error":"not found"})");
    }

    int next_sse_stream_count() {
        std::lock_guard lock(mutex_);
        return ++sse_stream_count_;
    }

    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::jthread accept_thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<LocalBridgeApiRequest> requests_;
    int work_poll_failures_remaining_ = 0;
    int work_poll_failure_status_ = 500;
    std::string work_poll_failure_reason_{"Internal Server Error"};
    std::string work_poll_failure_body_{R"({"error":"server error"})"};
    int heartbeat_failures_remaining_ = 0;
    int heartbeat_failure_status_ = 500;
    std::string heartbeat_failure_reason_{"Internal Server Error"};
    std::string heartbeat_failure_body_{R"({"error":"server error"})"};
    std::string work_secret_{bridge_test_encoded_work_secret()};
    int sse_stream_count_ = 0;
};

} // namespace

TEST(BridgeMessages, DetectsAndNormalizesMalformedBase64Images) {
    cc::bridge::ContentBlock malformed = cc::bridge::ImageBlock{
        .type = cc::bridge::ContentBlockType::Image,
        .source = {.media_type = "", .data = "iVBORw0KGgo="},
    };

    EXPECT_TRUE(cc::bridge::is_malformed_base64_image(malformed));
    EXPECT_EQ(cc::bridge::detect_image_format_from_base64("iVBORw0KGgo="), "image/png");

    auto normalized = cc::bridge::normalize_image_blocks({malformed});
    ASSERT_EQ(normalized.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<cc::bridge::ImageBlock>(normalized.front()));
    EXPECT_EQ(std::get<cc::bridge::ImageBlock>(normalized.front()).source.media_type, "image/png");
}

TEST(BridgeMessages, ExtractsOnlyUserMessagesWithContent) {
    cc::bridge::SDKMessage ignored;
    ignored.type = "assistant";
    EXPECT_FALSE(cc::bridge::extract_inbound_message_fields(ignored).has_value());

    cc::bridge::SDKMessage user;
    user.type = "user";
    user.message.content = std::string("hello bridge");
    user.uuid = "uuid-1";

    auto extracted = cc::bridge::extract_inbound_message_fields(user);
    ASSERT_TRUE(extracted.has_value());
    ASSERT_TRUE(std::holds_alternative<std::string>(extracted->content));
    EXPECT_EQ(std::get<std::string>(extracted->content), "hello bridge");
    ASSERT_TRUE(extracted->uuid.has_value());
    EXPECT_EQ(*extracted->uuid, "uuid-1");
}

TEST(BridgeMessages, FlushGateBuffersUntilOpened) {
    std::vector<std::string> handled;
    cc::bridge::MessageFlushGate gate([&handled](const cc::bridge::SDKMessage& msg) {
        handled.push_back(msg.type);
    });

    cc::bridge::SDKMessage message;
    message.type = "user";
    gate.enqueue(message);
    EXPECT_EQ(gate.buffer().size(), 1u);
    EXPECT_TRUE(handled.empty());

    gate.open();
    EXPECT_TRUE(gate.is_open());
    EXPECT_TRUE(gate.buffer().empty());
    ASSERT_EQ(handled.size(), 1u);
    EXPECT_EQ(handled.front(), "user");
}

TEST(BridgeConfig, LoadsDefaultsEnvironmentAndJsonFile) {
    ScopedEnvVar clear_port("CC_BRIDGE_PORT");
    ScopedEnvVar clear_host("CC_BRIDGE_HOST");
    ScopedEnvVar clear_token("CC_BRIDGE_TOKEN");

    cc::bridge::BridgeConfigLoader loader;
    auto defaults = loader.load();
    ASSERT_TRUE(defaults.has_value());
    EXPECT_EQ(defaults->host, "localhost");
    EXPECT_EQ(defaults->port, 7860u);
    EXPECT_EQ(defaults->transport, cc::bridge::TransportType::websocket);

    auto path = unique_temp_file("_bridge_config.json");
    {
        std::ofstream out(path);
        out << R"({"transport":"http-polling","host":"127.0.0.1","port":9000,"path":"/x","auth_token":"tok","debug_mode":true,"auto_connect":false})";
    }

    auto loaded = loader.load_from_file(path.string());
    std::filesystem::remove(path);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->transport, cc::bridge::TransportType::http_polling);
    EXPECT_EQ(loaded->host, "127.0.0.1");
    EXPECT_EQ(loaded->port, 9000u);
    EXPECT_EQ(loaded->path, "/x");
    ASSERT_TRUE(loaded->auth_token.has_value());
    EXPECT_EQ(*loaded->auth_token, "tok");
    EXPECT_TRUE(loaded->debug_mode);
    EXPECT_FALSE(loaded->auto_connect);
}

TEST(BridgeConfig, RejectsMissingOrInvalidConfigFiles) {
    cc::bridge::BridgeConfigLoader loader;
    EXPECT_FALSE(loader.load_from_file("/definitely/not/present/bridge.json").has_value());

    auto path = unique_temp_file("_bridge_config_invalid.json");
    {
        std::ofstream out(path);
        out << R"({"transport":"stdio","port":70000})";
    }

    auto loaded = loader.load_from_file(path.string());
    std::filesystem::remove(path);
    EXPECT_FALSE(loaded.has_value());
}

TEST(BridgeTransport, WebSocketConnectSendsAndDisconnects) {
    LocalBridgeWebSocketServer server;
    ASSERT_TRUE(server.ready());

    cc::bridge::WebSocketTransport transport;
    std::vector<cc::bridge::TransportState> transitions;
    std::vector<std::string> received;
    std::mutex received_mutex;
    std::condition_variable received_cv;
    transport.on_state_change([&transitions](cc::bridge::TransportState, cc::bridge::TransportState next) {
        transitions.push_back(next);
    });
    transport.on_message([&](cc::bridge::BridgeMessage msg) {
        {
            std::lock_guard lock(received_mutex);
            received.push_back(msg.id);
        }
        received_cv.notify_all();
    });

    const auto url = std::format("ws://127.0.0.1:{}/bridge", server.port());
    ASSERT_TRUE(transport.connect(url, std::nullopt).has_value());
    EXPECT_TRUE(transport.is_connected());

    cc::bridge::BridgeMessage message{
        .id = "msg-1",
        .type = "request",
        .method = "ping",
        .payload = R"({"ok":true})",
        .priority = cc::bridge::MessagePriority::high,
        .timestamp = std::chrono::system_clock::now(),
        .correlation_id = "corr-1",
    };
    ASSERT_TRUE(transport.send(message).has_value());
    ASSERT_EQ(transport.sent_frames().size(), 1u);
    EXPECT_NE(transport.sent_frames().front().find("msg-1"), std::string::npos);

    auto server_frames = server.wait_for_frames(1);
    ASSERT_TRUE(server_frames.has_value());
    ASSERT_EQ(server_frames->size(), 1u);
    EXPECT_NE(server_frames->front().find("msg-1"), std::string::npos);
    EXPECT_NE(server_frames->front().find(R"("method":"ping")"), std::string::npos);

    {
        std::unique_lock lock(received_mutex);
        ASSERT_TRUE(received_cv.wait_for(lock, std::chrono::seconds(3), [&] {
            return !received.empty();
        }));
    }
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received.front(), "srv-1");

    transport.disconnect();
    EXPECT_FALSE(transport.is_connected());
    ASSERT_FALSE(transitions.empty());
    EXPECT_EQ(transitions.back(), cc::bridge::TransportState::disconnected);
}

TEST(BridgeTransport, FlushGateQueuesUntilOpened) {
    cc::bridge::TransportFlushGate gate;
    std::vector<std::string> sent;
    gate.set_sender([&sent](cc::bridge::BridgeMessage msg) { sent.push_back(msg.id); });

    gate.close();
    cc::bridge::BridgeMessage queued;
    queued.id = "queued";
    queued.type = "event";
    gate.enqueue(queued);
    EXPECT_EQ(gate.pending_count(), 1u);
    EXPECT_TRUE(sent.empty());

    gate.open();
    EXPECT_EQ(gate.pending_count(), 0u);
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent.front(), "queued");
}

TEST(BridgeTransport, CapacityWakeInvokesCallbackOnReleaseBelowCapacity) {
    cc::bridge::CapacityWake capacity;
    bool notified = false;
    capacity.set_capacity(2);
    capacity.on_available([&] { notified = true; });

    capacity.record_usage(2);
    EXPECT_FALSE(capacity.has_capacity());

    capacity.release(1);
    EXPECT_TRUE(capacity.has_capacity());
    EXPECT_TRUE(notified);
}

TEST(BridgeTransport, HttpPollingPostsMessagesAndPollsInboundEvents) {
    LocalBridgeApiHttpServer server;
    ASSERT_TRUE(server.ready());

    cc::bridge::HttpPollingTransport transport;
    std::vector<cc::bridge::BridgeMessage> received;
    std::mutex received_mutex;
    std::condition_variable received_cv;
    transport.on_message([&](cc::bridge::BridgeMessage msg) {
        {
            std::lock_guard lock(received_mutex);
            received.push_back(std::move(msg));
        }
        received_cv.notify_all();
    });

    ASSERT_TRUE(transport.connect(server.base_url() + "/bridge", "poll-token").has_value());
    ASSERT_TRUE(transport.is_connected());

    cc::bridge::BridgeMessage outbound{
        .id = "client-1",
        .type = "event",
        .method = "client/ping",
        .payload = R"({"hello":true})",
        .priority = cc::bridge::MessagePriority::normal,
        .timestamp = std::chrono::system_clock::now(),
        .correlation_id = std::nullopt,
    };
    ASSERT_TRUE(transport.send(outbound).has_value());

    auto requests = server.wait_for_requests(2);
    ASSERT_TRUE(requests.has_value());
    bool saw_post = false;
    bool saw_poll = false;
    for (const auto& request : *requests) {
        if (request.method == "POST" && request.path == "/bridge/messages") {
            saw_post = true;
            EXPECT_NE(request.headers.find("Authorization: Bearer poll-token"), std::string::npos);
            EXPECT_NE(request.body.find(R"("id":"client-1")"), std::string::npos);
            EXPECT_NE(request.body.find(R"("method":"client/ping")"), std::string::npos);
        }
        if (request.method == "GET" && request.path == "/bridge/poll") {
            saw_poll = true;
            EXPECT_NE(request.headers.find("Authorization: Bearer poll-token"), std::string::npos);
        }
    }
    EXPECT_TRUE(saw_post);
    EXPECT_TRUE(saw_poll);

    {
        std::unique_lock lock(received_mutex);
        ASSERT_TRUE(received_cv.wait_for(lock, std::chrono::seconds(3), [&] {
            return !received.empty();
        }));
    }
    ASSERT_FALSE(received.empty());
    EXPECT_EQ(received.front().id, "poll-1");
    EXPECT_EQ(received.front().method, "server/poll");
    EXPECT_EQ(received.front().priority, cc::bridge::MessagePriority::high);
    ASSERT_TRUE(received.front().correlation_id.has_value());
    EXPECT_EQ(*received.front().correlation_id, "corr-1");
    for (const auto& message : received) {
        EXPECT_NE(message.id, "client-1");
    }

    transport.disconnect();
    EXPECT_FALSE(transport.is_connected());
}

TEST(BridgeApi, ValidatesSafeIdsAndNormalizesSessionIds) {
    EXPECT_TRUE(cc::bridge::is_safe_bridge_id("env_abc-123"));
    EXPECT_FALSE(cc::bridge::is_safe_bridge_id("env/abc"));

    const std::string legacy = "A1B2C3D4-E5F6-7890-ABCD-EF1234567890";
    EXPECT_TRUE(cc::bridge::is_legacy_session_id("a1b2c3d4-e5f6-7890-abcd-ef1234567890"));
    EXPECT_EQ(cc::bridge::normalize_session_id(legacy), "ses_a1b2c3d4e5f67890abcdef1234567890");

    auto generated = cc::bridge::generate_session_id();
    EXPECT_EQ(generated.size(), 36u);
    EXPECT_EQ(generated.rfind("ses_", 0), 0u);
}

TEST(BridgeApi, RejectsUnsafeIdsBeforeNetworkCalls) {
    cc::bridge::BridgeApiClient client(cc::bridge::BridgeApiConfig{
        .base_url = "https://bridge.example.test",
        .access_token = "token",
        .runner_version = "test",
        .trusted_device_token = std::nullopt,
    });

    EXPECT_FALSE(client.poll_for_work("bad/id", "secret").has_value());
    EXPECT_FALSE(client.acknowledge_work("env", "bad/id", "session").has_value());
    EXPECT_FALSE(client.archive_session("bad/id").has_value());
}

TEST(BridgeWorkSecret, DecodesExtendedRemotePayload) {
    auto decoded = cc::bridge::decode_work_secret(bridge_test_encoded_work_secret());
    ASSERT_TRUE(decoded.has_value()) << decoded.error();
    EXPECT_EQ(decoded->version, 1);
    EXPECT_EQ(decoded->session_ingress_token, "session-token-from-secret");
    EXPECT_EQ(decoded->api_base_url, "http://session-ingress.local");
    ASSERT_TRUE(decoded->use_code_sessions.has_value());
    EXPECT_TRUE(*decoded->use_code_sessions);
    ASSERT_TRUE(decoded->code_session_mode.has_value());
    EXPECT_EQ(*decoded->code_session_mode, "code-session");

    ASSERT_EQ(decoded->sources_json.size(), 1u);
    EXPECT_NE(decoded->sources_json.front().find(R"("type":"github")"), std::string::npos);
    ASSERT_TRUE(decoded->auth_json.has_value());
    EXPECT_NE(decoded->auth_json->find(R"("name":"console")"), std::string::npos);
    ASSERT_TRUE(decoded->mcp_config_json.has_value());
    EXPECT_NE(decoded->mcp_config_json->find(R"("linear")"), std::string::npos);
    ASSERT_TRUE(decoded->environment_json.has_value());
    EXPECT_NE(decoded->environment_json->find(R"("REMOTE_FLAG":"enabled")"), std::string::npos);
    ASSERT_TRUE(decoded->environment_variables.contains("REMOTE_FLAG"));
    EXPECT_EQ(decoded->environment_variables.at("REMOTE_FLAG"), "enabled");
    ASSERT_TRUE(decoded->environment_variables.contains("REMOTE_COUNT"));
    EXPECT_EQ(decoded->environment_variables.at("REMOTE_COUNT"), "42");
    EXPECT_NE(decoded->raw_json.find(R"("session_ingress_token":"session-token-from-secret")"), std::string::npos);
}

TEST(BridgeApi, ParsesRegistrationPollAndLifecycleResponsesFromServer) {
    LocalBridgeApiHttpServer server;
    ASSERT_TRUE(server.ready());

    cc::bridge::BridgeApiClient client(cc::bridge::BridgeApiConfig{
        .base_url = server.base_url(),
        .access_token = "oauth_token",
        .runner_version = "test-runner",
        .trusted_device_token = "trusted_device",
    });

    auto registration = client.register_environment(cc::bridge::BridgeConfig{
        .transport = cc::bridge::TransportType::websocket,
        .host = "127.0.0.1",
        .port = 7777,
        .path = "/bridge",
        .auth_token = std::nullopt,
    });
    ASSERT_TRUE(registration.has_value()) << registration.error().format();
    EXPECT_EQ(registration->environment_id, "env_backend_1");
    EXPECT_EQ(registration->environment_secret, "env_secret_1");

    auto work = client.poll_for_work(registration->environment_id, registration->environment_secret, 12345);
    ASSERT_TRUE(work.has_value()) << work.error().format();
    ASSERT_TRUE(work->has_value());
    EXPECT_EQ((*work)->id, "work_1");
    EXPECT_EQ((*work)->secret, bridge_test_encoded_work_secret());
    ASSERT_TRUE((*work)->data_type.has_value());
    EXPECT_EQ(*(*work)->data_type, "session");
    ASSERT_TRUE((*work)->data_id.has_value());
    EXPECT_EQ(*(*work)->data_id, "session_1");

    auto worker_registration = client.register_worker(
        server.base_url() + "/v1/code/sessions/session_1",
        "session-token-from-secret");
    ASSERT_TRUE(worker_registration.has_value()) << worker_registration.error().format();
    EXPECT_EQ(worker_registration->worker_epoch, 42);

    auto ack = client.acknowledge_work(registration->environment_id, (*work)->id, "session-token-from-secret");
    ASSERT_TRUE(ack.has_value()) << ack.error().format();

    auto heartbeat = client.heartbeat_work(registration->environment_id, (*work)->id, "session-token-from-secret");
    ASSERT_TRUE(heartbeat.has_value()) << heartbeat.error().format();
    EXPECT_TRUE(heartbeat->lease_extended);
    EXPECT_EQ(heartbeat->state, "active");
    ASSERT_TRUE(heartbeat->last_heartbeat.has_value());
    EXPECT_EQ(*heartbeat->last_heartbeat, "2026-06-07T00:00:00Z");
    ASSERT_TRUE(heartbeat->ttl_seconds.has_value());
    EXPECT_EQ(*heartbeat->ttl_seconds, 30);

    auto permission_response = client.send_permission_response_event(
        "session_1",
        cc::bridge::PermissionResponseEvent{
            .request_id = "permission-1",
            .response_json = R"({"behavior":"allow","updatedInput":{"cmd":"ls"}})",
            .subtype = "success",
            .error = std::nullopt,
        },
        "session-token-from-secret");
    ASSERT_TRUE(permission_response.has_value()) << permission_response.error().format();

    auto stop = client.stop_work(registration->environment_id, (*work)->id, true);
    ASSERT_TRUE(stop.has_value()) << stop.error().format();

    auto deregister = client.deregister_environment(registration->environment_id);
    ASSERT_TRUE(deregister.has_value()) << deregister.error().format();

    auto requests = server.wait_for_requests(8);
    ASSERT_TRUE(requests.has_value());
    ASSERT_EQ(requests->size(), 8u);

    EXPECT_EQ((*requests)[0].method, "POST");
    EXPECT_EQ((*requests)[0].path, "/v1/environments/bridge");
    EXPECT_NE((*requests)[0].headers.find("Authorization: Bearer oauth_token"), std::string::npos);
    EXPECT_NE((*requests)[0].headers.find("X-Trusted-Device-Token: trusted_device"), std::string::npos);
    EXPECT_NE((*requests)[0].body.find(R"("transport":"websocket")"), std::string::npos);

    EXPECT_EQ((*requests)[1].method, "GET");
    EXPECT_EQ((*requests)[1].path, "/v1/environments/env_backend_1/work/poll?reclaim_older_than_ms=12345");
    EXPECT_NE((*requests)[1].headers.find("Authorization: Bearer env_secret_1"), std::string::npos);

    EXPECT_EQ((*requests)[2].method, "POST");
    EXPECT_EQ((*requests)[2].path, "/v1/code/sessions/session_1/worker/register");
    EXPECT_NE((*requests)[2].headers.find("Authorization: Bearer session-token-from-secret"), std::string::npos);
    EXPECT_EQ((*requests)[2].body, "{}");

    EXPECT_EQ((*requests)[3].method, "POST");
    EXPECT_EQ((*requests)[3].path, "/v1/environments/env_backend_1/work/work_1/ack");
    EXPECT_NE((*requests)[3].headers.find("Authorization: Bearer session-token-from-secret"), std::string::npos);
    EXPECT_EQ((*requests)[3].body, "{}");

    EXPECT_EQ((*requests)[4].method, "POST");
    EXPECT_EQ((*requests)[4].path, "/v1/environments/env_backend_1/work/work_1/heartbeat");
    EXPECT_NE((*requests)[4].headers.find("Authorization: Bearer session-token-from-secret"), std::string::npos);
    EXPECT_EQ((*requests)[4].body, "{}");

    EXPECT_EQ((*requests)[5].method, "POST");
    EXPECT_EQ((*requests)[5].path, "/v1/sessions/session_1/events");
    EXPECT_NE((*requests)[5].headers.find("Authorization: Bearer session-token-from-secret"), std::string::npos);
    EXPECT_NE((*requests)[5].body.find(R"("events":[)"), std::string::npos);
    EXPECT_NE((*requests)[5].body.find(R"("type":"control_response")"), std::string::npos);
    EXPECT_NE((*requests)[5].body.find(R"("request_id":"permission-1")"), std::string::npos);
    EXPECT_NE((*requests)[5].body.find(R"("behavior":"allow")"), std::string::npos);
    EXPECT_NE((*requests)[5].body.find(R"("updatedInput":{"cmd":"ls"})"), std::string::npos);

    EXPECT_EQ((*requests)[6].method, "POST");
    EXPECT_EQ((*requests)[6].path, "/v1/environments/env_backend_1/work/work_1/stop");
    EXPECT_NE((*requests)[6].headers.find("Authorization: Bearer oauth_token"), std::string::npos);
    EXPECT_EQ((*requests)[6].body, R"({"force":true})");

    EXPECT_EQ((*requests)[7].method, "DELETE");
    EXPECT_EQ((*requests)[7].path, "/v1/environments/bridge/env_backend_1");
    EXPECT_NE((*requests)[7].headers.find("Authorization: Bearer oauth_token"), std::string::npos);
}

TEST(BridgeDaemon, PollsWorkAcknowledgesAndSpawnsSession) {
    LocalBridgeApiHttpServer server;
    ASSERT_TRUE(server.ready());

    cc::daemon::DaemonServer daemon(cc::daemon::DaemonConfig{
        .port = 0,
        .pid_file = unique_temp_file("_daemon.pid"),
        .port_file = unique_temp_file("_daemon.port"),
        .poll_interval = std::chrono::seconds{30},
        .heartbeat_interval = std::chrono::seconds{60},
        .max_sessions = 2,
        .work_api_url = server.base_url(),
        .bridge_environment_id = "env_backend_1",
        .bridge_environment_secret = "env_secret_1",
        .bridge_access_token = "oauth_token",
        .bridge_runner_version = "test-runner",
        .trusted_device_token = std::nullopt,
    });

    std::vector<std::string> spawned_task_ids;
    daemon.set_session_spawner([&](std::string_view task_id) -> std::expected<std::string, std::string> {
        spawned_task_ids.push_back(std::string(task_id));
        return "daemon-session-1";
    });

    auto spawned = daemon.poll_for_work_once();
    ASSERT_TRUE(spawned.has_value()) << spawned.error();
    ASSERT_TRUE(spawned->has_value());
    EXPECT_EQ(**spawned, "daemon-session-1");

    ASSERT_EQ(spawned_task_ids.size(), 1u);
    EXPECT_EQ(spawned_task_ids.front(), "work_1");

    auto sessions = daemon.sessions();
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions.front().id, "daemon-session-1");
    EXPECT_EQ(sessions.front().pid, -1);
    EXPECT_EQ(sessions.front().status, "running");
    EXPECT_EQ(sessions.front().task_id, "work_1");
    ASSERT_TRUE(sessions.front().remote_session_id.has_value());
    EXPECT_EQ(*sessions.front().remote_session_id, "session_1");
    ASSERT_TRUE(sessions.front().session_ingress_token.has_value());
    EXPECT_EQ(*sessions.front().session_ingress_token, "session-token-from-secret");
    ASSERT_TRUE(sessions.front().session_api_base_url.has_value());
    EXPECT_EQ(*sessions.front().session_api_base_url, "http://session-ingress.local");
    ASSERT_TRUE(sessions.front().use_code_sessions.has_value());
    EXPECT_TRUE(*sessions.front().use_code_sessions);
    ASSERT_TRUE(sessions.front().code_session_mode.has_value());
    EXPECT_EQ(*sessions.front().code_session_mode, "code-session");
    ASSERT_TRUE(sessions.front().worker_epoch.has_value());
    EXPECT_EQ(*sessions.front().worker_epoch, 42);
    ASSERT_EQ(sessions.front().work_secret_sources_json.size(), 1u);
    EXPECT_NE(sessions.front().work_secret_sources_json.front().find(R"("id":"source-1")"), std::string::npos);
    ASSERT_TRUE(sessions.front().work_secret_auth_json.has_value());
    EXPECT_NE(sessions.front().work_secret_auth_json->find(R"("name":"console")"), std::string::npos);
    ASSERT_TRUE(sessions.front().work_secret_mcp_config_json.has_value());
    EXPECT_NE(sessions.front().work_secret_mcp_config_json->find(R"("linear")"), std::string::npos);
    ASSERT_TRUE(sessions.front().work_secret_environment_json.has_value());
    EXPECT_NE(sessions.front().work_secret_environment_json->find(R"("REMOTE_FLAG":"enabled")"), std::string::npos);
    ASSERT_TRUE(sessions.front().work_secret_environment_variables.contains("REMOTE_FLAG"));
    EXPECT_EQ(sessions.front().work_secret_environment_variables.at("REMOTE_FLAG"), "enabled");
    ASSERT_TRUE(sessions.front().work_secret_raw_json.has_value());
    EXPECT_NE(sessions.front().work_secret_raw_json->find(R"("session_ingress_token":"session-token-from-secret")"), std::string::npos);

    auto requests = server.wait_for_requests(3);
    ASSERT_TRUE(requests.has_value());
    ASSERT_EQ(requests->size(), 3u);
    EXPECT_EQ((*requests)[0].method, "GET");
    EXPECT_EQ((*requests)[0].path, "/v1/environments/env_backend_1/work/poll");
    EXPECT_NE((*requests)[0].headers.find("Authorization: Bearer env_secret_1"), std::string::npos);

    EXPECT_EQ((*requests)[1].method, "POST");
    EXPECT_EQ((*requests)[1].path, "/v1/environments/env_backend_1/work/work_1/ack");
    EXPECT_NE((*requests)[1].headers.find("Authorization: Bearer session-token-from-secret"), std::string::npos);
    EXPECT_EQ((*requests)[1].body, "{}");

    EXPECT_EQ((*requests)[2].method, "POST");
    EXPECT_EQ((*requests)[2].path, "/v1/code/sessions/session_1/worker/register");
    EXPECT_NE((*requests)[2].headers.find("Authorization: Bearer session-token-from-secret"), std::string::npos);
    EXPECT_EQ((*requests)[2].body, "{}");

    auto completed = daemon.complete_session("daemon-session-1", "completed");
    ASSERT_TRUE(completed.has_value()) << completed.error();

    auto completion_requests = server.wait_for_requests(5);
    ASSERT_TRUE(completion_requests.has_value());
    ASSERT_EQ(completion_requests->size(), 5u);
    EXPECT_EQ((*completion_requests)[3].method, "POST");
    EXPECT_EQ((*completion_requests)[3].path, "/v1/environments/env_backend_1/work/work_1/stop");
    EXPECT_NE((*completion_requests)[3].headers.find("Authorization: Bearer oauth_token"), std::string::npos);
    EXPECT_EQ((*completion_requests)[3].body, R"({"force":false})");

    EXPECT_EQ((*completion_requests)[4].method, "POST");
    EXPECT_EQ((*completion_requests)[4].path, "/v1/sessions/session_1/archive");
    EXPECT_NE((*completion_requests)[4].headers.find("Authorization: Bearer oauth_token"), std::string::npos);
}

TEST(BridgeDaemon, HeartbeatsRunningRemoteWorkSessions) {
    LocalBridgeApiHttpServer server;
    ASSERT_TRUE(server.ready());

    cc::daemon::DaemonServer daemon(cc::daemon::DaemonConfig{
        .port = 0,
        .pid_file = unique_temp_file("_daemon_heartbeat.pid"),
        .port_file = unique_temp_file("_daemon_heartbeat.port"),
        .poll_interval = std::chrono::seconds{30},
        .heartbeat_interval = std::chrono::seconds{60},
        .max_sessions = 2,
        .work_api_url = server.base_url(),
        .bridge_environment_id = "env_backend_1",
        .bridge_environment_secret = "env_secret_1",
        .bridge_access_token = "oauth_token",
        .bridge_runner_version = "test-runner",
        .trusted_device_token = std::nullopt,
    });

    daemon.set_session_spawner([](std::string_view) -> std::expected<std::string, std::string> {
        return "daemon-session-heartbeat";
    });

    auto spawned = daemon.poll_for_work_once();
    ASSERT_TRUE(spawned.has_value()) << spawned.error();
    ASSERT_TRUE(spawned->has_value());

    auto heartbeat = daemon.heartbeat_sessions_once();
    ASSERT_TRUE(heartbeat.has_value()) << heartbeat.error();
    EXPECT_EQ(*heartbeat, 1u);

    auto sessions = daemon.sessions();
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions.front().id, "daemon-session-heartbeat");
    ASSERT_TRUE(sessions.front().last_heartbeat_at.has_value());
    ASSERT_TRUE(sessions.front().last_heartbeat_state.has_value());
    EXPECT_EQ(*sessions.front().last_heartbeat_state, "active");
    ASSERT_TRUE(sessions.front().heartbeat_ttl_seconds.has_value());
    EXPECT_EQ(*sessions.front().heartbeat_ttl_seconds, 30);
    EXPECT_FALSE(sessions.front().last_heartbeat_error.has_value());
    EXPECT_EQ(sessions.front().heartbeat_failures, 0);

    auto requests = server.wait_for_requests(4);
    ASSERT_TRUE(requests.has_value());
    ASSERT_EQ(requests->size(), 4u);
    EXPECT_EQ((*requests)[2].method, "POST");
    EXPECT_EQ((*requests)[2].path, "/v1/code/sessions/session_1/worker/register");
    EXPECT_NE((*requests)[2].headers.find("Authorization: Bearer session-token-from-secret"), std::string::npos);
    EXPECT_EQ((*requests)[2].body, "{}");
    EXPECT_EQ((*requests)[3].method, "POST");
    EXPECT_EQ((*requests)[3].path, "/v1/environments/env_backend_1/work/work_1/heartbeat");
    EXPECT_NE((*requests)[3].headers.find("Authorization: Bearer session-token-from-secret"), std::string::npos);
    EXPECT_EQ((*requests)[3].body, "{}");
}

TEST(BridgeDaemon, ForkExecsHeadlessSessionAndReportsCompletion) {
    LocalBridgeApiHttpServer server;
    ASSERT_TRUE(server.ready());

    cc::daemon::DaemonServer daemon(cc::daemon::DaemonConfig{
        .port = 0,
        .pid_file = unique_temp_file("_daemon_process.pid"),
        .port_file = unique_temp_file("_daemon_process.port"),
        .poll_interval = std::chrono::seconds{30},
        .heartbeat_interval = std::chrono::seconds{60},
        .max_sessions = 2,
        .work_api_url = server.base_url(),
        .bridge_environment_id = "env_backend_1",
        .bridge_environment_secret = "env_secret_1",
        .bridge_access_token = "oauth_token",
        .bridge_runner_version = "test-runner",
        .trusted_device_token = std::nullopt,
        .session_binary = "/usr/bin/true",
    });

    auto spawned = daemon.poll_for_work_once();
    ASSERT_TRUE(spawned.has_value()) << spawned.error();
    ASSERT_TRUE(spawned->has_value());

    std::vector<cc::daemon::DaemonSession> sessions;
    for (int i = 0; i < 500; ++i) {
        daemon.reap_sessions();
        sessions = daemon.sessions();
        if (!sessions.empty() && sessions.front().status != "running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions.front().status, "completed");
    EXPECT_TRUE(sessions.front().completion_reported);
    ASSERT_TRUE(sessions.front().remote_session_id.has_value());
    EXPECT_EQ(*sessions.front().remote_session_id, "session_1");

    auto requests = server.wait_for_requests(5);
    ASSERT_TRUE(requests.has_value());
    ASSERT_EQ(requests->size(), 5u);
    EXPECT_EQ((*requests)[0].method, "GET");
    EXPECT_EQ((*requests)[0].path, "/v1/environments/env_backend_1/work/poll");
    EXPECT_EQ((*requests)[1].method, "POST");
    EXPECT_EQ((*requests)[1].path, "/v1/environments/env_backend_1/work/work_1/ack");
    EXPECT_EQ((*requests)[2].method, "POST");
    EXPECT_EQ((*requests)[2].path, "/v1/code/sessions/session_1/worker/register");
    EXPECT_EQ((*requests)[3].method, "POST");
    EXPECT_EQ((*requests)[3].path, "/v1/environments/env_backend_1/work/work_1/stop");
    EXPECT_EQ((*requests)[3].body, R"({"force":false})");
    EXPECT_EQ((*requests)[4].method, "POST");
    EXPECT_EQ((*requests)[4].path, "/v1/sessions/session_1/archive");
}

TEST(BridgeDaemon, ForkExecsNativeHeadlessSessionThroughRemoteLifecycle) {
    const auto native_binary = native_cc_repl_binary_path();
    if (native_binary.empty() || !std::filesystem::exists(native_binary)) {
        GTEST_SKIP() << "native cc-repl binary is not available";
    }

    LocalBridgeApiHttpServer server;
    ASSERT_TRUE(server.ready());
    server.set_work_secret(bridge_test_encoded_work_secret(server.base_url()));

    const auto env_root = unique_temp_file("_native_headless_home");
    std::filesystem::remove_all(env_root);
    std::filesystem::create_directories(env_root);
    ScopedEnvVar api_key("ANTHROPIC_API_KEY", "fake-key");
    ScopedEnvVar anthropic_base("ANTHROPIC_BASE_URL", server.base_url());
    ScopedEnvVar xdg_config("XDG_CONFIG_HOME", env_root.string());
    ScopedEnvVar home("HOME", env_root.string());

    cc::daemon::DaemonServer daemon(cc::daemon::DaemonConfig{
        .port = 0,
        .pid_file = unique_temp_file("_daemon_native_process.pid"),
        .port_file = unique_temp_file("_daemon_native_process.port"),
        .poll_interval = std::chrono::seconds{30},
        .heartbeat_interval = std::chrono::seconds{60},
        .max_sessions = 2,
        .work_api_url = server.base_url(),
        .bridge_environment_id = "env_backend_1",
        .bridge_environment_secret = "env_secret_1",
        .bridge_access_token = "oauth_token",
        .bridge_runner_version = "test-runner",
        .trusted_device_token = std::nullopt,
        .session_binary = native_binary.string(),
    });

    auto spawned = daemon.poll_for_work_once();
    ASSERT_TRUE(spawned.has_value()) << spawned.error();
    ASSERT_TRUE(spawned->has_value());
    const auto session_id = **spawned;

    bool saw_assistant = false;
    bool saw_result = false;
    std::vector<std::string> lines;
    for (int i = 0; i < 300; ++i) {
        lines = daemon.session_stdout_lines(session_id);
        for (const auto& line : lines) {
            if (line.find("native daemon bridge product reply") != std::string::npos &&
                line.find(R"("type":"assistant")") != std::string::npos) {
                saw_assistant = true;
            }
            if (line.find("native daemon bridge product reply") != std::string::npos &&
                line.find(R"("type":"result")") != std::string::npos) {
                saw_result = true;
            }
        }
        if (saw_assistant && saw_result) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }

    auto sessions = daemon.sessions();
    ASSERT_EQ(sessions.size(), 1u);
    ASSERT_GT(sessions.front().pid, 0);
    if (!saw_assistant || !saw_result) {
        ::kill(sessions.front().pid, SIGTERM);
        daemon.reap_sessions();
    }
    ASSERT_TRUE(saw_assistant) << "stdout:\n" << [&] {
        std::ostringstream out;
        for (const auto& line : lines) out << line << '\n';
        return out.str();
    }();
    ASSERT_TRUE(saw_result) << "stdout:\n" << [&] {
        std::ostringstream out;
        for (const auto& line : lines) out << line << '\n';
        return out.str();
    }();

    auto requests_before_shutdown = server.wait_for_requests(1);
    ASSERT_TRUE(requests_before_shutdown.has_value());
    const auto request_count_before_shutdown = requests_before_shutdown->size();

    ::kill(sessions.front().pid, SIGTERM);
    for (int i = 0; i < 500; ++i) {
        daemon.reap_sessions();
        sessions = daemon.sessions();
        if (!sessions.empty() && sessions.front().status != "running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }

    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions.front().status, "completed");
    EXPECT_TRUE(sessions.front().completion_reported);
    ASSERT_TRUE(sessions.front().remote_session_id.has_value());
    EXPECT_EQ(*sessions.front().remote_session_id, "session_1");

    auto requests = server.wait_for_requests(request_count_before_shutdown + 2, std::chrono::seconds{5});
    ASSERT_TRUE(requests.has_value());

    auto saw_request = [&](std::string_view method, std::string_view path) {
        return std::ranges::any_of(*requests, [&](const LocalBridgeApiRequest& request) {
            return request.method == method && request.path == path;
        });
    };
    auto saw_body = [&](std::string_view path, std::string_view text) {
        return std::ranges::any_of(*requests, [&](const LocalBridgeApiRequest& request) {
            return request.path == path && request.body.find(text) != std::string::npos;
        });
    };

    EXPECT_TRUE(saw_request("GET", "/v1/environments/env_backend_1/work/poll"));
    EXPECT_TRUE(saw_request("POST", "/v1/environments/env_backend_1/work/work_1/ack"));
    EXPECT_TRUE(saw_request("POST", "/v1/code/sessions/session_1/worker/register"));
    EXPECT_TRUE(saw_request("GET", "/v1/code/sessions/session_1/worker/events/stream"));
    EXPECT_TRUE(saw_request("POST", "/v1/code/sessions/session_1/worker/heartbeat"));
    EXPECT_TRUE(saw_request("POST", "/v1/code/sessions/session_1/worker/events/delivery"));
    EXPECT_TRUE(saw_request("PUT", "/v1/code/sessions/session_1/worker"));
    EXPECT_TRUE(saw_request("POST", "/v1/messages"));
    EXPECT_TRUE(saw_request("POST", "/v1/environments/env_backend_1/work/work_1/stop"));
    EXPECT_TRUE(saw_request("POST", "/v1/sessions/session_1/archive"));
    EXPECT_TRUE(saw_body("/v1/messages", "run native daemon bridge product e2e"));
    EXPECT_TRUE(saw_body("/v1/code/sessions/session_1/worker/events", R"("status":"started")"));
    EXPECT_TRUE(saw_body("/v1/code/sessions/session_1/worker/events", R"("type":"assistant")"));
    EXPECT_TRUE(saw_body("/v1/code/sessions/session_1/worker/events", R"("type":"result")"));
    EXPECT_TRUE(saw_body("/v1/code/sessions/session_1/worker/events", R"("status":"stopped")"));

    std::filesystem::remove_all(env_root);
}

TEST(BridgeDaemon, ForkExecsHeadlessSessionWithCcrSdkUrl) {
    auto script = unique_temp_file("_headless_child_args.sh");
    {
        std::ofstream out(script);
        out << "#!/bin/sh\n"
            << "printf '%s\\n' \"$@\"\n";
    }
    std::filesystem::permissions(
        script,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);

    auto secret = cc::bridge::decode_work_secret(bridge_test_encoded_work_secret());
    ASSERT_TRUE(secret.has_value()) << secret.error();
    cc::bridge::WorkResponse work{
        .id = "work_1",
        .data_type = std::string("session"),
        .data_id = std::string("session_1"),
        .secret = bridge_test_encoded_work_secret(),
    };

    cc::daemon::DaemonServer daemon(cc::daemon::DaemonConfig{
        .port = 0,
        .pid_file = unique_temp_file("_daemon_sdk_url.pid"),
        .port_file = unique_temp_file("_daemon_sdk_url.port"),
        .poll_interval = std::chrono::seconds{30},
        .heartbeat_interval = std::chrono::seconds{60},
        .max_sessions = 2,
        .work_api_url = "",
        .bridge_environment_id = "",
        .bridge_environment_secret = "",
        .bridge_access_token = "",
        .bridge_runner_version = "test-runner",
        .trusted_device_token = std::nullopt,
        .session_binary = script.string(),
    });

    auto spawned = daemon.spawn_session_with_bridge_context("work_1", &work, &*secret);
    ASSERT_TRUE(spawned.has_value()) << spawned.error();

    std::vector<std::string> lines;
    bool saw_sdk_url_flag = false;
    bool saw_sdk_url = false;
    for (int i = 0; i < 500; ++i) {
        daemon.reap_sessions();
        lines = daemon.session_stdout_lines(*spawned);
        for (const auto& line : lines) {
            if (line == "--sdk-url") saw_sdk_url_flag = true;
            if (line == "http://session-ingress.local/v1/code/sessions/session_1") saw_sdk_url = true;
        }
        if (saw_sdk_url_flag && saw_sdk_url) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    ASSERT_FALSE(lines.empty());
    EXPECT_TRUE(saw_sdk_url_flag);
    EXPECT_TRUE(saw_sdk_url);

    auto sessions = daemon.sessions();
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions.front().status, "completed");

    std::filesystem::remove(script);
}

TEST(BridgeDaemon, ForkExecsHeadlessSessionWithV1SessionIngressSdkUrl) {
    auto script = unique_temp_file("_headless_child_v1_args.sh");
    {
        std::ofstream out(script);
        out << "#!/bin/sh\n"
            << "printf '%s\\n' \"$@\"\n"
            << "printf 'CLAUDE_CODE_POST_FOR_SESSION_INGRESS_V2=%s\\n' \"$CLAUDE_CODE_POST_FOR_SESSION_INGRESS_V2\"\n";
    }
    std::filesystem::permissions(
        script,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);

    auto secret = cc::bridge::decode_work_secret(bridge_test_encoded_v1_work_secret());
    ASSERT_TRUE(secret.has_value()) << secret.error();
    cc::bridge::WorkResponse work{
        .id = "work_1",
        .data_type = std::string("session"),
        .data_id = std::string("session_1"),
        .secret = bridge_test_encoded_v1_work_secret(),
    };

    cc::daemon::DaemonServer daemon(cc::daemon::DaemonConfig{
        .port = 0,
        .pid_file = unique_temp_file("_daemon_v1_sdk_url.pid"),
        .port_file = unique_temp_file("_daemon_v1_sdk_url.port"),
        .poll_interval = std::chrono::seconds{30},
        .heartbeat_interval = std::chrono::seconds{60},
        .max_sessions = 2,
        .work_api_url = "",
        .bridge_environment_id = "",
        .bridge_environment_secret = "",
        .bridge_access_token = "",
        .bridge_runner_version = "test-runner",
        .trusted_device_token = std::nullopt,
        .session_binary = script.string(),
    });

    auto spawned = daemon.spawn_session_with_bridge_context("work_1", &work, &*secret);
    ASSERT_TRUE(spawned.has_value()) << spawned.error();

    std::vector<std::string> lines;
    bool saw_sdk_url_flag = false;
    bool saw_sdk_url = false;
    bool saw_v1_post_env = false;
    for (int i = 0; i < 500; ++i) {
        daemon.reap_sessions();
        lines = daemon.session_stdout_lines(*spawned);
        for (const auto& line : lines) {
            if (line == "--sdk-url") saw_sdk_url_flag = true;
            if (line == "ws://127.0.0.1:19191/v2/session_ingress/ws/session_1") saw_sdk_url = true;
            if (line == "CLAUDE_CODE_POST_FOR_SESSION_INGRESS_V2=1") saw_v1_post_env = true;
        }
        if (saw_sdk_url_flag && saw_sdk_url && saw_v1_post_env) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    ASSERT_FALSE(lines.empty());
    EXPECT_TRUE(saw_sdk_url_flag);
    EXPECT_TRUE(saw_sdk_url);
    EXPECT_TRUE(saw_v1_post_env);

    auto sessions = daemon.sessions();
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions.front().status, "completed");

    std::filesystem::remove(script);
}

TEST(BridgeDaemon, ForkExecsHeadlessSessionWithCcrWorkerEpochEnvironment) {
    LocalBridgeApiHttpServer server;
    ASSERT_TRUE(server.ready());

    auto script = unique_temp_file("_headless_child_ccr_env.sh");
    {
        std::ofstream out(script);
        out << "#!/bin/sh\n"
            << "for arg in \"$@\"; do printf 'ARG:%s\\n' \"$arg\"; done\n"
            << "printf 'ENV:CLAUDE_CODE_WORKER_EPOCH=%s\\n' \"$CLAUDE_CODE_WORKER_EPOCH\"\n"
            << "printf 'ENV:CLAUDE_CODE_USE_CCR_V2=%s\\n' \"$CLAUDE_CODE_USE_CCR_V2\"\n"
            << "printf 'ENV:CLAUDE_CODE_REMOTE_API_BASE_URL=%s\\n' \"$CLAUDE_CODE_REMOTE_API_BASE_URL\"\n";
    }
    std::filesystem::permissions(
        script,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);

    auto secret = cc::bridge::decode_work_secret(bridge_test_encoded_work_secret());
    ASSERT_TRUE(secret.has_value()) << secret.error();
    cc::bridge::WorkResponse work{
        .id = "work_1",
        .data_type = std::string("session"),
        .data_id = std::string("session_1"),
        .secret = bridge_test_encoded_work_secret(),
    };

    cc::daemon::DaemonServer daemon(cc::daemon::DaemonConfig{
        .port = 0,
        .pid_file = unique_temp_file("_daemon_ccr_env.pid"),
        .port_file = unique_temp_file("_daemon_ccr_env.port"),
        .poll_interval = std::chrono::seconds{30},
        .heartbeat_interval = std::chrono::seconds{60},
        .max_sessions = 2,
        .work_api_url = server.base_url(),
        .bridge_environment_id = "",
        .bridge_environment_secret = "",
        .bridge_access_token = "",
        .bridge_runner_version = "test-runner",
        .trusted_device_token = std::nullopt,
        .session_binary = script.string(),
    });

    auto spawned = daemon.spawn_session_with_bridge_context("work_1", &work, &*secret);
    ASSERT_TRUE(spawned.has_value()) << spawned.error();

    std::vector<std::string> lines;
    const auto expected_sdk_url = "ARG:" + server.base_url() + "/v1/code/sessions/session_1";
    bool saw_sdk_url = false;
    bool saw_worker_epoch = false;
    bool saw_ccr_v2 = false;
    bool saw_api_base = false;
    for (int i = 0; i < 500; ++i) {
        daemon.reap_sessions();
        lines = daemon.session_stdout_lines(*spawned);
        for (const auto& line : lines) {
            if (line == expected_sdk_url) saw_sdk_url = true;
            if (line == "ENV:CLAUDE_CODE_WORKER_EPOCH=42") saw_worker_epoch = true;
            if (line == "ENV:CLAUDE_CODE_USE_CCR_V2=1") saw_ccr_v2 = true;
            if (line == "ENV:CLAUDE_CODE_REMOTE_API_BASE_URL=" + server.base_url()) saw_api_base = true;
        }
        if (saw_sdk_url && saw_worker_epoch && saw_ccr_v2 && saw_api_base) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    ASSERT_FALSE(lines.empty());
    EXPECT_TRUE(saw_sdk_url);
    EXPECT_TRUE(saw_worker_epoch);
    EXPECT_TRUE(saw_ccr_v2);
    EXPECT_TRUE(saw_api_base);

    auto sessions = daemon.sessions();
    ASSERT_EQ(sessions.size(), 1u);
    ASSERT_TRUE(sessions.front().worker_epoch.has_value());
    EXPECT_EQ(*sessions.front().worker_epoch, 42);

    auto requests = server.wait_for_requests(1);
    ASSERT_TRUE(requests.has_value());
    ASSERT_EQ(requests->size(), 1u);
    EXPECT_EQ((*requests)[0].method, "POST");
    EXPECT_EQ((*requests)[0].path, "/v1/code/sessions/session_1/worker/register");
    EXPECT_NE((*requests)[0].headers.find("Authorization: Bearer session-token-from-secret"), std::string::npos);
    EXPECT_EQ((*requests)[0].body, "{}");

    std::filesystem::remove(script);
}

TEST(BridgeDaemon, PipesHeadlessChildStdinAndCapturesStdout) {
    auto script = unique_temp_file("_headless_child.sh");
    {
        std::ofstream out(script);
        out << "#!/bin/sh\n"
            << "while IFS= read -r line; do\n"
            << "  printf '%s\\n' \"$line\"\n"
            << "  printf '%s\\n' '{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"child reply\"}]}}'\n"
            << "done\n";
    }
    std::filesystem::permissions(
        script,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);

    cc::daemon::DaemonServer daemon(cc::daemon::DaemonConfig{
        .port = 0,
        .pid_file = unique_temp_file("_daemon_stdio.pid"),
        .port_file = unique_temp_file("_daemon_stdio.port"),
        .poll_interval = std::chrono::seconds{30},
        .heartbeat_interval = std::chrono::seconds{60},
        .max_sessions = 2,
        .work_api_url = "",
        .bridge_environment_id = "",
        .bridge_environment_secret = "",
        .bridge_access_token = "",
        .bridge_runner_version = "test-runner",
        .trusted_device_token = std::nullopt,
        .session_binary = script.string(),
    });

    auto spawned = daemon.spawn_session("stdio-work");
    ASSERT_TRUE(spawned.has_value()) << spawned.error();
    ASSERT_TRUE(daemon.send_session_stdin(*spawned,
        R"({"type":"user","message":{"role":"user","content":"hello child"},"session_id":"session_1"})" "\n").has_value());
    ASSERT_TRUE(daemon.close_session_stdin(*spawned).has_value());

    std::vector<std::string> lines;
    std::vector<cc::daemon::DaemonSession> sessions;
    for (int i = 0; i < 500; ++i) {
        lines = daemon.session_stdout_lines(*spawned);
        sessions = daemon.sessions();
        if (lines.size() >= 2 && !sessions.empty() && sessions.front().stdout_closed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    ASSERT_GE(lines.size(), 2u);
    EXPECT_NE(lines[0].find(R"("content":"hello child")"), std::string::npos);
    EXPECT_EQ(lines[1], R"({"type":"assistant","message":{"content":[{"type":"text","text":"child reply"}]}})");

    daemon.reap_sessions();
    sessions = daemon.sessions();
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions.front().status, "completed");
    EXPECT_EQ(sessions.front().stdin_fd, -1);
    EXPECT_TRUE(sessions.front().stdout_closed);

    std::filesystem::remove(script);
}

TEST(BridgeDaemon, RpcStdinRoutesRemoteInputToHeadlessChild) {
    auto script = unique_temp_file("_headless_child_rpc.sh");
    {
        std::ofstream out(script);
        out << "#!/bin/sh\n"
            << "while IFS= read -r line; do\n"
            << "  printf '%s\\n' \"$line\"\n"
            << "  printf '%s\\n' '{\"type\":\"result\",\"result\":\"rpc child reply\"}'\n"
            << "done\n";
    }
    std::filesystem::permissions(
        script,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);

    cc::daemon::DaemonServer daemon(cc::daemon::DaemonConfig{
        .port = 0,
        .pid_file = unique_temp_file("_daemon_rpc_stdio.pid"),
        .port_file = unique_temp_file("_daemon_rpc_stdio.port"),
        .poll_interval = std::chrono::seconds{30},
        .heartbeat_interval = std::chrono::seconds{60},
        .max_sessions = 2,
        .work_api_url = "",
        .bridge_environment_id = "",
        .bridge_environment_secret = "",
        .bridge_access_token = "",
        .bridge_runner_version = "test-runner",
        .trusted_device_token = std::nullopt,
        .session_binary = script.string(),
    });

    auto started = daemon.start();
    ASSERT_TRUE(started.has_value()) << started.error();

    cc::daemon::DaemonClient client;
    ASSERT_TRUE(client.connect(*started).has_value());
    auto spawned = client.spawn("rpc-stdio-work");
    ASSERT_TRUE(spawned.has_value()) << spawned.error();
    auto session_id = extract_json_string_field(*spawned, "session_id");
    ASSERT_FALSE(session_id.empty()) << *spawned;

    auto sent = client.send_stdin(
        session_id,
        R"({"type":"user","message":{"role":"user","content":"rpc hello child"},"session_id":"session_1"})" "\n");
    ASSERT_TRUE(sent.has_value()) << sent.error();
    auto closed = client.close_stdin(session_id);
    ASSERT_TRUE(closed.has_value()) << closed.error();

    std::string stdout_response;
    for (int i = 0; i < 500; ++i) {
        daemon.reap_sessions();
        auto stdout_result = client.stdout_lines(session_id);
        ASSERT_TRUE(stdout_result.has_value()) << stdout_result.error();
        stdout_response = *stdout_result;
        if (stdout_response.find("rpc child reply") != std::string::npos) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    EXPECT_NE(stdout_response.find("rpc hello child"), std::string::npos);
    EXPECT_NE(stdout_response.find("rpc child reply"), std::string::npos);

    auto sessions = daemon.sessions();
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions.front().status, "completed");
    EXPECT_EQ(sessions.front().stdin_fd, -1);
    EXPECT_TRUE(sessions.front().stdout_closed);

    client.disconnect();
    daemon.stop();
    std::filesystem::remove(script);
}

TEST(BridgeDaemon, RpcEventRoutesRemotePayloadToHeadlessChildStdin) {
    auto script = unique_temp_file("_headless_child_event_rpc.sh");
    {
        std::ofstream out(script);
        out << "#!/bin/sh\n"
            << "while IFS= read -r line; do\n"
            << "  printf '%s\\n' \"$line\"\n"
            << "done\n";
    }
    std::filesystem::permissions(
        script,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);

    cc::daemon::DaemonServer daemon(cc::daemon::DaemonConfig{
        .port = 0,
        .pid_file = unique_temp_file("_daemon_rpc_event.pid"),
        .port_file = unique_temp_file("_daemon_rpc_event.port"),
        .poll_interval = std::chrono::seconds{30},
        .heartbeat_interval = std::chrono::seconds{60},
        .max_sessions = 2,
        .work_api_url = "",
        .bridge_environment_id = "",
        .bridge_environment_secret = "",
        .bridge_access_token = "",
        .bridge_runner_version = "test-runner",
        .trusted_device_token = std::nullopt,
        .session_binary = script.string(),
    });

    auto started = daemon.start();
    ASSERT_TRUE(started.has_value()) << started.error();

    cc::daemon::DaemonClient client;
    ASSERT_TRUE(client.connect(*started).has_value());
    auto spawned = client.spawn("rpc-event-work");
    ASSERT_TRUE(spawned.has_value()) << spawned.error();
    auto session_id = extract_json_string_field(*spawned, "session_id");
    ASSERT_FALSE(session_id.empty()) << *spawned;

    auto sent = client.send_event(
        session_id,
        R"({"type":"user","message":{"role":"user","content":"remote event hello"},"session_id":"session_1"})");
    ASSERT_TRUE(sent.has_value()) << sent.error();
    auto closed = client.close_stdin(session_id);
    ASSERT_TRUE(closed.has_value()) << closed.error();

    std::string stdout_response;
    for (int i = 0; i < 500; ++i) {
        daemon.reap_sessions();
        auto stdout_result = client.stdout_lines(session_id);
        ASSERT_TRUE(stdout_result.has_value()) << stdout_result.error();
        stdout_response = *stdout_result;
        if (stdout_response.find("remote event hello") != std::string::npos) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    EXPECT_NE(stdout_response.find("remote event hello"), std::string::npos);

    auto raw_lines = daemon.session_stdout_lines(session_id);
    ASSERT_FALSE(raw_lines.empty());
    EXPECT_NE(raw_lines.front().find(R"("type":"user")"), std::string::npos);
    EXPECT_NE(raw_lines.front().find("remote event hello"), std::string::npos);

    auto sessions = daemon.sessions();
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions.front().delivered_remote_events, 1u);
    ASSERT_TRUE(sessions.front().last_delivered_remote_event_type.has_value());
    EXPECT_EQ(*sessions.front().last_delivered_remote_event_type, "user");
    EXPECT_FALSE(sessions.front().last_remote_event_error.has_value());
    EXPECT_EQ(sessions.front().status, "completed");

    client.disconnect();
    daemon.stop();
    std::filesystem::remove(script);
}

TEST(BridgeDaemon, PollAuthFailureUpdatesBackoffAndResetsAfterSuccess) {
    LocalBridgeApiHttpServer server;
    ASSERT_TRUE(server.ready());
    server.fail_next_work_polls(1, 401, "Unauthorized", R"({"error":"unauthorized"})");

    cc::daemon::DaemonServer daemon(cc::daemon::DaemonConfig{
        .port = 0,
        .pid_file = unique_temp_file("_daemon_auth.pid"),
        .port_file = unique_temp_file("_daemon_auth.port"),
        .poll_interval = std::chrono::seconds{1},
        .heartbeat_interval = std::chrono::seconds{60},
        .max_sessions = 2,
        .work_api_url = server.base_url(),
        .bridge_environment_id = "env_backend_1",
        .bridge_environment_secret = "env_secret_1",
        .bridge_access_token = "oauth_token",
        .bridge_runner_version = "test-runner",
        .trusted_device_token = std::nullopt,
    });

    auto failed = daemon.poll_for_work_once();
    ASSERT_FALSE(failed.has_value());
    EXPECT_NE(failed.error().find("[E200]"), std::string::npos);
    EXPECT_NE(failed.error().find("HTTP 401"), std::string::npos);

    auto failed_state = daemon.backoff_state();
    EXPECT_EQ(failed_state.consecutive_poll_failures, 1);
    EXPECT_EQ(failed_state.current_poll_backoff, std::chrono::seconds{1});
    EXPECT_TRUE(failed_state.auth_failed);
    ASSERT_TRUE(failed_state.last_poll_error.has_value());
    EXPECT_NE(failed_state.last_poll_error->find("HTTP 401"), std::string::npos);

    daemon.set_session_spawner([](std::string_view) -> std::expected<std::string, std::string> {
        return "daemon-session-after-auth";
    });

    auto recovered = daemon.poll_for_work_once();
    ASSERT_TRUE(recovered.has_value()) << recovered.error();
    ASSERT_TRUE(recovered->has_value());
    EXPECT_EQ(**recovered, "daemon-session-after-auth");

    auto recovered_state = daemon.backoff_state();
    EXPECT_EQ(recovered_state.consecutive_poll_failures, 0);
    EXPECT_EQ(recovered_state.current_poll_backoff, std::chrono::seconds{0});
    EXPECT_FALSE(recovered_state.last_poll_error.has_value());
    EXPECT_FALSE(recovered_state.auth_failed);

    auto requests = server.wait_for_requests(4);
    ASSERT_TRUE(requests.has_value());
    ASSERT_EQ(requests->size(), 4u);
    EXPECT_EQ((*requests)[0].method, "GET");
    EXPECT_EQ((*requests)[0].path, "/v1/environments/env_backend_1/work/poll");
    EXPECT_NE((*requests)[0].headers.find("Authorization: Bearer env_secret_1"), std::string::npos);
    EXPECT_EQ((*requests)[1].method, "GET");
    EXPECT_EQ((*requests)[1].path, "/v1/environments/env_backend_1/work/poll");
    EXPECT_EQ((*requests)[2].method, "POST");
    EXPECT_EQ((*requests)[2].path, "/v1/environments/env_backend_1/work/work_1/ack");
    EXPECT_EQ((*requests)[3].method, "POST");
    EXPECT_EQ((*requests)[3].path, "/v1/code/sessions/session_1/worker/register");
}

TEST(BridgeDaemon, ConsecutivePollFailuresUseExponentialBackoffCappedAtFiveMinutes) {
    LocalBridgeApiHttpServer server;
    ASSERT_TRUE(server.ready());

    cc::daemon::DaemonServer daemon(cc::daemon::DaemonConfig{
        .port = 0,
        .pid_file = unique_temp_file("_daemon_backoff.pid"),
        .port_file = unique_temp_file("_daemon_backoff.port"),
        .poll_interval = std::chrono::seconds{5},
        .heartbeat_interval = std::chrono::seconds{60},
        .max_sessions = 2,
        .work_api_url = server.base_url(),
        .bridge_environment_id = "env_backend_1",
        .bridge_environment_secret = "env_secret_1",
        .bridge_access_token = "oauth_token",
        .bridge_runner_version = "test-runner",
        .trusted_device_token = std::nullopt,
    });

    const std::vector<std::chrono::seconds> expected_backoffs{
        std::chrono::seconds{5},
        std::chrono::seconds{10},
        std::chrono::seconds{20},
        std::chrono::seconds{40},
        std::chrono::seconds{80},
        std::chrono::seconds{160},
        std::chrono::seconds{300},
        std::chrono::seconds{300},
    };
    constexpr std::size_t http_attempts_per_failed_call = 4;
    server.fail_next_work_polls(
        static_cast<int>(expected_backoffs.size() * http_attempts_per_failed_call),
        500,
        "Internal Server Error",
        R"({"error":"server error"})");

    for (std::size_t i = 0; i < expected_backoffs.size(); ++i) {
        auto failed = daemon.poll_for_work_once();
        ASSERT_FALSE(failed.has_value()) << "failure index " << i;
        auto state = daemon.backoff_state();
        EXPECT_EQ(state.consecutive_poll_failures, static_cast<int>(i + 1));
        EXPECT_EQ(state.current_poll_backoff, expected_backoffs[i]);
        EXPECT_FALSE(state.auth_failed);
        ASSERT_TRUE(state.last_poll_error.has_value());
        EXPECT_NE(state.last_poll_error->find("HTTP 500"), std::string::npos);
    }

    auto requests = server.wait_for_requests(expected_backoffs.size() * http_attempts_per_failed_call);
    ASSERT_TRUE(requests.has_value());
    ASSERT_EQ(requests->size(), expected_backoffs.size() * http_attempts_per_failed_call);
    for (const auto& request : *requests) {
        EXPECT_EQ(request.method, "GET");
        EXPECT_EQ(request.path, "/v1/environments/env_backend_1/work/poll");
        EXPECT_NE(request.headers.find("Authorization: Bearer env_secret_1"), std::string::npos);
    }
}

TEST(BridgeDaemon, HeartbeatFailureRecordsErrorAndResetsAfterSuccess) {
    LocalBridgeApiHttpServer server;
    ASSERT_TRUE(server.ready());

    cc::daemon::DaemonServer daemon(cc::daemon::DaemonConfig{
        .port = 0,
        .pid_file = unique_temp_file("_daemon_heartbeat_failure.pid"),
        .port_file = unique_temp_file("_daemon_heartbeat_failure.port"),
        .poll_interval = std::chrono::seconds{30},
        .heartbeat_interval = std::chrono::seconds{60},
        .max_sessions = 2,
        .work_api_url = server.base_url(),
        .bridge_environment_id = "env_backend_1",
        .bridge_environment_secret = "env_secret_1",
        .bridge_access_token = "oauth_token",
        .bridge_runner_version = "test-runner",
        .trusted_device_token = std::nullopt,
    });
    daemon.set_session_spawner([](std::string_view) -> std::expected<std::string, std::string> {
        return "daemon-session-heartbeat-failure";
    });

    auto spawned = daemon.poll_for_work_once();
    ASSERT_TRUE(spawned.has_value()) << spawned.error();
    ASSERT_TRUE(spawned->has_value());

    server.fail_next_heartbeats(4, 503, "Service Unavailable", R"({"error":"temporary outage"})");
    auto failed_heartbeat = daemon.heartbeat_sessions_once();
    ASSERT_FALSE(failed_heartbeat.has_value());
    EXPECT_NE(failed_heartbeat.error().find("HTTP 503"), std::string::npos);

    auto failed_sessions = daemon.sessions();
    ASSERT_EQ(failed_sessions.size(), 1u);
    EXPECT_EQ(failed_sessions.front().heartbeat_failures, 1);
    ASSERT_TRUE(failed_sessions.front().last_heartbeat_error.has_value());
    EXPECT_NE(failed_sessions.front().last_heartbeat_error->find("HTTP 503"), std::string::npos);
    EXPECT_FALSE(failed_sessions.front().last_heartbeat_at.has_value());

    auto recovered_heartbeat = daemon.heartbeat_sessions_once();
    ASSERT_TRUE(recovered_heartbeat.has_value()) << recovered_heartbeat.error();
    EXPECT_EQ(*recovered_heartbeat, 1u);

    auto recovered_sessions = daemon.sessions();
    ASSERT_EQ(recovered_sessions.size(), 1u);
    EXPECT_EQ(recovered_sessions.front().heartbeat_failures, 0);
    EXPECT_FALSE(recovered_sessions.front().last_heartbeat_error.has_value());
    ASSERT_TRUE(recovered_sessions.front().last_heartbeat_at.has_value());
    ASSERT_TRUE(recovered_sessions.front().last_heartbeat_state.has_value());
    EXPECT_EQ(*recovered_sessions.front().last_heartbeat_state, "active");
    ASSERT_TRUE(recovered_sessions.front().heartbeat_ttl_seconds.has_value());
    EXPECT_EQ(*recovered_sessions.front().heartbeat_ttl_seconds, 30);

    auto requests = server.wait_for_requests(8);
    ASSERT_TRUE(requests.has_value());
    ASSERT_EQ(requests->size(), 8u);
    EXPECT_EQ((*requests)[0].path, "/v1/environments/env_backend_1/work/poll");
    EXPECT_EQ((*requests)[1].path, "/v1/environments/env_backend_1/work/work_1/ack");
    EXPECT_EQ((*requests)[2].path, "/v1/code/sessions/session_1/worker/register");
    EXPECT_EQ((*requests)[3].path, "/v1/environments/env_backend_1/work/work_1/heartbeat");
    EXPECT_EQ((*requests)[4].path, "/v1/environments/env_backend_1/work/work_1/heartbeat");
    EXPECT_EQ((*requests)[5].path, "/v1/environments/env_backend_1/work/work_1/heartbeat");
    EXPECT_EQ((*requests)[6].path, "/v1/environments/env_backend_1/work/work_1/heartbeat");
    EXPECT_EQ((*requests)[7].path, "/v1/environments/env_backend_1/work/work_1/heartbeat");
}
