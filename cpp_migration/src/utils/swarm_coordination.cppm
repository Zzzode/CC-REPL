// C++23 Module: Swarm Coordination
// Migrates: src/utils/swarm/ coordination files
// (swarmCoordinator.ts, swarmWorker.ts, swarmScheduler.ts, swarmMessageBus.ts,
//  swarmState.ts, swarmPermissions.ts, swarmDiscovery.ts, swarmHealth.ts)
module;

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <deque>
#include <unordered_map>

export module cc.utils.swarm_coordination;

export namespace cc::utils::swarm {

// --- Enums ---

enum class WorkerRole : uint8_t {
    Leader,
    Worker,
    Observer
};

enum class WorkerState : uint8_t {
    Idle,
    Working,
    Blocked,
    Waiting,
    Done,
    Failed
};

// --- Data Structures ---

struct SwarmMessage {
    std::string from;
    std::string to;
    std::string type;
    std::string payload;
    uint64_t sequence{0};
    std::chrono::system_clock::time_point timestamp{std::chrono::system_clock::now()};
};

struct WorkerInfo {
    std::string id;
    std::string name;
    WorkerRole role{WorkerRole::Worker};
    WorkerState state{WorkerState::Idle};
    std::optional<std::string> current_task;
    size_t tasks_completed{0};
    std::chrono::steady_clock::time_point joined_at{std::chrono::steady_clock::now()};
};

struct SwarmConfig {
    size_t max_workers{5};
    std::chrono::seconds task_timeout{300};
    std::chrono::seconds health_check_interval{30};
    bool auto_scale{false};
    std::string coordination_strategy{"round_robin"};
};

struct TaskAssignment {
    std::string task_id;
    std::string worker_id;
    std::string description;
    int priority{0};
    std::chrono::system_clock::time_point assigned_at{std::chrono::system_clock::now()};
};

struct HealthStatus {
    std::string worker_id;
    bool healthy{true};
    std::optional<std::string> error;
    std::chrono::steady_clock::time_point last_heartbeat{std::chrono::steady_clock::now()};
    size_t pending_tasks{0};
};

// --- Internal State ---

namespace detail {

struct SwarmState {
    std::mutex mutex;
    std::string swarm_id;
    SwarmConfig config;
    std::unordered_map<std::string, WorkerInfo> workers;
    std::vector<TaskAssignment> task_queue;
    std::deque<SwarmMessage> message_bus;
    std::unordered_map<std::string, HealthStatus> health;
    uint64_t next_worker_id{1};
    uint64_t next_msg_seq{1};
    static constexpr size_t MAX_MESSAGES = 500;
};

inline auto get_state() -> SwarmState& {
    static SwarmState state;
    return state;
}

} // namespace detail

// --- Functions ---

/// Create a new swarm with the given configuration.
/// Returns the swarm_id on success.
[[nodiscard]] inline auto create_swarm(SwarmConfig config = {})
    -> std::expected<std::string, std::string>
{
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    if (!state.swarm_id.empty()) {
        return std::unexpected(std::string{"Swarm already exists: " + state.swarm_id});
    }

    state.config = std::move(config);
    state.swarm_id = "swarm-" + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count() % 100000);
    state.workers.clear();
    state.task_queue.clear();
    state.message_bus.clear();
    state.health.clear();

    return state.swarm_id;
}

/// Join an existing swarm with the specified role.
[[nodiscard]] inline auto join_swarm(std::string_view swarm_id, WorkerRole role)
    -> std::expected<WorkerInfo, std::string>
{
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    if (state.swarm_id.empty() || state.swarm_id != swarm_id) {
        return std::unexpected("Swarm '" + std::string(swarm_id) + "' not found");
    }

    if (state.workers.size() >= state.config.max_workers) {
        return std::unexpected(std::string{"Swarm is at maximum capacity"});
    }

    // Check leader uniqueness
    if (role == WorkerRole::Leader) {
        for (const auto& [_, w] : state.workers) {
            if (w.role == WorkerRole::Leader) {
                return std::unexpected(std::string{"Swarm already has a leader"});
            }
        }
    }

    auto id = "worker-" + std::to_string(state.next_worker_id++);
    WorkerInfo info{
        .id = id,
        .name = id,
        .role = role,
        .state = WorkerState::Idle,
        .current_task = std::nullopt,
        .tasks_completed = 0,
        .joined_at = std::chrono::steady_clock::now(),
    };

    state.workers[id] = info;
    HealthStatus health;
    health.worker_id = id;
    health.healthy = true;
    state.health[id] = health;

    return info;
}

/// Leave the swarm gracefully.
inline void leave_swarm(std::string_view worker_id) {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    state.workers.erase(std::string(worker_id));
    state.health.erase(std::string(worker_id));

    // Unassign any tasks belonging to this worker
    for (auto& task : state.task_queue) {
        if (task.worker_id == worker_id) {
            task.worker_id.clear();
        }
    }
}

/// Assign a task to a worker.
[[nodiscard]] inline auto assign_task(TaskAssignment assignment)
    -> std::expected<void, std::string>
{
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    if (!assignment.worker_id.empty()) {
        auto it = state.workers.find(assignment.worker_id);
        if (it == state.workers.end()) {
            return std::unexpected("Worker '" + assignment.worker_id + "' not found");
        }
        it->second.state = WorkerState::Working;
        it->second.current_task = assignment.task_id;
    }

    state.task_queue.push_back(std::move(assignment));
    return {};
}

/// Get all workers currently in the swarm.
[[nodiscard]] inline auto get_workers() -> std::vector<WorkerInfo> {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    std::vector<WorkerInfo> result;
    result.reserve(state.workers.size());
    for (const auto& [_, w] : state.workers) {
        result.push_back(w);
    }
    return result;
}

/// Send a message to a specific worker.
[[nodiscard]] inline auto send_swarm_message(SwarmMessage msg)
    -> std::expected<void, std::string>
{
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    if (!msg.to.empty() && msg.to != "*") {
        if (state.workers.find(msg.to) == state.workers.end()) {
            return std::unexpected("Target worker '" + msg.to + "' not found");
        }
    }

    msg.sequence = state.next_msg_seq++;
    msg.timestamp = std::chrono::system_clock::now();

    while (state.message_bus.size() >= detail::SwarmState::MAX_MESSAGES) {
        state.message_bus.pop_front();
    }
    state.message_bus.push_back(std::move(msg));
    return {};
}

/// Broadcast a message to all workers.
inline void broadcast_message(std::string_view type, std::string_view payload) {
    SwarmMessage msg;
    msg.to = "*";
    msg.type = std::string(type);
    msg.payload = std::string(payload);
    (void)send_swarm_message(std::move(msg));
}

/// Check the health status of a specific worker.
[[nodiscard]] inline auto check_worker_health(std::string_view worker_id)
    -> HealthStatus
{
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    auto it = state.health.find(std::string(worker_id));
    if (it == state.health.end()) {
        return HealthStatus{.worker_id = std::string(worker_id), .healthy = false,
                           .error = "Worker not found"};
    }

    // Check if heartbeat is stale
    auto elapsed = std::chrono::steady_clock::now() - it->second.last_heartbeat;
    if (elapsed > state.config.health_check_interval * 3) {
        it->second.healthy = false;
        it->second.error = "Heartbeat timeout";
    }

    return it->second;
}

/// Get swarm statistics: (active_workers, total_workers).
[[nodiscard]] inline auto get_swarm_stats() -> std::pair<size_t, size_t> {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    size_t active = 0;
    for (const auto& [_, w] : state.workers) {
        if (w.state == WorkerState::Working || w.state == WorkerState::Idle) {
            active++;
        }
    }
    return {active, state.workers.size()};
}

/// Rebalance tasks across available workers.
[[nodiscard]] inline auto rebalance_tasks() -> std::vector<TaskAssignment> {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    std::vector<TaskAssignment> reassigned;

    // Find unassigned tasks
    std::vector<TaskAssignment*> unassigned;
    for (auto& task : state.task_queue) {
        if (task.worker_id.empty()) {
            unassigned.push_back(&task);
        }
    }

    if (unassigned.empty()) return reassigned;

    // Find idle workers
    std::vector<std::string> idle_workers;
    for (const auto& [id, w] : state.workers) {
        if (w.state == WorkerState::Idle && w.role != WorkerRole::Observer) {
            idle_workers.push_back(id);
        }
    }

    // Round-robin assignment
    size_t worker_idx = 0;
    for (auto* task : unassigned) {
        if (idle_workers.empty()) break;
        task->worker_id = idle_workers[worker_idx % idle_workers.size()];
        task->assigned_at = std::chrono::system_clock::now();

        auto wit = state.workers.find(task->worker_id);
        if (wit != state.workers.end()) {
            wit->second.state = WorkerState::Working;
            wit->second.current_task = task->task_id;
        }

        reassigned.push_back(*task);
        worker_idx++;
    }

    return reassigned;
}

/// Elect a new leader among the workers.
/// Returns the new leader's worker id.
[[nodiscard]] inline auto elect_leader() -> std::expected<std::string, std::string> {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    if (state.workers.empty()) {
        return std::unexpected(std::string{"No workers available for leader election"});
    }

    // Remove existing leader role
    for (auto& [_, w] : state.workers) {
        if (w.role == WorkerRole::Leader) {
            w.role = WorkerRole::Worker;
        }
    }

    // Elect the worker with most tasks completed (most experienced)
    std::string best_id;
    size_t best_tasks = 0;
    for (const auto& [id, w] : state.workers) {
        if (w.role == WorkerRole::Observer) continue;
        if (best_id.empty() || w.tasks_completed > best_tasks) {
            best_id = id;
            best_tasks = w.tasks_completed;
        }
    }

    if (best_id.empty()) {
        return std::unexpected(std::string{"No eligible workers for leader election"});
    }

    state.workers[best_id].role = WorkerRole::Leader;
    return best_id;
}

} // namespace cc::utils::swarm
