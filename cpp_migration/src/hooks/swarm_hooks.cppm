/// @file swarm_hooks.cppm
/// @brief Multi-agent orchestration state management for the UI layer.
/// Tracks active agents, message streams, handoff events, health/status,
/// conversation routing, and parallel execution progress.
module;

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.hooks.swarm_hooks;


export namespace cc::hooks {

// ============================================================
// Agent status and identification
// ============================================================

/// Current status of an agent in the swarm
enum class AgentStatus : std::uint8_t {
    Idle,
    Thinking,
    Executing,
    Waiting,
    Error,
    Completed,
};

/// Convert AgentStatus to display string
[[nodiscard]] constexpr auto agent_status_to_string(AgentStatus status) noexcept -> std::string_view {
    switch (status) {
        case AgentStatus::Idle:      return "idle";
        case AgentStatus::Thinking:  return "thinking";
        case AgentStatus::Executing: return "executing";
        case AgentStatus::Waiting:   return "waiting";
        case AgentStatus::Error:     return "error";
        case AgentStatus::Completed: return "completed";
    }
    return "unknown";
}

/// Information about a single agent in the swarm
struct AgentInfo {
    std::string id;
    std::string name;
    std::string role;
    AgentStatus status{AgentStatus::Idle};
    std::chrono::system_clock::time_point created_at;
    std::optional<std::string> current_task;
    std::optional<std::string> last_message;
    std::optional<std::string> error_message;
    std::size_t message_count{0};
    std::size_t tool_use_count{0};

    /// Check if agent has finished (completed or error)
    [[nodiscard]] auto is_terminal() const -> bool {
        return status == AgentStatus::Completed || status == AgentStatus::Error;
    }

    /// Get human-readable duration since creation
    [[nodiscard]] auto uptime_seconds() const -> std::size_t {
        auto now = std::chrono::system_clock::now();
        return static_cast<std::size_t>(
            std::chrono::duration_cast<std::chrono::seconds>(now - created_at).count());
    }
};

// ============================================================
// Handoff event
// ============================================================

/// Represents a task handoff between agents
struct HandoffEvent {
    std::string from_agent_id;
    std::string to_agent_id;
    std::string context;
    std::chrono::system_clock::time_point timestamp;
    std::optional<std::string> reason;
};

// ============================================================
// Execution graph node (for parallel tasks visualization)
// ============================================================

/// Node in the execution dependency graph
struct ExecutionNode {
    std::string agent_id;
    std::string task_description;
    AgentStatus status{AgentStatus::Idle};
    std::vector<std::string> depends_on;
    float progress{0.0f};
};

// ============================================================
// SwarmState - complete state of the swarm UI hook
// ============================================================

/// Full state of the multi-agent swarm for the UI layer
struct SwarmState {
    std::vector<AgentInfo> agents;
    std::optional<std::string> active_agent_id;
    std::vector<HandoffEvent> pending_handoffs;
    std::vector<HandoffEvent> handoff_history;
    std::vector<ExecutionNode> execution_graph;
    std::chrono::system_clock::time_point started_at;
    bool is_active{false};
};

// ============================================================
// Callback types
// ============================================================

/// Callback for agent status change events
using AgentEventCallback = std::function<void(const std::string& agent_id, AgentStatus new_status)>;
/// Callback for handoff events
using HandoffCallback = std::function<void(const HandoffEvent&)>;
/// Callback for swarm completion
using SwarmCompleteCallback = std::function<void(bool success)>;

// ============================================================
// SwarmHook - multi-agent UI state manager
// ============================================================

/// Manages the UI-layer state for multi-agent orchestration.
/// Provides reactive queries for rendering agent status panels,
/// execution progress indicators, and handoff notifications.
class SwarmHook {
public:
    SwarmHook() = default;

    // ─── Agent queries ─────────────────────────────────────────

    /// Get all active (non-terminal) agents
    [[nodiscard]] auto get_active_agents() const -> std::vector<AgentInfo> {
        std::vector<AgentInfo> active;
        for (const auto& agent : state_.agents) {
            if (!agent.is_terminal()) {
                active.push_back(agent);
            }
        }
        return active;
    }

    /// Get all agents (active and completed)
    [[nodiscard]] auto get_all_agents() const -> const std::vector<AgentInfo>& {
        return state_.agents;
    }

    /// Get a specific agent's status by ID
    [[nodiscard]] auto get_agent_status(std::string_view id) const -> std::optional<AgentStatus> {
        for (const auto& agent : state_.agents) {
            if (agent.id == id) return agent.status;
        }
        return std::nullopt;
    }

    /// Get full info for a specific agent
    [[nodiscard]] auto get_agent_info(std::string_view id) const -> std::optional<AgentInfo> {
        for (const auto& agent : state_.agents) {
            if (agent.id == id) return agent;
        }
        return std::nullopt;
    }

    /// Get the currently focused/active agent
    [[nodiscard]] auto get_focused_agent() const -> std::optional<AgentInfo> {
        if (!state_.active_agent_id) return std::nullopt;
        return get_agent_info(*state_.active_agent_id);
    }

    // ─── Agent lifecycle management ────────────────────────────

    /// Register a new agent in the swarm
    auto register_agent(AgentInfo info) -> void {
        state_.agents.push_back(std::move(info));
        if (!state_.is_active) {
            state_.is_active = true;
            state_.started_at = std::chrono::system_clock::now();
        }
    }

    /// Update an agent's status
    auto update_agent_status(std::string_view id, AgentStatus new_status) -> void {
        for (auto& agent : state_.agents) {
            if (agent.id == id) {
                agent.status = new_status;

                notify_agent_event(agent.id, new_status);
                break;
            }
        }
    }

    /// Update an agent's last message
    auto update_agent_message(std::string_view id, std::string message) -> void {
        for (auto& agent : state_.agents) {
            if (agent.id == id) {
                agent.last_message = std::move(message);
                agent.message_count++;
                break;
            }
        }
    }

    // ─── Handoff management ────────────────────────────────────

    /// Process a handoff from one agent to another
    auto handle_handoff(std::string_view from, std::string_view to,
                        std::string context) -> void {
        HandoffEvent event{
            .from_agent_id = std::string(from),
            .to_agent_id = std::string(to),
            .context = std::move(context),
            .timestamp = std::chrono::system_clock::now(),
        };
        state_.handoff_history.push_back(event);
        state_.active_agent_id = std::string(to);


        update_agent_status(from, AgentStatus::Completed);
        update_agent_status(to, AgentStatus::Thinking);


        if (on_handoff_) on_handoff_(event);
    }

    /// Get pending handoffs awaiting processing
    [[nodiscard]] auto get_pending_handoffs() const -> const std::vector<HandoffEvent>& {
        return state_.pending_handoffs;
    }

    // ─── Execution progress ────────────────────────────────────

    /// Get overall execution progress [0.0, 1.0]
    [[nodiscard]] auto get_execution_progress() const -> float {
        if (state_.agents.empty()) return 0.0f;
        std::size_t completed = 0;
        for (const auto& agent : state_.agents) {
            if (agent.is_terminal()) completed++;
        }
        return static_cast<float>(completed) / static_cast<float>(state_.agents.size());
    }

    /// Get the execution dependency graph for visualization
    [[nodiscard]] auto get_execution_graph() const -> const std::vector<ExecutionNode>& {
        return state_.execution_graph;
    }

    /// Set the execution graph (typically from coordinator)
    auto set_execution_graph(std::vector<ExecutionNode> graph) -> void {
        state_.execution_graph = std::move(graph);
    }

    // ─── Event subscriptions ───────────────────────────────────

    /// Subscribe to status change events for a specific agent
    auto subscribe_agent_events(std::string_view id, AgentEventCallback callback) -> void {
        agent_callbacks_[std::string(id)] = std::move(callback);
    }

    /// Subscribe to all handoff events
    auto subscribe_handoffs(HandoffCallback callback) -> void {
        on_handoff_ = std::move(callback);
    }

    /// Subscribe to swarm completion
    auto subscribe_completion(SwarmCompleteCallback callback) -> void {
        on_complete_ = std::move(callback);
    }

    // ─── Agent control ─────────────────────────────────────────

    /// Cancel a specific agent
    auto cancel_agent(std::string_view id) -> void {
        for (auto& agent : state_.agents) {
            if (agent.id == id && !agent.is_terminal()) {
                agent.status = AgentStatus::Error;
                agent.error_message = "Cancelled by user";
                notify_agent_event(agent.id, AgentStatus::Error);
                break;
            }
        }
        check_completion();
    }

    /// Cancel all running agents
    auto cancel_all() -> void {
        for (auto& agent : state_.agents) {
            if (!agent.is_terminal()) {
                agent.status = AgentStatus::Error;
                agent.error_message = "Cancelled (swarm shutdown)";
            }
        }
        state_.is_active = false;
        if (on_complete_) on_complete_(false);
    }

    /// Set the focused/active agent for UI display
    auto set_active_agent(std::string_view id) -> void {
        state_.active_agent_id = std::string(id);
    }

    // ─── State access ──────────────────────────────────────────

    [[nodiscard]] auto state() const -> const SwarmState& { return state_; }
    [[nodiscard]] auto is_active() const -> bool { return state_.is_active; }
    [[nodiscard]] auto agent_count() const -> std::size_t { return state_.agents.size(); }

    [[nodiscard]] auto active_agent_count() const -> std::size_t {
        return static_cast<std::size_t>(std::count_if(
            state_.agents.begin(), state_.agents.end(), [](const AgentInfo& a) { return !a.is_terminal(); }));
    }

private:
    SwarmState state_;
    std::unordered_map<std::string, AgentEventCallback> agent_callbacks_;
    HandoffCallback on_handoff_;
    SwarmCompleteCallback on_complete_;

    // ─── Internal helpers ──────────────────────────────────────

    /// Notify subscribers of an agent event
    auto notify_agent_event(const std::string& id, AgentStatus status) -> void {
        auto it = agent_callbacks_.find(id);
        if (it != agent_callbacks_.end() && it->second) {
            it->second(id, status);
        }
    }

    /// Check if all agents are done and fire completion callback
    auto check_completion() -> void {
        bool all_done = std::all_of(
            state_.agents.begin(), state_.agents.end(), [](const AgentInfo& a) { return a.is_terminal(); });
        if (all_done && state_.is_active) {
            state_.is_active = false;
            bool success = std::any_of(
                state_.agents.begin(), state_.agents.end(), [](const AgentInfo& a) {
                    return a.status == AgentStatus::Completed;
                });
            if (on_complete_) on_complete_(success);
        }
    }
};

} // namespace cc::hooks
