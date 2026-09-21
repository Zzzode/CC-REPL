/// @file context_visualization.cppm
/// @brief Context window usage visualization
module;
#include <string>
#include <cstdint>
#include <format>
#include <ftxui/dom/elements.hpp>
export module cc.ui.components.context_visualization;
export namespace cc::ui::components {
using namespace ftxui;
struct ContextUsage { uint64_t used_tokens{0}; uint64_t max_tokens{200000}; uint64_t cache_tokens{0}; };
[[nodiscard]] inline Element render_context_bar(const ContextUsage& usage) {
    double pct = static_cast<double>(usage.used_tokens) / static_cast<double>(usage.max_tokens);
    auto bar_color = pct > 0.9 ? Color::Red : pct > 0.7 ? Color::Yellow : Color::Green;
    return hbox({
        text(std::format("Context: {}/{}k", usage.used_tokens / 1000, usage.max_tokens / 1000)) | dim,
        text(" "),
        gauge(pct) | color(bar_color) | size(WIDTH, EQUAL, 20),
    });
}
} // namespace
