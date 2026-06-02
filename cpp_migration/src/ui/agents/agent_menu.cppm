/// @file agent_menu.cppm
/// @brief Agent selection menu component
module;
#include <string>
#include <vector>
#include <functional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.agents.agent_menu;
export namespace cc::ui::agents {
using namespace ftxui;
struct AgentMenuEntry { std::string name; std::string model; bool is_active{false}; };
[[nodiscard]] inline Component agent_menu(const std::vector<AgentMenuEntry>& entries, std::function<void(std::size_t)> on_select) {
    return Renderer([entries, on_select = std::move(on_select)] {
        std::vector<Element> elements;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            auto& e = entries[i];
            auto style = e.is_active ? bold : nothing;
            elements.push_back(text(e.name + " (" + e.model + ")") | style);
        }
        return vbox(elements) | border;
    });
}
} // namespace
