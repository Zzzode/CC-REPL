/// @file sse_transport.cppm
/// @brief Full SSE (Server-Sent Events) client transport with reconnection.
/// Implements proper SSE frame parsing per W3C spec, exponential backoff reconnect,
/// liveness timeout, and Last-Event-ID tracking for resume on reconnect.
module;
#include <string>
#include <string_view>
#include <map>
#include <deque>
#include <functional>
#include <expected>
#include <optional>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <random>
#include <algorithm>
#include <cstring>
#include <memory>
#include <cstdlib>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

export module cc.cli.sse_transport;

export namespace cc::cli {

using namespace std::chrono_literals;

// ============================================================
// SSE Reconnect Policy
// ============================================================

struct SSEReconnectPolicy {
    std::chrono::milliseconds initial_delay{1000};
    std::chrono::milliseconds max_delay{30000};
    double backoff_multiplier{2.0};
    double jitter_factor{0.25};
    uint32_t max_retries{60};               // ~10 minutes with 30s cap
    std::chrono::seconds liveness_timeout{45};  // no data → reconnect
};

// ============================================================
// Parsed SSE Event
// ============================================================

struct SSEEvent {
    std::string event;  // event type ("message" if unspecified)
    std::string data;   // concatenated data lines
    std::string id;     // last event id
    std::optional<std::chrono::milliseconds> retry;  // server-suggested retry
};

// ============================================================
// SSE Transport State
// ============================================================

enum class SSEState : uint8_t {
    disconnected,
    connecting,
    connected,
    reconnecting,
    closed       // user-initiated close, no reconnect
};

// ============================================================
// TLS session (https:// URLs only)
// ============================================================
//
// SSE endpoints (bridge / code-session streams) are https. Before this the
// transport parsed "https://" but connected with raw BSD sockets, so the TLS
// handshake was never performed and every https stream failed. The session is
// owned by the reader thread; close() interrupts it by shutting down the
// underlying fd, which unblocks SSL_read.
struct SSETlsSession {
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;

    SSETlsSession() = default;
    SSETlsSession(const SSETlsSession&) = delete;
    SSETlsSession& operator=(const SSETlsSession&) = delete;
    ~SSETlsSession() {
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
        }
        if (ctx) SSL_CTX_free(ctx);
    }
};

// ============================================================
// SSE (Server-Sent Events) Transport with Reconnect
// ============================================================

class SSETransport {
public:
    using EventCallback = std::function<void(const SSEEvent& event)>;
    using StateCallback = std::function<void(SSEState state)>;
    using ErrorCallback = std::function<void(std::string_view error)>;

    SSETransport() = default;
    explicit SSETransport(SSEReconnectPolicy policy) : policy_(std::move(policy)) {}
    ~SSETransport() { close(); }

    // Non-copyable, non-movable (owns thread)
    SSETransport(const SSETransport&) = delete;
    SSETransport& operator=(const SSETransport&) = delete;

    // ─── Connection ──────────────────────────────────────────

    /// Establish SSE connection. Starts background reader thread with auto-reconnect.
    std::expected<void, std::string> connect(std::string_view url,
                                              std::map<std::string, std::string> headers = {}) {
        if (state_.load() != SSEState::disconnected && state_.load() != SSEState::closed) {
            return std::unexpected("Already connected or connecting");
        }

        url_ = std::string(url);
        headers_ = std::move(headers);

        // Parse URL components
        if (!parse_url(url_)) {
            return std::unexpected("Invalid URL: must be http:// or https://");
        }

        set_state(SSEState::connecting);
        should_reconnect_.store(true);
        retry_count_ = 0;

        // Start reader thread
        reader_thread_ = std::jthread([this](std::stop_token stop) {
            connection_loop(stop);
        });

        return {};
    }

    /// Gracefully close the SSE connection. No reconnect.
    void close() {
        should_reconnect_.store(false);
        set_state(SSEState::closed);

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

    // ─── Callbacks ───────────────────────────────────────────

    void on_event(EventCallback cb) {
        std::lock_guard lock(cb_mutex_);
        event_cb_ = std::move(cb);
    }

    void on_state_change(StateCallback cb) {
        std::lock_guard lock(cb_mutex_);
        state_cb_ = std::move(cb);
    }

    void on_error(ErrorCallback cb) {
        std::lock_guard lock(cb_mutex_);
        error_cb_ = std::move(cb);
    }

    // ─── Accessors ───────────────────────────────────────────

    [[nodiscard]] bool is_connected() const { return state_.load() == SSEState::connected; }
    [[nodiscard]] SSEState state() const { return state_.load(); }

    [[nodiscard]] std::optional<std::string> get_last_event_id() const {
        std::lock_guard lock(data_mutex_);
        return last_event_id_;
    }

    [[nodiscard]] uint32_t retry_count() const { return retry_count_; }

    /// Receive a queued event (non-blocking). Returns nullopt if queue empty.
    [[nodiscard]] std::optional<SSEEvent> poll_event() {
        std::lock_guard lock(data_mutex_);
        if (event_queue_.empty()) return std::nullopt;
        auto ev = std::move(event_queue_.front());
        event_queue_.pop_front();
        return ev;
    }

    /// Blocking receive with timeout
    [[nodiscard]] std::optional<SSEEvent> receive(std::chrono::milliseconds timeout) {
        std::unique_lock lock(data_mutex_);
        if (event_cv_.wait_for(lock, timeout, [this] { return !event_queue_.empty(); })) {
            auto ev = std::move(event_queue_.front());
            event_queue_.pop_front();
            return ev;
        }
        return std::nullopt;
    }

private:
    // ─── URL Parsing ─────────────────────────────────────────

    struct UrlComponents {
        bool is_https = false;
        std::string host;
        uint16_t port = 80;
        std::string path;
    };

    bool parse_url(const std::string& url) {
        std::string_view sv(url);
        if (sv.starts_with("https://")) {
            url_parts_.is_https = true;
            sv.remove_prefix(8);
            url_parts_.port = 443;
        } else if (sv.starts_with("http://")) {
            url_parts_.is_https = false;
            sv.remove_prefix(7);
            url_parts_.port = 80;
        } else {
            return false;
        }

        auto slash_pos = sv.find('/');
        auto host_part = (slash_pos != std::string_view::npos) ? sv.substr(0, slash_pos) : sv;
        url_parts_.path = (slash_pos != std::string_view::npos) ? std::string(sv.substr(slash_pos)) : "/";

        auto colon_pos = host_part.find(':');
        if (colon_pos != std::string_view::npos) {
            url_parts_.host = std::string(host_part.substr(0, colon_pos));
            auto port_sv = host_part.substr(colon_pos + 1);
            url_parts_.port = static_cast<uint16_t>(std::atoi(std::string(port_sv).c_str()));
        } else {
            url_parts_.host = std::string(host_part);
        }
        return !url_parts_.host.empty();
    }

    // ─── Connection Loop (runs in background thread) ─────────

    void connection_loop(std::stop_token stop) {
        while (!stop.stop_requested() && should_reconnect_.load()) {
            auto fd = establish_connection();
            if (fd < 0) {
                emit_error("Connection failed");
                if (!wait_for_reconnect(stop)) break;
                continue;
            }

            socket_fd_.store(fd);
            set_state(SSEState::connected);
            retry_count_ = 0;
            current_delay_ = policy_.initial_delay;

            // TLS handshake for https URLs before any HTTP bytes are written.
            if (!establish_tls(fd)) {
                tls_.reset();
                ::close(fd);
                socket_fd_.store(-1);
                if (!wait_for_reconnect(stop)) break;
                continue;
            }

            // Send HTTP request
            if (!send_http_request(fd)) {
                tls_.reset();
                ::close(fd);
                socket_fd_.store(-1);
                emit_error("Failed to send HTTP request");
                if (!wait_for_reconnect(stop)) break;
                continue;
            }

            // Read and validate HTTP response headers
            if (!consume_http_headers(fd)) {
                tls_.reset();
                ::close(fd);
                socket_fd_.store(-1);
                emit_error("Invalid HTTP response");
                if (!wait_for_reconnect(stop)) break;
                continue;
            }

            // Stream SSE events
            read_sse_stream(fd, stop);

            tls_.reset();
            ::close(fd);
            socket_fd_.store(-1);

            if (stop.stop_requested() || !should_reconnect_.load()) break;

            // Connection was lost — reconnect
            set_state(SSEState::reconnecting);
            if (!wait_for_reconnect(stop)) break;
        }

        if (state_.load() != SSEState::closed) {
            set_state(SSEState::disconnected);
        }
    }

    // ─── TCP Connection ──────────────────────────────────────

    int establish_connection() {
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        auto port_str = std::to_string(url_parts_.port);
        if (getaddrinfo(url_parts_.host.c_str(), port_str.c_str(), &hints, &res) != 0) {
            return -1;
        }

        int fd = -1;
        for (auto* rp = res; rp != nullptr; rp = rp->ai_next) {
            fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (fd < 0) continue;

            // Set receive timeout for liveness detection
            struct timeval tv{};
            tv.tv_sec = policy_.liveness_timeout.count();
            tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            if (::connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
            ::close(fd);
            fd = -1;
        }
        freeaddrinfo(res);
        return fd;
    }

    // ─── HTTP Request ────────────────────────────────────────

    /// Read exactly up to `len` bytes, transparently over TLS when the URL
    /// is https. Returns the same values as ::recv (>=1 bytes, 0 on orderly
    /// close, <0 on error/timeout).
    ssize_t transport_recv(int fd, char* buf, std::size_t len) {
        if (tls_ && tls_->ssl) {
            int n = SSL_read(tls_->ssl, buf, static_cast<int>(len));
            if (n > 0) return n;
            int err = SSL_get_error(tls_->ssl, n);
            if (err == SSL_ERROR_ZERO_RETURN) return 0;  // clean TLS close
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                // Blocking socket + SO_RCVTIMEO: treat as liveness timeout so
                // the reconnect path (not a hard error) handles it.
                errno = EAGAIN;
                return -1;
            }
            return -1;
        }
        return ::recv(fd, buf, len, 0);
    }

    /// Write all bytes, transparently over TLS when the URL is https.
    bool transport_send_all(int fd, const std::string& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            ssize_t n = 0;
            if (tls_ && tls_->ssl) {
                n = SSL_write(tls_->ssl, data.data() + sent,
                              static_cast<int>(data.size() - sent));
                if (n <= 0) return false;
            } else {
                n = ::send(fd, data.data() + sent, data.size() - sent, 0);
                if (n <= 0) return false;
            }
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    /// Perform the TLS handshake (https only). Returns false on any failure;
    /// the caller tears the connection down and reconnects.
    bool establish_tls(int fd) {
        if (!url_parts_.is_https) return true;

        tls_ = std::make_unique<SSETlsSession>();
        tls_->ctx = SSL_CTX_new(TLS_client_method());
        if (!tls_->ctx) {
            emit_error("TLS: failed to create SSL context");
            tls_.reset();
            return false;
        }

        // Verify the server certificate against the system (or overridden)
        // CA bundle — an unverified stream carrying OAuth tokens and session
        // payloads would be worse than no transport at all.
        SSL_CTX_set_verify(tls_->ctx, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_default_verify_paths(tls_->ctx);
        if (const char* ca_env = std::getenv("LOOM_CA_BUNDLE");
            ca_env && *ca_env) {
            SSL_CTX_load_verify_locations(tls_->ctx, ca_env, nullptr);
        } else if (const char* ssl_cert = std::getenv("SSL_CERT_FILE");
                   ssl_cert && *ssl_cert) {
            SSL_CTX_load_verify_locations(tls_->ctx, ssl_cert, nullptr);
        }

        tls_->ssl = SSL_new(tls_->ctx);
        if (!tls_->ssl) {
            emit_error("TLS: failed to create SSL object");
            tls_.reset();
            return false;
        }
        SSL_set_fd(tls_->ssl, fd);
        // SNI + hostname verification: without these a valid certificate for
        // a different host would be accepted. IP literals are verified
        // against the certificate's IP SANs, not its DNS names.
        SSL_set_tlsext_host_name(tls_->ssl, url_parts_.host.c_str());
        unsigned char ip_buf[16] = {0};
        const bool is_ip_literal =
            ::inet_pton(AF_INET, url_parts_.host.c_str(), ip_buf) == 1 ||
            ::inet_pton(AF_INET6, url_parts_.host.c_str(), ip_buf) == 1;
        if (is_ip_literal) {
            X509_VERIFY_PARAM* param = SSL_get0_param(tls_->ssl);
            X509_VERIFY_PARAM_set1_ip_asc(param, url_parts_.host.c_str());
        } else {
            SSL_set1_host(tls_->ssl, url_parts_.host.c_str());
        }

        if (SSL_connect(tls_->ssl) != 1) {
            unsigned long err = ERR_get_error();
            char err_buf[256] = {0};
            if (err) ERR_error_string_n(err, err_buf, sizeof(err_buf));
            emit_error("TLS handshake failed" +
                       (err ? std::string(": ") + err_buf : std::string{}));
            tls_.reset();
            return false;
        }
        return true;
    }

    bool send_http_request(int fd) {
        std::string request = "GET " + url_parts_.path + " HTTP/1.1\r\n";
        request += "Host: " + url_parts_.host + "\r\n";
        request += "Accept: text/event-stream\r\n";
        request += "Cache-Control: no-cache\r\n";
        request += "Connection: keep-alive\r\n";

        // Add Last-Event-ID if we have one (for resume)
        {
            std::lock_guard lock(data_mutex_);
            if (last_event_id_.has_value()) {
                request += "Last-Event-ID: " + *last_event_id_ + "\r\n";
            }
        }

        // Custom headers
        for (const auto& [key, val] : headers_) {
            request += key + ": " + val + "\r\n";
        }
        request += "\r\n";

        return transport_send_all(fd, request);
    }

    // ─── HTTP Response Headers ───────────────────────────────

    bool consume_http_headers(int fd) {
        // Read until we see \r\n\r\n
        std::string header_buf;
        char c;
        while (header_buf.size() < 8192) {
            auto n = transport_recv(fd, &c, 1);
            if (n <= 0) return false;
            header_buf += c;
            if (header_buf.size() >= 4 && header_buf.ends_with("\r\n\r\n")) break;
        }

        // Check for 200 OK (or 2xx)
        if (header_buf.size() < 12) return false;
        auto status_line = header_buf.substr(0, header_buf.find("\r\n"));
        auto space1 = status_line.find(' ');
        if (space1 == std::string::npos) return false;
        auto status_code_str = status_line.substr(space1 + 1, 3);
        int status_code = std::atoi(status_code_str.c_str());

        // 2xx is success
        if (status_code < 200 || status_code >= 300) {
            emit_error("HTTP " + std::to_string(status_code));
            return false;
        }

        // Verify Content-Type contains text/event-stream
        auto ct_pos = header_buf.find("content-type:");
        if (ct_pos == std::string::npos) ct_pos = header_buf.find("Content-Type:");
        if (ct_pos != std::string::npos) {
            auto ct_line = header_buf.substr(ct_pos, header_buf.find("\r\n", ct_pos) - ct_pos);
            if (ct_line.find("text/event-stream") == std::string::npos) {
                // Not SSE content type — might still work, don't fail
            }
        }

        return true;
    }

    // ─── SSE Stream Reader ───────────────────────────────────

    void read_sse_stream(int fd, std::stop_token& stop) {
        // SSE parsing state per W3C spec
        std::string current_event;
        std::string current_data;
        std::string current_id;
        std::optional<std::chrono::milliseconds> current_retry;

        std::string line_buf;
        char buf[4096];

        while (!stop.stop_requested() && should_reconnect_.load()) {
            auto n = transport_recv(fd, buf, sizeof(buf));
            if (n < 0) {
                // Timeout (EAGAIN/EWOULDBLOCK) means liveness timeout exceeded
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    emit_error("Liveness timeout — no data received");
                    break;  // will trigger reconnect
                }
                break;
            }
            if (n == 0) break;  // connection closed by server

            // Process received bytes line by line
            for (ssize_t i = 0; i < n; ++i) {
                char c = buf[i];
                if (c == '\n') {
                    // Process the line
                    if (line_buf.empty()) {
                        // Empty line = dispatch event
                        if (!current_data.empty()) {
                            // Remove trailing newline from data
                            if (current_data.back() == '\n') {
                                current_data.pop_back();
                            }

                            // Update last event ID
                            if (!current_id.empty()) {
                                std::lock_guard lock(data_mutex_);
                                last_event_id_ = current_id;
                            }

                            // Update retry interval if server suggested one
                            if (current_retry.has_value()) {
                                current_delay_ = *current_retry;
                            }

                            // Dispatch event
                            SSEEvent event{
                                .event = current_event.empty() ? "message" : current_event,
                                .data = current_data,
                                .id = current_id,
                                .retry = current_retry
                            };

                            enqueue_event(std::move(event));
                        }
                        // Reset for next event
                        current_event.clear();
                        current_data.clear();
                        current_id.clear();
                        current_retry.reset();
                    } else {
                        // Parse field
                        parse_sse_line(line_buf, current_event, current_data,
                                      current_id, current_retry);
                        line_buf.clear();
                    }
                } else if (c == '\r') {
                    // Skip \r (handle \r\n and bare \r)
                    continue;
                } else {
                    line_buf += c;
                }
            }
        }
    }

    // ─── SSE Line Parser ─────────────────────────────────────

    static void parse_sse_line(const std::string& line,
                               std::string& event, std::string& data,
                               std::string& id,
                               std::optional<std::chrono::milliseconds>& retry) {
        // Lines starting with ':' are comments — ignore
        if (line.empty() || line[0] == ':') return;

        auto colon_pos = line.find(':');
        std::string_view field, value;

        if (colon_pos == std::string::npos) {
            field = line;
            value = "";
        } else {
            field = std::string_view(line).substr(0, colon_pos);
            value = std::string_view(line).substr(colon_pos + 1);
            // Strip single leading space from value per spec
            if (!value.empty() && value[0] == ' ') {
                value.remove_prefix(1);
            }
        }

        if (field == "event") {
            event = std::string(value);
        } else if (field == "data") {
            data += std::string(value);
            data += '\n';
        } else if (field == "id") {
            // IDs must not contain U+0000 NULL
            if (value.find('\0') == std::string_view::npos) {
                id = std::string(value);
            }
        } else if (field == "retry") {
            // Must be all digits
            bool all_digits = !value.empty();
            for (char c : value) {
                if (c < '0' || c > '9') { all_digits = false; break; }
            }
            if (all_digits) {
                retry = std::chrono::milliseconds(std::atoi(std::string(value).c_str()));
            }
        }
        // Unknown fields are ignored per spec
    }

    // ─── Reconnect Backoff ───────────────────────────────────

    bool wait_for_reconnect(std::stop_token& stop) {
        retry_count_++;
        if (retry_count_ > policy_.max_retries) {
            emit_error("Max reconnect attempts exceeded");
            set_state(SSEState::disconnected);
            return false;
        }

        // Exponential backoff with jitter
        auto delay = current_delay_;
        current_delay_ = std::chrono::milliseconds(
            std::min<int64_t>(
                static_cast<int64_t>(delay.count() * policy_.backoff_multiplier),
                policy_.max_delay.count()
            )
        );

        // Add jitter
        std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<double> dist(1.0 - policy_.jitter_factor,
                                                     1.0 + policy_.jitter_factor);
        auto jittered = std::chrono::milliseconds(
            static_cast<int64_t>(delay.count() * dist(rng))
        );

        // Sleep in small increments so we can respond to stop requests
        auto end_time = std::chrono::steady_clock::now() + jittered;
        while (std::chrono::steady_clock::now() < end_time) {
            if (stop.stop_requested() || !should_reconnect_.load()) return false;
            std::this_thread::sleep_for(50ms);
        }

        set_state(SSEState::connecting);
        return true;
    }

    // ─── Event Dispatch ──────────────────────────────────────

    void enqueue_event(SSEEvent event) {
        {
            std::lock_guard lock(cb_mutex_);
            if (event_cb_) {
                event_cb_(event);
            }
        }
        {
            std::lock_guard lock(data_mutex_);
            event_queue_.push_back(std::move(event));
            // Cap queue size to prevent unbounded growth
            while (event_queue_.size() > 10000) {
                event_queue_.pop_front();
            }
        }
        event_cv_.notify_one();
    }

    void set_state(SSEState new_state) {
        state_.store(new_state);
        std::lock_guard lock(cb_mutex_);
        if (state_cb_) state_cb_(new_state);
    }

    void emit_error(std::string msg) {
        std::lock_guard lock(cb_mutex_);
        if (error_cb_) error_cb_(msg);
    }

    // ─── State ───────────────────────────────────────────────

    std::string url_;
    UrlComponents url_parts_;
    std::map<std::string, std::string> headers_;
    SSEReconnectPolicy policy_;

    std::atomic<SSEState> state_{SSEState::disconnected};
    std::atomic<bool> should_reconnect_{false};
    std::atomic<int> socket_fd_{-1};
    /// TLS session for https URLs; owned by the reader thread, reset on every
    /// (re)connect. Non-null only mid-connection.
    std::unique_ptr<SSETlsSession> tls_;
    uint32_t retry_count_{0};
    std::chrono::milliseconds current_delay_{1000};

    // Thread
    std::jthread reader_thread_;

    // Event queue + Last-Event-ID
    mutable std::mutex data_mutex_;
    std::condition_variable event_cv_;
    std::deque<SSEEvent> event_queue_;
    std::optional<std::string> last_event_id_;

    // Callbacks
    std::mutex cb_mutex_;
    EventCallback event_cb_;
    StateCallback state_cb_;
    ErrorCallback error_cb_;
};

} // namespace cc::cli
