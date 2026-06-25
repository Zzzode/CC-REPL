/// @file session_runner.cppm
/// @brief SessionRunner manages the bridge lifecycle including
/// connection, reconnection, authentication, and message routing.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <expected>
#include <format>
#include <chrono>
#include <queue>
#include <atomic>
#include <algorithm>

#include <uv.h>

export module cc.bridge.session_runner;

import cc.types.types;
import cc.bridge.bridge;
import cc.bridge.messages;
import cc.bridge.inbound_messages;

export namespace cc::bridge {

using cc::core::Result;
using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::VoidResult;

// ============================================================
// Session Configuration
// ============================================================

/// Configuration for the bridge session runner
struct SessionConfig {
    std::string session_id;                        // Unique session ID
    std::uint16_t port = 0;                        // Port to listen on (0=auto)
    std::uint32_t reconnect_delay_ms = 1000;       // Initial reconnection delay
    std::uint32_t max_reconnect_delay_ms = 30000;  // Max backoff for reconnection
    std::uint32_t heartbeat_interval_ms = 30000;   // Heartbeat ping interval
    std::uint32_t auth_timeout_ms = 5000;          // Authentication timeout
    std::uint8_t max_reconnect_attempts = 10;      // Max reconnection attempts
    std::optional<std::string> jwt_secret{};       // Secret for JWT validation

    /// Default configuration for local development
    [[nodiscard]] static SessionConfig dev_defaults() {
        return SessionConfig{
            .session_id = "dev-session",
            .port = 19876,
            .reconnect_delay_ms = 500,
            .max_reconnect_delay_ms = 5000,
            .heartbeat_interval_ms = 10000,
            .auth_timeout_ms = 10000,
            .max_reconnect_attempts = 5,
        };
    }
};

// ============================================================
// Session State
// ============================================================

/// Internal runner state machine
enum class RunnerState : std::uint8_t {
    Idle,           // Not started
    Starting,       // Initializing event loop and bridge
    Listening,      // Waiting for IDE connection
    Authenticating, // Verifying JWT credentials
    Running,        // Fully operational, routing messages
    Reconnecting,   // Lost connection, attempting recovery
    Stopping,       // Graceful shutdown in progress
    Stopped,        // Terminated
};

// ============================================================
// Message Route - maps method to handler
// ============================================================

/// A registered message route with its handler function
struct MessageRoute {
    std::string method_prefix;  // Method pattern to match (e.g., "file/")
    std::function<void(const InboundMessage&)> handler;
};

// ============================================================
// SessionRunner - orchestrates the bridge lifecycle
// ============================================================

/// Manages the full lifecycle of a bridge session including
/// startup, authentication, message routing, and graceful shutdown.
class SessionRunner {
    SessionConfig config_;
    std::unique_ptr<Bridge> bridge_;
    uv_loop_t* loop_ = nullptr;
    bool owns_loop_ = false;

    // Lifecycle state
    RunnerState state_ = RunnerState::Idle;
    std::uint8_t reconnect_attempts_ = 0;
    std::uint32_t current_reconnect_delay_ = 0;

    // Timers
    uv_timer_t heartbeat_timer_{};
    uv_timer_t auth_timer_{};
    uv_timer_t reconnect_timer_{};

    // Message routing table
    std::vector<MessageRoute> routes_;

    // External callbacks
    std::function<void(RunnerState)> on_state_change_;
    std::function<void(const std::string&)> on_user_input_;
    std::function<void(const Error&)> on_error_;

public:
    explicit SessionRunner(SessionConfig config)
        : config_(std::move(config))
        , current_reconnect_delay_(config_.reconnect_delay_ms) {}

    ~SessionRunner() { stop(); }

    // Non-copyable
    SessionRunner(const SessionRunner&) = delete;
    SessionRunner& operator=(const SessionRunner&) = delete;

    /// Start the session runner with a new event loop
    [[nodiscard]] VoidResult start() {
        if (state_ != RunnerState::Idle && state_ != RunnerState::Stopped) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError, "SessionRunner already running"));
        }

        // Create event loop
        loop_ = new uv_loop_t;
        uv_loop_init(loop_);
        owns_loop_ = true;

        return start_with_loop(loop_);
    }

    /// Start with an externally-managed event loop
    [[nodiscard]] VoidResult start_with_loop(uv_loop_t* loop) {
        loop_ = loop;
        set_state(RunnerState::Starting);

        // Create bridge instance
        bridge_ = std::make_unique<Bridge>(loop_);

        // Wire up bridge callbacks
        bridge_->on_message([this](const InboundMessage& msg) {
            route_message(msg);
        });
        bridge_->on_error([this](const Error& err) {
            handle_error(err);
        });
        bridge_->on_state_change([this](ConnectionState cs) {
            handle_connection_state(cs);
        });

        // Start listening
        auto result = bridge_->listen(config_.port);
        if (!result) return result;

        // Initialize heartbeat timer
        uv_timer_init(loop_, &heartbeat_timer_);
        heartbeat_timer_.data = this;

        set_state(RunnerState::Listening);
        return {};
    }

    /// Run the event loop (blocking until stopped)
    void run() {
        if (loop_ && owns_loop_) {
            uv_run(loop_, UV_RUN_DEFAULT);
        }
    }

    /// Stop the session runner gracefully
    void stop() {
        if (state_ == RunnerState::Stopped || state_ == RunnerState::Idle) return;
        set_state(RunnerState::Stopping);

        // Stop timers
        uv_timer_stop(&heartbeat_timer_);
        uv_timer_stop(&auth_timer_);
        uv_timer_stop(&reconnect_timer_);

        // Shutdown bridge
        if (bridge_) bridge_->shutdown();

        // Cleanup event loop if we own it
        if (owns_loop_ && loop_) {
            uv_stop(loop_);
            uv_loop_close(loop_);
            delete loop_;
            loop_ = nullptr;
        }

        set_state(RunnerState::Stopped);
    }

    /// Register a message route for a specific method prefix
    void add_route(std::string method_prefix,
                   std::function<void(const InboundMessage&)> handler) {
        routes_.push_back(MessageRoute{
            .method_prefix = std::move(method_prefix),
            .handler = std::move(handler),
        });
    }

    /// Send a response message to the connected IDE
    [[nodiscard]] VoidResult send(OutboundMessage msg) {
        if (!bridge_) {
            return std::unexpected(Error::make(
                ErrorCode::ConnectionFailed, "Bridge not initialized"));
        }
        return bridge_->send(std::move(msg));
    }

    // --- Callback registration ---
    void on_state_change(std::function<void(RunnerState)> cb) { on_state_change_ = std::move(cb); }
    void on_user_input(std::function<void(const std::string&)> cb) { on_user_input_ = std::move(cb); }
    void on_error(std::function<void(const Error&)> cb) { on_error_ = std::move(cb); }

    // --- Accessors ---
    [[nodiscard]] RunnerState state() const noexcept { return state_; }
    [[nodiscard]] std::uint16_t port() const { return bridge_ ? bridge_->port() : 0; }
    [[nodiscard]] const SessionConfig& config() const noexcept { return config_; }

private:
    /// Update runner state and notify listeners
    void set_state(RunnerState new_state) {
        state_ = new_state;
        if (on_state_change_) on_state_change_(new_state);
    }

    /// Route an inbound message to the appropriate handler
    void route_message(const InboundMessage& msg) {
        // The lightweight bridge currently stores the routing method in type.
        std::string method = msg.type;

        // Handle user input specially
        if (method == "user/input" && on_user_input_) {
            on_user_input_(msg.content);
        }

        // Route to registered handlers by method prefix
        for (const auto& route : routes_) {
            if (method.starts_with(route.method_prefix)) {
                route.handler(msg);
                return;
            }
        }
    }

    /// Handle connection state changes from bridge
    void handle_connection_state(ConnectionState cs) {
        switch (cs) {
            case ConnectionState::Connected:
                set_state(RunnerState::Running);
                reconnect_attempts_ = 0;
                current_reconnect_delay_ = config_.reconnect_delay_ms;
                start_heartbeat();
                break;

            case ConnectionState::Disconnected:
                if (state_ != RunnerState::Stopping) {
                    attempt_reconnect();
                }
                break;

            case ConnectionState::Authenticating:
                set_state(RunnerState::Authenticating);
                start_auth_timeout();
                break;

            default:
                break;
        }
    }

    /// Handle errors from bridge
    void handle_error(const Error& err) {
        if (on_error_) on_error_(err);
    }

    /// Start the heartbeat timer
    void start_heartbeat() {
        uv_timer_start(&heartbeat_timer_, on_heartbeat,
                       config_.heartbeat_interval_ms,
                       config_.heartbeat_interval_ms);
    }

    /// Start authentication timeout timer
    void start_auth_timeout() {
        uv_timer_init(loop_, &auth_timer_);
        auth_timer_.data = this;
        uv_timer_start(&auth_timer_, on_auth_timeout, config_.auth_timeout_ms, 0);
    }

    /// Attempt to reconnect with exponential backoff
    void attempt_reconnect() {
        if (reconnect_attempts_ >= config_.max_reconnect_attempts) {
            set_state(RunnerState::Stopped);
            handle_error(Error::make(ErrorCode::ConnectionFailed,
                "Max reconnection attempts exceeded"));
            return;
        }

        set_state(RunnerState::Reconnecting);
        ++reconnect_attempts_;

        uv_timer_init(loop_, &reconnect_timer_);
        reconnect_timer_.data = this;
        uv_timer_start(&reconnect_timer_, on_reconnect, current_reconnect_delay_, 0);

        // Exponential backoff with cap
        current_reconnect_delay_ = std::min(
            current_reconnect_delay_ * 2, config_.max_reconnect_delay_ms);
    }

    // --- Static timer callbacks ---
    static void on_heartbeat(uv_timer_t* handle) {
        auto* self = static_cast<SessionRunner*>(handle->data);
        if (self->bridge_ && self->bridge_->is_connected()) {
            // Send a progress update as heartbeat
            if (auto result = self->send(OutboundMessage{.type = "heartbeat", .content = "keepalive"}); !result) {
                self->handle_error(result.error());
            }
        }
    }

    static void on_auth_timeout(uv_timer_t* handle) {
        auto* self = static_cast<SessionRunner*>(handle->data);
        if (self->state_ == RunnerState::Authenticating) {
            self->handle_error(Error::make(
                ErrorCode::AuthenticationFailed, "Authentication timed out"));
            self->bridge_->shutdown();
        }
        uv_timer_stop(handle);
    }

    static void on_reconnect(uv_timer_t* handle) {
        auto* self = static_cast<SessionRunner*>(handle->data);
        uv_timer_stop(handle);

        // Restart bridge listener
        if (self->bridge_) {
            self->bridge_->shutdown();
            self->bridge_ = std::make_unique<Bridge>(self->loop_);
            self->bridge_->on_message([self](const InboundMessage& m) { self->route_message(m); });
            self->bridge_->on_error([self](const Error& e) { self->handle_error(e); });
            self->bridge_->on_state_change([self](ConnectionState c) { self->handle_connection_state(c); });
            auto r = self->bridge_->listen(self->config_.port);
            if (r) self->set_state(RunnerState::Listening);
        }
    }
};

} // namespace cc::bridge
