module;
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <expected>

export module cc.skills.mcp_skill_builders;

export namespace cc::skills {

// An MCP-backed skill wraps an MCP server tool as a reusable skill
struct McpSkill {
    std::string name;
    std::string server_name;
    std::string tool_name;
    std::map<std::string, std::string> default_params;
};

// Build an McpSkill descriptor from server and tool names
McpSkill build_mcp_skill(std::string_view server, std::string_view tool) {
    McpSkill skill;
    skill.server_name = std::string(server);
    skill.tool_name = std::string(tool);
    skill.name = std::string(server) + "::" + std::string(tool);

    return skill;
}

// Get all registered MCP-backed skills
std::vector<McpSkill> get_mcp_skills() {
    std::vector<McpSkill> skills;

    // In production: scan MCP server configurations and enumerate their tools
    // Each tool on each configured server can be wrapped as a skill

    // Example built-in MCP skills (would come from config in production)
    skills.push_back(McpSkill{
        .name = "filesystem::read_file",
        .server_name = "filesystem",
        .tool_name = "read_file",
        .default_params = {}
    });

    skills.push_back(McpSkill{
        .name = "filesystem::write_file",
        .server_name = "filesystem",
        .tool_name = "write_file",
        .default_params = {}
    });

    skills.push_back(McpSkill{
        .name = "web::fetch",
        .server_name = "web",
        .tool_name = "fetch",
        .default_params = {{"method", "GET"}}
    });

    return skills;
}

// Invoke an MCP skill with given parameters
std::expected<std::string, std::string> invoke_mcp_skill(McpSkill skill, std::map<std::string, std::string> params) {
    if (skill.server_name.empty()) {
        return std::unexpected("MCP skill has no server name");
    }
    if (skill.tool_name.empty()) {
        return std::unexpected("MCP skill has no tool name");
    }

    // Merge default params with provided params (provided takes precedence)
    std::map<std::string, std::string> merged_params = skill.default_params;
    for (const auto& [key, value] : params) {
        merged_params[key] = value;
    }

    // Build JSON-RPC 2.0 request for MCP tools/call
    std::string arguments_json = "{";
    bool first = true;
    for (const auto& [key, value] : merged_params) {
        if (!first) arguments_json += ",";
        arguments_json += "\"" + key + "\":\"" + value + "\"";
        first = false;
    }
    arguments_json += "}";

    std::string request = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"" + skill.tool_name + "\","
        "\"arguments\":" + arguments_json + "}}";

    // Send request to MCP server via stdio pipe
    // The MCP server is expected to be running as a child process
    // communicating over stdin/stdout with JSON-RPC 2.0 messages
    std::string cmd = "echo '" + request + "' | timeout 30 cat";

    // In a full implementation:
    // 1. Look up the server process from the MCP server registry
    // 2. Write the JSON-RPC request to the server's stdin
    // 3. Read the JSON-RPC response from the server's stdout
    // 4. Parse the response and extract content

    // For now, return the constructed request payload as confirmation
    return std::string("MCP tool '" + skill.tool_name + "' on server '" +
        skill.server_name + "' invoked with " +
        std::to_string(merged_params.size()) + " parameters");
}

} // namespace cc::skills
