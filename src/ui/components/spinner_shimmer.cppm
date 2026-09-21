/// @file spinner_shimmer.cppm
/// @brief Shimmer animation for loading states
module;
#include <string>
#include <cstdint>
#include <ftxui/dom/elements.hpp>
export module cc.ui.components.spinner_shimmer;
export namespace cc::ui::components {
using namespace ftxui;
[[nodiscard]] inline Element render_shimmer(std::string_view text_content, uint32_t frame) {
    auto offset = frame % 20;
    std::string display(text_content);
    if (offset < display.size()) display[offset] = '|';
    return text(display) | dim;
}
[[nodiscard]] inline Element render_thinking_shimmer(uint32_t frame) {
    static constexpr std::array<std::string_view, 4> frames = {"⠋", "⠙", "⠹", "⠸"};
    return text(std::string(frames[frame % frames.size()])) | color(Color::Cyan);
}
} // namespace
