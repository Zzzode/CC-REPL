/// @file text_input_widget.cppm
/// @brief Full-featured text input with multi-line editing, vim mode,
/// placeholder, history, voice waveform cursor, and priority-based
/// combined highlights.  Migrated from:
///   src/components/TextInput.tsx          – cursor invert, voice waveform
///   src/components/BaseTextInput.tsx      – cursor filtering, viewport adjust
///   src/components/PromptInput/ShimmeredInput.tsx – HighlightedInput segment render
///   src/utils/textHighlighting.ts         – TextHighlight, segmentTextByHighlights
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <algorithm>
#include <cmath>
#include <chrono>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.text_input_widget;

import cc.types.types;
import cc.utils.text_highlighting;

export namespace cc::ui::text_input_widget {
using namespace ftxui;

// Re-export the canonical TextHighlight type from the highlighting module.
// TS REF: src/utils/textHighlighting.ts:11-19
using cc::utils::TextHighlight;
using cc::utils::TextSegment;
using cc::utils::segment_text_by_highlights;
using cc::utils::filter_highlights_at_cursor;
using cc::utils::adjust_highlights_for_viewport;
using namespace cc::utils::highlight_priority;

// ============================================================
// Types
// ============================================================

/// Vim mode state
enum class VimMode : std::uint8_t {
    Disabled,   // Standard editing (no vim)
    Normal,     // Vim normal mode
    Insert,     // Vim insert mode
    Visual,     // Vim visual mode
    Command,    // Vim : command mode
};

/// Voice input state
enum class VoiceState : std::uint8_t {
    Idle,
    Recording,
    Processing,
};

/// Cursor position in a multi-line buffer
struct CursorPos {
    int line = 0;
    int col = 0;
};

/// Props for the text input widget
struct TextInputWidgetOptions {
    std::string placeholder = "Type your message...";
    std::string prefix = "❯ ";
    bool multiline = true;
    bool show_line_numbers = false;
    bool enable_vim = false;
    bool show_history = true;
    bool reduced_motion = false;
    size_t max_history = 1000;
    /// Highlights from multiple sources (image chips, @-mentions, slash
    /// commands, btw triggers, etc.).  Combined via priority system —
    /// TS REF: src/components/PromptInput/PromptInput.tsx:601-741
    std::vector<TextHighlight> highlights;
    /// Flat character offset of the cursor in the full text buffer.
    /// Used for cursor-based highlight filtering.
    /// TS REF: src/components/BaseTextInput.tsx:93
    std::size_t cursor_offset = 0;
    /// Viewport horizontal scroll offset (for long single-line inputs).
    /// TS REF: src/components/BaseTextInput.tsx:98
    std::size_t viewport_char_offset = 0;
    std::size_t viewport_char_end = 0;  ///< End of viewport window (0 = full)

    std::function<void(const std::string&)> on_submit;
    std::function<void(const std::string&)> on_change;
    std::function<void()> on_cancel;
    std::function<void()> on_escape;
};

/// Audio level data for voice waveform cursor
struct AudioWaveformData {
    std::vector<float> levels;      // 0.0 - 1.0
    bool is_recording = false;
    float silence_threshold = 0.15f;
};

// ============================================================
// Buffer Implementation
// ============================================================

/// Multi-line text buffer with cursor management
class TextBuffer {
public:
    TextBuffer() { lines_.push_back(""); }

    void set_text(const std::string& text) {
        lines_.clear();
        size_t pos = 0;
        while (pos < text.size()) {
            auto nl = text.find('\n', pos);
            if (nl == std::string::npos) {
                lines_.push_back(text.substr(pos));
                break;
            }
            lines_.push_back(text.substr(pos, nl - pos));
            pos = nl + 1;
        }
        if (lines_.empty()) lines_.push_back("");
        clamp_cursor();
    }

    [[nodiscard]] std::string get_text() const {
        std::string result;
        for (size_t i = 0; i < lines_.size(); ++i) {
            if (i > 0) result += '\n';
            result += lines_[i];
        }
        return result;
    }

    /// Compute flat character offset of the cursor in the full text.
    /// Used for highlight cursor filtering.
    /// TS REF: src/components/BaseTextInput.tsx:93 (cursorOffset)
    [[nodiscard]] std::size_t cursor_flat_offset() const {
        std::size_t offset = 0;
        for (int i = 0; i < cursor_.line; ++i) {
            offset += lines_[static_cast<size_t>(i)].size() + 1;  // +1 for '\n'
        }
        offset += static_cast<std::size_t>(cursor_.col);
        return offset;
    }

    [[nodiscard]] bool empty() const {
        return lines_.size() == 1 && lines_[0].empty();
    }

    [[nodiscard]] CursorPos cursor() const { return cursor_; }
    [[nodiscard]] int line_count() const { return static_cast<int>(lines_.size()); }
    [[nodiscard]] const std::string& line(int idx) const { return lines_[static_cast<size_t>(idx)]; }
    [[nodiscard]] const std::vector<std::string>& lines() const { return lines_; }

    void insert_char(char c) {
        lines_[cursor_.line].insert(cursor_.col, 1, c);
        cursor_.col++;
    }

    void insert_newline() {
        std::string rest = lines_[cursor_.line].substr(cursor_.col);
        lines_[cursor_.line] = lines_[cursor_.line].substr(0, cursor_.col);
        lines_.insert(lines_.begin() + cursor_.line + 1, rest);
        cursor_.line++;
        cursor_.col = 0;
    }

    void backspace() {
        if (cursor_.col > 0) {
            lines_[cursor_.line].erase(cursor_.col - 1, 1);
            cursor_.col--;
        } else if (cursor_.line > 0) {
            cursor_.col = static_cast<int>(lines_[cursor_.line - 1].size());
            lines_[cursor_.line - 1] += lines_[cursor_.line];
            lines_.erase(lines_.begin() + cursor_.line);
            cursor_.line--;
        }
    }

    void delete_char() {
        if (cursor_.col < static_cast<int>(lines_[cursor_.line].size())) {
            lines_[cursor_.line].erase(cursor_.col, 1);
        } else if (cursor_.line < static_cast<int>(lines_.size()) - 1) {
            lines_[cursor_.line] += lines_[cursor_.line + 1];
            lines_.erase(lines_.begin() + cursor_.line + 1);
        }
    }

    void move_left() {
        if (cursor_.col > 0) {
            cursor_.col--;
        } else if (cursor_.line > 0) {
            cursor_.line--;
            cursor_.col = static_cast<int>(lines_[cursor_.line].size());
        }
    }

    void move_right() {
        if (cursor_.col < static_cast<int>(lines_[cursor_.line].size())) {
            cursor_.col++;
        } else if (cursor_.line < static_cast<int>(lines_.size()) - 1) {
            cursor_.line++;
            cursor_.col = 0;
        }
    }

    void move_up() {
        if (cursor_.line > 0) {
            cursor_.line--;
            clamp_col();
        }
    }

    void move_down() {
        if (cursor_.line < static_cast<int>(lines_.size()) - 1) {
            cursor_.line++;
            clamp_col();
        }
    }

    void move_home() { cursor_.col = 0; }
    void move_end() { cursor_.col = static_cast<int>(lines_[cursor_.line].size()); }
    void move_top() { cursor_.line = 0; cursor_.col = 0; }
    void move_bottom() {
        cursor_.line = static_cast<int>(lines_.size()) - 1;
        cursor_.col = static_cast<int>(lines_[cursor_.line].size());
    }

    void clear() {
        lines_.clear();
        lines_.push_back("");
        cursor_ = {0, 0};
    }

private:
    void clamp_cursor() {
        cursor_.line = std::clamp(cursor_.line, 0,
                                  static_cast<int>(lines_.size()) - 1);
        clamp_col();
    }

    void clamp_col() {
        cursor_.col = std::clamp(cursor_.col, 0,
                                 static_cast<int>(lines_[cursor_.line].size()));
    }

    std::vector<std::string> lines_;
    CursorPos cursor_;
};

// ============================================================
// Waveform Rendering
// ============================================================

/// Render voice waveform cursor
[[nodiscard]] inline Element RenderWaveformCursor(
    const AudioWaveformData& audio,
    [[maybe_unused]] std::chrono::steady_clock::time_point start_time) {

    if (!audio.is_recording || audio.levels.empty()) {
        return text(" ") | inverted | color(Color::White);
    }

    // Block characters for waveform bars
    static constexpr std::array<const char*, 9> bars =
        {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

    float level = audio.levels.back();
    bool is_silent = level < audio.silence_threshold;

    int bar_idx = std::clamp(
        static_cast<int>(std::round(level * 8.0f)), 1, 8);

    auto clr = is_silent ? Color::GrayDark : Color::CyanLight;
    return text(bars[bar_idx]) | color(clr) | bold;
}

// ============================================================
// Highlighted Line Rendering
// ============================================================

/// A single part of a line after splitting segments by newlines.
/// Mirrors TS LinePart in ShimmeredInput.tsx:10-14.
struct LinePart {
    std::string text;
    std::optional<TextHighlight> highlight;
    std::size_t start = 0;  ///< Flat offset within the full text
};

/// Split segments into per-line parts by breaking on '\n'.
/// Mirrors TS HighlightedInput segment→lines split logic.
/// TS REF: src/components/PromptInput/ShimmeredInput.tsx:23-43
[[nodiscard]] inline std::vector<std::vector<LinePart>> split_segments_into_lines(
    const std::vector<TextSegment>& segments) {

    std::vector<std::vector<LinePart>> lines = {{}};
    std::size_t pos = 0;

    for (const auto& seg : segments) {
        // Split the segment text by newlines
        std::size_t seg_start = 0;
        while (seg_start <= seg.text.size()) {
            auto nl = seg.text.find('\n', seg_start);
            std::string part_text;
            if (nl == std::string::npos) {
                part_text = seg.text.substr(seg_start);
            } else {
                part_text = seg.text.substr(seg_start, nl - seg_start);
            }

            if (!part_text.empty()) {
                lines.back().push_back(LinePart{
                    part_text,
                    seg.highlight,
                    pos + seg_start
                });
            }

            if (nl == std::string::npos) {
                pos += seg.text.size() - seg_start;
                break;
            }

            // Move past the newline
            pos += nl - seg_start + 1;
            seg_start = nl + 1;
            lines.push_back({});  // start new line
        }
    }

    // Ensure at least one empty part per line (so empty lines render a space)
    for (auto& line : lines) {
        if (line.empty()) {
            line.push_back(LinePart{ " ", std::nullopt, 0 });
        }
    }

    return lines;
}

/// Apply FTXUI decorators to a line part element based on its highlight.
/// TS REF: ShimmeredInput.tsx:111-116 (per-part rendering)
[[nodiscard]] inline Element decorate_line_part(
    const LinePart& part,
    [[maybe_unused]] bool has_shimmer_active = false) {

    Element el = text(part.text);

    if (part.highlight) {
        const auto& hl = *part.highlight;

        // Apply foreground color if specified
        // TS REF: ShimmeredInput.tsx:115 color={part.highlight?.color}
        if (hl.color) {
            el = el | color(*hl.color);
        }

        // Apply dim if set
        // TS REF: ShimmeredInput.tsx:115 dimColor={part.highlight?.dimColor}
        if (hl.dim) {
            el = el | dim;
        }

        // Apply inverse if set (e.g. cursor on [Image #N] chip)
        // TS REF: ShimmeredInput.tsx:115 inverse={part.highlight?.inverse}
        if (hl.inverse) {
            el = el | inverted;
        }

        // Shimmer: if the highlight has shimmer_color, we render with the
        // base color.  Full per-character shimmer animation requires a
        // 50ms ticker (TS useAnimationFrame); event-driven repaint rule
        // means we show the static base color here.  The shimmer sweep is
        // purely cosmetic and the static color is still readable.
        // TS REF: ShimmeredInput.tsx:112-114 (ShimmerChar per-char loop)
        if (hl.shimmer_color && !hl.color) {
            el = el | color(*hl.shimmer_color);
        }
    }

    return el;
}

// ============================================================
// Element Rendering
// ============================================================

/// Render the text input widget with priority-based highlight support.
///
/// When highlights are present, uses segment_text_by_highlights() to
/// build non-overlapping styled segments (matching TS HighlightedInput).
/// When no highlights are present, falls back to plain text rendering.
///
/// TS REF:
///   - Cursor filtering:  src/components/BaseTextInput.tsx:93
///   - Viewport adjust:   src/components/BaseTextInput.tsx:98-102
///   - Segment rendering: src/components/PromptInput/ShimmeredInput.tsx
///   - Combined builder:  src/components/PromptInput/PromptInput.tsx:601-741
[[nodiscard]] inline Element RenderTextInputWidget(
    const TextInputWidgetOptions& opts,
    const TextBuffer& buffer,
    VimMode vim_mode,
    bool focused) {

    Elements all_lines;

    if (buffer.empty() && !focused) {
        auto line = hbox({
            text(opts.prefix) | color(Color::Green) | bold,
            text(opts.placeholder) | dim,
        });
        return line;
    }

    auto cursor_pos = buffer.cursor();
    std::string full_text = buffer.get_text();

    // ── Build filtered & adjusted highlights ──────────────────────────────
    // TS REF: BaseTextInput.tsx:93 (cursorFiltered)
    auto cursor_off = buffer.cursor_flat_offset();
    auto cursor_filtered = filter_highlights_at_cursor(
        opts.highlights, cursor_off, focused);

    // TS REF: BaseTextInput.tsx:98-102 (viewport adjustment)
    std::size_t vp_end = opts.viewport_char_end > 0
        ? opts.viewport_char_end
        : full_text.size();
    auto adjusted = adjust_highlights_for_viewport(
        cursor_filtered, opts.viewport_char_offset, vp_end);

    // Determine viewport text
    std::string_view viewport_text = full_text;
    std::size_t vp_off = opts.viewport_char_offset;
    if (vp_off > 0 && vp_off < full_text.size()) {
        viewport_text = std::string_view(full_text).substr(vp_off);
    }

    // ── Build segments ────────────────────────────────────────────────────
    // TS REF: ShimmeredInput.tsx:23 (segmentTextByHighlights)
    auto segments = segment_text_by_highlights(viewport_text, adjusted);
    auto line_parts = split_segments_into_lines(segments);

    // ── Render each line ──────────────────────────────────────────────────
    for (int i = 0; i < buffer.line_count(); ++i) {
        Elements parts;

        // Prefix on first line, padding on others
        if (i == 0) {
            parts.push_back(text(opts.prefix) | color(Color::Green) | bold);
        } else if (opts.multiline) {
            parts.push_back(text(std::string(opts.prefix.size(), ' ')));
        }

        // Line numbers
        if (opts.show_line_numbers) {
            parts.push_back(
                text(std::format("{:3} ", i + 1)) | dim | color(Color::GrayDark));
        }

        // Render line content
        if (i < static_cast<int>(line_parts.size())) {
            const auto& line = line_parts[static_cast<std::size_t>(i)];

            if (i == cursor_pos.line && focused) {
                // Find the cursor position within this line's parts and
                // invert the character under the cursor.
                // TS REF: BaseTextInput.tsx uses useDeclaredCursor for
                // terminal cursor positioning; we invert the char here
                // to match the visual cursor behavior.
                int col_in_line = cursor_pos.col;
                bool cursor_rendered = false;

                for (std::size_t p = 0; p < line.size(); ++p) {
                    const auto& part = line[p];
                    int part_len = static_cast<int>(part.text.size());

                    if (cursor_rendered) {
                        parts.push_back(decorate_line_part(part));
                        continue;
                    }

                    if (col_in_line >= part_len) {
                        // Cursor is past this part
                        parts.push_back(decorate_line_part(part));
                        col_in_line -= part_len;
                        continue;
                    }

                    // Cursor is inside this part — split around it
                    std::string before = part.text.substr(0, col_in_line);
                    if (!before.empty()) {
                        LinePart before_part{ before, part.highlight, part.start };
                        parts.push_back(decorate_line_part(before_part));
                    }

                    // Cursor character — inverted
                    std::string cursor_ch(1, part.text[col_in_line]);
                    // Cursor takes priority over highlight styling
                    parts.push_back(text(cursor_ch) | inverted);

                    // Rest after cursor
                    std::string after = part.text.substr(col_in_line + 1);
                    if (!after.empty()) {
                        LinePart after_part{
                            after, part.highlight,
                            part.start + static_cast<std::size_t>(col_in_line) + 1
                        };
                        parts.push_back(decorate_line_part(after_part));
                    }

                    cursor_rendered = true;
                }

                // Cursor at end of line (past all parts)
                if (!cursor_rendered) {
                    parts.push_back(text(" ") | inverted);
                }
            } else {
                // Non-cursor line: just render all parts with their decorators
                for (const auto& part : line) {
                    parts.push_back(decorate_line_part(part));
                }
            }
        } else {
            // Line beyond our segment split (shouldn't happen normally)
            const auto& line_text = buffer.line(i);
            if (line_text.empty()) {
                parts.push_back(text(" "));
            } else {
                parts.push_back(text(line_text));
            }
        }

        all_lines.push_back(hbox(parts));
    }

    // Vim mode indicator
    if (opts.enable_vim && vim_mode != VimMode::Disabled) {
        std::string mode_str;
        Color mode_color;
        switch (vim_mode) {
            case VimMode::Normal:
                mode_str = "NORMAL"; mode_color = Color::Blue; break;
            case VimMode::Insert:
                mode_str = "INSERT"; mode_color = Color::Green; break;
            case VimMode::Visual:
                mode_str = "VISUAL"; mode_color = Color::Magenta; break;
            case VimMode::Command:
                mode_str = "COMMAND"; mode_color = Color::Yellow; break;
            default:
                mode_str = ""; mode_color = Color::White; break;
        }
        if (!mode_str.empty()) {
            all_lines.push_back(
                text(" -- " + mode_str + " --") | color(mode_color) | dim);
        }
    }

    return vbox(all_lines);
}

// ============================================================
// Interactive Component
// ============================================================

/// Create a full-featured text input widget component
[[nodiscard]] inline Component TextInputWidget(TextInputWidgetOptions options) {
    struct State {
        TextInputWidgetOptions opts;
        TextBuffer buffer;
        VimMode vim_mode;
        bool focused = true;
        std::deque<std::string> history;
        size_t history_index = std::string::npos;
        std::chrono::steady_clock::time_point start_time;
    };
    auto state = std::make_shared<State>();
    state->opts = std::move(options);
    state->vim_mode = state->opts.enable_vim ? VimMode::Normal : VimMode::Disabled;
    state->start_time = std::chrono::steady_clock::now();

    return Renderer([state] {
        // Update cursor_offset in opts for cursor-based highlight filtering.
        // TS REF: BaseTextInput.tsx:93 (cursorOffset prop passed to TextInput)
        state->opts.cursor_offset = state->buffer.cursor_flat_offset();
        return RenderTextInputWidget(
            state->opts, state->buffer, state->vim_mode, state->focused);
    }) | CatchEvent([state](Event event) -> bool {
        auto& buf = state->buffer;
        auto& opts = state->opts;

        // Vim normal mode handling
        if (state->vim_mode == VimMode::Normal) {
            if (event == Event::Character('i')) {
                state->vim_mode = VimMode::Insert;
                return true;
            }
            if (event == Event::Character('a')) {
                state->vim_mode = VimMode::Insert;
                buf.move_right();
                return true;
            }
            if (event == Event::Character('A')) {
                state->vim_mode = VimMode::Insert;
                buf.move_end();
                return true;
            }
            if (event == Event::Character('I')) {
                state->vim_mode = VimMode::Insert;
                buf.move_home();
                return true;
            }
            if (event == Event::Character('o')) {
                state->vim_mode = VimMode::Insert;
                buf.move_end();
                buf.insert_newline();
                return true;
            }
            if (event == Event::Character('h') || event == Event::ArrowLeft) {
                buf.move_left(); return true;
            }
            if (event == Event::Character('l') || event == Event::ArrowRight) {
                buf.move_right(); return true;
            }
            if (event == Event::Character('j') || event == Event::ArrowDown) {
                buf.move_down(); return true;
            }
            if (event == Event::Character('k') || event == Event::ArrowUp) {
                buf.move_up(); return true;
            }
            if (event == Event::Character('0') || event == Event::Home) {
                buf.move_home(); return true;
            }
            if (event == Event::Character('$') || event == Event::End) {
                buf.move_end(); return true;
            }
            if (event == Event::Character('x')) {
                buf.delete_char();
                if (opts.on_change) opts.on_change(buf.get_text());
                return true;
            }
            if (event == Event::Character('d')) {
                buf.clear();
                if (opts.on_change) opts.on_change(buf.get_text());
                return true;
            }
            return false;
        }

        // Escape returns to normal mode or cancels
        if (event == Event::Escape) {
            if (state->vim_mode == VimMode::Insert) {
                state->vim_mode = VimMode::Normal;
                return true;
            }
            if (opts.on_escape) opts.on_escape();
            return true;
        }

        // Insert mode / non-vim handling
        if (event.is_character()) {
            char c = event.character()[0];
            if (c >= 32 && c < 127) {
                buf.insert_char(c);
                if (opts.on_change) opts.on_change(buf.get_text());
                return true;
            }
        }

        if (event == Event::Return) {
            if (opts.multiline && event == Event::Return) {
                if (buf.line_count() == 1 || !opts.multiline) {
                    if (opts.on_submit) {
                        std::string text = buf.get_text();
                        if (!text.empty()) {
                            state->history.push_back(text);
                            if (state->history.size() > opts.max_history) {
                                state->history.pop_front();
                            }
                            opts.on_submit(text);
                            buf.clear();
                            state->history_index = std::string::npos;
                        }
                    }
                    return true;
                }
            }
            buf.insert_newline();
            if (opts.on_change) opts.on_change(buf.get_text());
            return true;
        }

        if (event == Event::Backspace) {
            buf.backspace();
            if (opts.on_change) opts.on_change(buf.get_text());
            return true;
        }
        if (event == Event::Delete) {
            buf.delete_char();
            if (opts.on_change) opts.on_change(buf.get_text());
            return true;
        }

        if (event == Event::ArrowLeft)  { buf.move_left();  return true; }
        if (event == Event::ArrowRight) { buf.move_right(); return true; }
        if (event == Event::ArrowUp) {
            if (buf.cursor().line == 0 && opts.show_history &&
                !state->history.empty()) {
                if (state->history_index == std::string::npos) {
                    state->history_index = state->history.size() - 1;
                } else if (state->history_index > 0) {
                    state->history_index--;
                }
                buf.set_text(state->history[state->history_index]);
                if (opts.on_change) opts.on_change(buf.get_text());
            } else {
                buf.move_up();
            }
            return true;
        }
        if (event == Event::ArrowDown) {
            if (buf.cursor().line == buf.line_count() - 1 &&
                state->history_index != std::string::npos) {
                if (state->history_index < state->history.size() - 1) {
                    state->history_index++;
                    buf.set_text(state->history[state->history_index]);
                } else {
                    state->history_index = std::string::npos;
                    buf.clear();
                }
                if (opts.on_change) opts.on_change(buf.get_text());
            } else {
                buf.move_down();
            }
            return true;
        }

        if (event == Event::Home) { buf.move_home(); return true; }
        if (event == Event::End)  { buf.move_end();  return true; }

        return false;
    });
}

/// Convenience: simple single-line text input
[[nodiscard]] inline Component SimpleTextInput(
    std::string placeholder,
    std::function<void(const std::string&)> on_submit) {

    TextInputWidgetOptions opts;
    opts.placeholder = std::move(placeholder);
    opts.multiline = false;
    opts.on_submit = std::move(on_submit);
    return TextInputWidget(std::move(opts));
}

} // namespace cc::ui::text_input_widget