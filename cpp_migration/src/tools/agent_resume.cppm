module;

#include <atomic>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <array>
#include <utility>
#include <sstream>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <sys/wait.h>

export module cc.tools.agent.resume;

import cc.utils.error;
import cc.utils.git;
import cc.tools.tool;
import cc.utils.json;
import cc.tools.agent_runtime;
import cc.tools.agent_constants;
import cc.tools.agent_memory;
import cc.tools.agent_memory_snapshot;
import cc.tools.agent_color_manager;
import cc.tools.agent_display;
import cc.tools.bash;
import cc.tools.todo_write;
import cc.tools.send_message;
import cc.tools.team;
import cc.tools.mcp;
import cc.tools.sleep;
import cc.tools.web_fetch;
import cc.skills.skill;
import cc.utils.team_helpers;
import cc.services.api.client;
import cc.services.api.streaming;
import cc.services.api.bootstrap;
import cc.services.mcp.types;
import cc.utils.swarm_backends;
import cc.utils.env_utils;
import cc.utils.tool_helpers;
import cc.utils.bash_execution;
import cc.tools.agent.utils;

export namespace cc::tools::agent::resume_ {

namespace fs = std::filesystem;

using cc::core::Tool;
using cc::core::ToolInput;
using cc::core::ToolResult;
using cc::core::ToolDefinition;
using cc::services::api::Message;
using cc::services::api::ContentBlock;
using cc::services::api::ContentBlockType;
using cc::tools::agent::utils::AgentExecutionPlan;
using cc::tools::agent::utils::filter_incomplete_tool_calls;
using cc::tools::agent::utils::filter_resume_unresolved_tool_use_messages;
using cc::tools::agent::utils::filter_resume_orphaned_thinking_messages;
using cc::tools::agent::utils::filter_resume_whitespace_assistant_messages;
using cc::tools::agent::utils::resume_content_replacements_from_entries;
using cc::tools::agent::utils::apply_resume_content_replacements;
using cc::tools::agent::utils::fork_context_messages_from_entries;


[[nodiscard]] inline std::string format_resumed_agent_context(
    const cc::tools::agent_runtime::NativeAgentRecord& record
) {
    constexpr std::size_t max_lines = 80;
    constexpr std::size_t max_chars = 24000;

    std::string context;
    context += "<resumed_agent_context>\n";
    context += std::format("<agent_id>{}</agent_id>\n", record.agent_id);
    context += std::format("<status>{}</status>\n", cc::tools::agent_runtime::native_agent_status_name(record.status));
    if (record.description && !record.description->empty()) {
        context += std::format("<description>{}</description>\n", *record.description);
    }
    if (record.output && !record.output->empty()) {
        context += std::format("<previous_output>{}</previous_output>\n", *record.output);
    }
    if (record.error && !record.error->empty()) {
        context += std::format("<previous_error>{}</previous_error>\n", *record.error);
    }

    context += "<recent_transcript>\n";
    const auto start = record.transcript.size() > max_lines
        ? record.transcript.size() - max_lines
        : std::size_t{0};
    for (std::size_t i = start; i < record.transcript.size(); ++i) {
        const auto& line = record.transcript[i];
        if (context.size() + line.size() + 2 > max_chars) {
            context += "[resumed transcript truncated]\n";
            break;
        }
        context += line;
        context += '\n';
    }
    context += "</recent_transcript>\n";
    context += "</resumed_agent_context>";
    return context;
}

inline void hydrate_resume_plan_from_existing_record(AgentExecutionPlan& plan) {
    if (!plan.resume_existing) return;
    auto existing = cc::tools::agent_runtime::native_agent_store().get(plan.agent_id);
    if (!existing) return;

    plan.parent_agent_id = plan.parent_agent_id.or_else([&] { return existing->parent_agent_id; });
    plan.description = plan.description.or_else([&] { return existing->description; });
    plan.name = plan.name.or_else([&] { return existing->name; });
    plan.team_name = plan.team_name.or_else([&] { return existing->team_name; });
    plan.mode = plan.mode.or_else([&] { return existing->mode; });
    plan.isolation = plan.isolation.or_else([&] { return existing->isolation; });
    plan.working_dir = plan.working_dir
        .or_else([&] { return existing->worktree_path; })
        .or_else([&] { return existing->cwd; });
    plan.worktree_path = plan.worktree_path.or_else([&] { return existing->worktree_path; });
    plan.worktree_branch = plan.worktree_branch.or_else([&] { return existing->worktree_branch; });
    plan.worktree_base_commit = plan.worktree_base_commit.or_else([&] { return existing->worktree_base_commit; });
    plan.worktree_git_root = plan.worktree_git_root.or_else([&] { return existing->worktree_git_root; });
    plan.teammate_backend = plan.teammate_backend.or_else([&] { return existing->teammate_backend; });
    plan.teammate_task_id = plan.teammate_task_id.or_else([&] { return existing->teammate_task_id; });
    plan.teammate_pane_id = plan.teammate_pane_id.or_else([&] { return existing->teammate_pane_id; });
    plan.teammate_color = plan.teammate_color.or_else([&] { return existing->teammate_color; });
    plan.parent_session_id = plan.parent_session_id.or_else([&] { return existing->parent_session_id; });
}


} // namespace cc::tools::agent::resume_
