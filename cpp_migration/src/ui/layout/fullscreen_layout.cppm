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
///     Interactive: click collapses it to a 0-row pad so row 0 of the
///     scroll region becomes the prompt that generated the visible
///     assistant responses (TS 3-state `stickyPrompt`: null / {text,
///     scrollTo} / 'clicked').
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
#include <functional>
#include <memory>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>

import cc.ui.design.theme;
import cc.ui.design.tokens;
import cc.ui.design.figures;

export module cc.ui.layout.fullscreen;

export namespace cc::ui::layout::fullscreen {

using namespace ftxui;
using Theme   = cc::ui::design::theme::Theme;
using Palette = cc::ui::design::tokens::Palette;
using Role    = cc::ui::design::tokens::Role;
namespace figures_ns = cc::ui::design::figures;

// ─── Constants (1:1 with TS FullscreenLayout.tsx) ───────────────────────────

/// Rows of transcript context kept visible above the modal pane's ▔ divider.
/// (TS: `const MODAL_TRANSCRIPT_PEEK = 2`.)
inline constexpr int kModalTranscriptPeek = 2;

// ─── Component-lifetime guard (copied verbatim from permission_rule_list.cppm
//     CompEl pattern — commit bd507bc).  An Element that holds a Component
//     shared_ptr alive so the Component's internal Box (used by mouse
//     reflect(), OnEvent) does not become a dangling reference.
// ──────────────────────────────────────────────────────────────────────────
namespace compel_detail {

class ComponentHolderNode : public Node {
 public:
    Component held_;
    ComponentHolderNode(Component c, Element el)
        : Node(Elements{std::move(el)}), held_(std::move(c)) {}
    void ComputeRequirement() override {
        Node::ComputeRequirement();
        requirement_ = children_[0]->requirement();
    }
    void SetBox(Box box) override {
        Node::SetBox(box);
        children_[0]->SetBox(box);
    }
};

}  // namespace compel_detail

/// Wrap a Component's rendered Element in a Node that owns the Component
/// shared_ptr.  This is required whenever a Component (e.g. Button, or any
/// subclass with an internal `Box box_` / `OnEvent`) is rendered into a
/// stateless Element tree: dropping the Component immediately after
/// `Render()` produces a heap-use-after-free because the returned Element's
/// Nodes reference `&box_` / other fields on the destroyed Component.
[[nodiscard]] inline Element CompEl(Component c) {
    if (!c) return emptyElement();
    Element el = c->Render();
    return std::make_shared<compel_detail::ComponentHolderNode>(
        std::move(c), std::move(el));
}

// ─── Sticky prompt 3-state model (TS REF: FullscreenLayout.tsx lines 293,
//     339-350, 551-589) ────────────────────────────────────────────────────
//
// TS `stickyPrompt` is a 3-way useState discriminant:
//   null                              → at bottom, no header
//   { text: string, scrollTo: fn }    → scrolled up; header visible
//   'clicked' (literal sentinel)      → user just clicked header → hide
//                                        it but keep padCollapsed=true so
//                                        the prompt occupies scroll row 0
//
// `padCollapsed = sticky != null && overlay == null` drops paddingTop from
// 1 → 0 as soon as sticky is non-null, EVEN IN 'clicked' — this is how
// clicking the header leaves the prompt that generated the visible replies
// anchored at the absolute top of the visible scroll area.  On the next
// scroll event, StickyTracker re-emits {text, scrollTo} (new scroll
// window → new sticky prompt) and the header re-appears.
//
// We model this as:
//   sticky_active        → sticky != null (drives padCollapsed)
//   sticky_header_visible → sticky is an OBJECT (not null, not 'clicked')
//   sticky_prompt_text   → sticky.text
//   on_sticky_click      → sticky.scrollTo wrapped to set the 'clicked'
//                          sentinel before invoking the real scrollTo

struct StickyPrompt {
    /// Header prompt text.  Empty is equivalent to stickyPrompt=null in TS.
    std::string text;
    /// Row index the header click should scroll to (content-coordinate, as
    /// produced by VirtualMessageList's StickyTracker).  Maps to
    /// TS `stickyPrompt.scrollTo` parameter capture (a closure that calls
    /// scrollTo(target) internally).
    std::size_t scroll_target_row = 0;
};

// ─── Slot bundle ────────────────────────────────────────────────────────────
//
// Mirrors TS `type Props = { scrollable, bottom, overlay?, modal?,
//                            bottomFloat? }` plus the chrome-driver props
// (hidePill / hideSticky / newMessageCount / pillVisible).  Every field has
// a default so a caller can populate incrementally without breaking.

struct FullscreenLayoutSlots {
    /// Pinned header drawn ABOVE the scroll region (never scrolls out of
    /// view).  Maps to TS `<LogoHeader>` in Messages.tsx line 679 — placed
    /// OUTSIDE <VirtualMessageList>, so scrolling messages never clip it.
    /// Optional; render-time null check skips it.
    Element header{};

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

    /// Sticky prompt 3-state model (TS REF: FullscreenLayout.tsx line 293).
    ///   std::nullopt                        → null (at bottom, no chrome)
    ///   {text, scroll_target_row}           → mid scroll, header visible
    ///   {text, scroll_target_row} + `sticky_clicked=true` → 'clicked' sentinel
    ///                                         (header hidden, pad collapsed)
    std::optional<StickyPrompt> sticky_prompt{};

    /// True when stickyPrompt is the 'clicked' sentinel (TS REF:
    /// FullscreenLayout.tsx line 340 `sticky !== 'clicked'` guard).
    /// When true: header is hidden but padCollapsed still applies.
    bool sticky_clicked{false};

    /// Callback fired by the StickyPromptHeader click handler.  The closure
    /// should (1) flip `sticky_clicked=true` and (2) perform the actual
    /// scroll-to-target (TS REF: line 344 `<StickyPromptHeader
    /// onClick={headerPrompt.scrollTo}/>` wraps `scrollTo`).  We intentionally
    /// decouple "state change" from "scroll mutation" here: callers can
    /// implement re-render loops without this module owning scroll state.
    std::function<void(const StickyPrompt&)> on_sticky_click{};

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

    /// Callback fired when the user "clicks" the pill — scroll-to-bottom.
    /// When set, the pill is rendered as an interactive Button (FTXUI
    /// Component) instead of a static Element, so mouse clicks dispatch.
    std::function<void()> on_pill_click{};
};

// ─── Chrome helpers ─────────────────────────────────────────────────────────

/// Resolve the active palette via ThemeProvider current_theme() fallback.
[[nodiscard]] inline const Palette& active_palette() noexcept {
    return *cc::ui::design::theme::current_theme().palette;
}

/// Sticky prompt header — interactive Component that mirrors TS
/// `StickyPromptHeader` (FullscreenLayout.tsx lines 551-589):
///
///   • Fixed-height 1 row (truncate-end for long prompts)
///   • Background = userMessageBackground, hover = userMessageBackgroundHover
///   • Text = subtle color, prefix = figures.pointer (▶)
///   • onClick() invokes the passed callback (which scrolls + enters
///     'clicked' sentinel state)
///   • onMouseEnter/Leave toggles hover styling
///
/// FTXUI LIFETIME: callers MUST wrap the returned Component in `CompEl()`
/// before inserting it into an Element tree, otherwise the Component's
/// `Box box_` (used by reflect/OnEvent) is a UAF after the Component is
/// dropped (see CompEl note above / commit bd507bc).
class StickyPromptHeaderComponent : public ComponentBase {
 public:
    using OnClickFn = std::function<void()>;

    StickyPromptHeaderComponent(std::string text, OnClickFn on_click)
        : text_(std::move(text)), on_click_(std::move(on_click)) {}

    Element Render() override {
        const auto& pal = active_palette();
        const Color bg = hovered_
            ? cc::ui::design::tokens::token_by_role(pal, Role::UserMessageBackgroundHover)
            : cc::ui::design::tokens::token_by_role(pal, Role::UserMessageBackground);
        const Color fg = cc::ui::design::tokens::token_by_role(pal, Role::Subtle);
        // TS REF: line 572 — `figures.pointer` prefix (▶ U+25B6).
        // NOTE: figures_ns::kPointer is constexpr string_view; std::string
        // ctor requires explicit materialization.
        const std::string pointer(figures_ns::kPointer);
        // NOTE: This FTXUI build does not expose a `truncation()` decorator.
        // We approximate end-truncation with xflex_shrink, which collapses
        // the text to available space and appends a default ellipsis in the
        // screen rasteriser when it overflows.  The "…" character is still
        // prepended to the trailing space as a visual sentinel.
        return hbox({
            text(" "),
            text(pointer) | color(fg),
            text(" "),
            text(text_) | color(fg) | xflex_shrink,
            text(" "),
        }) | bgcolor(bg)
           | reflect(box_)
           | size(HEIGHT, EQUAL, 1);
    }

    bool OnEvent(Event event) override {
        // TS REF: lines 562-569 — onClick + onMouseEnter/Leave.
        if (event.is_mouse()) {
            const auto& m = event.mouse();
            const bool inside = box_.Contain(m.x, m.y);
            // hover follows the cursor entering/leaving the row.  This FTXUI
            // version does not surface a separate Mouse::Moved motion enum —
            // every mouse event carries the current cursor position, so we
            // update the hover state from any event whose cursor is inside
            // the reflected box.
            if (inside != hovered_) {
                hovered_ = inside;
                return true;
            }
            if (!inside) return false;
            if (m.button == Mouse::Left && m.motion == Mouse::Released) {
                hovered_ = false;
                if (on_click_) on_click_();
                return true;
            }
        }
        // Also accept Enter/Space as click (keyboard-accessible fallback).
        if (event == Event::Return || event == Event::Character(' ')) {
            if (on_click_) on_click_();
            return true;
        }
        return ComponentBase::OnEvent(std::move(event));
    }

    bool Focusable() const override { return static_cast<bool>(on_click_); }

 private:
    std::string text_;
    OnClickFn on_click_;
    bool hovered_ = false;
    Box box_;
};

/// Thin helper: wrap a StickyPromptHeaderComponent in a Component and then
/// in CompEl so it is safe to drop into an Element tree.
[[nodiscard]] inline Element StickyPromptHeader(std::string text,
    std::function<void()> on_click) {
    auto c = Make<StickyPromptHeaderComponent>(std::move(text),
                                                std::move(on_click));
    return CompEl(std::move(c));
}

/// "N new messages ↓" pill — centred at the bottom of the scrollwrap.
/// Slack-style: floats over the last content row.  In FTXUI we compose it
/// as the last child of the scrollwrap vbox (a 1-row centred element), so
/// it overlays visually via dbox when stacked against the scroll content.
[[nodiscard]] inline Element NewMessagesPill(int count, bool actionable,
                                             std::function<void()> on_click) {
    // count > 0 → "N new message(s)"; 0 → "Jump to bottom".
    std::string label = count > 0
        ? std::to_string(count) + " new message" + (count == 1 ? "" : "s")
        : std::string("Jump to bottom");
    // TS REF: `figures.arrowDown` (↓ U+2193).  kArrowDown is a string_view so
    // materialize it to std::string for concatenation with const char[].
    label += " " + std::string(figures_ns::kArrowDown);

    const auto& pal = active_palette();
    const Color bg_normal = cc::ui::design::tokens::token_by_role(
        pal, Role::UserMessageBackground);
    const Color bg_hover = cc::ui::design::tokens::token_by_role(
        pal, Role::UserMessageBackgroundHover);
    const Color fg = cc::ui::design::tokens::token_by_role(pal, Role::Subtle);

    // If a click callback is supplied, render as an interactive Component
    // (Button pattern via a small OnEvent wrapper) so mouse events fire.
    if (on_click) {
        class PillComponent : public ComponentBase {
         public:
            PillComponent(std::string label, Color bg_n, Color bg_h,
                          Color fg, std::function<void()> cb)
                : label_(std::move(label)), bg_n_(bg_n), bg_h_(bg_h),
                  fg_(fg), cb_(std::move(cb)) {}
            Element Render() override {
                const Color bg = hovered_ ? bg_h_ : bg_n_;
                return hbox({
                    filler(),
                    text(" " + label_ + " ") | color(fg_) | bgcolor(bg),
                    filler(),
                }) | reflect(box_) | size(HEIGHT, EQUAL, 1);
            }
            bool OnEvent(Event ev) override {
                if (ev.is_mouse()) {
                    const auto& m = ev.mouse();
                    const bool inside = box_.Contain(m.x, m.y);
                    // Mirror StickyPromptHeader hover logic (see note there).
                    if (inside != hovered_) {
                        hovered_ = inside;
                        return true;
                    }
                    if (!inside) return false;
                    if (m.button == Mouse::Left && m.motion == Mouse::Released) {
                        hovered_ = false;
                        if (cb_) cb_();
                        return true;
                    }
                }
                return ComponentBase::OnEvent(std::move(ev));
            }
         private:
            std::string label_;
            Color bg_n_, bg_h_, fg_;
            std::function<void()> cb_;
            bool hovered_ = false;
            Box box_;
        };
        (void)actionable;
        return CompEl(Make<PillComponent>(std::move(label), bg_normal,
                                          bg_hover, fg, std::move(on_click)));
    }

    // Stateless fallback: render as plain text with actionable background.
    const Color bg = actionable ? bg_hover : bg_normal;
    return hbox({
        filler(),
        text(" " + label + " ") | color(fg) | bgcolor(bg),
        filler(),
    }) | size(HEIGHT, EQUAL, 1);
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
    // TS REF: line 426 — color="permission" for the divider.
    const auto& pal = active_palette();
    const Color perm = cc::ui::design::tokens::token_by_role(pal, Role::Permission);
    return vbox({
        text(divider) | color(perm),
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
///     { stickyHeader?,                   //   1 row (optional; 3-state)
///       paddingTop,                      //   0 or 1 row (padCollapsed)
///       scrollable_flex,                 //   flexGrow region
///       overlay?,                        //   painted inside scroll region
///       pill?                            //   centred bottom row
///       bottom_float? }                  //   bottom-right companion
///
/// Sticky 3-state logic (TS REF: FullscreenLayout.tsx lines 339-351):
///   const sticky       = hideSticky ? null : stickyPrompt
///   const headerPrompt = sticky != null && sticky !== 'clicked'
///                        && overlay == null ? sticky : null
///   const padCollapsed = sticky != null && overlay == null
///
/// Translated into C++ field semantics:
///   sticky_active        = sticky_prompt.has_value()
///   header_visible       = sticky_active && !sticky_clicked
///                          && !overlay.has_value() && !hide_sticky
///   pad_top              = (sticky_active && !overlay.has_value()) ? 0 : 1
///
/// @note All slot Elements are MOVED into the composition (FTXUI Elements
///       are shared_ptr-backed, cheap to move).  Callers should pass
///       `std::move(slot.scrollable)` etc.
[[nodiscard]] inline Element ComposeFullscreen(FullscreenLayoutSlots s) {
    // ── 3-state sticky resolution (TS REF: FullscreenLayout.tsx:339-351) ──
    const bool overlay_present = s.overlay.has_value() && *s.overlay;
    const bool sticky_active =
        !s.hide_sticky && s.sticky_prompt.has_value();
    // TS: `headerPrompt = sticky != null && sticky !== 'clicked' && overlay == null`
    const bool header_visible = sticky_active && !s.sticky_clicked
                             && !overlay_present
                             && !s.sticky_prompt->text.empty();
    // TS: `padCollapsed = sticky != null && overlay == null`
    // padTop = padCollapsed ? 0 : 1  (TS line 350 & 361: paddingTop={t9})
    const int padding_top = (sticky_active && !overlay_present) ? 0 : 1;

    // ── scrollwrap (flexGrow region) ────────────────────────────────────
    Elements scroll_children;

    // 1. Sticky prompt header (fixed 1 row, interactive).
    //    TS REF: lines 343-349.
    if (header_visible) {
        auto prompt = *s.sticky_prompt;
        std::function<void()> cb = nullptr;
        if (s.on_sticky_click) {
            cb = [prompt, cb = std::move(s.on_sticky_click)] {
                cb(prompt);
            };
        }
        scroll_children.push_back(
            StickyPromptHeader(std::move(prompt.text), std::move(cb)));
    }

    // 2. ScrollBox paddingTop row.  TS ScrollBox wraps scrollable+overlay
    //    with paddingTop = padCollapsed ? 0 : 1 (line 361).  FTXUI vbox
    //    has no direct paddingTop, so we emulate with an explicit 1-row
    //    separator() of background color OR a 0-row text("").
    if (padding_top > 0) {
        // Render a 1-row pad styled as the background.  In the TS
        // ScrollBox, paddingTop is drawn from the ScrollBox's own
        // background (canvas colour); we match with palette.background.
        const auto& pal = active_palette();
        scroll_children.push_back(text("")
            | bgcolor(pal.background)
            | size(HEIGHT, EQUAL, padding_top));
    }

    // 3. The scrollable transcript — fills remaining height.
    if (s.scrollable) {
        scroll_children.push_back(std::move(s.scrollable) | flex);
    } else {
        scroll_children.push_back(filler());
    }

    // 4. Overlay painted inside the scroll region (PermissionRequest).
    //    In TS this lives INSIDE <ScrollBox>{scrollable}{overlay}</ScrollBox>
    //    so the user can scroll up to see context.
    if (overlay_present) {
        scroll_children.push_back(std::move(*s.overlay));
    }

    // 5. New-messages pill (centred, 1 row).  TS REF: lines 371-381.
    if (!s.hide_pill && s.pill_visible && !overlay_present) {
        scroll_children.push_back(NewMessagesPill(
            s.new_message_count,
            /*actionable=*/static_cast<bool>(s.on_pill_click),
            std::move(s.on_pill_click)));
    }

    // 6. bottomFloat companion (absolute bottom-right in TS).
    if (s.bottom_float && *s.bottom_float) {
        scroll_children.push_back(hbox({ filler(),
                                         std::move(*s.bottom_float) }));
    }

    Element scrollwrap = vbox(std::move(scroll_children)) | flex;

    // ── bottom slot (flexShrink=0, maxHeight ≈ 50%) ─────────────────────
    //    TS wraps {SuggestionsOverlay, DialogOverlay, bottom} in a column
    //    with flexShrink=0 and maxHeight="50%".
    int bottom_max_h = std::max(4, s.term_rows / 2);
    Element bottom_slot = s.bottom
        ? (std::move(s.bottom) | size(HEIGHT, LESS_THAN, bottom_max_h))
        : (filler() | size(HEIGHT, EQUAL, 0));

    // ── normal-flow base: [pinned header (non-scroll)] + scrollwrap + bottom ──
    Elements normal_flow;
    normal_flow.reserve(3);
    if (s.header) normal_flow.push_back(std::move(s.header) | flex_shrink);
    normal_flow.push_back(std::move(scrollwrap));
    normal_flow.push_back(std::move(bottom_slot));
    Element base = vbox(std::move(normal_flow)) | flex;

    // ── modal overlay (absolute bottom-anchored, via dbox) ─────────────
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
