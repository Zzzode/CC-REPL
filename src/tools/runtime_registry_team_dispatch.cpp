// Implementation unit for cc.tools.runtime_registry — the team_create and
// team_delete branches extracted from the execute_simple_runtime_tool mega
// dispatcher so no single implementation unit approaches ~700 LOC.
module;

#include <cstdlib>  // std::getenv / ::setenv for LOOM_TEAM_NAME leader setup

module cc.tools.runtime_registry;

import std;

import cc.types.tool_types;
import cc.tools.tool;  // arch-check: keep-import (ToolRegistry; raw strings blind the parser)
import cc.utils.json;
import cc.tools.team;
import cc.tools.send_message;
import cc.tools.agent_runtime;
import cc.tools.runtime_team_shared;
import cc.tools.runtime_message_delivery;
import cc.utils.task_utils;

namespace cc::tools::detail {

using cc::core::Result;
using cc::core::ToolInput;
using cc::core::ToolRegistry;
using cc::core::ToolResult;

namespace json = cc::utils::json;

[[nodiscard]] Result<ToolResult> execute_team_create_runtime_tool(
    std::string_view json,
    ToolRegistry* registry
) {
    auto parsed = cc::utils::json::parse(json);
    if (!parsed || !parsed->root().is_obj()) {
        return ToolResult::error("team_create input must be a JSON object");
    }
    auto root = parsed->root();
    TeamCreateTool tool;
    auto team = runtime_json_string(root, "team_name").or_else([&] { return runtime_json_string(root, "name"); })
        .value_or(std::format("team-{}", std::chrono::steady_clock::now().time_since_epoch().count()));
    auto id = runtime_json_string(root, "team_id").or_else([&] { return runtime_json_string(root, "id"); }).value_or(team);
    auto members = runtime_team_shared::parse_team_members(root);
    auto member_start_options = runtime_team_shared::parse_team_member_start_options(root);
    auto tasks = runtime_team_shared::parse_team_tasks(root);
    const bool start_native_agents = runtime_json_bool(root, "start_native_agents")
        .or_else([&] { return runtime_json_bool(root, "start_agents"); })
        .or_else([&] { return runtime_json_bool(root, "run_agents"); })
        .value_or(false);
    const auto default_cwd = runtime_json_string(root, "cwd");
    const auto default_mode = runtime_json_string(root, "mode")
        .or_else([&] { return runtime_json_string(root, "permission_mode"); });
    const auto default_isolation = runtime_json_string(root, "isolation");
    if (start_native_agents && !registry) {
        return ToolResult::error("team_create start_native_agents requires an attached runtime registry");
    }
    if (start_native_agents && !runtime_has_agent_api_credentials()) {
        return ToolResult::error("team_create start_native_agents requires Anthropic API credentials");
    }
    auto result = tool.execute(id, team, members);
    if (!result) return ToolResult::error(std::string(format_error(result.error())));

    // Establish THIS process as the leader of the newly created team.
    // A normal interactive leader gets team identity at runtime via
    // team_create (not just the --team-name launch flag); without this,
    // the live teammate projection / pane observer / leader permission
    // inbox never activate because get_team_name() stays empty.
    // TS REF: TeamCreateTool sets setLeaderTeamName + AppState.teamContext.
    if (std::getenv("LOOM_TEAM_NAME") == nullptr &&
        std::getenv("CLAUDE_CODE_TEAM_NAME") == nullptr &&
        !(*result)->name.empty()) {
        ::setenv("LOOM_TEAM_NAME", (*result)->name.c_str(), 1);
        // task-list resolution also tracks the leader team.
        cc::utils::set_leader_team_name((*result)->name);
    }
    std::unordered_map<std::string, std::string> member_start_prompts;
    for (const auto& member : (*result)->members) {
        MessageRouter::instance().register_agent(member.agent_id);
        cc::tools::agent_runtime::NativeAgentRecord record;
        record.agent_id = member.agent_id;
        record.agent_type = std::string(member_role_name(member.role));
        record.team_name = (*result)->name;
        record.background = true;
        record.status = cc::tools::agent_runtime::NativeAgentStatus::Queued;
        cc::tools::agent_runtime::native_agent_store().upsert(std::move(record));
    }
    std::size_t task_assignments_enqueued = 0;
    for (auto& task : tasks) {
        auto task_id = task.id;
        auto task_description = task.description;
        auto assigned_to = task.assigned_to;
        auto added = global_team_store().add_task((*result)->id, std::move(task));
        if (!added) return ToolResult::error(std::string(format_error(added.error())));
        if (assigned_to && !assigned_to->empty()) {
            auto assigned = global_team_store().assign_task((*result)->id, task_id, *assigned_to);
            if (!assigned) return ToolResult::error(std::string(format_error(assigned.error())));
            auto assignment_message =
                detail::format_team_task_assignment_message((*result)->name, task_id, task_description);
            member_start_prompts[*assigned_to] = assignment_message;
            if (!start_native_agents) {
                cc::tools::agent_runtime::native_agent_store().mark_running(*assigned_to);
                cc::tools::agent_runtime::native_agent_store().append_transcript(
                    *assigned_to,
                    std::format("team task assigned {}: {}", task_id, task_description));
                cc::tools::agent_runtime::native_agent_store().enqueue_pending_message(
                    *assigned_to,
                    assignment_message);
            }
            ++task_assignments_enqueued;
        }
    }
    auto created_team = global_team_store().get((*result)->id);
    if (!created_team) return ToolResult::error(std::string(format_error(created_team.error())));
    auto artifacts = detail::ensure_team_runtime_artifacts(
        (*created_team)->name,
        std::span<const TeamMember>((*created_team)->members.data(), (*created_team)->members.size()),
        std::span<const SharedTaskItem>((*created_team)->task_list.data(), (*created_team)->task_list.size()),
        **created_team);
    if (!artifacts) return ToolResult::error(artifacts.error());
    std::size_t native_agents_started = 0;
    if (start_native_agents) {
        for (const auto& member : (*created_team)->members) {
            auto options_it = member_start_options.find(member.agent_id);
            const auto* options = options_it == member_start_options.end() ? nullptr : &options_it->second;
            auto prompt = options && options->prompt && !options->prompt->empty()
                ? *options->prompt
                : [&] {
                    auto assigned_prompt = member_start_prompts.find(member.agent_id);
                    if (assigned_prompt != member_start_prompts.end()) return assigned_prompt->second;
                    return std::format(
                        "You are teammate {} on team {}. Coordinate with the team lead and wait for assigned work.",
                        member.agent_id,
                        (*created_team)->name);
                }();
            auto started = registry->execute(
                "Agent",
                ToolInput::from_json(detail::build_team_member_agent_start_input_json(
                    member,
                    (*created_team)->name,
                    prompt,
                    options,
                    default_cwd,
                    default_mode,
                    default_isolation)));
            if (!started) {
                auto error = "failed to start team member " + member.agent_id + ": " + started.error().message;
                (void)global_team_store().update_member_status((*created_team)->id, member.agent_id, MemberStatus::Error, error);
                return ToolResult::error(error);
            }
            if (started->is_error) {
                auto error = "failed to start team member " + member.agent_id + ": " + detail::runtime_tool_result_text(*started);
                (void)global_team_store().update_member_status((*created_team)->id, member.agent_id, MemberStatus::Error, error);
                return ToolResult::error(error);
            }
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                member.agent_id,
                std::format("system: started by team_create for team {}", (*created_team)->name));
            ++native_agents_started;
        }
        if (auto refreshed_team = global_team_store().get((*created_team)->id)) {
            auto native_records = detail::collect_team_native_agents(
                (*refreshed_team)->id,
                (*refreshed_team)->name,
                std::span<const TeamMember>((*refreshed_team)->members.data(), (*refreshed_team)->members.size()));
            auto runtime_states = detail::team_config_runtime_states_from_native_records(
                std::span<const cc::tools::agent_runtime::NativeAgentRecord>(
                    native_records.data(),
                    native_records.size()));
            artifacts->team_config_written = detail::write_team_config_file(
                artifacts->team_file_path,
                **refreshed_team,
                runtime_states);
            if (!artifacts->team_config_written) {
                return ToolResult::error("failed to refresh team config after starting native agents");
            }
        }
    }
    json::JsonBuilder b;
    b.str("team_name", (*result)->name);
    b.str("team_file_path", artifacts->team_file_path.string());
    b.str("lead_agent_id", detail::team_lead_agent_id((*result)->name));
    b.str("team_id", (*result)->id);
    b.str("team_dir", artifacts->team_dir.string());
    b.size("members", (*result)->members.size());
    b.size("tasks", tasks.size());
    b.size("member_inboxes_initialized", artifacts->inboxes_initialized);
    b.size("task_assignments_enqueued", task_assignments_enqueued);
    b.boolean("team_config_written", artifacts->team_config_written);
    b.boolean("task_list_written", artifacts->task_list_written);
    b.size("native_agents_started", native_agents_started);
    return ToolResult::success(b.serialize());
}

[[nodiscard]] Result<ToolResult> execute_team_delete_runtime_tool(std::string_view json) {
    auto team = json_string(json, "team_id").or_else([&] { return json_string(json, "id"); })
        .or_else([&] { return json_string(json, "team_name"); }).or_else([&] { return json_string(json, "name"); });
    if (!team) return ToolResult::error("team_delete requires team_id");
    auto resolved = global_team_store().get_by_id_or_name(*team);
    if (!resolved) return ToolResult::error(std::string(format_error(resolved.error())));
    const auto team_id = (*resolved)->id;
    const auto team_name = (*resolved)->name;
    const auto members = (*resolved)->members;
    auto native_records = detail::collect_team_native_agents(
        team_id,
        team_name,
        std::span<const TeamMember>(members.data(), members.size()));
    auto cleanup = detail::cleanup_team_runtime_artifacts(
        team_id,
        team_name,
        std::span<const agent_runtime::NativeAgentRecord>(native_records.data(), native_records.size()));
    TeamDeleteTool tool;
    auto result = tool.execute(team_id);
    if (!result) return ToolResult::error(std::string(format_error(result.error())));
    return ToolResult::success(std::format(
        "Deleted team {} ({})\n"
        "native_agents_seen: {}\n"
        "cancelled_agents: {}\n"
        "teammate_terminations: {}\n"
        "teammate_kills: {}\n"
        "background_shell_tasks_stopped: {}\n"
        "transcript_artifacts_removed: {}\n"
        "worktree_cleanup_attempts: {}\n"
        "worktrees_removed: {}\n"
        "worktrees_retained: {}\n"
        "team_dirs_removed: {}",
        team_name,
        team_id,
        cleanup.native_agents_seen,
        cleanup.cancelled_agents,
        cleanup.teammate_terminations,
        cleanup.teammate_kills,
        cleanup.background_shell_tasks_stopped,
        cleanup.transcript_artifacts_removed,
        cleanup.worktree_cleanup_attempts,
        cleanup.worktrees_removed,
        cleanup.worktrees_retained,
        cleanup.team_dirs_removed));
}

} // namespace cc::tools::detail
