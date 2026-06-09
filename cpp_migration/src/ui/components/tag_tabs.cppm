module;

#include <string>
#include <vector>
#include <functional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.components.tag_tabs;

export namespace cc::ui::components {

struct Tab {
    std::string label;
    std::string id;
    bool is_all_tab = false;
};

struct TagTabsOptions {
    std::vector<Tab> tabs;
    int active_tab = 0;
    int available_width = 80;
    bool show_resume_label = true;
    std::function<void(int)> on_tab_change;
};

ftxui::Element TagTabs(const TagTabsOptions& options) {
    using namespace ftxui;
    
    std::vector<Element> elements;
    
    // Resume label
    if (options.show_resume_label) {
        elements.push_back(text("Resume") | color(Color::CyanLight) | dim);
        elements.push_back(text("  "));
    }
    
    // Calculate which tabs to show
    int start_idx = 0;
    int end_idx = static_cast<int>(options.tabs.size());
    bool show_left_arrow = false;
    bool show_right_arrow = false;
    
    // Simple overflow handling
    if (options.tabs.size() > 5) {
        // Center around active tab
        start_idx = std::max(0, options.active_tab - 2);
        end_idx = std::min(start_idx + 5, static_cast<int>(options.tabs.size()));
        show_left_arrow = start_idx > 0;
        show_right_arrow = end_idx < static_cast<int>(options.tabs.size());
    }
    
    // Left arrow
    if (show_left_arrow) {
        elements.push_back(text("← " + std::to_string(start_idx)) | dim);
        elements.push_back(text("  "));
    }
    
    // Visible tabs
    for (int i = start_idx; i < end_idx; ++i) {
        std::string display_text;
        if (options.tabs[i].is_all_tab) {
            display_text = options.tabs[i].label;
        } else {
            display_text = "#" + options.tabs[i].label;
        }
        
        auto label = text(display_text);
        if (i == options.active_tab) {
            label = label | bold | color(Color::White) | bgcolor(Color::Cyan) | inverted;
        } else {
            label = label | dim;
        }
        elements.push_back(hbox({text(" "), label, text(" ")}));
        if (i < end_idx - 1) {
            elements.push_back(text(" "));
        }
    }
    
    // Right arrow
    if (show_right_arrow) {
        int hidden = static_cast<int>(options.tabs.size()) - end_idx;
        elements.push_back(text("  →" + std::to_string(hidden)) | dim);
    }
    
    // Hint
    elements.push_back(filler());
    elements.push_back(text(" (tab to cycle)") | dim);
    
    return hbox(elements) | borderRounded;
}

ftxui::Component TagTabsComponent(const TagTabsOptions& options) {
    using namespace ftxui;
    
    auto state = std::make_shared<int>(options.active_tab);
    auto tabs = options.tabs;
    auto on_change = options.on_tab_change;
    
    auto renderer = Renderer([tabs, state] {
        std::vector<Element> tab_elements;
        
        for (size_t i = 0; i < tabs.size(); ++i) {
            std::string display_text = tabs[i].is_all_tab ? tabs[i].label : "#" + tabs[i].label;
            auto label = text(display_text);
            
            if (static_cast<int>(i) == *state) {
                label = label | bold | color(Color::White) | bgcolor(Color::Cyan) | inverted;
            }
            
            tab_elements.push_back(hbox({text(" "), label, text(" ")}) | borderEmpty);
            if (i < tabs.size() - 1) {
                tab_elements.push_back(text(" "));
            }
        }
        
        return hbox(tab_elements);
    });
    
    auto container = Container::Horizontal({
        renderer | CatchEvent([tabs, state, on_change](Event e) {
            if (e == Event::ArrowRight || e == Event::Tab) {
                *state = (*state + 1) % static_cast<int>(tabs.size());
                if (on_change) on_change(*state);
                return true;
            }
            if (e == Event::ArrowLeft || e == Event::TabReverse) {
                *state = (*state - 1 + static_cast<int>(tabs.size())) % static_cast<int>(tabs.size());
                if (on_change) on_change(*state);
                return true;
            }
            return false;
        })
    });
    
    return container;
}

} // namespace ui::components
