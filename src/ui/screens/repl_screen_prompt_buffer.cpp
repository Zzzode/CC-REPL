// repl_screen_prompt_buffer.cpp - impl unit for cc.ui.screens.repl_screen
// (RFC 0001 Phase C batch 9). prompt text-buffer mutation: utf8 cursor helpers, insert/backspace/delete,
// pending @mention drain, prompt stash/restore, cursor moves and the
// text-derived bash-mode predicate.
//
// Phase A (#184957): std is TEXTUAL here on purpose - no `import std;`.
// Empty-GMF impl unit of an FTXUI-GMF primary: clang 22's reduced-BMI
// writer would otherwise emit a duplicate aligned operator new
// (LLVM #184957). Same fallback as app_extra_methods.cpp /
// messages_list_geometry.cpp.
module;

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

module cc.ui.screens.repl_screen;

import cc.types.types;

namespace cc::ui::repl_screen {

[[nodiscard]] bool is_utf8_continuation_byte(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

[[nodiscard]] std::size_t clamp_input_cursor(
    const std::string& text,
    std::size_t pos) {
    if (pos == std::string::npos || pos > text.size()) return text.size();
    while (pos > 0 &&
           pos < text.size() &&
           is_utf8_continuation_byte(static_cast<unsigned char>(text[pos]))) {
        --pos;
    }
    return pos;
}

[[nodiscard]] std::size_t input_cursor_or_end(
    const ReplScreenState& s) {
    return clamp_input_cursor(s.input_text, s.input_cursor);
}

[[nodiscard]] std::size_t previous_utf8_boundary(
    const std::string& text,
    std::size_t pos) {
    pos = clamp_input_cursor(text, pos);
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 &&
           is_utf8_continuation_byte(static_cast<unsigned char>(text[pos]))) {
        --pos;
    }
    return pos;
}

[[nodiscard]] std::size_t next_utf8_boundary(
    const std::string& text,
    std::size_t pos) {
    pos = clamp_input_cursor(text, pos);
    if (pos >= text.size()) return text.size();
    ++pos;
    while (pos < text.size() &&
           is_utf8_continuation_byte(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
    return pos;
}

void set_prompt_input_text(
    const std::shared_ptr<ReplScreenState>& state,
    std::string value,
    std::size_t cursor) {
    state->input_text = std::move(value);
    state->input_cursor = clamp_input_cursor(state->input_text, cursor);
    state->is_prompt_input_active = true;
    state->last_keystroke = std::chrono::steady_clock::now();
}

void insert_prompt_text(
    const std::shared_ptr<ReplScreenState>& state,
    std::string_view value) {
    auto cursor = input_cursor_or_end(*state);
    state->input_text.insert(cursor, value);
    set_prompt_input_text(state, std::move(state->input_text), cursor + value.size());
}

// AT-09: drain inbound IDE at_mentioned tokens staged by the MCP receive
// thread. MUST be called on the render thread (it mutates input_text/cursor).
// Returns the number of tokens inserted. Empty under the lock fast-path when
// nothing is pending so per-frame cost is negligible (keeps app.cppm thin).
std::size_t DrainPendingAtMentionInserts(
    const std::shared_ptr<ReplScreenState>& state) {
    std::vector<std::string> batch;
    {
        std::lock_guard<std::mutex> lk(state->pending_at_mention_mutex);
        if (state->pending_at_mention_inserts.empty()) return 0;
        batch.swap(state->pending_at_mention_inserts);
    }
    for (const auto& token : batch) {
        if (!token.empty()) insert_prompt_text(state, token);
    }
    return batch.size();
}

// ── Stashed prompt restore (GAP 2) ──────────────────────────────────────
// TS REF: src/screens/REPL.tsx L1373-1377 — stashedPrompt state:
//   {text, cursorOffset, pastedContents}.  When the user has typed input
//   and a background agent finishes or a permission request interrupts, the
//   current input is stashed so it can be restored after the request
//   completes.  Restore at:
//     - TS L3251-3255 (after local-jsx result returns)
//     - TS L3344-3348 (on submit when not slash-command)
//     - TS L3527-3531 (after handlePromptSubmit for slash/loading)
//   The stash notice (PromptInputStashNotice.tsx) renders
//   "{figures.pointerSmall} Stashed (auto-restores after submit)" when
//   hasStash is true.

/// Stash the current input text and cursor position.  Called when a
/// background agent finishes or a permission request interrupts the user's
/// typing flow, or when the user presses Ctrl+S (chat:stash).
/// `pasted_images` and `pasted_texts` are the pasted-content maps from the
/// engine layer (app.cppm) so image/text refs in the stashed text survive
/// the stash/restore cycle.
/// Returns true if something was actually stashed (input was non-empty or
/// pasted contents were provided).
bool StashCurrentPrompt(
    const std::shared_ptr<ReplScreenState>& state,
    std::unordered_map<int, ::cc::core::ImageBlock> pasted_images,
    std::unordered_map<int, std::string> pasted_texts) {
    if (state->input_text.empty() && pasted_images.empty() && pasted_texts.empty())
        return false;
    ReplScreenState::StashedPrompt sp;
    sp.text = state->input_text;
    sp.cursor_offset = state->input_cursor;
    sp.pasted_images = std::move(pasted_images);
    sp.pasted_texts = std::move(pasted_texts);
    state->stashed_prompt = std::move(sp);
    return true;
}

/// Restore the stashed prompt into the input area.  Called after a submit
/// completes or when the user explicitly requests restore (Ctrl+S on empty
/// input).  Also returns the stashed pasted-contents maps via out-params so
/// the engine layer can restore [Image #N] / [...Truncated text #N] refs.
/// Returns true if a stash was restored.
bool RestoreStashedPrompt(
    const std::shared_ptr<ReplScreenState>& state,
    std::unordered_map<int, ::cc::core::ImageBlock>* out_images,
    std::unordered_map<int, std::string>* out_texts) {
    if (!state->stashed_prompt.has_value()) return false;
    auto stash = std::move(*state->stashed_prompt);
    state->stashed_prompt.reset();
    // Return pasted contents to the caller (engine layer).
    if (out_images) *out_images = std::move(stash.pasted_images);
    if (out_texts)  *out_texts  = std::move(stash.pasted_texts);
    set_prompt_input_text(state, std::move(stash.text),
        stash.cursor_offset == std::string::npos
            ? std::string::npos
            : stash.cursor_offset);
    return true;
}

/// True when a stashed prompt exists (for UI notice rendering).
bool HasStashedPrompt(const std::shared_ptr<ReplScreenState>& state) {
    return state->stashed_prompt.has_value();
}

bool backspace_prompt_text(const std::shared_ptr<ReplScreenState>& state) {
    auto cursor = input_cursor_or_end(*state);
    if (cursor == 0) return false;
    const auto prev = previous_utf8_boundary(state->input_text, cursor);
    state->input_text.erase(prev, cursor - prev);
    set_prompt_input_text(state, std::move(state->input_text), prev);
    return true;
}

// TS REF: src/components/PromptInput/PromptInput.tsx:1904-1908 —
//   `if (cursorOffset === 0 && (key.escape || key.backspace || key.delete ||
//        (key.ctrl && char === 'u'))) { onModeChange('prompt'); }`
//
// When the caret is at the very start of the buffer, Backspace/Escape/Delete/
// Ctrl+U exit any special input mode (Bash) back to Prompt.  This is what lets
// the user leave bash mode after typing a bare '!' into an empty prompt — the
// '!' is swallowed as a mode trigger (see the char handler below), so without
// this the buffer stays empty and Backspace would otherwise be a no-op,
// trapping the user in bash mode.  Returns true iff a mode reset occurred.
bool exit_input_mode_if_at_start(const std::shared_ptr<ReplScreenState>& state) {
    if (input_cursor_or_end(*state) != 0) return false;
    if (state->input_mode == InputMode::Normal) return false;
    state->input_mode = InputMode::Normal;
    state->is_prompt_input_active = true;
    state->last_keystroke = std::chrono::steady_clock::now();
    // Mode change alters the autocomplete provider context (TS parity with the
    // char-handler mode toggle) — drop any dismissed-suggestion memory.
    state->dismissed_autocomplete_for_input.clear();
    return true;
}

bool delete_prompt_text(const std::shared_ptr<ReplScreenState>& state) {
    auto cursor = input_cursor_or_end(*state);
    if (cursor >= state->input_text.size()) return false;
    const auto next = next_utf8_boundary(state->input_text, cursor);
    state->input_text.erase(cursor, next - cursor);
    set_prompt_input_text(state, std::move(state->input_text), cursor);
    return true;
}

void move_prompt_cursor_left(const std::shared_ptr<ReplScreenState>& state) {
    state->input_cursor = previous_utf8_boundary(
        state->input_text,
        input_cursor_or_end(*state));
    state->is_prompt_input_active = true;
    state->last_keystroke = std::chrono::steady_clock::now();
}

void move_prompt_cursor_right(const std::shared_ptr<ReplScreenState>& state) {
    state->input_cursor = next_utf8_boundary(
        state->input_text,
        input_cursor_or_end(*state));
    state->is_prompt_input_active = true;
    state->last_keystroke = std::chrono::steady_clock::now();
}

// ─── Effective bash-mode detection (TS getInputMode equivalent) ──────────
//
// TS PromptInput.tsx computes `inputMode = getInputMode(value)` on every
// render, where getInputMode checks the first character of the text value.
// This means the mode is TEXT-DERIVED: pasting "!ls" or typing '!' into
// non-empty input immediately flips the effective mode to Bash, even though
// the user never pressed bare-'!' to toggle.  The state-mode toggle
// (s.input_mode) only matters when the input buffer is empty — it persists
// the visual "! " prefix after a bare-'!' keystroke that was swallowed.
//
// Use this helper for ALL behavioural gates (autocomplete, @-mention
// suppression, shell-command $PATH scan) and for the prefix/border
// rendering to stay faithful to TS semantics.
[[nodiscard]] bool effective_is_bash(const ReplScreenState& s) {
    if (!s.input_text.empty() && s.input_text.front() == '!') return true;
    return s.input_mode == InputMode::Bash;
}

[[nodiscard]] std::optional<std::string> accept_selected_prompt_suggestion(
    const std::shared_ptr<ReplScreenState>& state) {
    if (state->autocomplete_suggestions.empty()) return std::nullopt;

    const int total = static_cast<int>(state->autocomplete_suggestions.size());
    const int selected = std::clamp(
        state->autocomplete_index < 0 ? 0 : state->autocomplete_index,
        0,
        total - 1);
    std::string accepted =
        state->autocomplete_suggestions[static_cast<std::size_t>(selected)].insert_text;
    if (accepted.empty()) {
        accepted =
            state->autocomplete_suggestions[static_cast<std::size_t>(selected)].display_text;
    }

    const auto& suggestion =
        state->autocomplete_suggestions[static_cast<std::size_t>(selected)];
    std::size_t start = suggestion.replacement_start;
    std::size_t end = suggestion.replacement_end;
    if (start == std::string::npos || end == std::string::npos ||
        start > end || end > state->input_text.size()) {
        start = 0;
        end = state->input_text.size();
    }

    state->input_text.replace(start, end - start, accepted);
    state->input_cursor = clamp_input_cursor(state->input_text, start + accepted.size());
    state->autocomplete_suggestions.clear();
    state->autocomplete_index = -1;
    state->is_prompt_input_active = true;
    state->last_keystroke = std::chrono::steady_clock::now();
    return state->input_text;
}

}  // namespace cc::ui::repl_screen
