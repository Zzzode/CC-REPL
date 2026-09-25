// Implementation unit for cc.tools.agent_runtime — JSON scalar/field/list
// helpers, the JSON overloads of parse_inline_mcp_server_config /
// parse_agent_mcp_servers, the five JSON agent-hook parsers, JSON agent
// definition loading (file/string/settings/flag/policy), content-text and
// string-array extraction.
module;

#include <cstdlib>

module cc.tools.agent_runtime;

import std;

import cc.utils.json;

namespace cc::tools::agent_runtime {

[[nodiscard]] std::optional<std::string> json_scalar_to_string(cc::utils::json::JsonVal value) {
    if (!value.valid()) return std::nullopt;
    if (value.is_str()) return std::string(value.as_str());
    if (value.is_bool()) return value.as_bool()
        ? std::optional<std::string>{"true"}
        : std::optional<std::string>{"false"};
    if (value.is_num()) return std::format("{}", value.as_double());
    return std::nullopt;
}
[[nodiscard]] std::optional<std::string> json_string_field(
    cc::utils::json::JsonVal object,
    std::string_view key
) {
    if (!object.valid() || !object.is_obj()) return std::nullopt;
    return json_scalar_to_string(object.get(key));
}
[[nodiscard]] std::vector<std::string> json_string_list(cc::utils::json::JsonVal value) {
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
[[nodiscard]] std::vector<std::string> json_string_list_field(
    cc::utils::json::JsonVal object,
    std::string_view key
) {
    if (!object.valid() || !object.is_obj()) return {};
    return json_string_list(object.get(key));
}
void append_json_string_map(
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
[[nodiscard]] std::optional<AgentInlineMcpServerConfig> parse_inline_mcp_server_config(
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
[[nodiscard]] ParsedAgentMcpServers parse_agent_mcp_servers(cc::utils::json::JsonVal value) {
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
[[nodiscard]] std::optional<int> json_positive_int_field(
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
[[nodiscard]] std::optional<bool> json_bool_field(
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
[[nodiscard]] std::optional<AgentHookCommand> parse_agent_hook_command(cc::utils::json::JsonVal value) {
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
[[nodiscard]] std::vector<AgentHookCommand> parse_agent_hook_commands(cc::utils::json::JsonVal value) {
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
[[nodiscard]] std::optional<AgentHookMatcher> parse_agent_hook_matcher(cc::utils::json::JsonVal value) {
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
[[nodiscard]] std::vector<AgentHookMatcher> parse_agent_hook_matchers(cc::utils::json::JsonVal value) {
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
[[nodiscard]] AgentHooksByEvent parse_agent_hooks(cc::utils::json::JsonVal value) {
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
[[nodiscard]] std::optional<AgentDefinition> parse_agent_json_definition(
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
    if (auto omit = json_bool_field(object, "omitLoomMd")
            .or_else([&] { return json_bool_field(object, "omit_loom_md"); })) {
        definition.omit_loom_md = *omit;
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
[[nodiscard]] std::vector<AgentDefinition> parse_agents_json_file(
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
[[nodiscard]] std::vector<AgentDefinition> parse_agents_json_string(
    std::string_view json,
    std::string source,
    const fs::path& virtual_path
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
[[nodiscard]] std::vector<AgentDefinition> load_agent_definitions_from_settings_file(
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
[[nodiscard]] std::vector<AgentDefinition> load_flag_agent_definitions() {
    const char* json = std::getenv("LOOM_AGENTS_JSON");
    if (!json || !*json) json = std::getenv("CLAUDE_CODE_AGENTS_JSON");
    if (!json || !*json) return {};
    return parse_agents_json_string(json, "flagSettings", fs::path{"<flag-agents>"});
}
[[nodiscard]] std::vector<AgentDefinition> load_policy_agent_definitions() {
    const char* path = std::getenv("LOOM_POLICY_SETTINGS");
    if (!path || !*path) return {};
    return load_agent_definitions_from_settings_file(fs::path{path}, "policySettings");
}
[[nodiscard]] std::string json_string_field(
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
[[nodiscard]] std::string json_text_from_content(cc::utils::json::JsonVal content) {
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
[[nodiscard]] std::vector<std::string> json_string_array(cc::utils::json::JsonVal value) {
    std::vector<std::string> out;
    if (!value.is_arr()) return out;
    value.iter([&](cc::utils::json::JsonVal item) {
        if (item.is_str()) out.push_back(std::string(item.as_str()));
    });
    return out;
}
} // namespace cc::tools::agent_runtime
