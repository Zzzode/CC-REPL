/// @file init.cppm
/// @brief REPL bridge initialization and core
module;

#include <string>
#include <functional>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

export module cc.bridge.init;

import cc.types.types;
import cc.bridge.config;
import cc.bridge.security;
import cc.bridge.ui;
import cc.bridge.transport;
import cc.bridge.messages;

export namespace cc::bridge {

using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::Result;

/// Bridge state
enum class BridgeState {
    Idle,
    Connecting,
    Connected,
    Reconnecting,
    Failed,
    Closed
};

/// Init bridge options
struct InitBridgeOptions {
    std::function<void(const SDKMessage&)> on_inbound_message;
    std::function<void()> on_interrupt;
    std::function<void(BridgeState, std::optional<std::string>)> on_state_change;
    std::optional<std::string> initial_name;
    std::vector<std::string> tags;
    bool perpetual = false;
    bool outbound_only = false;
};

/// Bridge handle (for controlling the bridge after init)
class ReplBridgeHandle {
public:
    virtual ~ReplBridgeHandle() = default;
    
    /// Get current state
    virtual BridgeState state() const = 0;
    
    /// Close the bridge
    virtual void close() = 0;
    
    /// Flush messages
    virtual void flush() = 0;
    
    /// Update title
    virtual void set_title(std::string_view title) = 0;
    
    /// Get environment ID
    virtual std::string environment_id() const = 0;
};

Result<std::unique_ptr<ReplBridgeHandle>> init_bridge_core(
    const BridgeConfig& config,
    const InitBridgeOptions& options
);

/// Initialize REPL bridge
Result<std::unique_ptr<ReplBridgeHandle>> init_repl_bridge(
    const BridgeConfig& config,
    const InitBridgeOptions& options = {}
) {
    return init_bridge_core(config, options);
}

/// Initialize bridge core
Result<std::unique_ptr<ReplBridgeHandle>> init_bridge_core(
    const BridgeConfig& config,
    const InitBridgeOptions& options = {}
) {
    class BasicReplBridgeHandle final : public ReplBridgeHandle {
        BridgeConfig config_;
        InitBridgeOptions options_;
        BridgeState state_{BridgeState::Idle};
        std::string environment_id_;
        std::string title_;

        void set_state(BridgeState next, std::optional<std::string> reason = std::nullopt) {
            state_ = next;
            if (options_.on_state_change) options_.on_state_change(next, std::move(reason));
        }

    public:
        BasicReplBridgeHandle(BridgeConfig config, InitBridgeOptions options)
            : config_(std::move(config)), options_(std::move(options)) {
            environment_id_ = std::format("env_{}", std::hash<std::string>{}(
                std::format("{}:{}{}", config_.host, config_.port, config_.path)));
            if (options_.initial_name) title_ = *options_.initial_name;
            set_state(config_.auto_connect ? BridgeState::Connected : BridgeState::Idle);
        }

        BridgeState state() const override { return state_; }

        void close() override { set_state(BridgeState::Closed); }

        void flush() override {
            if (state_ == BridgeState::Idle && config_.auto_connect) {
                set_state(BridgeState::Connected);
            }
        }

        void set_title(std::string_view title) override { title_ = std::string(title); }

        std::string environment_id() const override { return environment_id_; }
    };

    if (config.host.empty()) {
        return std::unexpected(Error::make(ErrorCode::InvalidInput, "Bridge host cannot be empty"));
    }
    return std::make_unique<BasicReplBridgeHandle>(config, options);
}

} // namespace cc::bridge
