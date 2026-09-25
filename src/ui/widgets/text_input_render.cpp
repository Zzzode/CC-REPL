// text_input_render.cpp - impl unit for cc.ui.widgets.text_input
// (RFC 0001 Phase C batch 8). All FTXUI rendering: TextInputImpl::Render and
// the input-area caret/multiline/selection painter (incl. the [Image #N]
// chip inversion that needs cc.utils.parse_references), the suggestions
// dropdown, reverse-search panel, paste-preview overlay, the two public
// render primitives (RenderInputAreaPub / RenderSuggestionsFromListPub),
// cursor_display_col, and the free TextInput() component factory.
// suggest_util::category_icon/category_color stay inline in the primary.
//
// Phase A (#184957): textual FTXUI dom/component headers + `import std;`
// (the textual FTXUI includes keep the reduced-BMI writer happy).
module;

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/screen/string.hpp>  // for string_width

module cc.ui.widgets.text_input;

import std;

import cc.utils.parse_references;
import cc.ui.prompt.prompt_paste_handler;
import cc.ui.prompt.placeholder_cascade;

namespace ui::components {

// ------------------------------------------------------------
// Cursor display position (for declared cursor / IME support)
// ------------------------------------------------------------
/// Display column (visual width) of the cursor on its line.
/// Unlike `compute_cursor_position` which returns byte offset, this
/// accounts for full-width CJK characters that occupy 2 terminal cells.
int TextInputImpl::cursor_display_col() const {
    int line = 0, byte_col = 0;
    compute_cursor_position(line, byte_col);
    // Find the line's start byte
    int line_start = 0;
    for (int i = 0; i < line; ++i) {
        // Advance past this line and its newline
        while (line_start < (int)text_.size() && text_[line_start] != '\n')
            ++line_start;
        ++line_start;  // skip the '\n'
    }
    int line_end = line_start;
    while (line_end < (int)text_.size() && text_[line_end] != '\n')
        ++line_end;
    const std::string line_str = text_.substr(line_start, byte_col);
    return string_width(line_str);
}

// ------------------------------------------------------------
// Rendering (Element output)
// ------------------------------------------------------------
Element TextInputImpl::Render() {
    using namespace ftxui;
    refresh_blink();

    // --- Search mode (reverse history search) ---
    if (search_mode_) {
        return RenderSearchMode();
    }

    Element input_el = RenderInputArea();

    // --- Paste preview confirmation overlay (GAP 1) ---
    // When a large paste (> 10000 chars) is pending confirmation,
    // show the preview box below the input area.
    // TS REF: inputPaste.ts — PastePreview rendered as overlay.
    if (paste_preview_) {
        Element preview = RenderPastePreviewOverlay();
        return vbox({
            input_el,
            preview,
        });
    }

    // --- Suggestions dropdown overlay ---
    if (showing_suggestions_ && !suggestions_.empty()) {
        Element dropdown = RenderSuggestionsDropdown();
        return vbox({
            input_el,
            dropdown,
        });
    }

    return input_el;
}

/// Render the paste preview confirmation overlay.
/// TS REF: inputPaste.ts — shows line count + "(large paste - press Enter
///   to confirm)" + a 200-char snippet of the truncated content.
Element TextInputImpl::RenderPastePreviewOverlay() const {
    if (!paste_preview_) return ftxui::text("");
    return cc::ui::prompt::render_paste_preview(*paste_preview_);
}

/// Render just the input/caret/multiline/selection area (no dropdown).
/// Faithful to TS BaseTextInput's declared-cursor body.
Element TextInputImpl::RenderInputAreaPub() { return RenderInputArea(); }

/// Render a suggestions dropdown from an externally-supplied list.
/// `selected` is clamped to [0, suggestions.size()-1]; -1 disables.
/// Used by repl_screen to surface autocomplete_suggestions inline.
Element TextInputImpl::RenderSuggestionsFromListPub(const std::vector<Suggestion>& sugs,
                                                     int selected) {
    // Temporarily install the supplied list + selection and reuse the
    // private dropdown renderer.  We do not mutate suggestions_ long
    // term because this is a pure render primitive called per-frame.
    const bool was_showing = showing_suggestions_;
    const auto old_sel = selected_suggestion_;
    const auto old_sugs = suggestions_;
    suggestions_ = sugs;
    showing_suggestions_ = !sugs.empty();
    selected_suggestion_ = sugs.empty() ? 0
        : ((selected < 0) ? 0
           : std::min(selected, static_cast<int>(sugs.size()) - 1));
    Element out = (showing_suggestions_ && !suggestions_.empty())
        ? RenderSuggestionsDropdown()
        : ftxui::text("");
    // Restore prior state so this call is side-effect-free for callers
    // that also use the interactive path.
    suggestions_ = old_sugs;
    selected_suggestion_ = old_sel;
    showing_suggestions_ = was_showing;
    return out;
}

Element TextInputImpl::RenderInputArea() {
    using namespace ftxui;
    Elements lines_elements;
    std::vector<std::string_view> lines = split_lines();
    int byte_offset = 0;
    const int total_lines = static_cast<int>(lines.size());
    const int sel_beg = has_selection() ? std::min(sel_start_, sel_end_) : -1;
    const int sel_fin = has_selection() ? std::max(sel_start_, sel_end_) : -1;
    int cursor_line = 0, cursor_col = 0;
    compute_cursor_position(cursor_line, cursor_col);

    // TS REF: PromptInput.tsx L581-584 imageRefPositions — parse [Image #N]
    // refs from the displayed value so we can invert the chip when the
    // cursor parks at its start (the "selected" state; backspace-delete
    // is visually obvious because the whole chip is highlighted).
    const auto all_refs = cc::utils::parse_references(text_);
    std::vector<cc::utils::ReferenceMatch> image_refs;
    image_refs.reserve(all_refs.size());
    for (const auto& r : all_refs) {
        if (r.match.starts_with("[Image")) image_refs.push_back(r);
    }

    for (int li = 0; li < total_lines; ++li) {
        Elements line_parts;

        // Line prefix (first line) or indent (subsequent lines).
        // NOTE (P0-1 glyph unification):
        //   The REPL faithful path passes a PER-MODE prefix_color and sets
        //   prefix_bold=false to match TS PromptInputModeIndicator semantics.
        //   Legacy standalone usages keep the historical default (Green+bold)
        //   via fallback.  The default prefix was also changed from "▶ "
        //   (U+25B6, a CPP-only invention) to "❯ " (figures.pointer U+276F),
        //   matching TS `figures.pointer` exactly.
        if (li == 0) {
            using namespace ftxui;
            Decorator prefix_decor = nothing;
            if (options_.prefix_color.has_value()) {
                prefix_decor = prefix_decor | color(*options_.prefix_color);
            } else {
                // Historical default: bold green.  New callers should set
                // prefix_color explicitly instead of relying on this.
                prefix_decor = prefix_decor | color(Color::Green) | bold;
            }
            if (options_.prefix_bold && !options_.prefix_color.has_value()) {
                prefix_decor = prefix_decor | bold;
            }
            line_parts.push_back(ftxui::text(options_.prefix) | prefix_decor);
        } else {
            // Prefix-width-preserving indent so wrapped lines don't drift
            // out of column with the prefix on line 0.  Use string_width()
            // (not prefix.size()) to handle wide UTF-8 glyphs (❯ = 2 bytes
            // but 1 display cell, vs some emoji 2 cells).
            const int display_w = ftxui::string_width(options_.prefix);
            line_parts.push_back(ftxui::text(std::string(
                static_cast<std::size_t>(display_w > 0 ? display_w : 0), ' ')));
        }

        // Line numbers
        if (options_.show_line_numbers && total_lines > 1) {
            int label_width =
                std::max(2, (int)std::format("{}", total_lines).size());
            line_parts.push_back(
                ftxui::text(std::format("{:>{}} ", li + 1, label_width)) |
                dim | color(Color::GrayDark));
        }

        const std::string_view& line = lines[li];
        const int line_len = static_cast<int>(line.size());
        const int line_start_byte = byte_offset;
        const int line_end_byte = byte_offset + line_len;

        // Apply masking for display
        std::string display_line_str;
        std::string_view display_line;
        if (options_.mask_input) {
            display_line_str = mask_text(std::string(line));
            display_line = display_line_str;
        } else {
            display_line = line;
        }

        // Determine segments for selection highlighting
        struct Segment { int start; int end; bool selected; };
        std::vector<Segment> segs;
        if (!has_selection()) {
            segs.push_back({0, line_len, false});
        } else {
            int abs_sel_beg = std::max(sel_beg, line_start_byte) - line_start_byte;
            int abs_sel_fin = std::min(sel_fin, line_end_byte) - line_start_byte;
            abs_sel_beg = std::clamp(abs_sel_beg, 0, line_len);
            abs_sel_fin = std::clamp(abs_sel_fin, 0, line_len);
            if (abs_sel_beg > 0) segs.push_back({0, abs_sel_beg, false});
            if (abs_sel_fin > abs_sel_beg) segs.push_back({abs_sel_beg, abs_sel_fin, true});
            if (abs_sel_fin < line_len) segs.push_back({abs_sel_fin, line_len, false});
            if (segs.empty()) segs.push_back({0, line_len, false});
        }

        bool is_cursor_line = (li == cursor_line);

        for (const auto& seg : segs) {
            if (seg.start == seg.end) continue;
            std::string seg_text{display_line.substr(seg.start, seg.end - seg.start)};

            if (is_cursor_line) {
                // Split around cursor within segment if applicable
                int cur_rel_start = cursor_col - seg.start;
                if (cur_rel_start >= 0 && cur_rel_start <= (int)seg_text.size() &&
                    !seg.selected) {
                    // Cursor is inside or at edge of this non-selected segment
                    std::string before = seg_text.substr(0, cur_rel_start);
                    std::string after = seg_text.substr(cur_rel_start);
                    if (!before.empty()) line_parts.push_back(ftxui::text(before));

                    // TS REF: PromptInput.tsx L604-616 — invert the entire
                    // [Image #N] chip when the cursor is parked at its
                    // start (chip.start is the "selected" state).  This
                    // makes backspace-to-delete visually obvious because
                    // the whole chip is highlighted, not just the first
                    // character.  The cursor_ absolute byte offset is
                    // compared against ref.index (also absolute bytes).
                    const cc::utils::ReferenceMatch* chip_at_cursor = nullptr;
                    for (const auto& ref : image_refs) {
                        if (static_cast<int>(ref.index) == cursor_) {
                            chip_at_cursor = &ref;
                            break;
                        }
                    }

                    if (chip_at_cursor && after.starts_with(chip_at_cursor->match)) {
                        // Cursor at start of an [Image #N] chip — invert
                        // the ENTIRE chip text (not just the first char).
                        // Always visible (no blink) because the inverted
                        // chip itself serves as the cursor indicator.
                        const std::size_t chip_len = chip_at_cursor->match.size();
                        std::string chip_text = after.substr(0, chip_len);
                        std::string after_chip = after.substr(chip_len);
                        line_parts.push_back(ftxui::text(chip_text) | inverted |
                                             color(Color::White));
                        if (!after_chip.empty())
                            line_parts.push_back(ftxui::text(after_chip));
                    } else if (!after.empty() && blink_visible_) {
                        std::string cursor_ch{after[0]};
                        // Keep rest UTF-8 valid by appending continuations
                        size_t cc = 1;
                        while (cc < after.size() && detail::is_utf8_continuation(
                                   static_cast<unsigned char>(after[cc]))) {
                            cursor_ch.push_back(after[cc]);
                            ++cc;
                        }
                        line_parts.push_back(ftxui::text(cursor_ch) | inverted |
                                             color(Color::White));
                        std::string rest = after.substr(cc);
                        if (!rest.empty()) line_parts.push_back(ftxui::text(rest));
                    } else {
                        // End of line cursor — declared-caret style
                        // (same inverse-glyph pattern used mid-line and in
                        // TS useDeclaredCursor).  A blank space + inverted
                        // paints a solid block in fg/bg swap = consistent
                        // visual with the mid-line caret.
                        line_parts.push_back(ftxui::text(
                            blink_visible_ ? " " : "") |
                            inverted | color(Color::White));
                        if (!after.empty()) line_parts.push_back(ftxui::text(after));
                    }
                } else if (seg.selected) {
                    line_parts.push_back(ftxui::text(seg_text) | bgcolor(Color::Blue) |
                                         color(Color::White));
                } else {
                    line_parts.push_back(ftxui::text(seg_text));
                }
            } else if (seg.selected) {
                line_parts.push_back(ftxui::text(seg_text) | bgcolor(Color::Blue) |
                                     color(Color::White));
            } else {
                line_parts.push_back(ftxui::text(seg_text));
            }
        }

        // Inline ghost text (auto-completion preview) — only on last line
        // when we're at end of line and no selection and no mask
        if (is_cursor_line && !has_selection() && !options_.mask_input &&
            !options_.inline_ghost_text.empty() &&
            cursor_col >= line_len) {
            line_parts.push_back(
                ftxui::text(options_.inline_ghost_text) | dim | color(Color::GrayLight));
        }

        // Argument hint (shown on first line for slash commands)
        if (li == 0 && !options_.argument_hint.empty() && cursor_col > 0) {
            // Only show if the first char indicates a command
            if (!text_.empty() && text_[0] == '/') {
                line_parts.push_back(
                    ftxui::text("  " + options_.argument_hint) | dim | color(Color::Cyan));
            }
        }

        // Empty line placeholder — declared-caret style (same as the
        // mid-line / end-of-line caret: inverted blank space = solid
        // foreground block, matching TS useDeclaredCursor).
        if (line_len == 0 && is_cursor_line && !has_selection()) {
            line_parts.push_back(ftxui::text(
                blink_visible_ ? " " : "") |
                inverted | color(Color::White));
        }

        lines_elements.push_back(hbox(line_parts));
        byte_offset += line_len + 1; // +1 for the '\n'
    }

    // --- Placeholder rendering for empty input ---
    // Delegated to cc::ui::placeholder::RenderPlaceholder which faithfully
    // ports TS renderPlaceholder.ts + BaseTextInput.tsx lines 91-112:
    //   * hide_text + cursor+focus+terminalFocus → invert(' ') only
    //   * cursor+focus+terminalFocus → invert(placeholder[0]) + dim(rest)
    //   * no cursor / no focus     → dim(full placeholder)
    //   * value empty + has text   → showPlaceholder = true
    // Prefix (options_.prefix) is passed through for visual consistency.
    if (lines_elements.size() == 1 && lines[0].empty()) {
        namespace ph = cc::ui::placeholder;

        std::optional<std::string_view> placeholder_sv;
        if (!options_.placeholder.empty()) {
            placeholder_sv = std::string_view(options_.placeholder);
        }

        // blink_visible_ serves as proxy for both "show cursor" and "focused"
        // in the TextInputImpl context — blink is only active when the widget
        // has focus and the cursor timer is in the visible phase.
        auto rendered = ph::RenderPlaceholder(
            placeholder_sv,
            /*value=*/"",           // empty because lines[0].empty()
            /*show_cursor=*/blink_visible_,
            /*focused=*/blink_visible_,
            /*terminal_focus=*/options_.terminal_focus,
            /*hide_text=*/options_.hide_placeholder_text,
            /*prefix=*/options_.prefix,
            /*prefix_color=*/options_.prefix_color);

        if (rendered.element.has_value()) {
            return *std::move(rendered.element);
        }
        // Fallback: if nothing rendered (e.g. hide_text without cursor),
        // show an empty prefix line to maintain layout.
        return ftxui::text(options_.prefix);
    }

    return vbox(lines_elements);
}

Element TextInputImpl::RenderSuggestionsDropdown() const {
    using namespace ftxui;
    const size_t visible = std::min(
        options_.max_visible_suggestions,
        suggestions_.size());

    int start_idx = 0;
    if ((size_t)selected_suggestion_ >= visible) {
        start_idx = selected_suggestion_ - (int)visible + 1;
    }
    int end_idx = std::min(start_idx + (int)visible, (int)suggestions_.size());

    Elements rows;
    rows.reserve(visible + 2);

    // Header
    rows.push_back(hbox({
        ftxui::text(" 💡 "),
        ftxui::text("Suggestions") | bold | color(Color::CyanLight),
        ftxui::text(std::format(" ({} total)", (int)suggestions_.size())) | dim,
    }));
    rows.push_back(separator() | color(Color::Blue));

    for (int i = start_idx; i < end_idx; ++i) {
        const auto& s = suggestions_[i];
        bool selected = (i == selected_suggestion_);

        Elements row_parts;
        row_parts.push_back(ftxui::text(selected ? " > " : "   "));

        // Icon
        if (s.icon.has_value() && !s.icon->empty()) {
            row_parts.push_back(ftxui::text(*s.icon + " "));
        } else {
            const char* cat_icon = suggest_util::category_icon(s.category);
            if (*cat_icon != '\0') {
                row_parts.push_back(ftxui::text(std::string(cat_icon) + " "));
            }
        }

        // Display text (or text if display_text empty)
        std::string label = s.display_text.empty() ? s.text : s.display_text;
        Element label_el = ftxui::text(label);
        Color accent = s.color_hint.value_or(
            suggest_util::category_color(s.category));
        if (selected) {
            label_el = label_el | bgcolor(Color::Blue) | color(Color::White) | bold;
        } else {
            label_el = label_el | color(accent);
        }
        row_parts.push_back(label_el);

        // Tag
        if (s.tag.has_value() && !s.tag->empty()) {
            row_parts.push_back(ftxui::text(" "));
            row_parts.push_back(ftxui::text(*s.tag) | dim | color(Color::Yellow));
        }

        // Description
        if (!s.description.empty()) {
            row_parts.push_back(ftxui::text("  "));
            row_parts.push_back(ftxui::text(s.description) | dim | color(Color::GrayLight));
        }

        rows.push_back(hbox(std::move(row_parts)));
    }

    // Footer hint
    rows.push_back(separator() | dim);
    rows.push_back(
        ftxui::text("  [↑/↓] navigate    [Tab] cycle    [Enter] accept    [Esc] close")
            | dim | color(Color::GrayDark));

    return vbox(std::move(rows))
         | border
         | color(Color::Blue);
}

Element TextInputImpl::RenderSearchMode() const {
    using namespace ftxui;
    Elements rows;

    // Search prompt line
    {
        Elements parts;
        parts.push_back(ftxui::text("reverse-i-search: ") | color(Color::Yellow) | bold);
        parts.push_back(ftxui::text("`" + search_query_ + "`") | color(Color::Cyan));
        parts.push_back(ftxui::text("  "));
        if (search_matches_.empty()) {
            parts.push_back(ftxui::text("(no matches)") | dim | color(Color::Red));
        } else {
            parts.push_back(ftxui::text(
                std::format("{} of {}", (int)search_selected_ + 1, (int)search_matches_.size()))
                | dim);
        }
        rows.push_back(hbox(std::move(parts)));
    }

    // Show current match (if any)
    if (!search_matches_.empty()) {
        const std::string& match = search_matches_[search_selected_];
        rows.push_back(separator() | dim);
        rows.push_back(
            ftxui::text(match) | color(Color::Green)
        );
    }

    // Hint
    rows.push_back(separator() | dim);
    rows.push_back(
        ftxui::text("[Enter] accept  [Esc] cancel  [Ctrl+R] next  [↑/↓] navigate")
            | dim | color(Color::GrayDark));

    return vbox(std::move(rows)) | border | color(Color::Yellow);
}

// ============================================================
// FTXUI Component wrapper
// ============================================================
/// Create an ftxui::Component backed by a TextInputImpl.
/// `out_impl` (optional, non-null) is populated with the internal pointer.
/// Default arguments live on the exported declaration in the primary;
/// this out-of-line definition repeats neither.
Component TextInput(const TextInputOptions& options,
                    std::shared_ptr<TextInputImpl>* out_impl) {
    auto impl = std::make_shared<TextInputImpl>(options);
    if (out_impl) *out_impl = impl;

    // Periodic blink refresh: we use the renderer closure itself to trigger
    // TickBlink() every frame (FTXUI redraws 30-60fps — more than enough).
    return Renderer([impl] {
        impl->TickBlink();
        return impl->Render();
    }) | CatchEvent([impl](Event e) {
        return impl->HandleEvent(e);
    });
}

}  // namespace ui::components
