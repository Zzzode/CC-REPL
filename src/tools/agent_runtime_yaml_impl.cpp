// Implementation unit for cc.tools.agent_runtime — YAML scalar/field/list
// helpers, the YAML overloads of parse_inline_mcp_server_config /
// parse_agent_mcp_servers, the five YAML agent-hook parsers, markdown
// frontmatter agent parsing (parse_agent_markdown), and get_parse_error.
module;

#include <cctype>
#include <cstdint>

module cc.tools.agent_runtime;

import std;

import cc.utils.yaml;

namespace cc::tools::agent_runtime {

[[nodiscard]] std::optional<std::string> yaml_scalar_to_string(const cc::utils::YamlValue& value) {
    if (const auto* text = std::get_if<std::string>(&value.data)) return *text;
    if (const auto* flag = std::get_if<bool>(&value.data)) return *flag ? "true" : "false";
    if (const auto* number = std::get_if<int64_t>(&value.data)) return std::to_string(*number);
    if (const auto* number = std::get_if<double>(&value.data)) return std::format("{}", *number);
    return std::nullopt;
}
[[nodiscard]] const cc::utils::YamlValue* yaml_field(
    const cc::utils::YamlMap& fields,
    std::string_view key
) {
    auto it = fields.find(std::string(key));
    return it == fields.end() ? nullptr : &it->second;
}
[[nodiscard]] std::optional<std::string> yaml_string_field(
    const cc::utils::YamlMap& fields,
    std::string_view key
) {
    const auto* value = yaml_field(fields, key);
    if (!value) return std::nullopt;
    return yaml_scalar_to_string(*value);
}
[[nodiscard]] std::vector<std::string> yaml_string_list(const cc::utils::YamlValue& value) {
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
[[nodiscard]] std::vector<std::string> yaml_string_list_field(
    const cc::utils::YamlMap& fields,
    std::string_view key
) {
    const auto* value = yaml_field(fields, key);
    return value ? yaml_string_list(*value) : std::vector<std::string>{};
}
void append_yaml_string_map(
    const cc::utils::YamlValue& value,
    std::unordered_map<std::string, std::string>& out
) {
    const auto* map = std::get_if<cc::utils::YamlMap>(&value.data);
    if (!map) return;
    for (const auto& [key, item] : *map) {
        if (auto scalar = yaml_scalar_to_string(item)) out[key] = std::move(*scalar);
    }
}
[[nodiscard]] std::optional<AgentInlineMcpServerConfig> parse_inline_mcp_server_config(
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
[[nodiscard]] ParsedAgentMcpServers parse_agent_mcp_servers(const cc::utils::YamlValue& value) {
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
[[nodiscard]] std::optional<AgentHookCommand> parse_agent_hook_command(const cc::utils::YamlValue& value) {
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
[[nodiscard]] std::vector<AgentHookCommand> parse_agent_hook_commands(const cc::utils::YamlValue& value) {
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
[[nodiscard]] std::optional<AgentHookMatcher> parse_agent_hook_matcher(const cc::utils::YamlValue& value) {
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
[[nodiscard]] std::vector<AgentHookMatcher> parse_agent_hook_matchers(const cc::utils::YamlValue& value) {
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
[[nodiscard]] AgentHooksByEvent parse_agent_hooks(const cc::utils::YamlValue& value) {
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
[[nodiscard]] std::optional<AgentDefinition> parse_agent_markdown(
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
    if (auto omit = yaml_string_field(*fields, "omitLoomMd")) {
        definition.omit_loom_md = parse_bool_field(*omit);
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
[[nodiscard]] std::string get_parse_error(
    const cc::utils::YamlMap& fields,
    std::string_view fallback
) {
    const auto name = yaml_string_field(fields, "name");
    if (!name || name->empty()) {
        return "Missing required \"name\" field in frontmatter";
    }
    const auto description = yaml_string_field(fields, "description");
    if (!description || description->empty()) {
        return "Missing required \"description\" field in frontmatter";
    }
    return std::string(fallback);
}
} // namespace cc::tools::agent_runtime
