module;
#include <string>
#include <vector>
#include <optional>
#include <sstream>
#include <algorithm>

export module cc.ui.design.list_item;

export namespace cc::ui::design {

// Configuration for a single list item
struct ListItemConfig {
    std::string label;
    std::optional<std::string> description;
    std::optional<std::string> badge;
    bool selected = false;
    bool disabled = false;
};

// Render a single list item
inline auto render_list_item(ListItemConfig config) -> std::string {
    std::ostringstream out;

    // Selection indicator
    if (config.selected) {
        out << "\033[36m❯\033[0m "; // cyan arrow
    } else {
        out << "  ";
    }

    // Main label with disabled dimming
    if (config.disabled) {
        out << "\033[2m" << config.label << "\033[0m";
    } else if (config.selected) {
        out << "\033[1m" << config.label << "\033[0m"; // bold when selected
    } else {
        out << config.label;
    }

    // Badge (right-aligned indicator)
    if (config.badge.has_value()) {
        out << " \033[33m[" << config.badge.value() << "]\033[0m";
    }

    // Description on same line, dimmed
    if (config.description.has_value()) {
        out << " \033[2m" << config.description.value() << "\033[0m";
    }

    return out.str();
}

// Render a scrollable list with visible window
inline auto render_list(std::vector<ListItemConfig> items,
                        int visible_count = 10,
                        int scroll_offset = 0) -> std::string {
    std::ostringstream out;
    int total = static_cast<int>(items.size());
    int effective_visible = std::min(visible_count, total);

    // Clamp scroll offset
    int max_offset = std::max(0, total - effective_visible);
    scroll_offset = std::clamp(scroll_offset, 0, max_offset);

    // Scroll-up indicator
    if (scroll_offset > 0) {
        out << "  \033[2m↑ " << scroll_offset << " more\033[0m\n";
    }

    // Render visible items
    for (int i = scroll_offset; i < scroll_offset + effective_visible && i < total; ++i) {
        out << render_list_item(items[i]);
        if (i < scroll_offset + effective_visible - 1) {
            out << "\n";
        }
    }

    // Scroll-down indicator
    int remaining = total - (scroll_offset + effective_visible);
    if (remaining > 0) {
        out << "\n  \033[2m↓ " << remaining << " more\033[0m";
    }

    return out.str();
}

} // namespace cc::ui::design
