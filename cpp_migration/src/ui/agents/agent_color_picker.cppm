/// @file agent_color_picker.cppm
/// @brief Color picker for agent customization
module;
#include <string>
#include <vector>
#include <array>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.agents.agent_color_picker;
export namespace cc::ui::agents {
using namespace ftxui;
inline constexpr std::array<std::string_view, 8> kAgentColors = {"red", "green", "blue", "yellow", "magenta", "cyan", "orange", "purple"};
[[nodiscard]] inline Element render_color_picker(std::size_t selected_index) {
    std::vector<Element> elements;
    for (std::size_t i = 0; i < kAgentColors.size(); ++i) {
        auto style = i == selected_index ? inverted : nothing;
        elements.push_back(text(std::string(kAgentColors[i])) | style);
    }
    return hbox(elements) | border;
}
} // namespace
