module;
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <algorithm>

export module cc.tools.built_in_agents;

import cc.tools.agent_types;

export namespace cc::tools {


inline auto get_explore_agent() -> MultiAgentAgentConfig {
    return MultiAgentAgentConfig{
        .type = AgentType::Explore,
        .name = "Explorer",
        .model = "claude-sonnet-4-20250514",
        .system_prompt = "",
        .allowed_tools = {
            "Read", "Glob", "Grep", "SearchCodebase", "LS"
        },
        .max_turns = 15
    };
}


inline auto get_plan_agent() -> MultiAgentAgentConfig {
    return MultiAgentAgentConfig{
        .type = AgentType::Plan,
        .name = "Planner",
        .model = "claude-sonnet-4-20250514",
        .system_prompt = "",
        .allowed_tools = {
            "Read", "Glob", "Grep", "SearchCodebase", "LS", "WebSearch"
        },
        .max_turns = 10
    };
}


inline auto get_verify_agent() -> MultiAgentAgentConfig {
    return MultiAgentAgentConfig{
        .type = AgentType::Verify,
        .name = "Verifier",
        .model = "claude-sonnet-4-20250514",
        .system_prompt = "",
        .allowed_tools = {
            "Read", "Glob", "Grep", "RunCommand", "SearchCodebase"
        },
        .max_turns = 20
    };
}


inline auto get_general_purpose_agent() -> MultiAgentAgentConfig {
    return MultiAgentAgentConfig{
        .type = AgentType::GeneralPurpose,
        .name = "Assistant",
        .model = "claude-sonnet-4-20250514",
        .system_prompt = "",
        .allowed_tools = {
            "Read", "Write", "Edit", "Glob", "Grep",
            "SearchCodebase", "RunCommand", "LS", "WebSearch", "WebFetch"
        },
        .max_turns = 25
    };
}


inline auto get_all_built_in_agents() -> std::vector<MultiAgentAgentConfig> {
    return {
        get_explore_agent(),
        get_plan_agent(),
        get_verify_agent(),
        get_general_purpose_agent()
    };
}


inline auto find_agent_by_name(std::string_view name) -> std::optional<MultiAgentAgentConfig> {
    auto agents = get_all_built_in_agents();
    auto it = std::find_if(agents.begin(), agents.end(),
        [&](const MultiAgentAgentConfig& config) {
            return config.name == name;
        });

    if (it != agents.end()) {
        return *it;
    }
    return std::nullopt;
}

} // namespace cc::tools
