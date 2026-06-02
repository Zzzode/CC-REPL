module;
#include <string>
#include <optional>
#include <algorithm>

export module cc.ui.layout.measure;

// Forward reference to LayoutNode from yoga module
export import cc.ui.layout.yoga;

export namespace cc::ui::layout {

// Measurement result for a UI element
struct Measurement {
    int min_width = 0;
    int max_width = 0;
    int preferred_width = 0;
    int height = 1;
};

// Measure plain text content to determine layout constraints
inline auto measure_text(std::string_view text, int max_width) -> Measurement {
    Measurement result;
    int current_line_width = 0;
    int max_line_width = 0;
    int line_count = 1;
    bool in_escape = false;

    for (char c : text) {
        // Skip ANSI escape sequences for width measurement
        if (c == '\033') {
            in_escape = true;
            continue;
        }
        if (in_escape) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                in_escape = false;
            }
            continue;
        }

        if (c == '\n') {
            max_line_width = std::max(max_line_width, current_line_width);
            current_line_width = 0;
            ++line_count;
            continue;
        }

        ++current_line_width;
    }
    max_line_width = std::max(max_line_width, current_line_width);

    result.min_width = std::min(max_line_width, max_width);
    result.max_width = max_line_width;
    result.preferred_width = std::min(max_line_width, max_width);

    // Calculate height with wrapping
    if (max_width > 0 && max_line_width > max_width) {
        // Approximate wrapped height
        result.height = 0;
        current_line_width = 0;
        for (char c : text) {
            if (c == '\033') { in_escape = true; continue; }
            if (in_escape) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) in_escape = false;
                continue;
            }
            if (c == '\n') {
                ++result.height;
                current_line_width = 0;
                continue;
            }
            ++current_line_width;
            if (current_line_width >= max_width) {
                ++result.height;
                current_line_width = 0;
            }
        }
        ++result.height; // Account for last line
    } else {
        result.height = line_count;
    }

    return result;
}

// Measure how many lines wrapped text will occupy at a given width
inline auto measure_wrapped_text(std::string_view text, int width) -> int {
    if (width <= 0) return 0;
    if (text.empty()) return 1;

    int line_count = 0;
    int current_width = 0;
    bool in_escape = false;

    for (char c : text) {
        if (c == '\033') { in_escape = true; continue; }
        if (in_escape) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) in_escape = false;
            continue;
        }

        if (c == '\n') {
            ++line_count;
            current_width = 0;
            continue;
        }

        ++current_width;
        if (current_width >= width) {
            ++line_count;
            current_width = 0;
        }
    }

    // Final line
    ++line_count;
    return line_count;
}

// Measure a layout node element
inline auto measure_element(LayoutNode& node) -> Measurement {
    Measurement result;

    // If node has explicit dimensions, use them
    if (node.width.has_value()) {
        result.min_width = node.width.value();
        result.max_width = node.width.value();
        result.preferred_width = node.width.value();
    }
    if (node.height.has_value()) {
        result.height = node.height.value();
    }

    // For container nodes, measure based on children
    if (!node.children.empty()) {
        if (node.direction == FlexDirection::Row) {
            // Row: width is sum of children, height is max
            int total_width = 0;
            int max_height = 0;
            for (auto* child : node.children) {
                auto child_m = measure_element(*child);
                total_width += child_m.preferred_width + child->margin * 2;
                max_height = std::max(max_height, child_m.height + child->margin * 2);
            }
            if (!node.width.has_value()) {
                result.min_width = total_width;
                result.max_width = total_width;
                result.preferred_width = total_width;
            }
            if (!node.height.has_value()) {
                result.height = max_height;
            }
        } else {
            // Column: width is max of children, height is sum
            int max_width = 0;
            int total_height = 0;
            for (auto* child : node.children) {
                auto child_m = measure_element(*child);
                max_width = std::max(max_width, child_m.preferred_width + child->margin * 2);
                total_height += child_m.height + child->margin * 2;
            }
            if (!node.width.has_value()) {
                result.min_width = max_width;
                result.max_width = max_width;
                result.preferred_width = max_width;
            }
            if (!node.height.has_value()) {
                result.height = total_height;
            }
        }
    }

    // Add padding
    result.min_width += node.padding * 2;
    result.max_width += node.padding * 2;
    result.preferred_width += node.padding * 2;
    result.height += node.padding * 2;

    return result;
}

} // namespace cc::ui::layout
