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

// 任务过滤条件
struct TaskFilter {
    std::optional<std::string> status;    // 按状态过滤
    std::optional<std::string> assignee;  // 按负责人过滤
    std::optional<std::string> tag;       // 按标签过滤
};

// 列出符合过滤条件的任务
inline auto list_tasks(const TaskFilter& filter) -> std::vector<TaskInfo> {
    std::vector<TaskInfo> tasks;
    for (const auto& task : detail::task_store) {
        if (filter.status && task.status != *filter.status) continue;
        if (filter.assignee && task.assignee != filter.assignee) continue;
        tasks.push_back(task);
    }

    return tasks;
}

// 获取所有活跃（非已完成）任务
inline auto get_active_tasks() -> std::vector<TaskInfo> {
    TaskFilter filter;
    filter.status = "pending"; // 或 in_progress

    // 合并 pending 和 in_progress 状态的任务
    auto pending = list_tasks(TaskFilter{.status = "pending"});
    auto in_progress = list_tasks(TaskFilter{.status = "in_progress"});

    std::vector<TaskInfo> active;
    active.reserve(pending.size() + in_progress.size());
    active.insert(active.end(), pending.begin(), pending.end());
    active.insert(active.end(), in_progress.begin(), in_progress.end());

    // 按创建时间排序（最新的在前）
    std::sort(active.begin(), active.end(),
        [](const TaskInfo& a, const TaskInfo& b) {
            return a.created > b.created;
        });

    return active;
}

// 格式化任务列表为可读字符串
inline auto format_task_list(std::span<const TaskInfo> tasks) -> std::string {
    if (tasks.empty()) {
        return "No tasks found.";
    }

    std::ostringstream oss;
    oss << "Tasks (" << tasks.size() << "):\n";
    oss << std::string(40, '-') << "\n";

    for (const auto& task : tasks) {
        // 状态图标
        if (task.status == "completed") {
            oss << "✓ ";
        } else if (task.status == "in_progress") {
            oss << "▶ ";
        } else {
            oss << "○ ";
        }

        // 任务 ID 和描述（截断过长的描述）
        oss << "[" << task.id << "] ";

        if (task.description.size() > 60) {
            oss << task.description.substr(0, 57) << "...";
        } else {
            oss << task.description;
        }

        // 负责人信息
        if (task.assignee.has_value()) {
            oss << " (@" << *task.assignee << ")";
        }

        oss << "\n";
    }

    return oss.str();
}

} // namespace cc::tools
