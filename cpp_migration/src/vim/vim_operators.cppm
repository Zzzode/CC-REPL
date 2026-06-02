module;
#include <string>
#include <string_view>
#include <optional>
#include <expected>
#include <functional>
#include <algorithm>
#include <vector>
#include <variant>
#include <cstddef>

export module cc.vim.operators;

import cc.vim.text_objects;

export namespace cc::vim {

// ============================================================================
// Core Operator Types
// ============================================================================

/// Vim operator kind.
enum class Operator { Delete, Change, Yank };

/// Vim find motion direction/type.
enum class FindType { f, F, t, T };

/// Text object scope (inner vs. around).
enum class TextObjScope { Inner, Around };

/// Indent direction.
enum class IndentDir { Right, Left };

/// Open-line direction.
enum class OpenLineDir { Above, Below };

// ============================================================================
// Recorded Changes (for dot-repeat)
// ============================================================================

struct ChangeInsert {
    std::string text;
};

struct ChangeOperator {
    Operator op;
    std::string motion;
    int count;
};

struct ChangeOperatorTextObj {
    Operator op;
    std::string obj_type;
    TextObjScope scope;
    int count;
};

struct ChangeOperatorFind {
    Operator op;
    FindType find;
    char ch;
    int count;
};

struct ChangeReplace {
    std::string ch;
    int count;
};

struct ChangeX {
    int count;
};

struct ChangeToggleCase {
    int count;
};

struct ChangeIndent {
    IndentDir dir;
    int count;
};

struct ChangeOpenLine {
    OpenLineDir direction;
};

struct ChangeJoin {
    int count;
};

/// Discriminated union of all recorded change types for dot-repeat.
using RecordedChange = std::variant<
    ChangeInsert,
    ChangeOperator,
    ChangeOperatorTextObj,
    ChangeOperatorFind,
    ChangeReplace,
    ChangeX,
    ChangeToggleCase,
    ChangeIndent,
    ChangeOpenLine,
    ChangeJoin
>;

// ============================================================================
// Operator Context
// ============================================================================

/// Last find info for `;` and `,` repeat.
struct LastFind {
    FindType type;
    char ch;
};

/// Context provided to operator execution functions.
/// Uses std::function callbacks so callers can wire in their own buffer logic.
struct OperatorContext {
    int cursor_offset;
    std::string text;

    std::function<void(std::string_view new_text)> set_text;
    std::function<void(int offset)> set_offset;
    std::function<void(int offset)> enter_insert;
    std::function<std::string()> get_register;
    std::function<void(std::string_view content, bool linewise)> set_register;
    std::function<std::optional<LastFind>()> get_last_find;
    std::function<void(FindType type, char ch)> set_last_find;
    std::function<void(RecordedChange change)> record_change;
};

// ============================================================================
// Internal Helpers
// ============================================================================

namespace detail {

/// Count occurrences of a character in a string_view.
[[nodiscard]] inline auto count_char(std::string_view s, char ch) -> int {
    int count = 0;
    for (char c : s) {
        if (c == ch) ++count;
    }
    return count;
}

/// Get the byte offset of the start of line N (0-indexed) given pre-split lines.
[[nodiscard]] inline auto get_line_start_offset(
    const std::vector<std::string_view>& lines,
    int line_index
) -> int {
    int offset = 0;
    for (int i = 0; i < line_index && i < static_cast<int>(lines.size()); ++i) {
        offset += static_cast<int>(lines[i].size()) + 1; // +1 for '\n'
    }
    return offset;
}

/// Split text into lines (views into the original text).
[[nodiscard]] inline auto split_lines(std::string_view text) -> std::vector<std::string_view> {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto pos = text.find('\n', start);
        if (pos == std::string_view::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return lines;
}

/// Find the logical line number for a given byte offset.
[[nodiscard]] inline auto offset_to_line(std::string_view text, int offset) -> int {
    return count_char(text.substr(0, std::min(static_cast<int>(text.size()), offset)), '\n');
}

/// Find the start offset of the line containing the given offset.
[[nodiscard]] inline auto line_start_for_offset(std::string_view text, int offset) -> int {
    int pos = offset;
    while (pos > 0 && text[pos - 1] != '\n') --pos;
    return pos;
}

/// Advance one character (clamped to text length).
[[nodiscard]] inline auto next_offset(std::string_view text, int offset) -> int {
    if (offset >= static_cast<int>(text.size())) return static_cast<int>(text.size());
    return offset + 1;
}

/// Get the first non-blank offset on a given line.
[[nodiscard]] inline auto first_non_blank(std::string_view line) -> int {
    int i = 0;
    while (i < static_cast<int>(line.size()) && (line[i] == ' ' || line[i] == '\t')) ++i;
    return i;
}

/// Apply operator (internal). Handles delete/change/yank logic.
inline auto apply_operator(
    Operator op,
    int from,
    int to,
    OperatorContext& ctx,
    bool linewise
) -> void {
    std::string_view text_view = ctx.text;
    std::string content{text_view.substr(from, to - from)};

    // Ensure linewise content ends with newline for paste detection
    if (linewise && (content.empty() || content.back() != '\n')) {
        content += '\n';
    }
    ctx.set_register(content, linewise);

    if (op == Operator::Yank) {
        ctx.set_offset(from);
    } else if (op == Operator::Delete) {
        std::string new_text = std::string(text_view.substr(0, from))
                             + std::string(text_view.substr(to));
        ctx.set_text(new_text);
        int max_off = std::max(0, static_cast<int>(new_text.size()) - 1);
        ctx.set_offset(std::min(from, max_off));
    } else if (op == Operator::Change) {
        std::string new_text = std::string(text_view.substr(0, from))
                             + std::string(text_view.substr(to));
        ctx.set_text(new_text);
        ctx.enter_insert(from);
    }
}

/// Apply operator without linewise flag (defaults to false).
inline auto apply_operator(
    Operator op,
    int from,
    int to,
    OperatorContext& ctx
) -> void {
    apply_operator(op, from, to, ctx, false);
}

} // namespace detail

// ============================================================================
// Operator Execution Functions
// ============================================================================

/// Execute an operator with a text object.
///
/// @param op        The operator (delete, change, yank)
/// @param scope     Inner or Around
/// @param obj_type  The text object type character ('w', '"', '(', etc.)
/// @param count     Repeat count
/// @param ctx       Operator execution context
inline auto execute_operator_text_obj(
    Operator op,
    TextObjScope scope,
    char obj_type,
    int count,
    OperatorContext& ctx
) -> void {
    bool is_inner = (scope == TextObjScope::Inner);
    auto range = find_text_object(ctx.text, ctx.cursor_offset, obj_type, is_inner);
    if (!range) return;

    detail::apply_operator(op, range->start, range->end, ctx);
    ctx.record_change(ChangeOperatorTextObj{
        op, std::string(1, obj_type), scope, count
    });
}

/// Execute a line operation (dd, cc, yy).
///
/// @param op     The operator
/// @param count  Number of lines to affect
/// @param ctx    Operator execution context
inline auto execute_line_op(
    Operator op,
    int count,
    OperatorContext& ctx
) -> void {
    std::string_view text = ctx.text;
    auto lines = detail::split_lines(text);
    int current_line = detail::offset_to_line(text, ctx.cursor_offset);
    int lines_to_affect = std::min(count, static_cast<int>(lines.size()) - current_line);

    int line_start = detail::line_start_for_offset(text, ctx.cursor_offset);
    int line_end = line_start;

    for (int i = 0; i < lines_to_affect; ++i) {
        auto nl = text.find('\n', line_end);
        line_end = (nl == std::string_view::npos)
            ? static_cast<int>(text.size())
            : static_cast<int>(nl) + 1;
    }

    std::string content{text.substr(line_start, line_end - line_start)};
    if (content.empty() || content.back() != '\n') {
        content += '\n';
    }
    ctx.set_register(content, true);

    if (op == Operator::Yank) {
        ctx.set_offset(line_start);
    } else if (op == Operator::Delete) {
        int delete_start = line_start;
        int delete_end = line_end;

        // If deleting to end of file and there's a preceding newline, include it
        if (delete_end == static_cast<int>(text.size()) && delete_start > 0
            && text[delete_start - 1] == '\n') {
            delete_start -= 1;
        }

        std::string new_text = std::string(text.substr(0, delete_start))
                             + std::string(text.substr(delete_end));
        ctx.set_text(new_text.empty() ? "" : new_text);
        int max_off = std::max(0, static_cast<int>(new_text.size()) - 1);
        ctx.set_offset(std::min(delete_start, max_off));
    } else if (op == Operator::Change) {
        if (static_cast<int>(lines.size()) == 1) {
            ctx.set_text("");
            ctx.enter_insert(0);
        } else {
            // Delete affected lines, replace with empty line, enter insert
            std::string new_text;
            for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
                if (i == current_line) {
                    new_text += "";
                    // Skip affected lines
                    i += lines_to_affect - 1;
                } else {
                    new_text += std::string(lines[i]);
                }
                if (i < static_cast<int>(lines.size()) - 1) {
                    new_text += '\n';
                }
            }
            ctx.set_text(new_text);
            ctx.enter_insert(line_start);
        }
    }

    // Record with the operator's first character as the "motion"
    std::string motion(1, op == Operator::Delete ? 'd' : op == Operator::Change ? 'c' : 'y');
    ctx.record_change(ChangeOperator{op, motion, count});
}

/// Execute delete character (x command).
///
/// @param count  Number of characters to delete
/// @param ctx    Operator execution context
inline auto execute_x(int count, OperatorContext& ctx) -> void {
    std::string_view text = ctx.text;
    int from = ctx.cursor_offset;

    if (from >= static_cast<int>(text.size())) return;

    int to = from;
    for (int i = 0; i < count && to < static_cast<int>(text.size()); ++i) {
        ++to;
    }

    std::string deleted{text.substr(from, to - from)};
    std::string new_text = std::string(text.substr(0, from))
                         + std::string(text.substr(to));

    ctx.set_register(deleted, false);
    ctx.set_text(new_text);
    int max_off = std::max(0, static_cast<int>(new_text.size()) - 1);
    ctx.set_offset(std::min(from, max_off));
    ctx.record_change(ChangeX{count});
}

/// Execute replace character (r command).
///
/// @param ch     The replacement character
/// @param count  Number of characters to replace
/// @param ctx    Operator execution context
inline auto execute_replace(
    char ch,
    int count,
    OperatorContext& ctx
) -> void {
    std::string new_text = ctx.text;
    int offset = ctx.cursor_offset;

    for (int i = 0; i < count && offset < static_cast<int>(new_text.size()); ++i) {
        new_text[offset] = ch;
        ++offset;
    }

    ctx.set_text(new_text);
    ctx.set_offset(std::max(0, offset - 1));
    ctx.record_change(ChangeReplace{std::string(1, ch), count});
}

/// Execute toggle case (~ command).
///
/// @param count  Number of characters to toggle
/// @param ctx    Operator execution context
inline auto execute_toggle_case(int count, OperatorContext& ctx) -> void {
    int start_offset = ctx.cursor_offset;
    if (start_offset >= static_cast<int>(ctx.text.size())) return;

    std::string new_text = ctx.text;
    int offset = start_offset;
    int toggled = 0;

    while (offset < static_cast<int>(new_text.size()) && toggled < count) {
        unsigned char c = static_cast<unsigned char>(new_text[offset]);
        if (std::isupper(c)) {
            new_text[offset] = static_cast<char>(std::tolower(c));
        } else if (std::islower(c)) {
            new_text[offset] = static_cast<char>(std::toupper(c));
        }
        ++offset;
        ++toggled;
    }

    ctx.set_text(new_text);
    ctx.set_offset(offset);
    ctx.record_change(ChangeToggleCase{count});
}

/// Execute join lines (J command).
///
/// @param count  Number of following lines to join
/// @param ctx    Operator execution context
inline auto execute_join(int count, OperatorContext& ctx) -> void {
    std::string_view text = ctx.text;
    auto lines = detail::split_lines(text);
    int current_line = detail::offset_to_line(text, ctx.cursor_offset);

    if (current_line >= static_cast<int>(lines.size()) - 1) return;

    int lines_to_join = std::min(count, static_cast<int>(lines.size()) - current_line - 1);
    std::string joined_line{lines[current_line]};
    int cursor_pos = static_cast<int>(joined_line.size());

    for (int i = 1; i <= lines_to_join; ++i) {
        std::string_view next = lines[current_line + i];
        // Trim leading whitespace from joined line
        int trim_start = 0;
        while (trim_start < static_cast<int>(next.size())
               && (next[trim_start] == ' ' || next[trim_start] == '\t')) {
            ++trim_start;
        }
        std::string_view trimmed = next.substr(trim_start);

        if (!trimmed.empty()) {
            if (!joined_line.empty() && joined_line.back() != ' ') {
                joined_line += ' ';
            }
            joined_line += std::string(trimmed);
        }
    }

    // Rebuild text
    std::string new_text;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        if (i == current_line) {
            new_text += joined_line;
            i += lines_to_join; // skip joined lines
        } else {
            new_text += std::string(lines[i]);
        }
        if (i < static_cast<int>(lines.size()) - 1) {
            new_text += '\n';
        }
    }

    ctx.set_text(new_text);
    // Cursor at the junction point
    int new_line_start = detail::get_line_start_offset(
        detail::split_lines(new_text), current_line);
    ctx.set_offset(new_line_start + cursor_pos);
    ctx.record_change(ChangeJoin{count});
}

/// Execute paste (p/P command).
///
/// @param after  true for 'p' (paste after), false for 'P' (paste before)
/// @param count  Number of times to paste
/// @param ctx    Operator execution context
inline auto execute_paste(bool after, int count, OperatorContext& ctx) -> void {
    std::string reg = ctx.get_register();
    if (reg.empty()) return;

    bool is_linewise = (!reg.empty() && reg.back() == '\n');
    std::string content = is_linewise ? reg.substr(0, reg.size() - 1) : reg;

    if (is_linewise) {
        auto lines = detail::split_lines(ctx.text);
        int current_line = detail::offset_to_line(ctx.text, ctx.cursor_offset);
        int insert_line = after ? current_line + 1 : current_line;

        // Build repeated content lines
        auto content_lines = detail::split_lines(content);
        std::vector<std::string_view> repeated_lines;
        for (int i = 0; i < count; ++i) {
            for (auto& cl : content_lines) {
                repeated_lines.push_back(cl);
            }
        }

        // Rebuild text with inserted lines
        std::string new_text;
        for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
            if (i == insert_line) {
                for (auto& rl : repeated_lines) {
                    new_text += std::string(rl) + '\n';
                }
            }
            new_text += std::string(lines[i]);
            if (i < static_cast<int>(lines.size()) - 1) {
                new_text += '\n';
            }
        }
        // Handle paste at end
        if (insert_line >= static_cast<int>(lines.size())) {
            for (auto& rl : repeated_lines) {
                new_text += '\n';
                new_text += std::string(rl);
            }
        }

        ctx.set_text(new_text);
        auto new_lines = detail::split_lines(new_text);
        ctx.set_offset(detail::get_line_start_offset(new_lines, insert_line));
    } else {
        // Characterwise paste
        std::string text_to_insert;
        for (int i = 0; i < count; ++i) {
            text_to_insert += content;
        }

        int insert_point = after
            ? std::min(ctx.cursor_offset + 1, static_cast<int>(ctx.text.size()))
            : ctx.cursor_offset;

        std::string new_text = ctx.text.substr(0, insert_point)
                             + text_to_insert
                             + ctx.text.substr(insert_point);

        int new_offset = insert_point + static_cast<int>(text_to_insert.size()) - 1;
        new_offset = std::max(insert_point, new_offset);

        ctx.set_text(new_text);
        ctx.set_offset(new_offset);
    }
}

/// Execute indent/dedent (>>/<<).
///
/// @param dir    Indent direction (Right for >>, Left for <<)
/// @param count  Number of lines to indent
/// @param ctx    Operator execution context
inline auto execute_indent(IndentDir dir, int count, OperatorContext& ctx) -> void {
    auto lines = detail::split_lines(ctx.text);
    int current_line = detail::offset_to_line(ctx.text, ctx.cursor_offset);
    int lines_to_affect = std::min(count, static_cast<int>(lines.size()) - current_line);

    // Convert to mutable strings
    std::vector<std::string> mut_lines;
    mut_lines.reserve(lines.size());
    for (auto& l : lines) mut_lines.emplace_back(l);

    constexpr std::string_view indent = "  "; // Two spaces

    for (int i = 0; i < lines_to_affect; ++i) {
        int idx = current_line + i;
        auto& line = mut_lines[idx];

        if (dir == IndentDir::Right) {
            line = std::string(indent) + line;
        } else {
            // Dedent: remove leading whitespace up to indent width
            if (line.size() >= indent.size()
                && line.substr(0, indent.size()) == indent) {
                line = line.substr(indent.size());
            } else if (!line.empty() && line[0] == '\t') {
                line = line.substr(1);
            } else {
                // Remove as much leading whitespace as possible up to indent length
                int removed = 0;
                while (removed < static_cast<int>(indent.size())
                       && removed < static_cast<int>(line.size())
                       && (line[removed] == ' ' || line[removed] == '\t')) {
                    ++removed;
                }
                line = line.substr(removed);
            }
        }
    }

    // Rebuild text
    std::string new_text;
    for (int i = 0; i < static_cast<int>(mut_lines.size()); ++i) {
        if (i > 0) new_text += '\n';
        new_text += mut_lines[i];
    }

    // Cursor on first non-blank of current line
    auto new_lines = detail::split_lines(new_text);
    int line_offset = detail::get_line_start_offset(new_lines, current_line);
    int fnb = detail::first_non_blank(new_lines[current_line]);

    ctx.set_text(new_text);
    ctx.set_offset(line_offset + fnb);
    ctx.record_change(ChangeIndent{dir, count});
}

/// Execute open line (o/O command).
///
/// @param direction  Above or Below
/// @param ctx        Operator execution context
inline auto execute_open_line(OpenLineDir direction, OperatorContext& ctx) -> void {
    auto lines = detail::split_lines(ctx.text);
    int current_line = detail::offset_to_line(ctx.text, ctx.cursor_offset);
    int insert_line = (direction == OpenLineDir::Below) ? current_line + 1 : current_line;

    // Insert an empty line at the target position
    std::vector<std::string> new_lines_vec;
    new_lines_vec.reserve(lines.size() + 1);
    for (auto& l : lines) new_lines_vec.emplace_back(l);
    new_lines_vec.insert(new_lines_vec.begin() + insert_line, "");

    // Rebuild text from lines
    std::string new_text;
    for (int i = 0; i < static_cast<int>(new_lines_vec.size()); ++i) {
        if (i > 0) new_text += '\n';
        new_text += new_lines_vec[i];
    }

    ctx.set_text(new_text);
    auto final_lines = detail::split_lines(new_text);
    ctx.enter_insert(detail::get_line_start_offset(final_lines, insert_line));
    ctx.record_change(ChangeOpenLine{direction});
}

/// Execute an operator targeting to end of file (dG, cG, yG).
///
/// @param op     The operator
/// @param count  Line number target (1 = no count given, meaning EOF)
/// @param ctx    Operator execution context
inline auto execute_operator_G(Operator op, int count, OperatorContext& ctx) -> void {
    std::string_view text = ctx.text;
    int len = static_cast<int>(text.size());

    // Determine target offset
    int target;
    if (count == 1) {
        // No count: go to last line start
        auto pos = text.rfind('\n');
        target = (pos == std::string_view::npos) ? 0 : static_cast<int>(pos) + 1;
    } else {
        // Go to line N
        auto lines = detail::split_lines(text);
        int target_line = std::clamp(count - 1, 0, static_cast<int>(lines.size()) - 1);
        target = detail::get_line_start_offset(lines, target_line);
    }

    if (target == ctx.cursor_offset) return;

    int from = std::min(ctx.cursor_offset, target);
    int to = std::max(ctx.cursor_offset, target);

    // Linewise: extend to full lines
    auto next_nl = text.find('\n', to);
    if (next_nl == std::string_view::npos) {
        to = len;
        if (from > 0 && text[from - 1] == '\n') --from;
    } else {
        to = static_cast<int>(next_nl) + 1;
    }

    detail::apply_operator(op, from, to, ctx, true);
    ctx.record_change(ChangeOperator{op, "G", count});
}

/// Execute an operator targeting to beginning of file (dgg, cgg, ygg).
///
/// @param op     The operator
/// @param count  Line number target (1 = no count given, meaning line 1)
/// @param ctx    Operator execution context
inline auto execute_operator_gg(Operator op, int count, OperatorContext& ctx) -> void {
    std::string_view text = ctx.text;

    // Determine target offset
    int target;
    if (count == 1) {
        target = 0; // First line
    } else {
        auto lines = detail::split_lines(text);
        int target_line = std::clamp(count - 1, 0, static_cast<int>(lines.size()) - 1);
        target = detail::get_line_start_offset(lines, target_line);
    }

    if (target == ctx.cursor_offset) return;

    int from = std::min(ctx.cursor_offset, target);
    int to = std::max(ctx.cursor_offset, target);

    // Linewise: extend to full lines
    auto next_nl = text.find('\n', to);
    if (next_nl == std::string_view::npos) {
        to = static_cast<int>(text.size());
        if (from > 0 && text[from - 1] == '\n') --from;
    } else {
        to = static_cast<int>(next_nl) + 1;
    }

    detail::apply_operator(op, from, to, ctx, true);
    ctx.record_change(ChangeOperator{op, "gg", count});
}

/// Execute an operator with a find motion (df, dt, cF, etc.).
///
/// @param op        The operator
/// @param find_type The find direction/style
/// @param ch        The target character
/// @param count     Repeat count
/// @param ctx       Operator execution context
inline auto execute_operator_find(
    Operator op,
    FindType find_type,
    char ch,
    int count,
    OperatorContext& ctx
) -> void {
    std::string_view text = ctx.text;
    int len = static_cast<int>(text.size());
    int cursor = ctx.cursor_offset;

    // Find the target character
    std::optional<int> target_offset;
    int found = 0;

    if (find_type == FindType::f || find_type == FindType::t) {
        // Search forward
        for (int i = cursor + 1; i < len && found < count; ++i) {
            if (text[i] == ch) {
                ++found;
                if (found == count) {
                    target_offset = (find_type == FindType::t) ? i - 1 : i;
                }
            }
        }
    } else {
        // Search backward (F, T)
        for (int i = cursor - 1; i >= 0 && found < count; --i) {
            if (text[i] == ch) {
                ++found;
                if (found == count) {
                    target_offset = (find_type == FindType::T) ? i + 1 : i;
                }
            }
        }
    }

    if (!target_offset) return;

    int from = std::min(cursor, *target_offset);
    int to = std::max(cursor, *target_offset) + 1; // inclusive

    detail::apply_operator(op, from, to, ctx);
    ctx.set_last_find(find_type, ch);
    ctx.record_change(ChangeOperatorFind{op, find_type, ch, count});
}

} // namespace cc::vim
