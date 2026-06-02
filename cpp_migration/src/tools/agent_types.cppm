module;
#include <string>
#include <string_view>
#include <vector>
#include <optional>

export module cc.tools.agent_types;

export namespace cc::tools {

// 内置子代理类型
enum class AgentType {
    Explore,        // 探索型：用于代码库搜索和理解
    Plan,           // 规划型：用于制定实施方案
    Verify,         // 验证型：用于测试和验证结果
    GeneralPurpose, // 通用型：适合大多数任务
    Custom          // 自定义：用户配置的代理
};

// 代理配置
struct AgentConfig {
    AgentType type;                        // 代理类型
    std::string name;                      // 代理名称
    std::string model;                     // 使用的模型
    std::string system_prompt;             // 系统提示词
    std::vector<std::string> allowed_tools; // 允许使用的工具列表
    std::optional<int> max_turns;          // 最大对话轮次限制
};

// 代理执行结果
struct AgentResult {
    std::string output;    // 代理的输出内容
    int turns_used;        // 实际使用的对话轮次
    int tokens_used;       // 消耗的 token 数
    bool completed;        // 是否正常完成（未超时/未截断）
};

// 将 AgentType 转为可读字符串
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

// 从字符串解析 AgentType
inline auto parse_agent_type(std::string_view str) -> std::optional<AgentType> {
    if (str == "explore")  return AgentType::Explore;
    if (str == "plan")     return AgentType::Plan;
    if (str == "verify")   return AgentType::Verify;
    if (str == "general")  return AgentType::GeneralPurpose;
    if (str == "custom")   return AgentType::Custom;
    return std::nullopt;
}

} // namespace cc::tools
