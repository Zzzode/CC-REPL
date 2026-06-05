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
#include <unordered_map>
#include <variant>

export module cc.tools.agent_runtime;

import cc.utils.json;
import cc.utils.yaml;

export namespace cc::tools::agent_runtime {

namespace fs = std::filesystem;

struct AgentRuntimeConfig {
    std::string agent_id;
    std::string working_dir;
    std::vector<std::string> capabilities;
    std::optional<std::string> parent_agent_id;
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
    std::vector<std::string> transcript;
    std::optional<double> progress;
    bool cancel_requested = false;
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

[[nodiscard]] inline std::optional<std::string> resolve_requested_agent_type(
    std::string_view requested_type,
    const std::vector<AgentDefinition>& agents
) {
    const auto requested = trim(requested_type);
    if (requested.empty()) return std::nullopt;

    for (const auto& agent : agents) {
        if (agent.agent_type == requested) return agent.agent_type;
    }

    if (auto canonical = find_canonical_agent_type_match(requested, agents)) {
        return canonical;
    }

    for (const auto& alias : agent_alias_candidates(requested)) {
        if (auto alias_match = find_canonical_agent_type_match(alias, agents)) {
            return alias_match;
        }
    }

    for (const auto& alias : agent_alias_candidates(requested)) {
        const auto suffix = ":" + alias;
        std::vector<std::string> matches;
        for (const auto& agent : agents) {
            const auto canonical = canonicalize_agent_type(agent.agent_type);
            if (canonical.ends_with(suffix)) matches.push_back(agent.agent_type);
        }
        if (matches.size() == 1) return matches.front();
    }

    return std::nullopt;
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

[[nodiscard]] inline std::vector<AgentDefinition> built_in_agent_definitions() {
    return {
        AgentDefinition{
            .agent_type = "general-purpose",
            .when_to_use = "General-purpose agent for researching, searching, and executing multi-step tasks.",
            .model = "inherit",
            .source = "built-in",
            .filename = std::nullopt,
            .path = std::nullopt,
            .system_prompt = "",
            .tools = {"Read", "Write", "Edit", "Glob", "Grep", "Bash", "WebSearch", "WebFetch"},
            .disallowed_tools = {},
            .max_turns = std::nullopt,
            .initial_prompt = std::nullopt,
            .background = false,
            .isolation = std::nullopt,
            .required_mcp_servers = {},
            .mcp_servers = {},
            .inline_mcp_servers = {},
            .skills = {},
            .hooks_present = false,
        },
        AgentDefinition{
            .agent_type = "Explore",
            .when_to_use = "Read-only exploration agent for understanding code and project context.",
            .model = "haiku",
            .source = "built-in",
            .filename = std::nullopt,
            .path = std::nullopt,
            .system_prompt = "",
            .tools = {"Read", "Glob", "Grep"},
            .disallowed_tools = {},
            .max_turns = 15,
            .initial_prompt = std::nullopt,
            .background = false,
            .isolation = std::nullopt,
            .required_mcp_servers = {},
            .mcp_servers = {},
            .inline_mcp_servers = {},
            .skills = {},
            .hooks_present = false,
        },
        AgentDefinition{
            .agent_type = "Plan",
            .when_to_use = "Read-only planning agent for producing implementation plans.",
            .model = "inherit",
            .source = "built-in",
            .filename = std::nullopt,
            .path = std::nullopt,
            .system_prompt = "",
            .tools = {"Read", "Glob", "Grep", "WebSearch"},
            .disallowed_tools = {},
            .max_turns = 10,
            .initial_prompt = std::nullopt,
            .background = false,
            .isolation = std::nullopt,
            .required_mcp_servers = {},
            .mcp_servers = {},
            .inline_mcp_servers = {},
            .skills = {},
            .hooks_present = false,
        },
        AgentDefinition{
            .agent_type = "verification",
            .when_to_use = "Verification agent for checking completed work against requirements.",
            .model = "inherit",
            .source = "built-in",
            .filename = std::nullopt,
            .path = std::nullopt,
            .system_prompt = "",
            .tools = {"Read", "Glob", "Grep", "Bash"},
            .disallowed_tools = {},
            .max_turns = 20,
            .initial_prompt = std::nullopt,
            .background = false,
            .isolation = std::nullopt,
            .required_mcp_servers = {},
            .mcp_servers = {},
            .inline_mcp_servers = {},
            .skills = {},
            .hooks_present = false,
        },
        AgentDefinition{
            .agent_type = "statusline-setup",
            .when_to_use = "Agent for configuring statusline behavior.",
            .model = "sonnet",
            .source = "built-in",
            .filename = std::nullopt,
            .path = std::nullopt,
            .system_prompt = "",
            .tools = {"Read", "Write", "Edit"},
            .disallowed_tools = {},
            .max_turns = std::nullopt,
            .initial_prompt = std::nullopt,
            .background = false,
            .isolation = std::nullopt,
            .required_mcp_servers = {},
            .mcp_servers = {},
            .inline_mcp_servers = {},
            .skills = {},
            .hooks_present = false,
        },
        AgentDefinition{
            .agent_type = "claude-code-guide",
            .when_to_use = "Agent for questions about Claude Code, Claude Agent SDK, and Claude API usage.",
            .model = "haiku",
            .source = "built-in",
            .filename = std::nullopt,
            .path = std::nullopt,
            .system_prompt = "",
            .tools = {"Read", "WebSearch", "WebFetch"},
            .disallowed_tools = {},
            .max_turns = std::nullopt,
            .initial_prompt = std::nullopt,
            .background = false,
            .isolation = std::nullopt,
            .required_mcp_servers = {},
            .mcp_servers = {},
            .inline_mcp_servers = {},
            .skills = {},
            .hooks_present = false,
        },
    };
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
    if (!name || !description) {
        return std::nullopt;
    }

    auto body_start = text.find('\n', frontmatter_end + 4);
    std::string body = body_start == std::string::npos ? "" : trim(std::string_view(text).substr(body_start + 1));

    AgentDefinition definition;
    definition.agent_type = *name;
    definition.when_to_use = *description;
    definition.model = yaml_string_field(*fields, "model").value_or("inherit");
    definition.source = std::move(source);
    definition.filename = path.stem().string();
    definition.path = path.string();
    definition.system_prompt = std::move(body);
    definition.tools = yaml_string_list_field(*fields, "tools");
    definition.disallowed_tools = yaml_string_list_field(*fields, "disallowedTools");
    if (auto max_turns = yaml_string_field(*fields, "maxTurns")) {
        definition.max_turns = parse_positive_int(*max_turns);
    }
    if (auto initial = yaml_string_field(*fields, "initialPrompt"); initial && !initial->empty()) {
        definition.initial_prompt = std::move(*initial);
    }
    if (auto background = yaml_string_field(*fields, "background")) {
        definition.background = parse_bool_field(*background);
    }
    if (auto isolation = yaml_string_field(*fields, "isolation"); isolation && !isolation->empty()) {
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
        if (entry.path().extension() != ".md") continue;
        if (auto parsed = parse_agent_markdown(entry.path(), source)) {
            agents.push_back(std::move(*parsed));
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

    if (fs::is_regular_file(path, ec) && path.extension() == ".md") {
        if (auto parsed = parse_agent_markdown(path, "plugin")) {
            append_plugin_agent_definition(agents, std::move(*parsed), plugin_name, namespace_parts);
        }
        return;
    }

    if (!fs::is_directory(path, ec)) return;
    for (const auto& entry : fs::directory_iterator(path, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec) && entry.path().extension() == ".md") {
            if (auto parsed = parse_agent_markdown(entry.path(), "plugin")) {
                append_plugin_agent_definition(agents, std::move(*parsed), plugin_name, namespace_parts);
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
    for (auto& agent : built_in_agent_definitions()) {
        by_type[agent.agent_type] = std::move(agent);
    }

    for (auto& agent : load_plugin_agent_definitions()) {
        by_type[agent.agent_type] = std::move(agent);
    }

    if (auto* home = std::getenv("HOME")) {
        for (auto& agent : load_agent_definitions_from_dir(
            fs::path(home) / ".claude" / "agents", "userSettings")) {
            by_type[agent.agent_type] = std::move(agent);
        }
    }

    for (auto& agent : load_agent_definitions_from_dir(
        cwd.value_or(fs::current_path()) / ".claude" / "agents", "projectSettings")) {
        by_type[agent.agent_type] = std::move(agent);
    }

    std::vector<AgentDefinition> active;
    for (auto& [_, agent] : by_type) active.push_back(std::move(agent));
    return active;
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

class NativeAgentStore {
public:
    void upsert(NativeAgentRecord record) {
        std::scoped_lock lock(mutex_);
        record.updated_at = std::chrono::system_clock::now();
        records_[record.agent_id] = std::move(record);
    }

    [[nodiscard]] std::optional<NativeAgentRecord> get(std::string_view agent_id) const {
        std::scoped_lock lock(mutex_);
        auto it = records_.find(std::string(agent_id));
        if (it == records_.end()) return std::nullopt;
        return it->second;
    }

    [[nodiscard]] std::vector<NativeAgentRecord> list() const {
        std::scoped_lock lock(mutex_);
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
            record.status = NativeAgentStatus::Completed;
            record.output = std::move(output);
            record.error = std::nullopt;
            record.progress = 1.0;
        });
    }

    void mark_failed(std::string_view agent_id, std::string error) {
        update(agent_id, [&](NativeAgentRecord& record) {
            record.status = NativeAgentStatus::Failed;
            record.error = std::move(error);
        });
    }

    void mark_cancelled(std::string_view agent_id, std::string reason) {
        update(agent_id, [&](NativeAgentRecord& record) {
            record.status = NativeAgentStatus::Cancelled;
            record.cancel_requested = true;
            record.error = std::move(reason);
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

    void append_transcript(std::string_view agent_id, std::string entry) {
        update(agent_id, [&](NativeAgentRecord& record) {
            record.transcript.push_back(std::move(entry));
        });
    }

    void update_progress(std::string_view agent_id, double progress) {
        update(agent_id, [&](NativeAgentRecord& record) {
            record.progress = std::clamp(progress, 0.0, 1.0);
        });
    }

private:
    template <typename Fn>
    void update(std::string_view agent_id, Fn&& fn) {
        std::scoped_lock lock(mutex_);
        auto it = records_.find(std::string(agent_id));
        if (it == records_.end()) return;
        fn(it->second);
        it->second.updated_at = std::chrono::system_clock::now();
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, NativeAgentRecord> records_;
};

inline NativeAgentStore& native_agent_store() {
    static NativeAgentStore store;
    return store;
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

    auto child_id = runtime_agent_id(config);
    if (child_id == parent_id) child_id += "-fork";
    native_agent_store().upsert(NativeAgentRecord{
        .agent_id = child_id,
        .agent_type = parent->agent_type,
        .parent_agent_id = std::string(parent_id),
        .team_name = parent->team_name,
        .cwd = config.working_dir.empty() ? parent->cwd : std::optional<std::string>{config.working_dir},
        .isolation = parent->isolation,
        .mode = parent->mode,
        .background = true,
        .status = NativeAgentStatus::Queued,
        .capabilities = config.capabilities.empty() ? parent->capabilities : config.capabilities,
    });
    return child_id;
}

inline std::expected<AgentExecutionResult, std::string> resume_agent(std::string_view agent_id) {
    auto record = native_agent_store().get(agent_id);
    if (!record) return std::unexpected("Agent not found: " + std::string(agent_id));

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
