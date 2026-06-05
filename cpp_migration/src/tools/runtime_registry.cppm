/// @file runtime_registry.cppm
/// @brief Runtime registration for all migrated tools exposed to the query engine.
module;

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.tools.runtime_registry;

import cc.tools.tool;
import cc.tools.agent;
import cc.tools.agent_runtime;
import cc.tools.ask_user;
import cc.tools.bash;
import cc.tools.brief;
import cc.tools.config;
import cc.tools.cron;
import cc.tools.computer_use;
import cc.tools.file_edit;
import cc.tools.file_read;
import cc.tools.file_write;
import cc.tools.glob;
import cc.tools.grep;
import cc.tools.lsp;
import cc.tools.mcp;
import cc.tools.notebook;
import cc.tools.plan_mode;
import cc.tools.powershell;
import cc.tools.remote_trigger_tool;
import cc.tools.repl_tool;
import cc.tools.script;
import cc.tools.send_message;
import cc.tools.shared_tool;
import cc.tools.skill_tool;
import cc.tools.sleep;
import cc.tools.synthetic_output_tool;
import cc.tools.task;
import cc.tools.team;
import cc.tools.testing_tool;
import cc.tools.todo_write;
import cc.tools.tungsten_tool;
import cc.tools.web_browser;
import cc.tools.web_fetch;
import cc.tools.web_search;
import cc.tools.workflow;
import cc.tools.worktree;
import cc.utils.json;
import cc.services.image;
import cc.skills.skill;

export namespace cc::tools {

namespace fs = std::filesystem;

namespace detail {

using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::InputSchema;
using cc::core::ITool;
using cc::core::Result;
using cc::core::SchemaProperty;
using cc::core::ToolDefinition;
using cc::core::ToolInput;
using cc::core::ToolOutputContent;
using cc::core::ToolPermission;
using cc::core::ToolRegistry;
using cc::core::ToolResult;

using RuntimeExecutor = std::function<Result<ToolResult>(const ToolInput&)>;

class RuntimeFunctionTool final : public ITool {
public:
    RuntimeFunctionTool(ToolDefinition definition, RuntimeExecutor executor)
        : definition_(std::move(definition)), executor_(std::move(executor)) {}

    [[nodiscard]] const ToolDefinition& definition() const override {
        return definition_;
    }

    [[nodiscard]] Result<ToolResult> execute(const ToolInput& input) override {
        return executor_(input);
    }

    [[nodiscard]] bool check_permission(const ToolInput& /*input*/) const override {
        return true;
    }

private:
    ToolDefinition definition_;
    RuntimeExecutor executor_;
};

[[nodiscard]] ToolDefinition define_tool(
    std::string name,
    std::string description,
    ToolPermission permission,
    std::vector<SchemaProperty> properties = {},
    std::string category = "runtime"
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
    std::string category = "runtime"
) {
    return std::make_unique<RuntimeFunctionTool>(
        define_tool(std::move(name), std::move(description), permission, std::move(properties), std::move(category)),
        std::move(executor)
    );
}

[[nodiscard]] std::string unescape_json_string(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    bool escaping = false;
    for (char c : value) {
        if (escaping) {
            switch (c) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case '\\': out.push_back('\\'); break;
                case '"': out.push_back('"'); break;
                default: out.push_back(c); break;
            }
            escaping = false;
        } else if (c == '\\') {
            escaping = true;
        } else {
            out.push_back(c);
        }
    }
    if (escaping) out.push_back('\\');
    return out;
}

[[nodiscard]] std::optional<std::string> json_string(std::string_view json, std::string_view key) {
    auto key_text = std::format("\"{}\"", key);
    auto key_pos = json.find(key_text);
    if (key_pos == std::string_view::npos) return std::nullopt;
    auto colon = json.find(':', key_pos + key_text.size());
    if (colon == std::string_view::npos) return std::nullopt;
    auto pos = colon + 1;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size() || json[pos] != '"') return std::nullopt;
    ++pos;
    std::size_t end = pos;
    bool escaping = false;
    while (end < json.size()) {
        auto c = json[end];
        if (escaping) {
            escaping = false;
        } else if (c == '\\') {
            escaping = true;
        } else if (c == '"') {
            break;
        }
        ++end;
    }
    if (end >= json.size()) return std::nullopt;
    return unescape_json_string(json.substr(pos, end - pos));
}

[[nodiscard]] std::optional<int> json_int(std::string_view json, std::string_view key) {
    auto key_text = std::format("\"{}\"", key);
    auto key_pos = json.find(key_text);
    if (key_pos == std::string_view::npos) return std::nullopt;
    auto colon = json.find(':', key_pos + key_text.size());
    if (colon == std::string_view::npos) return std::nullopt;
    auto pos = colon + 1;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    auto start = pos;
    if (pos < json.size() && json[pos] == '-') ++pos;
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos == start) return std::nullopt;
    try {
        return std::stoi(std::string(json.substr(start, pos - start)));
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] bool json_bool(std::string_view json, std::string_view key, bool fallback = false) {
    auto key_text = std::format("\"{}\"", key);
    auto key_pos = json.find(key_text);
    if (key_pos == std::string_view::npos) return fallback;
    auto colon = json.find(':', key_pos + key_text.size());
    if (colon == std::string_view::npos) return fallback;
    auto pos = colon + 1;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (json.substr(pos, 4) == "true") return true;
    if (json.substr(pos, 5) == "false") return false;
    return fallback;
}

[[nodiscard]] std::optional<std::string> runtime_json_string(cc::utils::json::JsonVal obj, std::string_view key) {
    auto val = obj.get(key);
    if (!val.is_str()) return std::nullopt;
    return std::string(val.as_str());
}

[[nodiscard]] std::optional<int> runtime_json_int(cc::utils::json::JsonVal obj, std::string_view key) {
    auto val = obj.get(key);
    if (!val.is_num()) return std::nullopt;
    return static_cast<int>(val.as_int());
}

[[nodiscard]] std::vector<std::string> json_string_array(std::string_view json, std::string_view key) {
    std::vector<std::string> values;
    auto parsed = cc::utils::json::parse(json);
    if (!parsed || !parsed->root().is_obj()) return values;
    auto node = parsed->root().get(key);
    if (node.is_arr()) {
        node.iter([&](cc::utils::json::JsonVal item) {
            if (item.is_str()) values.emplace_back(item.as_str());
        });
        return values;
    }
    if (node.is_str()) {
        std::string text(node.as_str());
        std::string current;
        for (char ch : text) {
            if (ch == '+' || ch == ',') {
                if (!current.empty()) values.push_back(std::exchange(current, {}));
            } else if (!std::isspace(static_cast<unsigned char>(ch))) {
                current.push_back(ch);
            }
        }
        if (!current.empty()) values.push_back(std::move(current));
    }
    return values;
}

[[nodiscard]] MemberRole parse_team_member_role(std::string_view role) {
    if (role == "leader") return MemberRole::Leader;
    if (role == "reviewer") return MemberRole::Reviewer;
    return MemberRole::Worker;
}

[[nodiscard]] std::vector<TeamMember> parse_team_members(cc::utils::json::JsonVal root) {
    std::vector<TeamMember> members;
    auto value = root.get("members");
    if (!value.valid() || !value.is_arr()) return members;

    value.iter([&](cc::utils::json::JsonVal item) {
        TeamMember member;
        if (item.is_str()) {
            member.agent_id = std::string(item.as_str());
        } else if (item.is_obj()) {
            member.agent_id = runtime_json_string(item, "agent_id")
                .or_else([&] { return runtime_json_string(item, "id"); })
                .or_else([&] { return runtime_json_string(item, "name"); })
                .value_or("");
            member.role = parse_team_member_role(runtime_json_string(item, "role").value_or("worker"));
            member.current_task = runtime_json_string(item, "current_task");
        }
        if (!member.agent_id.empty()) members.push_back(std::move(member));
    });
    return members;
}

[[nodiscard]] std::vector<SharedTaskItem> parse_team_tasks(cc::utils::json::JsonVal root) {
    std::vector<SharedTaskItem> tasks;
    auto value = root.get("task_list");
    if (!value.valid() || !value.is_arr()) {
        value = root.get("tasks");
    }
    if (!value.valid() || !value.is_arr()) return tasks;

    value.iter([&](cc::utils::json::JsonVal item) {
        if (!item.is_obj()) return;
        auto id = runtime_json_string(item, "id");
        auto description = runtime_json_string(item, "description")
            .or_else([&] { return runtime_json_string(item, "task"); });
        if (!id || !description) return;
        tasks.push_back(SharedTaskItem{
            .id = *id,
            .description = *description,
            .assigned_to = runtime_json_string(item, "assigned_to"),
            .completed = false,
            .result = std::nullopt,
        });
    });
    return tasks;
}

[[nodiscard]] std::optional<std::string> json_raw_value(std::string_view json, std::string_view key) {
    auto key_text = std::format("\"{}\"", key);
    auto key_pos = json.find(key_text);
    if (key_pos == std::string_view::npos) return std::nullopt;
    auto colon = json.find(':', key_pos + key_text.size());
    if (colon == std::string_view::npos) return std::nullopt;
    auto pos = colon + 1;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size()) return std::nullopt;

    const auto start = pos;
    if (json[pos] == '{' || json[pos] == '[') {
        const char open = json[pos];
        const char close = open == '{' ? '}' : ']';
        int depth = 0;
        bool in_string = false;
        bool escaping = false;
        while (pos < json.size()) {
            const char c = json[pos];
            if (in_string) {
                if (escaping) escaping = false;
                else if (c == '\\') escaping = true;
                else if (c == '"') in_string = false;
            } else if (c == '"') {
                in_string = true;
            } else if (c == open) {
                ++depth;
            } else if (c == close) {
                if (--depth == 0) {
                    return std::string(json.substr(start, pos - start + 1));
                }
            }
            ++pos;
        }
    return std::nullopt;
}

    if (json[pos] == '"') {
        ++pos;
        bool escaping = false;
        while (pos < json.size()) {
            const char c = json[pos];
            if (escaping) escaping = false;
            else if (c == '\\') escaping = true;
            else if (c == '"') return std::string(json.substr(start, pos - start + 1));
            ++pos;
        }
        return std::nullopt;
    }

    while (pos < json.size() && json[pos] != ',' && json[pos] != '}') ++pos;
    while (pos > start && std::isspace(static_cast<unsigned char>(json[pos - 1]))) --pos;
    return std::string(json.substr(start, pos - start));
}

[[nodiscard]] std::optional<std::size_t> parse_notebook_cell_index(std::string_view text) {
    if (text.starts_with("cell-")) {
        text.remove_prefix(5);
    }
    if (text.empty()) return std::nullopt;
    std::size_t value = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) return std::nullopt;
    return value;
}

[[nodiscard]] std::optional<std::size_t> resolve_notebook_cell_index(
    const Notebook& notebook,
    std::optional<std::string> cell_id
) {
    if (!cell_id || cell_id->empty()) return std::nullopt;
    for (std::size_t i = 0; i < notebook.cells.size(); ++i) {
        if (notebook.cells[i].id && *notebook.cells[i].id == *cell_id) {
            return i;
        }
    }
    return parse_notebook_cell_index(*cell_id);
}

[[nodiscard]] std::optional<CellOperation> parse_notebook_operation(std::string_view text) {
    if (text == "insert") return CellOperation::Insert;
    if (text == "delete") return CellOperation::Delete;
    if (text == "update" || text == "replace") return CellOperation::Update;
    if (text == "move") return CellOperation::Move;
    return std::nullopt;
}

[[nodiscard]] std::string runtime_shell_quote(std::string_view value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

[[nodiscard]] Result<ToolResult> run_command(std::string command, std::size_t max_bytes = 1024 * 512) {
    FILE* pipe = ::popen(command.c_str(), "r");
    if (!pipe) {
        return ToolResult::error("Failed to start command");
    }

    std::string output;
    std::array<char, 4096> buffer{};
    while (auto bytes = ::fread(buffer.data(), 1, buffer.size(), pipe)) {
        output.append(buffer.data(), bytes);
        if (output.size() > max_bytes) {
            output += "\n[output truncated]\n";
            break;
        }
    }
    auto status = ::pclose(pipe);
    if (output.empty()) output = std::format("Command exited with status {}", status);
    if (status != 0) {
        return ToolResult::error(std::format("Command failed with status {}:\n{}", status, output));
    }
    return ToolResult::success(output);
}

[[nodiscard]] std::string join_args(const std::vector<std::string>& args) {
    std::string out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i > 0) out += " ";
        out += args[i];
    }
    return out;
}

[[nodiscard]] std::vector<std::string> read_lines(const fs::path& file) {
    std::ifstream input(file);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) lines.push_back(line);
    return lines;
}

[[nodiscard]] std::string word_at_position(const std::vector<std::string>& lines, int line_no, int character) {
    if (line_no < 0 || static_cast<std::size_t>(line_no) >= lines.size()) return {};
    const auto& line = lines[static_cast<std::size_t>(line_no)];
    auto pos = std::clamp(character, 0, static_cast<int>(line.size()));
    auto is_word = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };
    int begin = pos;
    while (begin > 0 && is_word(line[static_cast<std::size_t>(begin - 1)])) --begin;
    int end = pos;
    while (end < static_cast<int>(line.size()) && is_word(line[static_cast<std::size_t>(end)])) ++end;
    if (end <= begin) return {};
    return line.substr(static_cast<std::size_t>(begin), static_cast<std::size_t>(end - begin));
}

[[nodiscard]] bool is_source_file(const fs::path& path) {
    auto ext = path.extension().string();
    static const std::vector<std::string> exts = {
        ".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hh",
        ".m", ".mm", ".ts", ".tsx", ".js", ".jsx", ".py",
        ".rs", ".go", ".java", ".kt", ".swift", ".cppm"
    };
    return std::ranges::find(exts, ext) != exts.end();
}

[[nodiscard]] LspAction parse_lsp_action(std::string_view action) {
    if (action == "diagnostics") return LspAction::Diagnostics;
    if (action == "definition") return LspAction::Definition;
    if (action == "references") return LspAction::References;
    if (action == "completion") return LspAction::Completion;
    if (action == "hover") return LspAction::Hover;
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
    auto output = result->stdout_output;
    if (!result->stderr_output.empty()) output += "\n" + result->stderr_output;
    if (output.empty()) output = std::format("Script exited with code {}", result->exit_code);
    if (result->timed_out) output += "\n[timed out]";
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
        std::string out = "Tasks:\n";
        for (const auto* task : listed) out += "- " + format_task_summary(*task) + "\n";
        if (listed.empty()) out += "No tasks.\n";
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
        TaskStopTool tool;
        auto result = tool.execute(*id);
        if (!result) return ToolResult::error(std::string(format_error(result.error())));
        return ToolResult::success(std::format("Stopped task {}", *id));
    }
    if (tool_name == "task_update") {
        TaskUpdateTool tool;
        auto status = parse_task_status(json_string(json, "status").value_or("running"));
        auto result_text = json_string(json, "result").or_else([&] { return json_string(json, "output"); });
        auto result = tool.execute(*id, status, result_text);
        if (!result) return ToolResult::error(std::string(format_error(result.error())));
        return ToolResult::success(std::format("Updated task {} [{}]", *id, task_status_name(status)));
    }
    if (tool_name == "task_output") {
        if (auto background = bash::get_background_task_snapshot(*id)) {
            return ToolResult::success(format_background_task_summary(*background, true));
        }
        TaskOutputTool tool;
        auto output = tool.execute(*id);
        if (!output) return ToolResult::error(std::string(format_error(output.error())));
        return ToolResult::success(output->empty() ? "(no output)" : std::string(*output));
    }
    return ToolResult::error(std::format("Unknown task tool: {}", tool_name));
}

[[nodiscard]] fs::path config_path() {
    if (const char* home = std::getenv("HOME")) return fs::path{home} / ".cc-repl" / "config.json";
    return fs::path{".cc-repl"} / "config.json";
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

[[nodiscard]] std::vector<std::string> runtime_tool_names_impl() {
    return {
        "Agent",
        "Bash",
        "computer_use",
        "Edit",
        "Glob",
        "Grep",
        "Read",
        "web_browser",
        "WebFetch",
        "WebSearch",
        "Write",
        "ask_user_question",
        "brief",
        "config",
        "enter_plan_mode",
        "enter_worktree",
        "exit_plan_mode",
        "exit_worktree",
        "list_mcp_resources",
        "lsp",
        "mcp",
        "mcp_auth",
        "notebook_edit",
        "powershell",
        "read_mcp_resource",
        "remote_trigger",
        "repl",
        "schedule_cron",
        "script",
        "send_message",
        "shared",
        "skill",
        "sleep",
        "synthetic_output",
        "task_create",
        "task_get",
        "task_list",
        "task_output",
        "task_stop",
        "task_update",
        "team_create",
        "team_delete",
        "testing",
        "todo_write",
        "tool_search",
        "tungsten",
        "workflow",
    };
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

[[nodiscard]] std::optional<cc::tools::BrowserAction> parse_browser_action(std::string_view action) {
    using cc::tools::BrowserAction;
    if (action == "navigate") return BrowserAction::Navigate;
    if (action == "click") return BrowserAction::Click;
    if (action == "extract") return BrowserAction::Extract;
    if (action == "screenshot") return BrowserAction::Screenshot;
    if (action == "fill_form") return BrowserAction::FillForm;
    if (action == "get_title") return BrowserAction::GetTitle;
    return std::nullopt;
}

[[nodiscard]] std::vector<cc::tools::FormField> json_form_fields(std::string_view json) {
    std::vector<cc::tools::FormField> fields;
    auto parsed = cc::utils::json::parse(json);
    if (!parsed || !parsed->root().is_obj()) return fields;

    auto node = parsed->root().get("form_fields");
    if (!node.valid() || !node.is_arr()) {
        node = parsed->root().get("fields");
    }
    if (!node.valid() || !node.is_arr()) return fields;

    node.iter([&](cc::utils::json::JsonVal item) {
        if (!item.is_obj()) return;
        auto selector = runtime_json_string(item, "selector");
        auto value = runtime_json_string(item, "value");
        if (!selector || selector->empty() || !value) return;
        fields.push_back(cc::tools::FormField{
            .selector = std::move(*selector),
            .value = std::move(*value),
        });
    });
    return fields;
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

[[nodiscard]] std::optional<cc::core::computer_use::ActionType> parse_computer_action(
    std::string_view action) {
    using cc::core::computer_use::ActionType;
    if (action == "screenshot") return ActionType::Screenshot;
    if (action == "move" || action == "mouse_move") return ActionType::MouseMove;
    if (action == "click" || action == "mouse_click") return ActionType::MouseClick;
    if (action == "double_click") return ActionType::MouseDoubleClick;
    if (action == "right_click") return ActionType::MouseRightClick;
    if (action == "drag") return ActionType::MouseDrag;
    if (action == "type") return ActionType::KeyType;
    if (action == "press") return ActionType::KeyPress;
    if (action == "hotkey") return ActionType::KeyHotkey;
    if (action == "scroll") return ActionType::Scroll;
    return std::nullopt;
}

inline std::optional<cc::core::computer_use::CaptureProvider> computer_use_capture_provider_override;
inline std::optional<cc::core::computer_use::InputProvider> computer_use_input_provider_override;

} // namespace detail

inline void set_runtime_computer_use_capture_provider_for_testing(
    cc::core::computer_use::CaptureProvider provider) {
    detail::computer_use_capture_provider_override = std::move(provider);
}

inline void clear_runtime_computer_use_capture_provider_for_testing() {
    detail::computer_use_capture_provider_override.reset();
}

inline void set_runtime_computer_use_input_provider_for_testing(
    cc::core::computer_use::InputProvider provider) {
    detail::computer_use_input_provider_override = std::move(provider);
}

inline void clear_runtime_computer_use_input_provider_for_testing() {
    detail::computer_use_input_provider_override.reset();
}

namespace detail {

[[nodiscard]] Result<ToolResult> execute_computer_use(const ToolInput& input) {
    auto json = input.json();
    auto action_text = json_string(json, "action").value_or("screenshot");
    auto action = parse_computer_action(action_text);
    if (!action) {
        return ToolResult::error(std::format("Unsupported computer-use action: {}", action_text));
    }

    auto point_from_xy = [&] -> std::optional<cc::core::computer_use::Point> {
        auto x = json_int(json, "x");
        auto y = json_int(json, "y");
        if (!x || !y) return std::nullopt;
        return cc::core::computer_use::Point{.x = *x, .y = *y};
    };

    cc::core::computer_use::ComputerAction request{
        .type = *action,
        .position = point_from_xy(),
        .drag_end = std::nullopt,
        .text = json_string(json, "text").or_else([&] { return json_string(json, "key"); }),
        .region = std::nullopt,
        .keys = json_string_array(json, "keys"),
    };
    if (request.keys.empty() && *action == cc::core::computer_use::ActionType::KeyHotkey) {
        request.keys = json_string_array(json, "key");
        if (request.keys.empty()) request.keys = json_string_array(json, "text");
    }

    if (auto end_x = json_int(json, "to_x"), end_y = json_int(json, "to_y"); end_x && end_y) {
        request.drag_end = cc::core::computer_use::Point{.x = *end_x, .y = *end_y};
    }
    if (auto w = json_int(json, "width"), h = json_int(json, "height"); w && h && *w > 0 && *h > 0) {
        request.region = cc::core::computer_use::Rect{
            .x = json_int(json, "x").value_or(0),
            .y = json_int(json, "y").value_or(0),
            .width = static_cast<std::uint32_t>(*w),
            .height = static_cast<std::uint32_t>(*h),
        };
    }

    cc::core::computer_use::ComputerUseManager manager{
        computer_use_capture_provider_override
            ? cc::core::computer_use::ScreenCapture{*computer_use_capture_provider_override}
            : cc::core::computer_use::ScreenCapture{},
        computer_use_input_provider_override
            ? *computer_use_input_provider_override
            : cc::core::computer_use::make_native_input_provider()
    };
    auto result = manager.execute_action(request);
    if (!result.success) {
        return ToolResult::error(result.error_message);
    }
    if (result.screenshot) {
        auto data = cc::services::image::ImageService::to_base64(
            std::span<const std::uint8_t>(result.screenshot->pixels.data(), result.screenshot->pixels.size()));
        auto media_type = result.screenshot->format == "png" ? "image/png" : "image/rgba";
        return ToolResult::success_multi({
            ToolOutputContent::text_output(std::format(
                "Captured screenshot {}x{}.",
                result.screenshot->width,
                result.screenshot->height)),
            ToolOutputContent::image_output(media_type, std::move(data)),
        });
    }
    return ToolResult::success(std::format("Computer-use action completed: {}", action_text));
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
    auto brief_path = fs::current_path() / ".cc-repl" / "brief.md";
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

[[nodiscard]] Result<ToolResult> execute_simple_runtime_tool(std::string_view name, const ToolInput& input) {
    auto json = input.json();
    if (name == "ask_user_question") {
        auto question = json_string(json, "question").value_or("Continue?");
        std::cout << "\n" << question << "\n> ";
        std::string answer;
        if (!std::getline(std::cin, answer)) return ToolResult::error("No interactive input available");
        return ToolResult::success(answer);
    }
    if (name == "brief") return execute_brief(input);
    if (name == "computer_use") return execute_computer_use(input);
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
        return ToolResult::success(result->content);
    }
    if (name == "mcp_auth") {
        auto server = json_string(json, "server_name").or_else([&] { return json_string(json, "server"); });
        if (!server) return ToolResult::error("mcp_auth requires server_name");
        auto code = json_string(json, "auth_code").or_else([&] { return json_string(json, "code"); });
        McpAuthTool tool;
        auto result = tool.execute(*server, code);
        if (!result) return ToolResult::error(std::string(format_error(result.error())));
        return ToolResult::success(*result);
    }
    if (name == "notebook_edit") return execute_notebook_edit(input);
    if (name == "powershell") {
#ifdef _WIN32
        auto command = json_string(json, "command");
        if (!command) return ToolResult::error("powershell requires command");
        return run_command("powershell -NoProfile -Command " + runtime_shell_quote(*command) + " 2>&1");
#else
        return ToolResult::error("PowerShell execution is only available on Windows in this runtime");
#endif
    }
    if (name == "remote_trigger") {
        auto target = json_string(json, "target").or_else([&] { return json_string(json, "url"); });
        auto message = json_string(json, "message").or_else([&] { return json_string(json, "payload"); });
        if (target && message) {
            std::map<std::string, std::string> params;
            if (auto parsed = cc::utils::json::parse(json); parsed && parsed->root().is_obj()) {
                auto params_node = parsed->root().get("params");
                if (params_node.is_obj()) {
                    params_node.iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal value) {
                        if (!key.is_str() || !value.is_str()) return;
                        params.emplace(key.as_str(), value.as_str());
                    });
                }
            }
            auto delivered = execute_remote_trigger(RemoteTriggerInput{
                .target = *target,
                .message = *message,
                .params = std::move(params),
            });
            if (!delivered) return ToolResult::error(delivered.error());
            return ToolResult::success(*delivered);
        }

        const char* command = std::getenv("CC_REPL_REMOTE_TRIGGER_COMMAND");
        if (!command) return ToolResult::error("remote_trigger requires target and message");
        auto payload = json_string(json, "payload").value_or(std::string(json));
        return run_command(std::format("{} {}", command, runtime_shell_quote(payload)));
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
        auto recipient = json_string(json, "target_agent")
            .or_else([&] { return json_string(json, "target"); })
            .or_else([&] { return json_string(json, "recipient"); })
            .or_else([&] { return json_string(json, "to"); });
        auto message = json_string(json, "content").or_else([&] { return json_string(json, "message"); });
        if (!recipient || recipient->empty()) return ToolResult::error("send_message requires target_agent");
        if (!message || message->empty()) return ToolResult::error("send_message requires content");

        MessagePriority priority = MessagePriority::Normal;
        auto priority_text = json_string(json, "priority").value_or("normal");
        if (priority_text == "low") priority = MessagePriority::Low;
        else if (priority_text == "high") priority = MessagePriority::High;
        else if (priority_text == "urgent") priority = MessagePriority::Urgent;

        SendMessageTool tool(json_string(json, "from_agent")
            .or_else([&] { return json_string(json, "from"); })
            .value_or("main"));
        auto sent = tool.execute(*recipient, *message, priority, json_string(json, "reply_to"));
        if (!sent) return ToolResult::error(std::string(format_error(sent.error())));
        return ToolResult::success(std::format(
            "Delivered message {} to {} [{}]",
            sent->message_id,
            *recipient,
            delivery_status_name(sent->status)));
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
    if (name == "skill") return execute_skill_tool(input);
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
    if (name == "team_create") {
        auto parsed = cc::utils::json::parse(json);
        if (!parsed || !parsed->root().is_obj()) {
            return ToolResult::error("team_create input must be a JSON object");
        }
        auto root = parsed->root();
        TeamCreateTool tool;
        auto team = runtime_json_string(root, "team_name").or_else([&] { return runtime_json_string(root, "name"); })
            .value_or(std::format("team-{}", std::chrono::steady_clock::now().time_since_epoch().count()));
        auto id = runtime_json_string(root, "team_id").or_else([&] { return runtime_json_string(root, "id"); }).value_or(team);
        auto members = parse_team_members(root);
        auto tasks = parse_team_tasks(root);
        auto result = tool.execute(id, team, members);
        if (!result) return ToolResult::error(std::string(format_error(result.error())));
        for (const auto& member : (*result)->members) {
            MessageRouter::instance().register_agent(member.agent_id);
            cc::tools::agent_runtime::native_agent_store().upsert(cc::tools::agent_runtime::NativeAgentRecord{
                .agent_id = member.agent_id,
                .agent_type = std::string(member_role_name(member.role)),
                .team_name = (*result)->name,
                .background = true,
                .status = cc::tools::agent_runtime::NativeAgentStatus::Queued,
            });
        }
        for (auto& task : tasks) {
            auto task_id = task.id;
            auto assigned_to = task.assigned_to;
            auto added = global_team_store().add_task((*result)->id, std::move(task));
            if (!added) return ToolResult::error(std::string(format_error(added.error())));
            if (assigned_to && !assigned_to->empty()) {
                auto assigned = global_team_store().assign_task((*result)->id, task_id, *assigned_to);
                if (!assigned) return ToolResult::error(std::string(format_error(assigned.error())));
                cc::tools::agent_runtime::native_agent_store().mark_running(*assigned_to);
            }
        }
        return ToolResult::success(std::format(
            "Created team {} ({}) with {} members and {} tasks",
            (*result)->name,
            (*result)->id,
            (*result)->members.size(),
            tasks.size()));
    }
    if (name == "team_delete") {
        auto team = json_string(json, "team_id").or_else([&] { return json_string(json, "id"); })
            .or_else([&] { return json_string(json, "team_name"); }).or_else([&] { return json_string(json, "name"); });
        if (!team) return ToolResult::error("team_delete requires team_id");
        TeamDeleteTool tool;
        auto result = tool.execute(*team);
        if (!result) return ToolResult::error(std::string(format_error(result.error())));
        return ToolResult::success(std::format("Deleted team {}", *team));
    }
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
    return ToolResult::error(std::format("Runtime tool '{}' has no runtime handler", name));
}

} // namespace detail

using cc::core::SchemaProperty;
using cc::core::ToolPermission;

[[nodiscard]] std::vector<std::string> runtime_tool_names() {
    return detail::runtime_tool_names_impl();
}

void register_runtime_tools(cc::core::ToolRegistry& registry) {
    registry.register_tool(make_agent_tool(AgentConfig{}, 0, &registry));
    registry.register_tool(make_bash_tool());
    registry.register_tool(make_file_edit_tool());
    registry.register_tool(make_file_read_tool());
    registry.register_tool(make_file_write_tool());
    registry.register_tool(make_glob_tool());
    registry.register_tool(make_grep_tool());
    registry.register_tool(make_todo_write_tool());
    registry.register_tool(make_web_fetch_tool());
    registry.register_tool(make_web_search_tool());

    const auto simple = [](std::string name, std::string description, ToolPermission permission,
                           std::vector<SchemaProperty> properties = {}, std::string category = "runtime") {
        auto name_copy = name;
        return detail::make_runtime_tool(std::move(name), std::move(description), permission, std::move(properties),
            [name_copy](const cc::core::ToolInput& input) {
                return detail::execute_simple_runtime_tool(name_copy, input);
            },
            std::move(category));
    };

    registry.register_tool(simple("ask_user_question", "Ask the interactive user a question and return the answer",
        ToolPermission::ReadOnly, {SchemaProperty{.name = "question", .type = "string", .description = "Question to ask", .required = true}}, "interaction"));
    registry.register_tool(simple("computer_use", "Control the local computer through screenshots, mouse, keyboard, and scroll actions",
        ToolPermission::Execute, {
            SchemaProperty{.name = "action", .type = "string", .description = "screenshot, move, click, double_click, right_click, drag, type, press, hotkey, or scroll", .required = true},
            SchemaProperty{.name = "x", .type = "number", .description = "X coordinate, region origin, or scroll delta x", .required = false},
            SchemaProperty{.name = "y", .type = "number", .description = "Y coordinate, region origin, or scroll delta y", .required = false},
            SchemaProperty{.name = "to_x", .type = "number", .description = "Drag target X coordinate", .required = false},
            SchemaProperty{.name = "to_y", .type = "number", .description = "Drag target Y coordinate", .required = false},
            SchemaProperty{.name = "width", .type = "number", .description = "Screenshot region width", .required = false},
            SchemaProperty{.name = "height", .type = "number", .description = "Screenshot region height", .required = false},
            SchemaProperty{.name = "text", .type = "string", .description = "Text to type or key to press", .required = false},
            SchemaProperty{.name = "key", .type = "string", .description = "Key name for press actions", .required = false},
        }, "computer_use"));
    registry.register_tool(simple("brief", "Read or write the workspace brief",
        ToolPermission::Write, {SchemaProperty{.name = "content", .type = "string", .description = "Brief content to save", .required = false}}, "context"));
    registry.register_tool(simple("config", "Read or update CC-REPL configuration",
        ToolPermission::Write, {SchemaProperty{.name = "action", .type = "string", .description = "get or set", .required = false}}, "config"));
    registry.register_tool(simple("enter_plan_mode", "Enter plan mode",
        ToolPermission::Write, {}, "planning"));
    registry.register_tool(simple("exit_plan_mode", "Exit plan mode",
        ToolPermission::Write, {}, "planning"));
    registry.register_tool(simple("enter_worktree", "Create and enter a git worktree",
        ToolPermission::Execute, {SchemaProperty{.name = "branch", .type = "string", .description = "Branch name", .required = true}}, "git"));
    registry.register_tool(simple("exit_worktree", "Remove a git worktree",
        ToolPermission::Execute, {SchemaProperty{.name = "path", .type = "string", .description = "Worktree path", .required = false}}, "git"));
    registry.register_tool(simple("lsp", "Fallback language intelligence for definitions, references, symbols, hover, and diagnostics",
        ToolPermission::ReadOnly, {SchemaProperty{.name = "file_path", .type = "string", .description = "File path", .required = true}}, "code"));
    registry.register_tool(simple("mcp", "Invoke a tool exposed by an MCP server",
        ToolPermission::Network, {SchemaProperty{.name = "server_name", .type = "string", .description = "MCP server name", .required = true}}, "mcp"));
    registry.register_tool(simple("list_mcp_resources", "List local MCP-style resources",
        ToolPermission::ReadOnly, {}, "mcp"));
    registry.register_tool(simple("read_mcp_resource", "Read a local MCP-style resource",
        ToolPermission::ReadOnly, {SchemaProperty{.name = "uri", .type = "string", .description = "Resource URI", .required = true}}, "mcp"));
    registry.register_tool(simple("mcp_auth", "Check MCP authentication token availability",
        ToolPermission::ReadOnly, {SchemaProperty{.name = "server_name", .type = "string", .description = "MCP server name", .required = true}}, "mcp"));
    registry.register_tool(simple("notebook_edit", "Edit Jupyter notebook cells by id or index",
        ToolPermission::Write, {
            SchemaProperty{.name = "notebook_path", .type = "string", .description = "Notebook path", .required = true},
            SchemaProperty{.name = "cell_id", .type = "string", .description = "Notebook cell id or cell-N index", .required = false},
            SchemaProperty{.name = "cell_index", .type = "integer", .description = "Notebook cell index", .required = false},
            SchemaProperty{.name = "new_source", .type = "string", .description = "Replacement or inserted source", .required = false},
            SchemaProperty{.name = "cell_type", .type = "string", .description = "code, markdown, or raw", .required = false},
            SchemaProperty{.name = "edit_mode", .type = "string", .description = "replace, insert, or delete", .required = false},
        }, "filesystem"));
    registry.register_tool(simple("powershell", "Execute a PowerShell command on Windows",
        ToolPermission::Execute, {SchemaProperty{.name = "command", .type = "string", .description = "Command", .required = true}}, "shell"));
    registry.register_tool(simple("remote_trigger", "Invoke a configured remote trigger command",
        ToolPermission::Execute, {SchemaProperty{.name = "payload", .type = "string", .description = "Trigger payload", .required = false}}, "remote"));
    registry.register_tool(simple("repl", "Run a one-shot REPL snippet",
        ToolPermission::Execute, {SchemaProperty{.name = "code", .type = "string", .description = "Code to execute", .required = true}}, "execution"));
    registry.register_tool(simple("schedule_cron", "Schedule a cron-style reminder for this process",
        ToolPermission::Write, {SchemaProperty{.name = "message", .type = "string", .description = "Scheduled message", .required = true}}, "tasks"));
    registry.register_tool(simple("script", "Execute a bounded script",
        ToolPermission::Execute, {SchemaProperty{.name = "code", .type = "string", .description = "Script code", .required = true}}, "execution"));
    registry.register_tool(simple("send_message", "Queue a message for an agent or team",
        ToolPermission::Write, {SchemaProperty{.name = "message", .type = "string", .description = "Message body", .required = true}}, "agents"));
    registry.register_tool(simple("shared", "Read or write shared runtime key-value state",
        ToolPermission::Write, {SchemaProperty{.name = "key", .type = "string", .description = "Shared key", .required = false}}, "agents"));
    registry.register_tool(simple("skill", "Load a local skill by name",
        ToolPermission::ReadOnly, {SchemaProperty{.name = "name", .type = "string", .description = "Skill name", .required = true}}, "skills"));
    registry.register_tool(simple("sleep", "Sleep for a bounded number of seconds",
        ToolPermission::Execute, {SchemaProperty{.name = "duration", .type = "number", .description = "Duration in seconds", .required = true}}, "execution"));
    registry.register_tool(simple("synthetic_output", "Return provided synthetic output content",
        ToolPermission::ReadOnly, {SchemaProperty{.name = "content", .type = "string", .description = "Content", .required = true}}, "testing"));

    for (const auto& name : {"task_create", "task_get", "task_list", "task_output", "task_stop", "task_update"}) {
        registry.register_tool(simple(name, std::format("Runtime task operation {}", name), ToolPermission::Write,
            {
                SchemaProperty{.name = "task_id", .type = "string", .description = "Task ID", .required = false},
                SchemaProperty{.name = "pid", .type = "number", .description = "Background process PID", .required = false},
            }, "tasks"));
    }
    registry.register_tool(simple("team_create", "Create a runtime team record", ToolPermission::Write,
        {SchemaProperty{.name = "team_name", .type = "string", .description = "Team name", .required = false}}, "agents"));
    registry.register_tool(simple("team_delete", "Delete a runtime team record", ToolPermission::Write,
        {SchemaProperty{.name = "team_name", .type = "string", .description = "Team name", .required = true}}, "agents"));
    registry.register_tool(simple("testing", "Run a test command", ToolPermission::Execute,
        {SchemaProperty{.name = "command", .type = "string", .description = "Test command", .required = false}}, "testing"));
    registry.register_tool(simple("tool_search", "Search registered runtime tools", ToolPermission::ReadOnly,
        {SchemaProperty{.name = "query", .type = "string", .description = "Search query", .required = false}}, "tools"));
    registry.register_tool(simple("tungsten", "Use the Tungsten integration when configured", ToolPermission::Network, {}, "integrations"));
    registry.register_tool(simple("web_browser", "Automate browser navigation, extraction, form fill, and screenshots",
        ToolPermission::Network, {SchemaProperty{.name = "action", .type = "string", .description = "Browser action", .required = true}}, "browser"));
    registry.register_tool(simple("workflow", "Read and execute workflow definitions", ToolPermission::ReadOnly,
        {SchemaProperty{.name = "file", .type = "string", .description = "Workflow file", .required = true}}, "workflow"));
}

} // namespace cc::tools
