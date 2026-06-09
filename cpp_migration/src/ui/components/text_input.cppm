/// @file text_input.cppm
/// @brief Full-featured text input component with multi-line editing,
/// selection, undo/redo stack, cursor blink, line numbers,
/// IME-ready glyph handling, and clipboard paste.
/// Migrated from PromptInput/PromptInput.tsx + inputPaste.ts.
module;

#include <string>
#include <string_view>
#include <vector>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <format>
#include <cstdint>
#include <cctype>
#include <algorithm>
#include <chrono>
#include <utility>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>

export module ui.components.text_input;

export namespace ui::components {
using namespace ftxui;

// ============================================================
// Public Types
// ============================================================

/// Category of an autocomplete suggestion (exposed for external wiring)
enum class SuggestionCategory : std::uint8_t {
    Command,        // Slash commands (/help, /clear, etc.)
    File,           // File paths
    Directory,      // Directory paths
    History,        // Previous inputs
    Agent,          // Agent / team members
    MCPResource,    // MCP server resource
    Shell,          // Shell / one-shot
    SlackChannel,   // Slack channel
    CustomTitle,    // Section header / label
    None,
};

/// A single autocomplete / suggestion item
struct Suggestion {
    std::string text;            // Inserted into buffer on accept
    std::string display_text;    // Shown in dropdown
    std::string description;     // Secondary description
    SuggestionCategory category = SuggestionCategory::None;
    std::optional<std::string> tag;   // E.g. "[skill]", "[agent]"
    std::optional<std::string> icon;  // Override category icon
    std::optional<Color> color_hint;
};

/// Permission / tool permission mode (subset of TS PermissionMode)
enum class PermissionMode : std::uint8_t {
    Default,      // Confirm each tool call
    AutoApprove,  // Auto-approve safe tools
    Unlimited,    // Full autonomy
    PlanOnly,     // Plan without execution
    UltraPlan,    // Extended planning
    UltraReview,  // Review mode
};

/// Prompt-level input mode
enum class PromptMode : std::uint8_t {
    Normal,   // Regular chat
    Bash,     // Shell-first (leading '!')
    Command,  // Slash-command mode (leading '/')
    FileRef,  // File-reference (leading '@')
    Agent,    // Agent mention (leading '*')
    Search,   // History search
    BgRun,    // '&' background
};

/// Data passed down to the footer & suggestion layer
struct PromptContext {
    // --- mode indicators ---
    PromptMode prompt_mode = PromptMode::Normal;
    PermissionMode permission = PermissionMode::Default;
    std::string effort_level;        // "", "low", "medium", "high", "ultra"
    std::optional<std::string> active_agent;
    std::optional<std::string> swarm_banner;
    bool is_loading = false;
    bool tasks_selected = false;
    bool teams_selected = false;
    bool tmux_selected = false;
    bool show_sandbox_hint = false;
    bool show_mode_cycle_hint = true;
    // --- counters ---
    std::int64_t token_count = 0;      // session token usage (verbose)
    std::int64_t char_count = 0;       // derived from input length
    std::int64_t line_count = 1;
    std::int64_t input_tokens_estimate = 0; // rough token estimate for current buffer
    // --- status ---
    bool is_above_warning_threshold = false;
    bool api_key_invalid = false;
    bool api_key_missing = false;
    bool in_overage_mode = false;
    bool debug_mode = false;
    bool verbose = true;
    // --- misc ---
    std::optional<std::string> remote_session_url;
};

/// Configurable options for the Prompt TextInput component
struct TextInputOptions {
    std::string placeholder = "Type your message here...";
    std::string prefix = "▶ ";
    bool multiline = true;
    bool show_line_numbers = true;
    bool enable_vim = false;
    bool show_history = true;
    bool enable_undo_redo = true;
    /// Interval between cursor blink toggles in milliseconds. 0 = no blink.
    int cursor_blink_ms = 530;
    size_t max_history = 1000;
    size_t max_undo_steps = 200;
    std::vector<Suggestion> builtin_commands;
    PromptContext context;

    std::function<void(const std::string&, const PromptContext&)> on_submit;
    /// Called on Enter submit when Ctrl+Enter modifier is used
    std::function<void(const std::string&, const PromptContext&)> on_soft_submit;
    std::function<void(const std::string&, const PromptContext&)> on_change;
    std::function<void()> on_cancel;
    std::function<void()> on_escape;
    /// Optional: suggestion provider; default is empty (always returns {})
    std::function<std::vector<Suggestion>(const std::string&, int /*cursor_pos*/,
                                          const PromptContext&)>
        get_suggestions;
    /// Optional: external paste callback. If null falls back to a simple insert.
    std::function<void(const std::string&)> on_paste;
};

// ============================================================
// Internal: Undo / Redo snapshot
// ============================================================
namespace detail {

struct BufferSnapshot {
    std::string text;
    int cursor = 0;
    int sel_start = -1;
    int sel_end = -1;
};

/// A UTF-8 compatible string index helper (byte -> glyph offset).
/// For the renderer we keep byte-indexed buffers but must guard against
/// breaking multibyte sequences when the cursor moves. This is a minimal
/// approximation; a full ICU layer is out of scope for the migration.
inline bool is_utf8_continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}
inline int glyph_forward(const std::string& s, int byte_pos) {
    int n = static_cast<int>(s.size());
    if (byte_pos >= n) return n;
    int p = byte_pos + 1;
    while (p < n && is_utf8_continuation(static_cast<unsigned char>(s[p]))) ++p;
    return p;
}
inline int glyph_back(const std::string& s, int byte_pos) {
    int p = byte_pos - 1;
    while (p > 0 && is_utf8_continuation(static_cast<unsigned char>(s[p]))) --p;
    return std::max(0, p);
}

} // namespace detail

// ============================================================
// TextInputImpl — Extended editor core
// ============================================================
/// Internal: the non-Component core of the editor. Separated so that
/// parent components can drive it without triggering extra renders.
class TextInputImpl {
public:
    explicit TextInputImpl(const TextInputOptions& options)
        : options_(options), cursor_(0), sel_start_(-1), sel_end_(-1),
          history_index_(std::string::npos),
          showing_suggestions_(false), selected_suggestion_(0),
          last_keystroke_ms_(std::chrono::steady_clock::now()),
          blink_at_(std::chrono::steady_clock::now()),
          blink_visible_(true),
          search_mode_(false),
          paste_burst_in_progress_(false) {}

    // ------------------------------------------------------------
    // Text / state accessors
    // ------------------------------------------------------------
    void set_text(const std::string& t) {
        push_undo();
        text_ = t;
        cursor_ = static_cast<int>(t.size());
        sel_start_ = sel_end_ = -1;
        recompute_derived();
    }
    const std::string& text() const { return text_; }
    int cursor() const { return cursor_; }
    bool has_selection() const {
        return sel_start_ >= 0 && sel_end_ >= 0 && sel_start_ != sel_end_;
    }
    std::pair<int, int> selection() const {
        if (!has_selection()) return {-1, -1};
        return {std::min(sel_start_, sel_end_),
                std::max(sel_start_, sel_end_)};
    }
    int history_index() const { return static_cast<int>(history_index_); }

    PromptContext& mutable_context() { return options_.context; }
    const PromptContext& context() const { return options_.context; }

    // ------------------------------------------------------------
    // History
    // ------------------------------------------------------------
    void add_to_history(const std::string& entry) {
        if (!entry.empty()) {
            auto it = std::find(history_.begin(), history_.end(), entry);
            if (it != history_.end()) history_.erase(it);
            history_.push_back(entry);
            if (history_.size() > options_.max_history)
                history_.pop_front();
        }
        history_index_ = std::string::npos;
    }
    void clear_history() { history_.clear(); history_index_ = std::string::npos; }

    // ------------------------------------------------------------
    // Core editing primitives (public for test / external driver)
    // ------------------------------------------------------------
    void insert_at_cursor(const std::string& s) {
        if (s.empty()) return;
        push_undo();
        if (has_selection()) delete_selection_internal();
        text_.insert(text_.begin() + cursor_, s.begin(), s.end());
        cursor_ += static_cast<int>(s.size());
        sel_start_ = sel_end_ = -1;
        recompute_derived();
    }
    void insert_char(char c) {
        push_undo();
        if (has_selection()) delete_selection_internal();
        text_.insert(text_.begin() + cursor_, c);
        ++cursor_;
        sel_start_ = sel_end_ = -1;
        recompute_derived();
    }
    void insert_newline() {
        push_undo();
        if (has_selection()) delete_selection_internal();
        text_.insert(text_.begin() + cursor_, '\n');
        ++cursor_;
        sel_start_ = sel_end_ = -1;
        recompute_derived();
    }
    void backspace() {
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
    void delete_char() {
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
    void move_cursor(int delta, bool extend_selection) {
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
    void move_home(bool extend) {
        // line-aware: home = start of current line
        int n = static_cast<int>(text_.size());
        int pos = cursor_;
        while (pos > 0 && text_[pos - 1] != '\n') --pos;
        apply_move(pos, extend);
        (void)n;
    }
    void move_end(bool extend) {
        int n = static_cast<int>(text_.size());
        int pos = cursor_;
        while (pos < n && text_[pos] != '\n') ++pos;
        apply_move(pos, extend);
    }
    void move_top(bool extend) { apply_move(0, extend); }
    void move_bottom(bool extend) {
        apply_move(static_cast<int>(text_.size()), extend);
    }
    void select_all() {
        sel_start_ = 0;
        sel_end_ = cursor_ = static_cast<int>(text_.size());
    }
    void clear_selection() { sel_start_ = sel_end_ = -1; }

    void undo() {
        if (!options_.enable_undo_redo) return;
        if (undo_stack_.empty()) return;
        redo_stack_.push_back(snapshot());
        load_snapshot(undo_stack_.back());
        undo_stack_.pop_back();
        recompute_derived();
    }
    void redo() {
        if (!options_.enable_undo_redo) return;
        if (redo_stack_.empty()) return;
        undo_stack_.push_back(snapshot());
        load_snapshot(redo_stack_.back());
        redo_stack_.pop_back();
        recompute_derived();
    }

    /// Attempt to read from clipboard (best-effort, platform specific).
    /// Returns pasted text or "" if unavailable. Fallback: caller can
    /// provide on_paste callback that intercepts Ctrl+V directly.
    std::string read_clipboard() const {
        // Best-effort: if no callback, do NOT block. Caller should use
        // on_paste callback in real UI. Here we provide a safe stub.
        return "";
    }

    void clear() {
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
    void navigate_history_up() {
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
    void navigate_history_down() {
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
    const std::vector<Suggestion>& suggestions() const { return suggestions_; }
    bool suggestions_visible() const { return showing_suggestions_; }
    int selected_suggestion() const { return selected_suggestion_; }
    void update_suggestions_from_provider() {
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
    void accept_suggestion() {
        if (!showing_suggestions_ || suggestions_.empty()) return;
        insert_suggestion(suggestions_[selected_suggestion_]);
        showing_suggestions_ = false;
    }
    void select_previous_suggestion() {
        if (suggestions_.empty()) return;
        selected_suggestion_ =
            (selected_suggestion_ - 1 + (int)suggestions_.size()) %
            (int)suggestions_.size();
    }
    void select_next_suggestion() {
        if (suggestions_.empty()) return;
        selected_suggestion_ =
            (selected_suggestion_ + 1) % (int)suggestions_.size();
    }
    void hide_suggestions() { showing_suggestions_ = false; }

    // ------------------------------------------------------------
    // Rendering (Element output)
    // ------------------------------------------------------------
    Element Render() {
        using namespace ftxui;
        refresh_blink();

        Elements lines_elements;
        std::vector<std::string_view> lines = split_lines();
        int byte_offset = 0;
        const int total_lines = static_cast<int>(lines.size());
        const int sel_beg = has_selection() ? std::min(sel_start_, sel_end_) : -1;
        const int sel_fin = has_selection() ? std::max(sel_start_, sel_end_) : -1;
        int cursor_line = 0, cursor_col = 0;
        compute_cursor_position(cursor_line, cursor_col);

        for (int li = 0; li < total_lines; ++li) {
            Elements line_parts;

            // Line prefix (first line) or indent (subsequent lines)
            if (li == 0) {
                line_parts.push_back(
                    ftxui::text(options_.prefix) | color(Color::Green) | bold);
            } else {
                line_parts.push_back(ftxui::text(std::string(options_.prefix.size(), ' ')));
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
                std::string seg_text{line.substr(seg.start, seg.end - seg.start)};

                if (is_cursor_line) {
                    // Split around cursor within segment if applicable
                    int cur_rel_start = cursor_col - seg.start;
                    if (cur_rel_start >= 0 && cur_rel_start <= (int)seg_text.size() &&
                        !seg.selected) {
                        // Cursor is inside or at edge of this non-selected segment
                        std::string before = seg_text.substr(0, cur_rel_start);
                        std::string after = seg_text.substr(cur_rel_start);
                        if (!before.empty()) line_parts.push_back(ftxui::text(before));
                        Element cursor_el;
                        if (!after.empty() && blink_visible_) {
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
                            // End of line cursor — render blinking block space
                            line_parts.push_back(ftxui::text(blink_visible_ ? "█" : " ") |
                                                 color(Color::CyanLight));
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
            // Empty line placeholder
            if (line_len == 0 && is_cursor_line && !has_selection()) {
                line_parts.push_back(ftxui::text(blink_visible_ ? "█" : " ") |
                                     color(Color::CyanLight));
            }

            lines_elements.push_back(hbox(line_parts));
            byte_offset += line_len + 1; // +1 for the '\n'
        }

        // --- Placeholder rendering for empty, unfocused ---
        if (lines_elements.size() == 1 && lines[0].empty()) {
            Elements ph_parts;
            ph_parts.push_back(ftxui::text(options_.prefix) | color(Color::Green) | bold);
            ph_parts.push_back(ftxui::text(options_.placeholder) | dim);
            return hbox(ph_parts);
        }

        return vbox(lines_elements);
    }

    // ------------------------------------------------------------
    // Event handling (returns true if consumed)
    // ------------------------------------------------------------
    bool HandleEvent(Event event) {
        using namespace ftxui;
        last_keystroke_ms_ = std::chrono::steady_clock::now();
        blink_visible_ = true;     // reset on user activity
        blink_at_ = last_keystroke_ms_;
        bool changed = false;

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
            // Tab without suggestions — cycle history
            if (event == Event::Tab) {
                navigate_history_up();
                return true;
            }
            if (event == Event::TabReverse) {
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

    /// External paste entry point (used by async clipboard provider)
    void PasteText(const std::string& paste_content) {
        paste_text(paste_content);
        update_suggestions_from_provider();
        recompute_derived();
        if (options_.on_change) options_.on_change(text_, options_.context);
    }

    /// Caller-side: force a blink refresh (useful on frame tick).
    void TickBlink() { refresh_blink(); }

private:
    // ------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------
    void apply_move(int pos, bool extend) {
        pos = std::clamp(pos, 0, static_cast<int>(text_.size()));
        if (extend) {
            if (sel_start_ < 0) sel_start_ = cursor_;
            sel_end_ = pos;
        } else {
            sel_start_ = sel_end_ = -1;
        }
        cursor_ = pos;
    }
    void move_line_vertical(int delta, bool extend) {
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
    void compute_cursor_position(int& out_line, int& out_col) const {
        out_line = 0;
        int col = 0;
        for (int i = 0; i < cursor_; ++i) {
            if (text_[i] == '\n') { ++out_line; col = 0; }
            else { ++col; }
        }
        out_col = col;
    }
    std::vector<std::string_view> split_lines() const {
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
    int count_lines() const {
        if (text_.empty()) return 1;
        int n = 1;
        for (char c : text_) if (c == '\n') ++n;
        return n;
    }
    void delete_selection_internal() {
        auto [a, b] = selection();
        if (a < 0 || b <= a) return;
        text_.erase(text_.begin() + a, text_.begin() + b);
        cursor_ = a;
        sel_start_ = sel_end_ = -1;
    }
    void paste_text(const std::string& paste) {
        push_undo();
        if (has_selection()) delete_selection_internal();
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
        text_.insert(text_.begin() + cursor_, normalized.begin(), normalized.end());
        cursor_ += (int)normalized.size();
        sel_start_ = sel_end_ = -1;
        paste_burst_in_progress_ = true;
    }
    void insert_suggestion(const Suggestion& s) {
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
    void submit_internal(bool hard) {
        if (text_.empty()) return;
        add_to_history(text_);
        if (hard) {
            if (options_.on_submit) options_.on_submit(text_, options_.context);
        } else {
            if (options_.on_soft_submit) options_.on_soft_submit(text_, options_.context);
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

    void push_undo() {
        if (!options_.enable_undo_redo) return;
        // De-dupe consecutive identical snapshots
        detail::BufferSnapshot now = snapshot();
        if (!undo_stack_.empty() && undo_stack_.back().text == now.text &&
            undo_stack_.back().cursor == now.cursor) return;
        undo_stack_.push_back(std::move(now));
        if (undo_stack_.size() > options_.max_undo_steps) undo_stack_.pop_front();
        redo_stack_.clear();
    }
    detail::BufferSnapshot snapshot() const {
        return {text_, cursor_, sel_start_, sel_end_};
    }
    void load_snapshot(const detail::BufferSnapshot& s) {
        text_ = s.text;
        cursor_ = s.cursor;
        sel_start_ = s.sel_start;
        sel_end_ = s.sel_end;
    }
    void refresh_blink() {
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
    static bool has_ctrl_modifier(const Event& e) {
        // FTXUI Event::Return doesn't carry ctrl modifier on all terminals;
        // we approximate: the default Enter keypress is plain. The actual
        // ctrl+enter detection falls back to on_soft_submit call from host.
        return e.is_character() && e.character().size() >= 2 &&
               e.character()[0] == '\n' && e.character()[1] == '\x0d';
    }
    // Shift modifier appears as ";2" in xterm-style escape sequences:
    //   \x1B[1;2D = Shift+Left,  \x1B[1;2A = Shift+Up, etc.
    static bool has_shift_modifier(const Event& e) {
        const std::string& s = e.input();
        return s.size() >= 5 &&
               s.substr(0, 5) == "\x1B[1;" &&
               s.find(";2") != std::string::npos;
    }
    // Match Home/End / PageUp/Down shift variants:
    //   Shift+Home = \x1B[1;2H, Shift+End  = \x1B[1;2F
    //   Shift+PageUp   = \x1B[5;2~
    //   Shift+PageDown = \x1B[6;2~
    static bool matches_with_shift(const Event& e, const Event& base) {
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
    void recompute_derived() {
        PromptContext& ctx = options_.context;
        ctx.char_count = text_.size();
        ctx.line_count = count_lines();
        // Crude token estimate: ~4 chars = 1 token. Avoid float where possible.
        ctx.input_tokens_estimate = (ctx.char_count + 2) / 4;
        // Update prompt mode from first character(s)
        if (!text_.empty()) {
            switch (text_[0]) {
                case '!': ctx.prompt_mode = PromptMode::Bash; break;
                case '/': ctx.prompt_mode = PromptMode::Command; break;
                case '@': ctx.prompt_mode = PromptMode::FileRef; break;
                case '*': ctx.prompt_mode = PromptMode::Agent; break;
                case '&': ctx.prompt_mode = PromptMode::BgRun; break;
                default:  ctx.prompt_mode = PromptMode::Normal; break;
            }
        } else {
            ctx.prompt_mode = PromptMode::Normal;
        }
    }

    // ============================================================
    // Data members
    // ============================================================
    TextInputOptions options_;
    std::string text_;
    int cursor_;
    int sel_start_;
    int sel_end_;
    std::deque<std::string> history_;
    size_t history_index_;
    detail::BufferSnapshot pre_search_snapshot_;
    std::deque<detail::BufferSnapshot> undo_stack_;
    std::vector<detail::BufferSnapshot> redo_stack_;
    std::vector<Suggestion> suggestions_;
    bool showing_suggestions_;
    int selected_suggestion_;
    std::chrono::steady_clock::time_point last_keystroke_ms_;
    std::chrono::steady_clock::time_point blink_at_;
    bool blink_visible_;
    bool search_mode_;
    bool paste_burst_in_progress_;
};

// ============================================================
// FTXUI Component wrapper
// ============================================================
/// Create a Prompt-level TextInput component wrapping TextInputImpl.
/// Exposes the internal impl via shared_ptr so parent components can
/// call PasteText() / TickBlink() / access context().
inline std::shared_ptr<TextInputImpl> MakeTextInputCore(
    const TextInputOptions& options = {}) {
    return std::make_shared<TextInputImpl>(options);
}

/// Create an ftxui::Component backed by a TextInputImpl.
/// `out_impl` (optional, non-null) is populated with the internal pointer.
Component TextInput(const TextInputOptions& options = {},
                    std::shared_ptr<TextInputImpl>* out_impl = nullptr) {
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

} // namespace ui::components

