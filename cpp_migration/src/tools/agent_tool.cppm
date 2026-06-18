// AgentTool - Sub-agent delegation with recursive API loop
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

export module cc.tools.agent;

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

// Sub-modules created during P1-04 split
import cc.tools.agent.utils;
import cc.tools.agent.run;
import cc.tools.agent.fork;
import cc.tools.agent.resume;

export namespace cc::tools::agent {

namespace fs = std::filesystem;

using cc::core::Tool;
using cc::core::ToolInput;
using cc::core::ToolResult;
using cc::core::ToolDefinition;
using cc::core::ToolPermission;
using cc::core::InputSchema;
using cc::core::SchemaProperty;
using cc::utils::Result;
using cc::services::api::AnthropicClient;
using cc::services::api::CreateMessageRequest;
using cc::services::api::Message;
using cc::services::api::ContentBlock;
using cc::services::api::ContentBlockType;
using cc::services::api::StreamParser;
using cc::services::api::StreamEventType;
using cc::services::api::StreamContentBlockType;
using cc::services::api::get_default_client;

// =========================================================================
// Re-import helpers from sub-modules (P1-04 split)
// =========================================================================

using utils::AgentConfig;
using utils::AgentLivePermissionCheck;
using utils::AgentLivePermissionCheckFn;
using utils::AgentToolRequest;
using utils::AgentMcpToolBinding;
using utils::AgentExecutionPlan;
using utils::RemoteAgentLaunchMetadata;
using utils::AgentTodoCleanupGuard;
using utils::AgentShellTaskCleanupGuard;
using utils::AgentInlineMcpServerRuntimeState;
using utils::AgentMcpCleanupGuard;
using utils::AgentWorktreeInfo;
using utils::AgentWorktreeCleanupResult;
using utils::AgentHookRunResult;
using utils::AgentToolHookContext;
using utils::AgentHookExecutionResult;
using utils::AgentHookContinuationStop;
using utils::AgentContentReplacementState;
using utils::AgentToolResultCandidate;
using utils::AgentToolResultBudgetOutcome;

using utils::json_escape_string;
using utils::is_todo_write_tool_name;
using utils::is_agent_scoped_shell_tool_name;
using utils::text_contains_fork_boilerplate;
using utils::query_source_is_fork_child;
using utils::inject_agent_id_into_tool_input;
using utils::inject_agent_id_into_todo_input;
using utils::append_json_string_field;
using utils::append_json_optional_string_field;
using utils::agent_schema_property;
using utils::is_auto_memory_enabled;
using utils::agent_memory_dir;
using utils::load_agent_memory_prompt;
using utils::add_agent_memory_tools;
using utils::json_string;
using utils::json_bool;
using utils::json_int;
using utils::resolve_agent_relative_path;
using utils::json_object_with_string_overrides;
using utils::parse_agent_tool_request;
using utils::join_fields;
using utils::built_in_system_prompt;
using utils::resolve_agent_model;
using utils::canonical_tool_name;
using utils::env_flag_enabled;
using utils::agent_model_supports_effort;
using utils::apply_agent_effort_to_request;
using utils::tool_rule_matches_tool_name;
using utils::normalized_tool_name_is;
using utils::normalized_tool_name_in;
using utils::is_mcp_tool_name;
using utils::apply_agent_tool_execution_context_to_input;
using utils::agent_base_filter_allows_tool;
using utils::agent_type_allowed_by_permission_rules;
using utils::tool_name_allowed_by_definition;
using utils::tool_name_disallowed_by_definition;
using utils::lowercase_ascii;
using utils::lowercase_copy;
using utils::prepend_initial_prompt;
using utils::format_critical_system_reminder;
using utils::format_agent_runtime_context;
using utils::shell_quote;
using utils::sanitized_agent_file_part;
using utils::default_agent_transcript_path;
using utils::create_agent_worktree;
using utils::cleanup_agent_worktree;
using utils::execute_agent_frontmatter_hooks;
using utils::execute_agent_tool_frontmatter_hooks;
using utils::format_tool_hook_additional_context;
using utils::format_hook_additional_context_message;
using utils::append_hook_additional_context_messages;
using utils::normalize_agent_cwd;
using utils::upsert_agent_record_for_plan;
using utils::agent_output_file_path;
using utils::teammate_role_for_agent_type;
using utils::teammate_agent_color;
using utils::teammate_parent_session_id;
using utils::tool_result_content_text;
using utils::update_teammate_completion_status;
using utils::message_content_text;
using utils::message_content_sidechain_json;
using utils::message_json_object;
using utils::message_from_json_value;
using utils::fork_context_messages_from_entries;
using utils::filter_incomplete_tool_calls;
using utils::filter_resume_unresolved_tool_use_messages;
using utils::filter_resume_orphaned_thinking_messages;
using utils::filter_resume_whitespace_assistant_messages;
using utils::apply_resume_content_replacements;
using utils::resume_content_replacements_from_entries;
using utils::agent_content_replacement_state_from_entries;
using utils::mark_seen_tool_result_ids;
using utils::unbounded_tool_result_budget_names;
using utils::tool_result_budget_thresholds;
using utils::apply_agent_tool_result_budget;
using utils::resume_messages_from_sidechain_entries;
using utils::append_agent_sidechain_message;
using utils::agent_tool_input_omits_agent_type;
using utils::next_agent_id;
using utils::sanitize_teammate_agent_name;
using utils::unique_teammate_agent_name;
using utils::format_teammate_agent_id;
using utils::current_session_is_teammate;
using utils::parse_remote_launch_metadata;
using utils::remote_agent_trigger_input_json;
using utils::AGENT_MAX_TOOL_RESULTS_PER_MESSAGE_CHARS;
using utils::AGENT_DEFAULT_TOOL_RESULT_THRESHOLD_CHARS;

using fork_::forked_messages_from_parent_assistant;
using fork_::forked_messages_from_parent_assistant_entries;
using fork_::message_contains_fork_boilerplate;
using fork_::messages_contain_fork_boilerplate;
using fork_::should_reject_fork_child_agent_call;
using fork_::exact_tools_allow_tool;
using fork_::exact_tool_names_from_api_tools;
using fork_::build_implicit_fork_agent_input_json;
using cc::tools::agent::utils::agent_hook_output_preview;

using resume_::format_resumed_agent_context;
using resume_::hydrate_resume_plan_from_existing_record;

using run_::build_agent_execution_plan;
using run_::execute_agent_sleep_tool;
using run_::execute_agent_web_fetch_tool;

// TODO(agent-split): refine placement — ambiguous dependency
// (symbols above left in agent_tool root via using-import from submodules)


// =========================================================================
// AgentTool Implementation
// =========================================================================

/// AgentTool - Delegates tasks to sub-agents via recursive API loop
class AgentTool {
public:
    static constexpr std::string_view kName = "Agent";
    static constexpr std::string_view kDescription = 
        "Launch a new agent to handle complex, multi-step tasks autonomously. "
        "The agent runs in a recursive loop, making API calls and executing tools "
        "until it completes the task or reaches the turn limit.";
    
    static ToolDefinition definition() {
        return ToolDefinition{
            .name = std::string(kName),
            .description = std::string(kDescription),
            .input_schema = InputSchema{
                .properties = {
                    agent_schema_property("description", "string", "A short description of the task", true),
                    agent_schema_property("prompt", "string", "The task for the agent to perform", true),
                    agent_schema_property("subagent_type", "string", "The specialized agent type to use"),
                    agent_schema_property("model", "string", "Optional model override: sonnet, opus, haiku, or a concrete model id"),
                    agent_schema_property("run_in_background", "boolean", "Run the agent asynchronously"),
                    agent_schema_property("task", "string", "Legacy alias for prompt"),
                    agent_schema_property("skill", "string", "Legacy alias for subagent_type"),
                    agent_schema_property("name", "string", "Teammate name for agent swarms"),
                    agent_schema_property("team_name", "string", "Team name for agent swarms"),
                    agent_schema_property("mode", "string", "Permission mode for spawned teammates"),
                    agent_schema_property("isolation", "string", "Isolation mode for worktree or remote agents"),
                    agent_schema_property("cwd", "string", "Working directory override for the spawned agent")
                }
            },
            .permission = ToolPermission::Execute,
            .category = "agent"
        };
    }
    
    explicit AgentTool(AgentConfig config = {}, int current_depth = 0,
                       cc::core::ToolRegistry* registry = nullptr,
                       AgentLivePermissionCheckFn permission_check = {},
                       bool permission_hook_valid_for_background = false)
        : config_(config),
          current_depth_(current_depth),
          registry_(registry),
          permission_check_(std::move(permission_check)),
          permission_hook_valid_for_background_(permission_hook_valid_for_background) {}
    
    [[nodiscard]] bool check_permission(const ToolInput& input) const {
        (void)input;
        // Deny if at max recursion depth
        if (current_depth_ >= config_.max_depth) return false;
        return true;
    }
    
    /// Check if a specific tool is allowed for this sub-agent
    [[nodiscard]] bool is_tool_allowed(std::string_view tool_name) const {
        // Check denied list first (takes precedence)
        for (const auto& denied : config_.denied_tools) {
            if (tool_rule_matches_tool_name(denied, tool_name)) return false;
        }
        // Agent(type) rules gate Agent invocations, not the spawned worker's tool pool.
        if (!config_.allowed_tools.empty()) {
            bool has_tool_allow_rule = false;
            for (const auto& allowed : config_.allowed_tools) {
                if (!normalized_tool_name_is(tool_name, "Agent") &&
                    tool_rule_matches_tool_name(allowed, "Agent")) {
                    continue;
                }
                has_tool_allow_rule = true;
                if (tool_rule_matches_tool_name(allowed, tool_name)) return true;
            }
            return !has_tool_allow_rule;
        }
        return true;  // No restrictions = allow all
    }
    
    /// Get config for creating child agents (depth incremented)
    [[nodiscard]] AgentConfig child_config(
        std::string_view parent_agent_id = {},
        const std::optional<std::string>& parent_permission_mode = std::nullopt
    ) const {
        AgentConfig child = config_;
        // Children inherit tool restrictions
        if (!parent_agent_id.empty()) {
            child.parent_agent_id = std::string(parent_agent_id);
        }
        if (parent_permission_mode) {
            child.parent_permission_mode = parent_permission_mode;
        }
        return child;
    }
    
	    [[nodiscard]] int current_depth() const { return current_depth_; }
	    [[nodiscard]] int max_depth() const { return config_.max_depth; }

    [[nodiscard]] AgentLivePermissionCheck check_live_tool_permission(
        std::string_view tool_name,
        std::string_view input_json,
        std::string_view tool_use_id
    ) {
        if (!permission_check_) return AgentLivePermissionCheck{};
        return permission_check_(tool_name, input_json, tool_use_id);
    }
    
    [[nodiscard]] Result<ToolResult> execute(const ToolInput& input) {
        auto request = parse_agent_tool_request(input);
        if (!request) return ToolResult::error(std::format("Invalid Agent input: {}", request.error()));

        if (request->prompt.empty()) {
            return ToolResult::error("Missing required 'prompt' field");
        }

        auto plan = build_agent_execution_plan(*request, config_);
        if (!plan) return ToolResult::error(plan.error());

        if (!plan->team_name) {
            if (auto current_team = cc::utils::get_team_name(); current_team && !current_team->empty()) {
                plan->team_name = std::move(*current_team);
            }
        }

        if (current_session_is_teammate() && plan->team_name && plan->name) {
            return ToolResult::error(
                "Teammates cannot spawn other teammates - the team roster is flat. "
                "To spawn a subagent instead, omit the name parameter.");
        }

        if (cc::utils::is_in_process_teammate() && plan->team_name && plan->background) {
            return ToolResult::error(
                "In-process teammates cannot spawn background agents. "
                "Use run_in_background=false for synchronous subagents.");
        }

        if (!agent_type_allowed_by_permission_rules(
                plan->agent_type,
                config_.allowed_tools,
                config_.denied_tools)) {
            return ToolResult::error(std::format(
                "Agent type '{}' is not allowed by current Agent tool permission rules",
                plan->agent_type));
        }
        
        // Check recursion depth
        if (current_depth_ >= config_.max_depth) {
            return ToolResult::error(std::format(
                "Agent recursion depth limit reached ({}/{})", 
                current_depth_, config_.max_depth));
        }

        if (!plan->resume_existing && plan->team_name && plan->name && plan->isolation) {
            return ToolResult::error("teammate spawn does not support isolation; omit name/team_name to spawn an isolated subagent");
        }

        if (!plan->resume_existing && plan->team_name && plan->name) {
            return start_teammate_agent(std::move(*plan));
        }

        if (!plan->resume_existing && plan->isolation) {
            if (*plan->isolation == "worktree") {
                const auto parent_working_dir = plan->working_dir.value_or(fs::current_path().string());
                auto worktree = create_agent_worktree(*plan);
                if (!worktree) return ToolResult::error(worktree.error());
                plan->working_dir = worktree->path.string();
                plan->worktree_path = worktree->path.string();
                plan->worktree_branch = worktree->branch;
                plan->worktree_base_commit = worktree->head_commit;
                plan->worktree_git_root = worktree->git_root.string();
                if (plan->fork_context_includes_prompt) {
                    plan->fork_context_messages.push_back(Message::from_text(
                        "user",
                        cc::tools::agent_runtime::build_worktree_fork_notice(
                            parent_working_dir,
                            worktree->path.string())));
                } else {
                    plan->system_prompt += std::format(
                        "\n\nNative worktree isolation:\n"
                        "- worktree_path: {}\n"
                        "- worktree_branch: {}\n"
                        "- base_commit: {}\n"
                        "Run all filesystem and shell work from this worktree path.",
                        worktree->path.string(),
                        worktree->branch,
                        worktree->head_commit);
                }
            } else if (*plan->isolation == "remote") {
                return start_remote_agent(std::move(*plan));
            } else {
                return ToolResult::error(std::format("Unsupported isolation mode '{}'", *plan->isolation));
            }
        }

        if (plan->background) {
            return start_background_agent(std::move(*plan));
        }
        
        upsert_agent_record_for_plan(*plan);
        // Run the sub-agent loop
        auto result = run_agent_loop(*plan);
        (void)cleanup_agent_worktree(plan->agent_id);
        return result;
    }

private:
    [[nodiscard]] Result<ToolResult> start_remote_agent(AgentExecutionPlan plan) {
        plan.background = true;
        cc::tools::MessageRouter::instance().register_agent(plan.agent_id);
        upsert_agent_record_for_plan(plan);
        if (!plan.fork_context_includes_prompt) {
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                plan.agent_id,
                "user: " + plan.prompt);
        }
        cc::tools::agent_runtime::native_agent_store().append_transcript(
            plan.agent_id,
            "system: remote isolation launch requested");

        if (!registry_) {
            const auto error = "remote isolation requires the runtime registry so remote_trigger can be executed";
            cc::tools::agent_runtime::native_agent_store().mark_failed(plan.agent_id, error);
            return ToolResult::error(error);
        }

        auto trigger_result = registry_->execute(
            "remote_trigger",
            ToolInput::from_json(remote_agent_trigger_input_json(plan)));
        if (!trigger_result) {
            const auto error = "remote isolation trigger failed: " + trigger_result.error().message;
            cc::tools::agent_runtime::native_agent_store().mark_failed(plan.agent_id, error);
            return ToolResult::error(error);
        }

        auto trigger_output = tool_result_content_text(*trigger_result);
        if (trigger_result->is_error) {
            const auto error = "remote isolation trigger failed: " + trigger_output;
            cc::tools::agent_runtime::native_agent_store().mark_failed(plan.agent_id, error);
            return ToolResult::error(error);
        }

        cc::tools::agent_runtime::native_agent_store().mark_running(plan.agent_id);
        if (!trigger_output.empty()) {
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                plan.agent_id,
                "system: remote trigger delivered: " + trigger_output);
        }
        auto remote_metadata = parse_remote_launch_metadata(trigger_output, plan);
        cc::tools::agent_runtime::native_agent_store().set_remote_metadata(
            plan.agent_id,
            remote_metadata.task_id,
            remote_metadata.task_type,
            remote_metadata.session_id,
            remote_metadata.session_url,
            remote_metadata.title,
            plan.prompt,
            remote_metadata.metadata_json,
            remote_metadata.is_review,
            remote_metadata.is_ultraplan,
            remote_metadata.is_long_running);
        if (remote_metadata.session_id) {
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                plan.agent_id,
                "system: remote session registered: " + *remote_metadata.session_id);
        }
        const bool auto_poll_started = remote_metadata.session_id
            ? cc::tools::agent_runtime::start_remote_agent_poll_loop(plan.agent_id)
            : false;

        const auto session_url_line = remote_metadata.session_url
            ? std::format("\nsession_url: {}", *remote_metadata.session_url)
            : std::string{};
        const auto remote_task_id_line = remote_metadata.task_id && *remote_metadata.task_id != plan.agent_id
            ? std::format("\nremote_task_id: {}", *remote_metadata.task_id)
            : std::string{};
        const auto auto_poll_line = remote_metadata.session_id
            ? std::format("\nremote_auto_poll: {}", auto_poll_started ? "true" : "false")
            : std::string{};

        return ToolResult::success(std::format(
            "Remote agent launched via configured remote trigger.\n"
            "taskId: {}\n"
            "agentId: {}{}{}{}\n"
            "output_file: {}\n"
            "The agent is running remotely. You will be notified when the remote runtime reports completion.",
            plan.agent_id,
            plan.agent_id,
            remote_task_id_line,
            session_url_line,
            auto_poll_line,
            agent_output_file_path(plan.agent_id)));
    }

    [[nodiscard]] Result<ToolResult> start_teammate_agent(AgentExecutionPlan plan) {
        if (!plan.team_name || plan.team_name->empty() || !plan.name || plan.name->empty()) {
            return ToolResult::error("teammate spawn requires team_name and name");
        }

        auto team = cc::tools::global_team_store().get_by_id_or_name(*plan.team_name);
        if (!team) {
            return ToolResult::error(std::format(
                "Team '{}' not found. Call team_create before spawning teammates.",
                *plan.team_name));
        }

        auto normalized_team_name = (*team)->name;
        auto team_id = (*team)->id;
        auto teammate_name = unique_teammate_agent_name(
            sanitize_teammate_agent_name(*plan.name),
            **team);
        auto teammate_id = format_teammate_agent_id(teammate_name, normalized_team_name);
        auto output_file = agent_output_file_path(teammate_id);

        plan.agent_id = teammate_id;
        plan.name = teammate_name;
        plan.team_name = normalized_team_name;
        plan.background = true;

        auto added = cc::tools::global_team_store().add_member(team_id, cc::tools::TeamMember{
            .agent_id = teammate_id,
            .role = teammate_role_for_agent_type(plan.agent_type),
            .status = cc::tools::MemberStatus::Working,
        });
        if (!added) return ToolResult::error(std::string(cc::tools::format_error(added.error())));

        cc::utils::swarm_backends::TeammateSpawnConfig spawn_config{
            .name = teammate_name,
            .team_name = normalized_team_name,
            .color = teammate_agent_color(plan.color),
            .plan_mode_required = plan.mode == "plan",
            .permission_mode = plan.mode,
            .agent_type = plan.agent_type,
            .prompt = plan.prompt,
            .cwd = plan.working_dir.value_or(fs::current_path().string()),
            .model = plan.model.empty() ? std::nullopt : std::optional<std::string>{plan.model},
            .system_prompt = plan.system_prompt.empty()
                ? std::nullopt
                : std::optional<std::string>{plan.system_prompt},
            .worktree_path = plan.worktree_path,
            .parent_session_id = teammate_parent_session_id(),
            .permissions = plan.allowed_tools,
        };
        auto executor = cc::utils::swarm_backends::BackendRegistry::get_teammate_executor(
            config_.prefer_in_process_teammate);
        auto spawned = executor->spawn(spawn_config);
        if (!spawned.success) {
            auto error = spawned.error.value_or("failed to spawn teammate backend");
            (void)cc::tools::global_team_store().update_member_status(
                team_id,
                teammate_id,
                cc::tools::MemberStatus::Error,
                error);
            return ToolResult::error(error);
        }

        auto backend = std::string(cc::utils::swarm_backends::backend_type_name(executor->type()));
        auto task_id = std::move(spawned.task_id);
        auto pane_id = std::move(spawned.pane_id);
        auto color = spawn_config.color
            ? std::optional<std::string>{std::string(cc::utils::swarm_backends::agent_color_name(*spawn_config.color))}
            : std::nullopt;
        auto parent_session_id = std::move(spawn_config.parent_session_id);

        plan.teammate_backend = backend;
        plan.teammate_task_id = task_id;
        plan.teammate_pane_id = pane_id;
        plan.teammate_color = color;
        plan.parent_session_id = parent_session_id;

        const bool runs_in_process = executor->type() == cc::utils::swarm_backends::BackendType::InProcess;
        if (runs_in_process) {
            auto started = start_background_agent(std::move(plan));
            if (!started) return started;
            if (started->is_error) return started;
        } else {
            auto mailbox = cc::utils::write_to_mailbox(
                teammate_name,
                cc::utils::TeammateMessage{
                    .from = "team-lead",
                    .text = plan.prompt,
                    .timestamp = {},
                    .read = false,
                    .color = std::nullopt,
                    .summary = "initial teammate instructions",
                },
                std::optional<std::string_view>{std::string_view(normalized_team_name)});
            if (!mailbox) {
                (void)cc::tools::global_team_store().update_member_status(
                    team_id,
                    teammate_id,
                    cc::tools::MemberStatus::Error,
                    mailbox.error());
                return ToolResult::error("failed to write teammate mailbox: " + mailbox.error());
            }

            cc::tools::MessageRouter::instance().register_agent(plan.agent_id);
            upsert_agent_record_for_plan(plan);
            cc::tools::agent_runtime::native_agent_store().mark_running(plan.agent_id);
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                plan.agent_id,
                std::format("system: spawned teammate via {} backend", backend));
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                plan.agent_id,
                "mailbox initial prompt: " + plan.prompt);
        }

        auto task_line = task_id ? std::format("task_id: {}\n", *task_id) : std::string{};
        auto pane_line = pane_id ? std::format("pane_id: {}\n", *pane_id) : std::string{};
        auto color_line = color ? std::format("color: {}\n", *color) : std::string{};
        auto execution_state = runs_in_process
            ? (registry_ ? std::string{"running"} : std::string{"queued"})
            : std::string{"running in the external teammate backend"};
        auto delivery_channel = runs_in_process
            ? std::string{"native runtime queue"}
            : std::string{"teammate mailbox"};
        return ToolResult::success(std::format(
            "Spawned successfully.\n"
            "agent_id: {}\n"
            "teammate_id: {}\n"
            "name: {}\n"
            "team_name: {}\n"
            "backend: {}\n"
            "{}{}{}"
            "status: teammate_spawned\n"
            "outputFile: {}\n"
            "The agent is {} and will receive instructions via the {}.",
            teammate_id,
            teammate_id,
            teammate_name,
            normalized_team_name,
            backend,
            task_line,
            pane_line,
            color_line,
            output_file,
            execution_state,
            delivery_channel));
    }

    [[nodiscard]] Result<ToolResult> start_background_agent(AgentExecutionPlan plan) {
        cc::tools::MessageRouter::instance().register_agent(plan.agent_id);
        upsert_agent_record_for_plan(plan);

        auto start_hooks = execute_agent_frontmatter_hooks(plan, "SubagentStart");
        if (!start_hooks.ok()) {
            cc::tools::agent_runtime::native_agent_store().mark_failed(plan.agent_id, *start_hooks.error);
            update_teammate_completion_status(plan, false, *start_hooks.error);
            return ToolResult::error(*start_hooks.error);
        }
        if (!start_hooks.output.empty()) {
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                plan.agent_id,
                "hook SubagentStart: " + start_hooks.output);
        }
        plan.hook_additional_contexts = std::move(start_hooks.additional_contexts);

        if (!registry_) {
            if (!plan.fork_context_includes_prompt) {
                cc::tools::agent_runtime::native_agent_store().append_transcript(plan.agent_id, "user: " + plan.prompt);
            }
            return ToolResult::success(std::format(
                "Queued background agent {} ({}) but execution is deferred because no tool registry is attached.\nagentId: {}\noutputFile: {}",
                plan.agent_id,
                plan.agent_type,
                plan.agent_id,
                agent_output_file_path(plan.agent_id)));
        }

        auto config = config_;
        auto depth = current_depth_;
        auto* registry = registry_;
        auto permission_check = permission_hook_valid_for_background_
            ? permission_check_
            : AgentLivePermissionCheckFn{};
        auto permission_hook_valid_for_background = permission_hook_valid_for_background_;
        auto agent_id = plan.agent_id;
        std::thread([
            plan = std::move(plan),
            config = std::move(config),
            depth,
            registry,
            permission_check = std::move(permission_check),
            permission_hook_valid_for_background,
            agent_id
        ]() mutable {
            cc::tools::agent_runtime::native_agent_store().mark_running(agent_id);
            AgentTool worker(
                std::move(config),
                depth,
                registry,
                std::move(permission_check),
                permission_hook_valid_for_background);
            auto result = worker.run_agent_loop(plan);
            (void)cleanup_agent_worktree(agent_id);
            if (result && !result->is_error) {
                auto output = tool_result_content_text(*result);
                cc::tools::agent_runtime::native_agent_store().mark_completed(agent_id, std::move(output));
                update_teammate_completion_status(plan, true, tool_result_content_text(*result));
            } else if (result) {
                auto error_text = tool_result_content_text(*result);
                if (auto current = cc::tools::agent_runtime::native_agent_store().get(agent_id);
                    current && current->status == cc::tools::agent_runtime::NativeAgentStatus::Cancelled) {
                    update_teammate_completion_status(plan, false, current->error.value_or(error_text));
                    return;
                }
                cc::tools::agent_runtime::native_agent_store().mark_failed(agent_id, error_text);
                update_teammate_completion_status(plan, false, error_text);
            } else {
                auto error_text = result.error().format();
                if (auto current = cc::tools::agent_runtime::native_agent_store().get(agent_id);
                    current && current->status == cc::tools::agent_runtime::NativeAgentStatus::Cancelled) {
                    update_teammate_completion_status(plan, false, current->error.value_or(error_text));
                    return;
                }
                cc::tools::agent_runtime::native_agent_store().mark_failed(agent_id, error_text);
                update_teammate_completion_status(plan, false, error_text);
            }
        }).detach();

        return ToolResult::success(std::format(
            "Started background agent {} ({})\nagentId: {}\noutputFile: {}",
            plan.agent_id,
            plan.agent_type,
            plan.agent_id,
            agent_output_file_path(plan.agent_id)));
    }

    [[nodiscard]] bool plan_allows_generic_mcp_tool(const AgentExecutionPlan& plan) const {
        return !plan.agent_mcp_tools.empty() &&
            is_tool_allowed("mcp") &&
            !tool_name_disallowed_by_definition("mcp", plan.disallowed_tools);
    }

    [[nodiscard]] bool agent_base_filter_allows_tool(
        std::string_view tool_name,
        const AgentExecutionPlan& plan
    ) const {
        const auto permission_mode = plan.mode
            ? std::optional<std::string_view>{std::string_view{*plan.mode}}
            : std::nullopt;
        const bool in_process_teammate =
            plan.team_name && plan.teammate_backend &&
            normalized_tool_name_is(*plan.teammate_backend, "in-process");
        return cc::tools::agent::agent_base_filter_allows_tool(
            tool_name,
            plan.is_built_in,
            plan.background,
            permission_mode,
            in_process_teammate);
    }

    [[nodiscard]] bool is_tool_allowed_for_plan(
        std::string_view tool_name,
        const AgentExecutionPlan& plan
    ) const {
        if (tool_name == "mcp" && plan_allows_generic_mcp_tool(plan)) {
            return true;
        }
        return agent_base_filter_allows_tool(tool_name, plan) &&
            is_tool_allowed(tool_name) &&
            tool_name_allowed_by_definition(tool_name, plan.allowed_tools) &&
            !tool_name_disallowed_by_definition(tool_name, plan.disallowed_tools);
    }

    [[nodiscard]] std::optional<std::string> mcp_scope_error_for_plan(
        const ToolInput& input,
        const AgentExecutionPlan& plan
    ) const {
        if (plan.agent_mcp_servers.empty()) return std::nullopt;

        auto doc = cc::utils::json::parse(input.json());
        if (!doc || !doc->root().is_obj()) {
            return "MCP input must be a JSON object in this sub-agent context";
        }

        auto root = doc->root();
        auto server = json_string(root, "server_name").or_else([&] { return json_string(root, "server"); });
        if (!server || server->empty()) {
            return "MCP server_name is required in this sub-agent context";
        }

        for (const auto& allowed : plan.agent_mcp_servers) {
            if (*server == allowed) return std::nullopt;
        }
        return std::format(
            "MCP server '{}' is not available in this sub-agent context. Available servers: {}",
            *server,
            join_fields(plan.agent_mcp_servers));
    }

    [[nodiscard]] std::vector<cc::services::api::ToolDefinition> api_tools_for_plan(
        const AgentExecutionPlan& plan
    ) const {
        std::vector<cc::services::api::ToolDefinition> tools;
        if (!registry_) return tools;

        for (const auto& definition : registry_->get_visible_definitions()) {
            if (plan.use_exact_tools) {
                if (!exact_tools_allow_tool(plan, definition.name)) continue;
            } else if (!is_tool_allowed_for_plan(definition.name, plan)) {
                continue;
            }
            tools.push_back(cc::services::api::ToolDefinition{
                .name = definition.name,
                .description = definition.description,
                .input_schema_json = definition.input_schema.to_json(),
                .defer_load = false,
            });
        }
        return tools;
    }

    void append_queued_agent_messages(
        const AgentExecutionPlan& plan,
        std::vector<Message>& messages
    ) const {
        for (auto& pending_message : cc::tools::agent_runtime::native_agent_store().take_pending_messages(plan.agent_id)) {
            auto queued_message = Message::from_text("user", pending_message);
            append_agent_sidechain_message(plan.agent_id, queued_message);
            messages.push_back(std::move(queued_message));
        }
        SendMessageTool inbox(plan.agent_id);
        while (auto message = inbox.receive()) {
            auto queued_message = Message::from_text(
                "user",
                std::format(
                    "[Message from {} priority={}]\n{}",
                    message->from_agent,
                    message_priority_name(message->priority),
                    message->content));
            append_agent_sidechain_message(plan.agent_id, queued_message);
            messages.push_back(std::move(queued_message));
        }
    }

    /// Run the sub-agent's recursive API loop
    [[nodiscard]] Result<ToolResult> run_agent_loop(const AgentExecutionPlan& plan) {
        auto resumed_record = plan.resume_existing
            ? cc::tools::agent_runtime::native_agent_store().get(plan.agent_id)
            : std::optional<cc::tools::agent_runtime::NativeAgentRecord>{};
        AgentTodoCleanupGuard todo_cleanup{plan.agent_id};
        AgentShellTaskCleanupGuard shell_task_cleanup{plan.agent_id};
        AgentMcpCleanupGuard mcp_cleanup{
            .agent_id = plan.agent_id,
            .inline_servers = plan.inline_mcp_server_states,
        };

        std::string final_output;
        bool agent_hook_prevented_continuation = false;
        std::optional<std::string> agent_hook_stop_reason;
        bool lifecycle_hooks_active = plan.background;
        bool stop_hooks_executed = false;
        auto run_stop_hooks_once = [&](std::string_view last_assistant_message) -> std::optional<std::string> {
            if (!lifecycle_hooks_active || stop_hooks_executed) return std::nullopt;
            stop_hooks_executed = true;
            auto stop_hooks = execute_agent_frontmatter_hooks(plan, "SubagentStop", last_assistant_message);
            if (!stop_hooks.output.empty()) {
                cc::tools::agent_runtime::native_agent_store().append_transcript(
                    plan.agent_id,
                    "hook SubagentStop: " + stop_hooks.output);
            }
            if (!stop_hooks.ok()) {
                cc::tools::agent_runtime::native_agent_store().append_transcript(
                    plan.agent_id,
                    "hook SubagentStop failed: " + *stop_hooks.error);
                return *stop_hooks.error;
            }
            return std::nullopt;
        };

        auto fail_agent = [&](std::string error) -> Result<ToolResult> {
            if (auto hook_error = run_stop_hooks_once(error)) {
                error += "\nSubagentStop hook failed: " + *hook_error;
            }
            cc::tools::agent_runtime::native_agent_store().mark_failed(plan.agent_id, error);
            return ToolResult::error(std::move(error));
        };

        cc::tools::agent_runtime::native_agent_store().mark_running(plan.agent_id);
        if (!plan.fork_context_includes_prompt) {
            cc::tools::agent_runtime::native_agent_store().append_transcript(plan.agent_id, "user: " + plan.prompt);
        }
        auto hook_additional_contexts = plan.hook_additional_contexts;
        if (!plan.background) {
            auto start_hooks = execute_agent_frontmatter_hooks(plan, "SubagentStart");
            if (!start_hooks.ok()) return fail_agent(*start_hooks.error);
            if (!start_hooks.output.empty()) {
                cc::tools::agent_runtime::native_agent_store().append_transcript(
                    plan.agent_id,
                    "hook SubagentStart: " + start_hooks.output);
            }
            hook_additional_contexts.insert(
                hook_additional_contexts.end(),
                std::make_move_iterator(start_hooks.additional_contexts.begin()),
                std::make_move_iterator(start_hooks.additional_contexts.end()));
            lifecycle_hooks_active = true;
        }
        
        auto client = get_default_client();
        
        // Build initial messages
        std::vector<Message> messages;
        for (const auto& context_message : plan.fork_context_messages) {
            append_agent_sidechain_message(plan.agent_id, context_message);
            messages.push_back(context_message);
        }
        for (const auto& skill_message : plan.preloaded_skill_messages) {
            auto message = Message::from_text("user", skill_message);
            append_agent_sidechain_message(plan.agent_id, message);
            messages.push_back(std::move(message));
        }
        const auto hook_message_start = messages.size();
        append_hook_additional_context_messages(messages, hook_additional_contexts);
        for (std::size_t i = hook_message_start; i < messages.size(); ++i) {
            append_agent_sidechain_message(plan.agent_id, messages[i]);
        }
        auto structured_resume_messages = resumed_record
            ? resume_messages_from_sidechain_entries(resumed_record->sidechain_entries)
            : std::vector<Message>{};
        if (!structured_resume_messages.empty()) {
            messages.insert(
                messages.end(),
                std::make_move_iterator(structured_resume_messages.begin()),
                std::make_move_iterator(structured_resume_messages.end()));
        } else if (resumed_record && !resumed_record->transcript.empty()) {
            auto message = Message::from_text("user", format_resumed_agent_context(*resumed_record));
            append_agent_sidechain_message(plan.agent_id, message);
            messages.push_back(std::move(message));
        }
        if (plan.agent_mcp_context_message) {
            auto message = Message::from_text("user", *plan.agent_mcp_context_message);
            append_agent_sidechain_message(plan.agent_id, message);
            messages.push_back(std::move(message));
        }
        if (!plan.fork_context_includes_prompt) {
            auto prompt_message = Message::from_text("user", plan.prompt);
            append_agent_sidechain_message(plan.agent_id, prompt_message);
            messages.push_back(std::move(prompt_message));
        }
        if (plan.critical_system_reminder && !plan.system_prompt_overridden) {
            auto message = Message::from_text(
                "user",
                format_critical_system_reminder(*plan.critical_system_reminder));
            append_agent_sidechain_message(plan.agent_id, message);
            messages.push_back(std::move(message));
        }
        
        auto content_replacement_state = resumed_record
            ? agent_content_replacement_state_from_entries(resumed_record->sidechain_entries)
            : AgentContentReplacementState{};
        mark_seen_tool_result_ids(content_replacement_state, messages);
        const auto content_replacement_skip_tools = registry_
            ? unbounded_tool_result_budget_names(registry_->get_visible_definitions())
            : std::unordered_set<std::string>{};
        const auto content_replacement_thresholds = registry_
            ? tool_result_budget_thresholds(registry_->get_visible_definitions())
            : std::unordered_map<std::string, std::size_t>{};

        auto cancel_if_requested = [&](std::string_view phase) -> std::optional<std::string> {
            if (!cc::tools::agent_runtime::native_agent_store().is_cancel_requested(plan.agent_id)) {
                return std::nullopt;
            }
            const auto reason = std::format("Agent {} cancelled {}", plan.agent_id, phase);
            (void)run_stop_hooks_once(reason);
            cc::tools::agent_runtime::native_agent_store().mark_cancelled(plan.agent_id, reason);
            return reason;
        };
        auto update_turn_progress = [&](int turn, double phase) {
            const auto denominator = static_cast<double>(std::max(plan.max_turns, 1));
            const auto progress = std::min(0.99, (static_cast<double>(turn) + phase) / denominator);
            cc::tools::agent_runtime::native_agent_store().update_progress(plan.agent_id, progress);
        };
        
        for (int turn = 0; turn < plan.max_turns; ++turn) {
            if (auto reason = cancel_if_requested(std::format("before turn {}", turn + 1))) {
                return ToolResult::error(std::move(*reason));
            }
            update_turn_progress(turn, 0.0);
            append_queued_agent_messages(plan, messages);
            (void)apply_agent_tool_result_budget(
                plan.agent_id,
                messages,
                content_replacement_state,
                content_replacement_skip_tools,
                content_replacement_thresholds);

            // Build request
            CreateMessageRequest req;
            req.model = plan.model;
            req.messages = messages;
            req.max_tokens = 16384;
            req.stream = true;
            if (!plan.system_prompt.empty()) req.system_prompt = plan.system_prompt;
            req.tools = api_tools_for_plan(plan);
            apply_agent_effort_to_request(req, plan.effort);
            
            // Perform streaming request
            auto stream_result = client.create_message_stream(req);
            if (!stream_result) {
                return fail_agent(std::format(
                    "Agent API call failed: {}", stream_result.error().message()));
            }
            update_turn_progress(turn, 0.05);
            
            auto& parser = *stream_result;
            
            // Consume the stream and accumulate content blocks
            std::string text_content;
            std::vector<ContentBlock> tool_uses;
            std::string stop_reason;
            ContentBlock current_block;
            std::string accumulated_json;
            bool in_block = false;

            while (true) {
                if (auto reason = cancel_if_requested("while waiting for model stream")) {
                    return ToolResult::error(std::move(*reason));
                }
                auto event_result = parser.next_event();
                if (!event_result) break;
                if (!event_result->has_value()) {
                    if (parser.has_error()) {
                        const auto error = parser.error_details();
                        const auto error_text = error && !error->error_message.empty()
                            ? error->error_message
                            : std::string{"stream connection failed"};
                        return fail_agent(std::format("Agent stream error: {}", error_text));
                    }
                    if (parser.is_finished()) break;
                    // Brief wait for producer
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                
                const auto& event = **event_result;
                
                switch (event.type) {
                    case StreamEventType::MessageStart:
                        update_turn_progress(turn, 0.10);
                        break;

                    case StreamEventType::ContentBlockStart:
                        update_turn_progress(turn, 0.20);
                        in_block = true;
                        current_block = ContentBlock{};
                        if (event.block_type == StreamContentBlockType::Text) {
                            current_block.type = ContentBlockType::Text;
                        } else if (event.block_type == StreamContentBlockType::ToolUse) {
                            current_block.type = ContentBlockType::ToolUse;
                            current_block.tool_use_id = event.tool_use_id;
                            current_block.tool_name = event.tool_name;
                            accumulated_json.clear();
                        }
                        break;
                        
                    case StreamEventType::ContentBlockDelta:
                        update_turn_progress(turn, 0.35);
                        if (event.delta.type == StreamContentBlockType::Text) {
                            current_block.text += event.delta.text;
                        } else if (event.delta.type == StreamContentBlockType::ToolUse) {
                            accumulated_json += event.delta.partial_json;
                        }
                        break;
                        
                    case StreamEventType::ContentBlockStop:
                        update_turn_progress(turn, 0.45);
                        if (in_block) {
                            if (current_block.type == ContentBlockType::Text) {
                                text_content += current_block.text;
                            } else if (current_block.type == ContentBlockType::ToolUse) {
                                current_block.tool_input_json = accumulated_json;
                                tool_uses.push_back(current_block);
                            }
                            in_block = false;
                        }
                        break;
                        
                    case StreamEventType::MessageDelta:
                        update_turn_progress(turn, 0.55);
                        if (event.message_delta.stop_reason) {
                            stop_reason = *event.message_delta.stop_reason;
                        }
                        break;
                        
                    case StreamEventType::MessageStop:
                        update_turn_progress(turn, 0.65);
                        goto stream_done;
                        
                    case StreamEventType::Error:
                        return fail_agent(std::format(
                            "Agent stream error: {}", event.error.error_message));
                        
                    default:
                        break;
                }
            }
            stream_done:
            
            // If stop_reason is "end_turn" and no tool_use, we're done
            if (stop_reason == "end_turn" || tool_uses.empty()) {
                final_output = text_content;
                if (!text_content.empty()) {
                    auto assistant_message = Message::from_text("assistant", text_content);
                    append_agent_sidechain_message(plan.agent_id, assistant_message);
                    cc::tools::agent_runtime::native_agent_store().append_transcript(
                        plan.agent_id,
                        "assistant: " + text_content);
                }
                break;
            }
            
            // Add assistant message to history
            Message assistant_msg;
            assistant_msg.role = "assistant";
            if (!text_content.empty()) {
                assistant_msg.content.push_back(ContentBlock{
                    .type = ContentBlockType::Text,
                    .text = text_content
                });
            }
            for (auto& tu : tool_uses) {
                assistant_msg.content.push_back(tu);
            }
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                plan.agent_id,
                "assistant: " + message_content_text(assistant_msg));
            append_agent_sidechain_message(plan.agent_id, assistant_msg);
            messages.push_back(std::move(assistant_msg));
            const auto& parent_assistant_message = messages.back();
            update_turn_progress(turn, 0.70);

            // Execute tools and add results
            Message tool_result_msg;
            tool_result_msg.role = "user";
            for (const auto& tu : tool_uses) {
                if (auto reason = cancel_if_requested("before executing tool " + tu.tool_name)) {
                    return ToolResult::error(std::move(*reason));
                }
                update_turn_progress(turn, 0.75);
                ContentBlock result_block;
                result_block.type = ContentBlockType::ToolResult;
                result_block.tool_use_id = tu.tool_use_id;
                auto tool_input_json = tu.tool_input_json;
                bool post_tool_hooks_enabled = false;
                bool tool_execution_succeeded = true;
                bool pre_tool_contexts_appended = false;
                bool tool_hook_prevented_continuation = false;
                std::optional<std::string> tool_hook_stop_reason;
                std::vector<std::string> pre_tool_additional_contexts;

                auto append_tool_hook_context = [&](std::string_view event, const std::vector<std::string>& contexts) {
                    if (contexts.empty()) return;
                    if (!result_block.text.empty()) result_block.text += "\n\n";
                    result_block.text += format_tool_hook_additional_context(
                        event,
                        tu.tool_name,
                        tu.tool_use_id,
                        contexts);
                };
                auto append_pre_tool_context_once = [&] {
                    if (pre_tool_contexts_appended) return;
                    pre_tool_contexts_appended = true;
                    append_tool_hook_context("PreToolUse", pre_tool_additional_contexts);
                };
                auto record_hook_prevented_continuation = [&](
                    const AgentHookExecutionResult& hooks,
                    std::string_view fallback_reason
                ) {
                    if (!hooks.prevent_continuation) return;
                    tool_hook_prevented_continuation = true;
                    tool_hook_stop_reason = hooks.stop_reason.value_or(std::string(fallback_reason));
                };

                if (!registry_) {
                    result_block.text = "[Tool execution not available: no registry]";
                    tool_execution_succeeded = false;
                } else if (should_reject_fork_child_agent_call(plan, tu.tool_name, tu.tool_input_json)) {
                    result_block.text =
                        "[Tool execution error: Fork is not available inside a forked worker. Complete your task directly using your tools.]";
                    tool_execution_succeeded = false;
                } else if (!is_tool_allowed_for_plan(tu.tool_name, plan)) {
                    result_block.text = std::format(
                        "[Tool '{}' not available in sub-agent context]", tu.tool_name);
                    tool_execution_succeeded = false;
                } else if (tu.tool_name == "Agent" && current_depth_ + 1 >= config_.max_depth) {
                    result_block.text = std::format(
                        "[Agent recursion depth limit reached ({}/{})]",
                        current_depth_ + 1, config_.max_depth);
                    tool_execution_succeeded = false;
                } else {
                    tool_input_json = apply_agent_tool_execution_context_to_input(
                        tu.tool_name,
                        tu.tool_input_json,
                        plan);
                    if (tu.tool_name == "Agent" && agent_tool_input_omits_agent_type(tool_input_json)) {
                        tool_input_json = build_implicit_fork_agent_input_json(
                            tool_input_json,
                            plan,
                            parent_assistant_message,
                            req.tools);
                    }

                    auto pre_hooks = execute_agent_tool_frontmatter_hooks(
                        plan,
                        "PreToolUse",
                        tu.tool_name,
                        tool_input_json,
                        tu.tool_use_id);
                    if (!pre_hooks.output.empty()) {
                        cc::tools::agent_runtime::native_agent_store().append_transcript(
                            plan.agent_id,
                            std::format("hook PreToolUse:{}: {}", tu.tool_name, pre_hooks.output));
                    }
                    if (!pre_hooks.ok()) {
                        result_block.text = std::format(
                            "[Tool execution denied by PreToolUse hook: {}]", *pre_hooks.error);
                        tool_execution_succeeded = false;
                        record_hook_prevented_continuation(pre_hooks, "Execution stopped by PreToolUse hook");
                        pre_tool_additional_contexts = std::move(pre_hooks.additional_contexts);
                        append_pre_tool_context_once();
                    } else {
                        record_hook_prevented_continuation(pre_hooks, "Execution stopped by PreToolUse hook");
                        pre_tool_additional_contexts = std::move(pre_hooks.additional_contexts);
                        if (pre_hooks.updated_input_json) {
                            tool_input_json = *pre_hooks.updated_input_json;
                            tool_input_json = apply_agent_tool_execution_context_to_input(
                                tu.tool_name,
                                tool_input_json,
                                plan);
                            cc::tools::agent_runtime::native_agent_store().append_transcript(
                                plan.agent_id,
                                std::format("hook PreToolUse:{} updated input: {}", tu.tool_name, tool_input_json));
                        }
                        bool skip_tool_execution = false;
                        auto live_permission = check_live_tool_permission(
                            tu.tool_name,
                            tool_input_json,
                            tu.tool_use_id);
                        if (!live_permission.allowed) {
                            auto reason = live_permission.message.value_or(
                                std::format("Permission denied for tool: {}", tu.tool_name));
                            result_block.text = std::format(
                                "[Tool execution denied by permission hook: {}]",
                                reason);
                            tool_execution_succeeded = false;
                            post_tool_hooks_enabled = true;
                            skip_tool_execution = true;
                        } else if (live_permission.updated_input_json) {
                            tool_input_json = *live_permission.updated_input_json;
                            tool_input_json = apply_agent_tool_execution_context_to_input(
                                tu.tool_name,
                                tool_input_json,
                                plan);
                            cc::tools::agent_runtime::native_agent_store().append_transcript(
                                plan.agent_id,
                                std::format(
                                    "permission hook {} updated input: {}",
                                    tu.tool_name,
                                    tool_input_json));
                        }
                        if (!skip_tool_execution) {
                            auto tool_input = ToolInput::from_json(tool_input_json);
                            if (auto scope_error = mcp_scope_error_for_plan(tool_input, plan)) {
                                result_block.text = std::format("[Tool execution error: {}]", *scope_error);
                                tool_execution_succeeded = false;
                                post_tool_hooks_enabled = true;
                                skip_tool_execution = true;
                            }
                            if (!skip_tool_execution) {
                                post_tool_hooks_enabled = true;
                                if (normalized_tool_name_is(tu.tool_name, "sleep")) {
                                    auto exec_result = execute_agent_sleep_tool(plan.agent_id, tool_input);
                                    if (auto reason = cancel_if_requested("while executing tool sleep")) {
                                        return ToolResult::error(std::move(*reason));
                                    }
                                    tool_execution_succeeded = !exec_result.is_error;
                                    std::string output;
                                    for (const auto& c : exec_result.content) {
                                        if (!output.empty()) output += "\n";
                                        output += c.text;
                                    }
                                    result_block.text = std::move(output);
                                } else if (normalized_tool_name_is(tu.tool_name, "WebFetch")) {
                                    auto exec_result = execute_agent_web_fetch_tool(plan.agent_id, tool_input);
                                    if (auto reason = cancel_if_requested("while executing tool WebFetch")) {
                                        return ToolResult::error(std::move(*reason));
                                    }
                                    tool_execution_succeeded = !exec_result.is_error;
                                    std::string output;
                                    for (const auto& c : exec_result.content) {
                                        if (!output.empty()) output += "\n";
                                        output += c.text;
                                    }
                                    result_block.text = std::move(output);
                                } else if (tu.tool_name == "Agent") {
                                    AgentTool child(
                                        child_config(plan.agent_id, plan.mode),
                                        current_depth_ + 1,
                                        registry_,
                                        permission_check_,
                                        permission_hook_valid_for_background_);
                                    auto child_result = child.execute(tool_input);
                                    if (child_result) {
                                        tool_execution_succeeded = !child_result->is_error;
                                        std::string output;
                                        for (const auto& c : child_result->content) {
                                            if (!output.empty()) output += "\n";
                                            output += c.text;
                                        }
                                        result_block.text = std::move(output);
                                    } else {
                                        tool_execution_succeeded = false;
                                        result_block.text = std::format(
                                            "[Tool execution error: {}]", child_result.error().format());
                                    }
                                } else {
                                    auto exec_result = registry_->execute(tu.tool_name, tool_input);
                                    if (auto reason = cancel_if_requested("while executing tool " + tu.tool_name)) {
                                        return ToolResult::error(std::move(*reason));
                                    }
                                    if (exec_result) {
                                        tool_execution_succeeded = !exec_result->is_error;
                                        // Concatenate all content blocks from the tool result
                                        std::string output;
                                        for (const auto& c : exec_result->content) {
                                            if (!output.empty()) output += "\n";
                                            output += c.text;
                                        }
                                        result_block.text = std::move(output);
                                    } else {
                                        tool_execution_succeeded = false;
                                        result_block.text = std::format(
                                            "[Tool execution error: {}]", exec_result.error().message);
                                    }
                                }
                            }
                        }
                    }
                }

                if (post_tool_hooks_enabled) {
                    const auto hook_event = tool_execution_succeeded ? std::string_view{"PostToolUse"} : std::string_view{"PostToolUseFailure"};
                    const auto output_preview = tool_execution_succeeded
                        ? agent_hook_output_preview(result_block.text)
                        : std::string{};
                    const auto error_preview = tool_execution_succeeded
                        ? std::string{}
                        : agent_hook_output_preview(result_block.text);
                    auto post_hooks = execute_agent_tool_frontmatter_hooks(
                        plan,
                        hook_event,
                        tu.tool_name,
                        tool_input_json,
                        tu.tool_use_id,
                        output_preview,
                        error_preview);
                    if (!post_hooks.output.empty()) {
                        cc::tools::agent_runtime::native_agent_store().append_transcript(
                            plan.agent_id,
                            std::format("hook {}:{}: {}", hook_event, tu.tool_name, post_hooks.output));
                    }
                    if (!post_hooks.ok()) {
                        if (!result_block.text.empty()) result_block.text += "\n\n";
                        result_block.text += std::format(
                            "[{} hook error: {}]", hook_event, *post_hooks.error);
                    }
                    if (hook_event == "PostToolUse") {
                        if (post_hooks.ok() &&
                            is_mcp_tool_name(tu.tool_name) &&
                            post_hooks.updated_mcp_tool_output_text) {
                            result_block.text = *post_hooks.updated_mcp_tool_output_text;
                            cc::tools::agent_runtime::native_agent_store().append_transcript(
                                plan.agent_id,
                                std::format(
                                    "hook PostToolUse:{} updated MCP output: {}",
                                    tu.tool_name,
                                    agent_hook_output_preview(result_block.text)));
                        }
                        record_hook_prevented_continuation(post_hooks, "Execution stopped by PostToolUse hook");
                    }
                    append_pre_tool_context_once();
                    append_tool_hook_context(hook_event, post_hooks.additional_contexts);
                }
                append_pre_tool_context_once();
                if (tool_hook_prevented_continuation) {
                    if (!result_block.text.empty()) result_block.text += "\n\n";
                    result_block.text += std::format(
                        "[Hook stopped continuation: {}]",
                        tool_hook_stop_reason.value_or("Execution stopped by hook"));
                }

                tool_result_msg.content.push_back(std::move(result_block));
                if (tool_hook_prevented_continuation) {
                    agent_hook_prevented_continuation = true;
                    agent_hook_stop_reason = std::move(tool_hook_stop_reason);
                }
                update_turn_progress(turn, 0.85);
            }
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                plan.agent_id,
                "user: " + message_content_text(tool_result_msg));
            if (plan.critical_system_reminder) {
                tool_result_msg.content.push_back(ContentBlock{
                    .type = ContentBlockType::Text,
                    .text = format_critical_system_reminder(*plan.critical_system_reminder)
                });
            }
            append_agent_sidechain_message(plan.agent_id, tool_result_msg);
            messages.push_back(std::move(tool_result_msg));
            update_turn_progress(turn, 0.90);
            
            // Record partial output
            if (!text_content.empty()) {
                final_output += text_content + "\n";
            }
            if (agent_hook_prevented_continuation) {
                const auto stop_text = agent_hook_stop_reason.value_or("Execution stopped by hook");
                if (!final_output.empty() && !final_output.ends_with('\n')) final_output += "\n";
                final_output += stop_text;
                cc::tools::agent_runtime::native_agent_store().append_transcript(
                    plan.agent_id,
                    "hook stopped continuation: " + stop_text);
                break;
            }
        }
        
        if (final_output.empty()) {
            final_output = "[Agent completed without producing output]";
        }

        if (auto hook_error = run_stop_hooks_once(final_output)) return fail_agent(*hook_error);
        cc::tools::agent_runtime::native_agent_store().mark_completed(plan.agent_id, final_output);
        
        return ToolResult::success(final_output);
    }
    
    AgentConfig config_;
    int current_depth_ = 0;
    cc::core::ToolRegistry* registry_ = nullptr;
    AgentLivePermissionCheckFn permission_check_;
    bool permission_hook_valid_for_background_ = false;
};

} // namespace cc::tools::agent

// Export main tool class
export namespace cc::tools {
    using cc::tools::agent::AgentTool;
    using cc::tools::agent::AgentConfig;
    using cc::tools::agent::AgentLivePermissionCheck;
    using cc::tools::agent::AgentLivePermissionCheckFn;

    /// Factory: create AgentTool wrapped as ITool (adapts Result types across modules)
    [[nodiscard]] auto make_agent_tool(AgentConfig config = {},
                                        int depth = 0,
                                        cc::core::ToolRegistry* registry = nullptr,
                                        AgentLivePermissionCheckFn permission_check = {},
                                        bool permission_hook_valid_for_background = false)
        -> std::unique_ptr<cc::core::ITool> {
        struct Adapter final : cc::core::ITool {
            AgentTool tool_;
            cc::core::ToolDefinition def_ = AgentTool::definition();

            explicit Adapter(
                AgentConfig cfg,
                int d,
                cc::core::ToolRegistry* reg,
                AgentLivePermissionCheckFn permission_check,
                bool hook_valid_for_background
            ) : tool_(std::move(cfg), d, reg, std::move(permission_check), hook_valid_for_background) {}

            const cc::core::ToolDefinition& definition() const override { return def_; }
            std::expected<cc::core::ToolResult, cc::core::Error> execute(const cc::core::ToolInput& input) override {
                auto result = tool_.execute(input);
                if (result) return std::move(*result);
                return std::unexpected(cc::core::Error::make(
                    cc::core::ErrorCode::ToolExecutionFailed, result.error().format()));
            }
            bool check_permission(const cc::core::ToolInput& input) const override {
                return tool_.check_permission(input);
            }
        };
        return std::make_unique<Adapter>(
            std::move(config),
            depth,
            registry,
            std::move(permission_check),
            permission_hook_valid_for_background);
    }
}
