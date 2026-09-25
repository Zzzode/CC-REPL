/// @file selectors.cppm
/// @brief Memoized selectors for derived state in the Loom REPL.
/// Provides efficient cached computations over AppState, avoiding
/// redundant recalculations when the underlying data hasn't changed.
///
/// RFC 0001 Phase C batch 10: every free-function body lives in six module
/// implementation units — selectors_core.cpp (basic + settings selectors),
/// selectors_bridge.cpp (REPL bridge state + the connectivity composites),
/// selectors_ui_tasks.cpp (UI state + tasks/agents + the task-view/multi-agent
/// composites), selectors_companion_mcp.cpp (companion buddy + MCP/plugins),
/// selectors_conversation.cpp (message/conversation + is_ui_busy; the only
/// impl unit that imports cc.types.types for cc::core::TokenUsage), and
/// selectors_features.cpp (prompt suggestion, speculation, skill improvement,
/// inbox, worker sandbox).  This primary keeps the MemoizedSelector class
/// template and the five create_*_based_selector factory templates inline
/// (they are templates) and only declarations for the 113 free functions.
module;

#include <cstdint>
#include <cstddef>

export module cc.state.selectors;

import std;

import cc.types.types;
import cc.state.app_state;

export namespace cc::state::selectors {

// ============================================================
// Memoization Infrastructure
// ============================================================

/// Generic memoized selector that caches result based on an input key.
/// Re-computes only when the extracted key changes.
template <typename Key, typename Value>
class MemoizedSelector {
    std::optional<Key> cached_key_;
    std::optional<Value> cached_value_;
    std::function<Key(const AppState&)> key_extractor_;
    std::function<Value(const AppState&)> compute_;
    mutable std::mutex mutex_;
    std::atomic<uint64_t> hit_count_{0};
    std::atomic<uint64_t> miss_count_{0};

public:
    /// Construct with key extraction function and computation function
    MemoizedSelector(
        std::function<Key(const AppState&)> key_fn,
        std::function<Value(const AppState&)> compute_fn
    ) : key_extractor_(std::move(key_fn)),
        compute_(std::move(compute_fn)) {}

    /// Get the cached or freshly computed value
    [[nodiscard]] Value select(const AppState& state) {
        std::lock_guard lock(mutex_);
        auto key = key_extractor_(state);
        if (cached_key_ && *cached_key_ == key && cached_value_) {
            hit_count_.fetch_add(1, std::memory_order_relaxed);
            return *cached_value_;
        }
        miss_count_.fetch_add(1, std::memory_order_relaxed);
        cached_key_ = key;
        cached_value_ = compute_(state);
        return *cached_value_;
    }

    /// Invalidate the cache
    void invalidate() {
        std::lock_guard lock(mutex_);
        cached_key_.reset();
        cached_value_.reset();
    }

    /// Get cache hit count
    [[nodiscard]] uint64_t get_hit_count() const {
        return hit_count_.load(std::memory_order_relaxed);
    }

    /// Get cache miss count
    [[nodiscard]] uint64_t get_miss_count() const {
        return miss_count_.load(std::memory_order_relaxed);
    }

    /// Get cache hit rate (0.0 to 1.0)
    [[nodiscard]] double get_hit_rate() const {
        auto hits = hit_count_.load(std::memory_order_relaxed);
        auto misses = miss_count_.load(std::memory_order_relaxed);
        auto total = hits + misses;
        return total > 0 ? static_cast<double>(hits) / static_cast<double>(total) : 0.0;
    }
};

// ============================================================
// Basic Selectors
// ============================================================

/// Check if verbose mode is enabled
[[nodiscard]] bool is_verbose(const AppState& state) noexcept;

/// Check if compact mode is enabled
[[nodiscard]] bool is_compact_mode(const AppState& state) noexcept;

/// Check if fast mode is enabled
[[nodiscard]] bool is_fast_mode(const AppState& state) noexcept;

/// Check if thinking is enabled
[[nodiscard]] bool is_thinking_enabled(const AppState& state) noexcept;

/// Check if prompt suggestions are enabled
[[nodiscard]] bool is_prompt_suggestion_enabled(const AppState& state) noexcept;

/// Check if Kairos is enabled
[[nodiscard]] bool is_kairos_enabled(const AppState& state) noexcept;

/// Check if Ultraplan mode is active
[[nodiscard]] bool is_ultraplan_mode(const AppState& state) noexcept;

/// Check if Ultraplan is launching
[[nodiscard]] bool is_ultraplan_launching(const AppState& state) noexcept;

/// Get the main loop model
[[nodiscard]] std::optional<std::string_view> get_main_loop_model(const AppState& state) noexcept;

/// Get the current agent name
[[nodiscard]] std::optional<std::string_view> get_agent(const AppState& state) noexcept;

/// Get the current expanded view
[[nodiscard]] ExpandedView get_expanded_view(const AppState& state) noexcept;

/// Check if tasks view is expanded
[[nodiscard]] bool is_tasks_view_expanded(const AppState& state) noexcept;

/// Check if teammates view is expanded
[[nodiscard]] bool is_teammates_view_expanded(const AppState& state) noexcept;

/// Get the current remote connection status
[[nodiscard]] RemoteConnectionStatus get_remote_connection_status(const AppState& state) noexcept;

/// Check if connected to remote
[[nodiscard]] bool is_remote_connected(const AppState& state) noexcept;

/// Check if reconnecting to remote
[[nodiscard]] bool is_remote_reconnecting(const AppState& state) noexcept;

/// Get the current permission mode
[[nodiscard]] PermissionMode get_permission_mode(const AppState& state) noexcept;

/// Get the status line text
[[nodiscard]] std::optional<std::string_view> get_status_line_text(const AppState& state) noexcept;

// ============================================================
// Settings Selectors
// ============================================================

/// Get the configured model from settings
[[nodiscard]] std::string_view get_settings_model(const AppState& state) noexcept;

/// Get the configured theme from settings
[[nodiscard]] std::string_view get_settings_theme(const AppState& state) noexcept;

/// Check if status line is enabled in settings
[[nodiscard]] bool is_status_line_enabled(const AppState& state) noexcept;

/// Get the status line command from settings
[[nodiscard]] std::string_view get_status_line_command(const AppState& state) noexcept;

/// Get the status line padding from settings
[[nodiscard]] int get_status_line_padding(const AppState& state) noexcept;

/// Get the output style from settings
[[nodiscard]] std::string_view get_output_style(const AppState& state) noexcept;

/// Check if all hooks are disabled in settings
[[nodiscard]] bool are_all_hooks_disabled(const AppState& state) noexcept;

/// Get the number of notifications
[[nodiscard]] size_t get_notification_count(const AppState& state) noexcept;

/// Check if there are any notifications
[[nodiscard]] bool has_notifications(const AppState& state) noexcept;

/// Get the active overlays
[[nodiscard]] const std::set<std::string>& get_active_overlays(const AppState& state) noexcept;

/// Check if there are any active overlays
[[nodiscard]] bool has_active_overlays(const AppState& state) noexcept;

/// Check if a specific overlay is active
[[nodiscard]] bool is_overlay_active(const AppState& state, std::string_view overlay_name) noexcept;

/// Get the auth version
[[nodiscard]] uint32_t get_auth_version(const AppState& state) noexcept;

/// Get the effort value
[[nodiscard]] std::optional<std::string_view> get_effort_value(const AppState& state) noexcept;

/// Get the advisor model
[[nodiscard]] std::optional<std::string_view> get_advisor_model(const AppState& state) noexcept;

/// Get the Ultraplan session URL
[[nodiscard]] std::optional<std::string_view> get_ultraplan_session_url(const AppState& state) noexcept;

/// Get the speculation session time saved
[[nodiscard]] int64_t get_speculation_session_time_saved_ms(const AppState& state) noexcept;

// ============================================================
// Bridge State Selectors
// ============================================================

/// Check if the REPL bridge is enabled
[[nodiscard]] bool is_repl_bridge_enabled(const AppState& state) noexcept;

/// Check if the REPL bridge is connected
[[nodiscard]] bool is_repl_bridge_connected(const AppState& state) noexcept;

/// Check if the REPL bridge session is active
[[nodiscard]] bool is_repl_bridge_session_active(const AppState& state) noexcept;

/// Check if the REPL bridge is reconnecting
[[nodiscard]] bool is_repl_bridge_reconnecting(const AppState& state) noexcept;

/// Check if the REPL bridge is in outbound-only mode
[[nodiscard]] bool is_repl_bridge_outbound_only(const AppState& state) noexcept;

/// Check if the REPL bridge is explicit
[[nodiscard]] bool is_repl_bridge_explicit(const AppState& state) noexcept;

/// Check if we should show the remote callout
[[nodiscard]] bool should_show_remote_callout(const AppState& state) noexcept;

/// Get the REPL bridge connect URL
[[nodiscard]] std::optional<std::string_view> get_repl_bridge_connect_url(const AppState& state) noexcept;

/// Get the REPL bridge session URL
[[nodiscard]] std::optional<std::string_view> get_repl_bridge_session_url(const AppState& state) noexcept;

/// Get the REPL bridge error
[[nodiscard]] std::optional<std::string_view> get_repl_bridge_error(const AppState& state) noexcept;

// ============================================================
// UI State Selectors
// ============================================================

/// Get the selected IP agent index
[[nodiscard]] int32_t get_selected_ip_agent_index(const AppState& state) noexcept;

/// Get the coordinator task index
[[nodiscard]] int32_t get_coordinator_task_index(const AppState& state) noexcept;

/// Get the view selection mode
[[nodiscard]] std::string_view get_view_selection_mode(const AppState& state) noexcept;

/// Check if we're in brief-only mode
[[nodiscard]] bool is_brief_only(const AppState& state) noexcept;

/// Check if we should show teammate message previews
[[nodiscard]] bool should_show_teammate_message_preview(const AppState& state) noexcept;

/// Get the footer selection
[[nodiscard]] std::optional<FooterItem> get_footer_selection(const AppState& state) noexcept;

/// Check if a specific footer item is selected
[[nodiscard]] bool is_footer_item_selected(const AppState& state, FooterItem item) noexcept;

/// Get the spinner tip
[[nodiscard]] std::optional<std::string_view> get_spinner_tip(const AppState& state) noexcept;

// ============================================================
// Tasks & Agents Selectors
// ============================================================

/// Get the number of tasks
[[nodiscard]] size_t get_task_count(const AppState& state) noexcept;

/// Check if there are any tasks
[[nodiscard]] bool has_tasks(const AppState& state) noexcept;

/// Get the foregrounded task ID
[[nodiscard]] std::optional<std::string_view> get_foregrounded_task_id(const AppState& state) noexcept;

/// Check if a specific task is foregrounded
[[nodiscard]] bool is_task_foregrounded(const AppState& state, std::string_view task_id) noexcept;

/// Get the viewing agent task ID
[[nodiscard]] std::optional<std::string_view> get_viewing_agent_task_id(const AppState& state) noexcept;

/// Check if we're viewing a specific agent's task
[[nodiscard]] bool is_viewing_agent_task(const AppState& state, std::string_view task_id) noexcept;

/// Get the number of agents in the name registry
[[nodiscard]] size_t get_agent_name_registry_count(const AppState& state) noexcept;

/// Check if an agent name is registered
[[nodiscard]] bool is_agent_name_registered(const AppState& state, std::string_view name) noexcept;

/// Get an agent ID by name
[[nodiscard]] std::optional<std::string_view> get_agent_id_by_name(const AppState& state, std::string_view name) noexcept;

// ============================================================
// Companion (Buddy) Selectors
// ============================================================

/// Get the companion reaction
[[nodiscard]] std::optional<std::string_view> get_companion_reaction(const AppState& state) noexcept;

/// Get the companion pet time
[[nodiscard]] std::optional<std::chrono::system_clock::time_point> get_companion_pet_time(const AppState& state) noexcept;

/// Check if companion was petted recently (within last 5 seconds)
[[nodiscard]] bool was_companion_petted_recently(const AppState& state) noexcept;

// ============================================================
// MCP & Plugins Selectors
// ============================================================

/// Get the number of MCP clients
[[nodiscard]] size_t get_mcp_client_count(const AppState& state) noexcept;

/// Check if there are any MCP clients
[[nodiscard]] bool has_mcp_clients(const AppState& state) noexcept;

/// Get the number of MCP tools
[[nodiscard]] size_t get_mcp_tool_count(const AppState& state) noexcept;

/// Check if there are any MCP tools
[[nodiscard]] bool has_mcp_tools(const AppState& state) noexcept;

/// Get the number of MCP commands
[[nodiscard]] size_t get_mcp_command_count(const AppState& state) noexcept;

/// Check if there are any MCP commands
[[nodiscard]] bool has_mcp_commands(const AppState& state) noexcept;

/// Get the MCP plugin reconnect key
[[nodiscard]] uint32_t get_mcp_plugin_reconnect_key(const AppState& state) noexcept;

/// Get the number of enabled plugins
[[nodiscard]] size_t get_enabled_plugin_count(const AppState& state) noexcept;

/// Get the number of disabled plugins
[[nodiscard]] size_t get_disabled_plugin_count(const AppState& state) noexcept;

/// Get the total number of plugins
[[nodiscard]] size_t get_total_plugin_count(const AppState& state) noexcept;

/// Check if there are any plugin errors
[[nodiscard]] bool has_plugin_errors(const AppState& state) noexcept;

/// Get the number of plugin errors
[[nodiscard]] size_t get_plugin_error_count(const AppState& state) noexcept;

/// Check if plugins need refresh
[[nodiscard]] bool do_plugins_need_refresh(const AppState& state) noexcept;

// ============================================================
// Message & Conversation Selectors
// ============================================================

/// Get the number of messages
[[nodiscard]] size_t get_message_count(const AppState& state) noexcept;

/// Check if there are any messages
[[nodiscard]] bool has_messages(const AppState& state) noexcept;

/// Check if loading
[[nodiscard]] bool is_loading(const AppState& state) noexcept;

/// Check if streaming
[[nodiscard]] bool is_streaming(const AppState& state) noexcept;

/// Get the error message
[[nodiscard]] std::optional<std::string_view> get_error_message(const AppState& state) noexcept;

/// Check if there's an error
[[nodiscard]] bool has_error(const AppState& state) noexcept;

/// Get the total token usage
[[nodiscard]] const cc::core::TokenUsage& get_total_usage(const AppState& state) noexcept;

/// Get the total cost in USD
[[nodiscard]] double get_total_cost_usd(const AppState& state) noexcept;

/// Get the working directory
[[nodiscard]] std::string_view get_working_directory(const AppState& state) noexcept;

/// Get allowed directories (paths added via /add-dir)
[[nodiscard]] const std::vector<std::string>& get_allowed_directories(const AppState& state) noexcept;

// ============================================================
// Prompt Suggestion Selectors
// ============================================================

/// Get the prompt suggestion text
[[nodiscard]] std::optional<std::string_view> get_prompt_suggestion_text(const AppState& state) noexcept;

/// Get the prompt suggestion prompt ID
[[nodiscard]] std::optional<std::string_view> get_prompt_suggestion_prompt_id(const AppState& state) noexcept;

/// Check if there's an active prompt suggestion
[[nodiscard]] bool has_prompt_suggestion(const AppState& state) noexcept;

// ============================================================
// Speculation Selectors
// ============================================================

/// Get the speculation status
[[nodiscard]] SpeculationStatus get_speculation_status(const AppState& state) noexcept;

/// Check if speculation is active
[[nodiscard]] bool is_speculation_active(const AppState& state) noexcept;

/// Check if speculation is idle
[[nodiscard]] bool is_speculation_idle(const AppState& state) noexcept;

/// Get the speculation ID
[[nodiscard]] std::string_view get_speculation_id(const AppState& state) noexcept;

/// Get the speculation message count
[[nodiscard]] size_t get_speculation_message_count(const AppState& state) noexcept;

/// Get the speculation suggestion length
[[nodiscard]] size_t get_speculation_suggestion_length(const AppState& state) noexcept;

/// Get the speculation tool use count
[[nodiscard]] size_t get_speculation_tool_use_count(const AppState& state) noexcept;

/// Check if speculation is pipelined
[[nodiscard]] bool is_speculation_pipelined(const AppState& state) noexcept;

// ============================================================
// Skill Improvement Selectors
// ============================================================

/// Check if there's a skill improvement suggestion
[[nodiscard]] bool has_skill_improvement_suggestion(const AppState& state) noexcept;

/// Get the skill improvement suggestion skill name
[[nodiscard]] std::optional<std::string_view> get_skill_improvement_skill_name(const AppState& state) noexcept;

// ============================================================
// Inbox Selectors
// ============================================================

/// Get the inbox message count
[[nodiscard]] size_t get_inbox_message_count(const AppState& state) noexcept;

/// Check if there are any inbox messages
[[nodiscard]] bool has_inbox_messages(const AppState& state) noexcept;

// ============================================================
// Worker Sandbox Selectors
// ============================================================

/// Get the number of pending sandbox permission requests
[[nodiscard]] size_t get_pending_sandbox_permission_count(const AppState& state) noexcept;

/// Check if there are any pending sandbox permission requests
[[nodiscard]] bool has_pending_sandbox_permissions(const AppState& state) noexcept;

/// Get the selected sandbox permission index
[[nodiscard]] size_t get_selected_sandbox_permission_index(const AppState& state) noexcept;

/// Check if there's a pending worker request
[[nodiscard]] bool has_pending_worker_request(const AppState& state) noexcept;

/// Check if there's a pending sandbox request
[[nodiscard]] bool has_pending_sandbox_request(const AppState& state) noexcept;

// ============================================================
// Derived Selectors (Composite)
// ============================================================

/// Check if the UI is busy (loading, streaming, or has error)
[[nodiscard]] bool is_ui_busy(const AppState& state) noexcept;

/// Check if we're connected to anything (remote or bridge)
[[nodiscard]] bool is_connected_to_anything(const AppState& state) noexcept;

/// Check if any bridge-related status is active
[[nodiscard]] bool has_any_bridge_activity(const AppState& state) noexcept;

/// Check if any task-related view is active
[[nodiscard]] bool is_any_task_view_active(const AppState& state) noexcept;

/// Check if we're in a multi-agent mode
[[nodiscard]] bool is_multi_agent_mode(const AppState& state) noexcept;

// ============================================================
// Selector Factory Functions
// ============================================================

/// Create a memoized selector using message count as cache key
template <typename Value>
[[nodiscard]] inline auto create_message_based_selector(
    std::function<Value(const AppState&)> compute
) {
    return MemoizedSelector<size_t, Value>(
        [](const AppState& s) { return s.messages.size(); },
        std::move(compute)
    );
}

/// Create a memoized selector using total token count as cache key
template <typename Value>
[[nodiscard]] inline auto create_usage_based_selector(
    std::function<Value(const AppState&)> compute
) {
    return MemoizedSelector<uint32_t, Value>(
        [](const AppState& s) { return s.total_usage.total(); },
        std::move(compute)
    );
}

/// Create a memoized selector using task count as cache key
template <typename Value>
[[nodiscard]] inline auto create_task_based_selector(
    std::function<Value(const AppState&)> compute
) {
    return MemoizedSelector<size_t, Value>(
        [](const AppState& s) { return s.tasks.size(); },
        std::move(compute)
    );
}

/// Create a memoized selector using auth version as cache key
template <typename Value>
[[nodiscard]] inline auto create_auth_based_selector(
    std::function<Value(const AppState&)> compute
) {
    return MemoizedSelector<uint32_t, Value>(
        [](const AppState& s) { return s.auth_version; },
        std::move(compute)
    );
}

/// Create a memoized selector using MCP reconnect key as cache key
template <typename Value>
[[nodiscard]] inline auto create_mcp_based_selector(
    std::function<Value(const AppState&)> compute
) {
    return MemoizedSelector<uint32_t, Value>(
        [](const AppState& s) { return s.mcp.plugin_reconnect_key; },
        std::move(compute)
    );
}

} // namespace cc::state::selectors
