/// @file fullscreen_layout.cppm
/// @brief Fullscreen terminal layout manager
module;
#include <string>
#include <functional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.components.fullscreen_layout;
export namespace cc::ui::components {
using namespace ftxui;
struct LayoutRegions { Element header; Element body; Element footer; Element sidebar; bool show_sidebar{false}; };
[[nodiscard]] inline Element render_fullscreen_layout(const LayoutRegions& regions) {
    auto main_content = vbox({regions.header, separator(), regions.body | flex, separator(), regions.footer});
    if (regions.show_sidebar) return hbox({regions.sidebar | size(WIDTH, EQUAL, 30), separator(), main_content | flex});
    return main_content;
}
} // namespace
