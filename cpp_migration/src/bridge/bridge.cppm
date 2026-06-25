/// @file bridge.cppm
/// @brief Bridge module for IDE integration (VS Code, JetBrains).
/// WebSocket-based bidirectional communication over libuv event loop.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <optional>
#include <expected>
#include <format>
#include <queue>
#include <mutex>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <array>

#include <uv.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

export module cc.bridge.bridge;

import cc.types.types;
import cc.bridge.messages;
import cc.bridge.inbound_messages;
import cc.bridge.security;

export namespace cc::bridge {

using cc::core::Result;
using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::VoidResult;

namespace detail {

[[nodiscard]] std::string base64url_encode(const unsigned char* data, std::size_t size) {
    std::string encoded;
    encoded.resize(4 * ((size + 2) / 3));
    auto out_len = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()),
        data,
        static_cast<int>(size));
    encoded.resize(static_cast<std::size_t>(out_len));
    for (auto& ch : encoded) {
        if (ch == '+') ch = '-';
        else if (ch == '/') ch = '_';
    }
    while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
    return encoded;
}

[[nodiscard]] bool verify_hs256_jwt(std::string_view token, std::string_view secret) {
    auto first_dot = token.find('.');
    auto second_dot = token.find('.', first_dot == std::string_view::npos ? 0 : first_dot + 1);
    if (first_dot == std::string_view::npos || second_dot == std::string_view::npos) return false;
    auto signing_input = token.substr(0, second_dot);
    auto signature = token.substr(second_dot + 1);

    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int digest_len = 0;
    HMAC(
        EVP_sha256(),
        secret.data(),
        static_cast<int>(secret.size()),
        reinterpret_cast<const unsigned char*>(signing_input.data()),
        signing_input.size(),
        digest,
        &digest_len);
    auto expected = base64url_encode(digest, digest_len);
    return expected == signature;
}

} // namespace detail

// ============================================================
// WebSocket Frame Types
// ============================================================

/// WebSocket opcode as per RFC 6455
enum class WsOpcode : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

/// Parsed WebSocket frame
struct WsFrame {
    WsOpcode opcode;
    bool fin = true;          // Final fragment flag
    std::string payload;      // Frame payload data

    /// Encode frame into wire format bytes
    [[nodiscard]] std::string encode() const {
        std::string out;
        // First byte: FIN + opcode
        out.push_back(static_cast<char>((fin ? 0x80 : 0x00) | static_cast<uint8_t>(opcode)));

        // Payload length encoding (server-to-client, no masking)
        if (payload.size() < 126) {
            out.push_back(static_cast<char>(payload.size()));
        } else if (payload.size() <= 0xFFFF) {
            out.push_back(126);
            out.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
            out.push_back(static_cast<char>(payload.size() & 0xFF));
        } else {
            out.push_back(127);
            for (int i = 7; i >= 0; --i) {
                out.push_back(static_cast<char>((payload.size() >> (8 * i)) & 0xFF));
            }
        }
        out.append(payload);
        return out;
    }
};

// ============================================================
// Bridge Connection State
// ============================================================

/// Connection lifecycle states
enum class ConnectionState : std::uint8_t {
    Disconnected,
    Connecting,
    Authenticating,
    Connected,
    Reconnecting,
    Closed,
};

/// JWT-based authentication token for bridge connections
struct BridgeAuth {
    std::string jwt_token;     // Signed JWT from IDE
    std::string session_id;    // Session this token is bound to
    std::uint64_t expires_at;  // Token expiry (unix ms)

    /// Check if token has expired
    [[nodiscard]] bool is_expired() const noexcept {
        using namespace std::chrono;
        auto now = duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count();
        return static_cast<std::uint64_t>(now) >= expires_at;
    }
};

// ============================================================
// Bridge Session
// ============================================================

/// Represents an active IDE<->CLI bridge session
struct BridgeSession {
    std::string session_id;
    std::string ide_name;        // "vscode", "jetbrains", "cursor"
    std::string ide_version;
    ConnectionState state = ConnectionState::Disconnected;
    std::uint64_t connected_at = 0;
    std::uint64_t last_heartbeat = 0;
    std::optional<BridgeAuth> auth;

    /// Check if session is currently active
    [[nodiscard]] bool is_active() const noexcept {
        return state == ConnectionState::Connected;
    }
};

// ============================================================
// Message Handler Callback Types
// ============================================================

using MessageHandler = std::function<void(const InboundMessage&)>;
using ErrorHandler = std::function<void(const Error&)>;
using ConnectionHandler = std::function<void(ConnectionState)>;

struct OutboundMessage {
    std::string type;
    std::string content;
};

[[nodiscard]] inline std::string serialize_outbound(const OutboundMessage& msg) {
    return std::format(R"({{"type":"{}","content":"{}"}})", msg.type, msg.content);
}

[[nodiscard]] inline std::optional<InboundMessage> deserialize_inbound(const std::string& payload) {
    return InboundMessage{
        .type = "message",
        .content = payload,
        .metadata = {},
        .received = std::chrono::system_clock::now(),
    };
}

// ============================================================
// Bridge Class - Main IDE integration interface
// ============================================================

/// Bidirectional bridge between CLI engine and IDE extensions.
/// Uses WebSocket protocol over libuv TCP for non-blocking IO.
class Bridge {
    uv_loop_t* loop_ = nullptr;              // libuv event loop (externally owned)
    uv_tcp_t server_{};                      // TCP server handle
    uv_tcp_t* client_ = nullptr;             // Connected client handle

    BridgeSession session_;                  // Current session state
    std::queue<OutboundMessage> send_queue_; // Outgoing message queue
    std::string recv_buffer_;                // Partial frame accumulator

    // Handler callbacks
    MessageHandler on_message_;
    ErrorHandler on_error_;
    ConnectionHandler on_state_change_;

    // Configuration
    std::string bind_host_ = "127.0.0.1";
    std::uint16_t bind_port_ = 0;           // 0 = auto-assign

    bool is_listening_ = false;

public:
    explicit Bridge(uv_loop_t* loop) : loop_(loop) {}
    ~Bridge() { shutdown(); }

    // Non-copyable, non-movable (due to libuv handle pointers)
    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    /// Start listening for IDE connections on the configured port
    [[nodiscard]] VoidResult listen(std::uint16_t port = 0) {
        bind_port_ = port;
        uv_tcp_init(loop_, &server_);
        server_.data = this;

        struct sockaddr_in addr{};
        uv_ip4_addr(bind_host_.c_str(), bind_port_, &addr);

        int r = uv_tcp_bind(&server_, reinterpret_cast<const sockaddr*>(&addr), 0);
        if (r != 0) {
            return std::unexpected(Error::make(ErrorCode::ConnectionFailed,
                std::format("Failed to bind: {}", uv_strerror(r))));
        }

        r = uv_listen(reinterpret_cast<uv_stream_t*>(&server_), 1, on_connection);
        if (r != 0) {
            return std::unexpected(Error::make(ErrorCode::ConnectionFailed,
                std::format("Failed to listen: {}", uv_strerror(r))));
        }

        is_listening_ = true;

        // Retrieve actual bound port if auto-assigned
        struct sockaddr_in bound_addr{};
        int namelen = sizeof(bound_addr);
        uv_tcp_getsockname(&server_, reinterpret_cast<sockaddr*>(&bound_addr), &namelen);
        bind_port_ = ntohs(bound_addr.sin_port);

        return {};
    }

    /// Send a message to the connected IDE
    [[nodiscard]] VoidResult send(OutboundMessage msg) {
        if (!session_.is_active()) {
            return std::unexpected(Error::make(
                ErrorCode::ConnectionFailed, "No active bridge connection"));
        }
        send_queue_.push(std::move(msg));
        flush_send_queue();
        return {};
    }

    /// Gracefully shut down the bridge
    void shutdown() {
        if (client_) {
            uv_close(reinterpret_cast<uv_handle_t*>(client_), nullptr);
            client_ = nullptr;
        }
        if (is_listening_) {
            uv_close(reinterpret_cast<uv_handle_t*>(&server_), nullptr);
            is_listening_ = false;
        }
        set_state(ConnectionState::Closed);
    }

    /// Authenticate an incoming connection with JWT
    [[nodiscard]] VoidResult authenticate(const BridgeAuth& auth) {
        if (auth.jwt_token.empty() || auth.session_id.empty()) {
            return std::unexpected(Error::make(
                ErrorCode::AuthenticationFailed, "Bridge JWT token and session ID are required"));
        }
        if (auth.is_expired()) {
            return std::unexpected(Error::make(
                ErrorCode::AuthenticationFailed, "Bridge JWT token expired"));
        }

        const char* secret = std::getenv("CC_BRIDGE_JWT_SECRET");
        if (!secret || *secret == '\0') {
            return std::unexpected(Error::make(
                ErrorCode::AuthenticationFailed, "CC_BRIDGE_JWT_SECRET is required for bridge JWT verification"));
        }
        if (!detail::verify_hs256_jwt(auth.jwt_token, secret)) {
            return std::unexpected(Error::make(
                ErrorCode::AuthenticationFailed, "Bridge JWT signature verification failed"));
        }

        auto payload = JwtUtils::decode_payload(auth.jwt_token);
        if (!payload) {
            return std::unexpected(Error::make(
                ErrorCode::AuthenticationFailed,
                std::format("Bridge JWT payload is invalid: {}", payload.error())));
        }
        if (JwtUtils::is_expired(*payload)) {
            return std::unexpected(Error::make(
                ErrorCode::AuthenticationFailed, "Bridge JWT payload is expired"));
        }
        if (!payload->sub.empty() && payload->sub != auth.session_id) {
            return std::unexpected(Error::make(
                ErrorCode::AuthenticationFailed, "Bridge JWT subject does not match session ID"));
        }

        session_.auth = auth;
        session_.session_id = auth.session_id;
        set_state(ConnectionState::Connected);
        return {};
    }

    // --- Callback registration ---
    void on_message(MessageHandler handler) { on_message_ = std::move(handler); }
    void on_error(ErrorHandler handler) { on_error_ = std::move(handler); }
    void on_state_change(ConnectionHandler handler) { on_state_change_ = std::move(handler); }

    // --- Accessors ---
    [[nodiscard]] const BridgeSession& session() const noexcept { return session_; }
    [[nodiscard]] std::uint16_t port() const noexcept { return bind_port_; }
    [[nodiscard]] bool is_connected() const noexcept { return session_.is_active(); }

private:
    /// Update connection state and fire callback
    void set_state(ConnectionState state) {
        session_.state = state;
        if (on_state_change_) on_state_change_(state);
    }

    /// Flush queued outbound messages over WebSocket
    void flush_send_queue() {
        while (!send_queue_.empty() && client_) {
            auto& msg = send_queue_.front();
            std::string json = serialize_outbound(msg);
            send_ws_frame(WsFrame{WsOpcode::Text, true, std::move(json)});
            send_queue_.pop();
        }
    }

    /// Encode and write a WebSocket frame to the client
    void send_ws_frame(const WsFrame& frame) {
        if (!client_) return;
        std::string wire = frame.encode();
        auto* req = new uv_write_t;
        auto* buf_data = new std::string(std::move(wire));
        uv_buf_t buf = uv_buf_init(buf_data->data(), static_cast<unsigned int>(buf_data->size()));
        req->data = buf_data;
        uv_write(req, reinterpret_cast<uv_stream_t*>(client_), &buf, 1, on_write_done);
    }

    /// Process received data, extract WebSocket frames
    void on_data_received(const char* data, std::size_t len) {
        recv_buffer_.append(data, len);

        // Try to parse complete frames from buffer
        while (recv_buffer_.size() >= 2) {
            // Minimal WebSocket frame parsing
            std::uint8_t b0 = static_cast<uint8_t>(recv_buffer_[0]);
            std::uint8_t b1 = static_cast<uint8_t>(recv_buffer_[1]);
            bool masked = (b1 & 0x80) != 0;
            std::uint64_t payload_len = b1 & 0x7F;
            std::size_t header_size = 2;

            if (payload_len == 126) {
                if (recv_buffer_.size() < 4) return; // Need more data
                payload_len = (static_cast<uint8_t>(recv_buffer_[2]) << 8)
                            | static_cast<uint8_t>(recv_buffer_[3]);
                header_size = 4;
            } else if (payload_len == 127) {
                if (recv_buffer_.size() < 10) return;
                payload_len = 0;
                for (int i = 0; i < 8; ++i) {
                    payload_len = (payload_len << 8) | static_cast<uint8_t>(recv_buffer_[2 + i]);
                }
                header_size = 10;
            }

            std::size_t mask_size = masked ? 4 : 0;
            std::size_t total = header_size + mask_size + payload_len;
            if (recv_buffer_.size() < total) return; // Incomplete frame

            // Extract and unmask payload
            std::string payload(recv_buffer_.begin() + header_size + mask_size,
                               recv_buffer_.begin() + total);
            if (masked) {
                const char* mask = recv_buffer_.data() + header_size;
                for (std::size_t i = 0; i < payload.size(); ++i) {
                    payload[i] ^= mask[i % 4];
                }
            }

            // Route by opcode
            auto opcode = static_cast<WsOpcode>(b0 & 0x0F);
            if (opcode == WsOpcode::Text && on_message_) {
                auto msg = deserialize_inbound(payload);
                if (msg) on_message_(*msg);
            } else if (opcode == WsOpcode::Ping) {
                send_ws_frame(WsFrame{WsOpcode::Pong, true, payload});
            } else if (opcode == WsOpcode::Close) {
                shutdown();
            }

            recv_buffer_.erase(0, total);
        }
    }

    // --- libuv static callbacks ---
    static void on_connection(uv_stream_t* server, int status) {
        auto* self = static_cast<Bridge*>(server->data);
        if (status < 0) {
            if (self->on_error_) {
                self->on_error_(Error::make(ErrorCode::ConnectionFailed,
                    std::format("Connection error: {}", uv_strerror(status))));
            }
            return;
        }

        self->client_ = new uv_tcp_t;
        uv_tcp_init(self->loop_, self->client_);
        self->client_->data = self;

        if (uv_accept(server, reinterpret_cast<uv_stream_t*>(self->client_)) == 0) {
            self->set_state(ConnectionState::Authenticating);
            uv_read_start(reinterpret_cast<uv_stream_t*>(self->client_),
                          on_alloc, on_read);
        }
    }

    static void on_alloc(uv_handle_t*, std::size_t suggested, uv_buf_t* buf) {
        buf->base = new char[suggested];
        buf->len = static_cast<unsigned int>(suggested);
    }

    static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
        auto* self = static_cast<Bridge*>(stream->data);
        if (nread > 0) {
            self->on_data_received(buf->base, static_cast<std::size_t>(nread));
        } else if (nread < 0) {
            // Connection closed or error
            self->set_state(ConnectionState::Disconnected);
            uv_close(reinterpret_cast<uv_handle_t*>(stream), nullptr);
            self->client_ = nullptr;
        }
        delete[] buf->base;
    }

    static void on_write_done(uv_write_t* req, int) {
        delete static_cast<std::string*>(req->data);
        delete req;
    }
};

} // namespace cc::bridge
