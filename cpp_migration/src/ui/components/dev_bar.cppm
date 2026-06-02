module;

#include <string>
#include <vector>
#include <ftxui/dom/elements.hpp>

export module ui.components.dev_bar;

export namespace ui::components {

struct SlowOperation {
    std::string operation;
    long duration_ms;
    long long timestamp;
};

struct DevBarOptions {
    bool show_dev_bar = true;
    bool is_debug_build = true;
    std::vector<SlowOperation> slow_operations;
};

namespace detail {

bool ShouldShowDevBar(bool is_debug_build) {
    return is_debug_build;
}

} // namespace detail

ftxui::Element DevBar(const DevBarOptions& options = {}) {
    using namespace ftxui;
    
    if (!detail::ShouldShowDevBar(options.show_dev_bar) || options.slow_operations.empty()) {
        return emptyElement();
    }
    
    std::vector<Element> elements;
    elements.reserve(options.slow_operations.size() * 2 - 1);
    
    int count = 0;
    for (const auto& op : options.slow_operations) {
        if (count > 0) {
            elements.push_back(text(" · ") | dim);
        }
        
        auto op_text = op.operation + " (" + std::to_string(op.duration_ms) + "ms)";
        elements.push_back(text(op_text));
        
        if (++count >= 3) {
            break;
        }
    }
    
    return hbox({
        text("[DEV] slow sync: ") | color(Color::Yellow) | bold,
        hbox(elements) | color(Color::Yellow)
    }) | borderRounded | color(Color::Yellow);
}

} // namespace ui::components
