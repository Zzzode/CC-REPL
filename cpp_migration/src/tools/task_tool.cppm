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

import cc.tools.agent_runtime;

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
            .result = std::nullopt,
            .error_message = std::nullopt,
            .output = {},
            .created_at = std::chrono::steady_clock::now(),
            .started_at = std::nullopt,
            .completed_at = std::nullopt
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

[[nodiscard]] inline bool native_record_is_task(const agent_runtime::NativeAgentRecord& record) {
    return record.background;
}

[[nodiscard]] inline TaskStatus native_task_status(agent_runtime::NativeAgentStatus status) {
    switch (status) {
        case agent_runtime::NativeAgentStatus::Queued: return TaskStatus::Pending;
        case agent_runtime::NativeAgentStatus::Running: return TaskStatus::Running;
        case agent_runtime::NativeAgentStatus::Completed: return TaskStatus::Completed;
        case agent_runtime::NativeAgentStatus::Failed: return TaskStatus::Failed;
        case agent_runtime::NativeAgentStatus::Cancelled: return TaskStatus::Cancelled;
    }
    return TaskStatus::Pending;
}

[[nodiscard]] inline AgentType native_task_agent_type(std::string_view agent_type) {
    return agent_type == "search" ? AgentType::Search : AgentType::GeneralPurpose;
}

[[nodiscard]] inline std::string native_task_description(const agent_runtime::NativeAgentRecord& record) {
    if (record.description && !record.description->empty()) return *record.description;
    if (record.name && !record.name->empty()) return *record.name;
    return std::format("Agent {}", record.agent_type.empty() ? std::string{"general-purpose"} : record.agent_type);
}

[[nodiscard]] inline std::string native_task_output_text(const agent_runtime::NativeAgentRecord& record) {
    std::string output;
    if (record.output && !record.output->empty()) {
        output += *record.output;
    } else if (record.error && !record.error->empty()) {
        output += *record.error;
    }
    if (!record.transcript.empty()) {
        if (!output.empty()) output += "\n\n";
        output += "Transcript:\n";
        for (const auto& line : record.transcript) {
            output += line + "\n";
        }
    }
    if (record.output_file_path && !record.output_file_path->empty()) {
        if (!output.empty()) output += "\n";
        output += "output_file: " + *record.output_file_path + "\n";
    }
    return output;
}

[[nodiscard]] inline Task native_task_from_record(const agent_runtime::NativeAgentRecord& record) {
    return Task{
        .id = record.agent_id,
        .description = native_task_description(record),
        .status = native_task_status(record.status),
        .agent_type = native_task_agent_type(record.agent_type),
        .result = record.output,
        .error_message = record.error,
        .output = native_task_output_text(record),
        .created_at = std::chrono::steady_clock::now(),
        .started_at = std::nullopt,
        .completed_at = std::nullopt
    };
}

[[nodiscard]] inline std::string xml_escape_task_text(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

[[nodiscard]] inline std::optional<std::string> native_task_terminal_status(TaskStatus status) {
    switch (status) {
        case TaskStatus::Completed: return "completed";
        case TaskStatus::Failed: return "failed";
        case TaskStatus::Cancelled: return "stopped";
        case TaskStatus::Pending:
        case TaskStatus::Running:
            return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string native_task_notification(const agent_runtime::NativeAgentRecord& record) {
    auto status = native_task_terminal_status(native_task_status(record.status));
    if (!status) return {};
    auto output_file = record.output_file_path
        .or_else([&] { return record.transcript_path; })
        .value_or(agent_runtime::agent_output_file_path(record.agent_id).string());
    auto result = record.output && !record.output->empty()
        ? std::optional<std::string>{*record.output}
        : record.error;
    return std::format(
        "<task_notification>\n"
        "<task_id>{}</task_id>\n"
        "<output_file>{}</output_file>\n"
        "<status>{}</status>\n"
        "<summary>{}</summary>{}\n"
        "</task_notification>",
        xml_escape_task_text(record.agent_id),
        xml_escape_task_text(output_file),
        xml_escape_task_text(*status),
        xml_escape_task_text(native_task_description(record)),
        result && !result->empty()
            ? std::format("\n<result>{}</result>", xml_escape_task_text(*result))
            : std::string{});
}

[[nodiscard]] inline std::string native_task_full_output(const agent_runtime::NativeAgentRecord& record) {
    auto task = native_task_from_record(record);
    std::string out = std::format("{} [{}] {}", task.id, task_status_name(task.status), task.description);
    if (!task.output.empty()) out += "\n\nOutput:\n" + task.output;
    if (auto notification = native_task_notification(record); !notification.empty()) {
        out += "\n" + notification;
    }
    return out;
}

inline thread_local std::vector<Task> native_task_list_snapshot;
inline thread_local std::optional<Task> native_task_get_snapshot;
inline thread_local std::string native_task_output_snapshot;

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
        if (auto task = global_task_store().get(id)) {
            return *task;
        }
        if (auto record = agent_runtime::native_agent_store().get_by_task_id_or_remote_id(id);
            record && native_record_is_task(*record)) {
            native_task_get_snapshot = native_task_from_record(*record);
            return &*native_task_get_snapshot;
        }
        return std::unexpected(TaskError::TaskNotFound);
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
        std::vector<const Task*> listed;
        listed.reserve(all.size());
        if (!filter) {
            listed = std::move(all);
        } else {
            std::copy_if(all.begin(), all.end(), std::back_inserter(listed), [&](const Task* t) {
                return t->status == *filter;
            });
        }

        native_task_list_snapshot.clear();
        for (const auto& record : agent_runtime::native_agent_store().list()) {
            if (!native_record_is_task(record)) continue;
            auto task = native_task_from_record(record);
            if (filter && task.status != *filter) continue;
            native_task_list_snapshot.push_back(std::move(task));
        }
        listed.reserve(listed.size() + native_task_list_snapshot.size());
        for (const auto& task : native_task_list_snapshot) listed.push_back(&task);
        return listed;
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
        if (auto task = global_task_store().get(id)) {
            (void)task;
            return global_task_store().cancel(id);
        }
        if (auto record = agent_runtime::native_agent_store().get_by_task_id_or_remote_id(id);
            record && native_record_is_task(*record)) {
            agent_runtime::native_agent_store().request_cancel(record->agent_id, "stop requested");
            return {};
        }
        return std::unexpected(TaskError::TaskNotFound);
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
        if (task) return std::string_view((*task)->output);
        if (auto record = agent_runtime::native_agent_store().get_by_task_id_or_remote_id(id);
            record && native_record_is_task(*record)) {
            native_task_output_snapshot = native_task_full_output(*record);
            return std::string_view(native_task_output_snapshot);
        }
        return std::unexpected(TaskError::TaskNotFound);
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
