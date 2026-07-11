// C++23 Module: Prompt input component with multi-line editing, history, typeahead, and vim mode
module;

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <cstdlib>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <filesystem>
#include <ftxui/dom/elements.hpp>

export module cc.ui.prompt_input;

import cc.ui.common.types;  // canonical VimMode

export namespace cc::ui {

// Canonical VimMode — imported from cc::ui::common (ui_types.cppm).
// TS REF: src/types/textInputTypes.ts:222 (public type = 'INSERT'|'NORMAL')
// This replaces the previous local 3-value enum { Normal, Insert, Visual }
// that conflicted with the 6-value version in vim_input.cppm.
using cc::ui::common::VimMode;

// Vim motion types
enum class VimMotion { Left, Right, Up, Down, WordForward, WordBack, LineStart, LineEnd };

// Cursor position in the input buffer
struct CursorPos {
    std::size_t line{0};
    std::size_t col{0};

    auto operator<=>(const CursorPos&) const = default;
};

// Selection range for shift+arrow and visual mode
struct Selection {
    CursorPos start;
    CursorPos end;

    // Normalize so start <= end
    [[nodiscard]] auto normalized() const -> Selection {
        return (start <= end) ? Selection{start, end} : Selection{end, start};
    }

    [[nodiscard]] auto empty() const -> bool { return start == end; }
};

// InputBuffer: multi-line text editing with cursor management
class InputBuffer {
public:
    InputBuffer() : lines_{""} {}

    // Insert text at cursor position, handles newlines
    auto insert(std::string_view text) -> void {
        for (std::size_t i = 0; i < text.size();) {
            if (text[i] == '\n') {
                // Split current line at cursor
                auto& current = lines_[cursor_.line];
                std::string remainder = current.substr(cursor_.col);
                current.erase(cursor_.col);
                lines_.insert(lines_.begin() + static_cast<long>(cursor_.line + 1), remainder);
                cursor_.line++;
                cursor_.col = 0;
                ++i;
            } else {
                auto len = utf8_codepoint_length(static_cast<unsigned char>(text[i]));
                len = std::min(len, text.size() - i);
                lines_[cursor_.line].insert(cursor_.col, text.substr(i, len));
                cursor_.col += len;
                i += len;
            }
        }
    }

    // Delete character before cursor (backspace)
    auto backspace() -> void {
        if (cursor_.col > 0) {
            auto& line = lines_[cursor_.line];
            auto erase_start = previous_codepoint_start(line, cursor_.col);
            line.erase(erase_start, cursor_.col - erase_start);
            cursor_.col = erase_start;
        } else if (cursor_.line > 0) {
            // Merge with previous line
            cursor_.col = lines_[cursor_.line - 1].size();
            lines_[cursor_.line - 1] += lines_[cursor_.line];
            lines_.erase(lines_.begin() + static_cast<long>(cursor_.line));
            cursor_.line--;
        }
    }

    // Delete character at cursor (delete key)
    auto delete_char() -> void {
        auto& line = lines_[cursor_.line];
        if (cursor_.col < line.size()) {
            auto erase_len = utf8_codepoint_length(static_cast<unsigned char>(line[cursor_.col]));
            erase_len = std::min(erase_len, line.size() - cursor_.col);
            line.erase(cursor_.col, erase_len);
        } else if (cursor_.line + 1 < lines_.size()) {
            lines_[cursor_.line] += lines_[cursor_.line + 1];
            lines_.erase(lines_.begin() + static_cast<long>(cursor_.line + 1));
        }
    }

    // Delete entire current line (dd in vim)
    auto delete_line() -> std::string {
        std::string deleted = lines_[cursor_.line];
        if (lines_.size() > 1) {
            lines_.erase(lines_.begin() + static_cast<long>(cursor_.line));
            if (cursor_.line >= lines_.size()) cursor_.line = lines_.size() - 1;
        } else {
            lines_[0].clear();
        }
        cursor_.col = 0;
        return deleted;
    }

    // Yank (copy) current line
    [[nodiscard]] auto yank_line() const -> std::string {
        return lines_[cursor_.line] + '\n';
    }

    // Paste text after cursor
    auto paste(std::string_view text) -> void { insert(text); }

    // Paste linewise register after the current logical line.
    auto paste_line_after(std::string_view text) -> void {
        std::vector<std::string> new_lines;
        std::size_t start = 0;
        while (start < text.size()) {
            auto end = text.find('\n', start);
            auto line = end == std::string_view::npos
                ? text.substr(start)
                : text.substr(start, end - start);
            if (!(line.empty() && end != std::string_view::npos && end + 1 >= text.size())) {
                new_lines.emplace_back(line);
            }
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
        if (new_lines.empty()) return;

        auto insert_pos = lines_.begin() + static_cast<long>(cursor_.line + 1);
        lines_.insert(insert_pos, new_lines.begin(), new_lines.end());
        cursor_.line += 1;
        cursor_.col = 0;
    }

    // Vim leaves insert mode with the cursor on the previous character.
    auto adjust_for_insert_to_normal() -> void {
        if (cursor_.col > 0) --cursor_.col;
    }

    // Move cursor with boundary checks
    auto move_cursor(VimMotion motion) -> void {
        switch (motion) {
            case VimMotion::Left:
                if (cursor_.col > 0) cursor_.col--;
                break;
            case VimMotion::Right:
                if (cursor_.col < lines_[cursor_.line].size()) cursor_.col++;
                break;
            case VimMotion::Up:
                if (cursor_.line > 0) {
                    cursor_.line--;
                    cursor_.col = std::min(cursor_.col, lines_[cursor_.line].size());
                }
                break;
            case VimMotion::Down:
                if (cursor_.line + 1 < lines_.size()) {
                    cursor_.line++;
                    cursor_.col = std::min(cursor_.col, lines_[cursor_.line].size());
                }
                break;
            case VimMotion::WordForward: {
                auto& line = lines_[cursor_.line];
                auto pos = line.find_first_of(" \t", cursor_.col);
                cursor_.col = (pos != std::string::npos) ? pos + 1 : line.size();
                break;
            }
            case VimMotion::WordBack: {
                if (cursor_.col == 0) break;
                auto& line = lines_[cursor_.line];
                auto pos = line.find_last_of(" \t", cursor_.col - 1);
                cursor_.col = (pos != std::string::npos) ? pos + 1 : 0;
                break;
            }
            case VimMotion::LineStart: cursor_.col = 0; break;
            case VimMotion::LineEnd: cursor_.col = lines_[cursor_.line].size(); break;
        }
    }

    // Get selected text from selection range
    [[nodiscard]] auto get_selection_text(const Selection& sel) const -> std::string {
        auto [start, end] = sel.normalized();
        if (start.line == end.line) {
            return lines_[start.line].substr(start.col, end.col - start.col);
        }
        std::string result = lines_[start.line].substr(start.col) + "\n";
        for (auto i = start.line + 1; i < end.line; ++i) {
            result += lines_[i] + "\n";
        }
        result += lines_[end.line].substr(0, end.col);
        return result;
    }

    // Delete selected text
    auto delete_selection(const Selection& sel) -> std::string {
        std::string text = get_selection_text(sel);
        auto [start, end] = sel.normalized();
        if (start.line == end.line) {
            lines_[start.line].erase(start.col, end.col - start.col);
        } else {
            lines_[start.line].erase(start.col);
            lines_[start.line] += lines_[end.line].substr(end.col);
            lines_.erase(lines_.begin() + static_cast<long>(start.line + 1),
                         lines_.begin() + static_cast<long>(end.line + 1));
        }
        cursor_ = start;
        return text;
    }

    // Word-wrap lines to fit terminal width
    [[nodiscard]] auto wrapped_lines(std::size_t width) const -> std::vector<std::string> {
        std::vector<std::string> result;
        for (const auto& line : lines_) {
            if (line.size() <= width || width == 0) {
                result.push_back(line);
            } else {
                // Wrap at word boundaries
                std::size_t pos = 0;
                while (pos < line.size()) {
                    std::size_t end = std::min(pos + width, line.size());
                    if (end < line.size()) {
                        auto wrap_at = line.rfind(' ', end);
                        if (wrap_at != std::string::npos && wrap_at > pos) end = wrap_at + 1;
                    }
                    result.push_back(line.substr(pos, end - pos));
                    pos = end;
                }
            }
        }
        return result;
    }

    [[nodiscard]] auto content() const -> std::string {
        std::string result;
        for (std::size_t i = 0; i < lines_.size(); ++i) {
            if (i > 0) result += '\n';
            result += lines_[i];
        }
        return result;
    }

    auto clear() -> void { lines_ = {""}; cursor_ = {0, 0}; }
    [[nodiscard]] auto cursor() const -> CursorPos { return cursor_; }
    [[nodiscard]] auto lines() const -> const std::vector<std::string>& { return lines_; }
    [[nodiscard]] auto empty() const -> bool { return lines_.size() == 1 && lines_[0].empty(); }

private:
    [[nodiscard]] static auto utf8_codepoint_length(unsigned char lead) -> std::size_t {
        if ((lead & 0b1000'0000) == 0) return 1;
        if ((lead & 0b1110'0000) == 0b1100'0000) return 2;
        if ((lead & 0b1111'0000) == 0b1110'0000) return 3;
        if ((lead & 0b1111'1000) == 0b1111'0000) return 4;
        return 1;
    }

    [[nodiscard]] static auto previous_codepoint_start(const std::string& line, std::size_t col) -> std::size_t {
        if (col == 0) return 0;
        std::size_t pos = col - 1;
        while (pos > 0 && (static_cast<unsigned char>(line[pos]) & 0b1100'0000) == 0b1000'0000) {
            --pos;
        }
        return pos;
    }

    std::vector<std::string> lines_;
    CursorPos cursor_;
};

// HistoryManager: command history with search
class HistoryManager {
public:
    explicit HistoryManager(std::size_t max_entries = 1000) : max_entries_(max_entries) {}

    auto push(std::string_view entry) -> void {
        if (entry.empty()) return;
        // Remove duplicate if exists
        std::erase_if(history_, [&](const auto& e) { return e == entry; });
        history_.push_back(std::string(entry));
        if (history_.size() > max_entries_) history_.pop_front();
        reset_navigation();
    }

    // Navigate up (older) in history
    [[nodiscard]] auto navigate_up() -> std::optional<std::string> {
        if (history_.empty() || nav_index_ == 0) return std::nullopt;
        if (nav_index_ == std::string::npos) nav_index_ = history_.size();
        --nav_index_;
        return history_[nav_index_];
    }

    // Navigate down (newer) in history
    [[nodiscard]] auto navigate_down() -> std::optional<std::string> {
        if (nav_index_ == std::string::npos) return std::nullopt;
        ++nav_index_;
        if (nav_index_ >= history_.size()) { nav_index_ = std::string::npos; return std::nullopt; }
        return history_[nav_index_];
    }

    // Search history matching prefix (Ctrl+R)
    [[nodiscard]] auto search(std::string_view query) const -> std::vector<std::string> {
        std::vector<std::string> results;
        for (const auto& entry : history_ | std::views::reverse) {
            if (entry.find(query) != std::string::npos) {
                results.push_back(entry);
                if (results.size() >= 10) break;
            }
        }
        return results;
    }

    auto reset_navigation() -> void { nav_index_ = std::string::npos; }
    [[nodiscard]] auto size() const -> std::size_t { return history_.size(); }

private:
    std::deque<std::string> history_;
    std::size_t max_entries_;
    std::size_t nav_index_{std::string::npos};
};

// Typeahead suggestion entry
struct Suggestion {
    std::string text;
    std::string description;
    std::string category; // "command", "file", "history"
};

// Typeahead: slash commands and file path completion
class Typeahead {
public:
    auto set_commands(std::vector<Suggestion> commands) -> void { commands_ = std::move(commands); }

    // Get suggestions based on current input
    [[nodiscard]] auto suggest(std::string_view input) const -> std::vector<Suggestion> {
        if (input.empty()) return {};
        std::vector<Suggestion> results;
        // Slash command suggestions
        if (input.starts_with('/')) {
            auto query = input.substr(1);
            for (const auto& cmd : commands_) {
                if (cmd.text.starts_with(query)) results.push_back(cmd);
            }
        }
        // File path completion (starts with ./, ../, or ~)
        if (input.starts_with("./") || input.starts_with("../") ||
            (input.size() > 1 && input[0] == '~')) {
            append_path_suggestions(input, results);
        }
        if (results.size() > 8) results.resize(8);
        return results;
    }

    [[nodiscard]] auto selected_index() const -> std::optional<std::size_t> { return selected_; }
    auto select_next(std::size_t total) -> void {
        selected_ = selected_.has_value() ? (*selected_ + 1) % total : 0;
    }
    auto select_prev(std::size_t total) -> void {
        selected_ = selected_.has_value() ? (*selected_ + total - 1) % total : total - 1;
    }
    auto reset() -> void { selected_ = std::nullopt; }

private:
    std::vector<Suggestion> commands_;
    std::optional<std::size_t> selected_;

    static auto append_path_suggestions(std::string_view input, std::vector<Suggestion>& results) -> void {
        std::filesystem::path prefix(input);
        if (input.starts_with("~")) {
            if (const char* home = std::getenv("HOME")) {
                prefix = std::filesystem::path(home) / std::string(input.substr(2));
            }
        }

        auto dir = prefix.parent_path();
        if (dir.empty()) dir = ".";
        auto stem = prefix.filename().string();
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) return;

        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            auto name = entry.path().filename().string();
            if (!stem.empty() && !name.starts_with(stem)) continue;
            auto suggestion = (dir == std::filesystem::path("."))
                ? name
                : (prefix.parent_path() / name).string();
            if (entry.is_directory(ec)) suggestion += "/";
            results.push_back({std::move(suggestion), "path completion", "file"});
            if (results.size() >= 8) break;
        }
    }
};

// Paste detection: distinguishes rapid keystrokes from paste
class PasteDetector {
    using Clock = std::chrono::steady_clock;
public:
    // Returns true if the sequence of inputs looks like a paste
    auto feed(char ch) -> bool {
        if (ch == '\0') {
            reset();
            return false;
        }
        auto now = Clock::now();
        if (last_input_ && (now - *last_input_) < std::chrono::milliseconds(5)) {
            rapid_count_++;
        } else {
            rapid_count_ = 0;
        }
        last_input_ = now;
        // More than 4 rapid chars suggests paste
        return rapid_count_ > 4;
    }

    auto reset() -> void {
        rapid_count_ = 0;
        last_input_ = std::nullopt;
    }

private:
    std::optional<Clock::time_point> last_input_;
    std::size_t rapid_count_{0};
};

// NOTE: VimHandler removed — vim mode handling is now consolidated in
//   - cc::ui::common::VimMode (canonical enum, ui_types.cppm)
//   - cc::ui::prompt::vim_input (standalone VimInput component)
//   - ui::components::TextInputImpl with optional<VimMode> (text_input.cppm)
// TS REF: src/hooks/useVimInput.ts — single vim state machine wrapping text input.

} // namespace cc::ui
