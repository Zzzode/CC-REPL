/// @file tool_ui_longtail.cppm
/// @brief Long-tail tool UIs — all remaining tools in one module.
///
/// MODULE:   cc.ui.tools.longtail
/// LICENCE:  Exported.  Imported by tool UI registry initialization.
///
/// These are the less-frequently-used tools that don't warrant their own
/// module.  All follow the same pattern: extract a key field from the input
/// JSON for the message summary, with appropriate user-facing names.
///
/// Faithful TS port — userFacingName values match the TS source.
module;

#include <string>
#include <string_view>
#include <optional>
#include <functional>

export module cc.ui.tools.longtail;

import cc.ui.tools.registry;

export namespace cc::ui::tools::longtail_ui {

using namespace cc::ui::tools;

namespace detail {

/// Extract a top-level string field from JSON by key name.
/// Lightweight string extraction — no full JSON parser needed.
[[nodiscard]] inline std::string extract_string_field(std::string_view json,
                                                      std::string_view key) {
    auto pos = json.find("\"" + std::string(key) + "\"");
    if (pos == std::string_view::npos) return {};
    auto colon = json.find(':', pos);
    if (colon == std::string_view::npos) return {};
    auto quote = json.find('"', colon + 1);
    if (quote == std::string_view::npos) return {};
    auto end = json.find('"', quote + 1);
    if (end == std::string_view::npos) return {};
    return std::string(json.substr(quote + 1, end - quote - 1));
}

/// Extract a top-level integer/numeric field from JSON by key name.
[[nodiscard]] inline std::string extract_number_field(std::string_view json,
                                                      std::string_view key) {
    auto pos = json.find("\"" + std::string(key) + "\"");
    if (pos == std::string_view::npos) return {};
    auto colon = json.find(':', pos);
    if (colon == std::string_view::npos) return {};
    // skip whitespace after colon
    auto start = colon + 1;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t'))
        ++start;
    if (start >= json.size()) return {};
    auto end = start;
    while (end < json.size() &&
           ((json[end] >= '0' && json[end] <= '9') || json[end] == '.' ||
            json[end] == '-' || json[end] == 'e' || json[end] == 'E'))
        ++end;
    if (end == start) return {};
    return std::string(json.substr(start, end - start));
}

/// Truncate a string to max_len, adding ellipsis if needed.
[[nodiscard]] inline std::string truncate(std::string_view s,
                                          std::size_t max_len = 60) {
    if (s.size() <= max_len) return std::string(s);
    return std::string(s.substr(0, max_len - 1)) + "\xE2\x80\xA6";
}

} // namespace detail

// ============================================================
// Helper: make simple tool UI with name + single field extraction
// ============================================================

namespace detail {

/// Build a ToolUIFunctions for a simple tool with:
/// - fixed user_facing_name
/// - message = extract field from JSON, fallback to default_msg
/// - no tag
/// - progress = "Running…"
/// - queued = "Waiting…"
[[nodiscard]] inline ToolUIFunctions make_simple_tool_ui(
    std::string user_facing_name,
    std::string default_msg,
    std::optional<std::string> field_for_message = std::nullopt,
    bool is_transparent_wrapper = false)
{
    ToolUIFunctions fns;
    fns.user_facing_name = [name = std::move(user_facing_name)](std::string_view) {
        return name;
    };

    if (field_for_message) {
        auto field = *field_for_message;
        fns.message = [field = std::move(field),
                        fallback = std::move(default_msg)](std::string_view input) {
            auto val = detail::extract_string_field(input, field);
            if (!val.empty()) return detail::truncate(val);
            return fallback;
        };
    } else {
        fns.message = [msg = std::move(default_msg)](std::string_view) {
            return msg;
        };
    }

    fns.tag = [](std::string_view) -> std::optional<std::string> {
        return std::nullopt;
    };

    fns.progress = [name = std::string{}](std::string_view, std::string_view) {
        // TS default for tools without custom progress
        return std::string{"Running…"};
    };

    fns.queued = [name = std::string{}](std::string_view) {
        return std::string{"Waiting to run…"};
    };

    fns.is_transparent_wrapper = is_transparent_wrapper;

    return fns;
}

} // namespace detail

// ============================================================
// ComputerUse
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_computer_use_ui() {
    ToolUIFunctions fns;
    fns.user_facing_name = [](std::string_view) { return std::string{"Computer Use"}; };
    fns.message = [](std::string_view input) {
        auto action = detail::extract_string_field(input, "action");
        if (!action.empty()) return detail::truncate(action);
        return std::string{"controlling computer…"};
    };
    fns.tag = [](std::string_view) -> std::optional<std::string> { return std::nullopt; };
    fns.progress = [](std::string_view, std::string_view) { return std::string{"Running…"}; };
    fns.queued = [](std::string_view) { return std::string{"Waiting to run…"}; };
    fns.is_transparent_wrapper = false;
    return fns;
}

// ============================================================
// WebBrowser
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_web_browser_ui() {
    ToolUIFunctions fns;
    fns.user_facing_name = [](std::string_view) { return std::string{"Browser"}; };
    fns.message = [](std::string_view input) {
        auto action = detail::extract_string_field(input, "action");
        auto url = detail::extract_string_field(input, "url");
        if (!action.empty() && !url.empty()) {
            return action + ": " + detail::truncate(url, 40);
        }
        if (!url.empty()) return detail::truncate(url);
        if (!action.empty()) return action;
        return std::string{"browsing web…"};
    };
    fns.tag = [](std::string_view) -> std::optional<std::string> { return std::nullopt; };
    fns.progress = [](std::string_view, std::string_view) { return std::string{"Running…"}; };
    fns.queued = [](std::string_view) { return std::string{"Waiting to run…"}; };
    fns.is_transparent_wrapper = false;
    return fns;
}

// ============================================================
// AskUserQuestion
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_ask_user_ui() {
    ToolUIFunctions fns;
    fns.user_facing_name = [](std::string_view) { return std::string{"Ask"}; };
    fns.message = [](std::string_view input) {
        auto q = detail::extract_string_field(input, "question");
        if (!q.empty()) return detail::truncate(q);
        return std::string{"asking user…"};
    };
    fns.tag = [](std::string_view) -> std::optional<std::string> { return std::nullopt; };
    fns.progress = [](std::string_view, std::string_view) { return std::string{"Waiting for user…"}; };
    fns.queued = [](std::string_view) { return std::string{"Waiting to ask…"}; };
    fns.is_transparent_wrapper = false;
    return fns;
}

// ============================================================
// Brief
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_brief_ui() {
    auto fns = detail::make_simple_tool_ui("Brief", "updating brief…", "mode");
    fns.message = [](std::string_view input) {
        auto mode = detail::extract_string_field(input, "mode");
        if (mode == "read") return std::string{"reading brief"};
        if (mode == "write") return std::string{"writing brief"};
        return std::string{"brief…"};
    };
    return fns;
}

// ============================================================
// Config
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_config_ui() {
    auto fns = detail::make_simple_tool_ui("Config", "reading config…", "mode");
    fns.message = [](std::string_view input) {
        auto mode = detail::extract_string_field(input, "mode");
        if (mode == "read") return std::string{"reading config"};
        if (mode == "update") return std::string{"updating config"};
        return std::string{"config…"};
    };
    return fns;
}

// ============================================================
// Enter / Exit Plan Mode
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_plan_mode_ui() {
    auto fns = detail::make_simple_tool_ui("Plan Mode", "managing plan mode…");
    return fns;
}

// ============================================================
// Enter / Exit Worktree
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_worktree_ui() {
    auto fns = detail::make_simple_tool_ui("Worktree", "managing worktree…", "name");
    return fns;
}

// ============================================================
// NotebookEdit
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_notebook_edit_ui() {
    auto fns = detail::make_simple_tool_ui("Notebook", "editing notebook…", "path");
    fns.message = [](std::string_view input) {
        auto path = detail::extract_string_field(input, "path");
        auto cell_id = detail::extract_string_field(input, "cell_id");
        if (!path.empty() && !cell_id.empty()) {
            return detail::truncate(path, 40) + " #" + cell_id;
        }
        if (!path.empty()) return detail::truncate(path);
        return std::string{"editing notebook…"};
    };
    return fns;
}

// ============================================================
// PowerShell
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_powershell_ui() {
    auto fns = detail::make_simple_tool_ui("PowerShell", "running command…", "command");
    fns.message = [](std::string_view input) {
        auto cmd = detail::extract_string_field(input, "command");
        if (!cmd.empty()) return detail::truncate(cmd);
        return std::string{"running command…"};
    };
    return fns;
}

// ============================================================
// REPL
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_repl_ui() {
    auto fns = detail::make_simple_tool_ui("REPL", "running snippet…", "code");
    fns.message = [](std::string_view input) {
        auto code = detail::extract_string_field(input, "code");
        if (!code.empty()) return detail::truncate(code);
        return std::string{"running snippet…"};
    };
    return fns;
}

// ============================================================
// ScheduleCron
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_schedule_ui() {
    auto fns = detail::make_simple_tool_ui("Schedule", "scheduling reminder…", "cron");
    return fns;
}

// ============================================================
// Script
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_script_ui() {
    auto fns = detail::make_simple_tool_ui("Script", "running script…", "script");
    return fns;
}

// ============================================================
// SendMessage
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_send_message_ui() {
    auto fns = detail::make_simple_tool_ui("Send", "sending message…", "target");
    return fns;
}

// ============================================================
// Shared (key-value)
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_shared_ui() {
    auto fns = detail::make_simple_tool_ui("Shared", "shared state…", "mode");
    return fns;
}

// ============================================================
// Sleep
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_sleep_ui() {
    auto fns = detail::make_simple_tool_ui("Sleep", "sleeping…", "seconds");
    fns.message = [](std::string_view input) {
        auto secs = detail::extract_number_field(input, "seconds");
        if (!secs.empty()) return "sleeping " + secs + "s…";
        return std::string{"sleeping…"};
    };
    fns.progress = [](std::string_view input, std::string_view) {
        auto secs = detail::extract_number_field(input, "seconds");
        if (!secs.empty()) return "Sleeping for " + secs + "s…";
        return std::string{"Sleeping…"};
    };
    return fns;
}

// ============================================================
// SyntheticOutput
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_synthetic_output_ui() {
    auto fns = detail::make_simple_tool_ui("Synthetic", "synthetic output…", "content");
    return fns;
}

// ============================================================
// Testing
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_testing_ui() {
    return detail::make_simple_tool_ui("Testing", "running test…");
}

// ============================================================
// TodoWrite
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_todo_write_ui() {
    auto fns = detail::make_simple_tool_ui("Todo", "updating todos…", "action");
    return fns;
}

// ============================================================
// ToolSearch
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_tool_search_ui() {
    auto fns = detail::make_simple_tool_ui("Search", "searching tools…", "query");
    return fns;
}

// ============================================================
// Tungsten (internal)
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_tungsten_ui() {
    return detail::make_simple_tool_ui("Tungsten", "tungsten…");
}

// ============================================================
// Workflow
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_workflow_ui() {
    auto fns = detail::make_simple_tool_ui("Workflow", "running workflow…", "name");
    return fns;
}

// ============================================================
// TaskGet / TaskList / TaskOutput / TaskStop
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_task_get_ui() {
    ToolUIFunctions fns;
    fns.user_facing_name = [](std::string_view) { return std::string{"TaskGet"}; };
    fns.message = [](std::string_view input) {
        auto id = detail::extract_string_field(input, "task_id");
        if (id.empty()) id = detail::extract_number_field(input, "task_id");
        if (!id.empty()) return "#" + id;
        return std::string{"getting task…"};
    };
    fns.tag = [](std::string_view) -> std::optional<std::string> { return std::nullopt; };
    fns.progress = [](std::string_view, std::string_view) { return std::string{"Getting task…"}; };
    fns.queued = [](std::string_view) { return std::string{"Waiting to get task…"}; };
    fns.is_transparent_wrapper = false;
    return fns;
}

[[nodiscard]] inline ToolUIFunctions make_task_list_ui() {
    auto fns = detail::make_simple_tool_ui("TaskList", "listing tasks…");
    return fns;
}

[[nodiscard]] inline ToolUIFunctions make_task_output_ui() {
    ToolUIFunctions fns;
    fns.user_facing_name = [](std::string_view) { return std::string{"TaskOutput"}; };
    fns.message = [](std::string_view input) {
        auto id = detail::extract_string_field(input, "task_id");
        if (id.empty()) id = detail::extract_number_field(input, "task_id");
        if (!id.empty()) return "#" + id + " output";
        return std::string{"task output…"};
    };
    fns.tag = [](std::string_view) -> std::optional<std::string> { return std::nullopt; };
    fns.progress = [](std::string_view, std::string_view) { return std::string{"Reading output…"}; };
    fns.queued = [](std::string_view) { return std::string{"Waiting to read output…"}; };
    fns.is_transparent_wrapper = false;
    return fns;
}

[[nodiscard]] inline ToolUIFunctions make_task_stop_ui() {
    ToolUIFunctions fns;
    fns.user_facing_name = [](std::string_view) { return std::string{"TaskStop"}; };
    fns.message = [](std::string_view input) {
        auto id = detail::extract_string_field(input, "task_id");
        if (id.empty()) id = detail::extract_number_field(input, "task_id");
        if (!id.empty()) return "stop #" + id;
        return std::string{"stopping task…"};
    };
    fns.tag = [](std::string_view) -> std::optional<std::string> {
        return std::string{"stop"};
    };
    fns.progress = [](std::string_view, std::string_view) { return std::string{"Stopping task…"}; };
    fns.queued = [](std::string_view) { return std::string{"Waiting to stop task…"}; };
    fns.is_transparent_wrapper = false;
    return fns;
}

// ============================================================
// TeamCreate / TeamDelete
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_team_create_ui() {
    auto fns = detail::make_simple_tool_ui("TeamCreate", "creating team…", "name");
    fns.is_transparent_wrapper = true;  // team tools are wrappers
    return fns;
}

[[nodiscard]] inline ToolUIFunctions make_team_delete_ui() {
    auto fns = detail::make_simple_tool_ui("TeamDelete", "deleting team…", "name");
    return fns;
}

// ============================================================
// MCP misc: list_mcp_resources / read_mcp_resource / mcp_auth
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_list_mcp_resources_ui() {
    ToolUIFunctions fns;
    fns.user_facing_name = [](std::string_view) { return std::string{"MCP"}; };
    fns.message = [](std::string_view input) {
        auto server = detail::extract_string_field(input, "server_name");
        if (!server.empty()) return "list resources: " + server;
        return std::string{"listing MCP resources…"};
    };
    fns.tag = [](std::string_view) -> std::optional<std::string> { return "MCP"; };
    fns.progress = [](std::string_view, std::string_view) { return std::string{"Listing resources…"}; };
    fns.queued = [](std::string_view) { return std::string{"Waiting to list…"}; };
    fns.is_transparent_wrapper = false;
    return fns;
}

[[nodiscard]] inline ToolUIFunctions make_read_mcp_resource_ui() {
    ToolUIFunctions fns;
    fns.user_facing_name = [](std::string_view) { return std::string{"MCP"}; };
    fns.message = [](std::string_view input) {
        auto uri = detail::extract_string_field(input, "uri");
        if (!uri.empty()) return "read: " + detail::truncate(uri, 45);
        return std::string{"reading MCP resource…"};
    };
    fns.tag = [](std::string_view) -> std::optional<std::string> { return "MCP"; };
    fns.progress = [](std::string_view, std::string_view) { return std::string{"Reading resource…"}; };
    fns.queued = [](std::string_view) { return std::string{"Waiting to read…"}; };
    fns.is_transparent_wrapper = false;
    return fns;
}

[[nodiscard]] inline ToolUIFunctions make_mcp_auth_ui() {
    ToolUIFunctions fns;
    fns.user_facing_name = [](std::string_view) { return std::string{"MCP"}; };
    fns.message = [](std::string_view input) {
        auto server = detail::extract_string_field(input, "server_name");
        if (!server.empty()) return "auth: " + server;
        return std::string{"checking MCP auth…"};
    };
    fns.tag = [](std::string_view) -> std::optional<std::string> { return "MCP"; };
    fns.progress = [](std::string_view, std::string_view) { return std::string{"Checking auth…"}; };
    fns.queued = [](std::string_view) { return std::string{"Waiting to check auth…"}; };
    fns.is_transparent_wrapper = false;
    return fns;
}

// ============================================================
// RemoteTrigger
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_remote_trigger_ui() {
    auto fns = detail::make_simple_tool_ui("Remote", "remote trigger…", "name");
    fns.message = [](std::string_view input) {
        auto name = detail::extract_string_field(input, "name");
        if (!name.empty()) return "trigger: " + name;
        return std::string{"remote trigger…"};
    };
    return fns;
}

// ============================================================
// Registration — register all longtail tool UIs
// ============================================================

/// Register all long-tail tool UIs in the global registry.
inline void register_longtail_tool_uis() {
    auto& reg = global_tool_ui_registry();

    // ComputerUse
    reg.register_tool_ui("computer_use", make_computer_use_ui());
    reg.register_tool_ui("ComputerUse", make_computer_use_ui());

    // WebBrowser
    reg.register_tool_ui("web_browser", make_web_browser_ui());
    reg.register_tool_ui("WebBrowser", make_web_browser_ui());

    // AskUserQuestion
    reg.register_tool_ui("ask_user_question", make_ask_user_ui());
    reg.register_tool_ui("AskUserQuestion", make_ask_user_ui());

    // Brief
    reg.register_tool_ui("brief", make_brief_ui());
    reg.register_tool_ui("Brief", make_brief_ui());

    // Config
    reg.register_tool_ui("config", make_config_ui());
    reg.register_tool_ui("Config", make_config_ui());

    // Plan mode (enter + exit share UI)
    reg.register_tool_ui("enter_plan_mode", make_plan_mode_ui());
    reg.register_tool_ui("exit_plan_mode", make_plan_mode_ui());
    reg.register_tool_ui("PlanMode", make_plan_mode_ui());

    // Worktree (enter + exit share UI)
    reg.register_tool_ui("enter_worktree", make_worktree_ui());
    reg.register_tool_ui("exit_worktree", make_worktree_ui());
    reg.register_tool_ui("Worktree", make_worktree_ui());

    // NotebookEdit
    reg.register_tool_ui("notebook_edit", make_notebook_edit_ui());
    reg.register_tool_ui("NotebookEdit", make_notebook_edit_ui());

    // PowerShell
    reg.register_tool_ui("powershell", make_powershell_ui());
    reg.register_tool_ui("PowerShell", make_powershell_ui());

    // REPL
    reg.register_tool_ui("repl", make_repl_ui());
    reg.register_tool_ui("REPL", make_repl_ui());

    // ScheduleCron
    reg.register_tool_ui("schedule_cron", make_schedule_ui());
    reg.register_tool_ui("ScheduleCron", make_schedule_ui());

    // Script
    reg.register_tool_ui("script", make_script_ui());
    reg.register_tool_ui("Script", make_script_ui());

    // SendMessage
    reg.register_tool_ui("send_message", make_send_message_ui());
    reg.register_tool_ui("SendMessage", make_send_message_ui());

    // Shared
    reg.register_tool_ui("shared", make_shared_ui());
    reg.register_tool_ui("Shared", make_shared_ui());

    // Sleep
    reg.register_tool_ui("sleep", make_sleep_ui());
    reg.register_tool_ui("Sleep", make_sleep_ui());

    // SyntheticOutput
    reg.register_tool_ui("synthetic_output", make_synthetic_output_ui());
    reg.register_tool_ui("SyntheticOutput", make_synthetic_output_ui());

    // Testing
    reg.register_tool_ui("testing", make_testing_ui());
    reg.register_tool_ui("Testing", make_testing_ui());

    // TodoWrite
    reg.register_tool_ui("todo_write", make_todo_write_ui());
    reg.register_tool_ui("TodoWrite", make_todo_write_ui());

    // ToolSearch
    reg.register_tool_ui("tool_search", make_tool_search_ui());
    reg.register_tool_ui("ToolSearch", make_tool_search_ui());

    // Tungsten
    reg.register_tool_ui("tungsten", make_tungsten_ui());
    reg.register_tool_ui("Tungsten", make_tungsten_ui());

    // Workflow
    reg.register_tool_ui("workflow", make_workflow_ui());
    reg.register_tool_ui("Workflow", make_workflow_ui());

    // Task tools (get/list/output/stop)
    reg.register_tool_ui("task_get", make_task_get_ui());
    reg.register_tool_ui("TaskGet", make_task_get_ui());

    reg.register_tool_ui("task_list", make_task_list_ui());
    reg.register_tool_ui("TaskList", make_task_list_ui());

    reg.register_tool_ui("task_output", make_task_output_ui());
    reg.register_tool_ui("TaskOutput", make_task_output_ui());

    reg.register_tool_ui("task_stop", make_task_stop_ui());
    reg.register_tool_ui("TaskStop", make_task_stop_ui());

    // Team tools
    reg.register_tool_ui("team_create", make_team_create_ui());
    reg.register_tool_ui("TeamCreate", make_team_create_ui());

    reg.register_tool_ui("team_delete", make_team_delete_ui());
    reg.register_tool_ui("TeamDelete", make_team_delete_ui());

    // MCP misc
    reg.register_tool_ui("list_mcp_resources", make_list_mcp_resources_ui());
    reg.register_tool_ui("ListMcpResources", make_list_mcp_resources_ui());

    reg.register_tool_ui("read_mcp_resource", make_read_mcp_resource_ui());
    reg.register_tool_ui("ReadMcpResource", make_read_mcp_resource_ui());

    reg.register_tool_ui("mcp_auth", make_mcp_auth_ui());
    reg.register_tool_ui("McpAuth", make_mcp_auth_ui());

    // RemoteTrigger
    reg.register_tool_ui("remote_trigger", make_remote_trigger_ui());
    reg.register_tool_ui("RemoteTrigger", make_remote_trigger_ui());
}

} // namespace cc::ui::tools::longtail_ui
