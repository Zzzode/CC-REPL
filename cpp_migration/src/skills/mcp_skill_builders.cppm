module;
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <expected>
#include <functional>
#include <mutex>

export module cc.skills.mcp_skill_builders;

export namespace cc::skills {

// An MCP-backed skill wraps an MCP server tool as a reusable skill
struct McpSkill {
    std::string name;
    std::string server_name;
    std::string tool_name;
    std::map<std::string, std::string> default_params;
};

using McpSkillInvoker = std::function<std::expected<std::string, std::string>(
    const McpSkill&, const std::map<std::string, std::string>&)>;

namespace detail {
inline std::mutex& invoker_mutex() {
    static std::mutex mutex;
    return mutex;
}

inline McpSkillInvoker& invoker() {
    static McpSkillInvoker fn;
    return fn;
}
} // namespace detail

void set_mcp_skill_invoker(McpSkillInvoker invoker) {
    std::lock_guard lock(detail::invoker_mutex());
    detail::invoker() = std::move(invoker);
}

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
    return {};
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

    McpSkillInvoker invoker;
    {
        std::lock_guard lock(detail::invoker_mutex());
        invoker = detail::invoker();
    }
    if (!invoker) {
        return std::unexpected("MCP skill invoker is not configured");
    }
    return invoker(skill, merged_params);
}

} // namespace cc::skills
