module;
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <sstream>
#include <span>
#include <algorithm>

export module cc.tools.task_list;

import cc.tools.task_get;

export namespace cc::tools {


struct TaskFilter {
    std::optional<std::string> status;
    std::optional<std::string> assignee;
    std::optional<std::string> tag;
};


inline auto list_tasks(const TaskFilter& filter) -> std::vector<TaskInfo> {
    std::vector<TaskInfo> tasks;
    for (const auto& task : detail::task_store) {
        if (filter.status && task.status != *filter.status) continue;
        if (filter.assignee && task.assignee != filter.assignee) continue;
        tasks.push_back(task);
    }

    return tasks;
}


inline auto get_active_tasks() -> std::vector<TaskInfo> {
    TaskFilter filter;
    filter.status = "pending";


    auto pending = list_tasks(TaskFilter{.status = "pending"});
    auto in_progress = list_tasks(TaskFilter{.status = "in_progress"});

    std::vector<TaskInfo> active;
    active.reserve(pending.size() + in_progress.size());
    active.insert(active.end(), pending.begin(), pending.end());
    active.insert(active.end(), in_progress.begin(), in_progress.end());


    std::sort(active.begin(), active.end(),
        [](const TaskInfo& a, const TaskInfo& b) {
            return a.created > b.created;
        });

    return active;
}


inline auto format_task_list(std::span<const TaskInfo> tasks) -> std::string {
    if (tasks.empty()) {
        return "No tasks found.";
    }

    std::ostringstream oss;
    oss << "Tasks (" << tasks.size() << "):\n";
    oss << std::string(40, '-') << "\n";

    for (const auto& task : tasks) {

        if (task.status == "completed") {
            oss << "✓ ";
        } else if (task.status == "in_progress") {
            oss << "▶ ";
        } else {
            oss << "○ ";
        }


        oss << "[" << task.id << "] ";

        if (task.description.size() > 60) {
            oss << task.description.substr(0, 57) << "...";
        } else {
            oss << task.description;
        }


        if (task.assignee.has_value()) {
            oss << " (@" << *task.assignee << ")";
        }

        oss << "\n";
    }

    return oss.str();
}

} // namespace cc::tools
