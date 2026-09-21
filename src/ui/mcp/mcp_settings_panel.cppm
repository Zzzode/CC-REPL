/// @file mcp_settings_panel.cppm
/// @brief MCP server settings configuration panel
module;
#include <string>
#include <vector>
#include <optional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.mcp.mcp_settings_panel;
export namespace cc::ui::mcp {
using namespace ftxui;
struct McpServerConfig { std::string name; std::string command; std::vector<std::string> args; bool is_connected{false}; };
[[nodiscard]] inline Element render_mcp_settings(const std::vector<McpServerConfig>& servers) {
    std::vector<Element> elements;
    elements.push_back(text("MCP Servers") | bold);
    elements.push_back(separator());
    for (const auto& s : servers) {
        auto status_color = s.is_connected ? Color::Green : Color::Red;
        elements.push_back(hbox({text(s.name) | bold, text(s.is_connected ? " [connected]" : " [disconnected]") | color(status_color)}));
        elements.push_back(text("  " + s.command) | dim);
    }
    return vbox(elements) | border;
}
} // namespace
