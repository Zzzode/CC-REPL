// markdown_lexer_impl.cpp - impl unit for cc.ui.visual.markdown (RFC 0001
// Phase C batch 6). Holds the block/inline lexer: tokenize_inline, the GFM
// table helpers, ordered-list detection, split_lines and lex_blocks - moved
// out of the interface BMI.
module;

#include <cctype>

module cc.ui.visual.markdown;

import std;

namespace cc::ui {
namespace detail {

[[nodiscard]] std::vector<InlineToken> tokenize_inline(std::string_view text) {
    std::vector<InlineToken> tokens;
    std::string buffer;
    buffer.reserve(text.size());

    auto flush_buffer = [&]() {
        if (!buffer.empty()) {
            tokens.push_back({InlineTokenKind::Text, std::move(buffer), {}});
            buffer.clear();
        }
    };

    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];

        // Escape: \* \` \[ etc.
        if (c == '\\' && i + 1 < text.size()) {
            char next = text[i + 1];
            if (next == '*' || next == '_' || next == '`' ||
                next == '[' || next == ']' || next == '(' ||
                next == ')' || next == '#' || next == '+' ||
                next == '-' || next == '.' || next == '!' ||
                next == '>' || next == '~' || next == '|' ||
                next == '$') {
                buffer += next;
                i += 1;
                continue;
            }
        }

        // Note: strikethrough (~~text~~) is intentionally NOT parsed.
        // The model often uses ~ for "approximate" (e.g., ~100) and rarely
        // intends actual strikethrough formatting. Mirrors the TS marked
        // configuration: marked.use({ tokenizer: { del() { return undefined } } }).

        // Inline code: `code`
        if (c == '`') {
            flush_buffer();
            auto end = text.find('`', i + 1);
            if (end != std::string_view::npos) {
                auto code = text.substr(i + 1, end - i - 1);
                tokens.push_back({InlineTokenKind::Code,
                                  std::string(code), {}});
                i = end;
                continue;
            }
        }

        // Bold: **text** or __text__
        if ((c == '*' && i + 1 < text.size() && text[i + 1] == '*') ||
            (c == '_' && i + 1 < text.size() && text[i + 1] == '_')) {
            flush_buffer();
            char marker = c;
            auto end = text.find(std::string(2, marker), i + 2);
            if (end != std::string_view::npos) {
                auto content = text.substr(i + 2, end - i - 2);
                tokens.push_back({InlineTokenKind::Bold,
                                  std::string(content), {}});
                i = end + 1;
                continue;
            }
        }

        // Italic: *text* or _text_
        // Opening marker must not be preceded by an alphanumeric (avoids
        // intra-word emphasis like `foo*bar`). The closing marker must not
        // be followed by whitespace and the content must be non-empty. We do
        // NOT require the char before the closing marker to be non-alphanumeric
        // (marked allows `*italic*` where `c` precedes the closing `*`).
        if ((c == '*' || c == '_') &&
            (i == 0 || !std::isalnum(static_cast<unsigned char>(text[i - 1])))) {
            flush_buffer();
            char marker = c;
            auto end = text.find(marker, i + 1);
            if (end != std::string_view::npos && end > i + 1) {
                // Closing marker must not be followed by whitespace (CommonMark
                // right-flanking rule for the close).
                bool close_ok = (end + 1 == text.size()) ||
                                !std::isspace(static_cast<unsigned char>(
                                    text[end + 1]));
                if (close_ok) {
                    auto content = text.substr(i + 1, end - i - 1);
                    tokens.push_back({InlineTokenKind::Italic,
                                      std::string(content), {}});
                    i = end;
                    continue;
                }
            }
        }

        // Inline math: $...$ (LaTeX math expression)
        // Opening $ must not be followed by whitespace or another $ (to
        // avoid matching $$...$$ block math inline — that's handled in
        // lex_blocks).  Closing $ must not be preceded by whitespace.
        // Mirrors marked-katex-extension / marked-math behavior.
        if (c == '$' && i + 1 < text.size() && text[i + 1] != '$' &&
            text[i + 1] != ' ' && text[i + 1] != '\t') {
            flush_buffer();
            auto end = text.find('$', i + 1);
            if (end != std::string_view::npos && end > i + 1 &&
                text[end - 1] != ' ' && text[end - 1] != '\t') {
                auto math = text.substr(i + 1, end - i - 1);
                tokens.push_back({InlineTokenKind::Math,
                                  std::string(math), {}});
                i = end;
                continue;
            }
        }

        // Link: [text](url)
        if (c == '[') {
            auto bracket_end = text.find(']', i + 1);
            if (bracket_end != std::string_view::npos &&
                bracket_end + 1 < text.size() &&
                text[bracket_end + 1] == '(') {
                auto paren_end = text.find(')', bracket_end + 2);
                if (paren_end != std::string_view::npos) {
                    flush_buffer();
                    auto link_text = text.substr(i + 1, bracket_end - i - 1);
                    auto url = text.substr(bracket_end + 2,
                                           paren_end - bracket_end - 2);
                    tokens.push_back({InlineTokenKind::Link,
                                      std::string(link_text),
                                      std::string(url)});
                    i = paren_end;
                    continue;
                }
            }
        }

        buffer += c;
    }
    flush_buffer();
    return tokens;
}

[[nodiscard]] std::string trim_cell(std::string_view value) {
    auto begin = value.find_first_not_of(" \t");
    if (begin == std::string_view::npos) return "";
    auto end = value.find_last_not_of(" \t");
    return std::string(value.substr(begin, end - begin + 1));
}

[[nodiscard]] std::vector<std::string> split_table_row(std::string_view line) {
    if (!line.empty() && line.front() == '|') line.remove_prefix(1);
    if (!line.empty() && line.back() == '|') line.remove_suffix(1);

    std::vector<std::string> cells;
    std::size_t start = 0;
    while (start <= line.size()) {
        auto end = line.find('|', start);
        auto cell = end == std::string_view::npos
            ? line.substr(start)
            : line.substr(start, end - start);
        cells.push_back(trim_cell(cell));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return cells;
}

[[nodiscard]] bool is_table_separator(std::string_view line) {
    auto cells = split_table_row(line);
    if (cells.empty()) return false;
    for (const auto& raw_cell : cells) {
        if (raw_cell.empty()) return false;
        auto cell = std::string_view(raw_cell);
        if (!cell.empty() && cell.front() == ':') cell.remove_prefix(1);
        if (!cell.empty() && cell.back() == ':') cell.remove_suffix(1);
        if (cell.size() < 3) return false;
        for (char ch : cell) {
            if (ch != '-') return false;
        }
    }
    return true;
}

[[nodiscard]] int count_list_indent(std::string_view line) {
    std::size_t pos = 0;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
        ++pos;
    }
    // TS marked: each 2 spaces = 1 depth level. Tabs count as 2 spaces.
    int spaces = 0;
    for (std::size_t i = 0; i < pos; ++i) {
        spaces += (line[i] == '\t') ? 2 : 1;
    }
    return spaces / 2;  // 0, 1, 2, ... depth levels
}

[[nodiscard]] std::optional<std::pair<int, std::string>>
parse_ordered_list(std::string_view line) {
    // Skip leading whitespace to support indented (nested) list items.
    std::size_t pos = 0;
    while (pos < line.size() &&
           (line[pos] == ' ' || line[pos] == '\t')) ++pos;

    std::size_t num_start = pos;
    while (pos < line.size() &&
           std::isdigit(static_cast<unsigned char>(line[pos]))) ++pos;
    if (pos == num_start || pos >= line.size() || line[pos] != '.') return std::nullopt;
    if (pos + 1 >= line.size() ||
        !std::isspace(static_cast<unsigned char>(line[pos + 1]))) {
        return std::nullopt;
    }
    std::size_t text_start = pos + 2;
    while (text_start < line.size() &&
           std::isspace(static_cast<unsigned char>(line[text_start]))) {
        ++text_start;
    }
    int num = 0;
    auto num_str = line.substr(num_start, pos - num_start);
    for (char ch : num_str) {
        num = num * 10 + (ch - '0');
    }
    return std::pair{num, std::string(line.substr(text_start))};
}

[[nodiscard]] std::vector<std::string> split_lines(std::string_view source) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < source.size()) {
        auto end = source.find('\n', start);
        lines.emplace_back(end == std::string_view::npos
                           ? source.substr(start)
                           : source.substr(start, end - start));
        start = (end == std::string_view::npos) ? source.size() : end + 1;
    }
    return lines;
}

[[nodiscard]] std::vector<BlockToken> lex_blocks(std::string_view source) {
    std::vector<BlockToken> tokens;
    auto lines = split_lines(source);

    // Current paragraph accumulator (raw lines joined by space)
    std::string para_buffer;
    bool in_para = false;

    auto flush_paragraph = [&]() {
        if (in_para && !para_buffer.empty()) {
            BlockToken tok;
            tok.kind = BlockTokenKind::Paragraph;
            tok.inlines = tokenize_inline(para_buffer);
            tokens.push_back(std::move(tok));
            para_buffer.clear();
            in_para = false;
        }
    };

    // Current list state
    bool in_ulist = false;
    bool in_olist = false;
    std::vector<std::vector<InlineToken>> list_items;
    std::vector<int> list_depths;

    auto flush_list = [&]() {
        if (!list_items.empty()) {
            BlockToken tok;
            tok.kind = in_ulist ? BlockTokenKind::UnorderedList
                                : BlockTokenKind::OrderedList;
            tok.list_items = std::move(list_items);
            tok.list_depths = std::move(list_depths);
            tokens.push_back(std::move(tok));
            list_items.clear();
            list_depths.clear();
            in_ulist = false;
            in_olist = false;
        }
    };

    // Current blockquote state
    bool in_quote = false;
    std::string quote_buffer;

    auto flush_quote = [&]() {
        if (in_quote && !quote_buffer.empty()) {
            BlockToken tok;
            tok.kind = BlockTokenKind::Blockquote;
            tok.inlines = tokenize_inline(quote_buffer);
            tok.quote_depth = 1;
            tokens.push_back(std::move(tok));
            quote_buffer.clear();
            in_quote = false;
        }
    };

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];

        // Skip empty lines for list continuation detection
        if (line.empty()) {
            flush_paragraph();
            flush_list();
            flush_quote();
            continue;
        }

        // Fenced code block
        if (line.starts_with("```")) {
            flush_paragraph();
            flush_list();
            flush_quote();

            auto lang_str = line.substr(3);
            // Trim whitespace from lang
            auto lb = lang_str.find_first_not_of(" \t");
            if (lb != std::string::npos) {
                auto le = lang_str.find_last_not_of(" \t");
                lang_str = lang_str.substr(lb, le - lb + 1);
            } else {
                lang_str = "";
            }

            std::string code;
            ++i;
            while (i < lines.size() && !lines[i].starts_with("```")) {
                if (!code.empty()) code += '\n';
                code += lines[i];
                ++i;
            }
            BlockToken tok;
            tok.kind = BlockTokenKind::CodeBlock;
            tok.code_lang = std::string(lang_str);
            tok.code_content = std::move(code);
            tokens.push_back(std::move(tok));
            continue;
        }

        // Block math: $...$ (LaTeX display math)
        // Mirrors marked-math extension: $$ on its own line starts a display
        // math block; closing $$ on its own line ends it.  Content between
        // is captured verbatim (no inline processing).
        if (line.starts_with("$")) {
            flush_paragraph();
            flush_list();
            flush_quote();

            std::string math;
            // Check if content is on the same line: $...$
            auto rest = line.substr(2);
            auto close_pos = rest.find("$");
            if (close_pos != std::string::npos) {
                // Single-line block math: $formula$
                math = rest.substr(0, close_pos);
            } else {
                // Multi-line: $ on opening line, content on subsequent lines,
                // $ on closing line.
                if (!rest.empty()) {
                    math = rest;
                }
                ++i;
                while (i < lines.size() && !lines[i].starts_with("$")) {
                    if (!math.empty()) math += '\n';
                    math += lines[i];
                    ++i;
                }
                // If we found the closing $ line, also grab any trailing
                // content after it on the same line (usually empty).
                if (i < lines.size()) {
                    auto tail = lines[i].substr(2);
                    if (!tail.empty()) {
                        if (!math.empty()) math += '\n';
                        math += tail;
                    }
                }
            }

            BlockToken tok;
            tok.kind = BlockTokenKind::MathBlock;
            tok.math_content = std::move(math);
            tokens.push_back(std::move(tok));
            continue;
        }

        // GFM Table: header row followed by alignment separator
        if (line.find('|') != std::string::npos && i + 1 < lines.size() &&
            is_table_separator(lines[i + 1])) {
            flush_paragraph();
            flush_list();
            flush_quote();

            BlockToken tok;
            tok.kind = BlockTokenKind::Table;
            tok.table_headers = split_table_row(line);
            i += 2;
            while (i < lines.size() &&
                   lines[i].find('|') != std::string::npos &&
                   !lines[i].empty()) {
                tok.table_rows.push_back(split_table_row(lines[i]));
                ++i;
            }
            --i;
            tokens.push_back(std::move(tok));
            continue;
        }

        // Headings (ATX style: # H1, ## H2, etc.)
        if (line.starts_with('#')) {
            flush_paragraph();
            flush_list();
            flush_quote();

            int level = 0;
            while (level < static_cast<int>(line.size()) &&
                   line[level] == '#') ++level;
            if (level <= 6 && level < static_cast<int>(line.size()) &&
                line[level] == ' ') {
                std::string heading_text = line.substr(level + 1);
                // Trim trailing # (closing sequence)
                auto rtrim = heading_text.find_last_not_of(" #");
                if (rtrim != std::string::npos) {
                    heading_text = heading_text.substr(0, rtrim + 1);
                }
                BlockToken tok;
                tok.kind = BlockTokenKind::Heading;
                tok.heading_level = level;
                tok.inlines = tokenize_inline(heading_text);
                tokens.push_back(std::move(tok));
                continue;
            }
        }

        // Horizontal rule
        if ((line == "---" || line == "***" || line == "___") &&
            line.size() >= 3) {
            // Verify all same chars
            char first = line[0];
            bool all_same = true;
            for (char c : line) {
                if (c != first) { all_same = false; break; }
            }
            if (all_same) {
                flush_paragraph();
                flush_list();
                flush_quote();
                BlockToken tok;
                tok.kind = BlockTokenKind::HorizontalRule;
                tokens.push_back(std::move(tok));
                continue;
            }
        }

        // Unordered list item (supports indented/nested items).
        // TS marked tokenizer: detects `- `, `* `, `+ ` at any indent level,
        // with `item.depth` = floor(leading_spaces / 2).
        {
            std::size_t ul_pos = 0;
            while (ul_pos < line.size() &&
                   (line[ul_pos] == ' ' || line[ul_pos] == '\t')) ++ul_pos;
            std::string_view line_sv(line);
            std::string_view stripped = line_sv.substr(ul_pos);
            int ul_depth = count_list_indent(line);
            if ((stripped.starts_with("- ") || stripped.starts_with("* ") ||
                 stripped.starts_with("+ ")) && stripped.size() >= 2) {
                flush_paragraph();
                flush_quote();
                if (!in_ulist) {
                    flush_list();
                    in_ulist = true;
                    in_olist = false;
                }
                list_items.push_back(tokenize_inline(stripped.substr(2)));
                list_depths.push_back(ul_depth);
                continue;
            }
        }

        // Ordered list item (supports indented/nested items).
        if (auto item = parse_ordered_list(line)) {
            flush_paragraph();
            flush_quote();
            if (!in_olist) {
                flush_list();
                in_olist = true;
                in_ulist = false;
            }
            list_items.push_back(tokenize_inline(item->second));
            list_depths.push_back(count_list_indent(line));
            continue;
        }

        // Blockquote
        if (line.starts_with("> ")) {
            flush_paragraph();
            flush_list();
            if (in_quote) quote_buffer += ' ';
            in_quote = true;
            quote_buffer += line.substr(2);
            continue;
        }
        // Blockquote with just >
        if (line == ">") {
            flush_paragraph();
            flush_list();
            if (in_quote && !quote_buffer.empty()) {
                // New paragraph in quote → flush current
                flush_quote();
            }
            in_quote = true;
            continue;
        }

        // Regular paragraph text
        if (in_para) para_buffer += ' ';
        in_para = true;
        para_buffer += line;
    }

    flush_paragraph();
    flush_list();
    flush_quote();
    return tokens;
}
} // namespace detail
} // namespace cc::ui
