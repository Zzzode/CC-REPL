// cc.hooks.direct_connect — manages direct API connections
// Migrated from: useDirectConnect.ts
module;

#include <string>
#include <string_view>
#include <expected>
#include <chrono>
#include <optional>
#include <mutex>
#include <atomic>

export module cc.hooks.direct_connect;

export namespace cc::hooks::direct_connect {

enum class DirectConnectState {
    Disconnected,
    Connecting,
    Connected,
    Error
};

struct ConnectionInfo {
    std::string endpoint;
    std::string api_key;
    std::chrono::steady_clock::time_point connected_at;
    DirectConnectState state;
    std::string error_message;
    int reconnect_attempts{0};
};

struct ConnectionConfig {
    std::string endpoint;
    std::string api_key;
    std::chrono::seconds connect_timeout{10};
    std::chrono::seconds keep_alive_interval{30};
    int max_reconnect_attempts{5};
    bool verify_tls{true};
};

namespace detail {

struct ConnectionState {
    std::mutex mutex;
    ConnectionInfo info;
    ConnectionConfig config;
    std::atomic<DirectConnectState> state{DirectConnectState::Disconnected};
};

inline auto get_state() -> ConnectionState& {
    static ConnectionState state;
    return state;
}

/// Validate the endpoint URL format.
inline auto validate_endpoint(std::string_view endpoint) -> std::expected<void, std::string> {
    if (endpoint.empty()) {
        return std::unexpected(std::string{"endpoint must not be empty"});
    }
    if (!endpoint.starts_with("https://") && !endpoint.starts_with("http://")) {
        return std::unexpected(std::string{"endpoint must start with http:// or https://"});
    }
    // Check for basic URL validity (has a host component)
    auto scheme_end = endpoint.find("://");
    auto host_start = scheme_end + 3;
    if (host_start >= endpoint.size()) {
        return std::unexpected(std::string{"endpoint URL has no host"});
    }
    return {};
}

/// Validate the API key format.
inline auto validate_api_key(std::string_view api_key) -> std::expected<void, std::string> {
    if (api_key.empty()) {
        return std::unexpected(std::string{"api_key must not be empty"});
    }
    if (api_key.size() < 10) {
        return std::unexpected(std::string{"api_key appears too short (minimum 10 characters)"});
    }
    // Check for obviously invalid characters
    for (char ch : api_key) {
        if (ch < 0x20 || ch > 0x7E) {
            return std::unexpected(std::string{"api_key contains invalid characters"});
        }
    }
    return {};
}

} // namespace detail

/// Establish a direct connection to the given API endpoint.
/// In production: opens an HTTP/2 or WebSocket persistent connection
/// to the Claude API endpoint for streaming responses.
inline auto establish_direct_connection(std::string_view endpoint, std::string_view api_key)
    -> std::expected<ConnectionInfo, std::string>
{
    auto ep_valid = detail::validate_endpoint(endpoint);
    if (!ep_valid) return std::unexpected(ep_valid.error());

    auto key_valid = detail::validate_api_key(api_key);
    if (!key_valid) return std::unexpected(key_valid.error());

    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    // If already connected to the same endpoint, return current info
    if (state.state.load() == DirectConnectState::Connected &&
        state.info.endpoint == endpoint) {
        return state.info;
    }

    // Transition to connecting state
    state.state.store(DirectConnectState::Connecting);
    state.config.endpoint = std::string{endpoint};
    state.config.api_key = std::string{api_key};

    // In production: initiate async TCP/TLS handshake here.
    // For the migration, we set up the connection info and mark connected.
    // The actual I/O will be wired through the transport layer (sse_transport / websocket_transport).

    state.info = ConnectionInfo{
        .endpoint = std::string{endpoint},
        .api_key = std::string{api_key},
        .connected_at = std::chrono::steady_clock::now(),
        .state = DirectConnectState::Connected,
        .error_message = {},
        .reconnect_attempts = 0
    };
    state.state.store(DirectConnectState::Connected);

    return state.info;
}

/// Establish connection using a full config struct.
inline auto establish_direct_connection(ConnectionConfig config)
    -> std::expected<ConnectionInfo, std::string>
{
    auto result = establish_direct_connection(config.endpoint, config.api_key);
    if (result) {
        auto& state = detail::get_state();
        std::lock_guard lock(state.mutex);
        state.config = std::move(config);
    }
    return result;
}

/// Disconnect and clean up the direct connection.
inline auto disconnect() -> void {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.state.store(DirectConnectState::Disconnected);
    state.info.state = DirectConnectState::Disconnected;
    state.info.error_message.clear();
    state.info.reconnect_attempts = 0;
}

/// Get the current connection state.
inline auto get_connection_state() -> DirectConnectState {
    return detail::get_state().state.load();
}

/// Check if currently connected.
inline auto is_direct_connected() -> bool {
    return get_connection_state() == DirectConnectState::Connected;
}

/// Get full connection info (thread-safe copy).
inline auto get_connection_info() -> ConnectionInfo {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    return state.info;
}

/// Get connection uptime duration.
inline auto get_connection_uptime() -> std::chrono::steady_clock::duration {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    if (state.info.state != DirectConnectState::Connected) {
        return std::chrono::steady_clock::duration::zero();
    }
    return std::chrono::steady_clock::now() - state.info.connected_at;
}

/// Attempt reconnection (called internally on connection failures).
inline auto attempt_reconnect() -> std::expected<void, std::string> {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    if (state.info.reconnect_attempts >= state.config.max_reconnect_attempts) {
        state.state.store(DirectConnectState::Error);
        state.info.state = DirectConnectState::Error;
        state.info.error_message = "Max reconnect attempts exceeded";
        return std::unexpected(state.info.error_message);
    }

    state.info.reconnect_attempts++;
    state.state.store(DirectConnectState::Connecting);
    state.info.state = DirectConnectState::Connecting;

    // In production: schedule reconnect with exponential backoff
    // The actual reconnection is handled by the transport layer event loop.
    return {};
}

} // namespace cc::hooks::direct_connect
