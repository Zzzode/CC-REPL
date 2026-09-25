// selectors_bridge.cpp - impl unit for cc.state.selectors
// (RFC 0001 Phase C batch 10). REPL bridge status selectors plus the
// composite connectivity / bridge-activity selectors.
module;

module cc.state.selectors;

import std;

import cc.state.app_state;

namespace cc::state::selectors {

// ============================================================
// Bridge State Selectors
// ============================================================

/// Check if the REPL bridge is enabled
[[nodiscard]] bool is_repl_bridge_enabled(const AppState& state) noexcept {
    return state.repl_bridge_enabled;
}

/// Check if the REPL bridge is connected
[[nodiscard]] bool is_repl_bridge_connected(const AppState& state) noexcept {
    return state.repl_bridge_connected;
}

/// Check if the REPL bridge session is active
[[nodiscard]] bool is_repl_bridge_session_active(const AppState& state) noexcept {
    return state.repl_bridge_session_active;
}

/// Check if the REPL bridge is reconnecting
[[nodiscard]] bool is_repl_bridge_reconnecting(const AppState& state) noexcept {
    return state.repl_bridge_reconnecting;
}

/// Check if the REPL bridge is in outbound-only mode
[[nodiscard]] bool is_repl_bridge_outbound_only(const AppState& state) noexcept {
    return state.repl_bridge_outbound_only;
}

/// Check if the REPL bridge is explicit
[[nodiscard]] bool is_repl_bridge_explicit(const AppState& state) noexcept {
    return state.repl_bridge_explicit;
}

/// Check if we should show the remote callout
[[nodiscard]] bool should_show_remote_callout(const AppState& state) noexcept {
    return state.show_remote_callout;
}

/// Get the REPL bridge connect URL
[[nodiscard]] std::optional<std::string_view> get_repl_bridge_connect_url(const AppState& state) noexcept {
    if (state.repl_bridge_connect_url) {
        return *state.repl_bridge_connect_url;
    }
    return std::nullopt;
}

/// Get the REPL bridge session URL
[[nodiscard]] std::optional<std::string_view> get_repl_bridge_session_url(const AppState& state) noexcept {
    if (state.repl_bridge_session_url) {
        return *state.repl_bridge_session_url;
    }
    return std::nullopt;
}

/// Get the REPL bridge error
[[nodiscard]] std::optional<std::string_view> get_repl_bridge_error(const AppState& state) noexcept {
    if (state.repl_bridge_error) {
        return *state.repl_bridge_error;
    }
    return std::nullopt;
}

// ============================================================
// Derived Selectors (Composite)
// ============================================================

/// Check if we're connected to anything (remote or bridge)
[[nodiscard]] bool is_connected_to_anything(const AppState& state) noexcept {
    return is_remote_connected(state) || is_repl_bridge_connected(state);
}

/// Check if any bridge-related status is active
[[nodiscard]] bool has_any_bridge_activity(const AppState& state) noexcept {
    return state.repl_bridge_enabled || 
           state.repl_bridge_connected || 
           state.repl_bridge_session_active ||
           state.repl_bridge_reconnecting;
}

} // namespace cc::state::selectors
