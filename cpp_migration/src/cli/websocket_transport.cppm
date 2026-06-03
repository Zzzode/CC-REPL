module;
#include <string>
#include <string_view>
#include <functional>
#include <expected>
#include <format>
#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <vector>
#include <chrono>
#include <cstdint>
#include <array>
#include <cstdlib>
#include <cstring>
#include <random>
#include <utility>
#include <sys/select.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

export module cc.cli.websocket_transport;

export namespace cc::cli {

// WebSocket transport for bidirectional communication
class WebSocketTransport {
public:
    WebSocketTransport() = default;
    ~WebSocketTransport() { close(); }

    // Establish WebSocket connection to the given URL
    std::expected<void, std::string> connect(std::string_view url) {
        if (connected_.load()) {
            return std::unexpected("Already connected");
        }

        url_ = std::string(url);

        auto parsed = parse_url(url_);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        auto fd = connect_tcp(parsed->host, parsed->port);
        if (!fd) {
            return std::unexpected(fd.error());
        }

        if (parsed->tls) {
            auto tls = connect_tls(*fd, parsed->host);
            if (!tls) {
                ::close(*fd);
                return std::unexpected(tls.error());
            }
            ssl_ctx_ = tls->first;
            ssl_ = tls->second;
            tls_ = true;
        }

        auto handshake = perform_handshake(*fd, ssl_, *parsed);
        if (!handshake) {
            cleanup_tls();
            ::close(*fd);
            return std::unexpected(handshake.error());
        }

        socket_fd_ = *fd;
        connected_.store(true);

        // Start message reading thread
        reader_thread_ = std::jthread([this](std::stop_token stop) {
            run_read_loop(stop);
        });

        return {};
    }

    // Send a text message over the WebSocket connection
    std::expected<void, std::string> send(std::string_view message) {
        if (!connected_.load()) {
            return std::unexpected("Not connected");
        }

        if (message.empty()) {
            return std::unexpected("Cannot send empty message");
        }

        // Frame the message as a WebSocket text frame
        std::lock_guard lock(send_mutex_);
        send_queue_.push(std::string(message));

        return {};
    }

    // Register callback for incoming messages
    void on_message(std::function<void(std::string_view)> callback) {
        std::lock_guard lock(callback_mutex_);
        message_callback_ = std::move(callback);
    }

    // Register callback for connection close events
    void on_close(std::function<void(int code, std::string_view reason)> callback) {
        std::lock_guard lock(callback_mutex_);
        close_callback_ = std::move(callback);
    }

    // Close the WebSocket connection with an optional status code
    void close(int code = 1000) {
        if (!connected_.load()) return;

        connected_.store(false);
        close_code_ = code;

        if (socket_fd_ >= 0) {
            (void)send_close_frame(socket_fd_, ssl_, code);
            if (ssl_ != nullptr) {
                (void)SSL_shutdown(ssl_);
            }
            ::shutdown(socket_fd_, SHUT_RDWR);
            ::close(socket_fd_);
            socket_fd_ = -1;
        }
        cleanup_tls();

        if (reader_thread_.joinable()) {
            reader_thread_.request_stop();
            reader_thread_.join();
        }

        // Notify close callback
        {
            std::lock_guard lock(callback_mutex_);
            if (close_callback_) {
                close_callback_(code, "Connection closed");
            }
        }

        // Clear pending messages
        std::lock_guard lock(send_mutex_);
        while (!send_queue_.empty()) send_queue_.pop();
    }

    // Check if the WebSocket is currently connected
    bool is_connected() const {
        return connected_.load();
    }

private:
    struct UrlParts {
        bool tls{false};
        std::string host;
        std::uint16_t port{80};
        std::string path{"/"};
    };

    [[nodiscard]] static std::expected<UrlParts, std::string> parse_url(std::string_view url) {
        UrlParts parts;
        if (url.starts_with("ws://")) {
            parts.tls = false;
            parts.port = 80;
            url.remove_prefix(5);
        } else if (url.starts_with("wss://")) {
            parts.tls = true;
            parts.port = 443;
            url.remove_prefix(6);
        } else {
            return std::unexpected("Invalid WebSocket URL: must start with ws:// or wss://");
        }

        auto slash = url.find('/');
        auto authority = slash == std::string_view::npos ? url : url.substr(0, slash);
        parts.path = slash == std::string_view::npos ? "/" : std::string(url.substr(slash));
        auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            parts.host = std::string(authority.substr(0, colon));
            auto port_text = std::string(authority.substr(colon + 1));
            auto port = std::strtol(port_text.c_str(), nullptr, 10);
            if (port <= 0 || port > 65535) return std::unexpected("Invalid WebSocket port");
            parts.port = static_cast<std::uint16_t>(port);
        } else {
            parts.host = std::string(authority);
        }
        if (parts.host.empty()) return std::unexpected("WebSocket host cannot be empty");
        return parts;
    }

    [[nodiscard]] static std::expected<int, std::string> connect_tcp(const std::string& host, std::uint16_t port) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* results = nullptr;
        auto port_text = std::to_string(port);
        int gai = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &results);
        if (gai != 0) return std::unexpected(std::string("DNS lookup failed: ") + gai_strerror(gai));

        int fd = -1;
        for (auto* ai = results; ai != nullptr; ai = ai->ai_next) {
            fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) continue;
            if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
            ::close(fd);
            fd = -1;
        }
        ::freeaddrinfo(results);
        if (fd < 0) return std::unexpected("Failed to establish TCP connection");
        return fd;
    }

    [[nodiscard]] static std::string ssl_error_message(std::string_view prefix) {
        unsigned long err = ERR_get_error();
        if (err == 0) return std::string(prefix);
        std::array<char, 256> buffer{};
        ERR_error_string_n(err, buffer.data(), buffer.size());
        return std::format("{}: {}", prefix, buffer.data());
    }

    [[nodiscard]] static std::expected<std::pair<SSL_CTX*, SSL*>, std::string> connect_tls(
        int fd, const std::string& host) {
        static std::once_flag init_flag;
        std::call_once(init_flag, [] {
            SSL_library_init();
            SSL_load_error_strings();
            OpenSSL_add_ssl_algorithms();
        });

        SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
        if (ctx == nullptr) {
            return std::unexpected(ssl_error_message("Failed to create TLS context"));
        }
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
            SSL_CTX_free(ctx);
            return std::unexpected(ssl_error_message("Failed to load default TLS trust store"));
        }

        SSL* ssl = SSL_new(ctx);
        if (ssl == nullptr) {
            SSL_CTX_free(ctx);
            return std::unexpected(ssl_error_message("Failed to create TLS session"));
        }
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, host.c_str());
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
        SSL_set1_host(ssl, host.c_str());
#endif

        if (SSL_connect(ssl) != 1) {
            auto message = ssl_error_message("TLS handshake failed");
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            return std::unexpected(message);
        }
        return std::pair{ctx, ssl};
    }

    [[nodiscard]] static bool send_all(int fd, SSL* ssl, std::string_view data) {
        while (!data.empty()) {
            int n = ssl == nullptr
                ? static_cast<int>(::send(fd, data.data(), data.size(), 0))
                : SSL_write(ssl, data.data(), static_cast<int>(data.size()));
            if (n <= 0) {
                if (ssl != nullptr) {
                    int err = SSL_get_error(ssl, n);
                    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
                }
                return false;
            }
            data.remove_prefix(static_cast<std::size_t>(n));
        }
        return true;
    }

    [[nodiscard]] static std::expected<void, std::string> read_exact(int fd, SSL* ssl, char* data, std::size_t size) {
        while (size > 0) {
            int n = ssl == nullptr
                ? static_cast<int>(::recv(fd, data, size, 0))
                : SSL_read(ssl, data, static_cast<int>(size));
            if (n <= 0) {
                if (ssl != nullptr) {
                    int err = SSL_get_error(ssl, n);
                    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
                }
                return std::unexpected("WebSocket connection closed while reading");
            }
            data += n;
            size -= static_cast<std::size_t>(n);
        }
        return {};
    }

    [[nodiscard]] static std::expected<std::string, std::string> read_http_headers(int fd, SSL* ssl) {
        std::string response;
        std::array<char, 1024> buffer{};
        while (response.find("\r\n\r\n") == std::string::npos) {
            int n = ssl == nullptr
                ? static_cast<int>(::recv(fd, buffer.data(), buffer.size(), 0))
                : SSL_read(ssl, buffer.data(), static_cast<int>(buffer.size()));
            if (n <= 0) {
                if (ssl != nullptr) {
                    int err = SSL_get_error(ssl, n);
                    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
                }
                return std::unexpected("Connection closed before WebSocket handshake completed");
            }
            response.append(buffer.data(), static_cast<std::size_t>(n));
            if (response.size() > 64 * 1024) return std::unexpected("WebSocket handshake response is too large");
        }
        return response;
    }

    [[nodiscard]] static std::string handshake_key() {
        std::array<unsigned char, 16> bytes{};
        std::random_device rd;
        for (auto& b : bytes) b = static_cast<unsigned char>(rd());
        static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        int val = 0;
        int valb = -6;
        for (unsigned char c : bytes) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) {
                out.push_back(alphabet[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        if (valb > -6) out.push_back(alphabet[((val << 8) >> (valb + 8)) & 0x3F]);
        while (out.size() % 4) out.push_back('=');
        return out;
    }

    [[nodiscard]] static std::expected<void, std::string> perform_handshake(int fd, SSL* ssl, const UrlParts& parts) {
        auto key = handshake_key();
        auto request =
            "GET " + parts.path + " HTTP/1.1\r\n" +
            "Host: " + parts.host + ":" + std::to_string(parts.port) + "\r\n" +
            "Connection: Upgrade\r\n"
            "Upgrade: websocket\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Key: " + key + "\r\n\r\n";
        if (!send_all(fd, ssl, request)) return std::unexpected("Failed to send WebSocket handshake");
        auto response = read_http_headers(fd, ssl);
        if (!response) return std::unexpected(response.error());
        if (response->find(" 101 ") == std::string::npos &&
            response->find(" 101\r\n") == std::string::npos) {
            return std::unexpected("WebSocket handshake failed: expected HTTP 101");
        }
        if (response->find("Upgrade") == std::string::npos &&
            response->find("upgrade") == std::string::npos) {
            return std::unexpected("WebSocket handshake response did not include Upgrade header");
        }
        return {};
    }

    [[nodiscard]] static std::expected<void, std::string> send_frame(int fd, SSL* ssl, std::uint8_t opcode, std::string_view payload) {
        std::string frame;
        frame.push_back(static_cast<char>(0x80 | opcode));
        auto len = payload.size();
        if (len < 126) {
            frame.push_back(static_cast<char>(0x80 | len));
        } else if (len <= 0xFFFF) {
            frame.push_back(static_cast<char>(0x80 | 126));
            frame.push_back(static_cast<char>((len >> 8) & 0xFF));
            frame.push_back(static_cast<char>(len & 0xFF));
        } else {
            frame.push_back(static_cast<char>(0x80 | 127));
            for (int i = 7; i >= 0; --i) {
                frame.push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
            }
        }
        std::array<unsigned char, 4> mask{};
        std::random_device rd;
        for (auto& b : mask) b = static_cast<unsigned char>(rd());
        for (auto b : mask) frame.push_back(static_cast<char>(b));
        for (std::size_t i = 0; i < payload.size(); ++i) {
            frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
        }
        if (!send_all(fd, ssl, frame)) return std::unexpected("Failed to send WebSocket frame");
        return {};
    }

    [[nodiscard]] static std::expected<void, std::string> send_close_frame(int fd, SSL* ssl, int code) {
        std::string payload;
        payload.push_back(static_cast<char>((code >> 8) & 0xFF));
        payload.push_back(static_cast<char>(code & 0xFF));
        return send_frame(fd, ssl, 0x8, payload);
    }

    struct IncomingFrame {
        std::uint8_t opcode{0};
        std::string payload;
    };

    [[nodiscard]] static std::expected<IncomingFrame, std::string> read_frame(int fd, SSL* ssl) {
        unsigned char header[2]{};
        auto header_result = read_exact(fd, ssl, reinterpret_cast<char*>(header), 2);
        if (!header_result) return std::unexpected(header_result.error());

        IncomingFrame frame;
        frame.opcode = header[0] & 0x0F;
        bool masked = (header[1] & 0x80) != 0;
        std::uint64_t len = header[1] & 0x7F;
        if (len == 126) {
            unsigned char ext[2]{};
            auto result = read_exact(fd, ssl, reinterpret_cast<char*>(ext), 2);
            if (!result) return std::unexpected(result.error());
            len = (static_cast<std::uint64_t>(ext[0]) << 8) | ext[1];
        } else if (len == 127) {
            unsigned char ext[8]{};
            auto result = read_exact(fd, ssl, reinterpret_cast<char*>(ext), 8);
            if (!result) return std::unexpected(result.error());
            len = 0;
            for (unsigned char b : ext) len = (len << 8) | b;
        }
        if (len > 16 * 1024 * 1024) return std::unexpected("WebSocket frame exceeds 16MiB limit");

        std::array<unsigned char, 4> mask{};
        if (masked) {
            auto result = read_exact(fd, ssl, reinterpret_cast<char*>(mask.data()), mask.size());
            if (!result) return std::unexpected(result.error());
        }
        frame.payload.resize(static_cast<std::size_t>(len));
        if (len > 0) {
            auto result = read_exact(fd, ssl, frame.payload.data(), frame.payload.size());
            if (!result) return std::unexpected(result.error());
        }
        if (masked) {
            for (std::size_t i = 0; i < frame.payload.size(); ++i) {
                frame.payload[i] = static_cast<char>(frame.payload[i] ^ mask[i % 4]);
            }
        }
        return frame;
    }

    // Read loop processes incoming WebSocket frames
    void run_read_loop(std::stop_token stop) {
        while (!stop.stop_requested() && connected_.load()) {
            // Drain send queue first (send pending outgoing messages)
            {
                std::lock_guard lock(send_mutex_);
                while (!send_queue_.empty()) {
                    auto& msg = send_queue_.front();
                    if (socket_fd_ < 0 || !send_frame(socket_fd_, ssl_, 0x1, msg)) {
                        connected_.store(false);
                        break;
                    }
                    sent_count_.fetch_add(1, std::memory_order_relaxed);
                    send_queue_.pop();
                }
            }

            if (socket_fd_ >= 0) {
                fd_set read_fds;
                FD_ZERO(&read_fds);
                FD_SET(socket_fd_, &read_fds);
                timeval timeout{0, 20'000};
                int ready = ::select(socket_fd_ + 1, &read_fds, nullptr, nullptr, &timeout);
                if (ready > 0 && FD_ISSET(socket_fd_, &read_fds)) {
                    auto frame = read_frame(socket_fd_, ssl_);
                    if (!frame) {
                        connected_.store(false);
                        break;
                    }
                    last_activity_ = std::chrono::steady_clock::now();
                    if (frame->opcode == 0x1 || frame->opcode == 0x2) {
                        dispatch_message(frame->payload);
                    } else if (frame->opcode == 0x8) {
                        connected_.store(false);
                        break;
                    } else if (frame->opcode == 0x9) {
                        (void)send_frame(socket_fd_, ssl_, 0xA, frame->payload);
                    }
                }
            }

            auto now = std::chrono::steady_clock::now();
            if (now - last_activity_ > std::chrono::seconds(30)) {
                if (socket_fd_ >= 0) (void)send_frame(socket_fd_, ssl_, 0x9, {});
                last_activity_ = now;
            }
        }
        connected_.store(false);
    }

    // Dispatch a received message to the callback
    void dispatch_message(std::string_view message) {
        std::lock_guard lock(callback_mutex_);
        if (message_callback_) {
            message_callback_(message);
        }
    }

    void cleanup_tls() {
        if (ssl_ != nullptr) {
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (ssl_ctx_ != nullptr) {
            SSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = nullptr;
        }
        tls_ = false;
    }

    std::string url_;
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> sent_count_{0};
    int close_code_{0};
    int socket_fd_{-1};
    SSL_CTX* ssl_ctx_{nullptr};
    SSL* ssl_{nullptr};
    bool tls_{false};
    std::chrono::steady_clock::time_point last_activity_{std::chrono::steady_clock::now()};

    std::function<void(std::string_view)> message_callback_;
    std::function<void(int, std::string_view)> close_callback_;
    std::mutex callback_mutex_;

    std::queue<std::string> send_queue_;
    std::mutex send_mutex_;

    std::jthread reader_thread_;
};

} // namespace cc::cli
