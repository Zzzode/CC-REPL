/// @file text_input.cppm
/// @brief Full-featured text input component with multi-line editing,
/// selection, undo/redo stack, cursor blink, line numbers,
/// IME-ready glyph handling, and clipboard paste.
/// Migrated from PromptInput/PromptInput.tsx + inputPaste.ts.
///
/// RFC 0001 Phase C batch 8: the editor bodies live in four module
/// implementation units — text_input_buffer.cpp (editing/history/paste
/// core), text_input_events.cpp (readline event dispatch + search),
/// text_input_vim.cpp (vim dispatch + operators), text_input_render.cpp
/// (FTXUI rendering + the TextInput() factory). This primary keeps the
/// complete TextInputImpl class/state, the option/context structs, the
/// inline trivial accessors, and the exported detail/suggest_util helpers
/// (the glyph helpers are declared here and defined in the buffer unit).
module;

#include <cstdint>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <cstddef>

export module cc.ui.widgets.text_input;

import std;

import cc.ui.foundation.ui_types;  // unified PromptInputMode canonical enum
import cc.ui.prompt.prompt_paste_handler;  // PastePreview struct (GAP 1)
import cc.vim.vim_controller;  // unified VimController state container

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

/// Canonical prompt input mode — unified definition lives in
/// cc::ui::common::PromptInputMode (ui_types.cppm).  Previously this
/// file defined its own PromptMode enum with 7 values; the unified
/// enum covers all 16 values across the codebase.  Old→new mapping:
///   PromptMode::Normal  → PromptInputMode::Normal
///   PromptMode::Bash    → PromptInputMode::Bash
///   PromptMode::Command → PromptInputMode::SlashCommand
///   PromptMode::FileRef → PromptInputMode::FileRef
///   PromptMode::Agent   → PromptInputMode::Agent
///   PromptMode::Search  → PromptInputMode::Search
///   PromptMode::BgRun   → PromptInputMode::BgRun
using cc::ui::common::PromptInputMode;

/// Data passed down to the footer & suggestion layer
struct PromptContext {
    // --- mode indicators ---
    PromptInputMode prompt_mode = PromptInputMode::Normal;
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
    // TS REF: src/components/PromptInput/PromptInputModeIndicator.tsx:54 —
    // the default prompt prefix is `figures.pointer` ('❯' U+276F) + space, NOT
    // the CPP-only "▶ " (U+25B6) invention.  See the glyph-unification note at
    // the render site (~line 1033) and the shared constant
    // cc::ui::design::figures::kPointerPrefix.  The faithful REPL path in
    // repl_screen.cppm overrides this per-mode, but standalone TextInputImpl
    // callers (dialogs, widgets) inherit this default, so it MUST match TS.
    std::string prefix = "❯ ";
    bool multiline = true;
    bool show_line_numbers = true;
    /// Vim mode: nullopt = vim disabled (standard readline bindings),
    /// otherwise vim is active in the given mode (Normal, Insert, Visual, etc.).
    /// TS REF: src/types/textInputTypes.ts:222 — VimMode = 'INSERT'|'NORMAL'.
    /// TS REF: src/hooks/useVimInput.ts:36 — mode starts at 'INSERT' when enabled.
    /// Replaces the previous bool enable_vim flag.
    std::optional<cc::ui::common::VimMode> vim_mode;
    bool show_history = true;
    bool enable_undo_redo = true;
    /// Interval between cursor blink toggles in milliseconds. 0 = no blink.
    int cursor_blink_ms = 530;
    size_t max_history = 1000;
    size_t max_undo_steps = 200;
    std::vector<Suggestion> builtin_commands;
    PromptContext context;

    /// When true, each character is replaced by mask_char (password mode)
    bool mask_input = false;
    /// Character used for masking when mask_input is true
    char mask_char = '*';
    /// Inline ghost text (auto-completion preview shown at cursor)
    std::string inline_ghost_text;
    /// Optional argument hint shown after a slash command
    std::string argument_hint;
    /// Explicit color for the prefix glyph.  Empty = use the default
    /// (historically Color::Green + bold — see below).  Set from the caller
    /// when the prefix needs to change per mode, e.g. bash mode renders the
    /// prefix with the TS `bashBorder` accent instead of theme.text.
    std::optional<ftxui::Color> prefix_color;
    /// When set, overrides the default `bold` applied to the prefix.
    /// Default = true (kept for back-compat with callers that don't set
    /// prefix_color).  The REPL faithful path sets this false because TS
    /// renders the prefix glyph at normal weight; bold mapping causes some
    /// terminals to swap pure white for bright-green/cyan.
    bool prefix_bold = true;

    /// Whether the terminal itself has focus (not just the widget).
    /// TS REF: src/hooks/renderPlaceholder.ts terminalFocus prop.
    /// Controls whether the first-character cursor inversion is applied.
    /// FTXUI cannot detect terminal focus natively, so this defaults true.
    bool terminal_focus = true;

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
    /// Optional: called when a large paste (>10K chars) is confirmed and
    /// truncated.  Receives the paste-id and the truncated middle content
    /// so the caller can store it for later expansion (e.g. expand_pasted_text_refs
    /// at submit time).  TS REF: inputPaste.ts maybeTruncateInput — stores
    /// {id, type: 'text', content: placeholderContent} in pastedContents.
    std::function<void(int id, const std::string& placeholder_content)> on_paste_truncated;
    /// Optional: character-level input filter. Return true to allow the char.
    /// Applied before insertion; multi-byte UTF-8 sequences pass the first byte.
    std::function<bool(char)> input_filter;
    /// Optional: called when the user presses Shift+Tab to cycle permission
    /// modes.  When set, TabReverse (shift+tab) is consumed by this callback
    /// instead of navigating history.  TS REF:
    /// src/components/PromptInput/PromptInput.tsx:1667 — 'chat:cycleMode'
    /// shortcut bound to shift+tab calls handleCycleMode().
    std::function<void()> on_permission_cycle;

    /// Maximum number of visible suggestions in the dropdown.
    size_t max_visible_suggestions = 8;
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
///
/// RFC 0001 Phase C batch 8: declarations stay exported here; the bodies
/// moved to text_input_buffer.cpp (the render unit calls
/// is_utf8_continuation).
bool is_utf8_continuation(unsigned char c);
int glyph_forward(const std::string& s, int byte_pos);
int glyph_back(const std::string& s, int byte_pos);

} // namespace detail

// ============================================================
// Suggestion helper utilities
// ============================================================
namespace suggest_util {

/// Get the display icon for a suggestion category.
inline const char* category_icon(SuggestionCategory cat) {
    switch (cat) {
        case SuggestionCategory::Command:      return "⚡";
        case SuggestionCategory::File:         return "📄";
        case SuggestionCategory::Directory:    return "📁";
        case SuggestionCategory::History:      return "🕘";
        case SuggestionCategory::Agent:        return "🤖";
        case SuggestionCategory::MCPResource:  return "🔌";
        case SuggestionCategory::Shell:        return "$ ";
        case SuggestionCategory::SlackChannel: return "#";
        case SuggestionCategory::CustomTitle:  return "◆ ";
        case SuggestionCategory::None:         return "";
        default:                               return "";
    }
}

/// Get the accent color for a suggestion category.
inline Color category_color(SuggestionCategory cat) {
    switch (cat) {
        case SuggestionCategory::Command:      return Color::CyanLight;
        case SuggestionCategory::File:         return Color::GreenLight;
        case SuggestionCategory::Directory:    return Color::YellowLight;
        case SuggestionCategory::History:      return Color::GrayLight;
        case SuggestionCategory::Agent:        return Color::MagentaLight;
        case SuggestionCategory::MCPResource:  return Color::BlueLight;
        case SuggestionCategory::Shell:        return Color::Green;
        case SuggestionCategory::SlackChannel: return Color::RedLight;
        case SuggestionCategory::CustomTitle:  return Color::Blue;
        case SuggestionCategory::None:         return Color::White;
        default:                               return Color::White;
    }
}

} // namespace suggest_util

// ============================================================
// TextInputImpl — Extended editor core
// ============================================================
/// Internal: the non-Component core of the editor. Separated so that
/// parent components can drive it without triggering extra renders.
class TextInputImpl {
public:
    explicit TextInputImpl(const TextInputOptions& options);

    // ------------------------------------------------------------
    // Text / state accessors
    // ------------------------------------------------------------
    void set_text(const std::string& t);
    const std::string& text() const { return text_; }
    int cursor() const { return cursor_; }

    // ------------------------------------------------------------
    // Cursor display position (for declared cursor / IME support)
    // ------------------------------------------------------------
    /// Zero-indexed line number of the cursor within the buffer.
    [[nodiscard]] int cursor_line() const {
        int line = 0, col = 0;
        compute_cursor_position(line, col);
        return line;
    }

    /// Display column (visual width) of the cursor on its line.
    /// Unlike `compute_cursor_position` which returns byte offset, this
    /// accounts for full-width CJK characters that occupy 2 terminal cells.
    [[nodiscard]] int cursor_display_col() const;
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
    // Vim mode accessors
    // TS REF: src/hooks/useVimInput.ts:310-315 — VimInputState exposes
    //   mode + setMode for external mode indicator display.
    // ------------------------------------------------------------
    /// Current vim mode (only meaningful when options_.vim_mode is set).
    [[nodiscard]] cc::ui::common::VimMode vim_mode() const {
        return vim_.mode;
    }
    /// Explicitly set the vim mode (e.g. from /vim command).
    void set_vim_mode(cc::ui::common::VimMode m) {
        vim_.mode = m;
        vim_.pending_operator.clear();
    }
    /// Returns true when vim mode is enabled and currently in a navigation mode
    /// (Normal, Visual, VisualLine, VisualBlock, Command).
    [[nodiscard]] bool vim_is_navigation() const {
        return options_.vim_mode.has_value() &&
               cc::ui::common::is_navigation_mode(vim_.mode);
    }
    /// Returns the yank register content (for status display / debugging).
    [[nodiscard]] const std::string& vim_register() const {
        return vim_.unnamed_register;
    }
    /// Returns the pending operator string ("d", "y", or "").
    [[nodiscard]] const std::string& vim_pending_operator() const {
        return vim_.pending_operator;
    }

    // ------------------------------------------------------------
    // History
    // ------------------------------------------------------------
    void add_to_history(const std::string& entry);
    void clear_history() { history_.clear(); history_index_ = std::string::npos; }

    // ------------------------------------------------------------
    // Core editing primitives (public for test / external driver)
    // ------------------------------------------------------------
    void insert_at_cursor(const std::string& s);
    void insert_char(char c);
    void insert_newline();
    void backspace();
    void delete_char();
    void move_cursor(int delta, bool extend_selection);
    void move_home(bool extend);
    void move_end(bool extend);
    void move_top(bool extend) { apply_move(0, extend); }
    void move_bottom(bool extend);
    void select_all();
    void clear_selection() { sel_start_ = sel_end_ = -1; }

    void undo();
    void redo();

    /// Attempt to read from clipboard (best-effort, platform specific).
    /// Returns pasted text or "" if unavailable. Fallback: caller can
    /// provide on_paste callback that intercepts Ctrl+V directly.
    std::string read_clipboard() const {
        // Best-effort: if no callback, do NOT block. Caller should use
        // on_paste callback in real UI. Here we provide a safe stub.
        return "";
    }

    void clear();

    // ------------------------------------------------------------
    // History navigation (moves text + cursor, no re-render in here)
    // ------------------------------------------------------------
    void navigate_history_up();
    void navigate_history_down();

    // ------------------------------------------------------------
    // Suggestions
    // ------------------------------------------------------------
    const std::vector<Suggestion>& suggestions() const { return suggestions_; }
    bool suggestions_visible() const { return showing_suggestions_; }
    int selected_suggestion() const { return selected_suggestion_; }
    void update_suggestions_from_provider();
    void accept_suggestion();
    void select_previous_suggestion();
    void select_next_suggestion();
    void hide_suggestions() { showing_suggestions_ = false; }

    // ------------------------------------------------------------
    // Rendering (Element output)
    // ------------------------------------------------------------
    Element Render();

    // ------------------------------------------------------------
    // Event handling (returns true if consumed)
    // ------------------------------------------------------------
    bool HandleEvent(Event event);

    /// External paste entry point (used by async clipboard provider)
    void PasteText(const std::string& paste_content);

    // ── Paste preview (GAP 1) ────────────────────────────────────────────

    /// True when a large paste (> 10000 chars) is pending confirmation.
    [[nodiscard]] bool HasPastePreview() const { return paste_preview_.has_value(); }

    /// Confirm the pending paste preview: insert the truncated content.
    /// TS REF: inputPaste.ts — confirmed large pastes are truncated to
    ///   head 500 + placeholder + tail 500 before insertion.
    /// If the preview carries placeholder_content (truncated middle), emit
    /// it via on_paste_truncated so the caller can store it for later
    /// expansion at submit time (expand_pasted_text_refs).
    void ConfirmPaste();

    /// Cancel the pending paste preview (discard the paste content).
    void CancelPaste() {
        paste_preview_.reset();
    }

    /// Render the paste preview confirmation overlay.
    /// TS REF: inputPaste.ts — shows line count + "(large paste - press Enter
    ///   to confirm)" + a 200-char snippet of the truncated content.
    [[nodiscard]] Element RenderPastePreviewOverlay() const;

    /// Caller-side: force a blink refresh (useful on frame tick).
    void TickBlink() { refresh_blink(); }

    // ------------------------------------------------------------
    // Public render-primitive accessors (M3)
    // ------------------------------------------------------------
    // The full Render() returns input area + suggestions dropdown combined.
    // Live prompt screens (repl_screen.cppm) need to drive the CARET /
    // MULTI-LINE / SELECTION painter as a standalone primitive so they can
    // prepend a TS-style prompt glyph (figures.pointer "❯") and re-colour
    // it per input mode — without re-implementing the cursor/selection
    // layout (which is exactly the shelfware gap M3 closes).  These thin
    // wrappers expose the existing private renderers without leaking any
    // other internals.
    /// Render just the input/caret/multiline/selection area (no dropdown).
    /// Faithful to TS BaseTextInput's declared-cursor body.
    Element RenderInputAreaPub();
    /// Render a suggestions dropdown from an externally-supplied list.
    /// `selected` is clamped to [0, suggestions.size()-1]; -1 disables.
    /// Used by repl_screen to surface autocomplete_suggestions inline.
    Element RenderSuggestionsFromListPub(const std::vector<Suggestion>& sugs,
                                         int selected);

private:
    // ------------------------------------------------------------
    // Editing / cursor / buffer helpers
    // (bodies in text_input_buffer.cpp unless inline)
    // ------------------------------------------------------------
    void apply_move(int pos, bool extend);
    void move_line_vertical(int delta, bool extend);
    void compute_cursor_position(int& out_line, int& out_col) const;
    std::vector<std::string_view> split_lines() const;
    int count_lines() const;
    void delete_selection_internal();
    void paste_text(const std::string& paste);
    void insert_suggestion(const Suggestion& s);
    void submit_internal(bool hard);

    void push_undo();
    detail::BufferSnapshot snapshot() const {
        return {text_, cursor_, sel_start_, sel_end_};
    }
    void load_snapshot(const detail::BufferSnapshot& s);
    void refresh_blink();

    // ------------------------------------------------------------
    // Event helpers (bodies in text_input_events.cpp)
    // ------------------------------------------------------------
    static bool has_ctrl_modifier(const Event& e);
    static bool has_shift_modifier(const Event& e);
    static bool matches_with_shift(const Event& e, const Event& base);
    bool HandleSearchEvent(Event event);
    void refresh_search_matches();

    // ------------------------------------------------------------
    // Rendering helpers (bodies in text_input_render.cpp)
    // ------------------------------------------------------------
    /// Apply mask character to text (for password mode).
    /// Each byte position is replaced by mask_char (simplification — for
    /// UTF-8 we mask each codepoint visually with a single mask_char,
    /// but since we store by byte index we use a simpler per-byte mask
    /// for rendering consistency with cursor position).
    std::string mask_text(const std::string& s) const;
    Element RenderInputArea();
    Element RenderSuggestionsDropdown() const;
    Element RenderSearchMode() const;

    // ------------------------------------------------------------
    // Vim mode event handling (bodies in text_input_vim.cpp)
    // TS REF: src/hooks/useVimInput.ts:175-295 — handleVimInput()
    //
    // Dispatches keys based on the current vim_.mode.
    // Returns true if the event was consumed (vim handled it),
    // false to fall through to the standard readline handler.
    // ------------------------------------------------------------
    bool HandleVimEvent(Event event);

    // Helper: word motion forward (+1) or backward (-1)
    void vim_word_motion(int dir, bool extend_selection);

    // Helper: dd — delete current line, yank to register
    void vim_delete_line();

    // Helper: yy — yank current line
    void vim_yank_line();

    // Helper: e — word end motion (TS REF: motions.ts 'e' → endOfVimWord)
    void vim_word_end_motion(bool extend_selection);

    // Helper: D / C — delete from cursor to end of line (TS REF: operators.ts
    //   executeOperatorMotion('delete', '$', ...) → deletes to end of logical line)
    void vim_delete_to_end();

    // Helper: J — join current line with next (TS REF: operators.ts executeJoin)
    // Replaces the newline between lines with a single space.
    void vim_join_lines();

    // Helper: o — open new line below (TS REF: operators.ts executeOpenLine('below'))
    void vim_open_line_below();

    // Helper: O — open new line above (TS REF: operators.ts executeOpenLine('above'))
    void vim_open_line_above();

    // Helper: ~ — toggle case of char under cursor (TS REF: operators.ts
    //   executeToggleCase)
    void vim_toggle_case();

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
    bool maybe_apply_input_truncation();
    void recompute_derived();

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
    std::string search_query_;
    std::vector<std::string> search_matches_;
    size_t search_selected_;
    bool paste_burst_in_progress_;
    int next_paste_id_{1};  ///< Monotonic counter for [...Truncated text #N] refs
    // TS REF: src/components/PromptInput/useMaybeTruncateInput.ts L21-22
    //   hasAppliedTruncationToInput — guards against re-truncating the same
    //   input session.  Reset when text is cleared (submit / clear()).
    bool has_applied_truncation_{false};

    // ============================================================
    // Paste preview (GAP 1: paste-text-truncation-10k-threshold)
    // TS REF: inputPaste.ts — when paste > 10000
    //   chars, show a confirmation overlay instead of inserting directly.
    //   Enter confirms (insert truncated), Esc cancels.
    // ============================================================
    std::optional<cc::ui::prompt::PastePreview> paste_preview_;

    // ============================================================
    // Vim mode state (unified VimController from cc.vim.vim_controller)
    // TS REF: src/hooks/useVimInput.ts — vim state machine wrapping text input
    // ============================================================
    cc::vim::VimController vim_;
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
/// Body lives in text_input_render.cpp and repeats neither default.
Component TextInput(const TextInputOptions& options = {},
                    std::shared_ptr<TextInputImpl>* out_impl = nullptr);

} // namespace ui::components
