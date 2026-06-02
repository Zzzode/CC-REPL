/// @file selectors.cppm
/// @brief Memoized selectors for derived state in the Claude Code REPL.
/// Provides efficient cached computations over AppState, avoiding
/// redundant recalculations when the underlying data hasn't changed.
module;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <functional>
#include <ranges>
#include <algorithm>
#include <unordered_map>
#include <mutex>
#include <concepts>
#include <tuple>
#include <memory>
#include <atomic>
#include <set>

export module cc.state.selectors;

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
[[nodiscard]] inline bool is_verbose(const AppState& state) noexcept {
    return state.verbose;
}

/// Check if compact mode is enabled
[[nodiscard]] inline bool is_compact_mode(const AppState& state) noexcept {
    return state.compact_mode;
}

/// Check if fast mode is enabled
[[nodiscard]] inline bool is_fast_mode(const AppState& state) noexcept {
    return state.fast_mode;
}

/// Check if thinking is enabled
[[nodiscard]] inline bool is_thinking_enabled(const AppState& state) noexcept {
    return state.thinking_enabled;
}

/// Check if prompt suggestions are enabled
[[nodiscard]] inline bool is_prompt_suggestion_enabled(const AppState& state) noexcept {
    return state.prompt_suggestion_enabled;
}

/// Check if Kairos is enabled
[[nodiscard]] inline bool is_kairos_enabled(const AppState& state) noexcept {
    return state.kairos_enabled;
}

/// Check if Ultraplan mode is active
[[nodiscard]] inline bool is_ultraplan_mode(const AppState& state) noexcept {
    return state.is_ultraplan_mode;
}

/// Check if Ultraplan is launching
[[nodiscard]] inline bool is_ultraplan_launching(const AppState& state) noexcept {
    return state.ultraplan_launching;
}

/// Get the main loop model
[[nodiscard]] inline std::optional<std::string_view> get_main_loop_model(const AppState& state) noexcept {
    if (state.main_loop_model) {
        return *state.main_loop_model;
    }
    return std::nullopt;
}

/// Get the current agent name
[[nodiscard]] inline std::optional<std::string_view> get_agent(const AppState& state) noexcept {
    if (state.agent) {
        return *state.agent;
    }
    return std::nullopt;
}

/// Get the current expanded view
[[nodiscard]] inline ExpandedView get_expanded_view(const AppState& state) noexcept {
    return state.expanded_view;
}

/// Check if tasks view is expanded
[[nodiscard]] inline bool is_tasks_view_expanded(const AppState& state) noexcept {
    return state.expanded_view == ExpandedView::Tasks;
}

/// Check if teammates view is expanded
[[nodiscard]] inline bool is_teammates_view_expanded(const AppState& state) noexcept {
    return state.expanded_view == ExpandedView::Teammates;
}

/// Get the current remote connection status
[[nodiscard]] inline RemoteConnectionStatus get_remote_connection_status(const AppState& state) noexcept {
    return state.remote_connection_status;
}

/// Check if connected to remote
[[nodiscard]] inline bool is_remote_connected(const AppState& state) noexcept {
    return state.remote_connection_status == RemoteConnectionStatus::Connected;
}

/// Check if reconnecting to remote
[[nodiscard]] inline bool is_remote_reconnecting(const AppState& state) noexcept {
    return state.remote_connection_status == RemoteConnectionStatus::Reconnecting;
}

/// Get the current permission mode
[[nodiscard]] inline PermissionMode get_permission_mode(const AppState& state) noexcept {
    return state.tool_permission_context.mode;
}

/// Get the status line text
[[nodiscard]] inline std::optional<std::string_view> get_status_line_text(const AppState& state) noexcept {
    if (state.status_line_text) {
        return *state.status_line_text;
    }
    return std::nullopt;
}

/// Get the number of notifications
[[nodiscard]] inline size_t get_notification_count(const AppState& state) noexcept {
    return state.notifications.size();
}

/// Check if there are any notifications
[[nodiscard]] inline bool has_notifications(const AppState& state) noexcept {
    return !state.notifications.empty();
}

/// Get the active overlays
[[nodiscard]] inline const std::set<std::string>& get_active_overlays(const AppState& state) noexcept {
    return state.active_overlays;
}

/// Check if there are any active overlays
[[nodiscard]] inline bool has_active_overlays(const AppState& state) noexcept {
    return !state.active_overlays.empty();
}

/// Check if a specific overlay is active
[[nodiscard]] inline bool is_overlay_active(const AppState& state, std::string_view overlay_name) noexcept {
    return state.active_overlays.contains(std::string(overlay_name));
}

/// Get the auth version
[[nodiscard]] inline uint32_t get_auth_version(const AppState& state) noexcept {
    return state.auth_version;
}

/// Get the effort value
[[nodiscard]] inline std::optional<std::string_view> get_effort_value(const AppState& state) noexcept {
    if (state.effort_value) {
        return *state.effort_value;
    }
    return std::nullopt;
}

/// Get the advisor model
[[nodiscard]] inline std::optional<std::string_view> get_advisor_model(const AppState& state) noexcept {
    if (state.advisor_model) {
        return *state.advisor_model;
    }
    return std::nullopt;
}

/// Get the Ultraplan session URL
[[nodiscard]] inline std::optional<std::string_view> get_ultraplan_session_url(const AppState& state) noexcept {
    if (state.ultraplan_session_url) {
        return *state.ultraplan_session_url;
    }
    return std::nullopt;
}

/// Get the speculation session time saved
[[nodiscard]] inline int64_t get_speculation_session_time_saved_ms(const AppState& state) noexcept {
    return state.speculation_session_time_saved_ms;
}

// ============================================================
// Bridge State Selectors
// ============================================================

/// Check if the REPL bridge is enabled
[[nodiscard]] inline bool is_repl_bridge_enabled(const AppState& state) noexcept {
    return state.repl_bridge_enabled;
}

/// Check if the REPL bridge is connected
[[nodiscard]] inline bool is_repl_bridge_connected(const AppState& state) noexcept {
    return state.repl_bridge_connected;
}

/// Check if the REPL bridge session is active
[[nodiscard]] inline bool is_repl_bridge_session_active(const AppState& state) noexcept {
    return state.repl_bridge_session_active;
}

/// Check if the REPL bridge is reconnecting
[[nodiscard]] inline bool is_repl_bridge_reconnecting(const AppState& state) noexcept {
    return state.repl_bridge_reconnecting;
}

/// Check if the REPL bridge is in outbound-only mode
[[nodiscard]] inline bool is_repl_bridge_outbound_only(const AppState& state) noexcept {
    return state.repl_bridge_outbound_only;
}

/// Check if the REPL bridge is explicit
[[nodiscard]] inline bool is_repl_bridge_explicit(const AppState& state) noexcept {
    return state.repl_bridge_explicit;
}

/// Check if we should show the remote callout
[[nodiscard]] inline bool should_show_remote_callout(const AppState& state) noexcept {
    return state.show_remote_callout;
}

/// Get the REPL bridge connect URL
[[nodiscard]] inline std::optional<std::string_view> get_repl_bridge_connect_url(const AppState& state) noexcept {
    if (state.repl_bridge_connect_url) {
        return *state.repl_bridge_connect_url;
    }
    return std::nullopt;
}

/// Get the REPL bridge session URL
[[nodiscard]] inline std::optional<std::string_view> get_repl_bridge_session_url(const AppState& state) noexcept {
    if (state.repl_bridge_session_url) {
        return *state.repl_bridge_session_url;
    }
    return std::nullopt;
}

/// Get the REPL bridge error
[[nodiscard]] inline std::optional<std::string_view> get_repl_bridge_error(const AppState& state) noexcept {
    if (state.repl_bridge_error) {
        return *state.repl_bridge_error;
    }
    return std::nullopt;
}

// ============================================================
// UI State Selectors
// ============================================================

/// Get the selected IP agent index
[[nodiscard]] inline int32_t get_selected_ip_agent_index(const AppState& state) noexcept {
    return state.selected_ip_agent_index;
}

/// Get the coordinator task index
[[nodiscard]] inline int32_t get_coordinator_task_index(const AppState& state) noexcept {
    return state.coordinator_task_index;
}

/// Get the view selection mode
[[nodiscard]] inline std::string_view get_view_selection_mode(const AppState& state) noexcept {
    return state.view_selection_mode;
}

/// Check if we're in brief-only mode
[[nodiscard]] inline bool is_brief_only(const AppState& state) noexcept {
    return state.is_brief_only;
}

/// Check if we should show teammate message previews
[[nodiscard]] inline bool should_show_teammate_message_preview(const AppState& state) noexcept {
    return state.show_teammate_message_preview;
}

/// Get the footer selection
[[nodiscard]] inline std::optional<FooterItem> get_footer_selection(const AppState& state) noexcept {
    return state.footer_selection;
}

/// Check if a specific footer item is selected
[[nodiscard]] inline bool is_footer_item_selected(const AppState& state, FooterItem item) noexcept {
    return state.footer_selection == item;
}

/// Get the spinner tip
[[nodiscard]] inline std::optional<std::string_view> get_spinner_tip(const AppState& state) noexcept {
    if (state.spinner_tip) {
        return *state.spinner_tip;
    }
    return std::nullopt;
}

// ============================================================
// Tasks & Agents Selectors
// ============================================================

/// Get the number of tasks
[[nodiscard]] inline size_t get_task_count(const AppState& state) noexcept {
    return state.tasks.size();
}

/// Check if there are any tasks
[[nodiscard]] inline bool has_tasks(const AppState& state) noexcept {
    return !state.tasks.empty();
}

/// Get the foregrounded task ID
[[nodiscard]] inline std::optional<std::string_view> get_foregrounded_task_id(const AppState& state) noexcept {
    if (state.foregrounded_task_id) {
        return *state.foregrounded_task_id;
    }
    return std::nullopt;
}

/// Check if a specific task is foregrounded
[[nodiscard]] inline bool is_task_foregrounded(const AppState& state, std::string_view task_id) noexcept {
    return state.foregrounded_task_id == task_id;
}

/// Get the viewing agent task ID
[[nodiscard]] inline std::optional<std::string_view> get_viewing_agent_task_id(const AppState& state) noexcept {
    if (state.viewing_agent_task_id) {
        return *state.viewing_agent_task_id;
    }
    return std::nullopt;
}

/// Check if we're viewing a specific agent's task
[[nodiscard]] inline bool is_viewing_agent_task(const AppState& state, std::string_view task_id) noexcept {
    return state.viewing_agent_task_id == task_id;
}

/// Get the number of agents in the name registry
[[nodiscard]] inline size_t get_agent_name_registry_count(const AppState& state) noexcept {
    return state.agent_name_registry.size();
}

/// Check if an agent name is registered
[[nodiscard]] inline bool is_agent_name_registered(const AppState& state, std::string_view name) noexcept {
    return state.agent_name_registry.contains(std::string(name));
}

/// Get an agent ID by name
[[nodiscard]] inline std::optional<std::string_view> get_agent_id_by_name(const AppState& state, std::string_view name) noexcept {
    auto it = state.agent_name_registry.find(std::string(name));
    if (it != state.agent_name_registry.end()) {
        return it->second;
    }
    return std::nullopt;
}

// ============================================================
// Companion (Buddy) Selectors
// ============================================================

/// Get the companion reaction
[[nodiscard]] inline std::optional<std::string_view> get_companion_reaction(const AppState& state) noexcept {
    if (state.companion_reaction) {
        return *state.companion_reaction;
    }
    return std::nullopt;
}

/// Get the companion pet time
[[nodiscard]] inline std::optional<std::chrono::system_clock::time_point> get_companion_pet_time(const AppState& state) noexcept {
    return state.companion_pet_at;
}

/// Check if companion was petted recently (within last 5 seconds)
[[nodiscard]] inline bool was_companion_petted_recently(const AppState& state) noexcept {
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
[[nodiscard]] inline size_t get_mcp_client_count(const AppState& state) noexcept {
    return state.mcp.clients.size();
}

/// Check if there are any MCP clients
[[nodiscard]] inline bool has_mcp_clients(const AppState& state) noexcept {
    return !state.mcp.clients.empty();
}

/// Get the number of MCP tools
[[nodiscard]] inline size_t get_mcp_tool_count(const AppState& state) noexcept {
    return state.mcp.tools.size();
}

/// Check if there are any MCP tools
[[nodiscard]] inline bool has_mcp_tools(const AppState& state) noexcept {
    return !state.mcp.tools.empty();
}

/// Get the number of MCP commands
[[nodiscard]] inline size_t get_mcp_command_count(const AppState& state) noexcept {
    return state.mcp.commands.size();
}

/// Check if there are any MCP commands
[[nodiscard]] inline bool has_mcp_commands(const AppState& state) noexcept {
    return !state.mcp.commands.empty();
}

/// Get the MCP plugin reconnect key
[[nodiscard]] inline uint32_t get_mcp_plugin_reconnect_key(const AppState& state) noexcept {
    return state.mcp.plugin_reconnect_key;
}

/// Get the number of enabled plugins
[[nodiscard]] inline size_t get_enabled_plugin_count(const AppState& state) noexcept {
    return state.plugins.enabled.size();
}

/// Get the number of disabled plugins
[[nodiscard]] inline size_t get_disabled_plugin_count(const AppState& state) noexcept {
    return state.plugins.disabled.size();
}

/// Get the total number of plugins
[[nodiscard]] inline size_t get_total_plugin_count(const AppState& state) noexcept {
    return state.plugins.enabled.size() + state.plugins.disabled.size();
}

/// Check if there are any plugin errors
[[nodiscard]] inline bool has_plugin_errors(const AppState& state) noexcept {
    return !state.plugins.errors.empty();
}

/// Get the number of plugin errors
[[nodiscard]] inline size_t get_plugin_error_count(const AppState& state) noexcept {
    return state.plugins.errors.size();
}

/// Check if plugins need refresh
[[nodiscard]] inline bool do_plugins_need_refresh(const AppState& state) noexcept {
    return state.plugins.needs_refresh;
}

// ============================================================
// Message & Conversation Selectors
// ============================================================

/// Get the number of messages
[[nodiscard]] inline size_t get_message_count(const AppState& state) noexcept {
    return state.messages.size();
}

/// Check if there are any messages
[[nodiscard]] inline bool has_messages(const AppState& state) noexcept {
    return !state.messages.empty();
}

/// Check if loading
[[nodiscard]] inline bool is_loading(const AppState& state) noexcept {
    return state.is_loading;
}

/// Check if streaming
[[nodiscard]] inline bool is_streaming(const AppState& state) noexcept {
    return state.is_streaming;
}

/// Get the error message
[[nodiscard]] inline std::optional<std::string_view> get_error_message(const AppState& state) noexcept {
    if (state.error_message) {
        return *state.error_message;
    }
    return std::nullopt;
}

/// Check if there's an error
[[nodiscard]] inline bool has_error(const AppState& state) noexcept {
    return state.error_message.has_value();
}

/// Get the total token usage
[[nodiscard]] inline const cc::core::TokenUsage& get_total_usage(const AppState& state) noexcept {
    return state.total_usage;
}

/// Get the total cost in USD
[[nodiscard]] inline double get_total_cost_usd(const AppState& state) noexcept {
    return state.total_cost_usd;
}

/// Get the working directory
[[nodiscard]] inline std::string_view get_working_directory(const AppState& state) noexcept {
    return state.working_directory;
}

// ============================================================
// Prompt Suggestion Selectors
// ============================================================

/// Get the prompt suggestion text
[[nodiscard]] inline std::optional<std::string_view> get_prompt_suggestion_text(const AppState& state) noexcept {
    if (state.prompt_suggestion.text) {
        return *state.prompt_suggestion.text;
    }
    return std::nullopt;
}

/// Get the prompt suggestion prompt ID
[[nodiscard]] inline std::optional<std::string_view> get_prompt_suggestion_prompt_id(const AppState& state) noexcept {
    if (state.prompt_suggestion.prompt_id) {
        return *state.prompt_suggestion.prompt_id;
    }
    return std::nullopt;
}

/// Check if there's an active prompt suggestion
[[nodiscard]] inline bool has_prompt_suggestion(const AppState& state) noexcept {
    return state.prompt_suggestion.text.has_value();
}

// ============================================================
// Speculation Selectors
// ============================================================

/// Get the speculation status
[[nodiscard]] inline SpeculationStatus get_speculation_status(const AppState& state) noexcept {
    return state.speculation.status;
}

/// Check if speculation is active
[[nodiscard]] inline bool is_speculation_active(const AppState& state) noexcept {
    return state.speculation.status == SpeculationStatus::Active;
}

/// Check if speculation is idle
[[nodiscard]] inline bool is_speculation_idle(const AppState& state) noexcept {
    return state.speculation.status == SpeculationStatus::Idle;
}

/// Get the speculation ID
[[nodiscard]] inline std::string_view get_speculation_id(const AppState& state) noexcept {
    return state.speculation.id;
}

/// Get the speculation message count
[[nodiscard]] inline size_t get_speculation_message_count(const AppState& state) noexcept {
    return state.speculation.messages.size();
}

/// Get the speculation suggestion length
[[nodiscard]] inline size_t get_speculation_suggestion_length(const AppState& state) noexcept {
    return state.speculation.suggestion_length;
}

/// Get the speculation tool use count
[[nodiscard]] inline size_t get_speculation_tool_use_count(const AppState& state) noexcept {
    return state.speculation.tool_use_count;
}

/// Check if speculation is pipelined
[[nodiscard]] inline bool is_speculation_pipelined(const AppState& state) noexcept {
    return state.speculation.is_pipelined;
}

// ============================================================
// Skill Improvement Selectors
// ============================================================

/// Check if there's a skill improvement suggestion
[[nodiscard]] inline bool has_skill_improvement_suggestion(const AppState& state) noexcept {
    return state.skill_improvement.suggestion.has_value();
}

/// Get the skill improvement suggestion skill name
[[nodiscard]] inline std::optional<std::string_view> get_skill_improvement_skill_name(const AppState& state) noexcept {
    if (state.skill_improvement.suggestion) {
        return state.skill_improvement.suggestion->skill_name;
    }
    return std::nullopt;
}

// ============================================================
// Inbox Selectors
// ============================================================

/// Get the inbox message count
[[nodiscard]] inline size_t get_inbox_message_count(const AppState& state) noexcept {
    return state.inbox.messages.size();
}

/// Check if there are any inbox messages
[[nodiscard]] inline bool has_inbox_messages(const AppState& state) noexcept {
    return !state.inbox.messages.empty();
}

// ============================================================
// Worker Sandbox Selectors
// ============================================================

/// Get the number of pending sandbox permission requests
[[nodiscard]] inline size_t get_pending_sandbox_permission_count(const AppState& state) noexcept {
    return state.worker_sandbox_permissions.queue.size();
}

/// Check if there are any pending sandbox permission requests
[[nodiscard]] inline bool has_pending_sandbox_permissions(const AppState& state) noexcept {
    return !state.worker_sandbox_permissions.queue.empty();
}

/// Get the selected sandbox permission index
[[nodiscard]] inline size_t get_selected_sandbox_permission_index(const AppState& state) noexcept {
    return state.worker_sandbox_permissions.selected_index;
}

/// Check if there's a pending worker request
[[nodiscard]] inline bool has_pending_worker_request(const AppState& state) noexcept {
    return state.pending_worker_request.has_value();
}

/// Check if there's a pending sandbox request
[[nodiscard]] inline bool has_pending_sandbox_request(const AppState& state) noexcept {
    return state.pending_sandbox_request.has_value();
}

// ============================================================
// Derived Selectors (Composite)
// ============================================================

/// Check if the UI is busy (loading, streaming, or has error)
[[nodiscard]] inline bool is_ui_busy(const AppState& state) noexcept {
    return state.is_loading || state.is_streaming || state.error_message.has_value();
}

/// Check if we're connected to anything (remote or bridge)
[[nodiscard]] inline bool is_connected_to_anything(const AppState& state) noexcept {
    return is_remote_connected(state) || is_repl_bridge_connected(state);
}

/// Check if any bridge-related status is active
[[nodiscard]] inline bool has_any_bridge_activity(const AppState& state) noexcept {
    return state.repl_bridge_enabled || 
           state.repl_bridge_connected || 
           state.repl_bridge_session_active ||
           state.repl_bridge_reconnecting;
}

/// Check if any task-related view is active
[[nodiscard]] inline bool is_any_task_view_active(const AppState& state) noexcept {
    return is_tasks_view_expanded(state) || 
           state.foregrounded_task_id.has_value() ||
           state.viewing_agent_task_id.has_value();
}

/// Check if we're in a multi-agent mode
[[nodiscard]] inline bool is_multi_agent_mode(const AppState& state) noexcept {
    return state.view_selection_mode == "selecting-agent" || 
           state.view_selection_mode == "viewing-agent";
}

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
