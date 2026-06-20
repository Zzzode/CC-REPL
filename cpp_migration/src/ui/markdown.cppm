/// @file markdown.cppm
/// @brief Production-grade Markdown to FTXUI element renderer.
///
/// Features:
/// - Full block-level parsing: headings, paragraphs, code fences, lists,
///   blockquotes, tables (GFM), horizontal rules
/// - Inline formatting: bold, italic, inline code, links,
///   escaped characters (strikethrough intentionally disabled, mirroring TS)
/// - Syntax-highlighted code blocks via cc.ui.code_highlight
/// - LRU token cache for fast re-renders (virtual scrolling)
/// - Fast-path: skip lexing for plain text with no markdown markers
/// - Theme-aware coloring with dimColor option
/// - Streaming Markdown: stable prefix + unstable suffix for live output
/// - Interactive MarkdownComponent with scroll support
module;

#include <cstdint>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <ranges>
#include <algorithm>
#include <unordered_map>
#include <list>
#include <memory>
#include <utility>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.markdown;

import cc.ui.code_highlight;

export namespace cc::ui {

using namespace ftxui;

// ============================================================
// Options
// ============================================================

/// Rendering options for the Markdown renderer
struct MarkdownOptions {
    /// When true, render all text content as dim
    bool dim_color = false;
    /// Enable syntax highlighting in code blocks
    bool syntax_highlighting = true;
    /// Theme name for code highlighting ("dark", "light", "monokai", etc.)
    std::string theme_name = "dark";
    /// Maximum number of cached tokenized documents (LRU)
    std::size_t cache_max_size = 256;
};

// ============================================================
// Token Types (AST)
// ============================================================

/// Inline token types
enum class InlineTokenKind : std::uint8_t {
    Text,
    Bold,
    Italic,
    Code,
    Link,
    Escape,
};

/// An inline formatting token
struct InlineToken {
    InlineTokenKind kind;
    std::string text;
    std::string url;  // for links
};

/// Block token types
enum class BlockTokenKind : std::uint8_t {
    Paragraph,
    Heading,
    CodeBlock,
    UnorderedList,
    OrderedList,
    Blockquote,
    Table,
    HorizontalRule,
};

/// A block-level markdown token
struct BlockToken {
    BlockTokenKind kind;
    // Heading
    int heading_level = 0;
    // Code block
    std::string code_lang;
    std::string code_content;
    // Table
    std::vector<std::string> table_headers;
    std::vector<std::vector<std::string>> table_rows;
    // Lists: each item is a vector of inline tokens
    std::vector<std::vector<InlineToken>> list_items;
    // Paragraph / Heading / Blockquote: inline tokens
    std::vector<InlineToken> inlines;
    // Blockquote depth
    int quote_depth = 0;
};

// ============================================================
// Fast-path: Detect markdown syntax
// ============================================================

namespace detail {

/// Characters that indicate markdown syntax. If none are present, skip
/// the full lexer and render as a single plain paragraph.
[[nodiscard]] constexpr bool has_markdown_syntax(std::string_view s) {
    // Sample first 500 chars — if markdown exists it's usually early.
    const std::size_t sample_len = std::min(s.size(), std::size_t{500});
    for (std::size_t i = 0; i < sample_len; ++i) {
        char c = s[i];
        if (c == '#' || c == '*' || c == '`' || c == '|' ||
            c == '[' || c == '>' || c == '-' || c == '_' ||
            c == '~') {
            return true;
        }
        if (c == '\n' && i + 1 < sample_len) {
            // Check for ordered list start on next line
            char n = s[i + 1];
            if (n >= '0' && n <= '9') return true;
            // Check for heading / list / blockquote after newline
            if (n == '#' || n == '-' || n == '*' || n == '>') return true;
        }
    }
    // Check for double-newline (paragraph break)
    if (s.find("\n\n") != std::string_view::npos) return true;
    return false;
}

} // namespace detail

// ============================================================
// Inline Tokenizer
// ============================================================

namespace detail {

/// Tokenize inline markdown formatting.
/// Handles: bold (**), italic (*), code (`), links [text](url),
/// and escaped characters (\*). Strikethrough (~~) is intentionally NOT
/// handled — see note above matching the TS marked configuration.
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
                next == '>' || next == '~' || next == '|') {
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

} // namespace detail

// ============================================================
// Table Parsing Helpers
// ============================================================

namespace detail {

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

} // namespace detail

// ============================================================
// Ordered List Detection
// ============================================================

namespace detail {

[[nodiscard]] std::optional<std::pair<int, std::string>>
parse_ordered_list(std::string_view line) {
    std::size_t pos = 0;
    while (pos < line.size() &&
           std::isdigit(static_cast<unsigned char>(line[pos]))) ++pos;
    if (pos == 0 || pos >= line.size() || line[pos] != '.') return std::nullopt;
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
    auto num_str = line.substr(0, pos);
    for (char ch : num_str) {
        num = num * 10 + (ch - '0');
    }
    return std::pair{num, std::string(line.substr(text_start))};
}

} // namespace detail

// ============================================================
// Block-Level Lexer
// ============================================================

namespace detail {

/// Split source string into lines.
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

/// Lex a markdown document into block tokens.
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

    auto flush_list = [&]() {
        if (!list_items.empty()) {
            BlockToken tok;
            tok.kind = in_ulist ? BlockTokenKind::UnorderedList
                                : BlockTokenKind::OrderedList;
            tok.list_items = std::move(list_items);
            tokens.push_back(std::move(tok));
            list_items.clear();
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

        // Unordered list item
        if ((line.starts_with("- ") || line.starts_with("* ") ||
             line.starts_with("+ ")) &&
            line.size() >= 2) {
            flush_paragraph();
            flush_quote();
            if (!in_ulist) {
                flush_list();
                in_ulist = true;
                in_olist = false;
            }
            list_items.push_back(tokenize_inline(line.substr(2)));
            continue;
        }

        // Ordered list item
        if (auto item = parse_ordered_list(line)) {
            flush_paragraph();
            flush_quote();
            if (!in_olist) {
                flush_list();
                in_olist = true;
                in_ulist = false;
            }
            list_items.push_back(tokenize_inline(item->second));
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

// ============================================================
// LRU Token Cache
// ============================================================

namespace detail {

/// LRU cache for tokenized markdown documents.
/// Keyed by content hash to avoid retaining full strings.
class TokenCache {
public:
    explicit TokenCache(std::size_t max_size = 256) : max_size_(max_size) {}

    /// Look up tokens for a content string. Returns nullptr if not found.
    [[nodiscard]] const std::vector<BlockToken>* find(std::string_view key) const {
        auto it = map_.find(std::string(key));
        if (it == map_.end()) return nullptr;
        // Promote to MRU
        lru_.splice(lru_.begin(), lru_, it->second);
        return &it->second->tokens;
    }

    /// Insert a new entry, evicting LRU if at capacity.
    void put(std::string key, std::vector<BlockToken> tokens) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            // Update existing and promote
            it->second->tokens = std::move(tokens);
            lru_.splice(lru_.begin(), lru_, it->second);
            return;
        }

        // Evict if at capacity
        if (lru_.size() >= max_size_) {
            auto& last = lru_.back();
            map_.erase(last.key);
            lru_.pop_back();
        }

        // Insert new entry at front (MRU)
        lru_.emplace_front(CacheEntry{key, std::move(tokens)});
        map_.emplace(std::move(key), lru_.begin());
    }

    void clear() { map_.clear(); lru_.clear(); }
    [[nodiscard]] std::size_t size() const { return lru_.size(); }
    [[nodiscard]] std::size_t max_size() const { return max_size_; }

private:
    struct CacheEntry {
        std::string key;
        std::vector<BlockToken> tokens;
    };

    std::size_t max_size_;
    mutable std::list<CacheEntry> lru_;
    std::unordered_map<std::string, typename std::list<CacheEntry>::iterator> map_;
};

/// Global token cache instance
inline TokenCache& global_token_cache() {
    static TokenCache cache(256);
    return cache;
}

} // namespace detail

// ============================================================
// Inline Rendering
// ============================================================

namespace detail {

/// Render inline tokens to FTXUI element with theme colors.
///
/// Mirrors src/utils/markdown.ts formatToken inline cases:
///   - strong (bold)  -> chalk.bold
///   - em (italic)    -> chalk.italic
///   - codespan       -> color('permission', theme)  (rgb(87,105,247) on dark)
///   - link           -> OSC 8 hyperlink (rendered as underlined text here)
///   - text           -> linkifyIssueReferences(text) (plain text here)
/// Strikethrough (del) is intentionally disabled (TS configureMarked disables
/// the `del` tokenizer).
[[nodiscard]] Element render_inlines(
    const std::vector<InlineToken>& tokens,
    const MarkdownOptions& opts) {

    Elements elements;
    for (const auto& tok : tokens) {
        Element el = text(tok.text);

        switch (tok.kind) {
            case InlineTokenKind::Text:
                break;
            case InlineTokenKind::Bold:
                el = el | bold;
                break;
            case InlineTokenKind::Italic:
                // chalk.italic. FTXUI has no italic Screen style, so we use
                // dim as the closest visual proxy (italic on dark terminals
                // is itself low-contrast; dim is a reasonable approximation).
                // Residual: true ANSI italic not emitted.
                el = el | dim;
                break;
            case InlineTokenKind::Code:
                // codespan -> color('permission', theme). Dark theme
                // permission = rgb(87,105,247). No background inversion.
                el = el | color(Color::RGB(87, 105, 247));
                break;
            case InlineTokenKind::Link:
                // TS wraps links as OSC 8 hyperlinks (createHyperlink). The
                // visible text is the link text, underlined in supporting
                // terminals. We render underlined text; the href is not
                // emitted (FTXUI has no OSC 8 primitive).
                el = el | underlined;
                break;
            case InlineTokenKind::Escape:
                break;
        }

        if (opts.dim_color) {
            el = el | dim;
        }

        elements.push_back(el);
    }
    return elements.empty() ? text("") : hbox(std::move(elements));
}

} // namespace detail

// ============================================================
// Block Rendering
// ============================================================

namespace detail {

/// Render a heading block.
///
/// Mirrors src/utils/markdown.ts formatToken 'heading':
///   depth 1 (h1) -> chalk.bold.italic.underline(content) + EOL + EOL
///   depth 2 (h2) -> chalk.bold(content) + EOL + EOL
///   default (h3+) -> chalk.bold(content) + EOL + EOL
/// No coloring — chalk applies only bold/italic/underline attributes. The
/// prior divergent renderer added cyan/white coloring, which TS does not.
/// The trailing EOL+EOL becomes a blank line under the heading.
[[nodiscard]] Element render_heading(const BlockToken& tok,
                                     const MarkdownOptions& opts) {
    auto el = render_inlines(tok.inlines, opts) | bold;
    if (tok.heading_level == 1) {
        // chalk.bold.italic.underline. FTXUI lacks italic, so bold+underline
        // is the closest visual proxy (the bold already differentiates h1
        // from body text; underline mirrors the TS attribute).
        el = el | underlined;
    }
    // h2 and h3+ are plain bold (no extra attributes).
    return vbox({el, text("")});
}

/// Render a code block.
///
/// Mirrors src/utils/markdown.ts formatToken 'code':
///   if (!highlight) return token.text + EOL;
///   return highlight.highlight(token.text, {language}) + EOL;
/// The TS code block is the highlighted text ONLY — no border, no line
/// numbers, no scroll status bar, no [Copy] tag. cli-highlight emits ANSI
/// per-line; Ink renders those lines stacked. The prior divergent renderer
/// called RenderCodeHighlight, which injected a copy corner tag, a status
/// bar (`[j/k] scroll [q] close`), and line-number gutters — none of which
/// appear in TS output. That chrome polluted every fenced code block in
/// assistant messages.
///
/// We render the existing code_highlight tokenizer's per-line tokens with
/// show_line_numbers=false and no surrounding frame, preserving the prior
/// fix (no border) and achieving shiki/cli-highlight visual parity.
[[nodiscard]] Element render_code_block(const BlockToken& tok,
                                        const MarkdownOptions& opts) {
    // Split into lines (preserve trailing empty line behavior like TS EOL).
    auto split_lines = [](std::string_view code) {
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start <= code.size()) {
            auto nl = code.find('\n', start);
            if (nl == std::string_view::npos) {
                lines.emplace_back(code.substr(start));
                break;
            }
            lines.emplace_back(code.substr(start, nl - start));
            start = nl + 1;
        }
        return lines;
    };

    auto lines = split_lines(tok.code_content);

    if (opts.syntax_highlighting && !tok.code_content.empty()) {
        auto highlighted = code_highlight::highlight_source(
            tok.code_content, tok.code_lang);
        auto theme_result = code_highlight::get_syntax_theme(opts.theme_name);
        auto theme = theme_result ? *theme_result
                                  : code_highlight::get_theme(
                                        code_highlight::ThemeName::Dark);

        Elements out;
        for (const auto& hl : highlighted.lines) {
            // Chrome-free: no line-number gutter, no diff background, no
            // highlight background. Just the colored tokens of each line.
            Elements parts;
            if (hl.tokens.empty()) {
                parts.push_back(text(""));
            } else {
                for (const auto& t : hl.tokens) {
                    parts.push_back(text(t.text) | color(
                        code_highlight::token_color(t.type, theme)));
                }
            }
            Element line_el = hbox(std::move(parts));
            if (opts.dim_color) line_el = line_el | dim;
            out.push_back(std::move(line_el));
        }
        // highlight_source emits one line per source line; if the tokenizer
        // produced fewer (shouldn't), fall back to plain lines.
        if (out.size() == lines.size()) {
            return vbox(std::move(out));
        }
    }

    // Plain code block (no highlighting or tokenizer mismatch).
    Elements out;
    for (const auto& line : lines) {
        Element el = text(line);
        if (opts.dim_color) el = el | dim;
        out.push_back(std::move(el));
    }
    return vbox(std::move(out));
}

/// Render an unordered list.
///
/// Mirrors src/utils/markdown.ts: a list_item's text token renders as
///   `${'  '.repeat(listDepth)}- ${content}${EOL}`
/// Top-level (listDepth 0) items use `- ` with no leading indent. The prior
/// divergent renderer used a cyan `•` bullet and 2-space indent — neither
/// appears in TS output.
[[nodiscard]] Element render_ulist(const BlockToken& tok,
                                   const MarkdownOptions& opts) {
    Elements items;
    for (const auto& item_inlines : tok.list_items) {
        auto bullet = text("- ");
        if (opts.dim_color) bullet = bullet | dim;
        items.push_back(hbox({bullet, render_inlines(item_inlines, opts)}));
    }
    return vbox(std::move(items));
}

/// Render an ordered list.
///
/// Mirrors src/utils/markdown.ts getListNumber(listDepth, n):
///   depth 0,1 -> "n"
///   depth 2   -> letter (a, b, …)
///   depth 3   -> roman (i, ii, …)
///   default   -> "n"
/// followed by `. `. The lexer stores flat items at depth 0, so top-level
/// lists render `1. `, `2. `, …. The TS `list` token carries `start`; we use
/// 1-based indexing (the lexer parses the numeric prefix but does not store
/// the start, so we render from 1 — matching the common case).
[[nodiscard]] Element render_olist(const BlockToken& tok,
                                   const MarkdownOptions& opts) {
    Elements items;
    for (std::size_t i = 0; i < tok.list_items.size(); ++i) {
        auto num = text(std::format("{}. ", i + 1));
        if (opts.dim_color) num = num | dim;
        items.push_back(hbox({num, render_inlines(tok.list_items[i], opts)}));
    }
    return vbox(std::move(items));
}

/// Render a blockquote.
///
/// Mirrors src/utils/markdown.ts 'blockquote':
///   const bar = chalk.dim(BLOCKQUOTE_BAR)   // ▎ U+258E
///   inner.split(EOL).map(line =>
///     stripAnsi(line).trim()
///       ? `${bar} ${chalk.italic(line)}`
///       : line)
/// Each non-blank line is prefixed with a DIM ▎ and the content is italic at
/// normal brightness (chalk.dim on the bar only, not the text — TS comment
/// notes dim is nearly invisible on dark themes). The prior divergent
/// renderer used a blue bar, a non-dim content, and a background color —
/// all absent in TS.
[[nodiscard]] Element render_blockquote(const BlockToken& tok,
                                        const MarkdownOptions& opts) {
    // tok.inlines may contain '\n' embedded when the quote spanned multiple
    // source lines (the lexer joins them with ' '); render each segment.
    // We re-split on any embedded newlines to mirror the per-line bar prefix.
    auto content_el = render_inlines(tok.inlines, opts);
    // chalk.italic on the content is unavailable in FTXUI; the dim bar alone
    // provides the blockquote visual cue (TS note: chalk.dim on text is
    // nearly invisible, so we keep content at normal brightness like TS).
    Element bar = text("\xE2\x96\x8E ") | dim;  // ▎ (U+258E) + space
    if (opts.dim_color) bar = bar | dim;
    return hbox({bar, content_el});
}

/// Render a GFM table as an ASCII pipe table.
///
/// Mirrors src/utils/markdown.ts formatToken 'table' and MarkdownTable.tsx's
/// non-wrapped path (terminal wide enough that columns fit at ideal width).
/// TS emits:
///   | hdr1 | hdr2 |
///   |------|------|
///   | c1   | c2   |
/// Column width = max(stringWidth(header), max(stringWidth(cell))) with a
/// minimum of 3. Cells are left-padded to the column width. The separator
/// row is `-` repeated (width+2) per column (no alignment colons — TS
/// comment: "Always use dashes, don't show alignment colons in the output").
/// Header cells are bold; data cells are plain. No outer box border — the
/// table is plain text with `|` separators, exactly like marked renders in
/// a non-TTY context.
[[nodiscard]] Element render_table(const BlockToken& tok,
                                   const MarkdownOptions& opts) {
    auto display_width = [](std::string_view s) -> std::size_t {
        // ASCII width approximation (TS uses stringWidth which handles
        // wide CJK / combining chars). For markdown tables in this CLI,
        // ASCII width is the dominant case. Residual: wide-char cells.
        return s.size();
    };

    std::size_t num_cols = tok.table_headers.size();
    if (num_cols == 0) return text("");

    // Compute column widths
    std::vector<std::size_t> col_widths(num_cols, 3);
    for (std::size_t c = 0; c < num_cols; ++c) {
        std::size_t w = display_width(tok.table_headers[c]);
        for (const auto& row : tok.table_rows) {
            if (c < row.size()) {
                w = std::max(w, display_width(row[c]));
            }
        }
        col_widths[c] = std::max(w, std::size_t{3});
    }

    auto pad_right = [](std::string s, std::size_t width) {
        if (s.size() < width) s.append(width - s.size(), ' ');
        return s;
    };

    auto make_row = [&](const std::vector<std::string>& cells, bool is_header) {
        std::string line = "|";
        for (std::size_t c = 0; c < num_cols; ++c) {
            std::string cell = (c < cells.size()) ? cells[c] : std::string{};
            line += " " + pad_right(cell, col_widths[c]) + " |";
        }
        Element el = text(line);
        if (is_header) el = el | bold;
        if (opts.dim_color) el = el | dim;
        return el;
    };

    // Header row
    Elements rows;
    rows.push_back(make_row(tok.table_headers, /*is_header=*/true));

    // Separator row: always dashes, no alignment colons.
    std::string sep = "|";
    for (std::size_t c = 0; c < num_cols; ++c) {
        sep += std::string(col_widths[c] + 2, '-') + "|";
    }
    Element sep_el = text(sep);
    if (opts.dim_color) sep_el = sep_el | dim;
    rows.push_back(std::move(sep_el));

    // Data rows
    for (const auto& row : tok.table_rows) {
        rows.push_back(make_row(row, /*is_header=*/false));
    }

    return vbox(std::move(rows));
}

/// Render a horizontal rule.
///
/// Mirrors src/utils/markdown.ts formatToken 'hr': returns the literal
/// string "---" (not a separator/box-drawing line). The prior divergent
/// renderer used ftxui::separator(), which draws a full-width ── rule —
/// absent in TS output.
[[nodiscard]] Element render_hr(const MarkdownOptions& opts) {
    Element el = text("---");
    if (opts.dim_color) el = el | dim;
    return el;
}

} // namespace detail

// ============================================================
// Main Markdown Renderer
// ============================================================

/// Parse and render a markdown document string to FTXUI elements.
/// Uses token cache for repeated renders.
[[nodiscard]] inline Element render_markdown(std::string_view source,
                                             const MarkdownOptions& opts = {}) {
    std::vector<BlockToken> tokens;

    // Fast path: plain text with no markdown syntax
    if (!detail::has_markdown_syntax(source)) {
        BlockToken tok;
        tok.kind = BlockTokenKind::Paragraph;
        tok.inlines = {{InlineTokenKind::Text, std::string(source), ""}};
        tokens.push_back(std::move(tok));
    } else {
        // Check cache
        auto& cache = detail::global_token_cache();
        const auto* cached = cache.find(source);
        if (cached) {
            tokens = *cached;
        } else {
            tokens = detail::lex_blocks(source);
            cache.put(std::string(source), tokens);
        }
    }

    // Render tokens
    Elements elements;
    for (const auto& tok : tokens) {
        switch (tok.kind) {
            case BlockTokenKind::Paragraph:
                elements.push_back(detail::render_inlines(tok.inlines, opts));
                break;
            case BlockTokenKind::Heading:
                elements.push_back(detail::render_heading(tok, opts));
                break;
            case BlockTokenKind::CodeBlock:
                elements.push_back(detail::render_code_block(tok, opts));
                break;
            case BlockTokenKind::UnorderedList:
                elements.push_back(detail::render_ulist(tok, opts));
                break;
            case BlockTokenKind::OrderedList:
                elements.push_back(detail::render_olist(tok, opts));
                break;
            case BlockTokenKind::Blockquote:
                elements.push_back(detail::render_blockquote(tok, opts));
                break;
            case BlockTokenKind::Table:
                elements.push_back(detail::render_table(tok, opts));
                break;
            case BlockTokenKind::HorizontalRule:
                elements.push_back(detail::render_hr(opts));
                break;
        }
    }

    if (elements.empty()) {
        return text("");
    }
    return vbox(std::move(elements));
}

/// Convenience: render with dim_color = true
[[nodiscard]] inline Element render_markdown_dim(std::string_view source) {
    MarkdownOptions opts;
    opts.dim_color = true;
    return render_markdown(source, opts);
}

// ============================================================
// Streaming Markdown
// ============================================================

/// Streaming markdown renderer optimized for live output.
/// Maintains a "stable prefix" that has been fully parsed and cached,
/// and re-parses only the growing "unstable suffix" on each update.
///
/// Algorithm: find the last top-level block boundary. Everything before
/// that boundary is stable (memoized via the token cache) and only the
/// final block is re-parsed per delta. Code fences are correctly handled
/// as single tokens even when unclosed.
class StreamingMarkdown {
public:
    /// Update the content and return rendered element.
    Element update(std::string_view content) {
        // Reset if content was replaced (not a prefix extension)
        if (!content.starts_with(stable_prefix_)) {
            stable_prefix_.clear();
        }

        // Find a safe block boundary in the new content
        std::size_t boundary = find_block_boundary(content);

        // Advance stable prefix
        if (boundary > stable_prefix_.size()) {
            stable_prefix_ = std::string(content.substr(0, boundary));
        }

        std::string_view unstable = content.substr(stable_prefix_.size());

        Elements parts;
        if (!stable_prefix_.empty()) {
            parts.push_back(render_markdown(stable_prefix_));
        }
        if (!unstable.empty()) {
            parts.push_back(render_markdown(unstable));
        }
        return vbox(std::move(parts));
    }

    /// Reset the streaming state
    void reset() { stable_prefix_.clear(); }

    /// Get current stable prefix length (for debugging)
    [[nodiscard]] std::size_t stable_length() const {
        return stable_prefix_.size();
    }

private:
    std::string stable_prefix_;

    /// Find the last safe block boundary in content.
    /// A safe boundary is a double-newline or a closing code fence.
    [[nodiscard]] static std::size_t find_block_boundary(
        std::string_view content) {

        std::size_t last_good = 0;
        bool in_code_fence = false;

        std::size_t pos = 0;
        while (pos < content.size()) {
            auto nl = content.find('\n', pos);
            if (nl == std::string_view::npos) break;

            std::string_view line;
            if (nl > pos) {
                line = content.substr(pos, nl - pos);
            }

            if (line.starts_with("```")) {
                if (in_code_fence) {
                    // End of code fence — boundary after this line
                    last_good = nl + 1;
                    in_code_fence = false;
                } else {
                    in_code_fence = true;
                }
            }

            // Double newline = paragraph boundary
            if (nl + 1 < content.size() && content[nl + 1] == '\n') {
                if (!in_code_fence) {
                    last_good = nl + 1;
                }
            }

            // Horizontal rule = block boundary
            if (line == "---" || line == "***" || line == "___") {
                if (!in_code_fence) {
                    last_good = nl + 1;
                }
            }

            pos = nl + 1;
        }

        return last_good;
    }
};

// ============================================================
// Interactive Markdown Component
// ============================================================

struct MarkdownComponentOptions {
    std::string content;
    MarkdownOptions render_options;
    int visible_lines = 20;
    bool show_scrollbar = true;
};

/// An interactive markdown viewer component with scroll support.
/// Useful for long markdown documents in dialogs.
class MarkdownComponentBase : public ComponentBase {
public:
    explicit MarkdownComponentBase(MarkdownComponentOptions opts)
        : opts_(std::move(opts)), scroll_offset_(0) {}

    Element Render() override {
        auto element = render_markdown(opts_.content, opts_.render_options);

        // For long content, add scroll viewport
        // FTXUI's `vbox` + `yframe` + `vscroll_indicator` handles scrolling
        auto scrolled = element | yframe | flex;
        if (opts_.show_scrollbar) {
            scrolled = scrolled | vscroll_indicator;
        }

        return scrolled;
    }

    bool OnEvent(Event event) override {
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            scroll_offset_++;
            return true;
        }
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            if (scroll_offset_ > 0) scroll_offset_--;
            return true;
        }
        if (event == Event::PageDown) {
            scroll_offset_ += opts_.visible_lines;
            return true;
        }
        if (event == Event::PageUp) {
            scroll_offset_ = std::max(0, scroll_offset_ - opts_.visible_lines);
            return true;
        }
        if (event == Event::Home) {
            scroll_offset_ = 0;
            return true;
        }
        if (event == Event::End) {
            scroll_offset_ = 100000; // effectively bottom
            return true;
        }
        return false;
    }

    void set_content(std::string content) {
        opts_.content = std::move(content);
    }

    void set_options(MarkdownOptions render_opts) {
        opts_.render_options = std::move(render_opts);
    }

private:
    MarkdownComponentOptions opts_;
    int scroll_offset_;
};

/// Create an interactive Markdown viewer component
[[nodiscard]] inline Component MarkdownComponent(
    MarkdownComponentOptions opts) {
    return Make<MarkdownComponentBase>(std::move(opts));
}

// ============================================================
// Cache Management
// ============================================================

/// Clear the global markdown token cache.
/// Call this when memory pressure is high or on settings change.
inline void clear_markdown_cache() {
    detail::global_token_cache().clear();
}

/// Get current cache size (for diagnostics)
[[nodiscard]] inline std::size_t markdown_cache_size() {
    return detail::global_token_cache().size();
}

/// Get maximum cache size
[[nodiscard]] inline std::size_t markdown_cache_max_size() {
    return detail::global_token_cache().max_size();
}

} // namespace cc::ui
