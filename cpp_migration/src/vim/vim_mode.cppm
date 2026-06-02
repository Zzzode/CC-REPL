module;
#include <string>
#include <optional>
#include <functional>

export module cc.vim.vim_mode;

export namespace cc::vim {

// Vim editing modes
enum class VimMode { Normal, Insert, Visual, VisualLine, Command, Replace };

// State machine for vim mode transitions and key processing
class VimStateMachine {
public:
    VimStateMachine() : mode_(VimMode::Normal) {}

    // Get the current mode
    [[nodiscard]] auto get_mode() const -> VimMode { return mode_; }

    // Process a key input and return any command output
    auto process_key(char key) -> std::optional<std::string> {
        switch (mode_) {
            case VimMode::Normal:
                return process_normal(key);
            case VimMode::Insert:
                return process_insert(key);
            case VimMode::Visual:
            case VimMode::VisualLine:
                return process_visual(key);
            case VimMode::Command:
                return process_command(key);
            case VimMode::Replace:
                return process_replace(key);
        }
        return std::nullopt;
    }

    // Get the status line text for current mode
    [[nodiscard]] auto get_status_line() const -> std::string {
        switch (mode_) {
            case VimMode::Normal:     return "-- NORMAL --";
            case VimMode::Insert:     return "-- INSERT --";
            case VimMode::Visual:     return "-- VISUAL --";
            case VimMode::VisualLine: return "-- VISUAL LINE --";
            case VimMode::Command:    return ":" + command_buffer_;
            case VimMode::Replace:    return "-- REPLACE --";
        }
        return "";
    }

    // Explicitly set the mode
    auto set_mode(VimMode mode) -> void {
        mode_ = mode;
        if (mode != VimMode::Command) {
            command_buffer_.clear();
        }
    }

private:
    auto process_normal(char key) -> std::optional<std::string> {
        switch (key) {
            case 'i':
                mode_ = VimMode::Insert;
                return std::nullopt;
            case 'a':
                mode_ = VimMode::Insert;
                return "cursor_right";
            case 'v':
                mode_ = VimMode::Visual;
                return std::nullopt;
            case 'V':
                mode_ = VimMode::VisualLine;
                return std::nullopt;
            case ':':
                mode_ = VimMode::Command;
                command_buffer_.clear();
                return std::nullopt;
            case 'R':
                mode_ = VimMode::Replace;
                return std::nullopt;
            case 'h': return "cursor_left";
            case 'j': return "cursor_down";
            case 'k': return "cursor_up";
            case 'l': return "cursor_right";
            case '0': return "line_start";
            case '$': return "line_end";
            case 'w': return "word_forward";
            case 'b': return "word_backward";
            case 'e': return "word_end";
            case 'x': return "delete_char";
            case 'u': return "undo";
            case 'p': return "paste_after";
            case 'P': return "paste_before";
            case 'o':
                mode_ = VimMode::Insert;
                return "open_below";
            case 'O':
                mode_ = VimMode::Insert;
                return "open_above";
            case 'A':
                mode_ = VimMode::Insert;
                return "line_end";
            case 'I':
                mode_ = VimMode::Insert;
                return "line_start";
            default:
                // Accumulate for multi-key commands (dd, yy, etc.)
                pending_ += key;
                if (pending_ == "dd") { pending_.clear(); return "delete_line"; }
                if (pending_ == "yy") { pending_.clear(); return "yank_line"; }
                if (pending_ == "gg") { pending_.clear(); return "goto_top"; }
                if (pending_.size() > 2) pending_.clear();
                return std::nullopt;
        }
    }

    auto process_insert(char key) -> std::optional<std::string> {
        if (key == '\x1b') { // Escape
            mode_ = VimMode::Normal;
            return "cursor_left";
        }
        // In insert mode, characters are passed through as input
        return "insert:" + std::string(1, key);
    }

    auto process_visual(char key) -> std::optional<std::string> {
        if (key == '\x1b') {
            mode_ = VimMode::Normal;
            return "clear_selection";
        }
        switch (key) {
            case 'h': return "extend_left";
            case 'j': return "extend_down";
            case 'k': return "extend_up";
            case 'l': return "extend_right";
            case 'd': mode_ = VimMode::Normal; return "delete_selection";
            case 'y': mode_ = VimMode::Normal; return "yank_selection";
            default: return std::nullopt;
        }
    }

    auto process_command(char key) -> std::optional<std::string> {
        if (key == '\x1b') {
            mode_ = VimMode::Normal;
            command_buffer_.clear();
            return std::nullopt;
        }
        if (key == '\r' || key == '\n') {
            mode_ = VimMode::Normal;
            std::string cmd = command_buffer_;
            command_buffer_.clear();
            return "ex:" + cmd;
        }
        if (key == '\x7f' || key == '\x08') { // Backspace
            if (!command_buffer_.empty()) {
                command_buffer_.pop_back();
            }
            return std::nullopt;
        }
        command_buffer_ += key;
        return std::nullopt;
    }

    auto process_replace(char key) -> std::optional<std::string> {
        if (key == '\x1b') {
            mode_ = VimMode::Normal;
            return std::nullopt;
        }
        mode_ = VimMode::Normal;
        return "replace:" + std::string(1, key);
    }

    VimMode mode_;
    std::string command_buffer_;
    std::string pending_; // Multi-key command buffer
};

// Global vim mode enabled state
namespace detail {
    inline bool vim_enabled = false;
}

// Check if vim mode is currently enabled
inline auto is_vim_mode_enabled() -> bool {
    return detail::vim_enabled;
}

// Enable vim keybindings
inline auto enable_vim_mode() -> void {
    detail::vim_enabled = true;
}

// Disable vim keybindings
inline auto disable_vim_mode() -> void {
    detail::vim_enabled = false;
}

} // namespace cc::vim
