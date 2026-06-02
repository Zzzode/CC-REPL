/// @file code_highlight.cppm
/// @brief Syntax-highlighted code rendering with theme support and line
/// numbers. Migrated from HighlightedCode/ and StructuredDiff/colorDiff.ts.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <unordered_map>
#include <expected>
#include <algorithm>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.code_highlight;

import cc.types.types;

export namespace cc::ui::code_highlight {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Token type for syntax highlighting
enum class TokenType : std::uint8_t {
    Plain,
    Keyword,
    String,
    Number,
    Comment,
    Operator,
    Punctuation,
    Function,
    Type,
    Variable,
    Constant,
    Preprocessor,
    Attribute,
    Error,
};

/// A highlighted token within a line
struct HighlightToken {
    std::string text;
    TokenType type;
    int start_col = 0;
    int end_col = 0;
};

/// A single highlighted line of code
struct HighlightedLine {
    int line_number;
    std::vector<HighlightToken> tokens;
    bool is_diff_added = false;
    bool is_diff_removed = false;
};

/// Color theme for syntax highlighting
struct SyntaxTheme {
    std::string name;
    Color plain          = Color::GrayLight;
    Color keyword        = Color::Magenta;
    Color string_lit     = Color::Green;
    Color number         = Color::Cyan;
    Color comment        = Color::GrayDark;
    Color op             = Color::White;
    Color punctuation    = Color::GrayLight;
    Color function       = Color::Yellow;
    Color type_name      = Color::Cyan;
    Color variable       = Color::White;
    Color constant       = Color::Red;
    Color preprocessor   = Color::Magenta;
    Color attribute      = Color::Yellow;
    Color error          = Color::Red;
    Color line_number    = Color::GrayDark;
    Color gutter_bg      = Color::RGB(20, 20, 20);
    Color bg             = Color::RGB(15, 15, 15);
    Color added_bg       = Color::RGB(0, 30, 0);
    Color removed_bg     = Color::RGB(30, 0, 0);
};

/// Well-known theme names
enum class ThemeName : std::uint8_t {
    Dark,
    Light,
    Monokai,
    Dracula,
    Nord,
};

/// Options for the code highlight component
struct CodeHighlightOptions {
    std::string source;
    std::string language;
    std::optional<SyntaxTheme> theme;
    bool show_line_numbers = true;
    int start_line = 1;
    int visible_lines = 30;
    int scroll_offset = 0;
    std::optional<int> highlight_line;       // Line to highlight
    std::vector<int> marked_lines;           // Lines with markers
    std::function<void(int line)> on_line_select;
};

/// Result of highlighting operation
struct HighlightResult {
    std::vector<HighlightedLine> lines;
    std::string language_detected;
};

// ============================================================
// Theme Utilities
// ============================================================

/// Get a built-in syntax theme
[[nodiscard]] inline SyntaxTheme get_theme(ThemeName name) {
    SyntaxTheme theme;
    switch (name) {
        case ThemeName::Monokai:
            theme.name = "monokai";
            theme.keyword = Color::RGB(249, 38, 114);
            theme.string_lit = Color::RGB(230, 219, 116);
            theme.number = Color::RGB(174, 129, 255);
            theme.comment = Color::RGB(117, 113, 94);
            theme.function = Color::RGB(166, 226, 46);
            theme.type_name = Color::RGB(102, 217, 239);
            theme.variable = Color::RGB(248, 248, 242);
            theme.constant = Color::RGB(174, 129, 255);
            break;
        case ThemeName::Dracula:
            theme.name = "dracula";
            theme.keyword = Color::RGB(255, 121, 198);
            theme.string_lit = Color::RGB(241, 250, 140);
            theme.number = Color::RGB(189, 147, 249);
            theme.comment = Color::RGB(98, 114, 164);
            theme.function = Color::RGB(80, 250, 123);
            theme.type_name = Color::RGB(139, 233, 253);
            theme.variable = Color::RGB(248, 248, 242);
            theme.constant = Color::RGB(189, 147, 249);
            break;
        case ThemeName::Nord:
            theme.name = "nord";
            theme.keyword = Color::RGB(129, 161, 193);
            theme.string_lit = Color::RGB(163, 190, 140);
            theme.number = Color::RGB(180, 142, 173);
            theme.comment = Color::RGB(76, 86, 106);
            theme.function = Color::RGB(136, 192, 208);
            theme.type_name = Color::RGB(143, 188, 187);
            theme.variable = Color::RGB(216, 222, 233);
            theme.constant = Color::RGB(180, 142, 173);
            break;
        case ThemeName::Light:
            theme.name = "light";
            theme.plain = Color::RGB(30, 30, 30);
            theme.keyword = Color::RGB(0, 0, 200);
            theme.string_lit = Color::RGB(0, 128, 0);
            theme.number = Color::RGB(0, 128, 128);
            theme.comment = Color::RGB(128, 128, 128);
            theme.function = Color::RGB(128, 0, 0);
            theme.type_name = Color::RGB(0, 0, 128);
            theme.variable = Color::RGB(30, 30, 30);
            theme.bg = Color::RGB(255, 255, 255);
            theme.gutter_bg = Color::RGB(240, 240, 240);
            theme.line_number = Color::RGB(150, 150, 150);
            break;
        default: // Dark
            theme.name = "dark";
            break;
    }
    return theme;
}

/// Get a theme by string name
[[nodiscard]] inline std::expected<SyntaxTheme, std::string>
get_syntax_theme(const std::string& name) {
    if (name == "dark" || name == "default") return get_theme(ThemeName::Dark);
    if (name == "light")   return get_theme(ThemeName::Light);
    if (name == "monokai") return get_theme(ThemeName::Monokai);
    if (name == "dracula") return get_theme(ThemeName::Dracula);
    if (name == "nord")    return get_theme(ThemeName::Nord);
    return std::unexpected("Unknown theme: " + name);
}

/// Get color for a token type from theme
[[nodiscard]] inline Color token_color(TokenType type, const SyntaxTheme& theme) {
    switch (type) {
        case TokenType::Keyword:      return theme.keyword;
        case TokenType::String:       return theme.string_lit;
        case TokenType::Number:       return theme.number;
        case TokenType::Comment:      return theme.comment;
        case TokenType::Operator:     return theme.op;
        case TokenType::Punctuation:  return theme.punctuation;
        case TokenType::Function:     return theme.function;
        case TokenType::Type:         return theme.type_name;
        case TokenType::Variable:     return theme.variable;
        case TokenType::Constant:     return theme.constant;
        case TokenType::Preprocessor: return theme.preprocessor;
        case TokenType::Attribute:    return theme.attribute;
        case TokenType::Error:        return theme.error;
        default:                      return theme.plain;
    }
}

// ============================================================
// Highlighting Engine (simplified — real implementation would
// use tree-sitter or similar via native binding)
// ============================================================

/// Simple tokenizer for common patterns (placeholder for full engine)
[[nodiscard]] inline std::vector<HighlightToken> tokenize_line(
    const std::string& line, [[maybe_unused]] const std::string& language) {

    // Simplified: return entire line as plain text
    // A real implementation would integrate with tree-sitter or syntect
    if (line.empty()) {
        return {};
    }
    return {{line, TokenType::Plain, 0, static_cast<int>(line.size())}};
}

/// Highlight entire source string
[[nodiscard]] inline HighlightResult highlight_source(
    const std::string& source, const std::string& language) {

    HighlightResult result;
    result.language_detected = language;

    // Split into lines
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < source.size()) {
        auto nl = source.find('\n', pos);
        if (nl == std::string::npos) {
            lines.push_back(source.substr(pos));
            break;
        }
        lines.push_back(source.substr(pos, nl - pos));
        pos = nl + 1;
    }

    result.lines.reserve(lines.size());
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        HighlightedLine hl;
        hl.line_number = i + 1;
        hl.tokens = tokenize_line(lines[i], language);
        result.lines.push_back(std::move(hl));
    }
    return result;
}

// ============================================================
// Element Rendering
// ============================================================

/// Render a single highlighted line
[[nodiscard]] inline Element RenderHighlightedLine(
    const HighlightedLine& line, const SyntaxTheme& theme,
    bool show_line_nums, bool is_highlighted, int gutter_width) {

    Elements parts;

    // Line number gutter
    if (show_line_nums) {
        auto num_str = std::format("{:>{}}", line.line_number, gutter_width);
        parts.push_back(
            text(num_str + " ") | color(theme.line_number)
            | bgcolor(theme.gutter_bg));
        parts.push_back(text(" "));
    }

    // Tokens
    if (line.tokens.empty()) {
        parts.push_back(text(" "));
    } else {
        for (const auto& tok : line.tokens) {
            auto el = text(tok.text) | color(token_color(tok.type, theme));
            parts.push_back(el);
        }
    }

    auto result = hbox(parts);

    // Background for diff lines
    if (line.is_diff_added) {
        result = result | bgcolor(theme.added_bg);
    } else if (line.is_diff_removed) {
        result = result | bgcolor(theme.removed_bg);
    } else if (is_highlighted) {
        result = result | bgcolor(Color::RGB(40, 40, 60));
    }

    return result;
}

/// Render the full highlighted code block
[[nodiscard]] inline Element RenderCodeHighlight(
    const CodeHighlightOptions& opts, const HighlightResult& highlighted) {

    SyntaxTheme theme = opts.theme.value_or(get_theme(ThemeName::Dark));

    int total_lines = static_cast<int>(highlighted.lines.size());
    int gutter_width = (total_lines > 0)
        ? static_cast<int>(std::format("{}", total_lines).size())
        : 1;

    // Compute visible range
    int start = std::max(0, opts.scroll_offset);
    int end = std::min(total_lines, start + opts.visible_lines);

    Elements lines;
    for (int i = start; i < end; ++i) {
        bool is_hl = opts.highlight_line.has_value() &&
                     *opts.highlight_line == highlighted.lines[i].line_number;
        lines.push_back(RenderHighlightedLine(
            highlighted.lines[i], theme, opts.show_line_numbers,
            is_hl, gutter_width));
    }

    if (lines.empty()) {
        lines.push_back(text("  (empty)") | dim);
    }

    // Status bar
    auto status = hbox({
        text(" " + highlighted.language_detected) | dim | color(Color::Cyan),
        text(std::format("  {}/{} lines", end - start, total_lines)) | dim,
        filler(),
        text("[j/k] scroll [q] close") | dim,
        text(" "),
    });

    return vbox({
        vbox(lines) | flex | bgcolor(theme.bg),
        status,
    });
}

// ============================================================
// Interactive Component
// ============================================================

/// Create an interactive code highlight component
[[nodiscard]] inline Component CodeHighlight(CodeHighlightOptions options) {
    struct State {
        CodeHighlightOptions opts;
        HighlightResult highlighted;
    };
    auto state = std::make_shared<State>();
    state->opts = std::move(options);
    state->highlighted = highlight_source(state->opts.source,
                                          state->opts.language);

    return Renderer([state] {
        return RenderCodeHighlight(state->opts, state->highlighted);
    }) | CatchEvent([state](Event event) -> bool {
        int total = static_cast<int>(state->highlighted.lines.size());

        if (event == Event::ArrowDown || event == Event::Character('j')) {
            if (state->opts.scroll_offset < total - state->opts.visible_lines) {
                state->opts.scroll_offset++;
            }
            return true;
        }
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            if (state->opts.scroll_offset > 0) {
                state->opts.scroll_offset--;
            }
            return true;
        }
        if (event == Event::PageDown) {
            state->opts.scroll_offset = std::min(
                state->opts.scroll_offset + state->opts.visible_lines,
                std::max(0, total - state->opts.visible_lines));
            return true;
        }
        if (event == Event::PageUp) {
            state->opts.scroll_offset = std::max(
                0, state->opts.scroll_offset - state->opts.visible_lines);
            return true;
        }
        if (event == Event::Home) {
            state->opts.scroll_offset = 0;
            return true;
        }
        if (event == Event::End) {
            state->opts.scroll_offset = std::max(0, total - state->opts.visible_lines);
            return true;
        }
        if (event == Event::Return) {
            if (state->opts.on_line_select) {
                int current_line = state->opts.scroll_offset +
                                   state->opts.start_line;
                state->opts.on_line_select(current_line);
            }
            return true;
        }
        return false;
    });
}

} // namespace cc::ui::code_highlight
