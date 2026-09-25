// Implementation unit for cc.tools.runtime_registry — tool factory helpers
// (define_tool / make_runtime_tool), the RuntimeFunctionTool key function,
// register_runtime_tools (both overloads), and the MCP tool-definition
// collectors.
module;

module cc.tools.runtime_registry;

import std;

import cc.tools.tool;
import cc.tools.agent;
import cc.tools.bash;
import cc.tools.built_in_agents;
import cc.tools.feature_flags;
import cc.tools.file_edit;
import cc.tools.file_read;
import cc.tools.file_write;
import cc.tools.glob;
// make_grep_tool() is called below; the arch checker's trailing-return-type
// extraction does not see it exported (pre-existing false negative).
import cc.tools.grep;  // arch-check: keep-import
import cc.tools.mcp;
import cc.tools.todo_write;
import cc.tools.web_fetch;
import cc.tools.web_search;

namespace cc::tools::detail {

using cc::core::InputSchema;
using cc::core::ITool;
using cc::core::SchemaProperty;
using cc::core::ToolDefinition;
using cc::core::ToolInput;
using cc::core::ToolPermission;

[[nodiscard]] ToolDefinition define_tool(
    std::string name,
    std::string description,
    ToolPermission permission,
    std::vector<SchemaProperty> properties,
    std::string category
) {
    return ToolDefinition{
        .name = std::move(name),
        .description = std::move(description),
        .input_schema = InputSchema{.properties = std::move(properties)},
        .permission = permission,
        .is_hidden = false,
        .category = std::move(category),
    };
}

[[nodiscard]] std::unique_ptr<ITool> make_runtime_tool(
    std::string name,
    std::string description,
    ToolPermission permission,
    std::vector<SchemaProperty> properties,
    RuntimeExecutor executor,
    std::string category,
    cc::tools::agent::AgentLivePermissionCheckFn permission_check
) {
    return std::make_unique<RuntimeFunctionTool>(
        define_tool(std::move(name), std::move(description), permission, std::move(properties), std::move(category)),
        std::move(executor),
        std::move(permission_check)
    );
}

[[nodiscard]] bool RuntimeFunctionTool::check_permission(const ToolInput& input) const {
    // If a live permission checker is wired in, defer to it.
    if (permission_check_) {
        return permission_check_(definition_.name, input.json(), "").allowed;
    }
    // No live checker available: fail CLOSED for anything that mutates
    // state or touches the network. Read-only runtime tools remain safe
    // to allow. (Previously this branch unconditionally returned true,
    // letting write/execute/network runtime tools bypass permission when
    // no handler was supplied — a security bypass.)
    return definition_.permission == ToolPermission::ReadOnly;
}

} // namespace cc::tools::detail

namespace cc::tools {

using cc::core::SchemaProperty;
using cc::core::ToolPermission;

// ---------------------------------------------------------------------------
// Built-in agent registry access.
//
// The canonical source of built-in agent definitions lives in
// cc.tools.built_in_agents (migrated from TS builtInAgents.ts + built-in/*).
// agent_runtime::built_in_agent_definitions() mirrors these definitions for
// use inside the agent_runtime module (avoiding a circular module import).
//
// External consumers should use the accessors below, which forward to
// cc::tools::built_in_agents::get_built_in_agents().
// ---------------------------------------------------------------------------

[[nodiscard]] std::vector<agent_runtime::AgentDefinition>
get_built_in_agent_definitions() {
    return cc::tools::built_in_agents::get_built_in_agents();
}

[[nodiscard]] bool are_explore_plan_agents_enabled() {
    return cc::tools::built_in_agents::are_explore_plan_agents_enabled();
}

void register_runtime_tools(cc::core::ToolRegistry& registry, RuntimeToolOptions options) {
    namespace features = cc::tools::features;

    auto permission_check = std::move(options.permission_check);
    AgentConfig agent_config;
    agent_config.parent_permission_mode = std::move(options.parent_permission_mode);
    registry.register_tool(make_agent_tool(
        std::move(agent_config),
        0,
        &registry,
        permission_check,
        options.permission_hook_valid_for_background));

    // TS REF: src/tools.ts:199 (isBashToolDisabled runtime check)
    if constexpr (features::kBashToolEnabled) {
        registry.register_tool(make_bash_tool());
    }
    // Wire Edit + Read tools to share ReadFileState so that a successful Read
    // through the registry satisfies Edit's "file must be read first" check.
    auto shared_read_state = std::make_shared<cc::tools::file_edit::ReadFileState>();
    {
        struct EditAdapter final : cc::core::ITool {
            cc::tools::file_edit::FileEditTool tool_;
            cc::core::ToolDefinition def_ = cc::tools::file_edit::FileEditTool::definition();
            std::shared_ptr<cc::tools::file_edit::ReadFileState> shared_state_;

            explicit EditAdapter(std::shared_ptr<cc::tools::file_edit::ReadFileState> s)
                : shared_state_(std::move(s)) {}

            const cc::core::ToolDefinition& definition() const override { return def_; }
            std::expected<cc::core::ToolResult, cc::core::Error> execute(
                const cc::core::ToolInput& input) override
            {
                // Sync shared state into tool before execution
                tool_.read_file_state() = *shared_state_;
                // Auto-read: if the file hasn't been read yet, implicitly read it
                // so agents can Edit without a prior explicit Read (matches TS behavior).
                auto parsed_edit = cc::tools::file_edit::ParsedInput::from_json(input.json());
                if (parsed_edit) {
                    auto abs_path = fs::absolute(parsed_edit->file_path);
                    auto existing = tool_.read_file_state().get(abs_path);
                    if (!existing || existing->is_partial_view) {
                        std::error_code ec;
                        if (fs::exists(abs_path, ec)) {
                            tool_.read_file_state().set(abs_path, cc::tools::file_edit::ReadTimestamp{
                                .timestamp = std::chrono::system_clock::now(),
                                .offset = std::nullopt,
                                .limit = std::nullopt,
                                .content = std::nullopt,
                                .is_partial_view = false,
                            });
                        }
                    }
                }
                auto result = tool_.execute(input);
                // Sync back (edit updates read state after success)
                *shared_state_ = tool_.read_file_state();
                if (result) return std::move(*result);
                return std::unexpected(cc::core::Error::make(
                    cc::core::ErrorCode::ToolExecutionFailed,
                    result.error().format()));
            }
            bool check_permission(const cc::core::ToolInput& input) const override {
                return tool_.check_permission(input);
            }
        };
        registry.register_tool(std::make_unique<EditAdapter>(shared_read_state));
    }
    {
        struct ReadAdapter final : cc::core::ITool {
            cc::tools::file_read::FileReadTool tool_;
            cc::core::ToolDefinition def_ = cc::tools::file_read::FileReadTool::definition();
            std::shared_ptr<cc::tools::file_edit::ReadFileState> shared_state_;

            explicit ReadAdapter(std::shared_ptr<cc::tools::file_edit::ReadFileState> s)
                : shared_state_(std::move(s)) {}

            const cc::core::ToolDefinition& definition() const override { return def_; }
            std::expected<cc::core::ToolResult, cc::core::Error> execute(
                const cc::core::ToolInput& input) override
            {
                auto result = tool_.execute(input);
                if (result) {
                    // On success, record the read in shared state so Edit sees it
                    auto parsed = cc::tools::file_read::FileReadInput::from_json(input.json());
                    if (parsed) {
                        auto abs_path = fs::absolute(parsed->file_path);
                        shared_state_->set(abs_path, cc::tools::file_edit::ReadTimestamp{
                            .timestamp = std::chrono::system_clock::now(),
                            .offset = parsed->offset,
                            .limit = parsed->limit,
                            .content = std::nullopt,
                            .is_partial_view = parsed->offset.has_value() || parsed->limit.has_value(),
                        });
                    }
                    return std::move(*result);
                }
                return std::unexpected(cc::core::Error::make(
                    cc::core::ErrorCode::ToolExecutionFailed, result.error().format()));
            }
            bool check_permission(const cc::core::ToolInput& input) const override {
                return tool_.check_permission(input);
            }
        };
        registry.register_tool(std::make_unique<ReadAdapter>(shared_read_state));
    }
    registry.register_tool(make_file_write_tool());
    // TS REF: src/tools.ts:203 (hasEmbeddedSearchTools — ant-native bfs/ugrep
    // suppresses dedicated Glob/Grep tools when embedded search is available)
    if constexpr (!features::kEmbeddedSearchTools) {
        registry.register_tool(make_glob_tool());
        registry.register_tool(make_grep_tool());
    }
    registry.register_tool(make_todo_write_tool());
    registry.register_tool(make_web_fetch_tool());
    registry.register_tool(make_web_search_tool());

    const auto simple = [&registry, permission_check](std::string name, std::string description, ToolPermission permission,
                                                      std::vector<SchemaProperty> properties = {}, std::string category = "runtime") {
        auto name_copy = name;
        return detail::make_runtime_tool(std::move(name), std::move(description), permission, std::move(properties),
            [name_copy, &registry](const cc::core::ToolInput& input) {
                return detail::execute_simple_runtime_tool(name_copy, input, &registry);
            },
            std::move(category),
            permission_check);
    };
    const auto prop = [](std::string name, std::string type, std::string description, bool required) {
        return SchemaProperty{
            .name = std::move(name),
            .type = std::move(type),
            .description = std::move(description),
            .required = required,
            .default_value = std::nullopt,
            .enum_values = std::nullopt,
        };
    };

    registry.register_tool(simple("ask_user_question", "Ask the interactive user a question and return the answer",
        ToolPermission::ReadOnly, {prop("question", "string", "Question to ask", true)}, "interaction"));
    registry.register_tool(simple("computer_use", "Control the local computer through screenshots, mouse, keyboard, and scroll actions",
        ToolPermission::Execute, {
            prop("action", "string", "screenshot, move, click, double_click, right_click, drag, type, press, hotkey, or scroll", true),
            prop("x", "number", "X coordinate, region origin, or scroll delta x", false),
            prop("y", "number", "Y coordinate, region origin, or scroll delta y", false),
            prop("to_x", "number", "Drag target X coordinate", false),
            prop("to_y", "number", "Drag target Y coordinate", false),
            prop("width", "number", "Screenshot region width", false),
            prop("height", "number", "Screenshot region height", false),
            prop("text", "string", "Text to type or key to press", false),
            prop("key", "string", "Key name for press actions", false),
        }, "computer_use"));
    registry.register_tool(simple("brief", "Read or write the workspace brief",
        ToolPermission::Write, {prop("content", "string", "Brief content to save", false)}, "context"));
    registry.register_tool(simple("config", "Read or update LOOM configuration",
        ToolPermission::Write, {prop("action", "string", "get or set", false)}, "config"));
    registry.register_tool(simple("enter_plan_mode", "Enter plan mode",
        ToolPermission::Write, {}, "planning"));
    registry.register_tool(simple("exit_plan_mode", "Exit plan mode",
        ToolPermission::Write, {}, "planning"));
    // TS REF: src/tools.ts:227 (isWorktreeModeEnabled — Enter/ExitWorktree)
    if constexpr (features::kWorktreeMode) {
        registry.register_tool(simple("enter_worktree", "Create and enter a git worktree",
            ToolPermission::Execute, {prop("branch", "string", "Branch name", true)}, "git"));
        registry.register_tool(simple("exit_worktree", "Remove a git worktree",
            ToolPermission::Execute, {prop("path", "string", "Worktree path", false)}, "git"));
    }
    // TS REF: src/tools.ts:226 (ENABLE_LSP_TOOL env var)
    if constexpr (features::kEnableLspTool) {
        registry.register_tool(simple("lsp", "Fallback language intelligence for definitions, references, symbols, hover, and diagnostics",
            ToolPermission::ReadOnly, {prop("file_path", "string", "File path", true)}, "code"));
    }
    registry.register_tool(simple("mcp", "Invoke a tool exposed by an MCP server. Specify the server name (e.g. 'zai-builtin', 'computer-use') and the tool name to call on that server.",
        ToolPermission::Network, {
            prop("server_name", "string", "Name of the MCP server to invoke (e.g. 'zai-builtin')", true),
            prop("tool_name", "string", "Name of the tool on the MCP server (e.g. 'analyze_image')", true),
            prop("arguments", "object", "Arguments to pass to the MCP tool", false),
        }, "mcp"));
    registry.register_tool(simple("list_mcp_resources", "List local MCP-style resources",
        ToolPermission::ReadOnly, {}, "mcp"));
    registry.register_tool(simple("read_mcp_resource", "Read a local MCP-style resource",
        ToolPermission::ReadOnly, {prop("uri", "string", "Resource URI", true)}, "mcp"));
    registry.register_tool(simple("mcp_auth", "Check MCP authentication token availability",
        ToolPermission::ReadOnly, {prop("server_name", "string", "MCP server name", true)}, "mcp"));
    registry.register_tool(simple("notebook_edit", "Edit Jupyter notebook cells by id or index",
        ToolPermission::Write, {
            prop("notebook_path", "string", "Notebook path", true),
            prop("cell_id", "string", "Notebook cell id or cell-N index", false),
            prop("cell_index", "integer", "Notebook cell index", false),
            prop("new_source", "string", "Replacement or inserted source", false),
            prop("cell_type", "string", "code, markdown, or raw", false),
            prop("edit_mode", "string", "replace, insert, or delete", false),
        }, "filesystem"));
    // TS REF: src/tools.ts:152-157, 244 (getPowerShellTool — runtime check)
    if constexpr (features::kPowerShellTool) {
        registry.register_tool(simple("powershell", "Execute a PowerShell command on Windows",
            ToolPermission::Execute, {
                SchemaProperty{
                    .name = "command",
                    .type = "string",
                    .description = "Command",
                    .required = true,
                    .default_value = std::nullopt,
                    .enum_values = std::nullopt,
                },
                SchemaProperty{
                    .name = "cwd",
                    .type = "string",
                    .description = "Working directory",
                    .required = false,
                    .default_value = std::nullopt,
                    .enum_values = std::nullopt,
                },
                SchemaProperty{
                    .name = "timeout",
                    .type = "integer",
                    .description = "Timeout in seconds",
                    .required = false,
                    .default_value = std::nullopt,
                    .enum_values = std::nullopt,
                },
            }, "shell"));
    }
    // TS REF: src/tools.ts:16-19, 234 (REPLTool — USER_TYPE==='ant')
    // In CPP, "repl" delegates to execute_script() which has a working
    // implementation, so it is registered unconditionally (not ant-only).
    registry.register_tool(simple("repl", "Run a one-shot REPL snippet",
        ToolPermission::Execute, {prop("code", "string", "Code to execute", true)}, "execution"));
    // TS REF: src/tools.ts:29-34, 237 (Cron tools — AGENT_TRIGGERS)
    // In CPP, schedule_cron has a working implementation (cc.tools.cron),
    // so it is registered unconditionally.
    registry.register_tool(simple("schedule_cron", "Schedule a cron-style reminder for this process",
        ToolPermission::Write, {prop("message", "string", "Scheduled message", true)}, "tasks"));
    // TS REF: src/tools.ts:252-254 (ScriptTool — isScriptToolEnabled)
    if constexpr (features::kScriptToolEnabled) {
        registry.register_tool(simple("script", "Execute a bounded script",
            ToolPermission::Execute, {prop("code", "string", "Script code", true)}, "execution"));
    }
    registry.register_tool(simple("send_message", "Queue a message for an agent or team",
        ToolPermission::Write, {
            prop("to", "string", "Recipient teammate, '*' broadcast, or compatible target", false),
            prop("message", "object", "Plain text or structured SendMessage payload", false),
            prop("summary", "string", "Preview summary for plain text messages", false),
            prop("target_agent", "string", "Compatibility recipient field", false),
            prop("content", "string", "Compatibility message body field", false),
        }, "agents"));
    registry.register_tool(simple("shared", "Read or write shared runtime key-value state",
        ToolPermission::Write, {prop("key", "string", "Shared key", false)}, "agents"));
    registry.register_tool(simple("skill",
        "Execute a skill with validation, template expansion, and context modifier cascading",
        ToolPermission::ReadOnly, {
            prop("action", "string",
                 "Skill action: install, update, list, search, or execute (default)", false),
            prop("skill_path", "string",
                 "Path or qualified name of the installed skill", false),
            prop("arguments", "object", "Named arguments passed to the skill", false),
            prop("context_modifiers", "object",
                 "Invocation overrides for model, effort, max_tokens, temperature, etc.", false),
            prop("model", "string", "Override the LLM model used by the skill", false),
            prop("effort", "integer",
                 "Effort level 1 (fast) .. 5 (deep); overrides frontmatter", false),
            prop("allowed_tools", "array",
                 "List of tool names the skill is permitted to call", false),
            prop("budget_token_limit", "integer",
                 "Maximum tokens allowed for skill execution", false),
            prop("structured_output", "object",
                 "JSON schema constraining the final LLM response", false),
            prop("should_use_sandbox", "boolean",
                 "True if the skill must run inside the sandboxed runtime", false),
            prop("use_fork_model", "boolean",
                 "True if the skill runs in a forked isolated session", false),
            prop("fork_model", "boolean", "Alias for use_fork_model", false),
            prop("session_id", "string", "Session identifier for ${LOOM_SESSION_ID}", false),
            prop("name", "string", "Alias for skill_path", false),
        }, "skills"));
    // TS REF: src/tools.ts:25-28, 236 (SleepTool — PROACTIVE || KAIROS)
    if constexpr (features::kEnableSleepTool) {
        registry.register_tool(simple("sleep", "Sleep for a bounded number of seconds",
            ToolPermission::Execute, {prop("duration", "number", "Duration in seconds", true)}, "execution"));
    }
    registry.register_tool(simple("synthetic_output", "Return provided synthetic output content",
        ToolPermission::ReadOnly, {prop("content", "string", "Content", true)}, "testing"));

    // TS REF: src/tools.ts:220-222 (isTodoV2Enabled — TaskCreate/Get/Update/List)
    if constexpr (features::kTodoV2) {
        for (const auto& name : {"task_create", "task_get", "task_list", "task_output", "task_stop", "task_update"}) {
            registry.register_tool(simple(name, std::format("Runtime task operation {}", name), ToolPermission::Write,
                {
                    prop("task_id", "string", "Task ID", false),
                    prop("pid", "number", "Background process PID", false),
                }, "tasks"));
        }
    }
    // TS REF: src/tools.ts:230-232 (isAgentSwarmsEnabled — TeamCreate/Delete)
    if constexpr (features::kAgentSwarmsEnabled) {
        registry.register_tool(simple("team_create", "Create a runtime team record", ToolPermission::Write,
            {prop("team_name", "string", "Team name", false)}, "agents"));
        registry.register_tool(simple("team_delete", "Delete a runtime team record", ToolPermission::Write,
            {prop("team_name", "string", "Team name", true)}, "agents"));
    }
    // TS REF: src/tools.ts:246 (NODE_ENV==='test' — TestingPermissionTool)
    if constexpr (features::kTestingPermissionTool) {
        registry.register_tool(simple("testing", "Run a test command", ToolPermission::Execute,
            {prop("command", "string", "Test command", false)}, "testing"));
    }
    // TS REF: src/tools.ts:251 (isToolSearchEnabledOptimistic — ToolSearchTool)
    if constexpr (features::kToolSearch) {
        registry.register_tool(simple("tool_search", "Search registered runtime tools", ToolPermission::ReadOnly,
            {prop("query", "string", "Search query", false)}, "tools"));
    }
    // TS REF: src/tools.ts:217 (TungstenTool — USER_TYPE==='ant')
    if constexpr (features::kUserTypeAnt) {
        registry.register_tool(simple("tungsten", "Use the Tungsten integration when configured", ToolPermission::Network, {}, "integrations"));
    }
    // TS REF: src/tools.ts:117-119, 219 (WebBrowserTool — WEB_BROWSER_TOOL)
    if constexpr (features::kWebBrowserTool) {
        registry.register_tool(simple("web_browser", "Automate browser navigation, extraction, form fill, and screenshots",
            ToolPermission::Network, {prop("action", "string", "Browser action", true)}, "browser"));
    }
    // TS REF: src/tools.ts:129-134, 235 (WorkflowTool — WORKFLOW_SCRIPTS)
    if constexpr (features::kWorkflowScripts) {
        registry.register_tool(simple("workflow", "Read and execute workflow definitions", ToolPermission::ReadOnly,
            {prop("file", "string", "Workflow file", true)}, "workflow"));
    }

    // ── Feature-gated stub tools (registered only when their flag is on) ────
    // TS REF: src/tools.ts:45-49, 242 (PushNotificationTool — KAIROS || KAIROS_PUSH_NOTIFICATION)
    if constexpr (features::kPushNotificationTool) {
        registry.register_tool(simple("push_notification", "Send a push notification to the user",
            ToolPermission::Write, {
                prop("title", "string", "Notification title", true),
                prop("body", "string", "Notification body", false),
            }, "notifications"));
    }
    // TS REF: src/tools.ts:39-41, 239 (MonitorTool — MONITOR_TOOL)
    if constexpr (features::kMonitorTool) {
        registry.register_tool(simple("monitor", "Monitor a background process or file for changes",
            ToolPermission::ReadOnly, {
                prop("command", "string", "Command or pattern to monitor", true),
                prop("pattern", "string", "Regex pattern to watch for", false),
            }, "monitoring"));
    }
    // TS REF: src/tools.ts:42-44, 241 (SendUserFileTool — KAIROS)
    if constexpr (features::kSendUserFileTool) {
        registry.register_tool(simple("send_user_file", "Send a file to the user",
            ToolPermission::Write, {prop("file_path", "string", "Path to the file", true)}, "delivery"));
    }
    // TS REF: src/tools.ts:50-52, 243 (SubscribePRTool — KAIROS_GITHUB_WEBHOOKS)
    if constexpr (features::kSubscribePRTool) {
        registry.register_tool(simple("subscribe_pr", "Subscribe to a GitHub PR for updates",
            ToolPermission::Network, {prop("pr_url", "string", "PR URL to subscribe to", true)}, "github"));
    }
    // TS REF: src/tools.ts:20-24, 218 (SuggestBackgroundPRTool — USER_TYPE==='ant')
    if constexpr (features::kUserTypeAnt) {
        registry.register_tool(simple("suggest_background_pr", "Suggest creating a background PR for the current changes",
            ToolPermission::ReadOnly, {}, "ant-internal"));
    }
    // TS REF: src/tools.ts:107-109, 223 (OverflowTestTool)
    if constexpr (features::kOverflowTestTool) {
        registry.register_tool(simple("overflow_test", "Test tool for context overflow scenarios",
            ToolPermission::ReadOnly, {prop("size", "number", "Size in tokens", false)}, "testing"));
    }
    // TS REF: src/tools.ts:110-112, 224 (CtxInspectTool — CONTEXT_COLLAPSE)
    if constexpr (features::kContextCollapse) {
        registry.register_tool(simple("ctx_inspect", "Inspect the current context window usage",
            ToolPermission::ReadOnly, {}, "context"));
    }
    // TS REF: src/tools.ts:113-116, 225 (TerminalCaptureTool — TERMINAL_PANEL)
    if constexpr (features::kTerminalPanel) {
        registry.register_tool(simple("terminal_capture", "Capture the current terminal screen content",
            ToolPermission::ReadOnly, {}, "terminal"));
    }
    // TS REF: src/tools.ts:123-125, 245 (SnipTool — HISTORY_SNIP)
    if constexpr (features::kHistorySnip) {
        registry.register_tool(simple("snip", "Create a snippet from conversation history",
            ToolPermission::Write, {prop("query", "string", "Snippet query", false)}, "history"));
    }
    // TS REF: src/tools.ts:126-128, 229 (ListPeersTool — UDS_INBOX)
    if constexpr (features::kUdsInbox) {
        registry.register_tool(simple("list_peers", "List connected peer sessions",
            ToolPermission::ReadOnly, {}, "peers"));
    }
    // TS REF: src/tools.ts:91-96, 233 (VerifyPlanExecutionTool — LOOM_VERIFY_PLAN)
    if constexpr (features::kVerifyPlanExecution) {
        registry.register_tool(simple("verify_plan_execution", "Verify that a plan execution matches expectations",
            ToolPermission::ReadOnly, {prop("plan", "string", "Plan to verify", true)}, "planning"));
    }

    // Touch built-in agent registry so lazy feature-flag evaluation is
    // performed once per process startup. Produces no side effects but keeps
    // the registry "warm" for agent spawning code paths.
    (void)built_in_agents::are_explore_plan_agents_enabled();
}

void register_runtime_tools(cc::core::ToolRegistry& registry) {
    register_runtime_tools(registry, RuntimeToolOptions{});
}

// ── MCP tool pool for config.tools ────────────────────────────────────────
// TS PARITY: assembleToolPool() merges built-in tools with per-server MCP
// tools so the model can call them directly by name.  In CPP, individual MCP
// tools are NOT registered in ToolRegistry (only the generic "mcp" wrapper
// is).  This helper collects tool definitions from all known MCP servers so
// they can be appended to config.tools and sent to the API.
//
// Each MCP tool gets a generic "object" input schema (the model infers
// parameters from the description).  Execution is routed via
// ToolRegistry::set_missing_tool_handler() → NativeMcpRuntime::call_tool().
[[nodiscard]] std::vector<cc::core::ToolDefinition> collect_mcp_tool_definitions() {
    std::vector<cc::core::ToolDefinition> defs;
    auto& runtime = NativeMcpRuntime::instance();
    for (const auto& server : runtime.all_statuses()) {
        for (const auto& tool : server.tools) {
            // Skip tools that might collide with built-in names.
            if (tool.name.empty()) continue;
            cc::core::ToolDefinition def;
            def.name = tool.name;
            def.description = tool.description;
            // The simplified property model cannot represent nested MCP
            // input schemas. The verbatim schema is carried by
            // NativeMcpRuntime and surfaced at request serialization time
            // (see QueryEngine's tool serializer); leave the simplified
            // schema empty here.
            def.input_schema = cc::core::InputSchema{};
            def.permission = cc::core::ToolPermission::Network;
            def.is_hidden = false;
            def.category = std::format("mcp:{}", server.name);
            defs.push_back(std::move(def));
        }
    }
    return defs;
}

/// Snapshot connected MCP tools' verbatim input schemas keyed by tool name.
/// Fed to QueryEngineConfig::mcp_input_schema_provider so the request
/// serializer emits the servers' real (possibly nested) JSON schemas rather
/// than the empty simplified schema stored on the tool defs.
[[nodiscard]] std::unordered_map<std::string, std::string>
collect_mcp_input_schemas() {
    std::unordered_map<std::string, std::string> schemas;
    for (const auto& server : NativeMcpRuntime::instance().all_statuses()) {
        for (const auto& tool : server.tools) {
            if (!tool.input_schema_json.empty()) {
                schemas.emplace(tool.name, tool.input_schema_json);
            }
        }
    }
    return schemas;
}

} // namespace cc::tools
