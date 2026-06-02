module;
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <chrono>

export module cc.tools.task_create;

import cc.tools.task_get;

export namespace cc::tools {

// 创建任务的输入参数
struct TaskCreateInput {
    std::string description;              // 任务描述（必填）
    std::optional<std::string> assignee;  // 指定负责人
    std::optional<std::string> priority;  // 优先级（high/medium/low）
    std::vector<std::string> tags;        // 标签列表
};

// 验证任务输入参数，返回错误列表
inline auto validate_task_input(const TaskCreateInput& input) -> std::vector<std::string> {
    std::vector<std::string> errors;

    // 描述不能为空
    if (input.description.empty()) {
        errors.emplace_back("Task description is required");
    }

    // 描述长度限制
    if (input.description.size() > 10000) {
        errors.emplace_back("Task description exceeds maximum length (10000 chars)");
    }

    // 验证优先级值
    if (input.priority.has_value()) {
        const auto& p = *input.priority;
        if (p != "high" && p != "medium" && p != "low") {
            errors.emplace_back("Invalid priority: must be 'high', 'medium', or 'low'");
        }
    }

    // 标签数量限制
    if (input.tags.size() > 20) {
        errors.emplace_back("Too many tags (maximum 20)");
    }

    // 验证标签格式
    for (const auto& tag : input.tags) {
        if (tag.empty()) {
            errors.emplace_back("Empty tag is not allowed");
        }
        if (tag.size() > 50) {
            errors.emplace_back("Tag too long: '" + tag.substr(0, 20) + "...' (max 50 chars)");
        }
    }

    return errors;
}

// 创建任务，返回任务 ID 或错误信息
inline auto create_task(const TaskCreateInput& input) -> std::expected<std::string, std::string> {
    // 先验证输入
    auto errors = validate_task_input(input);
    if (!errors.empty()) {
        return std::unexpected(errors.front());
    }

    // 生成任务 ID（基于时间戳）
    auto now = std::chrono::system_clock::now();
    auto epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();

    std::string task_id = "task_" + std::to_string(epoch);

    store_task(TaskInfo{
        .id = task_id,
        .description = input.description,
        .status = "pending",
        .assignee = input.assignee,
        .created = now,
        .completed = std::nullopt
    });
    return task_id;
}

// 获取创建任务工具的提示词
inline auto get_task_create_prompt() -> std::string {
    return R"(## TaskCreateTool

Create a new task for tracking work items, sub-tasks, or follow-up actions.

### Parameters:
- `description` (required): Clear description of what needs to be done
- `assignee` (optional): Who should work on this task
- `priority` (optional): "high", "medium", or "low" (default: "medium")
- `tags` (optional): Array of labels for categorization

### Usage:
- Create tasks for work that needs to be tracked independently
- Break down complex work into smaller trackable units
- Assign priority based on urgency and importance
- Use tags for filtering and organization

### Example:
```json
{
  "description": "Fix the login page redirect issue on Safari",
  "priority": "high",
  "tags": ["bug", "safari", "auth"]
}
```)";
}

} // namespace cc::tools
