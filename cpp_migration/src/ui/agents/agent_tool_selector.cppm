/// @file agent_tool_selector.cppm
/// @brief Tool selection UI for agent configuration
module;
#include <string>
#include <vector>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.agents.agent_tool_selector;
export namespace cc::ui::agents {
using namespace ftxui;
struct ToolOption { std::string name; std::string description; bool is_selected{false}; };
[[nodiscard]] inline Element render_tool_selector(const std::vector<ToolOption>& tools) {
    std::vector<Element> elements;
    elements.push_back(text("Select Tools") | bold);
    elements.push_back(separator());
    for (const auto& t : tools) {
        auto prefix = t.is_selected ? "[x] " : "[ ] ";
        elements.push_back(text(prefix + t.name) | (t.is_selected ? bold : nothing));
    }
    return vbox(elements) | border;
}
} // namespace
