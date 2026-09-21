/// @file agent_constants.cppm
/// @brief Shared constants for the Agent sub-system.
/// Migrated from src/tools/AgentTool/constants.ts
module;

#include <string>
#include <string_view>
#include <unordered_set>

export module cc.tools.agent_constants;

export namespace cc::tools::agent {

/// Official tool name used by the top-level agent delegation tool.
inline constexpr std::string_view AGENT_TOOL_NAME = "Agent";

/// Legacy wire name kept for backward compatibility with
/// persisted permission rules, hooks, and resumed sessions.
inline constexpr std::string_view LEGACY_AGENT_TOOL_NAME = "Task";

/// Agent type identifier for the verification / validation pipeline.
inline constexpr std::string_view VERIFICATION_AGENT_TYPE = "verification";

/// Built-in agents that run once and return a report — the parent never
/// calls SendMessages to continue them.  The wrapper intentionally skips
/// the agentId / SendMessage / usage trailer for these to save tokens
/// (~135 chars x millions of Explore runs/week).
[[nodiscard]] inline bool is_one_shot_builtin_agent(std::string_view agent_type) {
    static const std::unordered_set<std::string_view> oneshot{"Explore", "Plan"};
    return oneshot.contains(agent_type);
}

} // namespace cc::tools::agent
