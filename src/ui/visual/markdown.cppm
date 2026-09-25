/// @file markdown.cppm
/// @brief Production-grade Markdown to FTXUI element renderer.
///
/// Features:
/// - Full block-level parsing: headings, paragraphs, code fences, lists,
///   blockquotes, tables (GFM), horizontal rules
/// - Inline formatting: bold, italic, inline code, links,
///   escaped characters (strikethrough intentionally disabled, mirroring TS)
/// - Syntax-highlighted code blocks via cc.ui.visual.code_highlight
/// - LRU token cache for fast re-renders (virtual scrolling)
/// - Fast-path: skip lexing for plain text with no markdown markers
/// - Theme-aware coloring with dimColor option
/// - Streaming Markdown: stable prefix + unstable suffix for live output
/// - Interactive MarkdownComponent with scroll support
module;

#include <cstdint>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.visual.markdown;

import std;

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
    Math,       ///< Inline math: $...$ (LaTeX math expression)
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
    MathBlock,   ///< Block math: $$...$$ (LaTeX display math)
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
    // Per-item nesting depth (0 = top-level, 1 = first indent, ...).
    // Mirrors TS marked's list tokenizer which tracks `item.depth` based on
    // leading spaces.  Used by render_olist() for depth-based numbering
    // (Arabic / letters / Roman) and render_ulist() for indentation.
    std::vector<int> list_depths;
    // Paragraph / Heading / Blockquote: inline tokens
    std::vector<InlineToken> inlines;
    // Blockquote depth
    int quote_depth = 0;
    // Math block content (LaTeX source for $$...$$ display math)
    std::string math_content;
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
            c == '~' || c == '$') {
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
[[nodiscard]] std::vector<InlineToken> tokenize_inline(std::string_view text);

} // namespace detail

// ============================================================
// Table Parsing Helpers
// ============================================================

namespace detail {

[[nodiscard]] std::string trim_cell(std::string_view value);

[[nodiscard]] std::vector<std::string> split_table_row(std::string_view line);

[[nodiscard]] bool is_table_separator(std::string_view line);

} // namespace detail

// ============================================================
// Ordered List Detection
// ============================================================

namespace detail {

/// Count leading whitespace characters (spaces + tabs) in a line.
/// Each 2 spaces or 1 tab counts as one indentation level, matching
/// the TS marked tokenizer's `indent` calculation.
[[nodiscard]] int count_list_indent(std::string_view line);

[[nodiscard]] std::optional<std::pair<int, std::string>>
parse_ordered_list(std::string_view line);

} // namespace detail

// ============================================================
// Block-Level Lexer
// ============================================================

namespace detail {

/// Split source string into lines.
[[nodiscard]] std::vector<std::string> split_lines(std::string_view source);

/// Lex a markdown document into block tokens.
[[nodiscard]] std::vector<BlockToken> lex_blocks(std::string_view source);

} // namespace detail

// ============================================================
// LRU Token Cache
// ============================================================

namespace detail {

/// LRU cache for tokenized markdown documents.
/// Keyed by content hash to avoid retaining full strings.
class TokenCache {
public:
    explicit TokenCache(std::size_t max_size = 256);

    /// Look up tokens for a content string. Returns nullptr if not found.
    [[nodiscard]] const std::vector<BlockToken>* find(std::string_view key) const;

    /// Insert a new entry, evicting LRU if at capacity.
    void put(std::string key, std::vector<BlockToken> tokens);

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
TokenCache& global_token_cache();

} // namespace detail

// ============================================================
// GitHub Issue Reference Linkification
// ============================================================

namespace detail {

/// TS REF: src/utils/markdown.ts ISSUE_REF_PATTERN and linkifyIssueReferences()
///
/// Pattern: owner/repo#123  →  clickable OSC 8 link to GitHub issues page.
///
/// Regex equivalent:  /(^|[^\w.\/-])([A-Za-z0-9][\w-]*\/[A-Za-z0-9][\w.-]*)#(\d+)\b/g
///
/// We use manual scanning instead of std::regex for performance (markdown
/// rendering is on the hot path for streaming updates).
///
/// @param text  Plain text to scan for issue references.
/// @param opts  Current markdown options (for dim_color propagation).
/// @return      Elements vector with plain text segments interspersed with
///              hyperlinked issue references.
[[nodiscard]] Elements linkify_issue_references(
    std::string_view text,
    const MarkdownOptions& opts);

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
///   - text           -> linkifyIssueReferences(text)
/// Strikethrough (del) is intentionally disabled (TS configureMarked disables
/// the `del` tokenizer).
[[nodiscard]] Element render_inlines(
    const std::vector<InlineToken>& tokens,
    const MarkdownOptions& opts);

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
                                     const MarkdownOptions& opts);

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
                                        const MarkdownOptions& opts);

// ============================================================
// List Numbering Helpers
// ============================================================

namespace detail {

/// Convert a positive integer to its lowercase alphabetic representation.
/// Mirrors TS utils/markdown.ts numberToLetter(): 1→"a", 2→"b", ..., 26→"z",
/// 27→"aa", 28→"ab", etc.
[[nodiscard]] std::string number_to_letter(int n);

/// Convert a positive integer to its lowercase Roman numeral representation.
/// Mirrors TS utils/markdown.ts numberToRoman(): 1→"i", 4→"iv", 9→"ix", etc.
/// Returns the Arabic string for values outside the classical range (1–3999).
[[nodiscard]] std::string number_to_roman(int n);

/// Select the ordered-list number label based on nesting depth.
///
/// Mirrors TS utils/markdown.ts getListNumber(listDepth, orderedListNumber):
///   depth 0,1 → Arabic numeral
///   depth 2   → lowercase letter
///   depth 3   → lowercase Roman numeral
///   depth ≥4  → Arabic (wraps back)
///
/// @param depth  List nesting level (0 = top-level).
/// @param n      1-based item number within its depth group.
/// @return       Label string without the trailing ". " (caller appends it).
[[nodiscard]] std::string get_list_number(int depth, int n);

} // namespace detail

/// Render an unordered list.
///
/// Mirrors src/utils/markdown.ts: a list_item's text token renders as
///   `${'  '.repeat(listDepth)}- ${content}${EOL}`
/// Top-level (listDepth 0) items use `- ` with no leading indent. Nested
/// items get `'  '.repeat(depth)` indentation prefix. The prior divergent
/// renderer used a cyan `•` bullet and 2-space indent — neither appears in TS.
[[nodiscard]] Element render_ulist(const BlockToken& tok,
                                   const MarkdownOptions& opts);

/// Render an ordered list.
///
/// Mirrors src/utils/markdown.ts getListNumber(listDepth, n):
///   depth 0,1 → Arabic numeral  ("1.", "2.")
///   depth 2   → lowercase letter ("a.", "b.")
///   depth 3   → lowercase Roman  ("i.", "ii.")
///   depth 4+  → Arabic (wraps)
///
/// Numbering resets per depth level: each time a deeper depth appears,
/// numbering starts from 1 for that depth. This mirrors how TS marked
/// produces nested `list` tokens each with their own item counter.
///
/// Each item also gets `'  '.repeat(depth)` indentation prefix.
[[nodiscard]] Element render_olist(const BlockToken& tok,
                                   const MarkdownOptions& opts);

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
                                        const MarkdownOptions& opts);

/// Render a GFM table using Unicode box-drawing characters, matching the
/// TS <MarkdownTable> renderer (src/components/MarkdownTable.tsx) non-wrapped
/// path when columns fit the terminal at ideal width.
///
/// TS emits a fully bordered table:
///   ┌──────┬──────┐
///   │ hdr1 │ hdr2 │
///   ├──────┼──────┤
///   │ c1   │ c2   │
///   └──────┴──────┘
///
/// Borders use: │ U+2502 (vertical), ─ U+2500 (horizontal),
///              corners (┌┐└┘ U+250C/10/14/18),
///              cross pieces (┬┼┴ U+252C/3C/34, ├┤ U+251C/24).
///
/// Column width = max(stringWidth(header), max(stringWidth(cell))) with a
/// minimum of 3. Header cells are bold; data cells are plain.
///
/// The previous divergent renderer emitted a plaintext ASCII pipe table
/// (`|---|---|`) which leaked literal `---` dashes into the rendered text
/// (P0 bug: GFM separator line leaked as paragraph content). Using
/// box-drawing ─ chars instead of ASCII hyphens prevents any spurious
/// "---" substring match and matches the TS terminal-native output.
[[nodiscard]] Element render_table(const BlockToken& tok,
                                   const MarkdownOptions& opts);

/// Render a horizontal rule.
///
/// Mirrors src/utils/markdown.ts formatToken 'hr': returns the literal
/// string "---" (not a separator/box-drawing line). The prior divergent
/// renderer used ftxui::separator(), which draws a full-width ── rule —
/// absent in TS output.
[[nodiscard]] Element render_hr(const MarkdownOptions& opts);

} // namespace detail

// ============================================================
// Main Markdown Renderer
// ============================================================

/// Parse and render a markdown document string to FTXUI elements.
/// Uses token cache for repeated renders.
[[nodiscard]] Element render_markdown(std::string_view source,
                                      const MarkdownOptions& opts = {});

/// Convenience: render with dim_color = true
[[nodiscard]] Element render_markdown_dim(std::string_view source);

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
    Element update(std::string_view content);

    /// Reset the streaming state
    void reset();

    /// Get current stable prefix length (for debugging)
    [[nodiscard]] std::size_t stable_length() const {
        return stable_prefix_.size();
    }

private:
    std::string stable_prefix_;

    /// Find the last safe block boundary in content.
    /// A safe boundary is a double-newline or a closing code fence.
    [[nodiscard]] static std::size_t find_block_boundary(
        std::string_view content);
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
    explicit MarkdownComponentBase(MarkdownComponentOptions opts);

    Element Render() override;

    bool OnEvent(Event event) override;

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
[[nodiscard]] Component MarkdownComponent(MarkdownComponentOptions opts);

// ============================================================
// Cache Management
// ============================================================

/// Clear the global markdown token cache.
/// Call this when memory pressure is high or on settings change.
void clear_markdown_cache();

/// Get current cache size (for diagnostics)
[[nodiscard]] std::size_t markdown_cache_size();

/// Get maximum cache size
[[nodiscard]] std::size_t markdown_cache_max_size();

} // namespace cc::ui
