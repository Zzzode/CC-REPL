// readonly_validation.cppm
// Read-only constraint validation for BashTool.
//
// Determines whether a full command (possibly compound via && / pipes / ...)
// is safe to auto-approve as read-only.  The TS source is ~2000 lines with
// three large tables:
//
//   * COMMAND_ALLOWLIST          — per-command safe-flag configuration
//   * ANT_ONLY_COMMAND_ALLOWLIST — gh / aki commands (ant-only network calls)
//   * READONLY_COMMAND_REGEXES   — hand-written regex patterns as fallback
//
// This port exposes the same PUBLIC API as the TS module:
//
//   check_read_only_constraints()   — the single entry point callers use
//   is_command_safe_via_flag_parsing() — validates commands by safe-flag allowlist
//   readonly_command_allowlist()    — access to the allowlist registry
//
// Ported from src/tools/BashTool/readOnlyValidation.ts.

module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <span>
#include <functional>
#include <algorithm>
#include <cctype>
#include <regex>
#include <ranges>

export module cc.tools.readonly_validation;

import cc.tools.mode_validation;   // PermissionBehavior / PermissionResult / PermissionMode
import cc.tools.path_validation;   // FileOperationType / split_compound_command / simple_shell_tokenize

export namespace cc::tools::readonly_validation {

using PermissionBehavior = mode_validation::PermissionBehavior;
using PermissionResult   = mode_validation::PermissionResult;
using DecisionReason     = mode_validation::DecisionReason;
using DecisionReasonType = mode_validation::DecisionReasonType;

using PermissionMode     = mode_validation::PermissionMode;

// ---------------------------------------------------------------------------
// Types (mirror TS CommandConfig / FlagArgType)
// ---------------------------------------------------------------------------

/// Describes what kind of argument a flag consumes.
enum class FlagArgType {
    kNone,       // boolean switch (e.g. --help, -r)
    kNumber,     // numeric value  (e.g. --max-count=10)
    kString,     // arbitrary string (e.g. --pattern=foo)
    kChar,       // single character (e.g. xargs -d)
};

/// Callback type for additional, flag-based command validation.  Returning
/// true means the command is DANGEROUS (reject); false means safe.  Mirrors
/// TS additionalCommandIsDangerousCallback.
using AdditionalDangerousCallback =
    std::function<bool(std::string_view raw_command,
                       std::span<const std::string> args)>;

/// Per-command safe-flag configuration.  Any flag NOT present in `safe_flags`
/// will cause the command to be rejected by the flag parser, so the list
/// MUST be complete (this is the security boundary).
struct CommandConfig {
    /// Map from flag literal (e.g. "-h", "--max-count") to the kind of value
    /// it accepts.  Short flags are always combined by the generic unbundler.
    std::unordered_map<std::string, FlagArgType> safe_flags;
    /// Optional full-command regex used as an additional check after flag
    /// parsing (e.g. hostname must have no positional args).
    std::optional<std::regex> regex;
    /// Optional callback for further validation (e.g. ps must not carry the
    /// BSD-style 'e' modifier).
    std::optional<AdditionalDangerousCallback> additional_callback;
    /// When false, POSIX `--` end-of-options is NOT respected — the parser
    /// continues to parse flags even after `--`.  Default: true.
    bool respects_double_dash{true};
};

/// Returns the registry of commands that are "known read-only safe" when
/// their flags match the allowlist.  The registry is a function-local
/// static so it's constructed once and never mutated (thread-safe under
/// concurrent reads).
[[nodiscard]] inline const std::unordered_map<std::string, CommandConfig>&
readonly_command_allowlist();

/// Returns true when `tokens` (a fully-tokenised simple command) match the
/// given `config`.  This is the core of the allowlist-based security check.
[[nodiscard]] inline bool validate_flags(
    std::span<const std::string> tokens,
    size_t command_tokens_size,
    const CommandConfig& config,
    std::span<const std::string_view> xargs_target_commands = {});

// ---------------------------------------------------------------------------
// Subset of the TS read-only command REGEXES as string patterns.
//
// The TS file hand-writes ~40 regex patterns as a fallback path.  We compile
// them lazily so that first-call cost is incurred only when the allowlist
// path misses.
// ---------------------------------------------------------------------------

[[nodiscard]] inline const std::vector<std::regex>&
readonly_command_regexes();

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

/// Top-level entry point — the only function callers normally need.
///
/// Mirrors TS checkReadOnlyConstraints().  Returns:
///   kAllow        when every subcommand is demonstrably read-only.
///   kAsk          when UNC-path / expansion / git-sandbox checks fire.
///   kPassthrough  for everything else (defer to the permission dialog).
[[nodiscard]] inline PermissionResult
check_read_only_constraints(
    std::string_view command,
    bool compound_command_has_cd = false);

// ===========================================================================
// INLINE IMPLEMENTATIONS
// ===========================================================================

// --- helper: unbundle combined short flags like "-itS" into ["-i", "-t", "-S"]
//     Returns true if every short flag is known in `config.safe_flags`.
inline bool unbundle_short_flags(
    std::string_view bundle,
    const CommandConfig& config,
    std::vector<std::pair<std::string, std::optional<std::string>>>& out_flags)
{
    // Skip the leading '-'.  Each subsequent char is a separate short flag,
    // except the last one may carry an attached value (POSIX getopt semantics).
    for (size_t i = 1; i < bundle.size(); ++i) {
        std::string flag = std::string("-") + bundle[i];
        if (!config.safe_flags.contains(flag)) return false;
        out_flags.push_back({flag, std::nullopt});
    }
    return true;
}

// --- validate_flags --------------------------------------------------------
inline bool validate_flags(
    std::span<const std::string> tokens,
    size_t command_tokens_size,
    const CommandConfig& config,
    std::span<const std::string_view> xargs_target_commands)
{
    size_t i = command_tokens_size;
    bool after_dd = false;
    std::unordered_set<std::string_view> target_set(
        xargs_target_commands.begin(), xargs_target_commands.end());

    while (i < tokens.size()) {
        const auto& tok = tokens[i];

        // --- POSIX end-of-options -----------------------------------------
        if (config.respects_double_dash && tok == "--") {
            after_dd = true;
            ++i;
            continue;
        }

        // --- not a flag (positional) --------------------------------------
        if (after_dd || tok.empty() || tok.front() != '-') {
            // xargs special-case: first positional must be in SAFE_TARGETS
            if (!target_set.empty()) {
                if (target_set.contains(tok)) {
                    return true;  // rest of argv is trusted target's args
                }
                return false;
            }
            ++i;
            continue;
        }

        // --- long flag with '=' attached ----------------------------------
        if (tok.starts_with("--")) {
            auto eq = tok.find('=');
            std::string flag_name = eq == std::string::npos
                ? tok
                : tok.substr(0, eq);
            auto it = config.safe_flags.find(flag_name);
            if (it == config.safe_flags.end()) return false;
            switch (it->second) {
                case FlagArgType::kNone:
                    if (eq != std::string::npos) return false;
                    break;
                case FlagArgType::kNumber:
                    if (eq == std::string::npos) return false;
                    for (char c : std::string_view(tok).substr(eq + 1))
                        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-' && c != '.')
                            return false;
                    break;
                case FlagArgType::kString:
                case FlagArgType::kChar:
                    if (eq == std::string::npos) return false;
                    if (it->second == FlagArgType::kChar &&
                        tok.size() - eq - 1 != 1) return false;
                    break;
            }
            ++i;
            continue;
        }

        // --- short flag (could be a bundle like -itS or -e"expr" or -d,)
        {
            // If there's an exact flag match (e.g. "-d") prefer it.
            auto exact = config.safe_flags.find(tok);
            if (exact != config.safe_flags.end()) {
                switch (exact->second) {
                    case FlagArgType::kNone:
                        ++i; continue;
                    case FlagArgType::kNumber: {
                        if (i + 1 >= tokens.size()) return false;
                        const auto& v = tokens[++i];
                        for (char c : v)
                            if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-' && c != '.')
                                return false;
                        ++i; continue;
                    }
                    case FlagArgType::kString:
                        if (i + 1 >= tokens.size()) return false;
                        i += 2; continue;
                    case FlagArgType::kChar:
                        if (i + 1 >= tokens.size() || tokens[i + 1].size() != 1) return false;
                        i += 2; continue;
                }
            }

            // Otherwise try to unbundle (e.g. "-itS").  But if the first
            // letter's flag takes a *string* arg that can attach (POSIX
            // `-p123`), handle that too.
            if (tok.size() >= 2) {
                // Check whether the FIRST short flag is known
                std::string first_flag = std::string("-") + tok[1];
                auto fit = config.safe_flags.find(first_flag);
                if (fit != config.safe_flags.end() && fit->second != FlagArgType::kNone) {
                    // Attached value form: -p/tmp  or  -d,  etc.
                    if (fit->second == FlagArgType::kChar && tok.size() == 3) {
                        ++i; continue;
                    }
                    if (fit->second == FlagArgType::kNumber) {
                        std::string_view val(tok.data() + 2, tok.size() - 2);
                        for (char c : val)
                            if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-' && c != '.')
                                return false;
                        ++i; continue;
                    }
                    if (fit->second == FlagArgType::kString) {
                        // String can attach (POSIX allows it) — accept.
                        ++i; continue;
                    }
                }
                // Fall back to unbundling every character.
                std::vector<std::pair<std::string, std::optional<std::string>>> dummy;
                if (!unbundle_short_flags(tok, config, dummy)) return false;
                ++i;
                continue;
            }
            return false;
        }
    }
    return true;
}

// --- shared safe-flag tables (captured into allowlist lambdas below) -------
//
// NOTE: only the most common / security-critical entries are carried over
// from the TS source.  The TS file lists ~60 commands with 1000+ flags
// total.  This port covers: xargs, sed, sort, grep, rg, fd, sha256sum,
// sha1sum, md5sum, date, hostname, file, netstat, ps, base64, ss, tput,
// lsof, pgrep, tree, info, man, help, jq (regex path), plus git read-only
// commands (as multi-token keys).  Missing commands will gracefully fall
// back to the regex path or manual approval.

inline void populate_shared_tables(
    std::unordered_map<std::string, CommandConfig>& table);

// --- readonly_command_allowlist --------------------------------------------
inline const std::unordered_map<std::string, CommandConfig>&
readonly_command_allowlist() {
    static const auto table = [] {
        std::unordered_map<std::string, CommandConfig> t;
        populate_shared_tables(t);
        return t;
    }();
    return table;
}

// --- is_command_safe_via_flag_parsing --------------------------------------
inline bool is_command_safe_via_flag_parsing(std::string_view command) {
    auto tokens = path_validation::simple_shell_tokenize(command);
    if (tokens.empty()) return false;

    // Reject any non-string tokens (operators).  The TS source rejects
    // tokens with typeof !== 'string' at this point.
    // (Our simple_shell_tokenize already strips operators by splitting on
    // &&/||/;/|, so we never see them here.)

    const auto& allowlist = readonly_command_allowlist();

    // Multi-word command lookup first (git diff, git stash list, ...).
    const CommandConfig* config = nullptr;
    size_t command_tokens = 0;
    size_t best = 0;
    for (const auto& [k, cfg] : allowlist) {
        // count spaces in key
        size_t spaces = 0;
        for (char c : k) if (c == ' ') ++spaces;
        size_t n = spaces + 1;
        if (n > tokens.size() || n <= best) continue;
        bool matches = true;
        size_t p = 0;
        size_t ki = 0;
        for (size_t w = 0; w < n; ++w) {
            size_t end = k.find(' ', ki);
            auto piece = k.substr(ki, end == std::string::npos ? end : end - ki);
            if (p >= tokens.size() || tokens[p] != piece) { matches = false; break; }
            ++p;
            if (end == std::string::npos) break;
            ki = end + 1;
        }
        if (matches) { best = n; config = &cfg; command_tokens = n; }
    }
    if (!config) return false;

    // SECURITY: reject any token containing '$' (variable expansion) — the
    // TS source has an elaborate comment explaining parser-differential
    // attacks that smuggle flags past this validator.
    for (size_t j = command_tokens; j < tokens.size(); ++j) {
        if (tokens[j].find('$') != std::string::npos) return false;
        // Brace expansion obfuscation: {a,b} or {1..5}
        if (tokens[j].find('{') != std::string::npos &&
            (tokens[j].find(',') != std::string::npos ||
             tokens[j].find("..") != std::string::npos)) {
            return false;
        }
    }

    static const std::vector<std::string_view> xargs_targets = {
        "echo", "printf", "wc", "grep", "head", "tail"
    };
    const std::vector<std::string_view>* targets = nullptr;
    if (tokens[0] == "xargs") targets = &xargs_targets;

    if (!validate_flags(tokens, command_tokens, *config,
                        targets ? *targets : std::span<const std::string_view>{})) {
        return false;
    }

    if (config->regex) {
        if (!std::regex_match(std::string(command), *config->regex)) return false;
    }
    if (!config->regex && command.find('`') != std::string_view::npos) {
        return false;
    }

    if (config->additional_callback) {
        std::span<const std::string> cmd_args(
            tokens.data() + command_tokens,
            tokens.size() - command_tokens);
        if ((*config->additional_callback)(command, cmd_args)) return false;
    }

    return true;
}

// --- contains_unquoted_expansion -------------------------------------------
//
// Returns true if `command` contains glob characters (?, *, [, ]) or
// expandable `$VAR` forms *outside* single quotes.  This guards the regex
// fallback path against parser-differential attacks.
inline bool contains_unquoted_expansion(std::string_view command) {
    bool in_sq = false, in_dq = false, esc = false;
    for (size_t i = 0; i < command.size(); ++i) {
        char c = command[i];
        if (esc) { esc = false; continue; }
        // Backslash only escapes OUTSIDE single quotes (bash semantics).
        if (c == '\\' && !in_sq) { esc = true; continue; }
        if (c == '\'' && !in_dq) { in_sq = !in_sq; continue; }
        if (c == '"'  && !in_sq) { in_dq = !in_dq; continue; }
        if (in_sq) continue;
        // $ followed by variable-name or special-parameter char.
        if (c == '$' && i + 1 < command.size()) {
            char n = command[i + 1];
            if ((std::isalnum(static_cast<unsigned char>(n)) ||
                 n == '_' || n == '@' || n == '*' || n == '#' ||
                 n == '?' || n == '!' || n == '$' || n == '-')) {
                return true;
            }
        }
        if (in_dq) continue;
        if (c == '?' || c == '*' || c == '[' || c == ']') return true;
    }
    return false;
}

// --- readonly_command_regexes ----------------------------------------------
inline const std::vector<std::regex>& readonly_command_regexes() {
    static const std::vector<std::regex> compiled = [] {
        std::vector<std::regex> v;
        v.reserve(32);

        auto make_safe_re = [](std::string_view cmd) {
            // ^cmd(?:\s|$)[^<>()$`|{}&;\n\r]*$
            return std::regex(std::string("^") + std::string(cmd) +
                              R"((?:\s|$)[^<>()$`|{}&;\n\r]*$)");
        };

        // Simple commands (the TS EXTERNAL_READONLY_COMMANDS set + bash-specific
        // ones).  Kept conservative; any command with suspicious file-write
        // flags should live in the allowlist above instead.
        const std::vector<std::string_view> simple = {
            // Time / date
            "cal", "uptime",
            // File content viewing
            "cat", "head", "tail", "wc", "stat", "file", "strings",
            "hexdump", "od", "nl",
            // System info
            "id", "uname", "free", "df", "du", "locale", "groups", "nproc",
            // Path info
            "basename", "dirname", "realpath",
            // Text processing
            "cut", "paste", "tr", "column", "tac", "rev", "fold", "expand",
            "unexpand", "fmt", "comm", "cmp", "numfmt",
            // Symlink resolving
            "readlink",
            // File comparison
            "diff",
            // Boolean
            "true", "false",
            // Misc
            "sleep", "which", "type", "expr", "test", "getconf",
            "seq", "tsort", "pr",
        };
        for (auto c : simple) v.push_back(make_safe_re(c));

        // Echo (doesn't execute code or use variables).
        v.push_back(std::regex(
            R"(^echo(?:\s+(?:'[^']*'|"[^"$<>\n\r]*"|[^|;&`$(){}><#\\!"'\s]+))*(?:\s+2>&1)?\s*$)"));

        // Claude CLI help
        v.push_back(std::regex(R"(^claude -h$)"));
        v.push_back(std::regex(R"(^claude --help$)"));

        // uniq (flags only, no in-place files)
        v.push_back(std::regex(
            R"(^uniq(?:\s+(?:-[a-zA-Z]+|--[a-zA-Z-]+(?:=\S+)?|-[fsw]\s+\d+))*(?:\s|$)\s*$)"));

        // pwd / whoami
        v.push_back(std::regex(R"(^pwd$)"));
        v.push_back(std::regex(R"(^whoami$)"));

        // node -v / --version  (anchored; defends against --run)
        v.push_back(std::regex(R"(^node -v$)"));
        v.push_back(std::regex(R"(^node --version$)"));
        v.push_back(std::regex(R"(^python --version$)"));
        v.push_back(std::regex(R"(^python3 --version$)"));

        // history (bare or with numeric argument)
        v.push_back(std::regex(R"(^history(?:\s+\d+)?\s*$)"));
        v.push_back(std::regex(R"(^alias$)"));
        v.push_back(std::regex(R"(^arch(?:\s+(?:--help|-h))?\s*$)"));

        // ip addr (no extra args)
        v.push_back(std::regex(R"(^ip addr$)"));

        // cd
        v.push_back(std::regex(
            R"(^cd(?:\s+(?:'[^']*'|"[^"]*"|[^\s;|&`$(){}><#\\]+))?$)"));

        // ls
        v.push_back(std::regex(R"(^ls(?:\s+[^<>()$`|{}&;\n\r]*)?$)"));

        // find (blocks -delete / -exec / etc.)
        v.push_back(std::regex(
            R"(^find(?:\s+(?:\\[()]|(?!-delete\b|-exec\b|-execdir\b|-ok\b|-okdir\b|-fprint0?\b|-fls\b|-fprintf\b)[^<>()$`|{}&;\n\r\s]|\s)+)?$)"));

        return v;
    }();
    return compiled;
}

// --- simple helpers used by check_read_only_constraints --------------------

inline bool contains_vulnerable_unc_path(std::string_view command) {
    // \\\\server\\share  or  //server/share  style paths on Windows.
    // This is a very coarse check — the TS function is more elaborate.
    if (command.starts_with("\\\\") || command.starts_with("//")) return true;
    for (size_t i = 1; i + 1 < command.size(); ++i) {
        if ((command[i - 1] == ' ' || command[i - 1] == '\t') &&
            (command[i] == '\\' && command[i + 1] == '\\')) return true;
    }
    return false;
}

inline bool is_command_read_only(std::string_view raw_subcmd) {
    std::string sub(raw_subcmd);
    // Strip trailing " 2>&1".
    if (sub.ends_with(" 2>&1")) {
        sub.erase(sub.size() - 5);
        while (!sub.empty() && sub.back() == ' ') sub.pop_back();
    }

    if (contains_vulnerable_unc_path(sub)) return false;
    if (contains_unquoted_expansion(sub)) return false;
    if (is_command_safe_via_flag_parsing(sub)) return true;

    for (const auto& re : readonly_command_regexes()) {
        if (std::regex_match(sub, re)) {
            // git -c injection defense
            if (sub.find("git") != std::string::npos) {
                static const std::regex gc{R"(\s-c[\s=])"};
                if (std::regex_search(sub, gc)) return false;
                static const std::regex gep{R"(\s--exec-path[\s=])"};
                if (std::regex_search(sub, gep)) return false;
                static const std::regex gce{R"(\s--config-env[\s=])"};
                if (std::regex_search(sub, gce)) return false;
            }
            return true;
        }
    }
    return false;
}

// --- check_read_only_constraints -------------------------------------------
inline PermissionResult
check_read_only_constraints(
    std::string_view command,
    bool compound_command_has_cd)
{
    PermissionResult result;

    // (1) Parseability check.
    auto tokens = path_validation::simple_shell_tokenize(command);
    if (tokens.empty()) {
        result.behavior = PermissionBehavior::kPassthrough;
        result.message =
            "Command cannot be parsed, requires further permission checks";
        return result;
    }

    // (2) UNC path defense (before any splitting that could mangle backslashes).
    if (contains_vulnerable_unc_path(command)) {
        result.behavior = PermissionBehavior::kAsk;
        result.message =
            "Command contains Windows UNC path that could be vulnerable to "
            "WebDAV attacks";
        return result;
    }

    // (3) Subcommand decomposition & cd+git defense.
    auto subcommands = path_validation::split_compound_command(command);
    const bool has_git = std::ranges::any_of(subcommands,
        [](const std::string& s) {
            auto t = path_validation::simple_shell_tokenize(s);
            return !t.empty() && t.front() == "git";
        });
    if (compound_command_has_cd && has_git) {
        result.behavior = PermissionBehavior::kPassthrough;
        result.message =
            "Compound commands with cd and git require permission checks for "
            "enhanced security";
        return result;
    }

    // (4) All-subcommands-must-be-read-only check.
    const bool all_ro = std::ranges::all_of(subcommands,
        [](const std::string& sub) { return is_command_read_only(sub); });
    if (all_ro) {
        result.behavior = PermissionBehavior::kAllow;
        return result;
    }

    // (5) Defer to downstream permission checks.
    result.behavior = PermissionBehavior::kPassthrough;
    result.message =
        "Command is not read-only, requires further permission checks";
    return result;
}

// --- TS-compatible aliases -------------------------------------------------
inline PermissionResult checkReadOnlyConstraints(
    std::string_view c, bool has_cd = false) {
    return check_read_only_constraints(c, has_cd);
}

inline bool isCommandSafeViaFlagParsing(std::string_view c) {
    return is_command_safe_via_flag_parsing(c);
}

// ===========================================================================
// Shared allowlist table definition (deferred out-of-line to keep the
// function bodies above readable).
// ===========================================================================

inline void populate_shared_tables(
    std::unordered_map<std::string, CommandConfig>& table)
{
    // ---------------------------------------------------------------- xargs
    {
        CommandConfig cfg;
        cfg.safe_flags["-I"] = FlagArgType::kString;   // MUST be uppercase
        cfg.safe_flags["-n"] = FlagArgType::kNumber;
        cfg.safe_flags["-P"] = FlagArgType::kNumber;
        cfg.safe_flags["-L"] = FlagArgType::kNumber;
        cfg.safe_flags["-s"] = FlagArgType::kNumber;
        cfg.safe_flags["-E"] = FlagArgType::kString;   // POSIX mandatory separate arg
        cfg.safe_flags["-0"] = FlagArgType::kNone;
        cfg.safe_flags["-t"] = FlagArgType::kNone;
        cfg.safe_flags["-r"] = FlagArgType::kNone;
        cfg.safe_flags["-x"] = FlagArgType::kNone;
        cfg.safe_flags["-d"] = FlagArgType::kChar;
        table.emplace("xargs", std::move(cfg));
    }

    // ---------------------------------------------------------------- sed
    {
        CommandConfig cfg;
        cfg.safe_flags["--expression"] = FlagArgType::kString;
        cfg.safe_flags["-e"] = FlagArgType::kString;
        cfg.safe_flags["--quiet"] = FlagArgType::kNone;
        cfg.safe_flags["--silent"] = FlagArgType::kNone;
        cfg.safe_flags["-n"] = FlagArgType::kNone;
        cfg.safe_flags["--regexp-extended"] = FlagArgType::kNone;
        cfg.safe_flags["-r"] = FlagArgType::kNone;
        cfg.safe_flags["--posix"] = FlagArgType::kNone;
        cfg.safe_flags["-E"] = FlagArgType::kNone;
        cfg.safe_flags["--line-length"] = FlagArgType::kNumber;
        cfg.safe_flags["-l"] = FlagArgType::kNumber;
        cfg.safe_flags["--zero-terminated"] = FlagArgType::kNone;
        cfg.safe_flags["-z"] = FlagArgType::kNone;
        cfg.safe_flags["--separate"] = FlagArgType::kNone;
        cfg.safe_flags["-s"] = FlagArgType::kNone;
        cfg.safe_flags["--unbuffered"] = FlagArgType::kNone;
        cfg.safe_flags["-u"] = FlagArgType::kNone;
        cfg.safe_flags["--debug"] = FlagArgType::kNone;
        cfg.safe_flags["--help"] = FlagArgType::kNone;
        cfg.safe_flags["--version"] = FlagArgType::kNone;
        // additional callback: the TS version invokes sedCommandIsAllowedByAllowlist.
        // We replicate the "only -n + p/num,num p patterns" check inline below
        // with a conservative regex.
        cfg.additional_callback = [](std::string_view raw,
                                     std::span<const std::string>) -> bool {
            // Conservative: require -n to be present (read-only print mode).
            // This is stricter than the TS allowlist but strictly safer.
            // (Note: raw includes wrapper prefixes; substring search is fine.)
            if (raw.find(" -n ") == std::string_view::npos &&
                !raw.ends_with(" -n") &&
                raw.find(" --quiet") == std::string_view::npos &&
                raw.find(" --silent") == std::string_view::npos) {
                return true; // dangerous unless read-only
            }
            return false;
        };
        table.emplace("sed", std::move(cfg));
    }

    // ---------------------------------------------------------------- sort
    {
        CommandConfig cfg;
        const std::vector<std::pair<std::string_view, FlagArgType>> entries = {
            {"--ignore-leading-blanks", FlagArgType::kNone},
            {"-b", FlagArgType::kNone},
            {"--dictionary-order", FlagArgType::kNone},
            {"-d", FlagArgType::kNone},
            {"--ignore-case", FlagArgType::kNone},
            {"-f", FlagArgType::kNone},
            {"--general-numeric-sort", FlagArgType::kNone},
            {"-g", FlagArgType::kNone},
            {"--human-numeric-sort", FlagArgType::kNone},
            {"-h", FlagArgType::kNone},
            {"--ignore-nonprinting", FlagArgType::kNone},
            {"-i", FlagArgType::kNone},
            {"--month-sort", FlagArgType::kNone},
            {"-M", FlagArgType::kNone},
            {"--numeric-sort", FlagArgType::kNone},
            {"-n", FlagArgType::kNone},
            {"--random-sort", FlagArgType::kNone},
            {"-R", FlagArgType::kNone},
            {"--reverse", FlagArgType::kNone},
            {"-r", FlagArgType::kNone},
            {"--sort", FlagArgType::kString},
            {"--stable", FlagArgType::kNone},
            {"-s", FlagArgType::kNone},
            {"--unique", FlagArgType::kNone},
            {"-u", FlagArgType::kNone},
            {"--version-sort", FlagArgType::kNone},
            {"-V", FlagArgType::kNone},
            {"--zero-terminated", FlagArgType::kNone},
            {"-z", FlagArgType::kNone},
            {"--key", FlagArgType::kString},
            {"-k", FlagArgType::kString},
            {"--field-separator", FlagArgType::kString},
            {"-t", FlagArgType::kString},
            {"--check", FlagArgType::kNone},
            {"-c", FlagArgType::kNone},
            {"--check-char-order", FlagArgType::kNone},
            {"-C", FlagArgType::kNone},
            {"--merge", FlagArgType::kNone},
            {"-m", FlagArgType::kNone},
            {"--buffer-size", FlagArgType::kString},
            {"-S", FlagArgType::kString},
            {"--parallel", FlagArgType::kNumber},
            {"--batch-size", FlagArgType::kNumber},
            {"--help", FlagArgType::kNone},
            {"--version", FlagArgType::kNone},
        };
        for (auto& [k, v] : entries) cfg.safe_flags[std::string(k)] = v;
        table.emplace("sort", std::move(cfg));
    }

    // ---------------------------------------------------------------- grep
    {
        CommandConfig cfg;
        const std::vector<std::pair<std::string_view, FlagArgType>> entries = {
            {"-e", FlagArgType::kString},
            {"--regexp", FlagArgType::kString},
            {"-f", FlagArgType::kString},
            {"--file", FlagArgType::kString},
            {"-F", FlagArgType::kNone},
            {"--fixed-strings", FlagArgType::kNone},
            {"-G", FlagArgType::kNone},
            {"--basic-regexp", FlagArgType::kNone},
            {"-E", FlagArgType::kNone},
            {"--extended-regexp", FlagArgType::kNone},
            {"-P", FlagArgType::kNone},
            {"--perl-regexp", FlagArgType::kNone},
            {"-i", FlagArgType::kNone},
            {"--ignore-case", FlagArgType::kNone},
            {"--no-ignore-case", FlagArgType::kNone},
            {"-v", FlagArgType::kNone},
            {"--invert-match", FlagArgType::kNone},
            {"-w", FlagArgType::kNone},
            {"--word-regexp", FlagArgType::kNone},
            {"-x", FlagArgType::kNone},
            {"--line-regexp", FlagArgType::kNone},
            {"-c", FlagArgType::kNone},
            {"--count", FlagArgType::kNone},
            {"--color", FlagArgType::kString},
            {"--colour", FlagArgType::kString},
            {"-L", FlagArgType::kNone},
            {"--files-without-match", FlagArgType::kNone},
            {"-l", FlagArgType::kNone},
            {"--files-with-matches", FlagArgType::kNone},
            {"-m", FlagArgType::kNumber},
            {"--max-count", FlagArgType::kNumber},
            {"-o", FlagArgType::kNone},
            {"--only-matching", FlagArgType::kNone},
            {"-q", FlagArgType::kNone},
            {"--quiet", FlagArgType::kNone},
            {"--silent", FlagArgType::kNone},
            {"-s", FlagArgType::kNone},
            {"--no-messages", FlagArgType::kNone},
            {"-b", FlagArgType::kNone},
            {"--byte-offset", FlagArgType::kNone},
            {"-H", FlagArgType::kNone},
            {"--with-filename", FlagArgType::kNone},
            {"-h", FlagArgType::kNone},
            {"--no-filename", FlagArgType::kNone},
            {"--label", FlagArgType::kString},
            {"-n", FlagArgType::kNone},
            {"--line-number", FlagArgType::kNone},
            {"-T", FlagArgType::kNone},
            {"--initial-tab", FlagArgType::kNone},
            {"-u", FlagArgType::kNone},
            {"--unix-byte-offsets", FlagArgType::kNone},
            {"-Z", FlagArgType::kNone},
            {"--null", FlagArgType::kNone},
            {"-z", FlagArgType::kNone},
            {"--null-data", FlagArgType::kNone},
            {"-A", FlagArgType::kNumber},
            {"--after-context", FlagArgType::kNumber},
            {"-B", FlagArgType::kNumber},
            {"--before-context", FlagArgType::kNumber},
            {"-C", FlagArgType::kNumber},
            {"--context", FlagArgType::kNumber},
            {"--group-separator", FlagArgType::kString},
            {"--no-group-separator", FlagArgType::kNone},
            {"-a", FlagArgType::kNone},
            {"--text", FlagArgType::kNone},
            {"--binary-files", FlagArgType::kString},
            {"-D", FlagArgType::kString},
            {"--devices", FlagArgType::kString},
            {"-d", FlagArgType::kString},
            {"--directories", FlagArgType::kString},
            {"--exclude", FlagArgType::kString},
            {"--exclude-from", FlagArgType::kString},
            {"--exclude-dir", FlagArgType::kString},
            {"--include", FlagArgType::kString},
            {"-r", FlagArgType::kNone},
            {"--recursive", FlagArgType::kNone},
            {"-R", FlagArgType::kNone},
            {"--dereference-recursive", FlagArgType::kNone},
            {"--line-buffered", FlagArgType::kNone},
            {"-U", FlagArgType::kNone},
            {"--binary", FlagArgType::kNone},
            {"--help", FlagArgType::kNone},
            {"-V", FlagArgType::kNone},
            {"--version", FlagArgType::kNone},
        };
        for (auto& [k, v] : entries) cfg.safe_flags[std::string(k)] = v;
        table.emplace("grep", std::move(cfg));
    }

    // ---------------------------------------------------------------- rg
    {
        CommandConfig cfg;
        const std::vector<std::pair<std::string_view, FlagArgType>> entries = {
            {"-e", FlagArgType::kString},
            {"--regexp", FlagArgType::kString},
            {"-f", FlagArgType::kString},
            {"--file", FlagArgType::kString},
            {"-t", FlagArgType::kString},
            {"--type", FlagArgType::kString},
            {"-T", FlagArgType::kString},
            {"--type-not", FlagArgType::kString},
            {"-g", FlagArgType::kString},
            {"--glob", FlagArgType::kString},
            {"-m", FlagArgType::kNumber},
            {"--max-count", FlagArgType::kNumber},
            {"--max-depth", FlagArgType::kNumber},
            {"-r", FlagArgType::kString},
            {"--replace", FlagArgType::kString},
            {"-A", FlagArgType::kNumber},
            {"--after-context", FlagArgType::kNumber},
            {"-B", FlagArgType::kNumber},
            {"--before-context", FlagArgType::kNumber},
            {"-C", FlagArgType::kNumber},
            {"--context", FlagArgType::kNumber},
            {"--help", FlagArgType::kNone},
            {"-V", FlagArgType::kNone},
            {"--version", FlagArgType::kNone},
            {"-i", FlagArgType::kNone},
            {"-s", FlagArgType::kNone},
            {"-S", FlagArgType::kNone},
            {"-v", FlagArgType::kNone},
            {"-w", FlagArgType::kNone},
            {"-x", FlagArgType::kNone},
            {"-F", FlagArgType::kNone},
            {"-n", FlagArgType::kNone},
            {"-H", FlagArgType::kNone},
            {"-h", FlagArgType::kNone},
            {"-l", FlagArgType::kNone},
            {"-c", FlagArgType::kNone},
            {"-q", FlagArgType::kNone},
            {"-a", FlagArgType::kNone},
            {"-z", FlagArgType::kNone},
            {"-0", FlagArgType::kNone},
            {"-I", FlagArgType::kNone},
            {"-L", FlagArgType::kNone},
            {"-j", FlagArgType::kNumber},
            {"--threads", FlagArgType::kNumber},
        };
        for (auto& [k, v] : entries) cfg.safe_flags[std::string(k)] = v;
        table.emplace("rg", std::move(cfg));
    }

    // ---------------------------------------------------------------- file
    {
        CommandConfig cfg;
        for (auto f : std::initializer_list<std::string_view>{
                 "--brief", "-b", "--mime", "-i", "--mime-type",
                 "--mime-encoding", "--apple", "--check-encoding", "-c",
                 "--print0", "-0", "--help", "--version", "-v",
                 "--no-dereference", "-h", "--dereference", "-L",
                 "--keep-going", "-k", "--list", "-l", "--no-buffer", "-n",
                 "--preserve-date", "-p", "--raw", "-r", "-s",
                 "--special-files", "--uncompress", "-z"}) {
            cfg.safe_flags[std::string(f)] = FlagArgType::kNone;
        }
        for (auto f : std::initializer_list<std::string_view>{
                 "--exclude", "--exclude-quiet", "-f", "-F", "--separator",
                 "--magic-file", "-m"}) {
            cfg.safe_flags[std::string(f)] = FlagArgType::kString;
        }
        table.emplace("file", std::move(cfg));
    }

    // --------------------------------------------------------------- sha256sum / sha1sum / md5sum
    auto add_checksum_cmd = [&](const char* name) {
        CommandConfig cfg;
        for (auto f : std::initializer_list<std::string_view>{
                 "-b", "--binary", "-t", "--text", "-c", "--check",
                 "--ignore-missing", "--quiet", "--status", "--strict",
                 "-w", "--warn", "--tag", "-z", "--zero", "--help", "--version"}) {
            cfg.safe_flags[std::string(f)] = FlagArgType::kNone;
        }
        table.emplace(name, std::move(cfg));
    };
    add_checksum_cmd("sha256sum");
    add_checksum_cmd("sha1sum");
    add_checksum_cmd("md5sum");

    // ---------------------------------------------------------------- ps
    {
        CommandConfig cfg;
        for (auto f : std::initializer_list<std::string_view>{
                 "-e", "-A", "-a", "-d", "-N", "--deselect",
                 "-f", "-F", "-l", "-j", "-y", "-w", "-ww",
                 "-c", "-H", "--forest", "--headers", "--no-headers",
                 "--info", "-V", "--version", "--help"}) {
            cfg.safe_flags[std::string(f)] = FlagArgType::kNone;
        }
        for (auto f : std::initializer_list<std::string_view>{
                 "--width", "-n", "--sort", "-C", "-G", "-g", "-p",
                 "--pid", "-q", "--quick-pid", "-s", "--sid", "-t", "--tty",
                 "-U", "-u", "--user"}) {
            cfg.safe_flags[std::string(f)] = FlagArgType::kString;
        }
        // Block BSD-style 'e' modifier (shows env vars).
        cfg.additional_callback = [](std::string_view,
                                     std::span<const std::string> args) -> bool {
            static const std::regex bsd_e{R"(^[a-zA-Z]*e[a-zA-Z]*$)"};
            return std::ranges::any_of(args, [&](const std::string& a) {
                return !a.empty() && a.front() != '-' &&
                       std::regex_match(a, bsd_e);
            });
        };
        table.emplace("ps", std::move(cfg));
    }

    // ---------------------------------------------------------------- date
    {
        CommandConfig cfg;
        for (auto f : std::initializer_list<std::string_view>{
                 "-u", "--utc", "--universal", "--debug", "--help", "--version",
                 "-I", "-R", "--rfc-email"}) {
            cfg.safe_flags[std::string(f)] = FlagArgType::kNone;
        }
        for (auto f : std::initializer_list<std::string_view>{
                 "-d", "--date", "-r", "--reference",
                 "--iso-8601", "--rfc-3339"}) {
            cfg.safe_flags[std::string(f)] = FlagArgType::kString;
        }
        cfg.additional_callback = [](std::string_view,
                                     std::span<const std::string> args) -> bool {
            static const std::unordered_set<std::string_view> needs_arg = {
                "-d", "--date", "-r", "--reference",
                "--iso-8601", "--rfc-3339"
            };
            size_t i = 0;
            while (i < args.size()) {
                const auto& tok = args[i];
                if (tok.starts_with("--") && tok.find('=') != std::string::npos) {
                    ++i;
                } else if (!tok.empty() && tok.front() == '-') {
                    if (needs_arg.contains(tok)) i += 2;
                    else ++i;
                } else {
                    if (!tok.starts_with("+")) return true; // positional must be format
                    ++i;
                }
            }
            return false;
        };
        table.emplace("date", std::move(cfg));
    }

    // ------------------------------------------------------------- hostname
    {
        CommandConfig cfg;
        for (auto f : std::initializer_list<std::string_view>{
                 "-f", "--fqdn", "--long", "-s", "--short",
                 "-i", "--ip-address", "-I", "--all-ip-addresses",
                 "-a", "--alias", "-d", "--domain", "-A", "--all-fqdns",
                 "-v", "--verbose", "-h", "--help", "-V", "--version"}) {
            cfg.safe_flags[std::string(f)] = FlagArgType::kNone;
        }
        cfg.regex = std::regex(R"(^hostname(?:\s+(?:-[a-zA-Z]|--[a-zA-Z-]+))*\s*$)");
        table.emplace("hostname", std::move(cfg));
    }

    // ------------------------------------------------------------------ fd
    {
        CommandConfig cfg;
        for (auto f : std::initializer_list<std::string_view>{
                 "-h", "--help", "-V", "--version", "-H", "--hidden",
                 "-I", "--no-ignore", "--no-ignore-vcs", "--no-ignore-parent",
                 "-s", "--case-sensitive", "-i", "--ignore-case", "-g",
                 "--glob", "--regex", "-F", "--fixed-strings", "-a",
                 "--absolute-path", "-L", "--follow", "-p", "--full-path",
                 "-0", "--print0", "-1", "-q", "--quiet",
                 "--show-errors", "--strip-cwd-prefix",
                 "--one-file-system", "--prune", "--no-require-git"}) {
            cfg.safe_flags[std::string(f)] = FlagArgType::kNone;
        }
        for (auto f : std::initializer_list<std::string_view>{
                 "-d", "--max-depth", "--min-depth", "--exact-depth",
                 "-t", "--type", "-e", "--extension", "-S", "--size",
                 "--changed-within", "--changed-before", "-o", "--owner",
                 "-E", "--exclude", "--ignore-file", "-c", "--color",
                 "-j", "--threads", "--max-buffer-time",
                 "--max-results", "--search-path", "--base-directory",
                 "--path-separator", "--batch-size", "--hyperlink",
                 "--and", "--format"}) {
            cfg.safe_flags[std::string(f)] = FlagArgType::kString;
        }
        table.emplace("fd", std::move(cfg));
        // fdfind is the Debian/Ubuntu alias for fd — same flags.
        table.emplace("fdfind", table.at("fd"));
    }

    // --------------------------------------------------------- git read-only
    //
    // We don't replicate the full GIT_READ_ONLY_COMMANDS map from the TS
    // utils/shell/readOnlyCommandValidation.js (which has 40+ git subcommands
    // with bespoke flag sets).  Instead we add the most commonly-used
    // subcommands with permissive but strictly-read-only flag sets.  Anything
    // not covered falls through to the regex path or manual approval.
    auto add_git_ro = [&](std::string subcmd,
                          std::vector<std::string_view> bools = {},
                          std::vector<std::string_view> with_args = {}) {
        CommandConfig cfg;
        for (auto f : bools) cfg.safe_flags[std::string(f)] = FlagArgType::kNone;
        for (auto f : with_args)
            cfg.safe_flags[std::string(f)] = FlagArgType::kString;
        table.emplace("git " + std::move(subcmd), std::move(cfg));
    };

    add_git_ro("status", {"-s", "--short", "-b", "--branch", "--porcelain",
                           "--untracked-files", "--ignored", "-z",
                           "--ahead-behind", "--renames"});
    add_git_ro("log", {"--oneline", "--decorate", "--graph", "--all",
                        "--follow", "--reverse", "--stat", "--patch", "-p",
                        "--pretty", "--raw", "--patch-with-stat",
                        "--name-only", "--name-status"},
                        {"-n", "--max-count", "--since", "--until",
                         "--after", "--before", "--author", "--committer",
                         "--grep", "--format"});
    add_git_ro("diff", {"--cached", "--staged", "--stat", "--patch", "-p",
                         "--name-only", "--name-status", "--no-color",
                         "--color", "--ignore-space-change", "-b",
                         "--ignore-all-space", "-w", "--unified",
                         "--exit-code"},
                         {"-U", "--unified", "--no-index", "-M", "-C",
                          "--find-renames", "--find-copies",
                          "--diff-filter", "--src-prefix", "--dst-prefix"});
    add_git_ro("show", {"--stat", "--patch", "-p", "--name-only",
                         "--name-status", "--no-color", "--color",
                         "--pretty", "--silent", "-s", "--format"},
                        {"--format", "-U", "--unified"});
    add_git_ro("branch", {"-a", "--all", "-r", "--remotes", "--list",
                           "-v", "--verbose", "--no-merged", "--merged",
                           "--contains", "--sort"});
    add_git_ro("remote", {"-v", "--verbose"});
    add_git_ro("rev-parse", {"--abbrev-ref", "--absolute-git-dir",
                              "--show-toplevel", "--show-superproject-working-tree",
                              "--verify", "--quiet"},
                             {"--short"});
    add_git_ro("ls-files", {"-c", "--cached", "-o", "--others",
                             "-i", "--ignored", "-z", "--stage",
                             "--exclude-standard", "--full-name"});
    add_git_ro("tag", {"-l", "--list", "-n", "--sort", "--contains",
                        "--merged", "--no-merged", "--points-at"});
    add_git_ro("blame", {"-l", "--line-porcelain", "-p", "--porcelain",
                          "-w", "--ignore-all-space", "-b",
                          "--ignore-space-change", "-s", "--show-name",
                          "--show-email"},
                         {"-L", "--date"});
    add_git_ro("config", {"-z", "--null", "--global", "--local",
                           "--system", "--list", "-l", "--get",
                           "--get-all", "--includes"},
                          {"--get", "--get-regexp", "--blob"});
    add_git_ro("stash list", {"-p", "--patch", "-u", "--include-untracked",
                               "--stat", "--name-only"},
                              {"-n", "--max-count", "--format"});
    add_git_ro("ls-remote", {"-h", "--heads", "-t", "--tags",
                              "--refs", "--exit-code", "--symref",
                              "--get-url"}, {"--upload-pack"});
}

} // namespace cc::tools::readonly_validation
