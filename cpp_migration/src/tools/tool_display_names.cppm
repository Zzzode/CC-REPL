/// Tool identifier → human-readable display name registry.
///
/// Mirrors the small toolName.ts module (which historically only
/// exported `BASH_TOOL_NAME = 'Bash'`) but expands it to cover every
/// tool known to the C++ side, so UI components (permission prompts,
/// tool-use headers, collapse labels, …) can render friendly names
/// without duplicating the mapping in each caller.
module;
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <algorithm>

export module cc.tools.tool_display_names;

export namespace cc::tools {

/// Return a human-readable label for `tool_id`, falling back to the id
/// itself when no mapping is registered. The returned string_view is
/// backed by static storage and is safe to hold indefinitely.
[[nodiscard]] constexpr std::string_view
display_tool_name(std::string_view tool_id) noexcept
{
    // Table is kept sorted so a future linear scan is easy to binary-search.
    // Each pair is { internal_tool_id, display_label }.
    constexpr std::pair<std::string_view, std::string_view> mapping[] = {
        // --- core file operations -------------------------------------
        {"Bash",               "Run bash command"},
        {"Glob",               "Find files by glob pattern"},
        {"Grep",               "Search file contents"},
        {"Read",               "Read file"},
        {"Write",              "Write file"},
        {"Edit",               "Edit file"},
        {"NotebookEdit",       "Edit Jupyter notebook"},
        {"ToolSearch",         "Search available tools"},

        // --- web ------------------------------------------------------
        {"WebFetch",           "Fetch web page"},
        {"WebSearch",          "Search the web"},
        {"WebBrowser",         "Browse web page"},

        // --- agent swarms ---------------------------------------------
        {"Agent",              "Spawn sub-agent"},
        {"Skill",              "Execute skill workflow"},
        {"TeamCreate",         "Create agent team"},
        {"TeamDelete",         "Delete agent team"},
        {"SendMessage",        "Send message to agent"},

        // --- task tracking --------------------------------------------
        {"TaskCreate",         "Create task"},
        {"TaskGet",            "Get task details"},
        {"TaskUpdate",         "Update task"},
        {"TaskList",           "List tasks"},
        {"TaskStop",           "Stop background task"},
        {"TaskOutput",         "Retrieve task output"},

        // --- sandboxed scripting --------------------------------------
        {"Script",             "Execute sandboxed script"},
        {"REPL",               "Interactive REPL"},

        // --- planning / concurrency -----------------------------------
        {"EnterPlanMode",      "Enter plan mode"},
        {"ExitPlanMode",       "Exit plan mode"},
        {"Sleep",              "Sleep / defer"},
        {"CronCreate",         "Create cron trigger"},
        {"CronDelete",         "Delete cron trigger"},
        {"CronList",           "List cron triggers"},
        {"RemoteTrigger",      "Remote agent trigger"},
        {"Monitor",            "Monitor resource"},

        // --- user interaction -----------------------------------------
        {"AskUserQuestion",    "Ask user a question"},
        {"Config",             "View or edit configuration"},
        {"Brief",              "Compress conversation context"},
        {"TodoWrite",          "Write todo items"},

        // --- LSP / MCP ------------------------------------------------
        {"LSP",                "Language server operations"},
        {"ListMcpResources",   "List MCP resources"},
        {"ReadMcpResource",    "Read MCP resource"},
        {"McpAuth",            "MCP server authentication"},

        // --- git / workspace ------------------------------------------
        {"EnterWorktree",      "Enter git worktree"},
        {"ExitWorktree",       "Exit git worktree"},
        {"SuggestBackgroundPR","Suggest background PR"},
        {"VerifyPlanExecution","Verify plan execution"},
        {"OverflowTest",       "Overflow test harness"},
        {"CtxInspect",         "Inspect collapsed context"},
        {"TerminalCapture",    "Capture terminal panel"},
        {"PushNotification",   "Push notification"},
        {"SubscribePR",        "Subscribe to PR updates"},
        {"SendUserFile",       "Send file to user"},
        {"StructuredOutput",   "Structured / synthetic output"},
        {"Workflow",           "Workflow orchestration"},

        // --- testing & internal ---------------------------------------
        {"TungstenTool",       "Tungsten operations"},
        {"TestingPermission",  "Testing permission harness"},
    };

    for (const auto& [id, label] : mapping) {
        if (id == tool_id) return label;
    }
    return tool_id;   // fall back: show the raw id
}

/// Return the full (tool_id, display_label) table. Useful for UI
/// selectors, help lists, tests that want to enumerate every known
/// tool, etc.
[[nodiscard]] inline auto all_tool_display_names()
    -> std::vector<std::pair<std::string_view, std::string_view>>
{
    constexpr std::pair<std::string_view, std::string_view> mapping[] = {
        {"Bash",               "Run bash command"},
        {"Glob",               "Find files by glob pattern"},
        {"Grep",               "Search file contents"},
        {"Read",               "Read file"},
        {"Write",              "Write file"},
        {"Edit",               "Edit file"},
        {"NotebookEdit",       "Edit Jupyter notebook"},
        {"ToolSearch",         "Search available tools"},
        {"WebFetch",           "Fetch web page"},
        {"WebSearch",          "Search the web"},
        {"WebBrowser",         "Browse web page"},
        {"Agent",              "Spawn sub-agent"},
        {"Skill",              "Execute skill workflow"},
        {"TeamCreate",         "Create agent team"},
        {"TeamDelete",         "Delete agent team"},
        {"SendMessage",        "Send message to agent"},
        {"TaskCreate",         "Create task"},
        {"TaskGet",            "Get task details"},
        {"TaskUpdate",         "Update task"},
        {"TaskList",           "List tasks"},
        {"TaskStop",           "Stop background task"},
        {"TaskOutput",         "Retrieve task output"},
        {"Script",             "Execute sandboxed script"},
        {"REPL",               "Interactive REPL"},
        {"EnterPlanMode",      "Enter plan mode"},
        {"ExitPlanMode",       "Exit plan mode"},
        {"Sleep",              "Sleep / defer"},
        {"CronCreate",         "Create cron trigger"},
        {"CronDelete",         "Delete cron trigger"},
        {"CronList",           "List cron triggers"},
        {"RemoteTrigger",      "Remote agent trigger"},
        {"Monitor",            "Monitor resource"},
        {"AskUserQuestion",    "Ask user a question"},
        {"Config",             "View or edit configuration"},
        {"Brief",              "Compress conversation context"},
        {"TodoWrite",          "Write todo items"},
        {"LSP",                "Language server operations"},
        {"ListMcpResources",   "List MCP resources"},
        {"ReadMcpResource",    "Read MCP resource"},
        {"McpAuth",            "MCP server authentication"},
        {"EnterWorktree",      "Enter git worktree"},
        {"ExitWorktree",       "Exit git worktree"},
        {"SuggestBackgroundPR","Suggest background PR"},
        {"VerifyPlanExecution","Verify plan execution"},
        {"OverflowTest",       "Overflow test harness"},
        {"CtxInspect",         "Inspect collapsed context"},
        {"TerminalCapture",    "Capture terminal panel"},
        {"PushNotification",   "Push notification"},
        {"SubscribePR",        "Subscribe to PR updates"},
        {"SendUserFile",       "Send file to user"},
        {"StructuredOutput",   "Structured / synthetic output"},
        {"Workflow",           "Workflow orchestration"},
        {"TungstenTool",       "Tungsten operations"},
        {"TestingPermission",  "Testing permission harness"},
    };

    return std::vector<std::pair<std::string_view, std::string_view>>(
        std::begin(mapping), std::end(mapping));
}

/// Convenience: the canonical "Bash" tool id (matches TS
/// `BASH_TOOL_NAME`). Exposed as a constexpr so the same source of
/// truth feeds prompt-building callers and the UI.
inline constexpr std::string_view BASH_TOOL_NAME = "Bash";
inline constexpr std::string_view SCRIPT_TOOL_NAME = "Script";

/// Look up whether a tool id is known to the registry (vs. a fallback
/// that would render the raw id). Useful for validating config.
[[nodiscard]] constexpr bool is_known_tool(std::string_view tool_id) noexcept
{
    constexpr std::string_view ids[] = {
        "Bash","Glob","Grep","Read","Write","Edit","NotebookEdit","ToolSearch",
        "WebFetch","WebSearch","WebBrowser","Agent","Skill","TeamCreate",
        "TeamDelete","SendMessage","TaskCreate","TaskGet","TaskUpdate",
        "TaskList","TaskStop","TaskOutput","Script","REPL","EnterPlanMode",
        "ExitPlanMode","Sleep","CronCreate","CronDelete","CronList",
        "RemoteTrigger","Monitor","AskUserQuestion","Config","Brief",
        "TodoWrite","LSP","ListMcpResources","ReadMcpResource","McpAuth",
        "EnterWorktree","ExitWorktree","SuggestBackgroundPR",
        "VerifyPlanExecution","OverflowTest","CtxInspect","TerminalCapture",
        "PushNotification","SubscribePR","SendUserFile","StructuredOutput",
        "Workflow","TungstenTool","TestingPermission"
    };
    for (auto id : ids) if (id == tool_id) return true;
    return false;
}

} // namespace cc::tools
