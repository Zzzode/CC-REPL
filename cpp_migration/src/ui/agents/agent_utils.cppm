/// @file agent_utils.cppm
/// @brief Shared utilities for agent UI components
module;
#include <string>
#include <string_view>
#include <optional>
#include <ftxui/dom/elements.hpp>
export module cc.ui.agents.agent_utils;
export namespace cc::ui::agents {
using namespace ftxui;
[[nodiscard]] inline Color agent_color_from_string(std::string_view color_name) {
    if (color_name == "red") return Color::Red;
    if (color_name == "green") return Color::Green;
    if (color_name == "blue") return Color::Blue;
    if (color_name == "yellow") return Color::Yellow;
    if (color_name == "magenta") return Color::Magenta;
    if (color_name == "cyan") return Color::Cyan;
    return Color::White;
}
[[nodiscard]] inline Element render_agent_badge(std::string_view name, std::optional<std::string_view> color_name = std::nullopt) {
    auto c = color_name ? agent_color_from_string(*color_name) : Color::White;
    return text(std::string(name)) | bold | color(c);
}
} // namespace
