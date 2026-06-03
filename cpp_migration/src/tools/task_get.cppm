module;
#include <string>
#include <string_view>
#include <optional>
#include <expected>
#include <chrono>
#include <vector>
#include <utility>

export module cc.tools.task_get;

export namespace cc::tools {


struct TaskInfo {
    std::string id;
    std::string description;
    std::string status;
    std::optional<std::string> assignee;
    std::chrono::system_clock::time_point created;
    std::optional<std::chrono::system_clock::time_point> completed;
};

namespace detail {
inline std::vector<TaskInfo> task_store;
}

inline auto store_task(TaskInfo task) -> void {
    detail::task_store.push_back(std::move(task));
}

inline auto format_task_info(const TaskInfo& task) -> std::string;


inline auto get_task(std::string_view id) -> std::expected<TaskInfo, std::string> {
    if (id.empty()) {
        return std::unexpected(std::string("Task ID is required"));
    }


    if (!id.starts_with("task_")) {
        return std::unexpected(std::string("Invalid task ID format"));
    }

    for (const auto& task : detail::task_store) {
        if (task.id == id) return task;
    }
    return std::unexpected(std::string("Task not found: ") + std::string(id));
}


inline auto get_task_output(std::string_view id) -> std::expected<std::string, std::string> {
    if (id.empty()) {
        return std::unexpected(std::string("Task ID is required"));
    }

    if (!id.starts_with("task_")) {
        return std::unexpected(std::string("Invalid task ID format"));
    }

    for (const auto& task : detail::task_store) {
        if (task.id == id) return format_task_info(task);
    }
    return std::unexpected(std::string("Task output not available: ") + std::string(id));
}


inline auto format_task_info(const TaskInfo& task) -> std::string {
    std::string result;
    result += "ID: " + task.id + "\n";
    result += "Status: " + task.status + "\n";
    result += "Description: " + task.description + "\n";

    if (task.assignee.has_value()) {
        result += "Assignee: " + *task.assignee + "\n";
    }


    auto created_time = std::chrono::system_clock::to_time_t(task.created);
    result += "Created: " + std::string(std::ctime(&created_time));

    if (task.completed.has_value()) {
        auto completed_time = std::chrono::system_clock::to_time_t(*task.completed);
        result += "Completed: " + std::string(std::ctime(&completed_time));
    }

    return result;
}

} // namespace cc::tools
