// selectors_companion_mcp.cpp - impl unit for cc.state.selectors
// (RFC 0001 Phase C batch 10). Companion (buddy) reaction/pet selectors —
// was_companion_petted_recently is the only clock-reading selector — and the
// MCP client/tool/command + plugin selectors.
module;

#include <cstdint>
#include <cstddef>

module cc.state.selectors;

import std;

import cc.state.app_state;

namespace cc::state::selectors {

// ============================================================
// Companion (Buddy) Selectors
// ============================================================

/// Get the companion reaction
[[nodiscard]] std::optional<std::string_view> get_companion_reaction(const AppState& state) noexcept {
    if (state.companion_reaction) {
        return *state.companion_reaction;
    }
    return std::nullopt;
}

/// Get the companion pet time
[[nodiscard]] std::optional<std::chrono::system_clock::time_point> get_companion_pet_time(const AppState& state) noexcept {
    return state.companion_pet_at;
}

/// Check if companion was petted recently (within last 5 seconds)
[[nodiscard]] bool was_companion_petted_recently(const AppState& state) noexcept {
    if (!state.companion_pet_at) {
        return false;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now() - *state.companion_pet_at
    );
    return elapsed.count() < 5;
}

// ============================================================
// MCP & Plugins Selectors
// ============================================================

/// Get the number of MCP clients
[[nodiscard]] size_t get_mcp_client_count(const AppState& state) noexcept {
    return state.mcp.clients.size();
}

/// Check if there are any MCP clients
[[nodiscard]] bool has_mcp_clients(const AppState& state) noexcept {
    return !state.mcp.clients.empty();
}

/// Get the number of MCP tools
[[nodiscard]] size_t get_mcp_tool_count(const AppState& state) noexcept {
    return state.mcp.tools.size();
}

/// Check if there are any MCP tools
[[nodiscard]] bool has_mcp_tools(const AppState& state) noexcept {
    return !state.mcp.tools.empty();
}

/// Get the number of MCP commands
[[nodiscard]] size_t get_mcp_command_count(const AppState& state) noexcept {
    return state.mcp.commands.size();
}

/// Check if there are any MCP commands
[[nodiscard]] bool has_mcp_commands(const AppState& state) noexcept {
    return !state.mcp.commands.empty();
}

/// Get the MCP plugin reconnect key
[[nodiscard]] uint32_t get_mcp_plugin_reconnect_key(const AppState& state) noexcept {
    return state.mcp.plugin_reconnect_key;
}

/// Get the number of enabled plugins
[[nodiscard]] size_t get_enabled_plugin_count(const AppState& state) noexcept {
    return state.plugins.enabled.size();
}

/// Get the number of disabled plugins
[[nodiscard]] size_t get_disabled_plugin_count(const AppState& state) noexcept {
    return state.plugins.disabled.size();
}

/// Get the total number of plugins
[[nodiscard]] size_t get_total_plugin_count(const AppState& state) noexcept {
    return state.plugins.enabled.size() + state.plugins.disabled.size();
}

/// Check if there are any plugin errors
[[nodiscard]] bool has_plugin_errors(const AppState& state) noexcept {
    return !state.plugins.errors.empty();
}

/// Get the number of plugin errors
[[nodiscard]] size_t get_plugin_error_count(const AppState& state) noexcept {
    return state.plugins.errors.size();
}

/// Check if plugins need refresh
[[nodiscard]] bool do_plugins_need_refresh(const AppState& state) noexcept {
    return state.plugins.needs_refresh;
}

} // namespace cc::state::selectors
