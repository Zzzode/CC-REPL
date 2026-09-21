/// @file pagination_util.cppm
/// @brief Pure pagination state machine for scrollable list UIs.
///
/// Extracted from src/commands/plugin/usePagination.ts.
/// The React hooks (useCallback/useMemo/useRef) are removed; the windowing
/// logic, scroll-offset tracking, and navigation helpers are retained
/// as a side-effect-free class usable by any rendering layer
/// (FTXUI / CLI / tests).

module;

#include <cstdint>
#include <vector>
#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <optional>

export module cc.commands.plugin_pagination_util;

export namespace cc::commands::plugin {

/// Default number of items visible in the scroll window.
constexpr std::size_t DEFAULT_MAX_VISIBLE = 5;

/// Result of a pagination state snapshot.
/// Mirrors the return-value shape of TS: usePagination().
struct PaginationSnapshot {
    std::size_t current_page = 0;
    std::size_t total_pages = 1;
    std::size_t start_index = 0;
    std::size_t end_index = 0;
    bool needs_pagination = false;
    std::size_t page_size = DEFAULT_MAX_VISIBLE;

    // Scroll position info (for UI display).
    struct ScrollPosition {
        std::size_t current = 1;    // 1-based index of selected item
        std::size_t total = 0;      // total items
        bool can_scroll_up = false;
        bool can_scroll_down = false;
    };
    ScrollPosition scroll_position{};
};

/// Pure state-machine implementing the pagination window.
/// React hooks (`useRef`, `useMemo`, `useCallback`) from the original TS are
/// replaced by internal member fields and member functions.
class Paginator {
public:
    /// Initialize with the number of items and visible window size.
    explicit Paginator(std::size_t total_items,
                       std::size_t max_visible = DEFAULT_MAX_VISIBLE,
                       std::size_t selected_index = 0)
        : total_items_(total_items),
          max_visible_(max_visible == 0 ? DEFAULT_MAX_VISIBLE : max_visible),
          selected_index_(std::min(selected_index, total_items == 0 ? 0 : total_items - 1))
    {
        recompute_offset();
    }

    // -- Selection ------------------------------------------------------------

    /// Set the currently selected item index, clamping to [0, total_items).
    void set_selected_index(std::size_t index) {
        if (total_items_ == 0) return;
        selected_index_ = std::min(index, total_items_ - 1);
        recompute_offset();
    }

    [[nodiscard]] std::size_t selected_index() const noexcept { return selected_index_; }

    // -- Window ---------------------------------------------------------------

    /// Apply the visible window to an items slice. Returns the sub-range of
    /// items currently on screen.
    template <typename T>
    [[nodiscard]] std::vector<T> get_visible_items(const std::vector<T>& items) const {
        if (!needs_pagination()) return items;
        const auto s = start_index();
        const auto e = end_index();
        return std::vector<T>(items.begin() + static_cast<std::ptrdiff_t>(s),
                              items.begin() + static_cast<std::ptrdiff_t>(e));
    }

    /// Map a visible-window index (0..page_size-1) to the actual items index.
    [[nodiscard]] std::size_t to_actual_index(std::size_t visible_index) const noexcept {
        return scroll_offset_ + visible_index;
    }

    /// Whether an items index is currently inside the visible window.
    [[nodiscard]] bool is_on_current_page(std::size_t actual_index) const noexcept {
        return actual_index >= start_index() && actual_index < end_index();
    }

    // -- Navigation -----------------------------------------------------------

    /// Jump to a specific page number (0-based). Clamped to valid range.
    void go_to_page(std::size_t page) {
        if (!needs_pagination()) return;
        const auto max_page = total_pages() - 1;
        page = std::min(page, max_page);
        scroll_offset_ = page * max_visible_;
        // Move selection into the new window if it's outside.
        if (selected_index_ < scroll_offset_) selected_index_ = scroll_offset_;
        if (selected_index_ >= end_index())  selected_index_ = end_index() - 1;
    }

    /// Advance the window forward by one full page.
    void next_page() {
        if (!needs_pagination()) return;
        go_to_page(current_page() + 1);
    }

    /// Move the window back by one full page.
    void prev_page() {
        if (!needs_pagination() || current_page() == 0) return;
        go_to_page(current_page() - 1);
    }

    /// Selection-aware navigation: move to the next/previous item.
    /// Returns true if the selection actually changed.
    bool handle_selection_delta(int delta) {
        if (total_items_ == 0) return false;
        const auto new_index = static_cast<std::size_t>(
            std::max(0, static_cast<int>(selected_index_) + delta));
        const auto clamped = std::min(new_index, total_items_ - 1);
        if (clamped == selected_index_) return false;
        set_selected_index(clamped);
        return true;
    }

    /// Page-key navigation. Returns true if the window changed.
    /// Left = prev_page, Right = next_page.
    bool handle_page_navigation(char direction) {
        if (!needs_pagination()) return false;
        const auto page_before = current_page();
        if (direction == 'L' || direction == '<' || direction == -1) prev_page();
        else if (direction == 'R' || direction == '>' || direction == 1) next_page();
        else return false;
        return current_page() != page_before;
    }

    // -- Snapshot -------------------------------------------------------------

    /// Build the full snapshot (matching the React hook's return value).
    [[nodiscard]] PaginationSnapshot snapshot() const {
        PaginationSnapshot s{
            .current_page = current_page(),
            .total_pages = total_pages(),
            .start_index = start_index(),
            .end_index = end_index(),
            .needs_pagination = needs_pagination(),
            .page_size = max_visible_,
        };
        s.scroll_position = PaginationSnapshot::ScrollPosition{
            .current = total_items_ == 0 ? 0 : selected_index_ + 1,
            .total = total_items_,
            .can_scroll_up = scroll_offset_ > 0,
            .can_scroll_down = scroll_offset_ + max_visible_ < total_items_,
        };
        return s;
    }

    // -- Direct accessors -----------------------------------------------------

    [[nodiscard]] std::size_t total_items()  const noexcept { return total_items_; }
    [[nodiscard]] std::size_t max_visible()  const noexcept { return max_visible_; }
    [[nodiscard]] bool        needs_pagination() const noexcept { return total_items_ > max_visible_; }
    [[nodiscard]] std::size_t start_index()  const noexcept { return scroll_offset_; }
    [[nodiscard]] std::size_t end_index()    const noexcept { return std::min(scroll_offset_ + max_visible_, total_items_); }
    [[nodiscard]] std::size_t total_pages()  const noexcept {
        if (total_items_ == 0) return 1;
        return (total_items_ + max_visible_ - 1) / max_visible_;
    }
    [[nodiscard]] std::size_t current_page() const noexcept {
        return scroll_offset_ / max_visible_;
    }

private:
    std::size_t total_items_ = 0;
    std::size_t max_visible_ = DEFAULT_MAX_VISIBLE;
    std::size_t selected_index_ = 0;
    std::size_t scroll_offset_ = 0;

    /// After the selection changes, adjust scroll_offset_ so that the
    /// selected item always remains inside the visible window.
    void recompute_offset() noexcept {
        if (!needs_pagination()) { scroll_offset_ = 0; return; }

        if (selected_index_ < scroll_offset_) {
            // selected moved above the window -> scroll up.
            scroll_offset_ = selected_index_;
            return;
        }
        if (selected_index_ >= scroll_offset_ + max_visible_) {
            // selected moved below the window -> scroll down.
            scroll_offset_ = selected_index_ - max_visible_ + 1;
            return;
        }
        // selected is still inside the window, but clamp offset to valid range.
        const std::size_t max_offset = total_items_ > max_visible_
                                     ? total_items_ - max_visible_
                                     : 0;
        if (scroll_offset_ > max_offset) scroll_offset_ = max_offset;
    }
};

} // namespace cc::commands::plugin
