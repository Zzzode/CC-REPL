// text_input_buffer.cpp - impl unit for cc.ui.widgets.text_input
// (RFC 0001 Phase C batch 8). TextInputImpl ctor and the editing / buffer /
// history / suggestion core: text mutation, cursor/selection moves, undo
// stack, history navigation, paste (with the 10k truncation preview path),
// submit, blink, masking, input-truncation safety net and derived-state
// recompute.
//
// Phase A (#184957): this unit textually includes <ftxui/screen/color.hpp>
// because the out-of-line ctor memberwise-copies TextInputOptions, which
// holds std::optional<ftxui::Color>; the textual FTXUI header also keeps the
// reduced-BMI writer happy, so `import std;` works.
module;

#include <cctype>
#include <cstddef>

#include <ftxui/screen/color.hpp>

module cc.ui.widgets.text_input;

import std;

import cc.ui.foundation.design_figures;
import cc.ui.foundation.ui_types;
import cc.ui.prompt.prompt_paste_handler;
import cc.utils.parse_references;

namespace ui::components {

namespace {

// TS REF: inputPaste.ts TRUNCATION_THRESHOLD=10000, PREVIEW_LENGTH=1000.
// Delegates to cc::utils::maybe_truncate_paste for the actual truncation
// logic (shared with app.cppm's ProcessCompletedPastes text-paste path).
//
// RFC 0001 Phase C batch 8: demoted from a private TextInputImpl member (and
// its TruncatedPasteResult alias) to this file-local free function so the
// primary interface no longer names cc.utils.parse_references. paste_text is
// its only caller; maybe_apply_input_truncation calls the shared utility
// directly, as it always did.
cc::utils::TruncatedPasteResult truncate_paste_result(
    std::string_view text, int paste_id) {
    return cc::utils::maybe_truncate_paste(text, paste_id);
}

}  // namespace

namespace detail {

bool is_utf8_continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}
int glyph_forward(const std::string& s, int byte_pos) {
    int n = static_cast<int>(s.size());
    if (byte_pos >= n) return n;
    int p = byte_pos + 1;
    while (p < n && is_utf8_continuation(static_cast<unsigned char>(s[p]))) ++p;
    return p;
}
int glyph_back(const std::string& s, int byte_pos) {
    int p = byte_pos - 1;
    while (p > 0 && is_utf8_continuation(static_cast<unsigned char>(s[p]))) --p;
    return std::max(0, p);
}

}  // namespace detail

TextInputImpl::TextInputImpl(const TextInputOptions& options)
    : options_(options), cursor_(0), sel_start_(-1), sel_end_(-1),
      history_index_(std::string::npos),
      showing_suggestions_(false), selected_suggestion_(0),
      last_keystroke_ms_(std::chrono::steady_clock::now()),
      blink_at_(std::chrono::steady_clock::now()),
      blink_visible_(true),
      search_mode_(false),
      search_selected_(0),
      paste_burst_in_progress_(false) {
    vim_.mode = options.vim_mode.value_or(cc::ui::common::VimMode::Insert);
}

// ------------------------------------------------------------
// Text / state accessors
// ------------------------------------------------------------
void TextInputImpl::set_text(const std::string& t) {
    push_undo();
    text_ = t;
    cursor_ = static_cast<int>(t.size());
    sel_start_ = sel_end_ = -1;
    recompute_derived();
}

// ------------------------------------------------------------
// History
// ------------------------------------------------------------
void TextInputImpl::add_to_history(const std::string& entry) {
    if (!entry.empty()) {
        // TS REF: src/components/PromptInput/inputModes.ts:4-14
        //   (prependModeCharacterToInput)
        // History stores the raw mode-prefixed string so recalling a
        // bash entry re-detects the mode via getModeFromInput on the
        // first character.
        //
        // Only prepend '!' when (a) prompt_mode says the user intended
        // bash AND (b) the entry doesn't already carry a '!' prefix
        // (which happens when text_ was populated by insert_char /
        // paste / set_text from REPL state that already includes it).
        // Without guard (b), recalling "!cmd" would prepend again →
        // "!!cmd" and the second recall would show "!!cmd".
        std::string hist_entry = entry;
        namespace figs = cc::ui::design::figures;
        // TS REF: src/components/PromptInput/inputModes.ts:4-14
        //   (prependModeCharacterToInput)
        // Only prepend '!' when (a) prompt_mode says the user intended
        // bash AND (b) the entry doesn't already carry a '!' prefix
        // (which happens when text_ was populated by insert_char /
        // paste / set_text from REPL state that already includes it).
        // Without guard (b), recalling "!cmd" would prepend again →
        // "!!cmd" and the second recall would show "!!cmd".
        if (options_.context.prompt_mode == PromptInputMode::Bash &&
            (hist_entry.empty() || hist_entry.front() != figs::kBashModeChar)) {
            hist_entry = figs::prepend_mode_char(hist_entry, figs::PromptMode::kBash);
        }
        auto it = std::find(history_.begin(), history_.end(), hist_entry);
        if (it != history_.end()) history_.erase(it);
        history_.push_back(hist_entry);
        if (history_.size() > options_.max_history)
            history_.pop_front();
    }
    history_index_ = std::string::npos;
}

// ------------------------------------------------------------
// Core editing primitives (public for test / external driver)
// ------------------------------------------------------------
void TextInputImpl::insert_at_cursor(const std::string& s) {
    if (s.empty()) return;
    if (options_.input_filter) {
        for (char c : s) {
            if (!options_.input_filter(c)) return;
        }
    }
    push_undo();
    if (has_selection()) delete_selection_internal();
    text_.insert(text_.begin() + cursor_, s.begin(), s.end());
    cursor_ += static_cast<int>(s.size());
    sel_start_ = sel_end_ = -1;
    recompute_derived();
}
void TextInputImpl::insert_char(char c) {
    if (options_.input_filter && !options_.input_filter(c)) return;
    push_undo();
    if (has_selection()) delete_selection_internal();
    text_.insert(text_.begin() + cursor_, c);
    ++cursor_;
    sel_start_ = sel_end_ = -1;
    recompute_derived();
}
void TextInputImpl::insert_newline() {
    push_undo();
    if (has_selection()) delete_selection_internal();
    text_.insert(text_.begin() + cursor_, '\n');
    ++cursor_;
    sel_start_ = sel_end_ = -1;
    recompute_derived();
}
void TextInputImpl::backspace() {
    if (has_selection()) {
        push_undo();
        delete_selection_internal();
        recompute_derived();
        return;
    }
    if (cursor_ == 0) return;
    push_undo();
    int prev = detail::glyph_back(text_, cursor_);
    text_.erase(text_.begin() + prev, text_.begin() + cursor_);
    cursor_ = prev;
    recompute_derived();
}
void TextInputImpl::delete_char() {
    if (has_selection()) {
        push_undo();
        delete_selection_internal();
        recompute_derived();
        return;
    }
    if (cursor_ >= static_cast<int>(text_.size())) return;
    push_undo();
    int next = detail::glyph_forward(text_, cursor_);
    text_.erase(text_.begin() + cursor_, text_.begin() + next);
    recompute_derived();
}
void TextInputImpl::move_cursor(int delta, bool extend_selection) {
    int new_cursor = std::clamp(cursor_ + delta, 0, static_cast<int>(text_.size()));
    if (extend_selection) {
        if (sel_start_ < 0) {
            sel_start_ = cursor_;
        }
        sel_end_ = new_cursor;
    } else {
        sel_start_ = sel_end_ = -1;
    }
    cursor_ = new_cursor;
}
void TextInputImpl::move_home(bool extend) {
    // line-aware: home = start of current line
    int n = static_cast<int>(text_.size());
    int pos = cursor_;
    while (pos > 0 && text_[pos - 1] != '\n') --pos;
    apply_move(pos, extend);
    (void)n;
}
void TextInputImpl::move_end(bool extend) {
    int n = static_cast<int>(text_.size());
    int pos = cursor_;
    while (pos < n && text_[pos] != '\n') ++pos;
    apply_move(pos, extend);
}
void TextInputImpl::move_bottom(bool extend) {
    apply_move(static_cast<int>(text_.size()), extend);
}
void TextInputImpl::select_all() {
    sel_start_ = 0;
    sel_end_ = cursor_ = static_cast<int>(text_.size());
}

void TextInputImpl::undo() {
    if (!options_.enable_undo_redo) return;
    if (undo_stack_.empty()) return;
    redo_stack_.push_back(snapshot());
    load_snapshot(undo_stack_.back());
    undo_stack_.pop_back();
    recompute_derived();
}
void TextInputImpl::redo() {
    if (!options_.enable_undo_redo) return;
    if (redo_stack_.empty()) return;
    undo_stack_.push_back(snapshot());
    load_snapshot(redo_stack_.back());
    redo_stack_.pop_back();
    recompute_derived();
}

void TextInputImpl::clear() {
    push_undo();
    text_.clear();
    cursor_ = 0;
    sel_start_ = sel_end_ = -1;
    history_index_ = std::string::npos;
    recompute_derived();
}

// ------------------------------------------------------------
// History navigation (moves text + cursor, no re-render in here)
// ------------------------------------------------------------
void TextInputImpl::navigate_history_up() {
    if (!options_.show_history || history_.empty()) return;
    if (history_index_ == std::string::npos) {
        pre_search_snapshot_ = snapshot();
        history_index_ = history_.size() - 1;
    } else if (history_index_ > 0) {
        --history_index_;
    }
    text_ = history_[history_index_];
    cursor_ = static_cast<int>(text_.size());
    sel_start_ = sel_end_ = -1;
    recompute_derived();
}
void TextInputImpl::navigate_history_down() {
    if (history_.empty() || history_index_ == std::string::npos) return;
    if (history_index_ < history_.size() - 1) {
        ++history_index_;
        text_ = history_[history_index_];
    } else {
        history_index_ = std::string::npos;
        load_snapshot(pre_search_snapshot_);
    }
    cursor_ = static_cast<int>(text_.size());
    sel_start_ = sel_end_ = -1;
    recompute_derived();
}

// ------------------------------------------------------------
// Suggestions
// ------------------------------------------------------------
void TextInputImpl::update_suggestions_from_provider() {
    if (!options_.get_suggestions) {
        showing_suggestions_ = false;
        suggestions_.clear();
        return;
    }
    suggestions_ = options_.get_suggestions(
        text_, cursor_, options_.context);
    showing_suggestions_ = !suggestions_.empty();
    selected_suggestion_ = 0;
}
void TextInputImpl::accept_suggestion() {
    if (!showing_suggestions_ || suggestions_.empty()) return;
    insert_suggestion(suggestions_[selected_suggestion_]);
    showing_suggestions_ = false;
}
void TextInputImpl::select_previous_suggestion() {
    if (suggestions_.empty()) return;
    selected_suggestion_ =
        (selected_suggestion_ - 1 + (int)suggestions_.size()) %
        (int)suggestions_.size();
}
void TextInputImpl::select_next_suggestion() {
    if (suggestions_.empty()) return;
    selected_suggestion_ =
        (selected_suggestion_ + 1) % (int)suggestions_.size();
}

/// External paste entry point (used by async clipboard provider)
void TextInputImpl::PasteText(const std::string& paste_content) {
    paste_text(paste_content);
    update_suggestions_from_provider();
    recompute_derived();
    if (options_.on_change) options_.on_change(text_, options_.context);
}

/// Confirm the pending paste preview: insert the truncated content.
/// TS REF: inputPaste.ts — confirmed large pastes are truncated to
///   head 500 + placeholder + tail 500 before insertion.
/// If the preview carries placeholder_content (truncated middle), emit
/// it via on_paste_truncated so the caller can store it for later
/// expansion at submit time (expand_pasted_text_refs).
void TextInputImpl::ConfirmPaste() {
    if (!paste_preview_) return;
    push_undo();
    if (has_selection()) delete_selection_internal();
    const std::string& content = paste_preview_->content;
    text_.insert(text_.begin() + cursor_, content.begin(), content.end());
    cursor_ += static_cast<int>(content.size());
    sel_start_ = sel_end_ = -1;

    // If this was a truncated paste, notify the caller with the
    // paste-id and the middle content that was elided.
    if (paste_preview_->is_large && !paste_preview_->placeholder_content.empty()) {
        if (options_.on_paste_truncated) {
            options_.on_paste_truncated(
                paste_preview_->paste_id,
                paste_preview_->placeholder_content);
        }
    }

    paste_burst_in_progress_ = true;
    paste_preview_.reset();
    recompute_derived();
    if (options_.on_change) options_.on_change(text_, options_.context);
}

// ------------------------------------------------------------
// Private editing helpers
// ------------------------------------------------------------
void TextInputImpl::apply_move(int pos, bool extend) {
    pos = std::clamp(pos, 0, static_cast<int>(text_.size()));
    if (extend) {
        if (sel_start_ < 0) sel_start_ = cursor_;
        sel_end_ = pos;
    } else {
        sel_start_ = sel_end_ = -1;
    }
    cursor_ = pos;
}
void TextInputImpl::move_line_vertical(int delta, bool extend) {
    int cur_line, cur_col;
    compute_cursor_position(cur_line, cur_col);
    auto lines = split_lines();
    int target = cur_line + delta;
    if (target < 0) {
        // Move to start of first line
        apply_move(0, extend);
        return;
    }
    if (target >= (int)lines.size()) {
        move_bottom(extend);
        return;
    }
    // Compute byte offset up to line start
    int offset = 0;
    for (int i = 0; i < target; ++i) offset += (int)lines[i].size() + 1;
    int target_line_len = (int)lines[target].size();
    int land = offset + std::min(cur_col, target_line_len);
    apply_move(land, extend);
}
void TextInputImpl::compute_cursor_position(int& out_line, int& out_col) const {
    out_line = 0;
    int col = 0;
    for (int i = 0; i < cursor_; ++i) {
        if (text_[i] == '\n') { ++out_line; col = 0; }
        else { ++col; }
    }
    out_col = col;
}
std::vector<std::string_view> TextInputImpl::split_lines() const {
    std::vector<std::string_view> result;
    const char* p = text_.data();
    const char* end = p + text_.size();
    const char* line_start = p;
    while (p < end) {
        if (*p == '\n') {
            result.emplace_back(line_start, p - line_start);
            ++p;
            line_start = p;
        } else {
            ++p;
        }
    }
    result.emplace_back(line_start, p - line_start);
    return result;
}
int TextInputImpl::count_lines() const {
    if (text_.empty()) return 1;
    int n = 1;
    for (char c : text_) if (c == '\n') ++n;
    return n;
}
void TextInputImpl::delete_selection_internal() {
    auto [a, b] = selection();
    if (a < 0 || b <= a) return;
    text_.erase(text_.begin() + a, text_.begin() + b);
    cursor_ = a;
    sel_start_ = sel_end_ = -1;
}
void TextInputImpl::paste_text(const std::string& paste) {
    // Normalise CR/LF -> LF
    std::string normalized;
    normalized.reserve(paste.size());
    for (size_t i = 0; i < paste.size(); ++i) {
        if (paste[i] == '\r') {
            if (i + 1 < paste.size() && paste[i + 1] == '\n') continue;
            normalized.push_back('\n');
        } else {
            normalized.push_back(paste[i]);
        }
    }

    // GAP 1: paste-text-truncation-10k-threshold
    // TS REF: src/components/PromptInput/inputPaste.ts — pastes longer than
    //   TRUNCATION_THRESHOLD (10 000 chars) show a PastePreview confirmation
    //   overlay instead of being inserted directly.  The user must press
    //   Enter to confirm (truncated insert) or Esc to cancel.
    constexpr std::size_t kTruncationThreshold = 10000;
    if (normalized.size() > kTruncationThreshold) {
        // Count lines in the full paste (for preview display)
        std::size_t line_count = 1;
        for (char c : normalized) if (c == '\n') ++line_count;

        // Build the truncated version (head 500 + placeholder ref + tail 500)
        // that will be inserted on confirmation.  Also capture the truncated
        // middle content so the caller can store it for submit-time expansion.
        const int paste_id = next_paste_id_++;
        const auto result = truncate_paste_result(normalized, paste_id);

        paste_preview_ = cc::ui::prompt::PastePreview{
            .content             = result.truncated_text,
            .line_count          = line_count,
            .is_large            = true,
            .paste_id            = paste_id,
            .placeholder_content = result.placeholder_content,
        };
        return;  // Don't insert yet — wait for confirmation
    }

    // Normal paste (<= 10000 chars): insert directly
    push_undo();
    if (has_selection()) delete_selection_internal();
    text_.insert(text_.begin() + cursor_, normalized.begin(), normalized.end());
    cursor_ += (int)normalized.size();
    sel_start_ = sel_end_ = -1;
    paste_burst_in_progress_ = true;
}

void TextInputImpl::insert_suggestion(const Suggestion& s) {
    push_undo();
    if (has_selection()) delete_selection_internal();
    // Replace the current "prefix token" up to the last trigger symbol
    // Simple heuristic: back up until whitespace / start of buffer.
    int stop = cursor_;
    while (stop > 0 && !std::isspace(static_cast<unsigned char>(text_[stop - 1]))) {
        // Stop at known trigger chars as boundary for better UX
        if (stop > 1) {
            char prev = text_[stop - 1];
            if (prev == '/' || prev == '!' || prev == '@' || prev == '*' || prev == '&') {
                --stop; break;
            }
        }
        --stop;
    }
    text_.erase(text_.begin() + stop, text_.begin() + cursor_);
    text_.insert(text_.begin() + stop, s.text.begin(), s.text.end());
    cursor_ = stop + (int)s.text.size();
    sel_start_ = sel_end_ = -1;
    recompute_derived();
    if (options_.on_change) options_.on_change(text_, options_.context);
}
void TextInputImpl::submit_internal(bool hard) {
    if (text_.empty()) return;
    add_to_history(text_);
    // TS REF: src/components/PromptInput/inputModes.ts:23-29 (getValueFromInput)
    // Strip the mode-prefix char ('!' for bash) from the value passed
    // to on_submit / on_soft_submit so the engine receives clean text.
    // The raw text_ is preserved for history (add_to_history above).
    namespace figs = cc::ui::design::figures;
    std::string submit_val{figs::strip_mode_prefix(text_)};
    if (hard) {
        if (options_.on_submit) options_.on_submit(submit_val, options_.context);
    } else {
        if (options_.on_soft_submit) options_.on_soft_submit(submit_val, options_.context);
    }
    std::string submitted = std::move(text_);
    text_.clear();
    cursor_ = 0;
    sel_start_ = sel_end_ = -1;
    history_index_ = std::string::npos;
    showing_suggestions_ = false;
    (void)submitted;
    recompute_derived();
    if (options_.on_change) options_.on_change(text_, options_.context);
}

void TextInputImpl::push_undo() {
    if (!options_.enable_undo_redo) return;
    // De-dupe consecutive identical snapshots
    detail::BufferSnapshot now = snapshot();
    if (!undo_stack_.empty() && undo_stack_.back().text == now.text &&
        undo_stack_.back().cursor == now.cursor) return;
    undo_stack_.push_back(std::move(now));
    if (undo_stack_.size() > options_.max_undo_steps) undo_stack_.pop_front();
    redo_stack_.clear();
}
void TextInputImpl::load_snapshot(const detail::BufferSnapshot& s) {
    text_ = s.text;
    cursor_ = s.cursor;
    sel_start_ = s.sel_start;
    sel_end_ = s.sel_end;
}
void TextInputImpl::refresh_blink() {
    if (options_.cursor_blink_ms <= 0) {
        blink_visible_ = true;
        return;
    }
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - blink_at_).count();
    if (elapsed >= options_.cursor_blink_ms) {
        blink_visible_ = !blink_visible_;
        blink_at_ = now;
    }
}

// ------------------------------------------------------------
// Rendering helpers
// ------------------------------------------------------------
/// Apply mask character to text (for password mode).
/// Each byte position is replaced by mask_char (simplification — for
/// UTF-8 we mask each codepoint visually with a single mask_char,
/// but since we store by byte index we use a simpler per-byte mask
/// for rendering consistency with cursor position).
std::string TextInputImpl::mask_text(const std::string& s) const {
    if (!options_.mask_input) return s;
    // Count glyphs (codepoints) and produce mask chars.
    // Simplified: one mask char per UTF-8 codepoint start byte.
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (!detail::is_utf8_continuation(c)) {
            result.push_back(options_.mask_char);
        }
    }
    return result;
}

// TS REF: src/components/PromptInput/useMaybeTruncateInput.ts — the hook
//   that watches the entire input value and truncates it when it exceeds
//   TRUNCATION_THRESHOLD (10 000 chars), regardless of how it got there
//   (paste, set_text, accumulated typing, etc.).
//
//   Only applies once per "input session" (has_applied_truncation_ guard).
//   The guard is reset when the input is cleared (submit / clear),
//   matching the TS useEffect that resets hasAppliedTruncationToInput
//   when input === ''.
//
//   Returns true if truncation was applied (text_ was modified).
//   On truncation: stores the elided middle content via on_paste_truncated
//   so the caller can expand [...Truncated text #N] refs at submit time
//   (expand_pasted_text_refs).
bool TextInputImpl::maybe_apply_input_truncation() {
    if (has_applied_truncation_) return false;
    constexpr std::size_t kTruncationThreshold = 10000;
    if (text_.size() <= kTruncationThreshold) return false;

    // Use the shared truncation utility (same logic as paste_text path)
    const int paste_id = next_paste_id_++;
    const auto result = cc::utils::maybe_truncate_paste(text_, paste_id);
    if (result.placeholder_content.empty()) return false;  // no truncation

    // Replace text with the truncated version (head 500 + ref + tail 500)
    text_ = result.truncated_text;
    cursor_ = static_cast<int>(text_.size());
    sel_start_ = sel_end_ = -1;

    // Store the elided content for later expansion (expand_pasted_text_refs).
    // TS REF: useMaybeTruncateInput.ts L34-41 — setPastedContents stores
    //   {id, type: 'text', content: placeholderContent}.
    if (options_.on_paste_truncated) {
        options_.on_paste_truncated(paste_id, result.placeholder_content);
    }

    has_applied_truncation_ = true;
    return true;
}

void TextInputImpl::recompute_derived() {
    // TS REF: src/components/PromptInput/useMaybeTruncateInput.ts L24-50
    //   General safety net: if the total input exceeds 10k chars (regardless
    //   of how — paste, set_text, accumulated edits), truncate to head 500
    //   + placeholder ref + tail 500, store elided content for submit-time
    //   expansion.  Only applies once per input session (guard resets on
    //   clear).  Callers that reach `after_change` fire on_change with the
    //   already-truncated text; external set_text() callers can read back
    //   via text().
    (void)maybe_apply_input_truncation();

    // TS REF: useMaybeTruncateInput.ts L53-57 — reset the truncation guard
    //   when input is cleared (e.g. after submit), so the next session can
    //   be truncated independently.
    if (text_.empty()) {
        has_applied_truncation_ = false;
    }

    PromptContext& ctx = options_.context;
    ctx.char_count = text_.size();
    ctx.line_count = count_lines();
    // Crude token estimate: ~4 chars = 1 token. Avoid float where possible.
    ctx.input_tokens_estimate = (ctx.char_count + 2) / 4;
    // Update prompt mode from first character(s).
    // TS REF: src/components/PromptInput/inputModes.ts:16-21 (getModeFromInput)
    //
    // IMPORTANT: We do NOT strip the leading '!' from text_ here.  Per TS
    // semantics, the raw input buffer keeps the mode-prefix char so that
    // (a) standalone TextInput usage (tests, dialogs) sees the exact text
    // the user typed, and (b) history round-trips correctly via
    // prependModeCharacterToInput.  The "!" is stripped only at VALUE
    // extraction time (figures::strip_mode_prefix in submit paths and
    // REPL on_submit handlers) — TS REF: inputModes.ts:23-29
    // (getValueFromInput).
    //
    // The prompt_mode flag is still set here so that callers who read
    // ctx.prompt_mode (e.g. add_to_history prepend guard) know the
    // user's intent even when text_ was set externally (set_text from
    // REPL state).  The visual prefix glyph is rendered by the caller
    // (repl_screen via effective_is_bash + figures::kBashGlyph), NOT
    // by hiding text_[0] here.
    if (!text_.empty()) {
        namespace figs = cc::ui::design::figures;
        // TS REF: src/components/PromptInput/inputModes.ts:16-21 (getModeFromInput)
        // Use canonical figures::get_mode_from_input for bash detection (the
        // only mode that changes the prompt-prefix glyph per TS).  All other
        // prefix-triggered modes (/ @ * &) remain in the switch below since
        // they map to the extended CPP PromptInputMode enum values that don't
        // exist in the narrow TS PromptMode enum.
        if (figs::get_mode_from_input(text_) == figs::PromptMode::kBash) {
            ctx.prompt_mode = PromptInputMode::Bash;
            // TS REF: src/components/PromptInput/PromptInput.tsx:874
            //   onModeChange('bash') — caller (repl_screen) reads
            //   ctx.prompt_mode via effective_is_bash() to pick the
            //   kBashGlyph "!" prefix with bashBorder color.
        } else {
            switch (text_[0]) {
                case '/': ctx.prompt_mode = PromptInputMode::SlashCommand; break;
                case '@': ctx.prompt_mode = PromptInputMode::FileRef; break;
                case '*': ctx.prompt_mode = PromptInputMode::Agent; break;
                case '&': ctx.prompt_mode = PromptInputMode::BgRun; break;
                default:  ctx.prompt_mode = PromptInputMode::Normal; break;
            }
        }
    } else {
        ctx.prompt_mode = PromptInputMode::Normal;
    }
}

}  // namespace ui::components
