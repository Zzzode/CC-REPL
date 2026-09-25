// text_input_events.cpp - impl unit for cc.ui.widgets.text_input
// (RFC 0001 Phase C batch 8). The readline-style event dispatcher:
// TextInputImpl::HandleEvent (its `after_change` goto label stays intact in
// this one function), reverse-history search (HandleSearchEvent +
// refresh_search_matches) and the static ctrl/shift key-matching helpers.
//
// Phase A (#184957): textual FTXUI event header + `import std;` (the
// textual FTXUI include keeps the reduced-BMI writer happy).
module;

#include <ftxui/component/event.hpp>

module cc.ui.widgets.text_input;

import std;

namespace ui::components {

// ------------------------------------------------------------
// Event handling (returns true if consumed)
// ------------------------------------------------------------
bool TextInputImpl::HandleEvent(Event event) {
    using namespace ftxui;
    last_keystroke_ms_ = std::chrono::steady_clock::now();
    blink_visible_ = true;     // reset on user activity
    blink_at_ = last_keystroke_ms_;
    bool changed = false;

    // --- Search mode (Ctrl+R reverse history search) ---
    if (search_mode_) {
        return HandleSearchEvent(event);
    }

    // --- Paste preview confirmation (GAP 1) ---
    // When a large paste (> 10000 chars) is pending, Enter confirms
    // and Esc cancels.  All other keys are ignored until resolved.
    // TS REF: inputPaste.ts — user must confirm before large paste is
    // injected into the input buffer.
    if (paste_preview_) {
        if (event == Event::Return) {
            ConfirmPaste();
            return true;
        }
        if (event == Event::Escape) {
            CancelPaste();
            return true;
        }
        return true;  // Swallow all other keys during preview
    }

    // --- Vim mode dispatch (canonical VimMode from cc::ui::common) ---
    // TS REF: src/hooks/useVimInput.ts:175 — handleVimInput dispatches
    //   keys based on current vim mode before base textInput handler.
    // TS REF: src/types/textInputTypes.ts:222 — VimMode = 'INSERT'|'NORMAL'
    if (options_.vim_mode.has_value()) {
        return HandleVimEvent(event);
    }

    // --- Modifier-aware short cuts ---
    // Ctrl+A -> select all
    if (event == Event::Character('\x01')) { select_all(); return true; }
    // Ctrl+C -> copy selection (external clipboard bridge via on_paste cb)
    if (event == Event::Character('\x03')) {
        // Do not consume: let REPL screen handle interrupt
        return false;
    }
    // Ctrl+Z -> undo (bash convention suspends — handled externally)
    // Ctrl+Shift+Z or Ctrl+Y -> redo. Here we map Ctrl+Y to redo.
    if (event == Event::Character('\x19')) { redo(); changed = true; goto after_change; }
    if (event == Event::Character('\x1a')) { undo(); changed = true; goto after_change; }

    // Ctrl+V -> paste (via callback or best-effort clipboard)
    if (event == Event::Character('\x16')) {
        if (options_.on_paste) {
            // Let caller provide the text; callback may be async.
            // We expose a separate public method PasteText() for the caller.
            return true;
        } else {
            std::string clip = read_clipboard();
            if (!clip.empty()) { paste_text(clip); changed = true; goto after_change; }
            return false;
        }
    }
    // Ctrl+R -> enter history search mode
    if (event == Event::Character('\x12')) { search_mode_ = true; return true; }

    // Ctrl+D / Delete
    if (event == Event::Delete || event == Event::Character('\x04')) {
        if (!text_.empty() || has_selection()) {
            delete_char(); changed = true; goto after_change;
        }
        return false; // let caller handle
    }

    // Character input — but skip modifier-only control chars
    if (event.is_character()) {
        char c = event.character()[0];
        unsigned char uc = static_cast<unsigned char>(c);
        // Regular printable ASCII (>=32) or UTF-8 multi-byte start
        if (uc >= 32 || (uc & 0x80)) {
            // Prefer inserting the full event.character() string for IME
            insert_at_cursor(event.character());
            changed = true; goto after_change;
        }
    }

    // Tab / Shift+Tab when suggestion open
    if (showing_suggestions_ && !suggestions_.empty()) {
        if (event == Event::Tab) { select_next_suggestion(); return true; }
        if (event == Event::TabReverse) { select_previous_suggestion(); return true; }
    } else {
        // Tab without suggestions — cycle history (only if history exists)
        if (event == Event::Tab) {
            if (history_.empty()) return false;
            navigate_history_up();
            return true;
        }
        if (event == Event::TabReverse) {
            // TS REF: PromptInput.tsx:1667 — shift+tab ('chat:cycleMode') cycles
            // permission modes, not history.  When the caller provides an
            // on_permission_cycle callback, prefer it over history navigation.
            // This makes the footer's "(shift+tab to cycle)" hint truthful.
            if (options_.on_permission_cycle) {
                options_.on_permission_cycle();
                return true;
            }
            if (history_.empty()) return false;
            navigate_history_down();
            return true;
        }
    }

    // Arrow navigation with optional shift for selection
    if (matches_with_shift(event, Event::ArrowLeft)) {
        move_cursor(-1, has_shift_modifier(event)); return true;
    }
    if (matches_with_shift(event, Event::ArrowRight)) {
        move_cursor(+1, has_shift_modifier(event)); return true;
    }
    if (matches_with_shift(event, Event::Home)) { move_home(has_shift_modifier(event)); return true; }
    if (matches_with_shift(event, Event::End))  { move_end(has_shift_modifier(event));  return true; }
    if (matches_with_shift(event, Event::ArrowUp)) {
        if (options_.multiline) {
            move_line_vertical(-1, has_shift_modifier(event));
        } else {
            navigate_history_up();
            changed = true;
        }
        return true;
    }
    if (matches_with_shift(event, Event::ArrowDown)) {
        if (options_.multiline) {
            move_line_vertical(+1, has_shift_modifier(event));
        } else {
            navigate_history_down();
            changed = true;
        }
        return true;
    }
    if (matches_with_shift(event, Event::PageUp)) {
        // Simple: jump to top
        move_top(has_shift_modifier(event)); return true;
    }
    if (matches_with_shift(event, Event::PageDown)) {
        move_bottom(has_shift_modifier(event)); return true;
    }

    // Backspace
    if (event == Event::Backspace) { backspace(); changed = true; goto after_change; }

    // Enter: hard-submit if single-line + not Ctrl-mod.
    // Ctrl+Enter = soft-submit (newline for multiline)
    if (event == Event::Return) {
        bool is_ctrl_enter = has_ctrl_modifier(event);
        if (showing_suggestions_ && !suggestions_.empty()) {
            accept_suggestion();
            changed = true; goto after_change;
        }
        if (!options_.multiline) {
            submit_internal(/*hard=*/true);
            return true;
        }
        if (is_ctrl_enter) {
            // soft-submit: Ctrl+Enter => fire on_soft_submit with full text
            submit_internal(/*hard=*/false);
            return true;
        }
        // Plain Enter: heuristic — on single-line buffer submit,
        // otherwise insert newline (matches TS behaviour)
        int nlines = count_lines();
        if (nlines == 1) {
            submit_internal(/*hard=*/true);
            return true;
        }
        insert_newline();
        changed = true; goto after_change;
    }

    // Escape — clear suggestions & search mode
    if (event == Event::Escape) {
        if (showing_suggestions_) {
            showing_suggestions_ = false;
            search_mode_ = false;
            return true;
        }
        if (has_selection()) {
            clear_selection();
            return true;
        }
        if (options_.on_escape) options_.on_escape();
        return true;
    }

    after_change:
        if (changed) update_suggestions_from_provider();
        if (changed && options_.on_change)
            options_.on_change(text_, options_.context);
        return changed;
}

bool TextInputImpl::HandleSearchEvent(Event event) {
    using namespace ftxui;

    // Enter — accept current search result
    if (event == Event::Return) {
        if (!search_matches_.empty() && search_selected_ < search_matches_.size()) {
            push_undo();
            text_ = search_matches_[search_selected_];
            cursor_ = static_cast<int>(text_.size());
            sel_start_ = sel_end_ = -1;
            recompute_derived();
        }
        search_mode_ = false;
        search_query_.clear();
        search_matches_.clear();
        return true;
    }

    // Escape — cancel search
    if (event == Event::Escape) {
        search_mode_ = false;
        search_query_.clear();
        search_matches_.clear();
        return true;
    }

    // Ctrl+R — cycle to next match (reverse direction)
    if (event == Event::Character('\x12')) {
        if (!search_matches_.empty()) {
            search_selected_ = (search_selected_ + 1) % search_matches_.size();
        }
        return true;
    }

    // Backspace — remove last char from query
    if (event == Event::Backspace) {
        if (!search_query_.empty()) {
            search_query_.pop_back();
            refresh_search_matches();
        }
        return true;
    }

    // Arrow up/down — navigate matches
    if (event == Event::ArrowUp || event == Event::ArrowDown) {
        if (search_matches_.empty()) return true;
        if (event == Event::ArrowUp) {
            search_selected_ = (search_selected_ + 1) % search_matches_.size();
        } else {
            search_selected_ = search_selected_ == 0
                ? search_matches_.size() - 1
                : search_selected_ - 1;
        }
        return true;
    }

    // Character input — add to search query
    if (event.is_character()) {
        char c = event.character()[0];
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc >= 32 || (uc & 0x80)) {
            search_query_ += event.character();
            refresh_search_matches();
            return true;
        }
    }

    return false;
}

void TextInputImpl::refresh_search_matches() {
    search_matches_.clear();
    search_selected_ = 0;
    if (search_query_.empty() || history_.empty()) return;
    // Search in reverse (newest first)
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        if (it->find(search_query_) != std::string::npos) {
            search_matches_.push_back(*it);
            if (search_matches_.size() >= 50) break; // cap at 50 results
        }
    }
}

bool TextInputImpl::has_ctrl_modifier(const Event& e) {
    // FTXUI Event::Return doesn't carry ctrl modifier on all terminals;
    // we approximate: the default Enter keypress is plain. The actual
    // ctrl+enter detection falls back to on_soft_submit call from host.
    return e.is_character() && e.character().size() >= 2 &&
           e.character()[0] == '\n' && e.character()[1] == '\x0d';
}
// Shift modifier appears as ";2" in xterm-style escape sequences:
//   \x1B[1;2D = Shift+Left,  \x1B[1;2A = Shift+Up, etc.
bool TextInputImpl::has_shift_modifier(const Event& e) {
    const std::string& s = e.input();
    return s.size() >= 5 &&
           s.substr(0, 5) == "\x1B[1;" &&
           s.find(";2") != std::string::npos;
}
// Match Home/End / PageUp/Down shift variants:
//   Shift+Home = \x1B[1;2H, Shift+End  = \x1B[1;2F
//   Shift+PageUp   = \x1B[5;2~
//   Shift+PageDown = \x1B[1;6;2~
bool TextInputImpl::matches_with_shift(const Event& e, const Event& base) {
    if (e == base) return true;
    const std::string& s = e.input();
    const std::string& b = base.input();
    // Arrow variants:  \x1B[D  →  \x1B[1;2D
    if (b.size() == 3 && b.substr(0, 2) == "\x1B[" &&
        s.size() == 7 && s.substr(0, 5) == "\x1B[1;" &&
        s[5] == '2' && s.back() == b.back()) {
        return true;
    }
    // Home / End variants:  \x1B[H  →  \x1B[1;2H
    if (b.size() == 3 && b.substr(0, 2) == "\x1B[" &&
        s.size() == 7 && s.substr(0, 5) == "\x1B[1;" &&
        s[5] == '2' && s.back() == b.back()) {
        return true;
    }
    // PageUp  \x1B[5~  →  \x1B[5;2~
    // PageDown \x1B[6~ →  \x1B[6;2~
    if (b.size() == 4 && b[3] == '~' &&
        s.size() == 6 && s[5] == '~' &&
        s.substr(0, 3) == b.substr(0, 3) &&
        s.substr(3, 2) == ";2") {
        return true;
    }
    return false;
}

}  // namespace ui::components
