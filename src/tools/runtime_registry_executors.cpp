// Implementation unit for cc.tools.runtime_registry — the simple runtime tool
// executors (shell/lsp/script/task/config/resource/notebook/worktree/brief/
// web-browser). Bodies moved out of the god interface so an edit to one
// executor recompiles this object instead of the importer fan-out.
module;

#include <cctype>   // std::isalnum in safe_ref
#include <cstdlib>  // std::getenv in config_path

module cc.tools.runtime_registry;

import std;

import cc.types.tool_types;
import cc.tools.lsp;
import cc.tools.script;
import cc.tools.script_types;
import cc.tools.task;
import cc.tools.bash;
import cc.tools.agent_runtime;
import cc.tools.notebook;
import cc.tools.web_browser;
import cc.utils.bash_execution;
import cc.utils.json;

namespace cc::tools::detail {

using cc::core::Result;
using cc::core::ToolInput;
using cc::core::ToolOutputContent;
using cc::core::ToolResult;

namespace fs = std::filesystem;

[[nodiscard]] Result<ToolResult> run_command(std::string command, std::size_t max_bytes) {
    auto cap = cc::utils::bash::exec_capture(command);
    if (!cap) {
        return ToolResult::error("Failed to start command");
    }
    std::string output = std::move(cap->output);
    if (output.size() > max_bytes) {
        output = output.substr(0, max_bytes) + "\n[output truncated]\n";
    }
    auto status = cap->status;
    if (output.empty()) output = std::format("Command exited with status {}", status);
    if (status != 0) {
        return ToolResult::error(std::format("Command failed with status {}:\n{}", status, output));
    }
    return ToolResult::success(output);
}

[[nodiscard]] LspAction parse_lsp_action(std::string_view action) {
    // Canonical action strings mirror lsp_action_name() in lsp_tool.cppm.
    // Without these mappings the runtime registry's execute_lsp_tool would
    // silently fall through to LspAction::Symbols for the newer actions.
    if (action == "diagnostics") return LspAction::Diagnostics;
    if (action == "definition") return LspAction::Definition;
    if (action == "references") return LspAction::References;
    if (action == "completion") return LspAction::Completion;
    if (action == "hover") return LspAction::Hover;
    if (action == "symbols") return LspAction::Symbols;
    if (action == "implementation") return LspAction::Implementation;
    if (action == "workspaceSymbol") return LspAction::WorkspaceSymbol;
    if (action == "prepareCallHierarchy") return LspAction::PrepareCallHierarchy;
    if (action == "incomingCalls") return LspAction::IncomingCalls;
    if (action == "outgoingCalls") return LspAction::OutgoingCalls;
    return LspAction::Symbols;
}

[[nodiscard]] std::string format_lsp_result(const LspResult& result, std::string_view action) {
    if (result.empty()) return std::format("No LSP results for action '{}'.", action);
    std::string out;
    for (const auto& diagnostic : result.diagnostics) {
        out += std::format("{}:{}:{} {}\n", diagnostic.source,
            diagnostic.range.start.line, diagnostic.range.start.character, diagnostic.message);
    }
    for (const auto& location : result.locations) {
        out += std::format("{}:{}:{}\n", location.uri, location.range.start.line, location.range.start.character);
    }
    for (const auto& completion : result.completions) {
        out += std::format("{} {}\n", completion.label, completion.detail);
    }
    for (const auto& symbol : result.symbols) {
        out += std::format("{} {}\n", symbol.kind, symbol.name);
    }
    if (result.hover) out += result.hover->contents;
    return out.empty() ? std::format("No LSP results for action '{}'.", action) : out;
}

[[nodiscard]] Result<ToolResult> execute_lsp_tool(const ToolInput& input) {
    auto json = input.json();
    auto file_text = json_string(json, "file_path").or_else([&] { return json_string(json, "path"); });
    auto action = json_string(json, "action").value_or("symbols");
    if (!file_text || file_text->empty()) {
        return ToolResult::error("lsp requires file_path");
    }
    fs::path file = *file_text;
    if (!fs::exists(file)) {
        return ToolResult::error(std::format("File not found: {}", file.string()));
    }

    LspTool tool;
    tool.set_connected(true);
    LspRequest request{
        .action = parse_lsp_action(action),
        .file_path = file,
        .position = LspPosition{.line = json_int(json, "line").value_or(0),
                                .character = json_int(json, "character").value_or(0)},
        .query = json_string(json, "query"),
    };
    auto result = tool.execute(std::move(request));
    if (!result) return ToolResult::error(std::string(format_error(result.error())));
    return ToolResult::success(format_lsp_result(*result, action));
}

[[nodiscard]] Result<ToolResult> execute_script(const ToolInput& input) {
    auto json = input.json();
    auto code = json_string(json, "code");
    if (!code || code->empty()) return ToolResult::error("script requires code");

    auto language = json_string(json, "language").value_or("shell");
    auto timeout = std::clamp(json_int(json, "timeout").value_or(30), 1, 300);
    ScriptLanguage script_language = ScriptLanguage::Shell;
    if (language == "python" || language == "python3") {
        script_language = ScriptLanguage::Python;
    } else if (language == "javascript" || language == "js" || language == "node") {
        script_language = ScriptLanguage::JavaScript;
    } else if (language != "shell" && language != "sh") {
        return ToolResult::error(std::format("Unsupported script language: {}", language));
    }

    ScriptTool tool;
    auto result = tool.execute(ScriptRequest{
        .code = *code,
        .language = script_language,
        .limits = SandboxLimits{.timeout = std::chrono::seconds(timeout)},
        .enable_type_check = json_bool(json, "enable_type_check", false),
        .stdin_data = json_string(json, "stdin"),
    });
    if (!result) return ToolResult::error(std::string(format_error(result.error())));
    auto output = result->output;
    if (!result->errors.empty()) output += "\n" + result->errors;
    if (output.empty()) output = std::format("Script exited with code {}", result->exit_code);
    return result->exit_code == 0 ? ToolResult::success(output) : ToolResult::error(output);
}

[[nodiscard]] TaskStatus parse_task_status(std::string_view status) {
    if (status == "completed") return TaskStatus::Completed;
    if (status == "failed") return TaskStatus::Failed;
    if (status == "cancelled") return TaskStatus::Cancelled;
    if (status == "running") return TaskStatus::Running;
    return TaskStatus::Pending;
}

[[nodiscard]] std::string format_task_summary(const Task& task) {
    std::string out = std::format("{} [{}] {}", task.id, task_status_name(task.status), task.description);
    if (task.result) out += "\nresult: " + *task.result;
    if (task.error_message) out += "\nerror: " + *task.error_message;
    return out;
}

[[nodiscard]] std::string background_task_status(const bash::BackgroundTaskSnapshot& task) {
    if (task.stopped) return "stopped";
    if (task.running) return "running";
    if (task.error) return "failed";
    return "completed";
}

[[nodiscard]] std::string format_background_task_summary(
    const bash::BackgroundTaskSnapshot& task,
    bool include_output) {
    std::string out = std::format(
        "Task: {}\nStatus: {}\nPID: {}\nCommand: {}",
        task.id,
        background_task_status(task),
        task.pid,
        task.command);
    if (task.exit_code) {
        out += std::format("\nExit code: {}", *task.exit_code);
    }
    if (task.error) {
        out += "\nError: " + *task.error;
    }
    if (include_output) {
        out += "\n\nOutput:\n";
        out += task.output.empty() ? "(no output)" : task.output;
    }
    return out;
}

[[nodiscard]] Result<ToolResult> execute_task_tool(std::string_view tool_name, const ToolInput& input) {
    auto json = input.json();
    if (tool_name == "task_create") {
        auto description = json_string(json, "description").or_else([&] { return json_string(json, "task"); });
        if (!description || description->empty()) return ToolResult::error("task_create requires description");
        auto id = json_string(json, "task_id").or_else([&] { return json_string(json, "id"); })
            .value_or(std::format("task-{}", global_task_store().list().size() + 1));
        TaskCreateTool tool;
        auto task = tool.execute(id, *description);
        if (!task) return ToolResult::error(std::string(format_error(task.error())));
        return ToolResult::success("Created task " + format_task_summary(**task));
    }

    if (tool_name == "task_list") {
        TaskListTool tool;
        auto listed = tool.execute();
        auto native_agents = agent_runtime::native_agent_store().list();
        std::string out = "Tasks:\n";
        bool has_tasks = false;
        for (const auto* task : listed) {
            out += "- " + format_task_summary(*task) + "\n";
            has_tasks = true;
        }
        for (const auto& agent : native_agents) {
            if (!is_native_agent_task(agent)) continue;
            out += "- " + format_native_agent_task_summary(agent) + "\n";
            has_tasks = true;
        }
        if (!has_tasks) out += "No tasks.\n";
        return ToolResult::success(out);
    }

    auto id = json_string(json, "task_id")
        .or_else([&] { return json_string(json, "id"); })
        .or_else([&]() -> std::optional<std::string> {
            if (auto pid = json_int(json, "pid"); pid && *pid > 0) {
                return std::to_string(*pid);
            }
            return std::nullopt;
        });
    if (!id || id->empty()) return ToolResult::error(std::format("{} requires task_id or pid", tool_name));

    if (tool_name == "task_get") {
        if (auto background = bash::get_background_task_snapshot(*id)) {
            return ToolResult::success(format_background_task_summary(*background, false));
        }
        if (auto agent = agent_runtime::native_agent_store().get(*id);
            agent && is_native_agent_task(*agent)) {
            return ToolResult::success(format_native_agent_task_summary(*agent));
        }
        TaskGetTool tool;
        auto task = tool.execute(*id);
        if (!task) return ToolResult::error(std::string(format_error(task.error())));
        return ToolResult::success(format_task_summary(**task));
    }
    if (tool_name == "task_stop") {
        if (auto background = bash::get_background_task_snapshot(*id)) {
            if (!bash::stop_background_task(*id)) {
                return ToolResult::error(std::format("Failed to stop task {}", *id));
            }
            auto stopped = bash::get_background_task_snapshot(*id).value_or(*background);
            return ToolResult::success(format_background_task_summary(stopped, false));
        }
        if (auto agent = agent_runtime::native_agent_store().get(*id);
            agent && is_native_agent_task(*agent)) {
            agent_runtime::native_agent_store().request_cancel(agent->agent_id, "stop requested");
            auto stopped = agent_runtime::native_agent_store().get(agent->agent_id).value_or(*agent);
            return ToolResult::success(format_native_agent_task_summary(stopped));
        }
        TaskStopTool tool;
        auto result = tool.execute(*id);
        if (!result) return ToolResult::error(std::string(format_error(result.error())));
        return ToolResult::success(std::format("Stopped task {}", *id));
    }
    if (tool_name == "task_update") {
        auto status = parse_task_status(json_string(json, "status").value_or("running"));
        auto result_text = json_string(json, "result").or_else([&] { return json_string(json, "output"); });
        if (auto agent = agent_runtime::native_agent_store().get(*id);
            agent && is_native_agent_task(*agent)) {
            switch (status) {
                case TaskStatus::Running:
                    agent_runtime::native_agent_store().mark_running(agent->agent_id);
                    break;
                case TaskStatus::Completed:
                    agent_runtime::native_agent_store().mark_completed(agent->agent_id, result_text.value_or(""));
                    break;
                case TaskStatus::Failed:
                    agent_runtime::native_agent_store().mark_failed(agent->agent_id, result_text.value_or("task marked failed"));
                    break;
                case TaskStatus::Cancelled:
                    agent_runtime::native_agent_store().mark_cancelled(agent->agent_id, result_text.value_or("task marked cancelled"));
                    break;
                case TaskStatus::Pending:
                    break;
            }
            return ToolResult::success(std::format("Updated task {} [{}]", *id, task_status_name(status)));
        }
        TaskUpdateTool tool;
        auto result = tool.execute(*id, status, result_text);
        if (!result) return ToolResult::error(std::string(format_error(result.error())));
        return ToolResult::success(std::format("Updated task {} [{}]", *id, task_status_name(status)));
    }
    if (tool_name == "task_output") {
        if (auto background = bash::get_background_task_snapshot(*id)) {
            return ToolResult::success(format_background_task_summary(*background, true));
        }
        if (auto agent = agent_runtime::native_agent_store().get(*id);
            agent && is_native_agent_task(*agent)) {
            return ToolResult::success(format_native_agent_task_output(*agent));
        }
        TaskOutputTool tool;
        auto output = tool.execute(*id);
        if (!output) return ToolResult::error(std::string(format_error(output.error())));
        return ToolResult::success(output->empty() ? "(no output)" : std::string(*output));
    }
    return ToolResult::error(std::format("Unknown task tool: {}", tool_name));
}

[[nodiscard]] fs::path config_path() {
    if (const char* home = std::getenv("HOME")) return fs::path{home} / ".loom" / "config.json";
    return fs::path{".loom"} / "config.json";
}

[[nodiscard]] Result<ToolResult> execute_config_tool(const ToolInput& input) {
    auto json = input.json();
    auto action = json_string(json, "action").value_or("get");
    auto key = json_string(json, "key");
    auto value = json_string(json, "value");
    auto path = config_path();
    fs::create_directories(path.parent_path());

    if (action == "set") {
        if (!key || !value) return ToolResult::error("config set requires key and value");
        std::ofstream out(path, std::ios::app);
        if (!out) return ToolResult::error(std::format("Cannot write {}", path.string()));
        out << *key << "=" << *value << "\n";
        return ToolResult::success(std::format("Set {} in {}", *key, path.string()));
    }

    std::ifstream in(path);
    if (!in) return ToolResult::success(std::format("No config file found at {}", path.string()));
    std::stringstream buffer;
    buffer << in.rdbuf();
    return ToolResult::success(buffer.str());
}

[[nodiscard]] Result<ToolResult> execute_local_resource_read(const ToolInput& input) {
    auto uri = json_string(input.json(), "uri").or_else([&] { return json_string(input.json(), "resource_uri"); });
    if (!uri || uri->empty()) return ToolResult::error("read_mcp_resource requires uri");
    std::string path_text = *uri;
    if (path_text.starts_with("file://")) path_text = path_text.substr(7);
    fs::path path = path_text;
    if (!fs::exists(path)) return ToolResult::error(std::format("Resource not found: {}", path.string()));
    std::ifstream in(path);
    if (!in) return ToolResult::error(std::format("Cannot read resource: {}", path.string()));
    std::stringstream buffer;
    buffer << in.rdbuf();
    return ToolResult::success(buffer.str());
}

[[nodiscard]] Result<ToolResult> execute_resource_list(const ToolInput& input) {
    auto root = json_string(input.json(), "path").value_or(fs::current_path().string());
    fs::path base = root;
    if (!fs::exists(base)) return ToolResult::error(std::format("Path not found: {}", base.string()));
    std::string out = "Resources:\n";
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(base)) {
        if (!entry.is_regular_file()) continue;
        out += std::format("- file://{} [{} bytes]\n", entry.path().string(), fs::file_size(entry.path()));
        if (++count >= 100) break;
    }
    if (count == 0) out += "No file resources.\n";
    return ToolResult::success(out);
}

[[nodiscard]] Result<ToolResult> execute_web_browser(const ToolInput& input) {
    auto json = input.json();
    auto action_text = json_string(json, "action").value_or("extract");
    auto action = parse_browser_action(action_text);
    if (!action) {
        return ToolResult::error(std::format("Unsupported browser action: {}", action_text));
    }

    cc::tools::BrowserRequest request{
        .action = *action,
        .url = json_string(json, "url"),
        .selector = json_string(json, "selector"),
        .form_fields = json_form_fields(json),
        .timeout = std::chrono::seconds(std::clamp(json_int(json, "timeout").value_or(30), 1, 300)),
        .extract_selector = json_string(json, "extract_selector"),
    };

    static cc::tools::WebBrowserTool tool;
    auto result = tool.execute(std::move(request));
    if (!result) {
        return ToolResult::error(std::string(cc::tools::format_error(result.error())));
    }

    if (result->screenshot_base64) {
        std::vector<ToolOutputContent> content;
        content.push_back(ToolOutputContent::text_output(result->content));
        content.push_back(ToolOutputContent::image_output(
            result->media_type.value_or("image/png"),
            std::move(*result->screenshot_base64)));
        return ToolResult::success_multi(std::move(content));
    }

    return ToolResult::success(result->content);
}

[[nodiscard]] bool safe_ref(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '/' || c == '.';
    });
}

[[nodiscard]] Result<ToolResult> execute_worktree(std::string_view mode, const ToolInput& input) {
    auto json = input.json();
    if (mode == "enter") {
        auto branch = json_string(json, "branch").or_else([&] { return json_string(json, "branch_name"); });
        if (!branch || !safe_ref(*branch)) return ToolResult::error("enter_worktree requires a safe branch name");
        auto path = json_string(json, "path").value_or((fs::current_path().parent_path() /
            std::format("{}-{}", fs::current_path().filename().string(), *branch)).string());
        return run_command(std::format("git worktree add -B {} {} 2>&1", runtime_shell_quote(*branch), runtime_shell_quote(path)));
    }

    auto path = json_string(json, "path").value_or(fs::current_path().string());
    return run_command(std::format("git worktree remove {} 2>&1", runtime_shell_quote(path)));
}

[[nodiscard]] Result<ToolResult> execute_brief(const ToolInput& input) {
    auto brief_path = fs::current_path() / ".loom" / "brief.md";
    fs::create_directories(brief_path.parent_path());
    if (auto content = json_string(input.json(), "content").or_else([&] { return json_string(input.json(), "brief"); })) {
        std::ofstream out(brief_path);
        if (!out) return ToolResult::error(std::format("Cannot write {}", brief_path.string()));
        out << *content;
        return ToolResult::success(std::format("Brief saved to {}", brief_path.string()));
    }
    std::ifstream in(brief_path);
    if (!in) return ToolResult::success("No brief has been saved for this workspace.");
    std::stringstream buffer;
    buffer << in.rdbuf();
    return ToolResult::success(buffer.str());
}

[[nodiscard]] Result<ToolResult> execute_notebook_edit(const ToolInput& input) {
    auto parsed = cc::utils::json::parse(input.json());
    if (!parsed || !parsed->root().is_obj()) {
        return ToolResult::error("notebook_edit input must be a JSON object");
    }
    auto root = parsed->root();

    auto path = runtime_json_string(root, "notebook_path")
        .or_else([&] { return runtime_json_string(root, "file_path"); })
        .or_else([&] { return runtime_json_string(root, "path"); });
    if (!path) {
        return ToolResult::error("notebook_edit requires notebook_path");
    }

    NotebookEditTool tool;
    auto notebook_result = tool.load_notebook(*path);
    if (!notebook_result) {
        return ToolResult::error(std::string(format_error(notebook_result.error())));
    }
    const auto& notebook = *notebook_result;

    NotebookEditRequest request;
    request.notebook_path = *path;

    if (auto operation_text = runtime_json_string(root, "operation")) {
        auto operation = parse_notebook_operation(*operation_text);
        if (!operation) return ToolResult::error("notebook_edit operation must be insert, delete, update, or move");
        request.operation = *operation;
        auto index = runtime_json_int(root, "cell_index").or_else([&] { return runtime_json_int(root, "cell_number"); });
        if (!index || *index < 0) return ToolResult::error("notebook_edit requires a non-negative cell_index");
        request.cell_index = static_cast<std::size_t>(*index);
        if (auto target = runtime_json_int(root, "target_index"); target && *target >= 0) {
            request.target_index = static_cast<std::size_t>(*target);
        }
        request.source = runtime_json_string(root, "source")
            .or_else([&] { return runtime_json_string(root, "new_source"); });
    } else {
        auto edit_mode = runtime_json_string(root, "edit_mode").value_or("replace");
        auto operation = parse_notebook_operation(edit_mode);
        if (!operation) return ToolResult::error("notebook_edit edit_mode must be replace, insert, or delete");
        request.operation = *operation;
        request.source = runtime_json_string(root, "new_source")
            .or_else([&] { return runtime_json_string(root, "source"); });

        if (auto index = runtime_json_int(root, "cell_index").or_else([&] { return runtime_json_int(root, "cell_number"); })) {
            if (*index < 0) return ToolResult::error("notebook_edit cell index must be non-negative");
            request.cell_index = static_cast<std::size_t>(*index);
        } else {
            auto cell_id = runtime_json_string(root, "cell_id");
            if (cell_id) {
                auto resolved = resolve_notebook_cell_index(notebook, cell_id);
                if (!resolved) return ToolResult::error(std::format("Cell with ID \"{}\" not found in notebook", *cell_id));
                request.cell_index = *resolved;
                if (request.operation == CellOperation::Insert) {
                    ++request.cell_index;
                }
            } else if (request.operation == CellOperation::Insert) {
                request.cell_index = 0;
            } else {
                return ToolResult::error("notebook_edit requires cell_id or cell_index when not inserting");
            }
        }

        if (request.operation == CellOperation::Update && request.cell_index == notebook.cells.size()) {
            request.operation = CellOperation::Insert;
        }
    }

    if (auto cell_type_text = runtime_json_string(root, "cell_type")) {
        auto cell_type = parse_cell_type(*cell_type_text);
        if (!cell_type) return ToolResult::error("notebook_edit cell_type must be code, markdown, or raw");
        request.cell_type = *cell_type;
    }

    if ((request.operation == CellOperation::Insert || request.operation == CellOperation::Update) && !request.source) {
        return ToolResult::error("notebook_edit requires new_source for insert/update");
    }

    auto result = tool.execute(std::move(request));
    if (!result) {
        return ToolResult::error(std::string(format_error(result.error())));
    }

    return ToolResult::success(result->message);
}

} // namespace cc::tools::detail
