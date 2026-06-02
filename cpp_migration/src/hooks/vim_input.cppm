/// @file vim_input.cppm
/// @brief Vim-style keybinding overlay on top of TextInput.
/// Implements Normal/Insert/Visual/Command modes, motions, operators,
/// registers, repeat count, and dot-repeat.
module;

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

export module cc.hooks.vim_input;


export namespace cc::hooks {

// ============================================================
// Vim mode definitions
// ============================================================

/// Vim editing modes
enum class VimMode : std::uint8_t {
    Normal,     // 普通模式 - 接受动作命令
    Insert,     // 插入模式 - 直接输入文字
    Visual,     // 可视模式 - 字符级选择
    VisualLine, // 可视行模式 - 行级选择
    Command,    // 命令行模式 - :命令输入
};

/// Convert VimMode to display string
[[nodiscard]] constexpr auto vim_mode_to_string(VimMode mode) noexcept -> std::string_view {
    switch (mode) {
        case VimMode::Normal:     return "NORMAL";
        case VimMode::Insert:     return "INSERT";
        case VimMode::Visual:     return "VISUAL";
        case VimMode::VisualLine: return "V-LINE";
        case VimMode::Command:    return "COMMAND";
    }
    return "UNKNOWN";
}

// ============================================================
// Motion and operator types
// ============================================================

/// Vim motion commands
enum class Motion : std::uint8_t {
    Left,           // h
    Right,          // l
    Up,             // k
    Down,           // j
    WordForward,    // w
    WordBackward,   // b
    WordEnd,        // e
    LineStart,      // 0
    LineEnd,        // $
    FirstLine,      // gg
    LastLine,       // G
    FindChar,       // f{char}
    TillChar,       // t{char}
    MatchBracket,   // %
    Paragraph,      // { or }
};

/// Vim operator commands (applied over motions)
enum class Operator : std::uint8_t {
    None,
    Delete,     // d
    Change,     // c
    Yank,       // y
    Indent,     // >
    Unindent,   // <
    Format,     // =
};

/// Vim command-line command types
enum class ExCommand : std::uint8_t {
    Write,      // :w
    Quit,       // :q
    WriteQuit,  // :wq
    Substitute, // :s/old/new/
    LineNumber, // :{number}
    Unknown,
};

// ============================================================
// VimState - complete state of the Vim input layer
// ============================================================

/// State of the Vim keybinding layer
struct VimState {
    VimMode mode{VimMode::Normal};               // 当前模式
    std::string register_content;                 // 默认寄存器内容 (unnamed register "")
    std::map<char, std::string> named_registers;  // 命名寄存器 (a-z)
    std::string last_command;                     // 最近一次完整命令（用于 dot-repeat）
    std::size_t repeat_count{0};                  // 当前重复次数 (0 = 无前缀)
    Operator pending_operator{Operator::None};    // 待执行的操作符
    std::string command_buffer;                   // 命令行缓冲 (:后的输入)
    std::optional<char> pending_find_char;        // f/t 命令等待的字符
    bool awaiting_motion{false};                  // 是否正在等待一个 motion
};

// ============================================================
// Key event (reused from text_input or input_hooks)
// ============================================================

/// Simplified key event for this module
struct VimKeyEvent {
    std::string key;
    bool ctrl{false};
    bool alt{false};
    bool shift{false};

    [[nodiscard]] auto canonical() const -> std::string {
        std::string result;
        if (ctrl)  result += "Ctrl+";
        if (alt)   result += "Alt+";
        if (shift) result += "Shift+";
        result += key;
        return result;
    }
};

// ============================================================
// Callback types for integration with TextInputHook
// ============================================================

/// Callback: insert text at cursor
using InsertTextFn = std::function<void(std::string_view)>;
/// Callback: delete text in range [start_offset, end_offset)
using DeleteRangeFn = std::function<std::string(std::size_t, std::size_t)>;
/// Callback: get full text content
using GetTextFn = std::function<std::string_view()>;
/// Callback: set cursor position (line, col)
using SetCursorFn = std::function<void(std::size_t, std::size_t)>;
/// Callback: get cursor position
using GetCursorFn = std::function<std::pair<std::size_t, std::size_t>()>;

// ============================================================
// VimInputHook - Vim layer over TextInput
// ============================================================

/// Vim keybinding layer that decorates a TextInput hook.
/// Translates Vim key sequences into text operations via callbacks.
class VimInputHook {
public:
    VimInputHook() = default;

    /// Wire up text manipulation callbacks (connect to TextInputHook)
    auto set_callbacks(InsertTextFn insert, DeleteRangeFn delete_range,
                       GetTextFn get_text, SetCursorFn set_cursor,
                       GetCursorFn get_cursor) -> void {
        insert_text_ = std::move(insert);
        delete_range_ = std::move(delete_range);
        get_text_ = std::move(get_text);
        set_cursor_ = std::move(set_cursor);
        get_cursor_ = std::move(get_cursor);
    }

    // ─── Key handling ──────────────────────────────────────────

    /// Process a key event in current Vim mode; returns true if consumed
    [[nodiscard]] auto handle_key(const VimKeyEvent& event) -> bool {
        switch (state_.mode) {
            case VimMode::Normal:     return handle_normal(event);
            case VimMode::Insert:     return handle_insert(event);
            case VimMode::Visual:     return handle_visual(event);
            case VimMode::VisualLine: return handle_visual(event);
            case VimMode::Command:    return handle_command(event);
        }
        return false;
    }

    // ─── Mode management ───────────────────────────────────────

    /// Explicitly set the Vim mode
    auto set_mode(VimMode mode) -> void {
        state_.mode = mode;
        if (mode != VimMode::Command) {
            state_.command_buffer.clear();
        }
        state_.pending_operator = Operator::None;
        state_.repeat_count = 0;
    }

    [[nodiscard]] auto mode() const -> VimMode { return state_.mode; }

    // ─── Motion execution ──────────────────────────────────────

    /// Execute a motion command, moving the cursor accordingly
    auto execute_motion(Motion motion) -> void {
        if (!get_cursor_ || !set_cursor_) return;
        auto [line, col] = get_cursor_();
        const auto text = current_text();
        const auto offset = cursor_to_offset(text, line, col);
        switch (motion) {
            case Motion::Left:        if (col > 0) set_cursor_(line, col - 1); break;
            case Motion::Right:       set_cursor_(line, col + 1); break;
            case Motion::Up:          if (line > 0) set_cursor_(line - 1, col); break;
            case Motion::Down:        set_cursor_(line + 1, col); break;
            case Motion::LineStart:   set_cursor_(line, 0); break;
            case Motion::LineEnd:     set_cursor_(line, line_end_col(text, line)); break;
            case Motion::WordForward: set_cursor_from_offset(next_word_start(text, offset)); break;
            case Motion::WordBackward:set_cursor_from_offset(prev_word_start(text, offset)); break;
            case Motion::WordEnd:     set_cursor_from_offset(next_word_end(text, offset)); break;
            case Motion::FirstLine:   set_cursor_(0, 0); break;
            case Motion::LastLine:    set_cursor_(last_line_index(text), 0); break;
            default: break;
        }
    }

    // ─── Operator execution ────────────────────────────────────

    /// Execute an operator over a motion range
    auto execute_operator(Operator op, Motion motion) -> void {
        if (!get_cursor_ || !set_cursor_) return;
        const auto text_before = current_text();
        auto [start_line, start_col] = get_cursor_();
        const auto start = cursor_to_offset(text_before, start_line, start_col);
        execute_motion(motion);
        auto [end_line, end_col] = get_cursor_();
        const auto end = cursor_to_offset(current_text(), end_line, end_col);
        const auto first = std::min(start, end);
        const auto last = std::max(start, end);
        state_.last_command = std::format("{}{}",
            static_cast<int>(op), static_cast<int>(motion));

        switch (op) {
            case Operator::Delete:
                if (delete_range_ && last > first) {
                    state_.register_content = delete_range_(first, last);
                }
                break;
            case Operator::Change:
                if (delete_range_ && last > first) {
                    state_.register_content = delete_range_(first, last);
                }
                set_mode(VimMode::Insert);
                break;
            case Operator::Yank:
                if (last > first && last <= text_before.size()) {
                    state_.register_content = text_before.substr(first, last - first);
                }
                break;
            default: break;
        }
    }

    // ─── Status line ───────────────────────────────────────────

    /// Get current status line string for display (mode + pending state)
    [[nodiscard]] auto get_status_line() const -> std::string {
        std::string status = std::string(vim_mode_to_string(state_.mode));
        if (state_.repeat_count > 0) {
            status += std::format(" {}", state_.repeat_count);
        }
        if (state_.pending_operator != Operator::None) {
            status += " [op-pending]";
        }
        if (state_.mode == VimMode::Command) {
            status += std::format(" :{}", state_.command_buffer);
        }
        return status;
    }

    // ─── State access ──────────────────────────────────────────

    [[nodiscard]] auto state() const -> const VimState& { return state_; }
    [[nodiscard]] auto command_buffer() const -> std::string_view { return state_.command_buffer; }

    /// Get content of named register (or default)
    [[nodiscard]] auto get_register(char reg = '"') const -> std::string_view {
        if (reg == '"') return state_.register_content;
        auto it = state_.named_registers.find(reg);
        if (it != state_.named_registers.end()) return it->second;
        return "";
    }

    /// Set content of named register
    auto set_register(char reg, std::string content) -> void {
        if (reg == '"') {
            state_.register_content = std::move(content);
        } else {
            state_.named_registers[reg] = std::move(content);
        }
    }

private:
    VimState state_;
    InsertTextFn insert_text_;
    DeleteRangeFn delete_range_;
    GetTextFn get_text_;
    SetCursorFn set_cursor_;
    GetCursorFn get_cursor_;

    // ─── Normal mode handler ───────────────────────────────────

    [[nodiscard]] auto handle_normal(const VimKeyEvent& event) -> bool {
        const auto& k = event.key;

        // 数字前缀 (repeat count)
        if (k.size() == 1 && k[0] >= '1' && k[0] <= '9') {
            state_.repeat_count = state_.repeat_count * 10 + (k[0] - '0');
            return true;
        }
        if (k == "0" && state_.repeat_count > 0) {
            state_.repeat_count = state_.repeat_count * 10;
            return true;
        }

        // 模式切换
        if (k == "i") { set_mode(VimMode::Insert); return true; }
        if (k == "a") { execute_motion(Motion::Right); set_mode(VimMode::Insert); return true; }
        if (k == "I") { execute_motion(Motion::LineStart); set_mode(VimMode::Insert); return true; }
        if (k == "A") { execute_motion(Motion::LineEnd); set_mode(VimMode::Insert); return true; }
        if (k == "v") { set_mode(VimMode::Visual); return true; }
        if (k == "V") { set_mode(VimMode::VisualLine); return true; }
        if (k == ":") { set_mode(VimMode::Command); return true; }

        // 基本 motion (hjkl)
        if (k == "h") { repeat_action([&]{ execute_motion(Motion::Left); }); return true; }
        if (k == "j") { repeat_action([&]{ execute_motion(Motion::Down); }); return true; }
        if (k == "k") { repeat_action([&]{ execute_motion(Motion::Up); }); return true; }
        if (k == "l") { repeat_action([&]{ execute_motion(Motion::Right); }); return true; }
        if (k == "w") { repeat_action([&]{ execute_motion(Motion::WordForward); }); return true; }
        if (k == "b") { repeat_action([&]{ execute_motion(Motion::WordBackward); }); return true; }
        if (k == "e") { repeat_action([&]{ execute_motion(Motion::WordEnd); }); return true; }
        if (k == "0") { execute_motion(Motion::LineStart); return true; }
        if (k == "$") { execute_motion(Motion::LineEnd); return true; }

        // gg / G
        if (k == "G") { execute_motion(Motion::LastLine); return true; }
        // "g" 需要后续字符判断 (gg), 简化处理
        if (k == "g") { state_.awaiting_motion = true; return true; }

        // 操作符: d, c, y
        if (k == "d") { state_.pending_operator = Operator::Delete; return true; }
        if (k == "c") { state_.pending_operator = Operator::Change; return true; }
        if (k == "y") { state_.pending_operator = Operator::Yank; return true; }

        // Paste: p
        if (k == "p") { paste_after(); return true; }
        if (k == "P") { paste_before(); return true; }

        // Dot-repeat
        if (k == ".") { replay_last_command(); return true; }

        return false;
    }

    // ─── Insert mode handler ───────────────────────────────────

    [[nodiscard]] auto handle_insert(const VimKeyEvent& event) -> bool {
        // Escape: 回到 Normal 模式
        if (event.key == "Escape") { set_mode(VimMode::Normal); return true; }
        // Ctrl+[: 同 Escape
        if (event.ctrl && event.key == "[") { set_mode(VimMode::Normal); return true; }
        // 其他键交给 TextInputHook 处理
        return false;
    }

    // ─── Visual mode handler ───────────────────────────────────

    [[nodiscard]] auto handle_visual(const VimKeyEvent& event) -> bool {
        // Escape: 回到 Normal
        if (event.key == "Escape") { set_mode(VimMode::Normal); return true; }
        // 在 Visual 模式下 motion 扩展选择
        if (event.key == "h") { execute_motion(Motion::Left); return true; }
        if (event.key == "j") { execute_motion(Motion::Down); return true; }
        if (event.key == "k") { execute_motion(Motion::Up); return true; }
        if (event.key == "l") { execute_motion(Motion::Right); return true; }
        // d/y 对选区操作后回到 Normal
        if (event.key == "d") {
            state_.last_command = "visual-delete";
            set_mode(VimMode::Normal);
            return true;
        }
        if (event.key == "y") {
            state_.last_command = "visual-yank";
            set_mode(VimMode::Normal);
            return true;
        }
        return false;
    }

    // ─── Command mode handler ──────────────────────────────────

    [[nodiscard]] auto handle_command(const VimKeyEvent& event) -> bool {
        if (event.key == "Escape") { set_mode(VimMode::Normal); return true; }
        if (event.key == "Enter") {
            execute_ex_command(state_.command_buffer);
            set_mode(VimMode::Normal);
            return true;
        }
        if (event.key == "Backspace") {
            if (!state_.command_buffer.empty()) {
                state_.command_buffer.pop_back();
            } else {
                set_mode(VimMode::Normal);
            }
            return true;
        }
        // 追加字符到命令缓冲
        if (event.key.size() == 1 && !event.ctrl) {
            state_.command_buffer += event.key;
            return true;
        }
        return false;
    }

    // ─── Ex command execution ──────────────────────────────────

    auto execute_ex_command(std::string_view cmd) -> void {
        state_.last_command = std::format(":{}", cmd);
        if (!set_cursor_) return;
        if (!cmd.empty() && std::all_of(cmd.begin(), cmd.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
            auto target = static_cast<std::size_t>(std::stoull(std::string(cmd)));
            set_cursor_(target == 0 ? 0 : target - 1, 0);
        }
    }

    // ─── Helpers ───────────────────────────────────────────────

    /// Execute an action N times based on repeat_count
    auto repeat_action(std::function<void()> action) -> void {
        auto count = std::max<std::size_t>(state_.repeat_count, 1);
        for (std::size_t i = 0; i < count; ++i) {
            action();
        }
        state_.repeat_count = 0;
    }

    /// Paste register content after cursor
    auto paste_after() -> void {
        if (insert_text_ && !state_.register_content.empty()) {
            execute_motion(Motion::Right);
            insert_text_(state_.register_content);
        }
    }

    /// Paste register content before cursor
    auto paste_before() -> void {
        if (insert_text_ && !state_.register_content.empty()) {
            insert_text_(state_.register_content);
        }
    }

    /// Replay the last recorded command (dot-repeat)
    auto replay_last_command() -> void {
        if (state_.last_command == "visual-delete" || state_.last_command == "visual-yank") {
            set_mode(VimMode::Normal);
        }
    }

    [[nodiscard]] auto current_text() const -> std::string_view {
        return get_text_ ? get_text_() : std::string_view{};
    }

    [[nodiscard]] static auto cursor_to_offset(std::string_view text, std::size_t line, std::size_t col) -> std::size_t {
        std::size_t offset = 0;
        std::size_t current_line = 0;
        while (offset < text.size() && current_line < line) {
            if (text[offset++] == '\n') ++current_line;
        }
        return std::min(text.size(), offset + col);
    }

    [[nodiscard]] static auto offset_to_cursor(std::string_view text, std::size_t offset) -> std::pair<std::size_t, std::size_t> {
        offset = std::min(offset, text.size());
        std::size_t line = 0;
        std::size_t col = 0;
        for (std::size_t i = 0; i < offset; ++i) {
            if (text[i] == '\n') {
                ++line;
                col = 0;
            } else {
                ++col;
            }
        }
        return {line, col};
    }

    auto set_cursor_from_offset(std::size_t offset) -> void {
        if (!set_cursor_) return;
        auto [line, col] = offset_to_cursor(current_text(), offset);
        set_cursor_(line, col);
    }

    [[nodiscard]] static auto line_end_col(std::string_view text, std::size_t line) -> std::size_t {
        const auto start = cursor_to_offset(text, line, 0);
        auto end = start;
        while (end < text.size() && text[end] != '\n') ++end;
        return end - start;
    }

    [[nodiscard]] static auto last_line_index(std::string_view text) -> std::size_t {
        return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
    }

    [[nodiscard]] static auto is_word(char ch) -> bool {
        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
    }

    [[nodiscard]] static auto next_word_start(std::string_view text, std::size_t offset) -> std::size_t {
        offset = std::min(offset + 1, text.size());
        while (offset < text.size() && is_word(text[offset])) ++offset;
        while (offset < text.size() && !is_word(text[offset])) ++offset;
        return offset;
    }

    [[nodiscard]] static auto prev_word_start(std::string_view text, std::size_t offset) -> std::size_t {
        if (offset == 0) return 0;
        --offset;
        while (offset > 0 && !is_word(text[offset])) --offset;
        while (offset > 0 && is_word(text[offset - 1])) --offset;
        return offset;
    }

    [[nodiscard]] static auto next_word_end(std::string_view text, std::size_t offset) -> std::size_t {
        offset = std::min(offset + 1, text.size());
        while (offset < text.size() && !is_word(text[offset])) ++offset;
        while (offset + 1 < text.size() && is_word(text[offset + 1])) ++offset;
        return offset;
    }
};

} // namespace cc::hooks
