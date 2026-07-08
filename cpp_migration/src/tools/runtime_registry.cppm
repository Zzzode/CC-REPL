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
import cc.tools.runtime_computer_use;
import cc.tools.runtime_message_delivery;
import cc.tools.runtime_team_shared;
import cc.tools.runtime_shared_utils;
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
import cc.tools.remote_trigger;
import cc.tools.repl;
import cc.tools.script;
import cc.tools.script_types;
import cc.tools.send_message;
import cc.tools.shared_tool;
import cc.tools.skill;
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
import cc.utils.bash_execution;
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

namespace json = cc::utils::json;

using RuntimeExecutor = std::function<Result<ToolResult>(const ToolInput&)>;

class RuntimeFunctionTool final : public ITool {
public:
    RuntimeFunctionTool(
        ToolDefinition definition,
        RuntimeExecutor executor,
        cc::tools::agent::AgentLivePermissionCheckFn permission_check = {})
        : definition_(std::move(definition)),
          executor_(std::move(executor)),
          permission_check_(std::move(permission_check)) {}

    [[nodiscard]] const ToolDefinition& definition() const override {
        return definition_;
    }

    [[nodiscard]] Result<ToolResult> execute(const ToolInput& input) override {
        return executor_(input);
    }

    [[nodiscard]] bool check_permission(const ToolInput& input) const override {
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

private:
    ToolDefinition definition_;
    RuntimeExecutor executor_;
    cc::tools::agent::AgentLivePermissionCheckFn permission_check_;
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
    std::string category = "runtime",
    cc::tools::agent::AgentLivePermissionCheckFn permission_check = {}
) {
    return std::make_unique<RuntimeFunctionTool>(
        define_tool(std::move(name), std::move(description), permission, std::move(properties), std::move(category)),
        std::move(executor),
        std::move(permission_check)
    );
}

// Ad-hoc JSON field accessors over a raw JSON string. These now delegate to
// cc.utils.json (parse once, then typed access) instead of hand-written byte
// scanning — the scanner was obfuscation-prone (it matched the first "\"key\""
// substring anywhere, including inside string values) and is eliminated as
// part of the JSON-consolidation work (audit §13 #3). Signatures/semantics are
// preserved so the ~40 call sites are unchanged.

[[nodiscard]] std::optional<std::string> json_string(std::string_view json, std::string_view key) {
    auto parsed = cc::utils::json::parse(json);
    if (!parsed) return std::nullopt;
    auto val = parsed->root().get(key);
    if (!val.is_str()) return std::nullopt;
    return std::string(val.as_str());
}

[[nodiscard]] std::optional<int> json_int(std::string_view json, std::string_view key) {
    auto parsed = cc::utils::json::parse(json);
    if (!parsed) return std::nullopt;
    auto val = parsed->root().get(key);
    if (!val.is_num()) return std::nullopt;
    return static_cast<int>(val.as_int());
}

[[nodiscard]] bool json_bool(std::string_view json, std::string_view key, bool fallback = false) {
    auto parsed = cc::utils::json::parse(json);
    if (!parsed) return fallback;
    auto val = parsed->root().get(key);
    if (!val.is_bool()) return fallback;
    return val.as_bool();
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

constexpr auto runtime_shell_quote = &runtime_shared_utils::shell_quote;

[[nodiscard]] Result<ToolResult> run_command(std::string command, std::size_t max_bytes = 1024 * 512) {
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

constexpr auto escape_xml_text = &runtime_shared_utils::escape_xml;

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

constexpr auto runtime_delivery_message_id = &runtime_shared_utils::runtime_delivery_message_id;

constexpr auto format_agent_pending_user_message = &runtime_shared_utils::format_agent_pending_user_message;

constexpr auto format_team_task_assignment_message = &runtime_team_shared::format_team_task_assignment_message;

[[nodiscard]] bool native_agent_status_is_terminal(agent_runtime::NativeAgentStatus status) {
    return status == agent_runtime::NativeAgentStatus::Completed ||
        status == agent_runtime::NativeAgentStatus::Failed ||
        status == agent_runtime::NativeAgentStatus::Cancelled;
}

constexpr auto safe_runtime_dir_component = &runtime_shared_utils::safe_runtime_dir_component;

constexpr auto ts_sanitized_team_dir_name = &runtime_team_shared::ts_sanitized_team_dir_name;

constexpr auto path_has_prefix = &runtime_shared_utils::path_has_prefix;

constexpr auto normalized_absolute_path = &runtime_shared_utils::normalized_absolute_path;

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

using TeamDeletionCleanupSummary = runtime_team_shared::TeamDeletionCleanupSummary;
using TeamCreationArtifactsSummary = runtime_team_shared::TeamCreationArtifactsSummary;

constexpr auto team_member_inbox_name = &runtime_team_shared::team_member_inbox_name;
constexpr auto write_empty_inbox_if_missing = &runtime_team_shared::write_empty_inbox_if_missing;
constexpr auto write_team_task_snapshot = &runtime_team_shared::write_team_task_snapshot;
constexpr auto team_agent_name_from_id = &runtime_team_shared::team_agent_name_from_id;
constexpr auto team_lead_agent_id = &runtime_team_shared::team_lead_agent_id;

using TeamConfigMemberRuntimeState = runtime_team_shared::TeamConfigMemberRuntimeState;

constexpr auto write_team_config_optional_string = &runtime_team_shared::write_team_config_optional_string;
constexpr auto write_team_config_optional_bool = &runtime_team_shared::write_team_config_optional_bool;
constexpr auto write_team_config_member = &runtime_team_shared::write_team_config_member;
constexpr auto write_team_config_file = &runtime_team_shared::write_team_config_file;

/// Wrap the extracted function (which takes an explicit terminal-status
/// predicate) so call sites keep their zero-argument signature.
[[nodiscard]] inline std::unordered_map<std::string, TeamConfigMemberRuntimeState>
team_config_runtime_states_from_native_records(
    std::span<const agent_runtime::NativeAgentRecord> records
) {
    return runtime_team_shared::team_config_runtime_states_from_native_records(
        records,
        &native_agent_status_is_terminal);
}

constexpr auto ensure_team_runtime_artifacts = &runtime_team_shared::ensure_team_runtime_artifacts;
constexpr auto contains_agent_id = &runtime_team_shared::contains_agent_id;
constexpr auto collect_team_native_agents = &runtime_team_shared::collect_team_native_agents;

/// Wrap the extracted function (which takes two explicit predicate/cleanup
/// function pointers) so call sites keep their three-argument signature.
[[nodiscard]] inline TeamDeletionCleanupSummary cleanup_team_runtime_artifacts(
    std::string_view team_id,
    std::string_view team_name,
    std::span<const agent_runtime::NativeAgentRecord> records
) {
    return runtime_team_shared::cleanup_team_runtime_artifacts(
        team_id,
        team_name,
        records,
        &native_agent_status_is_terminal,
        &cleanup_native_agent_transcript_artifacts);
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

[[nodiscard]] std::expected<std::string, std::string> run_computer_use_command_backend(
    const cc::core::computer_use::ComputerAction& action
) {
    auto* command_env = std::getenv("CC_REPL_COMPUTER_USE_CMD");
    if (!command_env || std::string_view(command_env).empty()) {
        return std::unexpected("Computer-use command backend is not configured");
    }

    auto payload = runtime_computer_use::command_request_json(action);
    auto quoted_payload = runtime_shell_quote(payload);
    std::string command = command_env;
    if (command.find("{request}") != std::string::npos) {
        runtime_computer_use::replace_all(command, "{request}", quoted_payload);
    } else {
        command += ' ';
        command += quoted_payload;
    }

    auto cap = cc::utils::bash::exec_capture(command);
    if (!cap) return std::unexpected("Failed to start computer-use command backend");
    std::string output = std::move(cap->output);
    if (output.size() > 1024 * 512) {
        return std::unexpected("Computer-use command backend output exceeded 512 KiB");
    }
    auto status = cap->status;
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

namespace json = cc::utils::json;

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

constexpr auto runtime_timestamp_string = &runtime_shared_utils::runtime_timestamp_string;

using StructuredSendMessagePayload = runtime_message_delivery::StructuredSendMessagePayload;
using RuntimePeerAddressScheme = runtime_message_delivery::RuntimePeerAddressScheme;
using RuntimePeerAddress = runtime_message_delivery::RuntimePeerAddress;

constexpr auto parse_runtime_peer_address = &runtime_message_delivery::parse_runtime_peer_address;
constexpr auto build_runtime_json_object = &runtime_message_delivery::build_runtime_json_object;
constexpr auto build_cross_session_prompt = &runtime_message_delivery::build_cross_session_prompt;
constexpr auto build_uds_cross_session_payload = &runtime_message_delivery::build_uds_cross_session_payload;
constexpr auto runtime_env_value = &runtime_message_delivery::runtime_env_value;
constexpr auto first_runtime_env = &runtime_message_delivery::first_runtime_env;
constexpr auto strip_runtime_trailing_slashes = &runtime_message_delivery::strip_runtime_trailing_slashes;
constexpr auto is_safe_runtime_session_id = &runtime_message_delivery::is_safe_runtime_session_id;
constexpr auto build_bridge_cross_session_event = &runtime_message_delivery::build_bridge_cross_session_event;
constexpr auto send_bridge_cross_session_message = &runtime_message_delivery::send_bridge_cross_session_message;
constexpr auto send_uds_cross_session_message = &runtime_message_delivery::send_uds_cross_session_message;
constexpr auto build_structured_send_message_payload = &runtime_message_delivery::build_structured_send_message_payload;
constexpr auto runtime_has_agent_api_credentials = &runtime_message_delivery::runtime_has_agent_api_credentials;
constexpr auto native_agent_can_resume_locally = &runtime_message_delivery::native_agent_can_resume_locally;
constexpr auto native_agent_resume_cwd = &runtime_message_delivery::native_agent_resume_cwd;
constexpr auto build_native_agent_resume_input_json = &runtime_message_delivery::build_native_agent_resume_input_json;

// build_team_member_agent_start_input_json stays here: it depends on
// runtime_registry's ToolInput/ToolRegistry semantics and participates in the
// team_create dispatcher branch (S9), so it is not part of the S7 extraction.

[[nodiscard]] std::string build_team_member_agent_start_input_json(
    const TeamMember& member,
    std::string_view team_name,
    std::string_view prompt,
    const runtime_team_shared::TeamMemberStartOptions* options,
    const std::optional<std::string>& default_cwd,
    const std::optional<std::string>& default_mode,
    const std::optional<std::string>& default_isolation
) {
    json::JsonBuilder b;
    b.str("agent_id", member.agent_id);
    b.str("subagent_type",
         options && options->agent_type && !options->agent_type->empty()
             ? std::string_view{*options->agent_type}
             : std::string_view{"general-purpose"});
    b.str("prompt", prompt);
    b.boolean("run_in_background", true);
    b.str("team_name", team_name);
    b.str("description", std::format("Team member {} for {}", member.agent_id, team_name));
    if (options && options->mode && !options->mode->empty()) {
        b.str("mode", *options->mode);
    } else {
        b.opt_str("mode", default_mode);
    }
    if (options && options->cwd && !options->cwd->empty()) {
        b.str("cwd", *options->cwd);
    } else {
        b.opt_str("cwd", default_cwd);
    }
    if (options && options->isolation && !options->isolation->empty()) {
        b.str("isolation", *options->isolation);
    } else {
        b.opt_str("isolation", default_isolation);
    }
    return b.serialize();
}

constexpr auto runtime_tool_result_text = &runtime_message_delivery::runtime_tool_result_text;
constexpr auto try_start_native_agent_resume = &runtime_message_delivery::try_start_native_agent_resume;

[[nodiscard]] Result<ToolResult> execute_simple_runtime_tool(
    std::string_view name,
    const ToolInput& input,
    ToolRegistry* registry = nullptr
) {
    auto json = input.json();
    if (name == "ask_user_question") {
        auto question = json_string(json, "question").value_or("Continue?");
        auto default_answer = json_string(json, "default_answer");

        // Use the global UI responder if set (e.g. dialog-based prompt).
        // Falls back to stdio for headless / non-interactive builds.
        auto& responder = cc::tools::get_global_ask_user_responder();
        if (responder) {
            auto result = responder(question, default_answer);
            if (result.has_value()) {
                return ToolResult::success(*result);
            }
            return ToolResult::error("User cancelled the prompt");
        }

        // Fallback: stdio
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

        // TS PARITY (2026-07-04): if the MCP result has structured content_items,
        // preserve them as separate ToolOutputContent blocks so images and
        // multi-text results survive to the UI renderer.
        if (!result->content_items.empty()) {
            std::vector<ToolOutputContent> items;
            for (const auto& ci : result->content_items) {
                if (ci.type == "text") {
                    items.push_back(ToolOutputContent::text_output(ci.text));
                } else if (ci.type == "image") {
                    items.push_back(ToolOutputContent::image_output(
                        ci.media_type.value_or("image/png"),
                        ci.data.value_or("")));
                }
            }
            if (items.empty()) {
                // All items were non-text/non-image types; fall back to flattened
                items.push_back(ToolOutputContent::text_output(result->content));
            }
            auto tool_result = ToolResult::success_multi(std::move(items));
            tool_result.is_error = result->is_error;
            return tool_result;
        }
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
        return runtime_message_delivery::execute_send_message(
            json, registry, &native_agent_status_is_terminal);
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
    if (name == "skill") {
        // New path: use execute_skill_tool_simple which validates the skill,
        // expands templates, cascades context modifiers, and returns a
        // structured JSON response. Falls back to the original loader on
        // error (so tools that pass "name" only still work).
        auto simple = cc::tools::skill::execute_skill_tool_simple(input.json());
        if (simple) return ToolResult::success(*simple);
        return execute_skill_tool(input);
    }
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
        auto members = runtime_team_shared::parse_team_members(root);
        auto member_start_options = runtime_team_shared::parse_team_member_start_options(root);
        auto tasks = runtime_team_shared::parse_team_tasks(root);
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
            cc::tools::agent_runtime::NativeAgentRecord record;
            record.agent_id = member.agent_id;
            record.agent_type = std::string(member_role_name(member.role));
            record.team_name = (*result)->name;
            record.background = true;
            record.status = cc::tools::agent_runtime::NativeAgentStatus::Queued;
            cc::tools::agent_runtime::native_agent_store().upsert(std::move(record));
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
        json::JsonBuilder b;
        b.str("team_name", (*result)->name);
        b.str("team_file_path", artifacts->team_file_path.string());
        b.str("lead_agent_id", detail::team_lead_agent_id((*result)->name));
        b.str("team_id", (*result)->id);
        b.str("team_dir", artifacts->team_dir.string());
        b.size("members", (*result)->members.size());
        b.size("tasks", tasks.size());
        b.size("member_inboxes_initialized", artifacts->inboxes_initialized);
        b.size("task_assignments_enqueued", task_assignments_enqueued);
        b.boolean("team_config_written", artifacts->team_config_written);
        b.boolean("task_list_written", artifacts->task_list_written);
        b.size("native_agents_started", native_agents_started);
        return ToolResult::success(b.serialize());
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
    auto permission_check = std::move(options.permission_check);
    AgentConfig agent_config;
    agent_config.parent_permission_mode = std::move(options.parent_permission_mode);
    registry.register_tool(make_agent_tool(
        std::move(agent_config),
        0,
        &registry,
        permission_check,
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
    registry.register_tool(simple("config", "Read or update CC-REPL configuration",
        ToolPermission::Write, {prop("action", "string", "get or set", false)}, "config"));
    registry.register_tool(simple("enter_plan_mode", "Enter plan mode",
        ToolPermission::Write, {}, "planning"));
    registry.register_tool(simple("exit_plan_mode", "Exit plan mode",
        ToolPermission::Write, {}, "planning"));
    registry.register_tool(simple("enter_worktree", "Create and enter a git worktree",
        ToolPermission::Execute, {prop("branch", "string", "Branch name", true)}, "git"));
    registry.register_tool(simple("exit_worktree", "Remove a git worktree",
        ToolPermission::Execute, {prop("path", "string", "Worktree path", false)}, "git"));
    registry.register_tool(simple("lsp", "Fallback language intelligence for definitions, references, symbols, hover, and diagnostics",
        ToolPermission::ReadOnly, {prop("file_path", "string", "File path", true)}, "code"));
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
        ToolPermission::Execute, {prop("payload", "string", "Trigger payload", false)}, "remote"));
    registry.register_tool(simple("repl", "Run a one-shot REPL snippet",
        ToolPermission::Execute, {prop("code", "string", "Code to execute", true)}, "execution"));
    registry.register_tool(simple("schedule_cron", "Schedule a cron-style reminder for this process",
        ToolPermission::Write, {prop("message", "string", "Scheduled message", true)}, "tasks"));
    registry.register_tool(simple("script", "Execute a bounded script",
        ToolPermission::Execute, {prop("code", "string", "Script code", true)}, "execution"));
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
            prop("session_id", "string", "Session identifier for ${CLAUDE_SESSION_ID}", false),
            prop("name", "string", "Alias for skill_path", false),
        }, "skills"));
    registry.register_tool(simple("sleep", "Sleep for a bounded number of seconds",
        ToolPermission::Execute, {prop("duration", "number", "Duration in seconds", true)}, "execution"));
    registry.register_tool(simple("synthetic_output", "Return provided synthetic output content",
        ToolPermission::ReadOnly, {prop("content", "string", "Content", true)}, "testing"));

    for (const auto& name : {"task_create", "task_get", "task_list", "task_output", "task_stop", "task_update"}) {
        registry.register_tool(simple(name, std::format("Runtime task operation {}", name), ToolPermission::Write,
            {
                prop("task_id", "string", "Task ID", false),
                prop("pid", "number", "Background process PID", false),
            }, "tasks"));
    }
    registry.register_tool(simple("team_create", "Create a runtime team record", ToolPermission::Write,
        {prop("team_name", "string", "Team name", false)}, "agents"));
    registry.register_tool(simple("team_delete", "Delete a runtime team record", ToolPermission::Write,
        {prop("team_name", "string", "Team name", true)}, "agents"));
    registry.register_tool(simple("testing", "Run a test command", ToolPermission::Execute,
        {prop("command", "string", "Test command", false)}, "testing"));
    registry.register_tool(simple("tool_search", "Search registered runtime tools", ToolPermission::ReadOnly,
        {prop("query", "string", "Search query", false)}, "tools"));
    registry.register_tool(simple("tungsten", "Use the Tungsten integration when configured", ToolPermission::Network, {}, "integrations"));
    registry.register_tool(simple("web_browser", "Automate browser navigation, extraction, form fill, and screenshots",
        ToolPermission::Network, {prop("action", "string", "Browser action", true)}, "browser"));
    registry.register_tool(simple("workflow", "Read and execute workflow definitions", ToolPermission::ReadOnly,
        {prop("file", "string", "Workflow file", true)}, "workflow"));

    (void)agent_runtime::restore_remote_agent_poll_loops();

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
            // Generic input schema: accepts any JSON object.  The model
            // infers specific parameters from the tool description.
            def.input_schema = cc::core::InputSchema{};
            def.permission = cc::core::ToolPermission::Network;
            def.is_hidden = false;
            def.category = std::format("mcp:{}", server.name);
            defs.push_back(std::move(def));
        }
    }
    return defs;
}

} // namespace cc::tools
