/// @file runtime_registry.cppm
/// @brief Runtime registration for all migrated tools exposed to the query engine.
module;

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <expected>
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
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

export module cc.tools.runtime_registry;

import cc.tools.tool;
import cc.tools.built_in_agents;
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
import cc.tools.script_types;
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
import cc.utils.http;
import cc.utils.uuid_utils;
import cc.utils.swarm_backends;
import cc.utils.team_helpers;
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

[[nodiscard]] std::optional<bool> runtime_json_bool(cc::utils::json::JsonVal obj, std::string_view key) {
    auto val = obj.get(key);
    if (!val.is_bool()) return std::nullopt;
    return val.as_bool();
}

[[nodiscard]] std::optional<bool> runtime_json_semantic_bool(
    cc::utils::json::JsonVal obj,
    std::string_view key
) {
    auto val = obj.get(key);
    if (val.is_bool()) return val.as_bool();
    if (!val.is_str()) return std::nullopt;
    std::string normalized(val.as_str());
    std::ranges::transform(normalized, normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (normalized == "true" || normalized == "yes" || normalized == "y" ||
        normalized == "1" || normalized == "approve" || normalized == "approved") {
        return true;
    }
    if (normalized == "false" || normalized == "no" || normalized == "n" ||
        normalized == "0" || normalized == "reject" || normalized == "rejected" ||
        normalized == "deny" || normalized == "denied") {
        return false;
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<std::string> runtime_json_event_array(cc::utils::json::JsonVal obj, std::string_view key) {
    std::vector<std::string> values;
    auto node = obj.get(key);
    if (!node.is_arr()) return values;
    node.iter([&](cc::utils::json::JsonVal item) {
        if (item.is_str()) {
            values.emplace_back(item.as_str());
        } else if (item.valid()) {
            values.push_back(cc::utils::json::to_string(item));
        }
    });
    return values;
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

struct TeamMemberStartOptions {
    std::optional<std::string> prompt;
    std::optional<std::string> agent_type;
    std::optional<std::string> mode;
    std::optional<std::string> cwd;
    std::optional<std::string> isolation;
};

[[nodiscard]] std::unordered_map<std::string, TeamMemberStartOptions> parse_team_member_start_options(
    cc::utils::json::JsonVal root
) {
    std::unordered_map<std::string, TeamMemberStartOptions> options;
    auto value = root.get("members");
    if (!value.valid() || !value.is_arr()) return options;

    value.iter([&](cc::utils::json::JsonVal item) {
        if (!item.is_obj()) return;
        auto agent_id = runtime_json_string(item, "agent_id")
            .or_else([&] { return runtime_json_string(item, "id"); })
            .or_else([&] { return runtime_json_string(item, "name"); });
        if (!agent_id || agent_id->empty()) return;
        options.emplace(*agent_id, TeamMemberStartOptions{
            .prompt = runtime_json_string(item, "prompt"),
            .agent_type = runtime_json_string(item, "subagent_type")
                .or_else([&] { return runtime_json_string(item, "agent_type"); }),
            .mode = runtime_json_string(item, "mode")
                .or_else([&] { return runtime_json_string(item, "permission_mode"); }),
            .cwd = runtime_json_string(item, "cwd"),
            .isolation = runtime_json_string(item, "isolation"),
        });
    });
    return options;
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

[[nodiscard]] std::string escape_xml_text(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default: result.push_back(ch); break;
        }
    }
    return result;
}

[[nodiscard]] bool is_native_agent_task(const agent_runtime::NativeAgentRecord& record) {
    return record.background;
}

[[nodiscard]] std::string native_agent_display_name(const agent_runtime::NativeAgentRecord& record) {
    if (record.name && !record.name->empty()) return *record.name;
    return record.agent_id;
}

[[nodiscard]] std::string native_agent_output_file(const agent_runtime::NativeAgentRecord& record) {
    if (record.output_file_path && !record.output_file_path->empty()) return *record.output_file_path;
    if (record.transcript_path && !record.transcript_path->empty()) return *record.transcript_path;
    return agent_runtime::agent_output_file_path(record.agent_id).string();
}

[[nodiscard]] std::string runtime_delivery_message_id() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::format("msg-{}", std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

[[nodiscard]] std::string format_agent_pending_user_message(
    std::string_view from_agent,
    MessagePriority priority,
    std::string_view message
) {
    return std::format(
        "[Message from {} priority={}]\n{}",
        from_agent,
        message_priority_name(priority),
        message);
}

[[nodiscard]] std::string format_team_task_assignment_message(
    std::string_view team_name,
    std::string_view task_id,
    std::string_view description
) {
    return std::format(
        "[Team task {} assigned by {}]\n{}",
        task_id,
        team_name,
        description);
}

[[nodiscard]] bool native_agent_status_is_terminal(agent_runtime::NativeAgentStatus status) {
    return status == agent_runtime::NativeAgentStatus::Completed ||
        status == agent_runtime::NativeAgentStatus::Failed ||
        status == agent_runtime::NativeAgentStatus::Cancelled;
}

[[nodiscard]] std::string safe_runtime_dir_component(std::string_view value, std::string_view fallback) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.') {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? std::string(fallback) : out;
}

[[nodiscard]] std::string ts_sanitized_team_dir_name(std::string_view value, std::string_view fallback) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch)) {
            out.push_back(static_cast<char>(std::tolower(ch)));
        } else {
            out.push_back('-');
        }
    }
    return out.empty() ? std::string(fallback) : out;
}

[[nodiscard]] bool path_has_prefix(const fs::path& path, const fs::path& prefix) {
    auto path_it = path.begin();
    auto prefix_it = prefix.begin();
    for (; prefix_it != prefix.end(); ++prefix_it, ++path_it) {
        if (path_it == path.end() || *path_it != *prefix_it) return false;
    }
    return true;
}

[[nodiscard]] fs::path normalized_absolute_path(const fs::path& path) {
    std::error_code ec;
    auto absolute = fs::absolute(path, ec);
    if (ec) absolute = path;
    return absolute.lexically_normal();
}

[[nodiscard]] bool path_is_agent_runtime_artifact(const fs::path& path) {
    if (path.empty()) return false;
    const auto root = normalized_absolute_path(agent_runtime::runtime_state_dir());
    const auto candidate = normalized_absolute_path(path);
    return path_has_prefix(candidate, root);
}

inline void add_unique_artifact_path(std::vector<fs::path>& paths, fs::path path) {
    if (path.empty()) return;
    path = normalized_absolute_path(path);
    if (std::ranges::find(paths, path) == paths.end()) {
        paths.push_back(std::move(path));
    }
}

[[nodiscard]] std::size_t cleanup_native_agent_transcript_artifacts(
    const agent_runtime::NativeAgentRecord& record
) {
    std::vector<fs::path> paths;
    if (record.output_file_path) add_unique_artifact_path(paths, fs::path{*record.output_file_path});
    if (record.transcript_path) add_unique_artifact_path(paths, fs::path{*record.transcript_path});
    if (record.sidechain_jsonl_path) add_unique_artifact_path(paths, fs::path{*record.sidechain_jsonl_path});
    add_unique_artifact_path(paths, agent_runtime::agent_output_file_path(record.agent_id));
    add_unique_artifact_path(paths, agent_runtime::agent_transcript_path(record.agent_id));
    add_unique_artifact_path(paths, agent_runtime::agent_sidechain_jsonl_path(record.agent_id));

    std::size_t removed_count = 0;
    for (const auto& path : paths) {
        if (!path_is_agent_runtime_artifact(path)) continue;
        std::error_code ec;
        if (!fs::exists(path, ec) && !fs::is_symlink(path, ec)) continue;
        ec.clear();
        const auto removed = fs::remove_all(path, ec);
        if (!ec && removed > 0) ++removed_count;
    }
    return removed_count;
}

struct TeamDeletionCleanupSummary {
    std::size_t native_agents_seen = 0;
    std::size_t cancelled_agents = 0;
    std::size_t teammate_terminations = 0;
    std::size_t teammate_kills = 0;
    std::size_t background_shell_tasks_stopped = 0;
    std::size_t transcript_artifacts_removed = 0;
    std::size_t worktree_cleanup_attempts = 0;
    std::size_t worktrees_removed = 0;
    std::size_t worktrees_retained = 0;
    std::size_t team_dirs_removed = 0;
};

struct TeamCreationArtifactsSummary {
    fs::path team_dir;
    fs::path team_file_path;
    std::size_t inboxes_initialized = 0;
    bool team_config_written = false;
    bool task_list_written = false;
};

[[nodiscard]] std::string team_member_inbox_name(std::string_view agent_id) {
    auto name = std::string(agent_id);
    if (auto at = name.find('@'); at != std::string::npos) {
        name = name.substr(0, at);
    }
    return safe_runtime_dir_component(name, "agent");
}

inline bool write_empty_inbox_if_missing(const fs::path& inbox_path) {
    std::error_code ec;
    fs::create_directories(inbox_path.parent_path(), ec);
    if (ec) return false;
    if (fs::exists(inbox_path, ec)) return true;
    std::ofstream out(inbox_path, std::ios::trunc);
    if (!out) return false;
    out << "[]";
    return out.good();
}

inline bool write_team_task_snapshot(
    const fs::path& task_path,
    std::span<const SharedTaskItem> tasks
) {
    std::error_code ec;
    fs::create_directories(task_path.parent_path(), ec);
    if (ec) return false;

    std::ofstream out(task_path, std::ios::trunc);
    if (!out) return false;
    out << '[';
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        const auto& task = tasks[i];
        if (i != 0) out << ',';
        out << R"({"id":")" << team_json_escape(task.id)
            << R"(","description":")" << team_json_escape(task.description)
            << R"(","completed":)" << (task.completed ? "true" : "false");
        if (task.assigned_to) {
            out << R"(,"assigned_to":")" << team_json_escape(*task.assigned_to) << '"';
        }
        if (task.result) {
            out << R"(,"result":")" << team_json_escape(*task.result) << '"';
        }
        out << '}';
    }
    out << ']';
    return out.good();
}

[[nodiscard]] std::string team_agent_name_from_id(std::string_view agent_id) {
    if (auto at = agent_id.find('@'); at != std::string_view::npos) {
        return std::string(agent_id.substr(0, at));
    }
    return std::string(agent_id);
}

[[nodiscard]] std::string team_lead_agent_id(std::string_view team_name) {
    return std::format("team-lead@{}", team_name);
}

struct TeamConfigMemberRuntimeState {
    std::optional<std::string> agent_type;
    std::optional<std::string> cwd;
    std::optional<std::string> worktree_path;
    std::optional<std::string> backend_type;
    std::optional<std::string> pane_id;
    std::optional<std::string> color;
    std::optional<std::string> mode;
    std::optional<bool> is_active;
};

inline void write_team_config_optional_string(
    std::ostream& out,
    std::string_view key,
    const std::optional<std::string>& value
) {
    if (!value || value->empty()) return;
    out << R"(,")" << team_json_escape(key) << R"(":")" << team_json_escape(*value) << '"';
}

inline void write_team_config_optional_bool(
    std::ostream& out,
    std::string_view key,
    std::optional<bool> value
) {
    if (!value) return;
    out << R"(,")" << team_json_escape(key) << R"(":)" << (*value ? "true" : "false");
}

inline void write_team_config_member(
    std::ostream& out,
    std::string_view agent_id,
    std::string_view role,
    std::string_view cwd,
    std::int64_t timestamp_ms,
    const TeamConfigMemberRuntimeState* state = nullptr
) {
    const auto agent_type = state && state->agent_type && !state->agent_type->empty()
        ? std::string_view{*state->agent_type}
        : role;
    const auto member_cwd = state && state->cwd && !state->cwd->empty()
        ? std::string_view{*state->cwd}
        : cwd;
    const auto pane_id = state && state->pane_id && !state->pane_id->empty()
        ? std::string_view{*state->pane_id}
        : std::string_view{""};
    out << R"({"agentId":")" << team_json_escape(agent_id)
        << R"(","name":")" << team_json_escape(team_agent_name_from_id(agent_id))
        << R"(","agentType":")" << team_json_escape(agent_type)
        << R"(","model":"")"
        << R"(,"joinedAt":)" << timestamp_ms
        << R"(,"tmuxPaneId":")" << team_json_escape(pane_id)
        << R"(","cwd":")" << team_json_escape(member_cwd)
        << R"(","subscriptions":[])";
    if (state) {
        write_team_config_optional_string(out, "color", state->color);
        write_team_config_optional_string(out, "worktreePath", state->worktree_path);
        write_team_config_optional_string(out, "backendType", state->backend_type);
        write_team_config_optional_string(out, "mode", state->mode);
        write_team_config_optional_bool(out, "isActive", state->is_active);
    }
    out << '}';
}

inline bool write_team_config_file(
    const fs::path& config_path,
    const Team& team,
    const std::unordered_map<std::string, TeamConfigMemberRuntimeState>& member_states = {}
) {
    std::error_code ec;
    fs::create_directories(config_path.parent_path(), ec);
    if (ec) return false;
    std::ofstream out(config_path, std::ios::trunc);
    if (!out) return false;

    const auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto cwd = fs::current_path(ec);
    if (ec) cwd = fs::path{};
    const auto cwd_text = cwd.string();
    const auto lead_id = team_lead_agent_id(team.name);

    out << R"({"name":")" << team_json_escape(team.name)
        << R"(","description":"")"
        << R"(,"createdAt":)" << timestamp_ms
        << R"(,"leadAgentId":")" << team_json_escape(lead_id)
        << R"(","leadSessionId":"")"
        << R"(,"members":[)";
    bool first = true;
    auto write_member = [&](std::string_view agent_id, std::string_view role) {
        if (!first) out << ',';
        first = false;
        const auto state_it = member_states.find(std::string(agent_id));
        const auto* state = state_it == member_states.end() ? nullptr : &state_it->second;
        write_team_config_member(out, agent_id, role, cwd_text, timestamp_ms, state);
    };

    const bool has_explicit_lead = std::ranges::any_of(team.members, [&](const auto& member) {
        return member.agent_id == lead_id;
    });
    if (!has_explicit_lead) write_member(lead_id, "team-lead");
    for (const auto& member : team.members) {
        write_member(member.agent_id, member_role_name(member.role));
    }
    out << "]}";
    return out.good();
}

[[nodiscard]] std::unordered_map<std::string, TeamConfigMemberRuntimeState>
team_config_runtime_states_from_native_records(
    std::span<const agent_runtime::NativeAgentRecord> records
) {
    std::unordered_map<std::string, TeamConfigMemberRuntimeState> states;
    for (const auto& record : records) {
        TeamConfigMemberRuntimeState state{
            .agent_type = record.agent_type,
            .cwd = record.cwd,
            .worktree_path = record.worktree_path,
            .backend_type = record.teammate_backend,
            .pane_id = record.teammate_pane_id,
            .color = record.teammate_color,
            .mode = record.mode,
            .is_active = !native_agent_status_is_terminal(record.status),
        };
        if (!state.backend_type && record.team_name) {
            state.backend_type = "in-process";
        }
        if (!state.pane_id && state.backend_type && *state.backend_type == "in-process") {
            state.pane_id = "in-process";
        }
        states[record.agent_id] = std::move(state);
    }
    return states;
}

[[nodiscard]] std::expected<TeamCreationArtifactsSummary, std::string> ensure_team_runtime_artifacts(
    std::string_view team_name,
    std::span<const TeamMember> members,
    std::span<const SharedTaskItem> tasks,
    const Team& team
) {
    TeamCreationArtifactsSummary summary{
        .team_dir = team_runtime_dir() / ts_sanitized_team_dir_name(team_name, "team"),
        .team_file_path = {},
    };
    summary.team_file_path = summary.team_dir / "config.json";

    std::error_code ec;
    fs::create_directories(summary.team_dir / "inboxes", ec);
    if (ec) return std::unexpected(std::format("failed to create team directory: {}", ec.message()));

    for (const auto& member : members) {
        if (member.agent_id.empty()) continue;
        const auto inbox_name = team_member_inbox_name(member.agent_id);
        if (write_empty_inbox_if_missing(summary.team_dir / "inboxes" / (inbox_name + ".json"))) {
            ++summary.inboxes_initialized;
        }
    }

    summary.task_list_written = write_team_task_snapshot(summary.team_dir / "tasks.json", tasks);
    if (!summary.task_list_written) {
        return std::unexpected("failed to write team task snapshot");
    }
    summary.team_config_written = write_team_config_file(summary.team_file_path, team);
    if (!summary.team_config_written) {
        return std::unexpected("failed to write team config");
    }
    return summary;
}

[[nodiscard]] bool contains_agent_id(
    const std::vector<agent_runtime::NativeAgentRecord>& records,
    std::string_view agent_id
) {
    return std::ranges::any_of(records, [&](const auto& record) {
        return record.agent_id == agent_id;
    });
}

[[nodiscard]] std::vector<agent_runtime::NativeAgentRecord> collect_team_native_agents(
    std::string_view team_id,
    std::string_view team_name,
    std::span<const TeamMember> members
) {
    std::vector<agent_runtime::NativeAgentRecord> records;
    for (const auto& member : members) {
        if (auto record = agent_runtime::native_agent_store().get(member.agent_id)) {
            if (!contains_agent_id(records, record->agent_id)) records.push_back(std::move(*record));
        }
    }
    for (auto record : agent_runtime::native_agent_store().list()) {
        const bool belongs_to_team =
            record.team_name && (*record.team_name == team_name || *record.team_name == team_id);
        if (belongs_to_team && !contains_agent_id(records, record.agent_id)) {
            records.push_back(std::move(record));
        }
    }
    return records;
}

[[nodiscard]] TeamDeletionCleanupSummary cleanup_team_runtime_artifacts(
    std::string_view team_id,
    std::string_view team_name,
    std::span<const agent_runtime::NativeAgentRecord> records
) {
    TeamDeletionCleanupSummary summary{.native_agents_seen = records.size()};
    for (const auto& record : records) {
        if (record.teammate_backend && !record.teammate_backend->empty()) {
            const bool prefer_in_process = *record.teammate_backend == "in-process";
            auto executor = cc::utils::swarm_backends::BackendRegistry::get_teammate_executor(prefer_in_process);
            if (executor->terminate(record.agent_id, "team deleted")) {
                ++summary.teammate_terminations;
            }
            if (executor->kill(record.agent_id)) {
                ++summary.teammate_kills;
            }
        }

        auto stopped_shell_tasks = bash::stop_background_tasks_for_agent(record.agent_id);
        summary.background_shell_tasks_stopped += stopped_shell_tasks.size();

        auto cleanup = cc::tools::agent::cleanup_agent_worktree(record.agent_id);
        if (cleanup.attempted) {
            ++summary.worktree_cleanup_attempts;
            if (cleanup.removed) ++summary.worktrees_removed;
            if (cleanup.changed) ++summary.worktrees_retained;
        }

        if (!native_agent_status_is_terminal(record.status)) {
            agent_runtime::native_agent_store().mark_cancelled(
                record.agent_id,
                std::format("team deleted: {}", team_name));
            ++summary.cancelled_agents;
        }

        const auto persisted_record = agent_runtime::native_agent_store().get(record.agent_id).value_or(record);
        summary.transcript_artifacts_removed += cleanup_native_agent_transcript_artifacts(persisted_record);
    }

    std::error_code ec;
    const auto root = team_runtime_dir();
    for (auto component : {
        ts_sanitized_team_dir_name(team_name, "team"),
        ts_sanitized_team_dir_name(team_id, "team"),
        safe_runtime_dir_component(team_name, "team"),
        safe_runtime_dir_component(team_id, "team"),
    }) {
        auto removed = fs::remove_all(root / component, ec);
        if (!ec && removed > 0) ++summary.team_dirs_removed;
        ec.clear();
    }
    return summary;
}

[[nodiscard]] std::string format_native_agent_task_summary(const agent_runtime::NativeAgentRecord& record) {
    std::string out = std::format(
        "{} [{}] Agent {}: {}",
        record.agent_id,
        agent_runtime::native_agent_status_name(record.status),
        record.agent_type.empty() ? "general-purpose" : record.agent_type,
        cc::tools::detail::native_agent_display_name(record));
    out += "\noutput_file: " + cc::tools::detail::native_agent_output_file(record);
    if (record.team_name && !record.team_name->empty()) out += "\nteam: " + *record.team_name;
    if (record.cwd && !record.cwd->empty()) out += "\ncwd: " + *record.cwd;
    if (record.worktree_path && !record.worktree_path->empty()) out += "\nworktree_path: " + *record.worktree_path;
    if (record.worktree_branch && !record.worktree_branch->empty()) out += "\nworktree_branch: " + *record.worktree_branch;
    if (record.teammate_backend && !record.teammate_backend->empty()) out += "\nteammate_backend: " + *record.teammate_backend;
    if (record.teammate_task_id && !record.teammate_task_id->empty()) out += "\nteammate_task_id: " + *record.teammate_task_id;
    if (record.teammate_pane_id && !record.teammate_pane_id->empty()) out += "\nteammate_pane_id: " + *record.teammate_pane_id;
    if (record.teammate_color && !record.teammate_color->empty()) out += "\nteammate_color: " + *record.teammate_color;
    if (record.remote_task_id && !record.remote_task_id->empty()) out += "\nremote_task_id: " + *record.remote_task_id;
    if (record.remote_task_type && !record.remote_task_type->empty()) out += "\nremote_task_type: " + *record.remote_task_type;
    if (record.remote_session_id && !record.remote_session_id->empty()) out += "\nremote_session_id: " + *record.remote_session_id;
    if (record.remote_session_url && !record.remote_session_url->empty()) out += "\nremote_session_url: " + *record.remote_session_url;
    if (record.remote_title && !record.remote_title->empty()) out += "\nremote_title: " + *record.remote_title;
    if (record.remote_is_review) out += "\nremote_review: true";
    if (record.remote_is_ultraplan) out += "\nremote_ultraplan: true";
    if (record.remote_is_long_running) out += "\nremote_long_running: true";
    if (record.progress) out += std::format("\nprogress: {:.0f}%", *record.progress * 100.0);
    if (record.output && !record.output->empty()) out += "\nresult: " + *record.output;
    if (record.error && !record.error->empty()) out += "\nerror: " + *record.error;
    return out;
}

[[nodiscard]] std::optional<std::string> native_agent_notification_status(
    agent_runtime::NativeAgentStatus status) {
    switch (status) {
        case agent_runtime::NativeAgentStatus::Completed: return "completed";
        case agent_runtime::NativeAgentStatus::Failed: return "failed";
        case agent_runtime::NativeAgentStatus::Cancelled: return "stopped";
        case agent_runtime::NativeAgentStatus::Queued:
        case agent_runtime::NativeAgentStatus::Running:
            return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> format_native_agent_task_notification(
    const agent_runtime::NativeAgentRecord& record) {
    auto status = native_agent_notification_status(record.status);
    if (!status) return std::nullopt;

    const auto description = cc::tools::detail::native_agent_display_name(record);
    std::string summary;
    if (*status == "completed") {
        summary = std::format("Agent \"{}\" completed", description);
    } else if (*status == "failed") {
        summary = std::format("Agent \"{}\" failed: {}", description, record.error.value_or("Unknown error"));
    } else {
        summary = std::format("Agent \"{}\" was stopped", description);
    }

    std::string result_section;
    if (record.output && !record.output->empty()) {
        result_section = std::format("\n<result>{}</result>", escape_xml_text(*record.output));
    } else if (record.error && !record.error->empty()) {
        result_section = std::format("\n<result>{}</result>", escape_xml_text(*record.error));
    }
    std::string worktree_section;
    if (record.worktree_path && !record.worktree_path->empty()) {
        worktree_section += std::format("\n<worktree_path>{}</worktree_path>", escape_xml_text(*record.worktree_path));
    }
    if (record.worktree_branch && !record.worktree_branch->empty()) {
        worktree_section += std::format("\n<worktree_branch>{}</worktree_branch>", escape_xml_text(*record.worktree_branch));
    }
    std::string remote_section;
    if (record.isolation && *record.isolation == "remote") {
        remote_section += "\n<task_type>remote_agent</task_type>";
    }
    if (record.remote_task_id && !record.remote_task_id->empty()) {
        remote_section += std::format("\n<remote_task_id>{}</remote_task_id>", escape_xml_text(*record.remote_task_id));
    }
    if (record.remote_session_id && !record.remote_session_id->empty()) {
        remote_section += std::format("\n<session_id>{}</session_id>", escape_xml_text(*record.remote_session_id));
    }
    if (record.remote_session_url && !record.remote_session_url->empty()) {
        remote_section += std::format("\n<session_url>{}</session_url>", escape_xml_text(*record.remote_session_url));
    }

    return std::format(
        "<task_notification>\n"
        "<task_id>{}</task_id>\n"
        "<output_file>{}</output_file>\n"
        "<status>{}</status>\n"
        "<summary>{}</summary>{}{}{}\n"
        "</task_notification>",
        escape_xml_text(record.agent_id),
        escape_xml_text(cc::tools::detail::native_agent_output_file(record)),
        escape_xml_text(*status),
        escape_xml_text(summary),
        result_section,
        worktree_section,
        remote_section);
}

[[nodiscard]] std::string format_native_agent_task_output(const agent_runtime::NativeAgentRecord& record) {
    std::string out = format_native_agent_task_summary(record);
    if (record.output && !record.output->empty()) {
        out += "\n\nOutput:\n" + *record.output;
    } else if (record.error && !record.error->empty()) {
        out += "\n\nError:\n" + *record.error;
    }

    if (!record.transcript.empty()) {
        out += "\n\nTranscript:\n";
        for (const auto& line : record.transcript) {
            out += line + "\n";
        }
    }

    if (auto notification = cc::tools::detail::format_native_agent_task_notification(record)) {
        out += "\n" + *notification;
    }
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
        if (auto agent = agent_runtime::native_agent_store().get_by_task_id_or_remote_id(*id);
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
        if (auto agent = agent_runtime::native_agent_store().get_by_task_id_or_remote_id(*id);
            agent && is_native_agent_task(*agent)) {
            std::optional<std::string> archive_error;
            bool archive_attempted = false;
            if (agent->remote_session_id && !agent->remote_session_id->empty()) {
                archive_attempted = true;
                auto archived = agent_runtime::native_agent_store().archive_remote_agent_session(agent->agent_id);
                if (!archived) archive_error = archived.error();
            }
            agent_runtime::native_agent_store().request_cancel(agent->agent_id, "stop requested");
            auto stopped = agent_runtime::native_agent_store().get(agent->agent_id).value_or(*agent);
            auto summary = format_native_agent_task_summary(stopped);
            if (archive_error) {
                summary += "\nremote_archive_error: " + *archive_error;
            } else if (archive_attempted) {
                summary += "\nremote_archived: true";
            }
            return ToolResult::success(summary);
        }
        TaskStopTool tool;
        auto result = tool.execute(*id);
        if (!result) return ToolResult::error(std::string(format_error(result.error())));
        return ToolResult::success(std::format("Stopped task {}", *id));
    }
    if (tool_name == "task_update") {
        auto status = parse_task_status(json_string(json, "status").value_or("running"));
        auto result_text = json_string(json, "result").or_else([&] { return json_string(json, "output"); });
        if (auto agent = agent_runtime::native_agent_store().get_by_task_id_or_remote_id(*id);
            agent && is_native_agent_task(*agent)) {
            if (auto parsed = cc::utils::json::parse(json); parsed && parsed->root().is_obj()) {
                auto root = parsed->root();
                auto action = runtime_json_string(root, "action")
                    .or_else([&] { return runtime_json_string(root, "operation"); });
                const bool should_poll_remote =
                    (action && (*action == "poll_remote" || *action == "pollRemote" || *action == "poll")) ||
                    runtime_json_bool(root, "poll_remote").value_or(false) ||
                    runtime_json_bool(root, "pollRemote").value_or(false);
                if (should_poll_remote) {
                    auto applied = agent_runtime::native_agent_store().poll_remote_agent_once(agent->agent_id);
                    if (!applied) return ToolResult::error("Remote poll failed: " + applied.error());
                    auto updated = agent_runtime::native_agent_store().get(agent->agent_id).value_or(*agent);
                    return ToolResult::success(std::format(
                        "Polled remote task {} [{}]\nevents_appended: {}\n{}",
                        updated.agent_id,
                        agent_runtime::native_agent_status_name(applied->status),
                        applied->events_appended,
                        format_native_agent_task_summary(updated)));
                }
                auto session_status = runtime_json_string(root, "session_status")
                    .or_else([&] { return runtime_json_string(root, "sessionStatus"); });
                auto last_event_id = runtime_json_string(root, "last_event_id")
                    .or_else([&] { return runtime_json_string(root, "lastEventId"); });
                auto events = runtime_json_event_array(root, "events");
                if (events.empty()) events = runtime_json_event_array(root, "newEvents");
                auto completion_output = runtime_json_string(root, "completion_output")
                    .or_else([&] { return runtime_json_string(root, "remote_result"); });
                const bool has_remote_poll_payload =
                    session_status.has_value() || last_event_id.has_value() || !events.empty() ||
                    completion_output.has_value();
                if (has_remote_poll_payload) {
                    auto applied = agent_runtime::native_agent_store().apply_remote_poll_result(
                        agent->agent_id,
                        agent_runtime::RemoteAgentPollResult{
                            .session_status = std::move(session_status),
                            .events = std::move(events),
                            .last_event_id = std::move(last_event_id),
                            .completion_output = std::move(completion_output),
                            .result_failed = runtime_json_bool(root, "result_failed")
                                .or_else([&] { return runtime_json_bool(root, "resultFailed"); })
                                .value_or(status == TaskStatus::Failed),
                        });
                    auto updated = agent_runtime::native_agent_store().get(agent->agent_id).value_or(*agent);
                    return ToolResult::success(std::format(
                        "Updated remote task {} [{}]\n{}",
                        updated.agent_id,
                        agent_runtime::native_agent_status_name(applied.status),
                        format_native_agent_task_summary(updated)));
                }
            }
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
        if (auto agent = agent_runtime::native_agent_store().get_by_task_id_or_remote_id(*id);
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

[[nodiscard]] std::string_view computer_use_action_name(cc::core::computer_use::ActionType action) {
    using cc::core::computer_use::ActionType;
    switch (action) {
        case ActionType::Screenshot: return "screenshot";
        case ActionType::MouseMove: return "move";
        case ActionType::MouseClick: return "click";
        case ActionType::MouseDoubleClick: return "double_click";
        case ActionType::MouseRightClick: return "right_click";
        case ActionType::MouseDrag: return "drag";
        case ActionType::KeyType: return "type";
        case ActionType::KeyPress: return "press";
        case ActionType::KeyHotkey: return "hotkey";
        case ActionType::Scroll: return "scroll";
    }
    return "unknown";
}

[[nodiscard]] std::string computer_json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (unsigned char ch : value) {
        switch (ch) {
            case '\\': escaped += R"(\\)"; break;
            case '"': escaped += R"(\")"; break;
            case '\b': escaped += R"(\b)"; break;
            case '\f': escaped += R"(\f)"; break;
            case '\n': escaped += R"(\n)"; break;
            case '\r': escaped += R"(\r)"; break;
            case '\t': escaped += R"(\t)"; break;
            default:
                if (ch < 0x20) escaped += std::format(R"(\u{:04x})", static_cast<unsigned>(ch));
                else escaped.push_back(static_cast<char>(ch));
                break;
        }
    }
    return escaped;
}

inline void computer_append_separator(std::string& out, bool& first) {
    if (!first) out += ',';
    first = false;
}

inline void computer_append_string(std::string& out, std::string_view key, std::string_view value, bool& first) {
    computer_append_separator(out, first);
    out += std::format(R"("{}":"{}")", computer_json_escape(key), computer_json_escape(value));
}

inline void computer_append_int(std::string& out, std::string_view key, std::int64_t value, bool& first) {
    computer_append_separator(out, first);
    out += std::format(R"("{}":{})", computer_json_escape(key), value);
}

[[nodiscard]] std::string computer_use_command_request_json(
    const cc::core::computer_use::ComputerAction& action
) {
    std::string out = "{";
    bool first = true;
    computer_append_string(out, "action", computer_use_action_name(action.type), first);
    if (action.position && !action.region) {
        computer_append_int(out, "x", action.position->x, first);
        computer_append_int(out, "y", action.position->y, first);
    }
    if (action.drag_end) {
        computer_append_int(out, "to_x", action.drag_end->x, first);
        computer_append_int(out, "to_y", action.drag_end->y, first);
    }
    if (action.text) {
        computer_append_string(out, "text", *action.text, first);
    }
    if (!action.keys.empty()) {
        computer_append_separator(out, first);
        out += R"("keys":[)";
        for (std::size_t i = 0; i < action.keys.size(); ++i) {
            if (i != 0) out += ',';
            out += '"';
            out += computer_json_escape(action.keys[i]);
            out += '"';
        }
        out += ']';
    }
    if (action.region) {
        computer_append_int(out, "x", action.region->x, first);
        computer_append_int(out, "y", action.region->y, first);
        computer_append_int(out, "width", action.region->width, first);
        computer_append_int(out, "height", action.region->height, first);
        computer_append_separator(out, first);
        out += std::format(
            R"("region":{{"x":{},"y":{},"width":{},"height":{}}})",
            action.region->x,
            action.region->y,
            action.region->width,
            action.region->height);
    }
    out += '}';
    return out;
}

inline void computer_replace_all(std::string& text, std::string_view needle, std::string_view replacement) {
    if (needle.empty()) return;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        text.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

[[nodiscard]] std::expected<std::string, std::string> run_computer_use_command_backend(
    const cc::core::computer_use::ComputerAction& action
) {
    auto* command_env = std::getenv("CC_REPL_COMPUTER_USE_CMD");
    if (!command_env || std::string_view(command_env).empty()) {
        return std::unexpected("Computer-use command backend is not configured");
    }

    auto payload = computer_use_command_request_json(action);
    auto quoted_payload = runtime_shell_quote(payload);
    std::string command = command_env;
    if (command.find("{request}") != std::string::npos) {
        computer_replace_all(command, "{request}", quoted_payload);
    } else {
        command += ' ';
        command += quoted_payload;
    }

    FILE* pipe = ::popen(command.c_str(), "r");
    if (!pipe) return std::unexpected("Failed to start computer-use command backend");

    std::string output;
    std::array<char, 8192> buffer{};
    while (auto bytes = ::fread(buffer.data(), 1, buffer.size(), pipe)) {
        output.append(buffer.data(), bytes);
        if (output.size() > 1024 * 512) {
            return std::unexpected("Computer-use command backend output exceeded 512 KiB");
        }
    }
    auto status = ::pclose(pipe);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
    if (status != 0) {
        return std::unexpected(std::format("Computer-use command backend failed with status {}", status));
    }
    if (output.empty()) return std::unexpected("Computer-use command backend returned no JSON");
    return output;
}

[[nodiscard]] std::optional<std::string> computer_json_optional_string(
    cc::utils::json::JsonVal root,
    std::string_view key
) {
    auto value = root.get(key);
    if (!value || !value.is_str()) return std::nullopt;
    return std::string(value.as_str());
}

struct ComputerUseCommandBackendResult {
    std::optional<std::string> screenshot_base64;
    std::optional<std::string> format;
    std::optional<std::int64_t> width;
    std::optional<std::int64_t> height;
};

[[nodiscard]] std::expected<ComputerUseCommandBackendResult, std::string> parse_computer_command_result(
    std::string_view output
) {
    auto parsed = cc::utils::json::parse(output);
    if (!parsed || !parsed->root().is_obj()) {
        return std::unexpected("Computer-use command backend returned invalid JSON");
    }
    auto root = parsed->root();
    if (auto success = root.get("success"); success && success.is_bool() && !success.as_bool()) {
        return std::unexpected(
            computer_json_optional_string(root, "error")
                .or_else([&] { return computer_json_optional_string(root, "message"); })
                .value_or("Computer-use command backend reported failure"));
    }
    ComputerUseCommandBackendResult result{
        .screenshot_base64 = computer_json_optional_string(root, "screenshot_base64")
            .or_else([&] { return computer_json_optional_string(root, "base64"); })
            .or_else([&] { return computer_json_optional_string(root, "data"); }),
        .format = computer_json_optional_string(root, "format"),
        .width = std::nullopt,
        .height = std::nullopt,
    };
    if (auto width = root.get("width"); width && width.is_num()) result.width = width.as_int();
    if (auto height = root.get("height"); height && height.is_num()) result.height = height.as_int();
    return result;
}

[[nodiscard]] std::optional<cc::core::computer_use::CaptureProvider> computer_use_command_capture_provider() {
    auto* command_env = std::getenv("CC_REPL_COMPUTER_USE_CMD");
    if (!command_env || std::string_view(command_env).empty()) return std::nullopt;
    return [](std::optional<cc::core::computer_use::Rect> region)
        -> std::expected<cc::core::computer_use::ImageData, std::string> {
        cc::core::computer_use::ComputerAction action{
            .type = cc::core::computer_use::ActionType::Screenshot,
            .position = std::nullopt,
            .drag_end = std::nullopt,
            .text = std::nullopt,
            .region = region,
            .keys = {},
        };
        auto output = run_computer_use_command_backend(action);
        if (!output) return std::unexpected(output.error());
        auto root = parse_computer_command_result(*output);
        if (!root) return std::unexpected(root.error());

        if (!root->screenshot_base64) return std::unexpected("Computer-use command backend did not return screenshot_base64");
        auto decoded = cc::services::image::ImageService::from_base64(*root->screenshot_base64);
        if (!decoded) return std::unexpected(decoded.error().message);

        if (!root->width || !root->height || *root->width <= 0 || *root->height <= 0) {
            return std::unexpected("Computer-use command backend screenshot requires positive width and height");
        }
        return cc::core::computer_use::ImageData{
            .pixels = std::move(*decoded),
            .width = static_cast<std::uint32_t>(*root->width),
            .height = static_cast<std::uint32_t>(*root->height),
            .format = root->format.value_or("rgba"),
        };
    };
}

[[nodiscard]] std::optional<cc::core::computer_use::InputProvider> computer_use_command_input_provider() {
    auto* command_env = std::getenv("CC_REPL_COMPUTER_USE_CMD");
    if (!command_env || std::string_view(command_env).empty()) return std::nullopt;
    return [](const cc::core::computer_use::ComputerAction& action) -> std::expected<void, std::string> {
        auto output = run_computer_use_command_backend(action);
        if (!output) return std::unexpected(output.error());
        auto root = parse_computer_command_result(*output);
        if (!root) return std::unexpected(root.error());
        return {};
    };
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

    auto command_capture_provider = computer_use_command_capture_provider();
    auto command_input_provider = computer_use_command_input_provider();
    cc::core::computer_use::ComputerUseManager manager{
        computer_use_capture_provider_override
            ? cc::core::computer_use::ScreenCapture{*computer_use_capture_provider_override}
            : (command_capture_provider
                ? cc::core::computer_use::ScreenCapture{*command_capture_provider}
                : cc::core::computer_use::ScreenCapture{}),
        computer_use_input_provider_override
            ? *computer_use_input_provider_override
            : (command_input_provider
                ? *command_input_provider
                : cc::core::computer_use::make_native_input_provider())
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

[[nodiscard]] std::string runtime_json_escape_string(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (unsigned char ch : value) {
        switch (ch) {
            case '\\': escaped += R"(\\)"; break;
            case '"': escaped += R"(\")"; break;
            case '\b': escaped += R"(\b)"; break;
            case '\f': escaped += R"(\f)"; break;
            case '\n': escaped += R"(\n)"; break;
            case '\r': escaped += R"(\r)"; break;
            case '\t': escaped += R"(\t)"; break;
            default:
                if (ch < 0x20) {
                    escaped += std::format(R"(\u{:04x})", static_cast<unsigned int>(ch));
                } else {
                    escaped.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return escaped;
}

inline void append_runtime_json_string(
    std::string& out,
    std::string_view key,
    std::string_view value,
    bool& first
) {
    if (!first) out += ',';
    first = false;
    out += '"';
    out += runtime_json_escape_string(key);
    out += R"(":")";
    out += runtime_json_escape_string(value);
    out += '"';
}

inline void append_runtime_json_optional_string(
    std::string& out,
    std::string_view key,
    const std::optional<std::string>& value,
    bool& first
) {
    if (value && !value->empty()) append_runtime_json_string(out, key, *value, first);
}

inline void append_runtime_json_bool(
    std::string& out,
    std::string_view key,
    bool value,
    bool& first
) {
    if (!first) out += ',';
    first = false;
    out += '"';
    out += runtime_json_escape_string(key);
    out += R"(":)";
    out += value ? "true" : "false";
}

inline void append_runtime_json_size(
    std::string& out,
    std::string_view key,
    std::size_t value,
    bool& first
) {
    if (!first) out += ',';
    first = false;
    out += '"';
    out += runtime_json_escape_string(key);
    out += R"(":)";
    out += std::to_string(value);
}

[[nodiscard]] std::string runtime_timestamp_string() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

struct StructuredSendMessagePayload {
    std::string text;
    std::optional<std::string> request_id;
    std::string type;
};

enum class RuntimePeerAddressScheme {
    Other,
    Uds,
    Bridge,
};

struct RuntimePeerAddress {
    RuntimePeerAddressScheme scheme{RuntimePeerAddressScheme::Other};
    std::string target;
};

[[nodiscard]] RuntimePeerAddress parse_runtime_peer_address(std::string_view to) {
    if (to.starts_with("uds:")) {
        return RuntimePeerAddress{.scheme = RuntimePeerAddressScheme::Uds, .target = std::string(to.substr(4))};
    }
    if (to.starts_with("bridge:")) {
        return RuntimePeerAddress{.scheme = RuntimePeerAddressScheme::Bridge, .target = std::string(to.substr(7))};
    }
    if (to.starts_with("/")) {
        return RuntimePeerAddress{.scheme = RuntimePeerAddressScheme::Uds, .target = std::string(to)};
    }
    return RuntimePeerAddress{.scheme = RuntimePeerAddressScheme::Other, .target = std::string(to)};
}

[[nodiscard]] std::string build_runtime_json_object(
    std::initializer_list<std::pair<std::string_view, std::string_view>> strings,
    std::initializer_list<std::pair<std::string_view, bool>> bools = {}
) {
    std::string out = "{";
    bool first = true;
    for (const auto& [key, value] : strings) {
        append_runtime_json_string(out, key, value, first);
    }
    for (const auto& [key, value] : bools) {
        append_runtime_json_bool(out, key, value, first);
    }
	out += '}';
	return out;
}

[[nodiscard]] std::string build_cross_session_prompt(
    std::string_view from_agent,
    std::string_view message
) {
    return std::format(
        "<cross-session-message from=\"{}\">\n{}\n</cross-session-message>",
        escape_xml_text(from_agent),
        escape_xml_text(message));
}

[[nodiscard]] std::string build_uds_cross_session_payload(
    std::string_view from_agent,
    std::string_view message
) {
    const auto prompt = build_cross_session_prompt(from_agent, message);
    std::string out = "{";
    bool first = true;
    append_runtime_json_string(out, "type", "cross_session_message", first);
    append_runtime_json_string(out, "mode", "prompt", first);
    append_runtime_json_string(out, "from", from_agent, first);
    append_runtime_json_string(out, "message", message, first);
    append_runtime_json_string(out, "value", prompt, first);
    out += "}\n";
    return out;
}

[[nodiscard]] std::optional<std::string> runtime_env_value(const char* name) {
    if (const char* value = std::getenv(name); value && *value) return std::string(value);
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> first_runtime_env(std::initializer_list<const char*> names) {
    for (const auto* name : names) {
        if (auto value = runtime_env_value(name)) return value;
    }
    return std::nullopt;
}

[[nodiscard]] std::string strip_runtime_trailing_slashes(std::string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

[[nodiscard]] bool is_safe_runtime_session_id(std::string_view id) {
    if (id.empty()) return false;
    for (const auto ch : id) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '-') return false;
    }
    return true;
}

[[nodiscard]] std::string build_bridge_cross_session_event(
    std::string_view source_session_id,
    std::string_view target_session_id,
    std::string_view message
) {
    const auto prompt = build_cross_session_prompt(source_session_id, message);
    return std::format(
        R"({{"type":"user","message":{{"role":"user","content":"{}"}},"parent_tool_use_id":null,"session_id":"{}","uuid":"{}"}})",
        runtime_json_escape_string(prompt),
        runtime_json_escape_string(target_session_id),
        cc::utils::generate_uuid_v4());
}

[[nodiscard]] std::expected<void, std::string> send_bridge_cross_session_message(
    std::string_view target_session_id,
    std::string_view message
) {
    if (!is_safe_runtime_session_id(target_session_id)) {
        return std::unexpected("Invalid bridge session ID");
    }

    auto endpoint = first_runtime_env({
        "CLAUDE_CODE_REMOTE_API_BASE_URL",
        "CC_REPL_REMOTE_API_BASE_URL",
        "CLAUDE_CODE_SESSION_INGRESS_URL",
        "CC_REPL_SESSION_INGRESS_URL",
    });
    auto source_session_id = first_runtime_env({
        "CC_REMOTE_SESSION_ID",
        "CLAUDE_CODE_REMOTE_SESSION_ID",
    });
    auto auth_token = runtime_env_value("CLAUDE_CODE_SESSION_ACCESS_TOKEN");
    if (!endpoint || !source_session_id || !auth_token) {
        return std::unexpected(
            "Remote Control is not connected - cannot send to a bridge: target. Reconnect with /remote-control first.");
    }
    if (!is_safe_runtime_session_id(*source_session_id)) {
        return std::unexpected("Invalid active bridge session ID");
    }

    std::unordered_map<std::string, std::string> headers{
        {"Content-Type", "application/json"},
        {"Accept", "application/json"},
    };
    if (auth_token->starts_with("sk-ant-sid")) {
        headers["Cookie"] = "sessionKey=" + *auth_token;
        if (auto org = runtime_env_value("CLAUDE_CODE_ORGANIZATION_UUID")) {
            headers["X-Organization-Uuid"] = *org;
        }
    } else {
        headers["Authorization"] = "Bearer " + *auth_token;
    }

    const auto event = build_bridge_cross_session_event(*source_session_id, target_session_id, message);
    const auto body = std::format(R"({{"events":[{}]}})", event);
    const auto url = std::format(
        "{}/v1/sessions/{}/events",
        strip_runtime_trailing_slashes(*endpoint),
        target_session_id);

    cc::utils::HttpClient http;
    auto response = http.post(url, body, headers);
    if (!response) return std::unexpected(response.error().message);
    if (!response->is_ok()) {
        return std::unexpected(std::format("Session ingress returned HTTP {}", response->status));
    }
    return {};
}

[[nodiscard]] std::expected<void, std::string> send_uds_cross_session_message(
    std::string_view socket_path,
    std::string_view from_agent,
    std::string_view message
) {
    if (socket_path.empty()) return std::unexpected("address target must not be empty");
#ifdef _WIN32
    (void)from_agent;
    (void)message;
    return std::unexpected("Unix domain socket messaging is not available on Windows");
#else
    if (socket_path.size() >= sizeof(sockaddr_un{}.sun_path)) {
        return std::unexpected("Unix domain socket path is too long");
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return std::unexpected("Failed to create Unix domain socket");

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, socket_path.data(), socket_path.size());
    addr.sun_path[socket_path.size()] = '\0';

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const auto error = std::string(std::strerror(errno));
        ::close(fd);
        return std::unexpected("Failed to connect Unix domain socket: " + error);
    }

    auto payload = build_uds_cross_session_payload(from_agent, message);
    std::string_view remaining(payload);
    while (!remaining.empty()) {
        const auto sent = ::send(fd, remaining.data(), remaining.size(), 0);
        if (sent <= 0) {
            const auto error = std::string(std::strerror(errno));
            ::close(fd);
            return std::unexpected("Failed to send Unix domain socket message: " + error);
        }
        remaining.remove_prefix(static_cast<std::size_t>(sent));
    }
    ::shutdown(fd, SHUT_WR);
    ::close(fd);
    return {};
#endif
}

[[nodiscard]] std::expected<StructuredSendMessagePayload, std::string>
build_structured_send_message_payload(
    cc::utils::json::JsonVal message,
    std::string_view from_agent
) {
    auto type = runtime_json_string(message, "type");
    if (!type || type->empty()) return std::unexpected("structured message requires type");

    if (*type == "shutdown_request") {
        auto request_id = "shutdown-" + runtime_delivery_message_id().substr(std::string_view("msg-").size());
        auto reason = runtime_json_string(message, "reason").value_or("");
        auto text = build_runtime_json_object({
            {"type", "shutdown_request"},
            {"requestId", request_id},
            {"from", from_agent},
            {"reason", reason},
            {"timestamp", runtime_timestamp_string()},
        });
        return StructuredSendMessagePayload{
            .text = std::move(text),
            .request_id = std::move(request_id),
            .type = *type,
        };
    }

    if (*type == "shutdown_response") {
        auto request_id = runtime_json_string(message, "request_id")
            .or_else([&] { return runtime_json_string(message, "requestId"); });
        if (!request_id || request_id->empty()) {
            return std::unexpected("shutdown_response requires request_id");
        }
        auto approve = runtime_json_semantic_bool(message, "approve")
            .or_else([&] { return runtime_json_semantic_bool(message, "approved"); });
        if (!approve) return std::unexpected("shutdown_response requires approve");

        if (*approve) {
            auto text = build_runtime_json_object({
                {"type", "shutdown_approved"},
                {"requestId", *request_id},
                {"from", from_agent},
                {"timestamp", runtime_timestamp_string()},
            });
            return StructuredSendMessagePayload{
                .text = std::move(text),
                .request_id = *request_id,
                .type = *type,
            };
        }

        auto reason = runtime_json_string(message, "reason");
        if (!reason || reason->empty()) {
            return std::unexpected("reason is required when rejecting a shutdown request");
        }
        auto text = build_runtime_json_object({
            {"type", "shutdown_rejected"},
            {"requestId", *request_id},
            {"from", from_agent},
            {"reason", *reason},
            {"timestamp", runtime_timestamp_string()},
        });
        return StructuredSendMessagePayload{
            .text = std::move(text),
            .request_id = *request_id,
            .type = *type,
        };
    }

    if (*type == "plan_approval_response") {
        auto request_id = runtime_json_string(message, "request_id")
            .or_else([&] { return runtime_json_string(message, "requestId"); });
        if (!request_id || request_id->empty()) {
            return std::unexpected("plan_approval_response requires request_id");
        }
        auto approve = runtime_json_semantic_bool(message, "approve")
            .or_else([&] { return runtime_json_semantic_bool(message, "approved"); });
        if (!approve) return std::unexpected("plan_approval_response requires approve");

        std::string text = "{";
        bool first = true;
        append_runtime_json_string(text, "type", "plan_approval_response", first);
        append_runtime_json_string(text, "requestId", *request_id, first);
        append_runtime_json_bool(text, "approved", *approve, first);
        append_runtime_json_optional_string(text, "feedback", runtime_json_string(message, "feedback"), first);
        append_runtime_json_optional_string(
            text,
            "permissionMode",
            runtime_json_string(message, "permission_mode")
                .or_else([&] { return runtime_json_string(message, "permissionMode"); }),
            first);
        append_runtime_json_string(text, "timestamp", runtime_timestamp_string(), first);
        text += '}';
        return StructuredSendMessagePayload{
            .text = std::move(text),
            .request_id = *request_id,
            .type = *type,
        };
    }

    return std::unexpected("unsupported structured message type: " + *type);
}

[[nodiscard]] bool runtime_has_agent_api_credentials() {
    if (auto* key = std::getenv("ANTHROPIC_API_KEY"); key && key[0] != '\0') return true;
    if (auto* token = std::getenv("CLAUDE_AUTH_TOKEN"); token && token[0] != '\0') return true;
    return false;
}

[[nodiscard]] bool native_agent_can_resume_locally(const agent_runtime::NativeAgentRecord& record) {
    if (record.remote_session_id && !record.remote_session_id->empty()) return false;
    if (record.teammate_backend && !record.teammate_backend->empty() &&
        *record.teammate_backend != "in-process") {
        return false;
    }
    return true;
}

[[nodiscard]] std::optional<std::string> native_agent_resume_cwd(
    const agent_runtime::NativeAgentRecord& record
) {
    if (record.worktree_path && !record.worktree_path->empty()) {
        std::error_code ec;
        if (fs::exists(fs::path{*record.worktree_path}, ec) &&
            fs::is_directory(fs::path{*record.worktree_path}, ec)) {
            return record.worktree_path;
        }
    }
    if (record.cwd && !record.cwd->empty()) return record.cwd;
    return std::nullopt;
}

[[nodiscard]] std::string build_native_agent_resume_input_json(
    const agent_runtime::NativeAgentRecord& record
) {
    std::string out = "{";
    bool first = true;
    append_runtime_json_string(out, "agent_id", record.agent_id, first);
    append_runtime_json_string(out, "subagent_type", record.agent_type, first);
    append_runtime_json_string(out, "prompt",
        "Resume this existing background agent. Process queued follow-up messages and continue from the persisted agent context.",
        first);
    append_runtime_json_bool(out, "run_in_background", true, first);
    append_runtime_json_bool(out, "resume_existing", true, first);
    append_runtime_json_optional_string(out, "description", record.description, first);
    append_runtime_json_optional_string(out, "mode", record.mode, first);
    if (auto cwd = native_agent_resume_cwd(record)) {
        append_runtime_json_string(out, "cwd", *cwd, first);
    }
    out += '}';
    return out;
}

[[nodiscard]] std::string build_team_member_agent_start_input_json(
    const TeamMember& member,
    std::string_view team_name,
    std::string_view prompt,
    const TeamMemberStartOptions* options,
    const std::optional<std::string>& default_cwd,
    const std::optional<std::string>& default_mode,
    const std::optional<std::string>& default_isolation
) {
    std::string out = "{";
    bool first = true;
    append_runtime_json_string(out, "agent_id", member.agent_id, first);
    append_runtime_json_string(out, "subagent_type",
        options && options->agent_type && !options->agent_type->empty()
            ? *options->agent_type
            : std::string{"general-purpose"},
        first);
    append_runtime_json_string(out, "prompt", prompt, first);
    append_runtime_json_bool(out, "run_in_background", true, first);
    append_runtime_json_string(out, "team_name", team_name, first);
    append_runtime_json_string(out, "description",
        std::format("Team member {} for {}", member.agent_id, team_name),
        first);
    if (options && options->mode && !options->mode->empty()) {
        append_runtime_json_string(out, "mode", *options->mode, first);
    } else {
        append_runtime_json_optional_string(out, "mode", default_mode, first);
    }
    if (options && options->cwd && !options->cwd->empty()) {
        append_runtime_json_string(out, "cwd", *options->cwd, first);
    } else {
        append_runtime_json_optional_string(out, "cwd", default_cwd, first);
    }
    if (options && options->isolation && !options->isolation->empty()) {
        append_runtime_json_string(out, "isolation", *options->isolation, first);
    } else {
        append_runtime_json_optional_string(out, "isolation", default_isolation, first);
    }
    out += '}';
    return out;
}

[[nodiscard]] std::string runtime_tool_result_text(const ToolResult& result) {
    std::string out;
    for (const auto& content : result.content) {
        if (!out.empty()) out += '\n';
        out += content.text;
    }
    return out;
}

[[nodiscard]] std::optional<std::string> try_start_native_agent_resume(
    const agent_runtime::NativeAgentRecord& record,
    ToolRegistry* registry
) {
    if (!registry) return "background resume deferred: runtime registry is not attached";
    if (!native_agent_can_resume_locally(record)) {
        return "background resume deferred: agent is managed by a remote or external teammate backend";
    }
    if (!runtime_has_agent_api_credentials()) {
        return "background resume deferred: no Anthropic API credentials are configured";
    }

    auto started = registry->execute(
        "Agent",
        ToolInput::from_json(build_native_agent_resume_input_json(record)));
    if (!started) {
        auto error = "background resume failed: " + started.error().message;
        agent_runtime::native_agent_store().mark_failed(record.agent_id, error);
        return error;
    }
    auto text = runtime_tool_result_text(*started);
    if (started->is_error) {
        auto error = "background resume failed: " + text;
        agent_runtime::native_agent_store().mark_failed(record.agent_id, error);
        return error;
    }
    return "background resume started";
}

[[nodiscard]] Result<ToolResult> execute_simple_runtime_tool(
    std::string_view name,
    const ToolInput& input,
    ToolRegistry* registry = nullptr
) {
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
        auto parsed = cc::utils::json::parse(json);
        if (!parsed || !parsed->root().is_obj()) return ToolResult::error("send_message requires a JSON object input");
        auto root = parsed->root();

	        auto recipient = detail::runtime_json_string(root, "to")
	            .or_else([&] { return detail::runtime_json_string(root, "target_agent"); })
	            .or_else([&] { return detail::runtime_json_string(root, "target"); })
	            .or_else([&] { return detail::runtime_json_string(root, "recipient"); });
	        if (!recipient || recipient->empty()) return ToolResult::error("send_message requires to or target_agent");
	        const auto peer_address = detail::parse_runtime_peer_address(*recipient);
	        if (peer_address.scheme != detail::RuntimePeerAddressScheme::Other && peer_address.target.empty()) {
	            return ToolResult::error("address target must not be empty");
	        }

        MessagePriority priority = MessagePriority::Normal;
        auto priority_text = detail::runtime_json_string(root, "priority").value_or("normal");
        if (priority_text == "low") priority = MessagePriority::Low;
        else if (priority_text == "high") priority = MessagePriority::High;
        else if (priority_text == "urgent") priority = MessagePriority::Urgent;

        auto from_agent = detail::runtime_json_string(root, "from_agent")
            .or_else([&] { return detail::runtime_json_string(root, "from"); })
            .or_else([&] { return cc::utils::get_agent_name(); })
            .value_or("team-lead");
        auto team_name = detail::runtime_json_string(root, "team_name")
            .or_else([&] { return cc::utils::get_team_name(); });
        auto summary = detail::runtime_json_string(root, "summary");

        auto message_node = root.get("message");
        auto message = detail::runtime_json_string(root, "content");
        std::optional<detail::StructuredSendMessagePayload> structured_payload;
        if (!message && message_node.is_str()) {
            message = std::string(message_node.as_str());
	        } else if (!message && message_node.is_obj()) {
	            if (peer_address.scheme != detail::RuntimePeerAddressScheme::Other) {
	                return ToolResult::error("structured messages cannot be sent cross-session - only plain text");
	            }
	            if (*recipient == "*") {
	                return ToolResult::error("structured messages cannot be broadcast (to: \"*\")");
	            }
	            auto structured_type = detail::runtime_json_string(message_node, "type");
	            if (structured_type && *structured_type == "shutdown_response" && *recipient != "team-lead") {
	                return ToolResult::error("shutdown_response must be sent to \"team-lead\"");
	            }
	            auto built = detail::build_structured_send_message_payload(message_node, from_agent);
	            if (!built) return ToolResult::error(built.error());
            message = built->text;
            structured_payload = std::move(*built);
        }
        if (!message || message->empty()) return ToolResult::error("send_message requires content or message");

	        if (peer_address.scheme == detail::RuntimePeerAddressScheme::Bridge) {
	            auto sent = detail::send_bridge_cross_session_message(peer_address.target, *message);
	            if (!sent) return ToolResult::error("Failed to send to " + *recipient + ": " + sent.error());
	            const auto preview = summary.value_or(*message);
	            return ToolResult::success(std::format(
	                "\"{}\" -> {}",
	                detail::escape_xml_text(preview.size() > 50 ? preview.substr(0, 50) : preview),
	                *recipient));
	        }
	        if (peer_address.scheme == detail::RuntimePeerAddressScheme::Uds) {
	            auto sent = detail::send_uds_cross_session_message(peer_address.target, from_agent, *message);
	            if (!sent) return ToolResult::error("Failed to send to " + *recipient + ": " + sent.error());
	            const auto preview = summary.value_or(*message);
	            return ToolResult::success(std::format(
	                "\"{}\" -> {}",
	                detail::escape_xml_text(preview.size() > 50 ? preview.substr(0, 50) : preview),
	                *recipient));
	        }

        struct MailboxTarget {
            std::string agent_id;
            std::string recipient_name;
            std::string team_name;
        };
        struct DeliveryOutcome {
            std::string target_agent;
            std::string message_id;
            DeliveryStatus delivery_status = DeliveryStatus::Delivered;
            bool resumed_terminal_agent = false;
            std::optional<std::string> resume_status_note;
        };

        auto lower_ascii = [](std::string_view value) {
            std::string out(value);
            std::ranges::transform(out, out.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return out;
        };

        auto find_team_member = [&](std::string_view target) -> std::optional<MailboxTarget> {
            if (!team_name || team_name->empty()) return std::nullopt;
            auto team = cc::tools::global_team_store().get_by_id_or_name(*team_name);
            if (!team) return std::nullopt;
            const auto target_lower = lower_ascii(target);
            for (const auto& member : (*team)->members) {
                const auto member_name = detail::team_agent_name_from_id(member.agent_id);
                if (lower_ascii(member.agent_id) != target_lower && lower_ascii(member_name) != target_lower) {
                    continue;
                }
                return MailboxTarget{
                    .agent_id = member.agent_id,
                    .recipient_name = member_name,
                    .team_name = (*team)->name,
                };
            }
            return std::nullopt;
        };

        auto deliver_to_target = [&](std::string target_agent) -> std::expected<DeliveryOutcome, std::string> {
            auto mailbox_target = find_team_member(target_agent);
            if (mailbox_target) target_agent = mailbox_target->agent_id;

            auto recipient_record = cc::tools::agent_runtime::native_agent_store().get(target_agent);
            if (!recipient_record) {
                for (const auto& candidate : cc::tools::agent_runtime::native_agent_store().list()) {
                    const auto candidate_name = candidate.name.value_or(detail::team_agent_name_from_id(candidate.agent_id));
                    if (candidate_name != target_agent && candidate.agent_id != target_agent) continue;
                    if (team_name && (!candidate.team_name || *candidate.team_name != *team_name)) continue;
                    recipient_record = candidate;
                    target_agent = candidate.agent_id;
                    if (!mailbox_target && candidate.team_name && !candidate.team_name->empty()) {
                        mailbox_target = MailboxTarget{
                            .agent_id = candidate.agent_id,
                            .recipient_name = candidate.name.value_or(detail::team_agent_name_from_id(candidate.agent_id)),
                            .team_name = *candidate.team_name,
                        };
                    }
                    break;
                }
            }

            DeliveryOutcome outcome{
                .target_agent = target_agent,
                .message_id = detail::runtime_delivery_message_id(),
                .delivery_status = DeliveryStatus::Delivered,
                .resumed_terminal_agent = false,
                .resume_status_note = std::nullopt,
            };

            if (recipient_record) {
                SendMessageTool validator(from_agent);
                if (auto valid = validator.validate(target_agent, *message); !valid) {
                    return std::unexpected(std::string(format_error(valid.error())));
                }
                outcome.resumed_terminal_agent = detail::native_agent_status_is_terminal(recipient_record->status);
                cc::tools::agent_runtime::native_agent_store().enqueue_resume_message(
                    target_agent,
                    detail::format_agent_pending_user_message(from_agent, priority, *message));
                if (outcome.resumed_terminal_agent) {
                    auto queued_record = cc::tools::agent_runtime::native_agent_store().get(target_agent)
                        .value_or(*recipient_record);
                    outcome.resume_status_note = detail::try_start_native_agent_resume(queued_record, registry);
                }
                cc::tools::agent_runtime::native_agent_store().append_transcript(
                    target_agent,
                    std::format(
                        "message {} from {} [{}]: {}",
                        outcome.message_id,
                        from_agent,
                        message_priority_name(priority),
                        *message));
                if (!mailbox_target && recipient_record->team_name && !recipient_record->team_name->empty()) {
                    mailbox_target = MailboxTarget{
                        .agent_id = target_agent,
                        .recipient_name = recipient_record->name.value_or(detail::team_agent_name_from_id(target_agent)),
                        .team_name = *recipient_record->team_name,
                    };
                }
            } else if (!mailbox_target) {
                SendMessageTool tool(from_agent);
                auto sent = tool.execute(target_agent, *message, priority, detail::runtime_json_string(root, "reply_to"));
                if (!sent) return std::unexpected(std::string(format_error(sent.error())));
                outcome.message_id = sent->message_id;
                outcome.delivery_status = sent->status;
            }

            if (mailbox_target) {
                auto mailbox = cc::utils::write_to_mailbox(
                    mailbox_target->recipient_name,
                    cc::utils::TeammateMessage{
                        .from = from_agent,
                        .text = *message,
                        .timestamp = {},
                        .read = false,
                        .color = cc::utils::get_teammate_color(),
                        .summary = summary,
                    },
                    std::optional<std::string_view>{std::string_view(mailbox_target->team_name)});
                if (!mailbox) {
                    return std::unexpected("Delivered to runtime queue but failed to write teammate mailbox: " + mailbox.error());
                }
            }

            return outcome;
        };

        if (*recipient == "*") {
            if (!team_name || team_name->empty()) {
                return ToolResult::error("send_message broadcast requires team_name or active team context");
            }
            auto team = cc::tools::global_team_store().get_by_id_or_name(*team_name);
            if (!team) return ToolResult::error("Team not found: " + *team_name);

            std::vector<std::string> recipients;
            const auto sender_lower = lower_ascii(from_agent);
            for (const auto& member : (*team)->members) {
                const auto member_name = detail::team_agent_name_from_id(member.agent_id);
                if (lower_ascii(member.agent_id) == sender_lower || lower_ascii(member_name) == sender_lower) {
                    continue;
                }
                auto delivered = deliver_to_target(member.agent_id);
                if (!delivered) return ToolResult::error(delivered.error());
                recipients.push_back(member_name);
            }

            if (recipients.empty()) {
                return ToolResult::success("No teammates to broadcast to (you are the only team member)");
            }

            std::string joined;
            for (std::size_t i = 0; i < recipients.size(); ++i) {
                if (i != 0) joined += ", ";
                joined += recipients[i];
            }
            return ToolResult::success(std::format(
                "Message broadcast to {} teammate(s): {}",
                recipients.size(),
                joined));
        }

        auto delivered = deliver_to_target(*recipient);
        if (!delivered) return ToolResult::error(delivered.error());
        auto status_note = delivered->resumed_terminal_agent
            ? std::string{"\nAgent was stopped and has been queued for background resume."}
            : std::string{};
        if (delivered->resume_status_note && !delivered->resume_status_note->empty()) {
            status_note += "\n" + *delivered->resume_status_note;
        }
        if (structured_payload && structured_payload->request_id) {
            status_note += "\nrequest_id: " + *structured_payload->request_id;
        }
        return ToolResult::success(std::format(
            "Delivered message {} to {} [{}]{}",
            delivered->message_id,
            delivered->target_agent,
            delivery_status_name(delivered->delivery_status),
            status_note));
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
        auto member_start_options = parse_team_member_start_options(root);
        auto tasks = parse_team_tasks(root);
        const bool start_native_agents = runtime_json_bool(root, "start_native_agents")
            .or_else([&] { return runtime_json_bool(root, "start_agents"); })
            .or_else([&] { return runtime_json_bool(root, "run_agents"); })
            .value_or(false);
        const auto default_cwd = runtime_json_string(root, "cwd");
        const auto default_mode = runtime_json_string(root, "mode")
            .or_else([&] { return runtime_json_string(root, "permission_mode"); });
        const auto default_isolation = runtime_json_string(root, "isolation");
        if (start_native_agents && !registry) {
            return ToolResult::error("team_create start_native_agents requires an attached runtime registry");
        }
        if (start_native_agents && !runtime_has_agent_api_credentials()) {
            return ToolResult::error("team_create start_native_agents requires Anthropic API credentials");
        }
        auto result = tool.execute(id, team, members);
        if (!result) return ToolResult::error(std::string(format_error(result.error())));
        std::unordered_map<std::string, std::string> member_start_prompts;
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
        std::size_t task_assignments_enqueued = 0;
        for (auto& task : tasks) {
            auto task_id = task.id;
            auto task_description = task.description;
            auto assigned_to = task.assigned_to;
            auto added = global_team_store().add_task((*result)->id, std::move(task));
            if (!added) return ToolResult::error(std::string(format_error(added.error())));
            if (assigned_to && !assigned_to->empty()) {
                auto assigned = global_team_store().assign_task((*result)->id, task_id, *assigned_to);
                if (!assigned) return ToolResult::error(std::string(format_error(assigned.error())));
                auto assignment_message =
                    detail::format_team_task_assignment_message((*result)->name, task_id, task_description);
                member_start_prompts[*assigned_to] = assignment_message;
                if (!start_native_agents) {
                    cc::tools::agent_runtime::native_agent_store().mark_running(*assigned_to);
                    cc::tools::agent_runtime::native_agent_store().append_transcript(
                        *assigned_to,
                        std::format("team task assigned {}: {}", task_id, task_description));
                    cc::tools::agent_runtime::native_agent_store().enqueue_pending_message(
                        *assigned_to,
                        assignment_message);
                }
                ++task_assignments_enqueued;
            }
        }
        auto created_team = global_team_store().get((*result)->id);
        if (!created_team) return ToolResult::error(std::string(format_error(created_team.error())));
        auto artifacts = detail::ensure_team_runtime_artifacts(
            (*created_team)->name,
            std::span<const TeamMember>((*created_team)->members.data(), (*created_team)->members.size()),
            std::span<const SharedTaskItem>((*created_team)->task_list.data(), (*created_team)->task_list.size()),
            **created_team);
        if (!artifacts) return ToolResult::error(artifacts.error());
        std::size_t native_agents_started = 0;
        if (start_native_agents) {
            for (const auto& member : (*created_team)->members) {
                auto options_it = member_start_options.find(member.agent_id);
                const auto* options = options_it == member_start_options.end() ? nullptr : &options_it->second;
                auto prompt = options && options->prompt && !options->prompt->empty()
                    ? *options->prompt
                    : [&] {
                        auto assigned_prompt = member_start_prompts.find(member.agent_id);
                        if (assigned_prompt != member_start_prompts.end()) return assigned_prompt->second;
                        return std::format(
                            "You are teammate {} on team {}. Coordinate with the team lead and wait for assigned work.",
                            member.agent_id,
                            (*created_team)->name);
                    }();
                auto started = registry->execute(
                    "Agent",
                    ToolInput::from_json(detail::build_team_member_agent_start_input_json(
                        member,
                        (*created_team)->name,
                        prompt,
                        options,
                        default_cwd,
                        default_mode,
                        default_isolation)));
                if (!started) {
                    auto error = "failed to start team member " + member.agent_id + ": " + started.error().message;
                    (void)global_team_store().update_member_status((*created_team)->id, member.agent_id, MemberStatus::Error, error);
                    return ToolResult::error(error);
                }
                if (started->is_error) {
                    auto error = "failed to start team member " + member.agent_id + ": " + detail::runtime_tool_result_text(*started);
                    (void)global_team_store().update_member_status((*created_team)->id, member.agent_id, MemberStatus::Error, error);
                    return ToolResult::error(error);
                }
                cc::tools::agent_runtime::native_agent_store().append_transcript(
                    member.agent_id,
                    std::format("system: started by team_create for team {}", (*created_team)->name));
                ++native_agents_started;
            }
            if (auto refreshed_team = global_team_store().get((*created_team)->id)) {
                auto native_records = detail::collect_team_native_agents(
                    (*refreshed_team)->id,
                    (*refreshed_team)->name,
                    std::span<const TeamMember>((*refreshed_team)->members.data(), (*refreshed_team)->members.size()));
                auto runtime_states = detail::team_config_runtime_states_from_native_records(
                    std::span<const cc::tools::agent_runtime::NativeAgentRecord>(
                        native_records.data(),
                        native_records.size()));
                artifacts->team_config_written = detail::write_team_config_file(
                    artifacts->team_file_path,
                    **refreshed_team,
                    runtime_states);
                if (!artifacts->team_config_written) {
                    return ToolResult::error("failed to refresh team config after starting native agents");
                }
            }
        }
        std::string output = "{";
        bool first = true;
        append_runtime_json_string(output, "team_name", (*result)->name, first);
        append_runtime_json_string(output, "team_file_path", artifacts->team_file_path.string(), first);
        append_runtime_json_string(output, "lead_agent_id", detail::team_lead_agent_id((*result)->name), first);
        append_runtime_json_string(output, "team_id", (*result)->id, first);
        append_runtime_json_string(output, "team_dir", artifacts->team_dir.string(), first);
        append_runtime_json_size(output, "members", (*result)->members.size(), first);
        append_runtime_json_size(output, "tasks", tasks.size(), first);
        append_runtime_json_size(output, "member_inboxes_initialized", artifacts->inboxes_initialized, first);
        append_runtime_json_size(output, "task_assignments_enqueued", task_assignments_enqueued, first);
        append_runtime_json_bool(output, "team_config_written", artifacts->team_config_written, first);
        append_runtime_json_bool(output, "task_list_written", artifacts->task_list_written, first);
        append_runtime_json_size(output, "native_agents_started", native_agents_started, first);
        output += '}';
        return ToolResult::success(output);
    }
    if (name == "team_delete") {
        auto team = json_string(json, "team_id").or_else([&] { return json_string(json, "id"); })
            .or_else([&] { return json_string(json, "team_name"); }).or_else([&] { return json_string(json, "name"); });
        if (!team) return ToolResult::error("team_delete requires team_id");
        auto resolved = global_team_store().get_by_id_or_name(*team);
        if (!resolved) return ToolResult::error(std::string(format_error(resolved.error())));
        const auto team_id = (*resolved)->id;
        const auto team_name = (*resolved)->name;
        const auto members = (*resolved)->members;
        auto native_records = detail::collect_team_native_agents(
            team_id,
            team_name,
            std::span<const TeamMember>(members.data(), members.size()));
        auto cleanup = detail::cleanup_team_runtime_artifacts(
            team_id,
            team_name,
            std::span<const agent_runtime::NativeAgentRecord>(native_records.data(), native_records.size()));
        TeamDeleteTool tool;
        auto result = tool.execute(team_id);
        if (!result) return ToolResult::error(std::string(format_error(result.error())));
        return ToolResult::success(std::format(
            "Deleted team {} ({})\n"
            "native_agents_seen: {}\n"
            "cancelled_agents: {}\n"
            "teammate_terminations: {}\n"
            "teammate_kills: {}\n"
            "background_shell_tasks_stopped: {}\n"
            "transcript_artifacts_removed: {}\n"
            "worktree_cleanup_attempts: {}\n"
            "worktrees_removed: {}\n"
            "worktrees_retained: {}\n"
            "team_dirs_removed: {}",
            team_name,
            team_id,
            cleanup.native_agents_seen,
            cleanup.cancelled_agents,
            cleanup.teammate_terminations,
            cleanup.teammate_kills,
            cleanup.background_shell_tasks_stopped,
            cleanup.transcript_artifacts_removed,
            cleanup.worktree_cleanup_attempts,
            cleanup.worktrees_removed,
            cleanup.worktrees_retained,
            cleanup.team_dirs_removed));
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
using cc::tools::agent::AgentLivePermissionCheck;
using cc::tools::agent::AgentLivePermissionCheckFn;

struct RuntimeToolOptions {
    std::optional<std::string> parent_permission_mode;
    AgentLivePermissionCheckFn permission_check;
    bool permission_hook_valid_for_background = false;
};

[[nodiscard]] std::vector<std::string> runtime_tool_names() {
    return detail::runtime_tool_names_impl();
}

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
    AgentConfig agent_config;
    agent_config.parent_permission_mode = std::move(options.parent_permission_mode);
    registry.register_tool(make_agent_tool(
        std::move(agent_config),
        0,
        &registry,
        std::move(options.permission_check),
        options.permission_hook_valid_for_background));
    registry.register_tool(make_bash_tool());
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
    registry.register_tool(make_glob_tool());
    registry.register_tool(make_grep_tool());
    registry.register_tool(make_todo_write_tool());
    registry.register_tool(make_web_fetch_tool());
    registry.register_tool(make_web_search_tool());

    const auto simple = [&registry](std::string name, std::string description, ToolPermission permission,
                                    std::vector<SchemaProperty> properties = {}, std::string category = "runtime") {
        auto name_copy = name;
        return detail::make_runtime_tool(std::move(name), std::move(description), permission, std::move(properties),
            [name_copy, &registry](const cc::core::ToolInput& input) {
                return detail::execute_simple_runtime_tool(name_copy, input, &registry);
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
    registry.register_tool(simple("remote_trigger", "Invoke a configured remote trigger command",
        ToolPermission::Execute, {SchemaProperty{.name = "payload", .type = "string", .description = "Trigger payload", .required = false}}, "remote"));
    registry.register_tool(simple("repl", "Run a one-shot REPL snippet",
        ToolPermission::Execute, {SchemaProperty{.name = "code", .type = "string", .description = "Code to execute", .required = true}}, "execution"));
    registry.register_tool(simple("schedule_cron", "Schedule a cron-style reminder for this process",
        ToolPermission::Write, {SchemaProperty{.name = "message", .type = "string", .description = "Scheduled message", .required = true}}, "tasks"));
    registry.register_tool(simple("script", "Execute a bounded script",
        ToolPermission::Execute, {SchemaProperty{.name = "code", .type = "string", .description = "Script code", .required = true}}, "execution"));
    registry.register_tool(simple("send_message", "Queue a message for an agent or team",
        ToolPermission::Write, {
            SchemaProperty{.name = "to", .type = "string", .description = "Recipient teammate, '*' broadcast, or compatible target", .required = false},
            SchemaProperty{.name = "message", .type = "object", .description = "Plain text or structured SendMessage payload", .required = false},
            SchemaProperty{.name = "summary", .type = "string", .description = "Preview summary for plain text messages", .required = false},
            SchemaProperty{.name = "target_agent", .type = "string", .description = "Compatibility recipient field", .required = false},
            SchemaProperty{.name = "content", .type = "string", .description = "Compatibility message body field", .required = false},
        }, "agents"));
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

    (void)agent_runtime::restore_remote_agent_poll_loops();

    // Touch built-in agent registry so lazy feature-flag evaluation is
    // performed once per process startup. Produces no side effects but keeps
    // the registry "warm" for agent spawning code paths.
    (void)built_in_agents::are_explore_plan_agents_enabled();
}

void register_runtime_tools(cc::core::ToolRegistry& registry) {
    register_runtime_tools(registry, RuntimeToolOptions{});
}

} // namespace cc::tools
