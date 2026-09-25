// selectors_core.cpp - impl unit for cc.state.selectors
// (RFC 0001 Phase C batch 10). Basic AppState flag/enum selectors and the
// settings / notification / overlay / effort / Ultraplan selectors.
module;

#include <cstdint>
#include <cstddef>

module cc.state.selectors;

import std;

import cc.state.app_state;

namespace cc::state::selectors {

// ============================================================
// Basic Selectors
// ============================================================

/// Check if verbose mode is enabled
[[nodiscard]] bool is_verbose(const AppState& state) noexcept {
    return state.verbose;
}

/// Check if compact mode is enabled
[[nodiscard]] bool is_compact_mode(const AppState& state) noexcept {
    return state.compact_mode;
}

/// Check if fast mode is enabled
[[nodiscard]] bool is_fast_mode(const AppState& state) noexcept {
    return state.fast_mode;
}

/// Check if thinking is enabled
[[nodiscard]] bool is_thinking_enabled(const AppState& state) noexcept {
    return state.thinking_enabled;
}

/// Check if prompt suggestions are enabled
[[nodiscard]] bool is_prompt_suggestion_enabled(const AppState& state) noexcept {
    return state.prompt_suggestion_enabled;
}

/// Check if Kairos is enabled
[[nodiscard]] bool is_kairos_enabled(const AppState& state) noexcept {
    return state.kairos_enabled;
}

/// Check if Ultraplan mode is active
[[nodiscard]] bool is_ultraplan_mode(const AppState& state) noexcept {
    return state.is_ultraplan_mode;
}

/// Check if Ultraplan is launching
[[nodiscard]] bool is_ultraplan_launching(const AppState& state) noexcept {
    return state.ultraplan_launching;
}

/// Get the main loop model
[[nodiscard]] std::optional<std::string_view> get_main_loop_model(const AppState& state) noexcept {
    if (state.main_loop_model) {
        return *state.main_loop_model;
    }
    return std::nullopt;
}

/// Get the current agent name
[[nodiscard]] std::optional<std::string_view> get_agent(const AppState& state) noexcept {
    if (state.agent) {
        return *state.agent;
    }
    return std::nullopt;
}

/// Get the current expanded view
[[nodiscard]] ExpandedView get_expanded_view(const AppState& state) noexcept {
    return state.expanded_view;
}

/// Check if tasks view is expanded
[[nodiscard]] bool is_tasks_view_expanded(const AppState& state) noexcept {
    return state.expanded_view == ExpandedView::Tasks;
}

/// Check if teammates view is expanded
[[nodiscard]] bool is_teammates_view_expanded(const AppState& state) noexcept {
    return state.expanded_view == ExpandedView::Teammates;
}

/// Get the current remote connection status
[[nodiscard]] RemoteConnectionStatus get_remote_connection_status(const AppState& state) noexcept {
    return state.remote_connection_status;
}

/// Check if connected to remote
[[nodiscard]] bool is_remote_connected(const AppState& state) noexcept {
    return state.remote_connection_status == RemoteConnectionStatus::Connected;
}

/// Check if reconnecting to remote
[[nodiscard]] bool is_remote_reconnecting(const AppState& state) noexcept {
    return state.remote_connection_status == RemoteConnectionStatus::Reconnecting;
}

/// Get the current permission mode
[[nodiscard]] PermissionMode get_permission_mode(const AppState& state) noexcept {
    return state.tool_permission_context.mode;
}

/// Get the status line text
[[nodiscard]] std::optional<std::string_view> get_status_line_text(const AppState& state) noexcept {
    if (state.status_line_text) {
        return *state.status_line_text;
    }
    return std::nullopt;
}

// ============================================================
// Settings Selectors
// ============================================================

/// Get the configured model from settings
[[nodiscard]] std::string_view get_settings_model(const AppState& state) noexcept {
    return state.settings.model;
}

/// Get the configured theme from settings
[[nodiscard]] std::string_view get_settings_theme(const AppState& state) noexcept {
    return state.settings.theme;
}

/// Check if status line is enabled in settings
[[nodiscard]] bool is_status_line_enabled(const AppState& state) noexcept {
    return state.settings.status_line.enabled;
}

/// Get the status line command from settings
[[nodiscard]] std::string_view get_status_line_command(const AppState& state) noexcept {
    return state.settings.status_line.command;
}

/// Get the status line padding from settings
[[nodiscard]] int get_status_line_padding(const AppState& state) noexcept {
    return state.settings.status_line.padding;
}

/// Get the output style from settings
[[nodiscard]] std::string_view get_output_style(const AppState& state) noexcept {
    return state.settings.output_style;
}

/// Check if all hooks are disabled in settings
[[nodiscard]] bool are_all_hooks_disabled(const AppState& state) noexcept {
    return state.settings.disable_all_hooks;
}

/// Get the number of notifications
[[nodiscard]] size_t get_notification_count(const AppState& state) noexcept {
    return state.notifications.size();
}

/// Check if there are any notifications
[[nodiscard]] bool has_notifications(const AppState& state) noexcept {
    return !state.notifications.empty();
}

/// Get the active overlays
[[nodiscard]] const std::set<std::string>& get_active_overlays(const AppState& state) noexcept {
    return state.active_overlays;
}

/// Check if there are any active overlays
[[nodiscard]] bool has_active_overlays(const AppState& state) noexcept {
    return !state.active_overlays.empty();
}

/// Check if a specific overlay is active
[[nodiscard]] bool is_overlay_active(const AppState& state, std::string_view overlay_name) noexcept {
    return state.active_overlays.contains(std::string(overlay_name));
}

/// Get the auth version
[[nodiscard]] uint32_t get_auth_version(const AppState& state) noexcept {
    return state.auth_version;
}

/// Get the effort value
[[nodiscard]] std::optional<std::string_view> get_effort_value(const AppState& state) noexcept {
    if (state.effort_value) {
        return *state.effort_value;
    }
    return std::nullopt;
}

/// Get the advisor model
[[nodiscard]] std::optional<std::string_view> get_advisor_model(const AppState& state) noexcept {
    if (state.advisor_model) {
        return *state.advisor_model;
    }
    return std::nullopt;
}

/// Get the Ultraplan session URL
[[nodiscard]] std::optional<std::string_view> get_ultraplan_session_url(const AppState& state) noexcept {
    if (state.ultraplan_session_url) {
        return *state.ultraplan_session_url;
    }
    return std::nullopt;
}

/// Get the speculation session time saved
[[nodiscard]] int64_t get_speculation_session_time_saved_ms(const AppState& state) noexcept {
    return state.speculation_session_time_saved_ms;
}

} // namespace cc::state::selectors
