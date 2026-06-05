/// @file remote_connection.cppm
/// @brief WebSocket-based remote connection with automatic reconnection.
/// Provides a persistent connection to remote CC-REPL servers with exponential
/// backoff retry, ping/pong keepalive, and frame-level message handling.
module;

#include <string>
#include <string_view>
#include <expected>
#include <functional>
#include <map>
#include <vector>
#include <queue>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <random>
#include <cstdint>
#include <cstring>
#include <format>
#include <array>
#include <algorithm>

#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif

export module cc.remote.remote_connection;

export namespace cc::remote {

// ============================================================
// WebSocket frame types (RFC 6455)
// ============================================================

enum class WsOpcode : uint8_t {
    Continuation = 0x0,
    Text         = 0x1,
    Binary       = 0x2,
    Close        = 0x8,
    Ping         = 0x9,
    Pong         = 0xA,
};

enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting,
    Closed,
};

struct ReconnectPolicy {
    int max_retries = 10;
    std::chrono::seconds base_backoff{1};
    std::chrono::seconds max_backoff{30};
    double jitter_factor = 0.1;
};

// ============================================================
// WebSocket frame encoder/decoder
// ============================================================

namespace detail {

/// Generate a random 4-byte masking key (clients must mask frames per RFC 6455)
inline std::array<uint8_t, 4> generate_mask_key() {
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint32_t> dist;
    uint32_t val = dist(rng);
    std::array<uint8_t, 4> key{};
    std::memcpy(key.data(), &val, 4);
    return key;
}

/// Encode a WebSocket frame (client → server, always masked)
inline std::vector<uint8_t> encode_ws_frame(WsOpcode opcode, std::string_view payload) {
    std::vector<uint8_t> frame;
    frame.reserve(14 + payload.size());

    // FIN + opcode
    frame.push_back(0x80 | static_cast<uint8_t>(opcode));

    // Payload length + mask bit (clients must set mask=1)
    uint64_t len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<uint8_t>(len) | 0x80);
    } else if (len <= 0xFFFF) {
        frame.push_back(126 | 0x80);
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        frame.push_back(127 | 0x80);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
        }
    }

    // Masking key
    auto mask = generate_mask_key();
    frame.insert(frame.end(), mask.begin(), mask.end());

    // Masked payload
    for (size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);
    }

    return frame;
}

/// Decoded WebSocket frame
struct WsFrame {
    WsOpcode opcode;
    std::string payload;
    bool fin;
};

/// Decode a WebSocket frame from buffer. Returns bytes consumed or 0 if incomplete.
inline size_t decode_ws_frame(const uint8_t* data, size_t len, WsFrame& out) {
    if (len < 2) return 0;

    out.fin = (data[0] & 0x80) != 0;
    out.opcode = static_cast<WsOpcode>(data[0] & 0x0F);

    bool masked = (data[1] & 0x80) != 0;
    uint64_t payload_len = data[1] & 0x7F;
    size_t offset = 2;

    if (payload_len == 126) {
        if (len < 4) return 0;
        payload_len = (static_cast<uint64_t>(data[2]) << 8) | data[3];
        offset = 4;
    } else if (payload_len == 127) {
        if (len < 10) return 0;
        payload_len = 0;
        for (int i = 0; i < 8; ++i) {
            payload_len = (payload_len << 8) | data[2 + i];
        }
        offset = 10;
    }

    std::array<uint8_t, 4> mask_key{};
    if (masked) {
        if (len < offset + 4) return 0;
        std::memcpy(mask_key.data(), data + offset, 4);
        offset += 4;
    }

    if (len < offset + payload_len) return 0;

    out.payload.resize(payload_len);
    for (size_t i = 0; i < payload_len; ++i) {
        out.payload[i] = static_cast<char>(data[offset + i] ^ (masked ? mask_key[i % 4] : 0));
    }

    return offset + payload_len;
}

/// Base64 encode for WebSocket key
inline std::string base64_encode(const uint8_t* data, size_t len) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
        result.push_back(table[(n >> 18) & 0x3F]);
        result.push_back(table[(n >> 12) & 0x3F]);
        result.push_back((i + 1 < len) ? table[(n >> 6) & 0x3F] : '=');
        result.push_back((i + 2 < len) ? table[n & 0x3F] : '=');
    }
    return result;
}

/// Generate a random 16-byte WebSocket key
inline std::string generate_ws_key() {
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint8_t> dist(0, 255);
    std::array<uint8_t, 16> key{};
    for (auto& b : key) b = dist(rng);
    return base64_encode(key.data(), key.size());
}

/// SHA-1 for WebSocket accept validation
inline std::string sha1_base64(std::string_view input) {
    std::array<unsigned char, 20> hash{};
#ifdef __APPLE__
    CC_SHA1(input.data(), static_cast<CC_LONG>(input.size()), hash.data());
#else
    SHA1(reinterpret_cast<const unsigned char*>(input.data()), input.size(), hash.data());
#endif
    return base64_encode(hash.data(), hash.size());
}

} // namespace detail

// ============================================================
// RemoteConnection — WebSocket client with reconnection
// ============================================================

class RemoteConnection {
public:
    using MessageHandler = std::function<void(std::string_view message)>;
    using StateHandler = std::function<void(ConnectionState old_state, ConnectionState new_state)>;
    using ErrorHandler = std::function<void(std::string_view error)>;

    RemoteConnection() = default;
    ~RemoteConnection() { close(); }

    // Prevent copy
    RemoteConnection(const RemoteConnection&) = delete;
    RemoteConnection& operator=(const RemoteConnection&) = delete;

    /// Set reconnection policy
    void set_reconnect_policy(ReconnectPolicy policy) {
        policy_ = policy;
    }

    /// Register message handler
    void on_message(MessageHandler handler) {
        message_handler_ = std::move(handler);
    }

    /// Register state change handler
    void on_state_change(StateHandler handler) {
        state_handler_ = std::move(handler);
    }

    /// Register error handler
    void on_error(ErrorHandler handler) {
        error_handler_ = std::move(handler);
    }

    /// Establish a WebSocket connection to the given URL
    auto establish(std::string_view url, std::map<std::string, std::string> headers)
        -> std::expected<void, std::string> {
        if (url.empty()) {
            return std::unexpected("URL cannot be empty");
        }

        url_ = std::string(url);
        headers_ = std::move(headers);
        retry_count_ = 0;

        return connect_internal();
    }

    /// Send a text message over the WebSocket
    auto send(std::string_view message) -> std::expected<void, std::string> {
        if (state_ != ConnectionState::Connected || socket_fd_ < 0) {
            return std::unexpected("Not connected");
        }

        auto frame = detail::encode_ws_frame(WsOpcode::Text, message);
        if (!send_all(frame)) {
            handle_disconnect("Write failed");
            return std::unexpected("Send failed");
        }

        return {};
    }

    /// Send a ping frame
    auto send_ping() -> std::expected<void, std::string> {
        if (state_ != ConnectionState::Connected || socket_fd_ < 0) {
            return std::unexpected("Not connected");
        }

        auto frame = detail::encode_ws_frame(WsOpcode::Ping, "");
        if (!send_all(frame)) {
            handle_disconnect("Ping write failed");
            return std::unexpected("Ping failed");
        }
        return {};
    }

    /// Poll for incoming data. Call this in your event loop.
    /// Returns true if data was processed.
    bool poll(std::chrono::milliseconds timeout = std::chrono::milliseconds{100}) {
        if (socket_fd_ < 0) return false;

        struct pollfd pfd{};
        pfd.fd = socket_fd_;
        pfd.events = POLLIN;

        int ret = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
        if (ret <= 0) return false;

        if (pfd.revents & (POLLERR | POLLHUP)) {
            handle_disconnect("Connection lost");
            return false;
        }

        if (pfd.revents & POLLIN) {
            return read_and_process();
        }

        return false;
    }

    /// Force a reconnection attempt
    auto force_reconnect() -> std::expected<void, std::string> {
        close_socket();
        set_state(ConnectionState::Reconnecting);
        retry_count_ = 0;
        return connect_internal();
    }

    /// Close the connection permanently
    void close() {
        if (state_ == ConnectionState::Closed) return;
        
        // Send close frame if connected
        if (socket_fd_ >= 0 && state_ == ConnectionState::Connected) {
            auto frame = detail::encode_ws_frame(WsOpcode::Close, "");
            (void)send_all(frame);
        }

        close_socket();
        set_state(ConnectionState::Closed);
    }

    /// Get current state
    [[nodiscard]] ConnectionState state() const { return state_; }

    /// Check if connected
    [[nodiscard]] bool is_connected() const { return state_ == ConnectionState::Connected; }

    /// Get retry count
    [[nodiscard]] int retry_count() const { return retry_count_; }

private:
    /// Parse URL into (host, port, path)
    struct UrlParts {
        std::string host;
        uint16_t port;
        std::string path;
        bool tls;
    };

    static std::expected<UrlParts, std::string> parse_ws_url(std::string_view url) {
        UrlParts parts;
        
        if (url.starts_with("wss://")) {
            parts.tls = true;
            parts.port = 443;
            url.remove_prefix(6);
        } else if (url.starts_with("ws://")) {
            parts.tls = false;
            parts.port = 80;
            url.remove_prefix(5);
        } else {
            return std::unexpected("Invalid WebSocket URL scheme");
        }

        auto path_pos = url.find('/');
        auto host_part = (path_pos != std::string_view::npos) ? url.substr(0, path_pos) : url;
        parts.path = (path_pos != std::string_view::npos) ? std::string(url.substr(path_pos)) : "/";

        auto colon_pos = host_part.find(':');
        if (colon_pos != std::string_view::npos) {
            parts.host = std::string(host_part.substr(0, colon_pos));
            auto port_str = host_part.substr(colon_pos + 1);
            int port = 0;
            for (char c : port_str) {
                if (c < '0' || c > '9') return std::unexpected("Invalid port");
                port = port * 10 + (c - '0');
            }
            parts.port = static_cast<uint16_t>(port);
        } else {
            parts.host = std::string(host_part);
        }

        return parts;
    }

    /// Connect to the WebSocket server with retry
    auto connect_internal() -> std::expected<void, std::string> {
        set_state(ConnectionState::Connecting);

        while (retry_count_ <= policy_.max_retries) {
            auto result = do_connect();
            if (result.has_value()) {
                set_state(ConnectionState::Connected);
                retry_count_ = 0;
                return {};
            }

            ++retry_count_;
            if (retry_count_ > policy_.max_retries) {
                set_state(ConnectionState::Disconnected);
                return std::unexpected(std::format(
                    "Max retries ({}) exceeded: {}", policy_.max_retries, result.error()));
            }

            // Exponential backoff with jitter
            auto delay = compute_backoff();
            set_state(ConnectionState::Reconnecting);
            std::this_thread::sleep_for(delay);
        }

        return std::unexpected("Connection failed");
    }

    /// Single connection attempt: TCP connect + WebSocket handshake
    auto do_connect() -> std::expected<void, std::string> {
        auto url_parts = parse_ws_url(url_);
        if (!url_parts) return std::unexpected(url_parts.error());

        // DNS resolve
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        auto port_str = std::to_string(url_parts->port);
        int rc = ::getaddrinfo(url_parts->host.c_str(), port_str.c_str(), &hints, &res);
        if (rc != 0 || !res) {
            return std::unexpected(std::format("DNS resolution failed for {}", url_parts->host));
        }

        // Create socket and connect
        socket_fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (socket_fd_ < 0) {
            ::freeaddrinfo(res);
            return std::unexpected("Socket creation failed");
        }

        if (::connect(socket_fd_, res->ai_addr, res->ai_addrlen) < 0) {
            ::freeaddrinfo(res);
            close_socket();
            return std::unexpected(std::format("TCP connect to {}:{} failed", 
                url_parts->host, url_parts->port));
        }
        ::freeaddrinfo(res);

        if (url_parts->tls) {
            auto tls_result = connect_tls(url_parts->host);
            if (!tls_result) {
                close_socket();
                return std::unexpected(tls_result.error());
            }
        }

        // Perform WebSocket upgrade handshake
        return perform_handshake(url_parts->host, url_parts->port, url_parts->path);
    }

    /// Perform the WebSocket HTTP upgrade handshake
    auto perform_handshake(const std::string& host, uint16_t port, const std::string& path)
        -> std::expected<void, std::string> {
        
        ws_key_ = detail::generate_ws_key();

        // Build HTTP upgrade request
        std::string request = std::format(
            "GET {} HTTP/1.1\r\n"
            "Host: {}:{}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: {}\r\n"
            "Sec-WebSocket-Version: 13\r\n",
            path, host, port, ws_key_);

        // Add custom headers (auth, etc.)
        for (const auto& [key, val] : headers_) {
            request += std::format("{}: {}\r\n", key, val);
        }
        request += "\r\n";

        // Send request
        if (!send_all(request)) {
            return std::unexpected("Failed to send handshake");
        }

        // Read response (up to 4KB should be sufficient for headers)
        std::array<char, 4096> buf{};
        ssize_t bytes_read = read_some(buf.data(), buf.size() - 1);
        if (bytes_read <= 0) {
            return std::unexpected("No handshake response");
        }
        buf[bytes_read] = '\0';
        std::string_view response(buf.data(), static_cast<size_t>(bytes_read));

        // Validate 101 Switching Protocols
        if (response.find("HTTP/1.1 101") == std::string_view::npos) {
            return std::unexpected(std::format("Handshake rejected: {}", 
                response.substr(0, std::min(response.size(), size_t{64}))));
        }

        // Validate Sec-WebSocket-Accept
        auto expected_accept = detail::sha1_base64(
            ws_key_ + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        if (response.find(expected_accept) == std::string_view::npos) {
            return std::unexpected("Invalid Sec-WebSocket-Accept");
        }

        return {};
    }

    /// Read incoming data and process WebSocket frames
    bool read_and_process() {
        std::array<uint8_t, 65536> buf{};
        ssize_t n = read_some(reinterpret_cast<char*>(buf.data()), buf.size());
        if (n <= 0) {
            handle_disconnect("Read returned 0 or error");
            return false;
        }

        // Append to read buffer
        read_buffer_.insert(read_buffer_.end(), buf.data(), buf.data() + n);

        // Process all complete frames in the buffer
        bool processed = false;
        while (!read_buffer_.empty()) {
            detail::WsFrame frame;
            size_t consumed = detail::decode_ws_frame(
                read_buffer_.data(), read_buffer_.size(), frame);
            if (consumed == 0) break; // Incomplete frame

            read_buffer_.erase(read_buffer_.begin(), 
                              read_buffer_.begin() + static_cast<ptrdiff_t>(consumed));
            processed = true;

            switch (frame.opcode) {
                case WsOpcode::Text:
                case WsOpcode::Binary:
                    if (message_handler_) {
                        message_handler_(frame.payload);
                    }
                    break;
                case WsOpcode::Ping:
                    // Respond with pong
                    send_pong(frame.payload);
                    break;
                case WsOpcode::Pong:
                    last_pong_ = std::chrono::steady_clock::now();
                    break;
                case WsOpcode::Close:
                    handle_disconnect("Server sent close frame");
                    return true;
                default:
                    break;
            }
        }

        return processed;
    }

    /// Send a pong frame in response to a ping
    void send_pong(std::string_view payload) {
        if (socket_fd_ < 0) return;
        auto frame = detail::encode_ws_frame(WsOpcode::Pong, payload);
        (void)send_all(frame);
    }

    static std::string ssl_error_message(std::string_view prefix) {
        const unsigned long err = ERR_get_error();
        if (err == 0) return std::string(prefix);
        std::array<char, 256> buffer{};
        ERR_error_string_n(err, buffer.data(), buffer.size());
        return std::format("{}: {}", prefix, buffer.data());
    }

    auto connect_tls(const std::string& host) -> std::expected<void, std::string> {
        static std::once_flag init_flag;
        std::call_once(init_flag, [] {
            SSL_library_init();
            SSL_load_error_strings();
            OpenSSL_add_ssl_algorithms();
        });

        ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (ssl_ctx_ == nullptr) {
            return std::unexpected(ssl_error_message("Failed to create TLS context"));
        }
        SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_PEER, nullptr);
        if (SSL_CTX_set_default_verify_paths(ssl_ctx_) != 1) {
            cleanup_tls();
            return std::unexpected(ssl_error_message("Failed to load default TLS trust store"));
        }

        ssl_ = SSL_new(ssl_ctx_);
        if (ssl_ == nullptr) {
            cleanup_tls();
            return std::unexpected(ssl_error_message("Failed to create TLS session"));
        }

        SSL_set_fd(ssl_, socket_fd_);
        SSL_set_tlsext_host_name(ssl_, host.c_str());
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
        SSL_set1_host(ssl_, host.c_str());
#endif

        if (SSL_connect(ssl_) != 1) {
            auto message = ssl_error_message("TLS handshake failed");
            cleanup_tls();
            return std::unexpected(message);
        }
        return {};
    }

    [[nodiscard]] bool send_all(std::string_view data) {
        while (!data.empty()) {
            const auto written = write_some(data.data(), data.size());
            if (written <= 0) return false;
            data.remove_prefix(static_cast<std::size_t>(written));
        }
        return true;
    }

    [[nodiscard]] bool send_all(const std::vector<uint8_t>& data) {
        auto view = std::string_view(reinterpret_cast<const char*>(data.data()), data.size());
        return send_all(view);
    }

    [[nodiscard]] ssize_t write_some(const char* data, std::size_t size) {
        if (ssl_ == nullptr) {
            return ::write(socket_fd_, data, size);
        }
        while (true) {
            const int written = SSL_write(ssl_, data, static_cast<int>(size));
            if (written > 0) return written;
            const int err = SSL_get_error(ssl_, written);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
            return -1;
        }
    }

    [[nodiscard]] ssize_t read_some(char* data, std::size_t size) {
        if (ssl_ == nullptr) {
            return ::read(socket_fd_, data, size);
        }
        while (true) {
            const int read = SSL_read(ssl_, data, static_cast<int>(size));
            if (read > 0) return read;
            const int err = SSL_get_error(ssl_, read);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
            return -1;
        }
    }

    /// Handle a disconnection — attempt reconnect if policy allows
    void handle_disconnect(std::string_view reason) {
        close_socket();

        if (error_handler_) {
            error_handler_(reason);
        }

        if (state_ == ConnectionState::Closed) return;

        // Attempt reconnection
        set_state(ConnectionState::Reconnecting);
        auto result = connect_internal();
        if (!result.has_value() && error_handler_) {
            error_handler_(result.error());
        }
    }

    /// Compute backoff duration with jitter
    std::chrono::milliseconds compute_backoff() const {
        auto base_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            policy_.base_backoff * (1 << std::min(retry_count_, 5)));
        auto max_ms = std::chrono::duration_cast<std::chrono::milliseconds>(policy_.max_backoff);
        auto capped = std::min(base_ms, max_ms);

        // Add jitter
        static thread_local std::mt19937 rng(std::random_device{}());
        auto jitter = static_cast<int64_t>(capped.count() * policy_.jitter_factor);
        std::uniform_int_distribution<int64_t> dist(-jitter, jitter);
        return std::chrono::milliseconds(capped.count() + dist(rng));
    }

    /// Close the socket file descriptor
    void close_socket() {
        cleanup_tls();
        if (socket_fd_ >= 0) {
            ::close(socket_fd_);
            socket_fd_ = -1;
        }
        read_buffer_.clear();
    }

    void cleanup_tls() {
        if (ssl_ != nullptr) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (ssl_ctx_ != nullptr) {
            SSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = nullptr;
        }
    }

    /// Update state and notify handler
    void set_state(ConnectionState new_state) {
        auto old = state_;
        state_ = new_state;
        if (state_handler_ && old != new_state) {
            state_handler_(old, new_state);
        }
    }

    // Connection state
    std::string url_;
    std::map<std::string, std::string> headers_;
    std::string ws_key_;
    int socket_fd_ = -1;
    SSL_CTX* ssl_ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    ConnectionState state_ = ConnectionState::Disconnected;
    ReconnectPolicy policy_;
    int retry_count_ = 0;

    // Read buffer for frame assembly
    std::vector<uint8_t> read_buffer_;
    std::chrono::steady_clock::time_point last_pong_;

    // Handlers
    MessageHandler message_handler_;
    StateHandler state_handler_;
    ErrorHandler error_handler_;
};

} // namespace cc::remote
