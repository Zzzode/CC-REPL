// Implementation unit for cc.tools.agent.utils — tool allow/disallow rule
// parsing and matching, agent-type permission rules, native MCP server
// connect/prepare/upsert mapping, and skill discovery/preload.
module;

module cc.tools.agent.utils;

import std;

import cc.tools.mcp;
import cc.services.mcp.types;
import cc.tools.agent_runtime;
import cc.skills.skill;

namespace cc::tools::agent::utils {

namespace fs = std::filesystem;

[[nodiscard]] bool tool_rule_matches_tool_name(
    std::string_view rule,
    std::string_view tool_name
) {
    const auto parsed = permission_rule_tool_name(rule);
    return parsed == "*" || canonical_tool_name(parsed) == canonical_tool_name(tool_name);
}

[[nodiscard]] bool normalized_tool_name_is(std::string_view tool_name, std::string_view expected) {
    return canonical_tool_name(tool_name) == canonical_tool_name(expected);
}

[[nodiscard]] bool normalized_tool_name_in(
    std::string_view tool_name,
    std::initializer_list<std::string_view> names
) {
    const auto normalized = canonical_tool_name(tool_name);
    for (auto name : names) {
        if (normalized == canonical_tool_name(name)) return true;
    }
    return false;
}

[[nodiscard]] bool is_mcp_tool_name(std::string_view tool_name) {
    return normalized_tool_name_is(tool_name, "mcp") || tool_name.starts_with("mcp__");
}

[[nodiscard]] bool all_agent_disallows_tool(std::string_view tool_name) {
    const bool nested_agents_enabled = [] {
        if (const char* value = std::getenv("USER_TYPE"); value && std::string_view(value) == "ant") {
            return true;
        }
        if (const char* value = std::getenv("LOOM_ENABLE_NESTED_AGENTS"); value && *value) {
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

[[nodiscard]] bool custom_agent_disallows_tool(std::string_view tool_name) {
    return all_agent_disallows_tool(tool_name);
}

[[nodiscard]] bool async_agent_allows_tool(std::string_view tool_name) {
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

[[nodiscard]] bool in_process_teammate_allows_tool(std::string_view tool_name) {
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

[[nodiscard]] bool agent_base_filter_allows_tool(
    std::string_view tool_name,
    bool is_built_in,
    bool is_async,
    std::optional<std::string_view> permission_mode,
    bool is_in_process_teammate
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

[[nodiscard]] std::string canonical_tool_rule_value(std::string_view value) {
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

[[nodiscard]] std::vector<std::string> permission_rule_arguments(std::string_view rule) {
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

[[nodiscard]] bool agent_type_matches_permission_rule(
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

[[nodiscard]] bool agent_type_allowed_by_permission_rules(
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

[[nodiscard]] bool tool_name_allowed_by_definition(
    std::string_view tool_name,
    const std::vector<std::string>& allowed_tools
) {
    if (allowed_tools.empty()) return true;
    for (const auto& allowed : allowed_tools) {
        if (tool_rule_matches_tool_name(allowed, tool_name)) return true;
    }
    return false;
}

[[nodiscard]] bool tool_name_disallowed_by_definition(
    std::string_view tool_name,
    const std::vector<std::string>& disallowed_tools
) {
    for (const auto& disallowed : disallowed_tools) {
        if (tool_rule_matches_tool_name(disallowed, tool_name)) return true;
    }
    return false;
}

[[nodiscard]] std::string lowercase_copy(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    return out;
}

[[nodiscard]] bool case_insensitive_contains(std::string_view haystack, std::string_view needle) {
    return lowercase_copy(haystack).contains(lowercase_copy(needle));
}

[[nodiscard]] std::vector<std::string> available_mcp_servers_with_tools() {
    std::vector<std::string> names;
    for (const auto& status : cc::tools::native_mcp_statuses()) {
        if (status.status == "ready" && !status.tools.empty()) {
            names.push_back(status.name);
        }
    }
    return names;
}

[[nodiscard]] std::vector<std::string> missing_required_mcp_servers(
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

[[nodiscard]] std::optional<std::string> resolve_agent_skill_name(
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

[[nodiscard]] std::string format_preloaded_skill_message(
    std::string_view skill_name,
    std::string_view content
) {
    return std::format("<skill name=\"{}\">\n{}\n</skill>", skill_name, content);
}

[[nodiscard]] std::vector<std::string> load_preloaded_skill_messages(
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

[[nodiscard]] std::string format_agent_mcp_context_message(
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

[[nodiscard]] std::vector<AgentMcpToolBinding> connect_agent_mcp_servers(
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

[[nodiscard]] cc::tools::NativeMcpConfiguredServer to_native_agent_mcp_server(
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

void append_unique_agent_mcp_server(
    std::vector<std::string>& names,
    std::string name
) {
    if (name.empty()) return;
    for (const auto& existing : names) {
        if (existing == name) return;
    }
    names.push_back(std::move(name));
}

[[nodiscard]] std::expected<std::vector<AgentInlineMcpServerRuntimeState>, std::string>
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

} // namespace cc::tools::agent::utils
