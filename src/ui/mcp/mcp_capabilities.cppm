/// @file mcp_capabilities.cppm
/// @brief MCP server capabilities display
module;
#include <string>
#include <vector>
#include <ftxui/dom/elements.hpp>
export module cc.ui.mcp.mcp_capabilities;
export namespace cc::ui::mcp {
using namespace ftxui;
struct McpCapabilities { bool tools{false}; bool resources{false}; bool prompts{false}; bool logging{false}; std::vector<std::string> tool_names; };
[[nodiscard]] inline Element render_capabilities(const std::string& server_name, const McpCapabilities& caps) {
    std::vector<Element> elements;
    elements.push_back(text(server_name + " capabilities:") | bold);
    auto check = [](bool v) { return v ? "✓" : "✗"; };
    elements.push_back(text(std::string("  Tools: ") + check(caps.tools)));
    elements.push_back(text(std::string("  Resources: ") + check(caps.resources)));
    elements.push_back(text(std::string("  Prompts: ") + check(caps.prompts)));
    elements.push_back(text(std::string("  Logging: ") + check(caps.logging)));
    return vbox(elements);
}
} // namespace
