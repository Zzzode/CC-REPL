/// @file spinner_animations.cppm
/// @brief Custom spinner animation sets
module;
#include <string>
#include <array>
#include <cstdint>
#include <string_view>
#include <ftxui/dom/elements.hpp>
export module cc.ui.components.spinner_animations;
export namespace cc::ui::components {
using namespace ftxui;
inline constexpr std::array<std::string_view, 10> kDotsAnimation = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
inline constexpr std::array<std::string_view, 4> kLineAnimation = {"-","\\","|","/"};
inline constexpr std::array<std::string_view, 8> kCircleAnimation = {"◜","◠","◝","◞","◡","◟","◜","◠"};
[[nodiscard]] inline std::string_view get_spinner_frame(const auto& animation, uint32_t frame) {
    return animation[frame % animation.size()];
}
[[nodiscard]] inline Element render_spinner_with_label(std::string_view label, uint32_t frame) {
    return hbox({text(std::string(kDotsAnimation[frame % kDotsAnimation.size()])) | color(Color::Cyan), text(" " + std::string(label))});
}
} // namespace
