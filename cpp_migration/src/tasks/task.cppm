/// @file task.cppm
/// @brief Task system module for managing concurrent execution of shell commands, agent tasks,
/// monitors, and workflows.
/// Defines task types, task status, task handles, and task execution context.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <expected>
#include <chrono>
#include <atomic>
#include <mutex>
#include <variant>
#include <unordered_map>
#include <random>
#include <thread>

export module cc.tasks.task;

import cc.types.types;

export namespace cc::core {

// ============================================================
// Task Types
// ============================================================

/// Types of tasks supported by the engine
enum class TaskType : std::uint8_t {
    LocalBash,
    LocalAgent,
    RemoteAgent,
    InProcessTeammate,
    LocalWorkflow,
    MonitorMcp,
    Dream,
};

/// Convert TaskType to string
[[nodiscard]] constexpr std::string_view task_type_to_string(TaskType type) noexcept {
    switch (type) {
        case TaskType::LocalBash: return "local_bash";
        case TaskType::LocalAgent: return "local_agent";
        case TaskType::RemoteAgent: return "remote_agent";
        case TaskType::InProcessTeammate: return "in_process_teammate";
        case TaskType::LocalWorkflow: return "local_workflow";
        case TaskType::MonitorMcp: return "monitor_mcp";
        case TaskType::Dream: return "dream";
    }
    return "unknown";
}

/// Status of a task lifecycle
enum class TaskStatus : std::uint8_t {
    Pending,
    Running,
    Completed,
    Failed,
    Killed,
};

/// Check if a task is in a terminal (completed) state
[[nodiscard]] constexpr bool is_terminal_status(TaskStatus status) noexcept {
    return status == TaskStatus::Completed || 
           status == TaskStatus::Failed || 
           status == TaskStatus::Killed;
}

// ============================================================
// Task ID Generation
// ============================================================

/// Unique identifier for a task
struct TaskId {
    std::string value;

    auto operator<=>(const TaskId&) const = default;
    bool operator==(const TaskId&) const = default;

    [[nodiscard]] static TaskId generate(TaskType type) {
        static thread_local std::mt19937_64 rng{std::random_device{}()};
        std::uniform_int_distribution<std::uint64_t> dist;
        
        std::string prefix;
        switch (type) {
            case TaskType::LocalBash: prefix = "b"; break;
            case TaskType::LocalAgent: prefix = "a"; break;
            case TaskType::RemoteAgent: prefix = "r"; break;
            case TaskType::InProcessTeammate: prefix = "t"; break;
            case TaskType::LocalWorkflow: prefix = "w"; break;
            case TaskType::MonitorMcp: prefix = "m"; break;
            case TaskType::Dream: prefix = "d"; break;
            default: prefix = "x"; break;
        }
        
        std::string id = prefix;
        for (int i = 0; i < 8; ++i) {
            static constexpr char chars[] = "0123456789abcdefghijklmnopqrstuvwxyz";
            id += chars[dist(rng) % (sizeof(chars) - 1)];
        }
        return TaskId{id};
    }
};

// ============================================================
// Task Input Types
// ============================================================

/// Input for local bash task
struct LocalBashInput {
    std::string command;
    std::string description;
    std::optional<std::chrono::milliseconds> timeout;
    std::optional<std::string> tool_use_id;
    std::optional<std::string> agent_id;
    std::string kind = "bash"; // "bash" or "monitor"
};

// ============================================================
// Task State
// ============================================================

/// Base state shared by all task types
struct TaskStateBase {
    TaskId id;
    TaskType type;
    TaskStatus status;
    std::string description;
    std::optional<std::string> tool_use_id;
    std::chrono::system_clock::time_point start_time;
    std::optional<std::chrono::system_clock::time_point> end_time;
    std::optional<std::chrono::milliseconds> total_paused;
    std::string output_file;
    std::size_t output_offset;
    bool notified;
};

/// Complete task result returned by task execution
struct TaskResult {
    bool success;
    std::string output;
    std::optional<std::string> error;
    std::optional<int> exit_code;
    std::chrono::milliseconds duration;
};

// ============================================================
// Task Interface
// ============================================================

/// Type-erased interface for all task implementations
class ITask {
public:
    virtual ~ITask() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual TaskType type() const = 0;
    [[nodiscard]] virtual const TaskStateBase& state() const = 0;
    [[nodiscard]] virtual TaskStateBase& state_mut() = 0;
    
    /// Start task execution (async)
    virtual void start() = 0;
    
    /// Wait for task completion
    [[nodiscard]] virtual Result<TaskResult> wait() = 0;
    
    /// Kill a running task
    virtual void kill() = 0;
    
    /// Check if task is still running
    [[nodiscard]] virtual bool is_running() const = 0;
};

// ============================================================
// Local Bash Task Implementation
// ============================================================

/// Concrete implementation of a local bash command task
class LocalBashTask : public ITask {
    TaskStateBase state_;
    LocalBashInput input_;
    std::atomic<TaskStatus> status_;
    std::mutex mutex_;
    std::optional<TaskResult> result_;
    std::atomic<bool> killed_ = false;

public:
    explicit LocalBashTask(LocalBashInput input, std::string output_file)
        : input_(std::move(input)) {
        state_.id = TaskId::generate(TaskType::LocalBash);
        state_.type = TaskType::LocalBash;
        state_.status = TaskStatus::Pending;
        state_.description = input_.description;
        state_.tool_use_id = input_.tool_use_id;
        state_.start_time = std::chrono::system_clock::now();
        state_.output_file = std::move(output_file);
        state_.output_offset = 0;
        state_.notified = false;
        status_.store(TaskStatus::Pending);
    }

    [[nodiscard]] std::string name() const override { return "bash_task"; }
    [[nodiscard]] TaskType type() const override { return TaskType::LocalBash; }
    [[nodiscard]] const TaskStateBase& state() const override { return state_; }
    [[nodiscard]] TaskStateBase& state_mut() override { return state_; }

    void start() override {
        std::lock_guard lock(mutex_);
        if (status_.load() != TaskStatus::Pending) return;
        
        status_.store(TaskStatus::Running);
        state_.status = TaskStatus::Running;
        
        // In production, this would spawn a subprocess
        // For now, simulate execution
        state_.start_time = std::chrono::system_clock::now();
        
        // Simulate command execution
        result_ = TaskResult{
            true, "Command executed successfully", std::nullopt, 0, 
            std::chrono::milliseconds(100)};
        
        status_.store(TaskStatus::Completed);
        state_.status = TaskStatus::Completed;
        state_.end_time = std::chrono::system_clock::now();
    }

    [[nodiscard]] Result<TaskResult> wait() override {
        while (is_running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (result_) {
            return *result_;
        }
        return std::unexpected(Error::make(ErrorCode::InternalError, "Task did not produce a result"));
    }

    void kill() override {
        killed_.store(true);
        std::lock_guard lock(mutex_);
        if (status_.load() == TaskStatus::Running) {
            status_.store(TaskStatus::Killed);
            state_.status = TaskStatus::Killed;
            state_.end_time = std::chrono::system_clock::now();
        }
    }

    [[nodiscard]] bool is_running() const override {
        return status_.load() == TaskStatus::Running;
    }
};

// ============================================================
// Task Registry / Manager
// ============================================================

/// Manages all active tasks in the system
class TaskManager {
    std::unordered_map<std::string, std::unique_ptr<ITask>> tasks_;
    std::mutex mutex_;

public:
    TaskManager() = default;

    /// Register a new task
    void register_task(std::unique_ptr<ITask> task) {
        std::lock_guard lock(mutex_);
        tasks_[task->state().id.value] = std::move(task);
    }

    /// Get a task by ID
    [[nodiscard]] ITask* get_task(const std::string& id) {
        std::lock_guard lock(mutex_);
        auto it = tasks_.find(id);
        return it != tasks_.end() ? it->second.get() : nullptr;
    }

    /// Get all active tasks
    [[nodiscard]] std::vector<ITask*> get_all_tasks() {
        std::lock_guard lock(mutex_);
        std::vector<ITask*> result;
        result.reserve(tasks_.size());
        for (auto& [id, task] : tasks_) {
            result.push_back(task.get());
        }
        return result;
    }

    /// Kill a specific task
    [[nodiscard]] VoidResult kill_task(const std::string& id) {
        std::lock_guard lock(mutex_);
        auto it = tasks_.find(id);
        if (it == tasks_.end()) {
            return std::unexpected(Error::make(ErrorCode::SessionNotFound, 
                std::format("Task '{}' not found", id)));
        }
        it->second->kill();
        return {};
    }

    /// Remove a completed or killed task from the registry
    void remove_task(const std::string& id) {
        std::lock_guard lock(mutex_);
        auto it = tasks_.find(id);
        if (it != tasks_.end()) return;
        
        if (is_terminal_status(it->second->state().status)) {
            tasks_.erase(it);
        }
    }

    /// Clean up all completed/killed tasks
    void cleanup() {
        std::lock_guard lock(mutex_);
        for (auto it = tasks_.begin(); it != tasks_.end(); ) {
            if (is_terminal_status(it->second->state().status)) {
                it = tasks_.erase(it);
            } else {
                ++it;
            }
        }
    }

    /// Get count of active tasks
    [[nodiscard]] std::size_t active_count() const {
        std::lock_guard lock(const_cast<std::mutex&>(mutex_));
        std::size_t count = 0;
        for (const auto& [id, task] : tasks_) {
            if (task->state().status == TaskStatus::Running) {
                ++count;
            }
        }
        return count;
    }
};

} // namespace cc::core
