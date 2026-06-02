module;
#include <map>
#include <string>
#include <string_view>
#include <vector>

export module cc.hooks.tool_permission.coordinator_handler;

export namespace cc::hooks::tool_permission {

// 权限决策枚举（通用于所有权限处理器）
enum class PermissionDecision {
    Allow,
    Deny,
    Ask
};

inline bool is_coordinator_tool(std::string_view tool_name);

// 处理协调器工具的权限请求
inline PermissionDecision handle_coordinator_permission(
    std::string_view tool_name,
    std::map<std::string, std::string> params
) {
    // 协调器工具默认自动批准，因为它们是内部编排工具
    if (is_coordinator_tool(tool_name)) {
        return PermissionDecision::Allow;
    }
    (void)params;
    return PermissionDecision::Ask;
}

// 获取自动批准的协调器工具列表
inline std::vector<std::string> auto_approve_coordinator_tools() {
    return {
        "AgentTool",
        "TeamCreateTool",
        "TaskCreateTool",
        "TaskUpdateTool"
    };
}

// 判断给定工具是否属于协调器工具
inline bool is_coordinator_tool(std::string_view tool_name) {
    auto approved = auto_approve_coordinator_tools();
    for (const auto& t : approved) {
        if (t == tool_name) return true;
    }
    return false;
}

} // namespace cc::hooks::tool_permission
