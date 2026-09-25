// Implementation unit for cc.tools.runtime_registry — the
// execute_simple_runtime_tool mega dispatcher. The team_create/team_delete
// branches live in runtime_registry_team_dispatch.cpp; the function-local
// static shared map stays function-local here.
module;

module cc.tools.runtime_registry;

import std;

import cc.tools.tool;
import cc.tools.ask_user;
import cc.tools.plan_mode;
import cc.tools.worktree;
import cc.tools.mcp;
import cc.tools.powershell;
import cc.tools.cron;
import cc.tools.runtime_message_delivery;
import cc.tools.skill;
import cc.tools.sleep;
import cc.tools.tungsten_tool;
import cc.tools.workflow;
import cc.tools.agent_runtime;

namespace cc::tools::detail {

using cc::core::Result;
using cc::core::ToolInput;
using cc::core::ToolRegistry;
using cc::core::ToolResult;

[[nodiscard]] Result<ToolResult> execute_simple_runtime_tool(
    std::string_view name,
    const ToolInput& input,
    ToolRegistry* registry
) {
    auto json = input.json();
    if (name == "ask_user_question") {
        auto question = json_string(json, "question").value_or("Continue?");
        auto default_answer = json_string(json, "default_answer");

        // Use the global UI responder if set (e.g. dialog-based prompt).
        // Falls back to stdio for headless / non-interactive builds.
        auto& responder = cc::tools::get_global_ask_user_responder();
        if (responder) {
            auto result = responder(question, default_answer);
            if (result.has_value()) {
                return ToolResult::success(*result);
            }
            return ToolResult::error("User cancelled the prompt");
        }

        // Fallback: stdio
        std::cout << "\n" << question << "\n> ";
        std::string answer;
        if (!std::getline(std::cin, answer)) return ToolResult::error("No interactive input available");
        return ToolResult::success(answer);
    }
    if (name == "brief") return execute_brief(input);
    // "computer" is the Anthropic native wire name (model sees it via the
    // computer_20241022 tool); "computer_use" is the internal registry name.
    if (name == "computer_use" || name == "computer")
        return execute_computer_use(input);
    if (name == "config") return execute_config_tool(input);
    if (name == "enter_plan_mode") {
        EnterPlanModeTool tool;
        auto title = json_string(json, "title").or_else([&] { return json_string(json, "goal"); }).value_or("Plan");
        auto summary = json_string(json, "summary").value_or("");
        auto result = tool.execute(title, summary);
        if (!result) return ToolResult::error(std::string(format_error(result.error())));
        return ToolResult::success(std::format("Plan mode enabled: {}", title));
    }
    if (name == "exit_plan_mode") {
        ExitPlanModeTool tool;
        auto result = tool.execute();
        if (!result) return ToolResult::error(std::string(format_error(result.error())));
        return ToolResult::success(std::format("Plan mode finalized: {} ({} sections)",
            result->title, result->total_sections()));
    }
    if (name == "enter_worktree") {
        auto branch = json_string(json, "branch").or_else([&] { return json_string(json, "branch_name"); });
        if (!branch || branch->empty()) return ToolResult::error("enter_worktree requires branch_name");
        auto target_path_text = json_string(json, "path").or_else([&] { return json_string(json, "target_path"); });
        EnterWorktreeTool tool;
        auto result = tool.execute(WorktreeCreateRequest{
            .branch_name = *branch,
            .target_path = target_path_text ? std::optional<fs::path>{fs::path{*target_path_text}} : std::nullopt,
            .base_branch = json_string(json, "base_branch"),
            .create_branch = json_bool(json, "create_branch", true),
        });
        if (!result) return ToolResult::error(std::string(format_error(result.error())));
        return ToolResult::success(std::format("Entered worktree {} at {}",
            result->branch_name, result->worktree_path.string()));
    }
    if (name == "exit_worktree") {
        ExitWorktreeTool tool;
        auto result = tool.execute(json_bool(json, "remove_worktree", false));
        if (!result) return ToolResult::error(std::string(format_error(result.error())));
        return ToolResult::success(std::format("Exited worktree {} and returned to {}",
            result->branch_name, result->original_path.string()));
    }
    if (name == "lsp") return execute_lsp_tool(input);
    if (name == "list_mcp_resources") {
        auto server = json_string(json, "server_name").or_else([&] { return json_string(json, "server"); });
        ListMcpResourcesTool tool;
        auto resources = tool.execute(server);
        if (!resources) return ToolResult::error(std::string(format_error(resources.error())));
        std::string out = "MCP resources:\n";
        for (const auto& resource : *resources) {
            out += std::format("- {} ({})\n", resource.uri, resource.mime_type);
        }
        if (resources->empty()) out += "No MCP resources are registered.\n";
        return ToolResult::success(out);
    }
    if (name == "read_mcp_resource") {
        auto server = json_string(json, "server_name").or_else([&] { return json_string(json, "server"); });
        auto uri = json_string(json, "resource_uri").or_else([&] { return json_string(json, "uri"); });
        if (!server || !uri) return ToolResult::error("read_mcp_resource requires server_name and resource_uri");
        ReadMcpResourceTool tool;
        auto result = tool.execute(*server, *uri);
        if (!result) return ToolResult::error(std::string(format_error(result.error())));
        return ToolResult::success(result->content);
    }
    if (name == "mcp") {
        auto server = json_string(json, "server_name").or_else([&] { return json_string(json, "server"); });
        auto tool = json_string(json, "tool_name").or_else([&] { return json_string(json, "tool"); });
        if (!server || !tool) return ToolResult::error("mcp requires server_name and tool_name");
        McpTool mcp_tool;
        auto arguments = json_raw_value(json, "arguments").or_else([&] { return json_raw_value(json, "input"); })
            .value_or("{}");
        auto result = mcp_tool.execute(McpToolRequest{
            .server_name = *server,
            .tool_name = *tool,
            .arguments = {},
            .arguments_json = arguments,
        });
        if (!result) return ToolResult::error(std::string(format_error(result.error())));
        // Preserves structured content items (screenshots, multi-text).
        return mcp_result_to_tool_result(*result);
    }
    if (name == "mcp_auth") {
        auto server = json_string(json, "server_name").or_else([&] { return json_string(json, "server"); });
        if (!server) return ToolResult::error("mcp_auth requires server_name");
        auto code = json_string(json, "auth_code").or_else([&] { return json_string(json, "code"); });
        auto wait_for_callback =
            json_bool(json, "wait_for_callback", false) ||
            json_bool(json, "waitForCallback", false);
        auto authorization_url_file = json_string(json, "authorization_url_file")
            .or_else([&] { return json_string(json, "authorizationUrlFile"); });
        McpAuthTool tool;
        auto result = tool.execute(*server, code, wait_for_callback, authorization_url_file);
        if (!result) return ToolResult::error(std::string(format_error(result.error())));
        return ToolResult::success(*result);
    }
    if (name == "notebook_edit") return execute_notebook_edit(input);
    if (name == "powershell") {
        auto command = json_string(json, "command");
        if (!command) return ToolResult::error("powershell requires command");
        auto cwd = json_string(json, "cwd").or_else([&] { return json_string(json, "working_directory"); });
        auto timeout = std::clamp(json_int(json, "timeout").value_or(120), 1, 300);
        PowerShellTool tool;
        auto validation = tool.validate(PowerShellConfig{
            .command = *command,
            .working_directory = cwd ? fs::path{*cwd} : fs::path{},
            .timeout = std::chrono::seconds(timeout),
        });
        if (!validation) return ToolResult::error(std::string(format_error(validation.error())));
#ifdef _WIN32
        return run_command(build_powershell_process_command(PowerShellConfig{
            .command = *command,
            .working_directory = cwd ? fs::path{*cwd} : fs::path{},
            .timeout = std::chrono::seconds(timeout),
        }) + " 2>&1");
#else
        return ToolResult::error("PowerShell execution is only available on Windows in this runtime");
#endif
    }
    if (name == "repl") return execute_script(input);
    if (name == "schedule_cron") {
        ScheduleCronTool tool;
        auto action_text = json_string(json, "action").value_or("create");
        CronAction action = CronAction::Create;
        if (action_text == "list") action = CronAction::List;
        else if (action_text == "get") action = CronAction::Get;
        else if (action_text == "pause") action = CronAction::Pause;
        else if (action_text == "resume") action = CronAction::Resume;
        else if (action_text == "delete") action = CronAction::Delete;
        else if (action_text == "trigger") action = CronAction::Trigger;
        auto result = tool.execute(CronRequest{
            .action = action,
            .task_id = json_string(json, "task_id").or_else([&] { return json_string(json, "id"); }),
            .name = json_string(json, "name").value_or("scheduled-task"),
            .message = json_string(json, "message").or_else([&] { return json_string(json, "command"); }),
            .cron_expression = json_string(json, "cron").or_else([&] { return json_string(json, "cron_expression"); }).value_or("* * * * *"),
            .timezone = json_string(json, "timezone").value_or("UTC"),
        });
        if (!result) return ToolResult::error(std::string(format_error(result.error())));
        return ToolResult::success(*result);
    }
    if (name == "script") return execute_script(input);
    if (name == "send_message") {
        return runtime_message_delivery::execute_send_message(
            json, registry, &native_agent_status_is_terminal);
    }
    if (name == "shared") {
        auto key = json_string(json, "key");
        static std::unordered_map<std::string, std::string> shared;
        if (auto value = json_string(json, "value")) {
            if (!key) return ToolResult::error("shared set requires key");
            shared[*key] = *value;
            return ToolResult::success(std::format("Stored shared value '{}'", *key));
        }
        if (!key) return ToolResult::success(std::format("Shared keys: {}", shared.size()));
        auto it = shared.find(*key);
        return it == shared.end() ? ToolResult::error(std::format("Shared key not found: {}", *key))
                                  : ToolResult::success(it->second);
    }
    if (name == "skill") {
        // New path: use execute_skill_tool_simple which validates the skill,
        // expands templates, cascades context modifiers, and returns a
        // structured JSON response. Falls back to the original loader on
        // error (so tools that pass "name" only still work).
        auto simple = cc::tools::skill::execute_skill_tool_simple(input.json());
        if (simple) return ToolResult::success(*simple);
        return execute_skill_tool(input);
    }
    if (name == "sleep") {
        SleepTool tool;
        auto seconds = std::clamp(json_int(json, "duration").or_else([&] { return json_int(json, "seconds"); }).value_or(1), 1, 300);
        auto result = tool.execute(SleepRequest{
            .duration = std::chrono::seconds(seconds),
            .reason = json_string(json, "reason").value_or("scheduled wait"),
            .resume_hint = json_string(json, "resume_hint"),
        });
        if (!result) return ToolResult::error(std::string(format_error(result.error())));
        return ToolResult::success(std::format("Slept for {} ms", result->actual_duration.count()));
    }
    if (name == "synthetic_output") {
        return ToolResult::success(json_string(json, "content").or_else([&] { return json_string(json, "text"); }).value_or(std::string(json)));
    }
    if (name.starts_with("task_")) return execute_task_tool(name, input);
    if (name == "team_create") return execute_team_create_runtime_tool(json, registry);
    if (name == "team_delete") return execute_team_delete_runtime_tool(json);
    if (name == "testing") {
        auto command = json_string(json, "command").value_or("ctest --output-on-failure");
        return run_command(command + " 2>&1");
    }
    if (name == "todo_write") {
        return ToolResult::success("todo_write is registered through the native TodoWrite adapter");
    }
    if (name == "tool_search") return execute_tool_search(input);
    if (name == "tungsten") {
        auto operation = json_string(json, "operation").value_or("");
        auto result = tungsten::validate_request(tungsten::TungstenRequest{.operation = operation, .inputs = {}});
        return result.ok ? ToolResult::success(result.message) : ToolResult::error(result.message);
    }
    if (name == "workflow") {
        auto file = json_string(json, "file").or_else([&] { return json_string(json, "path"); });
        if (!file) return ToolResult::error("workflow requires file or path");
        std::ifstream in(*file);
        if (!in) return ToolResult::error(std::format("Workflow file not found: {}", *file));
        std::stringstream buffer;
        buffer << in.rdbuf();
        auto definition = parse_workflow_definition_json(buffer.str());
        if (!definition) return ToolResult::error(std::string(format_error(definition.error())));
        WorkflowTool tool;
        auto result = tool.execute(std::move(*definition));
        if (!result) return ToolResult::error(std::string(format_error(result.error())));
        std::string output = std::format(
            "Workflow {} {}\nSteps executed: {}\nSteps skipped: {}\n",
            result->workflow_name,
            result->success ? "completed" : "failed",
            result->steps_executed,
            result->steps_skipped);
        for (const auto& step : result->step_results) {
            output += std::format(
                "- {}: {}",
                step.step_id,
                step.success ? "ok" : "failed");
            if (!step.output.empty()) output += " " + step.output;
            if (step.error_message) output += " " + *step.error_message;
            if (!output.ends_with('\n')) output += "\n";
        }
        return result->success ? ToolResult::success(output) : ToolResult::error(output);
    }
    if (name == "web_browser") return execute_web_browser(input);

    // ── Feature-gated stub tools ──────────────────────────────────────────────
    // These tools exist in TS (src/tools.ts L16-158) but are gated behind Bun
    // feature() flags.  In CPP we provide minimal stubs so that when a feature
    // flag is enabled, the tool is registered and returns a meaningful "not yet
    // implemented" message rather than crashing.  TS REF: src/tools.ts:195-256

    if (name == "push_notification") {
        // TS REF: src/tools.ts:45-49 (PushNotificationTool)
        auto title = json_string(json, "title").value_or("Notification");
        auto body = json_string(json, "body").value_or("");
        return ToolResult::error(std::format(
            "push_notification stub: title=\"{}\", body=\"{}\" — not yet implemented in CPP migration",
            title, body));
    }
    if (name == "monitor") {
        // TS REF: src/tools.ts:39-41 (MonitorTool)
        return ToolResult::error(
            "monitor stub: MonitorTool is not yet implemented in CPP migration");
    }
    if (name == "send_user_file") {
        // TS REF: src/tools.ts:42-44 (SendUserFileTool)
        auto file_path = json_string(json, "file_path").value_or("");
        return ToolResult::error(std::format(
            "send_user_file stub: file_path=\"{}\" — not yet implemented in CPP migration",
            file_path));
    }
    if (name == "subscribe_pr") {
        // TS REF: src/tools.ts:50-52 (SubscribePRTool)
        auto pr_url = json_string(json, "pr_url").value_or("");
        return ToolResult::error(std::format(
            "subscribe_pr stub: pr_url=\"{}\" — not yet implemented in CPP migration",
            pr_url));
    }
    if (name == "suggest_background_pr") {
        // TS REF: src/tools.ts:20-24 (SuggestBackgroundPRTool — USER_TYPE==='ant')
        return ToolResult::error(
            "suggest_background_pr stub: not yet implemented in CPP migration");
    }
    if (name == "overflow_test") {
        // TS REF: src/tools.ts:107-109 (OverflowTestTool)
        return ToolResult::error(
            "overflow_test stub: OverflowTestTool is not yet implemented in CPP migration");
    }
    if (name == "ctx_inspect") {
        // TS REF: src/tools.ts:110-112 (CtxInspectTool)
        return ToolResult::error(
            "ctx_inspect stub: CtxInspectTool is not yet implemented in CPP migration");
    }
    if (name == "terminal_capture") {
        // TS REF: src/tools.ts:113-116 (TerminalCaptureTool)
        return ToolResult::error(
            "terminal_capture stub: TerminalCaptureTool is not yet implemented in CPP migration");
    }
    if (name == "snip") {
        // TS REF: src/tools.ts:123-125 (SnipTool)
        return ToolResult::error(
            "snip stub: SnipTool (history snippet) is not yet implemented in CPP migration");
    }
    if (name == "list_peers") {
        // TS REF: src/tools.ts:126-128 (ListPeersTool)
        return ToolResult::error(
            "list_peers stub: ListPeersTool is not yet implemented in CPP migration");
    }
    if (name == "verify_plan_execution") {
        // TS REF: src/tools.ts:91-96 (VerifyPlanExecutionTool)
        return ToolResult::error(
            "verify_plan_execution stub: VerifyPlanExecutionTool is not yet implemented in CPP migration");
    }

    return ToolResult::error(std::format("Runtime tool '{}' has no runtime handler", name));
}

} // namespace cc::tools::detail
