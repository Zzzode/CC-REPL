/// =========================================================================
/// @file scroll_keybindings.cppm
/// @brief Unified scroll-key FSM for all long lists (messages, tasks,
///        kanban, agent browse, plugins).  Mirrors TS ScrollKeybindingHandler.
///
/// MODULE:   cc.ui.messages.scroll_keys
/// LICENCE:  Exported.  VirtualMessageList, TasksList, KanbanList all import.
///
/// ┌─────────────────────────────────────────────────────────────────────┐
/// │  KEY MAP  (strict alignment with TS ScrollKeybindingHandler)       │
/// ├─────────────────────────────────────────────────────────────────────┤
/// │  j   /  Down    / Ctrl+N       →  Down1                            │
/// │  k   /  Up      / Ctrl+P       →  Up1                              │
/// │  Ctrl+B  / PageUp              →  PgUp   (full viewport)           │
/// │  Ctrl+F  / PageDown            →  PgDn                             │
/// │  Ctrl+U                        →  HalfPgUp                         │
/// │  Ctrl+D                        →  HalfPgDn                         │
/// │  gg (double-g, 500ms timeout)  →  Top                              │
/// │  G (shift+g)  /  End           →  Bottom                           │
/// │  Home                          →  Top                              │
/// │  z<Enter> / zt                 →  CenterOnSelected → TOP           │
/// │  z. / zz                      →  CenterOnSelected → CENTER        │
/// │  zb                            →  CenterOnSelected → BOTTOM        │
/// │  n                             →  SearchNext                       │
/// │  N  (shift+n)                  →  SearchPrev                       │
/// │  1-9 prefix  (e.g. 3j = 3×Down1, 800ms timeout)                   │
/// └─────────────────────────────────────────────────────────────────────┘
///
/// ┌─────────────────────────────────────────────────────────────────────┐
/// │  FOCUS PRIORITY (strict alignment with TS focus-domain rules)      │
/// ├─────────────────────────────────────────────────────────────────────┤
/// │  1) PromptInput has cursor     → only PgUp/PgDn                    │
/// │  2) SearchInput focused        → only Esc / Enter / Ctrl+Enter     │
/// │  3) Dropdown open             → ALL scroll keys disabled           │
/// │  4) Other                     → full scroll set                    │
/// └─────────────────────────────────────────────────────────────────────┘
///
/// TIME:  Frame-counter based (no std::chrono).  Caller advances
///        tick_frame() per Render().  Each frame ≈ 16.6ms → 30 frames
///        ≈ 500ms (gg double-key window).  Ties to FTXUI 60fps baseline.
/// =========================================================================

module;

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <functional>
#include <algorithm>
#include <bit>

#include <ftxui/component/event.hpp>

export module cc.ui.messages.scroll_keys;

export namespace cc::ui::messages::scroll_keys {

// ─── Constants ──────────────────────────────────────────────────────────────
// Frame counter → ms approximations (at FTXUI's 60fps nominal rate).
// Actual frame rate can drift but the windowing only needs *approximate*
// thresholds for double-key recognition; the TS code uses 500ms / 800ms.
inline constexpr int kFramesPerSecond     = 60;
inline constexpr int kGgDoubleKeyFrames   = 30;    //  30f ≈ 500ms
inline constexpr int kDigitPrefixFrames   = 48;    //  48f ≈ 800ms

// ─── Public API types ───────────────────────────────────────────────────────

enum class ScrollOp : std::uint8_t {
  Up1 = 0, Down1,
  PgUp,  PgDn,
  HalfPgUp, HalfPgDown,
  Top,   Bottom,
  ToRow,
  SearchNext, SearchPrev,
  CenterTop, CenterMiddle, CenterBottom,
  EnsureVisible
};

enum class CenterKind : std::uint8_t { Top, Middle, Bottom };

enum class FocusDomain : std::uint8_t {
  Messages,       // default / no competing input
  PromptInput,    // prompt has the cursor
  SearchInput,    // search widget focused
  Dropdown,       // suggestion / completer / custom-select dropdown open
  Dialog          // modal dialog
};

/// Mirrors TS `ScrollBoxHandle`'s observable state — enough for FSM math.
struct ScrollState {
  int scroll_top    = 0;     // 0-based, units = terminal rows
  int viewport_rows = 40;
  int total_rows    = 0;     // total content rows (post-virtualization total)
  std::optional<int> selected_idx;   // list-*row* index (not visual line)
};

/// Callbacks invoked after the FSM applies a scroll delta.  Returning
/// `false` from on_scroll_by / on_center means the caller didn't actually
/// move (e.g. the scroll reached a boundary); HandleScrollKey still reports
/// "consumed" but the auto-scroll sticky flag isn't broken.
struct ScrollCallbacks {
  /// Absolute scroll to a visual-line position (0 = top of content).
  std::function<void(int line)>          scroll_to       = nullptr;
  /// Relative scroll by `delta` visual lines (negative = up).
  /// Returns the resulting sticky state (true iff view reached the bottom).
  std::function<bool(int delta)>         scroll_by       = nullptr;
  /// Scroll to the last row (sticky-on-bottom).
  std::function<void()>                  scroll_bottom   = nullptr;
  /// Center / top-align / bottom-align the currently selected row.
  std::function<void(CenterKind kind)>   center_selected = nullptr;
  /// Row-index → visual-top (0-based cumulative line) lookup.  Used by
  /// ToRow / search jumpers to resolve idx → scroll target.
  std::function<int(int row_idx)>        row_to_visual   = nullptr;
  /// Visual-top → row-index lookup (inverse of above).  For EnsureVisible.
  std::function<int(int visual_line)>    visual_to_row   = nullptr;
  /// Search navigation (n/N).  Parameter: +1 = next, -1 = prev.
  /// Returns the target visual line to scroll to, or -1 if no match.
  std::function<int(int delta)>          search_step     = nullptr;
};

// ─── Internal: FSM state ───────────────────────────────────────────────────
//
// Hidden from the public ScrollState (it's pure scroll geometry), carried
// inside the handler context and advanced per-frame.  This is an internal
// struct — the factory (below) owns one and closes over it.

struct FSMContext {
  // ── digit prefix: {pending_digit_, pending_digit_frame_} ──
  int  pending_digit       = 0;
  int  pending_digit_frame = 0;

  // ── double-g: {g_pressed_frame_} ──
  // g_pressed_frame_ == -1 means "not waiting for second g"
  int  g_pressed_frame     = -1;

  // ── z-prefix: z_pending_ (for zt / zz / zb / z<enter>) ──
  bool z_pending           = false;
  int  z_pressed_frame     = -1;

  // Monotonic frame counter (tick_frame() increments each Render pass).
  int  frame               = 0;
};

// ─── Focus-domain gate (pure function, testable) ────────────────────────────

/// Pure predicate: should a scroll-key handler even *look* at `event` given
/// the current focus domain?  Mirrors TS rules.
[[nodiscard]] inline bool
ShouldHandleScrollKey(FocusDomain domain, ftxui::Event const &event) noexcept {
  using ftxui::Event;
  switch (domain) {
    case FocusDomain::Dropdown:
    case FocusDomain::Dialog:
      // Dropdown / Dialog own all navigation keys fully.
      return false;

    case FocusDomain::SearchInput: {
      // Search input: only Esc / Enter / Ctrl+Enter are let through (they
      // commit / cancel search → which re-arms scroll state).  Everything
      // else (arrows, j/k, n/N, etc.) goes to the search text field.
      if (event == Event::Escape) return true;
      if (event == Event::Return) return true;
      if (event == Event::Character('\n') || event.is_mouse()) return false;
      // ctrl+enter = custom composite; FTXUI gives this as a Character
      // under some terminal emulators.  Treat ctrl+j (= 0x0A) as enter.
      if (event.is_character()) {
        char32_t c = event.character()[0];
        if (c == char32_t(0x0A)) return true;   // Ctrl+J / Ctrl+Enter-ish
      }
      return false;
    }

    case FocusDomain::PromptInput: {
      // Prompt has the cursor → ONLY PgUp/PgDn are scoped to scroll.
      // (TS uses these for prompt-history browsing; they still map to
      // scroll:pageUp/scroll:pageDown via the keybinding registry.)
      return event == Event::PageUp || event == Event::PageDown;
    }

    case FocusDomain::Messages:
    default:
      // Full keyboard — handled below in HandleScrollKey.
      return true;
  }
}

// ─── FSM tick ───────────────────────────────────────────────────────────────

/// Advance frame counter and reap expired FSM watches.  Call once per
/// Render() — before any event dispatch that frame.
inline void tick_frame(FSMContext &fsm) noexcept {
  ++fsm.frame;
  int now = fsm.frame;

  // Digit prefix timeout.
  if (fsm.pending_digit != 0 &&
      now - fsm.pending_digit_frame > kDigitPrefixFrames) {
    fsm.pending_digit = 0;
    fsm.pending_digit_frame = 0;
  }
  // Double-g timeout.
  if (fsm.g_pressed_frame >= 0 &&
      now - fsm.g_pressed_frame > kGgDoubleKeyFrames) {
    fsm.g_pressed_frame = -1;
  }
  // z-prefix timeout.
  if (fsm.z_pending && now - fsm.z_pressed_frame > kGgDoubleKeyFrames) {
    fsm.z_pending = false;
    fsm.z_pressed_frame = -1;
  }
}

// ─── Helpers ────────────────────────────────────────────────────────────────

[[nodiscard]] inline int clamp_scroll_top(ScrollState const &s) noexcept {
  int const max = std::max(0, s.total_rows - s.viewport_rows);
  return std::clamp(s.scroll_top, 0, max);
}

inline void apply_scroll_to(ScrollState &s, ScrollCallbacks const &cb,
                            int target_line) noexcept {
  int const max = std::max(0, s.total_rows - s.viewport_rows);
  target_line   = std::clamp(target_line, 0, max);
  s.scroll_top  = target_line;
  if (cb.scroll_to) cb.scroll_to(target_line);
}

/// Apply a relative delta using jumpBy-semantics (mirrors TS jumpBy).
/// Returns the resulting sticky state: true iff view reached bottom.
[[nodiscard]] inline bool
apply_scroll_by(ScrollState &s, ScrollCallbacks const &cb, int delta) noexcept {
  int const max     = std::max(0, s.total_rows - s.viewport_rows);
  int       target  = s.scroll_top + delta;
  if (target >= max) {
    s.scroll_top = max;
    if (cb.scroll_bottom) cb.scroll_bottom();
    else if (cb.scroll_to) cb.scroll_to(max);
    return true;   // sticky
  }
  if (target <= 0) {
    s.scroll_top = 0;
    if (cb.scroll_to) cb.scroll_to(0);
    else if (cb.scroll_by) cb.scroll_by(delta);
    return false;
  }
  s.scroll_top = target;
  if (cb.scroll_by) return cb.scroll_by(delta);
  if (cb.scroll_to) cb.scroll_to(target);
  return false;
}

// ─── Dispatch a ScrollOp with a repetition multiplier ──────────────────────

[[nodiscard]] inline bool dispatch_op(ScrollOp op, int multiplier,
                                       ScrollState &s,
                                       ScrollCallbacks const &cb,
                                       FSMContext&) noexcept {
  int const vh = std::max(1, s.viewport_rows);
  switch (op) {
    case ScrollOp::Up1: {
      int n = std::max(1, multiplier);
      (void)apply_scroll_by(s, cb, -n);
      return true;
    }
    case ScrollOp::Down1: {
      int n = std::max(1, multiplier);
      (void)apply_scroll_by(s, cb, n);
      return true;
    }
    case ScrollOp::PgUp: {
      int n = std::max(1, multiplier) * vh;
      (void)apply_scroll_by(s, cb, -n);
      return true;
    }
    case ScrollOp::PgDn: {
      int n = std::max(1, multiplier) * vh;
      (void)apply_scroll_by(s, cb, n);
      return true;
    }
    case ScrollOp::HalfPgUp: {
      int half = std::max(1, vh / 2) * std::max(1, multiplier);
      (void)apply_scroll_by(s, cb, -half);
      return true;
    }
    case ScrollOp::HalfPgDown: {
      int half = std::max(1, vh / 2) * std::max(1, multiplier);
      (void)apply_scroll_by(s, cb, half);
      return true;
    }
    case ScrollOp::Top:
      apply_scroll_to(s, cb, 0);
      return true;
    case ScrollOp::Bottom: {
      int const max = std::max(0, s.total_rows - vh);
      apply_scroll_to(s, cb, max);
      if (cb.scroll_bottom) cb.scroll_bottom();
      return true;
    }
    case ScrollOp::ToRow: {
      if (!cb.row_to_visual) return false;
      // multiplier is the 1-based user input (e.g. "123G" → jump to row 123).
      int target_idx = std::max(1, multiplier) - 1;
      int line       = cb.row_to_visual(target_idx);
      apply_scroll_to(s, cb, line);
      return true;
    }
    case ScrollOp::SearchNext: {
      if (!cb.search_step) return false;
      for (int i = 0; i < std::max(1, multiplier); ++i) {
        int line = cb.search_step(+1);
        if (line >= 0) apply_scroll_to(s, cb, line);
      }
      return true;
    }
    case ScrollOp::SearchPrev: {
      if (!cb.search_step) return false;
      for (int i = 0; i < std::max(1, multiplier); ++i) {
        int line = cb.search_step(-1);
        if (line >= 0) apply_scroll_to(s, cb, line);
      }
      return true;
    }
    case ScrollOp::CenterTop:
      if (cb.center_selected) cb.center_selected(CenterKind::Top);
      return true;
    case ScrollOp::CenterMiddle:
      if (cb.center_selected) cb.center_selected(CenterKind::Middle);
      return true;
    case ScrollOp::CenterBottom:
      if (cb.center_selected) cb.center_selected(CenterKind::Bottom);
      return true;
    case ScrollOp::EnsureVisible: {
      if (!s.selected_idx || !cb.row_to_visual || !cb.visual_to_row)
        return false;
      int line = cb.row_to_visual(*s.selected_idx);
      if (line < s.scroll_top)
        apply_scroll_to(s, cb, line);
      else if (line >= s.scroll_top + vh)
        apply_scroll_to(s, cb, line - vh + 1);
      return true;
    }
  }
  return false;
}

// ─── Main: HandleScrollKey ──────────────────────────────────────────────────
//
// Returns `true` if the event was consumed and should not propagate.
// `false` = caller is free to handle it (Tab/Enter/printable to CustomSelect).
//
// NOTE: `s` and `fsm` are both mutated.  The caller MUST own a single
//       FSMContext per scrollable component (NOT share it across lists).
//
//       tick_frame() should be called in the Render() pass of the owning
//       component — BEFORE invoking HandleScrollKey on events from that
//       frame.  This ensures the timeouts are evaluated at a consistent
//       moment.

[[nodiscard]] inline bool
HandleScrollKey(ftxui::Event const &event, ScrollState &s,
                FSMContext &fsm, ScrollCallbacks const &cb) noexcept {
  using ftxui::Event;
  int now = fsm.frame;
  int mult = fsm.pending_digit;   // prefix multiplier (0 = "no prefix" → 1 later)

  // ── Mouse wheel: passthrough handled elsewhere (the component listens
  //   to Event::Mouse and routes via computeWheelStep).  This FSM handles
  //   *keys* only — a click is never consumed here.
  if (event.is_mouse()) return false;

  // ── Arrow / Home / End / Page keys ───────────────────────────────────
  if (event == Event::ArrowUp) {
    fsm.pending_digit = 0;
    return dispatch_op(ScrollOp::Up1, mult, s, cb, fsm);
  }
  if (event == Event::ArrowDown) {
    fsm.pending_digit = 0;
    return dispatch_op(ScrollOp::Down1, mult, s, cb, fsm);
  }
  if (event == Event::Home) {
    fsm.pending_digit = 0; fsm.g_pressed_frame = -1;
    return dispatch_op(ScrollOp::Top, 1, s, cb, fsm);
  }
  if (event == Event::End) {
    fsm.pending_digit = 0;
    return dispatch_op(ScrollOp::Bottom, mult, s, cb, fsm);
  }
  if (event == Event::PageUp) {
    fsm.pending_digit = 0;
    return dispatch_op(ScrollOp::PgUp, mult, s, cb, fsm);
  }
  if (event == Event::PageDown) {
    fsm.pending_digit = 0;
    return dispatch_op(ScrollOp::PgDn, mult, s, cb, fsm);
  }
  // Ctrl+Enter / Enter passthrough — handled by search / dropdown.
  if (event == Event::Return) return false;
  if (event == Event::Escape) {
    // Esc clears FSM pending state; not consumed (let search/CustomSelect).
    fsm.pending_digit = 0;
    fsm.g_pressed_frame = -1;
    fsm.z_pending = false;
    return false;
  }
  // Tab / shift-tab = navigation, never consumed by scroll.
  if (event == Event::Tab)   return false;

  // ── Character input (printable + ctrl combos) ────────────────────────
  if (!event.is_character()) return false;
  std::string const &raw = event.character();
  if (raw.empty()) return false;

  // FTXUI Event::character: control bytes are 0x01..0x1F (Ctrl+A..Ctrl+Z).
  // Printable chars are single UTF-8 codepoints; we only handle ASCII here.
  char32_t c0 = static_cast<unsigned char>(raw[0]);

  // ── Ctrl combos (0x01..0x1F) ──────────────────────────────────────
  if (c0 >= 0x01 && c0 <= 0x1F) {
    char letter = static_cast<char>('a' + (c0 - 0x01));
    switch (letter) {
      case 'n':  // Ctrl+N — emacs-line Down1
        fsm.pending_digit = 0;
        return dispatch_op(ScrollOp::Down1, mult, s, cb, fsm);
      case 'p':  // Ctrl+P — emacs-line Up1
        fsm.pending_digit = 0;
        return dispatch_op(ScrollOp::Up1, mult, s, cb, fsm);
      case 'b':  // Ctrl+B — full PageUp
        fsm.pending_digit = 0;
        return dispatch_op(ScrollOp::PgUp, mult, s, cb, fsm);
      case 'f':  // Ctrl+F — full PageDown
        fsm.pending_digit = 0;
        return dispatch_op(ScrollOp::PgDn, mult, s, cb, fsm);
      case 'u':  // Ctrl+U — half-page up
        fsm.pending_digit = 0;
        return dispatch_op(ScrollOp::HalfPgUp, mult, s, cb, fsm);
      case 'd':  // Ctrl+D — half-page down
        fsm.pending_digit = 0;
        return dispatch_op(ScrollOp::HalfPgDown, mult, s, cb, fsm);
      case 'j':  // Ctrl+J = 0x0A (alternate Enter in some envs): passthrough
      default:
        return false;
    }
  }

  // ── z-prefix state: next char selects the center kind ─────────────
  if (fsm.z_pending) {
    fsm.z_pending = false;
    fsm.z_pressed_frame = -1;
    CenterKind kind = CenterKind::Middle;   // z. / zz default
    bool handled = true;
    if (c0 == 't' || c0 == '\r')       kind = CenterKind::Top;
    else if (c0 == 'z' || c0 == '.')   kind = CenterKind::Middle;
    else if (c0 == 'b')                kind = CenterKind::Bottom;
    else { handled = false; /* unknown sequence */ }
    if (handled) {
      if (cb.center_selected) cb.center_selected(kind);
      fsm.pending_digit = 0;
      return true;
    }
    // fall through: unknown char re-dispatched below (e.g. "zj" — ignore prefix)
  }

  // ── Digit prefix (1–9): accumulate until non-digit ────────────────
  //    A bare "0" alone is ignored (matches Vim convention: starts at 1).
  if (c0 >= '0' && c0 <= '9') {
    int d = static_cast<int>(c0 - '0');
    // If there's an active double-g wait and user starts typing digits,
    // cancel the g-prefix (matches Vim: "g12j" = 12j, not gg+12).
    fsm.g_pressed_frame = -1;
    if (d == 0 && fsm.pending_digit == 0) {
      return false;   // leading zero discarded
    }
    // Cap at 99,999 (more than enough — avoids overflow).
    if (fsm.pending_digit <= 9999) {
      fsm.pending_digit = fsm.pending_digit * 10 + d;
    } else {
      fsm.pending_digit = 99999;
    }
    fsm.pending_digit_frame = now;
    return true;   // consumed
  }

  // From this point onward, the character is a dispatchable letter.
  // We will consume it or not, but a pending_digit (if any) will be
  // used then cleared.

  // ── Printable letter dispatches ───────────────────────────────────
  switch (c0) {
    case 'j':
      (void)dispatch_op(ScrollOp::Down1, mult, s, cb, fsm);
      fsm.pending_digit = 0;
      return true;
    case 'k':
      (void)dispatch_op(ScrollOp::Up1, mult, s, cb, fsm);
      fsm.pending_digit = 0;
      return true;

    case 'g': {
      // first g → arm double-key window.  second g (within window) → Top.
      if (fsm.g_pressed_frame >= 0 &&
          now - fsm.g_pressed_frame <= kGgDoubleKeyFrames) {
        fsm.g_pressed_frame = -1;
        fsm.pending_digit = 0;
        return dispatch_op(ScrollOp::Top, 1, s, cb, fsm);
      }
      fsm.g_pressed_frame = now;
      return true;   // consumed: arm the window
    }

    case 'G':   // shift+g — End / bottom.  Optional digit prefix = row N.
      if (mult > 0) {
        // "123G" = go to row 123.  Vim-style.
        (void)dispatch_op(ScrollOp::ToRow, mult, s, cb, fsm);
      } else {
        (void)dispatch_op(ScrollOp::Bottom, 1, s, cb, fsm);
      }
      fsm.pending_digit = 0;
      fsm.g_pressed_frame = -1;
      return true;

    case 'z':
      // Arm z-prefix window.  Next char decides kind (handled above).
      fsm.z_pending       = true;
      fsm.z_pressed_frame = now;
      return true;

    case 'n':
      (void)dispatch_op(ScrollOp::SearchNext, mult, s, cb, fsm);
      fsm.pending_digit = 0;
      return true;
    case 'N':
      (void)dispatch_op(ScrollOp::SearchPrev, mult, s, cb, fsm);
      fsm.pending_digit = 0;
      return true;

    case ' ':   // space = full page down (less convention)
      (void)dispatch_op(ScrollOp::PgDn, mult, s, cb, fsm);
      fsm.pending_digit = 0;
      return true;
    case 'b':   // bare b = page up (less convention, Ctrl+B handled above)
      (void)dispatch_op(ScrollOp::PgUp, mult, s, cb, fsm);
      fsm.pending_digit = 0;
      return true;

    default:
      // Unrecognized printable — clear stale prefixes, do NOT consume
      // (so prompt input / completer widgets can still see the keystroke).
      fsm.pending_digit = 0;
      fsm.g_pressed_frame = -1;
      fsm.z_pending       = false;
      return false;
  }
}

// ─── Factory: reusable handler closure ──────────────────────────────────────
//
// Convenience helper so callers don't have to carry FSMContext manually.
// Usage:
//   auto [handler, fsm] = scroll_keys::MakeScrollHandler();
//   …
//   if (scroll_keys::ShouldHandleScrollKey(domain, ev) &&
//       handler(ev, state, callbacks)) { return true; }

struct ScrollHandlerBundle {
  FSMContext ctx;
  /// Returns true if event was consumed.
  std::function<bool(ftxui::Event const &, ScrollState &,
                     ScrollCallbacks const &)>
      handle = [this](ftxui::Event const &ev, ScrollState &st,
                      ScrollCallbacks const &cb) noexcept {
        return HandleScrollKey(ev, st, this->ctx, cb);
      };
  /// Tick the frame counter (call per Render).
  void tick() noexcept { tick_frame(ctx); }
};

[[nodiscard]] inline ScrollHandlerBundle MakeScrollHandler() {
  return {};
}

} // namespace cc::ui::messages::scroll_keys

// ═══════════════════════════════════════════════════════════════════════════
// TEST FIXTURES  (activated via -DCC_VLIST_TEST at build time)
// ═══════════════════════════════════════════════════════════════════════════
#ifdef CC_VLIST_TEST

export namespace cc::ui::messages::scroll_keys::test {

/// Result type for step-based test helpers.  Returns "OK" or a short
/// failure diagnostic.  Pure — never invokes GoogleTest.
struct TestResult {
  bool        ok  = true;
  std::string msg = "OK";
};

[[nodiscard]] inline TestResult make_ok()    { return {true,  "OK"}; }
[[nodiscard]] inline TestResult make_fail(std::string m) {
  return {false, std::move(m)};
}

/// Fake in-memory scroll driver — installs callbacks that mutate a copy of
/// ScrollState and lets tests assert on it.  Pure — no FTXUI components.
struct FakeScrollCallbacks {
  ScrollState *state = nullptr;
  std::vector<int>  search_matches;   // visual lines of matches
  int               search_ptr = 0;   // current match index

  ScrollCallbacks build() {
    return ScrollCallbacks{
      .scroll_to = [this](int line) {
        int max = std::max(0, state->total_rows - state->viewport_rows);
        state->scroll_top = std::clamp(line, 0, max);
      },
      .scroll_by = [this](int delta) -> bool {
        int max  = std::max(0, state->total_rows - state->viewport_rows);
        int tgt  = state->scroll_top + delta;
        if (tgt >= max) { state->scroll_top = max; return true; }
        if (tgt <= 0)   { state->scroll_top = 0;   return false; }
        state->scroll_top = tgt;
        return false;
      },
      .scroll_bottom = [this] {
        state->scroll_top =
            std::max(0, state->total_rows - state->viewport_rows);
      },
      .search_step = [this](int dir) -> int {
        if (search_matches.empty()) return -1;
        search_ptr = static_cast<int>(
            (search_ptr + search_matches.size() + dir) % search_matches.size());
        return search_matches[search_ptr];
      },
    };
  }
};

/// Step 1: basic single-op smoke.
[[nodiscard]] inline TestResult BasicKeySanity() {
  using ftxui::Event;
  FakeScrollCallbacks fk;
  ScrollState s{.scroll_top = 0, .viewport_rows = 10, .total_rows = 100};
  fk.state = &s;
  auto cb = fk.build();
  FSMContext fsm{};

  // 10 × j → scroll_top == 10
  for (int i = 0; i < 10; ++i)
    HandleScrollKey(Event::Character("j"), s, fsm, cb);
  if (s.scroll_top != 10) return make_fail("10×j → row="+std::to_string(s.scroll_top));

  // Ctrl+U → half-page (5 rows) up.  10-5=5.
  HandleScrollKey(Event::Character(std::string(1, char(0x15))), s, fsm, cb);
  if (s.scroll_top != 5) return make_fail("Ctrl+U → row="+std::to_string(s.scroll_top));

  // End → 90.
  HandleScrollKey(Event::End, s, fsm, cb);
  if (s.scroll_top != 90) return make_fail("End → row="+std::to_string(s.scroll_top));

  // Home → 0.
  HandleScrollKey(Event::Home, s, fsm, cb);
  if (s.scroll_top != 0) return make_fail("Home → row="+std::to_string(s.scroll_top));
  return make_ok();
}

/// Step 2: digit prefix "12j" = 12×Down.
[[nodiscard]] inline TestResult DigitPrefix() {
  using ftxui::Event;
  FakeScrollCallbacks fk;
  ScrollState s{0, 10, 100};
  fk.state = &s;
  auto cb = fk.build();
  FSMContext fsm{};

  HandleScrollKey(Event::Character("1"), s, fsm, cb);
  HandleScrollKey(Event::Character("2"), s, fsm, cb);
  HandleScrollKey(Event::Character("j"), s, fsm, cb);
  if (s.scroll_top != 12)
    return make_fail("12j → row="+std::to_string(s.scroll_top));
  return make_ok();
}

/// Step 3: double-g (gg) timeout / no-timeout.
[[nodiscard]] inline TestResult DoubleG() {
  using ftxui::Event;
  FakeScrollCallbacks fk;
  ScrollState s{50, 10, 100};
  fk.state = &s;
  auto cb = fk.build();
  FSMContext fsm{};
  fsm.frame = 100;

  // Two 'g' within 30 frames → Top.
  HandleScrollKey(Event::Character("g"), s, fsm, cb);
  fsm.frame = 110;       // +10f (<30)
  HandleScrollKey(Event::Character("g"), s, fsm, cb);
  if (s.scroll_top != 0)
    return make_fail("gg → row="+std::to_string(s.scroll_top));

  // Reset, then one g + 40 frames + one g → Top should NOT fire (timeout).
  s.scroll_top = 50;
  fsm.frame = 200;
  HandleScrollKey(Event::Character("g"), s, fsm, cb);
  // Simulate 40 frames ticking by (timeout the window).
  for (int i = 0; i < 41; ++i) tick_frame(fsm);
  // Now second g should just arm (since first g timed out).
  HandleScrollKey(Event::Character("g"), s, fsm, cb);
  if (s.scroll_top != 50)
    return make_fail("g-timed-g → row="+std::to_string(s.scroll_top));
  // Third g (right after) completes the double.
  HandleScrollKey(Event::Character("g"), s, fsm, cb);
  if (s.scroll_top != 0)
    return make_fail("g-timed-gg → row="+std::to_string(s.scroll_top));
  return make_ok();
}

/// Step 4: Focus domain gating.
[[nodiscard]] inline TestResult FocusGating() {
  using ftxui::Event;
  // PromptInput: only PgUp/PgDn pass.
  if (!ShouldHandleScrollKey(FocusDomain::PromptInput, Event::PageUp))
    return make_fail("Prompt should pass PgUp");
  if (!ShouldHandleScrollKey(FocusDomain::PromptInput, Event::PageDown))
    return make_fail("Prompt should pass PgDn");
  if ( ShouldHandleScrollKey(FocusDomain::PromptInput,
                             Event::Character("j")))
    return make_fail("Prompt should block 'j'");

  // Dropdown blocks everything.
  if ( ShouldHandleScrollKey(FocusDomain::Dropdown, Event::PageUp))
    return make_fail("Dropdown should block PgUp");

  // SearchInput allows Esc, blocks 'j'.
  if (!ShouldHandleScrollKey(FocusDomain::SearchInput, Event::Escape))
    return make_fail("Search should allow Esc");
  if ( ShouldHandleScrollKey(FocusDomain::SearchInput,
                             Event::Character("n")))
    return make_fail("Search should block 'n'");

  return make_ok();
}

/// Run all scroll-key step tests and return a summary string.
[[nodiscard]] inline std::string RunAllScrollKeyTests() {
  std::string out;
  auto check = [&](std::string name, TestResult r) {
    out += (r.ok ? "PASS " : "FAIL ") + name + ": " + r.msg + "\n";
  };
  check("BasicKeySanity", BasicKeySanity());
  check("DigitPrefix",   DigitPrefix());
  check("DoubleG",       DoubleG());
  check("FocusGating",   FocusGating());
  return out;
}

} // namespace cc::ui::messages::scroll_keys::test

#endif // CC_VLIST_TEST
