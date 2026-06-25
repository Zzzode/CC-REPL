// Sed Edit Parser — Parses sed-style edit commands into structured data
//
// Migrated from: src/tools/BashTool/sedEditParser.ts
// Supports: s/pattern/replacement/flags (substitution), /pattern/d (delete),
//           /pattern/a\text (append), /pattern/i\text (insert),
//           /pattern/c\text (change), and address-based variants.
//
// Parser strategy: hand-written state machine (not std::regex).  The sed
// syntax is small enough that manual parsing is both more reliable and
// substantially faster than chaining regex calls.  Error types use the
// project's std::expected-based pattern.

module;

#include <cctype>
#include <expected>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

export module cc.tools.sed_edit_parser;

import cc.utils.argument_substitution;
import cc.utils.bash_shell_quoting;

export namespace cc::tools::sed_edit_parser {

// ---------------------------------------------------------------------------
// Error type
// ---------------------------------------------------------------------------

enum class SedParseError {
    NotASedCommand,          ///< String does not begin with "sed "
    MalformedShellTokens,    ///< Shell quoting is unbalanced / invalid
    MissingInPlaceFlag,      ///< In-place edit expected -i flag
    MissingExpression,       ///< No sed expression found
    MissingFilePath,         ///< No target file path found
    MultipleFiles,           ///< More than one file argument
    UnknownFlag,             ///< Unrecognised command-line flag
    InvalidSubstitution,     ///< s/// expression is malformed
    InvalidFlags,            ///< Substitution flags contain disallowed chars
    InvalidAddress,          ///< Address range is not parseable
    GlobToken,               ///< Glob patterns present; not supported
};

[[nodiscard]] constexpr std::string_view sed_parse_error_name(SedParseError e) noexcept {
    switch (e) {
        case SedParseError::NotASedCommand:        return "not_a_sed_command";
        case SedParseError::MalformedShellTokens:  return "malformed_shell_tokens";
        case SedParseError::MissingInPlaceFlag:    return "missing_in_place_flag";
        case SedParseError::MissingExpression:     return "missing_expression";
        case SedParseError::MissingFilePath:       return "missing_file_path";
        case SedParseError::MultipleFiles:         return "multiple_files";
        case SedParseError::UnknownFlag:           return "unknown_flag";
        case SedParseError::InvalidSubstitution:   return "invalid_substitution";
        case SedParseError::InvalidFlags:          return "invalid_flags";
        case SedParseError::InvalidAddress:        return "invalid_address";
        case SedParseError::GlobToken:             return "glob_token";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

/// Operation kind produced by the parser.
enum class SedOp {
    Substitute,   ///< s/pattern/replacement/flags
    Delete,       ///< [address]d
    Append,       ///< [address]a\text
    Insert,       ///< [address]i\text
    Change,       ///< [address]c\text
    Print,        ///< [address]p
    Transliterate ///< y/src/dest/
};

/// A single parsed sed command (one expression unit).
struct SedCommand {
    SedOp op{SedOp::Substitute};

    // Address range (optional).  Empty string means "no address".
    // A range is stored as two strings ("start", "end"); for single addresses
    // start is populated and end is empty.
    std::string address_start;
    std::string address_end;

    // Pattern (for s, y, and /pattern/ addressed commands)
    std::string pattern;

    // Replacement / appended / inserted / changed text
    std::string replacement;

    // Substitution flag characters (g, p, i, I, m, M, 1-9)
    std::string flags;

    // Whether the whole command was invoked with -E / -r
    bool extended_regex{false};

    // True if the 'g' (global) flag is present
    [[nodiscard]] bool global() const noexcept {
        return flags.find('g') != std::string::npos;
    }

    // True if any of the case-insensitive flags are present
    [[nodiscard]] bool case_insensitive() const noexcept {
        return flags.find('i') != std::string::npos || flags.find('I') != std::string::npos;
    }

    // True if any multiline flag is present
    [[nodiscard]] bool multiline() const noexcept {
        return flags.find('m') != std::string::npos || flags.find('M') != std::string::npos;
    }
};

/// Top-level in-place edit descriptor (matches TS SedEditInfo shape).
struct SedEditInfo {
    std::string file_path;         ///< Target file
    std::string pattern;           ///< Regex pattern (substitution only)
    std::string replacement;       ///< Replacement text
    std::string flags;             ///< Substitution flag chars
    bool extended_regex{false};    ///< -E / -r was passed
    std::vector<SedCommand> commands; ///< All parsed sub-commands
};

/// Result alias — follows project std::expected convention.
template <typename T>
using SedResult = std::expected<T, SedParseError>;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace detail {

/// Trim leading/trailing ASCII whitespace.
[[nodiscard]] inline std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

/// Check whether a character is valid in a substitution flag string.
/// TS allowlist: /^[gpimIM1-9]*$/
[[nodiscard]] inline bool is_valid_flag_char(char c) noexcept {
    return c == 'g' || c == 'p' || c == 'i' || c == 'I' || c == 'm' || c == 'M' ||
           (c >= '1' && c <= '9');
}

/// Validate all flag characters.
[[nodiscard]] inline bool validate_flags(std::string_view flags) noexcept {
    // Also enforce: at most one numeric (n-th match) flag.
    int numeric_count = 0;
    for (char c : flags) {
        if (!is_valid_flag_char(c)) return false;
        if (c >= '1' && c <= '9') ++numeric_count;
    }
    return numeric_count <= 1;
}

/// Parse a substitution expression `s<delim>pattern<delim>repl<delim>flags`.
/// Only '/' is accepted as delimiter (matches the TS parser).
/// Returns the populated SedCommand, or nullopt on failure.
[[nodiscard]] inline std::optional<SedCommand> parse_substitution(std::string_view expr) {
    // Must start with "s/"
    if (expr.size() < 2 || expr[0] != 's' || expr[1] != '/') {
        return std::nullopt;
    }
    std::string_view rest = expr.substr(2);

    SedCommand cmd;
    cmd.op = SedOp::Substitute;

    enum class State { Pattern, Replacement, Flags } state = State::Pattern;
    std::string *current_field = &cmd.pattern;

    for (std::size_t j = 0; j < rest.size(); ++j) {
        const char c = rest[j];

        // Escape: consume next character literally
        if (c == '\\' && j + 1 < rest.size()) {
            current_field->push_back(c);
            current_field->push_back(rest[j + 1]);
            ++j;
            continue;
        }

        if (c == '/') {
            if (state == State::Pattern) {
                state = State::Replacement;
                current_field = &cmd.replacement;
            } else if (state == State::Replacement) {
                state = State::Flags;
                current_field = &cmd.flags;
            } else {
                // Extra delimiter after flags — unexpected
                return std::nullopt;
            }
            continue;
        }

        current_field->push_back(c);
    }

    // Must have reached Flags state (i.e. at least two '/' delimiters)
    if (state != State::Flags) {
        return std::nullopt;
    }

    if (!validate_flags(cmd.flags)) {
        return std::nullopt;
    }

    return cmd;
}

/// Try to split a command string into shell tokens using the existing
/// argument-substitution helper.  Glob-like tokens are detected and rejected.
[[nodiscard]] inline SedResult<std::vector<std::string>> tokenize_args(std::string_view text) {
    auto parsed = cc::utils::argument_substitution::parse_shell_like_arguments(text);
    if (!parsed.has_value()) {
        return std::unexpected(SedParseError::MalformedShellTokens);
    }
    for (const auto& tok : *parsed) {
        // Glob tokens: any of * ? [  with the meaning that we cannot statically
        // resolve to a single file path.
        for (char c : tok) {
            if (c == '*' || c == '?' || c == '[') {
                return std::unexpected(SedParseError::GlobToken);
            }
        }
    }
    return std::move(*parsed);
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public API — parse_sed_commands
// ---------------------------------------------------------------------------

/// Parse a full `sed …` command line into a vector of structured commands.
/// The input is the full raw shell command (e.g. "sed -i 's/foo/bar/g' f.txt").
[[nodiscard]] inline SedResult<std::vector<SedCommand>>
parse_sed_commands(std::string_view input) {
    const auto trimmed = detail::trim(input);

    // Must start with "sed"
    if (!trimmed.starts_with("sed")) {
        return std::unexpected(SedParseError::NotASedCommand);
    }
    // Make sure it's the word "sed" (sedi, sedx, … are not the command)
    if (trimmed.size() > 3 && !std::isspace(static_cast<unsigned char>(trimmed[3]))) {
        return std::unexpected(SedParseError::NotASedCommand);
    }

    const auto after_sed = detail::trim(trimmed.substr(3));
    auto tokens_res = detail::tokenize_args(after_sed);
    if (!tokens_res) return std::unexpected(tokens_res.error());
    const auto& args = *tokens_res;

    bool extended = false;
    std::vector<std::string> expressions;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];

        // -i or --in-place (in-place flag is intentionally consumed but not
        // interpreted here — it matters at the SedEditInfo layer below).
        if (arg == "-i" || arg == "--in-place") {
            // Next argument, if non-flag-ish, may be a backup suffix.
            if (i + 1 < args.size()) {
                const auto& next = args[i + 1];
                if (!next.empty() && !next.starts_with('-') &&
                    (next == "." || next.starts_with('.'))) {
                    ++i; // consume suffix
                }
            }
            continue;
        }
        if (arg.starts_with("-i")) {
            // -i.bak or similar inline suffix
            continue;
        }

        // Extended regex
        if (arg == "-E" || arg == "-r" || arg == "--regexp-extended") {
            extended = true;
            continue;
        }

        // Explicit expression
        if (arg == "-e" || arg == "--expression") {
            if (i + 1 >= args.size()) {
                return std::unexpected(SedParseError::InvalidSubstitution);
            }
            expressions.push_back(args[++i]);
            continue;
        }
        if (arg.starts_with("--expression=")) {
            expressions.push_back(arg.substr(std::string_view("--expression=").size()));
            continue;
        }

        // Unknown flag → reject to stay conservative
        if (arg.starts_with('-')) {
            return std::unexpected(SedParseError::UnknownFlag);
        }

        // Positional: first non-flag, non-file argument is the expression.
        // File arguments we skip — commands only live in expressions.
        if (expressions.empty()) {
            expressions.push_back(arg);
        }
        // Otherwise: file path, ignore for command parsing
    }

    std::vector<SedCommand> out;
    for (const auto& expr : expressions) {
        // Support semicolon-separated sub-expressions.
        std::string_view remaining = detail::trim(expr);
        while (!remaining.empty()) {
            // Find the next ';' outside of /-delimited pattern regions.
            std::size_t sep = std::string_view::npos;
            bool in_pattern = false;
            bool in_replacement = false;
            for (std::size_t k = 0; k < remaining.size(); ++k) {
                const char c = remaining[k];
                if (c == '\\' && k + 1 < remaining.size()) {
                    ++k;
                    continue;
                }
                if (c == '/') {
                    if (!in_pattern && !in_replacement) {
                        in_pattern = true;
                    } else if (in_pattern) {
                        in_pattern = false;
                        in_replacement = true;
                    } else if (in_replacement) {
                        in_replacement = false;
                    }
                    continue;
                }
                if (c == ';' && !in_pattern && !in_replacement) {
                    sep = k;
                    break;
                }
            }

            std::string_view piece;
            if (sep == std::string_view::npos) {
                piece = remaining;
                remaining = {};
            } else {
                piece = remaining.substr(0, sep);
                remaining = remaining.substr(sep + 1);
            }
            piece = detail::trim(piece);
            if (piece.empty()) continue;

            auto parsed = detail::parse_substitution(piece);
            if (!parsed) {
                // Fall back: try delete / print / append / insert / change forms.
                if (piece.ends_with('d')) {
                    SedCommand c;
                    c.op = SedOp::Delete;
                    auto addr = piece.substr(0, piece.size() - 1);
                    addr = detail::trim(addr);
                    if (addr.starts_with('/')) {
                        // /pattern/d
                        if (addr.size() > 2 && addr.ends_with('/')) {
                            c.pattern = std::string(addr.substr(1, addr.size() - 2));
                        } else {
                            return std::unexpected(SedParseError::InvalidAddress);
                        }
                    } else {
                        c.address_start = std::string(addr);
                    }
                    out.push_back(std::move(c));
                    continue;
                }
                if (piece.ends_with('p')) {
                    SedCommand c;
                    c.op = SedOp::Print;
                    auto addr = piece.substr(0, piece.size() - 1);
                    addr = detail::trim(addr);
                    if (addr.starts_with('/')) {
                        if (addr.size() > 2 && addr.ends_with('/')) {
                            c.pattern = std::string(addr.substr(1, addr.size() - 2));
                        } else {
                            return std::unexpected(SedParseError::InvalidAddress);
                        }
                    } else {
                        c.address_start = std::string(addr);
                    }
                    out.push_back(std::move(c));
                    continue;
                }
                // a / i / c commands take the form: [address]op\text
                auto op_pos = piece.find("\\");
                if (op_pos != std::string_view::npos && op_pos > 0) {
                    const char op_char = piece[op_pos - 1];
                    if (op_char == 'a' || op_char == 'i' || op_char == 'c') {
                        SedCommand c;
                        if (op_char == 'a') c.op = SedOp::Append;
                        else if (op_char == 'i') c.op = SedOp::Insert;
                        else c.op = SedOp::Change;
                        auto addr = detail::trim(piece.substr(0, op_pos - 1));
                        if (!addr.empty()) c.address_start = std::string(addr);
                        if (op_pos + 1 < piece.size()) {
                            c.replacement = std::string(piece.substr(op_pos + 1));
                        }
                        out.push_back(std::move(c));
                        continue;
                    }
                }
                // y/src/dest/ — transliteration
                if (!piece.empty() && piece[0] == 'y') {
                    SedCommand c;
                    c.op = SedOp::Transliterate;
                    auto yp = detail::parse_substitution(piece);
                    if (yp) {
                        c.pattern = std::move(yp->pattern);
                        c.replacement = std::move(yp->replacement);
                        out.push_back(std::move(c));
                        continue;
                    }
                }
                // Unknown expression form → reject
                return std::unexpected(SedParseError::InvalidSubstitution);
            }

            parsed->extended_regex = extended;
            out.push_back(std::move(*parsed));
        }
    }

    if (out.empty()) {
        return std::unexpected(SedParseError::MissingExpression);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Public API — SedEditInfo parsers (1:1 TS migration)
// ---------------------------------------------------------------------------

/// Parse the full command into an in-place edit descriptor.
/// Returns `nullopt`-via-unexpected whenever the command is NOT a simple
/// `sed -i 's/pattern/replacement/flags' file` form.
[[nodiscard]] inline SedResult<SedEditInfo>
parse_sed_edit_command(std::string_view command) {
    const auto trimmed = detail::trim(command);

    if (!trimmed.starts_with("sed")) {
        return std::unexpected(SedParseError::NotASedCommand);
    }
    if (trimmed.size() > 3 && !std::isspace(static_cast<unsigned char>(trimmed[3]))) {
        return std::unexpected(SedParseError::NotASedCommand);
    }

    const auto after_sed = detail::trim(trimmed.substr(3));
    auto tokens_res = detail::tokenize_args(after_sed);
    if (!tokens_res) return std::unexpected(tokens_res.error());
    const auto& args = *tokens_res;

    bool has_in_place = false;
    bool extended_regex = false;
    std::optional<std::string> expression;
    std::optional<std::string> file_path;

    for (std::size_t i = 0; i < args.size();) {
        const auto& arg = args[i];

        if (arg == "-i" || arg == "--in-place") {
            has_in_place = true;
            ++i;
            if (i < args.size()) {
                const auto& next = args[i];
                if (!next.starts_with('-') && (next.empty() || next.starts_with('.'))) {
                    ++i;
                }
            }
            continue;
        }
        if (arg.starts_with("-i")) {
            has_in_place = true;
            ++i;
            continue;
        }

        if (arg == "-E" || arg == "-r" || arg == "--regexp-extended") {
            extended_regex = true;
            ++i;
            continue;
        }

        if (arg == "-e" || arg == "--expression") {
            if (expression.has_value()) {
                return std::unexpected(SedParseError::InvalidSubstitution);
            }
            if (i + 1 >= args.size()) {
                return std::unexpected(SedParseError::MissingExpression);
            }
            expression = args[i + 1];
            i += 2;
            continue;
        }
        if (arg.starts_with("--expression=")) {
            if (expression.has_value()) {
                return std::unexpected(SedParseError::InvalidSubstitution);
            }
            expression = arg.substr(std::string_view("--expression=").size());
            ++i;
            continue;
        }

        if (arg.starts_with('-')) {
            return std::unexpected(SedParseError::UnknownFlag);
        }

        if (!expression.has_value()) {
            expression = arg;
        } else if (!file_path.has_value()) {
            file_path = arg;
        } else {
            return std::unexpected(SedParseError::MultipleFiles);
        }
        ++i;
    }

    if (!has_in_place) return std::unexpected(SedParseError::MissingInPlaceFlag);
    if (!expression.has_value()) return std::unexpected(SedParseError::MissingExpression);
    if (!file_path.has_value()) return std::unexpected(SedParseError::MissingFilePath);

    auto parsed_cmd = detail::parse_substitution(*expression);
    if (!parsed_cmd) return std::unexpected(SedParseError::InvalidSubstitution);

    SedEditInfo info;
    info.file_path = std::move(*file_path);
    info.pattern = std::move(parsed_cmd->pattern);
    info.replacement = std::move(parsed_cmd->replacement);
    info.flags = std::move(parsed_cmd->flags);
    info.extended_regex = extended_regex;
    parsed_cmd->extended_regex = extended_regex;
    info.commands.push_back(std::move(*parsed_cmd));

    return info;
}

/// Convenience: returns true iff the command is a valid sed in-place edit.
[[nodiscard]] inline bool is_sed_in_place_edit(std::string_view command) {
    return parse_sed_edit_command(command).has_value();
}

// ---------------------------------------------------------------------------
// BRE → ERE conversion
// ---------------------------------------------------------------------------

namespace detail {

// Placeholder tokens used during BRE→ERE conversion.  Using the same
// null-byte-delimited sentinel approach as TS but static C++ strings.
constexpr std::string_view kBsPh  = "\x00" "BACKSLASH"  "\x00";
constexpr std::string_view kPlusPh= "\x00" "PLUS"       "\x00";
constexpr std::string_view kQstPh = "\x00" "QUESTION"   "\x00";
constexpr std::string_view kPipePh= "\x00" "PIPE"       "\x00";
constexpr std::string_view kLpPh  = "\x00" "LPAREN"     "\x00";
constexpr std::string_view kRpPh  = "\x00" "RPAREN"     "\x00";

inline void replace_all_str(std::string& s, std::string_view from, std::string_view to) {
    if (from.empty()) return;
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

} // namespace detail

/// Convert a sed BRE pattern into a C++ (ERE) regex pattern.
/// Mirrors the TS `applySedSubstitution` preprocessing step.
[[nodiscard]] inline std::string bre_to_ere(std::string pattern, bool extended_regex) {
    // Unescape \/ → /  (always)
    detail::replace_all_str(pattern, "\\/", "/");

    if (extended_regex) {
        return pattern; // ERE matches C++ std::regex semantics closely enough
    }

    // Step 1: protect literal backslash pairs.
    detail::replace_all_str(pattern, "\\\\", std::string(detail::kBsPh));
    // Step 2: convert BRE-escaped metachars → placeholders
    detail::replace_all_str(pattern, "\\+",  std::string(detail::kPlusPh));
    detail::replace_all_str(pattern, "\\?",  std::string(detail::kQstPh));
    detail::replace_all_str(pattern, "\\|",  std::string(detail::kPipePh));
    detail::replace_all_str(pattern, "\\(",  std::string(detail::kLpPh));
    detail::replace_all_str(pattern, "\\)",  std::string(detail::kRpPh));
    // Step 3: escape literal (un-escaped) metacharacters
    detail::replace_all_str(pattern, "+", "\\+");
    detail::replace_all_str(pattern, "?", "\\?");
    detail::replace_all_str(pattern, "|", "\\|");
    detail::replace_all_str(pattern, "(", "\\(");
    detail::replace_all_str(pattern, ")", "\\)");
    // Step 4: restore placeholders → ERE equivalents
    detail::replace_all_str(pattern, detail::kBsPh,   "\\\\");
    detail::replace_all_str(pattern, detail::kPlusPh, "+");
    detail::replace_all_str(pattern, detail::kQstPh,  "?");
    detail::replace_all_str(pattern, detail::kPipePh, "|");
    detail::replace_all_str(pattern, detail::kLpPh,   "(");
    detail::replace_all_str(pattern, detail::kRpPh,   ")");

    return pattern;
}

// ---------------------------------------------------------------------------
// apply_sed_substitution — 1:1 TS migration
// ---------------------------------------------------------------------------

/// Apply a single substitution to content, mimicking the JS replace() call
/// semantics of the TS module.  Uses std::regex internally.
[[nodiscard]] inline std::string
apply_sed_substitution(std::string_view content, const SedEditInfo& info) {
    // Build regex flags
    std::regex_constants::syntax_option_type opts = std::regex::ECMAScript;
    if (info.extended_regex) opts |= std::regex::icase; // no-op placeholder — handled below
    // ECMAScript is the default and matches JS semantics closest.

    auto ere_pattern = bre_to_ere(info.pattern, info.extended_regex);

    try {
        std::regex re;
        if (info.flags.find('i') != std::string::npos ||
            info.flags.find('I') != std::string::npos) {
            re.assign(ere_pattern, std::regex::icase);
        } else {
            re.assign(ere_pattern);
        }

        // Prepare replacement string:
        //   \/ → /
        //   \& → placeholder → literal &
        //   &  → $& (full match, escaped as $$& in C++ regex)
        // (C++ std::regex_replace uses $& for the whole match too — like JS.)

        constexpr std::string_view kEscAmpPh =
            "___ESCAPED_AMPERSAND_2F7A9C4E___";
        std::string repl = info.replacement;
        detail::replace_all_str(repl, "\\/", "/");
        detail::replace_all_str(repl, "\\&", std::string(kEscAmpPh));
        // In C++ regex_replace, $& is the match; a literal $ must be $$.
        detail::replace_all_str(repl, "&", "$$&");
        detail::replace_all_str(repl, kEscAmpPh, "&");

        const bool global = info.flags.find('g') != std::string::npos;
        if (global) {
            return std::regex_replace(std::string(content), re, repl);
        }
        return std::regex_replace(std::string(content), re, repl,
                                   std::regex_constants::format_first_only);
    } catch (const std::regex_error&) {
        // Invalid regex → return content unchanged (mirrors TS behaviour).
        return std::string(content);
    }
}

/// Overload taking a single SedCommand directly.
[[nodiscard]] inline std::string
apply_sed_substitution(std::string_view content, const SedCommand& cmd) {
    SedEditInfo info;
    info.pattern = cmd.pattern;
    info.replacement = cmd.replacement;
    info.flags = cmd.flags;
    info.extended_regex = cmd.extended_regex;
    return apply_sed_substitution(content, info);
}

} // namespace cc::tools::sed_edit_parser
