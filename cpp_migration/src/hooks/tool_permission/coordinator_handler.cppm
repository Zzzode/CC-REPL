module;
#include <map>
#include <string>
#include <string_view>
#include <vector>

export module cc.hooks.tool_permission.coordinator_handler;

export namespace cc::hooks::tool_permission {


enum class PermissionDecision {
    Allow,
    Deny,
    Ask
};

inline bool is_coordinator_tool(std::string_view tool_name);


inline PermissionDecision handle_coordinator_permission(
    std::string_view tool_name,
    std::map<std::string, std::string> params
) {

    if (is_coordinator_tool(tool_name)) {
        return PermissionDecision::Allow;
    }
    (void)params;
    return PermissionDecision::Ask;
}


inline std::vector<std::string> auto_approve_coordinator_tools() {
    return {
        "AgentTool",
        "TeamCreateTool",
        "TaskCreateTool",
        "TaskUpdateTool"
    };
}


inline bool is_coordinator_tool(std::string_view tool_name) {
    auto approved = auto_approve_coordinator_tools();
    for (const auto& t : approved) {
        if (t == tool_name) return true;
    }
    return false;
}

} // namespace cc::hooks::tool_permission
