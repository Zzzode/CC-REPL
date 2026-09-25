// text_input_vim.cpp - impl unit for cc.ui.widgets.text_input
// (RFC 0001 Phase C batch 8). TextInputImpl::HandleVimEvent — the normal /
// visual / command / replace vim dispatcher; its `after_vim` goto label
// stays intact inside this one function, which is never split — plus the
// vim motion / operator helpers (word motion, dd, yy, e, D, J, o, O, ~).
//
// Phase A (#184957): textual FTXUI event header + `import std;` (the
// textual FTXUI include keeps the reduced-BMI writer happy).
module;

#include <cctype>

#include <ftxui/component/event.hpp>

module cc.ui.widgets.text_input;

import std;

import cc.ui.foundation.ui_types;  // cc::ui::common::VimMode canonical enum

namespace ui::components {

    // ------------------------------------------------------------
    // Vim mode event handling
    // TS REF: src/hooks/useVimInput.ts:175-295 — handleVimInput()
    //
    // Dispatches keys based on the current vim_.mode.
    // Returns true if the event was consumed (vim handled it),
    // false to fall through to the standard readline handler.
    // ------------------------------------------------------------
    bool TextInputImpl::HandleVimEvent(Event event) {
        using namespace ftxui;
        using cc::ui::common::VimMode;
        bool changed = false;

        const bool is_visual = (vim_.mode == VimMode::Visual ||
                                vim_.mode == VimMode::VisualLine ||
                                vim_.mode == VimMode::VisualBlock);

        // --- Escape: mode-dependent cancel / switch ---
        // TS REF: useVimInput.ts:192-201
        if (event == Event::Escape) {
            // Always clear pending multi-key states on Escape
            vim_.pending_g = false;
            vim_.pending_replace = false;

            if (vim_.mode == VimMode::Insert ||
                vim_.mode == VimMode::Replace) {
                // INSERT → NORMAL: cursor left by 1 (vim convention)
                if (cursor_ > 0 && text_[cursor_ - 1] != '\n') {
                    move_cursor(-1, false);
                }
                vim_.mode = VimMode::Normal;
                vim_.pending_operator.clear();
                return true;
            }
            if (is_visual) {
                // VISUAL → NORMAL: clear selection
                vim_.mode = VimMode::Normal;
                clear_selection();
                vim_.pending_operator.clear();
                return true;
            }
            if (vim_.mode == VimMode::Normal) {
                // NORMAL: cancel pending operator
                vim_.pending_operator.clear();
                return true;
            }
            if (vim_.mode == VimMode::Command) {
                vim_.mode = VimMode::Normal;
                return true;
            }
        }

        // --- Enter: pass through to base handler (submission works from any mode) ---
        // TS REF: useVimInput.ts:204-207
        if (event == Event::Return) {
            return false;  // let base handler process Enter (submit / newline)
        }

        // --- INSERT / REPLACE mode: pass text input to base handler ---
        // TS REF: useVimInput.ts:209-228
        if (vim_.mode == VimMode::Insert ||
            vim_.mode == VimMode::Replace) {
            return false;  // fall through to standard readline handler
        }

        // --- COMMAND mode ---
        if (vim_.mode == VimMode::Command) {
            if (event == Event::Backspace) {
                // Could track command buffer here; for now just exit
                vim_.mode = VimMode::Normal;
                return true;
            }
            if (event.is_character()) {
                // Minimal command mode: just consume chars; Enter handled above
                return true;
            }
            return false;
        }

        // --- NORMAL / VISUAL mode: vim command dispatch ---
        // TS REF: useVimInput.ts:231+ (NORMAL mode command handling)

        // Arrow keys → vim motions in normal mode, extend selection in visual
        if (event == Event::ArrowLeft || event == Event::Character('h')) {
            move_cursor(-1, is_visual); return true;
        }
        if (event == Event::ArrowRight || event == Event::Character('l')) {
            move_cursor(+1, is_visual); return true;
        }
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            if (options_.multiline) {
                move_line_vertical(-1, is_visual);
            } else {
                navigate_history_up();
                changed = true;
            }
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            if (options_.multiline) {
                move_line_vertical(+1, is_visual);
            } else {
                navigate_history_down();
                changed = true;
            }
            return true;
        }

        // 0 = line start, $ = line end
        if (event == Event::Character('0')) { move_home(is_visual); return true; }
        if (event == Event::Character('$')) { move_end(is_visual); return true; }

        // w = word forward, b = word backward
        if (event == Event::Character('w')) { vim_word_motion(+1, is_visual); return true; }
        if (event == Event::Character('b')) { vim_word_motion(-1, is_visual); return true; }

        // Mode entry from Normal
        if (vim_.mode == VimMode::Normal) {
            if (event == Event::Character('i')) {
                vim_.mode = VimMode::Insert;
                vim_.pending_operator.clear();
                return true;
            }
            if (event == Event::Character('a')) {
                move_cursor(+1, false);
                vim_.mode = VimMode::Insert;
                vim_.pending_operator.clear();
                return true;
            }
            if (event == Event::Character('I')) {
                move_home(false);
                vim_.mode = VimMode::Insert;
                return true;
            }
            if (event == Event::Character('A')) {
                move_end(false);
                vim_.mode = VimMode::Insert;
                return true;
            }
            if (event == Event::Character('v')) {
                vim_.mode = VimMode::Visual;
                sel_start_ = cursor_;
                sel_end_ = cursor_;
                return true;
            }
            if (event == Event::Character('V')) {
                vim_.mode = VimMode::VisualLine;
                // Select entire current line
                {
                    int ls = cursor_;
                    while (ls > 0 && text_[ls - 1] != '\n') --ls;
                    int le = cursor_;
                    while (le < (int)text_.size() && text_[le] != '\n') ++le;
                    sel_start_ = ls;
                    sel_end_ = le;
                    cursor_ = ls;
                }
                return true;
            }
            // Ctrl+V = VisualBlock (block selection)
            if (event == Event::Character('\x16')) {
                vim_.mode = VimMode::VisualBlock;
                sel_start_ = cursor_;
                sel_end_ = cursor_;
                return true;
            }
            if (event == Event::Character('R')) {
                vim_.mode = VimMode::Replace;
                return true;
            }
            if (event == Event::Character(':')) {
                vim_.mode = VimMode::Command;
                return true;
            }
        }

        // Operators: d (delete), y (yank), c (change)
        if (vim_.mode == VimMode::Normal) {
            if (event == Event::Character('d')) {
                if (vim_.pending_operator == "d") {
                    // dd = delete line
                    vim_delete_line();
                    vim_.pending_operator.clear();
                    changed = true;
                } else {
                    vim_.pending_operator = "d";
                }
                goto after_vim;
            }
            if (event == Event::Character('y')) {
                if (vim_.pending_operator == "y") {
                    // yy = yank line
                    vim_yank_line();
                    vim_.pending_operator.clear();
                } else {
                    vim_.pending_operator = "y";
                }
                goto after_vim;
            }
        }

        // x = delete char at cursor (normal mode)
        if (vim_.mode == VimMode::Normal && event == Event::Character('x')) {
            vim_.unnamed_register = (cursor_ < (int)text_.size())
                ? std::string(1, text_[cursor_]) : "";
            vim_.yank_is_linewise = false;
            delete_char();
            changed = true;
            goto after_vim;
        }

        // p / P = paste (normal mode)
        if (vim_.mode == VimMode::Normal) {
            if (event == Event::Character('p')) {
                if (!vim_.unnamed_register.empty()) {
                    if (vim_.yank_is_linewise) {
                        // Linewise paste: insert after current line
                        int insert_pos = cursor_;
                        while (insert_pos < (int)text_.size() && text_[insert_pos] != '\n')
                            ++insert_pos;
                        if (insert_pos < (int)text_.size()) ++insert_pos; // skip '\n'
                        push_undo();
                        text_.insert(insert_pos, vim_.unnamed_register);
                        cursor_ = insert_pos;
                        sel_start_ = sel_end_ = -1;
                        recompute_derived();
                    } else {
                        // Character-wise paste: insert after cursor
                        push_undo();
                        int insert_pos = std::min(cursor_ + 1, (int)text_.size());
                        text_.insert(insert_pos, vim_.unnamed_register);
                        cursor_ = insert_pos + (int)vim_.unnamed_register.size() - 1;
                        sel_start_ = sel_end_ = -1;
                        recompute_derived();
                    }
                    changed = true;
                    goto after_vim;
                }
                return true;
            }
            if (event == Event::Character('P')) {
                if (!vim_.unnamed_register.empty()) {
                    push_undo();
                    text_.insert(cursor_, vim_.unnamed_register);
                    // cursor stays at original position
                    sel_start_ = sel_end_ = -1;
                    recompute_derived();
                    changed = true;
                    goto after_vim;
                }
                return true;
            }
        }

        // u = undo, Ctrl+R = redo (normal mode)
        // TS REF: useVimInput.ts:165-167 (u → onUndo)
        if (vim_.mode == VimMode::Normal) {
            if (event == Event::Character('u')) {
                undo(); changed = true; goto after_vim;
            }
            if (event == Event::Character('\x12')) {
                redo(); changed = true; goto after_vim;
            }
        }

        // ── Additional TS-faithful Normal-mode commands ────────────────
        // TS REF: src/vim/transitions.ts — handleNormalInput() covers
        //   D, C, Y, J, G, o, O, r, ~, e, gg, etc.
        if (vim_.mode == VimMode::Normal) {
            // e = end of word (TS REF: motions.ts — 'e' case)
            if (event == Event::Character('e')) {
                vim_word_end_motion(false); return true;
            }

            // G = last line, or count=NG = goto line N (TS REF: transitions.ts 'G')
            if (event == Event::Character('G')) {
                int n = (int)text_.size();
                // Jump to end (last line start)
                int last_line_start = n;
                while (last_line_start > 0 && text_[last_line_start - 1] != '\n')
                    --last_line_start;
                apply_move(last_line_start, false);
                return true;
            }

            // g = pending "gg" / "gj" / "gk" (TS REF: transitions.ts fromG)
            if (event == Event::Character('g')) {
                vim_.pending_g = true;
                return true;
            }

            // D = delete to end of line (TS REF: transitions.ts 'D' → executeOperatorMotion('delete','$',1,ctx))
            if (event == Event::Character('D')) {
                vim_delete_to_end();
                changed = true; goto after_vim;
            }

            // C = change to end of line (TS REF: transitions.ts 'C' → delete '$' + enterInsert)
            if (event == Event::Character('C')) {
                vim_delete_to_end();
                vim_.mode = VimMode::Insert;
                vim_.pending_operator.clear();
                changed = true; goto after_vim;
            }

            // Y = yank line (TS REF: transitions.ts 'Y' → executeLineOp('yank',count,ctx))
            if (event == Event::Character('Y')) {
                vim_yank_line();
                return true;
            }

            // J = join lines (TS REF: transitions.ts 'J' → executeJoin)
            if (event == Event::Character('J')) {
                vim_join_lines();
                changed = true; goto after_vim;
            }

            // o = open line below (TS REF: transitions.ts 'o' → executeOpenLine('below'))
            if (event == Event::Character('o')) {
                vim_open_line_below();
                vim_.mode = VimMode::Insert;
                vim_.pending_operator.clear();
                changed = true; goto after_vim;
            }

            // O = open line above (TS REF: transitions.ts 'O' → executeOpenLine('above'))
            if (event == Event::Character('O')) {
                vim_open_line_above();
                vim_.mode = VimMode::Insert;
                vim_.pending_operator.clear();
                changed = true; goto after_vim;
            }

            // r = replace single char (TS REF: transitions.ts 'r' → fromReplace state)
            // Simplified: r{char} replaces the char under cursor
            if (event == Event::Character('r')) {
                vim_.pending_replace = true;
                return true;
            }

            // ~ = toggle case (TS REF: transitions.ts '~' → executeToggleCase)
            // Simplified: toggle case of char under cursor
            if (event == Event::Character('~')) {
                vim_toggle_case();
                changed = true; goto after_vim;
            }
        }

        // Handle pending 'g' (gg / gj / gk)
        // TS REF: transitions.ts fromG — 'gg' → first line / goto count, 'gj'/'gk' → display lines
        if (vim_.pending_g && vim_.mode == VimMode::Normal) {
            if (event == Event::Character('g')) {
                // gg = first line
                apply_move(0, false);
                vim_.pending_g = false;
                return true;
            }
            if (event == Event::Character('j')) {
                // gj = down display line (simplified: same as j in single-line viewport)
                move_line_vertical(+1, false);
                vim_.pending_g = false;
                return true;
            }
            if (event == Event::Character('k')) {
                // gk = up display line (simplified: same as k in single-line viewport)
                move_line_vertical(-1, false);
                vim_.pending_g = false;
                return true;
            }
            vim_.pending_g = false;
            // unrecognized after g: cancel, don't consume
        }

        // Handle pending 'r' (replace single char)
        // TS REF: transitions.ts fromReplace — executeReplace(input, count, ctx)
        if (vim_.pending_replace && vim_.mode == VimMode::Normal) {
            if (event.is_character() && event.character().size() == 1) {
                char replacement = event.character()[0];
                if (cursor_ < (int)text_.size()) {
                    push_undo();
                    vim_.unnamed_register = std::string(1, text_[cursor_]);
                    vim_.yank_is_linewise = false;
                    text_[cursor_] = replacement;
                    changed = true;
                }
                vim_.pending_replace = false;
                goto after_vim;
            }
            // Escape or non-char: cancel replace
            vim_.pending_replace = false;
            if (event == Event::Escape) return true;
        }

        // Visual mode: d = delete selection, y = yank selection
        if (is_visual) {
            if (event == Event::Character('d')) {
                if (has_selection()) {
                    auto [a, b] = selection();
                    vim_.unnamed_register = text_.substr(a, b - a);
                    vim_.yank_is_linewise = (vim_.mode == VimMode::VisualLine);
                    push_undo();
                    text_.erase(a, b - a);
                    cursor_ = a;
                    sel_start_ = sel_end_ = -1;
                    recompute_derived();
                    changed = true;
                }
                vim_.mode = VimMode::Normal;
                goto after_vim;
            }
            if (event == Event::Character('y')) {
                if (has_selection()) {
                    auto [a, b] = selection();
                    vim_.unnamed_register = text_.substr(a, b - a);
                    vim_.yank_is_linewise = (vim_.mode == VimMode::VisualLine);
                }
                vim_.mode = VimMode::Normal;
                clear_selection();
                goto after_vim;
            }
        }

        // Unrecognized key in normal/visual mode: clear pending operator, don't consume
        if (!vim_.pending_operator.empty()) {
            vim_.pending_operator.clear();
        }
        return false;

        after_vim:
            if (changed) {
                update_suggestions_from_provider();
                recompute_derived();
                if (options_.on_change) options_.on_change(text_, options_.context);
            }
            return true;
    }

    // Helper: word motion forward (+1) or backward (-1)
    void TextInputImpl::vim_word_motion(int dir, bool extend_selection) {
        int n = (int)text_.size();
        int pos = cursor_;
        if (dir > 0) {
            // w: forward to start of next word
            if (pos >= n) return;
            // Skip current word chars
            if (!std::isspace(static_cast<unsigned char>(text_[pos]))) {
                while (pos < n && !std::isspace(static_cast<unsigned char>(text_[pos]))) ++pos;
            }
            // Skip whitespace
            while (pos < n && std::isspace(static_cast<unsigned char>(text_[pos]))) ++pos;
        } else {
            // b: backward to start of previous word
            if (pos <= 0) { apply_move(0, extend_selection); return; }
            --pos;
            // Skip whitespace
            while (pos > 0 && std::isspace(static_cast<unsigned char>(text_[pos]))) --pos;
            // Skip word chars backward
            while (pos > 0 && !std::isspace(static_cast<unsigned char>(text_[pos - 1]))) --pos;
        }
        apply_move(std::clamp(pos, 0, n), extend_selection);
    }

    // Helper: dd — delete current line, yank to register
    void TextInputImpl::vim_delete_line() {
        push_undo();
        int line_start = cursor_;
        while (line_start > 0 && text_[line_start - 1] != '\n') --line_start;
        int line_end = cursor_;
        while (line_end < (int)text_.size() && text_[line_end] != '\n') ++line_end;
        bool had_newline = (line_end < (int)text_.size());
        if (had_newline) ++line_end; // include the '\n'
        vim_.unnamed_register = text_.substr(line_start, line_end - line_start);
        vim_.yank_is_linewise = had_newline;
        text_.erase(line_start, line_end - line_start);
        cursor_ = std::min(line_start, (int)text_.size());
        sel_start_ = sel_end_ = -1;
        recompute_derived();
    }

    // Helper: yy — yank current line
    void TextInputImpl::vim_yank_line() {
        int line_start = cursor_;
        while (line_start > 0 && text_[line_start - 1] != '\n') --line_start;
        int line_end = cursor_;
        while (line_end < (int)text_.size() && text_[line_end] != '\n') ++line_end;
        bool had_newline = (line_end < (int)text_.size());
        if (had_newline) ++line_end;
        vim_.unnamed_register = text_.substr(line_start, line_end - line_start);
        vim_.yank_is_linewise = had_newline;
    }

    // Helper: e — word end motion (TS REF: motions.ts 'e' → endOfVimWord)
    void TextInputImpl::vim_word_end_motion(bool extend_selection) {
        int n = (int)text_.size();
        int pos = cursor_;
        if (pos >= n) return;
        // If on whitespace, advance to next word first
        if (std::isspace(static_cast<unsigned char>(text_[pos]))) {
            while (pos < n && std::isspace(static_cast<unsigned char>(text_[pos]))) ++pos;
        }
        // Skip to end of current word
        while (pos < n - 1 && !std::isspace(static_cast<unsigned char>(text_[pos + 1]))) ++pos;
        apply_move(std::clamp(pos, 0, n - 1), extend_selection);
    }

    // Helper: D / C — delete from cursor to end of line (TS REF: operators.ts
    //   executeOperatorMotion('delete', '$', ...) → deletes to end of logical line)
    void TextInputImpl::vim_delete_to_end() {
        push_undo();
        int line_end = cursor_;
        while (line_end < (int)text_.size() && text_[line_end] != '\n') ++line_end;
        if (line_end > cursor_) {
            vim_.unnamed_register = text_.substr(cursor_, line_end - cursor_);
            vim_.yank_is_linewise = false;
            text_.erase(cursor_, line_end - cursor_);
            sel_start_ = sel_end_ = -1;
            recompute_derived();
        }
    }

    // Helper: J — join current line with next (TS REF: operators.ts executeJoin)
    // Replaces the newline between lines with a single space.
    void TextInputImpl::vim_join_lines() {
        int n = (int)text_.size();
        int line_end = cursor_;
        while (line_end < n && text_[line_end] != '\n') ++line_end;
        if (line_end >= n) return;  // already last line
        push_undo();
        // Find first non-whitespace on next line
        int next_start = line_end + 1;
        int content_start = next_start;
        while (content_start < n && (text_[content_start] == ' ' || text_[content_start] == '\t'))
            ++content_start;
        // Replace '\n' + trailing whitespace with a single space
        text_.erase(line_end, content_start - line_end);
        text_.insert(line_end, " ");
        cursor_ = line_end;  // cursor on the joining space
        sel_start_ = sel_end_ = -1;
        recompute_derived();
    }

    // Helper: o — open new line below (TS REF: operators.ts executeOpenLine('below'))
    void TextInputImpl::vim_open_line_below() {
        push_undo();
        // Move to end of current line, then insert newline
        int line_end = cursor_;
        while (line_end < (int)text_.size() && text_[line_end] != '\n') ++line_end;
        text_.insert(line_end, "\n");
        cursor_ = line_end + 1;
        sel_start_ = sel_end_ = -1;
        recompute_derived();
    }

    // Helper: O — open new line above (TS REF: operators.ts executeOpenLine('above'))
    void TextInputImpl::vim_open_line_above() {
        push_undo();
        // Move to start of current line, then insert newline before
        int line_start = cursor_;
        while (line_start > 0 && text_[line_start - 1] != '\n') --line_start;
        text_.insert(line_start, "\n");
        cursor_ = line_start;
        sel_start_ = sel_end_ = -1;
        recompute_derived();
    }

    // Helper: ~ — toggle case of char under cursor (TS REF: operators.ts
    //   executeToggleCase)
    void TextInputImpl::vim_toggle_case() {
        if (cursor_ >= (int)text_.size()) return;
        unsigned char c = static_cast<unsigned char>(text_[cursor_]);
        if (std::islower(c)) {
            push_undo();
            text_[cursor_] = static_cast<char>(std::toupper(c));
            // Advance cursor (vim behavior: ~ moves right)
            if (cursor_ + 1 < (int)text_.size() && text_[cursor_ + 1] != '\n') {
                ++cursor_;
            }
            recompute_derived();
        } else if (std::isupper(c)) {
            push_undo();
            text_[cursor_] = static_cast<char>(std::tolower(c));
            if (cursor_ + 1 < (int)text_.size() && text_[cursor_ + 1] != '\n') {
                ++cursor_;
            }
            recompute_derived();
        }
    }

}  // namespace ui::components
