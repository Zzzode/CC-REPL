module;

#include <atomic>
#include <array>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <mutex>
#include <netdb.h>
#include <optional>
#include <openssl/sha.h>
#include <print>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

export module cc.bridge.transport;

import cc.utils.json;
import cc.utils.http;


export namespace cc::bridge {


enum class MessagePriority { low, normal, high, system };

namespace detail {

[[nodiscard]] auto json_escape(std::string_view value) -> std::string {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}
[[nodiscard]] auto priority_to_string(MessagePriority priority) -> std::string_view {
    switch (priority) {
        case MessagePriority::low: return "low";
        case MessagePriority::normal: return "normal";
        case MessagePriority::high: return "high";
        case MessagePriority::system: return "system";
    }
    return "normal";
}

[[nodiscard]] auto priority_from_string(std::string_view priority) -> MessagePriority {
    if (priority == "low") return MessagePriority::low;
    if (priority == "high") return MessagePriority::high;
    if (priority == "system") return MessagePriority::system;
    return MessagePriority::normal;
}

[[nodiscard]] auto base64_encode(const unsigned char* data, std::size_t len) -> std::string {
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

[[nodiscard]] auto websocket_accept_key(std::string_view key) -> std::string {
    const std::string input = std::string(key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::array<unsigned char, 20> hash{};
    SHA1(reinterpret_cast<const unsigned char*>(input.data()), input.size(), hash.data());
    return base64_encode(hash.data(), hash.size());
}

} // namespace detail


struct BridgeMessage {
    std::string id;
    std::string type;         // "request", "response", "event", "heartbeat"
    std::string method;
    std::string payload;      // JSON payload
    MessagePriority priority{MessagePriority::normal};
    std::chrono::system_clock::time_point timestamp;
    std::optional<std::string> correlation_id;
};


struct InboundAttachment {
    std::string filename;
    std::string mime_type;
    std::vector<uint8_t> data;
    size_t size_bytes;
};


enum class TransportState { disconnected, connecting, connected, reconnecting, error };


struct TransportError {
    enum Code { connection_refused, timeout, auth_failed, protocol_error, closed_by_peer };
    Code code;
    std::string message;
    bool retryable{true};
};


using MessageHandler = std::function<void(BridgeMessage)>;
using AttachmentHandler = std::function<void(InboundAttachment)>;
using StateChangeHandler = std::function<void(TransportState, TransportState)>;  // old, new
using ErrorHandler = std::function<void(TransportError)>;


class TransportFlushGate {
    std::atomic<bool> open_{true};
    std::queue<BridgeMessage> pending_;
    std::function<void(BridgeMessage)> sender_;
public:
    void close() { open_ = false; }
    void open() {
        open_ = true;
        while (sender_ && !pending_.empty()) {
            auto msg = std::move(pending_.front());
            pending_.pop();
            sender_(std::move(msg));
        }
    }
    void enqueue(BridgeMessage msg) {
        if (open_ && sender_) {
            sender_(std::move(msg));
        } else {
            pending_.push(std::move(msg));
        }
    }
    void set_sender(std::function<void(BridgeMessage)> sender) { sender_ = std::move(sender); }
    [[nodiscard]] auto pending_count() const -> size_t { return pending_.size(); }
    [[nodiscard]] auto is_open() const -> bool { return open_.load(); }
};


class CapacityWake {
    size_t capacity_{100};
    size_t current_load_{0};
    std::function<void()> on_capacity_available_;
public:
    void set_capacity(size_t cap) { capacity_ = cap; }
    void record_usage(size_t units = 1) { current_load_ += units; }
    void release(size_t units = 1) {
        if (current_load_ >= units) current_load_ -= units;
        if (current_load_ < capacity_ && on_capacity_available_) on_capacity_available_();
    }
    [[nodiscard]] auto has_capacity() const -> bool { return current_load_ < capacity_; }
    void on_available(std::function<void()> cb) { on_capacity_available_ = std::move(cb); }
};


class BridgeTransport {
protected:
    TransportState state_{TransportState::disconnected};
    std::vector<MessageHandler> message_handlers_;
    std::vector<StateChangeHandler> state_handlers_;
    std::vector<ErrorHandler> error_handlers_;
    TransportFlushGate flush_gate_;
    CapacityWake capacity_wake_;

public:
    BridgeTransport() {
        flush_gate_.set_sender([this](BridgeMessage msg) { emit_message(std::move(msg)); });
    }

    virtual ~BridgeTransport() = default;
    

    virtual auto connect(std::string_view url, std::optional<std::string_view> token) 
        -> std::expected<void, TransportError> = 0;
    virtual void disconnect() = 0;
    

    virtual auto send(BridgeMessage msg) -> std::expected<void, TransportError> = 0;
    

    [[nodiscard]] auto get_state() const -> TransportState { return state_; }
    [[nodiscard]] auto is_connected() const -> bool { return state_ == TransportState::connected; }
    

    void on_message(MessageHandler handler) { message_handlers_.push_back(std::move(handler)); }
    void on_state_change(StateChangeHandler handler) { state_handlers_.push_back(std::move(handler)); }
    void on_error(ErrorHandler handler) { error_handlers_.push_back(std::move(handler)); }
    

    auto& flush_gate() { return flush_gate_; }
    auto& capacity() { return capacity_wake_; }

protected:
    [[nodiscard]] static auto serialize_message(const BridgeMessage& msg) -> std::string {
        std::ostringstream json;
        json << R"({"id":")";
        json << detail::json_escape(msg.id) << R"(","type":")" << detail::json_escape(msg.type)
             << R"(","method":")" << detail::json_escape(msg.method)
             << R"(","payload":)" << (msg.payload.empty() ? "{}" : msg.payload)
             << R"(,"priority":")" << detail::priority_to_string(msg.priority) << '"';
        if (msg.correlation_id) {
            json << R"(,"correlation_id":")" << detail::json_escape(*msg.correlation_id) << '"';
        }
        json << '}';
        return json.str();
    }

    [[nodiscard]] static auto parse_message_value(cc::utils::json::JsonVal root) -> std::optional<BridgeMessage> {
        if (!root.valid() || !root.is_obj()) return std::nullopt;

        auto payload_value = root.get("payload");
        BridgeMessage message{
            .id = root.get_string("id"),
            .type = root.get_string("type"),
            .method = root.get_string("method"),
            .payload = payload_value.valid() ? payload_value.to_string() : std::string("{}"),
            .priority = detail::priority_from_string(root.get_string("priority")),
            .timestamp = std::chrono::system_clock::now(),
            .correlation_id = std::nullopt,
        };
        if (message.id.empty() || message.type.empty()) return std::nullopt;
        auto correlation_id = root.get("correlation_id");
        if (correlation_id.is_str()) message.correlation_id = std::string(correlation_id.as_str());
        return message;
    }

    [[nodiscard]] static auto parse_inbound_message(std::string_view payload) -> std::optional<BridgeMessage> {
        auto parsed = cc::utils::json::parse(payload);
        if (!parsed || !parsed->root().is_obj()) return std::nullopt;
        return parse_message_value(parsed->root());
    }

    void set_state(TransportState new_state) {
        auto old = state_;
        state_ = new_state;
        for (const auto& h : state_handlers_) if (h) h(old, new_state);
    }
    void emit_message(BridgeMessage msg) {
        for (const auto& h : message_handlers_) if (h) h(msg);
    }
    void emit_error(TransportError err) {
        for (const auto& h : error_handlers_) if (h) h(err);
    }
};


class WebSocketTransport : public BridgeTransport {
    struct UrlParts {
        std::string host;
        std::uint16_t port{80};
        std::string path{"/"};
    };

    struct IncomingFrame {
        std::uint8_t opcode{0};
        std::string payload;
    };

    std::string url_;
    std::vector<std::string> outbound_frames_;
    uint32_t reconnect_attempts_{0};
    uint32_t max_reconnects_{10};
    std::atomic<bool> connected_{false};
    std::atomic<int> socket_fd_{-1};
    std::mutex send_mutex_;
    std::jthread reader_thread_;

public:
    ~WebSocketTransport() override { disconnect(); }

    auto connect(std::string_view url, std::optional<std::string_view> token)
        -> std::expected<void, TransportError> override {
        url_ = std::string(url);
        set_state(TransportState::connecting);

        auto parsed = parse_url(url_);
        if (!parsed) {
            auto error = TransportError{TransportError::protocol_error, parsed.error(), false};
            set_state(TransportState::error);
            emit_error(error);
            return std::unexpected(std::move(error));
        }

        auto fd = connect_tcp(parsed->host, parsed->port);
        if (!fd) {
            auto error = fd.error();
            set_state(TransportState::error);
            emit_error(error);
            return std::unexpected(std::move(error));
        }

        auto handshake = perform_handshake(*fd, *parsed, token);
        if (!handshake) {
            ::close(*fd);
            auto error = handshake.error();
            set_state(TransportState::error);
            emit_error(error);
            return std::unexpected(std::move(error));
        }

        socket_fd_.store(*fd);
        connected_.store(true);
        set_state(TransportState::connected);
        reconnect_attempts_ = 0;
        reader_thread_ = std::jthread([this](std::stop_token stop) {
            read_loop(stop);
        });
        return {};
    }
    
    void disconnect() override {
        const bool was_already_disconnected =
            state_ == TransportState::disconnected && !connected_.load();
        if (connected_.exchange(false)) {
            outbound_frames_.push_back(R"({"type":"close","code":1000})");
            {
                std::lock_guard lock(send_mutex_);
                const int fd = socket_fd_.exchange(-1);
                if (fd >= 0) {
                    (void)send_frame_locked(fd, 0x8, close_payload(1000));
                    ::shutdown(fd, SHUT_RDWR);
                    ::close(fd);
                }
            }
        }
        if (reader_thread_.joinable()) {
            reader_thread_.request_stop();
            if (reader_thread_.get_id() != std::this_thread::get_id()) {
                reader_thread_.join();
            }
        }
        if (!was_already_disconnected && state_ != TransportState::disconnected) {
            set_state(TransportState::disconnected);
        }
    }
    
    auto send(BridgeMessage msg) -> std::expected<void, TransportError> override {
        if (state_ != TransportState::connected || !connected_.load()) {
            return std::unexpected(TransportError{TransportError::connection_refused, "Bridge WebSocket is not connected"});
        }

        auto frame = serialize_message(msg);
        {
            std::lock_guard lock(send_mutex_);
            const int fd = socket_fd_.load();
            if (fd < 0) {
                return std::unexpected(TransportError{TransportError::closed_by_peer, "Bridge WebSocket has been closed"});
            }
            auto sent = send_frame_locked(fd, 0x1, frame);
            if (!sent) {
                connected_.store(false);
                set_state(TransportState::error);
                auto error = TransportError{TransportError::closed_by_peer, sent.error(), true};
                emit_error(error);
                return std::unexpected(std::move(error));
            }
        }
        outbound_frames_.push_back(std::move(frame));
        return {};
    }

    [[nodiscard]] auto sent_frames() const -> const std::vector<std::string>& { return outbound_frames_; }
    [[nodiscard]] auto max_reconnects() const -> uint32_t { return max_reconnects_; }

private:
    [[nodiscard]] static auto parse_url(std::string_view url) -> std::expected<UrlParts, std::string> {
        if (!url.starts_with("ws://")) {
            return std::unexpected("Bridge WebSocket transport currently supports ws:// URLs");
        }
        url.remove_prefix(5);

        UrlParts parts;
        auto slash = url.find('/');
        auto authority = slash == std::string_view::npos ? url : url.substr(0, slash);
        parts.path = slash == std::string_view::npos ? "/" : std::string(url.substr(slash));
        auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            parts.host = std::string(authority.substr(0, colon));
            auto port_text = std::string(authority.substr(colon + 1));
            char* end = nullptr;
            long parsed = std::strtol(port_text.c_str(), &end, 10);
            if (end == port_text.c_str() || *end != '\0' || parsed <= 0 || parsed > 65535) {
                return std::unexpected("Invalid bridge WebSocket port");
            }
            parts.port = static_cast<std::uint16_t>(parsed);
        } else {
            parts.host = std::string(authority);
            parts.port = 80;
        }
        if (parts.host.empty()) return std::unexpected("Bridge WebSocket host cannot be empty");
        return parts;
    }

    [[nodiscard]] static auto connect_tcp(const std::string& host, std::uint16_t port)
        -> std::expected<int, TransportError> {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* results = nullptr;
        auto port_text = std::to_string(port);
        int gai = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &results);
        if (gai != 0) {
            return std::unexpected(TransportError{
                TransportError::connection_refused,
                std::string("Bridge WebSocket DNS lookup failed: ") + gai_strerror(gai),
                true,
            });
        }

        int fd = -1;
        for (auto* ai = results; ai != nullptr; ai = ai->ai_next) {
            fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) continue;
            if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
            ::close(fd);
            fd = -1;
        }
        ::freeaddrinfo(results);
        if (fd < 0) {
            return std::unexpected(TransportError{
                TransportError::connection_refused,
                "Failed to connect bridge WebSocket",
                true,
            });
        }
        return fd;
    }

    [[nodiscard]] static auto send_all(int fd, std::string_view data) -> bool {
        while (!data.empty()) {
#ifdef MSG_NOSIGNAL
            auto n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
#else
            auto n = ::send(fd, data.data(), data.size(), 0);
#endif
            if (n <= 0) return false;
            data.remove_prefix(static_cast<std::size_t>(n));
        }
        return true;
    }

    [[nodiscard]] static auto read_exact(int fd, char* data, std::size_t size) -> bool {
        while (size > 0) {
            auto n = ::recv(fd, data, size, 0);
            if (n <= 0) return false;
            data += n;
            size -= static_cast<std::size_t>(n);
        }
        return true;
    }

    [[nodiscard]] static auto read_http_headers(int fd) -> std::expected<std::string, std::string> {
        std::string response;
        std::array<char, 1024> buffer{};
        while (response.find("\r\n\r\n") == std::string::npos) {
            auto n = ::recv(fd, buffer.data(), buffer.size(), 0);
            if (n <= 0) return std::unexpected("Connection closed before bridge WebSocket handshake completed");
            response.append(buffer.data(), static_cast<std::size_t>(n));
            if (response.size() > 64 * 1024) return std::unexpected("Bridge WebSocket handshake response is too large");
        }
        return response;
    }

    [[nodiscard]] static auto handshake_key() -> std::string {
        std::array<unsigned char, 16> bytes{};
        std::random_device rd;
        for (auto& byte : bytes) byte = static_cast<unsigned char>(rd());
        return detail::base64_encode(bytes.data(), bytes.size());
    }

    [[nodiscard]] static auto perform_handshake(
        int fd,
        const UrlParts& parts,
        std::optional<std::string_view> token
    ) -> std::expected<void, TransportError> {
        const auto key = handshake_key();
        std::ostringstream request;
        request << "GET " << parts.path << " HTTP/1.1\r\n"
                << "Host: " << parts.host << ":" << parts.port << "\r\n"
                << "Connection: Upgrade\r\n"
                << "Upgrade: websocket\r\n"
                << "Sec-WebSocket-Version: 13\r\n"
                << "Sec-WebSocket-Key: " << key << "\r\n";
        if (token && !token->empty()) {
            request << "Authorization: Bearer " << *token << "\r\n";
        }
        request << "\r\n";

        if (!send_all(fd, request.str())) {
            return std::unexpected(TransportError{TransportError::closed_by_peer, "Failed to send bridge WebSocket handshake", true});
        }
        auto response = read_http_headers(fd);
        if (!response) {
            return std::unexpected(TransportError{TransportError::closed_by_peer, response.error(), true});
        }
        if (response->find(" 101 ") == std::string::npos &&
            response->find(" 101\r\n") == std::string::npos) {
            return std::unexpected(TransportError{TransportError::protocol_error, "Bridge WebSocket handshake failed: expected HTTP 101", false});
        }
        if (response->find(detail::websocket_accept_key(key)) == std::string::npos) {
            return std::unexpected(TransportError{TransportError::protocol_error, "Bridge WebSocket handshake response has invalid accept key", false});
        }
        return {};
    }

    [[nodiscard]] static auto close_payload(int code) -> std::string {
        std::string payload;
        payload.push_back(static_cast<char>((code >> 8) & 0xff));
        payload.push_back(static_cast<char>(code & 0xff));
        return payload;
    }

    [[nodiscard]] static auto send_frame_locked(int fd, std::uint8_t opcode, std::string_view payload)
        -> std::expected<void, std::string> {
        std::string frame;
        frame.push_back(static_cast<char>(0x80 | opcode));
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

        std::array<unsigned char, 4> mask{};
        std::random_device rd;
        for (auto& byte : mask) byte = static_cast<unsigned char>(rd());
        for (auto byte : mask) frame.push_back(static_cast<char>(byte));
        for (std::size_t i = 0; i < payload.size(); ++i) {
            frame.push_back(static_cast<char>(payload[i] ^ mask[i % mask.size()]));
        }
        if (!send_all(fd, frame)) return std::unexpected("Failed to send bridge WebSocket frame");
        return {};
    }

    [[nodiscard]] static auto read_frame(int fd) -> std::expected<IncomingFrame, std::string> {
        unsigned char header[2]{};
        if (!read_exact(fd, reinterpret_cast<char*>(header), 2)) {
            return std::unexpected("Bridge WebSocket closed while reading frame header");
        }

        IncomingFrame frame;
        frame.opcode = header[0] & 0x0f;
        const bool masked = (header[1] & 0x80) != 0;
        std::uint64_t len = header[1] & 0x7f;
        if (len == 126) {
            unsigned char ext[2]{};
            if (!read_exact(fd, reinterpret_cast<char*>(ext), 2)) {
                return std::unexpected("Bridge WebSocket closed while reading frame length");
            }
            len = (static_cast<std::uint64_t>(ext[0]) << 8) | ext[1];
        } else if (len == 127) {
            unsigned char ext[8]{};
            if (!read_exact(fd, reinterpret_cast<char*>(ext), 8)) {
                return std::unexpected("Bridge WebSocket closed while reading frame length");
            }
            len = 0;
            for (unsigned char byte : ext) len = (len << 8) | byte;
        }
        if (len > 16 * 1024 * 1024) return std::unexpected("Bridge WebSocket frame exceeds 16MiB limit");

        std::array<unsigned char, 4> mask{};
        if (masked && !read_exact(fd, reinterpret_cast<char*>(mask.data()), mask.size())) {
            return std::unexpected("Bridge WebSocket closed while reading frame mask");
        }

        frame.payload.resize(static_cast<std::size_t>(len));
        if (len > 0 && !read_exact(fd, frame.payload.data(), frame.payload.size())) {
            return std::unexpected("Bridge WebSocket closed while reading frame payload");
        }
        if (masked) {
            for (std::size_t i = 0; i < frame.payload.size(); ++i) {
                frame.payload[i] = static_cast<char>(frame.payload[i] ^ mask[i % mask.size()]);
            }
        }
        return frame;
    }

    void read_loop(std::stop_token stop) {
        while (!stop.stop_requested() && connected_.load()) {
            int fd = socket_fd_.load();
            if (fd < 0) break;

            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(fd, &read_fds);
            timeval timeout{0, 100'000};
            int ready = ::select(fd + 1, &read_fds, nullptr, nullptr, &timeout);
            if (ready <= 0) continue;

            auto frame = read_frame(fd);
            if (!frame) {
                connected_.store(false);
                emit_error(TransportError{TransportError::closed_by_peer, frame.error(), true});
                break;
            }
            if (frame->opcode == 0x1 || frame->opcode == 0x2) {
                if (auto message = parse_inbound_message(frame->payload)) {
                    emit_message(std::move(*message));
                } else {
                    emit_error(TransportError{TransportError::protocol_error, "Bridge WebSocket received invalid message JSON", false});
                }
            } else if (frame->opcode == 0x8) {
                connected_.store(false);
                break;
            } else if (frame->opcode == 0x9) {
                std::lock_guard lock(send_mutex_);
                const int pong_fd = socket_fd_.load();
                if (pong_fd >= 0) (void)send_frame_locked(pong_fd, 0xA, frame->payload);
            }
        }

        if (!connected_.load()) {
            std::lock_guard lock(send_mutex_);
            const int fd = socket_fd_.exchange(-1);
            if (fd >= 0) {
                ::shutdown(fd, SHUT_RDWR);
                ::close(fd);
            }
        }
        set_state(TransportState::disconnected);
    }
};


class HttpPollingTransport : public BridgeTransport {
    std::string base_url_;
    std::string token_;
    std::chrono::milliseconds poll_interval_{1000};
    std::vector<std::string> posted_messages_;
    std::atomic<bool> connected_{false};
    cc::utils::HttpClient http_{};
    std::jthread poll_thread_;

public:
    ~HttpPollingTransport() override { disconnect(); }

    auto connect(std::string_view url, std::optional<std::string_view> token)
        -> std::expected<void, TransportError> override {
        set_state(TransportState::connecting);
        auto parsed = cc::utils::parse_url(url);
        if (!parsed) {
            auto error = TransportError{
                TransportError::protocol_error,
                parsed.error().message,
                false,
            };
            set_state(TransportState::error);
            emit_error(error);
            return std::unexpected(std::move(error));
        }

        base_url_ = std::string(url);
        while (!base_url_.empty() && base_url_.back() == '/') base_url_.pop_back();
        token_ = token ? std::string(*token) : std::string{};
        connected_.store(true);
        set_state(TransportState::connected);
        poll_thread_ = std::jthread([this](std::stop_token stop) {
            poll_loop(stop);
        });
        return {};
    }

    void disconnect() override {
        connected_.store(false);
        if (poll_thread_.joinable()) {
            poll_thread_.request_stop();
            if (poll_thread_.get_id() != std::this_thread::get_id()) {
                poll_thread_.join();
            }
        }
        if (state_ != TransportState::disconnected) {
            set_state(TransportState::disconnected);
        }
    }

    auto send(BridgeMessage msg) -> std::expected<void, TransportError> override {
        if (state_ != TransportState::connected || !connected_.load()) {
            return std::unexpected(TransportError{TransportError::connection_refused, "Bridge HTTP polling transport is not connected"});
        }

        auto body = serialize_message(msg);
        auto response = http_.post(endpoint("/messages"), body, headers());
        if (!response) {
            auto error = map_http_error(response.error());
            emit_error(error);
            return std::unexpected(std::move(error));
        }
        if (auto error = error_for_status(response->status, "POST /messages")) {
            if (!error->retryable) connected_.store(false);
            emit_error(*error);
            return std::unexpected(std::move(*error));
        }
        posted_messages_.push_back(body);
        emit_response_messages(response->body);
        return {};
    }

    [[nodiscard]] auto posted_messages() const -> const std::vector<std::string>& { return posted_messages_; }
    [[nodiscard]] auto poll_interval() const -> std::chrono::milliseconds { return poll_interval_; }

private:
    [[nodiscard]] auto endpoint(std::string_view suffix) const -> std::string {
        return base_url_ + std::string(suffix);
    }

    [[nodiscard]] auto headers() const -> std::unordered_map<std::string, std::string> {
        std::unordered_map<std::string, std::string> result{
            {"Content-Type", "application/json"},
            {"Accept", "application/json"},
        };
        if (!token_.empty()) {
            result["Authorization"] = "Bearer " + token_;
        }
        return result;
    }

    [[nodiscard]] static auto map_http_error(const cc::utils::HttpError& error) -> TransportError {
        switch (error.code) {
            case cc::utils::HttpError::timeout:
                return {TransportError::timeout, error.message, true};
            case cc::utils::HttpError::cancelled:
                return {TransportError::closed_by_peer, error.message, true};
            case cc::utils::HttpError::ssl_error:
            case cc::utils::HttpError::dns_error:
            case cc::utils::HttpError::connection_failed:
                return {TransportError::connection_refused, error.message, true};
        }
        return {TransportError::connection_refused, error.message, true};
    }

    [[nodiscard]] static auto error_for_status(int status, std::string_view operation) -> std::optional<TransportError> {
        if (status >= 200 && status < 300) return std::nullopt;
        if (status == 401 || status == 403) {
            return TransportError{
                TransportError::auth_failed,
                std::format("Bridge HTTP polling {} failed with HTTP {}", operation, status),
                false,
            };
        }
        return TransportError{
            status == 408 || status == 429 ? TransportError::timeout : TransportError::protocol_error,
            std::format("Bridge HTTP polling {} failed with HTTP {}", operation, status),
            status == 408 || status == 429 || status >= 500,
        };
    }

    [[nodiscard]] static auto trim_body(std::string_view body) -> std::string_view {
        while (!body.empty() && std::isspace(static_cast<unsigned char>(body.front()))) body.remove_prefix(1);
        while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back()))) body.remove_suffix(1);
        return body;
    }

    [[nodiscard]] static auto parse_message_batch(std::string_view body) -> std::vector<BridgeMessage> {
        std::vector<BridgeMessage> messages;
        body = trim_body(body);
        if (body.empty() || body == "null") return messages;

        auto parsed = cc::utils::json::parse(body);
        if (!parsed) return messages;
        auto root = parsed->root();
        auto collect_array = [&messages](cc::utils::json::JsonVal array) {
            if (!array.valid() || !array.is_arr()) return;
            array.iter([&messages](cc::utils::json::JsonVal item) {
                if (auto message = parse_message_value(item)) {
                    messages.push_back(std::move(*message));
                }
            });
        };

        if (root.is_arr()) {
            collect_array(root);
            return messages;
        }
        if (!root.is_obj()) return messages;

        auto batch = root.get("messages");
        if (!batch.valid()) batch = root.get("events");
        if (batch.is_arr()) {
            collect_array(batch);
            return messages;
        }

        auto single = root.get("message");
        if (single.is_obj()) {
            if (auto message = parse_message_value(single)) {
                messages.push_back(std::move(*message));
            }
            return messages;
        }

        if (auto message = parse_message_value(root)) {
            messages.push_back(std::move(*message));
        }
        return messages;
    }

    void emit_response_messages(std::string_view body) {
        for (auto& message : parse_message_batch(body)) {
            emit_message(std::move(message));
        }
    }

    void poll_loop(std::stop_token stop) {
        while (!stop.stop_requested() && connected_.load()) {
            auto response = http_.get(endpoint("/poll"), headers());
            if (!connected_.load() || stop.stop_requested()) break;

            if (!response) {
                auto error = map_http_error(response.error());
                emit_error(error);
                set_state(TransportState::reconnecting);
            } else if (auto error = error_for_status(response->status, "GET /poll")) {
                emit_error(*error);
                if (!error->retryable) {
                    connected_.store(false);
                    set_state(TransportState::error);
                    break;
                }
                set_state(TransportState::reconnecting);
            } else {
                if (state_ != TransportState::connected) set_state(TransportState::connected);
                emit_response_messages(response->body);
            }

            const auto sleep_step = std::chrono::milliseconds{50};
            auto slept = std::chrono::milliseconds{0};
            while (!stop.stop_requested() && connected_.load() && slept < poll_interval_) {
                std::this_thread::sleep_for(sleep_step);
                slept += sleep_step;
            }
        }
    }
};


class StdioTransport : public BridgeTransport {
public:
    auto connect(std::string_view, std::optional<std::string_view>)
        -> std::expected<void, TransportError> override {
        set_state(TransportState::connected);
        return {};
    }
    void disconnect() override { set_state(TransportState::disconnected); }
    auto send(BridgeMessage msg) -> std::expected<void, TransportError> override {
        if (state_ != TransportState::connected)
            return std::unexpected(TransportError{TransportError::connection_refused, "not connected"});
        std::println(std::cout, "{}", serialize_message(msg));
        flush_gate_.enqueue(std::move(msg));
        return {};
    }
};

} // namespace cc::bridge
