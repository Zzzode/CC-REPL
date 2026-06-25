// TeamTool - Team management for parallel multi-agent coordination
module;
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <optional>
#include <ostream>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.tools.team;

import cc.utils.json;

export namespace cc::tools {

namespace fs = std::filesystem;

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

[[nodiscard]] inline std::optional<MemberStatus> member_status_from_string(std::string_view value) {
    if (value == "idle") return MemberStatus::Idle;
    if (value == "working") return MemberStatus::Working;
    if (value == "done") return MemberStatus::Done;
    if (value == "error") return MemberStatus::Error;
    return std::nullopt;
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

[[nodiscard]] inline std::optional<MemberRole> member_role_from_string(std::string_view value) {
    if (value == "leader") return MemberRole::Leader;
    if (value == "worker") return MemberRole::Worker;
    if (value == "reviewer") return MemberRole::Reviewer;
    return std::nullopt;
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
    std::string agent_id{};
    MemberRole role{MemberRole::Worker};
    MemberStatus status{MemberStatus::Idle};
    std::optional<std::string> current_task{};
    std::optional<std::string> last_result{};
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

[[nodiscard]] inline std::string team_json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\': out += R"(\\)"; break;
            case '"': out += R"(\")"; break;
            case '\n': out += R"(\n)"; break;
            case '\r': out += R"(\r)"; break;
            case '\t': out += R"(\t)"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

[[nodiscard]] inline fs::path team_runtime_dir() {
    if (const char* env = std::getenv("CC_REPL_TEAM_RUNTIME_DIR"); env && *env) {
        return fs::path{env};
    }
    return fs::current_path() / ".claude" / "teams";
}

[[nodiscard]] inline std::string safe_team_filename(std::string_view id) {
    std::string out;
    out.reserve(id.size());
    for (char ch : id) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.') {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "team" : out;
}

[[nodiscard]] inline fs::path team_record_path(std::string_view id) {
    return team_runtime_dir() / (safe_team_filename(id) + ".json");
}

inline void write_optional_string(std::ostream& out, std::string_view name, const std::optional<std::string>& value) {
    if (value) out << R"(,")" << name << R"(":")" << team_json_escape(*value) << '"';
}

inline bool persist_team_record(const Team& team) {
    std::error_code ec;
    fs::create_directories(team_runtime_dir(), ec);
    if (ec) return false;
    std::ofstream out(team_record_path(team.id), std::ios::trunc);
    if (!out) return false;
    out << R"({"id":")" << team_json_escape(team.id)
        << R"(","name":")" << team_json_escape(team.name)
        << R"(","members":[)";
    for (std::size_t i = 0; i < team.members.size(); ++i) {
        const auto& member = team.members[i];
        if (i != 0) out << ',';
        out << R"({"agent_id":")" << team_json_escape(member.agent_id)
            << R"(","role":")" << member_role_name(member.role)
            << R"(","status":")" << member_status_name(member.status) << '"';
        write_optional_string(out, "current_task", member.current_task);
        write_optional_string(out, "last_result", member.last_result);
        out << '}';
    }
    out << R"(],"task_list":[)";
    for (std::size_t i = 0; i < team.task_list.size(); ++i) {
        const auto& task = team.task_list[i];
        if (i != 0) out << ',';
        out << R"({"id":")" << team_json_escape(task.id)
            << R"(","description":")" << team_json_escape(task.description)
            << R"(","completed":)" << (task.completed ? "true" : "false");
        write_optional_string(out, "assigned_to", task.assigned_to);
        write_optional_string(out, "result", task.result);
        out << '}';
    }
    out << "]}";
    return out.good();
}

[[nodiscard]] inline std::optional<Team> load_team_record_from_path(const fs::path& path) {
    auto parsed = cc::utils::json::parse_file(path);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto root = parsed->root();
    auto id = root.get_string("id");
    auto name = root.get_string("name");
    if (id.empty() || name.empty()) return std::nullopt;

    Team team{
        .id = std::move(id),
        .name = std::move(name),
        .members = {},
        .task_list = {},
        .created_at = std::chrono::steady_clock::now(),
    };
    auto members = root.get("members");
    if (members.is_arr()) {
        members.iter([&](cc::utils::json::JsonVal item) {
            if (!item.is_obj()) return;
            auto agent_id = item.get_string("agent_id");
            if (agent_id.empty()) return;
            TeamMember member{
                .agent_id = std::move(agent_id),
                .role = member_role_from_string(item.get_string("role")).value_or(MemberRole::Worker),
                .status = member_status_from_string(item.get_string("status")).value_or(MemberStatus::Idle),
                .current_task = std::nullopt,
                .last_result = std::nullopt
            };
            auto current_task = item.get("current_task");
            if (current_task.is_str()) member.current_task = std::string(current_task.as_str());
            auto last_result = item.get("last_result");
            if (last_result.is_str()) member.last_result = std::string(last_result.as_str());
            team.members.push_back(std::move(member));
        });
    }
    auto tasks = root.get("task_list");
    if (tasks.is_arr()) {
        tasks.iter([&](cc::utils::json::JsonVal item) {
            if (!item.is_obj()) return;
            auto id = item.get_string("id");
            auto description = item.get_string("description");
            if (id.empty() || description.empty()) return;
            SharedTaskItem task{
                .id = std::move(id),
                .description = std::move(description),
                .assigned_to = std::nullopt,
                .completed = item.get("completed").is_bool() && item.get("completed").as_bool(),
                .result = std::nullopt
            };
            auto assigned_to = item.get("assigned_to");
            if (assigned_to.is_str()) task.assigned_to = std::string(assigned_to.as_str());
            auto result = item.get("result");
            if (result.is_str()) task.result = std::string(result.as_str());
            team.task_list.push_back(std::move(task));
        });
    }
    return team;
}

[[nodiscard]] inline std::optional<Team> load_team_record(std::string_view id) {
    return load_team_record_from_path(team_record_path(id));
}

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
            .task_list = {},
            .created_at = std::chrono::steady_clock::now(),
        };
        auto [it, _] = teams_.emplace(std::move(id), std::move(team));
        (void)persist_team_record(it->second);
        return &it->second;
    }

    auto get(const std::string& id) -> std::expected<Team*, TeamError> {
        auto it = teams_.find(id);
        if (it == teams_.end()) {
            auto loaded = load_team_record(id);
            if (!loaded) return std::unexpected(TeamError::TeamNotFound);
            auto [inserted, _] = teams_.emplace(loaded->id, std::move(*loaded));
            it = inserted;
        }
        return &it->second;
    }

    auto get_by_id_or_name(const std::string& key) -> std::expected<Team*, TeamError> {
        if (auto by_id = get(key)) return by_id;

        for (auto& [_, team] : teams_) {
            if (team.name == key) return &team;
        }

        std::error_code ec;
        if (!fs::exists(team_runtime_dir(), ec)) return std::unexpected(TeamError::TeamNotFound);
        for (const auto& entry : fs::directory_iterator(team_runtime_dir(), ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") continue;
            auto loaded = load_team_record_from_path(entry.path());
            if (!loaded) continue;
            auto id = loaded->id;
            auto it = teams_.find(id);
            if (it == teams_.end()) {
                auto [inserted, _] = teams_.emplace(std::move(id), std::move(*loaded));
                it = inserted;
            } else {
                it->second = std::move(*loaded);
            }
            if (it->second.id == key || it->second.name == key) return &it->second;
        }
        return std::unexpected(TeamError::TeamNotFound);
    }

    auto add_member(const std::string& team_key, TeamMember member) -> std::expected<void, TeamError> {
        auto team = get_by_id_or_name(team_key);
        if (!team) return std::unexpected(team.error());
        if (member.agent_id.empty()) return std::unexpected(TeamError::MemberNotFound);

        auto existing = std::ranges::find_if((*team)->members, [&](const auto& candidate) {
            return candidate.agent_id == member.agent_id;
        });
        if (existing != (*team)->members.end()) {
            existing->role = member.role;
            existing->status = member.status;
            existing->current_task = std::move(member.current_task);
            existing->last_result = std::move(member.last_result);
            (void)persist_team_record(**team);
            return {};
        }

        if ((*team)->members.size() >= kMaxMembersPerTeam) {
            return std::unexpected(TeamError::TooManyMembers);
        }
        (*team)->members.push_back(std::move(member));
        (void)persist_team_record(**team);
        return {};
    }

    auto remove(const std::string& id) -> std::expected<void, TeamError> {
        if (!teams_.contains(id) && !fs::exists(team_record_path(id))) {
            return std::unexpected(TeamError::TeamNotFound);
        }
        teams_.erase(id);
        std::error_code ec;
        fs::remove(team_record_path(id), ec);
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
        (void)persist_team_record(**team);
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
        (void)persist_team_record(**team);
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
        (void)persist_team_record(**team);
        return {};
    }

    auto update_member_status(
        const std::string& team_key,
        const std::string& agent_id,
        MemberStatus status,
        std::optional<std::string> result = std::nullopt
    ) -> std::expected<void, TeamError> {
        auto team = get_by_id_or_name(team_key);
        if (!team) return std::unexpected(team.error());

        auto member_it = std::ranges::find_if((*team)->members, [&](const auto& member) {
            return member.agent_id == agent_id;
        });
        if (member_it == (*team)->members.end()) {
            return std::unexpected(TeamError::MemberNotFound);
        }

        member_it->status = status;
        if (status != MemberStatus::Working) {
            member_it->current_task = std::nullopt;
        }
        if (result) {
            member_it->last_result = std::move(result);
        }
        (void)persist_team_record(**team);
        return {};
    }

    void clear_for_testing() {
        teams_.clear();
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
