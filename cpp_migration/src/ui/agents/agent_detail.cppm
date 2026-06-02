/// @file agent_detail.cppm
/// @brief Agent detail view showing configuration and status
module;
#include <string>
#include <vector>
#include <optional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.agents.agent_detail;
export namespace cc::ui::agents {
using namespace ftxui;
struct AgentDetailInfo {
    std::string name;
    std::string model;
    std::optional<std::string> color;
    std::vector<std::string> tools;
    std::vector<std::string> permissions;
    bool is_active{false};
};
[[nodiscard]] inline Element render_agent_detail(const AgentDetailInfo& info) {
    std::vector<Element> elements;
    elements.push_back(hbox({text(info.name) | bold, info.is_active ? (text(" [active]") | color(Color::Green)) : text("")}));
    elements.push_back(text("Model: " + info.model) | dim);
    if (info.color) elements.push_back(text("Color: " + *info.color) | dim);
    if (!info.tools.empty()) {
        elements.push_back(separator());
        elements.push_back(text("Tools:") | dim);
        for (const auto& t : info.tools) elements.push_back(text("  - " + t));
    }
    return vbox(elements) | border;
}
} // namespace
