module;
#include <string>
#include <string_view>
#include <map>
#include <sstream>

export module cc.tools.tool_prompts;

export namespace cc::tools {

// 根据工具名获取对应的提示词
inline auto get_tool_prompt(std::string_view tool_name) -> std::string {
    static const std::map<std::string, std::string, std::less<>> prompts = {
        {"Read", R"(Read file contents from the filesystem. Provide an absolute path.)"},
        {"Write", R"(Write content to a file. Overwrites existing content. Read the file first if it exists.)"},
        {"Edit", R"(Perform exact string replacement in a file. old_string must be unique in the file.)"},
        {"Glob", R"(Find files matching a glob pattern. Returns paths sorted by modification time.)"},
        {"Grep", R"(Search file contents with regex. Supports context lines and file type filters.)"},
        {"RunCommand", R"(Execute a terminal command. Use for git, build, test, and other shell operations.)"},
        {"SearchCodebase", R"(Semantic code search. Find code by meaning, not exact text.)"},
        {"WebSearch", R"(Search the web for real-time information.)"},
        {"WebFetch", R"(Fetch and parse content from a URL.)"},
        {"Agent", R"(Spawn a sub-agent for independent task execution.)"},
        {"TaskCreate", R"(Create a new tracked task.)"},
        {"TaskUpdate", R"(Update status or details of an existing task.)"},
    };

    auto it = prompts.find(tool_name);
    if (it != prompts.end()) {
        return it->second;
    }
    return std::string("No prompt available for tool: ") + std::string(tool_name);
}

// 获取所有工具的提示词映射
inline auto get_all_tool_prompts() -> std::map<std::string, std::string> {
    return {
        {"Read", get_tool_prompt("Read")},
        {"Write", get_tool_prompt("Write")},
        {"Edit", get_tool_prompt("Edit")},
        {"Glob", get_tool_prompt("Glob")},
        {"Grep", get_tool_prompt("Grep")},
        {"RunCommand", get_tool_prompt("RunCommand")},
        {"SearchCodebase", get_tool_prompt("SearchCodebase")},
        {"WebSearch", get_tool_prompt("WebSearch")},
        {"WebFetch", get_tool_prompt("WebFetch")},
        {"Agent", get_tool_prompt("Agent")},
        {"TaskCreate", get_tool_prompt("TaskCreate")},
        {"TaskUpdate", get_tool_prompt("TaskUpdate")},
    };
}

// 格式化工具调用结果（用于回传给 LLM）
inline auto format_tool_use_result(
    std::string_view tool_name,
    std::string_view result,
    bool is_error
) -> std::string {
    std::ostringstream oss;

    if (is_error) {
        oss << "Tool `" << tool_name << "` returned an error:\n";
        oss << result;
    } else {
        oss << result;
    }

    // 对过长的结果进行截断提示
    if (result.size() > 50000) {
        oss << "\n\n[Note: Output was truncated. "
            << result.size() << " total characters.]";
    }

    return oss.str();
}

// 获取工具的简短描述
inline auto get_tool_description(std::string_view tool_name) -> std::string {
    static const std::map<std::string, std::string, std::less<>> descriptions = {
        {"Read", "Reads file contents from the local filesystem"},
        {"Write", "Writes content to a file on the local filesystem"},
        {"Edit", "Performs exact string replacements in files"},
        {"Glob", "Fast file pattern matching with glob syntax"},
        {"Grep", "Searches file contents with regular expressions"},
        {"RunCommand", "Executes terminal commands"},
        {"SearchCodebase", "Semantic search over the codebase"},
        {"WebSearch", "Searches the web for information"},
        {"WebFetch", "Fetches content from a URL"},
        {"Agent", "Spawns a sub-agent for delegated tasks"},
        {"TaskCreate", "Creates a new tracked task"},
        {"TaskUpdate", "Updates an existing task"},
        {"LS", "Lists directory contents"},
        {"DeleteFile", "Deletes files from the filesystem"},
    };

    auto it = descriptions.find(tool_name);
    if (it != descriptions.end()) {
        return it->second;
    }
    return std::string("Tool: ") + std::string(tool_name);
}

} // namespace cc::tools
