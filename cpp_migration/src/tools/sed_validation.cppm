// Sed Validation — Safety allowlist / denylist for sed commands
//
// Migrated from: src/tools/BashTool/sedValidation.ts
//
// Two-layer model:
//   1. ALLOWLIST: every sed command must match a known-safe pattern
//      (line-printing with -n, or substitution).  Semicolons, unknown
//      flags, y-commands with w/e suffixes — all rejected.
//   2. DENYLIST:  even after passing the allowlist, any expression
//      containing w/W/e/E write/execute commands is rejected.
//
// Path-scope permission checks are delegated to bash_validation (already
// migrated) — do not duplicate them here.

module;

#include <algorithm>
#include <cctype>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

export module cc.tools.sed_validation;

import cc.tools.bash_validation;
import cc.tools.bash_security;
import cc.utils.argument_substitution;
import cc.tools.sed_edit_parser;

export namespace cc::tools::sed_validation {

using cc::tools::bash_validation::PathValidationContext;
using cc::tools::bash_validation::ValidationResult;

// ---------------------------------------------------------------------------
// Extract sed expressions from the raw command.
// Mirrors extractSedExpressions() in TS.
// ---------------------------------------------------------------------------

namespace detail {

[[nodiscard]] inline std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

/// Parse shell-style tokens; returns std::nullopt on unbalanced quotes.
[[nodiscard]] inline std::optional<std::vector<std::string>>
tokenize(std::string_view text) {
    return cc::utils::argument_substitution::parse_shell_like_arguments(text);
}

} // namespace detail

/// Extract the sed expression tokens from the full `sed …` command string.
/// Returns std::nullopt on parse error (callers treat parse errors as "not
/// allowed" in allowlist checks).
[[nodiscard]] inline std::optional<std::vector<std::string>>
extract_sed_expressions(std::string_view command) {
    std::vector<std::string> expressions;

    const auto trimmed = detail::trim(command);
    // Must start with "sed "
    if (!trimmed.starts_with("sed")) return expressions;
    if (trimmed.size() > 3 && !std::isspace(static_cast<unsigned char>(trimmed[3]))) {
        return expressions;
    }

    const auto after_sed = detail::trim(trimmed.substr(3));

    // Defense-in-depth: reject dangerous combined short-flag combinations
    // -ew, -eW, -ee, -we, -wE — combined -e with w/W/e commands.
    {
        const std::string s(after_sed);
        static const std::regex ew_re(R"(-e[wWe])");
        static const std::regex we_re(R"(-w[eE])");
        if (std::regex_search(s, ew_re) || std::regex_search(s, we_re)) {
            return std::nullopt;
        }
    }

    auto tokens = detail::tokenize(after_sed);
    if (!tokens.has_value()) return std::nullopt;

    bool found_e_flag = false;
    bool found_expression = false;

    for (std::size_t i = 0; i < tokens->size(); ++i) {
        const auto& arg = (*tokens)[i];

        if (arg == "-e" || arg == "--expression") {
            if (i + 1 >= tokens->size()) continue;
            found_e_flag = true;
            expressions.push_back((*tokens)[++i]);
            continue;
        }
        if (arg.starts_with("--expression=")) {
            found_e_flag = true;
            expressions.push_back(arg.substr(std::string_view("--expression=").size()));
            continue;
        }
        if (arg.starts_with("-e=")) {
            found_e_flag = true;
            expressions.push_back(arg.substr(std::string_view("-e=").size()));
            continue;
        }

        if (arg.starts_with('-')) continue; // skip other flags

        if (!found_e_flag && !found_expression) {
            expressions.push_back(arg);
            found_expression = true;
            continue;
        }
        // Everything else is a filename — stop
        break;
    }

    return expressions;
}

/// Check whether the command has file arguments (beyond the sed expression).
/// Mirrors hasFileArgs() in TS.
[[nodiscard]] inline bool has_file_args(std::string_view command) {
    const auto trimmed = detail::trim(command);
    if (!trimmed.starts_with("sed")) return false;
    if (trimmed.size() > 3 && !std::isspace(static_cast<unsigned char>(trimmed[3]))) {
        return false;
    }
    const auto after_sed = detail::trim(trimmed.substr(3));
    auto tokens = detail::tokenize(after_sed);
    if (!tokens.has_value()) return true; // parse failure → assume dangerous

    std::size_t arg_count = 0;
    bool has_e_flag = false;

    for (std::size_t i = 0; i < tokens->size(); ++i) {
        const auto& arg = (*tokens)[i];

        // Glob token → definitely resolves to file(s)
        for (char c : arg) {
            if (c == '*' || c == '?' || c == '[') return true;
        }

        if ((arg == "-e" || arg == "--expression") && i + 1 < tokens->size()) {
            has_e_flag = true;
            ++i;
            continue;
        }
        if (arg.starts_with("--expression=") || arg.starts_with("-e=")) {
            has_e_flag = true;
            continue;
        }
        if (arg.starts_with('-')) continue;

        ++arg_count;

        if (has_e_flag) return true;          // every positional is a file
        if (arg_count > 1) return true;        // first positional = expression
    }

    return false;
}

// ---------------------------------------------------------------------------
// Flag allowlist helpers
// ---------------------------------------------------------------------------

namespace detail {

inline bool flag_is_allowed(std::string_view flag,
                            const std::vector<std::string_view>& allowlist) {
    // Exact match
    for (auto a : allowlist) {
        if (flag == a) return true;
    }
    // Combined short flags like -nE — every character must be in allowlist
    // as a single-char flag.
    if (flag.size() >= 3 && flag[0] == '-' && flag[1] != '-') {
        for (std::size_t i = 1; i < flag.size(); ++i) {
            std::string single;
            single.push_back('-');
            single.push_back(flag[i]);
            bool found = false;
            for (auto a : allowlist) {
                if (a == single) {
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
        return true;
    }
    return false;
}

inline bool validate_flags_against_allowlist(
    const std::vector<std::string>& flags,
    const std::vector<std::string_view>& allowlist) {
    for (const auto& f : flags) {
        if (!flag_is_allowed(f, allowlist)) return false;
    }
    return true;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Pattern 1: line-printing (sed -n with print commands)
// ---------------------------------------------------------------------------

/// Strict print-command check — matches TS isPrintCommand().
/// Allowed forms: p, Np, N,Mp  (N, M are non-empty digit sequences).
[[nodiscard]] inline bool is_print_command(std::string_view cmd) {
    if (cmd.empty()) return false;
    // Must end with 'p'
    if (cmd.back() != 'p') return false;
    const auto body = cmd.substr(0, cmd.size() - 1);
    if (body.empty()) return true; // bare "p"

    // Possible forms: digits  OR  digits,digits
    auto comma = body.find(',');
    if (comma == std::string_view::npos) {
        for (char c : body) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        return true;
    }
    auto first = body.substr(0, comma);
    auto second = body.substr(comma + 1);
    if (first.empty() || second.empty()) return false;
    for (char c : first) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    for (char c : second) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

/// Check the command matches the line-printing pattern.
[[nodiscard]] inline bool is_line_printing_command(
    std::string_view command,
    const std::vector<std::string>& expressions) {

    const auto trimmed = detail::trim(command);
    if (!trimmed.starts_with("sed")) return false;
    if (trimmed.size() > 3 && !std::isspace(static_cast<unsigned char>(trimmed[3]))) {
        return false;
    }
    const auto after_sed = detail::trim(trimmed.substr(3));
    auto tokens = detail::tokenize(after_sed);
    if (!tokens.has_value()) return false;

    // Extract all flag arguments (anything starting with '-')
    std::vector<std::string> flags;
    for (const auto& t : *tokens) {
        if (t.starts_with('-') && t != "--") flags.push_back(t);
    }

    // Allowlist for pattern 1
    static const std::vector<std::string_view> allowed = {
        "-n", "--quiet", "--silent",
        "-E", "--regexp-extended", "-r",
        "-z", "--zero-terminated",
        "--posix",
    };
    if (!detail::validate_flags_against_allowlist(flags, allowed)) return false;

    // Require -n / --quiet / --silent (or combined form with 'n')
    bool has_n = false;
    for (const auto& f : flags) {
        if (f == "-n" || f == "--quiet" || f == "--silent") { has_n = true; break; }
        if (f.size() >= 2 && f[0] == '-' && f[1] != '-' && f.find('n') != std::string::npos) {
            has_n = true;
            break;
        }
    }
    if (!has_n) return false;

    if (expressions.empty()) return false;

    // Every expression must be all print commands (semicolon separated)
    for (const auto& expr : expressions) {
        std::string_view rest = detail::trim(expr);
        while (!rest.empty()) {
            auto sc = rest.find(';');
            std::string_view piece;
            if (sc == std::string_view::npos) {
                piece = rest;
                rest = {};
            } else {
                piece = rest.substr(0, sc);
                rest = rest.substr(sc + 1);
            }
            piece = detail::trim(piece);
            if (!is_print_command(piece)) return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Pattern 2: substitution commands
// ---------------------------------------------------------------------------

namespace detail {

/// Validate that `expr` is a well-formed s/pattern/replacement/flags string,
/// where flags are drawn from [gpimIM] with at most one digit 1-9.
[[nodiscard]] inline bool validate_substitution_expr(std::string_view expr) {
    if (expr.empty()) return false;
    // Must start with "s/"
    if (expr.size() < 2 || expr[0] != 's' || expr[1] != '/') return false;

    std::string_view rest = expr.substr(2);

    // Walk through, counting unescaped '/' — need exactly 2.
    int delim_count = 0;
    std::size_t last_delim = std::string_view::npos;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '\\' && i + 1 < rest.size()) {
            ++i;
            continue;
        }
        if (rest[i] == '/') {
            ++delim_count;
            last_delim = i;
        }
    }
    if (delim_count != 2) return false;

    // Extract flags
    const auto flags = last_delim + 1 < rest.size()
                           ? rest.substr(last_delim + 1)
                           : std::string_view{};

    // Allow: zero or more [gpimIM] followed by at most one [1-9] followed by
    // zero or more [gpimIM].
    bool seen_digit = false;
    for (char c : flags) {
        const bool is_gpim = c == 'g' || c == 'p' || c == 'i' || c == 'I' || c == 'm' || c == 'M';
        if (is_gpim) continue;
        if (c >= '1' && c <= '9') {
            if (seen_digit) return false;
            seen_digit = true;
            continue;
        }
        return false;
    }
    return true;
}

} // namespace detail

/// Pattern 2 check — matches TS isSubstitutionCommand().
[[nodiscard]] inline bool is_substitution_command(
    std::string_view command,
    const std::vector<std::string>& expressions,
    bool has_file_args_flag,
    bool allow_file_writes) {

    if (!allow_file_writes && has_file_args_flag) return false;

    const auto trimmed = detail::trim(command);
    if (!trimmed.starts_with("sed")) return false;
    if (trimmed.size() > 3 && !std::isspace(static_cast<unsigned char>(trimmed[3]))) {
        return false;
    }
    const auto after_sed = detail::trim(trimmed.substr(3));
    auto tokens = detail::tokenize(after_sed);
    if (!tokens.has_value()) return false;

    std::vector<std::string> flags;
    for (const auto& t : *tokens) {
        if (t.starts_with('-') && t != "--") flags.push_back(t);
    }

    std::vector<std::string_view> allowed = {
        "-E", "--regexp-extended", "-r", "--posix",
    };
    if (allow_file_writes) {
        allowed.push_back("-i");
        allowed.push_back("--in-place");
    }
    if (!detail::validate_flags_against_allowlist(flags, allowed)) return false;

    if (expressions.size() != 1) return false;

    const auto& expr = expressions[0];
    const auto etrim = detail::trim(expr);
    if (etrim.empty()) return false;
    if (etrim[0] != 's') return false;

    return detail::validate_substitution_expr(etrim);
}

// ---------------------------------------------------------------------------
// Denylist — containsDangerousOperations
// ---------------------------------------------------------------------------

namespace detail {

// Helper: "is the entire ASCII range [0x01..0x7F]"
[[nodiscard]] inline bool is_ascii_only(std::string_view s) noexcept {
    for (unsigned char c : s) {
        if (c == 0x00 || c > 0x7F) return false;
    }
    return true;
}

} // namespace detail

/// Broad rejections — mirrors TS containsDangerousOperations().
/// Returns true when the expression is dangerous.
[[nodiscard]] inline bool contains_dangerous_operations(std::string_view expr) {
    const auto cmd = detail::trim(expr);
    if (cmd.empty()) return false;
    const std::string s(cmd);

    // Non-ASCII characters (homoglyph / combining marks)
    if (!detail::is_ascii_only(cmd)) return true;

    // Curly braces (blocks) — too complex
    if (s.find('{') != std::string::npos || s.find('}') != std::string::npos) return true;

    // Newlines — multi-line commands
    if (s.find('\n') != std::string::npos) return true;

    // Hash (#) not immediately after 's' (i.e. not s#delim#repl#)
    {
        auto h = s.find('#');
        if (h != std::string::npos && !(h > 0 && s[h - 1] == 's')) return true;
    }

    // Negation:  !/pattern/,  /pattern/!,  1,10!,  $!  but NOT s!...!... (delim)
    static const std::regex neg_re(R"((^!|[/\d$]!))");
    if (std::regex_search(s, neg_re)) {
        // Allow  s!pattern!repl!  — detect 's' immediately before the '!' used as delimiter
        // A simple conservative approach: if any matched '!' is preceded by 's' at a
        // substitution position, accept.  Reject otherwise.
        bool all_safe = true;
        std::sregex_iterator it(s.begin(), s.end(), neg_re);
        std::sregex_iterator end;
        for (; it != end; ++it) {
            const auto pos = it->position();
            const auto bang_pos = pos + (it->length() - 1);
            if (bang_pos > 0 && s[bang_pos - 1] == 's') {
                // s! — substitution delimiter, safe only if this looks like s<delim>…
                // We treat s! followed by a second ! somewhere later as safe-ish.
                if (s.find('!', bang_pos + 1) != std::string::npos) continue;
            }
            all_safe = false;
            break;
        }
        if (!all_safe) return true;
    }

    // Tilde step addresses: digit~digit  ,~digit  $~digit  (allow whitespace)
    static const std::regex tilde_re(R"(\d\s*~\s*\d|,\s*~\s*\d|\$\s*~\s*\d)");
    if (std::regex_search(s, tilde_re)) return true;

    // Bare comma at start
    if (!s.empty() && s.front() == ',') return true;

    // Comma followed by +/- offsets
    static const std::regex comma_offs_re(R"(,\s*[+-])");
    if (std::regex_search(s, comma_offs_re)) return true;

    // Backslash tricks: s\  or  \|,  \#,  \%,  \@
    static const std::regex bs_tricks_re(R"(s\\|\\[|#%@])");
    if (std::regex_search(s, bs_tricks_re)) return true;

    // Escaped slashes followed by w / W  (e.g. /\/path\/to\/file/w)
    static const std::regex esc_slash_w_re(R"(\\/.*[wW])");
    if (std::regex_search(s, esc_slash_w_re)) return true;

    // Slash → non-slash chars → whitespace → w/W/e/E
    static const std::regex slash_danger_re(R"(/[^/]*\s+[wWeE])");
    if (std::regex_search(s, slash_danger_re)) return true;

    // Malformed substitution starting with s/  that is NOT properly s/p/r/f
    if (s.starts_with("s/")) {
        static const std::regex proper_subst_re(R"(^s/[^/]*/[^/]*/[^/]*$)");
        if (!std::regex_match(s, proper_subst_re)) return true;
    }

    // Paranoid: s<ANY>… ending in w/W/e/E, unless properly delimited substitution
    static const std::regex s_any_re(R"(^s.)");
    static const std::regex danger_end_re(R"([wWeE]$)");
    if (std::regex_search(s, s_any_re) && std::regex_search(s, danger_end_re)) {
        static const std::regex proper_any_delim(R"(^s([^\\\n]).*?\1.*?\1[^wWeE]*$)");
        if (!std::regex_match(s, proper_any_delim)) return true;
    }

    // -------- Write / execute commands (simplified forms to avoid
    //          catastrophic backtracking — see CodeQL note in TS).

    // At the start of the command after optional address
    auto starts_with_cmd = [&](char cmd_char) -> bool {
        // Strip leading whitespace-padded address:
        //   digits, optional ,digits  OR  $  OR  /pattern/[/pattern/]
        std::size_t i = 0;
        // Case 1: pure numeric
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        if (i > 0 && i < s.size() && s[i] == ',') {
            ++i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            if (i < s.size() && s[i] == '$') ++i;
        }
        // Case 2: $
        if (s.starts_with('$')) i = 1;
        // Case 3: /pattern/[/pattern/]  (regex forms)
        if (s.starts_with('/')) {
            i = 1;
            while (i < s.size()) {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    i += 2;
                    continue;
                }
                if (s[i] == '/') { ++i; break; }
                ++i;
            }
            // Optional second /pattern/
            if (i < s.size() && s[i] == 'I') ++i;
            if (i < s.size() && s[i] == 'M') ++i;
            if (i < s.size() && s[i] == 'i') ++i;
            if (i < s.size() && s[i] == 'm') ++i;
            if (i < s.size() && s[i] == ',') {
                ++i;
                if (i < s.size() && s[i] == '/') {
                    ++i;
                    while (i < s.size()) {
                        if (s[i] == '\\' && i + 1 < s.size()) {
                            i += 2;
                            continue;
                        }
                        if (s[i] == '/') { ++i; break; }
                        ++i;
                    }
                }
                if (i < s.size() && s[i] == 'I') ++i;
                if (i < s.size() && s[i] == 'M') ++i;
                if (i < s.size() && s[i] == 'i') ++i;
                if (i < s.size() && s[i] == 'm') ++i;
            }
        }
        // Skip optional whitespace
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i < s.size() && s[i] == cmd_char) {
            // Must be followed by whitespace + non-whitespace, OR end-of-string
            if (i + 1 == s.size()) return true; // bare "p", "d" etc.
            if (std::isspace(static_cast<unsigned char>(s[i + 1]))) {
                // Require at least one filename / arg for w/W/e/E
                if (cmd_char == 'w' || cmd_char == 'W' ||
                    cmd_char == 'e' || cmd_char == 'E') {
                    std::size_t j = i + 1;
                    while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j]))) ++j;
                    return j < s.size();
                }
                return true;
            }
        }
        return false;
    };

    // w / W  followed by a filename
    static const std::vector<std::regex> write_patterns = {
        std::regex(R"(^[wW]\s+\S+)"),
        std::regex(R"(^\d+\s*[wW]\s+\S+)"),
        std::regex(R"(^\$\s*[wW]\s+\S+)"),
        std::regex(R"(^/[^/]*/[IMim]*\s*[wW]\s+\S+)"),
        std::regex(R"(^\d+,\d+\s*[wW]\s+\S+)"),
        std::regex(R"(^\d+,\$\s*[wW]\s+\S+)"),
    };
    for (const auto& re : write_patterns) {
        if (std::regex_search(s, re)) return true;
    }
    if (starts_with_cmd('w') || starts_with_cmd('W')) return true;

    // e / E  execute commands
    static const std::vector<std::regex> exec_patterns = {
        std::regex(R"(^e)"),
        std::regex(R"(^\d+\s*e)"),
        std::regex(R"(^\$\s*e)"),
        std::regex(R"(^/[^/]*/[IMim]*\s*e)"),
        std::regex(R"(^\d+,\d+\s*e)"),
        std::regex(R"(^\d+,\$\s*e)"),
    };
    for (const auto& re : exec_patterns) {
        if (std::regex_search(s, re)) return true;
    }
    if (starts_with_cmd('e') || starts_with_cmd('E')) return true;

    // Substitution command with dangerous w / W / e / E in the flags section
    {
        static const std::regex subst_re(R"(s([^\\\n]).*?\1.*?\1(.*?)$)");
        std::smatch m;
        if (std::regex_match(s, m, subst_re) && m.size() >= 3) {
            const auto flags = m[2].str();
            if (flags.find('w') != std::string::npos ||
                flags.find('W') != std::string::npos ||
                flags.find('e') != std::string::npos ||
                flags.find('E') != std::string::npos) {
                return true;
            }
        }
    }

    // y/ (transliterate) — paranoid: reject any y command containing w/W/e/E
    if (!s.empty() && s[0] == 'y') {
        static const std::regex y_re(R"(y([^\\\n]))");
        if (std::regex_search(s, y_re)) {
            if (s.find('w') != std::string::npos ||
                s.find('W') != std::string::npos ||
                s.find('e') != std::string::npos ||
                s.find('E') != std::string::npos) {
                return true;
            }
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// allowlist entrypoint
// ---------------------------------------------------------------------------

/// Top-level allowlist check — matches TS sedCommandIsAllowedByAllowlist().
[[nodiscard]] inline bool sed_command_is_allowed_by_allowlist(
    std::string_view command,
    bool allow_file_writes = false) {

    auto expressions = extract_sed_expressions(command);
    if (!expressions.has_value()) return false;

    const bool files_present = has_file_args(command);

    bool pattern1 = false;
    bool pattern2 = false;

    if (allow_file_writes) {
        pattern2 = is_substitution_command(command, *expressions, files_present, true);
    } else {
        pattern1 = is_line_printing_command(command, *expressions);
        pattern2 = is_substitution_command(command, *expressions, files_present, false);
    }

    if (!pattern1 && !pattern2) return false;

    // Pattern 2 does not allow semicolons.
    for (const auto& e : *expressions) {
        if (pattern2 && e.find(';') != std::string::npos) return false;
    }

    // Defense-in-depth denylist
    for (const auto& e : *expressions) {
        if (contains_dangerous_operations(e)) return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Top-level safety API (called from tool permission layer)
// ---------------------------------------------------------------------------

/// Result of a cross-cutting sed safety check.
enum class SedSafetyDecision {
    Passthrough, ///< Safe (or not a sed command).
    Ask,         ///< Requires user approval.
};

struct SedSafetyResult {
    SedSafetyDecision decision;
    std::string message;
};

/// Top-level check, mirroring TS checkSedConstraints().
/// Takes the full compound command string and splits it on shell separators
/// (;, &&, ||).  When `allow_file_writes` is true, the `-i` flag and file
/// arguments are permitted for substitution commands.
[[nodiscard]] inline SedSafetyResult
check_sed_constraints(std::string_view command, bool allow_file_writes) {
    // Split on shell-level separators (handled via bash_shell_quoting helper
    // — that helper is quoted-token-aware).
    using cc::utils::bash_shell_quoting::detail::split_compound;
    const auto parts = split_compound(command);

    for (const auto& raw_part : parts) {
        const auto trimmed = detail::trim(raw_part);
        if (trimmed.empty()) continue;

        // Determine base command (first whitespace-separated token)
        const auto space = trimmed.find_first_of(" \t");
        const auto base = space == std::string_view::npos
                             ? trimmed
                             : trimmed.substr(0, space);
        if (base != "sed") continue;

        if (!sed_command_is_allowed_by_allowlist(trimmed, allow_file_writes)) {
            return {
                SedSafetyDecision::Ask,
                "sed command requires approval (contains potentially dangerous operations)",
            };
        }
    }

    return { SedSafetyDecision::Passthrough, "No dangerous sed operations detected" };
}

// ---------------------------------------------------------------------------
// Path-scope validation — delegates to bash_validation
// ---------------------------------------------------------------------------

/// Full sed safety check including path scope.
/// Returns .valid == true when the command is safe to auto-execute.
[[nodiscard]] inline ValidationResult
is_sed_safe(std::string_view sed_cmd, const PathValidationContext& ctx) {
    // 1. Cross-cutting dangerous-operations check.
    //    In "acceptEdits" mode (path restriction implies we're editing project
    //    files) we allow in-place writes; otherwise we require read-only.
    const bool allow_file_writes = !ctx.allowed_paths.empty();
    auto decision = check_sed_constraints(sed_cmd, allow_file_writes);
    if (decision.decision == SedSafetyDecision::Ask) {
        return {false, std::move(decision.message)};
    }

    // 2. Path scope check via bash_validation (existing logic, not duplicated).
    auto pr = cc::tools::bash_validation::validate_paths(sed_cmd, ctx);
    if (!pr.valid) return pr;

    // 3. Destructive operation check via bash_security.
    if (cc::tools::is_destructive_command(sed_cmd)) {
        auto reason = cc::tools::get_destructive_warning(sed_cmd);
        return {false, reason.value_or("sed command triggers destructive-operation warning")};
    }

    return {true, std::nullopt};
}

} // namespace cc::tools::sed_validation
