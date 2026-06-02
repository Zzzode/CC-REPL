// C++23 module: Tool name constants and tool set definitions for agent access control.
// Defines which tools are disallowed/allowed for different agent modes.
module;
#include <string>
#include <string_view>
#include <unordered_set>

export module cc.constants.tools_constants;


export namespace cc::constants::tools_constants {

// Tool name constants (matching the TypeScript tool name exports)
inline constexpr std::string_view task_output_tool_name = "TaskOutput";
inline constexpr std::string_view exit_plan_mode_v2_tool_name = "ExitPlanMode";
inline constexpr std::string_view enter_plan_mode_tool_name = "EnterPlanMode";
inline constexpr std::string_view agent_tool_name = "Agent";
inline constexpr std::string_view ask_user_question_tool_name = "AskUserQuestion";
inline constexpr std::string_view task_stop_tool_name = "TaskStop";
inline constexpr std::string_view file_read_tool_name = "Read";
inline constexpr std::string_view web_search_tool_name = "WebSearch";
inline constexpr std::string_view todo_write_tool_name = "TodoWrite";
inline constexpr std::string_view grep_tool_name = "Grep";
inline constexpr std::string_view web_fetch_tool_name = "WebFetch";
inline constexpr std::string_view glob_tool_name = "Glob";
inline constexpr std::string_view file_edit_tool_name = "Edit";
inline constexpr std::string_view file_write_tool_name = "Write";
inline constexpr std::string_view notebook_edit_tool_name = "NotebookEdit";
inline constexpr std::string_view skill_tool_name = "Skill";
inline constexpr std::string_view send_message_tool_name = "SendMessage";
inline constexpr std::string_view task_create_tool_name = "TaskCreate";
inline constexpr std::string_view task_get_tool_name = "TaskGet";
inline constexpr std::string_view task_list_tool_name = "TaskList";
inline constexpr std::string_view task_update_tool_name = "TaskUpdate";
inline constexpr std::string_view tool_search_tool_name = "ToolSearch";
inline constexpr std::string_view synthetic_output_tool_name = "SyntheticOutput";
inline constexpr std::string_view enter_worktree_tool_name = "EnterWorktree";
inline constexpr std::string_view exit_worktree_tool_name = "ExitWorktree";
inline constexpr std::string_view workflow_tool_name = "Workflow";
inline constexpr std::string_view bash_tool_name = "Bash";
inline constexpr std::string_view cron_create_tool_name = "CronCreate";
inline constexpr std::string_view cron_delete_tool_name = "CronDelete";
inline constexpr std::string_view cron_list_tool_name = "CronList";

// Tools disallowed for ALL agents (prevents recursion and mode conflicts)
inline const std::unordered_set<std::string_view> all_agent_disallowed_tools = {
    task_output_tool_name,
    exit_plan_mode_v2_tool_name,
    enter_plan_mode_tool_name,
    ask_user_question_tool_name,
    task_stop_tool_name,
    // Note: AgentTool is conditionally disallowed based on user type in TS.
    // In C++ this should be configured at runtime.
};

// Custom agent disallowed tools (same as all_agent_disallowed_tools)
inline const std::unordered_set<std::string_view> custom_agent_disallowed_tools = {
    task_output_tool_name,
    exit_plan_mode_v2_tool_name,
    enter_plan_mode_tool_name,
    ask_user_question_tool_name,
    task_stop_tool_name,
};

// Tools allowed for async agents
inline const std::unordered_set<std::string_view> async_agent_allowed_tools = {
    file_read_tool_name,
    web_search_tool_name,
    todo_write_tool_name,
    grep_tool_name,
    web_fetch_tool_name,
    glob_tool_name,
    bash_tool_name,
    file_edit_tool_name,
    file_write_tool_name,
    notebook_edit_tool_name,
    skill_tool_name,
    synthetic_output_tool_name,
    tool_search_tool_name,
    enter_worktree_tool_name,
    exit_worktree_tool_name,
};

// Tools allowed only for in-process teammates (not general async agents)
inline const std::unordered_set<std::string_view> in_process_teammate_allowed_tools = {
    task_create_tool_name,
    task_get_tool_name,
    task_list_tool_name,
    task_update_tool_name,
    send_message_tool_name,
};

// Tools allowed in coordinator mode - only output and agent management
inline const std::unordered_set<std::string_view> coordinator_mode_allowed_tools = {
    agent_tool_name,
    task_stop_tool_name,
    send_message_tool_name,
    synthetic_output_tool_name,
};

} // namespace cc::constants::tools_constants
