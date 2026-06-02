// ToolRegistry - Re-exports core ToolRegistry and provides tool list factory
module;

#include <string>
#include <vector>

export module cc.tools.registry;

import cc.tools.tool;

export namespace cc::tools::registry {

// Re-export core ToolRegistry for convenience
using cc::core::ToolRegistry;
using cc::core::ToolDefinition;
using cc::core::ToolInput;
using cc::core::ToolResult;
using cc::core::ITool;

/// Get the list of all built-in tool names
[[nodiscard]] inline std::vector<std::string> builtin_tool_names() {
    return {"Bash", "Read", "Write", "Edit", "Glob", "Grep", "WebFetch", "WebSearch", "Agent"};
}

} // namespace cc::tools::registry
