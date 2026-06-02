module;
#include <string>
#include <string_view>
#include <map>
#include <sstream>

export module cc.tools.agent_prompt;

import cc.tools.agent_types;

export namespace cc::tools {

// 根据代理类型获取对应的系统提示词
inline auto get_agent_system_prompt(AgentType type) -> std::string {
    switch (type) {
        case AgentType::Explore:
            return R"(You are an exploration agent. Your job is to understand codebases, find relevant files, and gather context.

Guidelines:
- Use search and read tools extensively
- Summarize findings concisely
- Identify key files, patterns, and architecture
- Report dependencies and relationships between components
- Do NOT make any changes to code)";

        case AgentType::Plan:
            return R"(You are a planning agent. Your job is to create detailed implementation plans.

Guidelines:
- Break down tasks into clear, sequential steps
- Identify risks and dependencies
- Suggest testing strategies
- Consider edge cases
- Output a structured plan with numbered steps
- Do NOT implement the plan yourself)";

        case AgentType::Verify:
            return R"(You are a verification agent. Your job is to test and validate changes.

Guidelines:
- Run tests and check for failures
- Verify code compiles/builds successfully
- Check for regressions
- Validate that requirements are met
- Report any issues found with specific details
- Suggest fixes for failures when possible)";

        case AgentType::GeneralPurpose:
            return R"(You are a sub-agent working on a delegated task. Complete the task thoroughly.

Guidelines:
- Focus only on the assigned task
- Use tools effectively and minimize unnecessary operations
- Report results concisely
- If blocked, explain what's needed to proceed)";

        case AgentType::Custom:
            return ""; // 自定义代理由用户提供提示词
    }
    return "";
}

// 获取调用子代理的工具使用说明
inline auto get_agent_tool_prompt() -> std::string {
    return R"(## AgentTool

Spawn a sub-agent to handle a specific task independently. The sub-agent runs in its own context with its own conversation history.

### When to use:
- Tasks that can be parallelized (e.g., exploring multiple directories)
- Well-contained subtasks that don't need your full context
- Verification tasks after making changes

### Parameters:
- `task` (required): Clear description of what the agent should accomplish
- `agent_type` (optional): "explore", "plan", "verify", or "general" (default: "general")
- `allowed_tools` (optional): List of tools the agent can use
- `max_turns` (optional): Maximum conversation turns (default: 10)

### Tips:
- Be specific in your task description
- Provide relevant file paths and context
- Sub-agents cannot see your conversation history)";
}

// 格式化代理执行上下文
inline auto format_agent_context(
    std::string_view task,
    const std::map<std::string, std::string>& context
) -> std::string {
    std::ostringstream oss;
    oss << "## Task\n" << task << "\n\n";

    if (!context.empty()) {
        oss << "## Context\n";
        for (const auto& [key, value] : context) {
            oss << "- **" << key << "**: " << value << "\n";
        }
        oss << "\n";
    }

    return oss.str();
}

} // namespace cc::tools
