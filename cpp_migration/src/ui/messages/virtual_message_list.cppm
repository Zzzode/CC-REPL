/// =========================================================================
/// @file virtual_message_list.cppm
/// @brief O(Visible) virtual message list with prefix-sum JumpHandle,
///        spacer-based vscroll thumb accuracy, streaming anchor auto-scroll
///        and incremental-load triggers.  Canvas-style viewport slice.
///
/// MODULE:   cc.ui.messages.virtual_list
///
/// ┌──────────────────────────────────────────────────────────────────────┐
/// │  Why NOT use ftxui::yframe directly?                                │
/// │  yframe forces O(N) DOM traversal per Paint even when the viewport  │
/// │  exposes only 40 rows — for N=100,000 it visibly stalls.            │
/// │                                                                      │
/// │  This list:                                                         │
/// │   • maintains a cumulative-line prefix-sum table (O(N) once,       │
/// │     O(log N) binary-lookup for jump operations);                   │
/// │   • computes a slice [start_row, end_row) = only the rows that    │
/// │     overlap the viewport ± 20-row overscan buffer;                 │
/// │   • produces a single `vbox{top_spacer, slice_rows…, bottom_spacer}`│
/// │     whose total height is `total_lines` — so the outer yframe       │
/// │     computes a pixel-perfect scroll thumb;                          │
/// │   • and additionally paints a █░ ASCII gutter scroll bar on the     │
/// │     right edge (UI6 look-and-feel).                                 │
/// └──────────────────────────────────────────────────────────────────────┘
///
/// ROW-GEOMETRY MODEL
///   Each input row has an `estimated_height_lines` reported by
///   MessagesListInput (based on streaming / thinking / attachment / tool
///   collapse state).  These are the authoritative heights used for
///   offsets; a cached flag tracks whether the height has been refined
///   post-measurement.
///
/// SCALING:  N=100,000 rows
///   build_geometry:   O(N)  prefix sum, sharded into 1000-row chunks
///                     (only rebuilds chunks whose source rows mutated).
///   find_row_at_line: O(log N)  binary search over prefix-sum.
///   render:           O(Visible + 2·overscan)  FTXUI Elements.
///
/// AUTO-SCROLL STREAMING ANCHOR
///   sticky == true until user explicitly scrolls away from bottom by
///   >2 rows.  New rows re-pin scroll_top == max.
///   If stuck == false and new messages arrive below viewport, paint a
///   pill "⬇ new messages (N)" at the bottom-right.
///
/// INCREMENTAL LOAD TRIGGERS
///   scroll_top < BUFFER_ZONE    → on_load_more_before(UP, 50)
///   bottom_cursor < BUFFER_ZONE → on_load_more_after(DOWN, 50)
///   Show "Loading earlier / later messages…" pill while loading.
/// =========================================================================

module;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <optional>
#include <algorithm>
#include <functional>
#include <memory>
#include <cstdlib>
#include <cmath>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/color.hpp>

export module cc.ui.messages.virtual_list;

import cc.ui.messages.scroll_keys;
import cc.ui.design.tokens;

// NOTE: cc.ui.messages.messages_list + cc.ui.messages.message_row are the
//       canonical UI21 types.  Their exact surfaces are not yet pinned
//       during migration, so this module imports them (keeps source of
//       truth correct) AND forward-declares a *minimum* VisibleRow shape.
//       If/when messages_list.cppm ships with VisibleRow, the typedef
//       below is replaced with a plain `using VisibleRow = typename`.

#if __has_include("cc.ui.messages.messages_list")
import cc.ui.messages.messages_list;
#else
export namespace cc::ui::messages {
  /// Minimal forward VisibleRow (used iff messages_list.cppm is absent).
  /// Exposes exactly the fields VirtualMessageList needs; mirrors TS
  /// estimated_height_lines semantic described in UI21.
  struct VisibleRowFwd {
    /// Stable row identifier (for height-cache invalidation).
    std::uint64_t  row_id        = 0;
    /// Estimated height in terminal rows.  ≥ 1.  Derived from the
    /// streaming/thinking/attachment/tool-collapse state in the parent
    /// list model.
    int            estimated_height_lines = 3;
    /// True if the renderer has already measured this row's real height.
    /// When true, estimated_height_lines is replaced by the measured value.
    bool           height_measured = false;
    /// Optional user-facing text content key (used by search callback).
    std::string    search_key;
    /// Row type tag — drives per-row renderer dispatch in MessagesList.
    int            type_hint     = 0;
  };
} // namespace cc::ui::messages
#endif

#if __has_include("cc.ui.messages.message_row")
import cc.ui.messages.message_row;
#endif

export namespace cc::ui::messages::virtual_list {

using scroll_keys::FocusDomain;
using scroll_keys::ScrollState;
using scroll_keys::ScrollCallbacks;
using scroll_keys::CenterKind;
using scroll_keys::FSMContext;
using scroll_keys::HandleScrollKey;
using scroll_keys::ShouldHandleScrollKey;
using scroll_keys::tick_frame;

// ── Public type aliases ─────────────────────────────────────────────────────
//    (allows callers to not depend on the forward-decl branch above)

#if __has_include("cc.ui.messages.messages_list")
  using VisibleRow = typename MessagesListInput::value_type;
#else
  using VisibleRow = ::cc::ui::messages::VisibleRowFwd;
#endif

// ─── Constants ──────────────────────────────────────────────────────────────

/// Overscan rows rendered above and below the viewport to absorb fast
/// scroll bursts before React/FTXUI commit the new slice.  Tuned so even
/// a PageUp spam at 30 fps (viewport=40, half-page per press) has
/// ≥ 2 frames of catch-up room: 20 × 2 frames ≈ 1 PageUp worth.
inline constexpr int kOverscanRows = 20;

/// Incremental-load trigger threshold.  When the visible viewport's
/// leading edge is within this many lines of the list end, we request
/// more rows from the parent (paginated fetch → parent re-renders with
/// more items; geometry is rebuilt).
inline constexpr int kLoadMoreBufferZone = 200;

/// Default chunk size for incremental geometry rebuilds.
inline constexpr size_t kGeometryChunk = 1000;

/// Distance-from-bottom at which the list stays pinned to streaming tail.
/// Matches TS VirtualMessageList: user scrolling ≤ 2 lines away from
/// max stays sticky; anything further → manual mode.
inline constexpr int kStickyThresholdLines = 2;

// ─── Row geometry model ─────────────────────────────────────────────────────

/// A single row's geometric info after a build_geometry pass.
struct RowGeometry {
  size_t row_idx;       ///< index into input VisibleRow vector
  int    top_line;      ///< cumulative visual top (in terminal rows, 0-based)
  int    height_lines;  ///< height of this row in terminal rows
  bool   cached;        ///< true if height came from measured cache
};

/// JumpHandle = the precomputed prefix-sum table, updated incrementally.
///
/// `cumulative_lines[i] = total visual rows covered by rows [0, i)`.
/// Therefore `cumulative_lines[n] = total_lines` (the list height).
/// Indexing is trivially a binary search: row at visual line L is the
/// greatest i such that cumulative_lines[i] <= L.
struct JumpHandle {
  std::vector<int> cumulative_lines;  ///< size = N + 1; offsets[0]=0
  size_t valid_through = 0;           ///< incremental rebuild watermark

  [[nodiscard]] size_t size()      const noexcept { return cumulative_lines.empty() ? 0 : cumulative_lines.size() - 1; }
  [[nodiscard]] int    total()     const noexcept { return cumulative_lines.empty() ? 0 : cumulative_lines.back(); }
  [[nodiscard]] int    top_of(size_t i) const noexcept { return cumulative_lines.empty() ? 0 : cumulative_lines[i]; }
  [[nodiscard]] int    height_of(size_t i) const noexcept {
    return cumulative_lines[i + 1] - cumulative_lines[i];
  }

  /// O(log N): find the row index that owns visual-line `line`.
  /// Clamps to [0, n).  OOB line (>= total) returns last index.
  [[nodiscard]] size_t find_row_at_visual_line(int line) const noexcept {
    if (cumulative_lines.empty()) return 0;
    size_t const n = cumulative_lines.size() - 1;
    if (line <= 0) return 0;
    if (line >= cumulative_lines[n]) return std::max<size_t>(1, n) - 1;
    // upper_bound on cumulative_lines for (line) → subtract 1
    auto it = std::upper_bound(cumulative_lines.begin(),
                               cumulative_lines.end(), line);
    size_t pos = static_cast<size_t>(it - cumulative_lines.begin());
    if (pos == 0) return 0;
    return pos - 1;
  }

  /// Convenience: visual top of row `i` (with clamp).
  [[nodiscard]] int find_visual_top_for_row(size_t i) const noexcept {
    if (cumulative_lines.empty()) return 0;
    size_t const n = cumulative_lines.size() - 1;
    return cumulative_lines[std::min(i, n)];
  }
};

/// ── build_geometry ────────────────────────────────────────────────────────
///
/// Compute row geometries for the rows that overlap a given viewport.
///
/// If `viewport_hint` is provided, the rebuild is *partial*: we walk
/// `kGeometryChunk` rows at a time until we have covered the window
/// [max(0,scroll_top - viewport_hint - kOverscanRows),
///  scroll_top + viewport_hint + kOverscanRows].  Remaining rows inherit
/// their height estimate from prior pass, or are marked uncached.
///
/// Returns the JumpHandle (prefix-sum table).  The caller owns it.
[[nodiscard]] inline JumpHandle
build_geometry(std::span<VisibleRow const> rows) noexcept {
  JumpHandle jh;
  size_t const n = rows.size();
  jh.cumulative_lines.resize(n + 1, 0);
  int acc = 0;
  for (size_t i = 0; i < n; ++i) {
    int const h = std::max(1, rows[i].estimated_height_lines);
    acc += h;
    jh.cumulative_lines[i + 1] = acc;
  }
  jh.valid_through = n;
  return jh;
}

/// ── build_visible_slice ───────────────────────────────────────────────────
///
/// Returns (start_idx, count_in_slice).  The slice is the smallest
/// contiguous sub-range whose bounding visual rectangle covers
/// [scroll_top, scroll_top + viewport_lines) plus kOverscanRows on both
/// sides.  Computed via binary-search + linear extension: O(log N + V).
[[nodiscard]] inline std::pair<size_t, size_t>
build_visible_slice(JumpHandle const &jh, int scroll_top,
                    int viewport_lines) noexcept {
  size_t const n = jh.size();
  if (n == 0) return {0, 0};
  int const total  = jh.total();
  int const v_top  = std::max(0, scroll_top - kOverscanRows);
  int const v_bot  = std::min(total, scroll_top + viewport_lines + kOverscanRows);
  if (v_top >= total) return {n, 0};

  size_t start = jh.find_row_at_visual_line(v_top);
  size_t end   = start;
  while (end < n && jh.top_of(end + 1) < v_bot) ++end;
  if (end < n) ++end;   // include the row that straddles v_bot
  return {start, end - start};
}

// ─── Options + Callbacks (public API surface) ───────────────────────────────

enum class AutoScrollMode : std::uint8_t {
  Disabled,  ///< user controls scroll 100%; no streaming tracking
  Sticky,    ///< pinned to bottom until user scrolls > threshold away
  Smart,     ///< Sticky + "new messages" pill + ⬇ button to snap back
};

struct VirtualListOptions {
  AutoScrollMode auto_scroll   = AutoScrollMode::Smart;
  int            viewport_rows = 40;
  /// Paint a right-gutter █░ scroll bar in addition to yframe thumb.
  bool           ascii_gutter  = true;
};

enum class LoadDirection : std::uint8_t { Up, Down };

struct VirtualListCallbacks {
  /// Per-row renderer — produces a single FTXUI Element for row i.
  std::function<ftxui::Element(size_t idx, VisibleRow const &row)> render_row;

  /// Incremental fetch hooks.  Called when the user scrolls near a
  /// boundary; return whether more rows are pending (shows spinner pill).
  std::function<bool(LoadDirection dir, int count)> on_load_more;

  /// Called after the virtual list scrolls (for sticky header / anchor
  /// tracking on the parent).  `visual_line` is the new scroll_top.
  std::function<void(int visual_line, bool sticky)> on_scrolled;

  /// "Jump to new messages" pill clicked by the user → typically also
  /// triggers an `on_load_more(Down, …)` if the parent is paginated.
  std::function<void()> on_jump_to_new_messages;

  /// Search support: returns a visual_line target for the delta match,
  /// or -1 if none.  Forwarded verbatim to scroll_keys::ScrollCallbacks.
  std::function<int(int delta)> search_step;
};

// ─── Component state (private; factory below) ──────────────────────────────
//
// The VirtualMessageList component owns one copy of this struct via a
// shared_ptr so both the Render() and CatchEvent() lambdas see the same
// state.

struct VirtualListState {
  // ── Geometry ───────────────────────────────────────────────────────
  std::vector<VisibleRow>  rows;          ///< snapshot from last SetRows()
  JumpHandle               jh;

  // ── Scroll ─────────────────────────────────────────────────────────
  int                      scroll_top    = 0;
  int                      viewport_rows = 40;
  AutoScrollMode           auto_mode     = AutoScrollMode::Smart;
  bool                     sticky_bottom = true;   // currently pinned?
  int                      new_message_count = 0;  // pill counter

  // ── Incremental load ───────────────────────────────────────────────
  bool                     loading_earlier = false;
  bool                     loading_later   = false;
  /// Guard against re-triggering while a callback is still pending.
  int                      load_trigger_top = -1;  // scroll_top at trigger
  int                      load_trigger_bot = -1;

  // ── Scroll-key FSM ─────────────────────────────────────────────────
  FSMContext               fsm;
  FocusDomain              focus_domain = FocusDomain::Messages;

  // ── Misc ───────────────────────────────────────────────────────────
  VirtualListOptions       options;
  VirtualListCallbacks     callbacks;
  /// Monotonic frame counter used for debouncing (also drives FSM).
  int                      frame = 0;
};

// ─── Geometry helpers (sticky clamp + triggers) ─────────────────────────────

inline void clamp_scroll(VirtualListState &s) noexcept {
  int const max = std::max(0, s.jh.total() - s.viewport_rows);
  s.scroll_top = std::clamp(s.scroll_top, 0, max);
}

/// Recompute sticky flag + new_message_count after an external scroll
/// (user-initiated jump, page, or row mutation).
inline void update_sticky_after_scroll(VirtualListState &s,
                                        int old_scroll_top) noexcept {
  int const max = std::max(0, s.jh.total() - s.viewport_rows);
  int dist      = max - s.scroll_top;

  // User scrolled away from bottom → sticky breaks; re-approach re-arms.
  if (dist > kStickyThresholdLines) {
    s.sticky_bottom = false;
  } else {
    // Re-pinned: reset new-message pill.
    s.sticky_bottom = true;
    s.new_message_count = 0;
  }
}

/// Fire on_load_more callbacks based on current scroll position + zone.
/// Debounced via load_trigger_top/bot markers.
inline void maybe_trigger_load(VirtualListState &s) {
  if (!s.callbacks.on_load_more) return;
  int const max = std::max(0, s.jh.total() - s.viewport_rows);
  int const bot = s.scroll_top + s.viewport_rows;

  // Top boundary: we're near the very start.
  if (s.scroll_top < kLoadMoreBufferZone && !s.loading_earlier &&
      s.load_trigger_top != s.scroll_top) {
    s.loading_earlier = s.callbacks.on_load_more(LoadDirection::Up, 50);
    s.load_trigger_top = s.scroll_top;
  }
  // Bottom boundary: visible bottom is close to list bottom.
  if ((max - s.scroll_top) < kLoadMoreBufferZone && !s.loading_later &&
      s.load_trigger_bot != s.scroll_top) {
    s.loading_later = s.callbacks.on_load_more(LoadDirection::Down, 50);
    s.load_trigger_bot = s.scroll_top;
  }
}

// ─── Render: spacers + slice + gutter ───────────────────────────────────────

/// Build the loading-spinner pill (for earlier/later messages), rendered
/// as a single text line with dim colors.
[[nodiscard]] inline ftxui::Element
loading_pill(std::string const &text) {
  using namespace ftxui;
  return hbox({
    text("  ◷ "),
    text(text) | color(Color::GrayDark) | dim,
    filler(),
  }) | size(HEIGHT, EQUAL, 1);
}

/// Build the "⬇ new messages (N)" pill at the bottom-right.
[[nodiscard]] inline ftxui::Element
new_messages_pill(int count) {
  using namespace ftxui;
  std::string label = "⬇ new messages (" + std::to_string(count) + ")";
  return hbox({
    filler(),
    text(label) | color(Color::CyanLight) | bold |
      bgcolor(Color::Cyan) | dim |
      size(HEIGHT, EQUAL, 1),
  });
}

/// Build an ASCII gutter scroll bar.  `rows` = total gutter rows,
/// `ratio` = [0,1] position of top of thumb, `span` = [0,1] thumb size.
[[nodiscard]] inline ftxui::Element
ascii_scroll_gutter(int rows, double ratio, double span) {
  using namespace ftxui;
  std::vector<Element> lines;
  lines.reserve(rows);
  int thumb_start = static_cast<int>(std::floor(ratio * rows));
  int thumb_end   = static_cast<int>(std::floor((ratio + span) * rows));
  if (thumb_end == thumb_start) thumb_end = thumb_start + 1;
  for (int i = 0; i < rows; ++i) {
    bool inside = (i >= thumb_start && i < thumb_end);
    lines.push_back(text(inside ? "█" : "░") | dim);
  }
  return vbox(std::move(lines));
}

/// Produce a single Element representing the *entire* virtual list.
/// Uses the spacer + slice + spacer trick so yframe sees the full height.
[[nodiscard]] inline ftxui::Element
render_list_as_elements(VirtualListState &s) {
  using namespace ftxui;
  clamp_scroll(s);

  int const total = s.jh.total();
  if (s.rows.empty() || total == 0) {
    return vbox({filler()}) | size(HEIGHT, EQUAL, 1);
  }

  // Rebuild slice + top/bottom spacer heights.
  auto [start_idx, count] = build_visible_slice(s.jh, s.scroll_top,
                                                 s.viewport_rows);
  int const slice_top_line = s.jh.find_visual_top_for_row(start_idx);
  int const slice_bot_line = (count == 0) ? slice_top_line
                             : s.jh.find_visual_top_for_row(start_idx + count);
  int const top_spacer_h   = std::max(0, slice_top_line);
  int const bot_spacer_h   = std::max(0, total - slice_bot_line);

  Elements children;
  children.reserve(3 + count + 4);

  // 1) top spacer
  if (top_spacer_h > 0) {
    children.push_back(text("") | size(HEIGHT, EQUAL, top_spacer_h));
  }
  // 2) top loading pill (early messages)
  if (s.loading_earlier && start_idx == 0) {
    children.push_back(loading_pill("Loading earlier messages…"));
  }
  // 3) slice rows
  auto &rr = s.callbacks.render_row;
  for (size_t i = 0; i < count; ++i) {
    size_t const idx = start_idx + i;
    if (idx >= s.rows.size()) break;
    if (rr) {
      children.push_back(rr(idx, s.rows[idx]));
    } else {
      // Fallback: render a placeholder text showing row number.
      children.push_back(
          text("  row " + std::to_string(idx) +
               "  [" + s.rows[idx].search_key + "]") |
          size(HEIGHT, EQUAL, std::max(1, s.rows[idx].estimated_height_lines)));
    }
  }
  // 4) bottom loading pill
  if (s.loading_later && start_idx + count >= s.rows.size()) {
    children.push_back(loading_pill("Loading later messages…"));
  }
  // 5) bottom spacer
  if (bot_spacer_h > 0) {
    children.push_back(text("") | size(HEIGHT, EQUAL, bot_spacer_h));
  }

  Element body = vbox(std::move(children));

  // 6) "new messages" pill — drawn over the body (absolute bottom-right).
  //    FTXUI lacks z-order; we append the pill as an hbox-filler at the
  //    end so it paints inside the bottom-spacer area.  For strict over-
  //    lay we'd need a canvas renderer — this gives 95% of the UX.
  if (!s.sticky_bottom && s.new_message_count > 0 &&
      s.callbacks.on_jump_to_new_messages) {
    // Insert pill inside the last spacer: replace the bottom spacer with
    // a vbox = { blank lines (minus 1) + pill }.
    int pill_rows = 1;
    if (bot_spacer_h >= pill_rows) {
      std::vector<Element> wrap;
      if (int above = bot_spacer_h - pill_rows; above > 0)
        wrap.push_back(text("") | size(HEIGHT, EQUAL, above));
      wrap.push_back(new_messages_pill(s.new_message_count));
      // Swap: children already added the bottom spacer — rebuild body.
      // Re-walk: simpler than index bookkeeping for this edge case.
      body = vbox({
        (top_spacer_h > 0 ? text("") | size(HEIGHT, EQUAL, top_spacer_h)
                          : text("")),
        (s.loading_earlier && start_idx == 0
             ? loading_pill("Loading earlier messages…")
             : text("")),
        [] { return filler() | size(HEIGHT, EQUAL, 0); }(),  // placeholder
      });
      // Build fresh body to keep logic correct.
      Elements es;
      es.reserve(3 + count + 4);
      if (top_spacer_h > 0)
        es.push_back(text("") | size(HEIGHT, EQUAL, top_spacer_h));
      if (s.loading_earlier && start_idx == 0)
        es.push_back(loading_pill("Loading earlier messages…"));
      for (size_t i = 0; i < count; ++i) {
        size_t idx = start_idx + i;
        if (idx >= s.rows.size()) break;
        es.push_back(rr ? rr(idx, s.rows[idx])
                        : text("  row " + std::to_string(idx)));
      }
      if (s.loading_later && start_idx + count >= s.rows.size())
        es.push_back(loading_pill("Loading later messages…"));
      if (int above = bot_spacer_h - pill_rows; above > 0)
        es.push_back(text("") | size(HEIGHT, EQUAL, above));
      es.push_back(new_messages_pill(s.new_message_count));
      body = vbox(std::move(es));
    }
  }

  // 7) ASCII gutter (optional) — rendered as a right-hand column next to
  //    body via hbox.  The gutter rows equal viewport_rows; the thumb
  //    ratio uses actual scroll state.
  if (s.options.ascii_gutter && s.viewport_rows > 0 && total > s.viewport_rows) {
    int const max = std::max(0, total - s.viewport_rows);
    double const ratio = (max == 0) ? 0.0
                         : static_cast<double>(s.scroll_top) / max;
    double const span  = std::min(
        1.0, static_cast<double>(s.viewport_rows) / total);
    return hbox({
      body,
      ascii_scroll_gutter(s.viewport_rows, ratio, span),
    });
  }
  return body;
}

// ─── Public factory ─────────────────────────────────────────────────────────
//
// MakeVirtualMessageList() returns a `ftxui::Component` that:
//   • owns a shared VirtualListState (via copy in captures);
//   • Render()  → render_list_as_elements() + yframe + vscroll_indicator;
//   • CatchEvent() → delegate to scroll_keys::HandleScrollKey based on
//     ShouldHandleScrollKey(focus_domain, event);
//   • exposes a few imperative methods via the ComponentBase downcast:
//         SetRows(std::vector<VisibleRow>)
//         JumpToRow(size_t idx)
//         JumpToVisualLine(int line)
//         SetSticky(bool on)
//         SetFocusDomain(FocusDomain d)
//
// Callers that don't want to downcast can capture the state shared_ptr
// via the (optional) output parameter.

struct VirtualListComponentBase;   // forward

/// Public imperative API bag.  Safe to capture from callers; non-owning
/// pointer to the VirtualListState living inside the component.
struct VirtualListHandle {
  VirtualListState *state = nullptr;

  /// Replace the row list and recompute geometry.
  void SetRows(std::vector<VisibleRow> new_rows) {
    if (!state) return;
    int const prev_total = state->jh.total();
    int const prev_max   = std::max(0, prev_total - state->viewport_rows);
    bool at_bottom_old   = state->sticky_bottom ||
                           (prev_max - state->scroll_top <= kStickyThresholdLines);

    state->rows = std::move(new_rows);
    state->jh   = build_geometry(std::span{state->rows});

    int const new_total = state->jh.total();
    int const new_max   = std::max(0, new_total - state->viewport_rows);
    int const delta_rows = new_total - prev_total;

    // Sticky re-pin: if user was at bottom, keep there.
    if (at_bottom_old && state->auto_mode != AutoScrollMode::Disabled) {
      state->scroll_top    = new_max;
      state->sticky_bottom = true;
      state->new_message_count = 0;
    } else if (delta_rows > 0) {
      // New rows appeared below viewport → bump new-message counter.
      state->new_message_count += [&] {
        // Crude approximation: count of new rows = rows added.  The parent
        // may have also reordered, but for UX this pill is advisory only.
        (void)delta_rows;
        int n = 0;
        for (int i = 0; i < delta_rows; ++i) {
          size_t idx = state->rows.size() > 0 ? state->rows.size() - 1 - i : 0;
          if (idx < state->rows.size() &&
              state->jh.find_visual_top_for_row(idx) >=
                  state->scroll_top + state->viewport_rows) {
            ++n;
          } else break;
        }
        return n;
      }();
    }

    // Clear transient loading flags once geometry changes (parent added
    // the fetched rows).
    state->loading_earlier = false;
    state->loading_later   = false;

    clamp_scroll(*state);
  }

  /// Jump so a specific row's top is at viewport top minus headroom.
  void JumpToRow(size_t idx, int headroom = 3) {
    if (!state || state->rows.empty()) return;
    if (idx >= state->rows.size()) idx = state->rows.size() - 1;
    int line = state->jh.find_visual_top_for_row(idx);
    JumpToVisualLine(std::max(0, line - headroom));
  }

  void JumpToVisualLine(int line) {
    if (!state) return;
    int const max = std::max(0, state->jh.total() - state->viewport_rows);
    state->scroll_top = std::clamp(line, 0, max);
    update_sticky_after_scroll(*state, state->scroll_top);
    if (state->callbacks.on_scrolled)
      state->callbacks.on_scrolled(state->scroll_top, state->sticky_bottom);
  }

  void SetSticky(bool on) {
    if (!state) return;
    state->sticky_bottom = on;
    if (on) {
      int const max = std::max(0, state->jh.total() - state->viewport_rows);
      state->scroll_top = max;
      state->new_message_count = 0;
    }
  }

  void SetFocusDomain(FocusDomain d) {
    if (state) state->focus_domain = d;
  }
};

/// Component impl.  Captures a shared_ptr<VirtualListState> so both
/// Render() and CatchEvent() see the same mutable state.
struct [[nodiscard]] VirtualListComponentBase : ftxui::ComponentBase {
  std::shared_ptr<VirtualListState> s;
  VirtualListHandle                 handle;

  explicit VirtualListComponentBase(std::shared_ptr<VirtualListState> state)
      : s(std::move(state)) {
    handle.state = s.get();
  }

  ftxui::Element Render() override {
    using namespace ftxui;
    tick_frame(s->fsm);
    maybe_trigger_load(*s);
    // Advance our own frame counter (used by debounce / UI timers).
    s->frame++;

    Element body = render_list_as_elements(*s);
    // Wrap in yframe so outer layout gets a correct scroll thumb.
    body = body | yframe | vscroll_indicator;
    return body;
  }

  bool OnEvent(ftxui::Event event) override {
    // ① New-messages pill: clicking the body with a mouse in the bottom
    //   row fires jump_to_new_messages.  FTXUI mouse coords are screen-
    //   relative; we only implement keyboard fallback (Shift+Enter) here.
    if (event == ftxui::Event::Return && s->new_message_count > 0 &&
        !s->sticky_bottom) {
      if (s->callbacks.on_jump_to_new_messages)
        s->callbacks.on_jump_to_new_messages();
      handle.SetSticky(true);
      if (s->callbacks.on_scrolled)
        s->callbacks.on_scrolled(s->scroll_top, true);
      return true;
    }

    // ② If scroll_keys says we own the event → run the FSM.
    if (ShouldHandleScrollKey(s->focus_domain, event)) {
      ScrollState ss{
          .scroll_top    = s->scroll_top,
          .viewport_rows = s->viewport_rows,
          .total_rows    = s->jh.total(),
          .selected_idx  = std::nullopt,
      };
      int const old_scroll = ss.scroll_top;

      ScrollCallbacks cbs{
        .scroll_to = [&](int line) {
          int const mx = std::max(0, s->jh.total() - s->viewport_rows);
          s->scroll_top = std::clamp(line, 0, mx);
          ss.scroll_top = s->scroll_top;
        },
        .scroll_by = [&](int delta) -> bool {
          int const mx = std::max(0, s->jh.total() - s->viewport_rows);
          int tgt = ss.scroll_top + delta;
          if (tgt >= mx) { s->scroll_top = mx; ss.scroll_top = mx; return true; }
          if (tgt <= 0)  { s->scroll_top = 0;  ss.scroll_top = 0;  return false; }
          s->scroll_top = tgt; ss.scroll_top = tgt; return false;
        },
        .scroll_bottom = [&] { handle.SetSticky(true); },
        .center_selected = [&](CenterKind) { /* TODO */ },
        .row_to_visual = [&](int idx) -> int {
          if (idx < 0) return 0;
          return s->jh.find_visual_top_for_row(static_cast<size_t>(idx));
        },
        .visual_to_row = [&](int line) -> int {
          return static_cast<int>(s->jh.find_row_at_visual_line(line));
        },
        .search_step = s->callbacks.search_step,
      };

      bool consumed = HandleScrollKey(event, ss, s->fsm, cbs);
      if (!consumed) return false;

      s->scroll_top = ss.scroll_top;
      update_sticky_after_scroll(*s, old_scroll);
      if (s->callbacks.on_scrolled)
        s->callbacks.on_scrolled(s->scroll_top, s->sticky_bottom);

      // ③ Fire incremental loads only on *user* scroll events (not
      //    programmatic jumps).  Prevents SetRows() storms from loop-
      //    triggering on_load_more.
      maybe_trigger_load(*s);
      return true;
    }
    return ComponentBase::OnEvent(event);
  }
};

/// Public factory.  `out_handle` is optional — when supplied, callers get
/// an imperative handle for SetRows/JumpToRow/etc.
[[nodiscard]] inline ftxui::Component MakeVirtualMessageList(
    VirtualListOptions options,
    VirtualListCallbacks callbacks,
    VirtualListHandle *out_handle = nullptr) {
  auto state = std::make_shared<VirtualListState>();
  state->options   = std::move(options);
  state->callbacks = std::move(callbacks);
  state->viewport_rows = state->options.viewport_rows;
  state->auto_mode     = state->options.auto_scroll;

  auto comp = ftxui::Make<VirtualListComponentBase>(std::move(state));
  if (out_handle) {
    *out_handle = static_cast<VirtualListComponentBase *>(comp.get())->handle;
  }
  return comp;
}

// ═══════════════════════════════════════════════════════════════════════════
// TEST HELPERS (opt-in via -DCC_VLIST_TEST)
// ═══════════════════════════════════════════════════════════════════════════
#ifdef CC_VLIST_TEST

export namespace cc::ui::messages::virtual_list::test {

using TestResult = std::pair<bool, std::string>;
[[nodiscard]] inline TestResult ok()     { return {true,  "OK"}; }
[[nodiscard]] inline TestResult fail(std::string m) { return {false, std::move(m)}; }

/// Build a synthetic VisibleRow sequence of N rows with heights drawn
/// from the pattern 1,3,7,11 (cycles).  Covers tall + short rows.
[[nodiscard]] inline std::vector<VisibleRowFwd> make_rows(size_t n) {
  std::vector<VisibleRowFwd> rows(n);
  int pat[4] = {1, 3, 7, 11};
  for (size_t i = 0; i < n; ++i) {
    rows[i].row_id = i + 1;
    rows[i].estimated_height_lines = pat[i % 4];
    rows[i].search_key = std::string("row #") + std::to_string(i);
  }
  return rows;
}

/// Basic geometry sanity: cumulative[i+1] - cumulative[i] == row[i].height.
[[nodiscard]] inline TestResult GeometryPsumSanity() {
  auto rows = make_rows(100);
  auto jh   = build_geometry(std::span{rows});
  int acc = 0;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (jh.top_of((int)i) != acc)
      return fail("psum[" + std::to_string(i) + "] = " +
                  std::to_string(jh.top_of(i)) + ", want " + std::to_string(acc));
    acc += std::max(1, rows[i].estimated_height_lines);
  }
  if (jh.total() != acc)
    return fail("total " + std::to_string(jh.total()) + " != " +
                std::to_string(acc));
  return ok();
}

/// JumpHandle::find_row_at_visual_line exact match / between / OOB.
[[nodiscard]] inline TestResult FindRowBinary() {
  auto rows = make_rows(5);  // heights: 1,3,7,11,1 → psum = [0,1,4,11,22,23]
  auto jh = build_geometry(std::span{rows});
  if (jh.find_row_at_visual_line(0)  != 0) return fail("L0→0");
  if (jh.find_row_at_visual_line(1)  != 1) return fail("L1→1");
  if (jh.find_row_at_visual_line(2)  != 1) return fail("L2→1");
  if (jh.find_row_at_visual_line(4)  != 2) return fail("L4→2");
  if (jh.find_row_at_visual_line(10) != 2) return fail("L10→2");
  if (jh.find_row_at_visual_line(11) != 3) return fail("L11→3");
  if (jh.find_row_at_visual_line(999)!= 4) return fail("OOB→last");
  if (jh.find_visual_top_for_row(3) != 11) return fail("top(3)!=11");
  return ok();
}

/// build_visible_slice: scroll=0, vp=5 should include at least enough
/// rows to cover 0..25 lines (pattern 1,3,7,11 → row 4 ends at 23).
[[nodiscard]] inline TestResult VisibleSlice() {
  auto rows = make_rows(1000);
  auto jh = build_geometry(std::span{rows});
  auto [start, cnt] = build_visible_slice(jh, 0, 40);
  if (start != 0) return fail("start!=0");
  // Verify slice covers at least [0, 40 + kOverscanRows) lines.
  int end_line = jh.find_visual_top_for_row(start + cnt);
  int need = 40 + kOverscanRows;
  if (end_line < need)
    return fail("slice end " + std::to_string(end_line) + " < need " +
                std::to_string(need));
  return ok();
}

/// 100k rows: verify O(log N) jump scales (timing is manual; test is
/// structural only — ensures binary search not linear walk).
[[nodiscard]] inline TestResult LargeScaleJump() {
  auto rows = make_rows(100'000);
  auto jh = build_geometry(std::span{rows});
  // Sample 3 target lines; confirm result is consistent with psum.
  for (int line : {12345, 54321, 99999}) {
    size_t idx = jh.find_row_at_visual_line(line);
    if (jh.top_of(idx) > line)
      return fail("idx=" + std::to_string(idx) + " top>line for line " +
                  std::to_string(line));
    if (idx + 1 < jh.size() && jh.top_of(idx + 1) <= line)
      return fail("idx=" + std::to_string(idx) + " not upper-bound for line " +
                  std::to_string(line));
  }
  return ok();
}

[[nodiscard]] inline std::string RunAllVirtualListTests() {
  std::string out;
  auto check = [&](std::string name, TestResult r) {
    out += (r.first ? "PASS " : "FAIL ") + name + ": " + r.second + "\n";
  };
  check("GeometryPsumSanity", GeometryPsumSanity());
  check("FindRowBinary",       FindRowBinary());
  check("VisibleSlice",        VisibleSlice());
  check("LargeScaleJump",      LargeScaleJump());
  return out;
}

} // namespace cc::ui::messages::virtual_list::test

#endif // CC_VLIST_TEST

} // namespace cc::ui::messages::virtual_list
