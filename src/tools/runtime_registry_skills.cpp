// Implementation unit for cc.tools.runtime_registry — the skill loader
// executor, the runtime tool-name list, and tool_search.
module;

#include <cstdlib>  // std::getenv("HOME") in execute_skill_tool

module cc.tools.runtime_registry;

import std;

import cc.tools.tool;
import cc.skills.skill;
import cc.tools.agent_runtime;
import cc.tools.feature_flags;
import cc.utils.json;

namespace cc::tools::detail {

using cc::core::Result;
using cc::core::ToolInput;
using cc::core::ToolResult;

namespace fs = std::filesystem;

[[nodiscard]] Result<ToolResult> execute_skill_tool(const ToolInput& input) {
    auto name = json_string(input.json(), "name").or_else([&] { return json_string(input.json(), "skill"); });
    if (!name || name->empty()) return ToolResult::error("skill requires name");

    cc::skills::SkillLoader loader;
    if (const char* home = std::getenv("HOME")) {
        loader.add_search_path(fs::path{home} / ".codex" / "skills");
        loader.add_search_path(fs::path{home} / ".agents" / "skills");
    }
    loader.add_search_path(fs::current_path() / "skills");

    std::vector<std::pair<std::string, fs::path>> plugin_skill_paths;
    for (const auto& plugin : cc::tools::agent_runtime::discover_plugin_component_paths()) {
        for (const auto& path : plugin.skills_paths) {
            plugin_skill_paths.emplace_back(plugin.plugin_name, path);
        }
    }
    auto discovered = loader.discover_all_with_plugin_skills(plugin_skill_paths);
    if (discovered) {
        for (const auto& skill : *discovered) {
            if (skill.name == *name) return ToolResult::success(skill.content);
        }
    }

    std::vector<fs::path> roots;
    if (const char* home = std::getenv("HOME")) {
        roots.push_back(fs::path{home} / ".codex" / "skills");
        roots.push_back(fs::path{home} / ".agents" / "skills");
    }
    roots.push_back(fs::current_path() / "skills");

    for (const auto& root : roots) {
        auto skill = root / *name / "SKILL.md";
        if (!fs::exists(skill)) continue;
        std::ifstream in(skill);
        std::stringstream buffer;
        buffer << in.rdbuf();
        return ToolResult::success(buffer.str());
    }
    return ToolResult::error(std::format("Skill not found: {}", *name));
}

[[nodiscard]] std::vector<std::string> runtime_tool_names_impl() {
    namespace features = cc::tools::features;

    std::vector<std::string> names = {
        "Agent",
        "computer_use",
        "Edit",
        "Read",
        "WebFetch",
        "WebSearch",
        "Write",
        "ask_user_question",
        "brief",
        "config",
        "enter_plan_mode",
        "exit_plan_mode",
        "list_mcp_resources",
        "mcp",
        "mcp_auth",
        "notebook_edit",
        "read_mcp_resource",
        "send_message",
        "shared",
        "skill",
        "synthetic_output",
        "todo_write",
    };
    // TS REF: src/tools.ts:199 (isBashToolDisabled)
    if constexpr (features::kBashToolEnabled) {
        names.push_back("Bash");
    }
    // TS REF: src/tools.ts:203 (hasEmbeddedSearchTools)
    if constexpr (!features::kEmbeddedSearchTools) {
        names.push_back("Glob");
        names.push_back("Grep");
    }
    // TS REF: src/tools.ts:227 (isWorktreeModeEnabled)
    if constexpr (features::kWorktreeMode) {
        names.push_back("enter_worktree");
        names.push_back("exit_worktree");
    }
    // TS REF: src/tools.ts:226 (ENABLE_LSP_TOOL)
    if constexpr (features::kEnableLspTool) {
        names.push_back("lsp");
    }
    // TS REF: src/tools.ts:152-157 (PowerShell)
    if constexpr (features::kPowerShellTool) {
        names.push_back("powershell");
    }
    // TS REF: src/tools.ts:36-38 (AGENT_TRIGGERS_REMOTE)
    // so it is registered unconditionally.  Runtime behavior is controlled by
    // LOOM_REMOTE_TRIGGER_COMMAND env var.
    // TS REF: src/tools.ts:16-19 (USER_TYPE==='ant' — REPLTool)
    // In CPP, "repl" delegates to execute_script() which has a working
    // implementation, so it is registered unconditionally (not ant-only).
    names.push_back("repl");
    // TS REF: src/tools.ts:29-34 (AGENT_TRIGGERS)
    // In CPP, schedule_cron has a working implementation (cc.tools.cron),
    // so it is registered unconditionally.
    names.push_back("schedule_cron");
    // TS REF: src/tools.ts:252-254 (isScriptToolEnabled)
    if constexpr (features::kScriptToolEnabled) {
        names.push_back("script");
    }
    // TS REF: src/tools.ts:25-28 (SleepTool)
    if constexpr (features::kEnableSleepTool) {
        names.push_back("sleep");
    }
    // TS REF: src/tools.ts:220-222 (isTodoV2Enabled)
    if constexpr (features::kTodoV2) {
        for (const auto* t : {"task_create", "task_get", "task_list", "task_output", "task_stop", "task_update"}) {
            names.push_back(t);
        }
    }
    // TS REF: src/tools.ts:230-232 (isAgentSwarmsEnabled)
    if constexpr (features::kAgentSwarmsEnabled) {
        names.push_back("team_create");
        names.push_back("team_delete");
    }
    // TS REF: src/tools.ts:246 (NODE_ENV==='test')
    if constexpr (features::kTestingPermissionTool) {
        names.push_back("testing");
    }
    // TS REF: src/tools.ts:251 (isToolSearchEnabledOptimistic)
    if constexpr (features::kToolSearch) {
        names.push_back("tool_search");
    }
    // TS REF: src/tools.ts:217 (USER_TYPE==='ant' — TungstenTool)
    if constexpr (features::kUserTypeAnt) {
        names.push_back("tungsten");
    }
    // TS REF: src/tools.ts:117-119 (WEB_BROWSER_TOOL)
    if constexpr (features::kWebBrowserTool) {
        names.push_back("web_browser");
    }
    // TS REF: src/tools.ts:129-134 (WORKFLOW_SCRIPTS)
    if constexpr (features::kWorkflowScripts) {
        names.push_back("workflow");
    }
    // Feature-gated stub tools
    if constexpr (features::kPushNotificationTool) names.push_back("push_notification");
    if constexpr (features::kMonitorTool) names.push_back("monitor");
    if constexpr (features::kSendUserFileTool) names.push_back("send_user_file");
    if constexpr (features::kSubscribePRTool) names.push_back("subscribe_pr");
    if constexpr (features::kUserTypeAnt) names.push_back("suggest_background_pr");
    if constexpr (features::kOverflowTestTool) names.push_back("overflow_test");
    if constexpr (features::kContextCollapse) names.push_back("ctx_inspect");
    if constexpr (features::kTerminalPanel) names.push_back("terminal_capture");
    if constexpr (features::kHistorySnip) names.push_back("snip");
    if constexpr (features::kUdsInbox) names.push_back("list_peers");
    if constexpr (features::kVerifyPlanExecution) names.push_back("verify_plan_execution");
    return names;
}

[[nodiscard]] Result<ToolResult> execute_tool_search(const ToolInput& input) {
    auto query = json_string(input.json(), "query").value_or("");
    auto names = runtime_tool_names_impl();
    std::string out = "Tools:\n";
    std::size_t count = 0;
    for (const auto& name : names) {
        if (!query.empty() && !name.contains(query)) continue;
        out += "- " + name + "\n";
        ++count;
    }
    if (count == 0) out += "No matching tools.\n";
    return ToolResult::success(out);
}

} // namespace cc::tools::detail

namespace cc::tools {

[[nodiscard]] std::vector<std::string> runtime_tool_names() {
    return detail::runtime_tool_names_impl();
}

} // namespace cc::tools
