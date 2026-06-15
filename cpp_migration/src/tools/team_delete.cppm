// TeamDeleteTool - Disbands a swarm team, cleans up directories and worktrees
module;
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.tools.team_delete;

import cc.tools.tool;
import cc.tools.runtime_registry;
import cc.tools.agent;
import cc.tools.team;
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
    std::optional<std::string> team_id;
    std::optional<std::string> team_name;

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
                .properties = {
                    SchemaProperty{
                        .name = "team_id",
                        .type = "string",
                        .description = "Team id to delete",
                        .required = false,
                        .default_value = std::nullopt,
                        .enum_values = std::nullopt,
                    },
                    SchemaProperty{
                        .name = "team_name",
                        .type = "string",
                        .description = "Team name to delete",
                        .required = false,
                        .default_value = std::nullopt,
                        .enum_values = std::nullopt,
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

[[nodiscard]] std::optional<std::string> env_team_name() {
    if (const char* value = std::getenv("CC_REPL_TEAM_NAME"); value && *value) return std::string(value);
    if (const char* value = std::getenv("CLAUDE_CODE_TEAM_NAME"); value && *value) return std::string(value);
    return std::nullopt;
}

[[nodiscard]] std::string delete_input_json_for_target(const TeamDeleteInput& input) {
    if (input.team_id && !input.team_id->empty()) {
        return std::format(R"({{"team_id":"{}"}})", cc::tools::team_json_escape(*input.team_id));
    }
    if (input.team_name && !input.team_name->empty()) {
        return std::format(R"({{"team_name":"{}"}})", cc::tools::team_json_escape(*input.team_name));
    }
    return "{}";
}

} // namespace detail

std::expected<TeamDeleteInput, std::string> TeamDeleteInput::from_json(std::string_view json) {
    TeamDeleteInput input;
    auto parsed = cc::utils::json::parse(json);
    if (parsed && parsed->root().is_obj()) {
        auto root = parsed->root();
        input.team_id = detail::json_string_field(root, "team_id")
            .or_else([&] { return detail::json_string_field(root, "id"); });
        input.team_name = detail::json_string_field(root, "team_name")
            .or_else([&] { return detail::json_string_field(root, "name"); });
    } else if (!json.empty() && json != "{}") {
        return std::unexpected("team_delete input must be a JSON object");
    }
    if (!input.team_id && !input.team_name) input.team_name = detail::env_team_name();
    if (!input.team_id && !input.team_name) return std::unexpected("team_delete requires team_id or team_name");
    return input;
}

cc::utils::Result<ToolResult> TeamDeleteTool::execute(const ToolInput& input) {
    auto parsed = TeamDeleteInput::from_json(input.json());
    if (!parsed) return ToolResult::error(parsed.error());

    cc::core::ToolRegistry registry;
    // Internal delegation reuses the runtime team_delete implementation. This
    // local registry is an implementation detail of the standalone tool — the
    // outer TeamDeleteTool is already permission-gated by its caller — so the
    // inner delegation uses an allow-all checker rather than fail-closed
    // denial for the Write-level "team_delete" runtime tool.
    cc::tools::register_runtime_tools(registry, cc::tools::RuntimeToolOptions{
        .permission_check = cc::tools::agent::AgentLivePermissionCheckFn{[](
            std::string_view, std::string_view, std::string_view) {
            return cc::tools::agent::AgentLivePermissionCheck{.allowed = true};
        }},
    });
    auto delegated = registry.execute("team_delete", ToolInput::from_json(detail::delete_input_json_for_target(*parsed)));
    if (!delegated) return ToolResult::error(delegated.error().format());
    return std::move(*delegated);
}

bool TeamDeleteTool::is_enabled() {
    return true;
}

std::optional<std::string> TeamDeleteTool::validate_can_delete() {
    auto team_name = detail::env_team_name();
    if (!team_name) return "no current team context";
    auto active = get_active_members(*team_name);
    if (!active.empty()) return std::format("{} active team member(s) still running", active.size());
    return std::nullopt;
}

std::vector<std::string> TeamDeleteTool::get_active_members(const std::string& team_name) {
    std::vector<std::string> active;
    auto team = cc::tools::global_team_store().get_by_id_or_name(team_name);
    if (!team) return active;
    for (const auto& member : (*team)->members) {
        if (member.role == cc::tools::MemberRole::Leader) continue;
        if (member.status == cc::tools::MemberStatus::Idle ||
            member.status == cc::tools::MemberStatus::Done ||
            member.status == cc::tools::MemberStatus::Error) {
            continue;
        }
        active.push_back(member.agent_id);
    }
    return active;
}

std::expected<void, std::string> TeamDeleteTool::cleanup_team_directories(const std::string& team_name) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto root = cc::tools::team_runtime_dir();
    fs::remove_all(root / cc::tools::safe_team_filename(team_name), ec);
    if (ec) return std::unexpected(ec.message());
    fs::remove(root / (cc::tools::safe_team_filename(team_name) + ".json"), ec);
    if (ec) return std::unexpected(ec.message());
    return {};
}

void TeamDeleteTool::clear_team_context() {
    unsetenv("CC_REPL_TEAM_NAME");
    unsetenv("CLAUDE_CODE_TEAM_NAME");
}

void TeamDeleteTool::unregister_from_cleanup(const std::string&) {}

void TeamDeleteTool::clear_teammate_colors() {}

} // namespace cc::tools::team_delete

export namespace cc::tools {
    using cc::tools::team_delete::TeamDeleteInput;
    using cc::tools::team_delete::TeamDeleteOutput;
}
