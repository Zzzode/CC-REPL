module;

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

export module cc.utils.task_utils;

export namespace cc::utils {

// ─── Task Status & Schema ────────────────────────────────────────────────────

/// Task status enumeration
enum class TaskStatus {
    pending,
    in_progress,
    completed
};

/// Convert task status to string representation
std::string_view task_status_to_string(TaskStatus status);

/// Parse string to TaskStatus
std::expected<TaskStatus, std::string> parse_task_status(std::string_view s);

/// A single task record
struct Task {
    std::string id;
    std::string subject;
    std::string description;
    TaskStatus status = TaskStatus::pending;
    std::string owner;
    std::string active_form;
    std::vector<std::string> blocked_by;
    std::vector<std::string> blocks;
};

// ─── Task List Management ────────────────────────────────────────────────────

/// Signal-based notification for task list updates
using TaskUpdateCallback = std::function<void()>;

/// Register a listener called when tasks are updated. Returns unsubscribe function.
std::function<void()> on_tasks_updated(TaskUpdateCallback callback);

/// Notify all registered listeners that tasks have been updated
void notify_tasks_updated();

/// Set the leader's team name for task list resolution
void set_leader_team_name(std::string_view team_name);

/// Clear the leader's team name
void clear_leader_team_name();

/// Create a new task. Returns the assigned task ID.
std::expected<std::string, std::string> create_task(
    std::string_view subject,
    std::string_view description,
    std::string_view active_form);

/// Update an existing task's fields
std::expected<void, std::string> update_task(
    std::string_view task_id,
    std::optional<TaskStatus> status,
    std::optional<std::string_view> subject,
    std::optional<std::string_view> description);

/// Get a task by ID
std::expected<Task, std::string> get_task(std::string_view task_id);

/// List all tasks for the current session/team
std::expected<std::vector<Task>, std::string> list_tasks();

/// Delete a task by ID
std::expected<void, std::string> delete_task(std::string_view task_id);

/// Sanitize a path component (remove unsafe characters)
std::string sanitize_path_component(std::string_view input);

// ─── Queue Processor ─────────────────────────────────────────────────────────

/// Represents a queued command to be processed
struct QueuedCommand {
    std::string value;
    std::string mode;             // e.g., "prompt", "bash", "task-notification"
    std::optional<std::string> agent_id;  // If set, targeted at a subagent
};

/// Result of queue processing
struct ProcessQueueResult {
    bool processed = false;
};

/// Function type for executing input commands
using ExecuteInputFn = std::function<void(std::vector<QueuedCommand>)>;

/// Check if a queued command is a slash command (value starts with '/')
bool is_slash_command(const QueuedCommand& cmd);

/// Processes commands from the queue.
/// Slash commands and bash-mode commands are processed one at a time.
/// Other non-slash commands with the same mode are batched.
ProcessQueueResult process_queue_if_ready(ExecuteInputFn execute_input);

/// Check if the queue has pending commands
bool has_queued_commands();

// ─── Retry Logic ─────────────────────────────────────────────────────────────

/// Configuration for retry behavior
struct RetryConfig {
    size_t max_retries = 3;
    std::chrono::milliseconds initial_delay{100};
    std::chrono::milliseconds max_delay{5000};
    double backoff_multiplier = 2.0;
};

/// Execute a function with retry logic and exponential backoff.
/// Returns the successful result or the last error.
template<typename T>
std::expected<T, std::string> with_retry(
    std::function<std::expected<T, std::string>()> fn,
    const RetryConfig& config)
{
    std::expected<T, std::string> last_result = std::unexpected("No attempts made");
    auto delay = config.initial_delay;

    for (size_t attempt = 0; attempt <= config.max_retries; ++attempt) {
        last_result = fn();
        if (last_result.has_value()) {
            return last_result;
        }
        if (attempt < config.max_retries) {
            std::this_thread::sleep_for(delay);
            delay = std::chrono::milliseconds(
                static_cast<int64_t>(delay.count() * config.backoff_multiplier));
            if (delay > config.max_delay) {
                delay = config.max_delay;
            }
        }
    }
    return last_result;
}

/// Void specialization for retry
std::expected<void, std::string> with_retry_void(
    std::function<std::expected<void, std::string>()> fn,
    const RetryConfig& config);

} // namespace cc::utils
