/// @file ink_utils.cppm
/// @brief Ink-style terminal UI utilities for CC-REPL.
/// Migrates: src/ink/ utility files
///   - wrap-text.ts, wrapAnsi.ts, colorize.ts, styles.ts,
///     searchHighlight.ts, selection.ts, tabstops.ts,
///     supports-hyperlinks.ts, clearTerminal.ts, log-update.ts,
///     hit-test.ts, focus.ts, constants.ts, node-cache.ts,
///     squash-text-nodes.ts, parse-keypress.ts,
///     terminal-querier.ts, terminal-focus-state.ts
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <functional>
#include <cstdint>
#include <cstddef>
#include <variant>
#include <utility>
#include <algorithm>
#include <cstdlib>

#ifndef _WIN32
#include <sys/ioctl.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

export module cc.ui.ink_utils;

import cc.ui.text_measure;

export namespace cc::ui::ink_utils {

// ─── Types ───────────────────────────────────────────────────────────────────

enum class WrapMode {
    Word,
    Char,
    None
};

struct WrapConfig {
    size_t width;
    WrapMode mode{WrapMode::Word};
    bool trim{true};
    std::string word_separators{" -_"};
};

struct HighlightRange {
    size_t start;
    size_t length;
    // style info
    bool bold;
    bool underline;
};

struct SelectionState {
    size_t anchor_offset;
    size_t focus_offset;
    bool is_active;
};

struct HitTestResult {
    int line;
    int column;
    bool inside_content;
};

struct FocusState {
    bool app_focused;
    bool terminal_focused;
    int focus_depth;
};

struct InkConstants {
    static constexpr int DEFAULT_TAB_WIDTH = 8;
    static constexpr int MIN_WIDTH = 1;
    static constexpr int MAX_LINES = 10000;
};

// ─── Internal Helpers ────────────────────────────────────────────────────────

namespace detail {

/// Check if a character is a word separator for word-wrap purposes.
inline auto is_word_separator(char ch, std::string_view separators) -> bool {
    for (char sep : separators) {
        if (ch == sep) return true;
    }
    return false;
}

/// Check if we're at the start of an ANSI escape sequence.
inline auto skip_ansi(const char* data, size_t len) -> size_t {
    if (len < 2 || data[0] != '\033') return 0;
    if (data[1] == '[') {
        size_t i = 2;
        while (i < len) {
            auto ch = static_cast<uint8_t>(data[i]);
            if (ch >= 0x40 && ch <= 0x7E) return i + 1;
            ++i;
        }
        return len;
    }
    if (data[1] == ']') {
        size_t i = 2;
        while (i < len) {
            if (data[i] == '\007') return i + 1;
            if (data[i] == '\033' && i + 1 < len && data[i + 1] == '\\') return i + 2;
            ++i;
        }
        return len;
    }
    return 2;
}

/// Find case-insensitive occurrence of needle in haystack starting at pos.
inline auto find_case_insensitive(std::string_view haystack, std::string_view needle, size_t pos = 0) -> size_t {
    if (needle.empty() || needle.size() > haystack.size()) return std::string_view::npos;
    for (size_t i = pos; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            char h = haystack[i + j];
            char n = needle[j];
            if (h >= 'A' && h <= 'Z') h += 32;
            if (n >= 'A' && n <= 'Z') n += 32;
            if (h != n) { match = false; break; }
        }
        if (match) return i;
    }
    return std::string_view::npos;
}

} // namespace detail

// ─── Functions ───────────────────────────────────────────────────────────────

/// Wraps text into lines according to the given configuration.
inline auto wrap_text(std::string_view text, WrapConfig config) -> std::vector<std::string> {
    if (config.mode == WrapMode::None || config.width == 0) {
        // Split by newlines only
        std::vector<std::string> lines;
        size_t start = 0;
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == '\n') {
                lines.emplace_back(text.substr(start, i - start));
                start = i + 1;
            }
        }
        return lines;
    }

    std::vector<std::string> result;
    size_t line_start = 0;

    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            auto line = text.substr(line_start, i - line_start);

            // Wrap this single line
            if (text_measure::string_width(line) <= config.width) {
                auto s = std::string{line};
                if (config.trim) {
                    while (!s.empty() && s.back() == ' ') s.pop_back();
                }
                result.push_back(std::move(s));
            } else if (config.mode == WrapMode::Char) {
                // Character-level wrapping
                std::string current;
                size_t current_width = 0;
                size_t j = 0;
                while (j < line.size()) {
                    auto skip = detail::skip_ansi(line.data() + j, line.size() - j);
                    if (skip > 0) {
                        current.append(line.data() + j, skip);
                        j += skip;
                        continue;
                    }
                    auto [cp, bytes] = text_measure::detail::decode_utf8(line.data() + j, line.size() - j);
                    auto cw = static_cast<size_t>(std::max(0, text_measure::char_width(cp)));
                    if (current_width + cw > config.width && !current.empty()) {
                        if (config.trim) {
                            while (!current.empty() && current.back() == ' ') current.pop_back();
                        }
                        result.push_back(std::move(current));
                        current.clear();
                        current_width = 0;
                    }
                    current.append(line.data() + j, bytes);
                    current_width += cw;
                    j += bytes;
                }
                if (!current.empty()) {
                    if (config.trim) {
                        while (!current.empty() && current.back() == ' ') current.pop_back();
                    }
                    result.push_back(std::move(current));
                }
            } else {
                // Word-level wrapping
                std::string current;
                size_t current_width = 0;
                std::string word;
                size_t word_width = 0;
                size_t j = 0;

                auto flush_word = [&]() {
                    if (word.empty()) return;
                    if (current_width + word_width > config.width && !current.empty()) {
                        if (config.trim) {
                            while (!current.empty() && current.back() == ' ') current.pop_back();
                        }
                        result.push_back(std::move(current));
                        current.clear();
                        current_width = 0;
                    }
                    // If a single word exceeds width, force char-break it
                    if (word_width > config.width && current.empty()) {
                        // Break the word into width-sized chunks
                        std::string chunk;
                        size_t chunk_width = 0;
                        size_t wi = 0;
                        while (wi < word.size()) {
                            auto [cp, bytes] = text_measure::detail::decode_utf8(
                                word.data() + wi, word.size() - wi);
                            auto cw = static_cast<size_t>(std::max(0, text_measure::char_width(cp)));
                            if (chunk_width + cw > config.width && !chunk.empty()) {
                                result.push_back(std::move(chunk));
                                chunk.clear();
                                chunk_width = 0;
                            }
                            chunk.append(word.data() + wi, bytes);
                            chunk_width += cw;
                            wi += bytes;
                        }
                        current = std::move(chunk);
                        current_width = chunk_width;
                    } else {
                        current += word;
                        current_width += word_width;
                    }
                    word.clear();
                    word_width = 0;
                };

                while (j < line.size()) {
                    auto skip = detail::skip_ansi(line.data() + j, line.size() - j);
                    if (skip > 0) {
                        word.append(line.data() + j, skip);
                        j += skip;
                        continue;
                    }

                    char ch = line[j];
                    if (detail::is_word_separator(ch, config.word_separators)) {
                        word += ch;
                        word_width += 1;
                        flush_word();
                        ++j;
                    } else {
                        auto [cp, bytes] = text_measure::detail::decode_utf8(
                            line.data() + j, line.size() - j);
                        auto cw = static_cast<size_t>(std::max(0, text_measure::char_width(cp)));
                        word.append(line.data() + j, bytes);
                        word_width += cw;
                        j += bytes;
                    }
                }
                flush_word();
                if (!current.empty()) {
                    if (config.trim) {
                        while (!current.empty() && current.back() == ' ') current.pop_back();
                    }
                    result.push_back(std::move(current));
                }
            }

            line_start = i + 1;
        }
    }

    return result;
}

/// Wraps ANSI-styled text to a given width, preserving escape sequences across line breaks.
inline auto wrap_ansi(std::string_view text, size_t width, WrapMode mode = WrapMode::Word) -> std::string {
    auto lines = wrap_text(text, WrapConfig{.width = width, .mode = mode, .trim = true});
    std::string result;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) result += '\n';
        result += lines[i];
    }
    return result;
}

/// Colorizes text with 24-bit RGB using ANSI escape sequences.
inline auto colorize(std::string_view text, uint8_t r, uint8_t g, uint8_t b) -> std::string {
    // ESC[38;2;R;G;Bm ... ESC[39m
    std::string result;
    result.reserve(text.size() + 24);
    result += "\033[38;2;";
    result += std::to_string(r);
    result += ';';
    result += std::to_string(g);
    result += ';';
    result += std::to_string(b);
    result += 'm';
    result += text;
    result += "\033[39m";
    return result;
}

/// Colorizes background with 24-bit RGB.
inline auto colorize_bg(std::string_view text, uint8_t r, uint8_t g, uint8_t b) -> std::string {
    std::string result;
    result.reserve(text.size() + 24);
    result += "\033[48;2;";
    result += std::to_string(r);
    result += ';';
    result += std::to_string(g);
    result += ';';
    result += std::to_string(b);
    result += 'm';
    result += text;
    result += "\033[49m";
    return result;
}

/// Applies multiple text styles via ANSI escape sequences.
inline auto apply_style(std::string_view text, bool bold, bool italic, bool underline, bool dim) -> std::string {
    std::string prefix;
    std::string suffix;
    if (bold)      { prefix += "\033[1m";  suffix += "\033[22m"; }
    if (italic)    { prefix += "\033[3m";  suffix += "\033[23m"; }
    if (underline) { prefix += "\033[4m";  suffix += "\033[24m"; }
    if (dim)       { prefix += "\033[2m";  suffix += "\033[22m"; }
    return prefix + std::string{text} + suffix;
}

/// Highlights search matches in text with ANSI escape codes (bold + inverse).
inline auto highlight_search(std::string_view text, std::string_view query, bool case_sensitive = false) -> std::string {
    if (query.empty()) return std::string{text};

    std::string result;
    result.reserve(text.size() * 2); // Generous allocation for escape sequences

    size_t pos = 0;
    while (pos < text.size()) {
        size_t found;
        if (case_sensitive) {
            found = text.find(query, pos);
        } else {
            found = detail::find_case_insensitive(text, query, pos);
        }

        if (found == std::string_view::npos) {
            result.append(text.data() + pos, text.size() - pos);
            break;
        }

        // Append text before match
        result.append(text.data() + pos, found - pos);

        // Append highlighted match (bold + inverse video)
        result += "\033[1;7m";
        result.append(text.data() + found, query.size());
        result += "\033[22;27m";

        pos = found + query.size();
    }

    return result;
}

/// Extracts the selected text from content given a selection state.
inline auto get_selection_text(std::string_view content, SelectionState sel) -> std::string {
    if (!sel.is_active) return {};
    auto start = std::min(sel.anchor_offset, sel.focus_offset);
    auto end = std::max(sel.anchor_offset, sel.focus_offset);
    if (start >= content.size()) return {};
    end = std::min(end, content.size());
    return std::string{content.substr(start, end - start)};
}

/// Renders selected text with inverse video.
inline auto render_selection(std::string_view content, SelectionState sel) -> std::string {
    if (!sel.is_active) return std::string{content};

    auto start = std::min(sel.anchor_offset, sel.focus_offset);
    auto end = std::max(sel.anchor_offset, sel.focus_offset);
    start = std::min(start, content.size());
    end = std::min(end, content.size());

    std::string result;
    result.reserve(content.size() + 16);
    result.append(content.data(), start);
    result += "\033[7m"; // Inverse video
    result.append(content.data() + start, end - start);
    result += "\033[27m"; // Reset inverse
    result.append(content.data() + end, content.size() - end);
    return result;
}

/// Expands tab characters to spaces according to tab_width.
inline auto expand_tabs(std::string_view text, int tab_width = 8) -> std::string {
    std::string result;
    result.reserve(text.size());
    int col = 0;
    for (char ch : text) {
        if (ch == '\t') {
            int spaces = tab_width - (col % tab_width);
            result.append(static_cast<size_t>(spaces), ' ');
            col += spaces;
        } else if (ch == '\n') {
            result += ch;
            col = 0;
        } else {
            result += ch;
            ++col;
        }
    }
    return result;
}

/// Checks if the terminal supports OSC 8 hyperlinks.
inline auto supports_hyperlinks() -> bool {
    // Check common terminal programs known to support hyperlinks
    const char* term_program = std::getenv("TERM_PROGRAM");
    if (term_program) {
        std::string_view tp{term_program};
        if (tp == "iTerm.app" || tp == "WezTerm" || tp == "Hyper") return true;
    }

    // VTE-based terminals (GNOME Terminal, etc.) version >= 50.0
    const char* vte_version = std::getenv("VTE_VERSION");
    if (vte_version) {
        int version = std::atoi(vte_version);
        if (version >= 5000) return true;
    }

    // Kitty terminal
    const char* term = std::getenv("TERM");
    if (term) {
        std::string_view t{term};
        if (t.find("kitty") != std::string_view::npos) return true;
    }

    // Windows Terminal
    const char* wt_session = std::getenv("WT_SESSION");
    if (wt_session && wt_session[0] != '\0') return true;

    // COLORTERM=truecolor is a good proxy for modern terminals
    const char* colorterm = std::getenv("COLORTERM");
    if (colorterm) {
        std::string_view ct{colorterm};
        if (ct == "truecolor" || ct == "24bit") return true;
    }

    return false;
}

/// Creates an OSC 8 hyperlink escape sequence.
inline auto make_hyperlink(std::string_view url, std::string_view text) -> std::string {
    // OSC 8 ; params ; uri ST  text  OSC 8 ; ; ST
    std::string result;
    result.reserve(url.size() + text.size() + 20);
    result += "\033]8;;";
    result += url;
    result += "\033\\";
    result += text;
    result += "\033]8;;\033\\";
    return result;
}

/// Returns the escape sequence to clear the terminal screen.
inline auto clear_terminal() -> std::string {
    // Move to home + clear screen + clear scrollback
    return "\033[2J\033[3J\033[H";
}

/// Performs a hit test to determine which character position a coordinate maps to.
inline auto hit_test(std::string_view content, int x, int y) -> HitTestResult {
    if (content.empty() || y < 0 || x < 0) {
        return HitTestResult{.line = y, .column = x, .inside_content = false};
    }

    // Split content into lines and find the target line
    int current_line = 0;
    size_t line_start = 0;

    for (size_t i = 0; i <= content.size(); ++i) {
        if (i == content.size() || content[i] == '\n') {
            if (current_line == y) {
                auto line = content.substr(line_start, i - line_start);
                auto line_width = static_cast<int>(text_measure::string_width(line));
                bool inside = x < line_width;
                return HitTestResult{
                    .line = y,
                    .column = std::min(x, line_width),
                    .inside_content = inside
                };
            }
            ++current_line;
            line_start = i + 1;
        }
    }

    // y exceeds total lines
    return HitTestResult{.line = y, .column = x, .inside_content = false};
}

/// Returns the current focus state of the terminal application.
inline auto get_focus_state() -> FocusState {
    // In a terminal context, we determine focus by environment and TTY state.
    // Real focus tracking requires DECRPM mode 1004 (focus events) which needs
    // a running event loop. For static queries, we assume focused if on a TTY.
    bool is_tty = false;
#ifndef _WIN32
    is_tty = isatty(STDIN_FILENO) != 0;
#else
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    is_tty = GetConsoleMode(h, &mode) != 0;
#endif

    return FocusState{
        .app_focused = is_tty,
        .terminal_focused = is_tty,
        .focus_depth = 0
    };
}

/// Queries the terminal for its current size (width, height in columns/rows).
inline auto query_terminal_size() -> std::pair<int, int> {
#ifndef _WIN32
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        return {static_cast<int>(ws.ws_col), static_cast<int>(ws.ws_row)};
    }
#else
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleScreenBufferInfo(h, &csbi)) {
        int cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        int rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        if (cols > 0 && rows > 0) return {cols, rows};
    }
#endif

    // Fallback: check COLUMNS and LINES env vars
    const char* cols_env = std::getenv("COLUMNS");
    const char* rows_env = std::getenv("LINES");
    int cols = cols_env ? std::atoi(cols_env) : 80;
    int rows = rows_env ? std::atoi(rows_env) : 24;
    if (cols <= 0) cols = 80;
    if (rows <= 0) rows = 24;
    return {cols, rows};
}

/// Queries the number of colors supported by the terminal.
inline auto query_terminal_colors() -> int {
    // Check COLORTERM for 24-bit support
    const char* colorterm = std::getenv("COLORTERM");
    if (colorterm) {
        std::string_view ct{colorterm};
        if (ct == "truecolor" || ct == "24bit") return 16777216; // 2^24
    }

    // Check TERM for known values
    const char* term = std::getenv("TERM");
    if (term) {
        std::string_view t{term};
        if (t.find("256color") != std::string_view::npos) return 256;
        if (t.find("kitty") != std::string_view::npos) return 16777216;
        if (t == "dumb") return 1;
        if (t == "linux") return 8;
    }

    // Windows Terminal supports truecolor
    const char* wt_session = std::getenv("WT_SESSION");
    if (wt_session && wt_session[0] != '\0') return 16777216;

    // Default assumption for modern terminals
    return 256;
}

/// Checks if the terminal supports Unicode (wide chars, emoji).
inline auto supports_unicode() -> bool {
    const char* lang = std::getenv("LANG");
    if (lang) {
        std::string_view l{lang};
        if (l.find("UTF-8") != std::string_view::npos ||
            l.find("utf-8") != std::string_view::npos ||
            l.find("UTF8") != std::string_view::npos) {
            return true;
        }
    }
    const char* lc_all = std::getenv("LC_ALL");
    if (lc_all) {
        std::string_view l{lc_all};
        if (l.find("UTF-8") != std::string_view::npos ||
            l.find("utf-8") != std::string_view::npos) {
            return true;
        }
    }
    // macOS defaults to UTF-8
#ifdef __APPLE__
    return true;
#endif
    return false;
}

} // namespace cc::ui::ink_utils
