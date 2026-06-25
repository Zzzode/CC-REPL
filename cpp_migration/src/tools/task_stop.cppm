// TaskStopTool - Stops a running background task by ID
module;
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.tools.task_stop;

import cc.tools.tool;
import cc.utils.json;
import cc.utils.error;

export namespace cc::tools::task_stop {

using cc::core::Tool;
using cc::core::ToolInput;
using cc::core::ToolResult;
using cc::core::ToolDefinition;
using cc::core::ToolPermission;
using cc::core::InputSchema;
using cc::core::SchemaProperty;

/// Input parameters for TaskStopTool
struct TaskStopInput {
    std::optional<std::string> task_id;   // The ID of the background task to stop
    std::optional<std::string> shell_id;  // Deprecated: backward compat with KillShell

    /// Get effective ID (task_id takes precedence over shell_id)
    [[nodiscard]] auto effective_id() const -> std::optional<std::string> {
        if (task_id.has_value()) return task_id;
        return shell_id;
    }

    static std::expected<TaskStopInput, std::string> from_json(std::string_view json);
};

/// Output result for TaskStopTool
struct TaskStopOutput {
    std::string message;    // Status message about the operation
    std::string task_id;    // The ID of the task that was stopped
    std::string task_type;  // The type of the task that was stopped
    std::optional<std::string> command; // The command/description of the stopped task
};

/// TaskStopTool - Stops a running background task
/// Also known as KillShell (deprecated alias for backward compatibility)
class TaskStopTool {
public:
    static constexpr std::string_view kName = "TaskStop";
    static constexpr std::string_view kDescription =
        "Stop a running background task by ID. Supports both task_id and "
        "legacy shell_id parameter for backward compatibility.";

    /// Deprecated alias for backward compatibility
    static constexpr std::string_view kAlias = "KillShell";

    static ToolDefinition definition() {
        return ToolDefinition{
            .name = std::string(kName),
            .description = std::string(kDescription),
            .input_schema = InputSchema{
                .properties = {
                    SchemaProperty{
                        .name = "task_id",
                        .type = "string",
                        .description = "The ID of the background task to stop",
                        .required = false,
                        .default_value = std::nullopt,
                        .enum_values = std::nullopt
                    },
                    SchemaProperty{
                        .name = "shell_id",
                        .type = "string",
                        .description = "Deprecated: use task_id instead",
                        .required = false,
                        .default_value = std::nullopt,
                        .enum_values = std::nullopt
                    },
                }
            },
            .permission = ToolPermission::Write,
            .category = "tasks"
        };
    }

    [[nodiscard]] auto execute(const ToolInput& input) -> cc::utils::Result<ToolResult>;

    /// Validate that the task exists and is running
    [[nodiscard]] auto validate_input(const TaskStopInput& input) -> std::optional<std::string>;

    /// Check if a task is currently running
    [[nodiscard]] auto is_task_running(const std::string& task_id) -> bool;

    /// Perform the actual stop operation
    [[nodiscard]] auto stop_task(const std::string& task_id) -> std::expected<TaskStopOutput, std::string>;
};

} // namespace cc::tools::task_stop

export namespace cc::tools {
    using cc::tools::task_stop::TaskStopTool;
    using cc::tools::task_stop::TaskStopInput;
    using cc::tools::task_stop::TaskStopOutput;
}
