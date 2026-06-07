// TeamCreateTool - Creates a new multi-agent swarm team for parallel coordination
module;
#include <algorithm>
#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.tools.team_create;

import cc.tools.tool;
import cc.tools.runtime_registry;
import cc.tools.team;
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

namespace detail {

[[nodiscard]] std::optional<std::string> json_string_field(
    cc::utils::json::JsonVal object,
    std::string_view key
) {
    auto value = object.get(key);
    if (!value.is_str()) return std::nullopt;
    auto text = std::string(value.as_str());
    return text.empty() ? std::nullopt : std::optional<std::string>{std::move(text)};
}

} // namespace detail

std::expected<TeamCreateInput, std::string> TeamCreateInput::from_json(std::string_view json) {
    auto parsed = cc::utils::json::parse(json);
    if (!parsed || !parsed->root().is_obj()) return std::unexpected("team_create input must be a JSON object");

    auto root = parsed->root();
    TeamCreateInput input{
        .team_name = detail::json_string_field(root, "team_name")
            .or_else([&] { return detail::json_string_field(root, "name"); })
            .value_or(""),
        .description = detail::json_string_field(root, "description"),
        .agent_type = detail::json_string_field(root, "agent_type"),
    };
    if (input.team_name.empty()) return std::unexpected("team_create requires team_name");
    return input;
}

cc::utils::Result<ToolResult> TeamCreateTool::execute(const ToolInput& input) {
    auto parsed = TeamCreateInput::from_json(input.json());
    if (!parsed) return ToolResult::error(parsed.error());
    if (auto error = validate_input(*parsed)) return ToolResult::error(*error);

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto delegated = registry.execute("team_create", input);
    if (!delegated) return ToolResult::error(delegated.error().format());
    return std::move(*delegated);
}

bool TeamCreateTool::is_enabled() {
    return true;
}

std::optional<std::string> TeamCreateTool::validate_input(const TeamCreateInput& input) {
    if (input.team_name.empty()) return "team_name is required";
    if (input.team_name.size() > 128) return "team_name is too long";
    return std::nullopt;
}

std::string TeamCreateTool::generate_unique_team_name(const std::string& provided_name) {
    auto base = provided_name.empty() ? std::string{"team"} : provided_name;
    if (!cc::tools::global_team_store().get_by_id_or_name(base)) return base;

    for (int suffix = 2; suffix < 10'000; ++suffix) {
        auto candidate = std::format("{}-{}", base, suffix);
        if (!cc::tools::global_team_store().get_by_id_or_name(candidate)) return candidate;
    }
    return std::format("{}-{}", base, std::chrono::steady_clock::now().time_since_epoch().count());
}

std::expected<std::string, std::string> TeamCreateTool::write_team_file(
    const std::string& team_name,
    const TeamFile& file
) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto path = cc::tools::team_runtime_dir() / (cc::tools::safe_team_filename(team_name) + ".json");
    fs::create_directories(path.parent_path(), ec);
    if (ec) return std::unexpected(std::format("failed to create team directory: {}", ec.message()));

    std::ofstream out(path, std::ios::trunc);
    if (!out) return std::unexpected(std::format("failed to write team file: {}", path.string()));
    out << R"({"name":")" << cc::tools::team_json_escape(file.name)
        << R"(","lead_agent_id":")" << cc::tools::team_json_escape(file.lead_agent_id)
        << R"(","lead_session_id":")" << cc::tools::team_json_escape(file.lead_session_id)
        << R"(","members":[)";
    for (std::size_t i = 0; i < file.members.size(); ++i) {
        const auto& member = file.members[i];
        if (i != 0) out << ',';
        out << R"({"agent_id":")" << cc::tools::team_json_escape(member.agent_id)
            << R"(","name":")" << cc::tools::team_json_escape(member.name)
            << R"(","agent_type":")" << cc::tools::team_json_escape(member.agent_type)
            << R"(","model":")" << cc::tools::team_json_escape(member.model)
            << R"(","tmux_pane_id":")" << cc::tools::team_json_escape(member.tmux_pane_id)
            << R"(","cwd":")" << cc::tools::team_json_escape(member.cwd)
            << R"("})";
    }
    out << "]}";
    if (!out.good()) return std::unexpected(std::format("failed to write team file: {}", path.string()));
    return path.string();
}

void TeamCreateTool::register_for_cleanup(const std::string&) {}

} // namespace cc::tools::team_create

export namespace cc::tools {
    using cc::tools::team_create::TeamCreateInput;
    using cc::tools::team_create::TeamCreateOutput;
    using cc::tools::team_create::TeamFile;
    using cc::tools::team_create::TeamMemberInfo;
}
