/// @file custom_select.cppm
/// @brief Filterable select dropdown with keyboard navigation, multi-select,
/// inline descriptions, virtual scrolling, search, and grouped rendering.
///
/// Migrated from the TS CustomSelect/ folder (15 files, ~3800 lines):
///   - select.tsx                 (top-level component, modes & layout)
///   - use-select-navigation.ts   (reducer-based nav state machine)
///   - use-select-state.ts        (single-select value + nav composition)
///   - use-multi-select-state.ts  (multi-select value, a/A, Enter/Space)
///   - use-select-input.ts        (search input, filter, digit shortcuts)
///   - select-input-option.tsx    (option renderer with highlight)
///   - SelectMulti.tsx            (multi-select layout)
///
/// NOTES / TODOs (out of scope for UI6, flag for follow-up work):
///   - Fuzzy-match: currently "case-insensitive contains"; plug a
///     fuzzy-match lib (e.g. libtre, or roll-your-own bitap) in ApplyFilter.
///   - Async remote-loading: on_search is synchronous today. For HTTP, wrap
///     in a std::future + render a "Loading…" spinner when unresolved.
///   - Custom option renderer: pass a std::function callback through props.
///   - Group collapse: groups render as label + separator today; no fold.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <algorithm>
#include <cctype>
#include <bitset>
#include <unordered_set>
#include <utility>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.custom_select;

import cc.types.types;

export namespace cc::ui::custom_select {
using namespace ftxui;

// ============================================================
// Enums & Types
// ============================================================

/// Selection behaviour
enum class SelectMode : std::uint8_t {
    Single,     // Select exactly one, Enter submits & closes
    Multi,      // Toggle many, Enter keeps open (submit via callback)
};

/// Layout mode for displaying options
enum class SelectLayout : std::uint8_t {
    Compact,            // One line per option (default)
    Expanded,           // Multiple lines with spacing + desc below
    CompactVertical,    // Compact index with descriptions below
};

/// Where keyboard focus lives inside the component
enum class FocusZone : std::uint8_t {
    List,       // The option list (default)
    Search,     // The embedded search input box
};

/// A single option in the select dropdown.
///
/// NOTE: For >100K options, populate `label`/`value`/`description`/`group`
/// from an external arena and prefer string_view-compatible storage. The
/// component copies SelectOption into its own state; if this is too heavy,
/// move the storage out and replace the vector with a span<const SelectOption>.
struct SelectOption {
    std::string label;            // Display text (highlighted in search)
    std::string value;            // Opaque value passed to on_submit / on_change
    std::string description;      // Secondary line (inline or expanded)
    std::string group;            // Group key for Tab-grouping & separators
    std::string icon;             // Optional 1-2 glyph prefix (e.g. "📁")
    bool disabled = false;        // Cannot be selected/focused
    bool dim_description = false; // Render description dimmer than default
};

// ------------------------------------------------------------------
// Props / API
// ------------------------------------------------------------------

/// Configuration for the CustomSelect component.
struct CustomSelectOptions {
    // -- data -------------------------------------------------------
    std::vector<SelectOption> options;

    // -- behaviour --------------------------------------------------
    SelectMode mode = SelectMode::Single;
    SelectLayout layout = SelectLayout::Compact;

    /// How many rows of options to render at once. Controls virtual-scroll
    /// window size AND page-size for PgUp/PgDn. Default 15 to support
    /// 10K+ smooth rendering (O(visible_count) draw calls per frame).
    int visible_count = 15;

    /// Single-mode: initial pre-selected value.
    std::optional<std::string> default_value;
    /// Multi-mode: initially selected values.
    std::vector<std::string> default_values;

    /// Focus last option instead of first on mount.
    bool initial_focus_last = false;

    /// If true, first-line option index labels are rendered and number keys
    /// 1..9 select by index.
    bool show_indexes = false;

    /// If true, render descriptions inline after the label (Expanded
    /// layout puts them on the next line regardless).
    bool inline_descriptions = false;

    /// Disable all input (for locked dialogs).
    bool disabled = false;

    /// When true, Enter/number keys do not fire selection (still allow
    /// scrolling). Used for preview-style pickers.
    bool disable_selection = false;

    /// When true, navigation wraps from last → first and first → last.
    /// Default false; matches TS use-select-input "onDownFromLastItem"
    /// callback semantics (when provided, wrapping is suppressed).
    bool wrap_navigation = false;

    /// Render a search input above the list. Pressing '/' focuses it.
    bool enable_search = true;
    /// Initial search text (rarely used).
    std::string initial_search;

    // -- callbacks --------------------------------------------------
    /// Single: fired immediately when user selects a value.
    /// Multi : fired every time a value is toggled (use for live changes).
    std::function<void(const std::string& value)> on_change;

    /// Single: fired on Enter (commit + close).
    /// Multi : fired by the consumer when they're done (the component does
    ///         NOT force-close on Enter in Multi mode, mirroring TS).
    std::function<void(const std::string& value)> on_submit_single;

    /// Multi: fire when consumer considers selection complete. The
    /// component exposes Submit() so you can wire it to an OK button.
    std::function<void(const std::vector<std::string>& values)> on_submit_multi;

    /// Fired whenever focus lands on a different option.
    std::function<void(const std::string& value)> on_focus;

    /// Fired on Escape (consumer typically tears down the dialog).
    std::function<void()> on_cancel;

    /// Up-arrow pressed on first item; if set, navigation does NOT wrap
    /// even if wrap_navigation is true.
    std::function<void()> on_up_from_first;

    /// Down-arrow pressed on last filtered item.
    std::function<void()> on_down_from_last;

    /// Optional: custom renderer per option.
    /// If unset, the default renderer is used (icon + index + label + desc).
    std::function<Element(const SelectOption& opt,
                          bool hovered, bool selected,
                          int display_index)> custom_render;

    /// Optional: remote (async) search. Return a vector of *indices into
    /// the original options vector* that match the query. Leave nullopt to
    /// use the builtin case-insensitive contains filter.
    /// NOTE: synchronous today. For true async, return a future and track
    /// a "loading" bit to render a spinner.
    std::function<std::vector<int>(std::string_view query)> on_search;
};

/// External handle to a running CustomSelect (returned by factories).
/// Lets consumers Submit() or ReadCurrentSelection() without reaching
/// into internal state.
class CustomSelectHandle
    : public std::enable_shared_from_this<CustomSelectHandle> {
public:
    virtual ~CustomSelectHandle() = default;

    /// Trigger submit semantics manually (e.g. for an OK button).
    virtual void Submit() = 0;

    /// Read currently-selected values (Multi: all checked; Single: size 1
    /// or 0).
    [[nodiscard]] virtual std::vector<std::string> SelectedValues() const = 0;

    /// Focused option's value (useful for per-option previews).
    [[nodiscard]] virtual std::optional<std::string> FocusedValue() const = 0;

    /// Focus a specific option by value (no-op if not found / filtered out).
    virtual void FocusOption(const std::string& value) = 0;
};

// ============================================================
// Helpers: case-insensitive contains / group detection
// ============================================================

namespace detail {

[[nodiscard]] inline std::string ToLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

[[nodiscard]] inline bool ContainsCI(std::string_view hay,
                                     std::string_view needle) {
    if (needle.empty()) return true;
    auto hay_l = ToLower(hay);
    auto ndl_l = ToLower(needle);
    return hay_l.find(ndl_l) != std::string::npos;
}

/// Detect group boundaries. Returns [group_name, is_header_started_new_group].
/// Used during rendering to print group label + separator before the first
/// option of each group.
struct GroupMark {
    bool is_new_group;
    std::string group_name;
};

} // namespace detail

// ============================================================
// Rendering helpers
// ============================================================

/// Highlight all case-insensitive matches of `highlight` within `label`.
[[nodiscard]] inline Element RenderHighlightedLabel(
    const std::string& label, const std::string& highlight) {

    if (highlight.empty()) return text(label);
    std::string lower_label = detail::ToLower(label);
    std::string lower_hl = detail::ToLower(highlight);

    Elements parts;
    size_t cursor = 0;
    size_t pos;
    while ((pos = lower_label.find(lower_hl, cursor)) != std::string::npos) {
        if (pos > cursor)
            parts.push_back(text(label.substr(cursor, pos - cursor)));
        parts.push_back(text(label.substr(pos, highlight.size()))
                        | bold | color(Color::Yellow));
        cursor = pos + highlight.size();
    }
    if (cursor < label.size())
        parts.push_back(text(label.substr(cursor)));
    return hbox(std::move(parts));
}

/// Render a vertical ASCII scrollbar of `height` rows, showing the current
/// `ratio` (0..1) and `thumb_ratio` (0..1, fraction of visible / total).
/// Characters: │█│░ — visually distinct even on 256-color terminals.
[[nodiscard]] inline Elements RenderScrollBar(int height, double ratio,
                                              double thumb_ratio) {
    if (height <= 0) return {};
    thumb_ratio = std::clamp(thumb_ratio, 1.0 / std::max(1, height), 1.0);
    ratio = std::clamp(ratio, 0.0, 1.0);

    int thumb_h = std::max(1, (int)std::round(thumb_ratio * height));
    int thumb_top = (int)std::round(ratio * (height - thumb_h));

    Elements out;
    out.reserve(height);
    for (int i = 0; i < height; ++i) {
        bool in_thumb = (i >= thumb_top && i < thumb_top + thumb_h);
        if (in_thumb)
            out.push_back(text("█") | color(Color::Cyan));
        else
            out.push_back(text("░") | dim);
    }
    return out;
}

// ============================================================
// State machine + component
// ============================================================

/// Concrete implementation. Exposed via Component and CustomSelectHandle.
class CustomSelectImpl : public CustomSelectHandle {
public:
    explicit CustomSelectImpl(CustomSelectOptions opts)
        : opts_(std::move(opts)) {
        InitializeState();
    }

    // ------------------------------------------------------------------
    // CustomSelectHandle
    // ------------------------------------------------------------------

    void Submit() override {
        if (opts_.mode == SelectMode::Single) {
            if (opts_.on_submit_single && single_selected_.has_value())
                opts_.on_submit_single(*single_selected_);
        } else {
            if (opts_.on_submit_multi)
                opts_.on_submit_multi(SelectedValues());
        }
    }

    [[nodiscard]] std::vector<std::string> SelectedValues() const override {
        std::vector<std::string> out;
        if (opts_.mode == SelectMode::Single) {
            if (single_selected_.has_value()) out.push_back(*single_selected_);
        } else {
            out.reserve(multi_selected_.count());
            for (size_t i = 0; i < opts_.options.size(); ++i) {
                if (multi_selected_.test(std::min(i, size_t(kMaxBitset - 1))) &&
                    IsSelectedBitset(i)) {
                    out.push_back(opts_.options[i].value);
                }
            }
        }
        return out;
    }

    [[nodiscard]] std::optional<std::string> FocusedValue() const override {
        if (FocusedFiltered() < 0) return std::nullopt;
        return opts_.options[filtered_to_original_[FocusedFiltered()]].value;
    }

    void FocusOption(const std::string& value) override {
        for (size_t i = 0; i < filtered_to_original_.size(); ++i) {
            if (opts_.options[filtered_to_original_[i]].value == value) {
                focused_filtered_ = (int)i;
                EnsureFocusVisible();
                FireOnFocus();
                return;
            }
        }
    }

    // ------------------------------------------------------------------
    // Public API used by the Component wrapper (event + render)
    // ------------------------------------------------------------------

    [[nodiscard]] Component BuildComponent() {
        auto self = std::static_pointer_cast<CustomSelectImpl>(shared_from_this());
        // Two internal sub-components: search Input (optional), and the list
        // Renderer + CatchEvent. We compose them vertically.
        auto list = Renderer([self] { return self->RenderList(); })
                   | CatchEvent([self](Event e) { return self->HandleListEvent(e); });

        if (!opts_.enable_search) return list;

        search_input_ = Input(&search_text_, "Search options… (type / to focus)");
        // Hook on_change so filtering recomputes as the user types.
        auto search_on_change = search_input_
            | CatchEvent([self](Event) {
                  // After the Input handles it, our `search_text_` ref is
                  // updated. Recompute filter.
                  self->ApplyFilter();
                  self->EnsureFocusValid();
                  return false;
              });

        auto sep = Renderer([] { return separator(); });
        return Container::Vertical({
            search_on_change,
            sep,
            list,
        }) | CatchEvent([self](Event e) -> bool {
            // Top-level shortcuts that work regardless of focus zone.
            if (e == Event::Escape) {
                if (self->zone_ == FocusZone::Search) {
                    // Esc in search box: clear search + return to list
                    self->search_text_.clear();
                    self->ApplyFilter();
                    self->zone_ = FocusZone::List;
                    return true;
                }
                if (self->opts_.on_cancel) self->opts_.on_cancel();
                return true;
            }
            // '/' → always jump to search input
            if (self->opts_.enable_search && e == Event::Character('/') &&
                self->zone_ != FocusZone::Search) {
                self->zone_ = FocusZone::Search;
                self->search_input_->TakeFocus();
                // Don't consume '/' yet - let Input append it so the user
                // can immediately start typing a query.
                return false;
            }
            // Ctrl-F / Ctrl-E search aliases (standard bindings)
            if (self->opts_.enable_search && e.input() == "ctrl+f") {
                self->zone_ = FocusZone::Search;
                self->search_input_->TakeFocus();
                return true;
            }
            // Forward to the default container dispatcher otherwise.
            return false;
        });
    }

private:
    // Allow shared_from_this: we inherit from handle but hold Component;
    // the Component captures a shared_ptr<CustomSelectImpl>.
    // (CustomSelectHandle is enabled_shared_from_this via the inheritance
    // pattern in MakeShared — we build with shared_ptr manually.)

    static constexpr size_t kMaxBitset = 1u << 16; // 65536
    // NOTE: options.size() may exceed kMaxBitset. For such large lists we
    // fall back to a sparse unordered_set<size_t> so multi-select still
    // works without deep-copy cost.

    CustomSelectOptions opts_;

    // -- filter state -------------------------------------------------
    std::string search_text_;
    /// Index mapping: filtered slot i → original index in opts_.options.
    std::vector<int> filtered_to_original_;

    // -- nav state (indexes into filtered_to_original_) ---------------
    int focused_filtered_ = 0;
    int visible_start_ = 0;
    FocusZone zone_ = FocusZone::List;
    Component search_input_;

    // -- selection state ---------------------------------------------
    std::optional<std::string> single_selected_;
    std::bitset<kMaxBitset> multi_selected_;
    std::unordered_set<size_t> multi_selected_large_; // overflow for >=64K
    bool use_large_set_ = false;

    // -- group ordering (for Tab group-jumping) -----------------------
    /// groups_ in order of first appearance, each → first filtered index.
    std::vector<std::pair<std::string, int>> group_anchor_;

    // ------------------------------------------------------------------
    void InitializeState() {
        size_t n = opts_.options.size();
        use_large_set_ = (n >= kMaxBitset);

        // Apply default search
        search_text_ = opts_.initial_search;
        ApplyFilter();

        // Set selection defaults
        if (opts_.mode == SelectMode::Single) {
            single_selected_ = opts_.default_value;
        } else {
            std::unordered_set<std::string> dv(opts_.default_values.begin(),
                                               opts_.default_values.end());
            for (size_t i = 0; i < n; ++i) {
                if (dv.count(opts_.options[i].value)) {
                    if (use_large_set_) multi_selected_large_.insert(i);
                    else multi_selected_.set(i);
                }
            }
        }

        // Initial focus
        if (!filtered_to_original_.empty()) {
            if (opts_.initial_focus_last)
                focused_filtered_ = (int)filtered_to_original_.size() - 1;
            else if (opts_.default_value.has_value())
                FocusOption(*opts_.default_value);
            else
                focused_filtered_ = FirstNonDisabled(0);
        }
        EnsureFocusVisible();
        FireOnFocus();
    }

    // ------------------------------------------------------------------
    // Filter
    // ------------------------------------------------------------------
    void ApplyFilter() {
        filtered_to_original_.clear();
        group_anchor_.clear();

        if (!search_text_.empty() && opts_.on_search) {
            // Custom search provider (e.g. fuzzy / remote).
            filtered_to_original_ = opts_.on_search(search_text_);
            // Validate all indices.
            filtered_to_original_.erase(
                std::remove_if(filtered_to_original_.begin(),
                               filtered_to_original_.end(),
                               [&](int i) {
                                   return i < 0 ||
                                          i >= (int)opts_.options.size();
                               }),
                filtered_to_original_.end());
        } else if (!search_text_.empty()) {
            // Builtin: case-insensitive contains on label OR description OR
            // value.
            filtered_to_original_.reserve(opts_.options.size());
            for (size_t i = 0; i < opts_.options.size(); ++i) {
                const auto& o = opts_.options[i];
                if (detail::ContainsCI(o.label, search_text_) ||
                    detail::ContainsCI(o.description, search_text_) ||
                    detail::ContainsCI(o.value, search_text_)) {
                    filtered_to_original_.push_back((int)i);
                }
            }
        } else {
            // No filter: identity mapping.
            filtered_to_original_.reserve(opts_.options.size());
            for (size_t i = 0; i < opts_.options.size(); ++i)
                filtered_to_original_.push_back((int)i);
        }

        // Build group anchors.
        std::unordered_set<std::string> seen;
        seen.reserve(filtered_to_original_.size() > 0 ? 8 : 0);
        for (size_t i = 0; i < filtered_to_original_.size(); ++i) {
            const auto& g = opts_.options[filtered_to_original_[i]].group;
            if (g.empty()) continue;
            if (seen.insert(g).second)
                group_anchor_.emplace_back(g, (int)i);
        }
    }

    // ------------------------------------------------------------------
    // Nav helpers
    // ------------------------------------------------------------------
    [[nodiscard]] int FocusedFiltered() const { return focused_filtered_; }
    [[nodiscard]] int FilteredCount() const {
        return (int)filtered_to_original_.size();
    }

    void EnsureFocusValid() {
        if (FilteredCount() == 0) { focused_filtered_ = 0; return; }
        focused_filtered_ = std::clamp(focused_filtered_, 0, FilteredCount() - 1);
        // If the currently focused slot is disabled, walk forward to the
        // first non-disabled.
        focused_filtered_ = FirstNonDisabled(focused_filtered_);
    }

    /// First non-disabled filtered index starting from `from` (inclusive).
    /// Wraps around once iff wrap_navigation and no on_down_from_last.
    [[nodiscard]] int FirstNonDisabled(int from) const {
        int n = FilteredCount();
        if (n == 0) return 0;
        bool can_wrap = opts_.wrap_navigation && !opts_.on_down_from_last;
        for (int i = 0; i < n; ++i) {
            int idx = (from + i) % n;
            if (can_wrap || (from + i) < n) {
                if (!opts_.options[filtered_to_original_[idx]].disabled)
                    return idx;
            }
        }
        return from; // all disabled, keep original
    }

    [[nodiscard]] int LastNonDisabled(int from) const {
        int n = FilteredCount();
        if (n == 0) return 0;
        bool can_wrap = opts_.wrap_navigation && !opts_.on_up_from_first;
        for (int i = 0; i < n; ++i) {
            int idx = (from - i + n * 2) % n;
            if (can_wrap || (from - i) >= 0) {
                if (!opts_.options[filtered_to_original_[idx]].disabled)
                    return idx;
            }
        }
        return from;
    }

    void EnsureFocusVisible() {
        int n = FilteredCount();
        int vc = std::max(1, std::min(opts_.visible_count,
                                      std::max(1, n)));
        if (focused_filtered_ < visible_start_) {
            visible_start_ = focused_filtered_;
        } else if (focused_filtered_ >= visible_start_ + vc) {
            visible_start_ = focused_filtered_ - vc + 1;
        }
        visible_start_ = std::clamp(visible_start_, 0, std::max(0, n - vc));
    }

    void MoveDown(int delta = 1) {
        int n = FilteredCount();
        if (n == 0) return;
        // If at last item
        if (focused_filtered_ == n - 1) {
            if (opts_.on_down_from_last) {
                opts_.on_down_from_last();
                return;
            }
            if (!opts_.wrap_navigation) return;
        }
        int next = (focused_filtered_ + delta) % n;
        // skip disabled
        for (int i = 0; i < n; ++i) {
            int idx = (focused_filtered_ + 1 + i) % n;
            if (!opts_.options[filtered_to_original_[idx]].disabled) {
                next = idx; break;
            }
        }
        focused_filtered_ = next;
        EnsureFocusVisible();
        FireOnFocus();
        (void)delta;
    }

    void MoveUp(int delta = 1) {
        int n = FilteredCount();
        if (n == 0) return;
        if (focused_filtered_ == 0) {
            if (opts_.on_up_from_first) {
                opts_.on_up_from_first();
                return;
            }
            if (!opts_.wrap_navigation) return;
        }
        int prev = (focused_filtered_ - delta + n * 2) % n;
        for (int i = 0; i < n; ++i) {
            int idx = (focused_filtered_ - 1 - i + n * 2) % n;
            if (!opts_.options[filtered_to_original_[idx]].disabled) {
                prev = idx; break;
            }
        }
        focused_filtered_ = prev;
        EnsureFocusVisible();
        FireOnFocus();
        (void)delta;
    }

    void MoveToStart() {
        if (FilteredCount() == 0) return;
        focused_filtered_ = FirstNonDisabled(0);
        EnsureFocusVisible();
        FireOnFocus();
    }

    void MoveToEnd() {
        if (FilteredCount() == 0) return;
        focused_filtered_ = LastNonDisabled(FilteredCount() - 1);
        EnsureFocusVisible();
        FireOnFocus();
    }

    void MovePageDown() {
        int vc = std::max(1, std::min(opts_.visible_count,
                                      std::max(1, FilteredCount())));
        int target = std::min(FilteredCount() - 1, focused_filtered_ + vc);
        focused_filtered_ = FirstNonDisabled(target);
        EnsureFocusVisible();
        FireOnFocus();
    }

    void MovePageUp() {
        int vc = std::max(1, std::min(opts_.visible_count,
                                      std::max(1, FilteredCount())));
        int target = std::max(0, focused_filtered_ - vc);
        focused_filtered_ = FirstNonDisabled(target);
        EnsureFocusVisible();
        FireOnFocus();
    }

    void JumpToNextGroup() {
        if (group_anchor_.empty()) return;
        // Binary search / linear: find first anchor.index > focused_filtered_
        for (const auto& [g, i] : group_anchor_) {
            if (i > focused_filtered_) {
                focused_filtered_ = FirstNonDisabled(i);
                EnsureFocusVisible();
                FireOnFocus();
                return;
            }
        }
        // wrap to first group
        focused_filtered_ = FirstNonDisabled(group_anchor_.front().second);
        EnsureFocusVisible();
        FireOnFocus();
    }

    void JumpToPrevGroup() {
        if (group_anchor_.empty()) return;
        int anchor = -1;
        for (const auto& [g, i] : group_anchor_) {
            if (i < focused_filtered_) anchor = i;
        }
        if (anchor < 0) anchor = group_anchor_.back().second;
        focused_filtered_ = FirstNonDisabled(anchor);
        EnsureFocusVisible();
        FireOnFocus();
    }

    // ------------------------------------------------------------------
    // Selection
    // ------------------------------------------------------------------
    void ToggleFocused() {
        if (FilteredCount() == 0) return;
        int orig = filtered_to_original_[focused_filtered_];
        const auto& opt = opts_.options[orig];
        if (opt.disabled) return;

        if (opts_.mode == SelectMode::Single) {
            single_selected_ = opt.value;
            if (opts_.on_change) opts_.on_change(opt.value);
            return;
        }
        // Multi: toggle
        if (IsSelectedBitset(orig))
            UnsetSelected(orig);
        else
            SetSelected(orig);
        if (opts_.on_change) opts_.on_change(opt.value);
    }

    void SelectAllMulti() {
        if (opts_.mode != SelectMode::Multi) return;
        for (int i : filtered_to_original_)
            if (!opts_.options[i].disabled) SetSelected(i);
        if (opts_.on_change && !opts_.options.empty())
            opts_.on_change(opts_.options[filtered_to_original_[
                std::min(focused_filtered_, FilteredCount()-1)]].value);
    }

    void InvertSelectionMulti() {
        if (opts_.mode != SelectMode::Multi) return;
        for (int i : filtered_to_original_) {
            if (opts_.options[i].disabled) continue;
            if (IsSelectedBitset(i)) UnsetSelected(i);
            else SetSelected(i);
        }
        if (opts_.on_change && !opts_.options.empty())
            opts_.on_change(opts_.options[filtered_to_original_[
                std::min(focused_filtered_, FilteredCount()-1)]].value);
    }

    // Bitset helpers -----------------------------------------------------
    [[nodiscard]] bool IsSelectedBitset(size_t orig) const {
        if (opts_.mode != SelectMode::Multi) return false;
        if (use_large_set_) return multi_selected_large_.count(orig) > 0;
        return orig < kMaxBitset && multi_selected_.test(orig);
    }
    void SetSelected(size_t orig) {
        if (use_large_set_) multi_selected_large_.insert(orig);
        else if (orig < kMaxBitset) multi_selected_.set(orig);
    }
    void UnsetSelected(size_t orig) {
        if (use_large_set_) multi_selected_large_.erase(orig);
        else if (orig < kMaxBitset) multi_selected_.reset(orig);
    }

    // ------------------------------------------------------------------
    // Event handling
    // ------------------------------------------------------------------
    [[nodiscard]] bool HandleListEvent(Event event) {
        if (opts_.disabled) return false;

        // --- Nav: up/down/j/k/Ctrl+N/Ctrl+P ---------------------------------
        if (event == Event::ArrowUp || event == Event::Character('k') ||
            event.input() == "ctrl+p") {
            MoveUp();
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j') ||
            event.input() == "ctrl+n") {
            MoveDown();
            return true;
        }

        // Home / End / g / G
        if (event == Event::Home || event == Event::Character('g')) {
            MoveToStart();
            return true;
        }
        if (event == Event::End || event == Event::Character('G')) {
            MoveToEnd();
            return true;
        }

        // PgUp / PgDn
        if (event == Event::PageUp) { MovePageUp(); return true; }
        if (event == Event::PageDown) { MovePageDown(); return true; }

        // Tab / Shift-Tab group jump
        if (event == Event::Tab) { JumpToNextGroup(); return true; }
        if (event == Event::TabReverse) { JumpToPrevGroup(); return true; }

        // a/A — select-all / invert (multi only)
        if (opts_.mode == SelectMode::Multi) {
            if (event == Event::Character('a')) {
                SelectAllMulti();
                return true;
            }
            if (event == Event::Character('A')) {
                InvertSelectionMulti();
                return true;
            }
            // Space — toggle without closing
            if (event == Event::Character(' ')) {
                ToggleFocused();
                return true;
            }
        }

        // Enter — semantics differ per mode
        if (event == Event::Return && !opts_.disable_selection) {
            if (FilteredCount() == 0) return true;
            int orig = filtered_to_original_[focused_filtered_];
            if (opts_.options[orig].disabled) return true;

            if (opts_.mode == SelectMode::Single) {
                // select + submit + (consumer closes)
                single_selected_ = opts_.options[orig].value;
                if (opts_.on_change) opts_.on_change(*single_selected_);
                if (opts_.on_submit_single) opts_.on_submit_single(*single_selected_);
            } else {
                // toggle, keep open
                if (IsSelectedBitset(orig)) UnsetSelected(orig);
                else SetSelected(orig);
                if (opts_.on_change) opts_.on_change(opts_.options[orig].value);
            }
            return true;
        }

        // Escape handled in outer wrapper (also clears search if focused).

        // Numeric quick-select 1..9
        if (opts_.show_indexes && event.is_character()) {
            char ch = event.character().empty() ? '\0' : event.character()[0];
            if (ch >= '1' && ch <= '9') {
                int idx = ch - '1';
                if (idx < FilteredCount()) {
                    focused_filtered_ = FirstNonDisabled(idx);
                    EnsureFocusVisible();
                    FireOnFocus();
                    if (!opts_.disable_selection) {
                        int orig = filtered_to_original_[focused_filtered_];
                        if (!opts_.options[orig].disabled) {
                            if (opts_.mode == SelectMode::Single) {
                                single_selected_ = opts_.options[orig].value;
                                if (opts_.on_change) opts_.on_change(*single_selected_);
                            } else {
                                if (IsSelectedBitset(orig)) UnsetSelected(orig);
                                else SetSelected(orig);
                                if (opts_.on_change)
                                    opts_.on_change(opts_.options[orig].value);
                            }
                        }
                    }
                    return true;
                }
            }
        }

        return false;
    }

    // ------------------------------------------------------------------
    // Rendering
    // ------------------------------------------------------------------
    void FireOnFocus() const {
        if (!opts_.on_focus) return;
        auto v = FocusedValue();
        if (v.has_value()) opts_.on_focus(*v);
    }

    [[nodiscard]] Element RenderList() const {
        int n = FilteredCount();
        int vc = std::max(1, std::min(opts_.visible_count, std::max(1, n)));

        // Compute visible window (clamp to available rows).
        int start = visible_start_;
        int end = std::min(start + vc, n);
        int actual_rows = end - start;

        // Always show exactly vc rows so the scrollbar is stable; pad with
        // empty lines at the end if the last page is shorter than vc.
        int pad_after = vc - actual_rows;

        Elements body;
        body.reserve(vc + 2);

        // Header: search-result count summary
        if (!search_text_.empty() || !group_anchor_.empty()) {
            auto header_parts = Elements{};
            header_parts.push_back(text(std::format(
                " {} of {} option{}",
                n,
                (int)opts_.options.size(),
                (int)opts_.options.size() == 1 ? "" : "s"))
                | dim);
            if (!search_text_.empty()) {
                header_parts.push_back(text("  "));
                header_parts.push_back(text(std::format("filter: '{}'", search_text_))
                    | color(Color::Cyan) | dim);
            }
            body.push_back(hbox(std::move(header_parts)));
            body.push_back(separator());
        }

        if (n == 0) {
            body.push_back(text("  No results, try different keywords")
                            | color(Color::Yellow) | dim);
            // Fill remaining vertical slots
            for (int i = 0; i < vc - 1; ++i)
                body.push_back(text(""));
            return vbox(std::move(body));
        }

        // Track previous group for separator insertion.
        std::string prev_group;
        Elements option_rows;
        option_rows.reserve(actual_rows);

        for (int fi = start; fi < end; ++fi) {
            int orig = filtered_to_original_[fi];
            const auto& opt = opts_.options[orig];

            // Group header + separator when group changes.
            if (!opt.group.empty() && opt.group != prev_group) {
                if (!prev_group.empty() || fi != start)
                    option_rows.push_back(separator() | dim);
                option_rows.push_back(hbox({
                    text(" ◆ ") | color(Color::Blue),
                    text(opt.group) | bold | color(Color::BlueLight),
                }));
                prev_group = opt.group;
            } else if (opt.group.empty() && !prev_group.empty()) {
                option_rows.push_back(separator() | dim);
                prev_group.clear();
            }

            bool hovered = (fi == focused_filtered_);
            bool selected =
                (opts_.mode == SelectMode::Single)
                    ? (single_selected_.has_value() &&
                       *single_selected_ == opt.value)
                    : IsSelectedBitset(orig);

            Element row;
            if (opts_.custom_render) {
                row = opts_.custom_render(opt, hovered, selected, orig);
            } else {
                row = RenderOptionRow(opt, hovered, selected, fi + 1);
            }
            option_rows.push_back(row);

            // Expanded layout: add description below on its own line.
            if (opts_.layout == SelectLayout::Expanded &&
                !opts_.inline_descriptions &&
                !opt.description.empty()) {
                auto desc = text("     " + opt.description)
                    | color(Color::GrayLight) | dim;
                option_rows.push_back(std::move(desc));
                if (fi < end - 1) option_rows.push_back(text(""));
            }
        }

        // Pad the end with empties so the list has a fixed height.
        for (int i = 0; i < pad_after; ++i) option_rows.push_back(text(""));

        // Build the scrollbar alongside the option rows.
        Elements bar = RenderScrollBar(
            (int)option_rows.size(),
            n <= 1 ? 0.0
                   : (double)start / (double)(n - std::min(vc, n)),
            n == 0 ? 1.0
                   : (double)std::min(vc, n) / (double)n);

        // Pair each row with a bar cell.
        Elements combined;
        combined.reserve(option_rows.size());
        for (size_t i = 0; i < option_rows.size(); ++i) {
            Element bar_cell = (i < bar.size()) ? bar[i] : filler();
            combined.push_back(hbox({
                option_rows[i],
                bar_cell | align_right,
            }) | yflex_grow);
        }

        // Prepend summary header
        body.insert(body.end(),
                    std::make_move_iterator(combined.begin()),
                    std::make_move_iterator(combined.end()));

        // Footer: position indicator + shortcut hints
        Elements footer_bits;
        footer_bits.push_back(text(std::format(
            " {}-{} / {}",
            n == 0 ? 0 : start + 1,
            n == 0 ? 0 : std::min(end, n),
            n)) | dim);
        if (opts_.mode == SelectMode::Multi) {
            int sel = (int)SelectedValues().size();
            footer_bits.push_back(text(std::format("  ·  {} selected", sel))
                                  | color(Color::Green) | dim);
        }
        footer_bits.push_back(text("  ·  ↑↓j/k Nav  ⏎ Select  Space Toggle  "
                                   "a/A All/Invert  / Search  g/G Home/End  "
                                   "Tab Groups  Esc Cancel")
                              | dim);
        body.push_back(separator());
        body.push_back(hbox(std::move(footer_bits)));

        return vbox(std::move(body));
    }

    [[nodiscard]] Element RenderOptionRow(const SelectOption& opt,
                                          bool hovered, bool selected,
                                          int display_idx_1based) const {
        Elements parts;
        parts.reserve(8);

        // Selection / hover gutter
        if (selected)
            parts.push_back(text(" ✓ ") | color(Color::Green) | bold);
        else if (hovered)
            parts.push_back(text(" › ") | color(Color::Cyan) | bold);
        else
            parts.push_back(text("   "));

        // Icon (if any)
        if (!opt.icon.empty()) {
            parts.push_back(text(opt.icon));
            parts.push_back(text(" "));
        }

        // Index
        if (opts_.show_indexes) {
            parts.push_back(text(std::format("{}.", display_idx_1based)) | dim);
            parts.push_back(text(" "));
        }

        // Label with search highlighting
        auto label_el = RenderHighlightedLabel(opt.label, search_text_);
        if (opt.disabled) label_el = label_el | dim;
        else if (hovered) label_el = label_el | bold;
        if (selected) label_el = label_el | color(Color::GreenLight);
        parts.push_back(label_el);

        // Inline description
        if (opts_.inline_descriptions && !opt.description.empty()) {
            auto desc = text(" — " + opt.description);
            if (opt.dim_description) desc = desc | dim;
            else desc = desc | color(Color::GrayLight);
            parts.push_back(desc);
        }

        // Disabled badge
        if (opt.disabled) {
            parts.push_back(text("  [disabled]") | color(Color::Red) | dim);
        }

        Element row = hbox(std::move(parts));
        if (hovered) row = row | bgcolor(Color::RGB(25, 35, 50));
        return row;
    }
};

// ============================================================
// Factories — the intended external API.
// ============================================================

namespace detail {
/// Concrete holder used by MakeCustomSelect.  Defined at namespace scope
/// (rather than inside the function) so its vtable is reliably emitted in
/// this module's object file.
struct Holder : CustomSelectImpl {
    using CustomSelectImpl::CustomSelectImpl;
    ~Holder() override;
};
Holder::~Holder() = default;
} // namespace detail

/// Build a CustomSelect component. Returns a pair:
///   - the FTXUI Component (to insert into your tree)
///   - a shared handle to submit / query state externally.
[[nodiscard]] inline std::pair<Component, std::shared_ptr<CustomSelectHandle>>
MakeCustomSelect(CustomSelectOptions opts) {
    // Construct via shared_ptr directly. CustomSelectHandle is not
    // enable_shared_from_this itself, so we build the ptr and pass it in.
    auto impl = std::make_shared<detail::Holder>(std::move(opts));
    auto comp = impl->BuildComponent();
    return {std::move(comp), std::static_pointer_cast<CustomSelectHandle>(impl)};
}

/// Single-select convenience. Matches TS `select.tsx` default usage.
/// Caller gets the selected value via on_submit (fired on Enter).
[[nodiscard]] inline Component MakeSingleSelect(
    std::vector<SelectOption> options,
    std::function<void(const std::string& value)> on_submit,
    std::function<void()> on_cancel = nullptr) {

    CustomSelectOptions opts;
    opts.options = std::move(options);
    opts.mode = SelectMode::Single;
    opts.on_submit_single = std::move(on_submit);
    opts.on_cancel = std::move(on_cancel);
    return MakeCustomSelect(std::move(opts)).first;
}

/// Multi-select convenience. Matches TS SelectMulti.tsx usage.
/// Caller gets values via on_submit_multi — call handle->Submit() from an
/// OK button to fire it, or rely on on_change for live updates.
[[nodiscard]] inline std::pair<Component, std::shared_ptr<CustomSelectHandle>>
MakeMultiSelect(
    std::vector<SelectOption> options,
    std::function<void(const std::vector<std::string>& values)> on_submit,
    std::function<void(const std::string& changed_value)> on_change = nullptr,
    std::function<void()> on_cancel = nullptr) {

    CustomSelectOptions opts;
    opts.options = std::move(options);
    opts.mode = SelectMode::Multi;
    opts.on_submit_multi = std::move(on_submit);
    opts.on_change = std::move(on_change);
    opts.on_cancel = std::move(on_cancel);
    return MakeCustomSelect(std::move(opts));
}

// ============================================================
// Backward-compatible API (from the original 345-LoC skeleton).
// Consumers that depend on `Select` / `SimpleSelect` keep compiling.
// ============================================================

/// Legacy layout enum kept for compatibility. New code should use
/// CustomSelectOptions + MakeCustomSelect / MakeSingleSelect / MakeMultiSelect.
enum class [[deprecated("Use SelectLayout from CustomSelectOptions directly")]]
    SelectLayoutLegacy : std::uint8_t {
    Compact = (std::uint8_t)SelectLayout::Compact,
    Expanded = (std::uint8_t)SelectLayout::Expanded,
    CompactVertical = (std::uint8_t)SelectLayout::CompactVertical,
};

/// Legacy Props struct — maps to CustomSelectOptions internally.
struct SelectProps {
    std::vector<SelectOption> options;
    std::optional<std::string> default_value;
    bool is_disabled = false;
    bool disable_selection = false;
    bool hide_indexes = false;
    bool multi_select = false;
    bool inline_descriptions = false;
    int visible_option_count = 5;
    std::string highlight_text;
    SelectLayout layout = SelectLayout::Compact;

    std::function<void(const std::string& value)> on_change;
    std::function<void(const std::string& value)> on_focus;
    std::function<void()> on_cancel;
    std::function<void()> on_up_from_first;
    std::function<void()> on_down_from_last;
};

/// Legacy interactive select (API-compatible with the v1 skeleton).
[[nodiscard]] inline Component Select(SelectProps props) {
    CustomSelectOptions o;
    o.options = std::move(props.options);
    o.mode = props.multi_select ? SelectMode::Multi : SelectMode::Single;
    o.layout = props.layout;
    o.visible_count = props.visible_option_count;
    o.default_value = props.default_value;
    o.show_indexes = !props.hide_indexes;
    o.inline_descriptions = props.inline_descriptions;
    o.disabled = props.is_disabled;
    o.disable_selection = props.disable_selection;
    o.on_change = std::move(props.on_change);
    o.on_focus = std::move(props.on_focus);
    o.on_cancel = std::move(props.on_cancel);
    o.on_up_from_first = std::move(props.on_up_from_first);
    o.on_down_from_last = std::move(props.on_down_from_last);
    o.initial_search = std::move(props.highlight_text);

    // Multi-mode legacy: Enter toggles (like new semantics).
    // Single-mode legacy: Enter calls on_change (not on_submit_single).
    // Wire on_submit_single → on_change to match old `Select` semantics.
    if (o.mode == SelectMode::Single && o.on_change != nullptr) {
        auto cb = o.on_change;
        o.on_submit_single = [cb](const std::string& v) { cb(v); };
    }
    return MakeCustomSelect(std::move(o)).first;
}

/// Legacy convenience: simple single-select.
[[nodiscard]] inline Component SimpleSelect(
    std::vector<SelectOption> options,
    std::function<void(const std::string&)> on_select,
    std::function<void()> on_cancel) {
    return MakeSingleSelect(std::move(options), std::move(on_select),
                            std::move(on_cancel));
}

} // namespace cc::ui::custom_select
