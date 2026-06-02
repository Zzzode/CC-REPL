/// @file remote_session.cppm
/// @brief Remote session management over WebSocket.
/// Implements the session subscribe protocol: connect, authenticate,
/// send/receive JSON-RPC messages, handle keepalive, and manage session lifecycle.
module;

#include <string>
#include <string_view>
#include <expected>
#include <optional>
#include <functional>
#include <map>
#include <vector>
#include <queue>
#include <chrono>
#include <atomic>
#include <mutex>
#include <thread>
#include <cstdint>
#include <format>

export module cc.remote.remote_session;

export import cc.remote.remote_connection;

export namespace cc::remote {

// ============================================================
// Remote Session Configuration
// ============================================================

struct RemoteSessionConfig {
    std::string host;
    uint16_t port = 443;
    std::string token;
    std::optional<std::string> session_id;
    std::string api_version = "v1";
    std::chrono::seconds ping_interval{30};
    std::chrono::seconds connect_timeout{10};
};

// ============================================================
// Session protocol message types
// ============================================================

enum class SessionMessageType {
    Auth,
    Subscribe,
    Unsubscribe,
    Request,
    Response,
    Event,
    Ping,
    Pong,
    Error,
};

struct SessionMessage {
    SessionMessageType type;
    std::string id;
    std::string method;
    std::string payload;    // JSON payload body
    std::optional<std::string> error;
};

enum class SessionStatus {
    Idle,
    Connecting,
    Authenticating,
    Subscribed,
    Disconnected,
    Error,
};

// ============================================================
// RemoteSession — high-level session management
// ============================================================

class RemoteSession {
public:
    using MessageHandler = std::function<void(const SessionMessage& msg)>;
    using StatusHandler = std::function<void(SessionStatus old_status, SessionStatus new_status)>;
    using ErrorHandler = std::function<void(std::string_view error)>;

    RemoteSession() = default;
    ~RemoteSession() { disconnect(); }

    // Prevent copy
    RemoteSession(const RemoteSession&) = delete;
    RemoteSession& operator=(const RemoteSession&) = delete;

    /// Connect and subscribe to a remote session
    auto connect(RemoteSessionConfig config) -> std::expected<void, std::string> {
        if (config.host.empty()) {
            return std::unexpected("Host cannot be empty");
        }
        if (config.token.empty()) {
            return std::unexpected("Authentication token required");
        }

        config_ = std::move(config);
        set_status(SessionStatus::Connecting);

        // Build WebSocket URL
        auto url = build_ws_url();

        // Set auth header
        std::map<std::string, std::string> headers;
        headers["Authorization"] = std::format("Bearer {}", config_.token);
        headers["X-Api-Version"] = config_.api_version;

        // Configure reconnection
        ReconnectPolicy policy;
        policy.max_retries = 10;
        policy.base_backoff = std::chrono::seconds{1};
        policy.max_backoff = std::chrono::seconds{30};
        connection_.set_reconnect_policy(policy);

        // Set up message handler
        connection_.on_message([this](std::string_view msg) {
            handle_incoming_message(msg);
        });

        connection_.on_state_change([this](ConnectionState old_state, ConnectionState new_state) {
            handle_connection_state_change(old_state, new_state);
        });

        connection_.on_error([this](std::string_view err) {
            if (error_handler_) error_handler_(err);
        });

        // Establish WebSocket connection
        auto result = connection_.establish(url, std::move(headers));
        if (!result) {
            set_status(SessionStatus::Error);
            return std::unexpected(std::format("Connection failed: {}", result.error()));
        }

        // Send authentication message
        set_status(SessionStatus::Authenticating);
        auto auth_result = send_auth();
        if (!auth_result) {
            set_status(SessionStatus::Error);
            return std::unexpected(auth_result.error());
        }

        // Subscribe to session events
        auto sub_result = send_subscribe();
        if (!sub_result) {
            set_status(SessionStatus::Error);
            return std::unexpected(sub_result.error());
        }

        set_status(SessionStatus::Subscribed);
        start_keepalive();
        return {};
    }

    /// Send a request message to the remote server
    auto send_request(std::string_view method, std::string_view payload)
        -> std::expected<void, std::string> {
        if (status_ != SessionStatus::Subscribed) {
            return std::unexpected("Not subscribed");
        }

        auto msg_id = generate_message_id();
        auto json = std::format(
            R"({{"type":"request","id":"{}","method":"{}","payload":{}}})",
            msg_id, method, payload.empty() ? "{}" : payload);

        return connection_.send(json);
    }

    /// Send a raw text message
    auto send_raw(std::string_view message) -> std::expected<void, std::string> {
        return connection_.send(message);
    }

    /// Poll for incoming messages (non-blocking)
    bool poll(std::chrono::milliseconds timeout = std::chrono::milliseconds{100}) {
        return connection_.poll(timeout);
    }

    /// Register handler for incoming session messages
    void on_message(MessageHandler handler) {
        message_handler_ = std::move(handler);
    }

    /// Register status change handler
    void on_status_change(StatusHandler handler) {
        status_handler_ = std::move(handler);
    }

    /// Register error handler
    void on_error(ErrorHandler handler) {
        error_handler_ = std::move(handler);
    }

    /// Disconnect from the remote session
    void disconnect() {
        stop_keepalive();

        if (status_ == SessionStatus::Subscribed) {
            // Send unsubscribe message (best-effort)
            auto json = std::format(
                R"({{"type":"unsubscribe","id":"{}","session_id":"{}"}})",
                generate_message_id(),
                config_.session_id.value_or(""));
            connection_.send(json);
        }

        connection_.close();
        set_status(SessionStatus::Disconnected);
    }

    /// Get current session status
    [[nodiscard]] SessionStatus status() const { return status_; }

    /// Get session ID (may be assigned by server)
    [[nodiscard]] const std::optional<std::string>& session_id() const { 
        return config_.session_id; 
    }

private:
    /// Build the WebSocket URL for session connection
    std::string build_ws_url() const {
        std::string url = std::format("wss://{}:{}/{}/sessions/ws",
            config_.host, config_.port, config_.api_version);
        
        if (config_.session_id.has_value()) {
            url += std::format("/{}/subscribe", *config_.session_id);
        }
        return url;
    }

    /// Send authentication message
    auto send_auth() -> std::expected<void, std::string> {
        auto json = std::format(
            R"({{"type":"auth","id":"{}","token":"{}"}})",
            generate_message_id(), config_.token);
        return connection_.send(json);
    }

    /// Send session subscribe message
    auto send_subscribe() -> std::expected<void, std::string> {
        auto json = std::format(
            R"({{"type":"subscribe","id":"{}","session_id":"{}"}})",
            generate_message_id(),
            config_.session_id.value_or("new"));
        return connection_.send(json);
    }

    /// Handle incoming raw WebSocket message
    void handle_incoming_message(std::string_view raw) {
        auto msg = parse_session_message(raw);
        if (!msg) return;

        switch (msg->type) {
            case SessionMessageType::Response:
            case SessionMessageType::Event:
                if (message_handler_) message_handler_(*msg);
                break;
            case SessionMessageType::Ping:
                // Respond with application-level pong
                send_app_pong(msg->id);
                break;
            case SessionMessageType::Error:
                if (error_handler_) {
                    error_handler_(msg->error.value_or("Unknown error"));
                }
                break;
            default:
                if (message_handler_) message_handler_(*msg);
                break;
        }
    }

    /// Parse a raw JSON message into SessionMessage
    static std::optional<SessionMessage> parse_session_message(std::string_view raw) {
        SessionMessage msg;

        // Simple JSON field extraction (for type, id, method, error)
        auto extract = [&](std::string_view key) -> std::string {
            auto pattern = std::format(R"("{}":")", key);
            auto pos = raw.find(pattern);
            if (pos == std::string_view::npos) return "";
            pos += pattern.size();
            auto end = raw.find('"', pos);
            if (end == std::string_view::npos) return "";
            return std::string(raw.substr(pos, end - pos));
        };

        auto type_str = extract("type");
        if (type_str == "auth") msg.type = SessionMessageType::Auth;
        else if (type_str == "subscribe") msg.type = SessionMessageType::Subscribe;
        else if (type_str == "unsubscribe") msg.type = SessionMessageType::Unsubscribe;
        else if (type_str == "request") msg.type = SessionMessageType::Request;
        else if (type_str == "response") msg.type = SessionMessageType::Response;
        else if (type_str == "event") msg.type = SessionMessageType::Event;
        else if (type_str == "ping") msg.type = SessionMessageType::Ping;
        else if (type_str == "pong") msg.type = SessionMessageType::Pong;
        else if (type_str == "error") msg.type = SessionMessageType::Error;
        else return std::nullopt;

        msg.id = extract("id");
        msg.method = extract("method");
        
        auto err = extract("error");
        if (!err.empty()) msg.error = err;

        // Extract payload (could be object or string)
        auto payload_pos = raw.find("\"payload\":");
        if (payload_pos != std::string_view::npos) {
            msg.payload = std::string(raw.substr(payload_pos + 10));
            // Trim trailing }
            if (!msg.payload.empty() && msg.payload.back() == '}') {
                msg.payload.pop_back();
            }
        }

        return msg;
    }

    /// Send application-level pong
    void send_app_pong(std::string_view ping_id) {
        auto json = std::format(R"({{"type":"pong","id":"{}"}})", ping_id);
        connection_.send(json);
    }

    /// Handle WebSocket connection state changes
    void handle_connection_state_change(ConnectionState /*old_state*/, ConnectionState new_state) {
        switch (new_state) {
            case ConnectionState::Connected:
                // Re-authenticate and re-subscribe after reconnection
                if (status_ == SessionStatus::Disconnected || 
                    status_ == SessionStatus::Connecting) {
                    send_auth();
                    send_subscribe();
                    set_status(SessionStatus::Subscribed);
                }
                break;
            case ConnectionState::Disconnected:
            case ConnectionState::Closed:
                set_status(SessionStatus::Disconnected);
                break;
            default:
                break;
        }
    }

    /// Start keepalive ping thread
    void start_keepalive() {
        keepalive_running_ = true;
        keepalive_thread_ = std::thread([this] {
            while (keepalive_running_) {
                std::this_thread::sleep_for(config_.ping_interval);
                if (!keepalive_running_) break;
                connection_.send_ping();
            }
        });
    }

    /// Stop keepalive thread
    void stop_keepalive() {
        keepalive_running_ = false;
        if (keepalive_thread_.joinable()) {
            keepalive_thread_.join();
        }
    }

    /// Generate a unique message ID
    std::string generate_message_id() {
        return std::format("msg_{}", ++msg_counter_);
    }

    /// Update status and notify handler
    void set_status(SessionStatus new_status) {
        auto old = status_;
        status_ = new_status;
        if (status_handler_ && old != new_status) {
            status_handler_(old, new_status);
        }
    }

    // State
    RemoteSessionConfig config_;
    RemoteConnection connection_;
    SessionStatus status_ = SessionStatus::Idle;
    uint64_t msg_counter_ = 0;

    // Keepalive
    std::thread keepalive_thread_;
    std::atomic<bool> keepalive_running_{false};

    // Handlers
    MessageHandler message_handler_;
    StatusHandler status_handler_;
    ErrorHandler error_handler_;
};

} // namespace cc::remote
