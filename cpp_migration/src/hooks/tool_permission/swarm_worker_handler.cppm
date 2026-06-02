module;
#include <string>
#include <string_view>
#include <vector>

export module cc.hooks.tool_permission.swarm_worker_handler;

export namespace cc::hooks::tool_permission {

// 权限决策枚举
enum class PermissionDecision {
    Allow,
    Deny,
    Ask
};

inline bool is_swarm_safe_tool(std::string_view tool_name);

// 处理 Swarm Worker 的工具权限请求
inline PermissionDecision handle_swarm_permission(
    std::string_view tool_name,
    std::string_view worker_id
) {
    // Swarm worker 只允许使用安全的只读工具
    if (is_swarm_safe_tool(tool_name)) {
        return PermissionDecision::Allow;
    }
    (void)worker_id;
    return PermissionDecision::Deny;
}

// 获取 Swarm Worker 被允许使用的工具列表
inline std::vector<std::string> get_swarm_allowed_tools() {
    return {
        "FileReadTool",
        "GlobTool",
        "GrepTool",
        "SearchCodebase",
        "WebFetchTool",
        "WebSearchTool"
    };
}

// 判断工具是否对 Swarm Worker 安全（只读、无副作用）
inline bool is_swarm_safe_tool(std::string_view tool_name) {
    auto allowed = get_swarm_allowed_tools();
    for (const auto& t : allowed) {
        if (t == tool_name) return true;
    }
    return false;
}

} // namespace cc::hooks::tool_permission
