/// @file markdown.cppm
/// @brief Markdown to FTXUI element renderer.
/// Parses a subset of Markdown and converts it to styled FTXUI elements
/// including code blocks, bold/italic, lists, tables, and inline code.
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
#include <regex>

#include <ftxui/dom/elements.hpp>

export module cc.ui.markdown;

export namespace cc::ui {

// ============================================================
// Syntax Highlighting for Code Blocks
// ============================================================

/// Supported languages for syntax highlighting
enum class Language : std::uint8_t {
    Unknown, Cpp, Python, TypeScript, JavaScript,
    Rust, Go, Java, Bash, Json, Yaml, Markdown,
};

/// Detect language from fenced code block identifier
[[nodiscard]] constexpr Language detect_language(std::string_view lang) noexcept {
    if (lang == "cpp" || lang == "c++") return Language::Cpp;
    if (lang == "python" || lang == "py") return Language::Python;
    if (lang == "typescript" || lang == "ts") return Language::TypeScript;
    if (lang == "javascript" || lang == "js") return Language::JavaScript;
    if (lang == "rust" || lang == "rs") return Language::Rust;
    if (lang == "go") return Language::Go;
    if (lang == "java") return Language::Java;
    if (lang == "bash" || lang == "sh" || lang == "shell") return Language::Bash;
    if (lang == "json") return Language::Json;
    if (lang == "yaml" || lang == "yml") return Language::Yaml;
    if (lang == "markdown" || lang == "md") return Language::Markdown;
    return Language::Unknown;
}

/// Token type for basic syntax highlighting
enum class TokenKind : std::uint8_t {
    Plain, Keyword, String, Comment, Number, Operator, Type, Function,
};

/// A single highlighted token
struct HighlightToken {
    std::string text;
    TokenKind kind;
};

/// Apply basic keyword-based syntax highlighting to a code line
[[nodiscard]] inline std::vector<HighlightToken> highlight_line(
    std::string_view line, Language lang) {
    // Simplified keyword highlighting: detect comments, strings, keywords
    std::vector<HighlightToken> tokens;

    // Check for line comments
    std::size_t comment_pos = std::string::npos;
    if (lang == Language::Cpp || lang == Language::JavaScript ||
        lang == Language::TypeScript || lang == Language::Rust ||
        lang == Language::Go || lang == Language::Java) {
        comment_pos = line.find("//");
    } else if (lang == Language::Python || lang == Language::Bash || lang == Language::Yaml) {
        comment_pos = line.find('#');
    }

    if (comment_pos != std::string::npos && comment_pos == 0) {
        tokens.push_back({std::string(line), TokenKind::Comment});
        return tokens;
    }

    // Fallback: treat entire line as plain text
    if (comment_pos != std::string::npos) {
        tokens.push_back({std::string(line.substr(0, comment_pos)), TokenKind::Plain});
        tokens.push_back({std::string(line.substr(comment_pos)), TokenKind::Comment});
    } else {
        tokens.push_back({std::string(line), TokenKind::Plain});
    }
    return tokens;
}

/// Map token kind to FTXUI color
[[nodiscard]] inline ftxui::Color token_color(TokenKind kind) {
    switch (kind) {
        case TokenKind::Keyword:  return ftxui::Color::Magenta;
        case TokenKind::String:   return ftxui::Color::Green;
        case TokenKind::Comment:  return ftxui::Color::GrayDark;
        case TokenKind::Number:   return ftxui::Color::Yellow;
        case TokenKind::Operator: return ftxui::Color::Cyan;
        case TokenKind::Type:     return ftxui::Color::Blue;
        case TokenKind::Function: return ftxui::Color::Yellow;
        default:                  return ftxui::Color::White;
    }
}

// ============================================================
// Markdown Block Types (internal AST)
// ============================================================

/// Parsed markdown block variants
struct ParagraphBlock { std::string text; };
struct HeadingBlock { std::uint8_t level; std::string text; };
struct CodeBlock { std::string code; Language language; };
struct UnorderedListBlock { std::vector<std::string> items; };
struct OrderedListBlock { std::vector<std::string> items; };
struct TableBlock {
    std::vector<std::string> headers;
    std::vector<std::vector<std::string>> rows;
};
struct HorizontalRuleBlock {};

using MarkdownBlock = std::variant<
    ParagraphBlock, HeadingBlock, CodeBlock,
    UnorderedListBlock, OrderedListBlock,
    TableBlock, HorizontalRuleBlock>;

[[nodiscard]] inline std::string trim_cell(std::string_view value) {
    auto begin = value.find_first_not_of(" \t");
    if (begin == std::string_view::npos) return "";
    auto end = value.find_last_not_of(" \t");
    return std::string(value.substr(begin, end - begin + 1));
}

[[nodiscard]] inline std::vector<std::string> split_table_row(std::string_view line) {
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

[[nodiscard]] inline bool is_table_separator(std::string_view line) {
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

[[nodiscard]] inline std::optional<std::pair<std::string, std::string>> parse_ordered_list(std::string_view line) {
    std::size_t pos = 0;
    while (pos < line.size() && std::isdigit(static_cast<unsigned char>(line[pos]))) ++pos;
    if (pos == 0 || pos >= line.size() || line[pos] != '.') return std::nullopt;
    if (pos + 1 >= line.size() || !std::isspace(static_cast<unsigned char>(line[pos + 1]))) return std::nullopt;
    std::size_t text_start = pos + 2;
    while (text_start < line.size() && std::isspace(static_cast<unsigned char>(line[text_start]))) ++text_start;
    return std::pair{std::string(line.substr(0, pos)), std::string(line.substr(text_start))};
}

// ============================================================
// Inline Rendering
// ============================================================

/// Render inline markdown formatting (bold, italic, code, links)
[[nodiscard]] inline ftxui::Element render_inline(std::string_view text) {
    std::vector<ftxui::Element> elements;
    std::string buffer;

    for (std::size_t i = 0; i < text.size(); ++i) {
        // Inline code: `code`
        if (text[i] == '`') {
            if (!buffer.empty()) {
                elements.push_back(ftxui::text(buffer));
                buffer.clear();
            }
            auto end = text.find('`', i + 1);
            if (end != std::string_view::npos) {
                auto code = text.substr(i + 1, end - i - 1);
                elements.push_back(
                    ftxui::text(std::string(code))
                    | ftxui::color(ftxui::Color::Yellow)
                    | ftxui::inverted);
                i = end;
                continue;
            }
        }
        // Bold: **text**
        if (i + 1 < text.size() && text[i] == '*' && text[i + 1] == '*') {
            if (!buffer.empty()) {
                elements.push_back(ftxui::text(buffer));
                buffer.clear();
            }
            auto end = text.find("**", i + 2);
            if (end != std::string_view::npos) {
                auto bold_text = text.substr(i + 2, end - i - 2);
                elements.push_back(ftxui::text(std::string(bold_text)) | ftxui::bold);
                i = end + 1;
                continue;
            }
        }
        // Italic: *text*
        if (text[i] == '*' && (i + 1 >= text.size() || text[i + 1] != '*')) {
            if (!buffer.empty()) {
                elements.push_back(ftxui::text(buffer));
                buffer.clear();
            }
            auto end = text.find('*', i + 1);
            if (end != std::string_view::npos) {
                auto italic_text = text.substr(i + 1, end - i - 1);
                elements.push_back(ftxui::text(std::string(italic_text)) | ftxui::dim);
                i = end;
                continue;
            }
        }
        // Link: [text](url) - render as colored text
        if (text[i] == '[') {
            auto bracket_end = text.find(']', i + 1);
            if (bracket_end != std::string_view::npos &&
                bracket_end + 1 < text.size() && text[bracket_end + 1] == '(') {
                auto paren_end = text.find(')', bracket_end + 2);
                if (paren_end != std::string_view::npos) {
                    if (!buffer.empty()) {
                        elements.push_back(ftxui::text(buffer));
                        buffer.clear();
                    }
                    auto link_text = text.substr(i + 1, bracket_end - i - 1);
                    elements.push_back(
                        ftxui::text(std::string(link_text))
                        | ftxui::color(ftxui::Color::Blue) | ftxui::underlined);
                    i = paren_end;
                    continue;
                }
            }
        }
        buffer += text[i];
    }
    if (!buffer.empty()) {
        elements.push_back(ftxui::text(buffer));
    }
    return elements.empty() ? ftxui::text("") : ftxui::hbox(elements);
}

// ============================================================
// Block Rendering
// ============================================================

/// Render a code block with syntax highlighting and border
[[nodiscard]] inline ftxui::Element render_code_block(const CodeBlock& block) {
    std::vector<ftxui::Element> lines;
    std::string_view code = block.code;
    // Split code by newlines and highlight each line
    std::size_t start = 0;
    while (start < code.size()) {
        auto end = code.find('\n', start);
        auto line = (end == std::string_view::npos)
                    ? code.substr(start)
                    : code.substr(start, end - start);

        auto tokens = highlight_line(line, block.language);
        std::vector<ftxui::Element> line_elements;
        for (const auto& tok : tokens) {
            line_elements.push_back(ftxui::text(tok.text) | ftxui::color(token_color(tok.kind)));
        }
        lines.push_back(ftxui::hbox(line_elements));
        start = (end == std::string_view::npos) ? code.size() : end + 1;
    }
    return ftxui::vbox(lines);
}

/// Render a table block with aligned columns
[[nodiscard]] inline ftxui::Element render_table(const TableBlock& block) {
    std::vector<ftxui::Elements> table_rows;
    // Header row
    ftxui::Elements header_cells;
    for (const auto& h : block.headers) {
        header_cells.push_back(ftxui::text(h) | ftxui::bold | ftxui::center);
    }
    table_rows.push_back(header_cells);
    // Data rows
    for (const auto& row : block.rows) {
        ftxui::Elements cells;
        for (const auto& cell : row) {
            cells.push_back(ftxui::text(cell) | ftxui::center);
        }
        table_rows.push_back(cells);
    }
    return ftxui::gridbox(table_rows) | ftxui::border;
}

// ============================================================
// Main Markdown Renderer
// ============================================================

/// Parse and render a full markdown document string to FTXUI elements
[[nodiscard]] inline ftxui::Element render_markdown(std::string_view source) {
    std::vector<ftxui::Element> elements;
    std::vector<std::string> lines;

    // Split source into lines
    std::size_t start = 0;
    while (start < source.size()) {
        auto end = source.find('\n', start);
        lines.emplace_back(end == std::string_view::npos
                           ? source.substr(start)
                           : source.substr(start, end - start));
        start = (end == std::string_view::npos) ? source.size() : end + 1;
    }

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];

        // Fenced code block
        if (line.starts_with("```")) {
            auto lang_str = line.substr(3);
            auto lang = detect_language(lang_str);
            std::string code;
            ++i;
            while (i < lines.size() && !lines[i].starts_with("```")) {
                if (!code.empty()) code += '\n';
                code += lines[i];
                ++i;
            }
            elements.push_back(render_code_block(CodeBlock{std::move(code), lang}));
            continue;
        }
        // GFM table: header row followed by an alignment separator.
        if (line.find('|') != std::string::npos && i + 1 < lines.size() &&
            is_table_separator(lines[i + 1])) {
            TableBlock table;
            table.headers = split_table_row(line);
            i += 2;
            while (i < lines.size() && lines[i].find('|') != std::string::npos && !lines[i].empty()) {
                table.rows.push_back(split_table_row(lines[i]));
                ++i;
            }
            --i;
            elements.push_back(render_table(table));
            continue;
        }
        // Headings
        if (line.starts_with("### ")) {
            elements.push_back(render_inline(line.substr(4)) | ftxui::bold);
        } else if (line.starts_with("## ")) {
            elements.push_back(render_inline(line.substr(3)) | ftxui::bold | ftxui::underlined);
        } else if (line.starts_with("# ")) {
            elements.push_back(render_inline(line.substr(2)) | ftxui::bold | ftxui::underlined
                               | ftxui::color(ftxui::Color::Cyan));
        }
        // Horizontal rule
        else if (line == "---" || line == "***" || line == "___") {
            elements.push_back(ftxui::separator());
        }
        // Unordered list
        else if (line.starts_with("- ") || line.starts_with("* ")) {
            auto bullet = ftxui::text("  • ") | ftxui::color(ftxui::Color::Cyan);
            elements.push_back(ftxui::hbox({bullet, render_inline(line.substr(2))}));
        }
        // Ordered list ("1. ...", "10. ...")
        else if (auto item = parse_ordered_list(line)) {
            auto num = ftxui::text(std::format("  {}. ", item->first))
                       | ftxui::color(ftxui::Color::Cyan);
            elements.push_back(ftxui::hbox({num, render_inline(item->second)}));
        }
        // Empty line -> spacing
        else if (line.empty()) {
            elements.push_back(ftxui::text(""));
        }
        // Regular paragraph text
        else {
            elements.push_back(render_inline(line));
        }
    }
    return ftxui::vbox(elements);
}

} // namespace cc::ui
