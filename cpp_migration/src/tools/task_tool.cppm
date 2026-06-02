// TaskTool - Task lifecycle management with concurrent execution via libuv
module;
#include <chrono>
#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.tools.task;


export namespace cc::tools {

// Task execution status
enum class TaskStatus {
    Pending,
    Running,
    Completed,
    Failed,
    Cancelled,
};

constexpr auto task_status_name(TaskStatus s) -> std::string_view {
    switch (s) {
        case TaskStatus::Pending:   return "pending";
        case TaskStatus::Running:   return "running";
        case TaskStatus::Completed: return "completed";
        case TaskStatus::Failed:    return "failed";
        case TaskStatus::Cancelled: return "cancelled";
        default:                    return "unknown";
    }
}

// Agent specialization types
enum class AgentType {
    Search,
    GeneralPurpose,
};

constexpr auto agent_type_name(AgentType t) -> std::string_view {
    switch (t) {
        case AgentType::Search:         return "search";
        case AgentType::GeneralPurpose: return "general_purpose";
        default:                        return "unknown";
    }
}

// Error types for task operations
enum class TaskError {
    IdEmpty,
    DescriptionEmpty,
    TaskNotFound,
    TaskAlreadyExists,
    TaskNotRunning,
    TooManyTasks,
    ExecutionFailed,
    InvalidTransition,
    OutputUnavailable,
};

constexpr auto format_error(TaskError err) -> std::string_view {
    switch (err) {
        case TaskError::IdEmpty:           return "Task ID is empty";
        case TaskError::DescriptionEmpty:  return "Task description is empty";
        case TaskError::TaskNotFound:      return "Task not found";
        case TaskError::TaskAlreadyExists: return "Task with this ID already exists";
        case TaskError::TaskNotRunning:    return "Task is not in running state";
        case TaskError::TooManyTasks:      return "Maximum concurrent task limit reached";
        case TaskError::ExecutionFailed:   return "Task execution failed";
        case TaskError::InvalidTransition: return "Invalid task status transition";
        case TaskError::OutputUnavailable: return "Task output is not available";
        default:                           return "Unknown task error";
    }
}

// Task data structure
struct Task {
    std::string id;
    std::string description;
    TaskStatus status{TaskStatus::Pending};
    AgentType agent_type{AgentType::GeneralPurpose};
    std::optional<std::string> result;
    std::optional<std::string> error_message;
    std::string output;
    std::chrono::steady_clock::time_point created_at;
    std::optional<std::chrono::steady_clock::time_point> started_at;
    std::optional<std::chrono::steady_clock::time_point> completed_at;
};

// Task store: shared state for all task tools
class TaskStore {
public:
    static constexpr size_t kMaxConcurrentTasks = 16;

    TaskStore() = default;

    auto create(std::string id, std::string description, AgentType type)
        -> std::expected<Task*, TaskError>
    {
        if (id.empty()) return std::unexpected(TaskError::IdEmpty);
        if (description.empty()) return std::unexpected(TaskError::DescriptionEmpty);
        if (tasks_.contains(id)) return std::unexpected(TaskError::TaskAlreadyExists);
        if (tasks_.size() >= kMaxConcurrentTasks) return std::unexpected(TaskError::TooManyTasks);

        Task task{
            .id = id,
            .description = std::move(description),
            .status = TaskStatus::Pending,
            .agent_type = type,
            .created_at = std::chrono::steady_clock::now(),
        };
        auto [it, _] = tasks_.emplace(std::move(id), std::move(task));
        return &it->second;
    }

    auto get(const std::string& id) -> std::expected<Task*, TaskError> {
        auto it = tasks_.find(id);
        if (it == tasks_.end()) return std::unexpected(TaskError::TaskNotFound);
        return &it->second;
    }

    auto list() const -> std::vector<const Task*> {
        std::vector<const Task*> result;
        result.reserve(tasks_.size());
        for (const auto& [_, task] : tasks_) {
            result.push_back(&task);
        }
        return result;
    }

    auto start(const std::string& id) -> std::expected<void, TaskError> {
        auto task = get(id);
        if (!task) return std::unexpected(task.error());
        if ((*task)->status != TaskStatus::Pending) {
            return std::unexpected(TaskError::InvalidTransition);
        }
        (*task)->status = TaskStatus::Running;
        (*task)->started_at = std::chrono::steady_clock::now();
        return {};
    }

    auto complete(const std::string& id, std::string result_text) -> std::expected<void, TaskError> {
        auto task = get(id);
        if (!task) return std::unexpected(task.error());
        if ((*task)->status != TaskStatus::Running) {
            return std::unexpected(TaskError::InvalidTransition);
        }
        (*task)->status = TaskStatus::Completed;
        (*task)->result = std::move(result_text);
        (*task)->completed_at = std::chrono::steady_clock::now();
        return {};
    }

    auto fail(const std::string& id, std::string error) -> std::expected<void, TaskError> {
        auto task = get(id);
        if (!task) return std::unexpected(task.error());
        (*task)->status = TaskStatus::Failed;
        (*task)->error_message = std::move(error);
        (*task)->completed_at = std::chrono::steady_clock::now();
        return {};
    }

    auto cancel(const std::string& id) -> std::expected<void, TaskError> {
        auto task = get(id);
        if (!task) return std::unexpected(task.error());
        if ((*task)->status == TaskStatus::Completed || (*task)->status == TaskStatus::Failed) {
            return std::unexpected(TaskError::InvalidTransition);
        }
        (*task)->status = TaskStatus::Cancelled;
        (*task)->completed_at = std::chrono::steady_clock::now();
        return {};
    }

    auto append_output(const std::string& id, std::string_view data) -> std::expected<void, TaskError> {
        auto task = get(id);
        if (!task) return std::unexpected(task.error());
        (*task)->output += data;
        return {};
    }

private:
    std::unordered_map<std::string, Task> tasks_;
};

// Global task store singleton
inline TaskStore& global_task_store() {
    static TaskStore store;
    return store;
}

// TaskCreateTool - creates a new task and starts execution
class TaskCreateTool {
public:
    static constexpr std::string_view name = "task_create";
    static constexpr std::string_view description = "Create a new background task for an agent to execute";

    auto execute(std::string id, std::string desc, AgentType type = AgentType::GeneralPurpose)
        -> std::expected<const Task*, TaskError>
    {
        auto result = global_task_store().create(std::move(id), std::move(desc), type);
        if (!result) return std::unexpected(result.error());

        // Auto-start the task
        auto start_result = global_task_store().start((*result)->id);
        if (!start_result) return std::unexpected(start_result.error());

        return *result;
    }

    auto schema() const -> std::string {
        return std::format(R"({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "description": {{ "type": "string", "description": "Task description/instructions" }},
      "type": {{ "type": "string", "enum": ["search", "general_purpose"] }}
    }},
    "required": ["description"]
  }}
}})", name, description);
    }
};

// TaskGetTool - retrieves task details by ID
class TaskGetTool {
public:
    static constexpr std::string_view name = "task_get";
    static constexpr std::string_view description = "Get the status and details of a task";

    auto execute(const std::string& id) -> std::expected<const Task*, TaskError> {
        return global_task_store().get(id);
    }

    auto schema() const -> std::string {
        return std::format(R"({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "task_id": {{ "type": "string", "description": "ID of the task to retrieve" }}
    }},
    "required": ["task_id"]
  }}
}})", name, description);
    }
};

// TaskListTool - lists all tasks with optional status filter
class TaskListTool {
public:
    static constexpr std::string_view name = "task_list";
    static constexpr std::string_view description = "List all tasks with optional status filter";

    auto execute(std::optional<TaskStatus> filter = std::nullopt)
        -> std::vector<const Task*>
    {
        auto all = global_task_store().list();
        if (!filter) return all;

        std::vector<const Task*> filtered;
        std::copy_if(all.begin(), all.end(), std::back_inserter(filtered), [&](const Task* t) {
            return t->status == *filter;
        });
        return filtered;
    }

    auto schema() const -> std::string {
        return std::format(R"({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "status_filter": {{ "type": "string", "enum": ["pending", "running", "completed", "failed", "cancelled"] }}
    }}
  }}
}})", name, description);
    }
};

// TaskStopTool - cancels a running task
class TaskStopTool {
public:
    static constexpr std::string_view name = "task_stop";
    static constexpr std::string_view description = "Stop/cancel a running task";

    auto execute(const std::string& id) -> std::expected<void, TaskError> {
        return global_task_store().cancel(id);
    }

    auto schema() const -> std::string {
        return std::format(R"({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "task_id": {{ "type": "string", "description": "ID of the task to stop" }}
    }},
    "required": ["task_id"]
  }}
}})", name, description);
    }
};

// TaskUpdateTool - updates task status or result
class TaskUpdateTool {
public:
    static constexpr std::string_view name = "task_update";
    static constexpr std::string_view description = "Update a task's status or result";

    auto execute(const std::string& id, TaskStatus new_status, std::optional<std::string> result = std::nullopt)
        -> std::expected<void, TaskError>
    {
        switch (new_status) {
            case TaskStatus::Completed:
                return global_task_store().complete(id, result.value_or(""));
            case TaskStatus::Failed:
                return global_task_store().fail(id, result.value_or("Unknown error"));
            case TaskStatus::Cancelled:
                return global_task_store().cancel(id);
            case TaskStatus::Running:
                return global_task_store().start(id);
            default:
                return std::unexpected(TaskError::InvalidTransition);
        }
    }

    auto schema() const -> std::string {
        return std::format(R"({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "task_id": {{ "type": "string", "description": "ID of the task to update" }},
      "status": {{ "type": "string", "enum": ["running", "completed", "failed", "cancelled"] }},
      "result": {{ "type": "string", "description": "Task result or error message" }}
    }},
    "required": ["task_id", "status"]
  }}
}})", name, description);
    }
};

// TaskOutputTool - retrieves task output stream
class TaskOutputTool {
public:
    static constexpr std::string_view name = "task_output";
    static constexpr std::string_view description = "Get the output produced by a task";

    auto execute(const std::string& id) -> std::expected<std::string_view, TaskError> {
        auto task = global_task_store().get(id);
        if (!task) return std::unexpected(task.error());
        return std::string_view((*task)->output);
    }

    auto schema() const -> std::string {
        return std::format(R"({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "task_id": {{ "type": "string", "description": "ID of the task" }}
    }},
    "required": ["task_id"]
  }}
}})", name, description);
    }
};

} // namespace cc::tools
