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


struct TaskCreateInput {
    std::string description;
    std::optional<std::string> assignee;
    std::optional<std::string> priority;
    std::vector<std::string> tags;
};


inline auto validate_task_input(const TaskCreateInput& input) -> std::vector<std::string> {
    std::vector<std::string> errors;


    if (input.description.empty()) {
        errors.emplace_back("Task description is required");
    }


    if (input.description.size() > 10000) {
        errors.emplace_back("Task description exceeds maximum length (10000 chars)");
    }


    if (input.priority.has_value()) {
        const auto& p = *input.priority;
        if (p != "high" && p != "medium" && p != "low") {
            errors.emplace_back("Invalid priority: must be 'high', 'medium', or 'low'");
        }
    }


    if (input.tags.size() > 20) {
        errors.emplace_back("Too many tags (maximum 20)");
    }


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


inline auto create_task(const TaskCreateInput& input) -> std::expected<std::string, std::string> {

    auto errors = validate_task_input(input);
    if (!errors.empty()) {
        return std::unexpected(errors.front());
    }


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
