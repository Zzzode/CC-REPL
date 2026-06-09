/// @file code_highlight.cppm
/// @brief Syntax-highlighted code rendering with theme support and line
/// numbers. Migrated from HighlightedCode/ and StructuredDiff/colorDiff.ts.
/// PARTIAL COMPLETED by UI27: HighlightedCode Fallback (heuristic regex
///   colorizer) lives in cc.ui.components.partial_completions —
///   RenderHighlightedCodeFallback(code, lang, opts). Integration point:
///   replace the `if (!shiki_available)` branch in RenderCodeBlock with a
///   call to it; the 6-rule (comment > string > keyword/type > number >
///   function) heuristic matches TS Fallback.tsx line-for-line.
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
#include <string_view>
#include <array>
#include <cctype>

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
// Per-Language Keyword Sets
//
// NOTE: A full LSP-backed syntax highlighter is out of scope here
//       (TODO: integrate with cc/utils/cli_highlight / the HL module
//       once it is exposed). The heuristic keyword + string + number
//       + comment tokenizer below mirrors HighlightedCode/Fallback.tsx:
//       it gives users reasonable, deterministic coloring across 8
//       common languages without pulling in grammars worth of code.
// ===========================================================================

struct LanguageSpec {
    std::string_view id;
    std::vector<std::string_view> keywords;     // 20-30 common words
    std::vector<std::string_view> type_names;   // built-in types
    // Comment markers
    std::string_view line_comment = "//";
    bool has_block_comment_c = true;            // /* */
    bool has_hash_comment = false;
    bool has_dash_comment = false;              // -- (sql)
    // String delimiters enabled
    bool allow_single_quote = true;
    bool allow_double_quote = true;
    bool allow_backtick = false;
};

// Helper: make keyword list at compile time.
constexpr std::array<std::string_view, 31> kCppKeywords = {
    "alignas","alignof","and","and_eq","asm","auto","bitand","bitor","bool",
    "break","case","catch","char","char8_t","char16_t","char32_t","class",
    "co_await","co_return","co_yield","compl","concept","const","consteval",
    "constexpr","constinit","const_cast","continue","decltype","default","delete"
};
constexpr std::array<std::string_view, 16> kCppKeywords2 = {
    "do","double","dynamic_cast","else","enum","explicit","export","extern",
    "false","float","for","friend","goto","if","inline","int"
};
constexpr std::array<std::string_view, 19> kCppKeywords3 = {
    "long","mutable","namespace","new","noexcept","not","not_eq","nullptr",
    "operator","or","or_eq","private","protected","public","register",
    "reinterpret_cast","requires","return","short"
};
constexpr std::array<std::string_view, 22> kCppKeywords4 = {
    "signed","sizeof","static","static_assert","static_cast","struct","switch",
    "template","this","thread_local","throw","true","try","typedef","typeid",
    "typename","union","unsigned","using","virtual","void","volatile"
};
constexpr std::array<std::string_view, 3> kCppKeywords5 = {
    "wchar_t","while","xor"
};

constexpr std::array<std::string_view, 28> kPyKeywords = {
    "False","None","True","and","as","assert","async","await","break","class",
    "continue","def","del","elif","else","except","finally","for","from",
    "global","if","import","in","is","lambda","nonlocal","not","or"
};
constexpr std::array<std::string_view, 7> kPyKeywords2 = {
    "pass","raise","return","try","while","with","yield"
};

constexpr std::array<std::string_view, 35> kJsKeywords = {
    "abstract","arguments","async","await","boolean","break","byte","case",
    "catch","char","class","const","continue","debugger","default","delete",
    "do","double","else","enum","eval","export","extends","false","final",
    "finally","float","for","function","goto","if","implements","import","in",
    "instanceof"
};
constexpr std::array<std::string_view, 26> kJsKeywords2 = {
    "int","interface","let","long","native","new","null","package","private",
    "protected","public","return","short","static","super","switch",
    "synchronized","this","throw","throws","transient","true","try","typeof",
    "var","void"
};
constexpr std::array<std::string_view, 7> kJsKeywords3 = {
    "volatile","while","with","yield","of","from","as"
};

constexpr std::array<std::string_view, 27> kBashKeywords = {
    "if","then","else","elif","fi","case","esac","for","in","done","while",
    "until","do","function","select","time","return","exit","break",
    "continue","local","export","readonly","declare","typeset","source","."
};

constexpr std::array<std::string_view, 3> kJsonKeywords = {
    "true","false","null" // JSON has no keywords — only literals
};

constexpr std::array<std::string_view, 3> kMdKeywords = {
    // Headings and markers are handled inline
    "TODO","FIXME","NOTE"
};

constexpr std::array<std::string_view, 33> kYamlKeywords = {
    "true","false","null","yes","no","on","off","---","...",
    "anchor","alias","mapping","sequence","block","flow","folded","literal",
    "map","list"
};

/// Build LanguageSpec by id. Falls back to C-like defaults.
[[nodiscard]] inline LanguageSpec get_language_spec(std::string_view lang) {
    LanguageSpec s;
    s.id = lang;

    auto is_lang = [&](std::initializer_list<std::string_view> names) {
        for (auto n : names) {
            if (lang == n) return true;
            if (!lang.empty() && n.size() <= lang.size() &&
                lang.substr(0, n.size()) == n) return true;
        }
        return false;
    };

    // C / C++ / C# / Objective-C
    if (is_lang({"c","cpp","c++","cxx","cc","hpp","h","hh","hxx",
                  "csharp","cs","objective-c","m","mm"})) {
        for (auto w : kCppKeywords)  s.keywords.push_back(w);
        for (auto w : kCppKeywords2) s.keywords.push_back(w);
        for (auto w : kCppKeywords3) s.keywords.push_back(w);
        for (auto w : kCppKeywords4) s.keywords.push_back(w);
        for (auto w : kCppKeywords5) s.keywords.push_back(w);
        s.line_comment = "//";
        s.has_block_comment_c = true;
        s.allow_backtick = false;
        return s;
    }

    if (is_lang({"py","python","python3","python2","pyi"})) {
        for (auto w : kPyKeywords)  s.keywords.push_back(w);
        for (auto w : kPyKeywords2) s.keywords.push_back(w);
        s.line_comment = "#";
        s.has_hash_comment = true;
        s.has_block_comment_c = false;
        s.allow_single_quote = true;
        s.allow_double_quote = true;
        s.allow_backtick = false;
        return s;
    }

    if (is_lang({"js","javascript","jsx","ts","typescript","tsx",
                  "mjs","cjs","es6","es"})) {
        for (auto w : kJsKeywords)  s.keywords.push_back(w);
        for (auto w : kJsKeywords2) s.keywords.push_back(w);
        for (auto w : kJsKeywords3) s.keywords.push_back(w);
        s.line_comment = "//";
        s.has_block_comment_c = true;
        s.allow_backtick = true;
        return s;
    }

    if (is_lang({"bash","sh","zsh","shell","ksh","fish","dash"})) {
        for (auto w : kBashKeywords) s.keywords.push_back(w);
        s.line_comment = "#";
        s.has_hash_comment = true;
        s.has_block_comment_c = false;
        s.allow_single_quote = true;
        s.allow_double_quote = true;
        s.allow_backtick = true;
        return s;
    }

    if (is_lang({"json","json5","jsonc"})) {
        for (auto w : kJsonKeywords) s.keywords.push_back(w);
        s.line_comment = "";
        s.has_block_comment_c = (lang == "jsonc" || lang == "json5");
        s.allow_backtick = false;
        return s;
    }

    if (is_lang({"md","markdown"})) {
        for (auto w : kMdKeywords) s.keywords.push_back(w);
        s.line_comment = "";
        s.has_block_comment_c = true;   // C-style block comments not used, stub
        s.has_hash_comment = false;     // # is heading, not comment
        return s;
    }

    if (is_lang({"yaml","yml"})) {
        for (auto w : kYamlKeywords) s.keywords.push_back(w);
        s.line_comment = "#";
        s.has_hash_comment = true;
        s.has_block_comment_c = false;
        return s;
    }

    if (is_lang({"sql","sql"})) {
        // SQL keywords — common subset
        for (auto w : {
            "SELECT","FROM","WHERE","AND","OR","NOT","NULL","IN","LIKE","BETWEEN",
            "INSERT","INTO","VALUES","UPDATE","SET","DELETE","CREATE","TABLE",
            "DROP","ALTER","INDEX","JOIN","LEFT","RIGHT","INNER","OUTER","ON",
            "GROUP","BY","ORDER","HAVING","LIMIT","AS","UNION","DISTINCT",
            "PRIMARY","KEY","FOREIGN","REFERENCES","DEFAULT","UNIQUE","CHECK"
        }) s.keywords.push_back(w);
        s.line_comment = "--";
        s.has_dash_comment = true;
        s.has_block_comment_c = true;
        return s;
    }

    // Default: C-like line comments
    for (auto w : kCppKeywords)  s.keywords.push_back(w);
    for (auto w : kCppKeywords2) s.keywords.push_back(w);
    s.line_comment = "//";
    s.has_block_comment_c = true;
    return s;
}

// ============================================================
// Highlighting Engine
// ============================================================

/// State for multi-line tokens (block comments / multi-line strings).
enum class MultilineState : std::uint8_t {
    None,
    BlockComment,   // in /* ... */
    StringDq,       // in "...
    StringSq,       // in '...
    StringBt,       // in `...
};

/// Character predicate for identifier boundaries.
[[nodiscard]] inline bool is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_' || c == '$' ||
           static_cast<unsigned char>(c) >= 0x80; // rough UTF-8 pass-through
}

[[nodiscard]] inline bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_' || c == '$' ||
           static_cast<unsigned char>(c) >= 0x80;
}

/// Tokenize a line with full keyword/string/number/comment support.
/// `ml_state` carries over from previous line and is updated in place.
[[nodiscard]] inline std::vector<HighlightToken> tokenize_line_ex(
    const std::string& line,
    const LanguageSpec& spec,
    MultilineState& ml_state,
    int start_col = 0) {

    std::vector<HighlightToken> out;
    if (line.empty()) return out;

    auto push = [&](TokenType t, std::string_view text, int col) {
        if (text.empty()) return;
        out.push_back({std::string(text), t, col,
                       col + static_cast<int>(text.size())});
    };

    int col = start_col;
    size_t i = 0;
    const size_t N = line.size();

    // Multi-line continuation from previous line
    auto try_consume_until = [&](MultilineState state,
                                  const char* end_seq,
                                  TokenType tok) -> bool {
        size_t es = std::char_traits<char>::length(end_seq);
        size_t p = 0;
        while (p + es <= N) {
            bool match = true;
            for (size_t k = 0; k < es; ++k)
                if (line[p + k] != end_seq[k]) { match = false; break; }
            if (match) {
                push(tok, std::string_view(line.data() + i, p + es - i), col);
                col += static_cast<int>(p + es - i);
                i = p + es;
                ml_state = MultilineState::None;
                return true;
            }
            // Handle escape: skip next char
            if (line[p] == '\\') { p += 2; continue; }
            ++p;
        }
        // Consumed rest of line (continues)
        push(tok, std::string_view(line.data() + i, N - i), col);
        col += static_cast<int>(N - i);
        i = N;
        return true; // consumed rest
    };

    if (ml_state == MultilineState::BlockComment) {
        try_consume_until(ml_state, "*/", TokenType::Comment);
        return out;
    }
    if (ml_state == MultilineState::StringDq) {
        try_consume_until(ml_state, "\"", TokenType::String);
        return out;
    }
    if (ml_state == MultilineState::StringSq) {
        try_consume_until(ml_state, "'", TokenType::String);
        return out;
    }
    if (ml_state == MultilineState::StringBt) {
        try_consume_until(ml_state, "`", TokenType::String);
        return out;
    }

    // Line-level comment (// or # or --)
    auto line_comment_match = [&](size_t pos) -> size_t {
        if (!spec.line_comment.empty()) {
            size_t ll = spec.line_comment.size();
            if (pos + ll <= N &&
                line.substr(pos, ll) == spec.line_comment) return ll;
        }
        if (spec.has_hash_comment) {
            if (pos < N && line[pos] == '#') return 1;
        }
        if (spec.has_dash_comment) {
            if (pos + 1 < N && line[pos] == '-' && line[pos + 1] == '-') return 2;
        }
        return 0;
    };

    while (i < N) {
        size_t col_start = col;
        char c = line[i];

        // Line comment — to end of line
        if (size_t ll = line_comment_match(i); ll > 0) {
            push(TokenType::Comment,
                 std::string_view(line.data() + i, N - i), col);
            col += static_cast<int>(N - i);
            i = N;
            break;
        }

        // Block comment start
        if (spec.has_block_comment_c &&
            i + 1 < N && c == '/' && line[i + 1] == '*') {
            ml_state = MultilineState::BlockComment;
            i += 2; col += 2;
            // consume until */
            size_t p = i;
            bool found = false;
            while (p + 1 < N) {
                if (line[p] == '*' && line[p + 1] == '/') {
                    found = true;
                    push(TokenType::Comment,
                         std::string_view(line.data() + col_start,
                                          p + 2 - col_start),
                         (int)col_start);
                    col = static_cast<int>(p + 2);
                    i = p + 2;
                    ml_state = MultilineState::None;
                    break;
                }
                ++p;
            }
            if (!found) {
                push(TokenType::Comment,
                     std::string_view(line.data() + col_start, N - col_start),
                     (int)col_start);
                col = static_cast<int>(N);
                i = N;
            }
            continue;
        }

        // String literals
        auto start_string = [&](char delim, MultilineState st) {
            ml_state = st;
            size_t p = i + 1;
            bool found = false;
            while (p < N) {
                if (line[p] == '\\') { p += 2; continue; }
                if (line[p] == delim) {
                    found = true;
                    push(TokenType::String,
                         std::string_view(line.data() + i, p + 1 - i), col);
                    col += static_cast<int>(p + 1 - i);
                    i = p + 1;
                    ml_state = MultilineState::None;
                    break;
                }
                ++p;
            }
            if (!found) {
                push(TokenType::String,
                     std::string_view(line.data() + i, N - i), col);
                col += static_cast<int>(N - i);
                i = N;
            }
        };

        if (c == '"' && spec.allow_double_quote) {
            start_string('"', MultilineState::StringDq);
            continue;
        }
        if (c == '\'' && spec.allow_single_quote) {
            start_string('\'', MultilineState::StringSq);
            continue;
        }
        if (c == '`' && spec.allow_backtick) {
            start_string('`', MultilineState::StringBt);
            continue;
        }

        // Numbers
        if (isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < N &&
             isdigit(static_cast<unsigned char>(line[i + 1])))) {
            size_t p = i;
            if (c == '0' && i + 1 < N &&
                (line[i + 1] == 'x' || line[i + 1] == 'X')) {
                p += 2;
                while (p < N &&
                       (isdigit(static_cast<unsigned char>(line[p])) ||
                        (line[p] >= 'a' && line[p] <= 'f') ||
                        (line[p] >= 'A' && line[p] <= 'F'))) ++p;
            } else if (c == '0' && i + 1 < N &&
                       (line[i + 1] == 'b' || line[i + 1] == 'B')) {
                p += 2;
                while (p < N && (line[p] == '0' || line[p] == '1')) ++p;
            } else {
                while (p < N &&
                       isdigit(static_cast<unsigned char>(line[p]))) ++p;
                if (p < N && line[p] == '.') {
                    ++p;
                    while (p < N &&
                           isdigit(static_cast<unsigned char>(line[p]))) ++p;
                }
                if (p < N && (line[p] == 'e' || line[p] == 'E')) {
                    ++p;
                    if (p < N && (line[p] == '+' || line[p] == '-')) ++p;
                    while (p < N &&
                           isdigit(static_cast<unsigned char>(line[p]))) ++p;
                }
            }
            // Type suffixes: f, u, l, ll, etc.
            while (p < N && (line[p] == 'f' || line[p] == 'F' ||
                             line[p] == 'u' || line[p] == 'U' ||
                             line[p] == 'l' || line[p] == 'L' ||
                             line[p] == 'd' || line[p] == 'D' ||
                             line[p] == 'm' || line[p] == 'M')) ++p;
            push(TokenType::Number,
                 std::string_view(line.data() + i, p - i), col);
            col += static_cast<int>(p - i);
            i = p;
            continue;
        }

        // Identifier / keyword
        if (is_ident_start(c)) {
            size_t p = i + 1;
            while (p < N && is_ident_char(line[p])) ++p;
            std::string_view word(line.data() + i, p - i);

            // Check keywords with case-insensitive for SQL, case-sensitive rest
            bool kw = false;
            bool sql_case_insensitive = false;
            for (auto k : spec.keywords) {
                if (k == word) { kw = true; break; }
                // SQL: upper-case in keyword list, match lower
                if (k.size() == word.size()) {
                    bool eq = true;
                    for (size_t q = 0; q < k.size(); ++q) {
                        char a = k[q], b = word[q];
                        if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
                        if (b >= 'A' && b <= 'Z') b = b - 'A' + 'a';
                        if (a != b) { eq = false; break; }
                    }
                    if (eq && spec.has_dash_comment /*SQL marker*/) {
                        kw = true; break;
                    }
                }
            }

            // Heuristic type check: CamelCase identifier
            bool type_like = false;
            if (!kw && word.size() >= 2) {
                if ((word[0] >= 'A' && word[0] <= 'Z')) type_like = true;
                if (word.size() >= 3 && word[word.size() - 2] == '_' &&
                    word[word.size() - 1] == 't') type_like = true; // _t suffix
            }

            // Function call: identifier immediately followed by '('
            bool is_func = false;
            {
                size_t pp = p;
                while (pp < N && (line[pp] == ' ' || line[pp] == '\t')) ++pp;
                if (pp < N && line[pp] == '(') is_func = true;
            }

            if (kw) push(TokenType::Keyword, word, col);
            else if (is_func) push(TokenType::Function, word, col);
            else if (type_like) push(TokenType::Type, word, col);
            else push(TokenType::Plain, word, col);
            col += static_cast<int>(p - i);
            i = p;
            continue;
        }

        // Operator / punctuation
        switch (c) {
            case '+': case '-': case '*': case '/': case '%':
            case '=': case '<': case '>': case '!':
            case '&': case '|': case '^': case '~':
            case '?': case ':':
                push(TokenType::Operator,
                     std::string_view(line.data() + i, 1), col);
                col++; i++;
                continue;
            case '(': case ')': case '[': case ']': case '{': case '}':
            case ';': case ',': case '.':
                push(TokenType::Punctuation,
                     std::string_view(line.data() + i, 1), col);
                col++; i++;
                continue;
            default:
                break;
        }

        // Fallback: single character plain
        push(TokenType::Plain, std::string_view(line.data() + i, 1), col);
        col++; i++;
    }

    return out;
}

/// Compatibility wrapper: previous callers expect no multiline state.
[[nodiscard]] inline std::vector<HighlightToken> tokenize_line(
    const std::string& line, const std::string& language) {

    auto spec = get_language_spec(language);
    MultilineState st = MultilineState::None;
    return tokenize_line_ex(line, spec, st, 0);
}

/// Highlight entire source string, full engine with stateful multiline.
[[nodiscard]] inline HighlightResult highlight_source(
    const std::string& source, const std::string& language) {

    HighlightResult result;
    result.language_detected = language;
    auto spec = get_language_spec(language);

    // Split into lines
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= source.size()) {
        auto nl = source.find('\n', pos);
        if (nl == std::string::npos) {
            lines.emplace_back(source.substr(pos));
            break;
        }
        lines.emplace_back(source.substr(pos, nl - pos));
        pos = nl + 1;
    }

    result.lines.reserve(lines.size());
    MultilineState ml_state = MultilineState::None;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        HighlightedLine hl;
        hl.line_number = i + 1;
        hl.tokens = tokenize_line_ex(lines[i], spec, ml_state, 0);
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

/// Large-file fold constants
constexpr int kFoldThresholdLines = 1000;
constexpr int kFoldShownLines = 200;

/// Render the full highlighted code block with optional copy corner tag,
/// line gutter, and large-file fold.
[[nodiscard]] inline Element RenderCodeHighlight(
    const CodeHighlightOptions& opts, const HighlightResult& highlighted) {

    SyntaxTheme theme = opts.theme.value_or(get_theme(ThemeName::Dark));

    int total_lines = static_cast<int>(highlighted.lines.size());
    int gutter_width = (total_lines > 0)
        ? static_cast<int>(std::format("{}", total_lines).size())
        : 1;

    // Large-file fold: show only first kFoldShownLines + "…expand…" notice.
    // Users can scroll manually via component events to reveal more lines.
    bool is_folded = (total_lines > kFoldThresholdLines);
    int eff_visible_lines = is_folded ? kFoldShownLines : total_lines;

    // Compute visible range
    int start = std::max(0, opts.scroll_offset);
    int end = std::min(eff_visible_lines, start + opts.visible_lines);
    (void)end; // used implicitly below by iterate limit

    Elements lines;
    int upper = std::min(eff_visible_lines, start + opts.visible_lines);
    for (int i = start; i < upper; ++i) {
        bool is_hl = opts.highlight_line.has_value() &&
                     *opts.highlight_line == highlighted.lines[i].line_number;
        lines.push_back(RenderHighlightedLine(
            highlighted.lines[i], theme, opts.show_line_numbers,
            is_hl, gutter_width));
    }

    if (lines.empty()) {
        lines.push_back(text("  (empty)") | dim);
    }

    // Fold notice
    if (is_folded) {
        lines.push_back(hbox({
            text(" ") | dim,
            filler(),
            text(std::format("Showing first {} of {} lines… ",
                             kFoldShownLines, total_lines))
               | color(Color::Yellow) | dim,
            text("[Expand]") | color(Color::Cyan) | bold,
            filler(),
            text(" ") | dim,
        }) | bgcolor(Color::RGB(30, 25, 0)));
    }

    // Status bar
    auto status = hbox({
        text(" " + highlighted.language_detected) | dim | color(Color::Cyan),
        text(std::format("  {}/{} lines",
                         std::min(upper, total_lines), total_lines)) | dim,
        filler(),
        text("[j/k] scroll [q] close") | dim,
        text(" "),
    });

    // Build content box + copy corner tag (upper-right)
    auto body = vbox(lines) | flex | bgcolor(theme.bg);
    auto box = vbox({
        body,
        status,
    });

    // Copy "tag" rendered as a short overlay in the top-right corner.
    // FTXUI doesn't have z-order; we use hbox with filler and a padded tag
    // element above the code body, on a topbar row.
    auto copy_tag = hbox({
        filler(),
        text(" [Copy] ") | color(Color::GrayDark)
                        | bgcolor(Color::RGB(40, 40, 40))
                        | borderStyled(Color::GrayDark),
    });

    return vbox({
        copy_tag,
        box,
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

// ============================================================
// Factory: RenderCodeBlock — public API for assistant_text_message
// Takes raw code string + language, returns pre-built Element.
// ===========================================================================

struct RenderCodeBlockOptions {
    std::string_view code;
    std::string_view language;
    std::optional<std::string_view> file_path;      // optional display name
    int visible_lines = 40;
    bool show_line_numbers = true;
    bool show_copy_tag = true;
    std::optional<SyntaxTheme> theme;
};

[[nodiscard]] inline Element RenderCodeBlock(const RenderCodeBlockOptions& opts) {
    CodeHighlightOptions c;
    c.source = std::string(opts.code);
    c.language = std::string(opts.language);
    c.theme = opts.theme;
    c.show_line_numbers = opts.show_line_numbers;
    c.visible_lines = opts.visible_lines;

    HighlightResult hl = highlight_source(c.source, c.language);

    auto title_bar = [&]() -> Element {
        Elements parts;
        parts.push_back(text(" ") | dim);
        if (opts.file_path && !opts.file_path->empty()) {
            parts.push_back(text(std::string(*opts.file_path))
                            | bold | color(Color::Cyan));
        } else if (!opts.language.empty()) {
            parts.push_back(text(std::string(opts.language))
                            | bold | color(Color::Cyan));
        }
        if (opts.show_copy_tag) {
            parts.push_back(filler());
            parts.push_back(text(" Copy ") | color(Color::GrayDark)
                                          | bgcolor(Color::RGB(40,40,40)));
            parts.push_back(text(" "));
        } else {
            parts.push_back(filler());
        }
        return hbox(std::move(parts));
    };

    return vbox({
        title_bar(),
        RenderCodeHighlight(c, hl) | flex,
    });
}

} // namespace cc::ui::code_highlight
