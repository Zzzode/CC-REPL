/// @file vim_input.cppm
/// @brief Vim mode input support - provides modal editing with Normal, Insert,
/// Visual, and Command-line modes for the prompt input.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.prompt.vim_input;

import cc.types.types;
import cc.ui.common.types;  // TS REF: canonical VimMode lives here

export namespace cc::ui::prompt::vim_input {
using namespace ftxui;

// ============================================================
// ARCHITECTURE NOTE (2026-07-10, P2 "vim-dual-conflicting-impls")
// ============================================================
// This module provides a standalone VimInput component with its own VimState.
// It is primarily used for:
//   1. `mode_display()` utility — returns {label, color} pairs consumed by
//      repl_screen.cppm and prompt_input_footer.cppm for the "-- NORMAL --"
//      / "-- INSERT --" indicator.
//   2. Legacy standalone usage in screens that do not use TextInputImpl.
//
// The CANONICAL vim editing implementation lives in:
//   cc::ui::components::TextInputImpl  (text_input.cppm)
// which uses `std::optional<VimMode> vim_mode` in TextInputOptions and has
// the full HandleVimEvent() state machine matching TS useVimInput.ts.
//
// Do NOT add new text-editing logic here.  Extend TextInputImpl instead.
// ============================================================

// ============================================================
// Types
// ============================================================

// Canonical VimMode — imported from cc::ui::common (ui_types.cppm).
// TS REF: src/types/textInputTypes.ts:222 (public type = 'INSERT'|'NORMAL')
//          src/hooks/useVimInput.ts:36 (internal state machine tracks more)
// This replaces the previous local 6-value enum that was missing VisualBlock.
using cc::ui::common::VimMode;

/// Vim register for yank/paste
struct VimRegister {
    std::string content;
    bool is_linewise = false;
};

/// Cursor shape hint for terminal
enum class CursorShape : std::uint8_t {
    Block,      // Normal mode
    Bar,        // Insert mode
    Underline,  // Replace mode
};

/// State of the vim input component
struct VimState {
    VimMode mode = VimMode::Normal;
    std::string text;
    int cursor_pos = 0;
    int visual_start = -1;      // Start of visual selection
    std::string pending_keys;   // Partial command buffer (e.g., "d" waiting for motion)
    int repeat_count = 0;       // Numeric prefix (e.g., 3dw)
    std::string command_line;   // Content of : command line
    VimRegister unnamed_reg;    // Default register (")
    std::string last_search;    // Last / search pattern
    std::string status_message; // Shown in status area
    std::vector<std::string> undo_stack;
    int undo_index = -1;
};

/// Options for the vim input component
struct VimInputOptions {
    std::string initial_text;
    bool start_in_insert = false;
    std::function<void(const std::string&)> on_submit;
    std::function<void(const std::string&)> on_change;
    std::function<void(const std::string&)> on_command;  // For :commands
    std::function<void(VimMode)> on_mode_change;
    int max_undo = 100;
};

// ============================================================
// Vim Motion Helpers
// ============================================================

/// Find the start of the current word
[[nodiscard]] inline int word_start(const std::string& text, int pos) {
    if (pos <= 0) return 0;
    int p = pos - 1;
    // Skip whitespace
    while (p > 0 && std::isspace(static_cast<unsigned char>(text[p]))) --p;
    // Skip word chars
    while (p > 0 && !std::isspace(static_cast<unsigned char>(text[p - 1]))) --p;
    return p;
}

/// Find the end of the current word
[[nodiscard]] inline int word_end(const std::string& text, int pos) {
    int len = static_cast<int>(text.size());
    if (pos >= len) return len;
    int p = pos;
    // Skip current word
    while (p < len && !std::isspace(static_cast<unsigned char>(text[p]))) ++p;
    // Skip whitespace
    while (p < len && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
    return p;
}

/// Find next word boundary (w motion)
[[nodiscard]] inline int next_word(const std::string& text, int pos) {
    int len = static_cast<int>(text.size());
    if (pos >= len) return len;
    int p = pos;
    // Skip current word chars
    if (!std::isspace(static_cast<unsigned char>(text[p]))) {
        while (p < len && !std::isspace(static_cast<unsigned char>(text[p]))) ++p;
    }
    // Skip whitespace
    while (p < len && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
    return p;
}

/// Find previous word boundary (b motion)
[[nodiscard]] inline int prev_word(const std::string& text, int pos) {
    if (pos <= 0) return 0;
    int p = pos - 1;
    // Skip whitespace
    while (p > 0 && std::isspace(static_cast<unsigned char>(text[p]))) --p;
    // Skip word chars backward
    while (p > 0 && !std::isspace(static_cast<unsigned char>(text[p - 1]))) --p;
    return p;
}

// ============================================================
// Rendering
// ============================================================

/// Get the cursor shape for the current mode
/// TS REF: useVimInput.ts — cursor style changes per mode (block for normal,
/// bar for insert, underline for replace).
[[nodiscard]] inline CursorShape cursor_for_mode(VimMode mode) {
    switch (mode) {
        case VimMode::Normal:      return CursorShape::Block;
        case VimMode::Insert:      return CursorShape::Bar;
        case VimMode::Visual:      return CursorShape::Block;
        case VimMode::VisualLine:  return CursorShape::Block;
        case VimMode::VisualBlock: return CursorShape::Block;
        case VimMode::Replace:     return CursorShape::Underline;
        case VimMode::Command:     return CursorShape::Bar;
    }
    return CursorShape::Block;
}

/// Get mode display label and color.
/// TS REF: vim mode indicator color coding — green=normal, blue=insert,
/// magenta=visual, red=replace, yellow=command.
[[nodiscard]] inline std::pair<std::string, Color> mode_display(VimMode mode) {
    switch (mode) {
        case VimMode::Normal:      return {"NORMAL",      Color::Green};
        case VimMode::Insert:      return {"INSERT",      Color::Blue};
        case VimMode::Visual:      return {"VISUAL",      Color::Magenta};
        case VimMode::VisualLine:  return {"V-LINE",      Color::Magenta};
        case VimMode::VisualBlock: return {"V-BLOCK",     Color::Magenta};
        case VimMode::Replace:     return {"REPLACE",     Color::Red};
        case VimMode::Command:     return {"COMMAND",     Color::Yellow};
    }
    return {"???", Color::White};
}

/// Render the vim input line with cursor and mode indicator
[[nodiscard]] inline Element RenderVimInput(const VimState& state) {
    auto [mode_label, mode_color] = mode_display(state.mode);

    // Mode indicator badge
    auto mode_badge = text(" " + mode_label + " ") | bold
                      | color(Color::White) | bgcolor(mode_color);

    // Text with cursor
    Elements text_parts;
    int len = static_cast<int>(state.text.size());

    // Visual selection range
    int sel_start = -1, sel_end = -1;
    bool is_visual = (state.mode == VimMode::Visual ||
                      state.mode == VimMode::VisualLine ||
                      state.mode == VimMode::VisualBlock);
    if (is_visual && state.visual_start >= 0) {
        sel_start = std::min(state.visual_start, state.cursor_pos);
        sel_end = std::max(state.visual_start, state.cursor_pos);
    }

    for (int i = 0; i <= len; ++i) {
        if (i == state.cursor_pos) {
            // Cursor position
            if (i < len) {
                std::string ch(1, state.text[i]);
                if (state.mode == VimMode::Normal || is_visual) {
                    text_parts.push_back(text(ch) | inverted | bold);
                } else {
                    text_parts.push_back(text("|") | color(Color::Cyan) | blink);
                    text_parts.push_back(text(ch));
                }
            } else {
                // Cursor at end
                if (state.mode == VimMode::Normal || is_visual) {
                    text_parts.push_back(text(" ") | inverted);
                } else {
                    text_parts.push_back(text("|") | color(Color::Cyan) | blink);
                }
            }
        } else if (i < len) {
            std::string ch(1, state.text[i]);
            if (sel_start >= 0 && i >= sel_start && i <= sel_end) {
                text_parts.push_back(text(ch) | bgcolor(Color::RGB(60, 60, 120)));
            } else {
                text_parts.push_back(text(ch));
            }
        }
    }

    auto input_line = hbox(text_parts);
    if (state.text.empty() && state.mode == VimMode::Insert) {
        input_line = hbox({
            text("|") | color(Color::Cyan) | blink,
            text("Type your message...") | dim,
        });
    }

    // Status bar at bottom
    Elements status_parts = {mode_badge, text(" ")};
    if (!state.pending_keys.empty()) {
        status_parts.push_back(text(state.pending_keys) | color(Color::Yellow));
    }
    if (state.repeat_count > 0) {
        status_parts.push_back(text(std::to_string(state.repeat_count)) | color(Color::Cyan));
    }
    status_parts.push_back(filler());
    if (!state.status_message.empty()) {
        status_parts.push_back(text(state.status_message) | dim);
    }
    status_parts.push_back(
        text(std::format(" {}:{} ", state.cursor_pos + 1, len)) | dim);

    auto status_bar = hbox(status_parts);

    // Command line (when in : mode)
    if (state.mode == VimMode::Command) {
        return vbox({
            input_line | flex,
            separator() | dim,
            hbox({
                text(":") | color(Color::Yellow) | bold,
                text(state.command_line),
                text("|") | color(Color::Cyan) | blink,
            }),
        });
    }

    return vbox({
        input_line | flex,
        status_bar,
    });
}

// ============================================================
// Interactive Component
// ============================================================

/// Create a vim-mode input component
[[nodiscard]] inline Component VimInput(VimInputOptions options) {
    auto state = std::make_shared<VimState>();
    auto opts = std::make_shared<VimInputOptions>(std::move(options));

    state->text = opts->initial_text;
    state->cursor_pos = static_cast<int>(state->text.size());
    if (opts->start_in_insert) {
        state->mode = VimMode::Insert;
    }

    // Save initial state for undo
    state->undo_stack.push_back(state->text);
    state->undo_index = 0;

    return Renderer([state] {
        return RenderVimInput(*state);
    }) | CatchEvent([state, opts](Event event) -> bool {
        auto save_undo = [&] {
            if (state->undo_index < static_cast<int>(state->undo_stack.size()) - 1) {
                state->undo_stack.resize(state->undo_index + 1);
            }
            state->undo_stack.push_back(state->text);
            if (static_cast<int>(state->undo_stack.size()) > opts->max_undo) {
                state->undo_stack.erase(state->undo_stack.begin());
            }
            state->undo_index = static_cast<int>(state->undo_stack.size()) - 1;
        };

        auto notify_change = [&] {
            if (opts->on_change) opts->on_change(state->text);
        };

        auto set_mode = [&](VimMode m) {
            state->mode = m;
            if (opts->on_mode_change) opts->on_mode_change(m);
        };

        int len = static_cast<int>(state->text.size());

        // --- COMMAND MODE ---
        if (state->mode == VimMode::Command) {
            if (event == Event::Escape) {
                state->command_line.clear();
                set_mode(VimMode::Normal);
                return true;
            }
            if (event == Event::Return) {
                if (opts->on_command) opts->on_command(state->command_line);
                state->command_line.clear();
                set_mode(VimMode::Normal);
                return true;
            }
            if (event == Event::Backspace) {
                if (!state->command_line.empty()) {
                    state->command_line.pop_back();
                } else {
                    set_mode(VimMode::Normal);
                }
                return true;
            }
            if (event.is_character()) {
                state->command_line += event.character();
                return true;
            }
            return false;
        }

        // --- INSERT MODE ---
        if (state->mode == VimMode::Insert) {
            if (event == Event::Escape) {
                set_mode(VimMode::Normal);
                if (state->cursor_pos > 0) state->cursor_pos--;
                save_undo();
                return true;
            }
            if (event == Event::Return) {
                if (opts->on_submit) opts->on_submit(state->text);
                return true;
            }
            if (event == Event::Backspace) {
                if (state->cursor_pos > 0) {
                    state->text.erase(state->cursor_pos - 1, 1);
                    state->cursor_pos--;
                    notify_change();
                }
                return true;
            }
            if (event.is_character()) {
                state->text.insert(state->cursor_pos, event.character());
                state->cursor_pos += static_cast<int>(event.character().size());
                notify_change();
                return true;
            }
            return false;
        }

        // --- REPLACE MODE ---
        // TS REF: useVimInput.ts — Replace mode (R) replaces characters at cursor
        // until Escape exits back to Normal.
        if (state->mode == VimMode::Replace) {
            if (event == Event::Escape) {
                set_mode(VimMode::Normal);
                if (state->cursor_pos > 0) state->cursor_pos--;
                return true;
            }
            if (event == Event::Return) {
                if (opts->on_submit) opts->on_submit(state->text);
                return true;
            }
            if (event.is_character()) {
                // Replace character at cursor position
                if (state->cursor_pos < len) {
                    state->unnamed_reg.content = state->text.substr(state->cursor_pos, 1);
                    state->text[state->cursor_pos] = event.character()[0];
                    state->cursor_pos++;
                    save_undo();
                    notify_change();
                } else {
                    // At end: behave like insert
                    state->text.insert(state->cursor_pos, event.character());
                    state->cursor_pos += static_cast<int>(event.character().size());
                    notify_change();
                }
                return true;
            }
            return false;
        }

        // --- NORMAL / VISUAL MODE ---
        // TS REF: useVimInput.ts:231 — NORMAL mode handles all vim commands.
        // Visual modes share motion keys with Normal; Escape exits back to Normal.
        bool is_any_visual = (state->mode == VimMode::Visual ||
                              state->mode == VimMode::VisualLine ||
                              state->mode == VimMode::VisualBlock);
        if (state->mode == VimMode::Normal || is_any_visual) {
            // Mode transitions from Normal
            if (state->mode == VimMode::Normal) {
                if (event == Event::Character('i')) {
                    set_mode(VimMode::Insert);
                    return true;
                }
                if (event == Event::Character('a')) {
                    set_mode(VimMode::Insert);
                    if (state->cursor_pos < len) state->cursor_pos++;
                    return true;
                }
                if (event == Event::Character('I')) {
                    set_mode(VimMode::Insert);
                    state->cursor_pos = 0;
                    return true;
                }
                if (event == Event::Character('A')) {
                    set_mode(VimMode::Insert);
                    state->cursor_pos = len;
                    return true;
                }
                if (event == Event::Character('R')) {
                    set_mode(VimMode::Replace);
                    return true;
                }
            }

            // Visual mode entry (works from Normal mode)
            if (event == Event::Character('v') && state->mode == VimMode::Normal) {
                set_mode(VimMode::Visual);
                state->visual_start = state->cursor_pos;
                return true;
            }
            // Toggle off visual when pressing 'v' again in Visual mode
            if (event == Event::Character('v') && state->mode == VimMode::Visual) {
                set_mode(VimMode::Normal);
                state->visual_start = -1;
                return true;
            }
            // V = VisualLine (line-wise selection)
            if (event == Event::Character('V')) {
                if (state->mode == VimMode::VisualLine) {
                    set_mode(VimMode::Normal);
                    state->visual_start = -1;
                } else {
                    set_mode(VimMode::VisualLine);
                    state->visual_start = state->cursor_pos;
                }
                return true;
            }
            // Ctrl+V = VisualBlock (block-wise selection)
            if (event == Event::Character('\x16')) {
                if (state->mode == VimMode::VisualBlock) {
                    set_mode(VimMode::Normal);
                    state->visual_start = -1;
                } else {
                    set_mode(VimMode::VisualBlock);
                    state->visual_start = state->cursor_pos;
                }
                return true;
            }

            if (event == Event::Character(':')) {
                set_mode(VimMode::Command);
                state->command_line.clear();
                return true;
            }

            // Motions
            if (event == Event::Character('h') || event == Event::ArrowLeft) {
                if (state->cursor_pos > 0) state->cursor_pos--;
                return true;
            }
            if (event == Event::Character('l') || event == Event::ArrowRight) {
                if (state->cursor_pos < len - 1) state->cursor_pos++;
                return true;
            }
            if (event == Event::Character('0') && state->repeat_count == 0) {
                state->cursor_pos = 0;
                return true;
            }
            if (event == Event::Character('$')) {
                state->cursor_pos = std::max(0, len - 1);
                return true;
            }
            if (event == Event::Character('w')) {
                state->cursor_pos = next_word(state->text, state->cursor_pos);
                if (state->cursor_pos >= len) state->cursor_pos = std::max(0, len - 1);
                return true;
            }
            if (event == Event::Character('b')) {
                state->cursor_pos = prev_word(state->text, state->cursor_pos);
                return true;
            }

            // Operators: dd (delete line), yy (yank line)
            // TS REF: useVimInput.ts:257 — operator pending state tracks
            // the first 'd' or 'y' waiting for a motion or second key.
            if (event == Event::Character('d')) {
                if (state->pending_keys == "d") {
                    // dd = delete current line
                    int line_start = state->cursor_pos;
                    while (line_start > 0 && state->text[line_start - 1] != '\n')
                        --line_start;
                    int line_end = state->cursor_pos;
                    while (line_end < len && state->text[line_end] != '\n')
                        ++line_end;
                    if (line_end < len) ++line_end; // include '\n'
                    state->unnamed_reg.content = state->text.substr(line_start, line_end - line_start);
                    state->unnamed_reg.is_linewise = true;
                    state->text.erase(line_start, line_end - line_start);
                    state->cursor_pos = std::min(line_start, static_cast<int>(state->text.size()));
                    state->pending_keys.clear();
                    save_undo();
                    notify_change();
                } else {
                    state->pending_keys = "d";
                }
                return true;
            }
            if (event == Event::Character('y')) {
                if (state->pending_keys == "y") {
                    // yy = yank current line
                    int line_start = state->cursor_pos;
                    while (line_start > 0 && state->text[line_start - 1] != '\n')
                        --line_start;
                    int line_end = state->cursor_pos;
                    while (line_end < len && state->text[line_end] != '\n')
                        ++line_end;
                    if (line_end < len) ++line_end;
                    state->unnamed_reg.content = state->text.substr(line_start, line_end - line_start);
                    state->unnamed_reg.is_linewise = true;
                    state->pending_keys.clear();
                } else {
                    state->pending_keys = "y";
                }
                return true;
            }

            // Editing
            if (event == Event::Character('x')) {
                if (state->cursor_pos < len) {
                    state->unnamed_reg.content = state->text.substr(state->cursor_pos, 1);
                    state->text.erase(state->cursor_pos, 1);
                    if (state->cursor_pos >= static_cast<int>(state->text.size()) && state->cursor_pos > 0) {
                        state->cursor_pos--;
                    }
                    save_undo();
                    notify_change();
                }
                return true;
            }
            if (event == Event::Character('u')) {
                // Undo
                if (state->undo_index > 0) {
                    state->undo_index--;
                    state->text = state->undo_stack[state->undo_index];
                    state->cursor_pos = std::min(state->cursor_pos,
                        std::max(0, static_cast<int>(state->text.size()) - 1));
                    notify_change();
                }
                return true;
            }
            // Ctrl+R = redo
            if (event == Event::Character('\x12')) {
                if (state->undo_index < static_cast<int>(state->undo_stack.size()) - 1) {
                    state->undo_index++;
                    state->text = state->undo_stack[state->undo_index];
                    state->cursor_pos = std::min(state->cursor_pos,
                        std::max(0, static_cast<int>(state->text.size()) - 1));
                    notify_change();
                }
                return true;
            }
            if (event == Event::Character('p')) {
                // Paste after cursor
                if (!state->unnamed_reg.content.empty()) {
                    int insert_pos = std::min(state->cursor_pos + 1, len);
                    state->text.insert(insert_pos, state->unnamed_reg.content);
                    state->cursor_pos = insert_pos +
                        static_cast<int>(state->unnamed_reg.content.size()) - 1;
                    save_undo();
                    notify_change();
                }
                return true;
            }

            // Submit with Enter in normal mode
            if (event == Event::Return) {
                if (opts->on_submit) opts->on_submit(state->text);
                return true;
            }

            if (event == Event::Escape) {
                if (is_any_visual) {
                    set_mode(VimMode::Normal);
                    state->visual_start = -1;
                }
                state->pending_keys.clear();
                state->repeat_count = 0;
                return true;
            }
        }

        return false;
    });
}

} // namespace cc::ui::prompt::vim_input
