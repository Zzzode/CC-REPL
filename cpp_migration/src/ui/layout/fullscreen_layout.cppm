/// =========================================================================
/// @file fullscreen_layout.cppm
/// @brief Faithful C++/FTXUI port of TS `FullscreenLayout.tsx` — the
///        slot-based REPL shell architecture.
///
/// MODULE:   cc.ui.layout.fullscreen
/// LICENCE:  Exported.  Imported by repl_screen.cppm (RenderReplScreen).
///
/// TS REGION MODEL (FullscreenLayout.tsx, ~lines 270-330):
///   ┌───────────────────────────────────────────────────────────┐
///   │ scrollwrap = vbox:                                        │
///   │   • StickyPromptHeader (1 row, optional)                  │
///   │   • ScrollBox (flexGrow=1) = {scrollable, overlay}        │
///   │   • NewMessagesPill (absolute bottom-centre, optional)    │
///   │   • bottomFloat (absolute bottom-right, optional)         │
///   │    ↳ the scrollwrap is the flexGrow region (fills height) │
///   ├───────────────────────────────────────────────────────────┤
///   │ bottom (flexShrink=0, maxHeight≈50%) =                    │
///   │   SuggestionsOverlay, DialogOverlay, {bottom}              │
///   ├───────────────────────────────────────────────────────────┤
///   │ modal (absolute bottom-anchored, maxHeight=rows-PEEK):     │
///   │   ▔ top divider, {modal} body — paints over scrollwrap+   │
///   │   bottom; MODAL_TRANSCRIPT_PEEK rows of transcript visible│
///   │   above it                                                │
///   └───────────────────────────────────────────────────────────┘
///
/// These three are stacked via position:absolute → in FTXUI we express
/// that stacking with `dbox` (overlay).  `scrollable` fills remaining
/// height via `flex`; `bottom` is pinned (no flex); `overlay` paints
/// inside the scroll region; `modal` floats anchored-bottom via dbox.
///
/// DISCIPLINE: this is a COMPOSITION module.  It accepts already-rendered
///   Element slots from the caller (repl_screen) and assembles them.  It
///   does NOT own message/prompt/dialog rendering — those stay in their
///   existing modules.  This mirrors how the TS FullscreenLayout wraps a
///   `<ScrollBox>{scrollable}{overlay}</ScrollBox>` without knowing what
///   those ReactNode props are.
///
/// CHROME (built here, faithful to TS):
///   • StickyPromptHeader — a 1-row context breadcrumb shown above the
///     scroll region while scrolled up (single fixed row so the ScrollBox
///     anchor never shifts — see TS StickyPromptHeader comment).
///   • NewMessagesPill — "N new messages ↓" / "Jump to bottom ↓" centred
///     at the bottom of the scroll region when new content arrived below
///     the fold while unpinned.
///   • Modal divider — the ▔ row atop an open modal pane.
/// =========================================================================

module;

#include <string>
#include <optional>
#include <vector>
#include <algorithm>

#include <ftxui/dom/elements.hpp>

export module cc.ui.layout.fullscreen;

export namespace cc::ui::layout::fullscreen {

using namespace ftxui;

// ─── Constants (1:1 with TS FullscreenLayout.tsx) ───────────────────────────

/// Rows of transcript context kept visible above the modal pane's ▔ divider.
/// (TS: `const MODAL_TRANSCRIPT_PEEK = 2`.)
inline constexpr int kModalTranscriptPeek = 2;

// ─── Slot bundle ────────────────────────────────────────────────────────────
//
// Mirrors TS `type Props = { scrollable, bottom, overlay?, modal?,
//                            bottomFloat? }` plus the chrome-driver props
// (hidePill / hideSticky / newMessageCount / pillVisible).  Every field has
// a default so a caller can populate incrementally without breaking.

struct FullscreenLayoutSlots {
    /// Content that scrolls (messages, tool output).  Maps to TS `scrollable`.
    /// Required — drives the flexGrow region.
    Element scrollable{};

    /// Content pinned to the bottom (status bar, prompt, footer).  Maps to
    /// TS `bottom`.  flexShrink=0 so it never collapses under scroll content.
    Element bottom{};

    /// Content painted inside the ScrollBox region over the transcript
    /// (PermissionRequest).  Maps to TS `overlay`.  Optional.
    std::optional<Element> overlay{};

    /// Slash-command / centered dialog content.  Maps to TS `modal`.
    /// Rendered in a bottom-anchored absolute pane with a ▔ top divider.
    std::optional<Element> modal{};

    /// Absolute bottom-right companion bubble (buddy speech).  Maps to TS
    /// `bottomFloat`.  Optional; can be a stub.
    std::optional<Element> bottom_float{};

    // ── Chrome-driver inputs (mirror TS props) ─────────────────────────────

    /// Sticky prompt header text.  Empty => no header shown.  Maps to
    /// TS `stickyPrompt.text` (via ScrollChromeContext).
    std::string sticky_prompt_text;

    /// Force-hide the sticky header (e.g. viewing a teammate task).
    /// Maps to TS `hideSticky`.
    bool hide_sticky{false};

    /// Should the "N new messages" pill render this frame?  Caller computes
    /// it from scroll state (scroll_pinned_to_bottom + divider snapshot).
    /// Maps to TS `pillVisible` (useSyncExternalStore).
    bool pill_visible{false};

    /// Force-hide the pill.  Maps to TS `hidePill`.
    bool hide_pill{false};

    /// Count for the pill label.  0 => "Jump to bottom", >0 => "N new msgs".
    /// Maps to TS `newMessageCount`.
    int new_message_count{0};

    /// Terminal geometry for modal height clamping.  Probed once per frame
    /// by the caller (repl_screen) via ink_utils::query_terminal_size().
    int term_cols{80};
    int term_rows{24};

    /// Callback fired when the user "clicks" the pill — left as a sentinel
    /// bool here; the caller wires the real scroll-to-bottom action since
    /// this module is DOM-only (no Component event loop).  When true, the
    /// pill renders with a highlighted style to signal it is interactive.
    bool pill_actionable{false};
};

// ─── Chrome helpers ─────────────────────────────────────────────────────────

/// Sticky prompt header — a FIXED-height (1 row) context breadcrumb.
/// Single row so the ScrollBox anchor never shifts when the sticky prompt
/// swaps mid-scroll (mirrors the TS comment about DECSTBM region shifts).
[[nodiscard]] inline Element StickyPromptHeader(std::string_view body) {
    // TS uses userMessageBackground + dimColor text, 1 row, truncate-end.
    // We approximate: a single dim line on a subtle bg, ellipsised by FTXUI.
    return hbox({
        text(" "),
        text(std::string{body}) | dim | flex,
        text(" "),
    }) | bgcolor(Color::RGB(38, 38, 42))          // userMessageBackground-ish
      | size(HEIGHT, EQUAL, 1);
}

/// "N new messages ↓" pill — centred at the bottom of the scrollwrap.
/// Slack-style: floats over the last content row.  In FTXUI we compose it
/// as the last child of the scrollwrap vbox (a 1-row centred element), so
/// it overlays visually via dbox when stacked against the scroll content.
[[nodiscard]] inline Element NewMessagesPill(int count, bool actionable) {
    // count > 0 → "N new message(s)"; 0 → "Jump to bottom".
    std::string label = count > 0
        ? std::to_string(count) + " new message" + (count == 1 ? "" : "s")
        : std::string("Jump to bottom");
    // ↓ (U+2193) — TS uses figures.arrowDown.
    label += " \xE2\x86\x93";

    Color bg = actionable ? Color::RGB(58, 58, 64)   // userMessageBackgroundHover
                          : Color::RGB(48, 48, 52);  // userMessageBackground
    return hbox({
        text(" " + label + " ") | dim | color(Color::White) | bgcolor(bg),
    }) | center | size(HEIGHT, EQUAL, 1);
}

/// Modal pane: a bottom-anchored box with a ▔ top divider and the modal
/// body, sized so `kModalTranscriptPeek` rows of transcript remain visible
/// above it.  Mirrors the TS modal Box: position absolute, bottom 0,
/// maxHeight = terminalRows - MODAL_TRANSCRIPT_PEEK.
[[nodiscard]] inline Element ModalPane(Element body, int term_cols, int term_rows) {
    int max_h = std::max(4, term_rows - kModalTranscriptPeek);
    // ▔ (U+2594) divider spanning the full width (TS: "▔".repeat(columns)).
    // Build as UTF-8 so the multibyte sequence stays intact.
    static constexpr char kPeekDiv[] = "\xE2\x96\x94";   // ▔
    std::string divider;
    int n = std::max(1, term_cols);
    divider.reserve(static_cast<std::size_t>(n) * 3);
    for (int i = 0; i < n; ++i) divider.append(kPeekDiv);
    return vbox({
        text(divider) | color(Color::RGB(180, 130, 255)),   // permission-themed
        // Body kept at natural height — the outer size(LESS_THAN, max_h)
        // caps it and the bottom-anchor filler above absorbs slack.  Do
        // NOT flex here: a windowed dialog border compresses badly under
        // flex when the anchor region is taller than the dialog.
        std::move(body),
    }) | size(HEIGHT, LESS_THAN, max_h);
}

// ─── Main composer ──────────────────────────────────────────────────────────

/// Compose the 5-slot fullscreen shell from already-rendered slot Elements.
///
/// Layout (faithful to TS region model, expressed in FTXUI):
///   result = dbox(
///     vbox(                              // normal-flow base
///       scrollwrap,                      //   flexGrow=1 (scroll region)
///       bottom_slot                      //   flexShrink=0 (pinned)
///     ),
///     modal_overlay                      //   absolute bottom-anchored
///   )
///
/// where scrollwrap = vbox:
///     { stickyHeader?,                   //   1 row (optional)
///       scrollable_flex,                 //   flexGrow region
///       overlay?,                        //   painted inside scroll region
///       pill?                            //   centred bottom row
///       bottom_float? }                  //   bottom-right companion
///
/// @note All slot Elements are MOVED into the composition (FTXUI Elements
///       are shared_ptr-backed, cheap to move).  Callers should pass
///       `std::move(slot.scrollable)` etc.
[[nodiscard]] inline Element ComposeFullscreen(FullscreenLayoutSlots s) {
    // ── scrollwrap (flexGrow region) ────────────────────────────────────
    Elements scroll_children;
    // 1. Sticky prompt header (fixed 1 row).
    if (!s.hide_sticky && !s.sticky_prompt_text.empty()) {
        scroll_children.push_back(StickyPromptHeader(s.sticky_prompt_text));
    }
    // 2. The scrollable transcript — fills remaining height.  (Element is
    //    shared_ptr-backed; we move it into the vbox directly so the flex
    //    decorator wraps the shared_ptr, not a dereferenced Node.)
    if (s.scrollable) {
        scroll_children.push_back(std::move(s.scrollable) | flex);
    } else {
        // No content yet — reserve the region with a flex filler so the
        // bottom slot still pins correctly (mirrors TS flexGrow fallback).
        scroll_children.push_back(filler());
    }
    // 3. Overlay painted inside the scroll region (PermissionRequest).
    //    In TS this lives INSIDE <ScrollBox>{scrollable}{overlay}</ScrollBox>
    //    so the user can scroll up to see context.  Appending it after the
    //    flex region keeps it visually pinned to the bottom of the visible
    //    scroll area when present.
    if (s.overlay && *s.overlay) {
        scroll_children.push_back(std::move(*s.overlay));
    }
    // 4. New-messages pill (centred, 1 row).  TS renders this absolutely
    //    over the last scroll row; here it is a 1-row centred element at
    //    the tail of the scrollwrap, visually equivalent for static render.
    if (!s.hide_pill && s.pill_visible) {
        scroll_children.push_back(NewMessagesPill(s.new_message_count,
                                                  s.pill_actionable));
    }
    // 5. bottomFloat companion (absolute bottom-right in TS).  Stubbed as
    //    a right-aligned row at the scrollwrap tail when present.
    if (s.bottom_float && *s.bottom_float) {
        scroll_children.push_back(hbox({ filler(),
                                         std::move(*s.bottom_float) }));
    }

    Element scrollwrap = vbox(std::move(scroll_children)) | flex;

    // ── bottom slot (flexShrink=0, maxHeight ≈ 50%) ─────────────────────
    //    TS wraps {SuggestionsOverlay, DialogOverlay, bottom} in a column
    //    with flexShrink=0 and maxHeight="50%".  We cap at half the
    //    terminal so a giant bottom region never starves the scroll area.
    int bottom_max_h = std::max(4, s.term_rows / 2);
    Element bottom_slot = s.bottom
        ? (std::move(s.bottom) | size(HEIGHT, LESS_THAN, bottom_max_h))
        : (filler() | size(HEIGHT, EQUAL, 0));

    // ── normal-flow base: scrollwrap grows, bottom pinned ──────────────
    Element base = vbox({
        std::move(scrollwrap),
        std::move(bottom_slot),
    }) | flex;

    // ── modal overlay (absolute bottom-anchored, via dbox) ─────────────
    //   TS: position absolute, bottom:0, maxHeight=rows-PEEK.  In FTXUI a
    //   dbox stacks elements left-aligned at the top and clips to the
    //   viewport, so to bottom-anchor we wrap the pane in a fixed-height
    //   (term_rows) box with a filler above it.  The filler absorbs the
    //   slack, leaving the pane at its NATURAL height anchored to the
    //   bottom (we do NOT flex the pane — that would compress the window
    //   border the way the old flat layout did).  The pane's own
    //   size(LESS_THAN, max_h) caps it so kModalTranscriptPeek rows of
    //   transcript stay visible above.
    if (s.modal && *s.modal) {
        Element pane = ModalPane(std::move(*s.modal), s.term_cols, s.term_rows);
        Element modal_overlay = vbox({
            filler(),
            std::move(pane),
        }) | size(HEIGHT, EQUAL, s.term_rows);
        return dbox({ std::move(base), std::move(modal_overlay) });
    }
    return base;
}

} // namespace cc::ui::layout::fullscreen
