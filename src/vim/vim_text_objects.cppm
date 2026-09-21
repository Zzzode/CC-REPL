module;
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <algorithm>
#include <cctype>
#include <utility>

export module cc.vim.text_objects;

export namespace cc::vim {

// ============================================================================
// Text Object Range
// ============================================================================

/// A selected range for a text object (start inclusive, end exclusive).
struct TextObjectRange {
    int start;
    int end;
};

// ============================================================================
// Character Classification
// ============================================================================

/// Check if a character is vim whitespace (space or tab).
[[nodiscard]] inline auto is_vim_whitespace(char ch) -> bool {
    return ch == ' ' || ch == '\t';
}

/// Check if a character is a vim word character (alphanumeric or underscore).
[[nodiscard]] inline auto is_vim_word_char(char ch) -> bool {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

/// Check if a character is vim punctuation (not word char, not whitespace, not newline).
[[nodiscard]] inline auto is_vim_punctuation(char ch) -> bool {
    return !is_vim_word_char(ch) && !is_vim_whitespace(ch) && ch != '\n';
}

/// Check if a character is a non-whitespace character (for WORD objects).
[[nodiscard]] inline auto is_vim_WORD_char(char ch) -> bool {
    return !is_vim_whitespace(ch) && ch != '\n';
}

// ============================================================================
// Delimiter Pair Lookup
// ============================================================================

/// Get the open/close delimiter pair for a given text object type character.
/// Returns nullopt if the character is not a paired delimiter.
[[nodiscard]] inline auto get_delimiter_pair(char obj_type)
    -> std::optional<std::pair<char, char>> {
    switch (obj_type) {
        case '(': case ')': case 'b': return std::pair{'(', ')'};
        case '[': case ']':           return std::pair{'[', ']'};
        case '{': case '}': case 'B': return std::pair{'{', '}'};
        case '<': case '>':           return std::pair{'<', '>'};
        case '"':                     return std::pair{'"', '"'};
        case '\'':                    return std::pair{'\'', '\''};
        case '`':                     return std::pair{'`', '`'};
        default:                      return std::nullopt;
    }
}

// ============================================================================
// Internal Helpers
// ============================================================================

namespace detail {

/// Find a word text object (iw/aw or iW/aW) at the given offset.
/// word_pred determines what constitutes a "word character".
[[nodiscard]] inline auto find_word_object(
    std::string_view text,
    int offset,
    bool is_inner,
    bool (*word_pred)(char)
) -> std::optional<TextObjectRange> {
    int len = static_cast<int>(text.size());
    if (offset < 0 || offset >= len) return std::nullopt;

    char ch = text[offset];

    int start = offset;
    int end = offset;

    if (word_pred(ch)) {
        // Expand over word characters
        while (start > 0 && word_pred(text[start - 1])) --start;
        while (end < len && word_pred(text[end])) ++end;
    } else if (is_vim_whitespace(ch)) {
        // Expand over whitespace
        while (start > 0 && is_vim_whitespace(text[start - 1])) --start;
        while (end < len && is_vim_whitespace(text[end])) ++end;
        return TextObjectRange{start, end};
    } else if (is_vim_punctuation(ch)) {
        // Expand over punctuation
        while (start > 0 && is_vim_punctuation(text[start - 1])) --start;
        while (end < len && is_vim_punctuation(text[end])) ++end;
    } else {
        return std::nullopt;
    }

    if (!is_inner) {
        // "a word" includes surrounding whitespace
        if (end < len && is_vim_whitespace(text[end])) {
            while (end < len && is_vim_whitespace(text[end])) ++end;
        } else if (start > 0 && is_vim_whitespace(text[start - 1])) {
            while (start > 0 && is_vim_whitespace(text[start - 1])) --start;
        }
    }

    return TextObjectRange{start, end};
}

/// Find a quote text object (i"/a", i'/a', i`/a`) at the given offset.
/// Searches only within the current line.
[[nodiscard]] inline auto find_quote_object(
    std::string_view text,
    int offset,
    char quote,
    bool is_inner
) -> std::optional<TextObjectRange> {
    int len = static_cast<int>(text.size());
    if (offset < 0 || offset >= len) return std::nullopt;

    // Find current line boundaries
    int line_start = offset;
    while (line_start > 0 && text[line_start - 1] != '\n') --line_start;

    int line_end = offset;
    while (line_end < len && text[line_end] != '\n') ++line_end;

    // Collect quote positions on this line
    std::vector<int> positions;
    for (int i = line_start; i < line_end; ++i) {
        if (text[i] == quote) positions.push_back(i);
    }

    // Pair quotes: 0-1, 2-3, 4-5, etc. Find the pair containing offset
    for (int i = 0; i + 1 < static_cast<int>(positions.size()); i += 2) {
        int qs = positions[i];
        int qe = positions[i + 1];
        if (qs <= offset && offset <= qe) {
            if (is_inner) {
                return TextObjectRange{qs + 1, qe};
            } else {
                return TextObjectRange{qs, qe + 1};
            }
        }
    }

    return std::nullopt;
}

/// Find a bracket/paren text object at the given offset.
/// Handles nested delimiters via depth counting.
[[nodiscard]] inline auto find_bracket_object(
    std::string_view text,
    int offset,
    char open,
    char close,
    bool is_inner
) -> std::optional<TextObjectRange> {
    int len = static_cast<int>(text.size());
    if (offset < 0 || offset >= len) return std::nullopt;

    // Search backward for matching open delimiter
    int depth = 0;
    int start = -1;
    for (int i = offset; i >= 0; --i) {
        if (text[i] == close && i != offset) {
            ++depth;
        } else if (text[i] == open) {
            if (depth == 0) {
                start = i;
                break;
            }
            --depth;
        }
    }
    if (start == -1) return std::nullopt;

    // Search forward for matching close delimiter
    depth = 0;
    int end = -1;
    for (int i = start + 1; i < len; ++i) {
        if (text[i] == open) {
            ++depth;
        } else if (text[i] == close) {
            if (depth == 0) {
                end = i;
                break;
            }
            --depth;
        }
    }
    if (end == -1) return std::nullopt;

    if (is_inner) {
        return TextObjectRange{start + 1, end};
    } else {
        return TextObjectRange{start, end + 1};
    }
}

} // namespace detail

// ============================================================================
// Public API
// ============================================================================

/// Find a text object at the given position.
///
/// @param text       The full buffer text
/// @param offset     Cursor position (byte offset)
/// @param obj_type   The text object type character ('w', 'W', '"', '(', etc.)
/// @param is_inner   true for inner (i), false for around (a)
/// @return           The selection range, or nullopt if no object found
[[nodiscard]] inline auto find_text_object(
    std::string_view text,
    int offset,
    char obj_type,
    bool is_inner
) -> std::optional<TextObjectRange> {
    if (text.empty()) return std::nullopt;

    // Word objects
    if (obj_type == 'w') {
        return detail::find_word_object(text, offset, is_inner, is_vim_word_char);
    }
    if (obj_type == 'W') {
        return detail::find_word_object(text, offset, is_inner, is_vim_WORD_char);
    }

    // Paired delimiter objects
    auto pair = get_delimiter_pair(obj_type);
    if (pair) {
        auto [open, close] = *pair;
        if (open == close) {
            // Quote-style: same open/close character
            return detail::find_quote_object(text, offset, open, is_inner);
        } else {
            // Bracket-style: different open/close characters
            return detail::find_bracket_object(text, offset, open, close, is_inner);
        }
    }

    return std::nullopt;
}

/// Convenience overload: find text object with string object type (first char used).
[[nodiscard]] inline auto find_text_object(
    std::string_view text,
    int offset,
    std::string_view obj_type,
    bool is_inner
) -> std::optional<TextObjectRange> {
    if (obj_type.empty()) return std::nullopt;
    return find_text_object(text, offset, obj_type[0], is_inner);
}

} // namespace cc::vim
