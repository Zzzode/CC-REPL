// Implementation unit for cc.tools.agent.utils — the three RAII cleanup
// guard destructors, command-hook execution (popen/pclose + WIF* status
// macros), hook JSON output parsers, frontmatter/tool hook runners, hook
// context formatting, worktree create/cleanup, cwd normalization, agent
// record upsert, and the runtime-context formatter.
//
// <sys/wait.h> is a global-module-fragment header because WIFEXITED /
// WEXITSTATUS / WIFSIGNALED / WTERMSIG are preprocessor macros that cannot
// arrive through `import std;`; <cstdio> declares FILE for popen_spawn, and
// std::fgets comes through import std.
module;

#include <cstdio>
#include <sys/wait.h>

module cc.tools.agent.utils;

import std;

import cc.utils.git;
import cc.utils.bash_execution;
import cc.utils.json;
import cc.tools.todo_write;
import cc.tools.bash;
import cc.tools.mcp;
import cc.tools.agent_runtime;
import cc.services.api.client;

namespace cc::tools::agent::utils {

namespace fs = std::filesystem;

AgentTodoCleanupGuard::~AgentTodoCleanupGuard() {
    if (!agent_id.empty()) {
        (void)cc::tools::clear_todos_for_agent(agent_id);
    }
}

AgentShellTaskCleanupGuard::~AgentShellTaskCleanupGuard() {
    if (agent_id.empty()) return;
    auto stopped = cc::tools::bash::stop_background_tasks_for_agent(agent_id);
    if (!stopped.empty()) {
        cc::tools::agent_runtime::native_agent_store().append_transcript(
            agent_id,
            std::format("system: stopped {} background shell task(s) owned by agent on exit", stopped.size()));
    }
}

AgentMcpCleanupGuard::~AgentMcpCleanupGuard() {
    if (inline_servers.empty()) return;

    std::vector<std::string> remove_names;
    std::vector<cc::tools::NativeMcpConfiguredServer> restore_servers;
    for (const auto& server : inline_servers) {
        if (server.name.empty()) continue;
        if (server.previous_config) {
            restore_servers.push_back(*server.previous_config);
        } else {
            remove_names.push_back(server.name);
        }
    }

    bool cleaned = false;
    if (!remove_names.empty()) {
        if (auto removed = cc::tools::remove_native_mcp_servers(std::move(remove_names)); removed) {
            cleaned = true;
        }
    }
    if (!restore_servers.empty()) {
        if (auto restored = cc::tools::upsert_native_mcp_servers(std::move(restore_servers)); restored) {
            cleaned = true;
        }
    }
    if (cleaned && !agent_id.empty()) {
        cc::tools::agent_runtime::native_agent_store().append_transcript(
            agent_id,
            "system: cleaned agent-specific MCP server configuration");
    }
}

[[nodiscard]] std::string format_agent_runtime_context(
    const AgentExecutionPlan& plan
) {
    std::string context = "Native agent runtime context:\n";
    context += std::format("- agent_id: {}\n", plan.agent_id);
    if (plan.description) context += std::format("- description: {}\n", *plan.description);
    if (plan.name) context += std::format("- name: {}\n", *plan.name);
    if (plan.team_name) context += std::format("- team_name: {}\n", *plan.team_name);
    if (plan.working_dir) context += std::format("- cwd: {}\n", *plan.working_dir);
    if (plan.isolation) context += std::format("- isolation: {}\n", *plan.isolation);
    if (plan.mode) context += std::format("- permission_mode: {}\n", *plan.mode);
    if (plan.effort) context += std::format("- effort: {}\n", *plan.effort);
    if (plan.memory) context += std::format("- memory: {}\n", *plan.memory);
    if (plan.color) context += std::format("- color: {}\n", *plan.color);
    if (plan.omit_loom_md) context += "- omit_loom_md: true\n";
    if (plan.critical_system_reminder) context += "- critical_system_reminder: configured\n";
    if (plan.parent_agent_id) context += std::format("- parent_agent_id: {}\n", *plan.parent_agent_id);
    if (plan.background) context += "- background: true\n";
    return context;
}

[[nodiscard]] std::string shell_quote(std::string_view value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
}

[[nodiscard]] std::string sanitized_agent_file_part(std::string_view agent_id) {
    std::string out;
    out.reserve(agent_id.size());
    for (char ch : agent_id) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "agent" : out;
}

[[nodiscard]] std::string default_agent_transcript_path(std::string_view agent_id) {
    return cc::tools::agent_runtime::agent_transcript_path(agent_id).string();
}

[[nodiscard]] std::expected<AgentWorktreeInfo, std::string> create_agent_worktree(
    const AgentExecutionPlan& plan
) {
    auto base_cwd = plan.working_dir ? fs::path{*plan.working_dir} : fs::current_path();
    auto git_root = cc::utils::git::find_git_root(base_cwd);
    if (!git_root) {
        return std::unexpected(
            "Cannot create agent worktree: not in a git repository. Use cwd inside a git repository or omit isolation=worktree.");
    }

    auto slug = sanitized_agent_file_part(plan.agent_id);
    if (slug.size() > 40) slug.resize(40);
    auto branch = "cc-agent-" + slug;
    auto worktree_path = *git_root / ".loom" / "worktrees" / slug;

    std::error_code ec;
    fs::create_directories(worktree_path.parent_path(), ec);
    if (ec) return std::unexpected("Cannot create worktree parent directory: " + ec.message());

    auto head = cc::utils::git::run_git_command("rev-parse HEAD", *git_root);
    if (!head.success) return std::unexpected("Cannot resolve git HEAD for agent worktree: " + head.output);

    auto created = cc::utils::git::run_git_command(
        std::format("worktree add -B \"{}\" \"{}\" HEAD", branch, worktree_path.string()),
        *git_root);
    if (!created.success) {
        return std::unexpected("Failed to create agent worktree: " + created.output);
    }

    return AgentWorktreeInfo{
        .path = std::move(worktree_path),
        .branch = std::move(branch),
        .head_commit = std::move(head.output),
        .git_root = std::move(*git_root),
    };
}

[[nodiscard]] bool hook_condition_allows(const cc::tools::agent_runtime::AgentHookCommand& hook) {
    if (!hook.condition || hook.condition->empty()) return true;
    const auto condition = lowercase_copy(*hook.condition);
    return condition == "true" || condition == "1" || condition == "yes";
}

[[nodiscard]] bool hook_pattern_matches_one(std::string_view pattern, std::string_view value) {
    if (pattern.empty() || pattern == "*") return true;
    if (pattern == value) return true;
    if (pattern.ends_with("*")) return value.starts_with(pattern.substr(0, pattern.size() - 1));
    if (pattern.starts_with("*")) return value.ends_with(pattern.substr(1));
    return false;
}

[[nodiscard]] bool hook_pattern_matches(
    const std::optional<std::string>& pattern,
    std::string_view value
) {
    if (!pattern || pattern->empty()) return true;
    std::size_t start = 0;
    while (start <= pattern->size()) {
        auto sep = pattern->find('|', start);
        auto part = std::string_view(*pattern).substr(
            start,
            sep == std::string_view::npos ? std::string_view::npos : sep - start);
        if (hook_pattern_matches_one(part, value)) return true;
        if (sep == std::string_view::npos) break;
        start = sep + 1;
    }
    return false;
}

[[nodiscard]] AgentHookRunResult run_agent_command_hook(
    const cc::tools::agent_runtime::AgentHookCommand& hook,
    const AgentExecutionPlan& plan,
    std::string_view event,
    std::string_view last_assistant_message,
    std::optional<AgentToolHookContext> tool_context
) {
    const auto transcript_path = default_agent_transcript_path(plan.agent_id);
    const auto cwd = plan.working_dir.value_or(fs::current_path().string());
    const auto shell = hook.shell.empty() ? std::string("bash") : hook.shell;
    std::string command;
    command += "cd " + shell_quote(cwd) + " && ";
    command += "LOOM_HOOK_EVENT=" + shell_quote(event) + " ";
    command += "LOOM_HOOK_AGENT_ID=" + shell_quote(plan.agent_id) + " ";
    command += "LOOM_HOOK_AGENT_TYPE=" + shell_quote(plan.agent_type) + " ";
    command += "LOOM_HOOK_AGENT_TRANSCRIPT_PATH=" + shell_quote(transcript_path) + " ";
    command += "LOOM_HOOK_CWD=" + shell_quote(cwd) + " ";
    if (!last_assistant_message.empty()) {
        command += "LOOM_HOOK_LAST_ASSISTANT_MESSAGE=" + shell_quote(last_assistant_message) + " ";
    }
    if (tool_context) {
        command += "LOOM_HOOK_TOOL_NAME=" + shell_quote(tool_context->tool_name) + " ";
        command += "LOOM_HOOK_TOOL_INPUT_JSON=" + shell_quote(tool_context->tool_input_json) + " ";
        command += "LOOM_HOOK_TOOL_USE_ID=" + shell_quote(tool_context->tool_use_id) + " ";
        if (!tool_context->tool_output_preview.empty()) {
            command += "LOOM_HOOK_TOOL_OUTPUT_PREVIEW=" + shell_quote(tool_context->tool_output_preview) + " ";
        }
        if (!tool_context->tool_error.empty()) {
            command += "LOOM_HOOK_TOOL_ERROR=" + shell_quote(tool_context->tool_error) + " ";
        }
    }
    command += shell_quote(shell) + " -c " + shell_quote(hook.command) + " 2>&1";

    AgentHookRunResult result;
    FILE* pipe = cc::utils::bash::popen_spawn(command.c_str());
    if (!pipe) {
        result.exit_code = 127;
        result.output = "failed to start hook command";
        return result;
    }

    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        result.output += buffer.data();
        if (result.output.size() > 64 * 1024) {
            result.output.resize(64 * 1024);
            break;
        }
    }
    const auto status = cc::utils::bash::pclose_spawn(pipe);
    if (status == -1) {
        result.exit_code = 127;
    } else if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}

[[nodiscard]] std::string agent_hook_output_preview(std::string_view output) {
    constexpr std::size_t kMaxPreviewBytes = 16 * 1024;
    auto preview = std::string(output.substr(0, std::min(output.size(), kMaxPreviewBytes)));
    if (output.size() > kMaxPreviewBytes) preview += "\n[truncated]";
    return preview;
}

[[nodiscard]] std::optional<AgentHookContinuationStop> hook_continuation_stop_from_output(
    std::string_view output
) {
    output = trim_tool_rule(output);
    if (output.empty() || !output.starts_with('{')) return std::nullopt;

    auto parsed = cc::utils::json::parse(output);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto root = parsed->root();
    auto should_continue = root.get("continue");
    if (!should_continue.valid() || !should_continue.is_bool() || should_continue.as_bool()) {
        return std::nullopt;
    }

    AgentHookContinuationStop stop;
    auto reason = root.get("stopReason");
    if (reason.valid() && reason.is_str() && !reason.as_str().empty()) {
        stop.stop_reason = std::string(reason.as_str());
    }
    return stop;
}

[[nodiscard]] std::optional<std::string> hook_additional_context_from_output(std::string_view output) {
    output = trim_tool_rule(output);
    if (output.empty()) return std::nullopt;

    auto parsed = cc::utils::json::parse(output);
    if (!parsed) return std::nullopt;
    auto specific = parsed->root().get("hookSpecificOutput");
    if (!specific.valid() || !specific.is_obj()) return std::nullopt;
    auto event = specific.get("hookEventName");
    if (event.valid() && event.is_str() && event.as_str() != "SubagentStart") {
        return std::nullopt;
    }
    auto context = specific.get("additionalContext");
    if (!context.valid() || !context.is_str()) return std::nullopt;
    auto text = trim_tool_rule(context.as_str());
    if (text.empty()) return std::nullopt;
    return std::string(text);
}

[[nodiscard]] std::optional<std::string> pre_tool_hook_denial_reason(std::string_view output) {
    output = trim_tool_rule(output);
    if (output.empty() || !output.starts_with('{')) return std::nullopt;

    auto parsed = cc::utils::json::parse(output);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto root = parsed->root();
    if (auto decision = json_string(root, "decision"); decision && *decision == "block") {
        return json_string(root, "reason").value_or("Blocked by hook");
    }

    auto specific = root.get("hookSpecificOutput");
    if (!specific.valid() || !specific.is_obj()) return std::nullopt;
    auto event = specific.get("hookEventName");
    if (event.valid() && event.is_str() && event.as_str() != "PreToolUse") return std::nullopt;
    auto permission = specific.get("permissionDecision");
    if (!permission.valid() || !permission.is_str() || permission.as_str() != "deny") return std::nullopt;
    auto reason = specific.get("permissionDecisionReason");
    if (reason.valid() && reason.is_str() && !reason.as_str().empty()) return std::string(reason.as_str());
    return json_string(root, "reason").value_or("Blocked by hook");
}

[[nodiscard]] std::optional<std::string> pre_tool_hook_updated_input_json(std::string_view output) {
    output = trim_tool_rule(output);
    if (output.empty() || !output.starts_with('{')) return std::nullopt;

    auto parsed = cc::utils::json::parse(output);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto specific = parsed->root().get("hookSpecificOutput");
    if (!specific.valid() || !specific.is_obj()) return std::nullopt;
    auto event = specific.get("hookEventName");
    if (event.valid() && event.is_str() && event.as_str() != "PreToolUse") return std::nullopt;
    auto updated = specific.get("updatedInput");
    if (!updated.valid() || !updated.is_obj()) return std::nullopt;
    auto serialized = cc::utils::json::to_string(updated);
    if (serialized.empty()) return std::nullopt;
    return serialized;
}

[[nodiscard]] std::optional<std::string> post_tool_hook_updated_mcp_output_text(std::string_view output) {
    output = trim_tool_rule(output);
    if (output.empty() || !output.starts_with('{')) return std::nullopt;

    auto parsed = cc::utils::json::parse(output);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto specific = parsed->root().get("hookSpecificOutput");
    if (!specific.valid() || !specific.is_obj()) return std::nullopt;
    auto event = specific.get("hookEventName");
    if (event.valid() && event.is_str() && event.as_str() != "PostToolUse") return std::nullopt;
    auto updated = specific.get("updatedMCPToolOutput");
    if (!updated.valid()) return std::nullopt;
    if (updated.is_str()) return std::string(updated.as_str());
    auto serialized = cc::utils::json::to_string(updated);
    if (serialized.empty()) return std::nullopt;
    return serialized;
}

[[nodiscard]] std::optional<std::string> hook_additional_context_for_event(
    std::string_view output,
    std::string_view event_name
) {
    output = trim_tool_rule(output);
    if (output.empty() || !output.starts_with('{')) return std::nullopt;

    auto parsed = cc::utils::json::parse(output);
    if (!parsed) return std::nullopt;
    auto specific = parsed->root().get("hookSpecificOutput");
    if (!specific.valid() || !specific.is_obj()) return std::nullopt;
    auto event = specific.get("hookEventName");
    if (event.valid() && event.is_str() && event.as_str() != event_name) return std::nullopt;
    auto context = specific.get("additionalContext");
    if (!context.valid() || !context.is_str()) return std::nullopt;
    auto text = trim_tool_rule(context.as_str());
    if (text.empty()) return std::nullopt;
    return std::string(text);
}

[[nodiscard]] AgentHookExecutionResult execute_agent_frontmatter_hooks(
    const AgentExecutionPlan& plan,
    std::string_view event,
    std::string_view last_assistant_message
) {
    AgentHookExecutionResult aggregate;
    auto it = plan.frontmatter_hooks.find(std::string(event));
    if (it == plan.frontmatter_hooks.end()) return aggregate;

    for (const auto& matcher : it->second) {
        if (!hook_pattern_matches(matcher.matcher, plan.agent_type)) continue;
        for (const auto& hook : matcher.hooks) {
            if (!hook_condition_allows(hook)) continue;
            auto result = run_agent_command_hook(hook, plan, event, last_assistant_message);
            ++aggregate.hook_count;
            if (!result.output.empty()) {
                if (!aggregate.output.empty()) aggregate.output += "\n";
                aggregate.output += result.output;
                if (event == "SubagentStart") {
                    if (auto context = hook_additional_context_from_output(result.output)) {
                        aggregate.additional_contexts.push_back(std::move(*context));
                    }
                }
            }
            if (result.exit_code != 0 && !aggregate.error) {
                aggregate.error = std::format(
                    "{} hook for agent '{}' exited with code {}{}{}",
                    event,
                    plan.agent_id,
                    result.exit_code,
                    result.output.empty() ? "" : ": ",
                    result.output.empty() ? "" : result.output);
            }
        }
    }
    return aggregate;
}

[[nodiscard]] AgentHookExecutionResult execute_agent_tool_frontmatter_hooks(
    const AgentExecutionPlan& plan,
    std::string_view event,
    std::string_view tool_name,
    std::string_view tool_input_json,
    std::string_view tool_use_id,
    std::string_view tool_output_preview,
    std::string_view tool_error
) {
    AgentHookExecutionResult aggregate;
    auto it = plan.frontmatter_hooks.find(std::string(event));
    if (it == plan.frontmatter_hooks.end()) return aggregate;

    std::string current_tool_input_json{tool_input_json};
    for (const auto& matcher : it->second) {
        if (!hook_pattern_matches(matcher.matcher, tool_name)) continue;
        for (const auto& hook : matcher.hooks) {
            if (!hook_condition_allows(hook)) continue;
            AgentToolHookContext context{
                .tool_name = tool_name,
                .tool_input_json = current_tool_input_json,
                .tool_use_id = tool_use_id,
                .tool_output_preview = tool_output_preview,
                .tool_error = tool_error,
            };
            auto result = run_agent_command_hook(hook, plan, event, {}, context);
            ++aggregate.hook_count;
            if (!result.output.empty()) {
                if (!aggregate.output.empty()) aggregate.output += "\n";
                aggregate.output += result.output;
                if (auto context_text = hook_additional_context_for_event(result.output, event)) {
                    aggregate.additional_contexts.push_back(std::move(*context_text));
                }
                if (auto stop = hook_continuation_stop_from_output(result.output)) {
                    aggregate.prevent_continuation = true;
                    if (stop->stop_reason && !stop->stop_reason->empty()) {
                        aggregate.stop_reason = std::move(*stop->stop_reason);
                    }
                }
                if (event == "PreToolUse") {
                    if (auto denial = pre_tool_hook_denial_reason(result.output); denial && !aggregate.error) {
                        aggregate.error = std::move(*denial);
                    }
                    if (auto updated_input = pre_tool_hook_updated_input_json(result.output)) {
                        current_tool_input_json = *updated_input;
                        aggregate.updated_input_json = std::move(*updated_input);
                    }
                } else if (event == "PostToolUse") {
                    if (auto updated_output = post_tool_hook_updated_mcp_output_text(result.output)) {
                        aggregate.updated_mcp_tool_output_text = std::move(*updated_output);
                    }
                }
            }
            if (result.exit_code != 0 && !aggregate.error) {
                aggregate.error = std::format(
                    "{} hook for tool '{}' in agent '{}' exited with code {}{}{}",
                    event,
                    tool_name,
                    plan.agent_id,
                    result.exit_code,
                    result.output.empty() ? "" : ": ",
                    result.output.empty() ? "" : result.output);
            }
        }
    }
    return aggregate;
}

[[nodiscard]] std::string format_tool_hook_additional_context(
    std::string_view event,
    std::string_view tool_name,
    std::string_view tool_use_id,
    const std::vector<std::string>& contexts
) {
    std::string message = std::format(
        "<hook_additional_context hook=\"{}:{}\" tool_use_id=\"{}\">\n",
        event,
        tool_name,
        tool_use_id);
    for (std::size_t i = 0; i < contexts.size(); ++i) {
        if (i > 0) message += "\n\n";
        message += contexts[i];
    }
    message += "\n</hook_additional_context>";
    return message;
}

[[nodiscard]] std::string format_hook_additional_context_message(
    const std::vector<std::string>& contexts
) {
    std::string message =
        "<hook_additional_context hook=\"SubagentStart\">\n";
    for (std::size_t i = 0; i < contexts.size(); ++i) {
        if (i > 0) message += "\n\n";
        message += contexts[i];
    }
    message += "\n</hook_additional_context>";
    return message;
}

void append_hook_additional_context_messages(
    std::vector<Message>& messages,
    const std::vector<std::string>& contexts
) {
    if (contexts.empty()) return;
    messages.push_back(Message::from_text("user", format_hook_additional_context_message(contexts)));
}

[[nodiscard]] std::expected<std::optional<std::string>, std::string> normalize_agent_cwd(
    const std::optional<std::string>& cwd
) {
    if (!cwd || cwd->empty()) return std::optional<std::string>{};
    std::error_code ec;
    fs::path path = fs::path{*cwd};
    if (path.is_relative()) path = fs::current_path(ec) / path;
    if (ec) return std::unexpected(std::format("Cannot resolve current working directory: {}", ec.message()));
    path = fs::weakly_canonical(path, ec);
    if (ec) return std::unexpected(std::format("Cannot resolve agent cwd '{}': {}", *cwd, ec.message()));
    if (!fs::exists(path, ec) || !fs::is_directory(path, ec)) {
        return std::unexpected(std::format("Agent cwd does not exist or is not a directory: {}", path.string()));
    }
    return path.string();
}

void upsert_agent_record_for_plan(const AgentExecutionPlan& plan) {
    cc::tools::agent_runtime::NativeAgentRecord record{
        .agent_id = plan.agent_id,
        .agent_type = plan.agent_type,
        .parent_agent_id = plan.parent_agent_id,
        .description = plan.description,
        .name = plan.name,
        .team_name = plan.team_name,
        .cwd = plan.working_dir,
        .isolation = plan.isolation,
        .mode = plan.mode,
        .background = plan.background,
        .status = cc::tools::agent_runtime::NativeAgentStatus::Queued,
        .transcript_path = default_agent_transcript_path(plan.agent_id),
        .output_file_path = cc::tools::agent_runtime::agent_output_file_path(plan.agent_id).string(),
        .worktree_path = plan.worktree_path,
        .worktree_branch = plan.worktree_branch,
        .worktree_base_commit = plan.worktree_base_commit,
        .worktree_git_root = plan.worktree_git_root,
        .teammate_backend = plan.teammate_backend,
        .teammate_task_id = plan.teammate_task_id,
        .teammate_pane_id = plan.teammate_pane_id,
        .teammate_color = plan.teammate_color,
        .parent_session_id = plan.parent_session_id,
        .progress = 0.0,
    };
    record.capabilities = plan.allowed_tools;
    if (plan.fork_child_context && !std::ranges::contains(record.capabilities, "fork-subagent")) {
        record.capabilities.push_back("fork-subagent");
    }

    if (plan.resume_existing) {
        if (auto existing = cc::tools::agent_runtime::native_agent_store().get(plan.agent_id)) {
            record.parent_agent_id = record.parent_agent_id.or_else([&] { return existing->parent_agent_id; });
            record.description = record.description.or_else([&] { return existing->description; });
            record.name = record.name.or_else([&] { return existing->name; });
            record.team_name = record.team_name.or_else([&] { return existing->team_name; });
            record.cwd = record.cwd.or_else([&] { return existing->cwd; });
            record.isolation = record.isolation.or_else([&] { return existing->isolation; });
            record.mode = record.mode.or_else([&] { return existing->mode; });
            record.transcript_path = existing->transcript_path.value_or(default_agent_transcript_path(plan.agent_id));
            record.sidechain_jsonl_path = existing->sidechain_jsonl_path;
            record.output_file_path = existing->output_file_path.value_or(
                cc::tools::agent_runtime::agent_output_file_path(plan.agent_id).string());
            for (const auto& capability : existing->capabilities) {
                if (!std::ranges::contains(record.capabilities, capability)) {
                    record.capabilities.push_back(capability);
                }
            }
            if (plan.fork_child_context && !std::ranges::contains(record.capabilities, "fork-subagent")) {
                record.capabilities.push_back("fork-subagent");
            }
            record.transcript = std::move(existing->transcript);
            record.sidechain_entries = std::move(existing->sidechain_entries);
            record.pending_messages = std::move(existing->pending_messages);
            record.worktree_path = record.worktree_path.or_else([&] { return existing->worktree_path; });
            record.worktree_branch = record.worktree_branch.or_else([&] { return existing->worktree_branch; });
            record.worktree_base_commit = record.worktree_base_commit.or_else([&] { return existing->worktree_base_commit; });
            record.worktree_git_root = record.worktree_git_root.or_else([&] { return existing->worktree_git_root; });
            record.teammate_backend = record.teammate_backend.or_else([&] { return existing->teammate_backend; });
            record.teammate_task_id = record.teammate_task_id.or_else([&] { return existing->teammate_task_id; });
            record.teammate_pane_id = record.teammate_pane_id.or_else([&] { return existing->teammate_pane_id; });
            record.teammate_color = record.teammate_color.or_else([&] { return existing->teammate_color; });
            record.parent_session_id = record.parent_session_id.or_else([&] { return existing->parent_session_id; });
            record.worktree_cleanup_performed = existing->worktree_cleanup_performed;
        }
    }

    cc::tools::agent_runtime::native_agent_store().upsert(std::move(record));
}

[[nodiscard]] AgentWorktreeCleanupResult cleanup_agent_worktree(std::string_view agent_id) {
    auto record = cc::tools::agent_runtime::native_agent_store().get(agent_id);
    if (!record || !record->worktree_path || !record->worktree_git_root || !record->worktree_branch) {
        return {};
    }

    AgentWorktreeCleanupResult result{
        .attempted = true,
        .message = "worktree cleanup inspected",
    };

    const auto worktree_path = fs::path{*record->worktree_path};
    const auto git_root = fs::path{*record->worktree_git_root};
    const auto branch = *record->worktree_branch;

    std::error_code ec;
    if (!fs::exists(worktree_path, ec)) {
        cc::tools::agent_runtime::native_agent_store().mark_worktree_cleaned(agent_id);
        result.removed = true;
        result.message = "worktree was already absent and metadata was cleaned";
        return result;
    }

    auto status = cc::utils::git::run_git_command("status --porcelain", worktree_path);
    if (!status.success) {
        result.changed = true;
        result.message = "worktree status could not be inspected; preserving worktree";
        cc::tools::agent_runtime::native_agent_store().append_transcript(
            agent_id,
            "system: retained worktree at " + worktree_path.string() + " because status inspection failed");
        return result;
    }
    if (!status.output.empty()) {
        result.changed = true;
        result.message = "worktree has uncommitted changes and was preserved";
        cc::tools::agent_runtime::native_agent_store().append_transcript(
            agent_id,
            "system: retained worktree with uncommitted changes at " + worktree_path.string());
        return result;
    }

    if (record->worktree_base_commit && !record->worktree_base_commit->empty()) {
        auto head = cc::utils::git::run_git_command("rev-parse HEAD", worktree_path);
        if (!head.success) {
            result.changed = true;
            result.message = "worktree HEAD could not be inspected; preserving worktree";
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                agent_id,
                "system: retained worktree at " + worktree_path.string() + " because HEAD inspection failed");
            return result;
        }
        if (head.output != *record->worktree_base_commit) {
            result.changed = true;
            result.message = "worktree branch contains commits and was preserved";
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                agent_id,
                "system: retained worktree branch " + branch + " at " + worktree_path.string());
            return result;
        }
    }

    auto removed = cc::utils::git::run_git_command(
        "worktree remove --force " + shell_quote(worktree_path.string()),
        git_root);
    if (!removed.success) {
        result.changed = true;
        result.message = "worktree removal failed; preserving metadata";
        cc::tools::agent_runtime::native_agent_store().append_transcript(
            agent_id,
            "system: failed to remove worktree at " + worktree_path.string() + ": " + removed.output);
        return result;
    }

    (void)cc::utils::git::run_git_command("branch -D " + shell_quote(branch), git_root);
    cc::tools::agent_runtime::native_agent_store().mark_worktree_cleaned(agent_id);
    cc::tools::agent_runtime::native_agent_store().append_transcript(
        agent_id,
        "system: cleaned worktree " + worktree_path.string() + " and branch " + branch);
    result.removed = true;
    result.message = "worktree removed";
    return result;
}

[[nodiscard]] std::string agent_output_file_path(std::string_view agent_id) {
    return cc::tools::agent_runtime::agent_output_file_path(agent_id).string();
}

} // namespace cc::tools::agent::utils
