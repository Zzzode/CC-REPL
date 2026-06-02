/// @file core.cppm
/// @brief Remote bridge core and poll configuration
module;

#include <string>
#include <chrono>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <functional>
#include <utility>

export module cc.bridge.core;

import cc.types.types;
import cc.bridge.config;
import cc.bridge.transport;
import cc.bridge.security;
import cc.bridge.init;
import cc.bridge.ui;

export namespace cc::bridge {

/// Poll interval configuration (pollConfigDefaults.ts)
struct PollIntervalConfig {
    std::chrono::milliseconds normal{1000};
    std::chrono::milliseconds idle{5000};
    std::chrono::milliseconds reconnect_min{500};
    std::chrono::milliseconds reconnect_max{30000};
    std::chrono::milliseconds heartbeat{30000};
};

/// Get default poll config
PollIntervalConfig get_poll_config_defaults() {
    return PollIntervalConfig{};
}

/// Remote bridge core (remoteBridgeCore.ts)
class RemoteBridgeCore {
    std::unique_ptr<BridgeTransport> transport_;
    std::unique_ptr<BridgeLogger> logger_;
    BridgeConfig config_;
    BridgeState state_ = BridgeState::Idle;
    std::string environment_id_;
    std::string session_id_;
    std::string title_;
    
public:
    RemoteBridgeCore(BridgeConfig config, 
                     std::unique_ptr<BridgeLogger> logger = nullptr)
        : logger_(logger ? std::move(logger) : std::make_unique<BridgeLogger>()),
          config_(std::move(config)) {}
    
    /// Connect to bridge
    Result<void> connect() {
        state_ = BridgeState::Connecting;
        environment_id_ = std::format("env_{}", std::hash<std::string>{}(
            std::format("{}:{}{}", config_.host, config_.port, config_.path)));
        session_id_ = SessionIdCompat::generate();

        switch (config_.transport) {
            case TransportType::websocket:
                transport_ = std::make_unique<WebSocketTransport>();
                break;
            case TransportType::http_polling:
                transport_ = std::make_unique<HttpPollingTransport>();
                break;
            case TransportType::stdio:
                transport_ = std::make_unique<StdioTransport>();
                break;
        }

        const auto endpoint = std::format("{}://{}:{}{}",
            config_.transport == TransportType::websocket ? "ws" : "http",
            config_.host,
            config_.port,
            config_.path);
        auto token = config_.auth_token
            ? std::optional<std::string_view>{*config_.auth_token}
            : std::nullopt;
        auto connected = transport_->connect(endpoint, token);
        if (!connected) {
            state_ = BridgeState::Failed;
            return std::unexpected(Error::make(ErrorCode::ConnectionFailed, connected.error().message));
        }

        state_ = BridgeState::Connected;
        if (logger_) logger_->print_banner(config_, environment_id_);
        return {};
    }
    
    /// Disconnect from bridge
    void disconnect() {
        if (transport_) transport_->disconnect();
        state_ = BridgeState::Closed;
    }
    
    /// Get current state
    BridgeState state() const { return state_; }
    
    /// Get environment ID
    std::string_view environment_id() const { return environment_id_; }
    
    /// Get session ID
    std::string_view session_id() const { return session_id_; }
    
    /// Send a message
    Result<void> send(const BridgeMessage& msg) {
        if (!transport_ || !transport_->is_connected()) {
            return std::unexpected(Error::make(ErrorCode::ConnectionFailed, "Bridge transport is not connected"));
        }
        auto outbound = msg;
        outbound.timestamp = std::chrono::system_clock::now();
        auto sent = transport_->send(std::move(outbound));
        if (!sent) {
            return std::unexpected(Error::make(ErrorCode::ConnectionFailed, sent.error().message));
        }
        return {};
    }
    
    /// Set title
    void set_title(std::string_view title) {
        title_ = std::string(title);
        if (transport_ && transport_->is_connected()) {
            BridgeMessage msg{
                .id = SessionIdCompat::generate(),
                .type = "event",
                .method = "session/title",
                .payload = std::format(R"({{"title":"{}"}})", title_),
                .priority = MessagePriority::normal,
                .timestamp = std::chrono::system_clock::now(),
                .correlation_id = std::nullopt,
            };
            (void)transport_->send(std::move(msg));
        }
    }
};

} // namespace cc::bridge
