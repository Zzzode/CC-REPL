module;
#include <string>
#include <string_view>
#include <vector>

export module cc.hooks.tool_permission.swarm_worker_handler;

export namespace cc::hooks::tool_permission {


enum class PermissionDecision {
    Allow,
    Deny,
    Ask
};

inline bool is_swarm_safe_tool(std::string_view tool_name);


inline PermissionDecision handle_swarm_permission(
    std::string_view tool_name,
    std::string_view worker_id
) {

    if (is_swarm_safe_tool(tool_name)) {
        return PermissionDecision::Allow;
    }
    (void)worker_id;
    return PermissionDecision::Deny;
}


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


inline bool is_swarm_safe_tool(std::string_view tool_name) {
    auto allowed = get_swarm_allowed_tools();
    for (const auto& t : allowed) {
        if (t == tool_name) return true;
    }
    return false;
}

} // namespace cc::hooks::tool_permission
