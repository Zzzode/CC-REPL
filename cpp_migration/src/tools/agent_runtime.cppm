module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>
#include <functional>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <sstream>
#include <map>
#include <algorithm>
#include <cctype>
#include <iterator>
#include <chrono>
#include <mutex>
#include <ostream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>

export module cc.tools.agent_runtime;

import cc.utils.json;
import cc.utils.team_helpers;
import cc.utils.teleport_utils;
import cc.utils.yaml;

export namespace cc::tools::agent_runtime {

namespace fs = std::filesystem;

inline constexpr std::string_view teammate_system_prompt_addendum = R"(
# Agent Teammate Communication

IMPORTANT: You are running as an agent in a team. To communicate with anyone on your team:
- Use the SendMessage tool with `to: "<name>"` to send messages to specific teammates
- Use the SendMessage tool with `to: "*"` sparingly for team-wide broadcasts

Just writing a response in text is not visible to others on your team - you MUST use the SendMessage tool.

The user interacts primarily with the team lead. Your work is coordinated through the task system and teammate messaging.
)";

struct AgentRuntimeConfig {
    std::string agent_id;
    std::string working_dir;
    std::vector<std::string> capabilities;
    std::optional<std::string> parent_agent_id;
    std::optional<std::string> worktree_path;
    std::optional<std::string> worktree_branch;
    std::optional<std::string> worktree_base_commit;
    std::optional<std::string> worktree_git_root;
    std::optional<std::string> fork_directive;
    bool allow_fork{true};
};

struct AgentExecutionResult {
    std::string agent_id;
    int exit_code;
    std::string output;
    std::optional<std::string> error;
    std::vector<std::string> transcript;
};

enum class AgentLifecycle {
    Starting,
    Running,
    Suspended,
    Completed,
    Failed,
    Cancelled
};

enum class NativeAgentStatus {
    Queued,
    Running,
    Completed,
    Failed,
    Cancelled
};

[[nodiscard]] inline std::string_view native_agent_status_name(NativeAgentStatus status) {
    switch (status) {
        case NativeAgentStatus::Queued: return "queued";
        case NativeAgentStatus::Running: return "running";
        case NativeAgentStatus::Completed: return "completed";
        case NativeAgentStatus::Failed: return "failed";
        case NativeAgentStatus::Cancelled: return "cancelled";
    }
    return "unknown";
}

struct NativeAgentRecord {
    std::string agent_id;
    std::string agent_type;
    std::optional<std::string> parent_agent_id;
    std::optional<std::string> description;
    std::optional<std::string> name;
    std::optional<std::string> team_name;
    std::optional<std::string> cwd;
    std::optional<std::string> isolation;
    std::optional<std::string> mode;
    bool background = false;
    NativeAgentStatus status = NativeAgentStatus::Queued;
    std::chrono::system_clock::time_point created_at = std::chrono::system_clock::now();
    std::chrono::system_clock::time_point updated_at = created_at;
    std::optional<std::string> output;
    std::optional<std::string> error;
    std::vector<std::string> capabilities;
    std::optional<std::string> transcript_path;
    std::optional<std::string> sidechain_jsonl_path;
    std::optional<std::string> output_file_path;
    std::vector<std::string> sidechain_entries;
    std::vector<std::string> pending_messages;
    std::optional<std::string> worktree_path;
    std::optional<std::string> worktree_branch;
    std::optional<std::string> worktree_base_commit;
    std::optional<std::string> worktree_git_root;
    std::optional<std::string> teammate_backend;
    std::optional<std::string> teammate_task_id;
    std::optional<std::string> teammate_pane_id;
    std::optional<std::string> teammate_color;
    std::optional<std::string> parent_session_id;
    std::optional<std::string> remote_task_id;
    std::optional<std::string> remote_task_type;
    std::optional<std::string> remote_session_id;
    std::optional<std::string> remote_session_url;
    std::optional<std::string> remote_title;
    std::optional<std::string> remote_command;
    std::optional<std::string> remote_metadata_json;
    std::optional<std::string> remote_last_event_id;
    std::size_t remote_idle_polls = 0;
    std::vector<std::string> transcript;
    std::optional<double> progress;
    bool cancel_requested = false;
    bool notification_delivered = false;
    bool worktree_cleanup_performed = false;
    bool remote_is_review = false;
    bool remote_is_ultraplan = false;
    bool remote_is_long_running = false;
    bool remote_has_output = false;
};

struct RemoteAgentPollResult {
    std::optional<std::string> session_status;
    std::vector<std::string> events;
    std::optional<std::string> last_event_id;
    std::optional<std::string> completion_output;
    bool result_failed = false;
};

struct RemoteAgentPollApplication {
    std::size_t events_appended = 0;
    bool terminal = false;
    NativeAgentStatus status = NativeAgentStatus::Running;
};

struct AgentInlineMcpServerConfig {
    std::string name;
    std::string transport;
    std::string command;
    std::vector<std::string> args;
    std::unordered_map<std::string, std::string> env;
    std::string url;
    std::unordered_map<std::string, std::string> headers;
    std::string headers_helper;
};

struct AgentHookCommand {
    std::string command;
    std::string shell = "bash";
    std::optional<int> timeout_seconds;
    std::optional<std::string> condition;
};

struct AgentHookMatcher {
    std::optional<std::string> matcher;
    std::vector<AgentHookCommand> hooks;
};

using AgentHooksByEvent = std::map<std::string, std::vector<AgentHookMatcher>, std::less<>>;

struct AgentDefinition {
    std::string agent_type;
    std::string when_to_use;
    std::string model;
    std::string source;
    std::optional<std::string> filename;
    std::optional<std::string> path;
    std::string system_prompt;
    std::vector<std::string> tools;
    std::vector<std::string> disallowed_tools;
    std::optional<std::string> permission_mode;
    std::optional<int> max_turns;
    std::optional<std::string> initial_prompt;
    bool background = false;
    std::optional<std::string> isolation;
    std::vector<std::string> required_mcp_servers;
    std::vector<std::string> mcp_servers;
    std::vector<AgentInlineMcpServerConfig> inline_mcp_servers;
    std::vector<std::string> skills;
    AgentHooksByEvent hooks;
    bool hooks_present = false;
    std::optional<std::string> effort;
    std::optional<std::string> memory;
    std::optional<std::string> color;
    bool omit_claude_md = false;
    std::optional<std::string> critical_system_reminder;
};

struct PluginComponentPaths {
    std::string plugin_name;
    fs::path plugin_dir;
    std::vector<fs::path> agents_paths;
    std::vector<fs::path> skills_paths;
};

[[nodiscard]] inline std::string trim(std::string_view value) {
    auto begin = value.begin();
    auto end = value.end();
    while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) ++begin;
    while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1)))) --end;
    return std::string(begin, end);
}

[[nodiscard]] inline std::string unquote(std::string value) {
    value = trim(value);
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

[[nodiscard]] inline std::vector<std::string> split_list_value(std::string_view value) {
    std::string text = trim(value);
    if (text.size() >= 2 && text.front() == '[' && text.back() == ']') {
        text = text.substr(1, text.size() - 2);
    }
    std::vector<std::string> values;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        auto parsed = unquote(item);
        if (!parsed.empty()) values.push_back(std::move(parsed));
    }
    if (values.empty() && !text.empty()) values.push_back(unquote(text));
    return values;
}

[[nodiscard]] inline std::optional<int> parse_positive_int(std::string_view value) {
    auto text = trim(value);
    if (text.empty()) return std::nullopt;
    int parsed = 0;
    for (char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return std::nullopt;
        parsed = parsed * 10 + (ch - '0');
    }
    return parsed > 0 ? std::optional<int>{parsed} : std::nullopt;
}

[[nodiscard]] inline std::string canonicalize_agent_type(std::string_view value) {
    auto trimmed = trim(value);
    std::string canonical;
    canonical.reserve(trimmed.size());
    for (char ch : trimmed) {
        if (ch == '_' || ch == ' ') {
            canonical.push_back('-');
        } else {
            canonical.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return canonical;
}

[[nodiscard]] inline bool parse_bool_field(std::string_view value, bool fallback = false) {
    auto text = canonicalize_agent_type(value);
    if (text == "true" || text == "1" || text == "yes") return true;
    if (text == "false" || text == "0" || text == "no") return false;
    return fallback;
}

[[nodiscard]] inline bool env_truthy(const char* name) {
    const char* value = std::getenv(name);
    if (!value) return false;
    auto text = canonicalize_agent_type(value);
    return text == "1" || text == "true" || text == "yes" || text == "on";
}

[[nodiscard]] inline bool valid_agent_memory_scope(std::string_view value) {
    return value == "user" || value == "project" || value == "local";
}

[[nodiscard]] inline bool valid_agent_color(std::string_view value) {
    return value == "red" || value == "blue" || value == "green" || value == "yellow" ||
        value == "purple" || value == "orange" || value == "pink" || value == "cyan";
}

[[nodiscard]] inline bool valid_agent_effort(std::string_view value) {
    auto text = canonicalize_agent_type(value);
    if (text == "low" || text == "medium" || text == "high" || text == "max") return true;
    if (text.empty()) return false;
    return std::ranges::all_of(text, [](char ch) {
        return std::isdigit(static_cast<unsigned char>(ch));
    });
}

[[nodiscard]] inline bool is_ant_user_type() {
    const char* value = std::getenv("USER_TYPE");
    return value && std::string_view(value) == "ant";
}

[[nodiscard]] inline bool valid_agent_isolation(std::string_view value) {
    if (value == "worktree") return true;
    if (value == "remote") return is_ant_user_type();
    return false;
}

[[nodiscard]] inline std::string valid_agent_isolation_options() {
    return is_ant_user_type() ? "worktree, remote" : "worktree";
}

[[nodiscard]] inline std::optional<std::string> yaml_scalar_to_string(const cc::utils::YamlValue& value) {
    if (const auto* text = std::get_if<std::string>(&value.data)) return *text;
    if (const auto* flag = std::get_if<bool>(&value.data)) return *flag ? "true" : "false";
    if (const auto* number = std::get_if<int64_t>(&value.data)) return std::to_string(*number);
    if (const auto* number = std::get_if<double>(&value.data)) return std::format("{}", *number);
    return std::nullopt;
}

[[nodiscard]] inline const cc::utils::YamlValue* yaml_field(
    const cc::utils::YamlMap& fields,
    std::string_view key
) {
    auto it = fields.find(std::string(key));
    return it == fields.end() ? nullptr : &it->second;
}

[[nodiscard]] inline std::optional<std::string> yaml_string_field(
    const cc::utils::YamlMap& fields,
    std::string_view key
) {
    const auto* value = yaml_field(fields, key);
    if (!value) return std::nullopt;
    return yaml_scalar_to_string(*value);
}

[[nodiscard]] inline std::vector<std::string> yaml_string_list(const cc::utils::YamlValue& value) {
    std::vector<std::string> values;
    if (const auto* array = std::get_if<cc::utils::YamlArray>(&value.data)) {
        for (const auto& item : *array) {
            if (auto scalar = yaml_scalar_to_string(item); scalar && !scalar->empty()) {
                values.push_back(std::move(*scalar));
            }
        }
        return values;
    }

    if (auto scalar = yaml_scalar_to_string(value)) {
        return split_list_value(*scalar);
    }
    return values;
}

[[nodiscard]] inline std::vector<std::string> yaml_string_list_field(
    const cc::utils::YamlMap& fields,
    std::string_view key
) {
    const auto* value = yaml_field(fields, key);
    return value ? yaml_string_list(*value) : std::vector<std::string>{};
}

inline void append_yaml_string_map(
    const cc::utils::YamlValue& value,
    std::unordered_map<std::string, std::string>& out
) {
    const auto* map = std::get_if<cc::utils::YamlMap>(&value.data);
    if (!map) return;
    for (const auto& [key, item] : *map) {
        if (auto scalar = yaml_scalar_to_string(item)) out[key] = std::move(*scalar);
    }
}

[[nodiscard]] inline std::optional<std::string> json_scalar_to_string(cc::utils::json::JsonVal value) {
    if (!value.valid()) return std::nullopt;
    if (value.is_str()) return std::string(value.as_str());
    if (value.is_bool()) return value.as_bool()
        ? std::optional<std::string>{"true"}
        : std::optional<std::string>{"false"};
    if (value.is_num()) return std::format("{}", value.as_double());
    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string> json_string_field(
    cc::utils::json::JsonVal object,
    std::string_view key
) {
    if (!object.valid() || !object.is_obj()) return std::nullopt;
    return json_scalar_to_string(object.get(key));
}

[[nodiscard]] inline std::vector<std::string> json_string_list(cc::utils::json::JsonVal value) {
    std::vector<std::string> values;
    if (!value.valid()) return values;
    if (value.is_arr()) {
        value.iter([&](cc::utils::json::JsonVal item) {
            if (auto scalar = json_scalar_to_string(item); scalar && !scalar->empty()) {
                values.push_back(std::move(*scalar));
            }
        });
        return values;
    }

    if (auto scalar = json_scalar_to_string(value)) {
        return split_list_value(*scalar);
    }
    return values;
}

[[nodiscard]] inline std::vector<std::string> json_string_list_field(
    cc::utils::json::JsonVal object,
    std::string_view key
) {
    if (!object.valid() || !object.is_obj()) return {};
    return json_string_list(object.get(key));
}

inline void append_json_string_map(
    cc::utils::json::JsonVal value,
    std::unordered_map<std::string, std::string>& out
) {
    if (!value.valid() || !value.is_obj()) return;
    value.iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal item) {
        if (!key.is_str()) return;
        if (auto scalar = json_scalar_to_string(item)) {
            out[std::string(key.as_str())] = std::move(*scalar);
        }
    });
}

[[nodiscard]] inline std::optional<AgentInlineMcpServerConfig> parse_inline_mcp_server_config(
    std::string name,
    const cc::utils::YamlValue& value
) {
    const auto* map = std::get_if<cc::utils::YamlMap>(&value.data);
    if (!map || name.empty()) return std::nullopt;

    AgentInlineMcpServerConfig config;
    config.name = std::move(name);
    if (auto type = yaml_string_field(*map, "type")) config.transport = std::move(*type);
    if (config.transport.empty()) {
        if (auto transport = yaml_string_field(*map, "transport")) config.transport = std::move(*transport);
    }
    if (auto command = yaml_string_field(*map, "command")) config.command = std::move(*command);
    if (auto url = yaml_string_field(*map, "url")) config.url = std::move(*url);
    if (const auto* args = yaml_field(*map, "args")) config.args = yaml_string_list(*args);
    if (const auto* env = yaml_field(*map, "env")) append_yaml_string_map(*env, config.env);
    if (const auto* headers = yaml_field(*map, "headers")) append_yaml_string_map(*headers, config.headers);
    if (auto helper = yaml_string_field(*map, "headersHelper")) config.headers_helper = std::move(*helper);
    if (config.headers_helper.empty()) {
        if (auto helper = yaml_string_field(*map, "headers_helper")) config.headers_helper = std::move(*helper);
    }
    return config;
}

[[nodiscard]] inline std::optional<AgentInlineMcpServerConfig> parse_inline_mcp_server_config(
    std::string name,
    cc::utils::json::JsonVal value
) {
    if (!value.valid() || !value.is_obj() || name.empty()) return std::nullopt;

    AgentInlineMcpServerConfig config;
    config.name = std::move(name);
    if (auto type = json_string_field(value, "type")) config.transport = std::move(*type);
    if (config.transport.empty()) {
        if (auto transport = json_string_field(value, "transport")) config.transport = std::move(*transport);
    }
    if (auto command = json_string_field(value, "command")) config.command = std::move(*command);
    if (auto url = json_string_field(value, "url")) config.url = std::move(*url);
    config.args = json_string_list_field(value, "args");
    append_json_string_map(value.get("env"), config.env);
    append_json_string_map(value.get("headers"), config.headers);
    if (auto helper = json_string_field(value, "headersHelper")) config.headers_helper = std::move(*helper);
    if (config.headers_helper.empty()) {
        if (auto helper = json_string_field(value, "headers_helper")) config.headers_helper = std::move(*helper);
    }
    return config;
}

struct ParsedAgentMcpServers {
    std::vector<std::string> references;
    std::vector<AgentInlineMcpServerConfig> inline_configs;
};

[[nodiscard]] inline ParsedAgentMcpServers parse_agent_mcp_servers(const cc::utils::YamlValue& value) {
    ParsedAgentMcpServers parsed;
    if (const auto* array = std::get_if<cc::utils::YamlArray>(&value.data)) {
        for (const auto& item : *array) {
            if (auto scalar = yaml_scalar_to_string(item); scalar && !scalar->empty()) {
                parsed.references.push_back(std::move(*scalar));
                continue;
            }

            const auto* map = std::get_if<cc::utils::YamlMap>(&item.data);
            if (!map) continue;
            for (const auto& [server_name, config] : *map) {
                if (auto inline_config = parse_inline_mcp_server_config(server_name, config)) {
                    parsed.inline_configs.push_back(std::move(*inline_config));
                }
            }
        }
        return parsed;
    }

    if (const auto* map = std::get_if<cc::utils::YamlMap>(&value.data)) {
        for (const auto& [server_name, config] : *map) {
            if (auto inline_config = parse_inline_mcp_server_config(server_name, config)) {
                parsed.inline_configs.push_back(std::move(*inline_config));
            }
        }
        return parsed;
    }

    if (auto scalar = yaml_scalar_to_string(value)) {
        parsed.references = split_list_value(*scalar);
    }
    return parsed;
}

[[nodiscard]] inline ParsedAgentMcpServers parse_agent_mcp_servers(cc::utils::json::JsonVal value) {
    ParsedAgentMcpServers parsed;
    if (!value.valid()) return parsed;

    if (value.is_arr()) {
        value.iter([&](cc::utils::json::JsonVal item) {
            if (auto scalar = json_scalar_to_string(item); scalar && !scalar->empty()) {
                parsed.references.push_back(std::move(*scalar));
                return;
            }

            if (!item.is_obj()) return;
            item.iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal config) {
                if (!key.is_str()) return;
                if (auto inline_config = parse_inline_mcp_server_config(std::string(key.as_str()), config)) {
                    parsed.inline_configs.push_back(std::move(*inline_config));
                }
            });
        });
        return parsed;
    }

    if (value.is_obj()) {
        value.iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal config) {
            if (!key.is_str()) return;
            if (auto inline_config = parse_inline_mcp_server_config(std::string(key.as_str()), config)) {
                parsed.inline_configs.push_back(std::move(*inline_config));
            }
        });
        return parsed;
    }

    if (auto scalar = json_scalar_to_string(value)) {
        parsed.references = split_list_value(*scalar);
    }
    return parsed;
}

[[nodiscard]] inline std::string canonical_hook_event_name(std::string_view event) {
    auto canonical = canonicalize_agent_type(event);
    if (canonical == "pretooluse" || canonical == "pre-tool-use") return "PreToolUse";
    if (canonical == "posttooluse" || canonical == "post-tool-use") return "PostToolUse";
    if (canonical == "posttoolusefailure" || canonical == "post-tool-use-failure") return "PostToolUseFailure";
    if (canonical == "permissiondenied" || canonical == "permission-denied") return "PermissionDenied";
    if (canonical == "notification") return "Notification";
    if (canonical == "userpromptsubmit" || canonical == "user-prompt-submit") return "UserPromptSubmit";
    if (canonical == "sessionstart" || canonical == "session-start") return "SessionStart";
    if (canonical == "sessionend" || canonical == "session-end") return "SessionEnd";
    if (canonical == "stop") return "SubagentStop";
    if (canonical == "subagentstart" || canonical == "subagent-start") return "SubagentStart";
    if (canonical == "subagentstop" || canonical == "subagent-stop") return "SubagentStop";
    if (canonical == "precompact" || canonical == "pre-compact") return "PreCompact";
    if (canonical == "postcompact" || canonical == "post-compact") return "PostCompact";
    if (canonical == "elicitation") return "Elicitation";
    if (canonical == "elicitationresult" || canonical == "elicitation-result") return "ElicitationResult";
    return std::string(event);
}

[[nodiscard]] inline std::optional<AgentHookCommand> parse_agent_hook_command(const cc::utils::YamlValue& value) {
    AgentHookCommand command;
    if (auto scalar = yaml_scalar_to_string(value)) {
        if (scalar->empty()) return std::nullopt;
        command.command = std::move(*scalar);
        return command;
    }

    const auto* map = std::get_if<cc::utils::YamlMap>(&value.data);
    if (!map) return std::nullopt;

    auto type = yaml_string_field(*map, "type").value_or("command");
    if (!type.empty() && canonicalize_agent_type(type) != "command") return std::nullopt;
    auto raw_command = yaml_string_field(*map, "command").or_else([&] { return yaml_string_field(*map, "cmd"); });
    if (!raw_command || raw_command->empty()) return std::nullopt;
    command.command = std::move(*raw_command);
    if (auto shell = yaml_string_field(*map, "shell"); shell && !shell->empty()) {
        command.shell = std::move(*shell);
    }
    if (auto timeout = yaml_string_field(*map, "timeout").or_else([&] { return yaml_string_field(*map, "timeoutSeconds"); })) {
        command.timeout_seconds = parse_positive_int(*timeout);
    }
    if (auto condition = yaml_string_field(*map, "if"); condition && !condition->empty()) {
        command.condition = std::move(*condition);
    }
    return command;
}

[[nodiscard]] inline std::vector<AgentHookCommand> parse_agent_hook_commands(const cc::utils::YamlValue& value) {
    std::vector<AgentHookCommand> commands;
    if (const auto* array = std::get_if<cc::utils::YamlArray>(&value.data)) {
        for (const auto& item : *array) {
            if (auto command = parse_agent_hook_command(item)) commands.push_back(std::move(*command));
        }
        return commands;
    }

    if (const auto* map = std::get_if<cc::utils::YamlMap>(&value.data)) {
        if (const auto* hooks = yaml_field(*map, "hooks")) {
            return parse_agent_hook_commands(*hooks);
        }
    }

    if (auto command = parse_agent_hook_command(value)) commands.push_back(std::move(*command));
    return commands;
}

[[nodiscard]] inline std::optional<AgentHookMatcher> parse_agent_hook_matcher(const cc::utils::YamlValue& value) {
    AgentHookMatcher matcher;
    if (const auto* map = std::get_if<cc::utils::YamlMap>(&value.data)) {
        if (auto match = yaml_string_field(*map, "matcher"); match && !match->empty()) {
            matcher.matcher = std::move(*match);
        }
        if (const auto* hooks = yaml_field(*map, "hooks")) {
            matcher.hooks = parse_agent_hook_commands(*hooks);
        } else if (auto command = parse_agent_hook_command(value)) {
            matcher.hooks.push_back(std::move(*command));
        }
    } else if (auto command = parse_agent_hook_command(value)) {
        matcher.hooks.push_back(std::move(*command));
    }

    if (matcher.hooks.empty()) return std::nullopt;
    return matcher;
}

[[nodiscard]] inline std::vector<AgentHookMatcher> parse_agent_hook_matchers(const cc::utils::YamlValue& value) {
    std::vector<AgentHookMatcher> matchers;
    if (const auto* array = std::get_if<cc::utils::YamlArray>(&value.data)) {
        for (const auto& item : *array) {
            if (auto matcher = parse_agent_hook_matcher(item)) matchers.push_back(std::move(*matcher));
        }
        return matchers;
    }

    if (const auto* map = std::get_if<cc::utils::YamlMap>(&value.data)) {
        if (map->contains("hooks") || map->contains("command") || map->contains("cmd")) {
            if (auto matcher = parse_agent_hook_matcher(value)) matchers.push_back(std::move(*matcher));
            return matchers;
        }
        for (const auto& [matcher_name, hooks] : *map) {
            auto commands = parse_agent_hook_commands(hooks);
            if (!commands.empty()) {
                matchers.push_back(AgentHookMatcher{
                    .matcher = matcher_name,
                    .hooks = std::move(commands),
                });
            }
        }
        return matchers;
    }

    if (auto matcher = parse_agent_hook_matcher(value)) matchers.push_back(std::move(*matcher));
    return matchers;
}

[[nodiscard]] inline AgentHooksByEvent parse_agent_hooks(const cc::utils::YamlValue& value) {
    AgentHooksByEvent hooks;
    if (const auto* map = std::get_if<cc::utils::YamlMap>(&value.data)) {
        for (const auto& [event, event_hooks] : *map) {
            auto matchers = parse_agent_hook_matchers(event_hooks);
            if (!matchers.empty()) {
                auto target_event = canonical_hook_event_name(event);
                auto& out = hooks[target_event];
                out.insert(out.end(), std::make_move_iterator(matchers.begin()), std::make_move_iterator(matchers.end()));
            }
        }
        return hooks;
    }

    if (const auto* array = std::get_if<cc::utils::YamlArray>(&value.data)) {
        for (const auto& item : *array) {
            if (auto event = yaml_scalar_to_string(item); event && !event->empty()) {
                hooks.try_emplace(canonical_hook_event_name(*event), std::vector<AgentHookMatcher>{});
            }
        }
    } else if (auto event = yaml_scalar_to_string(value); event && !event->empty()) {
        hooks.try_emplace(canonical_hook_event_name(*event), std::vector<AgentHookMatcher>{});
    }
    return hooks;
}

[[nodiscard]] inline std::vector<std::string> agent_alias_candidates(std::string_view requested_type) {
    const auto canonical = canonicalize_agent_type(requested_type);
    if (canonical == "explore") return {"explore", "code-explorer"};
    if (canonical == "explorer") return {"explore", "code-explorer", "explorer"};
    if (canonical == "plan") return {"plan"};
    if (canonical == "planner") return {"plan"};
    return {canonical};
}

enum class ResolutionError {
    EmptyRequestedType,
    ExactOrCanonicalMatchNotFound,
    LegacyAliasAmbiguous,
    LegacyAliasNoMatch,
    SuffixMatchAmbiguous,
    NoCompatibleMatch,
};

[[nodiscard]] inline std::string_view resolution_error_name(ResolutionError err) {
    switch (err) {
        case ResolutionError::EmptyRequestedType:        return "empty_requested_type";
        case ResolutionError::ExactOrCanonicalMatchNotFound: return "exact_or_canonical_match_not_found";
        case ResolutionError::LegacyAliasAmbiguous:      return "legacy_alias_ambiguous";
        case ResolutionError::LegacyAliasNoMatch:        return "legacy_alias_no_match";
        case ResolutionError::SuffixMatchAmbiguous:      return "suffix_match_ambiguous";
        case ResolutionError::NoCompatibleMatch:         return "no_compatible_match";
    }
    return "unknown";
}

[[nodiscard]] inline std::optional<std::string> find_canonical_agent_type_match(
    std::string_view requested_type,
    const std::vector<AgentDefinition>& agents
) {
    const auto canonical_requested = canonicalize_agent_type(requested_type);
    for (const auto& agent : agents) {
        if (canonicalize_agent_type(agent.agent_type) == canonical_requested) {
            return agent.agent_type;
        }
    }
    return std::nullopt;
}

// Resolves a user-provided agent type string into a concrete agent.
//
// Resolution order (matches TypeScript resolveRequestedAgentType):
//   1. Exact string equality on agent.agent_type
//      // Test case: exact match when present  →  Plan → "Plan" (preserves casing)
//   2. Case- and separator-insensitive canonical match
//      // Test case: case-insensitive + space/dash variants  →  "General Purpose" → "general-purpose"
//   3. Legacy alias expansion (explore/explorer/plan/planner) with canonical match
//      // Test case: legacy Explore → namespaced code-explorer  →  "Explore" → "feature-dev:code-explorer"
//   4. Legacy alias expansion with `:alias` suffix filter (single match only)
//      // Test case: ambiguous legacy suffix → undefined  →  "Explore" with two ":code-explorer" agents → nullopt
//   5. Otherwise nullopt
//      // Test case: no compatible match  →  "non-existent-agent" → undefined
[[nodiscard]] inline std::optional<std::string> resolve_requested_agent_type(
    std::string_view requested_type,
    const std::vector<AgentDefinition>& agents
) {
    const auto requested = trim(requested_type);
    // migrated edge case: empty/whitespace-only input returns nullopt
    if (requested.empty()) return std::nullopt;

    // migrated edge case: exact match preserves original casing ("Plan" != canonical "plan")
    for (const auto& agent : agents) {
        if (agent.agent_type == requested) return agent.agent_type;
    }

    // migrated edge case: canonical match folds case, underscores, spaces to dashes
    if (auto canonical = find_canonical_agent_type_match(requested, agents)) {
        return canonical;
    }

    // migrated edge case: legacy alias table (explore/explorer/plan/planner)
    for (const auto& alias : agent_alias_candidates(requested)) {
        if (auto alias_match = find_canonical_agent_type_match(alias, agents)) {
            return alias_match;
        }
    }

    // migrated edge case: suffix match `:alias` requires exactly one candidate
    for (const auto& alias : agent_alias_candidates(requested)) {
        const auto suffix = ":" + alias;
        std::vector<std::string> matches;
        for (const auto& agent : agents) {
            const auto canonical = canonicalize_agent_type(agent.agent_type);
            if (canonical.ends_with(suffix)) matches.push_back(agent.agent_type);
        }
        // migrated edge case: ambiguous suffix match (e.g. two code-explorer
        // variants under different namespaces) returns nullopt instead of first
        if (matches.size() == 1) return matches.front();
    }

    // migrated edge case: no compatible match returns nullopt (caller surfaces error)
    return std::nullopt;
}

// Rich-result wrapper around resolve_requested_agent_type: returns the matched
// AgentDefinition directly, or a ResolutionError explaining why resolution failed.
//
// This is the C++-idiomatic public API consumed by cc.tools.agent_type_resolution.
[[nodiscard]] inline std::expected<AgentDefinition, ResolutionError> resolve_agent_type(
    std::string_view id,
    const std::vector<AgentDefinition>& agents
) {
    const auto requested = trim(id);
    if (requested.empty()) return std::unexpected(ResolutionError::EmptyRequestedType);

    auto resolved = resolve_requested_agent_type(requested, agents);
    if (!resolved) {
        const auto canonical = canonicalize_agent_type(requested);
        const bool is_legacy_alias =
            canonical == "explore" || canonical == "explorer" ||
            canonical == "plan" || canonical == "planner";
        if (is_legacy_alias) {
            bool ambiguous = false;
            for (const auto& alias : agent_alias_candidates(requested)) {
                const auto suffix = ":" + alias;
                std::size_t count = 0;
                for (const auto& agent : agents) {
                    if (canonicalize_agent_type(agent.agent_type).ends_with(suffix)) ++count;
                }
                if (count > 1) { ambiguous = true; break; }
            }
            return std::unexpected(ambiguous
                ? ResolutionError::LegacyAliasAmbiguous
                : ResolutionError::LegacyAliasNoMatch);
        }
        return std::unexpected(ResolutionError::NoCompatibleMatch);
    }

    for (const auto& agent : agents) {
        if (agent.agent_type == *resolved) return agent;
    }
    return std::unexpected(ResolutionError::NoCompatibleMatch);
}

[[nodiscard]] inline std::string format_agent_type_list(
    const std::vector<AgentDefinition>& agents
) {
    std::string output;
    for (const auto& agent : agents) {
        if (!output.empty()) output += ", ";
        output += agent.agent_type;
    }
    return output;
}

[[nodiscard]] inline bool is_sdk_entrypoint() {
    const char* entrypoint = std::getenv("CLAUDE_CODE_ENTRYPOINT");
    const std::string_view entry = entrypoint ? std::string_view(entrypoint) : std::string_view{};
    return entry == "sdk-ts" || entry == "sdk-py" || entry == "sdk-cli";
}

// --- built-in agent system prompt and configuration constants -------------
// Migrated from src/tools/AgentTool/built-in/*.ts (Agent 1 migration).
//
// Inline implementations live here (rather than in built_in_agents.cppm) to
// avoid a circular module import: built_in_agents.cppm already imports
// agent_runtime for the AgentDefinition type. Callers outside agent_runtime
// should use cc::tools::built_in_agents::get_built_in_agents() which returns
// equivalent definitions.

namespace builtin_detail {

// Tool-name constants aligned with the TS prompt strings.
inline constexpr std::string_view kBash = "Bash";
inline constexpr std::string_view kRead = "Read";
inline constexpr std::string_view kEdit = "Edit";
inline constexpr std::string_view kWrite = "Write";
inline constexpr std::string_view kGlob = "Glob";
inline constexpr std::string_view kGrep = "Grep";
inline constexpr std::string_view kNotebookEdit = "NotebookEdit";
inline constexpr std::string_view kExitPlanMode = "ExitPlanMode";
inline constexpr std::string_view kAgent = "Agent";
inline constexpr std::string_view kWebFetch = "WebFetch";
inline constexpr std::string_view kWebSearch = "WebSearch";
inline constexpr std::string_view kSendMessage = "SendMessage";

// For the open-source C++ port we use the dedicated Glob/Grep tool path (the
// ant-native embedded-search branch uses find/grep aliases via Bash).
inline constexpr bool kEmbeddedSearch = false;

[[nodiscard]] inline bool is_ant() {
    const char* v = std::getenv("USER_TYPE");
    return v && std::string_view(v) == "ant";
}

// ---- general-purpose ----
inline constexpr std::string_view kGpPrefix =
    R"(You are an agent for Claude Code, Anthropic's official CLI for Claude. Given the user's message, you should use the tools available to complete the task. Complete the task fully—don't gold-plate, but don't leave it half-done.)";
inline constexpr std::string_view kGpGuidelines =
    R"(Your strengths:
- Searching for code, configurations, and patterns across large codebases
- Analyzing multiple files to understand system architecture
- Investigating complex questions that require exploring many files
- Performing multi-step research tasks

Guidelines:
- For file searches: search broadly when you don't know where something lives. Use Read when you know the specific file path.
- For analysis: Start broad and narrow down. Use multiple search strategies if the first doesn't yield results.
- Be thorough: Check multiple locations, consider different naming conventions, look for related files.
- NEVER create files unless they're absolutely necessary for achieving your goal. ALWAYS prefer editing an existing file to creating a new one.
- NEVER proactively create documentation files (*.md) or README files. Only create documentation files if explicitly requested.)";

[[nodiscard]] inline std::string gp_prompt() {
    return std::format(
        "{} When you complete the task, respond with a concise report covering what was done and any key findings — the caller will relay this to the user, so it only needs the essentials.\n\n{}",
        kGpPrefix, kGpGuidelines
    );
}

inline constexpr std::string_view kGpWhen =
    "General-purpose agent for researching complex questions, searching for code, and executing multi-step tasks. When you are searching for a keyword or file and are not confident that you will find the right match in the first few tries use this agent to perform the search for you.";

// ---- explore ----
[[nodiscard]] inline std::string explore_prompt() {
    const bool emb = kEmbeddedSearch;
    const auto glob_s = emb ? std::format("- Use `find` via {} for broad file pattern matching", kBash)
                            : std::format("- Use {} for broad file pattern matching", kGlob);
    const auto grep_s = emb ? std::format("- Use `grep` via {} for searching file contents with regex", kBash)
                            : std::format("- Use {} for searching file contents with regex", kGrep);
    const std::string bash_tail = emb ? ", grep" : "";
    return std::format(
        R"(You are a file search specialist for Claude Code, Anthropic's official CLI for Claude. You excel at thoroughly navigating and exploring codebases.

=== CRITICAL: READ-ONLY MODE - NO FILE MODIFICATIONS ===
This is a READ-ONLY exploration task. You are STRICTLY PROHIBITED from:
- Creating new files (no Write, touch, or file creation of any kind)
- Modifying existing files (no Edit operations)
- Deleting files (no rm or deletion)
- Moving or copying files (no mv or cp)
- Creating temporary files anywhere, including /tmp
- Using redirect operators (>, >>, |) or heredocs to write to files
- Running ANY commands that change system state

Your role is EXCLUSIVELY to search and analyze existing code. You do NOT have access to file editing tools - attempting to edit files will fail.

Your strengths:
- Rapidly finding files using glob patterns
- Searching code and text with powerful regex patterns
- Reading and analyzing file contents

Guidelines:
{}
{}
- Use {} when you know the specific file path you need to read
- Use {} ONLY for read-only operations (ls, git status, git log, git diff, find{}, cat, head, tail)
- NEVER use {} for: mkdir, touch, rm, cp, mv, git add, git commit, npm install, pip install, or any file creation/modification
- Adapt your search approach based on the thoroughness level specified by the caller
- Communicate your final report directly as a regular message - do NOT attempt to create files

NOTE: You are meant to be a fast agent that returns output as quickly as possible. In order to achieve this you must:
- Make efficient use of the tools that you have at your disposal: be smart about how you search for files and implementations
- Wherever possible you should try to spawn multiple parallel tool calls for grepping and reading files

Complete the user's search request efficiently and report your findings clearly.)",
        glob_s, grep_s, kRead, kBash, bash_tail, kBash
    );
}

inline constexpr std::string_view kExploreWhen =
    "Fast agent specialized for exploring codebases. Use this when you need to quickly find files by patterns (eg. \"src/components/**/*.tsx\"), search code for keywords (eg. \"API endpoints\"), or answer questions about the codebase (eg. \"how do API endpoints work?\"). When calling this agent, specify the desired thoroughness level: \"quick\" for basic searches, \"medium\" for moderate exploration, or \"very thorough\" for comprehensive analysis across multiple locations and naming conventions.";

// ---- plan ----
[[nodiscard]] inline std::string plan_prompt() {
    const auto search = kEmbeddedSearch
        ? std::format("`find`, `grep`, and {}", kRead)
        : std::format("{}, {}, and {}", kGlob, kGrep, kRead);
    const std::string bash_tail = kEmbeddedSearch ? ", grep" : "";
    return std::format(
        R"(You are a software architect and planning specialist for Claude Code. Your role is to explore the codebase and design implementation plans.

=== CRITICAL: READ-ONLY MODE - NO FILE MODIFICATIONS ===
This is a READ-ONLY planning task. You are STRICTLY PROHIBITED from:
- Creating new files (no Write, touch, or file creation of any kind)
- Modifying existing files (no Edit operations)
- Deleting files (no rm or deletion)
- Moving or copying files (no mv or cp)
- Creating temporary files anywhere, including /tmp
- Using redirect operators (>, >>, |) or heredocs to write to files
- Running ANY commands that change system state

Your role is EXCLUSIVELY to explore the codebase and design implementation plans. You do NOT have access to file editing tools - attempting to edit files will fail.

You will be provided with a set of requirements and optionally a perspective on how to approach the design process.

## Your Process

1. **Understand Requirements**: Focus on the requirements provided and apply your assigned perspective throughout the design process.

2. **Explore Thoroughly**:
   - Read any files provided to you in the initial prompt
   - Find existing patterns and conventions using {}
   - Understand the current architecture
   - Identify similar features as reference
   - Trace through relevant code paths
   - Use {} ONLY for read-only operations (ls, git status, git log, git diff, find{}, cat, head, tail)
   - NEVER use {} for: mkdir, touch, rm, cp, mv, git add, git commit, npm install, pip install, or any file creation/modification

3. **Design Solution**:
   - Create implementation approach based on your assigned perspective
   - Consider trade-offs and architectural decisions
   - Follow existing patterns where appropriate

4. **Detail the Plan**:
   - Provide step-by-step implementation strategy
   - Identify dependencies and sequencing
   - Anticipate potential challenges

## Required Output

End your response with:

### Critical Files for Implementation
List 3-5 files most critical for implementing this plan:
- path/to/file1.ts
- path/to/file2.ts
- path/to/file3.ts

REMEMBER: You can ONLY explore and plan. You CANNOT and MUST NOT write, edit, or modify any files. You do NOT have access to file editing tools.)",
        search, kBash, bash_tail, kBash
    );
}

inline constexpr std::string_view kPlanWhen =
    "Software architect agent for designing implementation plans. Use this when you need to plan the implementation strategy for a task. Returns step-by-step plans, identifies critical files, and considers architectural trade-offs.";

// ---- statusline-setup ----
inline constexpr std::string_view kStatuslinePrompt =
    R"(You are a status line setup agent for Claude Code. Your job is to create or update the statusLine command in the user's Claude Code settings.

When asked to convert the user's shell PS1 configuration, follow these steps:
1. Read the user's shell configuration files in this order of preference:
   - ~/.zshrc
   - ~/.bashrc
   - ~/.bash_profile
   - ~/.profile

2. Extract the PS1 value using this regex pattern: /(?:^|\n)\s*(?:export\s+)?PS1\s*=\s*["']([^"']+)["']/m

3. Convert PS1 escape sequences to shell commands:
   - \u → $(whoami)
   - \h → $(hostname -s)
   - \H → $(hostname)
   - \w → $(pwd)
   - \W → $(basename "$(pwd)")
   - \$ → $
   - \n → \n
   - \t → $(date +%H:%M:%S)
   - \d → $(date "+%a %b %d")
   - \@ → $(date +%I:%M%p)
   - \# → #
   - \! → !

4. When using ANSI color codes, be sure to use `printf`. Do not remove colors. Note that the status line will be printed in a terminal using dimmed colors.

5. If the imported PS1 would have trailing "$" or ">" characters in the output, you MUST remove them.

6. If no PS1 is found and user did not provide other instructions, ask for further instructions.

How to use the statusLine command:
1. The statusLine command will receive the following JSON input via stdin:
   {
     "session_id": "string",
     "session_name": "string",
     "transcript_path": "string",
     "cwd": "string",
     "model": {
       "id": "string",
       "display_name": "string"
     },
     "workspace": {
       "current_dir": "string",
       "project_dir": "string",
       "added_dirs": ["string"]
     },
     "version": "string",
     "output_style": {
       "name": "string"
     },
     "context_window": {
       "total_input_tokens": 0,
       "total_output_tokens": 0,
       "context_window_size": 0,
       "current_usage": {
         "input_tokens": 0,
         "output_tokens": 0,
         "cache_creation_input_tokens": 0,
         "cache_read_input_tokens": 0
       },
       "used_percentage": 0,
       "remaining_percentage": 0
     },
     "rate_limits": {
       "five_hour": {
         "used_percentage": 0,
         "resets_at": 0
       },
       "seven_day": {
         "used_percentage": 0,
         "resets_at": 0
       }
     },
     "vim": {
       "mode": "INSERT"
     },
     "agent": {
       "name": "string",
       "type": "string"
     },
     "worktree": {
       "name": "string",
       "path": "string",
       "branch": "string",
       "original_cwd": "string",
       "original_branch": "string"
     }
   }

   You can use this JSON data in your command like:
   - $(cat | jq -r '.model.display_name')
   - $(cat | jq -r '.workspace.current_dir')
   - $(cat | jq -r '.output_style.name')

   Or store it in a variable first:
   - input=$(cat); echo "$(echo "$input" | jq -r '.model.display_name') in $(echo "$input" | jq -r '.workspace.current_dir')"

   To display context remaining percentage:
   - input=$(cat); remaining=$(echo "$input" | jq -r '.context_window.remaining_percentage // empty'); [ -n "$remaining" ] && echo "Context: $remaining% remaining"

   To display Claude.ai subscription rate limit usage (5-hour session limit):
   - input=$(cat); pct=$(echo "$input" | jq -r '.rate_limits.five_hour.used_percentage // empty'); [ -n "$pct" ] && printf "5h: %.0f%%" "$pct"

   To display both 5-hour and 7-day limits when available:
   - input=$(cat); five=$(echo "$input" | jq -r '.rate_limits.five_hour.used_percentage // empty'); week=$(echo "$input" | jq -r '.rate_limits.seven_day.used_percentage // empty'); out=""; [ -n "$five" ] && out="5h:$(printf '%.0f' "$five")%"; [ -n "$week" ] && out="$out 7d:$(printf '%.0f' "$week")%"; echo "$out"

2. For longer commands, save a new file in ~/.claude, e.g. ~/.claude/statusline-command.sh, and reference it in settings.

3. Update the user's ~/.claude/settings.json with:
   { "statusLine": { "type": "command", "command": "your_command_here" } }

4. If ~/.claude/settings.json is a symlink, update the target file instead.

Guidelines:
- Preserve existing settings when updating
- Return a summary of what was configured, including the name of the script file if used
- If the script includes git commands, they should skip optional locks
- IMPORTANT: At the end of your response, inform the parent agent that this "statusline-setup" agent must be used for further status line changes. Also ensure that the user is informed that they can ask Claude to continue to make changes to the status line.)";

inline constexpr std::string_view kStatuslineWhen =
    "Use this agent to configure the user's Claude Code status line setting.";

// ---- verification ----
[[nodiscard]] inline std::string verification_prompt() {
    return std::format(
        R"(You are a verification specialist. Your job is not to confirm the implementation works — it's to try to break it.

You have two documented failure patterns. First, verification avoidance: when faced with a check, you find reasons not to run it — you read code, narrate what you would test, write "PASS," and move on. Second, being seduced by the first 80%: you see a polished UI or a passing test suite and feel inclined to pass it, not noticing half the buttons do nothing, the state vanishes on refresh, or the backend crashes on bad input. The first 80% is the easy part. Your entire value is in finding the last 20%. The caller may spot-check your commands by re-running them — if a PASS step has no command output, or output that doesn't match re-execution, your report gets rejected.

=== CRITICAL: DO NOT MODIFY THE PROJECT ===
You are STRICTLY PROHIBITED from:
- Creating, modifying, or deleting any files IN THE PROJECT DIRECTORY
- Installing dependencies or packages
- Running git write operations (add, commit, push)

You MAY write ephemeral test scripts to a temp directory (/tmp or $TMPDIR) via {} redirection when inline commands aren't sufficient. Clean up after yourself.

Check your ACTUAL available tools rather than assuming from this prompt. You may have browser automation (mcp__claude-in-chrome__*, mcp__playwright__*), {}, or other MCP tools depending on the session — do not skip capabilities you didn't think to check for.

=== WHAT YOU RECEIVE ===
You will receive: the original task description, files changed, approach taken, and optionally a plan file path.

=== VERIFICATION STRATEGY ===
Adapt your strategy based on what was changed:

**Frontend changes**: Start dev server → use browser automation tools to navigate, screenshot, click, and read console → curl sample subresources → run frontend tests
**Backend/API changes**: Start server → curl/fetch endpoints → verify response shapes → test error handling → check edge cases
**CLI/script changes**: Run with representative inputs → verify stdout/stderr/exit codes → test edge inputs → verify --help output
**Infrastructure/config changes**: Validate syntax → dry-run where possible → check env vars / secrets are referenced
**Library/package changes**: Build → full test suite → import from fresh context and exercise public API
**Bug fixes**: Reproduce the original bug → verify fix → run regression tests → check side effects
**Mobile (iOS/Android)**: Clean build → install on simulator/emulator → dump accessibility/UI tree → tap → kill/relaunch for persistence → check crash logs
**Data/ML pipeline**: Run with sample input → verify output shape/schema/types → test empty/NaN/null handling → check for silent data loss
**Database migrations**: Run migration up → verify schema → run migration down → test against existing data
**Refactoring (no behavior change)**: Test suite MUST pass unchanged → diff public API surface → spot-check observable behavior
**Other change types**: The pattern is always the same — (a) exercise the change directly, (b) check outputs against expectations, (c) try to break it.

=== REQUIRED STEPS (universal baseline) ===
1. Read the project's CLAUDE.md / README for build/test commands and conventions.
2. Run the build (if applicable). A broken build is an automatic FAIL.
3. Run the project's test suite (if it has one). Failing tests are an automatic FAIL.
4. Run linters/type-checkers if configured.
5. Check for regressions in related code.

=== RECOGNIZE YOUR OWN RATIONALIZATIONS ===
- "The code looks correct based on my reading" → reading is not verification. Run it.
- "The implementer's tests already pass" → verify independently.
- "This is probably fine" → probably is not verified. Run it.
- "Let me start the server and check the code" → start the server and hit the endpoint.
- "I don't have a browser" → check for MCP browser tools first; use the fallback.
- "This would take too long" → not your call.

=== ADVERSARIAL PROBES (adapt to change type) ===
- **Concurrency**: parallel requests to create-if-not-exists paths
- **Boundary values**: 0, -1, empty string, very long strings, unicode, MAX_INT
- **Idempotency**: same mutating request twice
- **Orphan operations**: delete/reference IDs that don't exist

=== BEFORE ISSUING PASS ===
Your report must include at least one adversarial probe and its result.

=== OUTPUT FORMAT (REQUIRED) ===
Every check MUST follow this structure:

```
### Check: [what you're verifying]
**Command run:**
  [exact command]
**Output observed:**
  [actual terminal output]
**Result: PASS** (or FAIL — Expected vs Actual)
```

End with exactly this line (parsed by caller):

VERDICT: PASS / VERDICT: FAIL / VERDICT: PARTIAL

Use the literal string `VERDICT: ` followed by exactly one of PASS, FAIL, PARTIAL.
- **FAIL**: include what failed, exact error output, reproduction steps.
- **PARTIAL**: environmental limitations only.)",
        kBash, kWebFetch
    );
}

inline constexpr std::string_view kVerificationWhen =
    "Use this agent to verify that implementation work is correct before reporting completion. Invoke after non-trivial tasks (3+ file edits, backend/API changes, infrastructure changes). Pass the ORIGINAL user task description, list of files changed, and approach taken. The agent runs builds, tests, linters, and checks to produce a PASS/FAIL/PARTIAL verdict with evidence.";

inline constexpr std::string_view kVerificationReminder =
    "CRITICAL: This is a VERIFICATION-ONLY task. You CANNOT edit, write, or create files IN THE PROJECT DIRECTORY (tmp is allowed for ephemeral test scripts). You MUST end with VERDICT: PASS, VERDICT: FAIL, or VERDICT: PARTIAL.";

// ---- claude-code-guide ----
inline constexpr std::string_view kCcdocsMap = "https://code.claude.com/docs/en/claude_code_docs_map.md";
inline constexpr std::string_view kCdpDocsMap = "https://platform.claude.com/llms.txt";

[[nodiscard]] inline std::string guide_base_prompt() {
    const auto local = kEmbeddedSearch
        ? std::format("{}, `find`, and `grep`", kRead)
        : std::format("{}, {}, and {}", kRead, kGlob, kGrep);
    return std::format(
        R"(You are the Claude guide agent. Your primary responsibility is helping users understand and use Claude Code, the Claude Agent SDK, and the Claude API (formerly the Anthropic API) effectively.

**Your expertise spans three domains:**

1. **Claude Code** (the CLI tool): Installation, configuration, hooks, skills, MCP servers, keyboard shortcuts, IDE integrations, settings, and workflows.
2. **Claude Agent SDK**: Framework for building custom AI agents. Node.js/TypeScript and Python.
3. **Claude API**: Direct model interaction, tool use, and integrations.

**Documentation sources:**

- **Claude Code docs** ({}): Install/setup, hooks, skills, MCP, IDE integrations, settings, shortcuts, subagents, plugins, sandboxing.
- **Claude Agent SDK docs** ({}): SDK overview, agent config + custom tools, session management, permissions, MCP integration, hosting, cost tracking.
- **Claude API docs** ({}): Messages API + streaming, tool use (computer use, code execution, web search, bash, programmatic tool calling, tool search, context editing, Files API, structured outputs), vision, PDF, citations, extended thinking, MCP connector, cloud providers (Bedrock, Vertex, Foundry).

**Approach:**
1. Determine domain
2. Use {} to fetch the docs map
3. Identify relevant URLs
4. Fetch specific pages
5. Provide clear, actionable guidance
6. Use {} if docs don't cover the topic
7. Reference local project files (CLAUDE.md, .claude/) using {}

**Guidelines:**
- Prioritize official documentation
- Keep responses concise and actionable
- Include specific examples / code snippets when helpful
- Reference exact URLs
- Proactively suggest related commands, shortcuts, capabilities

Complete the user's request with accurate, documentation-based guidance.)",
        kCcdocsMap, kCdpDocsMap, kCdpDocsMap,
        kWebFetch, kWebSearch, local
    );
}

inline constexpr std::string_view kGuideWhen =
    "Use this agent when the user asks questions (\"Can Claude...\", \"Does Claude...\", \"How do I...\") about: (1) Claude Code CLI tool - features, hooks, slash commands, MCP servers, settings, IDE integrations, keyboard shortcuts; (2) Claude Agent SDK - building custom agents; (3) Claude API - API usage, tool use, SDK usage. **IMPORTANT:** Before spawning a new agent, check if there is already a running or recently completed claude-code-guide agent that you can continue via SendMessage.";

} // namespace builtin_detail

// Feature flag gate for Explore + Plan (TS: areExplorePlanAgentsEnabled).
// 3P (open-source / Bedrock / Vertex) default: on. Ant-native default: off
// (opt-in via GrowthBook tengu_amber_stoat — ported as explicit env override).
[[nodiscard]] inline bool are_explore_plan_agents_enabled() {
#if defined(ANT_NATIVE_BUILD)
    return env_truthy("CLAUDE_CODE_ENABLE_EXPLORE_PLAN_AGENTS") ||
           env_truthy("BUILTIN_EXPLORE_PLAN_AGENTS");
#else
    const char* disable = std::getenv("CLAUDE_CODE_DISABLE_EXPLORE_PLAN_AGENTS");
    if (disable && std::string_view(disable) == "1") return false;
    return true;
#endif
}

[[nodiscard]] inline bool is_verification_agent_enabled() {
    return env_truthy("CLAUDE_CODE_ENABLE_VERIFICATION_AGENT") ||
           env_truthy("VERIFICATION_AGENT");
}

[[nodiscard]] inline std::vector<AgentDefinition> built_in_agent_definitions() {
    using namespace builtin_detail;

    if (env_truthy("CLAUDE_AGENT_SDK_DISABLE_BUILTIN_AGENTS") && is_sdk_entrypoint()) return {};

    std::vector<AgentDefinition> agents;
    agents.reserve(6);

    // --- general-purpose ---
    agents.push_back(AgentDefinition{
        .agent_type = "general-purpose",
        .when_to_use = std::string{kGpWhen},
        .model = "",  // uses default subagent model
        .source = "built-in",
        .filename = std::nullopt,
        .path = std::nullopt,
        .system_prompt = gp_prompt(),
        .tools = {"*"},
        .disallowed_tools = {},
        .permission_mode = std::nullopt,
        .max_turns = std::nullopt,
        .initial_prompt = std::nullopt,
        .background = false,
        .isolation = std::nullopt,
        .required_mcp_servers = {},
        .mcp_servers = {},
        .inline_mcp_servers = {},
        .skills = {},
        .hooks_present = false,
        .effort = std::nullopt,
        .memory = std::nullopt,
        .color = std::nullopt,
        .omit_claude_md = false,
        .critical_system_reminder = std::nullopt,
    });

    // --- statusline-setup ---
    agents.push_back(AgentDefinition{
        .agent_type = "statusline-setup",
        .when_to_use = std::string{kStatuslineWhen},
        .model = "sonnet",
        .source = "built-in",
        .filename = std::nullopt,
        .path = std::nullopt,
        .system_prompt = std::string{kStatuslinePrompt},
        .tools = {"Read", "Edit"},
        .disallowed_tools = {},
        .permission_mode = std::nullopt,
        .max_turns = std::nullopt,
        .initial_prompt = std::nullopt,
        .background = false,
        .isolation = std::nullopt,
        .required_mcp_servers = {},
        .mcp_servers = {},
        .inline_mcp_servers = {},
        .skills = {},
        .hooks_present = false,
        .effort = std::nullopt,
        .memory = std::nullopt,
        .color = "orange",
        .omit_claude_md = false,
        .critical_system_reminder = std::nullopt,
    });

    // --- Explore + Plan (feature-gated) ---
    if (are_explore_plan_agents_enabled()) {
        agents.push_back(AgentDefinition{
            .agent_type = "Explore",
            .when_to_use = std::string{kExploreWhen},
            .model = is_ant() ? "inherit" : "haiku",
            .source = "built-in",
            .filename = std::nullopt,
            .path = std::nullopt,
            .system_prompt = explore_prompt(),
            .tools = {"Read", "Glob", "Grep"},
            .disallowed_tools = {
                std::string{kAgent},
                std::string{kExitPlanMode},
                std::string{kEdit},
                std::string{kWrite},
                std::string{kNotebookEdit},
            },
            .permission_mode = std::nullopt,
            .max_turns = 15,
            .initial_prompt = std::nullopt,
            .background = false,
            .isolation = std::nullopt,
            .required_mcp_servers = {},
            .mcp_servers = {},
            .inline_mcp_servers = {},
            .skills = {},
            .hooks_present = false,
            .effort = std::nullopt,
            .memory = std::nullopt,
            .color = std::nullopt,
            .omit_claude_md = true,
            .critical_system_reminder = std::nullopt,
        });
        agents.push_back(AgentDefinition{
            .agent_type = "Plan",
            .when_to_use = std::string{kPlanWhen},
            .model = "inherit",
            .source = "built-in",
            .filename = std::nullopt,
            .path = std::nullopt,
            .system_prompt = plan_prompt(),
            .tools = {"Read", "Glob", "Grep"},
            .disallowed_tools = {
                std::string{kAgent},
                std::string{kExitPlanMode},
                std::string{kEdit},
                std::string{kWrite},
                std::string{kNotebookEdit},
            },
            .permission_mode = std::nullopt,
            .max_turns = 10,
            .initial_prompt = std::nullopt,
            .background = false,
            .isolation = std::nullopt,
            .required_mcp_servers = {},
            .mcp_servers = {},
            .inline_mcp_servers = {},
            .skills = {},
            .hooks_present = false,
            .effort = std::nullopt,
            .memory = std::nullopt,
            .color = std::nullopt,
            .omit_claude_md = true,
            .critical_system_reminder = std::nullopt,
        });
    }

    // --- claude-code-guide (suppressed for SDK entrypoints) ---
    if (!is_sdk_entrypoint()) {
        const std::vector<std::string> guide_tools = kEmbeddedSearch
            ? std::vector<std::string>{
                  std::string{kBash}, std::string{kRead},
                  std::string{kWebFetch}, std::string{kWebSearch}}
            : std::vector<std::string>{
                  std::string{kGlob}, std::string{kGrep}, std::string{kRead},
                  std::string{kWebFetch}, std::string{kWebSearch}};
        const std::string feedback =
            "- When you cannot find an answer or the feature doesn't exist, direct the user to use /feedback to report a feature request or bug";
        const std::string system_prompt = guide_base_prompt() + "\n" + feedback;
        agents.push_back(AgentDefinition{
            .agent_type = "claude-code-guide",
            .when_to_use = std::string{kGuideWhen},
            .model = "haiku",
            .source = "built-in",
            .filename = std::nullopt,
            .path = std::nullopt,
            .system_prompt = system_prompt,
            .tools = guide_tools,
            .disallowed_tools = {},
            .permission_mode = "dontAsk",
            .max_turns = std::nullopt,
            .initial_prompt = std::nullopt,
            .background = false,
            .isolation = std::nullopt,
            .required_mcp_servers = {},
            .mcp_servers = {},
            .inline_mcp_servers = {},
            .skills = {},
            .hooks_present = false,
            .effort = std::nullopt,
            .memory = std::nullopt,
            .color = std::nullopt,
            .omit_claude_md = false,
            .critical_system_reminder = std::nullopt,
        });
    }

    // --- verification (feature-gated, A/B default off) ---
    if (is_verification_agent_enabled()) {
        agents.push_back(AgentDefinition{
            .agent_type = "verification",
            .when_to_use = std::string{kVerificationWhen},
            .model = "inherit",
            .source = "built-in",
            .filename = std::nullopt,
            .path = std::nullopt,
            .system_prompt = verification_prompt(),
            .tools = {"Read", "Glob", "Grep", "Bash"},
            .disallowed_tools = {
                std::string{kAgent},
                std::string{kExitPlanMode},
                std::string{kEdit},
                std::string{kWrite},
                std::string{kNotebookEdit},
            },
            .permission_mode = std::nullopt,
            .max_turns = 20,
            .initial_prompt = std::nullopt,
            .background = true,
            .isolation = std::nullopt,
            .required_mcp_servers = {},
            .mcp_servers = {},
            .inline_mcp_servers = {},
            .skills = {},
            .hooks_present = false,
            .effort = std::nullopt,
            .memory = std::nullopt,
            .color = "red",
            .omit_claude_md = false,
            .critical_system_reminder = std::string{kVerificationReminder},
        });
    }

    return agents;
}

inline void append_existing_plugin_component_path(
    cc::utils::json::JsonVal value,
    const fs::path& plugin_dir,
    std::vector<fs::path>& out
) {
    auto append_one = [&](std::string_view raw) {
        if (raw.empty()) return;
        fs::path path{std::string(raw)};
        if (path.is_relative()) path = plugin_dir / path;
        std::error_code ec;
        if (fs::exists(path, ec)) out.push_back(std::move(path));
    };

    if (value.is_str()) {
        append_one(value.as_str());
    } else if (value.is_arr()) {
        value.iter([&](cc::utils::json::JsonVal item) {
            if (item.is_str()) append_one(item.as_str());
        });
    }
}

[[nodiscard]] inline std::optional<PluginComponentPaths> read_plugin_component_paths(
    const fs::path& plugin_dir
) {
    const auto manifest_path = plugin_dir / "plugin.json";
    std::ifstream input(manifest_path);
    if (!input) return std::nullopt;

    std::stringstream buffer;
    buffer << input.rdbuf();
    auto doc = cc::utils::json::parse(buffer.str());
    if (!doc) return std::nullopt;

    auto root = doc->root();
    auto name = root.get("name");
    if (!root.is_obj() || !name.is_str() || name.as_str().empty()) return std::nullopt;

    PluginComponentPaths paths{
        .plugin_name = std::string(name.as_str()),
        .plugin_dir = plugin_dir,
        .agents_paths = {},
        .skills_paths = {},
    };

    const auto default_agents = plugin_dir / "agents";
    std::error_code ec;
    if (fs::exists(default_agents, ec)) paths.agents_paths.push_back(default_agents);
    if (auto agents = root.get("agents"); agents.valid()) {
        append_existing_plugin_component_path(agents, plugin_dir, paths.agents_paths);
    }

    const auto default_skills = plugin_dir / "skills";
    if (fs::exists(default_skills, ec)) paths.skills_paths.push_back(default_skills);
    if (auto skills = root.get("skills"); skills.valid()) {
        append_existing_plugin_component_path(skills, plugin_dir, paths.skills_paths);
    }

    if (paths.agents_paths.empty() && paths.skills_paths.empty()) return std::nullopt;
    return paths;
}

[[nodiscard]] inline std::vector<PluginComponentPaths> discover_plugin_component_paths() {
    std::vector<PluginComponentPaths> discovered;
    std::vector<fs::path> roots;
    if (const char* home = std::getenv("HOME")) {
        roots.push_back(fs::path{home} / ".claude" / "plugins");
    }
    roots.push_back(fs::current_path() / ".claude" / "plugins");

    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (ec) break;
            if (!entry.is_directory(ec)) continue;
            if (auto paths = read_plugin_component_paths(entry.path())) {
                discovered.push_back(std::move(*paths));
            }
        }
    }

    std::ranges::sort(discovered, {}, &PluginComponentPaths::plugin_name);
    return discovered;
}

[[nodiscard]] inline std::optional<AgentDefinition> parse_agent_markdown(
    const fs::path& path,
    std::string source
) {
    std::ifstream input(path);
    if (!input) return std::nullopt;

    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!text.starts_with("---\n") && !text.starts_with("---\r\n")) {
        return std::nullopt;
    }

    auto first_newline = text.find('\n');
    if (first_newline == std::string::npos) return std::nullopt;
    auto frontmatter_end = text.find("\n---", first_newline + 1);
    if (frontmatter_end == std::string::npos) return std::nullopt;

    const auto frontmatter_text = std::string_view(text).substr(
        first_newline + 1,
        frontmatter_end - first_newline - 1
    );
    auto parsed_frontmatter = cc::utils::parse_yaml(frontmatter_text);
    const auto* fields = std::get_if<cc::utils::YamlMap>(&parsed_frontmatter.data);
    if (!fields) return std::nullopt;

    auto name = yaml_string_field(*fields, "name");
    auto description = yaml_string_field(*fields, "description");

    // migrated edge case: `name` must be a non-empty string (not just truthy),
    // since `name: 0` or `name: false` would otherwise round-trip as strings
    // "0"/"false" and silently register an agent nobody can reference.
    if (!name || name->empty()) return std::nullopt;
    // migrated edge case: TS silently skips when description is missing OR not a string.
    // We also need to differentiate "co-located reference markdown without name"
    // (skip silently) vs. "agent file with `name:` but no `description:`" (log error).
    // This check is done by the caller (load_agent_definitions_from_dir) below via
    // the get_parse_error() fallback — the result stays `nullopt` and the caller
    // decides whether to emit a diagnostic. Here we return nullopt regardless.
    if (!description || description->empty()) return std::nullopt;

    // migrated edge case: TS silently unescapes `\\n` sequences inside
    // description strings that were YAML-escaped during parse.
    {
        std::string unescaped;
        unescaped.reserve(description->size());
        for (std::size_t i = 0; i < description->size(); ++i) {
            if ((*description)[i] == '\\' && i + 1 < description->size() &&
                (*description)[i + 1] == 'n') {
                unescaped.push_back('\n');
                ++i;
            } else {
                unescaped.push_back((*description)[i]);
            }
        }
        *description = std::move(unescaped);
    }

    auto body_start = text.find('\n', frontmatter_end + 4);
    std::string body = body_start == std::string::npos ? "" : trim(std::string_view(text).substr(body_start + 1));

    AgentDefinition definition;
    definition.agent_type = *name;
    definition.when_to_use = *description;
    // migrated edge case: model field — TS silently lowercases and treats the
    // value "inherit" case-insensitively as the string "inherit". We keep the
    // raw value otherwise so custom model aliases survive the round-trip.
    auto model_raw = yaml_string_field(*fields, "model");
    if (model_raw && !model_raw->empty()) {
        auto trimmed = trim(*model_raw);
        auto lowered = trimmed;
        std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        definition.model = lowered == "inherit" ? std::string{"inherit"} : trimmed;
    } else {
        definition.model = "inherit";
    }
    // migrated edge case: TS validates `background` against strict set of
    // string/bool values; junk values are rejected (instead of defaulting to
    // false) via parse_bool_field fallback semantics. parse_bool_field treats
    // anything non-boolean as `fallback` (false), matching the TS behaviour of
    // only allowing "true"/"false" as valid string forms.
    definition.source = std::move(source);
    definition.filename = path.stem().string();
    definition.path = path.string();
    definition.system_prompt = std::move(body);
    definition.tools = yaml_string_list_field(*fields, "tools");
    definition.disallowed_tools = yaml_string_list_field(*fields, "disallowedTools");
    if (auto permission_mode = yaml_string_field(*fields, "permissionMode"); permission_mode && !permission_mode->empty()) {
        definition.permission_mode = std::move(*permission_mode);
    }
    if (const auto* effort_value = yaml_field(*fields, "effort")) {
        if (auto effort = yaml_scalar_to_string(*effort_value); effort && valid_agent_effort(*effort)) {
            definition.effort = std::move(*effort);
        }
    }
    if (auto memory = yaml_string_field(*fields, "memory"); memory && valid_agent_memory_scope(*memory)) {
        definition.memory = std::move(*memory);
    }
    if (auto color = yaml_string_field(*fields, "color"); color && valid_agent_color(*color)) {
        definition.color = std::move(*color);
    }
    if (auto omit = yaml_string_field(*fields, "omitClaudeMd")) {
        definition.omit_claude_md = parse_bool_field(*omit);
    }
    auto critical = yaml_string_field(*fields, "criticalSystemReminder_EXPERIMENTAL")
        .or_else([&] { return yaml_string_field(*fields, "criticalSystemReminder"); });
    if (critical && !critical->empty()) {
        definition.critical_system_reminder = std::move(*critical);
    }
    if (auto max_turns = yaml_string_field(*fields, "maxTurns")) {
        definition.max_turns = parse_positive_int(*max_turns);
    }
    if (auto initial = yaml_string_field(*fields, "initialPrompt"); initial && !initial->empty()) {
        definition.initial_prompt = std::move(*initial);
    }
    if (auto background = yaml_string_field(*fields, "background")) {
        definition.background = parse_bool_field(*background);
    }
    if (auto isolation = yaml_string_field(*fields, "isolation");
        isolation && !isolation->empty() && valid_agent_isolation(*isolation)) {
        definition.isolation = std::move(*isolation);
    }
    definition.required_mcp_servers = yaml_string_list_field(*fields, "requiredMcpServers");
    if (const auto* mcp = yaml_field(*fields, "mcpServers")) {
        auto parsed_mcp = parse_agent_mcp_servers(*mcp);
        definition.mcp_servers = std::move(parsed_mcp.references);
        definition.inline_mcp_servers = std::move(parsed_mcp.inline_configs);
    }
    definition.skills = yaml_string_list_field(*fields, "skills");
    if (const auto* hooks = yaml_field(*fields, "hooks")) {
        definition.hooks = parse_agent_hooks(*hooks);
        definition.hooks_present = true;
    }
    return definition;
}

[[nodiscard]] inline std::optional<int> json_positive_int_field(
    cc::utils::json::JsonVal object,
    std::string_view key
) {
    if (!object.valid() || !object.is_obj()) return std::nullopt;
    auto value = object.get(key);
    if (value.is_num()) {
        auto parsed = static_cast<int>(value.as_int());
        return parsed > 0 ? std::optional<int>{parsed} : std::nullopt;
    }
    if (auto scalar = json_scalar_to_string(value)) {
        return parse_positive_int(*scalar);
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<bool> json_bool_field(
    cc::utils::json::JsonVal object,
    std::string_view key
) {
    if (!object.valid() || !object.is_obj()) return std::nullopt;
    auto value = object.get(key);
    if (!value.valid()) return std::nullopt;
    if (value.is_bool()) return value.as_bool();
    if (auto scalar = json_scalar_to_string(value)) return parse_bool_field(*scalar);
    return std::nullopt;
}

[[nodiscard]] inline std::optional<AgentHookCommand> parse_agent_hook_command(cc::utils::json::JsonVal value) {
    AgentHookCommand command;
    if (auto scalar = json_scalar_to_string(value)) {
        if (scalar->empty()) return std::nullopt;
        command.command = std::move(*scalar);
        return command;
    }

    if (!value.valid() || !value.is_obj()) return std::nullopt;

    auto type = json_string_field(value, "type").value_or("command");
    if (!type.empty() && canonicalize_agent_type(type) != "command") return std::nullopt;
    auto raw_command = json_string_field(value, "command").or_else([&] {
        return json_string_field(value, "cmd");
    });
    if (!raw_command || raw_command->empty()) return std::nullopt;
    command.command = std::move(*raw_command);
    if (auto shell = json_string_field(value, "shell"); shell && !shell->empty()) {
        command.shell = std::move(*shell);
    }
    command.timeout_seconds = json_positive_int_field(value, "timeout").or_else([&] {
        return json_positive_int_field(value, "timeoutSeconds");
    });
    if (auto condition = json_string_field(value, "if"); condition && !condition->empty()) {
        command.condition = std::move(*condition);
    }
    return command;
}

[[nodiscard]] inline std::vector<AgentHookCommand> parse_agent_hook_commands(cc::utils::json::JsonVal value) {
    std::vector<AgentHookCommand> commands;
    if (!value.valid()) return commands;

    if (value.is_arr()) {
        value.iter([&](cc::utils::json::JsonVal item) {
            if (auto command = parse_agent_hook_command(item)) commands.push_back(std::move(*command));
        });
        return commands;
    }

    if (value.is_obj()) {
        if (auto hooks = value.get("hooks"); hooks.valid()) {
            return parse_agent_hook_commands(hooks);
        }
    }

    if (auto command = parse_agent_hook_command(value)) commands.push_back(std::move(*command));
    return commands;
}

[[nodiscard]] inline std::optional<AgentHookMatcher> parse_agent_hook_matcher(cc::utils::json::JsonVal value) {
    AgentHookMatcher matcher;
    if (value.valid() && value.is_obj()) {
        if (auto match = json_string_field(value, "matcher"); match && !match->empty()) {
            matcher.matcher = std::move(*match);
        }
        if (auto hooks = value.get("hooks"); hooks.valid()) {
            matcher.hooks = parse_agent_hook_commands(hooks);
        } else if (auto command = parse_agent_hook_command(value)) {
            matcher.hooks.push_back(std::move(*command));
        }
    } else if (auto command = parse_agent_hook_command(value)) {
        matcher.hooks.push_back(std::move(*command));
    }

    if (matcher.hooks.empty()) return std::nullopt;
    return matcher;
}

[[nodiscard]] inline std::vector<AgentHookMatcher> parse_agent_hook_matchers(cc::utils::json::JsonVal value) {
    std::vector<AgentHookMatcher> matchers;
    if (!value.valid()) return matchers;

    if (value.is_arr()) {
        value.iter([&](cc::utils::json::JsonVal item) {
            if (auto matcher = parse_agent_hook_matcher(item)) matchers.push_back(std::move(*matcher));
        });
        return matchers;
    }

    if (value.is_obj()) {
        if (value.get("hooks").valid() || value.get("command").valid() || value.get("cmd").valid()) {
            if (auto matcher = parse_agent_hook_matcher(value)) matchers.push_back(std::move(*matcher));
            return matchers;
        }
        value.iter_obj([&](cc::utils::json::JsonVal matcher_name, cc::utils::json::JsonVal hooks) {
            if (!matcher_name.is_str()) return;
            auto commands = parse_agent_hook_commands(hooks);
            if (!commands.empty()) {
                matchers.push_back(AgentHookMatcher{
                    .matcher = std::string(matcher_name.as_str()),
                    .hooks = std::move(commands),
                });
            }
        });
        return matchers;
    }

    if (auto matcher = parse_agent_hook_matcher(value)) matchers.push_back(std::move(*matcher));
    return matchers;
}

[[nodiscard]] inline AgentHooksByEvent parse_agent_hooks(cc::utils::json::JsonVal value) {
    AgentHooksByEvent hooks;
    if (!value.valid()) return hooks;

    if (value.is_obj()) {
        value.iter_obj([&](cc::utils::json::JsonVal event, cc::utils::json::JsonVal event_hooks) {
            if (!event.is_str()) return;
            auto matchers = parse_agent_hook_matchers(event_hooks);
            if (!matchers.empty()) {
                auto& out = hooks[canonical_hook_event_name(event.as_str())];
                out.insert(out.end(), std::make_move_iterator(matchers.begin()), std::make_move_iterator(matchers.end()));
            }
        });
        return hooks;
    }

    if (value.is_arr()) {
        value.iter([&](cc::utils::json::JsonVal item) {
            if (auto event = json_scalar_to_string(item); event && !event->empty()) {
                hooks.try_emplace(canonical_hook_event_name(*event), std::vector<AgentHookMatcher>{});
            }
        });
    } else if (auto event = json_scalar_to_string(value); event && !event->empty()) {
        hooks.try_emplace(canonical_hook_event_name(*event), std::vector<AgentHookMatcher>{});
    }
    return hooks;
}

[[nodiscard]] inline std::optional<AgentDefinition> parse_agent_json_definition(
    std::string name,
    cc::utils::json::JsonVal object,
    const fs::path& path,
    std::string source
) {
    if (name.empty() || !object.valid() || !object.is_obj()) return std::nullopt;

    auto description = json_string_field(object, "description");
    auto prompt = json_string_field(object, "prompt")
        .or_else([&] { return json_string_field(object, "systemPrompt"); })
        .or_else([&] { return json_string_field(object, "system_prompt"); });
    if (!description || description->empty() || !prompt || prompt->empty()) return std::nullopt;

    AgentDefinition definition;
    definition.agent_type = std::move(name);
    definition.when_to_use = std::move(*description);
    definition.model = json_string_field(object, "model").value_or("inherit");
    definition.source = std::move(source);
    definition.filename = path.stem().string();
    definition.path = path.string();
    definition.system_prompt = trim(*prompt);
    definition.tools = json_string_list_field(object, "tools");
    definition.disallowed_tools = json_string_list_field(object, "disallowedTools");
    if (definition.disallowed_tools.empty()) {
        definition.disallowed_tools = json_string_list_field(object, "disallowed_tools");
    }
    if (auto permission_mode = json_string_field(object, "permissionMode")
            .or_else([&] { return json_string_field(object, "permission_mode"); });
        permission_mode && !permission_mode->empty()) {
        definition.permission_mode = std::move(*permission_mode);
    }
    if (auto effort = json_string_field(object, "effort"); effort && valid_agent_effort(*effort)) {
        definition.effort = std::move(*effort);
    }
    if (auto memory = json_string_field(object, "memory"); memory && valid_agent_memory_scope(*memory)) {
        definition.memory = std::move(*memory);
    }
    if (auto color = json_string_field(object, "color"); color && valid_agent_color(*color)) {
        definition.color = std::move(*color);
    }
    if (auto omit = json_bool_field(object, "omitClaudeMd")
            .or_else([&] { return json_bool_field(object, "omit_claude_md"); })) {
        definition.omit_claude_md = *omit;
    }
    auto critical = json_string_field(object, "criticalSystemReminder_EXPERIMENTAL")
        .or_else([&] { return json_string_field(object, "criticalSystemReminder"); })
        .or_else([&] { return json_string_field(object, "critical_system_reminder"); });
    if (critical && !critical->empty()) {
        definition.critical_system_reminder = std::move(*critical);
    }
    definition.max_turns = json_positive_int_field(object, "maxTurns")
        .or_else([&] { return json_positive_int_field(object, "max_turns"); });
    if (auto initial = json_string_field(object, "initialPrompt")
            .or_else([&] { return json_string_field(object, "initial_prompt"); });
        initial && !initial->empty()) {
        definition.initial_prompt = std::move(*initial);
    }
    if (auto background = json_bool_field(object, "background")) {
        definition.background = *background;
    }
    if (auto isolation = json_string_field(object, "isolation"); isolation && !isolation->empty()) {
        if (!valid_agent_isolation(*isolation)) return std::nullopt;
        definition.isolation = std::move(*isolation);
    }
    definition.required_mcp_servers = json_string_list_field(object, "requiredMcpServers");
    if (definition.required_mcp_servers.empty()) {
        definition.required_mcp_servers = json_string_list_field(object, "required_mcp_servers");
    }
    if (auto mcp = object.get("mcpServers"); mcp.valid()) {
        auto parsed_mcp = parse_agent_mcp_servers(mcp);
        definition.mcp_servers = std::move(parsed_mcp.references);
        definition.inline_mcp_servers = std::move(parsed_mcp.inline_configs);
    } else if (auto mcp_snake = object.get("mcp_servers"); mcp_snake.valid()) {
        auto parsed_mcp = parse_agent_mcp_servers(mcp_snake);
        definition.mcp_servers = std::move(parsed_mcp.references);
        definition.inline_mcp_servers = std::move(parsed_mcp.inline_configs);
    }
    definition.skills = json_string_list_field(object, "skills");
    if (auto hooks = object.get("hooks"); hooks.valid()) {
        definition.hooks = parse_agent_hooks(hooks);
        definition.hooks_present = true;
    }
    return definition;
}

[[nodiscard]] inline std::vector<AgentDefinition> parse_agents_json_file(
    const fs::path& path,
    std::string source
) {
    std::vector<AgentDefinition> agents;
    auto parsed = cc::utils::json::parse_file(path);
    if (!parsed || !parsed->root().is_obj()) return agents;

    parsed->root().iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal value) {
        if (!key.is_str()) return;
        if (auto agent = parse_agent_json_definition(std::string(key.as_str()), value, path, source)) {
            agents.push_back(std::move(*agent));
        }
    });
    return agents;
}

[[nodiscard]] inline std::vector<AgentDefinition> parse_agents_json_string(
    std::string_view json,
    std::string source,
    const fs::path& virtual_path = {}
) {
    std::vector<AgentDefinition> agents;
    auto parsed = cc::utils::json::parse(json);
    if (!parsed || !parsed->root().is_obj()) return agents;

    parsed->root().iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal value) {
        if (!key.is_str()) return;
        if (auto agent = parse_agent_json_definition(std::string(key.as_str()), value, virtual_path, source)) {
            agents.push_back(std::move(*agent));
        }
    });
    return agents;
}

[[nodiscard]] inline std::vector<AgentDefinition> load_agent_definitions_from_settings_file(
    const fs::path& path,
    std::string source
) {
    std::vector<AgentDefinition> agents;
    std::error_code ec;
    if (!fs::exists(path, ec) || !fs::is_regular_file(path, ec)) return agents;

    auto parsed = cc::utils::json::parse_file(path);
    if (!parsed || !parsed->root().is_obj()) return agents;
    auto node = parsed->root().get("agents");
    if (!node.valid() || !node.is_obj()) return agents;

    node.iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal value) {
        if (!key.is_str()) return;
        if (auto agent = parse_agent_json_definition(std::string(key.as_str()), value, path, source)) {
            agents.push_back(std::move(*agent));
        }
    });
    return agents;
}

[[nodiscard]] inline std::vector<AgentDefinition> load_flag_agent_definitions() {
    const char* json = std::getenv("CC_REPL_AGENTS_JSON");
    if (!json || !*json) json = std::getenv("CLAUDE_CODE_AGENTS_JSON");
    if (!json || !*json) return {};
    return parse_agents_json_string(json, "flagSettings", fs::path{"<flag-agents>"});
}

[[nodiscard]] inline std::vector<AgentDefinition> load_policy_agent_definitions() {
    const char* path = std::getenv("CLAUDE_CODE_POLICY_SETTINGS");
    if (!path || !*path) return {};
    return load_agent_definitions_from_settings_file(fs::path{path}, "policySettings");
}

[[nodiscard]] inline std::vector<AgentDefinition> load_agent_definitions_from_dir(
    const fs::path& dir,
    std::string source
) {
    std::vector<AgentDefinition> agents;
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return agents;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() == ".md") {
            if (auto parsed = parse_agent_markdown(entry.path(), source)) {
                agents.push_back(std::move(*parsed));
            }
        } else if (entry.path().extension() == ".json") {
            auto parsed = parse_agents_json_file(entry.path(), source);
            agents.insert(
                agents.end(),
                std::make_move_iterator(parsed.begin()),
                std::make_move_iterator(parsed.end()));
        }
    }
    std::ranges::sort(agents, {}, &AgentDefinition::agent_type);
    return agents;
}

[[nodiscard]] inline std::string qualify_plugin_component_name(
    std::string_view plugin_name,
    std::string value
) {
    if (value.empty() || value.starts_with("plugin:")) return value;
    return std::format("plugin:{}:{}", plugin_name, value);
}

inline void qualify_plugin_mcp_names(AgentDefinition& agent, std::string_view plugin_name) {
    for (auto& server : agent.required_mcp_servers) {
        server = qualify_plugin_component_name(plugin_name, std::move(server));
    }
    for (auto& server : agent.mcp_servers) {
        server = qualify_plugin_component_name(plugin_name, std::move(server));
    }
    for (auto& server : agent.inline_mcp_servers) {
        server.name = qualify_plugin_component_name(plugin_name, std::move(server.name));
    }
}

inline void append_plugin_agent_definition(
    std::vector<AgentDefinition>& agents,
    AgentDefinition agent,
    std::string_view plugin_name,
    const std::vector<std::string>& namespace_parts
) {
    std::string qualified = std::string(plugin_name);
    for (const auto& part : namespace_parts) {
        if (!part.empty()) qualified += ":" + part;
    }
    qualified += ":" + agent.agent_type;
    agent.agent_type = std::move(qualified);
    agent.source = "plugin";
    qualify_plugin_mcp_names(agent, plugin_name);
    agents.push_back(std::move(agent));
}

inline void load_plugin_agents_from_path(
    std::vector<AgentDefinition>& agents,
    const fs::path& path,
    std::string_view plugin_name,
    std::vector<std::string> namespace_parts = {}
) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return;

    if (fs::is_regular_file(path, ec)) {
        if (path.extension() == ".md") {
            if (auto parsed = parse_agent_markdown(path, "plugin")) {
                append_plugin_agent_definition(agents, std::move(*parsed), plugin_name, namespace_parts);
            }
        } else if (path.extension() == ".json") {
            for (auto& parsed : parse_agents_json_file(path, "plugin")) {
                append_plugin_agent_definition(agents, std::move(parsed), plugin_name, namespace_parts);
            }
        }
        return;
    }

    if (!fs::is_directory(path, ec)) return;
    for (const auto& entry : fs::directory_iterator(path, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec)) {
            if (entry.path().extension() == ".md") {
                if (auto parsed = parse_agent_markdown(entry.path(), "plugin")) {
                    append_plugin_agent_definition(agents, std::move(*parsed), plugin_name, namespace_parts);
                }
            } else if (entry.path().extension() == ".json") {
                for (auto& parsed : parse_agents_json_file(entry.path(), "plugin")) {
                    append_plugin_agent_definition(agents, std::move(parsed), plugin_name, namespace_parts);
                }
            }
        } else if (entry.is_directory(ec)) {
            auto nested = namespace_parts;
            nested.push_back(entry.path().filename().string());
            load_plugin_agents_from_path(agents, entry.path(), plugin_name, std::move(nested));
        }
    }
}

[[nodiscard]] inline std::vector<AgentDefinition> load_plugin_agent_definitions() {
    std::vector<AgentDefinition> agents;
    for (const auto& plugin : discover_plugin_component_paths()) {
        for (const auto& path : plugin.agents_paths) {
            load_plugin_agents_from_path(agents, path, plugin.plugin_name);
        }
    }
    std::ranges::sort(agents, {}, &AgentDefinition::agent_type);
    return agents;
}

[[nodiscard]] inline std::vector<AgentDefinition> get_all_agent_definitions(
    std::optional<fs::path> cwd = std::nullopt
) {
    std::map<std::string, AgentDefinition> by_type;
    auto active_agents = [&]() {
        std::vector<AgentDefinition> active;
        active.reserve(by_type.size());
        for (auto& [_, agent] : by_type) active.push_back(std::move(agent));
        return active;
    };

    for (auto& agent : built_in_agent_definitions()) {
        by_type[agent.agent_type] = std::move(agent);
    }
    if (env_truthy("CLAUDE_CODE_SIMPLE")) return active_agents();

    for (auto& agent : load_plugin_agent_definitions()) {
        by_type[agent.agent_type] = std::move(agent);
    }

    if (auto* home = std::getenv("HOME")) {
        const auto user_settings = fs::path(home) / ".claude" / "settings.json";
        for (auto& agent : load_agent_definitions_from_dir(
            fs::path(home) / ".claude" / "agents", "userSettings")) {
            by_type[agent.agent_type] = std::move(agent);
        }
        for (auto& agent : load_agent_definitions_from_settings_file(user_settings, "userSettings")) {
            by_type[agent.agent_type] = std::move(agent);
        }
    }

    const auto project_cwd = cwd.value_or(fs::current_path());
    for (auto& agent : load_agent_definitions_from_dir(
        project_cwd / ".claude" / "agents", "projectSettings")) {
        by_type[agent.agent_type] = std::move(agent);
    }
    for (auto& agent : load_agent_definitions_from_settings_file(
        project_cwd / ".claude" / "settings.json", "projectSettings")) {
        by_type[agent.agent_type] = std::move(agent);
    }
    for (auto& agent : load_agent_definitions_from_settings_file(
        project_cwd / ".claude" / "settings.local.json", "localSettings")) {
        by_type[agent.agent_type] = std::move(agent);
    }
    for (auto& agent : load_flag_agent_definitions()) {
        by_type[agent.agent_type] = std::move(agent);
    }
    for (auto& agent : load_policy_agent_definitions()) {
        by_type[agent.agent_type] = std::move(agent);
    }

    return active_agents();
}

[[nodiscard]] inline std::optional<AgentDefinition> find_agent_definition(
    std::string_view requested_type,
    std::optional<fs::path> cwd = std::nullopt
) {
    auto agents = get_all_agent_definitions(std::move(cwd));
    auto resolved = resolve_requested_agent_type(requested_type, agents);
    if (!resolved) return std::nullopt;
    for (const auto& agent : agents) {
        if (agent.agent_type == *resolved) return agent;
    }
    return std::nullopt;
}

[[nodiscard]] inline bool has_teammate_identity() {
    auto agent_id = cc::utils::get_agent_id();
    auto agent_name = cc::utils::get_agent_name();
    auto team_name = cc::utils::get_team_name();
    return agent_id && !agent_id->empty() &&
        agent_name && !agent_name->empty() &&
        team_name && !team_name->empty();
}

inline void append_prompt_section(std::string& prompt, std::string_view section) {
    if (section.empty()) return;
    if (!prompt.empty()) prompt += "\n\n";
    prompt += section;
}

[[nodiscard]] inline std::optional<std::string> build_teammate_append_system_prompt(
    std::optional<std::string> existing_append_prompt = std::nullopt,
    std::optional<fs::path> cwd = std::nullopt
) {
    if (!has_teammate_identity()) return existing_append_prompt;

    std::string append_prompt = existing_append_prompt.value_or("");
    append_prompt_section(append_prompt, teammate_system_prompt_addendum);

    if (auto agent_type = cc::utils::get_agent_type(); agent_type && !agent_type->empty()) {
        auto agent = find_agent_definition(*agent_type, std::move(cwd));
        if (agent && agent->source != "built-in" && !agent->system_prompt.empty()) {
            append_prompt_section(
                append_prompt,
                std::format("# Custom Agent Instructions\n{}", agent->system_prompt));
        }
    }

    if (append_prompt.empty()) return std::nullopt;
    return append_prompt;
}

inline std::expected<AgentExecutionResult, std::string> run_agent(const AgentRuntimeConfig& config);

inline std::expected<std::string, std::string> fork_subagent(std::string_view parent_id, const AgentRuntimeConfig& config);

inline std::expected<AgentExecutionResult, std::string> resume_agent(std::string_view agent_id);

inline std::expected<std::vector<std::string>, std::string> load_agents_from_dir(std::string_view dir_path) {
    std::vector<std::string> names;
    for (const auto& agent : load_agent_definitions_from_dir(fs::path(dir_path), "custom")) {
        names.push_back(agent.agent_type);
    }
    return names;
}

inline AgentLifecycle get_agent_lifecycle(std::string_view agent_id);

[[nodiscard]] inline std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\': out += R"(\\)"; break;
            case '"': out += R"(\")"; break;
            case '\b': out += R"(\b)"; break;
            case '\f': out += R"(\f)"; break;
            case '\n': out += R"(\n)"; break;
            case '\r': out += R"(\r)"; break;
            case '\t': out += R"(\t)"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

[[nodiscard]] inline fs::path runtime_state_dir() {
    if (const char* env = std::getenv("CC_REPL_AGENT_RUNTIME_DIR"); env && *env) {
        return fs::path{env};
    }
    return fs::current_path() / ".claude" / "agent-runtime";
}

[[nodiscard]] inline std::string safe_agent_filename(std::string_view agent_id) {
    std::string out;
    out.reserve(agent_id.size());
    for (char ch : agent_id) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.') {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "agent" : out;
}

[[nodiscard]] inline fs::path agent_record_path(std::string_view agent_id) {
    return runtime_state_dir() / (safe_agent_filename(agent_id) + ".json");
}

[[nodiscard]] inline fs::path agent_transcript_path(std::string_view agent_id) {
    return runtime_state_dir() / (safe_agent_filename(agent_id) + ".transcript");
}

[[nodiscard]] inline fs::path agent_output_file_path(std::string_view agent_id) {
    return runtime_state_dir() / (safe_agent_filename(agent_id) + ".output");
}

[[nodiscard]] inline fs::path agent_sidechain_jsonl_path(std::string_view agent_id) {
    return runtime_state_dir() / (safe_agent_filename(agent_id) + ".sidechain.jsonl");
}

[[nodiscard]] inline std::optional<NativeAgentStatus> native_agent_status_from_string(std::string_view status) {
    if (status == "queued") return NativeAgentStatus::Queued;
    if (status == "running") return NativeAgentStatus::Running;
    if (status == "completed") return NativeAgentStatus::Completed;
    if (status == "failed") return NativeAgentStatus::Failed;
    if (status == "cancelled") return NativeAgentStatus::Cancelled;
    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string> native_agent_terminal_notification_status(NativeAgentStatus status) {
    switch (status) {
        case NativeAgentStatus::Completed: return "completed";
        case NativeAgentStatus::Failed: return "failed";
        case NativeAgentStatus::Cancelled: return "stopped";
        case NativeAgentStatus::Queued:
        case NativeAgentStatus::Running:
            return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string remote_json_string_field(
    cc::utils::json::JsonVal value,
    std::initializer_list<std::string_view> keys
) {
    if (!value.valid() || !value.is_obj()) return {};
    for (auto key : keys) {
        auto field = value.get(key);
        if (field.is_str() && !field.as_str().empty()) return std::string(field.as_str());
    }
    return {};
}

[[nodiscard]] inline std::string remote_text_from_content(cc::utils::json::JsonVal content) {
    if (content.is_str()) return std::string(content.as_str());
    if (!content.is_arr()) return {};

    std::string out;
    content.iter([&](cc::utils::json::JsonVal block) {
        if (block.is_str()) {
            if (!out.empty()) out += '\n';
            out += block.as_str();
            return;
        }
        if (!block.is_obj()) return;
        auto type = block.get("type");
        auto text = block.get("text");
        if (text.is_str() && (!type.is_str() || type.as_str() == "text")) {
            if (!out.empty()) out += '\n';
            out += text.as_str();
        }
    });
    return out;
}

[[nodiscard]] inline std::optional<std::string> remote_event_transcript_entry(std::string_view event) {
    auto parsed = cc::utils::json::parse(event);
    if (!parsed || !parsed->root().is_obj()) {
        auto text = trim(event);
        if (text.empty()) return std::nullopt;
        return "system: " + text;
    }

    auto root = parsed->root();
    const auto type = remote_json_string_field(root, {"type"});
    if (type == "assistant") {
        std::string text;
        auto message = root.get("message");
        if (message.is_obj()) text = remote_text_from_content(message.get("content"));
        if (text.empty()) text = remote_text_from_content(root.get("content"));
        if (text.empty()) text = remote_json_string_field(root, {"text", "stdout"});
        if (!text.empty()) return "assistant: " + text;
        return std::nullopt;
    }
    if (type == "user") {
        std::string text;
        auto message = root.get("message");
        if (message.is_obj()) text = remote_text_from_content(message.get("content"));
        if (text.empty()) text = remote_text_from_content(root.get("content"));
        if (!text.empty()) return "user: " + text;
        return std::nullopt;
    }
    if (type == "system") {
        auto text = remote_json_string_field(root, {"stdout", "content", "text", "message"});
        if (!text.empty()) return "system: " + text;
        return "system: " + cc::utils::json::to_string(root);
    }
    if (type == "result") {
        auto subtype = remote_json_string_field(root, {"subtype", "status"});
        auto text = remote_json_string_field(root, {"result", "output", "content", "error"});
        if (text.empty()) text = subtype.empty() ? "remote result" : "remote result: " + subtype;
        return "system: " + text;
    }

    auto text = remote_json_string_field(root, {"content", "text", "message", "stdout"});
    if (!text.empty()) return "system: " + text;
    return "system: " + cc::utils::json::to_string(root);
}

[[nodiscard]] inline std::optional<std::string> remote_result_event_output(std::string_view event) {
    auto parsed = cc::utils::json::parse(event);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto root = parsed->root();
    if (remote_json_string_field(root, {"type"}) != "result") return std::nullopt;
    auto text = remote_json_string_field(root, {"result", "output", "content", "error"});
    if (!text.empty()) return text;
    auto subtype = remote_json_string_field(root, {"subtype", "status"});
    if (!subtype.empty()) return "remote result: " + subtype;
    return "remote result";
}

[[nodiscard]] inline bool remote_result_event_failed(std::string_view event) {
    auto parsed = cc::utils::json::parse(event);
    if (!parsed || !parsed->root().is_obj()) return false;
    auto root = parsed->root();
    if (remote_json_string_field(root, {"type"}) != "result") return false;
    auto subtype = remote_json_string_field(root, {"subtype", "status"});
    return !subtype.empty() && subtype != "success" && subtype != "completed";
}

[[nodiscard]] inline std::string xml_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

[[nodiscard]] inline std::string native_agent_display_name(const NativeAgentRecord& record) {
    if (record.description && !record.description->empty()) return *record.description;
    if (record.name && !record.name->empty()) return *record.name;
    return record.agent_id;
}

[[nodiscard]] inline std::string native_agent_output_file(const NativeAgentRecord& record) {
    if (record.output_file_path && !record.output_file_path->empty()) return *record.output_file_path;
    if (record.transcript_path && !record.transcript_path->empty()) return *record.transcript_path;
    return agent_output_file_path(record.agent_id).string();
}

[[nodiscard]] inline std::optional<std::string> format_native_agent_task_notification(
    const NativeAgentRecord& record
) {
    auto status = native_agent_terminal_notification_status(record.status);
    if (!status) return std::nullopt;

    const auto description = native_agent_display_name(record);
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
        result_section = std::format("\n<result>{}</result>", xml_escape(*record.output));
    } else if (record.error && !record.error->empty()) {
        result_section = std::format("\n<result>{}</result>", xml_escape(*record.error));
    }
    std::string worktree_section;
    if (record.worktree_path && !record.worktree_path->empty()) {
        worktree_section += std::format("\n<worktree_path>{}</worktree_path>", xml_escape(*record.worktree_path));
    }
    if (record.worktree_branch && !record.worktree_branch->empty()) {
        worktree_section += std::format("\n<worktree_branch>{}</worktree_branch>", xml_escape(*record.worktree_branch));
    }
    std::string remote_section;
    if (record.isolation && *record.isolation == "remote") {
        remote_section += "\n<task_type>remote_agent</task_type>";
    }
    if (record.remote_task_id && !record.remote_task_id->empty()) {
        remote_section += std::format("\n<remote_task_id>{}</remote_task_id>", xml_escape(*record.remote_task_id));
    }
    if (record.remote_session_id && !record.remote_session_id->empty()) {
        remote_section += std::format("\n<session_id>{}</session_id>", xml_escape(*record.remote_session_id));
    }
    if (record.remote_session_url && !record.remote_session_url->empty()) {
        remote_section += std::format("\n<session_url>{}</session_url>", xml_escape(*record.remote_session_url));
    }

    return std::format(
        "<task_notification>\n"
        "<task_id>{}</task_id>\n"
        "<output_file>{}</output_file>\n"
        "<status>{}</status>\n"
        "<summary>{}</summary>{}{}{}\n"
        "</task_notification>",
        xml_escape(record.agent_id),
        xml_escape(native_agent_output_file(record)),
        xml_escape(*status),
        xml_escape(summary),
        result_section,
        worktree_section,
        remote_section);
}

inline void write_json_string_array(std::ostream& out, std::string_view name, const std::vector<std::string>& values) {
    out << R"(,")" << name << R"(":[)";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out << ',';
        out << '"' << json_escape(values[i]) << '"';
    }
    out << ']';
}

inline void write_json_optional_string(
    std::ostream& out,
    std::string_view name,
    const std::optional<std::string>& value
) {
    if (value) out << R"(,")" << name << R"(":")" << json_escape(*value) << '"';
}

inline void refresh_native_agent_output_symlink(
    const fs::path& output_path,
    const fs::path& transcript_path
) {
    if (output_path.empty() || output_path == transcript_path) return;

    std::error_code ec;
    fs::create_directories(output_path.parent_path(), ec);
    if (ec) return;

    fs::remove(output_path, ec);
    ec.clear();
    fs::create_symlink(transcript_path, output_path, ec);
    if (ec) {
        ec.clear();
        fs::copy_file(transcript_path, output_path, fs::copy_options::overwrite_existing, ec);
    }
}

[[nodiscard]] inline std::pair<std::string_view, std::string_view> split_transcript_role(
    std::string_view entry
) {
    const auto sep = entry.find(": ");
    if (sep == std::string_view::npos || sep == 0) return {"system", entry};
    auto role = entry.substr(0, sep);
    if (role != "user" && role != "assistant" && role != "system" && role != "tool" && role != "hook") {
        return {"system", entry};
    }
    return {role, entry.substr(sep + 2)};
}

[[nodiscard]] inline std::string sidechain_message_role(std::string_view role) {
    if (role == "user" || role == "assistant" || role == "system") return std::string(role);
    return "system";
}

[[nodiscard]] inline std::string fallback_sidechain_content_json(std::string_view text) {
    return std::format(R"([{{"type":"text","text":"{}"}}])", json_escape(text));
}

[[nodiscard]] inline std::string normalize_sidechain_content_json(
    std::string_view content_json,
    std::string_view fallback_text
) {
    auto content = trim(content_json);
    if (content.empty()) return fallback_sidechain_content_json(fallback_text);
    auto parsed = cc::utils::json::parse(content);
    if (!parsed) return fallback_sidechain_content_json(fallback_text);
    auto root = parsed->root();
    if (!root.valid() || (!root.is_arr() && !root.is_obj() && !root.is_str())) {
        return fallback_sidechain_content_json(fallback_text);
    }
    return std::string(content);
}

[[nodiscard]] inline std::string make_sidechain_jsonl_entry(
    std::string_view agent_id,
    std::size_t index,
    std::string_view role,
    std::string_view content_json,
    std::string_view fallback_text
) {
    const auto message_role = sidechain_message_role(role);
    const auto uuid = std::format("{}-{}", agent_id, index);
    const auto parent_uuid = index == 0 ? std::string{} : std::format("{}-{}", agent_id, index - 1);
    const auto normalized_content = normalize_sidechain_content_json(content_json, fallback_text);

    std::string entry;
    entry.reserve(normalized_content.size() + agent_id.size() * 3 + message_role.size() * 2 + 128);
    entry += R"({"type":")";
    entry += json_escape(message_role);
    entry += R"(","uuid":")";
    entry += json_escape(uuid);
    entry += R"(","parentUuid":)";
    if (index == 0) {
        entry += "null";
    } else {
        entry += '"';
        entry += json_escape(parent_uuid);
        entry += '"';
    }
    entry += R"(,"isSidechain":true,"agentId":")";
    entry += json_escape(agent_id);
    entry += R"(","message":{"role":")";
    entry += json_escape(message_role);
    entry += R"(","content":)";
    entry += normalized_content;
    entry += "}}";
    return entry;
}

[[nodiscard]] inline std::optional<std::string> rebase_sidechain_jsonl_entry_for_agent(
    std::string_view entry,
    std::string_view agent_id,
    std::size_t index
) {
    auto parsed = cc::utils::json::parse(entry);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;

    auto root = parsed->root();
    auto message = root.get("message");
    std::string role = remote_json_string_field(root, {"type"});
    cc::utils::json::JsonVal content = root.get("content");

    if (message.is_obj()) {
        auto message_role = remote_json_string_field(message, {"role"});
        if (!message_role.empty()) role = std::move(message_role);
        content = message.get("content");
    }
    if (role.empty()) role = "user";

    auto content_json = content.valid() ? content.to_string() : std::string{};
    if (content_json.empty()) {
        auto text = remote_json_string_field(root, {"raw", "content", "text"});
        content_json = fallback_sidechain_content_json(text);
    }

    return make_sidechain_jsonl_entry(
        agent_id,
        index,
        role,
        content_json,
        {});
}

[[nodiscard]] inline std::optional<std::string> rebase_content_replacement_entry_for_agent(
    std::string_view entry,
    std::string_view agent_id
) {
    auto parsed = cc::utils::json::parse(entry);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto root = parsed->root();
    if (remote_json_string_field(root, {"type"}) != "content-replacement") return std::nullopt;

    auto replacements = root.get("replacements");
    if (!replacements.is_arr()) return std::nullopt;

    std::string rebased;
    rebased.reserve(entry.size() + agent_id.size() + 64);
    rebased += R"({"type":"content-replacement")";
    auto session_id = remote_json_string_field(root, {"sessionId", "session_id"});
    if (!session_id.empty()) {
        rebased += R"(,"sessionId":")";
        rebased += json_escape(session_id);
        rebased += '"';
    }
    rebased += R"(,"agentId":")";
    rebased += json_escape(agent_id);
    rebased += R"(","replacements":)";
    rebased += replacements.to_string();
    rebased += '}';
    return rebased;
}

[[nodiscard]] inline std::vector<std::string> fork_sidechain_entries_for_child(
    const NativeAgentRecord& parent,
    std::string_view child_agent_id
) {
    std::vector<std::string> entries;
    entries.reserve(parent.sidechain_entries.empty()
        ? parent.transcript.size()
        : parent.sidechain_entries.size());

    if (!parent.sidechain_entries.empty()) {
        std::size_t message_index = 0;
        for (const auto& entry : parent.sidechain_entries) {
            if (entry.empty()) continue;
            if (auto rebased_replacement = rebase_content_replacement_entry_for_agent(entry, child_agent_id)) {
                entries.push_back(std::move(*rebased_replacement));
                continue;
            }
            if (auto rebased = rebase_sidechain_jsonl_entry_for_agent(
                    entry,
                    child_agent_id,
                    message_index)) {
                entries.push_back(std::move(*rebased));
                ++message_index;
            }
        }
        if (!entries.empty()) return entries;
    }

    std::size_t message_index = 0;
    for (const auto& transcript_entry : parent.transcript) {
        const auto [role, content] = split_transcript_role(transcript_entry);
        entries.push_back(make_sidechain_jsonl_entry(
            child_agent_id,
            message_index++,
            role,
            {},
            content));
    }
    return entries;
}

inline void collect_sidechain_tool_use_state(
    cc::utils::json::JsonVal content,
    std::vector<std::string>& tool_use_ids,
    std::unordered_set<std::string>& seen_tool_use_ids,
    std::unordered_set<std::string>& tool_result_ids
) {
    if (!content.valid()) return;

    if (content.is_arr()) {
        content.iter([&](cc::utils::json::JsonVal block) {
            collect_sidechain_tool_use_state(block, tool_use_ids, seen_tool_use_ids, tool_result_ids);
        });
        return;
    }

    if (!content.is_obj()) return;
    const auto type = remote_json_string_field(content, {"type"});
    if (type == "tool_use") {
        auto id = remote_json_string_field(content, {"id"});
        if (!id.empty() && !seen_tool_use_ids.contains(id)) {
            seen_tool_use_ids.insert(id);
            tool_use_ids.push_back(std::move(id));
        }
        return;
    }
    if (type == "tool_result") {
        auto id = remote_json_string_field(content, {"tool_use_id"});
        if (!id.empty()) tool_result_ids.insert(std::move(id));
        return;
    }

    collect_sidechain_tool_use_state(content.get("content"), tool_use_ids, seen_tool_use_ids, tool_result_ids);
}

[[nodiscard]] inline std::vector<std::string> unresolved_tool_use_ids_from_sidechain_entries(
    const std::vector<std::string>& entries
) {
    std::vector<std::string> tool_use_ids;
    std::unordered_set<std::string> seen_tool_use_ids;
    std::unordered_set<std::string> tool_result_ids;

    for (const auto& entry : entries) {
        auto parsed = cc::utils::json::parse(entry);
        if (!parsed || !parsed->root().is_obj()) continue;
        auto root = parsed->root();
        auto message = root.get("message");
        auto content = message.is_obj() ? message.get("content") : root.get("content");
        collect_sidechain_tool_use_state(content, tool_use_ids, seen_tool_use_ids, tool_result_ids);
    }

    std::vector<std::string> unresolved;
    for (const auto& id : tool_use_ids) {
        if (!tool_result_ids.contains(id)) unresolved.push_back(id);
    }
    return unresolved;
}

[[nodiscard]] inline std::string fork_missing_tool_results_content_json(
    const std::vector<std::string>& tool_use_ids,
    std::string_view directive_message
) {
    if (tool_use_ids.empty() && directive_message.empty()) return {};

    std::string content = "[";
    bool first = true;
    for (const auto& id : tool_use_ids) {
        if (!first) content += ',';
        first = false;
        content += R"({"type":"tool_result","tool_use_id":")";
        content += json_escape(id);
        content += R"(","content":[{"type":"text","text":"Fork started \u2014 processing in background"}]})";
    }
    if (!directive_message.empty()) {
        if (!first) content += ',';
        content += R"({"type":"text","text":")";
        content += json_escape(directive_message);
        content += R"("})";
    }
    content += ']';
    return content;
}

[[nodiscard]] inline bool native_agent_record_is_fork_child(const NativeAgentRecord& record) {
    if (record.agent_type == "fork") return true;

    for (const auto& capability : record.capabilities) {
        if (capability == "fork" || capability == "fork-subagent" || capability == "fork_child") {
            return true;
        }
    }

    const auto has_fork_marker = [](std::string_view text) {
        return text.find("<fork-boilerplate>") != std::string_view::npos ||
            text.find("system: forked from ") != std::string_view::npos;
    };
    for (const auto& line : record.transcript) {
        if (has_fork_marker(line)) return true;
    }
    for (const auto& entry : record.sidechain_entries) {
        if (has_fork_marker(entry)) return true;
    }
    return false;
}

[[nodiscard]] inline std::vector<std::string> fork_child_capabilities(
    const NativeAgentRecord& parent,
    const AgentRuntimeConfig& config
) {
    auto capabilities = config.capabilities.empty() ? parent.capabilities : config.capabilities;
    if (!std::ranges::contains(capabilities, "fork-subagent")) {
        capabilities.push_back("fork-subagent");
    }
    return capabilities;
}

inline bool write_sidechain_jsonl(
    const NativeAgentRecord& record,
    const fs::path& sidechain_path
) {
    std::error_code ec;
    fs::create_directories(sidechain_path.parent_path(), ec);
    if (ec) return false;

    std::ofstream sidechain(sidechain_path, std::ios::trunc);
    if (!sidechain) return false;
    if (!record.sidechain_entries.empty()) {
        for (const auto& entry : record.sidechain_entries) {
            if (entry.empty()) continue;
            sidechain << entry;
            if (!entry.ends_with('\n')) sidechain << '\n';
        }
        return sidechain.good();
    }
    for (std::size_t i = 0; i < record.transcript.size(); ++i) {
        const auto& entry = record.transcript[i];
        const auto [role, content] = split_transcript_role(entry);
        const auto message_type =
            (role == "user" || role == "assistant" || role == "system") ? role : std::string_view{"system"};
        const auto uuid = std::format("{}-{}", record.agent_id, i);
        const auto parent_uuid = i == 0 ? std::string{} : std::format("{}-{}", record.agent_id, i - 1);
        sidechain
            << R"({"type":")" << json_escape(message_type)
            << R"(","uuid":")" << json_escape(uuid)
            << R"(","parentUuid":)";
        if (i == 0) {
            sidechain << "null";
        } else {
            sidechain << '"' << json_escape(parent_uuid) << '"';
        }
        sidechain
            << R"(,"isSidechain":true)"
            << R"(,"agentId":")" << json_escape(record.agent_id)
            << R"(","message":{"role":")" << json_escape(message_type)
            << R"(","content":[{"type":"text","text":")" << json_escape(content)
            << R"("}]})"
            << R"(,"agent_id":")" << json_escape(record.agent_id)
            << R"(","parent_uuid":)";
        if (i == 0) {
            sidechain << "null";
        } else {
            sidechain << '"' << json_escape(parent_uuid) << '"';
        }
        sidechain
            << R"(,"role":")" << json_escape(message_type)
            << R"(","content":")" << json_escape(content)
            << R"(","raw":")" << json_escape(entry)
            << "\"}\n";
    }
    return sidechain.good();
}

inline bool persist_native_agent_record(const NativeAgentRecord& record) {
    std::error_code ec;
    const auto dir = runtime_state_dir();
    fs::create_directories(dir, ec);
    if (ec) return false;

    auto transcript_path = record.transcript_path
        ? fs::path{*record.transcript_path}
        : agent_transcript_path(record.agent_id);
    if (transcript_path.is_relative()) transcript_path = dir / transcript_path;
    fs::create_directories(transcript_path.parent_path(), ec);

    {
        std::ofstream transcript(transcript_path, std::ios::trunc);
        if (!transcript) return false;
        for (const auto& line : record.transcript) transcript << line << '\n';
    }

    auto sidechain_path = record.sidechain_jsonl_path
        ? fs::path{*record.sidechain_jsonl_path}
        : agent_sidechain_jsonl_path(record.agent_id);
    if (sidechain_path.is_relative()) sidechain_path = dir / sidechain_path;
    (void)write_sidechain_jsonl(record, sidechain_path);

    auto output_path = record.output_file_path
        ? fs::path{*record.output_file_path}
        : agent_output_file_path(record.agent_id);
    if (output_path.is_relative()) output_path = dir / output_path;
    refresh_native_agent_output_symlink(output_path, transcript_path);

    std::ofstream out(agent_record_path(record.agent_id), std::ios::trunc);
    if (!out) return false;
    out << R"({"agent_id":")" << json_escape(record.agent_id)
        << R"(","agent_type":")" << json_escape(record.agent_type)
        << R"(","background":)" << (record.background ? "true" : "false")
        << R"(,"status":")" << native_agent_status_name(record.status) << '"'
        << R"(,"cancel_requested":)" << (record.cancel_requested ? "true" : "false")
        << R"(,"notification_delivered":)" << (record.notification_delivered ? "true" : "false")
        << R"(,"worktree_cleanup_performed":)" << (record.worktree_cleanup_performed ? "true" : "false")
        << R"(,"remote_is_review":)" << (record.remote_is_review ? "true" : "false")
        << R"(,"remote_is_ultraplan":)" << (record.remote_is_ultraplan ? "true" : "false")
        << R"(,"remote_is_long_running":)" << (record.remote_is_long_running ? "true" : "false")
        << R"(,"remote_has_output":)" << (record.remote_has_output ? "true" : "false")
        << R"(,"remote_idle_polls":)" << record.remote_idle_polls
        << R"(,"transcript_path":")" << json_escape(transcript_path.string()) << '"'
        << R"(,"sidechain_jsonl_path":")" << json_escape(sidechain_path.string()) << '"'
        << R"(,"output_file_path":")" << json_escape(output_path.string()) << '"';
    write_json_optional_string(out, "parent_agent_id", record.parent_agent_id);
    write_json_optional_string(out, "description", record.description);
    write_json_optional_string(out, "name", record.name);
    write_json_optional_string(out, "team_name", record.team_name);
    write_json_optional_string(out, "cwd", record.cwd);
    write_json_optional_string(out, "isolation", record.isolation);
    write_json_optional_string(out, "mode", record.mode);
    write_json_optional_string(out, "output", record.output);
    write_json_optional_string(out, "error", record.error);
    write_json_optional_string(out, "worktree_path", record.worktree_path);
    write_json_optional_string(out, "worktree_branch", record.worktree_branch);
    write_json_optional_string(out, "worktree_base_commit", record.worktree_base_commit);
    write_json_optional_string(out, "worktree_git_root", record.worktree_git_root);
    write_json_optional_string(out, "teammate_backend", record.teammate_backend);
    write_json_optional_string(out, "teammate_task_id", record.teammate_task_id);
    write_json_optional_string(out, "teammate_pane_id", record.teammate_pane_id);
    write_json_optional_string(out, "teammate_color", record.teammate_color);
    write_json_optional_string(out, "parent_session_id", record.parent_session_id);
    write_json_optional_string(out, "remote_task_id", record.remote_task_id);
    write_json_optional_string(out, "remote_task_type", record.remote_task_type);
    write_json_optional_string(out, "remote_session_id", record.remote_session_id);
    write_json_optional_string(out, "remote_session_url", record.remote_session_url);
    write_json_optional_string(out, "remote_title", record.remote_title);
    write_json_optional_string(out, "remote_command", record.remote_command);
    write_json_optional_string(out, "remote_metadata_json", record.remote_metadata_json);
    write_json_optional_string(out, "remote_last_event_id", record.remote_last_event_id);
    if (record.progress) out << R"(,"progress":)" << *record.progress;
    write_json_string_array(out, "sidechain_entries", record.sidechain_entries);
    write_json_string_array(out, "pending_messages", record.pending_messages);
    write_json_string_array(out, "capabilities", record.capabilities);
    out << '}';
    return out.good();
}

[[nodiscard]] inline std::vector<std::string> read_transcript_lines(const fs::path& path) {
    std::vector<std::string> lines;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) lines.push_back(std::move(line));
    }
    return lines;
}

[[nodiscard]] inline std::string transcript_text_from_content(cc::utils::json::JsonVal content) {
    if (!content.valid()) return {};
    if (content.is_str()) return std::string(content.as_str());

    if (content.is_obj()) {
        if (auto text = content.get("text"); text.is_str()) return std::string(text.as_str());
        return transcript_text_from_content(content.get("content"));
    }

    if (!content.is_arr()) return {};

    std::string out;
    auto append = [&](std::string text) {
        text = trim(text);
        if (text.empty()) return;
        if (!out.empty()) out += '\n';
        out += std::move(text);
    };

    content.iter([&](cc::utils::json::JsonVal block) {
        if (block.is_str()) {
            append(std::string(block.as_str()));
            return;
        }
        if (!block.is_obj()) return;

        const auto type = remote_json_string_field(block, {"type"});
        if (auto text = block.get("text"); text.is_str()) {
            append(std::string(text.as_str()));
            return;
        }
        if (type == "tool_use") {
            auto name = remote_json_string_field(block, {"name"});
            append(name.empty() ? "[tool_use]" : "[tool_use:" + name + "]");
            return;
        }
        if (type == "tool_result") {
            auto nested = transcript_text_from_content(block.get("content"));
            append(nested.empty() ? "[tool_result]" : "tool_result: " + nested);
            return;
        }
        append(transcript_text_from_content(block.get("content")));
    });
    return out;
}

[[nodiscard]] inline std::optional<std::string> transcript_entry_from_ts_jsonl(cc::utils::json::JsonVal root) {
    if (!root.valid() || !root.is_obj()) return std::nullopt;

    auto type = remote_json_string_field(root, {"type"});
    if (type != "user" && type != "assistant" && type != "system") return std::nullopt;

    std::string role = type;
    cc::utils::json::JsonVal content = root.get("content");
    auto message = root.get("message");
    if (message.is_obj()) {
        auto message_role = remote_json_string_field(message, {"role"});
        if (!message_role.empty()) role = message_role;
        content = message.get("content");
    }

    auto text = transcript_text_from_content(content);
    if (text.empty()) return std::nullopt;
    return std::format("{}: {}", role, text);
}

[[nodiscard]] inline std::optional<std::string> transcript_entry_from_sidechain_jsonl_line(std::string_view line) {
    auto parsed = cc::utils::json::parse(line);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto root = parsed->root();
    auto raw = root.get("raw");
    if (raw.is_str() && !raw.as_str().empty()) {
        return std::string(raw.as_str());
    }
    auto role = root.get("role");
    auto content = root.get("content");
    if (role.is_str() && content.is_str()) {
        return std::format("{}: {}", role.as_str(), content.as_str());
    }
    return transcript_entry_from_ts_jsonl(root);
}

[[nodiscard]] inline std::vector<std::string> transcript_lines_from_sidechain_entries(
    const std::vector<std::string>& entries
) {
    std::vector<std::string> lines;
    lines.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.empty()) continue;
        if (auto transcript_entry = transcript_entry_from_sidechain_jsonl_line(entry)) {
            lines.push_back(std::move(*transcript_entry));
        }
    }
    return lines;
}

[[nodiscard]] inline std::vector<std::string> read_sidechain_transcript_lines(const fs::path& path) {
    std::vector<std::string> lines;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        if (auto transcript_entry = transcript_entry_from_sidechain_jsonl_line(line)) {
            lines.push_back(std::move(*transcript_entry));
        }
    }
    return lines;
}

[[nodiscard]] inline std::vector<std::string> read_sidechain_jsonl_entries(const fs::path& path) {
    std::vector<std::string> entries;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) entries.push_back(std::move(line));
    }
    return entries;
}

[[nodiscard]] inline std::vector<std::string> json_string_array(cc::utils::json::JsonVal value) {
    std::vector<std::string> out;
    if (!value.is_arr()) return out;
    value.iter([&](cc::utils::json::JsonVal item) {
        if (item.is_str()) out.push_back(std::string(item.as_str()));
    });
    return out;
}

[[nodiscard]] inline std::optional<NativeAgentRecord> load_native_agent_record_from_path(const fs::path& path) {
    auto parsed = cc::utils::json::parse_file(path);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto root = parsed->root();
    auto agent_id = root.get_string("agent_id");
    if (agent_id.empty()) return std::nullopt;

    auto status = native_agent_status_from_string(root.get_string("status"))
        .value_or(NativeAgentStatus::Queued);
    NativeAgentRecord record{
        .agent_id = std::move(agent_id),
        .agent_type = root.get_string("agent_type").empty() ? std::string("runtime") : root.get_string("agent_type"),
        .background = root.get("background").is_bool() && root.get("background").as_bool(),
        .status = status,
        .capabilities = json_string_array(root.get("capabilities")),
        .cancel_requested = root.get("cancel_requested").is_bool() && root.get("cancel_requested").as_bool(),
        .notification_delivered = root.get("notification_delivered").is_bool() &&
            root.get("notification_delivered").as_bool(),
        .worktree_cleanup_performed = root.get("worktree_cleanup_performed").is_bool() &&
            root.get("worktree_cleanup_performed").as_bool(),
        .remote_is_review = root.get("remote_is_review").is_bool() && root.get("remote_is_review").as_bool(),
        .remote_is_ultraplan = root.get("remote_is_ultraplan").is_bool() && root.get("remote_is_ultraplan").as_bool(),
        .remote_is_long_running = root.get("remote_is_long_running").is_bool() &&
            root.get("remote_is_long_running").as_bool(),
        .remote_has_output = root.get("remote_has_output").is_bool() && root.get("remote_has_output").as_bool(),
    };
    auto remote_idle_polls = root.get("remote_idle_polls");
    if (remote_idle_polls.is_num()) {
        record.remote_idle_polls = static_cast<std::size_t>(std::max<int64_t>(0, remote_idle_polls.as_int()));
    }

    auto assign_optional = [&](std::string_view key, std::optional<std::string>& field) {
        auto value = root.get(key);
        if (value.is_str()) field = std::string(value.as_str());
    };
    assign_optional("parent_agent_id", record.parent_agent_id);
    assign_optional("description", record.description);
    assign_optional("name", record.name);
    assign_optional("team_name", record.team_name);
    assign_optional("cwd", record.cwd);
    assign_optional("isolation", record.isolation);
    assign_optional("mode", record.mode);
    assign_optional("output", record.output);
    assign_optional("error", record.error);
    assign_optional("transcript_path", record.transcript_path);
    assign_optional("sidechain_jsonl_path", record.sidechain_jsonl_path);
    assign_optional("output_file_path", record.output_file_path);
    assign_optional("worktree_path", record.worktree_path);
    assign_optional("worktree_branch", record.worktree_branch);
    assign_optional("worktree_base_commit", record.worktree_base_commit);
    assign_optional("worktree_git_root", record.worktree_git_root);
    assign_optional("teammate_backend", record.teammate_backend);
    assign_optional("teammate_task_id", record.teammate_task_id);
    assign_optional("teammate_pane_id", record.teammate_pane_id);
    assign_optional("teammate_color", record.teammate_color);
    assign_optional("parent_session_id", record.parent_session_id);
    assign_optional("remote_task_id", record.remote_task_id);
    assign_optional("remote_task_type", record.remote_task_type);
    assign_optional("remote_session_id", record.remote_session_id);
    assign_optional("remote_session_url", record.remote_session_url);
    assign_optional("remote_title", record.remote_title);
    assign_optional("remote_command", record.remote_command);
    assign_optional("remote_metadata_json", record.remote_metadata_json);
    assign_optional("remote_last_event_id", record.remote_last_event_id);
    auto progress = root.get("progress");
    if (progress.is_num()) record.progress = progress.as_double();
    record.sidechain_entries = json_string_array(root.get("sidechain_entries"));
    record.pending_messages = json_string_array(root.get("pending_messages"));
    if (record.sidechain_entries.empty() && record.sidechain_jsonl_path) {
        record.sidechain_entries = read_sidechain_jsonl_entries(fs::path{*record.sidechain_jsonl_path});
    }
    if (record.transcript_path) {
        record.transcript = read_transcript_lines(fs::path{*record.transcript_path});
    }
    if (record.transcript.empty() && record.sidechain_jsonl_path) {
        record.transcript = read_sidechain_transcript_lines(fs::path{*record.sidechain_jsonl_path});
    }
    return record;
}

[[nodiscard]] inline std::optional<NativeAgentRecord> load_native_agent_record(std::string_view agent_id) {
    return load_native_agent_record_from_path(agent_record_path(agent_id));
}

[[nodiscard]] inline std::vector<NativeAgentRecord> load_all_native_agent_records() {
    std::vector<NativeAgentRecord> records;
    std::error_code ec;
    const auto dir = runtime_state_dir();
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return records;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") continue;
        if (auto record = load_native_agent_record_from_path(entry.path())) {
            records.push_back(std::move(*record));
        }
    }
    return records;
}

class NativeAgentStore {
public:
    void upsert(NativeAgentRecord record) {
        std::scoped_lock lock(mutex_);
        if (!record.transcript_path) {
            record.transcript_path = agent_transcript_path(record.agent_id).string();
        }
        if (!record.sidechain_jsonl_path) {
            record.sidechain_jsonl_path = agent_sidechain_jsonl_path(record.agent_id).string();
        }
        if (!record.output_file_path) {
            record.output_file_path = agent_output_file_path(record.agent_id).string();
        }
        record.updated_at = std::chrono::system_clock::now();
        auto agent_id = record.agent_id;
        auto stored = record;
        records_.insert_or_assign(std::move(agent_id), std::move(record));
        (void)persist_native_agent_record(stored);
    }

    [[nodiscard]] std::optional<NativeAgentRecord> get(std::string_view agent_id) const {
        std::scoped_lock lock(mutex_);
        auto it = records_.find(std::string(agent_id));
        if (it == records_.end()) {
            auto loaded = load_native_agent_record(agent_id);
            if (!loaded) return std::nullopt;
            auto loaded_agent_id = loaded->agent_id;
            auto [inserted, _] = records_.emplace(std::move(loaded_agent_id), std::move(*loaded));
            it = inserted;
        }
        return it->second;
    }

    [[nodiscard]] std::optional<NativeAgentRecord> get_by_task_id_or_remote_id(std::string_view id) const {
        if (auto direct = get(id)) return direct;

        std::scoped_lock lock(mutex_);
        for (auto record : load_all_native_agent_records()) {
            auto agent_id = record.agent_id;
            records_[std::move(agent_id)] = std::move(record);
        }
        for (const auto& [_, record] : records_) {
            if (record.remote_task_id && *record.remote_task_id == id) return record;
            if (record.remote_session_id && *record.remote_session_id == id) return record;
        }
        return std::nullopt;
    }

    [[nodiscard]] RemoteAgentPollApplication apply_remote_poll_result(
        std::string_view id,
        RemoteAgentPollResult poll
    ) {
        auto record = get_by_task_id_or_remote_id(id);
        if (!record) return {};

        RemoteAgentPollApplication applied;
        update(record->agent_id, [&](NativeAgentRecord& current) {
            if (current.status != NativeAgentStatus::Queued &&
                current.status != NativeAgentStatus::Running) {
                applied.terminal = true;
                applied.status = current.status;
                return;
            }

            current.status = NativeAgentStatus::Running;
            current.progress = current.progress.value_or(0.0);
            if (poll.last_event_id && !poll.last_event_id->empty()) {
                current.remote_last_event_id = std::move(poll.last_event_id);
            }

            std::optional<std::string> result_output;
            bool result_failed = poll.result_failed;
            for (const auto& event : poll.events) {
                if (auto entry = remote_event_transcript_entry(event)) {
                    current.transcript.push_back(std::move(*entry));
                    current.remote_has_output = true;
                    ++applied.events_appended;
                }
                if (auto output = remote_result_event_output(event)) {
                    result_output = std::move(*output);
                }
                result_failed = result_failed || remote_result_event_failed(event);
            }

            if (poll.completion_output && !poll.completion_output->empty()) {
                result_output = std::move(poll.completion_output);
            }

            if (result_output) {
                if (result_failed) {
                    current.status = NativeAgentStatus::Failed;
                    current.error = std::move(*result_output);
                    current.output = std::nullopt;
                } else {
                    current.status = NativeAgentStatus::Completed;
                    current.output = std::move(*result_output);
                    current.error = std::nullopt;
                    current.progress = 1.0;
                }
                current.remote_idle_polls = 0;
                current.notification_delivered = false;
                applied.terminal = true;
                applied.status = current.status;
                return;
            }

            const auto status = poll.session_status.value_or("");
            if (status == "archived") {
                current.status = NativeAgentStatus::Completed;
                current.output = current.output.value_or("Remote session archived");
                current.error = std::nullopt;
                current.progress = 1.0;
                current.remote_idle_polls = 0;
                current.notification_delivered = false;
                applied.terminal = true;
                applied.status = current.status;
                return;
            }

            if (status == "idle" && poll.events.empty() && current.remote_has_output &&
                !current.remote_is_ultraplan && !current.remote_is_long_running) {
                ++current.remote_idle_polls;
                if (current.remote_idle_polls >= 5) {
                    current.status = NativeAgentStatus::Completed;
                    current.output = current.output.value_or("Remote session became idle after producing output");
                    current.error = std::nullopt;
                    current.progress = 1.0;
                    current.notification_delivered = false;
                    applied.terminal = true;
                    applied.status = current.status;
                    return;
                }
            } else if (status == "running" || status == "requires_action" || !poll.events.empty()) {
                current.remote_idle_polls = 0;
            }

            applied.status = current.status;
        });
        return applied;
    }

    [[nodiscard]] std::expected<RemoteAgentPollApplication, std::string>
    poll_remote_agent_once(std::string_view id) {
        auto record = get_by_task_id_or_remote_id(id);
        if (!record) return std::unexpected(std::format("Remote task not found: {}", id));
        if (!record->remote_session_id || record->remote_session_id->empty()) {
            return std::unexpected(std::format("Task {} does not have a remote session id", record->agent_id));
        }

        auto config = cc::utils::teleport::default_remote_session_api_config();
        if (!config) return std::unexpected(config.error());

        auto events = cc::utils::teleport::poll_remote_session_events(
            *config,
            *record->remote_session_id,
            record->remote_last_event_id,
            false);
        if (!events) return std::unexpected(events.error());

        return apply_remote_poll_result(
            record->agent_id,
            RemoteAgentPollResult{
                .session_status = std::move(events->session_status),
                .events = std::move(events->new_events),
                .last_event_id = std::move(events->last_event_id),
                .completion_output = std::nullopt,
                .result_failed = false,
            });
    }

    [[nodiscard]] std::expected<void, std::string>
    archive_remote_agent_session(std::string_view id) {
        auto record = get_by_task_id_or_remote_id(id);
        if (!record) return std::unexpected(std::format("Remote task not found: {}", id));
        if (!record->remote_session_id || record->remote_session_id->empty()) {
            return std::unexpected(std::format("Task {} does not have a remote session id", record->agent_id));
        }

        auto config = cc::utils::teleport::default_remote_session_api_config();
        if (!config) return std::unexpected(config.error());
        return cc::utils::teleport::archive_remote_session(*config, *record->remote_session_id);
    }

    [[nodiscard]] std::vector<NativeAgentRecord> list() const {
        std::scoped_lock lock(mutex_);
        for (auto record : load_all_native_agent_records()) {
            auto agent_id = record.agent_id;
            records_[std::move(agent_id)] = std::move(record);
        }
        std::vector<NativeAgentRecord> out;
        out.reserve(records_.size());
        for (const auto& [_, record] : records_) out.push_back(record);
        std::ranges::sort(out, {}, &NativeAgentRecord::agent_id);
        return out;
    }

    void mark_running(std::string_view agent_id) {
        update(agent_id, [](NativeAgentRecord& record) {
            record.status = NativeAgentStatus::Running;
            record.progress = 0.0;
        });
    }

    void mark_completed(std::string_view agent_id, std::string output) {
        update(agent_id, [&](NativeAgentRecord& record) {
            if (!output.empty()) {
                const auto transcript_entry = "assistant: " + output;
                if (!std::ranges::contains(record.transcript, transcript_entry)) {
                    record.transcript.push_back(transcript_entry);
                }
            }
            record.status = NativeAgentStatus::Completed;
            record.output = std::move(output);
            record.error = std::nullopt;
            record.progress = 1.0;
            record.notification_delivered = false;
        });
    }

    void mark_failed(std::string_view agent_id, std::string error) {
        update(agent_id, [&](NativeAgentRecord& record) {
            if (!error.empty()) {
                const auto transcript_entry = "system: agent failed: " + error;
                if (!std::ranges::contains(record.transcript, transcript_entry)) {
                    record.transcript.push_back(transcript_entry);
                }
            }
            record.status = NativeAgentStatus::Failed;
            record.error = std::move(error);
            record.output = std::nullopt;
            record.notification_delivered = false;
        });
    }

    void mark_cancelled(std::string_view agent_id, std::string reason) {
        update(agent_id, [&](NativeAgentRecord& record) {
            if (!reason.empty()) {
                const auto transcript_entry = "system: agent cancelled: " + reason;
                if (!std::ranges::contains(record.transcript, transcript_entry)) {
                    record.transcript.push_back(transcript_entry);
                }
            }
            record.status = NativeAgentStatus::Cancelled;
            record.cancel_requested = true;
            record.error = std::move(reason);
            record.output = std::nullopt;
            record.notification_delivered = false;
        });
    }

    void request_cancel(std::string_view agent_id, std::string reason = "cancel requested") {
        mark_cancelled(agent_id, std::move(reason));
    }

    [[nodiscard]] bool is_cancel_requested(std::string_view agent_id) const {
        std::scoped_lock lock(mutex_);
        auto it = records_.find(std::string(agent_id));
        return it != records_.end() && it->second.cancel_requested;
    }

    [[nodiscard]] std::vector<std::string> take_pending_task_notifications() {
        std::scoped_lock lock(mutex_);
        for (auto record : load_all_native_agent_records()) {
            auto agent_id = record.agent_id;
            records_[std::move(agent_id)] = std::move(record);
        }

        std::vector<std::string> notifications;
        for (auto& [_, record] : records_) {
            if (!record.background || record.notification_delivered) continue;
            auto notification = format_native_agent_task_notification(record);
            if (!notification) continue;
            notifications.push_back(std::move(*notification));
            record.notification_delivered = true;
            (void)persist_native_agent_record(record);
        }
        std::ranges::sort(notifications);
        return notifications;
    }

    void clear_for_testing() {
        std::scoped_lock lock(mutex_);
        records_.clear();
    }

    void append_transcript(std::string_view agent_id, std::string entry) {
        update(agent_id, [&](NativeAgentRecord& record) {
            record.transcript.push_back(std::move(entry));
        });
    }

    void enqueue_pending_message(std::string_view agent_id, std::string message) {
        update(agent_id, [&](NativeAgentRecord& record) {
            record.pending_messages.push_back(std::move(message));
        });
    }

    void enqueue_resume_message(std::string_view agent_id, std::string message) {
        update(agent_id, [&](NativeAgentRecord& record) {
            const bool was_terminal =
                record.status == NativeAgentStatus::Completed ||
                record.status == NativeAgentStatus::Failed ||
                record.status == NativeAgentStatus::Cancelled;
            record.pending_messages.push_back(std::move(message));
            if (!was_terminal) return;

            record.status = NativeAgentStatus::Queued;
            record.output = std::nullopt;
            record.error = std::nullopt;
            record.progress = 0.0;
            record.cancel_requested = false;
            record.notification_delivered = false;
            record.transcript.push_back("system: resume requested from pending message");
        });
    }

    [[nodiscard]] std::vector<std::string> take_pending_messages(std::string_view agent_id) {
        std::vector<std::string> messages;
        update(agent_id, [&](NativeAgentRecord& record) {
            messages = std::move(record.pending_messages);
            record.pending_messages.clear();
        });
        return messages;
    }

    void append_sidechain_message(
        std::string_view agent_id,
        std::string_view role,
        std::string_view content_json,
        std::string_view fallback_text = {}
    ) {
        update(agent_id, [&](NativeAgentRecord& record) {
            record.sidechain_entries.push_back(make_sidechain_jsonl_entry(
                record.agent_id,
                record.sidechain_entries.size(),
                role,
                content_json,
                fallback_text));
        });
    }

    void append_sidechain_entry(std::string_view agent_id, std::string entry) {
        if (entry.empty()) return;
        update(agent_id, [&](NativeAgentRecord& record) {
            record.sidechain_entries.push_back(std::move(entry));
        });
    }

    void update_progress(std::string_view agent_id, double progress) {
        update(agent_id, [&](NativeAgentRecord& record) {
            record.progress = std::clamp(progress, 0.0, 1.0);
        });
    }

    void set_worktree_metadata(
        std::string_view agent_id,
        std::string path,
        std::string branch,
        std::string base_commit,
        std::string git_root
    ) {
        update(agent_id, [&](NativeAgentRecord& record) {
            record.worktree_path = std::move(path);
            record.worktree_branch = std::move(branch);
            record.worktree_base_commit = std::move(base_commit);
            record.worktree_git_root = std::move(git_root);
            record.worktree_cleanup_performed = false;
        });
    }

    void mark_worktree_cleaned(std::string_view agent_id) {
        update(agent_id, [](NativeAgentRecord& record) {
            if (record.worktree_path && record.cwd == record.worktree_path) {
                record.cwd = std::nullopt;
            }
            record.worktree_path = std::nullopt;
            record.worktree_branch = std::nullopt;
            record.worktree_base_commit = std::nullopt;
            record.worktree_git_root = std::nullopt;
            record.worktree_cleanup_performed = true;
        });
    }

    void set_teammate_metadata(
        std::string_view agent_id,
        std::string backend,
        std::optional<std::string> task_id = std::nullopt,
        std::optional<std::string> pane_id = std::nullopt,
        std::optional<std::string> color = std::nullopt,
        std::optional<std::string> parent_session_id = std::nullopt
    ) {
        update(agent_id, [&](NativeAgentRecord& record) {
            record.teammate_backend = std::move(backend);
            record.teammate_task_id = std::move(task_id);
            record.teammate_pane_id = std::move(pane_id);
            record.teammate_color = std::move(color);
            record.parent_session_id = std::move(parent_session_id);
        });
    }

    void set_remote_metadata(
        std::string_view agent_id,
        std::optional<std::string> task_id,
        std::optional<std::string> task_type,
        std::optional<std::string> session_id,
        std::optional<std::string> session_url,
        std::optional<std::string> title,
        std::optional<std::string> command,
        std::optional<std::string> metadata_json = std::nullopt,
        bool is_review = false,
        bool is_ultraplan = false,
        bool is_long_running = false
    ) {
        update(agent_id, [&](NativeAgentRecord& record) {
            record.remote_task_id = std::move(task_id);
            record.remote_task_type = std::move(task_type);
            record.remote_session_id = std::move(session_id);
            record.remote_session_url = std::move(session_url);
            record.remote_title = std::move(title);
            record.remote_command = std::move(command);
            record.remote_metadata_json = std::move(metadata_json);
            record.remote_is_review = is_review;
            record.remote_is_ultraplan = is_ultraplan;
            record.remote_is_long_running = is_long_running;
        });
    }

private:
    template <typename Fn>
    void update(std::string_view agent_id, Fn&& fn) {
        std::scoped_lock lock(mutex_);
        auto it = records_.find(std::string(agent_id));
        if (it == records_.end()) {
            if (auto loaded = load_native_agent_record(agent_id)) {
                auto loaded_agent_id = loaded->agent_id;
                auto [inserted, _] = records_.emplace(std::move(loaded_agent_id), std::move(*loaded));
                it = inserted;
            } else {
                return;
            }
        }
        fn(it->second);
        if (!it->second.transcript_path) {
            it->second.transcript_path = agent_transcript_path(it->second.agent_id).string();
        }
        if (!it->second.sidechain_jsonl_path) {
            it->second.sidechain_jsonl_path = agent_sidechain_jsonl_path(it->second.agent_id).string();
        }
        if (!it->second.output_file_path) {
            it->second.output_file_path = agent_output_file_path(it->second.agent_id).string();
        }
        it->second.updated_at = std::chrono::system_clock::now();
        (void)persist_native_agent_record(it->second);
    }

    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, NativeAgentRecord> records_;
};

inline NativeAgentStore& native_agent_store() {
    static NativeAgentStore store;
    return store;
}

[[nodiscard]] inline bool remote_agent_auto_poll_enabled() {
    if (const char* value = std::getenv("CC_REPL_REMOTE_AGENT_AUTO_POLL"); value && *value) {
        auto text = canonicalize_agent_type(value);
        return text != "0" && text != "false" && text != "no" && text != "off";
    }
    return true;
}

[[nodiscard]] inline int remote_agent_poll_interval_ms() {
    if (const char* value = std::getenv("CC_REPL_REMOTE_AGENT_POLL_INTERVAL_MS"); value && *value) {
        if (auto parsed = parse_positive_int(value)) {
            return std::clamp(*parsed, 10, 60'000);
        }
    }
    return 1'000;
}

[[nodiscard]] inline bool native_agent_status_terminal(NativeAgentStatus status) {
    return status == NativeAgentStatus::Completed ||
        status == NativeAgentStatus::Failed ||
        status == NativeAgentStatus::Cancelled;
}

[[nodiscard]] inline bool remote_poll_error_is_missing_local_config(std::string_view error) {
    return error.find("No Claude.ai OAuth access token found") != std::string_view::npos ||
        error.find("No organization UUID found") != std::string_view::npos;
}

[[nodiscard]] inline bool remote_agent_poll_should_continue(std::string_view agent_id) {
    auto record = native_agent_store().get(agent_id);
    if (!record) return false;
    if (record->cancel_requested) return false;
    return !native_agent_status_terminal(record->status);
}

inline std::mutex& remote_agent_pollers_mutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::unordered_set<std::string>& active_remote_agent_pollers() {
    static std::unordered_set<std::string> pollers;
    return pollers;
}

[[nodiscard]] inline bool reserve_remote_agent_poller(std::string_view agent_id) {
    std::scoped_lock lock(remote_agent_pollers_mutex());
    return active_remote_agent_pollers().insert(std::string(agent_id)).second;
}

inline void release_remote_agent_poller(std::string_view agent_id) {
    std::scoped_lock lock(remote_agent_pollers_mutex());
    active_remote_agent_pollers().erase(std::string(agent_id));
}

struct RemoteAgentPollerLease {
    std::string agent_id;

    explicit RemoteAgentPollerLease(std::string id) : agent_id(std::move(id)) {}
    RemoteAgentPollerLease(const RemoteAgentPollerLease&) = delete;
    RemoteAgentPollerLease& operator=(const RemoteAgentPollerLease&) = delete;
    ~RemoteAgentPollerLease() {
        release_remote_agent_poller(agent_id);
    }
};

inline void sleep_remote_agent_poll_interval(std::string_view agent_id, int interval_ms) {
    int slept = 0;
    while (slept < interval_ms && remote_agent_poll_should_continue(agent_id)) {
        const auto chunk = std::min(100, interval_ms - slept);
        std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
        slept += chunk;
    }
}

inline bool start_remote_agent_poll_loop(std::string agent_id) {
    if (!remote_agent_auto_poll_enabled()) {
        native_agent_store().append_transcript(agent_id, "system: remote auto poll disabled by environment");
        return false;
    }

    if (auto config = cc::utils::teleport::default_remote_session_api_config(); !config) {
        native_agent_store().append_transcript(
            agent_id,
            "system: remote auto poll not started: " + config.error());
        return false;
    }

    auto record = native_agent_store().get(agent_id);
    if (!record || !record->remote_session_id || record->remote_session_id->empty()) {
        native_agent_store().append_transcript(
            agent_id,
            "system: remote auto poll not started: missing remote session id");
        return false;
    }

    if (!reserve_remote_agent_poller(agent_id)) {
        return true;
    }

    const auto interval_ms = remote_agent_poll_interval_ms();
    std::thread([agent_id = std::move(agent_id), interval_ms] {
        RemoteAgentPollerLease lease(agent_id);
        std::size_t failure_count = 0;
        while (remote_agent_poll_should_continue(agent_id)) {
            auto applied = native_agent_store().poll_remote_agent_once(agent_id);
            if (applied) {
                failure_count = 0;
                if (applied->terminal) return;
            } else {
                ++failure_count;
                if (remote_poll_error_is_missing_local_config(applied.error())) {
                    native_agent_store().append_transcript(
                        agent_id,
                        "system: remote auto poll stopped: " + applied.error());
                    return;
                }
                if (failure_count == 1 || failure_count % 30 == 0) {
                    native_agent_store().append_transcript(
                        agent_id,
                        std::format("system: remote poll failed ({}): {}", failure_count, applied.error()));
                }
            }
            sleep_remote_agent_poll_interval(agent_id, interval_ms);
        }
    }).detach();
    return true;
}

inline std::size_t restore_remote_agent_poll_loops() {
    if (!remote_agent_auto_poll_enabled()) return 0;

    std::size_t restored = 0;
    for (const auto& record : native_agent_store().list()) {
        if (!record.background) continue;
        if (!record.remote_session_id || record.remote_session_id->empty()) continue;
        if (record.cancel_requested || native_agent_status_terminal(record.status)) continue;
        if (start_remote_agent_poll_loop(record.agent_id)) ++restored;
    }
    return restored;
}

[[nodiscard]] inline std::string build_fork_child_message(std::string_view directive) {
    std::string message =
        "<fork-boilerplate>\n"
        "STOP. READ THIS FIRST.\n\n"
        "You are a forked worker process. You are NOT the main agent.\n\n"
        "RULES (non-negotiable):\n"
        "1. Your system prompt says \"default to forking.\" IGNORE IT ";
    message += "\u2014";
    message +=
        " that's for the parent. You ARE the fork. Do NOT spawn sub-agents; execute directly.\n"
        "2. Do NOT converse, ask questions, or suggest next steps\n"
        "3. Do NOT editorialize or add meta-commentary\n"
        "4. USE your tools directly: Bash, Read, Write, etc.\n"
        "5. If you modify files, commit your changes before reporting. Include the commit hash in your report.\n"
        "6. Do NOT emit text between tool calls. Use tools silently, then report once at the end.\n"
        "7. Stay strictly within your directive's scope. If you discover related systems outside your scope, mention them in one sentence at most ";
    message += "\u2014";
    message +=
        " other workers cover those areas.\n"
        "8. Keep your report under 500 words unless the directive specifies otherwise. Be factual and concise.\n"
        "9. Your response MUST begin with \"Scope:\". No preamble, no thinking-out-loud.\n"
        "10. REPORT structured facts, then stop\n\n"
        "Output format (plain text labels, not markdown headers):\n"
        "  Scope: <echo back your assigned scope in one sentence>\n"
        "  Result: <the answer or key findings, limited to the scope above>\n"
        "  Key files: <relevant file paths ";
    message += "\u2014";
    message +=
        " include for research tasks>\n"
        "  Files changed: <list with commit hash ";
    message += "\u2014";
    message +=
        " include only if you modified files>\n"
        "  Issues: <list ";
    message += "\u2014";
    message +=
        " include only if there are issues to flag>\n"
        "</fork-boilerplate>\n\n"
        "Your directive: ";
    message += directive;
    return message;
}

[[nodiscard]] inline std::string build_worktree_fork_notice(
    std::string_view parent_cwd,
    std::string_view worktree_cwd
) {
    return std::format(
        "You've inherited the conversation context above from a parent agent working in {}. "
        "You are operating in an isolated git worktree at {} - same repository, same relative file structure, "
        "separate working copy. Paths in the inherited context refer to the parent's working directory; "
        "translate them to your worktree root. Re-read files before editing if the parent may have modified "
        "them since they appear in the context. Your changes stay in this worktree and will not affect the parent's files.",
        parent_cwd,
        worktree_cwd);
}

[[nodiscard]] inline std::string runtime_agent_id(const AgentRuntimeConfig& config) {
    if (!config.agent_id.empty()) return config.agent_id;
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return "agent-" + std::to_string(now);
}

inline std::expected<AgentExecutionResult, std::string> run_agent(const AgentRuntimeConfig& config) {
    auto id = runtime_agent_id(config);
    if (!config.working_dir.empty()) {
        std::error_code ec;
        if (!fs::exists(config.working_dir, ec) || !fs::is_directory(config.working_dir, ec)) {
            NativeAgentRecord failed{
                .agent_id = id,
                .agent_type = "runtime",
                .parent_agent_id = config.parent_agent_id,
                .cwd = config.working_dir,
                .status = NativeAgentStatus::Failed,
                .error = "Working directory does not exist",
                .capabilities = config.capabilities,
                .worktree_path = config.worktree_path,
                .worktree_branch = config.worktree_branch,
                .worktree_base_commit = config.worktree_base_commit,
                .worktree_git_root = config.worktree_git_root,
            };
            native_agent_store().upsert(std::move(failed));
            return std::unexpected("Working directory does not exist: " + config.working_dir);
        }
    }

    native_agent_store().upsert(NativeAgentRecord{
        .agent_id = id,
        .agent_type = "runtime",
        .parent_agent_id = config.parent_agent_id,
        .cwd = config.working_dir.empty() ? std::nullopt : std::optional<std::string>{config.working_dir},
        .status = NativeAgentStatus::Queued,
        .capabilities = config.capabilities,
        .worktree_path = config.worktree_path,
        .worktree_branch = config.worktree_branch,
        .worktree_base_commit = config.worktree_base_commit,
        .worktree_git_root = config.worktree_git_root,
    });
    native_agent_store().mark_running(id);

    std::string output = "Agent " + id + " completed";
    if (!config.working_dir.empty()) output += " in " + config.working_dir;
    if (!config.capabilities.empty()) {
        output += " with capabilities: ";
        for (std::size_t i = 0; i < config.capabilities.size(); ++i) {
            if (i != 0) output += ", ";
            output += config.capabilities[i];
        }
    }
    native_agent_store().append_transcript(id, "user: " + id);
    native_agent_store().append_transcript(id, "assistant: " + output);
    native_agent_store().mark_completed(id, output);
    auto record = native_agent_store().get(id);
    return AgentExecutionResult{
        .agent_id = id,
        .exit_code = 0,
        .output = output,
        .error = std::nullopt,
        .transcript = record ? record->transcript : std::vector<std::string>{},
    };
}

inline std::expected<std::string, std::string> fork_subagent(std::string_view parent_id, const AgentRuntimeConfig& config) {
    if (parent_id.empty()) return std::unexpected("Parent agent id is required");
    if (!config.allow_fork) return std::unexpected("Agent forking is disabled by runtime config");

    auto parent = native_agent_store().get(parent_id);
    if (!parent) return std::unexpected("Parent agent not found: " + std::string(parent_id));
    if (native_agent_record_is_fork_child(*parent)) {
        return std::unexpected("Fork is not available inside a forked worker. Complete your task directly using your tools.");
    }

    auto child_id = runtime_agent_id(config);
    if (child_id == parent_id) child_id += "-fork";

    auto inherited_sidechain_entries = fork_sidechain_entries_for_child(*parent, child_id);
    auto inherited_transcript = transcript_lines_from_sidechain_entries(inherited_sidechain_entries);
    if (inherited_transcript.empty()) inherited_transcript = parent->transcript;
    inherited_transcript.push_back(std::format("system: forked from {}", parent->agent_id));
    auto unresolved_tool_use_ids = unresolved_tool_use_ids_from_sidechain_entries(inherited_sidechain_entries);
    if (config.fork_directive && !config.fork_directive->empty()) {
        auto directive_message = build_fork_child_message(*config.fork_directive);
        auto directive_content_json = fork_missing_tool_results_content_json(
            unresolved_tool_use_ids,
            directive_message);
        auto directive_entry = make_sidechain_jsonl_entry(
            child_id,
            inherited_sidechain_entries.size(),
            "user",
            directive_content_json,
            directive_message);
        if (auto transcript_entry = transcript_entry_from_sidechain_jsonl_line(directive_entry)) {
            inherited_transcript.push_back(std::move(*transcript_entry));
        } else {
            inherited_transcript.push_back("user: " + directive_message);
        }
        inherited_sidechain_entries.push_back(std::move(directive_entry));
    } else if (!unresolved_tool_use_ids.empty()) {
        auto missing_tool_results_content_json = fork_missing_tool_results_content_json(unresolved_tool_use_ids, {});
        auto missing_tool_results_entry = make_sidechain_jsonl_entry(
            child_id,
            inherited_sidechain_entries.size(),
            "user",
            missing_tool_results_content_json,
            {});
        if (auto transcript_entry = transcript_entry_from_sidechain_jsonl_line(missing_tool_results_entry)) {
            inherited_transcript.push_back(std::move(*transcript_entry));
        }
        inherited_sidechain_entries.push_back(std::move(missing_tool_results_entry));
    }

    auto child_cwd = config.working_dir.empty()
        ? parent->cwd
        : std::optional<std::string>{config.working_dir};
    auto child_worktree_path = config.worktree_path;
    auto child_worktree_branch = config.worktree_branch;
    auto child_worktree_base_commit = config.worktree_base_commit;
    auto child_worktree_git_root = config.worktree_git_root;
    if (!child_worktree_path && parent->worktree_path && child_cwd && *child_cwd == *parent->worktree_path) {
        child_worktree_path = parent->worktree_path;
        child_worktree_branch = parent->worktree_branch;
        child_worktree_base_commit = parent->worktree_base_commit;
        child_worktree_git_root = parent->worktree_git_root;
    }
    if (child_worktree_path && parent->cwd && *child_worktree_path != *parent->cwd) {
        auto worktree_notice = build_worktree_fork_notice(*parent->cwd, *child_worktree_path);
        inherited_transcript.push_back("user: " + worktree_notice);
        inherited_sidechain_entries.push_back(make_sidechain_jsonl_entry(
            child_id,
            inherited_sidechain_entries.size(),
            "user",
            {},
            worktree_notice));
    }

    native_agent_store().upsert(NativeAgentRecord{
        .agent_id = child_id,
        .agent_type = parent->agent_type,
        .parent_agent_id = std::string(parent_id),
        .team_name = parent->team_name,
        .cwd = std::move(child_cwd),
        .isolation = parent->isolation,
        .mode = parent->mode,
        .background = true,
        .status = NativeAgentStatus::Queued,
        .capabilities = fork_child_capabilities(*parent, config),
        .sidechain_entries = std::move(inherited_sidechain_entries),
        .worktree_path = std::move(child_worktree_path),
        .worktree_branch = std::move(child_worktree_branch),
        .worktree_base_commit = std::move(child_worktree_base_commit),
        .worktree_git_root = std::move(child_worktree_git_root),
        .transcript = std::move(inherited_transcript),
    });
    return child_id;
}

inline std::expected<AgentExecutionResult, std::string> resume_agent(std::string_view agent_id) {
    auto record = native_agent_store().get(agent_id);
    if (!record) return std::unexpected("Agent not found: " + std::string(agent_id));

    if (record->worktree_path && !record->worktree_path->empty()) {
        std::error_code ec;
        auto worktree_path = fs::path{*record->worktree_path};
        if (fs::exists(worktree_path, ec) && fs::is_directory(worktree_path, ec)) {
            ec.clear();
            fs::last_write_time(worktree_path, fs::file_time_type::clock::now(), ec);
        } else {
            auto previous_worktree = *record->worktree_path;
            native_agent_store().mark_worktree_cleaned(agent_id);
            native_agent_store().append_transcript(
                agent_id,
                "system: resumed worktree " + previous_worktree +
                    " no longer exists; falling back to parent cwd");
            record = native_agent_store().get(agent_id);
            if (!record) return std::unexpected("Agent not found after worktree resume update: " + std::string(agent_id));
        }
    }

    std::string output = record->output.value_or(
        std::format("Agent {} is {}", record->agent_id, native_agent_status_name(record->status)));
    auto error = record->error;
    auto exit_code = record->status == NativeAgentStatus::Failed ? 1 :
        record->status == NativeAgentStatus::Cancelled ? 130 : 0;
    return AgentExecutionResult{
        .agent_id = record->agent_id,
        .exit_code = exit_code,
        .output = std::move(output),
        .error = std::move(error),
        .transcript = record->transcript,
    };
}

inline AgentLifecycle get_agent_lifecycle(std::string_view agent_id) {
    auto record = native_agent_store().get(agent_id);
    if (!record) return AgentLifecycle::Failed;
    switch (record->status) {
        case NativeAgentStatus::Queued: return AgentLifecycle::Starting;
        case NativeAgentStatus::Running: return AgentLifecycle::Running;
        case NativeAgentStatus::Completed: return AgentLifecycle::Completed;
        case NativeAgentStatus::Failed: return AgentLifecycle::Failed;
        case NativeAgentStatus::Cancelled: return AgentLifecycle::Cancelled;
    }
    return AgentLifecycle::Failed;
}

} // namespace cc::tools::agent_runtime
