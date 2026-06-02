/// @file text_input.cppm
/// @brief TextInput hook: manages text editing state for the REPL prompt.
/// Supports cursor movement, multi-line editing, clipboard, undo/redo,
/// word-level navigation, and selection ranges.
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.hooks.text_input;


export namespace cc::hooks {

// ============================================================
// Direction and movement types
// ============================================================

/// Movement direction for cursor operations
enum class Direction : std::uint8_t { Left, Right, Up, Down };

/// Represents a cursor position in the text buffer
struct CursorPos {
    std::size_t line{0};
    std::size_t col{0};

    auto operator<=>(const CursorPos&) const = default;
};

/// Selection range within the text buffer
struct SelectionRange {
    CursorPos start;
    CursorPos end;

    /// Whether the selection is empty (zero-width)
    [[nodiscard]] auto empty() const -> bool { return start == end; }

    /// Normalize so start <= end
    [[nodiscard]] auto normalized() const -> SelectionRange {
        if (end < start) return {end, start};
        return *this;
    }
};

// ============================================================
// Undo/Redo history entry
// ============================================================

/// Single undo/redo history entry capturing a text state snapshot
struct TextSnapshot {
    std::string text;
    CursorPos cursor;
};

// ============================================================
// TextInputState - complete state of the text input
// ============================================================

/// Full state of the text input hook
struct TextInputState {
    std::string text;                                // 当前文本内容
    CursorPos cursor_pos;                            // 光标位置
    std::optional<CursorPos> selection_start;        // 选择起点（无选择时为 nullopt）
    std::optional<CursorPos> selection_end;          // 选择终点
    std::vector<TextSnapshot> undo_history;          // 撤销历史栈
    std::vector<TextSnapshot> redo_history;          // 重做历史栈
    bool multi_line{true};                           // 是否支持多行
};

// ============================================================
// Key event (simplified, for this hook's interface)
// ============================================================

/// Key event passed from the input system
struct KeyEvent {
    std::string key;
    bool ctrl{false};
    bool alt{false};
    bool shift{false};
    bool meta{false};

    [[nodiscard]] auto canonical() const -> std::string {
        std::string result;
        if (ctrl)  result += "Ctrl+";
        if (alt)   result += "Alt+";
        if (shift) result += "Shift+";
        if (meta)  result += "Meta+";
        result += key;
        return result;
    }
};

// ============================================================
// TextInputHook - core text editing logic
// ============================================================

/// Manages REPL prompt text editing with full editor-like capabilities.
/// Designed to be composed with VimInputHook or used standalone.
class TextInputHook {
public:
    static constexpr std::size_t kMaxUndoHistory = 100;

    explicit TextInputHook(bool multi_line = true) {
        state_.multi_line = multi_line;
    }

    // ─── Key handling ──────────────────────────────────────────

    /// Process a key event; returns true if the event was consumed
    [[nodiscard]] auto handle_key(const KeyEvent& event) -> bool {
        // Ctrl+Z: undo
        if (event.ctrl && event.key == "z") { undo(); return true; }
        // Ctrl+Y / Ctrl+Shift+Z: redo
        if (event.ctrl && (event.key == "y" || (event.shift && event.key == "z"))) {
            redo(); return true;
        }
        // Ctrl+C: copy selection
        if (event.ctrl && event.key == "c") { copy(); return true; }
        // Ctrl+V: paste
        if (event.ctrl && event.key == "v") { paste(); return true; }
        // Ctrl+X: cut selection
        if (event.ctrl && event.key == "x") { cut(); return true; }
        // Ctrl+A: select all
        if (event.ctrl && event.key == "a") { select_all(); return true; }

        // Word movement: Ctrl+Left / Ctrl+Right
        if (event.ctrl && event.key == "Left")  { move_cursor_word(Direction::Left); return true; }
        if (event.ctrl && event.key == "Right") { move_cursor_word(Direction::Right); return true; }

        // Home/End: line start/end
        if (event.key == "Home") { move_to_line_start(); return true; }
        if (event.key == "End")  { move_to_line_end(); return true; }

        // Arrow keys: cursor movement
        if (event.key == "Left")  { move_cursor(Direction::Left); return true; }
        if (event.key == "Right") { move_cursor(Direction::Right); return true; }
        if (event.key == "Up")    { move_cursor(Direction::Up); return true; }
        if (event.key == "Down")  { move_cursor(Direction::Down); return true; }

        // Backspace / Delete
        if (event.key == "Backspace") { delete_backward(); return true; }
        if (event.key == "Delete")    { delete_forward(); return true; }

        // Enter: newline (only in multi-line mode)
        if (event.key == "Enter" && state_.multi_line) {
            push_undo();
            insert_text("\n");
            return true;
        }

        // Printable character insertion
        if (event.key.size() == 1 && !event.ctrl && !event.alt && !event.meta) {
            push_undo();
            insert_text(event.key);
            return true;
        }

        return false; // 未消耗
    }

    // ─── Text manipulation ─────────────────────────────────────

    /// Insert text at current cursor position, replacing selection if active
    auto insert_text(std::string_view text) -> void {
        if (has_selection()) {
            delete_selection();
        }
        auto offset = cursor_to_offset(state_.cursor_pos);
        state_.text.insert(offset, text);
        // 移动光标到插入文本末尾
        advance_cursor(text.size());
        state_.redo_history.clear();
    }

    /// Delete the current selection, returns deleted text
    auto delete_selection() -> std::string {
        if (!has_selection()) return "";
        auto range = get_selection_range().normalized();
        auto start_off = cursor_to_offset(range.start);
        auto end_off = cursor_to_offset(range.end);
        std::string deleted = state_.text.substr(start_off, end_off - start_off);
        state_.text.erase(start_off, end_off - start_off);
        state_.cursor_pos = range.start;
        clear_selection();
        return deleted;
    }

    /// Get the currently selected text
    [[nodiscard]] auto get_selected_text() const -> std::string {
        if (!has_selection()) return "";
        auto range = get_selection_range().normalized();
        auto start_off = cursor_to_offset(range.start);
        auto end_off = cursor_to_offset(range.end);
        return state_.text.substr(start_off, end_off - start_off);
    }

    // ─── Cursor movement ───────────────────────────────────────

    /// Move cursor one step in the given direction
    auto move_cursor(Direction dir) -> void {
        clear_selection();
        auto lines = get_lines();
        switch (dir) {
            case Direction::Left:
                if (state_.cursor_pos.col > 0) {
                    state_.cursor_pos.col--;
                } else if (state_.cursor_pos.line > 0) {
                    state_.cursor_pos.line--;
                    state_.cursor_pos.col = lines[state_.cursor_pos.line].size();
                }
                break;
            case Direction::Right:
                if (state_.cursor_pos.col < lines[state_.cursor_pos.line].size()) {
                    state_.cursor_pos.col++;
                } else if (state_.cursor_pos.line + 1 < lines.size()) {
                    state_.cursor_pos.line++;
                    state_.cursor_pos.col = 0;
                }
                break;
            case Direction::Up:
                if (state_.cursor_pos.line > 0) {
                    state_.cursor_pos.line--;
                    state_.cursor_pos.col = std::min(state_.cursor_pos.col,
                                                     lines[state_.cursor_pos.line].size());
                }
                break;
            case Direction::Down:
                if (state_.cursor_pos.line + 1 < lines.size()) {
                    state_.cursor_pos.line++;
                    state_.cursor_pos.col = std::min(state_.cursor_pos.col,
                                                     lines[state_.cursor_pos.line].size());
                }
                break;
        }
    }

    /// Move cursor by word boundary in the given direction
    auto move_cursor_word(Direction dir) -> void {
        clear_selection();
        auto offset = cursor_to_offset(state_.cursor_pos);
        if (dir == Direction::Left) {
            // 跳过空白，然后跳过连续单词字符
            while (offset > 0 && is_whitespace(state_.text[offset - 1])) offset--;
            while (offset > 0 && !is_whitespace(state_.text[offset - 1])) offset--;
        } else if (dir == Direction::Right) {
            auto len = state_.text.size();
            while (offset < len && !is_whitespace(state_.text[offset])) offset++;
            while (offset < len && is_whitespace(state_.text[offset])) offset++;
        }
        state_.cursor_pos = offset_to_cursor(offset);
    }

    /// Move cursor to beginning of current line
    auto move_to_line_start() -> void {
        clear_selection();
        state_.cursor_pos.col = 0;
    }

    /// Move cursor to end of current line
    auto move_to_line_end() -> void {
        clear_selection();
        auto lines = get_lines();
        state_.cursor_pos.col = lines[state_.cursor_pos.line].size();
    }

    // ─── Undo / Redo ───────────────────────────────────────────

    /// Undo the last text change
    auto undo() -> void {
        if (state_.undo_history.empty()) return;
        // 保存当前状态到 redo
        state_.redo_history.push_back({state_.text, state_.cursor_pos});
        auto snapshot = std::move(state_.undo_history.back());
        state_.undo_history.pop_back();
        state_.text = std::move(snapshot.text);
        state_.cursor_pos = snapshot.cursor;
        clear_selection();
    }

    /// Redo a previously undone change
    auto redo() -> void {
        if (state_.redo_history.empty()) return;
        state_.undo_history.push_back({state_.text, state_.cursor_pos});
        auto snapshot = std::move(state_.redo_history.back());
        state_.redo_history.pop_back();
        state_.text = std::move(snapshot.text);
        state_.cursor_pos = snapshot.cursor;
        clear_selection();
    }

    // ─── Clipboard operations ──────────────────────────────────

    /// Copy selected text to internal clipboard
    auto copy() -> void {
        if (has_selection()) {
            clipboard_ = get_selected_text();
        }
    }

    /// Cut selected text to internal clipboard
    auto cut() -> void {
        if (has_selection()) {
            push_undo();
            clipboard_ = delete_selection();
        }
    }

    /// Paste from internal clipboard
    auto paste() -> void {
        if (!clipboard_.empty()) {
            push_undo();
            insert_text(clipboard_);
        }
    }

    // ─── Selection ─────────────────────────────────────────────

    /// Select all text
    auto select_all() -> void {
        auto lines = get_lines();
        state_.selection_start = CursorPos{0, 0};
        state_.selection_end = CursorPos{
            lines.size() - 1,
            lines.back().size()
        };
    }

    [[nodiscard]] auto has_selection() const -> bool {
        return state_.selection_start.has_value() && state_.selection_end.has_value();
    }

    // ─── State access ──────────────────────────────────────────

    /// Clear all text and reset state
    auto clear() -> void {
        push_undo();
        state_.text.clear();
        state_.cursor_pos = {0, 0};
        clear_selection();
    }

    [[nodiscard]] auto state() const -> const TextInputState& { return state_; }
    [[nodiscard]] auto text() const -> std::string_view { return state_.text; }
    [[nodiscard]] auto cursor() const -> CursorPos { return state_.cursor_pos; }
    [[nodiscard]] auto is_empty() const -> bool { return state_.text.empty(); }

    /// Set text content programmatically (resets cursor)
    auto set_text(std::string new_text) -> void {
        push_undo();
        state_.text = std::move(new_text);
        state_.cursor_pos = {0, 0};
        clear_selection();
    }

private:
    TextInputState state_;
    std::string clipboard_;

    // ─── Internal helpers ──────────────────────────────────────

    /// Push current state onto undo stack
    auto push_undo() -> void {
        state_.undo_history.push_back({state_.text, state_.cursor_pos});
        if (state_.undo_history.size() > kMaxUndoHistory) {
            state_.undo_history.erase(state_.undo_history.begin());
        }
    }

    /// Split text into lines
    [[nodiscard]] auto get_lines() const -> std::vector<std::string_view> {
        std::vector<std::string_view> lines;
        std::string_view sv = state_.text;
        std::size_t pos = 0;
        while (pos <= sv.size()) {
            auto nl = sv.find('\n', pos);
            if (nl == std::string_view::npos) {
                lines.push_back(sv.substr(pos));
                break;
            }
            lines.push_back(sv.substr(pos, nl - pos));
            pos = nl + 1;
        }
        if (lines.empty()) lines.push_back("");
        return lines;
    }

    /// Convert CursorPos to linear offset
    [[nodiscard]] auto cursor_to_offset(CursorPos pos) const -> std::size_t {
        auto lines = get_lines();
        std::size_t offset = 0;
        for (std::size_t i = 0; i < pos.line && i < lines.size(); ++i) {
            offset += lines[i].size() + 1; // +1 for newline
        }
        offset += std::min(pos.col, lines[pos.line].size());
        return std::min(offset, state_.text.size());
    }

    /// Convert linear offset to CursorPos
    [[nodiscard]] auto offset_to_cursor(std::size_t offset) const -> CursorPos {
        CursorPos pos{0, 0};
        std::size_t remaining = offset;
        auto lines = get_lines();
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (remaining <= lines[i].size()) {
                pos.line = i;
                pos.col = remaining;
                return pos;
            }
            remaining -= lines[i].size() + 1; // +1 for newline
        }
        // 超出末尾，定位到文本末尾
        pos.line = lines.size() - 1;
        pos.col = lines.back().size();
        return pos;
    }

    /// Advance cursor by N characters from current offset
    auto advance_cursor(std::size_t chars) -> void {
        auto offset = cursor_to_offset(state_.cursor_pos) + chars;
        state_.cursor_pos = offset_to_cursor(std::min(offset, state_.text.size()));
    }

    /// Get current selection as a range
    [[nodiscard]] auto get_selection_range() const -> SelectionRange {
        return SelectionRange{
            state_.selection_start.value_or(state_.cursor_pos),
            state_.selection_end.value_or(state_.cursor_pos)
        };
    }

    /// Clear selection markers
    auto clear_selection() -> void {
        state_.selection_start = std::nullopt;
        state_.selection_end = std::nullopt;
    }

    /// Delete one character backward (Backspace)
    auto delete_backward() -> void {
        if (has_selection()) { push_undo(); delete_selection(); return; }
        auto offset = cursor_to_offset(state_.cursor_pos);
        if (offset == 0) return;
        push_undo();
        state_.text.erase(offset - 1, 1);
        state_.cursor_pos = offset_to_cursor(offset - 1);
    }

    /// Delete one character forward (Delete key)
    auto delete_forward() -> void {
        if (has_selection()) { push_undo(); delete_selection(); return; }
        auto offset = cursor_to_offset(state_.cursor_pos);
        if (offset >= state_.text.size()) return;
        push_undo();
        state_.text.erase(offset, 1);
    }

    /// Check if character is whitespace
    [[nodiscard]] static auto is_whitespace(char c) -> bool {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }
};

} // namespace cc::hooks
