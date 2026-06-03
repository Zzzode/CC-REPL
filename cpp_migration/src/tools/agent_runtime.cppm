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
#include <sstream>
#include <map>
#include <algorithm>
#include <cctype>
#include <iterator>

export module cc.tools.agent_runtime;

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
};

enum class AgentLifecycle {
    Starting,
    Running,
    Suspended,
    Completed,
    Failed
};

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
    std::vector<std::string> skills;
    bool hooks_present = false;
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
            .skills = {},
            .hooks_present = false,
        },
    };
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

    std::map<std::string, std::string> fields;
    std::stringstream frontmatter(text.substr(first_newline + 1, frontmatter_end - first_newline - 1));
    std::string line;
    while (std::getline(frontmatter, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto key = trim(std::string_view(line).substr(0, colon));
        auto value = unquote(line.substr(colon + 1));
        if (!key.empty()) fields[key] = value;
    }

    auto name_it = fields.find("name");
    auto description_it = fields.find("description");
    if (name_it == fields.end() || description_it == fields.end()) {
        return std::nullopt;
    }

    auto body_start = text.find('\n', frontmatter_end + 4);
    std::string body = body_start == std::string::npos ? "" : trim(std::string_view(text).substr(body_start + 1));

    AgentDefinition definition;
    definition.agent_type = name_it->second;
    definition.when_to_use = description_it->second;
    definition.model = fields.contains("model") ? fields["model"] : "inherit";
    definition.source = std::move(source);
    definition.filename = path.stem().string();
    definition.path = path.string();
    definition.system_prompt = std::move(body);
    if (auto tools_it = fields.find("tools"); tools_it != fields.end()) {
        definition.tools = split_list_value(tools_it->second);
    }
    if (auto disallowed_it = fields.find("disallowedTools"); disallowed_it != fields.end()) {
        definition.disallowed_tools = split_list_value(disallowed_it->second);
    }
    if (auto max_turns_it = fields.find("maxTurns"); max_turns_it != fields.end()) {
        definition.max_turns = parse_positive_int(max_turns_it->second);
    }
    if (auto initial_it = fields.find("initialPrompt"); initial_it != fields.end() && !initial_it->second.empty()) {
        definition.initial_prompt = initial_it->second;
    }
    if (auto background_it = fields.find("background"); background_it != fields.end()) {
        definition.background = parse_bool_field(background_it->second);
    }
    if (auto isolation_it = fields.find("isolation"); isolation_it != fields.end() && !isolation_it->second.empty()) {
        definition.isolation = isolation_it->second;
    }
    if (auto required_mcp_it = fields.find("requiredMcpServers"); required_mcp_it != fields.end()) {
        definition.required_mcp_servers = split_list_value(required_mcp_it->second);
    }
    if (auto mcp_it = fields.find("mcpServers"); mcp_it != fields.end()) {
        definition.mcp_servers = split_list_value(mcp_it->second);
    }
    if (auto skills_it = fields.find("skills"); skills_it != fields.end()) {
        definition.skills = split_list_value(skills_it->second);
    }
    definition.hooks_present = fields.contains("hooks");
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

[[nodiscard]] inline std::vector<AgentDefinition> get_all_agent_definitions(
    std::optional<fs::path> cwd = std::nullopt
) {
    std::map<std::string, AgentDefinition> by_type;
    for (auto& agent : built_in_agent_definitions()) {
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

inline std::expected<AgentExecutionResult, std::string> run_agent(const AgentRuntimeConfig& config) {
    return AgentExecutionResult{config.agent_id, 0, "", std::nullopt};
}

inline std::expected<std::string, std::string> fork_subagent(std::string_view parent_id, const AgentRuntimeConfig& config) {
    return std::string(config.agent_id);
}

inline std::expected<AgentExecutionResult, std::string> resume_agent(std::string_view agent_id) {
    return AgentExecutionResult{std::string(agent_id), 0, "", std::nullopt};
}

inline std::expected<std::vector<std::string>, std::string> load_agents_from_dir(std::string_view dir_path) {
    std::vector<std::string> names;
    for (const auto& agent : load_agent_definitions_from_dir(fs::path(dir_path), "custom")) {
        names.push_back(agent.agent_type);
    }
    return names;
}

inline AgentLifecycle get_agent_lifecycle(std::string_view agent_id) {
    return AgentLifecycle::Completed;
}

} // namespace cc::tools::agent_runtime
