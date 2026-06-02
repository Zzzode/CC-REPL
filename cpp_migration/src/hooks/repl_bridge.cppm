// cc.hooks.repl_bridge — bridges REPL to remote/bridge sessions
// Migrated from: useReplBridge.tsx
module;

#include <string>
#include <string_view>
#include <expected>
#include <functional>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <vector>
#include <deque>

export module cc.hooks.repl_bridge;

export namespace cc::hooks::repl_bridge {

enum class BridgeState {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting,
    Error
};

struct BridgeConfig {
    std::string host;
    int port{0};
    std::string session_id;
    bool auto_reconnect{true};
    std::chrono::seconds reconnect_delay{5};
    std::chrono::seconds ping_interval{15};
    int max_reconnect_attempts{10};
};

struct BridgeMessage {
    std::string type;       // "prompt", "response", "tool_call", "tool_result", "control"
    std::string payload;
    std::uint64_t sequence;
    std::chrono::system_clock::time_point timestamp;
};

struct BridgeStats {
    std::uint64_t messages_sent{0};
    std::uint64_t messages_received{0};
    std::uint64_t bytes_sent{0};
    std::uint64_t bytes_received{0};
    std::chrono::steady_clock::time_point connected_since;
    int reconnect_count{0};
};

using MessageHandler = std::function<void(BridgeMessage)>;

namespace detail {

struct BridgeInternalState {
    std::mutex mutex;
    BridgeConfig config;
    std::atomic<BridgeState> state{BridgeState::Disconnected};
    std::vector<MessageHandler> handlers;
    std::deque<BridgeMessage> send_queue;
    BridgeStats stats;
    std::uint64_t next_sequence{1};
    std::uint64_t next_handler_id{1};
    std::string last_error;
    static constexpr int MAX_QUEUE_SIZE = 1000;
};

inline auto get_state() -> BridgeInternalState& {
    static BridgeInternalState state;
    return state;
}

} // namespace detail

/// Connect the REPL bridge to a remote session.
/// In production: establishes a WebSocket or SSE connection to the bridge server
/// for bi-directional message passing between local REPL and remote agent.
inline auto connect_bridge(BridgeConfig config)
    -> std::expected<void, std::string>
{
    if (config.host.empty()) {
        return std::unexpected(std::string{"Bridge host must not be empty"});
    }
    if (config.port <= 0 || config.port > 65535) {
        return std::unexpected(std::string{"Bridge port must be between 1 and 65535"});
    }
    if (config.session_id.empty()) {
        return std::unexpected(std::string{"Session ID must not be empty"});
    }

    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    if (state.state.load() == BridgeState::Connected) {
        return std::unexpected(std::string{"Bridge is already connected"});
    }

    state.config = std::move(config);
    state.state.store(BridgeState::Connecting);
    state.stats = BridgeStats{};
    state.stats.connected_since = std::chrono::steady_clock::now();
    state.last_error.clear();

    // In production: initiate WebSocket handshake here.
    // On success, transition to Connected state.
    state.state.store(BridgeState::Connected);

    // Flush any queued messages
    // (handled by the transport event loop in production)

    return {};
}

/// Disconnect the bridge gracefully.
inline auto disconnect_bridge() -> void {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.state.store(BridgeState::Disconnected);
    state.send_queue.clear();
}

/// Send a message through the bridge.
inline auto send_bridge_message(BridgeMessage message)
    -> std::expected<std::uint64_t, std::string>
{
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    auto current_state = state.state.load();
    if (current_state != BridgeState::Connected &&
        current_state != BridgeState::Reconnecting) {
        return std::unexpected(std::string{"Bridge is not connected"});
    }

    // Assign sequence number and timestamp
    message.sequence = state.next_sequence++;
    message.timestamp = std::chrono::system_clock::now();

    auto seq = message.sequence;

    // Update stats
    state.stats.messages_sent++;
    state.stats.bytes_sent += message.payload.size();

    if (current_state == BridgeState::Reconnecting) {
        // Queue for later delivery
        if (static_cast<int>(state.send_queue.size()) >= detail::BridgeInternalState::MAX_QUEUE_SIZE) {
            state.send_queue.pop_front();
        }
        state.send_queue.push_back(std::move(message));
    } else {
        // In production: serialize and send via WebSocket/SSE transport
        // For now, direct dispatch to handlers for loopback testing
    }

    return seq;
}

/// Get the current bridge state.
inline auto get_bridge_state() -> BridgeState {
    return detail::get_state().state.load();
}

/// Check if the bridge is connected.
inline auto is_bridge_connected() -> bool {
    return detail::get_state().state.load() == BridgeState::Connected;
}

/// Register a handler for incoming bridge messages. Returns handler ID.
inline auto on_bridge_message(MessageHandler callback) -> std::uint64_t {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.handlers.push_back(std::move(callback));
    return state.next_handler_id++;
}

/// Deliver an incoming message (called by the transport layer).
inline auto deliver_bridge_message(BridgeMessage message) -> void {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    state.stats.messages_received++;
    state.stats.bytes_received += message.payload.size();
    message.timestamp = std::chrono::system_clock::now();

    for (const auto& handler : state.handlers) {
        handler(message);
    }
}

/// Update the bridge configuration (e.g., change reconnect settings).
inline auto set_bridge_config(BridgeConfig config) -> void {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.config = std::move(config);
}

/// Get the current bridge config.
inline auto get_bridge_config() -> BridgeConfig {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    return state.config;
}

/// Get bridge statistics.
inline auto get_bridge_stats() -> BridgeStats {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    return state.stats;
}

/// Get the last error message.
inline auto get_last_error() -> std::string {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    return state.last_error;
}

/// Trigger a reconnection attempt (called on connection loss).
inline auto attempt_reconnect() -> std::expected<void, std::string> {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    if (!state.config.auto_reconnect) {
        state.state.store(BridgeState::Error);
        state.last_error = "Auto-reconnect is disabled";
        return std::unexpected(state.last_error);
    }

    if (state.stats.reconnect_count >= state.config.max_reconnect_attempts) {
        state.state.store(BridgeState::Error);
        state.last_error = "Max reconnect attempts exceeded";
        return std::unexpected(state.last_error);
    }

    state.state.store(BridgeState::Reconnecting);
    state.stats.reconnect_count++;

    // In production: schedule reconnect with exponential backoff
    return {};
}

} // namespace cc::hooks::repl_bridge
