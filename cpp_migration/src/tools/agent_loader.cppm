// Agent directory loader facade — thin wrapper around
// cc.tools.agent_runtime's load_agent_definitions_from_dir[_ex] / get_all_agent_definitions.
//
// Mirrors TypeScript: src/tools/AgentTool/loadAgentsDir.ts
//   - getAgentDefinitionsWithOverrides  →  get_all_agent_definitions
//   - directory scanning                →  load_agents_dir
//
// The file-level parsers (parse_agent_markdown, parse_agent_json_definition,
// parse_agents_json_file, load_agent_definitions_from_settings_file) continue
// to live in agent_runtime.cppm; this module only exposes the "load a whole
// directory / aggregate everything" surface that most callers want.
module;

#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <optional>
#include <algorithm>

export module cc.tools.agent_loader;

import cc.tools.agent_runtime;

export namespace cc::tools::agent_loader {

namespace fs = std::filesystem;

using cc::tools::agent_runtime::AgentDefinition;
using cc::tools::agent_runtime::FailedAgentFile;
using cc::tools::agent_runtime::LoadAgentDefinitionsResult;

// Scans a directory for agent definition files (`.md` with YAML frontmatter,
// `.json` as a record of name → definition). Mirrors TS loadAgentsDir for a
// single directory; use get_all_agent_definitions() below to get the full
// union of built-in + plugin + user + project + policy + flag agents.
//
// `source` labels the returned AgentDefinition::source field so callers can
// later distinguish "user" agents from "project" agents during conflict
// resolution (see get_all_agent_definitions).
[[nodiscard]] inline LoadAgentDefinitionsResult load_agents_dir(
    const fs::path& dir,
    std::string_view source = "custom"
) {
    return cc::tools::agent_runtime::load_agents_dir(dir, source);
}

// Convenience: only the successfully parsed definitions.
[[nodiscard]] inline std::vector<AgentDefinition> load_agents_dir_names(
    const fs::path& dir,
    std::string_view source = "custom"
) {
    return load_agents_dir(dir, source).agents;
}

// Returns the full union of active agent definitions (matching TS
// `getAgentDefinitionsWithOverrides`). Later sources shadow earlier ones; the
// precedence order (lowest to highest) is:
//   1. built-in
//   2. plugin
//   3. userSettings   (~/.claude/agents + ~/.claude/settings.json agents)
//   4. projectSettings (<cwd>/.claude/agents + <cwd>/.claude/settings.json)
//   5. localSettings   (<cwd>/.claude/settings.local.json)
//   6. flagSettings    (env CC_REPL_AGENTS_JSON / CLAUDE_CODE_AGENTS_JSON)
//   7. policySettings  (env CLAUDE_CODE_POLICY_SETTINGS)
//
// Matches TS `CLAUDE_CODE_SIMPLE` environment gate: when set the function
// returns only the built-in agent list.
[[nodiscard]] inline std::vector<AgentDefinition> get_all_agent_definitions(
    std::optional<fs::path> cwd = std::nullopt
) {
    return cc::tools::agent_runtime::get_all_agent_definitions(std::move(cwd));
}

// Filters `agents` down to those whose required MCP server patterns are all
// satisfied by the available MCP server names.
//
// Mirrors TS filterAgentsByMcpRequirements / hasRequiredMcpServers. Each
// pattern is matched case-insensitively against each available server name
// (a substring match is used, matching TS behavior: `includes`).
[[nodiscard]] inline std::vector<AgentDefinition> filter_agents_by_mcp_requirements(
    std::vector<AgentDefinition> agents,
    const std::vector<std::string>& available_servers
) {
    const auto matches = [&](std::string_view pattern) -> bool {
        if (available_servers.empty()) return false;
        std::string pattern_lower;
        pattern_lower.reserve(pattern.size());
        for (unsigned char c : pattern) pattern_lower.push_back(static_cast<char>(std::tolower(c)));
        for (const auto& server : available_servers) {
            std::string server_lower;
            server_lower.reserve(server.size());
            for (unsigned char c : server) server_lower.push_back(static_cast<char>(std::tolower(c)));
            if (server_lower.find(pattern_lower) != std::string::npos) return true;
        }
        return false;
    };

    std::vector<AgentDefinition> kept;
    kept.reserve(agents.size());
    for (auto& agent : agents) {
        if (agent.required_mcp_servers.empty()) {
            kept.push_back(std::move(agent));
            continue;
        }
        bool ok = true;
        for (const auto& pattern : agent.required_mcp_servers) {
            if (!matches(pattern)) { ok = false; break; }
        }
        if (ok) kept.push_back(std::move(agent));
    }
    return kept;
}

} // namespace cc::tools::agent_loader
