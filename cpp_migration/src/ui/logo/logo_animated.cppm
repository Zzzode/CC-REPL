/// @file logo_animated.cppm
/// @brief Animated ASCII logo for startup
module;
#include <string>
#include <vector>
#include <cstdint>
#include <ftxui/dom/elements.hpp>
export module cc.ui.logo.logo_animated;
export namespace cc::ui::logo {
using namespace ftxui;
[[nodiscard]] inline Element render_animated_logo(uint32_t frame) {
    static const std::vector<std::string> logo_lines = {
        R"(   _____ _                 _       )",
        R"(  / ____| |               | |      )",
        R"( | |    | | __ _ _   _  __| | ___  )",
        R"( | |    | |/ _` | | | |/ _` |/ _ \ )",
        R"( | |____| | (_| | |_| | (_| |  __/ )",
        R"(  \_____|_|\__,_|\__,_|\__,_|\___| )",
    };
    std::vector<Element> elements;
    for (std::size_t i = 0; i < logo_lines.size(); ++i) {
        auto visible_chars = std::min(logo_lines[i].size(), static_cast<std::size_t>(frame * 2 + i * 3));
        elements.push_back(text(logo_lines[i].substr(0, visible_chars)) | color(Color::Magenta));
    }
    return vbox(elements);
}
} // namespace
