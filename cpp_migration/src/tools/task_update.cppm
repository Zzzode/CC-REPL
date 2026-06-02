// TaskUpdateTool - Updates task properties, status, dependencies, and ownership
module;
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module cc.tools.task_update;

import cc.tools.tool;
import cc.utils.json;
import cc.utils.error;

export namespace cc::tools::task_update {

using cc::core::Tool;
using cc::core::ToolInput;
using cc::core::ToolResult;
using cc::core::ToolDefinition;
using cc::core::ToolPermission;
using cc::core::InputSchema;
using cc::core::SchemaProperty;

/// Task status values (superset for update, includes 'deleted' as action)
enum class TaskUpdateStatus {
    Pending,
    InProgress,
    Completed,
    Deleted,
};

constexpr auto task_update_status_name(TaskUpdateStatus s) -> std::string_view {
    switch (s) {
        case TaskUpdateStatus::Pending:    return "pending";
        case TaskUpdateStatus::InProgress: return "in_progress";
        case TaskUpdateStatus::Completed:  return "completed";
        case TaskUpdateStatus::Deleted:    return "deleted";
        default:                           return "unknown";
    }
}

/// Input parameters for TaskUpdateTool
struct TaskUpdateInput {
    std::string task_id;                                   // Required: task to update
    std::optional<std::string> subject;                    // New subject/title
    std::optional<std::string> description;                // New description
    std::optional<std::string> active_form;                // Present continuous form for spinner
    std::optional<TaskUpdateStatus> status;                // New status
    std::optional<std::string> owner;                      // New owner (agent name)
    std::optional<std::vector<std::string>> add_blocks;    // Task IDs that this task blocks
    std::optional<std::vector<std::string>> add_blocked_by; // Task IDs that block this task
    std::optional<std::unordered_map<std::string, std::string>> metadata; // Metadata to merge

    static std::expected<TaskUpdateInput, std::string> from_json(std::string_view json);
};

/// Output result for TaskUpdateTool
struct TaskUpdateOutput {
    bool success{false};
    std::string task_id;
    std::vector<std::string> updated_fields;
    std::optional<std::string> error;
    struct StatusChange {
        std::string from;
        std::string to;
    };
    std::optional<StatusChange> status_change;
    bool verification_nudge_needed{false};
};

/// TaskUpdateTool - Updates existing tasks in the task list
class TaskUpdateTool {
public:
    static constexpr std::string_view kName = "TaskUpdate";
    static constexpr std::string_view kDescription =
        "Update an existing task's properties including status, subject, "
        "description, owner, dependencies, and metadata.";

    static ToolDefinition definition() {
        return ToolDefinition{
            .name = std::string(kName),
            .description = std::string(kDescription),
            .input_schema = InputSchema{
                .properties = {
                    SchemaProperty{
                        .name = "taskId",
                        .type = "string",
                        .description = "The ID of the task to update",
                        .required = true
                    },
                    SchemaProperty{
                        .name = "subject",
                        .type = "string",
                        .description = "New subject for the task",
                        .required = false
                    },
                    SchemaProperty{
                        .name = "description",
                        .type = "string",
                        .description = "New description for the task",
                        .required = false
                    },
                    SchemaProperty{
                        .name = "activeForm",
                        .type = "string",
                        .description = "Present continuous form shown in spinner when in_progress",
                        .required = false
                    },
                    SchemaProperty{
                        .name = "status",
                        .type = "string",
                        .description = "New status: pending, in_progress, completed, or deleted",
                        .required = false
                    },
                    SchemaProperty{
                        .name = "owner",
                        .type = "string",
                        .description = "New owner for the task",
                        .required = false
                    },
                    SchemaProperty{
                        .name = "addBlocks",
                        .type = "array",
                        .description = "Task IDs that this task blocks",
                        .required = false
                    },
                    SchemaProperty{
                        .name = "addBlockedBy",
                        .type = "array",
                        .description = "Task IDs that block this task",
                        .required = false
                    },
                    SchemaProperty{
                        .name = "metadata",
                        .type = "object",
                        .description = "Metadata keys to merge. Set a key to null to delete it.",
                        .required = false
                    },
                }
            },
            .permission = ToolPermission::Write,
            .category = "tasks"
        };
    }

    [[nodiscard]] auto execute(const ToolInput& input) -> cc::utils::Result<ToolResult>;

    /// Check if task exists before update
    [[nodiscard]] auto validate_input(const TaskUpdateInput& input) -> std::optional<std::string>;

private:
    /// Apply status change with lifecycle hooks
    auto apply_status_change(
        const std::string& task_id,
        TaskUpdateStatus new_status,
        TaskUpdateOutput& output
    ) -> std::optional<std::string>;

    /// Handle task deletion
    auto handle_delete(const std::string& task_id) -> TaskUpdateOutput;

    /// Notify new owner via mailbox when ownership changes
    void notify_owner_change(
        const std::string& task_id,
        const std::string& new_owner,
        const std::string& subject
    );
};

} // namespace cc::tools::task_update

export namespace cc::tools {
    using cc::tools::task_update::TaskUpdateTool;
    using cc::tools::task_update::TaskUpdateInput;
    using cc::tools::task_update::TaskUpdateOutput;
    using cc::tools::task_update::TaskUpdateStatus;
}
