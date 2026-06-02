// TeamTool - Team management for parallel multi-agent coordination
module;
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.tools.team;


export namespace cc::tools {

// Team member status
enum class MemberStatus {
    Idle,
    Working,
    Done,
    Error,
};

constexpr auto member_status_name(MemberStatus s) -> std::string_view {
    switch (s) {
        case MemberStatus::Idle:    return "idle";
        case MemberStatus::Working: return "working";
        case MemberStatus::Done:    return "done";
        case MemberStatus::Error:   return "error";
        default:                    return "unknown";
    }
}

// Team member role
enum class MemberRole {
    Leader,
    Worker,
    Reviewer,
};

constexpr auto member_role_name(MemberRole r) -> std::string_view {
    switch (r) {
        case MemberRole::Leader:   return "leader";
        case MemberRole::Worker:   return "worker";
        case MemberRole::Reviewer: return "reviewer";
        default:                   return "unknown";
    }
}

// Error types for team operations
enum class TeamError {
    IdEmpty,
    NameEmpty,
    TeamNotFound,
    TeamAlreadyExists,
    MemberNotFound,
    TooManyMembers,
    TooManyTeams,
    TaskListFull,
    InvalidOperation,
};

constexpr auto format_error(TeamError err) -> std::string_view {
    switch (err) {
        case TeamError::IdEmpty:           return "Team ID is empty";
        case TeamError::NameEmpty:         return "Team name is empty";
        case TeamError::TeamNotFound:      return "Team not found";
        case TeamError::TeamAlreadyExists: return "Team with this ID already exists";
        case TeamError::MemberNotFound:    return "Team member not found";
        case TeamError::TooManyMembers:    return "Maximum team member limit reached";
        case TeamError::TooManyTeams:      return "Maximum team limit reached";
        case TeamError::TaskListFull:      return "Shared task list is full";
        case TeamError::InvalidOperation:  return "Invalid operation on team";
        default:                           return "Unknown team error";
    }
}

// Team member representation
struct TeamMember {
    std::string agent_id;
    MemberRole role{MemberRole::Worker};
    MemberStatus status{MemberStatus::Idle};
    std::optional<std::string> current_task;
    std::optional<std::string> last_result;
};

// Shared task item for team coordination
struct SharedTaskItem {
    std::string id;
    std::string description;
    std::optional<std::string> assigned_to;
    bool completed{false};
    std::optional<std::string> result;
};

// Team structure
struct Team {
    std::string id;
    std::string name;
    std::vector<TeamMember> members;
    std::vector<SharedTaskItem> task_list;
    std::chrono::steady_clock::time_point created_at;
};

// Team store: manages all teams
class TeamStore {
public:
    static constexpr size_t kMaxTeams = 8;
    static constexpr size_t kMaxMembersPerTeam = 16;
    static constexpr size_t kMaxTasksPerTeam = 50;

    auto create(std::string id, std::string team_name, std::vector<TeamMember> members)
        -> std::expected<Team*, TeamError>
    {
        if (id.empty()) return std::unexpected(TeamError::IdEmpty);
        if (team_name.empty()) return std::unexpected(TeamError::NameEmpty);
        if (teams_.contains(id)) return std::unexpected(TeamError::TeamAlreadyExists);
        if (teams_.size() >= kMaxTeams) return std::unexpected(TeamError::TooManyTeams);
        if (members.size() > kMaxMembersPerTeam) return std::unexpected(TeamError::TooManyMembers);

        Team team{
            .id = id,
            .name = std::move(team_name),
            .members = std::move(members),
            .created_at = std::chrono::steady_clock::now(),
        };
        auto [it, _] = teams_.emplace(std::move(id), std::move(team));
        return &it->second;
    }

    auto get(const std::string& id) -> std::expected<Team*, TeamError> {
        auto it = teams_.find(id);
        if (it == teams_.end()) return std::unexpected(TeamError::TeamNotFound);
        return &it->second;
    }

    auto remove(const std::string& id) -> std::expected<void, TeamError> {
        if (!teams_.contains(id)) return std::unexpected(TeamError::TeamNotFound);
        teams_.erase(id);
        return {};
    }

    auto add_task(const std::string& team_id, SharedTaskItem task)
        -> std::expected<void, TeamError>
    {
        auto team = get(team_id);
        if (!team) return std::unexpected(team.error());
        if ((*team)->task_list.size() >= kMaxTasksPerTeam) {
            return std::unexpected(TeamError::TaskListFull);
        }
        (*team)->task_list.push_back(std::move(task));
        return {};
    }

    auto assign_task(const std::string& team_id, const std::string& task_id,
                     const std::string& agent_id) -> std::expected<void, TeamError>
    {
        auto team = get(team_id);
        if (!team) return std::unexpected(team.error());

        // Find the task
        auto task_it = std::ranges::find_if((*team)->task_list, [&](const auto& t) {
            return t.id == task_id;
        });
        if (task_it == (*team)->task_list.end()) {
            return std::unexpected(TeamError::InvalidOperation);
        }

        // Find the member
        auto member_it = std::ranges::find_if((*team)->members, [&](const auto& m) {
            return m.agent_id == agent_id;
        });
        if (member_it == (*team)->members.end()) {
            return std::unexpected(TeamError::MemberNotFound);
        }

        task_it->assigned_to = agent_id;
        member_it->current_task = task_id;
        member_it->status = MemberStatus::Working;
        return {};
    }

    auto complete_task(const std::string& team_id, const std::string& task_id,
                       std::string result) -> std::expected<void, TeamError>
    {
        auto team = get(team_id);
        if (!team) return std::unexpected(team.error());

        auto task_it = std::ranges::find_if((*team)->task_list, [&](const auto& t) {
            return t.id == task_id;
        });
        if (task_it == (*team)->task_list.end()) {
            return std::unexpected(TeamError::InvalidOperation);
        }

        task_it->completed = true;
        task_it->result = std::move(result);

        // Update member status
        if (task_it->assigned_to) {
            auto member_it = std::ranges::find_if((*team)->members, [&](const auto& m) {
                return m.agent_id == *task_it->assigned_to;
            });
            if (member_it != (*team)->members.end()) {
                member_it->status = MemberStatus::Done;
                member_it->last_result = task_it->result;
                member_it->current_task = std::nullopt;
            }
        }
        return {};
    }

private:
    std::unordered_map<std::string, Team> teams_;
};

// Global team store singleton
inline TeamStore& global_team_store() {
    static TeamStore store;
    return store;
}

// TeamCreateTool - creates a new team with members and shared task list
class TeamCreateTool {
public:
    static constexpr std::string_view name = "team_create";
    static constexpr std::string_view description = "Create a team of agents for parallel task execution";

    auto execute(std::string id, std::string team_name, std::vector<TeamMember> members)
        -> std::expected<const Team*, TeamError>
    {
        auto result = global_team_store().create(std::move(id), std::move(team_name), std::move(members));
        if (!result) return std::unexpected(result.error());
        return *result;
    }

    auto schema() const -> std::string {
        return std::format(R"({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "name": {{ "type": "string", "description": "Team name" }},
      "members": {{
        "type": "array",
        "items": {{
          "type": "object",
          "properties": {{
            "agent_id": {{ "type": "string" }},
            "role": {{ "type": "string", "enum": ["leader", "worker", "reviewer"] }}
          }},
          "required": ["agent_id"]
        }}
      }},
      "task_list": {{
        "type": "array",
        "items": {{
          "type": "object",
          "properties": {{
            "id": {{ "type": "string" }},
            "description": {{ "type": "string" }}
          }},
          "required": ["id", "description"]
        }}
      }}
    }},
    "required": ["name", "members"]
  }}
}})", name, description);
    }
};

// TeamDeleteTool - removes a team and cleans up resources
class TeamDeleteTool {
public:
    static constexpr std::string_view name = "team_delete";
    static constexpr std::string_view description = "Delete a team and release its resources";

    auto execute(const std::string& id) -> std::expected<void, TeamError> {
        return global_team_store().remove(id);
    }

    auto schema() const -> std::string {
        return std::format(R"({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "team_id": {{ "type": "string", "description": "ID of the team to delete" }}
    }},
    "required": ["team_id"]
  }}
}})", name, description);
    }
};

} // namespace cc::tools
