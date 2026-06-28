/// @file at_attachments_impl.cpp
/// @brief impl unit for cc.ui.prompt.at_attachments. Holds the heavy
/// cc.tools.agent_runtime + cc.tools.mcp imports (AT-10 agent-mention and
/// AT-11 MCP-resource attachment) OUT of the interface module's BMI, so that
/// app.cppm (which imports this module) doesn't transitively pull them in and
/// blow clang's 2GB source-location budget in importers like tests/test_ui.cpp.
module;

#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>

module cc.ui.prompt.at_attachments;

import cc.types.types;
import cc.tools.agent_runtime;
import cc.tools.mcp;

namespace cc::ui::prompt::at_attachments {

namespace core = cc::core;
namespace agent_runtime = cc::tools::agent_runtime;
namespace mcp = cc::tools;
namespace fs = std::filesystem;

// AT-10: agent mention — load agent definitions, attach when_to_use on match.
std::optional<core::ContentBlock> try_attach_agent(
    std::string_view name, const fs::path& cwd) {
    for (const auto& agent : agent_runtime::get_all_agent_definitions(cwd)) {
        if (name == agent.agent_type) {
            return core::ContentBlock{core::TextBlock{
                std::format("The user mentioned the agent @{}:\n{}",
                            agent.agent_type, agent.when_to_use)}};
        }
    }
    return std::nullopt;
}

// AT-11: MCP resource — pre-validate against listed resources (avoid blocking
// submit on an unknown/slow server), then read_resource.
std::optional<core::ContentBlock> try_attach_mcp_resource(
    std::string_view server, std::string_view uri) {
    const std::string sname(server);
    const std::string suri(uri);
    auto listed = mcp::list_native_mcp_resources(std::nullopt);
    if (!listed) return std::nullopt;
    bool known = false;
    for (const auto& r : *listed) {
        if (r.server_name == sname && r.uri == suri) { known = true; break; }
    }
    if (!known) return std::nullopt;
    auto read = mcp::read_native_mcp_resource(sname, suri);
    if (!read) return std::nullopt;
    return core::ContentBlock{core::TextBlock{
        std::format("The user attached MCP resource @{}:{}:\n{}",
                    sname, suri, read->content)}};
}

}  // namespace cc::ui::prompt::at_attachments
