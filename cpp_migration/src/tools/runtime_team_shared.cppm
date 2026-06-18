/// @file runtime_team_shared.cppm
/// @brief Team-config parsing helpers, team-shared value types, team runtime
/// filesystem/config writers — all extracted from the (former) monolithic
/// runtime_registry.cppm as part of audit §13 #1.
///
/// Everything in this module is "about a team": it parses team JSON payloads
/// into the `cc::tools::team` value types, writes team runtime artifacts
/// (inboxes, task snapshots, config.json) to the filesystem, and performs
/// book-keeping operations (native-agent record collection, runtime cleanup)
/// tied to team lifecycle. No tool dispatchers, no registry calls — those
/// stay in the runtime-registry dispatcher layer.
module;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.tools.runtime_team_shared;

import cc.utils.json;
import cc.tools.agent;                // cleanup_agent_worktree
import cc.tools.agent_runtime;        // NativeAgentRecord / native_agent_store / runtime_state_dir
import cc.tools.bash;                 // stop_background_tasks_for_agent
import cc.tools.team;
import cc.tools.runtime_shared_utils; // safe_runtime_dir_component, path helpers
import cc.utils.team_helpers;         // team_runtime_dir
import cc.utils.swarm_backends;       // BackendRegistry

export namespace cc::tools::runtime_team_shared {

namespace fs = std::filesystem;
namespace json = cc::utils::json;
using cc::tools::agent_runtime::NativeAgentRecord;

// ---------------------------------------------------------------------------
//  2. Convenience predicates and S2 aggregate types
// ---------------------------------------------------------------------------

/// Default "is this agent status terminal?" predicate. Matches the behaviour
/// hard-coded in `runtime_registry::native_agent_status_is_terminal`. Tests
/// and wrappers can use this as a fallback when no caller-defined predicate
/// is supplied.
[[nodiscard]] inline bool is_terminal_default(cc::tools::agent_runtime::NativeAgentStatus status) {
    using S = cc::tools::agent_runtime::NativeAgentStatus;
    return status == S::Completed || status == S::Failed || status == S::Cancelled;
}

// ---------------------------------------------------------------------------
//  3. JSON payload parsing (shipped in the original extraction; preserved
//     verbatim because they already have 3 call sites in runtime_registry).
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::optional<std::string> json_string(json::JsonVal obj, std::string_view key) {
    auto val = obj.get(key);
    if (!val.is_str()) return std::nullopt;
    return std::string(val.as_str());
}

[[nodiscard]] inline MemberRole parse_team_member_role(std::string_view role) {
    if (role == "leader") return MemberRole::Leader;
    if (role == "reviewer") return MemberRole::Reviewer;
    return MemberRole::Worker;
}

[[nodiscard]] inline std::vector<TeamMember> parse_team_members(json::JsonVal root) {
    std::vector<TeamMember> members;
    auto value = root.get("members");
    if (!value.valid() || !value.is_arr()) return members;

    value.iter([&](json::JsonVal item) {
        TeamMember member;
        if (item.is_str()) {
            member.agent_id = std::string(item.as_str());
        } else if (item.is_obj()) {
            member.agent_id = json_string(item, "agent_id")
                .or_else([&] { return json_string(item, "id"); })
                .or_else([&] { return json_string(item, "name"); })
                .value_or("");
            member.role = parse_team_member_role(json_string(item, "role").value_or("worker"));
            member.current_task = json_string(item, "current_task");
        }
        if (!member.agent_id.empty()) members.push_back(std::move(member));
    });
    return members;
}

struct TeamMemberStartOptions {
    std::optional<std::string> prompt;
    std::optional<std::string> agent_type;
    std::optional<std::string> mode;
    std::optional<std::string> cwd;
    std::optional<std::string> isolation;
};

[[nodiscard]] inline std::unordered_map<std::string, TeamMemberStartOptions> parse_team_member_start_options(
    json::JsonVal root
) {
    std::unordered_map<std::string, TeamMemberStartOptions> options;
    auto value = root.get("members");
    if (!value.valid() || !value.is_arr()) return options;

    value.iter([&](json::JsonVal item) {
        if (!item.is_obj()) return;
        auto agent_id = json_string(item, "agent_id")
            .or_else([&] { return json_string(item, "id"); })
            .or_else([&] { return json_string(item, "name"); });
        if (!agent_id || agent_id->empty()) return;
        options.emplace(*agent_id, TeamMemberStartOptions{
            .prompt = json_string(item, "prompt"),
            .agent_type = json_string(item, "subagent_type")
                .or_else([&] { return json_string(item, "agent_type"); }),
            .mode = json_string(item, "mode")
                .or_else([&] { return json_string(item, "permission_mode"); }),
            .cwd = json_string(item, "cwd"),
            .isolation = json_string(item, "isolation"),
        });
    });
    return options;
}

[[nodiscard]] inline std::vector<SharedTaskItem> parse_team_tasks(json::JsonVal root) {
    std::vector<SharedTaskItem> tasks;
    auto value = root.get("task_list");
    if (!value.valid() || !value.is_arr()) {
        value = root.get("tasks");
    }
    if (!value.valid() || !value.is_arr()) return tasks;

    value.iter([&](json::JsonVal item) {
        if (!item.is_obj()) return;
        auto id = json_string(item, "id");
        auto description = json_string(item, "description")
            .or_else([&] { return json_string(item, "task"); });
        if (!id || !description) return;
        tasks.push_back(SharedTaskItem{
            .id = *id,
            .description = *description,
            .assigned_to = json_string(item, "assigned_to"),
            .completed = false,
            .result = std::nullopt,
        });
    });
    return tasks;
}

// ---------------------------------------------------------------------------
//  2. Team-shared aggregate types (S2 structs)
// ---------------------------------------------------------------------------

/// Counters populated while cleaning up team runtime state; returned to
/// callers as a human-readable summary (printed by the team_delete handler).
struct TeamDeletionCleanupSummary {
    std::size_t native_agents_seen = 0;
    std::size_t cancelled_agents = 0;
    std::size_t teammate_terminations = 0;
    std::size_t teammate_kills = 0;
    std::size_t background_shell_tasks_stopped = 0;
    std::size_t transcript_artifacts_removed = 0;
    std::size_t worktree_cleanup_attempts = 0;
    std::size_t worktrees_removed = 0;
    std::size_t worktrees_retained = 0;
    std::size_t team_dirs_removed = 0;
};

/// Path/metadata produced by `ensure_team_runtime_artifacts` so the caller
/// can both return the paths (e.g. team_file_path) and mutate the summary
/// structs after team member agents have been launched.
struct TeamCreationArtifactsSummary {
    fs::path team_dir;
    fs::path team_file_path;
    std::size_t inboxes_initialized = 0;
    bool team_config_written = false;
    bool task_list_written = false;
};

/// Live runtime state for a single team member (agent type, cwd, tmux pane,
/// colour, etc). Written into `config.json` by `write_team_config_file` so
/// downstream tools (team UI, teammate backends) can introspect a team
/// without reaching into the native-agent store.
struct TeamConfigMemberRuntimeState {
    std::optional<std::string> agent_type;
    std::optional<std::string> cwd;
    std::optional<std::string> worktree_path;
    std::optional<std::string> backend_type;
    std::optional<std::string> pane_id;
    std::optional<std::string> color;
    std::optional<std::string> mode;
    std::optional<bool> is_active;
};

// ---------------------------------------------------------------------------
//  3. Team-shared pure helpers (S2 utility free functions)
// ---------------------------------------------------------------------------

/// Lowercase-alnum-with-dashes sanitisation used for team directory names.
/// Differs from `safe_runtime_dir_component` by replacing non-alnum with
/// '-' rather than '_' (keeps team dirs visually distinct from agent dirs).
[[nodiscard]] inline std::string ts_sanitized_team_dir_name(
    std::string_view value,
    std::string_view fallback
) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch)) {
            out.push_back(static_cast<char>(std::tolower(ch)));
        } else {
            out.push_back('-');
        }
    }
    return out.empty() ? std::string(fallback) : out;
}

/// Convert an agent id like "alice@team-fancy" into the short inbox /
/// display name "alice". Used both for inbox filenames and for human-facing
/// display of team membership.
[[nodiscard]] inline std::string team_agent_name_from_id(std::string_view agent_id) {
    if (auto at = agent_id.find('@'); at != std::string_view::npos) {
        return std::string(agent_id.substr(0, at));
    }
    return std::string(agent_id);
}

/// Build the canonical synthetic agent_id for a team's lead. Convention:
/// leads are named `team-lead@<team-name>` so they can be resolved from any
/// member of the team without a lookup table.
[[nodiscard]] inline std::string team_lead_agent_id(std::string_view team_name) {
    return std::format("team-lead@{}", team_name);
}

/// Translate the member's agent-id into the filename of their on-disk inbox
/// (strips the `@team` suffix so inboxes are stable across team reassigns).
[[nodiscard]] inline std::string team_member_inbox_name(std::string_view agent_id) {
    auto name = std::string(agent_id);
    if (auto at = name.find('@'); at != std::string::npos) {
        name = name.substr(0, at);
    }
    return runtime_shared_utils::safe_runtime_dir_component(name, "agent");
}

/// Human-readable message inserted into a team-member agent's prompt /
/// transcript when its leader assigns a shared task to it.
[[nodiscard]] inline std::string format_team_task_assignment_message(
    std::string_view team_name,
    std::string_view task_id,
    std::string_view description
) {
    return std::format(
        "[Team task {} assigned by {}]\n{}",
        task_id,
        team_name,
        description);
}

// ---------------------------------------------------------------------------
//  4. Team config writers (S3)
// ---------------------------------------------------------------------------

/// If the inbox JSON file is missing, create the directory and drop an empty
/// JSON array `[]` so downstream mailbox consumers don't have to special-case
/// "file missing" vs "file present with zero messages".
inline bool write_empty_inbox_if_missing(const fs::path& inbox_path) {
    std::error_code ec;
    fs::create_directories(inbox_path.parent_path(), ec);
    if (ec) return false;
    if (fs::exists(inbox_path, ec)) return true;
    std::ofstream out(inbox_path, std::ios::trunc);
    if (!out) return false;
    out << "[]";
    return out.good();
}

/// Serialise the current shared task list to `tasks.json` via `JsonMutDoc`
/// (no hand-written escaping). Tasks are written in the same order they
/// appear in the input span.
inline bool write_team_task_snapshot(
    const fs::path& task_path,
    std::span<const SharedTaskItem> tasks
) {
    std::error_code ec;
    fs::create_directories(task_path.parent_path(), ec);
    if (ec) return false;

    std::ofstream out(task_path, std::ios::trunc);
    if (!out) return false;
    json::JsonMutDoc doc;
    auto arr = doc.array();
    for (const auto& task : tasks) {
        auto obj = doc.object();
        obj.add("id", doc.string(task.id));
        obj.add("description", doc.string(task.description));
        obj.add("completed", doc.boolean(task.completed));
        if (task.assigned_to) obj.add("assigned_to", doc.string(*task.assigned_to));
        if (task.result) obj.add("result", doc.string(*task.result));
        arr.append(obj);
    }
    doc.set_root(arr);
    out << doc.to_string();
    return out.good();
}

namespace team_config_detail {
// Splice a pre-serialized `{"key": value}` fragment into an enclosing
// comma-separated object body. The value's escaping was already done by
// yyjson when the fragment was produced; we only cut the surrounding braces.
inline void splice_json_member(std::ostream& out, json::JsonMutDoc& doc) {
    auto serialized = doc.to_string();
    if (serialized.size() >= 2) {
        out << ',' << std::string_view{serialized}.substr(1, serialized.size() - 2);
    }
}
} // namespace team_config_detail

inline void write_team_config_optional_string(
    std::ostream& out,
    std::string_view key,
    const std::optional<std::string>& value
) {
    if (!value || value->empty()) return;
    json::JsonMutDoc doc;
    auto obj = doc.object();
    obj.add(key, doc.string(*value));
    doc.set_root(obj);
    team_config_detail::splice_json_member(out, doc);
}

inline void write_team_config_optional_bool(
    std::ostream& out,
    std::string_view key,
    std::optional<bool> value
) {
    if (!value) return;
    json::JsonMutDoc doc;
    auto obj = doc.object();
    obj.add(key, doc.boolean(*value));
    doc.set_root(obj);
    team_config_detail::splice_json_member(out, doc);
}

/// Render a single team-member object (as used by the `members` array inside
/// config.json). Uses JsonMutDoc for all string / number escaping so control
/// characters in the agent id can't corrupt the output JSON.
inline void write_team_config_member(
    std::ostream& out,
    std::string_view agent_id,
    std::string_view role,
    std::string_view cwd,
    std::int64_t timestamp_ms,
    const TeamConfigMemberRuntimeState* state = nullptr
) {
    const auto agent_type = state && state->agent_type && !state->agent_type->empty()
        ? std::string_view{*state->agent_type}
        : role;
    const auto member_cwd = state && state->cwd && !state->cwd->empty()
        ? std::string_view{*state->cwd}
        : cwd;
    const auto pane_id = state && state->pane_id && !state->pane_id->empty()
        ? std::string_view{*state->pane_id}
        : std::string_view{""};

    json::JsonMutDoc doc;
    auto obj = doc.object();
    obj.add("agentId", doc.string(agent_id));
    obj.add("name", doc.string(team_agent_name_from_id(agent_id)));
    obj.add("agentType", doc.string(agent_type));
    obj.add("model", doc.string(""));
    obj.add("joinedAt", doc.number(timestamp_ms));
    obj.add("tmuxPaneId", doc.string(pane_id));
    obj.add("cwd", doc.string(member_cwd));
    auto subscriptions = doc.array();
    obj.add("subscriptions", subscriptions);
    if (state) {
        auto add_opt_str = [&](std::string_view k, const std::optional<std::string>& v) {
            if (v && !v->empty()) obj.add(k, doc.string(*v));
        };
        auto add_opt_bool = [&](std::string_view k, const std::optional<bool>& v) {
            if (v) obj.add(k, doc.boolean(*v));
        };
        add_opt_str("color", state->color);
        add_opt_str("worktreePath", state->worktree_path);
        add_opt_str("backendType", state->backend_type);
        add_opt_str("mode", state->mode);
        add_opt_bool("isActive", state->is_active);
    }
    doc.set_root(obj);
    out << doc.to_string();
}

/// Write the top-level team config.json file. Members are rendered by
/// `write_team_config_member`; the root object uses the same yyjson-backed
/// builder. Always inserts the implicit team-lead entry so downstream tools
/// don't need to know about the `team-lead@<team>` convention.
inline bool write_team_config_file(
    const fs::path& config_path,
    const Team& team,
    const std::unordered_map<std::string, TeamConfigMemberRuntimeState>& member_states = {}
) {
    std::error_code ec;
    fs::create_directories(config_path.parent_path(), ec);
    if (ec) return false;
    std::ofstream out(config_path, std::ios::trunc);
    if (!out) return false;

    const auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto cwd = fs::current_path(ec);
    if (ec) cwd = fs::path{};
    const auto cwd_text = cwd.string();
    const auto lead_id = team_lead_agent_id(team.name);

    auto member_to_string = [&](std::string_view agent_id,
                                std::string_view role) -> std::string {
        std::ostringstream oss;
        const auto state_it = member_states.find(std::string(agent_id));
        const auto* state = state_it == member_states.end() ? nullptr : &state_it->second;
        write_team_config_member(oss, agent_id, role, cwd_text, timestamp_ms, state);
        return oss.str();
    };

    std::vector<std::string> member_json;
    const bool has_explicit_lead = std::ranges::any_of(team.members, [&](const auto& member) {
        return member.agent_id == lead_id;
    });
    if (!has_explicit_lead) member_json.push_back(member_to_string(lead_id, "team-lead"));
    for (const auto& member : team.members) {
        member_json.push_back(member_to_string(member.agent_id, member_role_name(member.role)));
    }

    json::JsonMutDoc doc;
    auto root = doc.object();
    root.add("name", doc.string(team.name));
    root.add("description", doc.string(""));
    root.add("createdAt", doc.number(static_cast<std::int64_t>(timestamp_ms)));
    root.add("leadAgentId", doc.string(lead_id));
    root.add("leadSessionId", doc.string(""));
    auto members = doc.array();
    for (const auto& json_text : member_json) {
        members.append(doc.raw_json(json_text));
    }
    root.add("members", members);
    doc.set_root(root);
    out << doc.to_string();
    return out.good();
}

// ---------------------------------------------------------------------------
//  5. Team lifecycle book-keeping (S2 bridges to native-agent store /
//     swarm backends / bash background tasks / filesystem cleanup)
// ---------------------------------------------------------------------------

/// Project a span of native-agent records into the runtime-state map consumed
/// by `write_team_config_file`. Runs after team member agents have been
/// launched so the written config reflects live data (cwd, backend, tmux
/// pane, etc.).
[[nodiscard]] inline std::unordered_map<std::string, TeamConfigMemberRuntimeState>
team_config_runtime_states_from_native_records(
    std::span<const NativeAgentRecord> records,
    bool (*is_terminal)(cc::tools::agent_runtime::NativeAgentStatus)
) {
    std::unordered_map<std::string, TeamConfigMemberRuntimeState> states;
    for (const auto& record : records) {
        TeamConfigMemberRuntimeState state{
            .agent_type = record.agent_type,
            .cwd = record.cwd,
            .worktree_path = record.worktree_path,
            .backend_type = record.teammate_backend,
            .pane_id = record.teammate_pane_id,
            .color = record.teammate_color,
            .mode = record.mode,
            .is_active = !is_terminal(record.status),
        };
        if (!state.backend_type && record.team_name) {
            state.backend_type = "in-process";
        }
        if (!state.pane_id && state.backend_type && *state.backend_type == "in-process") {
            state.pane_id = "in-process";
        }
        states[record.agent_id] = std::move(state);
    }
    return states;
}

[[nodiscard]] inline bool contains_agent_id(
    const std::vector<NativeAgentRecord>& records,
    std::string_view agent_id
) {
    return std::ranges::any_of(records, [&](const auto& record) {
        return record.agent_id == agent_id;
    });
}

/// Collect every native-agent record that is either (a) an explicit team
/// member or (b) carries a team_name/team_id tag matching `team_name` /
/// `team_id`. Dedup on agent_id so downstream cleanup only touches each
/// agent once.
[[nodiscard]] inline std::vector<NativeAgentRecord> collect_team_native_agents(
    std::string_view team_id,
    std::string_view team_name,
    std::span<const TeamMember> members
) {
    std::vector<NativeAgentRecord> records;
    for (const auto& member : members) {
        if (auto record = agent_runtime::native_agent_store().get(member.agent_id)) {
            if (!contains_agent_id(records, record->agent_id)) records.push_back(std::move(*record));
        }
    }
    for (auto record : agent_runtime::native_agent_store().list()) {
        const bool belongs_to_team =
            record.team_name && (*record.team_name == team_name || *record.team_name == team_id);
        if (belongs_to_team && !contains_agent_id(records, record.agent_id)) {
            records.push_back(std::move(record));
        }
    }
    return records;
}

/// Best-effort artifact cleanup: teammate backend terminate/kill, bash
/// background-task shutdown, agent worktree cleanup, cancelled-agent
/// marking, transcript-JSONL removal, and final team-dir scrubbing.
///
/// Any step that fails is silently absorbed (the team *record* is already
/// gone from the team store at this point), but counters are populated so
/// the caller can report what actually happened.
[[nodiscard]] inline TeamDeletionCleanupSummary cleanup_team_runtime_artifacts(
    std::string_view team_id,
    std::string_view team_name,
    std::span<const NativeAgentRecord> records,
    bool (*is_terminal)(cc::tools::agent_runtime::NativeAgentStatus),
    std::size_t (*cleanup_transcript)(const NativeAgentRecord&)
) {
    namespace swarm = cc::utils::swarm_backends;
    namespace bash_ns = cc::tools::bash;

    TeamDeletionCleanupSummary summary{.native_agents_seen = records.size()};
    for (const auto& record : records) {
        if (record.teammate_backend && !record.teammate_backend->empty()) {
            const bool prefer_in_process = *record.teammate_backend == "in-process";
            auto executor = swarm::BackendRegistry::get_teammate_executor(prefer_in_process);
            if (executor->terminate(record.agent_id, "team deleted")) {
                ++summary.teammate_terminations;
            }
            if (executor->kill(record.agent_id)) {
                ++summary.teammate_kills;
            }
        }

        auto stopped_shell_tasks = bash_ns::stop_background_tasks_for_agent(record.agent_id);
        summary.background_shell_tasks_stopped += stopped_shell_tasks.size();

        auto cleanup = cc::tools::agent::cleanup_agent_worktree(record.agent_id);
        if (cleanup.attempted) {
            ++summary.worktree_cleanup_attempts;
            if (cleanup.removed) ++summary.worktrees_removed;
            if (cleanup.changed) ++summary.worktrees_retained;
        }

        if (!is_terminal(record.status)) {
            agent_runtime::native_agent_store().mark_cancelled(
                record.agent_id,
                std::format("team deleted: {}", team_name));
            ++summary.cancelled_agents;
        }

        const auto persisted_record =
            agent_runtime::native_agent_store().get(record.agent_id).value_or(record);
        summary.transcript_artifacts_removed += cleanup_transcript(persisted_record);
    }

    std::error_code ec;
    const auto root = team_runtime_dir();
    for (auto component : {
        ts_sanitized_team_dir_name(team_name, "team"),
        ts_sanitized_team_dir_name(team_id, "team"),
        runtime_shared_utils::safe_runtime_dir_component(team_name, "team"),
        runtime_shared_utils::safe_runtime_dir_component(team_id, "team"),
    }) {
        auto removed = fs::remove_all(root / component, ec);
        if (!ec && removed > 0) ++summary.team_dirs_removed;
        ec.clear();
    }
    return summary;
}

/// Idempotently create the team runtime directory structure (<team>/inboxes/,
/// <team>/tasks.json, <team>/config.json). Returned summary carries the
/// resulting paths so the caller can later re-write config.json with
/// populated member runtime states (after native agents are launched).
[[nodiscard]] inline std::expected<TeamCreationArtifactsSummary, std::string>
ensure_team_runtime_artifacts(
    std::string_view team_name,
    std::span<const TeamMember> members,
    std::span<const SharedTaskItem> tasks,
    const Team& team
) {
    TeamCreationArtifactsSummary summary{
        .team_dir = team_runtime_dir() / ts_sanitized_team_dir_name(team_name, "team"),
        .team_file_path = {},
    };
    summary.team_file_path = summary.team_dir / "config.json";

    std::error_code ec;
    fs::create_directories(summary.team_dir / "inboxes", ec);
    if (ec) return std::unexpected(std::format("failed to create team directory: {}", ec.message()));

    for (const auto& member : members) {
        if (member.agent_id.empty()) continue;
        const auto inbox_name = team_member_inbox_name(member.agent_id);
        if (write_empty_inbox_if_missing(summary.team_dir / "inboxes" / (inbox_name + ".json"))) {
            ++summary.inboxes_initialized;
        }
    }

    summary.task_list_written = write_team_task_snapshot(summary.team_dir / "tasks.json", tasks);
    if (!summary.task_list_written) {
        return std::unexpected("failed to write team task snapshot");
    }
    summary.team_config_written = write_team_config_file(summary.team_file_path, team);
    if (!summary.team_config_written) {
        return std::unexpected("failed to write team config");
    }
    return summary;
}

} // namespace cc::tools::runtime_team_shared
