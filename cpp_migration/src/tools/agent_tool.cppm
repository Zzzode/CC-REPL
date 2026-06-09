// AgentTool - Sub-agent delegation with recursive API loop
module;

#include <atomic>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <array>
#include <utility>
#include <sstream>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <sys/wait.h>

export module cc.tools.agent;

import cc.utils.error;
import cc.utils.git;
import cc.tools.tool;
import cc.utils.json;
import cc.tools.agent_runtime;
import cc.tools.agent_constants;
import cc.tools.agent_memory;
import cc.tools.agent_memory_snapshot;
import cc.tools.agent_color_manager;
import cc.tools.agent_display;
import cc.tools.bash;
import cc.tools.todo_write;
import cc.tools.send_message;
import cc.tools.team;
import cc.tools.mcp;
import cc.tools.sleep;
import cc.tools.web_fetch;
import cc.skills.skill;
import cc.utils.team_helpers;
import cc.services.api.client;
import cc.services.api.streaming;
import cc.services.api.bootstrap;
import cc.services.mcp.types;
import cc.utils.swarm_backends;
import cc.utils.env_utils;
import cc.utils.tool_helpers;

export namespace cc::tools::agent {

namespace fs = std::filesystem;

using cc::core::Tool;
using cc::core::ToolInput;
using cc::core::ToolResult;
using cc::core::ToolDefinition;
using cc::core::ToolPermission;
using cc::core::InputSchema;
using cc::core::SchemaProperty;
using cc::utils::Result;
using cc::services::api::AnthropicClient;
using cc::services::api::CreateMessageRequest;
using cc::services::api::Message;
using cc::services::api::ContentBlock;
using cc::services::api::ContentBlockType;
using cc::services::api::StreamParser;
using cc::services::api::StreamEventType;
using cc::services::api::StreamContentBlockType;
using cc::services::api::get_default_client;

// =========================================================================
// Agent Configuration
// =========================================================================

struct AgentConfig {
    int max_turns = 200;           // Max agentic loop iterations
    int max_depth = 3;             // Max recursive agent nesting depth
    std::string default_model = "claude-sonnet-4-20250514";
    std::vector<std::string> allowed_tools;  // Empty = inherit all from parent
    std::vector<std::string> denied_tools;   // Explicitly blocked tools for sub-agents
    std::optional<std::string> parent_agent_id;
    std::optional<std::string> parent_permission_mode;
    bool prefer_in_process_teammate = false;
};

struct AgentLivePermissionCheck {
    bool allowed = true;
    std::optional<std::string> updated_input_json;
    std::optional<std::string> message;
};

using AgentLivePermissionCheckFn = std::function<AgentLivePermissionCheck(
    std::string_view tool_name,
    std::string_view input_json,
    std::string_view tool_use_id
)>;

[[nodiscard]] inline std::string json_escape_string(std::string_view value) {
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

[[nodiscard]] inline bool is_todo_write_tool_name(std::string_view tool_name) {
    return tool_name == "todo_write" || tool_name == "TodoWrite";
}

[[nodiscard]] inline bool is_agent_scoped_shell_tool_name(std::string_view tool_name) {
    return tool_name == "bash" || tool_name == "Bash";
}

[[nodiscard]] inline bool text_contains_fork_boilerplate(std::string_view text) {
    return text.find("<fork-boilerplate>") != std::string_view::npos;
}

[[nodiscard]] inline bool query_source_is_fork_child(std::string_view query_source) {
    return query_source == "agent:builtin:fork";
}

[[nodiscard]] inline std::string inject_agent_id_into_tool_input(
    std::string_view raw_json,
    std::string_view agent_id
) {
    if (agent_id.empty()) return std::string(raw_json);

    auto doc = cc::utils::json::parse(raw_json);
    if (!doc) return std::string(raw_json);
    auto root = doc->root();
    if (!root.valid() || !root.is_obj()) return std::string(raw_json);
    if (root.get("agent_id").valid() || root.get("agentId").valid()) {
        return std::string(raw_json);
    }

    std::size_t open_brace = 0;
    while (open_brace < raw_json.size() && std::isspace(static_cast<unsigned char>(raw_json[open_brace]))) {
        ++open_brace;
    }
    if (open_brace >= raw_json.size() || raw_json[open_brace] != '{') {
        return std::string(raw_json);
    }

    std::size_t next = open_brace + 1;
    while (next < raw_json.size() && std::isspace(static_cast<unsigned char>(raw_json[next]))) {
        ++next;
    }
    const bool object_empty = next < raw_json.size() && raw_json[next] == '}';

    std::string scoped;
    scoped.reserve(raw_json.size() + agent_id.size() + 24);
    scoped.append(raw_json.substr(0, open_brace + 1));
    scoped += R"("agent_id":")";
    scoped += json_escape_string(agent_id);
    scoped += '"';
    if (!object_empty) scoped += ',';
    scoped.append(raw_json.substr(open_brace + 1));
    return scoped;
}

[[nodiscard]] inline std::string inject_agent_id_into_todo_input(
    std::string_view raw_json,
    std::string_view agent_id
) {
    return inject_agent_id_into_tool_input(raw_json, agent_id);
}

struct AgentTodoCleanupGuard {
    std::string agent_id;

    ~AgentTodoCleanupGuard() {
        if (!agent_id.empty()) {
            (void)cc::tools::clear_todos_for_agent(agent_id);
        }
    }
};

struct AgentShellTaskCleanupGuard {
    std::string agent_id;

    ~AgentShellTaskCleanupGuard() {
        if (agent_id.empty()) return;
        auto stopped = cc::tools::bash::stop_background_tasks_for_agent(agent_id);
        if (!stopped.empty()) {
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                agent_id,
                std::format("system: stopped {} background shell task(s) owned by agent on exit", stopped.size()));
        }
    }
};

struct AgentInlineMcpServerRuntimeState {
    std::string name;
    std::optional<cc::tools::NativeMcpConfiguredServer> previous_config;
};

struct AgentMcpCleanupGuard {
    std::string agent_id;
    std::vector<AgentInlineMcpServerRuntimeState> inline_servers;

    ~AgentMcpCleanupGuard() {
        if (inline_servers.empty()) return;

        std::vector<std::string> remove_names;
        std::vector<cc::tools::NativeMcpConfiguredServer> restore_servers;
        for (const auto& server : inline_servers) {
            if (server.name.empty()) continue;
            if (server.previous_config) {
                restore_servers.push_back(*server.previous_config);
            } else {
                remove_names.push_back(server.name);
            }
        }

        bool cleaned = false;
        if (!remove_names.empty()) {
            if (auto removed = cc::tools::remove_native_mcp_servers(std::move(remove_names)); removed) {
                cleaned = true;
            }
        }
        if (!restore_servers.empty()) {
            if (auto restored = cc::tools::upsert_native_mcp_servers(std::move(restore_servers)); restored) {
                cleaned = true;
            }
        }
        if (cleaned && !agent_id.empty()) {
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                agent_id,
                "system: cleaned agent-specific MCP server configuration");
        }
    }
};

inline void append_json_string_field(
    std::string& out,
    std::string_view name,
    std::string_view value,
    bool& first
) {
    if (!first) out += ',';
    first = false;
    out += '"';
    out += json_escape_string(name);
    out += R"(":")";
    out += json_escape_string(value);
    out += '"';
}

inline void append_json_optional_string_field(
    std::string& out,
    std::string_view name,
    const std::optional<std::string>& value,
    bool& first
) {
    if (value && !value->empty()) append_json_string_field(out, name, *value, first);
}

[[nodiscard]] inline SchemaProperty agent_schema_property(
    std::string name,
    std::string type,
    std::string description,
    bool required = false
) {
    return SchemaProperty{
        std::move(name),
        std::move(type),
        std::move(description),
        required,
        std::nullopt,
        std::nullopt
    };
}

struct AgentToolRequest {
    std::string prompt;
    std::optional<std::string> description;
    std::string subagent_type = "general-purpose";
    std::optional<std::string> model;
    bool run_in_background = false;
    std::optional<std::string> agent_id_override;
    bool resume_existing = false;
    std::optional<std::string> query_source;
    bool fork_child_context = false;
    std::optional<std::string> parent_system_prompt;
    std::vector<std::string> exact_tools;
    bool use_exact_tools = false;
    std::vector<std::string> fork_context_entries;
    std::vector<std::string> parent_assistant_message_entries;
    std::optional<std::string> name;
    std::optional<std::string> team_name;
    std::optional<std::string> mode;
    std::optional<std::string> isolation;
    std::optional<std::string> cwd;
};

struct AgentMcpToolBinding {
    std::string server_name;
    std::string tool_name;
    std::string description;
};

struct AgentExecutionPlan {
    std::string agent_id;
    std::string prompt;
    std::optional<std::string> description;
    std::string agent_type;
    bool is_built_in = false;
    std::string model;
    std::string system_prompt;
    std::vector<std::string> preloaded_skill_messages;
    std::vector<std::string> hook_additional_contexts;
    std::vector<std::string> agent_mcp_servers;
    std::vector<AgentMcpToolBinding> agent_mcp_tools;
    std::optional<std::string> agent_mcp_context_message;
    std::vector<AgentInlineMcpServerRuntimeState> inline_mcp_server_states;
    std::vector<std::string> allowed_tools;
    std::vector<std::string> disallowed_tools;
    int max_turns = 200;
    bool background = false;
    bool resume_existing = false;
    std::optional<std::string> query_source;
    bool fork_child_context = false;
    bool system_prompt_overridden = false;
    std::vector<std::string> exact_tools;
    bool use_exact_tools = false;
    std::vector<Message> fork_context_messages;
    bool fork_context_includes_prompt = false;
    std::optional<std::string> name;
    std::optional<std::string> team_name;
    std::optional<std::string> mode;
    std::optional<std::string> isolation;
    std::optional<std::string> working_dir;
    std::optional<std::string> worktree_path;
    std::optional<std::string> worktree_branch;
    std::optional<std::string> worktree_base_commit;
    std::optional<std::string> worktree_git_root;
    cc::tools::agent_runtime::AgentHooksByEvent frontmatter_hooks;
    std::optional<std::string> effort;
    std::optional<std::string> memory;
    std::optional<std::string> color;
    std::optional<std::string> teammate_backend;
    std::optional<std::string> teammate_task_id;
    std::optional<std::string> teammate_pane_id;
    std::optional<std::string> teammate_color;
    std::optional<std::string> parent_agent_id;
    std::optional<std::string> parent_session_id;
    bool omit_claude_md = false;
    std::optional<std::string> critical_system_reminder;
};

struct RemoteAgentLaunchMetadata {
    std::optional<std::string> task_id;
    std::string task_type = "remote-agent";
    std::optional<std::string> session_id;
    std::optional<std::string> session_url;
    std::optional<std::string> title;
    std::optional<std::string> metadata_json;
    bool is_review = false;
    bool is_ultraplan = false;
    bool is_long_running = false;
};

[[nodiscard]] inline bool is_auto_memory_enabled() {
    const char* disable = std::getenv("CLAUDE_CODE_DISABLE_AUTO_MEMORY");
    if (cc::utils::is_env_truthy(disable)) return false;
    if (cc::utils::is_env_defined_falsy(disable)) return true;
    if (cc::utils::is_env_truthy(std::getenv("CLAUDE_CODE_SIMPLE"))) return false;
    if (cc::utils::is_env_truthy(std::getenv("CLAUDE_CODE_REMOTE")) &&
        (!std::getenv("CLAUDE_CODE_REMOTE_MEMORY_DIR") || !*std::getenv("CLAUDE_CODE_REMOTE_MEMORY_DIR"))) {
        return false;
    }
    return true;
}

[[nodiscard]] inline fs::path claude_config_home_dir() {
    if (const char* configured = std::getenv("CLAUDE_CONFIG_DIR"); configured && *configured) {
        return fs::path{configured};
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return fs::path{home} / ".claude";
    }
    return fs::path{".claude"};
}

[[nodiscard]] inline fs::path agent_memory_base_dir() {
    if (const char* remote = std::getenv("CLAUDE_CODE_REMOTE_MEMORY_DIR"); remote && *remote) {
        return fs::path{remote};
    }
    return claude_config_home_dir();
}

[[nodiscard]] inline std::string sanitize_agent_memory_component(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        out.push_back(ch == ':' ? '-' : ch);
    }
    return out.empty() ? std::string{"agent"} : out;
}

[[nodiscard]] inline std::string agent_memory_dir(
    std::string_view agent_type,
    std::string_view scope,
    const std::optional<std::string>& working_dir
) {
    const auto dir_name = sanitize_agent_memory_component(agent_type);
    const auto cwd = working_dir && !working_dir->empty() ? fs::path{*working_dir} : fs::current_path();
    if (scope == "project") {
        return ((cwd / ".claude" / "agent-memory" / dir_name).string() + fs::path::preferred_separator);
    }
    if (scope == "local") {
        if (const char* remote = std::getenv("CLAUDE_CODE_REMOTE_MEMORY_DIR"); remote && *remote) {
            const auto git_root = cc::utils::git::find_git_root(cwd).value_or(cwd);
            const auto project_component = sanitize_agent_memory_component(git_root.string());
            return ((fs::path{remote} / "projects" / project_component / "agent-memory-local" / dir_name).string() +
                fs::path::preferred_separator);
        }
        return ((cwd / ".claude" / "agent-memory-local" / dir_name).string() + fs::path::preferred_separator);
    }
    return ((agent_memory_base_dir() / "agent-memory" / dir_name).string() + fs::path::preferred_separator);
}

[[nodiscard]] inline std::string agent_memory_scope_note(std::string_view scope) {
    if (scope == "project") {
        return "- Since this memory is project-scope and shared with your team via version control, tailor your memories to this project";
    }
    if (scope == "local") {
        return "- Since this memory is local-scope (not checked into version control), tailor your memories to this project and machine";
    }
    return "- Since this memory is user-scope, keep learnings general since they apply across all projects";
}

[[nodiscard]] inline std::string join_agent_memory_prompt_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) out += '\n';
        out += lines[i];
    }
    return out;
}

[[nodiscard]] inline std::string load_agent_memory_prompt(
    std::string_view agent_type,
    std::string_view scope,
    const std::optional<std::string>& working_dir
) {
    const auto dir = agent_memory_dir(agent_type, scope, working_dir);
    std::error_code ec;
    fs::create_directories(fs::path{dir}, ec);
    return join_agent_memory_prompt_lines({
        "# Persistent Agent Memory",
        "",
        "You have a persistent, file-based memory system at `" + dir + "`.",
        "This directory already exists; write to it directly with the Write tool.",
        "",
        "If the user explicitly asks you to remember something, save it immediately as the best fitting memory type. If they ask you to forget something, find and remove the relevant entry.",
        "",
        "## How to save memories",
        "Write each memory to its own file and keep `MEMORY.md` as a concise index of links to those files.",
        "Use descriptive filenames, avoid duplicate memories, and update or remove memories that are wrong or outdated.",
        "",
        "## When to access memories",
        "Access memory when it seems relevant, when the user refers to prior work, or when the user explicitly asks you to check, recall, or remember.",
        "If the user asks you to ignore memory, proceed as if `MEMORY.md` were empty.",
        "",
        agent_memory_scope_note(scope),
    });
}

inline void add_agent_memory_tools(std::vector<std::string>& tools) {
    if (tools.empty()) return;
    for (std::string_view tool : {"Write", "Edit", "Read"}) {
        if (!std::ranges::contains(tools, tool)) {
            tools.emplace_back(tool);
        }
    }
}

[[nodiscard]] inline std::string trim_remote_trigger_output(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

[[nodiscard]] inline std::string trim_ascii_copy(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

[[nodiscard]] inline std::optional<std::string> json_any_string(
    cc::utils::json::JsonVal root,
    std::initializer_list<std::string_view> keys
) {
    for (const auto key : keys) {
        auto value = root.get(key);
        if (value.is_str() && !value.as_str().empty()) return std::string(value.as_str());
    }
    return std::nullopt;
}

[[nodiscard]] inline bool json_any_bool(
    cc::utils::json::JsonVal root,
    std::initializer_list<std::string_view> keys,
    bool fallback = false
) {
    for (const auto key : keys) {
        auto value = root.get(key);
        if (value.is_bool()) return value.as_bool();
    }
    return fallback;
}

[[nodiscard]] inline std::optional<std::string> extract_embedded_json_object(std::string_view output) {
    const auto start = output.find('{');
    const auto end = output.rfind('}');
    if (start == std::string_view::npos || end == std::string_view::npos || end <= start) {
        return std::nullopt;
    }
    return std::string(output.substr(start, end - start + 1));
}

[[nodiscard]] inline RemoteAgentLaunchMetadata parse_remote_launch_metadata(
    std::string_view trigger_output,
    const AgentExecutionPlan& plan
) {
    RemoteAgentLaunchMetadata metadata;
    metadata.task_id = plan.agent_id;
    metadata.title = plan.description.value_or(plan.agent_id);

    const auto trimmed = trim_remote_trigger_output(trigger_output);
    auto parsed = cc::utils::json::parse(trimmed);
    if (!parsed) {
        if (auto embedded = extract_embedded_json_object(trimmed)) {
            parsed = cc::utils::json::parse(*embedded);
        }
    }
    if (!parsed || !parsed->root().is_obj()) {
        return metadata;
    }

    auto root = parsed->root();
    if (auto task_id = json_any_string(root, {"task_id", "taskId"})) metadata.task_id = std::move(*task_id);
    if (auto task_type = json_any_string(root, {"remote_task_type", "remoteTaskType", "task_type", "taskType"})) {
        metadata.task_type = std::move(*task_type);
    }
    if (auto session_id = json_any_string(root, {"session_id", "sessionId", "session"})) {
        metadata.session_id = std::move(*session_id);
    }
    if (auto session_url = json_any_string(root, {"session_url", "sessionUrl", "url"})) {
        metadata.session_url = std::move(*session_url);
    }
    if (!metadata.session_url && metadata.session_id) {
        metadata.session_url = std::format("https://claude.ai/chat/{}", *metadata.session_id);
    }
    if (auto title = json_any_string(root, {"title", "description"})) metadata.title = std::move(*title);
    if (auto metadata_json = json_any_string(root, {"remote_metadata_json", "remoteMetadataJson", "metadata"})) {
        metadata.metadata_json = std::move(*metadata_json);
    }
    metadata.is_review = json_any_bool(root, {"is_remote_review", "isRemoteReview"}, false);
    metadata.is_ultraplan = json_any_bool(root, {"is_ultraplan", "isUltraplan"}, metadata.task_type == "ultraplan");
    metadata.is_long_running = json_any_bool(root, {"is_long_running", "isLongRunning"}, false);
    return metadata;
}

[[nodiscard]] inline std::string remote_agent_payload_json(const AgentExecutionPlan& plan) {
    std::string payload = "{";
    bool first = true;
    append_json_string_field(payload, "agent_id", plan.agent_id, first);
    append_json_string_field(payload, "agent_type", plan.agent_type, first);
    append_json_string_field(payload, "prompt", plan.prompt, first);
    append_json_string_field(
        payload,
        "output_file",
        cc::tools::agent_runtime::agent_output_file_path(plan.agent_id).string(),
        first);
    append_json_string_field(payload, "model", plan.model, first);
    append_json_optional_string_field(payload, "description", plan.description, first);
    append_json_optional_string_field(payload, "cwd", plan.working_dir, first);
    append_json_optional_string_field(payload, "permission_mode", plan.mode, first);
    append_json_optional_string_field(payload, "effort", plan.effort, first);
    payload += '}';
    return payload;
}

[[nodiscard]] inline std::string remote_agent_trigger_input_json(const AgentExecutionPlan& plan) {
    if (const char* target = std::getenv("CC_REPL_REMOTE_AGENT_TARGET"); target && *target) {
        std::string input = "{";
        bool first = true;
        append_json_string_field(input, "target", target, first);
        append_json_string_field(input, "message", plan.prompt, first);
        if (!first) input += ',';
        first = false;
        input += R"("params":{)";
        bool params_first = true;
        append_json_string_field(input, "agent_id", plan.agent_id, params_first);
        append_json_string_field(input, "agent_type", plan.agent_type, params_first);
        append_json_string_field(input, "output_file", cc::tools::agent_runtime::agent_output_file_path(plan.agent_id).string(), params_first);
        append_json_optional_string_field(input, "cwd", plan.working_dir, params_first);
        input += "}}";
        return input;
    }

    std::string input = "{";
    bool first = true;
    append_json_string_field(input, "payload", remote_agent_payload_json(plan), first);
    input += '}';
    return input;
}

[[nodiscard]] inline std::optional<std::string> json_string(
    cc::utils::json::JsonVal root,
    std::string_view key
) {
    auto value = root.get(key);
    if (!value.is_str()) return std::nullopt;
    return std::string(value.as_str());
}

[[nodiscard]] inline bool json_bool(
    cc::utils::json::JsonVal root,
    std::string_view key,
    bool fallback = false
) {
    auto value = root.get(key);
    return value.is_bool() ? value.as_bool() : fallback;
}

[[nodiscard]] inline std::optional<int> json_int(
    cc::utils::json::JsonVal root,
    std::string_view key
) {
    auto value = root.get(key);
    if (!value.is_num()) return std::nullopt;
    return static_cast<int>(value.as_int());
}

[[nodiscard]] inline bool has_non_empty_string(
    cc::utils::json::JsonVal root,
    std::string_view key
) {
    auto value = root.get(key);
    return value.is_str() && !value.as_str().empty();
}

[[nodiscard]] inline std::optional<std::string> resolve_agent_relative_path(
    const std::optional<std::string>& working_dir,
    std::string_view value
) {
    if (!working_dir || working_dir->empty() || value.empty()) return std::nullopt;
    fs::path path{std::string(value)};
    if (path.is_absolute()) return path.lexically_normal().string();
    return (fs::path{*working_dir} / path).lexically_normal().string();
}

[[nodiscard]] inline std::string json_object_with_string_overrides(
    std::string_view raw_json,
    const std::unordered_map<std::string, std::string>& overrides
) {
    if (overrides.empty()) return std::string(raw_json);
    auto parsed = cc::utils::json::parse(raw_json);
    if (!parsed || !parsed->root().is_obj()) return std::string(raw_json);

    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();
    std::unordered_set<std::string> written;
    parsed->root().iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal value) {
        if (!key.is_str()) return;
        auto key_text = std::string(key.as_str());
        if (auto override = overrides.find(key_text); override != overrides.end()) {
            root.add(key_text, doc.string(override->second));
            written.insert(std::move(key_text));
            return;
        }
        root.add(key_text, doc.copy_val(value));
    });
    for (const auto& [key, value] : overrides) {
        if (!written.contains(key)) root.add(key, doc.string(value));
    }
    doc.set_root(root);
    return doc.to_string();
}

[[nodiscard]] inline std::vector<std::string> json_string_array(cc::utils::json::JsonVal value) {
    std::vector<std::string> out;
    if (!value.valid()) return out;
    if (value.is_arr()) {
        value.iter([&](cc::utils::json::JsonVal item) {
            if (item.is_str()) {
                out.emplace_back(item.as_str());
            } else if (item.valid()) {
                out.push_back(item.to_string());
            }
        });
    } else if (value.is_str()) {
        out.emplace_back(value.as_str());
    }
    return out;
}

[[nodiscard]] inline std::vector<std::string> json_string_array_field(
    cc::utils::json::JsonVal root,
    std::string_view key
) {
    return json_string_array(root.get(key));
}

[[nodiscard]] inline bool json_array_looks_like_content_blocks(cc::utils::json::JsonVal value) {
    if (!value.valid() || !value.is_arr() || value.size() == 0) return false;
    bool saw_content_block = false;
    bool saw_message = false;
    value.iter([&](cc::utils::json::JsonVal item) {
        if (item.is_str()) {
            saw_content_block = true;
            return;
        }
        if (!item.is_obj()) return;
        if (item.get("message").is_obj() || item.get("role").is_str()) {
            saw_message = true;
            return;
        }
        if (item.get("type").is_str()) saw_content_block = true;
    });
    return saw_content_block && !saw_message;
}

[[nodiscard]] inline std::string assistant_content_array_json_to_message_json(std::string_view content_json) {
    std::string out = R"({"role":"assistant","content":)";
    out += content_json;
    out += '}';
    return out;
}

[[nodiscard]] inline std::vector<std::string> json_message_entries(cc::utils::json::JsonVal value) {
    std::vector<std::string> entries;
    if (!value.valid()) return entries;
    if (value.is_str()) {
        entries.emplace_back(value.as_str());
        return entries;
    }
    if (value.is_obj()) {
        entries.push_back(value.to_string());
        return entries;
    }
    if (!value.is_arr()) return entries;

    if (json_array_looks_like_content_blocks(value)) {
        entries.push_back(assistant_content_array_json_to_message_json(value.to_string()));
        return entries;
    }

    value.iter([&](cc::utils::json::JsonVal item) {
        if (item.is_str()) {
            entries.emplace_back(item.as_str());
        } else if (item.valid()) {
            entries.push_back(item.to_string());
        }
    });
    return entries;
}

[[nodiscard]] inline std::vector<std::string> json_message_entries_field(
    cc::utils::json::JsonVal root,
    std::string_view key
) {
    return json_message_entries(root.get(key));
}

[[nodiscard]] inline bool agent_tool_input_omits_agent_type(std::string_view raw_json) {
    auto doc = cc::utils::json::parse(raw_json);
    if (!doc) return true;
    auto root = doc->root();
    if (!root.valid() || !root.is_obj()) return true;

    auto type = json_string(root, "subagent_type").or_else([&] { return json_string(root, "skill"); });
    return !type || type->empty();
}

[[nodiscard]] inline std::string next_agent_id(const std::optional<std::string>& preferred_name) {
    if (preferred_name && !preferred_name->empty()) return *preferred_name;
    static std::atomic<std::uint64_t> counter{0};
    return std::format("agent-{}", counter.fetch_add(1, std::memory_order_relaxed) + 1);
}

[[nodiscard]] inline std::string sanitize_teammate_agent_name(std::string name) {
    if (name.empty()) return "agent";
    for (auto& ch : name) {
        if (ch == '@') ch = '-';
    }
    return name;
}

[[nodiscard]] inline std::string lowercase_ascii(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

[[nodiscard]] inline std::string teammate_name_from_agent_id(std::string_view agent_id) {
    const auto at = agent_id.find('@');
    if (at == std::string_view::npos) return std::string{agent_id};
    return std::string{agent_id.substr(0, at)};
}

[[nodiscard]] inline std::string unique_teammate_agent_name(
    std::string base_name,
    const cc::tools::Team& team
) {
    if (base_name.empty()) base_name = "agent";

    std::unordered_set<std::string> existing_names;
    for (const auto& member : team.members) {
        existing_names.insert(lowercase_ascii(teammate_name_from_agent_id(member.agent_id)));
    }

    const auto base_key = lowercase_ascii(base_name);
    if (!existing_names.contains(base_key)) return base_name;

    int suffix = 2;
    while (existing_names.contains(lowercase_ascii(std::format("{}-{}", base_name, suffix)))) {
        ++suffix;
    }
    return std::format("{}-{}", base_name, suffix);
}

[[nodiscard]] inline std::string format_teammate_agent_id(std::string_view agent_name, std::string_view team_name) {
    return std::format("{}@{}", agent_name, team_name);
}

[[nodiscard]] inline bool current_session_is_teammate() {
    if (cc::utils::is_in_process_teammate()) return true;
    auto agent_id = cc::utils::get_agent_id();
    auto team_name = cc::utils::get_team_name();
    return agent_id && !agent_id->empty() && team_name && !team_name->empty();
}

[[nodiscard]] inline std::optional<std::string> resolve_agent_model(std::optional<std::string> model) {
    if (!model || model->empty() || *model == "inherit") return std::nullopt;
    if (*model == "sonnet") return "claude-sonnet-4-20250514";
    if (*model == "opus") return "claude-opus-4-20250514";
    if (*model == "haiku") return "claude-3-5-haiku-20241022";
    return model;
}

[[nodiscard]] inline std::expected<AgentToolRequest, std::string> parse_agent_tool_request(
    const ToolInput& input
) {
    auto doc = cc::utils::json::parse(input.json());
    if (!doc) return std::unexpected(std::string(doc.error().format()));

    auto root = doc->root();
    if (!root.is_obj()) return std::unexpected("Agent input must be a JSON object");

    AgentToolRequest request;
    if (auto prompt = json_string(root, "prompt").or_else([&] { return json_string(root, "task"); })) {
        request.prompt = *prompt;
    }
    request.description = json_string(root, "description");
    if (auto subagent = json_string(root, "subagent_type").or_else([&] { return json_string(root, "skill"); })) {
        if (!subagent->empty()) request.subagent_type = *subagent;
    }
    request.model = resolve_agent_model(json_string(root, "model"));
    request.run_in_background = json_bool(root, "run_in_background", false);
    request.agent_id_override = json_string(root, "agent_id").or_else([&] { return json_string(root, "agentId"); });
    request.resume_existing = json_bool(root, "resume_existing", false) ||
        json_bool(root, "resumeExisting", false);
    request.query_source = json_string(root, "query_source").or_else([&] { return json_string(root, "querySource"); });
    request.fork_child_context = json_bool(root, "fork_child", false) ||
        json_bool(root, "forkChild", false) ||
        (request.query_source && query_source_is_fork_child(*request.query_source)) ||
        text_contains_fork_boilerplate(request.prompt);
    request.parent_system_prompt = json_string(root, "parent_system_prompt")
        .or_else([&] { return json_string(root, "parentSystemPrompt"); })
        .or_else([&] { return json_string(root, "override_system_prompt"); })
        .or_else([&] { return json_string(root, "system_prompt_override"); });
    request.exact_tools = json_string_array_field(root, "exact_tools");
    if (request.exact_tools.empty()) request.exact_tools = json_string_array_field(root, "exactTools");
    if (request.exact_tools.empty()) request.exact_tools = json_string_array_field(root, "available_tools");
    if (request.exact_tools.empty()) request.exact_tools = json_string_array_field(root, "availableTools");
    request.use_exact_tools = json_bool(root, "use_exact_tools", false) ||
        json_bool(root, "useExactTools", false) ||
        !request.exact_tools.empty();
    request.fork_context_entries = json_string_array_field(root, "fork_context");
    if (request.fork_context_entries.empty()) {
        request.fork_context_entries = json_string_array_field(root, "forkContext");
    }
    if (request.fork_context_entries.empty()) {
        request.fork_context_entries = json_string_array_field(root, "fork_context_messages");
    }
    if (request.fork_context_entries.empty()) {
        request.fork_context_entries = json_string_array_field(root, "forkContextMessages");
    }
    request.parent_assistant_message_entries = json_message_entries_field(root, "parent_assistant_message");
    if (request.parent_assistant_message_entries.empty()) {
        request.parent_assistant_message_entries = json_message_entries_field(root, "parentAssistantMessage");
    }
    if (request.parent_assistant_message_entries.empty()) {
        request.parent_assistant_message_entries = json_message_entries_field(root, "assistant_message");
    }
    if (request.parent_assistant_message_entries.empty()) {
        request.parent_assistant_message_entries = json_message_entries_field(root, "assistantMessage");
    }
    if (request.parent_assistant_message_entries.empty()) {
        request.parent_assistant_message_entries = json_message_entries_field(root, "live_parent_assistant_message");
    }
    if (request.parent_assistant_message_entries.empty()) {
        request.parent_assistant_message_entries = json_message_entries_field(root, "liveParentAssistantMessage");
    }
    if (!request.parent_assistant_message_entries.empty()) request.fork_child_context = true;
    request.name = json_string(root, "name");
    request.team_name = json_string(root, "team_name").or_else([&] { return json_string(root, "teamName"); });
    request.mode = json_string(root, "mode")
        .or_else([&] { return json_string(root, "permission_mode"); })
        .or_else([&] { return json_string(root, "permissionMode"); });
    request.isolation = json_string(root, "isolation");
    request.cwd = json_string(root, "cwd");
    return request;
}

[[nodiscard]] inline std::string join_fields(const std::vector<std::string>& fields) {
    std::string output;
    for (const auto& field : fields) {
        if (!output.empty()) output += ", ";
        output += field;
    }
    return output;
}

[[nodiscard]] inline std::string built_in_system_prompt(std::string_view agent_type) {
    if (agent_type == "Explore") {
        return R"(You are an exploration agent. Your job is to understand codebases, find relevant files, and gather context.

Guidelines:
- Use search and read tools extensively.
- Summarize findings concisely.
- Identify key files, patterns, and architecture.
- Report dependencies and relationships between components.
- Do not make code changes.)";
    }
    if (agent_type == "Plan") {
        return R"(You are a planning agent. Your job is to create detailed implementation plans.

Guidelines:
- Break down tasks into clear, sequential steps.
- Identify risks and dependencies.
- Suggest testing strategies.
- Consider edge cases.
- Output a structured plan.
- Do not implement the plan yourself.)";
    }
    if (agent_type == "verification") {
        return R"(You are a verification agent. Your job is to test and validate completed work.

Guidelines:
- Run targeted checks when tools are available.
- Verify code compiles or builds successfully.
- Check for regressions.
- Validate that requirements are met.
- Report issues with specific evidence.)";
    }
    return R"(You are a sub-agent working on a delegated task. Complete the task thoroughly.

Guidelines:
- Focus only on the assigned task.
- Use available tools effectively.
- Report results concisely.
- If blocked, explain what is needed to proceed.)";
}

[[nodiscard]] inline std::string_view trim_tool_rule(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] inline std::string_view permission_rule_tool_name(std::string_view rule) {
    rule = trim_tool_rule(rule);
    const auto paren = rule.find('(');
    if (paren != std::string_view::npos) {
        rule = rule.substr(0, paren);
    }
    return trim_tool_rule(rule);
}

[[nodiscard]] inline std::string canonical_tool_name(std::string_view value) {
    auto trimmed = trim_tool_rule(value);
    std::string out;
    out.reserve(trimmed.size());
    for (char ch : trimmed) {
        if (ch == '_' || ch == '-' || ch == ' ') continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

[[nodiscard]] inline bool env_flag_enabled(const char* name) {
    const char* raw = std::getenv(name);
    if (!raw || *raw == '\0') return false;
    const auto value = lowercase_ascii(raw);
    return value != "0" && value != "false" && value != "no" && value != "off";
}

[[nodiscard]] inline bool is_ant_user() {
    if (const char* value = std::getenv("USER_TYPE")) {
        return std::string_view(value) == "ant";
    }
    return false;
}

[[nodiscard]] inline bool agent_model_supports_effort(std::string_view model) {
    if (env_flag_enabled("CLAUDE_CODE_ALWAYS_ENABLE_EFFORT")) return true;
    const auto lower = lowercase_ascii(model);
    if (lower.find("opus-4-6") != std::string::npos ||
        lower.find("sonnet-4-6") != std::string::npos) {
        return true;
    }
    if (lower.find("haiku") != std::string::npos ||
        lower.find("sonnet") != std::string::npos ||
        lower.find("opus") != std::string::npos) {
        return false;
    }
    return true;
}

[[nodiscard]] inline bool agent_model_supports_max_effort(std::string_view model) {
    return is_ant_user() || lowercase_ascii(model).find("opus-4-6") != std::string::npos;
}

[[nodiscard]] inline bool is_agent_effort_level(std::string_view value) {
    return value == "low" || value == "medium" || value == "high" || value == "max";
}

[[nodiscard]] inline std::optional<std::string> normalized_permission_mode(
    const std::optional<std::string>& mode
) {
    if (!mode) return std::nullopt;
    const auto trimmed = trim_tool_rule(*mode);
    if (trimmed.empty()) return std::nullopt;
    return std::string{trimmed};
}

[[nodiscard]] inline bool parent_permission_mode_blocks_agent_override(
    const std::optional<std::string>& parent_mode
) {
    const auto mode = normalized_permission_mode(parent_mode);
    if (!mode) return false;
    return *mode == "bypassPermissions" || *mode == "acceptEdits" || *mode == "auto";
}

[[nodiscard]] inline std::optional<std::string> effective_agent_permission_mode(
    const std::optional<std::string>& request_mode,
    const std::optional<std::string>& definition_mode,
    const std::optional<std::string>& parent_mode
) {
    if (auto explicit_mode = normalized_permission_mode(request_mode)) return explicit_mode;
    auto normalized_parent_mode = normalized_permission_mode(parent_mode);
    if (parent_permission_mode_blocks_agent_override(normalized_parent_mode)) {
        return normalized_parent_mode;
    }
    if (auto agent_mode = normalized_permission_mode(definition_mode)) return agent_mode;
    return normalized_parent_mode;
}

[[nodiscard]] inline std::optional<int> parse_agent_numeric_effort(std::string_view value) {
    value = trim_tool_rule(value);
    if (value.empty()) return std::nullopt;
    int parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end) return std::nullopt;
    return parsed;
}

inline void append_agent_effort_beta(CreateMessageRequest& request) {
    static constexpr std::string_view k_effort_beta_header = "effort-2025-11-24";
    if (!std::ranges::contains(request.betas, k_effort_beta_header)) {
        request.betas.emplace_back(k_effort_beta_header);
    }
}

inline void apply_agent_effort_to_request(
    CreateMessageRequest& request,
    const std::optional<std::string>& effort
) {
    if (!effort) return;
    const auto trimmed = trim_tool_rule(*effort);
    if (trimmed.empty()) return;

    if (auto numeric = parse_agent_numeric_effort(trimmed)) {
        if (is_ant_user()) {
            request.internal_effort_override = *numeric;
        }
        return;
    }

    if (!agent_model_supports_effort(request.model)) return;
    auto level = lowercase_ascii(trimmed);
    if (!is_agent_effort_level(level)) return;
    if (level == "max" && !agent_model_supports_max_effort(request.model)) {
        level = "high";
    }
    request.output_effort = std::move(level);
    append_agent_effort_beta(request);
}

[[nodiscard]] inline bool tool_rule_matches_tool_name(
    std::string_view rule,
    std::string_view tool_name
) {
    const auto parsed = permission_rule_tool_name(rule);
    return parsed == "*" || canonical_tool_name(parsed) == canonical_tool_name(tool_name);
}

[[nodiscard]] inline bool normalized_tool_name_is(std::string_view tool_name, std::string_view expected) {
    return canonical_tool_name(tool_name) == canonical_tool_name(expected);
}

[[nodiscard]] inline bool normalized_tool_name_in(
    std::string_view tool_name,
    std::initializer_list<std::string_view> names
) {
    const auto normalized = canonical_tool_name(tool_name);
    for (auto name : names) {
        if (normalized == canonical_tool_name(name)) return true;
    }
    return false;
}

[[nodiscard]] inline bool is_mcp_tool_name(std::string_view tool_name) {
    return normalized_tool_name_is(tool_name, "mcp") || tool_name.starts_with("mcp__");
}

[[nodiscard]] inline std::string apply_agent_tool_execution_context_to_input(
    std::string_view tool_name,
    std::string_view raw_json,
    const AgentExecutionPlan& plan
) {
    auto scoped_json = (is_todo_write_tool_name(tool_name) ||
                        is_agent_scoped_shell_tool_name(tool_name))
        ? inject_agent_id_into_tool_input(raw_json, plan.agent_id)
        : std::string(raw_json);
    if (!plan.working_dir || plan.working_dir->empty()) return scoped_json;

    auto parsed = cc::utils::json::parse(scoped_json);
    if (!parsed || !parsed->root().is_obj()) return scoped_json;
    auto root = parsed->root();
    std::unordered_map<std::string, std::string> overrides;

    auto override_relative_path_field = [&](std::string_view key) {
        auto value = json_string(root, key);
        if (!value || value->empty()) return;
        if (auto resolved = resolve_agent_relative_path(plan.working_dir, *value)) {
            overrides.emplace(std::string(key), std::move(*resolved));
        }
    };
    auto default_or_override_path_field = [&](std::string_view key) {
        auto value = json_string(root, key);
        if (!value || value->empty()) {
            overrides.emplace(std::string(key), *plan.working_dir);
            return;
        }
        if (auto resolved = resolve_agent_relative_path(plan.working_dir, *value)) {
            overrides.emplace(std::string(key), std::move(*resolved));
        }
    };

    if (is_agent_scoped_shell_tool_name(tool_name)) {
        auto cwd = json_string(root, "cwd");
        if (!cwd || cwd->empty()) {
            overrides.emplace("cwd", *plan.working_dir);
        } else if (auto resolved = resolve_agent_relative_path(plan.working_dir, *cwd)) {
            overrides.emplace("cwd", std::move(*resolved));
        }
    } else if (normalized_tool_name_in(tool_name, {"Read", "Write", "Edit"})) {
        override_relative_path_field("file_path");
    } else if (normalized_tool_name_in(tool_name, {"notebook_edit", "NotebookEdit"})) {
        override_relative_path_field("notebook_path");
    } else if (normalized_tool_name_in(tool_name, {"Grep", "Glob"})) {
        default_or_override_path_field("path");
    }

    return json_object_with_string_overrides(scoped_json, overrides);
}

[[nodiscard]] inline bool all_agent_disallows_tool(std::string_view tool_name) {
    const bool nested_agents_enabled = [] {
        if (const char* value = std::getenv("USER_TYPE"); value && std::string_view(value) == "ant") {
            return true;
        }
        if (const char* value = std::getenv("CC_REPL_ENABLE_NESTED_AGENTS"); value && *value) {
            return true;
        }
        return false;
    }();
    if (!nested_agents_enabled && normalized_tool_name_is(tool_name, "Agent")) return true;

    return normalized_tool_name_in(tool_name, {
        "task_output",
        "TaskOutput",
        "exit_plan_mode",
        "ExitPlanMode",
        "enter_plan_mode",
        "EnterPlanMode",
        "ask_user_question",
        "AskUserQuestion",
        "ask_user",
        "task_stop",
        "TaskStop",
        "workflow",
        "Workflow",
    });
}

[[nodiscard]] inline bool custom_agent_disallows_tool(std::string_view tool_name) {
    return all_agent_disallows_tool(tool_name);
}

[[nodiscard]] inline bool async_agent_allows_tool(std::string_view tool_name) {
    return normalized_tool_name_in(tool_name, {
        "Read",
        "WebSearch",
        "todo_write",
        "TodoWrite",
        "Grep",
        "WebFetch",
        "Glob",
        "Bash",
        "powershell",
        "Edit",
        "Write",
        "notebook_edit",
        "NotebookEdit",
        "skill",
        "Skill",
        "synthetic_output",
        "SyntheticOutput",
        "tool_search",
        "ToolSearch",
        "sleep",
        "Sleep",
        "enter_worktree",
        "EnterWorktree",
        "exit_worktree",
        "ExitWorktree",
    });
}

[[nodiscard]] inline bool in_process_teammate_allows_tool(std::string_view tool_name) {
    return normalized_tool_name_in(tool_name, {
        "task_create",
        "TaskCreate",
        "task_get",
        "TaskGet",
        "task_list",
        "TaskList",
        "task_update",
        "TaskUpdate",
        "send_message",
        "SendMessage",
        "schedule_cron",
        "CronCreate",
        "CronDelete",
        "CronList",
    });
}

[[nodiscard]] inline bool agent_base_filter_allows_tool(
    std::string_view tool_name,
    bool is_built_in,
    bool is_async,
    std::optional<std::string_view> permission_mode = std::nullopt,
    bool is_in_process_teammate = false
) {
    if (is_mcp_tool_name(tool_name)) return true;
    if (normalized_tool_name_is(tool_name, "exit_plan_mode") &&
        permission_mode && *permission_mode == "plan") {
        return true;
    }
    if (all_agent_disallows_tool(tool_name)) return false;
    if (!is_built_in && custom_agent_disallows_tool(tool_name)) return false;
    if (is_async && !async_agent_allows_tool(tool_name)) {
        if (is_in_process_teammate) {
            if (normalized_tool_name_is(tool_name, "Agent")) return true;
            if (in_process_teammate_allows_tool(tool_name)) return true;
        }
        return false;
    }
    return true;
}

[[nodiscard]] inline std::string canonical_tool_rule_value(std::string_view value) {
    auto trimmed = trim_tool_rule(value);
    std::string out;
    out.reserve(trimmed.size());
    for (char ch : trimmed) {
        if (ch == '_' || ch == ' ') {
            out.push_back('-');
        } else {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return out;
}

[[nodiscard]] inline std::vector<std::string> permission_rule_arguments(std::string_view rule) {
    rule = trim_tool_rule(rule);
    const auto open = rule.find('(');
    const auto close = rule.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open + 1) {
        return {};
    }

    std::vector<std::string> values;
    auto args = rule.substr(open + 1, close - open - 1);
    std::size_t start = 0;
    while (start <= args.size()) {
        auto sep = args.find(',', start);
        auto part = trim_tool_rule(args.substr(
            start,
            sep == std::string_view::npos ? std::string_view::npos : sep - start));
        if (!part.empty()) values.push_back(canonical_tool_rule_value(part));
        if (sep == std::string_view::npos) break;
        start = sep + 1;
    }
    return values;
}

[[nodiscard]] inline bool agent_type_matches_permission_rule(
    std::string_view rule,
    std::string_view agent_type
) {
    if (!tool_rule_matches_tool_name(rule, "Agent")) return false;
    if (permission_rule_tool_name(rule) == "*") return true;

    auto allowed_types = permission_rule_arguments(rule);
    if (allowed_types.empty()) return true;

    const auto requested = canonical_tool_rule_value(agent_type);
    return std::ranges::any_of(allowed_types, [&](const auto& allowed) {
        return allowed == requested;
    });
}

[[nodiscard]] inline bool agent_type_allowed_by_permission_rules(
    std::string_view agent_type,
    const std::vector<std::string>& allowed_tools,
    const std::vector<std::string>& denied_tools
) {
    for (const auto& denied : denied_tools) {
        if (agent_type_matches_permission_rule(denied, agent_type)) return false;
    }

    if (allowed_tools.empty()) return true;
    for (const auto& allowed : allowed_tools) {
        if (agent_type_matches_permission_rule(allowed, agent_type)) return true;
    }
    return false;
}

[[nodiscard]] inline bool tool_name_allowed_by_definition(
    std::string_view tool_name,
    const std::vector<std::string>& allowed_tools
) {
    if (allowed_tools.empty()) return true;
    for (const auto& allowed : allowed_tools) {
        if (tool_rule_matches_tool_name(allowed, tool_name)) return true;
    }
    return false;
}

[[nodiscard]] inline bool tool_name_disallowed_by_definition(
    std::string_view tool_name,
    const std::vector<std::string>& disallowed_tools
) {
    for (const auto& disallowed : disallowed_tools) {
        if (tool_rule_matches_tool_name(disallowed, tool_name)) return true;
    }
    return false;
}

[[nodiscard]] inline std::string lowercase_copy(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    return out;
}

[[nodiscard]] inline bool case_insensitive_contains(std::string_view haystack, std::string_view needle) {
    return lowercase_copy(haystack).contains(lowercase_copy(needle));
}

[[nodiscard]] inline std::vector<std::string> available_mcp_servers_with_tools() {
    std::vector<std::string> names;
    for (const auto& status : cc::tools::native_mcp_statuses()) {
        if (status.status == "ready" && !status.tools.empty()) {
            names.push_back(status.name);
        }
    }
    return names;
}

[[nodiscard]] inline std::vector<std::string> missing_required_mcp_servers(
    const std::vector<std::string>& required_patterns,
    const std::vector<std::string>& available_servers
) {
    std::vector<std::string> missing;
    for (const auto& pattern : required_patterns) {
        bool matched = false;
        for (const auto& server : available_servers) {
            if (case_insensitive_contains(server, pattern)) {
                matched = true;
                break;
            }
        }
        if (!matched) missing.push_back(pattern);
    }
    return missing;
}

[[nodiscard]] inline std::string prepend_initial_prompt(
    const std::optional<std::string>& initial_prompt,
    std::string_view prompt
) {
    if (!initial_prompt || initial_prompt->empty()) return std::string(prompt);
    return std::format("{}\n\n{}", *initial_prompt, prompt);
}

[[nodiscard]] inline std::string format_critical_system_reminder(std::string_view reminder) {
    return std::format("<critical_system_reminder>\n{}\n</critical_system_reminder>", reminder);
}

[[nodiscard]] inline std::optional<std::string> resolve_agent_skill_name(
    std::string_view skill_name,
    const std::vector<cc::skills::SkillDefinition>& skills,
    const cc::tools::agent_runtime::AgentDefinition& agent_definition
) {
    for (const auto& skill : skills) {
        if (skill.name == skill_name) return skill.name;
    }

    const auto colon = agent_definition.agent_type.find(':');
    if (colon != std::string::npos) {
        const auto qualified = std::format("{}:{}", agent_definition.agent_type.substr(0, colon), skill_name);
        for (const auto& skill : skills) {
            if (skill.name == qualified) return skill.name;
        }
    }

    const auto suffix = std::format(":{}", skill_name);
    for (const auto& skill : skills) {
        if (skill.name.ends_with(suffix)) return skill.name;
    }

    return std::nullopt;
}

[[nodiscard]] inline std::string format_preloaded_skill_message(
    std::string_view skill_name,
    std::string_view content
) {
    return std::format("<skill name=\"{}\">\n{}\n</skill>", skill_name, content);
}

[[nodiscard]] inline std::vector<std::string> load_preloaded_skill_messages(
    const cc::tools::agent_runtime::AgentDefinition& definition
) {
    std::vector<std::string> messages;
    if (definition.skills.empty()) return messages;

    cc::skills::SkillLoader loader;
    std::vector<std::pair<std::string, fs::path>> plugin_skill_paths;
    for (const auto& plugin : cc::tools::agent_runtime::discover_plugin_component_paths()) {
        for (const auto& path : plugin.skills_paths) {
            plugin_skill_paths.emplace_back(plugin.plugin_name, path);
        }
    }
    auto discovered = loader.discover_all_with_plugin_skills(plugin_skill_paths);
    if (!discovered) return messages;

    for (const auto& requested : definition.skills) {
        auto resolved_name = resolve_agent_skill_name(requested, *discovered, definition);
        if (!resolved_name) continue;

        for (const auto& skill : *discovered) {
            if (skill.name != *resolved_name) continue;
            messages.push_back(format_preloaded_skill_message(skill.name, skill.content));
            break;
        }
    }
    return messages;
}

[[nodiscard]] inline std::string format_agent_mcp_context_message(
    const std::vector<AgentMcpToolBinding>& tools
) {
    if (tools.empty()) return {};

    std::string message = "The following MCP tools are available to this agent through the `mcp` tool.\n";
    message += "Call `mcp` with `server_name`, `tool_name`, and `arguments`.\n\n";
    for (const auto& tool : tools) {
        message += std::format("- {}/{}", tool.server_name, tool.tool_name);
        if (!tool.description.empty()) message += std::format(": {}", tool.description);
        message += "\n";
    }
    return message;
}

[[nodiscard]] inline std::vector<AgentMcpToolBinding> connect_agent_mcp_servers(
    const std::vector<std::string>& server_names
) {
    std::vector<AgentMcpToolBinding> tools;
    for (const auto& server_name : server_names) {
        if (server_name.empty()) continue;

        auto status = cc::tools::restart_native_mcp_server(server_name);
        if (!status || status->status != "ready") continue;

        for (const auto& tool : status->tools) {
            tools.push_back(AgentMcpToolBinding{
                .server_name = status->name,
                .tool_name = tool.name,
                .description = tool.description,
            });
        }
    }
    return tools;
}

[[nodiscard]] inline cc::tools::NativeMcpConfiguredServer to_native_agent_mcp_server(
    const cc::tools::agent_runtime::AgentInlineMcpServerConfig& config
) {
    cc::tools::NativeMcpConfiguredServer server;
    server.name = config.name;
    server.command = config.command;
    server.args = config.args;
    server.env = config.env;
    server.url = config.url;
    server.headers = config.headers;
    server.headers_helper = config.headers_helper;

    const auto transport = lowercase_copy(config.transport);
    if (transport == "sse") {
        server.transport = cc::services::mcp::TransportType::Sse;
    } else if (transport == "http" || transport == "streamable-http" || transport == "streamablehttp" ||
               (!server.url.empty() && server.command.empty())) {
        server.transport = cc::services::mcp::TransportType::StreamableHttp;
    } else {
        server.transport = cc::services::mcp::TransportType::Stdio;
    }
    return server;
}

inline void append_unique_agent_mcp_server(
    std::vector<std::string>& names,
    std::string name
) {
    if (name.empty()) return;
    for (const auto& existing : names) {
        if (existing == name) return;
    }
    names.push_back(std::move(name));
}

[[nodiscard]] inline std::expected<std::vector<AgentInlineMcpServerRuntimeState>, std::string>
prepare_agent_inline_mcp_servers(
    const std::vector<cc::tools::agent_runtime::AgentInlineMcpServerConfig>& configs
) {
    std::vector<AgentInlineMcpServerRuntimeState> states;
    if (configs.empty()) return states;

    std::vector<cc::tools::NativeMcpConfiguredServer> servers;
    servers.reserve(configs.size());
    for (const auto& config : configs) {
        if (config.name.empty()) continue;
        const bool already_tracked = std::ranges::any_of(states, [&](const auto& state) {
            return state.name == config.name;
        });
        if (!already_tracked) {
            states.push_back(AgentInlineMcpServerRuntimeState{
                .name = config.name,
                .previous_config = cc::tools::native_mcp_configured_server(config.name),
            });
        }
        servers.push_back(to_native_agent_mcp_server(config));
    }
    if (servers.empty()) return states;
    if (auto upserted = cc::tools::upsert_native_mcp_servers(std::move(servers)); !upserted) {
        return std::unexpected(upserted.error());
    }
    return states;
}

[[nodiscard]] inline std::string format_agent_runtime_context(
    const AgentExecutionPlan& plan
) {
    std::string context = "Native agent runtime context:\n";
    context += std::format("- agent_id: {}\n", plan.agent_id);
    if (plan.description) context += std::format("- description: {}\n", *plan.description);
    if (plan.name) context += std::format("- name: {}\n", *plan.name);
    if (plan.team_name) context += std::format("- team_name: {}\n", *plan.team_name);
    if (plan.working_dir) context += std::format("- cwd: {}\n", *plan.working_dir);
    if (plan.isolation) context += std::format("- isolation: {}\n", *plan.isolation);
    if (plan.mode) context += std::format("- permission_mode: {}\n", *plan.mode);
    if (plan.effort) context += std::format("- effort: {}\n", *plan.effort);
    if (plan.memory) context += std::format("- memory: {}\n", *plan.memory);
    if (plan.color) context += std::format("- color: {}\n", *plan.color);
    if (plan.omit_claude_md) context += "- omit_claude_md: true\n";
    if (plan.critical_system_reminder) context += "- critical_system_reminder: configured\n";
    if (plan.parent_agent_id) context += std::format("- parent_agent_id: {}\n", *plan.parent_agent_id);
    if (plan.background) context += "- background: true\n";
    return context;
}

[[nodiscard]] inline std::string shell_quote(std::string_view value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
}

[[nodiscard]] inline std::string sanitized_agent_file_part(std::string_view agent_id) {
    std::string out;
    out.reserve(agent_id.size());
    for (char ch : agent_id) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "agent" : out;
}

[[nodiscard]] inline std::string default_agent_transcript_path(std::string_view agent_id) {
    return cc::tools::agent_runtime::agent_transcript_path(agent_id).string();
}

struct AgentWorktreeInfo {
    fs::path path;
    std::string branch;
    std::string head_commit;
    fs::path git_root;
};

[[nodiscard]] inline std::expected<AgentWorktreeInfo, std::string> create_agent_worktree(
    const AgentExecutionPlan& plan
) {
    auto base_cwd = plan.working_dir ? fs::path{*plan.working_dir} : fs::current_path();
    auto git_root = cc::utils::git::find_git_root(base_cwd);
    if (!git_root) {
        return std::unexpected(
            "Cannot create agent worktree: not in a git repository. Use cwd inside a git repository or omit isolation=worktree.");
    }

    auto slug = sanitized_agent_file_part(plan.agent_id);
    if (slug.size() > 40) slug.resize(40);
    auto branch = "cc-agent-" + slug;
    auto worktree_path = *git_root / ".claude" / "worktrees" / slug;

    std::error_code ec;
    fs::create_directories(worktree_path.parent_path(), ec);
    if (ec) return std::unexpected("Cannot create worktree parent directory: " + ec.message());

    auto head = cc::utils::git::run_git_command("rev-parse HEAD", *git_root);
    if (!head.success) return std::unexpected("Cannot resolve git HEAD for agent worktree: " + head.output);

    auto created = cc::utils::git::run_git_command(
        std::format("worktree add -B \"{}\" \"{}\" HEAD", branch, worktree_path.string()),
        *git_root);
    if (!created.success) {
        return std::unexpected("Failed to create agent worktree: " + created.output);
    }

    return AgentWorktreeInfo{
        .path = std::move(worktree_path),
        .branch = std::move(branch),
        .head_commit = std::move(head.output),
        .git_root = std::move(*git_root),
    };
}

[[nodiscard]] inline bool hook_condition_allows(const cc::tools::agent_runtime::AgentHookCommand& hook) {
    if (!hook.condition || hook.condition->empty()) return true;
    const auto condition = lowercase_copy(*hook.condition);
    return condition == "true" || condition == "1" || condition == "yes";
}

[[nodiscard]] inline bool hook_pattern_matches_one(std::string_view pattern, std::string_view value) {
    if (pattern.empty() || pattern == "*") return true;
    if (pattern == value) return true;
    if (pattern.ends_with("*")) return value.starts_with(pattern.substr(0, pattern.size() - 1));
    if (pattern.starts_with("*")) return value.ends_with(pattern.substr(1));
    return false;
}

[[nodiscard]] inline bool hook_pattern_matches(
    const std::optional<std::string>& pattern,
    std::string_view value
) {
    if (!pattern || pattern->empty()) return true;
    std::size_t start = 0;
    while (start <= pattern->size()) {
        auto sep = pattern->find('|', start);
        auto part = std::string_view(*pattern).substr(
            start,
            sep == std::string::npos ? std::string_view::npos : sep - start);
        if (hook_pattern_matches_one(part, value)) return true;
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return false;
}

struct AgentHookRunResult {
    int exit_code = 0;
    std::string output;
};

struct AgentToolHookContext {
    std::string_view tool_name;
    std::string_view tool_input_json;
    std::string_view tool_use_id;
    std::string_view tool_output_preview;
    std::string_view tool_error;
};

[[nodiscard]] inline AgentHookRunResult run_agent_command_hook(
    const cc::tools::agent_runtime::AgentHookCommand& hook,
    const AgentExecutionPlan& plan,
    std::string_view event,
    std::string_view last_assistant_message,
    std::optional<AgentToolHookContext> tool_context = std::nullopt
) {
    const auto transcript_path = default_agent_transcript_path(plan.agent_id);
    const auto cwd = plan.working_dir.value_or(fs::current_path().string());
    const auto shell = hook.shell.empty() ? std::string("bash") : hook.shell;
    std::string command;
    command += "cd " + shell_quote(cwd) + " && ";
    command += "CLAUDE_HOOK_EVENT=" + shell_quote(event) + " ";
    command += "CLAUDE_HOOK_AGENT_ID=" + shell_quote(plan.agent_id) + " ";
    command += "CLAUDE_HOOK_AGENT_TYPE=" + shell_quote(plan.agent_type) + " ";
    command += "CLAUDE_HOOK_AGENT_TRANSCRIPT_PATH=" + shell_quote(transcript_path) + " ";
    command += "CLAUDE_HOOK_CWD=" + shell_quote(cwd) + " ";
    if (!last_assistant_message.empty()) {
        command += "CLAUDE_HOOK_LAST_ASSISTANT_MESSAGE=" + shell_quote(last_assistant_message) + " ";
    }
    if (tool_context) {
        command += "CLAUDE_HOOK_TOOL_NAME=" + shell_quote(tool_context->tool_name) + " ";
        command += "CLAUDE_HOOK_TOOL_INPUT_JSON=" + shell_quote(tool_context->tool_input_json) + " ";
        command += "CLAUDE_HOOK_TOOL_USE_ID=" + shell_quote(tool_context->tool_use_id) + " ";
        if (!tool_context->tool_output_preview.empty()) {
            command += "CLAUDE_HOOK_TOOL_OUTPUT_PREVIEW=" + shell_quote(tool_context->tool_output_preview) + " ";
        }
        if (!tool_context->tool_error.empty()) {
            command += "CLAUDE_HOOK_TOOL_ERROR=" + shell_quote(tool_context->tool_error) + " ";
        }
    }
    command += shell_quote(shell) + " -c " + shell_quote(hook.command) + " 2>&1";

    AgentHookRunResult result;
    FILE* pipe = ::popen(command.c_str(), "r");
    if (!pipe) {
        result.exit_code = 127;
        result.output = "failed to start hook command";
        return result;
    }

    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        result.output += buffer.data();
        if (result.output.size() > 64 * 1024) {
            result.output.resize(64 * 1024);
            break;
        }
    }
    const auto status = ::pclose(pipe);
    if (status == -1) {
        result.exit_code = 127;
    } else if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}

[[nodiscard]] inline std::string agent_hook_output_preview(std::string_view output) {
    constexpr std::size_t kMaxPreviewBytes = 16 * 1024;
    auto preview = std::string(output.substr(0, std::min(output.size(), kMaxPreviewBytes)));
    if (output.size() > kMaxPreviewBytes) preview += "\n[truncated]";
    return preview;
}

struct AgentHookExecutionResult {
    int hook_count = 0;
    std::string output;
    std::vector<std::string> additional_contexts;
    std::optional<std::string> error;
    std::optional<std::string> updated_input_json;
    std::optional<std::string> updated_mcp_tool_output_text;
    bool prevent_continuation = false;
    std::optional<std::string> stop_reason;

    [[nodiscard]] bool ok() const { return !error.has_value(); }
};

struct AgentHookContinuationStop {
    std::optional<std::string> stop_reason;
};

[[nodiscard]] inline std::optional<AgentHookContinuationStop> hook_continuation_stop_from_output(
    std::string_view output
) {
    output = trim_tool_rule(output);
    if (output.empty() || !output.starts_with('{')) return std::nullopt;

    auto parsed = cc::utils::json::parse(output);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto root = parsed->root();
    auto should_continue = root.get("continue");
    if (!should_continue.valid() || !should_continue.is_bool() || should_continue.as_bool()) {
        return std::nullopt;
    }

    AgentHookContinuationStop stop;
    auto reason = root.get("stopReason");
    if (reason.valid() && reason.is_str() && !reason.as_str().empty()) {
        stop.stop_reason = std::string(reason.as_str());
    }
    return stop;
}

[[nodiscard]] inline std::optional<std::string> hook_additional_context_from_output(std::string_view output) {
    output = trim_tool_rule(output);
    if (output.empty()) return std::nullopt;

    auto parsed = cc::utils::json::parse(output);
    if (!parsed) return std::nullopt;
    auto specific = parsed->root().get("hookSpecificOutput");
    if (!specific.valid() || !specific.is_obj()) return std::nullopt;
    auto event = specific.get("hookEventName");
    if (event.valid() && event.is_str() && event.as_str() != "SubagentStart") {
        return std::nullopt;
    }
    auto context = specific.get("additionalContext");
    if (!context.valid() || !context.is_str()) return std::nullopt;
    auto text = trim_tool_rule(context.as_str());
    if (text.empty()) return std::nullopt;
    return std::string(text);
}

[[nodiscard]] inline std::optional<std::string> pre_tool_hook_denial_reason(std::string_view output) {
    output = trim_tool_rule(output);
    if (output.empty() || !output.starts_with('{')) return std::nullopt;

    auto parsed = cc::utils::json::parse(output);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto root = parsed->root();
    if (auto decision = json_string(root, "decision"); decision && *decision == "block") {
        return json_string(root, "reason").value_or("Blocked by hook");
    }

    auto specific = root.get("hookSpecificOutput");
    if (!specific.valid() || !specific.is_obj()) return std::nullopt;
    auto event = specific.get("hookEventName");
    if (event.valid() && event.is_str() && event.as_str() != "PreToolUse") return std::nullopt;
    auto permission = specific.get("permissionDecision");
    if (!permission.valid() || !permission.is_str() || permission.as_str() != "deny") return std::nullopt;
    auto reason = specific.get("permissionDecisionReason");
    if (reason.valid() && reason.is_str() && !reason.as_str().empty()) return std::string(reason.as_str());
    return json_string(root, "reason").value_or("Blocked by hook");
}

[[nodiscard]] inline std::optional<std::string> pre_tool_hook_updated_input_json(std::string_view output) {
    output = trim_tool_rule(output);
    if (output.empty() || !output.starts_with('{')) return std::nullopt;

    auto parsed = cc::utils::json::parse(output);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto specific = parsed->root().get("hookSpecificOutput");
    if (!specific.valid() || !specific.is_obj()) return std::nullopt;
    auto event = specific.get("hookEventName");
    if (event.valid() && event.is_str() && event.as_str() != "PreToolUse") return std::nullopt;
    auto updated = specific.get("updatedInput");
    if (!updated.valid() || !updated.is_obj()) return std::nullopt;
    auto serialized = cc::utils::json::to_string(updated);
    if (serialized.empty()) return std::nullopt;
    return serialized;
}

[[nodiscard]] inline std::optional<std::string> post_tool_hook_updated_mcp_output_text(std::string_view output) {
    output = trim_tool_rule(output);
    if (output.empty() || !output.starts_with('{')) return std::nullopt;

    auto parsed = cc::utils::json::parse(output);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto specific = parsed->root().get("hookSpecificOutput");
    if (!specific.valid() || !specific.is_obj()) return std::nullopt;
    auto event = specific.get("hookEventName");
    if (event.valid() && event.is_str() && event.as_str() != "PostToolUse") return std::nullopt;
    auto updated = specific.get("updatedMCPToolOutput");
    if (!updated.valid()) return std::nullopt;
    if (updated.is_str()) return std::string(updated.as_str());
    auto serialized = cc::utils::json::to_string(updated);
    if (serialized.empty()) return std::nullopt;
    return serialized;
}

[[nodiscard]] inline std::optional<std::string> hook_additional_context_for_event(
    std::string_view output,
    std::string_view event_name
) {
    output = trim_tool_rule(output);
    if (output.empty() || !output.starts_with('{')) return std::nullopt;

    auto parsed = cc::utils::json::parse(output);
    if (!parsed) return std::nullopt;
    auto specific = parsed->root().get("hookSpecificOutput");
    if (!specific.valid() || !specific.is_obj()) return std::nullopt;
    auto event = specific.get("hookEventName");
    if (event.valid() && event.is_str() && event.as_str() != event_name) return std::nullopt;
    auto context = specific.get("additionalContext");
    if (!context.valid() || !context.is_str()) return std::nullopt;
    auto text = trim_tool_rule(context.as_str());
    if (text.empty()) return std::nullopt;
    return std::string(text);
}

[[nodiscard]] inline AgentHookExecutionResult execute_agent_frontmatter_hooks(
    const AgentExecutionPlan& plan,
    std::string_view event,
    std::string_view last_assistant_message = {}
) {
    AgentHookExecutionResult aggregate;
    auto it = plan.frontmatter_hooks.find(std::string(event));
    if (it == plan.frontmatter_hooks.end()) return aggregate;

    for (const auto& matcher : it->second) {
        if (!hook_pattern_matches(matcher.matcher, plan.agent_type)) continue;
        for (const auto& hook : matcher.hooks) {
            if (!hook_condition_allows(hook)) continue;
            auto result = run_agent_command_hook(hook, plan, event, last_assistant_message);
            ++aggregate.hook_count;
            if (!result.output.empty()) {
                if (!aggregate.output.empty()) aggregate.output += "\n";
                aggregate.output += result.output;
                if (event == "SubagentStart") {
                    if (auto context = hook_additional_context_from_output(result.output)) {
                        aggregate.additional_contexts.push_back(std::move(*context));
                    }
                }
            }
            if (result.exit_code != 0 && !aggregate.error) {
                aggregate.error = std::format(
                    "{} hook for agent '{}' exited with code {}{}{}",
                    event,
                    plan.agent_id,
                    result.exit_code,
                    result.output.empty() ? "" : ": ",
                    result.output.empty() ? "" : result.output);
            }
        }
    }
    return aggregate;
}

[[nodiscard]] inline AgentHookExecutionResult execute_agent_tool_frontmatter_hooks(
    const AgentExecutionPlan& plan,
    std::string_view event,
    std::string_view tool_name,
    std::string_view tool_input_json,
    std::string_view tool_use_id,
    std::string_view tool_output_preview = {},
    std::string_view tool_error = {}
) {
    AgentHookExecutionResult aggregate;
    auto it = plan.frontmatter_hooks.find(std::string(event));
    if (it == plan.frontmatter_hooks.end()) return aggregate;

    std::string current_tool_input_json{tool_input_json};
    for (const auto& matcher : it->second) {
        if (!hook_pattern_matches(matcher.matcher, tool_name)) continue;
        for (const auto& hook : matcher.hooks) {
            if (!hook_condition_allows(hook)) continue;
            AgentToolHookContext context{
                .tool_name = tool_name,
                .tool_input_json = current_tool_input_json,
                .tool_use_id = tool_use_id,
                .tool_output_preview = tool_output_preview,
                .tool_error = tool_error,
            };
            auto result = run_agent_command_hook(hook, plan, event, {}, context);
            ++aggregate.hook_count;
            if (!result.output.empty()) {
                if (!aggregate.output.empty()) aggregate.output += "\n";
                aggregate.output += result.output;
                if (auto context_text = hook_additional_context_for_event(result.output, event)) {
                    aggregate.additional_contexts.push_back(std::move(*context_text));
                }
                if (auto stop = hook_continuation_stop_from_output(result.output)) {
                    aggregate.prevent_continuation = true;
                    if (stop->stop_reason && !stop->stop_reason->empty()) {
                        aggregate.stop_reason = std::move(*stop->stop_reason);
                    }
                }
                if (event == "PreToolUse") {
                    if (auto denial = pre_tool_hook_denial_reason(result.output); denial && !aggregate.error) {
                        aggregate.error = std::move(*denial);
                    }
                    if (auto updated_input = pre_tool_hook_updated_input_json(result.output)) {
                        current_tool_input_json = *updated_input;
                        aggregate.updated_input_json = std::move(*updated_input);
                    }
                } else if (event == "PostToolUse") {
                    if (auto updated_output = post_tool_hook_updated_mcp_output_text(result.output)) {
                        aggregate.updated_mcp_tool_output_text = std::move(*updated_output);
                    }
                }
            }
            if (result.exit_code != 0 && !aggregate.error) {
                aggregate.error = std::format(
                    "{} hook for tool '{}' in agent '{}' exited with code {}{}{}",
                    event,
                    tool_name,
                    plan.agent_id,
                    result.exit_code,
                    result.output.empty() ? "" : ": ",
                    result.output.empty() ? "" : result.output);
            }
        }
    }
    return aggregate;
}

[[nodiscard]] inline std::string format_tool_hook_additional_context(
    std::string_view event,
    std::string_view tool_name,
    std::string_view tool_use_id,
    const std::vector<std::string>& contexts
) {
    std::string message = std::format(
        "<hook_additional_context hook=\"{}:{}\" tool_use_id=\"{}\">\n",
        event,
        tool_name,
        tool_use_id);
    for (std::size_t i = 0; i < contexts.size(); ++i) {
        if (i > 0) message += "\n\n";
        message += contexts[i];
    }
    message += "\n</hook_additional_context>";
    return message;
}

[[nodiscard]] inline std::string format_hook_additional_context_message(
    const std::vector<std::string>& contexts
) {
    std::string message =
        "<hook_additional_context hook=\"SubagentStart\">\n";
    for (std::size_t i = 0; i < contexts.size(); ++i) {
        if (i > 0) message += "\n\n";
        message += contexts[i];
    }
    message += "\n</hook_additional_context>";
    return message;
}

inline void append_hook_additional_context_messages(
    std::vector<Message>& messages,
    const std::vector<std::string>& contexts
) {
    if (contexts.empty()) return;
    messages.push_back(Message::from_text("user", format_hook_additional_context_message(contexts)));
}

[[nodiscard]] inline std::expected<std::optional<std::string>, std::string> normalize_agent_cwd(
    const std::optional<std::string>& cwd
) {
    if (!cwd || cwd->empty()) return std::optional<std::string>{};
    std::error_code ec;
    fs::path path = fs::path{*cwd};
    if (path.is_relative()) path = fs::current_path(ec) / path;
    if (ec) return std::unexpected(std::format("Cannot resolve current working directory: {}", ec.message()));
    path = fs::weakly_canonical(path, ec);
    if (ec) return std::unexpected(std::format("Cannot resolve agent cwd '{}': {}", *cwd, ec.message()));
    if (!fs::exists(path, ec) || !fs::is_directory(path, ec)) {
        return std::unexpected(std::format("Agent cwd does not exist or is not a directory: {}", path.string()));
    }
    return path.string();
}

inline void upsert_agent_record_for_plan(const AgentExecutionPlan& plan) {
    cc::tools::agent_runtime::NativeAgentRecord record{
        .agent_id = plan.agent_id,
        .agent_type = plan.agent_type,
        .parent_agent_id = plan.parent_agent_id,
        .description = plan.description,
        .name = plan.name,
        .team_name = plan.team_name,
        .cwd = plan.working_dir,
        .isolation = plan.isolation,
        .mode = plan.mode,
        .background = plan.background,
        .status = cc::tools::agent_runtime::NativeAgentStatus::Queued,
        .transcript_path = default_agent_transcript_path(plan.agent_id),
        .output_file_path = cc::tools::agent_runtime::agent_output_file_path(plan.agent_id).string(),
        .worktree_path = plan.worktree_path,
        .worktree_branch = plan.worktree_branch,
        .worktree_base_commit = plan.worktree_base_commit,
        .worktree_git_root = plan.worktree_git_root,
        .teammate_backend = plan.teammate_backend,
        .teammate_task_id = plan.teammate_task_id,
        .teammate_pane_id = plan.teammate_pane_id,
        .teammate_color = plan.teammate_color,
        .parent_session_id = plan.parent_session_id,
        .progress = 0.0,
    };
    record.capabilities = plan.allowed_tools;
    if (plan.fork_child_context && !std::ranges::contains(record.capabilities, "fork-subagent")) {
        record.capabilities.push_back("fork-subagent");
    }

    if (plan.resume_existing) {
        if (auto existing = cc::tools::agent_runtime::native_agent_store().get(plan.agent_id)) {
            record.parent_agent_id = record.parent_agent_id.or_else([&] { return existing->parent_agent_id; });
            record.description = record.description.or_else([&] { return existing->description; });
            record.name = record.name.or_else([&] { return existing->name; });
            record.team_name = record.team_name.or_else([&] { return existing->team_name; });
            record.cwd = record.cwd.or_else([&] { return existing->cwd; });
            record.isolation = record.isolation.or_else([&] { return existing->isolation; });
            record.mode = record.mode.or_else([&] { return existing->mode; });
            record.transcript_path = existing->transcript_path.value_or(default_agent_transcript_path(plan.agent_id));
            record.sidechain_jsonl_path = existing->sidechain_jsonl_path;
            record.output_file_path = existing->output_file_path.value_or(
                cc::tools::agent_runtime::agent_output_file_path(plan.agent_id).string());
            for (const auto& capability : existing->capabilities) {
                if (!std::ranges::contains(record.capabilities, capability)) {
                    record.capabilities.push_back(capability);
                }
            }
            if (plan.fork_child_context && !std::ranges::contains(record.capabilities, "fork-subagent")) {
                record.capabilities.push_back("fork-subagent");
            }
            record.transcript = std::move(existing->transcript);
            record.sidechain_entries = std::move(existing->sidechain_entries);
            record.pending_messages = std::move(existing->pending_messages);
            record.worktree_path = record.worktree_path.or_else([&] { return existing->worktree_path; });
            record.worktree_branch = record.worktree_branch.or_else([&] { return existing->worktree_branch; });
            record.worktree_base_commit = record.worktree_base_commit.or_else([&] { return existing->worktree_base_commit; });
            record.worktree_git_root = record.worktree_git_root.or_else([&] { return existing->worktree_git_root; });
            record.teammate_backend = record.teammate_backend.or_else([&] { return existing->teammate_backend; });
            record.teammate_task_id = record.teammate_task_id.or_else([&] { return existing->teammate_task_id; });
            record.teammate_pane_id = record.teammate_pane_id.or_else([&] { return existing->teammate_pane_id; });
            record.teammate_color = record.teammate_color.or_else([&] { return existing->teammate_color; });
            record.parent_session_id = record.parent_session_id.or_else([&] { return existing->parent_session_id; });
            record.worktree_cleanup_performed = existing->worktree_cleanup_performed;
        }
    }

    cc::tools::agent_runtime::native_agent_store().upsert(std::move(record));
}

struct AgentWorktreeCleanupResult {
    bool attempted = false;
    bool removed = false;
    bool changed = false;
    std::string message;
};

[[nodiscard]] inline AgentWorktreeCleanupResult cleanup_agent_worktree(std::string_view agent_id) {
    auto record = cc::tools::agent_runtime::native_agent_store().get(agent_id);
    if (!record || !record->worktree_path || !record->worktree_git_root || !record->worktree_branch) {
        return {};
    }

    AgentWorktreeCleanupResult result{
        .attempted = true,
        .message = "worktree cleanup inspected",
    };

    const auto worktree_path = fs::path{*record->worktree_path};
    const auto git_root = fs::path{*record->worktree_git_root};
    const auto branch = *record->worktree_branch;

    std::error_code ec;
    if (!fs::exists(worktree_path, ec)) {
        cc::tools::agent_runtime::native_agent_store().mark_worktree_cleaned(agent_id);
        result.removed = true;
        result.message = "worktree was already absent and metadata was cleaned";
        return result;
    }

    auto status = cc::utils::git::run_git_command("status --porcelain", worktree_path);
    if (!status.success) {
        result.changed = true;
        result.message = "worktree status could not be inspected; preserving worktree";
        cc::tools::agent_runtime::native_agent_store().append_transcript(
            agent_id,
            "system: retained worktree at " + worktree_path.string() + " because status inspection failed");
        return result;
    }
    if (!status.output.empty()) {
        result.changed = true;
        result.message = "worktree has uncommitted changes and was preserved";
        cc::tools::agent_runtime::native_agent_store().append_transcript(
            agent_id,
            "system: retained worktree with uncommitted changes at " + worktree_path.string());
        return result;
    }

    if (record->worktree_base_commit && !record->worktree_base_commit->empty()) {
        auto head = cc::utils::git::run_git_command("rev-parse HEAD", worktree_path);
        if (!head.success) {
            result.changed = true;
            result.message = "worktree HEAD could not be inspected; preserving worktree";
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                agent_id,
                "system: retained worktree at " + worktree_path.string() + " because HEAD inspection failed");
            return result;
        }
        if (head.output != *record->worktree_base_commit) {
            result.changed = true;
            result.message = "worktree branch contains commits and was preserved";
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                agent_id,
                "system: retained worktree branch " + branch + " at " + worktree_path.string());
            return result;
        }
    }

    auto removed = cc::utils::git::run_git_command(
        "worktree remove --force " + shell_quote(worktree_path.string()),
        git_root);
    if (!removed.success) {
        result.changed = true;
        result.message = "worktree removal failed; preserving metadata";
        cc::tools::agent_runtime::native_agent_store().append_transcript(
            agent_id,
            "system: failed to remove worktree at " + worktree_path.string() + ": " + removed.output);
        return result;
    }

    (void)cc::utils::git::run_git_command("branch -D " + shell_quote(branch), git_root);
    cc::tools::agent_runtime::native_agent_store().mark_worktree_cleaned(agent_id);
    cc::tools::agent_runtime::native_agent_store().append_transcript(
        agent_id,
        "system: cleaned worktree " + worktree_path.string() + " and branch " + branch);
    result.removed = true;
    result.message = "worktree removed";
    return result;
}

[[nodiscard]] inline std::string agent_output_file_path(std::string_view agent_id) {
    return cc::tools::agent_runtime::agent_output_file_path(agent_id).string();
}

[[nodiscard]] inline cc::tools::MemberRole teammate_role_for_agent_type(std::string_view agent_type) {
    auto lower = std::string(agent_type);
    std::ranges::transform(lower, lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (lower.find("leader") != std::string::npos || lower.find("lead") != std::string::npos) {
        return cc::tools::MemberRole::Leader;
    }
    if (lower.find("review") != std::string::npos ||
        lower.find("verify") != std::string::npos ||
        lower.find("verification") != std::string::npos ||
        lower.find("validator") != std::string::npos) {
        return cc::tools::MemberRole::Reviewer;
    }
    return cc::tools::MemberRole::Worker;
}

[[nodiscard]] inline std::optional<cc::utils::swarm_backends::AgentColor> teammate_agent_color(
    const std::optional<std::string>& color
) {
    if (!color || color->empty()) return std::nullopt;
    auto lower = *color;
    std::ranges::transform(lower, lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    using cc::utils::swarm_backends::AgentColor;
    if (lower == "red") return AgentColor::Red;
    if (lower == "blue") return AgentColor::Blue;
    if (lower == "green") return AgentColor::Green;
    if (lower == "yellow") return AgentColor::Yellow;
    if (lower == "purple") return AgentColor::Purple;
    if (lower == "orange") return AgentColor::Orange;
    if (lower == "pink") return AgentColor::Pink;
    if (lower == "cyan") return AgentColor::Cyan;
    return std::nullopt;
}

[[nodiscard]] inline std::string teammate_parent_session_id() {
    if (const char* value = std::getenv("CC_REPL_SESSION_ID"); value && *value) {
        return value;
    }
    if (const char* value = std::getenv("CLAUDE_SESSION_ID"); value && *value) {
        return value;
    }
    return "native-session";
}

[[nodiscard]] inline std::string tool_result_content_text(const ToolResult& result) {
    std::string output;
    for (const auto& content : result.content) {
        if (!output.empty()) output += "\n";
        output += content.text;
    }
    return output;
}

inline void update_teammate_completion_status(
    const AgentExecutionPlan& plan,
    bool success,
    const std::string& result_text
) {
    if (!plan.team_name || plan.team_name->empty()) return;
    auto stored_result = result_text.empty()
        ? std::optional<std::string>{}
        : std::optional<std::string>{result_text};
    (void)cc::tools::global_team_store().update_member_status(
        *plan.team_name,
        plan.agent_id,
        success ? cc::tools::MemberStatus::Done : cc::tools::MemberStatus::Error,
        std::move(stored_result));
}

[[nodiscard]] inline std::string message_content_text(const Message& message) {
    std::string out;
    for (const auto& block : message.content) {
        if (!out.empty()) out += "\n";
        if (block.type == ContentBlockType::Text || block.type == ContentBlockType::ToolResult) {
            out += block.text;
        } else if (block.type == ContentBlockType::ToolUse) {
            out += std::format("[tool_use:{}]", block.tool_name);
        }
    }
    return out;
}

[[nodiscard]] inline std::string normalized_tool_input_json(std::string_view raw_json) {
    if (raw_json.empty()) return "{}";
    auto parsed = cc::utils::json::parse(raw_json);
    if (!parsed || !parsed->root().valid()) return "{}";
    return cc::utils::json::to_string(parsed->root());
}

[[nodiscard]] inline std::string message_content_sidechain_json(const Message& message) {
    std::string out = "[";
    bool first = true;
    auto begin_block = [&] {
        if (!first) out += ',';
        first = false;
        out += '{';
    };
    auto append_field_prefix = [&](std::string_view name, bool& first_field) {
        if (!first_field) out += ',';
        first_field = false;
        out += '"';
        out += json_escape_string(name);
        out += R"(":)";
    };
    auto append_string_field = [&](std::string_view name, std::string_view value, bool& first_field) {
        append_field_prefix(name, first_field);
        out += '"';
        out += json_escape_string(value);
        out += '"';
    };

    for (const auto& block : message.content) {
        begin_block();
        bool first_field = true;
        switch (block.type) {
            case ContentBlockType::Text:
                append_string_field("type", "text", first_field);
                append_string_field("text", block.text, first_field);
                break;
            case ContentBlockType::ToolUse:
                append_string_field("type", "tool_use", first_field);
                append_string_field("id", block.tool_use_id, first_field);
                append_string_field("name", block.tool_name, first_field);
                append_field_prefix("input", first_field);
                out += normalized_tool_input_json(block.tool_input_json);
                break;
            case ContentBlockType::ToolResult:
                append_string_field("type", "tool_result", first_field);
                append_string_field("tool_use_id", block.tool_use_id, first_field);
                append_field_prefix("content", first_field);
                out += R"([{"type":"text","text":")";
                out += json_escape_string(block.text);
                out += R"("}])";
                break;
            case ContentBlockType::Image:
            case ContentBlockType::Document:
                append_string_field(
                    "type",
                    block.type == ContentBlockType::Image ? "image" : "document",
                    first_field);
                append_field_prefix("source", first_field);
                out += R"({"type":"base64","media_type":")";
                out += json_escape_string(block.media_type);
                out += R"(","data":")";
                out += json_escape_string(block.image_data);
                out += R"("})";
                break;
            case ContentBlockType::Thinking:
                append_string_field("type", "thinking", first_field);
                append_string_field("thinking", block.thinking, first_field);
                if (!block.signature.empty()) append_string_field("signature", block.signature, first_field);
                break;
            case ContentBlockType::RedactedThinking:
                append_string_field("type", "redacted_thinking", first_field);
                append_string_field("data", block.thinking, first_field);
                break;
        }
        out += '}';
    }
    out += ']';
    return out;
}

[[nodiscard]] inline std::string json_string_member(
    cc::utils::json::JsonVal object,
    std::string_view key
) {
    if (!object.valid() || !object.is_obj()) return {};
    auto value = object.get(key);
    return value.is_str() ? std::string(value.as_str()) : std::string{};
}

[[nodiscard]] inline std::string text_from_json_content(cc::utils::json::JsonVal content) {
    if (!content.valid()) return {};
    if (content.is_str()) return std::string(content.as_str());
    if (!content.is_arr()) return {};

    std::string out;
    content.iter([&](cc::utils::json::JsonVal block) {
        std::string text;
        if (block.is_str()) {
            text = std::string(block.as_str());
        } else if (block.is_obj()) {
            auto type = json_string_member(block, "type");
            if (type == "text") {
                text = json_string_member(block, "text");
            } else if (type == "tool_result") {
                text = text_from_json_content(block.get("content"));
            }
        }
        if (text.empty()) return;
        if (!out.empty()) out += "\n";
        out += text;
    });
    return out;
}

[[nodiscard]] inline ContentBlock content_block_from_json(cc::utils::json::JsonVal block) {
    if (block.is_str()) {
        return ContentBlock{.type = ContentBlockType::Text, .text = std::string(block.as_str())};
    }
    if (!block.valid() || !block.is_obj()) {
        return ContentBlock{
            .type = ContentBlockType::Text,
            .text = block.valid() ? block.to_string() : std::string{},
        };
    }

    auto type = json_string_member(block, "type");
    if (type == "tool_use") {
        return ContentBlock{
            .type = ContentBlockType::ToolUse,
            .tool_use_id = json_string_member(block, "id"),
            .tool_name = json_string_member(block, "name"),
            .tool_input_json = block.get("input").valid() ? block.get("input").to_string() : std::string{"{}"},
        };
    }
    if (type == "tool_result") {
        return ContentBlock{
            .type = ContentBlockType::ToolResult,
            .text = text_from_json_content(block.get("content")),
            .tool_use_id = json_string_member(block, "tool_use_id"),
        };
    }
    if (type == "thinking") {
        return ContentBlock{
            .type = ContentBlockType::Thinking,
            .thinking = json_string_member(block, "thinking"),
            .signature = json_string_member(block, "signature"),
        };
    }
    if (type == "redacted_thinking") {
        return ContentBlock{
            .type = ContentBlockType::RedactedThinking,
            .thinking = json_string_member(block, "data"),
        };
    }

    auto text = json_string_member(block, "text");
    if (text.empty()) text = text_from_json_content(block.get("content"));
    return ContentBlock{.type = ContentBlockType::Text, .text = std::move(text)};
}

[[nodiscard]] inline std::optional<Message> message_from_json_value(cc::utils::json::JsonVal root) {
    if (!root.valid() || !root.is_obj()) return std::nullopt;

    auto message_obj = root.get("message");
    auto source = message_obj.is_obj() ? message_obj : root;
    auto role = json_string_member(source, "role");
    if (role.empty()) role = json_string_member(root, "type");
    role = role == "assistant" ? "assistant" : "user";

    auto content = source.get("content");
    if (content.is_str()) {
        return Message::from_text(role, content.as_str());
    }

    Message message;
    message.role = std::move(role);
    if (content.is_arr()) {
        content.iter([&](cc::utils::json::JsonVal block) {
            auto parsed = content_block_from_json(block);
            if (parsed.type == ContentBlockType::Text && parsed.text.empty()) return;
            message.content.push_back(std::move(parsed));
        });
    }

    if (message.content.empty()) {
        auto text = json_string_member(root, "raw");
        if (text.empty()) text = json_string_member(root, "text");
        if (text.empty()) text = json_string_member(root, "content");
        if (!text.empty()) return Message::from_text(message.role, text);
    }
    if (message.content.empty()) return std::nullopt;
    return message;
}

[[nodiscard]] inline std::vector<Message> fork_context_messages_from_entries(
    const std::vector<std::string>& entries
) {
    std::vector<Message> messages;
    for (const auto& entry : entries) {
        auto parsed = cc::utils::json::parse(entry);
        if (!parsed) continue;
        if (auto message = message_from_json_value(parsed->root())) {
            messages.push_back(std::move(*message));
        }
    }
    return messages;
}

[[nodiscard]] inline std::vector<std::string> tool_use_ids_in_message(const Message& message) {
    std::vector<std::string> ids;
    if (message.role != "assistant" && message.role != "user") return ids;
    for (const auto& block : message.content) {
        if (block.type == ContentBlockType::ToolUse && !block.tool_use_id.empty()) {
            ids.push_back(block.tool_use_id);
        }
    }
    return ids;
}

[[nodiscard]] inline std::vector<std::string> tool_result_ids_in_message(const Message& message) {
    std::vector<std::string> ids;
    if (message.role != "assistant" && message.role != "user") return ids;
    for (const auto& block : message.content) {
        if (block.type == ContentBlockType::ToolResult && !block.tool_use_id.empty()) {
            ids.push_back(block.tool_use_id);
        }
    }
    return ids;
}

[[nodiscard]] inline std::vector<Message> filter_resume_unresolved_tool_use_messages(
    std::vector<Message> messages
) {
    std::unordered_set<std::string> tool_use_ids;
    std::unordered_set<std::string> tool_result_ids;
    for (const auto& message : messages) {
        for (const auto& id : tool_use_ids_in_message(message)) tool_use_ids.insert(id);
        for (const auto& id : tool_result_ids_in_message(message)) tool_result_ids.insert(id);
    }

    std::unordered_set<std::string> unresolved_ids;
    for (const auto& id : tool_use_ids) {
        if (!tool_result_ids.contains(id)) unresolved_ids.insert(id);
    }
    if (unresolved_ids.empty()) return messages;

    std::vector<Message> filtered;
    filtered.reserve(messages.size());
    for (auto& message : messages) {
        if (message.role != "assistant") {
            filtered.push_back(std::move(message));
            continue;
        }
        auto ids = tool_use_ids_in_message(message);
        if (ids.empty()) {
            filtered.push_back(std::move(message));
            continue;
        }
        const bool all_unresolved = std::ranges::all_of(ids, [&](const std::string& id) {
            return unresolved_ids.contains(id);
        });
        if (!all_unresolved) filtered.push_back(std::move(message));
    }
    return filtered;
}

[[nodiscard]] inline bool message_is_thinking_only(const Message& message) {
    if (message.role != "assistant" || message.content.empty()) return false;
    return std::ranges::all_of(message.content, [](const ContentBlock& block) {
        return block.type == ContentBlockType::Thinking ||
            block.type == ContentBlockType::RedactedThinking;
    });
}

[[nodiscard]] inline std::vector<Message> filter_resume_orphaned_thinking_messages(
    std::vector<Message> messages
) {
    std::vector<Message> filtered;
    filtered.reserve(messages.size());
    for (auto& message : messages) {
        if (message_is_thinking_only(message)) continue;
        filtered.push_back(std::move(message));
    }
    return filtered;
}

[[nodiscard]] inline bool message_is_whitespace_only_assistant(const Message& message) {
    if (message.role != "assistant" || message.content.empty()) return false;
    return std::ranges::all_of(message.content, [](const ContentBlock& block) {
        return block.type == ContentBlockType::Text && trim_ascii_copy(block.text).empty();
    });
}

inline void append_merged_user_message(std::vector<Message>& messages, Message message) {
    if (!messages.empty() && messages.back().role == "user" && message.role == "user") {
        messages.back().content.insert(
            messages.back().content.end(),
            std::make_move_iterator(message.content.begin()),
            std::make_move_iterator(message.content.end()));
        return;
    }
    messages.push_back(std::move(message));
}

[[nodiscard]] inline std::vector<Message> filter_resume_whitespace_assistant_messages(
    std::vector<Message> messages
) {
    std::vector<Message> filtered;
    filtered.reserve(messages.size());
    bool changed = false;
    for (auto& message : messages) {
        if (message_is_whitespace_only_assistant(message)) {
            changed = true;
            continue;
        }
        if (changed) {
            append_merged_user_message(filtered, std::move(message));
        } else {
            filtered.push_back(std::move(message));
        }
    }
    return filtered;
}

// Filters out assistant messages that contain `tool_use` blocks for which NO
// matching `tool_result` block exists in the transcript.
//
// Mirrors TS filterIncompleteToolCalls in runAgent.ts. This is stricter than
// filter_resume_unresolved_tool_use_messages (which only drops an assistant
// message when ALL of its tool_uses are unresolved): here a single orphaned
// tool_use is enough to exclude the entire assistant message, because the
// Anthropic API rejects requests where any tool_use is missing its result.
//
// Use this when splicing a parent's conversation history into a sub-agent's
// context (fork / resume paths) to avoid sending malformed message sequences.
[[nodiscard]] inline std::vector<Message> filter_incomplete_tool_calls(
    std::vector<Message> messages
) {
    // migrated edge case: build the set of tool_use IDs that have results by
    // doing a full forward scan. TS does two passes (first pass collect
    // result IDs, second pass filter messages). We mirror exactly.
    std::unordered_set<std::string> tool_use_ids_with_results;
    for (const auto& message : messages) {
        if (message.role != "user") continue;
        for (const auto& block : message.content) {
            if (block.type == ContentBlockType::ToolResult && !block.tool_use_id.empty()) {
                tool_use_ids_with_results.insert(block.tool_use_id);
            }
        }
    }

    std::vector<Message> filtered;
    filtered.reserve(messages.size());
    for (auto& message : messages) {
        // migrated edge case: non-assistant messages always pass through; the
        // API only rejects malformed assistant→user tool_use/tool_result pairs.
        if (message.role != "assistant") {
            filtered.push_back(std::move(message));
            continue;
        }
        bool has_incomplete_tool_call = false;
        for (const auto& block : message.content) {
            if (block.type == ContentBlockType::ToolUse && !block.tool_use_id.empty()) {
                if (!tool_use_ids_with_results.contains(block.tool_use_id)) {
                    has_incomplete_tool_call = true;
                    break;
                }
            }
        }
        // migrated edge case: exclude the assistant message if ANY tool_use
        // lacks a matching tool_result. TS keeps assistant messages with
        // zero tool_uses, even if they're whitespace-only.
        if (!has_incomplete_tool_call) {
            filtered.push_back(std::move(message));
        }
    }
    return filtered;
}

[[nodiscard]] inline std::unordered_map<std::string, std::string> resume_content_replacements_from_entries(
    const std::vector<std::string>& entries
) {
    std::unordered_map<std::string, std::string> replacements;
    for (const auto& entry : entries) {
        auto parsed = cc::utils::json::parse(entry);
        if (!parsed || !parsed->root().is_obj()) continue;
        auto root = parsed->root();
        if (json_string_member(root, "type") != "content-replacement") continue;

        auto replacement_entries = root.get("replacements");
        if (!replacement_entries.is_arr()) continue;
        replacement_entries.iter([&](cc::utils::json::JsonVal item) {
            if (!item.is_obj()) return;
            auto kind = json_string_member(item, "kind");
            if (!kind.empty() && kind != "tool-result") return;
            auto id = json_string_member(item, "toolUseId");
            if (id.empty()) id = json_string_member(item, "tool_use_id");
            auto replacement = json_string_member(item, "replacement");
            if (!id.empty()) replacements[std::move(id)] = std::move(replacement);
        });
    }
    return replacements;
}

inline void apply_resume_content_replacements(
    std::vector<Message>& messages,
    const std::unordered_map<std::string, std::string>& replacements
) {
    if (replacements.empty()) return;
    for (auto& message : messages) {
        if (message.role != "user") continue;
        for (auto& block : message.content) {
            if (block.type != ContentBlockType::ToolResult || block.tool_use_id.empty()) continue;
            if (auto replacement = replacements.find(block.tool_use_id); replacement != replacements.end()) {
                block.text = replacement->second;
            }
        }
    }
}

struct AgentContentReplacementState {
    std::unordered_set<std::string> seen_ids;
    std::unordered_map<std::string, std::string> replacements;
};

struct AgentToolResultCandidate {
    std::size_t message_index = 0;
    std::size_t block_index = 0;
    std::string tool_use_id;
    std::size_t size = 0;
};

struct AgentToolResultBudgetOutcome {
    std::size_t newly_replaced = 0;
    std::size_t reapplied = 0;
};

inline constexpr std::size_t AGENT_MAX_TOOL_RESULTS_PER_MESSAGE_CHARS = 200'000;
inline constexpr std::size_t AGENT_DEFAULT_TOOL_RESULT_THRESHOLD_CHARS = 50'000;
inline constexpr std::string_view AGENT_PERSIST_THRESHOLD_OVERRIDE_FLAG = "tengu_satin_quoll";
inline constexpr std::string_view AGENT_PER_MESSAGE_BUDGET_OVERRIDE_FLAG = "tengu_hawthorn_window";

[[nodiscard]] inline bool agent_growthbook_env_overrides_enabled() {
    const char* user_type = std::getenv("USER_TYPE");
    return user_type && std::string_view(user_type) == "ant";
}

[[nodiscard]] inline std::optional<std::size_t> json_positive_size_t(
    cc::utils::json::JsonVal value
) {
    if (!value.valid() || !value.is_num()) return std::nullopt;
    const auto numeric = value.as_double();
    if (!std::isfinite(numeric) || numeric <= 0.0) return std::nullopt;
    if (numeric > static_cast<double>(std::numeric_limits<std::size_t>::max())) return std::nullopt;
    return static_cast<std::size_t>(numeric);
}

template <typename Fn>
inline void with_agent_growthbook_env_overrides(Fn&& fn) {
    if (!agent_growthbook_env_overrides_enabled()) return;
    const char* raw = std::getenv("CLAUDE_INTERNAL_FC_OVERRIDES");
    if (!raw || !*raw) return;
    auto parsed = cc::utils::json::parse(raw);
    if (!parsed || !parsed->root().is_obj()) return;
    fn(parsed->root());
}

[[nodiscard]] inline std::optional<std::size_t> agent_tool_threshold_override(
    std::string_view tool_name
) {
    std::optional<std::size_t> override;
    with_agent_growthbook_env_overrides([&](cc::utils::json::JsonVal root) {
        auto overrides = root.get(AGENT_PERSIST_THRESHOLD_OVERRIDE_FLAG);
        if (!overrides.valid() || !overrides.is_obj()) return;

        auto lookup = [&](std::string_view key) -> std::optional<std::size_t> {
            return json_positive_size_t(overrides.get(key));
        };

        override = lookup(tool_name);
        if (!override) {
            const auto lowered = lowercase_ascii(tool_name);
            if (lowered != tool_name) override = lookup(lowered);
        }
    });
    return override;
}

[[nodiscard]] inline std::size_t agent_per_message_budget_limit() {
    std::optional<std::size_t> override;
    with_agent_growthbook_env_overrides([&](cc::utils::json::JsonVal root) {
        override = json_positive_size_t(root.get(AGENT_PER_MESSAGE_BUDGET_OVERRIDE_FLAG));
    });
    return override.value_or(AGENT_MAX_TOOL_RESULTS_PER_MESSAGE_CHARS);
}

[[nodiscard]] inline AgentContentReplacementState agent_content_replacement_state_from_entries(
    const std::vector<std::string>& entries
) {
    return AgentContentReplacementState{
        .seen_ids = {},
        .replacements = resume_content_replacements_from_entries(entries),
    };
}

inline void mark_seen_tool_result_ids(
    AgentContentReplacementState& state,
    const std::vector<Message>& messages
) {
    for (const auto& message : messages) {
        if (message.role != "user") continue;
        for (const auto& block : message.content) {
            if (block.type == ContentBlockType::ToolResult && !block.tool_use_id.empty()) {
                state.seen_ids.insert(block.tool_use_id);
            }
        }
    }
}

[[nodiscard]] inline bool tool_result_already_replaced(std::string_view text) {
    return text.starts_with(cc::utils::PERSISTED_OUTPUT_TAG);
}

[[nodiscard]] inline std::unordered_set<std::string> unbounded_tool_result_budget_names(
    const std::vector<ToolDefinition>& definitions
) {
    std::unordered_set<std::string> names;
    for (const auto& definition : definitions) {
        if (definition.max_result_size_unbounded) {
            names.insert(lowercase_ascii(definition.name));
        }
    }
    return names;
}

[[nodiscard]] inline std::unordered_map<std::string, std::size_t> tool_result_budget_thresholds(
    const std::vector<ToolDefinition>& definitions
) {
    std::unordered_map<std::string, std::size_t> thresholds;
    for (const auto& definition : definitions) {
        if (definition.max_result_size_unbounded) continue;
        auto threshold = agent_tool_threshold_override(definition.name).value_or(definition.max_result_size_chars == 0
            ? AGENT_DEFAULT_TOOL_RESULT_THRESHOLD_CHARS
            : std::min(definition.max_result_size_chars, AGENT_DEFAULT_TOOL_RESULT_THRESHOLD_CHARS));
        thresholds[lowercase_ascii(definition.name)] = threshold;
    }
    return thresholds;
}

[[nodiscard]] inline std::unordered_map<std::string, std::string> tool_name_by_tool_use_id(
    const std::vector<Message>& messages
) {
    std::unordered_map<std::string, std::string> names;
    for (const auto& message : messages) {
        if (message.role != "assistant") continue;
        for (const auto& block : message.content) {
            if (block.type == ContentBlockType::ToolUse && !block.tool_use_id.empty()) {
                names[block.tool_use_id] = block.tool_name;
            }
        }
    }
    return names;
}

[[nodiscard]] inline bool should_skip_agent_budget_candidate(
    const std::unordered_map<std::string, std::string>& tool_names,
    const std::unordered_set<std::string>& skip_tool_names,
    std::string_view tool_use_id
) {
    auto it = tool_names.find(std::string(tool_use_id));
    if (it == tool_names.end()) return false;
    return skip_tool_names.contains(lowercase_ascii(it->second));
}

[[nodiscard]] inline std::size_t agent_budget_threshold_for_candidate(
    const std::unordered_map<std::string, std::string>& tool_names,
    const std::unordered_map<std::string, std::size_t>& thresholds,
    std::string_view tool_use_id
) {
    auto name = tool_names.find(std::string(tool_use_id));
    if (name == tool_names.end()) return AGENT_DEFAULT_TOOL_RESULT_THRESHOLD_CHARS;
    auto threshold = thresholds.find(lowercase_ascii(name->second));
    if (threshold == thresholds.end()) return AGENT_DEFAULT_TOOL_RESULT_THRESHOLD_CHARS;
    return threshold->second;
}

[[nodiscard]] inline std::vector<std::vector<AgentToolResultCandidate>> collect_agent_budget_candidates_by_message(
    const std::vector<Message>& messages
) {
    std::vector<std::vector<AgentToolResultCandidate>> groups;
    std::vector<AgentToolResultCandidate> current;
    auto flush = [&] {
        if (!current.empty()) groups.push_back(std::exchange(current, {}));
    };

    for (std::size_t message_index = 0; message_index < messages.size(); ++message_index) {
        const auto& message = messages[message_index];
        if (message.role == "assistant") {
            flush();
            continue;
        }
        if (message.role != "user") continue;
        for (std::size_t block_index = 0; block_index < message.content.size(); ++block_index) {
            const auto& block = message.content[block_index];
            if (block.type != ContentBlockType::ToolResult || block.tool_use_id.empty()) continue;
            if (block.text.empty() || tool_result_already_replaced(block.text)) continue;
            current.push_back(AgentToolResultCandidate{
                .message_index = message_index,
                .block_index = block_index,
                .tool_use_id = block.tool_use_id,
                .size = block.text.size(),
            });
        }
    }
    flush();
    return groups;
}

[[nodiscard]] inline fs::path agent_tool_result_path(
    std::string_view agent_id,
    std::string_view tool_use_id
) {
    return cc::tools::agent_runtime::runtime_state_dir() /
        "tool-results" /
        (cc::tools::agent_runtime::safe_agent_filename(agent_id) + "-" +
            cc::tools::agent_runtime::safe_agent_filename(tool_use_id) + ".txt");
}

[[nodiscard]] inline std::optional<std::string> build_agent_tool_result_replacement(
    std::string_view agent_id,
    const AgentToolResultCandidate& candidate,
    std::string_view content
) {
    auto path = agent_tool_result_path(agent_id, candidate.tool_use_id);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return std::nullopt;
    if (!fs::exists(path, ec)) {
        std::ofstream out(path, std::ios::trunc);
        if (!out) return std::nullopt;
        out << content;
        if (!out.good()) return std::nullopt;
    }

    constexpr std::size_t preview_size = 2'000;
    const auto preview_len = std::min(preview_size, content.size());
    std::string preview{content.substr(0, preview_len)};
    const bool has_more = content.size() > preview_len;

    std::string replacement;
    replacement.reserve(preview.size() + path.string().size() + 192);
    replacement += cc::utils::PERSISTED_OUTPUT_TAG;
    replacement += "\n";
    replacement += std::format(
        "Output too large ({} bytes). Full output saved to: {}\n\n",
        content.size(),
        path.string());
    replacement += std::format("Preview (first {} bytes):\n", preview_len);
    replacement += preview;
    replacement += has_more ? "\n...\n" : "\n";
    replacement += cc::utils::PERSISTED_OUTPUT_CLOSING_TAG;
    return replacement;
}

[[nodiscard]] inline std::string agent_content_replacement_entry_json(
    std::string_view agent_id,
    const std::vector<std::pair<std::string, std::string>>& replacements
) {
    std::string out = R"({"type":"content-replacement","sessionId":")";
    out += json_escape_string(teammate_parent_session_id());
    out += R"(","agentId":")";
    out += json_escape_string(agent_id);
    out += R"(","replacements":[)";
    for (std::size_t i = 0; i < replacements.size(); ++i) {
        if (i != 0) out += ',';
        out += R"({"kind":"tool-result","toolUseId":")";
        out += json_escape_string(replacements[i].first);
        out += R"(","replacement":")";
        out += json_escape_string(replacements[i].second);
        out += R"("})";
    }
    out += R"(]})";
    return out;
}

inline AgentToolResultBudgetOutcome apply_agent_tool_result_budget(
    std::string_view agent_id,
    std::vector<Message>& messages,
    AgentContentReplacementState& state,
    const std::unordered_set<std::string>& skip_tool_names = {},
    const std::unordered_map<std::string, std::size_t>& tool_thresholds = {}
) {
    AgentToolResultBudgetOutcome outcome;
    const auto tool_names = tool_name_by_tool_use_id(messages);
    const auto message_budget_limit = agent_per_message_budget_limit();
    std::vector<std::pair<std::string, std::string>> newly_replaced;

    for (const auto& candidates : collect_agent_budget_candidates_by_message(messages)) {
        std::vector<AgentToolResultCandidate> fresh;
        std::size_t frozen_size = 0;
        std::size_t fresh_size = 0;

        for (const auto& candidate : candidates) {
            if (auto replacement = state.replacements.find(candidate.tool_use_id);
                replacement != state.replacements.end()) {
                messages[candidate.message_index].content[candidate.block_index].text = replacement->second;
                state.seen_ids.insert(candidate.tool_use_id);
                ++outcome.reapplied;
            } else if (state.seen_ids.contains(candidate.tool_use_id)) {
                frozen_size += candidate.size;
            } else if (should_skip_agent_budget_candidate(tool_names, skip_tool_names, candidate.tool_use_id)) {
                state.seen_ids.insert(candidate.tool_use_id);
            } else {
                fresh.push_back(candidate);
                fresh_size += candidate.size;
            }
        }

        std::vector<AgentToolResultCandidate> selected;
        std::size_t selected_size = 0;
        std::unordered_set<std::string> selected_ids;
        for (const auto& candidate : fresh) {
            if (candidate.size > agent_budget_threshold_for_candidate(tool_names, tool_thresholds, candidate.tool_use_id)) {
                selected.push_back(candidate);
                selected_ids.insert(candidate.tool_use_id);
                selected_size += candidate.size;
            }
        }

        auto visible_size_after_threshold_replacements = frozen_size + fresh_size - selected_size;
        if (!fresh.empty() && visible_size_after_threshold_replacements > message_budget_limit) {
            std::vector<AgentToolResultCandidate> remaining_candidates;
            remaining_candidates.reserve(fresh.size());
            for (const auto& candidate : fresh) {
                if (!selected_ids.contains(candidate.tool_use_id)) {
                    remaining_candidates.push_back(candidate);
                }
            }
            std::ranges::sort(remaining_candidates, {}, &AgentToolResultCandidate::size);
            std::ranges::reverse(remaining_candidates);
            for (const auto& candidate : remaining_candidates) {
                if (visible_size_after_threshold_replacements <= message_budget_limit) break;
                selected.push_back(candidate);
                selected_ids.insert(candidate.tool_use_id);
                visible_size_after_threshold_replacements -= candidate.size;
            }
        }

        for (const auto& candidate : fresh) {
            if (!selected_ids.contains(candidate.tool_use_id)) {
                state.seen_ids.insert(candidate.tool_use_id);
            }
        }

        for (const auto& candidate : selected) {
            auto& block = messages[candidate.message_index].content[candidate.block_index];
            auto replacement = build_agent_tool_result_replacement(agent_id, candidate, block.text);
            state.seen_ids.insert(candidate.tool_use_id);
            if (!replacement) continue;
            block.text = *replacement;
            state.replacements[candidate.tool_use_id] = *replacement;
            newly_replaced.emplace_back(candidate.tool_use_id, *replacement);
            ++outcome.newly_replaced;
        }
    }

    if (!newly_replaced.empty()) {
        cc::tools::agent_runtime::native_agent_store().append_sidechain_entry(
            agent_id,
            agent_content_replacement_entry_json(agent_id, newly_replaced));
    }
    return outcome;
}

[[nodiscard]] inline std::vector<Message> resume_messages_from_sidechain_entries(
    const std::vector<std::string>& entries
) {
    auto messages = fork_context_messages_from_entries(entries);
    // migrated edge case: drop assistant messages with any tool_use that
    // never received a tool_result (mirrors TS filterUnresolvedToolUses in
    // resumeAgent). filter_resume_unresolved_tool_use_messages handles the
    // case where an assistant message's tool_uses are *all* unresolved;
    // filter_incomplete_tool_calls is stricter and drops an assistant
    // whenever *any* tool_use lacks a result.
    messages = filter_incomplete_tool_calls(std::move(messages));
    messages = filter_resume_unresolved_tool_use_messages(std::move(messages));
    messages = filter_resume_orphaned_thinking_messages(std::move(messages));
    messages = filter_resume_whitespace_assistant_messages(std::move(messages));
    apply_resume_content_replacements(messages, resume_content_replacements_from_entries(entries));
    return messages;
}

[[nodiscard]] inline std::string message_json_object(const Message& message) {
    std::string out = R"({"role":")";
    out += json_escape_string(message.role);
    out += R"(","content":)";
    out += message_content_sidechain_json(message);
    out += '}';
    return out;
}

[[nodiscard]] inline std::vector<Message> forked_messages_from_parent_assistant(
    std::string_view directive,
    Message assistant_message
) {
    assistant_message.role = "assistant";

    std::vector<ContentBlock> tool_uses;
    for (const auto& block : assistant_message.content) {
        if (block.type == ContentBlockType::ToolUse && !block.tool_use_id.empty()) {
            tool_uses.push_back(block);
        }
    }

    const auto directive_message = cc::tools::agent_runtime::build_fork_child_message(directive);
    if (tool_uses.empty()) {
        return {Message::from_text("user", directive_message)};
    }

    Message missing_tool_results;
    missing_tool_results.role = "user";
    for (const auto& tool_use : tool_uses) {
        missing_tool_results.content.push_back(ContentBlock{
            .type = ContentBlockType::ToolResult,
            .text = "Fork started \u2014 processing in background",
            .tool_use_id = tool_use.tool_use_id,
        });
    }
    missing_tool_results.content.push_back(ContentBlock{
        .type = ContentBlockType::Text,
        .text = directive_message,
    });

    return {std::move(assistant_message), std::move(missing_tool_results)};
}

[[nodiscard]] inline std::vector<Message> forked_messages_from_parent_assistant_entries(
    std::string_view directive,
    const std::vector<std::string>& entries
) {
    for (const auto& entry : entries) {
        auto parsed = cc::utils::json::parse(entry);
        if (!parsed) continue;
        if (auto message = message_from_json_value(parsed->root())) {
            return forked_messages_from_parent_assistant(directive, std::move(*message));
        }
    }
    return {};
}

[[nodiscard]] inline bool message_contains_fork_boilerplate(const Message& message) {
    return std::ranges::any_of(message.content, [](const ContentBlock& block) {
        return block.type == ContentBlockType::Text && text_contains_fork_boilerplate(block.text);
    });
}

[[nodiscard]] inline bool messages_contain_fork_boilerplate(const std::vector<Message>& messages) {
    return std::ranges::any_of(messages, [](const Message& message) {
        return message_contains_fork_boilerplate(message);
    });
}

inline void append_agent_sidechain_message(std::string_view agent_id, const Message& message) {
    cc::tools::agent_runtime::native_agent_store().append_sidechain_message(
        agent_id,
        message.role,
        message_content_sidechain_json(message),
        message_content_text(message));
}

[[nodiscard]] inline std::string format_resumed_agent_context(
    const cc::tools::agent_runtime::NativeAgentRecord& record
) {
    constexpr std::size_t max_lines = 80;
    constexpr std::size_t max_chars = 24000;

    std::string context;
    context += "<resumed_agent_context>\n";
    context += std::format("<agent_id>{}</agent_id>\n", record.agent_id);
    context += std::format("<status>{}</status>\n", cc::tools::agent_runtime::native_agent_status_name(record.status));
    if (record.description && !record.description->empty()) {
        context += std::format("<description>{}</description>\n", *record.description);
    }
    if (record.output && !record.output->empty()) {
        context += std::format("<previous_output>{}</previous_output>\n", *record.output);
    }
    if (record.error && !record.error->empty()) {
        context += std::format("<previous_error>{}</previous_error>\n", *record.error);
    }

    context += "<recent_transcript>\n";
    const auto start = record.transcript.size() > max_lines
        ? record.transcript.size() - max_lines
        : std::size_t{0};
    for (std::size_t i = start; i < record.transcript.size(); ++i) {
        const auto& line = record.transcript[i];
        if (context.size() + line.size() + 2 > max_chars) {
            context += "[resumed transcript truncated]\n";
            break;
        }
        context += line;
        context += '\n';
    }
    context += "</recent_transcript>\n";
    context += "</resumed_agent_context>";
    return context;
}

[[nodiscard]] inline bool should_reject_fork_child_agent_call(
    const AgentExecutionPlan& plan,
    std::string_view tool_name,
    std::string_view tool_input_json
) {
    if (!plan.fork_child_context) return false;
    if (tool_name != "Agent") return false;
    return agent_tool_input_omits_agent_type(tool_input_json);
}

[[nodiscard]] inline bool exact_tools_allow_tool(
    const AgentExecutionPlan& plan,
    std::string_view tool_name
) {
    if (!plan.use_exact_tools) return true;
    if (plan.exact_tools.empty()) return true;
    return std::ranges::contains(plan.exact_tools, tool_name);
}

[[nodiscard]] inline bool implicit_fork_injection_replaces_key(std::string_view key) {
    constexpr std::array<std::string_view, 16> keys{
        "query_source",
        "querySource",
        "fork_child",
        "forkChild",
        "run_in_background",
        "parent_system_prompt",
        "parentSystemPrompt",
        "exact_tools",
        "exactTools",
        "available_tools",
        "availableTools",
        "use_exact_tools",
        "useExactTools",
        "parent_assistant_message",
        "parentAssistantMessage",
        "assistantMessage",
    };
    return std::ranges::contains(keys, key);
}

[[nodiscard]] inline std::vector<std::string> exact_tool_names_from_api_tools(
    const std::vector<cc::services::api::ToolDefinition>& tools
) {
    std::vector<std::string> names;
    names.reserve(tools.size());
    for (const auto& tool : tools) {
        if (tool.name.empty() || std::ranges::contains(names, tool.name)) continue;
        names.push_back(tool.name);
    }
    return names;
}

[[nodiscard]] inline std::string build_implicit_fork_agent_input_json(
    std::string_view raw_json,
    const AgentExecutionPlan& parent_plan,
    const Message& parent_assistant_message,
    const std::vector<cc::services::api::ToolDefinition>& parent_tools
) {
    auto parsed = cc::utils::json::parse(raw_json);
    if (!parsed || !parsed->root().is_obj()) return std::string(raw_json);

    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();
    parsed->root().iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal value) {
        if (!key.is_str()) return;
        auto key_text = key.as_str();
        if (implicit_fork_injection_replaces_key(key_text)) return;
        root.add(key_text, doc.copy_val(value));
    });

    root.add("querySource", doc.string("agent:builtin:fork"));
    root.add("forkChild", doc.boolean(true));
    root.add("run_in_background", doc.boolean(true));
    if (!parent_plan.system_prompt.empty()) {
        root.add("parentSystemPrompt", doc.string(parent_plan.system_prompt));
    }

    auto exact_tools = doc.array();
    for (const auto& name : exact_tool_names_from_api_tools(parent_tools)) {
        exact_tools.append(doc.string(name));
    }
    root.add("exactTools", exact_tools);
    root.add("useExactTools", doc.boolean(true));

    if (auto parent_message = doc.raw_json(message_json_object(parent_assistant_message));
        parent_message.valid()) {
        root.add("parentAssistantMessage", parent_message);
    }

    doc.set_root(root);
    return doc.to_string();
}

inline void hydrate_resume_plan_from_existing_record(AgentExecutionPlan& plan) {
    if (!plan.resume_existing) return;
    auto existing = cc::tools::agent_runtime::native_agent_store().get(plan.agent_id);
    if (!existing) return;

    plan.parent_agent_id = plan.parent_agent_id.or_else([&] { return existing->parent_agent_id; });
    plan.description = plan.description.or_else([&] { return existing->description; });
    plan.name = plan.name.or_else([&] { return existing->name; });
    plan.team_name = plan.team_name.or_else([&] { return existing->team_name; });
    plan.mode = plan.mode.or_else([&] { return existing->mode; });
    plan.isolation = plan.isolation.or_else([&] { return existing->isolation; });
    plan.working_dir = plan.working_dir
        .or_else([&] { return existing->worktree_path; })
        .or_else([&] { return existing->cwd; });
    plan.worktree_path = plan.worktree_path.or_else([&] { return existing->worktree_path; });
    plan.worktree_branch = plan.worktree_branch.or_else([&] { return existing->worktree_branch; });
    plan.worktree_base_commit = plan.worktree_base_commit.or_else([&] { return existing->worktree_base_commit; });
    plan.worktree_git_root = plan.worktree_git_root.or_else([&] { return existing->worktree_git_root; });
    plan.teammate_backend = plan.teammate_backend.or_else([&] { return existing->teammate_backend; });
    plan.teammate_task_id = plan.teammate_task_id.or_else([&] { return existing->teammate_task_id; });
    plan.teammate_pane_id = plan.teammate_pane_id.or_else([&] { return existing->teammate_pane_id; });
    plan.teammate_color = plan.teammate_color.or_else([&] { return existing->teammate_color; });
    plan.parent_session_id = plan.parent_session_id.or_else([&] { return existing->parent_session_id; });
}

[[nodiscard]] inline std::expected<AgentExecutionPlan, std::string> build_agent_execution_plan(
    const AgentToolRequest& request,
    const AgentConfig& config
) {
    auto normalized_cwd = normalize_agent_cwd(request.cwd);
    if (!normalized_cwd) return std::unexpected(normalized_cwd.error());

    const auto agents = cc::tools::agent_runtime::get_all_agent_definitions(
        normalized_cwd->has_value() ? std::optional<fs::path>{fs::path{**normalized_cwd}} : std::nullopt);

    // migrated edge case: use resolve_agent_type (std::expected wrapper) so
    // callers get a richer error category. Legacy alias ambiguity and other
    // failure modes are translated to descriptive strings; previous code used
    // the simpler "not found" surface.
    auto resolved_definition = cc::tools::agent_runtime::resolve_agent_type(request.subagent_type, agents);
    if (!resolved_definition) {
        const auto error_kind = cc::tools::agent_runtime::resolution_error_name(resolved_definition.error());
        if (resolved_definition.error() == cc::tools::agent_runtime::ResolutionError::LegacyAliasAmbiguous) {
            return std::unexpected(std::format(
                "Agent type '{}' matched multiple namespaced variants; "
                "specify the fully-qualified agent type explicitly. Available agents: {}",
                request.subagent_type,
                cc::tools::agent_runtime::format_agent_type_list(agents)));
        }
        return std::unexpected(std::format(
            "Agent type '{}' not found ({}). Available agents: {}",
            request.subagent_type,
            error_kind,
            cc::tools::agent_runtime::format_agent_type_list(agents)));
    }
    cc::tools::agent_runtime::AgentDefinition definition = std::move(*resolved_definition);

    auto inline_mcp_servers = prepare_agent_inline_mcp_servers(definition.inline_mcp_servers);
    if (!inline_mcp_servers) {
        return std::unexpected(std::format(
            "Failed to configure MCP servers for agent '{}': {}",
            definition.agent_type,
            inline_mcp_servers.error()));
    }
    AgentMcpCleanupGuard inline_mcp_plan_cleanup{
        .agent_id = {},
        .inline_servers = std::move(*inline_mcp_servers),
    };

    auto definition_model = resolve_agent_model(definition.model);
    AgentExecutionPlan plan;
    plan.agent_id = request.agent_id_override.value_or(next_agent_id(request.name));
    plan.prompt = prepend_initial_prompt(definition.initial_prompt, request.prompt);
    plan.description = request.description;
    plan.agent_type = definition.agent_type;
    plan.is_built_in = definition.source == "built-in";
    plan.model = request.model.value_or(definition_model.value_or(config.default_model));
    plan.system_prompt = definition.system_prompt.empty()
        ? built_in_system_prompt(definition.agent_type)
        : definition.system_prompt;
    if (request.parent_system_prompt && !request.parent_system_prompt->empty()) {
        plan.system_prompt = *request.parent_system_prompt;
        plan.system_prompt_overridden = true;
    }
    plan.preloaded_skill_messages = load_preloaded_skill_messages(*definition);
    plan.agent_mcp_servers = definition.mcp_servers;
    for (const auto& inline_config : definition.inline_mcp_servers) {
        append_unique_agent_mcp_server(plan.agent_mcp_servers, inline_config.name);
    }
    plan.agent_mcp_tools = connect_agent_mcp_servers(plan.agent_mcp_servers);
    if (!plan.agent_mcp_tools.empty()) {
        plan.agent_mcp_context_message = format_agent_mcp_context_message(plan.agent_mcp_tools);
    }
    if (!definition.required_mcp_servers.empty()) {
        const auto available_servers = available_mcp_servers_with_tools();
        const auto missing = missing_required_mcp_servers(definition.required_mcp_servers, available_servers);
        if (!missing.empty()) {
            return std::unexpected(std::format(
                "Agent '{}' requires MCP servers matching: {}. MCP servers with tools: {}. Use /mcp to configure and authenticate the required MCP servers.",
                definition.agent_type,
                join_fields(missing),
                available_servers.empty() ? "none" : join_fields(available_servers)));
        }
    }
    plan.inline_mcp_server_states = std::move(inline_mcp_plan_cleanup.inline_servers);
    plan.allowed_tools = definition.tools;
    plan.disallowed_tools = definition.disallowed_tools;
    plan.max_turns = definition.max_turns.value_or(config.max_turns);
    plan.background = request.run_in_background || definition.background;
    plan.resume_existing = request.resume_existing;
    plan.query_source = request.query_source;
    plan.fork_child_context = request.fork_child_context;
    plan.exact_tools = request.exact_tools;
    plan.use_exact_tools = request.use_exact_tools;
    plan.fork_context_messages = forked_messages_from_parent_assistant_entries(
        request.prompt,
        request.parent_assistant_message_entries);
    if (!plan.fork_context_messages.empty()) {
        plan.fork_child_context = true;
        plan.fork_context_includes_prompt = true;
    }
    auto explicit_fork_context_messages = fork_context_messages_from_entries(request.fork_context_entries);
    plan.fork_context_messages.insert(
        plan.fork_context_messages.end(),
        std::make_move_iterator(explicit_fork_context_messages.begin()),
        std::make_move_iterator(explicit_fork_context_messages.end()));
    // migrated edge case: filter out any assistant messages whose tool_use
    // blocks lack corresponding tool_results. Without this the Anthropic
    // API rejects the request outright ("message with tool_use must be
    // followed by user message with tool_result"). Mirrors TS
    // filterIncompleteToolCalls applied to fork_context_messages.
    if (!plan.fork_context_messages.empty()) {
        plan.fork_context_messages = filter_incomplete_tool_calls(
            std::move(plan.fork_context_messages));
    }
    if (!plan.fork_context_includes_prompt &&
        messages_contain_fork_boilerplate(plan.fork_context_messages)) {
        plan.fork_context_includes_prompt = true;
    }
    if (!plan.fork_child_context) {
        if (auto existing = cc::tools::agent_runtime::native_agent_store().get(plan.agent_id);
            existing && cc::tools::agent_runtime::native_agent_record_is_fork_child(*existing)) {
            plan.fork_child_context = true;
        }
    }
    plan.name = request.name;
    plan.team_name = request.team_name;
    plan.mode = effective_agent_permission_mode(
        request.mode,
        definition.permission_mode,
        config.parent_permission_mode);
    plan.isolation = request.isolation.or_else([&] { return definition.isolation; });
    if (plan.isolation && !cc::tools::agent_runtime::valid_agent_isolation(*plan.isolation)) {
        return std::unexpected(std::format(
            "Unsupported isolation mode '{}'. Valid options for this environment: {}",
            *plan.isolation,
            cc::tools::agent_runtime::valid_agent_isolation_options()));
    }
    plan.working_dir = *normalized_cwd;
    plan.frontmatter_hooks = definition.hooks;
    plan.effort = definition.effort;
    plan.memory = definition.memory;
    if (plan.memory && is_auto_memory_enabled()) {
        add_agent_memory_tools(plan.allowed_tools);
        plan.system_prompt = std::format(
            "{}\n\n{}",
            plan.system_prompt,
            load_agent_memory_prompt(plan.agent_type, *plan.memory, plan.working_dir));
    }
    plan.color = definition.color;
    plan.omit_claude_md = definition.omit_claude_md;
    plan.critical_system_reminder = definition.critical_system_reminder;
    plan.parent_agent_id = config.parent_agent_id;
    hydrate_resume_plan_from_existing_record(plan);
    const auto runtime_context = format_agent_runtime_context(plan);
    if (!plan.system_prompt_overridden) {
        plan.system_prompt = plan.system_prompt.empty()
            ? runtime_context
            : std::format("{}\n\n{}", plan.system_prompt, runtime_context);
    }
    if (plan.critical_system_reminder && !plan.system_prompt_overridden) {
        plan.system_prompt += "\n\n" + format_critical_system_reminder(*plan.critical_system_reminder);
    }
    return plan;
}

[[nodiscard]] inline ToolResult execute_agent_sleep_tool(
    std::string_view agent_id,
    const ToolInput& input
) {
    auto parsed = cc::utils::json::parse(input.json());
    if (!parsed || !parsed->root().is_obj()) {
        return ToolResult::error("sleep requires a JSON object input");
    }

    auto root = parsed->root();
    const auto seconds = std::clamp(
        json_int(root, "duration")
            .or_else([&] { return json_int(root, "seconds"); })
            .value_or(1),
        1,
        static_cast<int>(SleepTool::kMaxDuration.count()));
    auto reason = json_string(root, "reason").value_or("scheduled wait");

    auto signal = std::make_shared<AbortSignal>();
    std::atomic_bool finished{false};
    std::thread watcher([signal, agent_id = std::string(agent_id), &finished] {
        while (!finished.load(std::memory_order_acquire)) {
            if (cc::tools::agent_runtime::native_agent_store().is_cancel_requested(agent_id)) {
                signal->abort();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    SleepTool tool(signal);
    auto result = tool.execute(SleepRequest{
        .duration = std::chrono::seconds(seconds),
        .reason = std::move(reason),
        .resume_hint = json_string(root, "resume_hint"),
    });

    finished.store(true, std::memory_order_release);
    if (watcher.joinable()) watcher.join();

    if (!result) {
        return ToolResult::error(std::string(format_error(result.error())));
    }
    if (result->was_cancelled) {
        return ToolResult::error(std::format(
            "Sleep cancelled after {} ms", result->actual_duration.count()));
    }
    return ToolResult::success(std::format("Slept for {} ms", result->actual_duration.count()));
}

[[nodiscard]] inline ToolResult execute_agent_web_fetch_tool(
    std::string_view agent_id,
    const ToolInput& input
) {
    auto parsed_url = cc::tools::web_fetch::detail::parse_url(input.json());
    if (!parsed_url) {
        return ToolResult::error(parsed_url.error());
    }

    const auto& url = *parsed_url;
    if (!url.starts_with("http://") && !url.starts_with("https://")) {
        return ToolResult::error("URL must start with http:// or https://");
    }

    auto result = cc::tools::web_fetch::detail::run_curl_with_cancel(
        url,
        [agent_id = std::string(agent_id)] {
            return cc::tools::agent_runtime::native_agent_store().is_cancel_requested(agent_id);
        });
    if (!result) return ToolResult::error(result.error());
    if (result->cancelled) {
        return ToolResult::error("WebFetch cancelled by agent stop request");
    }
    if (result->exit_status != 0) {
        return ToolResult::error(std::format("Failed to fetch URL: {}", url));
    }
    return ToolResult::success(std::move(result->output));
}

// =========================================================================
// AgentTool Implementation
// =========================================================================

/// AgentTool - Delegates tasks to sub-agents via recursive API loop
class AgentTool {
public:
    static constexpr std::string_view kName = "Agent";
    static constexpr std::string_view kDescription = 
        "Launch a new agent to handle complex, multi-step tasks autonomously. "
        "The agent runs in a recursive loop, making API calls and executing tools "
        "until it completes the task or reaches the turn limit.";
    
    static ToolDefinition definition() {
        return ToolDefinition{
            .name = std::string(kName),
            .description = std::string(kDescription),
            .input_schema = InputSchema{
                .properties = {
                    agent_schema_property("description", "string", "A short description of the task", true),
                    agent_schema_property("prompt", "string", "The task for the agent to perform", true),
                    agent_schema_property("subagent_type", "string", "The specialized agent type to use"),
                    agent_schema_property("model", "string", "Optional model override: sonnet, opus, haiku, or a concrete model id"),
                    agent_schema_property("run_in_background", "boolean", "Run the agent asynchronously"),
                    agent_schema_property("task", "string", "Legacy alias for prompt"),
                    agent_schema_property("skill", "string", "Legacy alias for subagent_type"),
                    agent_schema_property("name", "string", "Teammate name for agent swarms"),
                    agent_schema_property("team_name", "string", "Team name for agent swarms"),
                    agent_schema_property("mode", "string", "Permission mode for spawned teammates"),
                    agent_schema_property("isolation", "string", "Isolation mode for worktree or remote agents"),
                    agent_schema_property("cwd", "string", "Working directory override for the spawned agent")
                }
            },
            .permission = ToolPermission::Execute,
            .category = "agent"
        };
    }
    
    explicit AgentTool(AgentConfig config = {}, int current_depth = 0,
                       cc::core::ToolRegistry* registry = nullptr,
                       AgentLivePermissionCheckFn permission_check = {},
                       bool permission_hook_valid_for_background = false)
        : config_(config),
          current_depth_(current_depth),
          registry_(registry),
          permission_check_(std::move(permission_check)),
          permission_hook_valid_for_background_(permission_hook_valid_for_background) {}
    
    [[nodiscard]] bool check_permission(const ToolInput& input) const {
        (void)input;
        // Deny if at max recursion depth
        if (current_depth_ >= config_.max_depth) return false;
        return true;
    }
    
    /// Check if a specific tool is allowed for this sub-agent
    [[nodiscard]] bool is_tool_allowed(std::string_view tool_name) const {
        // Check denied list first (takes precedence)
        for (const auto& denied : config_.denied_tools) {
            if (tool_rule_matches_tool_name(denied, tool_name)) return false;
        }
        // Agent(type) rules gate Agent invocations, not the spawned worker's tool pool.
        if (!config_.allowed_tools.empty()) {
            bool has_tool_allow_rule = false;
            for (const auto& allowed : config_.allowed_tools) {
                if (!normalized_tool_name_is(tool_name, "Agent") &&
                    tool_rule_matches_tool_name(allowed, "Agent")) {
                    continue;
                }
                has_tool_allow_rule = true;
                if (tool_rule_matches_tool_name(allowed, tool_name)) return true;
            }
            return !has_tool_allow_rule;
        }
        return true;  // No restrictions = allow all
    }
    
    /// Get config for creating child agents (depth incremented)
    [[nodiscard]] AgentConfig child_config(
        std::string_view parent_agent_id = {},
        const std::optional<std::string>& parent_permission_mode = std::nullopt
    ) const {
        AgentConfig child = config_;
        // Children inherit tool restrictions
        if (!parent_agent_id.empty()) {
            child.parent_agent_id = std::string(parent_agent_id);
        }
        if (parent_permission_mode) {
            child.parent_permission_mode = parent_permission_mode;
        }
        return child;
    }
    
	    [[nodiscard]] int current_depth() const { return current_depth_; }
	    [[nodiscard]] int max_depth() const { return config_.max_depth; }

    [[nodiscard]] AgentLivePermissionCheck check_live_tool_permission(
        std::string_view tool_name,
        std::string_view input_json,
        std::string_view tool_use_id
    ) {
        if (!permission_check_) return AgentLivePermissionCheck{};
        return permission_check_(tool_name, input_json, tool_use_id);
    }
    
    [[nodiscard]] Result<ToolResult> execute(const ToolInput& input) {
        auto request = parse_agent_tool_request(input);
        if (!request) return ToolResult::error(std::format("Invalid Agent input: {}", request.error()));

        if (request->prompt.empty()) {
            return ToolResult::error("Missing required 'prompt' field");
        }

        auto plan = build_agent_execution_plan(*request, config_);
        if (!plan) return ToolResult::error(plan.error());

        if (!plan->team_name) {
            if (auto current_team = cc::utils::get_team_name(); current_team && !current_team->empty()) {
                plan->team_name = std::move(*current_team);
            }
        }

        if (current_session_is_teammate() && plan->team_name && plan->name) {
            return ToolResult::error(
                "Teammates cannot spawn other teammates - the team roster is flat. "
                "To spawn a subagent instead, omit the name parameter.");
        }

        if (cc::utils::is_in_process_teammate() && plan->team_name && plan->background) {
            return ToolResult::error(
                "In-process teammates cannot spawn background agents. "
                "Use run_in_background=false for synchronous subagents.");
        }

        if (!agent_type_allowed_by_permission_rules(
                plan->agent_type,
                config_.allowed_tools,
                config_.denied_tools)) {
            return ToolResult::error(std::format(
                "Agent type '{}' is not allowed by current Agent tool permission rules",
                plan->agent_type));
        }
        
        // Check recursion depth
        if (current_depth_ >= config_.max_depth) {
            return ToolResult::error(std::format(
                "Agent recursion depth limit reached ({}/{})", 
                current_depth_, config_.max_depth));
        }

        if (!plan->resume_existing && plan->team_name && plan->name && plan->isolation) {
            return ToolResult::error("teammate spawn does not support isolation; omit name/team_name to spawn an isolated subagent");
        }

        if (!plan->resume_existing && plan->team_name && plan->name) {
            return start_teammate_agent(std::move(*plan));
        }

        if (!plan->resume_existing && plan->isolation) {
            if (*plan->isolation == "worktree") {
                const auto parent_working_dir = plan->working_dir.value_or(fs::current_path().string());
                auto worktree = create_agent_worktree(*plan);
                if (!worktree) return ToolResult::error(worktree.error());
                plan->working_dir = worktree->path.string();
                plan->worktree_path = worktree->path.string();
                plan->worktree_branch = worktree->branch;
                plan->worktree_base_commit = worktree->head_commit;
                plan->worktree_git_root = worktree->git_root.string();
                if (plan->fork_context_includes_prompt) {
                    plan->fork_context_messages.push_back(Message::from_text(
                        "user",
                        cc::tools::agent_runtime::build_worktree_fork_notice(
                            parent_working_dir,
                            worktree->path.string())));
                } else {
                    plan->system_prompt += std::format(
                        "\n\nNative worktree isolation:\n"
                        "- worktree_path: {}\n"
                        "- worktree_branch: {}\n"
                        "- base_commit: {}\n"
                        "Run all filesystem and shell work from this worktree path.",
                        worktree->path.string(),
                        worktree->branch,
                        worktree->head_commit);
                }
            } else if (*plan->isolation == "remote") {
                return start_remote_agent(std::move(*plan));
            } else {
                return ToolResult::error(std::format("Unsupported isolation mode '{}'", *plan->isolation));
            }
        }

        if (plan->background) {
            return start_background_agent(std::move(*plan));
        }
        
        upsert_agent_record_for_plan(*plan);
        // Run the sub-agent loop
        auto result = run_agent_loop(*plan);
        (void)cleanup_agent_worktree(plan->agent_id);
        return result;
    }

private:
    [[nodiscard]] Result<ToolResult> start_remote_agent(AgentExecutionPlan plan) {
        plan.background = true;
        cc::tools::MessageRouter::instance().register_agent(plan.agent_id);
        upsert_agent_record_for_plan(plan);
        if (!plan.fork_context_includes_prompt) {
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                plan.agent_id,
                "user: " + plan.prompt);
        }
        cc::tools::agent_runtime::native_agent_store().append_transcript(
            plan.agent_id,
            "system: remote isolation launch requested");

        if (!registry_) {
            const auto error = "remote isolation requires the runtime registry so remote_trigger can be executed";
            cc::tools::agent_runtime::native_agent_store().mark_failed(plan.agent_id, error);
            return ToolResult::error(error);
        }

        auto trigger_result = registry_->execute(
            "remote_trigger",
            ToolInput::from_json(remote_agent_trigger_input_json(plan)));
        if (!trigger_result) {
            const auto error = "remote isolation trigger failed: " + trigger_result.error().message;
            cc::tools::agent_runtime::native_agent_store().mark_failed(plan.agent_id, error);
            return ToolResult::error(error);
        }

        auto trigger_output = tool_result_content_text(*trigger_result);
        if (trigger_result->is_error) {
            const auto error = "remote isolation trigger failed: " + trigger_output;
            cc::tools::agent_runtime::native_agent_store().mark_failed(plan.agent_id, error);
            return ToolResult::error(error);
        }

        cc::tools::agent_runtime::native_agent_store().mark_running(plan.agent_id);
        if (!trigger_output.empty()) {
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                plan.agent_id,
                "system: remote trigger delivered: " + trigger_output);
        }
        auto remote_metadata = parse_remote_launch_metadata(trigger_output, plan);
        cc::tools::agent_runtime::native_agent_store().set_remote_metadata(
            plan.agent_id,
            remote_metadata.task_id,
            remote_metadata.task_type,
            remote_metadata.session_id,
            remote_metadata.session_url,
            remote_metadata.title,
            plan.prompt,
            remote_metadata.metadata_json,
            remote_metadata.is_review,
            remote_metadata.is_ultraplan,
            remote_metadata.is_long_running);
        if (remote_metadata.session_id) {
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                plan.agent_id,
                "system: remote session registered: " + *remote_metadata.session_id);
        }
        const bool auto_poll_started = remote_metadata.session_id
            ? cc::tools::agent_runtime::start_remote_agent_poll_loop(plan.agent_id)
            : false;

        const auto session_url_line = remote_metadata.session_url
            ? std::format("\nsession_url: {}", *remote_metadata.session_url)
            : std::string{};
        const auto remote_task_id_line = remote_metadata.task_id && *remote_metadata.task_id != plan.agent_id
            ? std::format("\nremote_task_id: {}", *remote_metadata.task_id)
            : std::string{};
        const auto auto_poll_line = remote_metadata.session_id
            ? std::format("\nremote_auto_poll: {}", auto_poll_started ? "true" : "false")
            : std::string{};

        return ToolResult::success(std::format(
            "Remote agent launched via configured remote trigger.\n"
            "taskId: {}\n"
            "agentId: {}{}{}{}\n"
            "output_file: {}\n"
            "The agent is running remotely. You will be notified when the remote runtime reports completion.",
            plan.agent_id,
            plan.agent_id,
            remote_task_id_line,
            session_url_line,
            auto_poll_line,
            agent_output_file_path(plan.agent_id)));
    }

    [[nodiscard]] Result<ToolResult> start_teammate_agent(AgentExecutionPlan plan) {
        if (!plan.team_name || plan.team_name->empty() || !plan.name || plan.name->empty()) {
            return ToolResult::error("teammate spawn requires team_name and name");
        }

        auto team = cc::tools::global_team_store().get_by_id_or_name(*plan.team_name);
        if (!team) {
            return ToolResult::error(std::format(
                "Team '{}' not found. Call team_create before spawning teammates.",
                *plan.team_name));
        }

        auto normalized_team_name = (*team)->name;
        auto team_id = (*team)->id;
        auto teammate_name = unique_teammate_agent_name(
            sanitize_teammate_agent_name(*plan.name),
            **team);
        auto teammate_id = format_teammate_agent_id(teammate_name, normalized_team_name);
        auto output_file = agent_output_file_path(teammate_id);

        plan.agent_id = teammate_id;
        plan.name = teammate_name;
        plan.team_name = normalized_team_name;
        plan.background = true;

        auto added = cc::tools::global_team_store().add_member(team_id, cc::tools::TeamMember{
            .agent_id = teammate_id,
            .role = teammate_role_for_agent_type(plan.agent_type),
            .status = cc::tools::MemberStatus::Working,
        });
        if (!added) return ToolResult::error(std::string(cc::tools::format_error(added.error())));

        cc::utils::swarm_backends::TeammateSpawnConfig spawn_config{
            .name = teammate_name,
            .team_name = normalized_team_name,
            .color = teammate_agent_color(plan.color),
            .plan_mode_required = plan.mode == "plan",
            .permission_mode = plan.mode,
            .agent_type = plan.agent_type,
            .prompt = plan.prompt,
            .cwd = plan.working_dir.value_or(fs::current_path().string()),
            .model = plan.model.empty() ? std::nullopt : std::optional<std::string>{plan.model},
            .system_prompt = plan.system_prompt.empty()
                ? std::nullopt
                : std::optional<std::string>{plan.system_prompt},
            .worktree_path = plan.worktree_path,
            .parent_session_id = teammate_parent_session_id(),
            .permissions = plan.allowed_tools,
        };
        auto executor = cc::utils::swarm_backends::BackendRegistry::get_teammate_executor(
            config_.prefer_in_process_teammate);
        auto spawned = executor->spawn(spawn_config);
        if (!spawned.success) {
            auto error = spawned.error.value_or("failed to spawn teammate backend");
            (void)cc::tools::global_team_store().update_member_status(
                team_id,
                teammate_id,
                cc::tools::MemberStatus::Error,
                error);
            return ToolResult::error(error);
        }

        auto backend = std::string(cc::utils::swarm_backends::backend_type_name(executor->type()));
        auto task_id = std::move(spawned.task_id);
        auto pane_id = std::move(spawned.pane_id);
        auto color = spawn_config.color
            ? std::optional<std::string>{std::string(cc::utils::swarm_backends::agent_color_name(*spawn_config.color))}
            : std::nullopt;
        auto parent_session_id = std::move(spawn_config.parent_session_id);

        plan.teammate_backend = backend;
        plan.teammate_task_id = task_id;
        plan.teammate_pane_id = pane_id;
        plan.teammate_color = color;
        plan.parent_session_id = parent_session_id;

        const bool runs_in_process = executor->type() == cc::utils::swarm_backends::BackendType::InProcess;
        if (runs_in_process) {
            auto started = start_background_agent(std::move(plan));
            if (!started) return started;
            if (started->is_error) return started;
        } else {
            auto mailbox = cc::utils::write_to_mailbox(
                teammate_name,
                cc::utils::TeammateMessage{
                    .from = "team-lead",
                    .text = plan.prompt,
                    .timestamp = {},
                    .read = false,
                    .color = std::nullopt,
                    .summary = "initial teammate instructions",
                },
                std::optional<std::string_view>{std::string_view(normalized_team_name)});
            if (!mailbox) {
                (void)cc::tools::global_team_store().update_member_status(
                    team_id,
                    teammate_id,
                    cc::tools::MemberStatus::Error,
                    mailbox.error());
                return ToolResult::error("failed to write teammate mailbox: " + mailbox.error());
            }

            cc::tools::MessageRouter::instance().register_agent(plan.agent_id);
            upsert_agent_record_for_plan(plan);
            cc::tools::agent_runtime::native_agent_store().mark_running(plan.agent_id);
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                plan.agent_id,
                std::format("system: spawned teammate via {} backend", backend));
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                plan.agent_id,
                "mailbox initial prompt: " + plan.prompt);
        }

        auto task_line = task_id ? std::format("task_id: {}\n", *task_id) : std::string{};
        auto pane_line = pane_id ? std::format("pane_id: {}\n", *pane_id) : std::string{};
        auto color_line = color ? std::format("color: {}\n", *color) : std::string{};
        auto execution_state = runs_in_process
            ? (registry_ ? std::string{"running"} : std::string{"queued"})
            : std::string{"running in the external teammate backend"};
        auto delivery_channel = runs_in_process
            ? std::string{"native runtime queue"}
            : std::string{"teammate mailbox"};
        return ToolResult::success(std::format(
            "Spawned successfully.\n"
            "agent_id: {}\n"
            "teammate_id: {}\n"
            "name: {}\n"
            "team_name: {}\n"
            "backend: {}\n"
            "{}{}{}"
            "status: teammate_spawned\n"
            "outputFile: {}\n"
            "The agent is {} and will receive instructions via the {}.",
            teammate_id,
            teammate_id,
            teammate_name,
            normalized_team_name,
            backend,
            task_line,
            pane_line,
            color_line,
            output_file,
            execution_state,
            delivery_channel));
    }

    [[nodiscard]] Result<ToolResult> start_background_agent(AgentExecutionPlan plan) {
        cc::tools::MessageRouter::instance().register_agent(plan.agent_id);
        upsert_agent_record_for_plan(plan);

        auto start_hooks = execute_agent_frontmatter_hooks(plan, "SubagentStart");
        if (!start_hooks.ok()) {
            cc::tools::agent_runtime::native_agent_store().mark_failed(plan.agent_id, *start_hooks.error);
            update_teammate_completion_status(plan, false, *start_hooks.error);
            return ToolResult::error(*start_hooks.error);
        }
        if (!start_hooks.output.empty()) {
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                plan.agent_id,
                "hook SubagentStart: " + start_hooks.output);
        }
        plan.hook_additional_contexts = std::move(start_hooks.additional_contexts);

        if (!registry_) {
            if (!plan.fork_context_includes_prompt) {
                cc::tools::agent_runtime::native_agent_store().append_transcript(plan.agent_id, "user: " + plan.prompt);
            }
            return ToolResult::success(std::format(
                "Queued background agent {} ({}) but execution is deferred because no tool registry is attached.\nagentId: {}\noutputFile: {}",
                plan.agent_id,
                plan.agent_type,
                plan.agent_id,
                agent_output_file_path(plan.agent_id)));
        }

        auto config = config_;
        auto depth = current_depth_;
        auto* registry = registry_;
        auto permission_check = permission_hook_valid_for_background_
            ? permission_check_
            : AgentLivePermissionCheckFn{};
        auto permission_hook_valid_for_background = permission_hook_valid_for_background_;
        auto agent_id = plan.agent_id;
        std::thread([
            plan = std::move(plan),
            config = std::move(config),
            depth,
            registry,
            permission_check = std::move(permission_check),
            permission_hook_valid_for_background,
            agent_id
        ]() mutable {
            cc::tools::agent_runtime::native_agent_store().mark_running(agent_id);
            AgentTool worker(
                std::move(config),
                depth,
                registry,
                std::move(permission_check),
                permission_hook_valid_for_background);
            auto result = worker.run_agent_loop(plan);
            (void)cleanup_agent_worktree(agent_id);
            if (result && !result->is_error) {
                auto output = tool_result_content_text(*result);
                cc::tools::agent_runtime::native_agent_store().mark_completed(agent_id, std::move(output));
                update_teammate_completion_status(plan, true, tool_result_content_text(*result));
            } else if (result) {
                auto error_text = tool_result_content_text(*result);
                if (auto current = cc::tools::agent_runtime::native_agent_store().get(agent_id);
                    current && current->status == cc::tools::agent_runtime::NativeAgentStatus::Cancelled) {
                    update_teammate_completion_status(plan, false, current->error.value_or(error_text));
                    return;
                }
                cc::tools::agent_runtime::native_agent_store().mark_failed(agent_id, error_text);
                update_teammate_completion_status(plan, false, error_text);
            } else {
                auto error_text = result.error().format();
                if (auto current = cc::tools::agent_runtime::native_agent_store().get(agent_id);
                    current && current->status == cc::tools::agent_runtime::NativeAgentStatus::Cancelled) {
                    update_teammate_completion_status(plan, false, current->error.value_or(error_text));
                    return;
                }
                cc::tools::agent_runtime::native_agent_store().mark_failed(agent_id, error_text);
                update_teammate_completion_status(plan, false, error_text);
            }
        }).detach();

        return ToolResult::success(std::format(
            "Started background agent {} ({})\nagentId: {}\noutputFile: {}",
            plan.agent_id,
            plan.agent_type,
            plan.agent_id,
            agent_output_file_path(plan.agent_id)));
    }

    [[nodiscard]] bool plan_allows_generic_mcp_tool(const AgentExecutionPlan& plan) const {
        return !plan.agent_mcp_tools.empty() &&
            is_tool_allowed("mcp") &&
            !tool_name_disallowed_by_definition("mcp", plan.disallowed_tools);
    }

    [[nodiscard]] bool agent_base_filter_allows_tool(
        std::string_view tool_name,
        const AgentExecutionPlan& plan
    ) const {
        const auto permission_mode = plan.mode
            ? std::optional<std::string_view>{std::string_view{*plan.mode}}
            : std::nullopt;
        const bool in_process_teammate =
            plan.team_name && plan.teammate_backend &&
            normalized_tool_name_is(*plan.teammate_backend, "in-process");
        return cc::tools::agent::agent_base_filter_allows_tool(
            tool_name,
            plan.is_built_in,
            plan.background,
            permission_mode,
            in_process_teammate);
    }

    [[nodiscard]] bool is_tool_allowed_for_plan(
        std::string_view tool_name,
        const AgentExecutionPlan& plan
    ) const {
        if (tool_name == "mcp" && plan_allows_generic_mcp_tool(plan)) {
            return true;
        }
        return agent_base_filter_allows_tool(tool_name, plan) &&
            is_tool_allowed(tool_name) &&
            tool_name_allowed_by_definition(tool_name, plan.allowed_tools) &&
            !tool_name_disallowed_by_definition(tool_name, plan.disallowed_tools);
    }

    [[nodiscard]] std::optional<std::string> mcp_scope_error_for_plan(
        const ToolInput& input,
        const AgentExecutionPlan& plan
    ) const {
        if (plan.agent_mcp_servers.empty()) return std::nullopt;

        auto doc = cc::utils::json::parse(input.json());
        if (!doc || !doc->root().is_obj()) {
            return "MCP input must be a JSON object in this sub-agent context";
        }

        auto root = doc->root();
        auto server = json_string(root, "server_name").or_else([&] { return json_string(root, "server"); });
        if (!server || server->empty()) {
            return "MCP server_name is required in this sub-agent context";
        }

        for (const auto& allowed : plan.agent_mcp_servers) {
            if (*server == allowed) return std::nullopt;
        }
        return std::format(
            "MCP server '{}' is not available in this sub-agent context. Available servers: {}",
            *server,
            join_fields(plan.agent_mcp_servers));
    }

    [[nodiscard]] std::vector<cc::services::api::ToolDefinition> api_tools_for_plan(
        const AgentExecutionPlan& plan
    ) const {
        std::vector<cc::services::api::ToolDefinition> tools;
        if (!registry_) return tools;

        for (const auto& definition : registry_->get_visible_definitions()) {
            if (plan.use_exact_tools) {
                if (!exact_tools_allow_tool(plan, definition.name)) continue;
            } else if (!is_tool_allowed_for_plan(definition.name, plan)) {
                continue;
            }
            tools.push_back(cc::services::api::ToolDefinition{
                .name = definition.name,
                .description = definition.description,
                .input_schema_json = definition.input_schema.to_json(),
                .defer_load = false,
            });
        }
        return tools;
    }

    void append_queued_agent_messages(
        const AgentExecutionPlan& plan,
        std::vector<Message>& messages
    ) const {
        for (auto& pending_message : cc::tools::agent_runtime::native_agent_store().take_pending_messages(plan.agent_id)) {
            auto queued_message = Message::from_text("user", pending_message);
            append_agent_sidechain_message(plan.agent_id, queued_message);
            messages.push_back(std::move(queued_message));
        }
        SendMessageTool inbox(plan.agent_id);
        while (auto message = inbox.receive()) {
            auto queued_message = Message::from_text(
                "user",
                std::format(
                    "[Message from {} priority={}]\n{}",
                    message->from_agent,
                    message_priority_name(message->priority),
                    message->content));
            append_agent_sidechain_message(plan.agent_id, queued_message);
            messages.push_back(std::move(queued_message));
        }
    }

    /// Run the sub-agent's recursive API loop
    [[nodiscard]] Result<ToolResult> run_agent_loop(const AgentExecutionPlan& plan) {
        auto resumed_record = plan.resume_existing
            ? cc::tools::agent_runtime::native_agent_store().get(plan.agent_id)
            : std::optional<cc::tools::agent_runtime::NativeAgentRecord>{};
        AgentTodoCleanupGuard todo_cleanup{plan.agent_id};
        AgentShellTaskCleanupGuard shell_task_cleanup{plan.agent_id};
        AgentMcpCleanupGuard mcp_cleanup{
            .agent_id = plan.agent_id,
            .inline_servers = plan.inline_mcp_server_states,
        };

        std::string final_output;
        bool agent_hook_prevented_continuation = false;
        std::optional<std::string> agent_hook_stop_reason;
        bool lifecycle_hooks_active = plan.background;
        bool stop_hooks_executed = false;
        auto run_stop_hooks_once = [&](std::string_view last_assistant_message) -> std::optional<std::string> {
            if (!lifecycle_hooks_active || stop_hooks_executed) return std::nullopt;
            stop_hooks_executed = true;
            auto stop_hooks = execute_agent_frontmatter_hooks(plan, "SubagentStop", last_assistant_message);
            if (!stop_hooks.output.empty()) {
                cc::tools::agent_runtime::native_agent_store().append_transcript(
                    plan.agent_id,
                    "hook SubagentStop: " + stop_hooks.output);
            }
            if (!stop_hooks.ok()) {
                cc::tools::agent_runtime::native_agent_store().append_transcript(
                    plan.agent_id,
                    "hook SubagentStop failed: " + *stop_hooks.error);
                return *stop_hooks.error;
            }
            return std::nullopt;
        };

        auto fail_agent = [&](std::string error) -> Result<ToolResult> {
            if (auto hook_error = run_stop_hooks_once(error)) {
                error += "\nSubagentStop hook failed: " + *hook_error;
            }
            cc::tools::agent_runtime::native_agent_store().mark_failed(plan.agent_id, error);
            return ToolResult::error(std::move(error));
        };

        cc::tools::agent_runtime::native_agent_store().mark_running(plan.agent_id);
        if (!plan.fork_context_includes_prompt) {
            cc::tools::agent_runtime::native_agent_store().append_transcript(plan.agent_id, "user: " + plan.prompt);
        }
        auto hook_additional_contexts = plan.hook_additional_contexts;
        if (!plan.background) {
            auto start_hooks = execute_agent_frontmatter_hooks(plan, "SubagentStart");
            if (!start_hooks.ok()) return fail_agent(*start_hooks.error);
            if (!start_hooks.output.empty()) {
                cc::tools::agent_runtime::native_agent_store().append_transcript(
                    plan.agent_id,
                    "hook SubagentStart: " + start_hooks.output);
            }
            hook_additional_contexts.insert(
                hook_additional_contexts.end(),
                std::make_move_iterator(start_hooks.additional_contexts.begin()),
                std::make_move_iterator(start_hooks.additional_contexts.end()));
            lifecycle_hooks_active = true;
        }
        
        auto client = get_default_client();
        
        // Build initial messages
        std::vector<Message> messages;
        for (const auto& context_message : plan.fork_context_messages) {
            append_agent_sidechain_message(plan.agent_id, context_message);
            messages.push_back(context_message);
        }
        for (const auto& skill_message : plan.preloaded_skill_messages) {
            auto message = Message::from_text("user", skill_message);
            append_agent_sidechain_message(plan.agent_id, message);
            messages.push_back(std::move(message));
        }
        const auto hook_message_start = messages.size();
        append_hook_additional_context_messages(messages, hook_additional_contexts);
        for (std::size_t i = hook_message_start; i < messages.size(); ++i) {
            append_agent_sidechain_message(plan.agent_id, messages[i]);
        }
        auto structured_resume_messages = resumed_record
            ? resume_messages_from_sidechain_entries(resumed_record->sidechain_entries)
            : std::vector<Message>{};
        if (!structured_resume_messages.empty()) {
            messages.insert(
                messages.end(),
                std::make_move_iterator(structured_resume_messages.begin()),
                std::make_move_iterator(structured_resume_messages.end()));
        } else if (resumed_record && !resumed_record->transcript.empty()) {
            auto message = Message::from_text("user", format_resumed_agent_context(*resumed_record));
            append_agent_sidechain_message(plan.agent_id, message);
            messages.push_back(std::move(message));
        }
        if (plan.agent_mcp_context_message) {
            auto message = Message::from_text("user", *plan.agent_mcp_context_message);
            append_agent_sidechain_message(plan.agent_id, message);
            messages.push_back(std::move(message));
        }
        if (!plan.fork_context_includes_prompt) {
            auto prompt_message = Message::from_text("user", plan.prompt);
            append_agent_sidechain_message(plan.agent_id, prompt_message);
            messages.push_back(std::move(prompt_message));
        }
        if (plan.critical_system_reminder && !plan.system_prompt_overridden) {
            auto message = Message::from_text(
                "user",
                format_critical_system_reminder(*plan.critical_system_reminder));
            append_agent_sidechain_message(plan.agent_id, message);
            messages.push_back(std::move(message));
        }
        
        auto content_replacement_state = resumed_record
            ? agent_content_replacement_state_from_entries(resumed_record->sidechain_entries)
            : AgentContentReplacementState{};
        mark_seen_tool_result_ids(content_replacement_state, messages);
        const auto content_replacement_skip_tools = registry_
            ? unbounded_tool_result_budget_names(registry_->get_visible_definitions())
            : std::unordered_set<std::string>{};
        const auto content_replacement_thresholds = registry_
            ? tool_result_budget_thresholds(registry_->get_visible_definitions())
            : std::unordered_map<std::string, std::size_t>{};

        auto cancel_if_requested = [&](std::string_view phase) -> std::optional<std::string> {
            if (!cc::tools::agent_runtime::native_agent_store().is_cancel_requested(plan.agent_id)) {
                return std::nullopt;
            }
            const auto reason = std::format("Agent {} cancelled {}", plan.agent_id, phase);
            (void)run_stop_hooks_once(reason);
            cc::tools::agent_runtime::native_agent_store().mark_cancelled(plan.agent_id, reason);
            return reason;
        };
        auto update_turn_progress = [&](int turn, double phase) {
            const auto denominator = static_cast<double>(std::max(plan.max_turns, 1));
            const auto progress = std::min(0.99, (static_cast<double>(turn) + phase) / denominator);
            cc::tools::agent_runtime::native_agent_store().update_progress(plan.agent_id, progress);
        };
        
        for (int turn = 0; turn < plan.max_turns; ++turn) {
            if (auto reason = cancel_if_requested(std::format("before turn {}", turn + 1))) {
                return ToolResult::error(std::move(*reason));
            }
            update_turn_progress(turn, 0.0);
            append_queued_agent_messages(plan, messages);
            (void)apply_agent_tool_result_budget(
                plan.agent_id,
                messages,
                content_replacement_state,
                content_replacement_skip_tools,
                content_replacement_thresholds);

            // Build request
            CreateMessageRequest req;
            req.model = plan.model;
            req.messages = messages;
            req.max_tokens = 16384;
            req.stream = true;
            if (!plan.system_prompt.empty()) req.system_prompt = plan.system_prompt;
            req.tools = api_tools_for_plan(plan);
            apply_agent_effort_to_request(req, plan.effort);
            
            // Perform streaming request
            auto stream_result = client.create_message_stream(req);
            if (!stream_result) {
                return fail_agent(std::format(
                    "Agent API call failed: {}", stream_result.error().message()));
            }
            update_turn_progress(turn, 0.05);
            
            auto& parser = *stream_result;
            
            // Consume the stream and accumulate content blocks
            std::string text_content;
            std::vector<ContentBlock> tool_uses;
            std::string stop_reason;
            ContentBlock current_block;
            std::string accumulated_json;
            bool in_block = false;

            while (true) {
                if (auto reason = cancel_if_requested("while waiting for model stream")) {
                    return ToolResult::error(std::move(*reason));
                }
                auto event_result = parser.next_event();
                if (!event_result) break;
                if (!event_result->has_value()) {
                    if (parser.has_error()) {
                        const auto error = parser.error_details();
                        const auto error_text = error && !error->error_message.empty()
                            ? error->error_message
                            : std::string{"stream connection failed"};
                        return fail_agent(std::format("Agent stream error: {}", error_text));
                    }
                    if (parser.is_finished()) break;
                    // Brief wait for producer
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                
                const auto& event = **event_result;
                
                switch (event.type) {
                    case StreamEventType::MessageStart:
                        update_turn_progress(turn, 0.10);
                        break;

                    case StreamEventType::ContentBlockStart:
                        update_turn_progress(turn, 0.20);
                        in_block = true;
                        current_block = ContentBlock{};
                        if (event.block_type == StreamContentBlockType::Text) {
                            current_block.type = ContentBlockType::Text;
                        } else if (event.block_type == StreamContentBlockType::ToolUse) {
                            current_block.type = ContentBlockType::ToolUse;
                            current_block.tool_use_id = event.tool_use_id;
                            current_block.tool_name = event.tool_name;
                            accumulated_json.clear();
                        }
                        break;
                        
                    case StreamEventType::ContentBlockDelta:
                        update_turn_progress(turn, 0.35);
                        if (event.delta.type == StreamContentBlockType::Text) {
                            current_block.text += event.delta.text;
                        } else if (event.delta.type == StreamContentBlockType::ToolUse) {
                            accumulated_json += event.delta.partial_json;
                        }
                        break;
                        
                    case StreamEventType::ContentBlockStop:
                        update_turn_progress(turn, 0.45);
                        if (in_block) {
                            if (current_block.type == ContentBlockType::Text) {
                                text_content += current_block.text;
                            } else if (current_block.type == ContentBlockType::ToolUse) {
                                current_block.tool_input_json = accumulated_json;
                                tool_uses.push_back(current_block);
                            }
                            in_block = false;
                        }
                        break;
                        
                    case StreamEventType::MessageDelta:
                        update_turn_progress(turn, 0.55);
                        if (event.message_delta.stop_reason) {
                            stop_reason = *event.message_delta.stop_reason;
                        }
                        break;
                        
                    case StreamEventType::MessageStop:
                        update_turn_progress(turn, 0.65);
                        goto stream_done;
                        
                    case StreamEventType::Error:
                        return fail_agent(std::format(
                            "Agent stream error: {}", event.error.error_message));
                        
                    default:
                        break;
                }
            }
            stream_done:
            
            // If stop_reason is "end_turn" and no tool_use, we're done
            if (stop_reason == "end_turn" || tool_uses.empty()) {
                final_output = text_content;
                if (!text_content.empty()) {
                    auto assistant_message = Message::from_text("assistant", text_content);
                    append_agent_sidechain_message(plan.agent_id, assistant_message);
                    cc::tools::agent_runtime::native_agent_store().append_transcript(
                        plan.agent_id,
                        "assistant: " + text_content);
                }
                break;
            }
            
            // Add assistant message to history
            Message assistant_msg;
            assistant_msg.role = "assistant";
            if (!text_content.empty()) {
                assistant_msg.content.push_back(ContentBlock{
                    .type = ContentBlockType::Text,
                    .text = text_content
                });
            }
            for (auto& tu : tool_uses) {
                assistant_msg.content.push_back(tu);
            }
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                plan.agent_id,
                "assistant: " + message_content_text(assistant_msg));
            append_agent_sidechain_message(plan.agent_id, assistant_msg);
            messages.push_back(std::move(assistant_msg));
            const auto& parent_assistant_message = messages.back();
            update_turn_progress(turn, 0.70);

            // Execute tools and add results
            Message tool_result_msg;
            tool_result_msg.role = "user";
            for (const auto& tu : tool_uses) {
                if (auto reason = cancel_if_requested("before executing tool " + tu.tool_name)) {
                    return ToolResult::error(std::move(*reason));
                }
                update_turn_progress(turn, 0.75);
                ContentBlock result_block;
                result_block.type = ContentBlockType::ToolResult;
                result_block.tool_use_id = tu.tool_use_id;
                auto tool_input_json = tu.tool_input_json;
                bool post_tool_hooks_enabled = false;
                bool tool_execution_succeeded = true;
                bool pre_tool_contexts_appended = false;
                bool tool_hook_prevented_continuation = false;
                std::optional<std::string> tool_hook_stop_reason;
                std::vector<std::string> pre_tool_additional_contexts;

                auto append_tool_hook_context = [&](std::string_view event, const std::vector<std::string>& contexts) {
                    if (contexts.empty()) return;
                    if (!result_block.text.empty()) result_block.text += "\n\n";
                    result_block.text += format_tool_hook_additional_context(
                        event,
                        tu.tool_name,
                        tu.tool_use_id,
                        contexts);
                };
                auto append_pre_tool_context_once = [&] {
                    if (pre_tool_contexts_appended) return;
                    pre_tool_contexts_appended = true;
                    append_tool_hook_context("PreToolUse", pre_tool_additional_contexts);
                };
                auto record_hook_prevented_continuation = [&](
                    const AgentHookExecutionResult& hooks,
                    std::string_view fallback_reason
                ) {
                    if (!hooks.prevent_continuation) return;
                    tool_hook_prevented_continuation = true;
                    tool_hook_stop_reason = hooks.stop_reason.value_or(std::string(fallback_reason));
                };

                if (!registry_) {
                    result_block.text = "[Tool execution not available: no registry]";
                    tool_execution_succeeded = false;
                } else if (should_reject_fork_child_agent_call(plan, tu.tool_name, tu.tool_input_json)) {
                    result_block.text =
                        "[Tool execution error: Fork is not available inside a forked worker. Complete your task directly using your tools.]";
                    tool_execution_succeeded = false;
                } else if (!is_tool_allowed_for_plan(tu.tool_name, plan)) {
                    result_block.text = std::format(
                        "[Tool '{}' not available in sub-agent context]", tu.tool_name);
                    tool_execution_succeeded = false;
                } else if (tu.tool_name == "Agent" && current_depth_ + 1 >= config_.max_depth) {
                    result_block.text = std::format(
                        "[Agent recursion depth limit reached ({}/{})]",
                        current_depth_ + 1, config_.max_depth);
                    tool_execution_succeeded = false;
                } else {
                    tool_input_json = apply_agent_tool_execution_context_to_input(
                        tu.tool_name,
                        tu.tool_input_json,
                        plan);
                    if (tu.tool_name == "Agent" && agent_tool_input_omits_agent_type(tool_input_json)) {
                        tool_input_json = build_implicit_fork_agent_input_json(
                            tool_input_json,
                            plan,
                            parent_assistant_message,
                            req.tools);
                    }

                    auto pre_hooks = execute_agent_tool_frontmatter_hooks(
                        plan,
                        "PreToolUse",
                        tu.tool_name,
                        tool_input_json,
                        tu.tool_use_id);
                    if (!pre_hooks.output.empty()) {
                        cc::tools::agent_runtime::native_agent_store().append_transcript(
                            plan.agent_id,
                            std::format("hook PreToolUse:{}: {}", tu.tool_name, pre_hooks.output));
                    }
                    if (!pre_hooks.ok()) {
                        result_block.text = std::format(
                            "[Tool execution denied by PreToolUse hook: {}]", *pre_hooks.error);
                        tool_execution_succeeded = false;
                        record_hook_prevented_continuation(pre_hooks, "Execution stopped by PreToolUse hook");
                        pre_tool_additional_contexts = std::move(pre_hooks.additional_contexts);
                        append_pre_tool_context_once();
                    } else {
                        record_hook_prevented_continuation(pre_hooks, "Execution stopped by PreToolUse hook");
                        pre_tool_additional_contexts = std::move(pre_hooks.additional_contexts);
                        if (pre_hooks.updated_input_json) {
                            tool_input_json = *pre_hooks.updated_input_json;
                            tool_input_json = apply_agent_tool_execution_context_to_input(
                                tu.tool_name,
                                tool_input_json,
                                plan);
                            cc::tools::agent_runtime::native_agent_store().append_transcript(
                                plan.agent_id,
                                std::format("hook PreToolUse:{} updated input: {}", tu.tool_name, tool_input_json));
                        }
                        bool skip_tool_execution = false;
                        auto live_permission = check_live_tool_permission(
                            tu.tool_name,
                            tool_input_json,
                            tu.tool_use_id);
                        if (!live_permission.allowed) {
                            auto reason = live_permission.message.value_or(
                                std::format("Permission denied for tool: {}", tu.tool_name));
                            result_block.text = std::format(
                                "[Tool execution denied by permission hook: {}]",
                                reason);
                            tool_execution_succeeded = false;
                            post_tool_hooks_enabled = true;
                            skip_tool_execution = true;
                        } else if (live_permission.updated_input_json) {
                            tool_input_json = *live_permission.updated_input_json;
                            tool_input_json = apply_agent_tool_execution_context_to_input(
                                tu.tool_name,
                                tool_input_json,
                                plan);
                            cc::tools::agent_runtime::native_agent_store().append_transcript(
                                plan.agent_id,
                                std::format(
                                    "permission hook {} updated input: {}",
                                    tu.tool_name,
                                    tool_input_json));
                        }
                        if (!skip_tool_execution) {
                            auto tool_input = ToolInput::from_json(tool_input_json);
                            if (auto scope_error = mcp_scope_error_for_plan(tool_input, plan)) {
                                result_block.text = std::format("[Tool execution error: {}]", *scope_error);
                                tool_execution_succeeded = false;
                                post_tool_hooks_enabled = true;
                                skip_tool_execution = true;
                            }
                            if (!skip_tool_execution) {
                                post_tool_hooks_enabled = true;
                                if (normalized_tool_name_is(tu.tool_name, "sleep")) {
                                    auto exec_result = execute_agent_sleep_tool(plan.agent_id, tool_input);
                                    if (auto reason = cancel_if_requested("while executing tool sleep")) {
                                        return ToolResult::error(std::move(*reason));
                                    }
                                    tool_execution_succeeded = !exec_result.is_error;
                                    std::string output;
                                    for (const auto& c : exec_result.content) {
                                        if (!output.empty()) output += "\n";
                                        output += c.text;
                                    }
                                    result_block.text = std::move(output);
                                } else if (normalized_tool_name_is(tu.tool_name, "WebFetch")) {
                                    auto exec_result = execute_agent_web_fetch_tool(plan.agent_id, tool_input);
                                    if (auto reason = cancel_if_requested("while executing tool WebFetch")) {
                                        return ToolResult::error(std::move(*reason));
                                    }
                                    tool_execution_succeeded = !exec_result.is_error;
                                    std::string output;
                                    for (const auto& c : exec_result.content) {
                                        if (!output.empty()) output += "\n";
                                        output += c.text;
                                    }
                                    result_block.text = std::move(output);
                                } else if (tu.tool_name == "Agent") {
                                    AgentTool child(
                                        child_config(plan.agent_id, plan.mode),
                                        current_depth_ + 1,
                                        registry_,
                                        permission_check_,
                                        permission_hook_valid_for_background_);
                                    auto child_result = child.execute(tool_input);
                                    if (child_result) {
                                        tool_execution_succeeded = !child_result->is_error;
                                        std::string output;
                                        for (const auto& c : child_result->content) {
                                            if (!output.empty()) output += "\n";
                                            output += c.text;
                                        }
                                        result_block.text = std::move(output);
                                    } else {
                                        tool_execution_succeeded = false;
                                        result_block.text = std::format(
                                            "[Tool execution error: {}]", child_result.error().format());
                                    }
                                } else {
                                    auto exec_result = registry_->execute(tu.tool_name, tool_input);
                                    if (auto reason = cancel_if_requested("while executing tool " + tu.tool_name)) {
                                        return ToolResult::error(std::move(*reason));
                                    }
                                    if (exec_result) {
                                        tool_execution_succeeded = !exec_result->is_error;
                                        // Concatenate all content blocks from the tool result
                                        std::string output;
                                        for (const auto& c : exec_result->content) {
                                            if (!output.empty()) output += "\n";
                                            output += c.text;
                                        }
                                        result_block.text = std::move(output);
                                    } else {
                                        tool_execution_succeeded = false;
                                        result_block.text = std::format(
                                            "[Tool execution error: {}]", exec_result.error().message);
                                    }
                                }
                            }
                        }
                    }
                }

                if (post_tool_hooks_enabled) {
                    const auto hook_event = tool_execution_succeeded ? std::string_view{"PostToolUse"} : std::string_view{"PostToolUseFailure"};
                    const auto output_preview = tool_execution_succeeded
                        ? agent_hook_output_preview(result_block.text)
                        : std::string{};
                    const auto error_preview = tool_execution_succeeded
                        ? std::string{}
                        : agent_hook_output_preview(result_block.text);
                    auto post_hooks = execute_agent_tool_frontmatter_hooks(
                        plan,
                        hook_event,
                        tu.tool_name,
                        tool_input_json,
                        tu.tool_use_id,
                        output_preview,
                        error_preview);
                    if (!post_hooks.output.empty()) {
                        cc::tools::agent_runtime::native_agent_store().append_transcript(
                            plan.agent_id,
                            std::format("hook {}:{}: {}", hook_event, tu.tool_name, post_hooks.output));
                    }
                    if (!post_hooks.ok()) {
                        if (!result_block.text.empty()) result_block.text += "\n\n";
                        result_block.text += std::format(
                            "[{} hook error: {}]", hook_event, *post_hooks.error);
                    }
                    if (hook_event == "PostToolUse") {
                        if (post_hooks.ok() &&
                            is_mcp_tool_name(tu.tool_name) &&
                            post_hooks.updated_mcp_tool_output_text) {
                            result_block.text = *post_hooks.updated_mcp_tool_output_text;
                            cc::tools::agent_runtime::native_agent_store().append_transcript(
                                plan.agent_id,
                                std::format(
                                    "hook PostToolUse:{} updated MCP output: {}",
                                    tu.tool_name,
                                    agent_hook_output_preview(result_block.text)));
                        }
                        record_hook_prevented_continuation(post_hooks, "Execution stopped by PostToolUse hook");
                    }
                    append_pre_tool_context_once();
                    append_tool_hook_context(hook_event, post_hooks.additional_contexts);
                }
                append_pre_tool_context_once();
                if (tool_hook_prevented_continuation) {
                    if (!result_block.text.empty()) result_block.text += "\n\n";
                    result_block.text += std::format(
                        "[Hook stopped continuation: {}]",
                        tool_hook_stop_reason.value_or("Execution stopped by hook"));
                }

                tool_result_msg.content.push_back(std::move(result_block));
                if (tool_hook_prevented_continuation) {
                    agent_hook_prevented_continuation = true;
                    agent_hook_stop_reason = std::move(tool_hook_stop_reason);
                }
                update_turn_progress(turn, 0.85);
            }
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                plan.agent_id,
                "user: " + message_content_text(tool_result_msg));
            if (plan.critical_system_reminder) {
                tool_result_msg.content.push_back(ContentBlock{
                    .type = ContentBlockType::Text,
                    .text = format_critical_system_reminder(*plan.critical_system_reminder)
                });
            }
            append_agent_sidechain_message(plan.agent_id, tool_result_msg);
            messages.push_back(std::move(tool_result_msg));
            update_turn_progress(turn, 0.90);
            
            // Record partial output
            if (!text_content.empty()) {
                final_output += text_content + "\n";
            }
            if (agent_hook_prevented_continuation) {
                const auto stop_text = agent_hook_stop_reason.value_or("Execution stopped by hook");
                if (!final_output.empty() && !final_output.ends_with('\n')) final_output += "\n";
                final_output += stop_text;
                cc::tools::agent_runtime::native_agent_store().append_transcript(
                    plan.agent_id,
                    "hook stopped continuation: " + stop_text);
                break;
            }
        }
        
        if (final_output.empty()) {
            final_output = "[Agent completed without producing output]";
        }

        if (auto hook_error = run_stop_hooks_once(final_output)) return fail_agent(*hook_error);
        cc::tools::agent_runtime::native_agent_store().mark_completed(plan.agent_id, final_output);
        
        return ToolResult::success(final_output);
    }
    
    AgentConfig config_;
    int current_depth_ = 0;
    cc::core::ToolRegistry* registry_ = nullptr;
    AgentLivePermissionCheckFn permission_check_;
    bool permission_hook_valid_for_background_ = false;
};

} // namespace cc::tools::agent

// Export main tool class
export namespace cc::tools {
    using cc::tools::agent::AgentTool;
    using cc::tools::agent::AgentConfig;
    using cc::tools::agent::AgentLivePermissionCheck;
    using cc::tools::agent::AgentLivePermissionCheckFn;

    /// Factory: create AgentTool wrapped as ITool (adapts Result types across modules)
    [[nodiscard]] auto make_agent_tool(AgentConfig config = {},
                                        int depth = 0,
                                        cc::core::ToolRegistry* registry = nullptr,
                                        AgentLivePermissionCheckFn permission_check = {},
                                        bool permission_hook_valid_for_background = false)
        -> std::unique_ptr<cc::core::ITool> {
        struct Adapter final : cc::core::ITool {
            AgentTool tool_;
            cc::core::ToolDefinition def_ = AgentTool::definition();

            explicit Adapter(
                AgentConfig cfg,
                int d,
                cc::core::ToolRegistry* reg,
                AgentLivePermissionCheckFn permission_check,
                bool hook_valid_for_background
            ) : tool_(std::move(cfg), d, reg, std::move(permission_check), hook_valid_for_background) {}

            const cc::core::ToolDefinition& definition() const override { return def_; }
            std::expected<cc::core::ToolResult, cc::core::Error> execute(const cc::core::ToolInput& input) override {
                auto result = tool_.execute(input);
                if (result) return std::move(*result);
                return std::unexpected(cc::core::Error::make(
                    cc::core::ErrorCode::ToolExecutionFailed, result.error().format()));
            }
            bool check_permission(const cc::core::ToolInput& input) const override {
                return tool_.check_permission(input);
            }
        };
        return std::make_unique<Adapter>(
            std::move(config),
            depth,
            registry,
            std::move(permission_check),
            permission_hook_valid_for_background);
    }
}
