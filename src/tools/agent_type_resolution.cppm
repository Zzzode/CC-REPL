// Agent type resolution facade — thin, re-exporting wrapper around
// cc.tools.agent_runtime's resolve_requested_agent_type / resolve_agent_type.
//
// Exists as a standalone module so that other consumers (UI, slash commands,
// tests) can depend on "agent type resolution" without pulling in the full
// runtime bookkeeping (store, transcripts, sidechain persistence) that lives
// in the agent_runtime module.
//
// Mirrors TypeScript: src/tools/AgentTool/agentTypeResolution.ts
//   - resolveRequestedAgentType  →  resolve_requested_agent_type
//   - plus a std::expected wrapper resolve_agent_type with ResolutionError.
//
// All five test cases from agentTypeResolution.test.ts are annotated inside
// resolve_requested_agent_type (in agent_runtime.cppm) with `Test case:` and
// `migrated edge case:` markers; see the canonical impl for details.
module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>
#include <filesystem>

export module cc.tools.agent_type_resolution;

import cc.tools.agent_runtime;

export namespace cc::tools::agent_type_resolution {

using cc::tools::agent_runtime::AgentDefinition;
using cc::tools::agent_runtime::ResolutionError;
using cc::tools::agent_runtime::resolution_error_name;
using cc::tools::agent_runtime::canonicalize_agent_type;
using cc::tools::agent_runtime::agent_alias_candidates;

// Lightweight alias: same as agent_runtime's resolve_requested_agent_type.
// Returns the agent's *canonical* agent_type string as it appears in the
// AgentDefinition list, or std::nullopt when no unambiguous match exists.
[[nodiscard]] inline std::optional<std::string> resolve_requested_agent_type(
    std::string_view requested_type,
    const std::vector<AgentDefinition>& agents
) {
    return cc::tools::agent_runtime::resolve_requested_agent_type(requested_type, agents);
}

// Full std::expected API: returns either the matched AgentDefinition, or a
// ResolutionError code explaining *why* resolution failed (useful for
// surfacing diagnostics in UI or CLI error output).
[[nodiscard]] inline std::expected<AgentDefinition, ResolutionError> resolve_agent_type(
    std::string_view id,
    const std::vector<AgentDefinition>& agents
) {
    return cc::tools::agent_runtime::resolve_agent_type(id, agents);
}

// Convenience overload: resolve against all globally-registered agent
// definitions (built-ins + plugins + user + project + policy + flag),
// optionally scoped to a specific working directory.
[[nodiscard]] inline std::expected<AgentDefinition, ResolutionError> resolve_agent_type(
    std::string_view id,
    const std::optional<std::filesystem::path>& cwd = std::nullopt
) {
    auto agents = cc::tools::agent_runtime::get_all_agent_definitions(cwd);
    return cc::tools::agent_runtime::resolve_agent_type(id, agents);
}

} // namespace cc::tools::agent_type_resolution
