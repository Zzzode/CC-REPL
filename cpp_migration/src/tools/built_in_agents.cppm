module;
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <algorithm>

export module cc.tools.built_in_agents;

import cc.tools.agent_types;

export namespace cc::tools {

// 获取探索型代理配置
inline auto get_explore_agent() -> AgentConfig {
    return AgentConfig{
        .type = AgentType::Explore,
        .name = "Explorer",
        .model = "claude-sonnet-4-20250514",
        .system_prompt = "", // 使用 get_agent_system_prompt 获取
        .allowed_tools = {
            "Read", "Glob", "Grep", "SearchCodebase", "LS"
        },
        .max_turns = 15
    };
}

// 获取规划型代理配置
inline auto get_plan_agent() -> AgentConfig {
    return AgentConfig{
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

// 获取验证型代理配置
inline auto get_verify_agent() -> AgentConfig {
    return AgentConfig{
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

// 获取通用型代理配置
inline auto get_general_purpose_agent() -> AgentConfig {
    return AgentConfig{
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

// 获取所有内置代理配置列表
inline auto get_all_built_in_agents() -> std::vector<AgentConfig> {
    return {
        get_explore_agent(),
        get_plan_agent(),
        get_verify_agent(),
        get_general_purpose_agent()
    };
}

// 按名称查找代理配置
inline auto find_agent_by_name(std::string_view name) -> std::optional<AgentConfig> {
    auto agents = get_all_built_in_agents();
    auto it = std::find_if(agents.begin(), agents.end(),
        [&](const AgentConfig& config) {
            return config.name == name;
        });

    if (it != agents.end()) {
        return *it;
    }
    return std::nullopt;
}

} // namespace cc::tools
