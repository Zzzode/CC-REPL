// Tool name constants — centralized from all src/tools/*/constants.ts single-liners.
// These exist in TS to break circular dependencies between prompt.ts files.
// Covers: Bash, Config, EnterPlanMode, EnterWorktree, ExitPlanMode, ExitWorktree,
//         NotebookEdit, REPL, SendMessage, Skill, TaskCreate, TaskGet, TaskList,
//         TaskOutput, TaskStop, TaskUpdate, TeamCreate, TeamDelete, TodoWrite,
//         ToolSearch, Workflow, ScheduleCron.
module;
#include <string>
#include <string_view>

export module cc.tools.tool_name_constants;

export namespace cc::tools::names {

// --- Core tools ---
inline constexpr std::string_view BASH_TOOL_NAME = "Bash";
inline constexpr std::string_view TODO_WRITE_TOOL_NAME = "TodoWrite";

// --- Task management tools ---
inline constexpr std::string_view TASK_CREATE_TOOL_NAME = "TaskCreate";
inline constexpr std::string_view TASK_GET_TOOL_NAME = "TaskGet";
inline constexpr std::string_view TASK_LIST_TOOL_NAME = "TaskList";
inline constexpr std::string_view TASK_STOP_TOOL_NAME = "TaskStop";
inline constexpr std::string_view TASK_UPDATE_TOOL_NAME = "TaskUpdate";
inline constexpr std::string_view TASK_OUTPUT_TOOL_NAME = "TaskOutput";

// --- Team management tools ---
inline constexpr std::string_view TEAM_CREATE_TOOL_NAME = "TeamCreate";
inline constexpr std::string_view TEAM_DELETE_TOOL_NAME = "TeamDelete";
inline constexpr std::string_view SEND_MESSAGE_TOOL_NAME = "SendMessage";

// --- Plan mode tools ---
inline constexpr std::string_view ENTER_PLAN_MODE_TOOL_NAME = "EnterPlanMode";
inline constexpr std::string_view EXIT_PLAN_MODE_V2_TOOL_NAME = "ExitPlanModeV2";

// --- Worktree tools ---
inline constexpr std::string_view ENTER_WORKTREE_TOOL_NAME = "EnterWorktree";
inline constexpr std::string_view EXIT_WORKTREE_TOOL_NAME = "ExitWorktree";

// --- Other tools ---
inline constexpr std::string_view CONFIG_TOOL_NAME = "Config";
inline constexpr std::string_view SKILL_TOOL_NAME = "Skill";
inline constexpr std::string_view TOOL_SEARCH_TOOL_NAME = "ToolSearch";
inline constexpr std::string_view NOTEBOOK_EDIT_TOOL_NAME = "NotebookEdit";
inline constexpr std::string_view REPL_TOOL_NAME = "REPL";
inline constexpr std::string_view WORKFLOW_TOOL_NAME = "Workflow";
inline constexpr std::string_view SCHEDULE_CRON_TOOL_NAME = "ScheduleCron";
inline constexpr std::string_view CRON_CREATE_TOOL_NAME = "CronCreate";
inline constexpr std::string_view CRON_DELETE_TOOL_NAME = "CronDelete";
inline constexpr std::string_view CRON_LIST_TOOL_NAME = "CronList";

} // namespace cc::tools::names
