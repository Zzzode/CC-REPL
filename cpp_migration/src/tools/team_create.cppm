// TeamCreateTool - Creates a new multi-agent swarm team for parallel coordination
module;
#include <chrono>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.tools.team_create;

import cc.tools.tool;
import cc.utils.json;
import cc.utils.error;

export namespace cc::tools::team_create {

using cc::core::Tool;
using cc::core::ToolInput;
using cc::core::ToolResult;
using cc::core::ToolDefinition;
using cc::core::ToolPermission;
using cc::core::InputSchema;
using cc::core::SchemaProperty;

/// Team member information at creation time
struct TeamMemberInfo {
    std::string agent_id;
    std::string name;
    std::string agent_type;
    std::string model;
    std::chrono::system_clock::time_point joined_at;
    std::string tmux_pane_id;
    std::string cwd;
    std::vector<std::string> subscriptions;
};

/// Team file structure persisted to disk
struct TeamFile {
    std::string name;
    std::optional<std::string> description;
    std::chrono::system_clock::time_point created_at;
    std::string lead_agent_id;
    std::string lead_session_id;
    std::vector<TeamMemberInfo> members;
};

/// Input parameters for TeamCreateTool
struct TeamCreateInput {
    std::string team_name;                    // Required: name for the new team
    std::optional<std::string> description;   // Optional: team description/purpose
    std::optional<std::string> agent_type;    // Optional: type/role of the team lead

    static std::expected<TeamCreateInput, std::string> from_json(std::string_view json);
};

/// Output result for TeamCreateTool
struct TeamCreateOutput {
    std::string team_name;
    std::string team_file_path;
    std::string lead_agent_id;
};

/// TeamCreateTool - Creates a new team for coordinating multiple agents
class TeamCreateTool {
public:
    static constexpr std::string_view kName = "TeamCreate";
    static constexpr std::string_view kDescription =
        "Create a new team for coordinating multiple agents. "
        "Sets up the team file, task list directory, and leader context.";

    static ToolDefinition definition() {
        return ToolDefinition{
            .name = std::string(kName),
            .description = std::string(kDescription),
            .input_schema = InputSchema{
                .properties = {
                    SchemaProperty{
                        .name = "team_name",
                        .type = "string",
                        .description = "Name for the new team to create",
                        .required = true
                    },
                    SchemaProperty{
                        .name = "description",
                        .type = "string",
                        .description = "Team description/purpose",
                        .required = false
                    },
                    SchemaProperty{
                        .name = "agent_type",
                        .type = "string",
                        .description = "Type/role of the team lead (e.g., researcher, test-runner)",
                        .required = false
                    },
                }
            },
            .permission = ToolPermission::Write,
            .category = "team"
        };
    }

    [[nodiscard]] auto execute(const ToolInput& input) -> cc::utils::Result<ToolResult>;

    /// Check if agent swarms feature is enabled
    [[nodiscard]] static auto is_enabled() -> bool;

    /// Validate input parameters
    [[nodiscard]] auto validate_input(const TeamCreateInput& input) -> std::optional<std::string>;

    /// Generate a unique team name if the provided one already exists
    [[nodiscard]] auto generate_unique_team_name(const std::string& provided_name) -> std::string;

    /// Write team file to disk
    [[nodiscard]] auto write_team_file(const std::string& team_name, const TeamFile& file)
        -> std::expected<std::string, std::string>;

    /// Register team for session cleanup on exit
    void register_for_cleanup(const std::string& team_name);
};

} // namespace cc::tools::team_create

export namespace cc::tools {
    using cc::tools::team_create::TeamCreateTool;
    using cc::tools::team_create::TeamCreateInput;
    using cc::tools::team_create::TeamCreateOutput;
    using cc::tools::team_create::TeamFile;
    using cc::tools::team_create::TeamMemberInfo;
}
