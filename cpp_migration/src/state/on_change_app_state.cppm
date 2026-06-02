/// @file on_change_app_state.cppm
/// @brief State change callback system for the Claude Code REPL.
/// Provides callbacks that react to specific state changes and trigger
/// appropriate side effects like persistence, notifications, etc.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <algorithm>

export module cc.state.on_change;

import cc.state.app_state;
import cc.state.selectors;
import cc.state.persistence;

export namespace cc::state::on_change {

// ============================================================
// Change Detection Helpers
// ============================================================

/// Check if a value has changed between two states
template <typename Getter>
[[nodiscard]] inline bool has_changed(
    const AppState& prev,
    const AppState& next,
    Getter get_value
) noexcept {
    return get_value(prev) != get_value(next);
}

/// Check if an optional value has changed
template <typename Getter>
[[nodiscard]] inline bool optional_has_changed(
    const AppState& prev,
    const AppState& next,
    Getter get_value
) noexcept {
    const auto& prev_val = get_value(prev);
    const auto& next_val = get_value(next);
    if (prev_val.has_value() != next_val.has_value()) {
        return true;
    }
    if (prev_val.has_value() && next_val.has_value()) {
        return *prev_val != *next_val;
    }
    return false;
}

// ============================================================
// State Change Callback Type
// ============================================================

/// Type for state change callbacks
using StateChangeCallback = std::function<void(const AppState& prev, const AppState& next)>;

/// Type for conditional state change callbacks
using ConditionalCallback = std::pair<
    std::function<bool(const AppState& prev, const AppState& next)>,
    StateChangeCallback
>;

// ============================================================
// State Change Handler Registry
// ============================================================

/// Registry for state change handlers
class StateChangeRegistry {
    std::vector<ConditionalCallback> callbacks_;
    mutable std::shared_mutex mutex_;

public:
    /// Register a callback that always runs on any state change
    void register_callback(StateChangeCallback callback) {
        std::unique_lock lock(mutex_);
        callbacks_.push_back({
            [](const AppState&, const AppState&) { return true; },
            std::move(callback)
        });
    }

    /// Register a conditional callback that runs only when the condition is true
    void register_conditional_callback(
        std::function<bool(const AppState&, const AppState&)> condition,
        StateChangeCallback callback
    ) {
        std::unique_lock lock(mutex_);
        callbacks_.push_back({std::move(condition), std::move(callback)});
    }

    /// Clear all registered callbacks
    void clear_callbacks() {
        std::unique_lock lock(mutex_);
        callbacks_.clear();
    }

    /// Run all applicable callbacks for a state change
    void run_callbacks(const AppState& prev, const AppState& next) {
        std::vector<ConditionalCallback> callbacks_copy;
        {
            std::shared_lock lock(mutex_);
            callbacks_copy = callbacks_;
        }
        for (const auto& [condition, callback] : callbacks_copy) {
            if (condition(prev, next)) {
                callback(prev, next);
            }
        }
    }
};

// ============================================================
// Predefined Change Detectors
// ============================================================

/// Check if verbose mode has changed
[[nodiscard]] inline bool verbose_changed(const AppState& prev, const AppState& next) noexcept {
    return has_changed(prev, next, &selectors::is_verbose);
}

/// Check if compact mode has changed
[[nodiscard]] inline bool compact_mode_changed(const AppState& prev, const AppState& next) noexcept {
    return has_changed(prev, next, &selectors::is_compact_mode);
}

/// Check if fast mode has changed
[[nodiscard]] inline bool fast_mode_changed(const AppState& prev, const AppState& next) noexcept {
    return has_changed(prev, next, &selectors::is_fast_mode);
}

/// Check if expanded view has changed
[[nodiscard]] inline bool expanded_view_changed(const AppState& prev, const AppState& next) noexcept {
    return has_changed(prev, next, &selectors::get_expanded_view);
}

/// Check if permission mode has changed
[[nodiscard]] inline bool permission_mode_changed(const AppState& prev, const AppState& next) noexcept {
    return has_changed(prev, next, &selectors::get_permission_mode);
}

/// Check if remote connection status has changed
[[nodiscard]] inline bool remote_connection_changed(const AppState& prev, const AppState& next) noexcept {
    return has_changed(prev, next, &selectors::get_remote_connection_status);
}

/// Check if repl bridge enabled has changed
[[nodiscard]] inline bool repl_bridge_enabled_changed(const AppState& prev, const AppState& next) noexcept {
    return has_changed(prev, next, &selectors::is_repl_bridge_enabled);
}

/// Check if repl bridge connected has changed
[[nodiscard]] inline bool repl_bridge_connected_changed(const AppState& prev, const AppState& next) noexcept {
    return has_changed(prev, next, &selectors::is_repl_bridge_connected);
}

/// Check if main loop model has changed
[[nodiscard]] inline bool main_loop_model_changed(const AppState& prev, const AppState& next) noexcept {
    return optional_has_changed(prev, next, &selectors::get_main_loop_model);
}

/// Check if messages have changed
[[nodiscard]] inline bool messages_changed(const AppState& prev, const AppState& next) noexcept {
    return selectors::get_message_count(prev) != selectors::get_message_count(next);
}

/// Check if tasks have changed
[[nodiscard]] inline bool tasks_changed(const AppState& prev, const AppState& next) noexcept {
    return selectors::get_task_count(prev) != selectors::get_task_count(next);
}

/// Check if token usage has changed
[[nodiscard]] inline bool usage_changed(const AppState& prev, const AppState& next) noexcept {
    return prev.total_usage.total() != next.total_usage.total();
}

/// Check if auth version has changed
[[nodiscard]] inline bool auth_version_changed(const AppState& prev, const AppState& next) noexcept {
    return has_changed(prev, next, &selectors::get_auth_version);
}

/// Check if ultraplan mode has changed
[[nodiscard]] inline bool ultraplan_mode_changed(const AppState& prev, const AppState& next) noexcept {
    return has_changed(prev, next, &selectors::is_ultraplan_mode);
}

/// Check if mcp plugin reconnect key has changed
[[nodiscard]] inline bool mcp_reconnect_key_changed(const AppState& prev, const AppState& next) noexcept {
    return has_changed(prev, next, &selectors::get_mcp_plugin_reconnect_key);
}

// ============================================================
// Common Change Handlers
// ============================================================

/// Handler for permission mode changes
inline void on_permission_mode_changed(const AppState& prev, const AppState& next) {
    // In a real implementation, this would:
    // 1. Notify external listeners (like the CCR bridge)
    // 2. Update any UI components that depend on permission mode
    // 3. Log the change if verbose mode is enabled
    if (next.verbose) {
        // Log the change
    }
}

/// Handler for main loop model changes
inline void on_main_loop_model_changed(const AppState& prev, const AppState& next) {
    // Update settings, notify model manager, etc.
}

/// Handler for verbose mode changes
inline void on_verbose_mode_changed(const AppState& prev, const AppState& next) {
    // Update logging configuration
}

/// Handler for expanded view changes
inline void on_expanded_view_changed(const AppState& prev, const AppState& next) {
    // This would persist the expanded view setting to config
    // Similar to how TypeScript version updates global config
}

/// Handler for messages changes
inline void on_messages_changed(const AppState& prev, const AppState& next) {
    // Trigger UI update, scroll to bottom, etc.
}

/// Handler for tasks changes
inline void on_tasks_changed(const AppState& prev, const AppState& next) {
    // Update task list UI, notify task manager, etc.
}

/// Handler for auth version changes
inline void on_auth_version_changed(const AppState& prev, const AppState& next) {
    // Clear caches, re-authenticate if needed, etc.
}

/// Handler for MCP reconnect key changes
inline void on_mcp_reconnect_key_changed(const AppState& prev, const AppState& next) {
    // Trigger MCP reconnection
}

// ============================================================
// Default Handler Setup
// ============================================================

/// Set up default handlers in a registry
inline void setup_default_handlers(StateChangeRegistry& registry) {
    // Permission mode changes
    registry.register_conditional_callback(
        &permission_mode_changed,
        &on_permission_mode_changed
    );

    // Main loop model changes
    registry.register_conditional_callback(
        &main_loop_model_changed,
        &on_main_loop_model_changed
    );

    // Verbose mode changes
    registry.register_conditional_callback(
        &verbose_changed,
        &on_verbose_mode_changed
    );

    // Expanded view changes
    registry.register_conditional_callback(
        &expanded_view_changed,
        &on_expanded_view_changed
    );

    // Messages changes
    registry.register_conditional_callback(
        &messages_changed,
        &on_messages_changed
    );

    // Tasks changes
    registry.register_conditional_callback(
        &tasks_changed,
        &on_tasks_changed
    );

    // Auth version changes
    registry.register_conditional_callback(
        &auth_version_changed,
        &on_auth_version_changed
    );

    // MCP reconnect key changes
    registry.register_conditional_callback(
        &mcp_reconnect_key_changed,
        &on_mcp_reconnect_key_changed
    );
}

// ============================================================
// Global Registry Access (Optional)
// ============================================================

/// Get the global state change registry
inline StateChangeRegistry& get_global_registry() {
    static StateChangeRegistry registry;
    return registry;
}

/// Initialize the global registry with default handlers
inline void initialize_global_registry() {
    auto& registry = get_global_registry();
    setup_default_handlers(registry);
}

} // namespace cc::state::on_change
