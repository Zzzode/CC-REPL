module;
#include <string>
#include <string_view>
#include <vector>
#include <optional>

export module cc.tools.agent_types;

export namespace cc::tools {


enum class AgentType {
    Explore,
    Plan,
    Verify,
    GeneralPurpose,
    Custom
};


struct MultiAgentAgentConfig {
    AgentType type;
    std::string name;
    std::string model;
    std::string system_prompt;
    std::vector<std::string> allowed_tools;
    std::optional<int> max_turns;
};


struct MultiAgentResult {
    std::string output;
    int turns_used;
    int tokens_used;
    bool completed;
};


inline auto agent_type_to_string(AgentType type) -> std::string_view {
    switch (type) {
        case AgentType::Explore:        return "explore";
        case AgentType::Plan:           return "plan";
        case AgentType::Verify:         return "verify";
        case AgentType::GeneralPurpose: return "general";
        case AgentType::Custom:         return "custom";
    }
    return "unknown";
}


inline auto parse_agent_type(std::string_view str) -> std::optional<AgentType> {
    if (str == "explore")  return AgentType::Explore;
    if (str == "plan")     return AgentType::Plan;
    if (str == "verify")   return AgentType::Verify;
    if (str == "general")  return AgentType::GeneralPurpose;
    if (str == "custom")   return AgentType::Custom;
    return std::nullopt;
}

} // namespace cc::tools
