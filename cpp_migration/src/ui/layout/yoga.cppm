module;
#include <string>
#include <vector>
#include <optional>
#include <algorithm>
#include <numeric>

export module cc.ui.layout.yoga;

export namespace cc::ui::layout {

// Flex layout direction
enum class FlexDirection { Row, Column };

// Main axis alignment
enum class Justify { Start, Center, End, SpaceBetween, SpaceAround };

// Cross axis alignment
enum class Align { Start, Center, End, Stretch };

// A node in the layout tree (flexbox-like)
struct LayoutNode {
    FlexDirection direction = FlexDirection::Column;
    Justify justify = Justify::Start;
    Align align = Align::Stretch;
    std::optional<int> width;
    std::optional<int> height;
    int flex = 0;         // Flex grow factor
    int margin = 0;       // Outer margin (all sides)
    int padding = 0;      // Inner padding (all sides)
    std::vector<LayoutNode*> children;
};

// Computed position and size after layout
struct ComputedLayout {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

namespace detail {

// Get the main axis size of a node
inline auto get_main_size(const LayoutNode& node, FlexDirection dir) -> int {
    if (dir == FlexDirection::Row) {
        return node.width.value_or(0);
    }
    return node.height.value_or(0);
}

// Get the cross axis size of a node
inline auto get_cross_size(const LayoutNode& node, FlexDirection dir) -> int {
    if (dir == FlexDirection::Row) {
        return node.height.value_or(0);
    }
    return node.width.value_or(0);
}

} // namespace detail

// Compute layout for the entire tree, returning computed layouts in tree order
inline auto compute_layout(LayoutNode& root,
                           int container_width,
                           int container_height) -> std::vector<ComputedLayout> {
    std::vector<ComputedLayout> results;

    // Root node fills the container
    ComputedLayout root_layout;
    root_layout.x = root.margin;
    root_layout.y = root.margin;
    root_layout.width = root.width.value_or(container_width) - root.margin * 2;
    root_layout.height = root.height.value_or(container_height) - root.margin * 2;
    results.push_back(root_layout);

    if (root.children.empty()) return results;

    // Available space for children (minus padding)
    int avail_width = root_layout.width - root.padding * 2;
    int avail_height = root_layout.height - root.padding * 2;
    int content_x = root_layout.x + root.padding;
    int content_y = root_layout.y + root.padding;

    bool is_row = (root.direction == FlexDirection::Row);
    int main_space = is_row ? avail_width : avail_height;
    int cross_space = is_row ? avail_height : avail_width;

    // First pass: calculate fixed sizes and total flex
    int total_fixed = 0;
    int total_flex = 0;
    std::vector<int> child_main_sizes(root.children.size(), 0);

    for (size_t i = 0; i < root.children.size(); ++i) {
        auto* child = root.children[i];
        int fixed_size = detail::get_main_size(*child, root.direction);
        if (child->flex > 0) {
            total_flex += child->flex;
        } else {
            child_main_sizes[i] = fixed_size + child->margin * 2;
            total_fixed += child_main_sizes[i];
        }
    }

    // Distribute remaining space to flex children
    int remaining = std::max(0, main_space - total_fixed);
    for (size_t i = 0; i < root.children.size(); ++i) {
        auto* child = root.children[i];
        if (child->flex > 0 && total_flex > 0) {
            child_main_sizes[i] = (remaining * child->flex / total_flex) + child->margin * 2;
        }
    }

    // Calculate starting position based on justify
    int total_children_size = std::accumulate(
        child_main_sizes.begin(), child_main_sizes.end(), 0);
    int gap = 0;
    int start_offset = 0;

    switch (root.justify) {
        case Justify::Start:
            start_offset = 0;
            break;
        case Justify::Center:
            start_offset = (main_space - total_children_size) / 2;
            break;
        case Justify::End:
            start_offset = main_space - total_children_size;
            break;
        case Justify::SpaceBetween:
            if (root.children.size() > 1) {
                gap = (main_space - total_children_size) /
                      static_cast<int>(root.children.size() - 1);
            }
            break;
        case Justify::SpaceAround:
            if (!root.children.empty()) {
                gap = (main_space - total_children_size) /
                      static_cast<int>(root.children.size());
                start_offset = gap / 2;
            }
            break;
    }

    // Second pass: position children
    int main_offset = start_offset;
    for (size_t i = 0; i < root.children.size(); ++i) {
        auto* child = root.children[i];
        int child_main = child_main_sizes[i] - child->margin * 2;
        int child_cross = detail::get_cross_size(*child, root.direction);

        // Cross axis sizing
        if (child_cross == 0 || root.align == Align::Stretch) {
            child_cross = cross_space - child->margin * 2;
        }

        // Cross axis alignment
        int cross_offset = 0;
        switch (root.align) {
            case Align::Start:
            case Align::Stretch:
                cross_offset = 0;
                break;
            case Align::Center:
                cross_offset = (cross_space - child_cross - child->margin * 2) / 2;
                break;
            case Align::End:
                cross_offset = cross_space - child_cross - child->margin * 2;
                break;
        }

        ComputedLayout child_layout;
        if (is_row) {
            child_layout.x = content_x + main_offset + child->margin;
            child_layout.y = content_y + cross_offset + child->margin;
            child_layout.width = child_main;
            child_layout.height = child_cross;
        } else {
            child_layout.x = content_x + cross_offset + child->margin;
            child_layout.y = content_y + main_offset + child->margin;
            child_layout.width = child_cross;
            child_layout.height = child_main;
        }
        results.push_back(child_layout);

        main_offset += child_main_sizes[i] + gap;
    }

    return results;
}

} // namespace cc::ui::layout
