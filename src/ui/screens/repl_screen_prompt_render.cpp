// repl_screen_prompt_render.cpp - impl unit for cc.ui.screens.repl_screen
// (RFC 0001 Phase C batch 9). placeholder cascade adapter, the full prompt-input renderer (synced
// TextInputImpl + vim badge + stash notice + declared caret), and the
// autocomplete suggestions dropdown.
//
// Phase A (#184957): textual FTXUI GMF headers + `import std;`
// (the textual FTXUI includes keep the reduced-BMI writer happy).
module;

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/screen.hpp>  // ftxui::Screen::Cursor

module cc.ui.screens.repl_screen;

import std;

import cc.ui.widgets.text_input;
import cc.ui.foundation.design_figures;
import cc.ui.foundation.design_tokens;
import cc.ui.foundation.theme_provider;
import cc.ui.prompt.vim_input;
import cc.ui.foundation.declared_cursor;
import cc.ui.prompt.prompt_stash_notice;
import cc.ui.prompt.placeholder_cascade;

namespace cc::ui::repl_screen {
using namespace ftxui;

// ─── Placeholder cascade (TS REF: usePromptInputPlaceholder.ts + PromptInput.tsx) ──
//
// Faithful port of the TS contextual placeholder system.  Priority order:
//
//   1. Input non-empty          → std::nullopt (no placeholder)
//   2. AI prompt suggestion     → next_action_suggestion (override layer from
//                                  PromptInput.tsx line 2014)
//   3. Viewing teammate         → "Message @{name}..."
//   4. Queued commands hint     → "Press up to edit queued messages"
//                                  (shown ≤3 times, only if editable queued cmds exist)
//   5. Onboarding example       → "Try \"{example command}\""
//                                  (only before first submit, with suggestions enabled)
//   6. Fallback                 → std::nullopt (no placeholder shown)
//
// Implementation lives in cc.ui.prompt.placeholder_cascade module for
// reusability by standalone TextInputImpl and dialog widgets.  This thin
// adapter projects ReplScreenState onto PlaceholderContext.

[[nodiscard]] std::optional<std::string> ComputePlaceholder(
    const ReplScreenState& s) {
    namespace ph = cc::ui::placeholder;

    ph::PlaceholderContext ctx;
    ctx.input_text                    = s.input_text;
    ctx.input_mode                    = s.input_mode;
    ctx.submit_count                  = s.submit_count;
    ctx.queued_hint_shown_count       = s.queued_command_hint_shown_count;
    ctx.has_editable_queued           = s.has_editable_queued_commands;
    ctx.prompt_suggestion_enabled     = s.prompt_suggestion_enabled;
    ctx.autocomplete_suggestions_empty = s.autocomplete_suggestions.empty();

    if (s.viewing_agent_name.has_value()) {
        ctx.viewing_agent_name = std::string_view(*s.viewing_agent_name);
    }
    if (s.next_action_suggestion.has_value()) {
        ctx.next_action_suggestion = std::string_view(*s.next_action_suggestion);
    }

    return ph::ComputePlaceholder(ctx);
}

// UI2: prompt input shell.  Full feature parity in prompt_input_full.cppm.
//
// M3 — WIRED TO THE REAL COMPONENT.  Previously this was a ~90-line
// hand-rolled body that IGNORED the real ui::components::TextInputImpl
// (cursor tracking, selection, vim modes, autocomplete, multiline, masking)
// and was flagged 0/25 faithful by the 1:1 audit.  We now delegate the
// caret/multiline/selection painting to a TextInputImpl that is SYNCED from
// ReplScreenState each render — mirroring TS BaseTextInput.tsx's
// useDeclaredCursor (which parks the real terminal cursor at the insertion
// point and lets screen readers follow the input).
//
// The pure-function signature `Element RenderPromptInput(const
// ReplScreenState&)` is PRESERVED so app.cppm's input handling (which writes
// s.input_text / s.input_mode / s.autocomplete_* between frames and forwards
// keystrokes via ReplScreen's CatchEvent) is untouched.  The TextInputImpl
// is used purely as a render primitive here — it is rebuilt per-frame from
// the projection, never as the interactive event target.
//
// Rendered faithful to TS BaseTextInput.tsx:
//   * TS prompt glyph figures.pointer "❯" (green) for normal mode,
//     "!" (red) for bash, "❮" (yellow/magenta) for vim Normal/Visual —
//     driven by s.input_mode.
//   * DECLARED CARET at the insertion point: TextInputImpl.Render() draws an
//     inverted glyph at the cursor offset (TS parks the real terminal cursor
//     there via useDeclaredCursor; we render a visible caret that lands on
//     the same byte offset).  Multi-line content lays out as a vbox.
//   * Contextual placeholder when empty (TS renderPlaceholder), styled dim.
//   * Selection highlight (TS HighlightedInput path) — provided by the real
//     impl when a selection range is set.
//   * Vim-mode badge (-- INSERT -- / -- NORMAL -- / -- VISUAL --) like TS,
//     driven by the existing vim_input::mode_display() helper.
//   * Prompt chrome uses top and bottom horizontal rules, matching TS
//     borderStyle="round" with left/right borders disabled.
[[nodiscard]] Element RenderPromptInput(const ReplScreenState& s,
                                                  int term_cols) {
    namespace uic   = ::ui::components;
    namespace vim   = cc::ui::prompt::vim_input;
    namespace figs  = cc::ui::design::figures;

    // --- 1. Prompt glyph + accent colour (TS faithfulness, unified) ---------
    //
    // REFERENCE (TS files):
    //   PromptInputModeIndicator.tsx + inputModes.ts + theme.ts
    //
    // SEMANTICS (simplified from TS — the CPP InputMode enum is kept
    // intact for backward compat with autocomplete gates in app.cppm,
    // but the PREFIX GLYPH COLLAPSES to exactly TWO visual variants per TS,
    // with priority matching PromptInputModeIndicator.tsx line 82):
    //
    //   PRIORITY 1 — viewingAgentName set:
    //                                        glyph = kPointer     "❯"
    //                                        color = teammate_prefix_color
    //                                                or palette.text
    //   PRIORITY 2 — mode == Bash (no viewing agent):
    //                                        glyph = kBashGlyph   "!"
    //                                        color = bashBorder  rgb(255,0,135)
    //   PRIORITY 3 — ALL OTHER modes:     glyph = kPointer     "❯"
    //                                        color = teammate_prefix_color
    //                                                or palette.text
    //                 (teammate_prefix_color is the engine-resolved
    //                 AGENT_COLOR_TO_THEME_COLOR for both the viewing-agent
    //                 path and the swarms-enabled default path)
    //
    // The old CPP-only per-mode glyphs (Slash "/", History "?", Plan "▣",
    // VimNormal "❮", VimVisual "❮", Permission "!", Task "*") are ELIMINATED
    // from the prefix position per TS:
    //   - Slash / History are routing semantics, not visual glyphs — TS's
    //     PromptInputModeIndicator falls through to ❯ even for those.
    //   - Vim mode is shown as a SEPARATE badge below the prefix (the
    //     "-- INSERT --" / "-- NORMAL --" row rendered later in this fn).
    //   - Plan mode is shown as a badge in the footer (StatusLine) or as a
    //     bubble marker, never as a replacement prefix glyph.
    //   - OrphanedPermission / TaskNotification fall through to the default
    //     "❯" pointer per TS.
    //
    // Fetch the currently active palette via the theme provider (TS ThemeContext
    // equivalent).  This respects ThemeVariant::Dark / Light / Daltonized /
    // Monochrome plus the force_monochrome a11y flag.  theme::current_theme()
    // is a cheap value copy (2 pointers + 3 booleans) with a short mutex grab.
    namespace thm = cc::ui::design::theme;
    namespace tok = cc::ui::design::tokens;
    const tok::Palette& pal = *thm::current_theme().palette;
    // TS getInputMode(value) equivalent: text-derived when text is present,
    // state-toggle when empty.  See effective_is_bash() for rationale.
    const bool is_bash_mode = effective_is_bash(s);
    std::string prefix_str;     // passed into TextInputOptions.prefix;
    Color       prefix_color;   // applied to the prefix inside renderInputArea.

    // Step 1a: pick glyph.
    // Priority (TS REF: PromptInputModeIndicator.tsx line 82):
    //   1. viewingAgentName set  → ❯ (always, regardless of bash mode)
    //   2. mode === 'bash'       → !
    //   3. otherwise             → ❯
    // When a viewing agent is active, the prefix is ALWAYS ❯ (never !),
    // matching TS where `viewingAgentName ?` is checked BEFORE
    // `mode === 'bash'`.
    const bool has_viewing_agent = s.viewing_agent_name.has_value()
        && !s.viewing_agent_name->empty();
    const bool show_bash_glyph = is_bash_mode && !has_viewing_agent;
    prefix_str += show_bash_glyph
        ? std::string(figs::kBashGlyph)
        : std::string(figs::kPointer);
    prefix_str += " ";   // trailing NBSP/space — 2 display cells total (TS).

    // Step 1b: pick color.
    //
    // Priority matches the glyph selection above:
    //   1. viewingAgentName set  → teammate_prefix_color (engine-resolved
    //                               agent color) or palette.text
    //   2. bash mode (no viewing agent) → bashBorder
    //   3. otherwise             → teammate_prefix_color or palette.text
    //
    // Bash mode always uses bashBorder (TS: dark rgb(255,0,135), daltonized
    // blue variants, light same).  All other modes: use the teammate color if
    // the engine has supplied one via s.teammate_prefix_color (TS
    // AGENT_COLOR_TO_THEME_COLOR map in agentColorManager.ts), otherwise fall
    // through to palette.text (dark: pure white, light: pure black).
    if (show_bash_glyph) {
        prefix_color = pal.bash_border;
    } else if (s.teammate_prefix_color.has_value()) {
        prefix_color = *s.teammate_prefix_color;
    } else {
        prefix_color = pal.text;
    }
    // (Prefix rendering happens INSIDE TextInputImpl via opts.prefix — see
    // below — so cursor_display_col returns correct values automatically
    // and the declared cursor lands at the right screen column.)

    // --- 2. Sync a TextInputImpl from the projection --------------------
    uic::TextInputOptions opts;
    // Compute contextual placeholder via the TS-faithful cascade.
    // If the cascade returns nullopt (no condition matched), fall back to
    // the static input_placeholder string for backward compatibility with
    // standalone TextInputImpl usage.
    if (auto computed = ComputePlaceholder(s); computed.has_value()) {
        opts.placeholder = *std::move(computed);
    } else {
        opts.placeholder = s.input_placeholder;
    }
    opts.prefix       = std::move(prefix_str);   // ← RENDERED INSIDE now (BUG-2 fix)
    opts.prefix_color = prefix_color;            // ← new field: explicit color for prefix
    opts.multiline    = true;
    opts.show_line_numbers = false;
    opts.enable_undo_redo   = false;
    opts.cursor_blink_ms    = 0;   // deterministic snapshot (no flicker)
    opts.show_history       = false;
    opts.argument_hint      = s.pending_argument_hint;  // SL-03
    opts.inline_ghost_text = s.pending_ghost_text;      // SL-05
    auto impl = std::make_shared<uic::TextInputImpl>(std::move(opts));
    impl->set_text(s.input_text);  // parks cursor at end of buffer
    const auto cursor = input_cursor_or_end(s);
    const auto end = s.input_text.size();
    if (cursor < end) {
        impl->move_cursor(
            -static_cast<int>(end - cursor),
            /*extend_selection=*/false);
    }

    // --- 3. Render the input area from the REAL component ---------------
    Element input_area = impl->RenderInputAreaPub();

    // --- 3b. Declared cursor (IME / accessibility) ----------------------
    // Faithful port of TS useDeclaredCursor: park the real terminal cursor at
    // the insertion point so IME preedit renders inline and screen readers /
    // magnifiers can follow the input.
    //
    // NOTE on cursor-display math (BUG-2 FIXED):
    //   We previously rendered the prefix OUTSIDE TextInputImpl in a separate
    //   hbox, but left opts.prefix empty.  declared_cursor then used
    //   `string_width(opts.prefix) = 0` as the prefix width, so the native
    //   cursor parked 3-4 display columns to the LEFT of actual text start.
    //
    //   After this commit: opts.prefix contains the rendered 2-cell glyph,
    //   impl->cursor_display_col() includes prefix width in its return, and
    //   declared_cursor below positions the terminal cursor EXACTLY over the
    //   character where the next keystroke will insert.  No arithmetic tricks
    //   are required — TextInputImpl's own prefix logic (see RenderInputArea
    //   inside text_input.cppm) already accounts for it.
    //
    //   We ALSO add 1 column for the left-side " " padding space rendered in
    //   hbox #5 (the `text(" ")` before the text area hbox row — this is a
    //   pure layout margin that TextInputImpl does NOT know about so we
    //   account for it here manually).
    {
        // Native terminal cursor parks at the screen bottom-right (Hidden) via
        // the root CursorResetNode in app.cppm.  We intentionally do NOT declare
        // the prompt caret position here: FTXUI's ScreenInteractive emits a
        // cursor-MOVE sequence every frame (from bottom-right to the declared
        // position) even when nothing else changed, and many terminals render
        // those hidden-cursor moves as visible flicker during the ~20Hz idle
        // re-render.  Leaving the cursor at bottom-right makes the move delta
        // zero, so FTXUI emits no move and the idle frame is flicker-free.
        // The visible caret is still drawn by TextInputImpl (inverted glyph), so
        // the user sees their caret; only the hidden native cursor (IME/a11y
        // anchor) parks at the corner instead of over the caret.
    }

    Elements box_body;

    // --- 4. Vim-mode badge (-- INSERT -- / -- NORMAL -- / -- VISUAL --) -
    // Faithful to TS: drawn as a separate row (NOT a prefix glyph swap),
    // dim+bold, per-mode color (see vim_input.cppm mode_display).
    std::optional<std::pair<std::string, Color>> vim_badge;
    if (s.input_mode == InputMode::VimInsert)
        vim_badge = {"-- INSERT --", vim::mode_display(vim::VimMode::Insert).second};
    else if (s.input_mode == InputMode::VimNormal)
        vim_badge = {"-- NORMAL --", vim::mode_display(vim::VimMode::Normal).second};
    else if (s.input_mode == InputMode::VimVisual)
        vim_badge = {"-- VISUAL --", vim::mode_display(vim::VimMode::Visual).second};
    if (vim_badge) {
        box_body.push_back(hbox({
            text("  "),
            text(vim_badge->first) | color(vim_badge->second) | bold | dim,
        }));
    }

    // --- 4b. Stash notice (GAP 2: stashed-prompt-restore-logic-missing) ---
    // TS REF: PromptInputStashNotice.tsx — renders
    //   "{figures.pointerSmall} Stashed (auto-restores after submit)"
    //   when hasStash is true.  Shown above the input area so the user knows
    //   their typed input was saved and will be restored after the current
    //   request completes.
    if (s.stashed_prompt.has_value()) {
        namespace psn = cc::ui::prompt;
        psn::StashNotice notice;
        notice.stashed_text = s.stashed_prompt->text;
        notice.char_count = s.stashed_prompt->text.size();
        // TS REF: <Box paddingLeft={2}> — render_stash_notice handles the
        // 2-space left padding internally, matching TS paddingLeft={2}.
        box_body.push_back(psn::render_stash_notice(notice));
    }

    // --- 5. Compose -----------------------------------------------------
    // Per TS layout: a leading space (`text(" ")`) followed by the
    // TextInputImpl's rendered output (which itself is `prefix_glyph + space
    // + text`).  The leading space was originally introduced so the glyph
    // doesn't hug the left edge; we keep it for visual parity.
    box_body.push_back(hbox({
        text(" "),
        input_area,
    }));

    auto content = vbox(std::move(box_body));

    // TS PromptInput.tsx:2237/2268: <Box borderStyle="round" borderLeft={false}
    // borderRight={false} borderBottom>.  Ink defaults borderTop to TRUE when
    // borderStyle is set (render-background.js: `borderTop !== false ? 1 : 0`).
    // The top border may carry `borderText` (fast-mode cooldown), but normally
    // it's just a plain horizontal rule — the prompt glyph `❯` lives INSIDE the
    // input area as the TextInput prefix, NOT in the border.
    //
    // FTXUI separator() renders as box-drawing characters, matching Ink's
    // border lines.  Both top and bottom rules use the mode-appropriate
    // border colour: bashBorder in Bash mode (TS: rgb(255,0,135) pink),
    // promptBorder otherwise (TS: grey).
    const Color frame_color = is_bash_mode ? pal.bash_border : pal.prompt_border;
    Element top_rule    = separator() | color(frame_color);
    Element bottom_rule = separator() | color(frame_color);

    // ── Declared cursor (IME / accessibility) ──────────────────────────────
    // Faithful port of TS useDeclaredCursor: park the real terminal cursor at
    // the insertion point so IME preedit renders inline and screen readers /
    // magnifiers can follow the input.
    //
    // Cursor position relative to the returned vbox:
    //   rel_y = 1 (top_rule) + (vim_badge ? 1 : 0)
    //   rel_x = 1 (leading space in hbox) + prefix_width + cursor_display_col()
    //
    // NOTE: cursor_display_col() returns width of text up to caret (NOT
    // including prefix), so we add prefix_width manually.
    namespace dc = cc::ui::common::declared_cursor;
    const int prefix_width = ftxui::string_width(opts.prefix);
    const int caret_col = impl->cursor_display_col();
    const int rel_y = 1 + (vim_badge ? 1 : 0);
    const int rel_x = 1 + prefix_width + caret_col;

    auto result = vbox({
        std::move(top_rule),
        content,
        std::move(bottom_rule),
    }) | size(WIDTH, EQUAL, std::max(term_cols, 40));

    // Apply declared_cursor so the hidden native cursor parks at the caret
    // position (TS: useDeclaredCursor).  This overrides cursor_reset()'s
    // bottom-right parking.  Shape=Hidden because the visible caret is drawn
    // by TextInputImpl itself (inverted glyph), not the terminal cursor.
    return std::move(result) | dc::declared_cursor(
        /*active=*/true, rel_x, rel_y,
        ftxui::Screen::Cursor::Shape::Hidden);
}

[[nodiscard]] std::string pad_to_columns(std::string text, int width) {
    const int pad = width - string_width(text);
    if (pad > 0) text.append(static_cast<std::size_t>(pad), ' ');
    return text;
}

/// TS REF: PromptInputFooterSuggestions.tsx — renders autocomplete suggestion
/// items in a vertical list.
///
/// Two rendering modes (matching TS):
///   - Non-fullscreen (inline in footer): adaptive maxVisibleItems =
///     min(6, max(1, term_rows - 3)), items bottom-aligned (flex-end).
///   - Fullscreen (overlay portal): floating overlay above the prompt with
///     opaque background, OVERLAY_MAX_ITEMS = 5, no flex-end alignment.
///
/// TS REF: FullscreenLayout.tsx L591-607 — overlay uses position="absolute"
/// bottom="100%" opaque={true} to escape the bottom-slot overflowY:hidden clip.
/// In FTXUI there's no CSS overflow clip, so we render inline but apply
/// overlay visual styling (background + top border) when is_overlay=true.
///
/// @param is_overlay  When true, apply fullscreen overlay styling.
/// @param term_rows   Terminal height in rows (for adaptive maxVisibleItems).
[[nodiscard]] Element RenderPromptSuggestions(const ReplScreenState& s,
                                                     int term_cols,
                                                     bool is_overlay,
                                                     int term_rows) {
    if (s.autocomplete_suggestions.empty()) return Element{};

    // TS REF: PromptInputFooterSuggestions.tsx L224 — maxVisibleItems differs
    // between overlay (fixed 5) and inline (adaptive to terminal height).
    constexpr int kOverlayMaxItems = 5;  // TS: OVERLAY_MAX_ITEMS
    const int kInlineMaxItems = std::min(6, std::max(1, term_rows - 3));
    const int kMaxVisibleItems = is_overlay ? kOverlayMaxItems : kInlineMaxItems;

    const int total = static_cast<int>(s.autocomplete_suggestions.size());
    const int selected = std::clamp(
        s.autocomplete_index < 0 ? 0 : s.autocomplete_index,
        0,
        total - 1);
    const int visible = std::min(kMaxVisibleItems, total);
    const int start = std::max(
        0,
        std::min(selected - visible / 2, total - visible));
    const int end = std::min(start + visible, total);

    int widest = 0;
    if (s.autocomplete_stable_name_width > 0) {
        // INF-03: stable precomputed width (slash-command path) — no jitter.
        widest = s.autocomplete_stable_name_width;
    } else {
        for (int i = start; i < end; ++i) {
            const auto& item = s.autocomplete_suggestions[static_cast<std::size_t>(i)];
            // TS REF: PromptInputFooterSuggestions.tsx — icon takes display width
            // before the label. Add icon width to the name column so labels
            // align vertically when some rows have icons and others don't.
            const int icon_w = item.icon.empty() ? 0 : string_width(item.icon) + 1;
            widest = std::max(
                widest,
                icon_w + string_width(item.display_text));
        }
    }
    const int max_name_width = std::max(10, term_cols * 2 / 5);
    const int name_width = std::min(widest + 5, max_name_width);
    const int desc_width = std::max(0, term_cols - name_width - 4);

    namespace thm = cc::ui::design::theme;
    const auto& pal = *thm::current_theme().palette;

    Elements rows;
    rows.reserve(static_cast<std::size_t>(visible));
    for (int i = start; i < end; ++i) {
        const auto& item = s.autocomplete_suggestions[static_cast<std::size_t>(i)];
        const bool is_selected = i == selected;

        // Build the label: optional colored dot + icon + display_text,
        // padded to name_width.
        // TS REF: PromptInputFooterSuggestions.tsx renderRow — icon glyph then
        // the label, both styled together.
        // TS REF: src/hooks/unifiedSuggestions.ts:77-108 — agent defs include
        //   a color field used to tint the avatar dot.
        Elements label_parts;
        int used = 0;
        // Colored dot for agent/teammate suggestions.
        if (!item.color_name.empty()) {
            auto agent_color = [&]() -> Color {
                if (item.color_name == "red") return Color::Red;
                if (item.color_name == "blue") return Color::Blue;
                if (item.color_name == "green") return Color::Green;
                if (item.color_name == "yellow") return Color::Yellow;
                if (item.color_name == "purple") return Color::Magenta;
                if (item.color_name == "orange") return Color::Yellow;
                if (item.color_name == "pink") return Color::MagentaLight;
                if (item.color_name == "cyan") return Color::Cyan;
                return Color::Default;
            }();
            label_parts.push_back(text("● ") | color(agent_color) | bold);
            used += 2;  // "● " is 2 display columns
        }
        if (!item.icon.empty()) {
            label_parts.push_back(text(item.icon + " "));
            used += string_width(item.icon) + 1;
        }
        const int text_budget = std::max(1, name_width - used);
        auto display = truncate_columns(item.display_text, text_budget);
        label_parts.push_back(text(pad_to_columns(std::move(display), text_budget)));

        Element name = hbox(std::move(label_parts));
        Element detail = text(truncate_columns(item.description, desc_width));
        if (is_selected) {
            // TS REF: selected item uses suggestion color (lavender in dark).
            name = name | color(pal.suggestion) | bold;
            detail = detail | color(pal.suggestion);
        } else {
            name = name | dim;
            detail = detail | dim;
        }
        Element row_el = hbox({
            text("  "),
            std::move(name),
            std::move(detail),
            filler(),
        });
        // TS REF: FullscreenLayout.tsx L607 — overlay items get the surface
        // background so the floating list doesn't show messages through it.
        if (is_overlay && is_selected) {
            row_el = row_el | bgcolor(pal.message_actions_background);
        }
        rows.push_back(std::move(row_el));
    }

    Element content = vbox(std::move(rows));

    // TS REF: FullscreenLayout.tsx L607 — overlay wrapper:
    //   <Box position="absolute" bottom="100%" ... opaque={true}>
    // In FTXUI we apply: background fill + top separator line to visually
    // separate the floating overlay from scrollback messages above it.
    if (is_overlay) {
        // Build a top separator line using the chrome color (TS border-top
        // equivalent).  The separator spans the full width so the overlay
        // reads as a distinct floating panel.
        Element top_sep = separator() | color(pal.chrome);
        content = vbox({
            text("") | size(HEIGHT, EQUAL, 1),  // marginTop=1 above overlay
            top_sep,
            hbox({text("  "), content, filler()}) | bgcolor(pal.background),
        });
    }

    return content;
}

}  // namespace cc::ui::repl_screen
