/// @file stop_task.cppm
/// @brief Shared logic for stopping a running task.
/// Used by TaskStopTool (LLM-invoked) and SDK stop_task control request.
/// Migrated from src/tasks/stopTask.ts
module;

#include <string>
#include <optional>
#include <functional>
#include <stdexcept>
#include <format>

export module cc.tasks.stop_task;

import cc.tasks.task;
import cc.tasks.types;

export namespace cc::tasks {

// ============================================================
// Error Types
// ============================================================

/// Error codes for task stop failures
enum class StopTaskErrorCode : std::uint8_t {
    NotFound,
    NotRunning,
    UnsupportedType,
};

/// Exception thrown when a task cannot be stopped
class StopTaskError : public std::runtime_error {
    StopTaskErrorCode code_;

public:
    StopTaskError(const std::string& message, StopTaskErrorCode code)
        : std::runtime_error(message), code_(code) {}
    
    [[nodiscard]] StopTaskErrorCode code() const noexcept { return code_; }
};

// ============================================================
// Result Types
// ============================================================

/// Result returned after successfully stopping a task
struct StopTaskResult {
    std::string task_id;
    std::string task_type;
    std::optional<std::string> command;
};

// ============================================================
// Stop Task Logic
// ============================================================

/// Context needed to stop a task
struct StopTaskContext {
    /// Get a task by ID (returns nullptr if not found)
    std::function<const cc::core::TaskStateBase*(const std::string&)> get_task;
    /// Kill a task by type and ID
    std::function<void(const std::string&, cc::core::TaskType)> kill_task;
    /// Update a task's notified flag
    std::function<void(const std::string&, std::function<void(cc::core::TaskStateBase&)>)> update_task;
};

/// Look up a task by ID, validate it is running, kill it, and mark it as notified.
///
/// Throws StopTaskError when the task cannot be stopped (not found,
/// not running, or unsupported type). Callers can inspect error.code() to
/// distinguish the failure reason.
[[nodiscard]] inline StopTaskResult stop_task(
    const std::string& task_id,
    const StopTaskContext& context
) {
    // Look up the task
    const auto* task = context.get_task(task_id);
    if (!task) {
        throw StopTaskError(
            std::format("No task found with ID: {}", task_id),
            StopTaskErrorCode::NotFound
        );
    }
    
    // Validate it's running
    if (task->status != cc::core::TaskStatus::Running) {
        throw StopTaskError(
            std::format("Task {} is not running (status: {})", task_id,
                static_cast<int>(task->status)),
            StopTaskErrorCode::NotRunning
        );
    }
    
    // Kill the task via its type-specific implementation
    context.kill_task(task_id, task->type);
    
    // For shell tasks: suppress the "exit code 137" notification (noise)
    // Agent tasks: don't suppress - the notification carries extractPartialResult
    if (task->type == cc::core::TaskType::LocalBash) {
        context.update_task(task_id, [](cc::core::TaskStateBase& t) {
            t.notified = true;
        });
    }
    
    // Determine command string for result
    std::optional<std::string> command;
    if (task->type == cc::core::TaskType::LocalBash) {
        command = task->description;  // Shell tasks use command as description
    } else {
        command = task->description;
    }
    
    return StopTaskResult{
        .task_id = task_id,
        .task_type = std::string(cc::core::task_type_to_string(task->type)),
        .command = command,
    };
}

} // namespace cc::tasks
