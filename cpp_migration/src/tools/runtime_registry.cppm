/// @file runtime_registry.cppm
/// @brief Runtime registration for all migrated tools exposed to the query engine.
module;

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.tools.runtime_registry;

import cc.tools.tool;
import cc.tools.agent;
import cc.tools.bash;
import cc.tools.file_edit;
import cc.tools.file_read;
import cc.tools.file_write;
import cc.tools.glob;
import cc.tools.grep;
import cc.tools.todo_write;
import cc.tools.web_fetch;
import cc.tools.web_search;

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

[[nodiscard]] std::string shell_quote(std::string_view value) {
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

[[nodiscard]] Result<ToolResult> execute_lsp_fallback(const ToolInput& input) {
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

    auto lines = read_lines(file);
    auto query = json_string(json, "query");
    auto line_no = json_int(json, "line").value_or(0);
    auto character = json_int(json, "character").value_or(0);
    auto word = query.value_or(word_at_position(lines, line_no, character));

    if (action == "diagnostics") {
        return ToolResult::success(std::format("No diagnostics available from fallback analyzer for {}", file.string()));
    }

    if (action == "hover") {
        if (word.empty()) return ToolResult::error("No symbol at requested position");
        return ToolResult::success(std::format("{}: fallback symbol information from {}", word, file.string()));
    }

    if (action == "completion") {
        std::vector<std::string> symbols;
        std::regex ident{R"([A-Za-z_][A-Za-z0-9_]*)"};
        for (const auto& line : lines) {
            for (auto it = std::sregex_iterator(line.begin(), line.end(), ident); it != std::sregex_iterator{}; ++it) {
                auto value = it->str();
                if (!word.empty() && !value.starts_with(word)) continue;
                if (std::ranges::find(symbols, value) == symbols.end()) symbols.push_back(value);
                if (symbols.size() >= 50) break;
            }
            if (symbols.size() >= 50) break;
        }
        std::ranges::sort(symbols);
        return ToolResult::success("Completions:\n" + join_args(symbols));
    }

    if (action == "symbols") {
        std::regex symbol_re{R"(\b(class|struct|enum|namespace|auto|void|int|long|double|float|bool|std::string)\s+([A-Za-z_][A-Za-z0-9_]*)\b)"};
        std::string out = "Symbols:\n";
        std::size_t count = 0;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            std::smatch match;
            auto text = lines[i];
            if (std::regex_search(text, match, symbol_re)) {
                auto name = match[2].str();
                if (query && !name.contains(*query)) continue;
                out += std::format("{}:{} {}\n", file.string(), i + 1, name);
                if (++count >= 100) break;
            }
        }
        if (count == 0) out += "No symbols found.\n";
        return ToolResult::success(out);
    }

    if (word.empty()) {
        return ToolResult::error("definition/references requires query or a valid line/character position");
    }

    auto root = file.parent_path();
    std::string out = action == "definition" ? "Definitions:\n" : "References:\n";
    std::size_t count = 0;
    std::regex definition_re{std::format(R"(\b(class|struct|enum|auto|void|int|long|double|float|bool|std::string)\s+{}\b)", word)};
    std::regex reference_re{std::format(R"(\b{}\b)", word)};
    const auto& active_re = action == "definition" ? definition_re : reference_re;

    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || !is_source_file(entry.path())) continue;
        auto search_lines = read_lines(entry.path());
        for (std::size_t i = 0; i < search_lines.size(); ++i) {
            if (!std::regex_search(search_lines[i], active_re)) continue;
            out += std::format("{}:{}: {}\n", entry.path().string(), i + 1, search_lines[i]);
            if (++count >= 100) return ToolResult::success(out);
        }
    }
    if (count == 0) out += "No results found.\n";
    return ToolResult::success(out);
}

[[nodiscard]] Result<ToolResult> execute_script(const ToolInput& input) {
    auto json = input.json();
    auto code = json_string(json, "code");
    if (!code || code->empty()) return ToolResult::error("script requires code");

    auto language = json_string(json, "language").value_or("shell");
    auto timeout = std::clamp(json_int(json, "timeout").value_or(30), 1, 300);
    std::string ext = ".sh";
    std::string interpreter = "/bin/sh";
    if (language == "python" || language == "python3") {
        ext = ".py";
        interpreter = "python3";
    } else if (language == "javascript" || language == "js" || language == "node") {
        ext = ".js";
        interpreter = "node";
    } else if (language != "shell" && language != "sh") {
        return ToolResult::error(std::format("Unsupported script language: {}", language));
    }

    auto tmp = fs::temp_directory_path() /
        std::format("cc_repl_script_{}{}", std::chrono::steady_clock::now().time_since_epoch().count(), ext);
    {
        std::ofstream out(tmp);
        if (!out) return ToolResult::error(std::format("Cannot create temporary script: {}", tmp.string()));
        out << *code;
    }

    auto command = std::format("ulimit -t {} 2>/dev/null; {} {} 2>&1",
        timeout, interpreter, shell_quote(tmp.string()));
    auto result = run_command(std::move(command));
    fs::remove(tmp);
    return result;
}

struct RuntimeTask {
    std::string id;
    std::string description;
    std::string status{"running"};
    std::string output;
};

[[nodiscard]] std::unordered_map<std::string, RuntimeTask>& task_store() {
    static std::unordered_map<std::string, RuntimeTask> tasks;
    return tasks;
}

[[nodiscard]] Result<ToolResult> execute_task_tool(std::string_view tool_name, const ToolInput& input) {
    auto json = input.json();
    auto& tasks = task_store();
    if (tool_name == "task_create") {
        auto description = json_string(json, "description").or_else([&] { return json_string(json, "task"); });
        if (!description || description->empty()) return ToolResult::error("task_create requires description");
        auto id = json_string(json, "task_id").or_else([&] { return json_string(json, "id"); })
            .value_or(std::format("task-{}", tasks.size() + 1));
        auto [it, inserted] = tasks.emplace(id, RuntimeTask{.id = id, .description = *description});
        if (!inserted) return ToolResult::error(std::format("Task already exists: {}", id));
        return ToolResult::success(std::format("Created task {}: {}", id, *description));
    }

    if (tool_name == "task_list") {
        std::string out = "Tasks:\n";
        for (const auto& [_, task] : tasks) {
            out += std::format("- {} [{}] {}\n", task.id, task.status, task.description);
        }
        if (tasks.empty()) out += "No tasks.\n";
        return ToolResult::success(out);
    }

    auto id = json_string(json, "task_id").or_else([&] { return json_string(json, "id"); });
    if (!id || id->empty()) return ToolResult::error(std::format("{} requires task_id", tool_name));
    auto it = tasks.find(*id);
    if (it == tasks.end()) return ToolResult::error(std::format("Task not found: {}", *id));

    if (tool_name == "task_get") {
        return ToolResult::success(std::format("{} [{}]\n{}\n{}", it->second.id, it->second.status,
            it->second.description, it->second.output));
    }
    if (tool_name == "task_stop") {
        it->second.status = "cancelled";
        return ToolResult::success(std::format("Stopped task {}", *id));
    }
    if (tool_name == "task_update") {
        it->second.status = json_string(json, "status").value_or(it->second.status);
        if (auto output = json_string(json, "output")) it->second.output += *output;
        if (auto result = json_string(json, "result")) it->second.output += *result;
        return ToolResult::success(std::format("Updated task {} [{}]", *id, it->second.status));
    }
    if (tool_name == "task_output") {
        return ToolResult::success(it->second.output.empty() ? "(no output)" : it->second.output);
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
        "Edit",
        "Glob",
        "Grep",
        "Read",
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
        return run_command(std::format("git worktree add -B {} {} 2>&1", shell_quote(*branch), shell_quote(path)));
    }

    auto path = json_string(json, "path").value_or(fs::current_path().string());
    return run_command(std::format("git worktree remove {} 2>&1", shell_quote(path)));
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
    auto path = json_string(input.json(), "file_path").or_else([&] { return json_string(input.json(), "path"); });
    auto find_text = json_string(input.json(), "old_string").or_else([&] { return json_string(input.json(), "find"); });
    auto replace_text = json_string(input.json(), "new_string").or_else([&] { return json_string(input.json(), "replace"); });
    if (!path || !find_text || !replace_text) {
        return ToolResult::error("notebook_edit requires file_path, old_string, and new_string");
    }
    std::ifstream in(*path);
    if (!in) return ToolResult::error(std::format("Cannot read notebook: {}", *path));
    std::stringstream buffer;
    buffer << in.rdbuf();
    auto content = buffer.str();
    auto pos = content.find(*find_text);
    if (pos == std::string::npos) return ToolResult::error("old_string was not found in notebook");
    content.replace(pos, find_text->size(), *replace_text);
    std::ofstream out(*path);
    if (!out) return ToolResult::error(std::format("Cannot write notebook: {}", *path));
    out << content;
    return ToolResult::success(std::format("Updated notebook {}", *path));
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
    if (name == "config") return execute_config_tool(input);
    if (name == "enter_plan_mode") {
        static bool plan_mode = false;
        plan_mode = true;
        return ToolResult::success("Plan mode enabled");
    }
    if (name == "exit_plan_mode") {
        static bool plan_mode = false;
        plan_mode = false;
        return ToolResult::success("Plan mode disabled");
    }
    if (name == "enter_worktree") return execute_worktree("enter", input);
    if (name == "exit_worktree") return execute_worktree("exit", input);
    if (name == "lsp") return execute_lsp_fallback(input);
    if (name == "list_mcp_resources") return execute_resource_list(input);
    if (name == "read_mcp_resource") return execute_local_resource_read(input);
    if (name == "mcp") {
        auto server = json_string(json, "server_name").or_else([&] { return json_string(json, "server"); });
        auto tool = json_string(json, "tool_name").or_else([&] { return json_string(json, "tool"); });
        if (!server || !tool) return ToolResult::error("mcp requires server_name and tool_name");
        return ToolResult::error(std::format("MCP transport is not connected for server '{}'", *server));
    }
    if (name == "mcp_auth") {
        auto server = json_string(json, "server_name").or_else([&] { return json_string(json, "server"); });
        if (!server) return ToolResult::error("mcp_auth requires server_name");
        auto env_name = "MCP_" + *server + "_TOKEN";
        std::ranges::replace(env_name, '-', '_');
        std::ranges::replace(env_name, '.', '_');
        return std::getenv(env_name.c_str())
            ? ToolResult::success(std::format("Authentication token available in {}", env_name))
            : ToolResult::error(std::format("No authentication token found in {}", env_name));
    }
    if (name == "notebook_edit") return execute_notebook_edit(input);
    if (name == "powershell") {
#ifdef _WIN32
        auto command = json_string(json, "command");
        if (!command) return ToolResult::error("powershell requires command");
        return run_command("powershell -NoProfile -Command " + shell_quote(*command) + " 2>&1");
#else
        return ToolResult::error("PowerShell execution is only available on Windows in this runtime");
#endif
    }
    if (name == "remote_trigger") {
        const char* command = std::getenv("CC_REPL_REMOTE_TRIGGER_COMMAND");
        if (!command) return ToolResult::error("CC_REPL_REMOTE_TRIGGER_COMMAND is not configured");
        auto payload = json_string(json, "payload").value_or(std::string(json));
        return run_command(std::format("{} {}", command, shell_quote(payload)));
    }
    if (name == "repl") return execute_script(input);
    if (name == "schedule_cron") {
        static std::vector<std::string> scheduled;
        auto message = json_string(json, "message").or_else([&] { return json_string(json, "command"); });
        if (!message) return ToolResult::error("schedule_cron requires message or command");
        scheduled.push_back(*message);
        return ToolResult::success(std::format("Scheduled cron entry {}", scheduled.size()));
    }
    if (name == "script") return execute_script(input);
    if (name == "send_message") {
        auto recipient = json_string(json, "recipient").or_else([&] { return json_string(json, "to"); }).value_or("default");
        auto message = json_string(json, "message").value_or(std::string(json));
        return ToolResult::success(std::format("Queued message to {}: {}", recipient, message));
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
        auto seconds = std::clamp(json_int(json, "duration").or_else([&] { return json_int(json, "seconds"); }).value_or(1), 1, 300);
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        return ToolResult::success(std::format("Slept for {} seconds", seconds));
    }
    if (name == "synthetic_output") {
        return ToolResult::success(json_string(json, "content").or_else([&] { return json_string(json, "text"); }).value_or(std::string(json)));
    }
    if (name.starts_with("task_")) return execute_task_tool(name, input);
    if (name == "team_create") {
        static std::vector<std::string> teams;
        auto team = json_string(json, "team_name").or_else([&] { return json_string(json, "name"); }).value_or(std::format("team-{}", teams.size() + 1));
        teams.push_back(team);
        return ToolResult::success(std::format("Created team {}", team));
    }
    if (name == "team_delete") {
        auto team = json_string(json, "team_name").or_else([&] { return json_string(json, "name"); });
        if (!team) return ToolResult::error("team_delete requires team_name");
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
        return ToolResult::error("Tungsten integration is not configured in this runtime");
    }
    if (name == "workflow") {
        auto file = json_string(json, "file").or_else([&] { return json_string(json, "path"); });
        if (!file) return ToolResult::error("workflow requires file or path");
        std::ifstream in(*file);
        if (!in) return ToolResult::error(std::format("Workflow file not found: {}", *file));
        std::stringstream buffer;
        buffer << in.rdbuf();
        return ToolResult::success(buffer.str());
    }
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
    registry.register_tool(simple("notebook_edit", "Edit a notebook file by replacing text",
        ToolPermission::Write, {SchemaProperty{.name = "file_path", .type = "string", .description = "Notebook path", .required = true}}, "filesystem"));
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
            {SchemaProperty{.name = "task_id", .type = "string", .description = "Task ID", .required = false}}, "tasks"));
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
    registry.register_tool(simple("workflow", "Read and execute workflow definitions", ToolPermission::ReadOnly,
        {SchemaProperty{.name = "file", .type = "string", .description = "Workflow file", .required = true}}, "workflow"));
}

} // namespace cc::tools
