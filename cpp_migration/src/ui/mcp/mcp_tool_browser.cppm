/// @file mcp_tool_browser.cppm
/// @brief MCP tool browser UI for discovering available tools
module;
#include <string>
#include <vector>
#include <optional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.mcp.mcp_tool_browser;
export namespace cc::ui::mcp {
using namespace ftxui;
struct McpToolInfo { std::string name; std::string description; std::string server_name; bool is_enabled{true}; };
[[nodiscard]] inline Element render_mcp_tool_browser(const std::vector<McpToolInfo>& tools, std::size_t selected) {
    std::vector<Element> elements;
    elements.push_back(text("MCP Tools") | bold);
    elements.push_back(separator());
    for (std::size_t i = 0; i < tools.size(); ++i) {
        auto style = i == selected ? inverted : nothing;
        auto enabled = tools[i].is_enabled ? "✓" : "✗";
        elements.push_back(hbox({text(enabled) | (tools[i].is_enabled ? color(Color::Green) : color(Color::Red)), text(" " + tools[i].name + " (" + tools[i].server_name + ")")}) | style);
    }
    return vbox(elements) | border;
}
} // namespace
