module;

#include <string>
#include <ftxui/dom/elements.hpp>

export module ui.components.fast_icon;

import ui.components.figures;

export namespace ui::components {

struct FastIconOptions {
    bool cooldown = false;
};

ftxui::Element FastIcon(const FastIconOptions& options = {}) {
    using namespace ftxui;
    namespace figs = ui::components::figures;
    
    if (options.cooldown) {
        return text(std::string(figs::LIGHTNING_BOLT)) | dim;
    }
    
    return text(std::string(figs::LIGHTNING_BOLT)) | color(Color::Yellow);
}

std::string GetFastIconString(bool apply_color = true, bool cooldown = false) {
    namespace figs = ui::components::figures;
    
    if (!apply_color) {
        return std::string(figs::LIGHTNING_BOLT);
    }
    
    // Note: Color application would depend on theme system
    return std::string(figs::LIGHTNING_BOLT);
}

} // namespace ui::components
