/// @file in_process_teammate_task.cppm
/// @brief In-process teammate task lifecycle management.
/// Migrated from src/tasks/InProcessTeammateTask/
module;

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <algorithm>

export module cc.tasks.in_process_teammate_task;

import cc.tasks.task;
import cc.tasks.types;

export namespace cc::tasks {

// ============================================================
// Teammate Task Lifecycle
// ============================================================

/// Callback type for updating teammate task state
using UpdateTeammateTaskFn = std::function<void(const std::string&, std::function<void(InProcessTeammateTaskState&)>)>;

/// Request shutdown for a teammate
inline void request_teammate_shutdown(
    const std::string& task_id,
    UpdateTeammateTaskFn update_task
) {
    update_task(task_id, [](InProcessTeammateTaskState& task) {
        if (task.status != cc::core::TaskStatus::Running || task.shutdown_requested) {
            return;
        }
        task.shutdown_requested = true;
    });
}

/// Append a message to a teammate's conversation history (for zoomed view)
inline void append_teammate_message(
    const std::string& task_id,
    const std::string& message,
    UpdateTeammateTaskFn update_task
) {
    update_task(task_id, [&](InProcessTeammateTaskState& task) {
        if (task.status != cc::core::TaskStatus::Running) return;
        // Keep a lightweight in-state copy for UI zoom/drain consumers until a
        // dedicated message store is attached to the task registry.
        task.pending_user_messages.push_back(message);
    });
}

/// Inject a user message to a teammate's pending queue
inline void inject_user_message_to_teammate(
    const std::string& task_id,
    const std::string& message,
    UpdateTeammateTaskFn update_task
) {
    update_task(task_id, [&](InProcessTeammateTaskState& task) {
        // Only reject if teammate is in a terminal state
        if (cc::core::is_terminal_status(task.status)) {
            return;
        }
        task.pending_user_messages.push_back(message);
    });
}

/// Get teammate task by agent ID, preferring running tasks
[[nodiscard]] inline std::optional<InProcessTeammateTaskState> find_teammate_task_by_agent_id(
    const std::string& agent_id,
    const std::vector<InProcessTeammateTaskState>& all_tasks
) {
    std::optional<InProcessTeammateTaskState> fallback;
    
    for (const auto& task : all_tasks) {
        if (task.identity.agent_id == agent_id) {
            // Prefer running tasks
            if (task.status == cc::core::TaskStatus::Running) {
                return task;
            }
            if (!fallback) {
                fallback = task;
            }
        }
    }
    return fallback;
}

/// Get all in-process teammate tasks
[[nodiscard]] inline std::vector<InProcessTeammateTaskState> get_all_in_process_teammate_tasks(
    const std::vector<cc::core::TaskStateBase*>& all_tasks
) {
    std::vector<InProcessTeammateTaskState> result;
    for (const auto* task : all_tasks) {
        if (task == nullptr || !is_in_process_teammate_task(*task)) continue;
        result.push_back(*static_cast<const InProcessTeammateTaskState*>(task));
    }
    return result;
}

/// Get running in-process teammates sorted alphabetically by agentName
[[nodiscard]] inline std::vector<InProcessTeammateTaskState> get_running_teammates_sorted(
    const std::vector<InProcessTeammateTaskState>& all_tasks
) {
    std::vector<InProcessTeammateTaskState> running;
    for (const auto& t : all_tasks) {
        if (t.status == cc::core::TaskStatus::Running) {
            running.push_back(t);
        }
    }
    std::sort(running.begin(), running.end(), 
        [](const auto& a, const auto& b) {
            return a.identity.agent_name < b.identity.agent_name;
        });
    return running;
}

/// Drain pending user messages from a teammate task
[[nodiscard]] inline std::vector<std::string> drain_teammate_pending_messages(
    const std::string& task_id,
    std::function<std::optional<InProcessTeammateTaskState>(const std::string&)> get_task,
    UpdateTeammateTaskFn update_task
) {
    auto task_opt = get_task(task_id);
    if (!task_opt || task_opt->pending_user_messages.empty()) {
        return {};
    }
    
    auto drained = task_opt->pending_user_messages;
    update_task(task_id, [](InProcessTeammateTaskState& task) {
        task.pending_user_messages.clear();
    });
    return drained;
}

} // namespace cc::tasks
