// TaskOutputTool - Retrieves output from background tasks (shell, agent, etc.)
module;
#include <chrono>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module cc.tools.task_output;

import cc.tools.tool;
import cc.utils.json;
import cc.utils.error;

export namespace cc::tools::task_output {

using cc::core::Tool;
using cc::core::ToolInput;
using cc::core::ToolResult;
using cc::core::ToolDefinition;
using cc::core::ToolPermission;
using cc::core::InputSchema;
using cc::core::SchemaProperty;

/// Task types supported by the output tool
enum class TaskType {
    LocalBash,
    LocalAgent,
    RemoteAgent,
};

constexpr auto task_type_name(TaskType t) -> std::string_view {
    switch (t) {
        case TaskType::LocalBash:   return "local_bash";
        case TaskType::LocalAgent:  return "local_agent";
        case TaskType::RemoteAgent: return "remote_agent";
        default:                    return "unknown";
    }
}

/// Retrieval status of the output request
enum class RetrievalStatus {
    Success,
    Timeout,
    NotReady,
};

constexpr auto retrieval_status_name(RetrievalStatus s) -> std::string_view {
    switch (s) {
        case RetrievalStatus::Success:  return "success";
        case RetrievalStatus::Timeout:  return "timeout";
        case RetrievalStatus::NotReady: return "not_ready";
        default:                        return "unknown";
    }
}

/// Input parameters for TaskOutputTool
struct TaskOutputInput {
    std::string task_id;                     // Required: task to get output from
    bool block{true};                        // Whether to wait for completion
    std::chrono::milliseconds timeout{30000}; // Max wait time (0-600000ms)

    static std::expected<TaskOutputInput, std::string> from_json(std::string_view json);
};

/// Individual task output data
struct TaskOutputData {
    std::string task_id;
    TaskType task_type;
    std::string status;
    std::string description;
    std::string output;
    std::optional<int> exit_code;
    std::optional<std::string> error;
    // Agent-specific fields
    std::optional<std::string> prompt;
    std::optional<std::string> result;
};

/// Output result for TaskOutputTool
struct TaskOutputResult {
    RetrievalStatus retrieval_status;
    std::optional<TaskOutputData> task;
};

/// Progress update during blocking wait
struct TaskOutputProgress {
    std::string task_id;
    std::string status;
    std::string partial_output;
    std::chrono::milliseconds elapsed;
};

/// TaskOutputTool - Gets output from running or completed background tasks
class TaskOutputTool {
public:
    static constexpr std::string_view kName = "TaskOutput";
    static constexpr std::string_view kDescription =
        "Retrieve output from a background task. Can block until completion "
        "or return immediately with current state.";

    static ToolDefinition definition() {
        return ToolDefinition{
            .name = std::string(kName),
            .description = std::string(kDescription),
            .input_schema = InputSchema{
                .properties = {
                    SchemaProperty{
                        .name = "task_id",
                        .type = "string",
                        .description = "The task ID to get output from",
                        .required = true
                    },
                    SchemaProperty{
                        .name = "block",
                        .type = "boolean",
                        .description = "Whether to wait for task completion (default: true)",
                        .required = false
                    },
                    SchemaProperty{
                        .name = "timeout",
                        .type = "number",
                        .description = "Max wait time in milliseconds (0-600000, default: 30000)",
                        .required = false
                    },
                }
            },
            .permission = ToolPermission::ReadOnly,
            .category = "tasks"
        };
    }

    [[nodiscard]] auto execute(const ToolInput& input) -> cc::utils::Result<ToolResult>;

    /// Poll for task completion with timeout
    [[nodiscard]] auto wait_for_completion(
        const std::string& task_id,
        std::chrono::milliseconds timeout
    ) -> TaskOutputResult;

    /// Get current output without blocking
    [[nodiscard]] auto get_current_output(const std::string& task_id) -> TaskOutputResult;

    /// Format output for display based on task type
    [[nodiscard]] static auto format_output(const TaskOutputData& data) -> std::string;
};

} // namespace cc::tools::task_output

export namespace cc::tools {
    using cc::tools::task_output::TaskOutputTool;
    using cc::tools::task_output::TaskOutputInput;
    using cc::tools::task_output::TaskOutputResult;
    using cc::tools::task_output::TaskType;
    using cc::tools::task_output::RetrievalStatus;
}
