// TeamDeleteTool - Disbands a swarm team, cleans up directories and worktrees
module;
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.tools.team_delete;

import cc.tools.tool;
import cc.utils.json;
import cc.utils.error;

export namespace cc::tools::team_delete {

using cc::core::Tool;
using cc::core::ToolInput;
using cc::core::ToolResult;
using cc::core::ToolDefinition;
using cc::core::ToolPermission;
using cc::core::InputSchema;
using cc::core::SchemaProperty;

/// Input parameters for TeamDeleteTool (empty - operates on current team)
struct TeamDeleteInput {
    // No input required - operates on the current team context

    static std::expected<TeamDeleteInput, std::string> from_json(std::string_view json);
};

/// Output result for TeamDeleteTool
struct TeamDeleteOutput {
    bool success{false};
    std::string message;
    std::optional<std::string> team_name;
};

/// TeamDeleteTool - Cleans up team and task directories when the swarm is complete
class TeamDeleteTool {
public:
    static constexpr std::string_view kName = "TeamDelete";
    static constexpr std::string_view kDescription =
        "Clean up team and task directories when the swarm is complete. "
        "Disbands the current team, removes associated directories, and "
        "clears the team context from app state.";

    static ToolDefinition definition() {
        return ToolDefinition{
            .name = std::string(kName),
            .description = std::string(kDescription),
            .input_schema = InputSchema{
                .properties = {}  // No input parameters
            },
            .permission = ToolPermission::Write,
            .category = "team"
        };
    }

    [[nodiscard]] auto execute(const ToolInput& input) -> cc::utils::Result<ToolResult>;

    /// Check if agent swarms feature is enabled
    [[nodiscard]] static auto is_enabled() -> bool;

    /// Validate that the team can be deleted (no active members)
    [[nodiscard]] auto validate_can_delete() -> std::optional<std::string>;

    /// Get list of active (non-lead, non-idle) members
    [[nodiscard]] auto get_active_members(const std::string& team_name)
        -> std::vector<std::string>;

    /// Clean up team directories (team file, task dirs, worktrees)
    [[nodiscard]] auto cleanup_team_directories(const std::string& team_name)
        -> std::expected<void, std::string>;

    /// Clear team context from app state
    void clear_team_context();

    /// Unregister team from session cleanup
    void unregister_from_cleanup(const std::string& team_name);

    /// Clear teammate color assignments
    void clear_teammate_colors();
};

} // namespace cc::tools::team_delete

export namespace cc::tools {
    using cc::tools::team_delete::TeamDeleteTool;
    using cc::tools::team_delete::TeamDeleteInput;
    using cc::tools::team_delete::TeamDeleteOutput;
}
