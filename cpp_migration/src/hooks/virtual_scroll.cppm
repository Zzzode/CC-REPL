/// @file virtual_scroll.cppm
/// @brief Virtual scrolling hook for large message lists.
/// Only renders visible items for performance, supports smooth scrolling,
/// dynamic item heights, scroll-to-bottom, and viewport calculation.
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>
#include <vector>

export module cc.hooks.virtual_scroll;


export namespace cc::hooks {

// ============================================================
// Scroll event types
// ============================================================

/// Scroll event passed from the UI layer
struct ScrollEvent {
    enum class Type : std::uint8_t {
        WheelUp,
        WheelDown,
        PageUp,
        PageDown,
        Home,
        End,
        DragThumb,   // 拖动滚动条滑块
    };
    Type type;
    int delta{0};                     // 精确滚动量（像素或行数）
    std::optional<float> thumb_pos;   // DragThumb 时的归一化位置 [0, 1]
};

// ============================================================
// ScrollState - complete state of the virtual scroll
// ============================================================

/// Full state of the virtual scroll hook
struct ScrollState {
    std::size_t total_items{0};               // 消息总数
    std::size_t viewport_height{24};          // 视口高度（行数）
    std::size_t scroll_offset{0};             // 滚动偏移（第一个可见项的索引）
    std::vector<std::size_t> item_heights;    // 每项的高度（行数），动态更新
    bool auto_scroll{true};                   // 是否自动滚动到底部
    std::size_t overscan{3};                  // 视口外预渲染的额外项数
};

// ============================================================
// Visible range result
// ============================================================

/// Represents the currently visible range of items
struct VisibleRange {
    std::size_t start{0};     // 第一个可见项索引
    std::size_t end{0};       // 最后一个可见项索引（exclusive）
    std::size_t offset_in_first{0}; // 第一项中被裁剪的行数
};

// ============================================================
// VirtualScrollHook - virtual scrolling logic
// ============================================================

/// Manages virtual scrolling for large message lists in the conversation view.
/// Only computes render data for visible items, allowing thousands of messages
/// without performance degradation.
class VirtualScrollHook {
public:
    explicit VirtualScrollHook(std::size_t viewport_height = 24) {
        state_.viewport_height = viewport_height;
    }

    // ─── Scroll operations ─────────────────────────────────────

    /// Scroll to a specific item index
    auto scroll_to(std::size_t index) -> void {
        if (index >= state_.total_items) index = state_.total_items > 0 ? state_.total_items - 1 : 0;
        state_.scroll_offset = compute_offset_for_item(index);
        state_.auto_scroll = false;
    }

    /// Scroll by a delta (positive = down, negative = up)
    auto scroll_by(int delta) -> void {
        auto new_offset = static_cast<int64_t>(state_.scroll_offset) + delta;
        state_.scroll_offset = static_cast<std::size_t>(
            std::clamp<int64_t>(new_offset, 0, static_cast<int64_t>(max_scroll_offset())));
        // 如果滚动到底部则恢复自动滚动
        state_.auto_scroll = is_at_bottom();
    }

    /// Scroll to the bottom of the content
    auto scroll_to_bottom() -> void {
        state_.scroll_offset = max_scroll_offset();
        state_.auto_scroll = true;
    }

    /// Get the currently visible range of items (with overscan)
    [[nodiscard]] auto get_visible_range() const -> std::pair<std::size_t, std::size_t> {
        if (state_.total_items == 0) return {0, 0};
        auto range = compute_visible_range();
        // 应用 overscan 扩展范围
        auto start = range.start > state_.overscan ? range.start - state_.overscan : 0;
        auto end = std::min(range.end + state_.overscan, state_.total_items);
        return {start, end};
    }

    /// Get detailed visible range including offset within first item
    [[nodiscard]] auto get_visible_range_detailed() const -> VisibleRange {
        return compute_visible_range();
    }

    // ─── Item management ───────────────────────────────────────

    /// Update the height of a specific item (called when content renders)
    auto update_item_height(std::size_t index, std::size_t height) -> void {
        if (index >= state_.item_heights.size()) {
            state_.item_heights.resize(index + 1, 1); // 默认高度为 1 行
        }
        state_.item_heights[index] = std::max<std::size_t>(height, 1);
        // 如果 auto_scroll 开启，保持底部
        if (state_.auto_scroll) {
            scroll_to_bottom();
        }
    }

    /// Notify that new items have been added
    auto add_items(std::size_t count, std::size_t default_height = 1) -> void {
        state_.total_items += count;
        state_.item_heights.resize(state_.total_items, default_height);
        // 新消息到达时如果在底部则自动滚动
        if (state_.auto_scroll) {
            scroll_to_bottom();
        }
    }

    /// Remove items from the beginning (for trimming old history)
    auto remove_items_front(std::size_t count) -> void {
        if (count >= state_.total_items) {
            state_.total_items = 0;
            state_.item_heights.clear();
            state_.scroll_offset = 0;
            return;
        }
        state_.total_items -= count;
        state_.item_heights.erase(state_.item_heights.begin(),
                                   state_.item_heights.begin() + static_cast<long>(count));
        // 调整 scroll_offset
        auto removed_height = compute_height_range(0, count);
        state_.scroll_offset = state_.scroll_offset > removed_height ?
                               state_.scroll_offset - removed_height : 0;
    }

    // ─── Event handling ────────────────────────────────────────

    /// Handle a scroll event from the UI layer; returns true if consumed
    [[nodiscard]] auto handle_scroll_event(const ScrollEvent& event) -> bool {
        switch (event.type) {
            case ScrollEvent::Type::WheelUp:
                scroll_by(-3);  // 滚动3行
                return true;
            case ScrollEvent::Type::WheelDown:
                scroll_by(3);
                return true;
            case ScrollEvent::Type::PageUp:
                scroll_by(-static_cast<int>(state_.viewport_height));
                return true;
            case ScrollEvent::Type::PageDown:
                scroll_by(static_cast<int>(state_.viewport_height));
                return true;
            case ScrollEvent::Type::Home:
                state_.scroll_offset = 0;
                state_.auto_scroll = false;
                return true;
            case ScrollEvent::Type::End:
                scroll_to_bottom();
                return true;
            case ScrollEvent::Type::DragThumb:
                if (event.thumb_pos) {
                    auto pos = std::clamp(*event.thumb_pos, 0.0f, 1.0f);
                    state_.scroll_offset = static_cast<std::size_t>(
                        pos * static_cast<float>(max_scroll_offset()));
                    state_.auto_scroll = is_at_bottom();
                }
                return true;
        }
        return false;
    }

    // ─── Query methods ─────────────────────────────────────────

    /// Check if the scroll is at the bottom
    [[nodiscard]] auto is_at_bottom() const -> bool {
        return state_.scroll_offset >= max_scroll_offset();
    }

    /// Get the total content height in lines
    [[nodiscard]] auto total_content_height() const -> std::size_t {
        return compute_height_range(0, state_.total_items);
    }

    /// Get scrollbar thumb position as normalized float [0, 1]
    [[nodiscard]] auto scrollbar_position() const -> float {
        auto max_off = max_scroll_offset();
        if (max_off == 0) return 1.0f;
        return static_cast<float>(state_.scroll_offset) / static_cast<float>(max_off);
    }

    /// Get scrollbar thumb size as fraction of viewport [0, 1]
    [[nodiscard]] auto scrollbar_thumb_size() const -> float {
        auto total = total_content_height();
        if (total == 0) return 1.0f;
        return std::min(1.0f,
            static_cast<float>(state_.viewport_height) / static_cast<float>(total));
    }

    // ─── Configuration ─────────────────────────────────────────

    /// Set viewport height (e.g., on terminal resize)
    auto set_viewport_height(std::size_t height) -> void {
        state_.viewport_height = std::max<std::size_t>(height, 1);
        if (state_.auto_scroll) scroll_to_bottom();
    }

    /// Set overscan count
    auto set_overscan(std::size_t overscan) -> void { state_.overscan = overscan; }

    /// Enable or disable auto-scroll
    auto set_auto_scroll(bool enabled) -> void { state_.auto_scroll = enabled; }

    [[nodiscard]] auto state() const -> const ScrollState& { return state_; }
    [[nodiscard]] auto viewport_height() const -> std::size_t { return state_.viewport_height; }
    [[nodiscard]] auto total_items() const -> std::size_t { return state_.total_items; }

private:
    ScrollState state_;

    // ─── Internal helpers ──────────────────────────────────────

    /// Compute total height of items in range [start, end)
    [[nodiscard]] auto compute_height_range(std::size_t start, std::size_t end) const -> std::size_t {
        if (start >= end || state_.item_heights.empty()) return 0;
        end = std::min(end, state_.item_heights.size());
        std::size_t height = 0;
        for (std::size_t i = start; i < end; ++i) {
            height += state_.item_heights[i];
        }
        return height;
    }

    /// Compute the scroll offset needed to bring an item into view
    [[nodiscard]] auto compute_offset_for_item(std::size_t index) const -> std::size_t {
        return compute_height_range(0, index);
    }

    /// Maximum possible scroll offset (total height - viewport)
    [[nodiscard]] auto max_scroll_offset() const -> std::size_t {
        auto total = total_content_height();
        if (total <= state_.viewport_height) return 0;
        return total - state_.viewport_height;
    }

    /// Compute which items are visible in the current viewport
    [[nodiscard]] auto compute_visible_range() const -> VisibleRange {
        VisibleRange range;
        if (state_.total_items == 0) return range;

        // 查找第一个可见项
        std::size_t accumulated = 0;
        for (std::size_t i = 0; i < state_.total_items; ++i) {
            auto item_h = i < state_.item_heights.size() ? state_.item_heights[i] : 1;
            if (accumulated + item_h > state_.scroll_offset) {
                range.start = i;
                range.offset_in_first = state_.scroll_offset - accumulated;
                break;
            }
            accumulated += item_h;
        }

        // 查找最后一个可见项
        std::size_t visible_height = 0;
        for (std::size_t i = range.start; i < state_.total_items; ++i) {
            auto item_h = i < state_.item_heights.size() ? state_.item_heights[i] : 1;
            visible_height += item_h;
            if (visible_height >= state_.viewport_height + range.offset_in_first) {
                range.end = i + 1;
                return range;
            }
        }
        range.end = state_.total_items;
        return range;
    }
};

} // namespace cc::hooks
