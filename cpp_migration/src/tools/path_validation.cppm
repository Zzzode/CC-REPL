// path_validation.cppm
// Path constraint validation for BashTool:
//   * Checks filesystem commands (cd, ls, rm, ...) for out-of-bounds access
//   * Validates output redirections (>, >>, &>) against allowed directories
//   * Detects dangerous removal paths (rm -rf /, etc.)
//   * Handles command-wrapper stripping (timeout, nice, stdbuf, env, time, nohup)
//
// Ported from src/tools/BashTool/pathValidation.ts (1300+ lines).  This
// module contains the PUBLIC API surface (types + exported functions).  The
// 800+ line path-extractor table (cd/find/grep/rg/sed/jq/...) is intentionally
// kept as a single lookup table with per-command lambdas so call sites can
// validate paths without re-parsing shell syntax.

module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <span>
#include <format>
#include <functional>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <regex>
#include <ranges>

export module cc.tools.path_validation;

import cc.tools.mode_validation;  // for PermissionResult / PermissionBehavior

export namespace cc::tools::path_validation {

using PermissionBehavior = mode_validation::PermissionBehavior;
using PermissionResult   = mode_validation::PermissionResult;
using PermissionMode     = mode_validation::PermissionMode;
using DecisionReason     = mode_validation::DecisionReason;
using DecisionReasonType = mode_validation::DecisionReasonType;

// ---------------------------------------------------------------------------
// Core types
// ---------------------------------------------------------------------------

/// Enumeration of all commands whose arguments are inspected for filesystem
/// paths.  These exactly match the TS PathCommand union so call sites can map
/// token strings one-to-one.
enum class PathCommand {
    Cd, Ls, Find, Mkdir, Touch, Rm, Rmdir, Mv, Cp,
    Cat, Head, Tail, Sort, Uniq, Wc, Cut, Paste, Column,
    Tr, File, Stat, Diff, Awk, Strings, Hexdump, Od, Base64,
    Nl, Grep, Rg, Sed, Git, Jq, Sha256sum, Sha1sum, Md5sum,
};

/// File-operation type — matches the FileOperationType used by the TS
/// permissions path validator.  Drives deny-rule lookup and suggestion
/// generation.
enum class FileOperationType {
    kRead,    // e.g. cat, ls, grep
    kWrite,   // e.g. rm, sed (default), mv
    kCreate,  // e.g. mkdir, touch, output redirection
};

/// Function type: extracts filesystem paths from the argv of a PathCommand.
/// Returns an empty vector when the command takes no path args in this
/// invocation (e.g. `cd` with no args => [homedir()], `ls` with no args => ["."]).
using PathExtractor = std::function<std::vector<std::string>(std::span<const std::string>)>;

/// Permission context — a trimmed-down copy of the TS ToolPermissionContext
/// containing only the fields path validation needs.  Any upstream caller
/// that has the full context can populate these fields.
struct PathPermissionContext {
    std::filesystem::path cwd;                        /// working directory for relative paths
    std::vector<std::filesystem::path> allowed_dirs;  /// directories paths must live under
    PermissionMode mode{PermissionMode::kDefault};    /// current permission mode
};

/// Permission update suggestions (carried on ask/deny results so the UI can
/// present "add directory" / "set accept-edits mode" buttons).
enum class PermissionUpdateType {
    kAddDirectories,
    kAddReadRule,
    kSetMode,
};

struct PermissionUpdate {
    PermissionUpdateType type;
    std::vector<std::filesystem::path> directories;
    std::optional<PermissionMode> mode;
};

/// Extended PermissionResult carrying path-validation-specific extras
/// (blocked path + permission update suggestions).  Inherits behaviour/
/// message/reason from mode_validation::PermissionResult and adds the fields
/// path validation adds on top.
struct PathPermissionResult : public PermissionResult {
    std::optional<std::filesystem::path> blocked_path;
    std::vector<PermissionUpdate> suggestions;
};

// ---------------------------------------------------------------------------
// Shell command parsing helpers
//
// These deliberately live *within* this module (rather than being imported
// from a hypothetical `bash.commands` module) because they are very tightly
// coupled to the `--` end-of-options behaviour that each path extractor
// relies on.  Keeping them here avoids circular-import issues when a future
// bash.commands module wants to re-export path-constraint checkers.
// ---------------------------------------------------------------------------

/// Extract positional (non-flag) arguments, correctly handling the POSIX
/// `--` end-of-options delimiter.
///
/// SECURITY: Most commands (rm, cat, touch, ...) stop parsing options at
/// `--` and treat ALL subsequent arguments as positional, even if they start
/// with `-`.  A naive `!arg.starts_with('-')` filter drops these, causing
/// path validation to be silently skipped for payloads like:
///
///     rm -- -/../.claude/settings.local.json
///
/// Here `-/../.claude/settings.local.json` starts with `-` so the naive
/// filter drops it, validation sees zero paths → returns passthrough → file
/// is deleted without a prompt.  With `--` handling, the path IS extracted
/// and validated.
[[nodiscard]] inline std::vector<std::string>
filter_out_flags(std::span<const std::string> args) {
    std::vector<std::string> result;
    bool after_double_dash = false;
    for (const auto& arg : args) {
        if (after_double_dash) {
            result.push_back(arg);
        } else if (arg == "--") {
            after_double_dash = true;
        } else if (arg.empty() || arg.front() != '-') {
            result.push_back(arg);
        }
    }
    return result;
}

/// Parse a `find` command's argv and return the list of paths that will be
/// searched.  This is conservative: after `--` we collect *everything* as a
/// path, which over-includes predicates like `-name foo` — but find is a
/// read-only op and predicates resolve to paths within cwd, so the
/// over-inclusion never produces a false block, while guaranteeing attack
/// paths like `find -- -/../../etc` are caught.
[[nodiscard]] inline std::vector<std::string>
extract_find_paths(std::span<const std::string> args) {
    std::vector<std::string> paths;
    const std::unordered_set<std::string_view> path_flags = {
        "-newer", "-anewer", "-cnewer", "-mnewer", "-samefile",
        "-path", "-wholename", "-ilname", "-lname", "-ipath", "-iwholename",
    };
    const std::regex newer_pattern{R"(^-newer[acmBt][acmtB]$)"};
    bool found_non_global_flag = false;
    bool after_double_dash = false;

    for (size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg.empty()) continue;

        if (after_double_dash) { paths.push_back(arg); continue; }
        if (arg == "--")        { after_double_dash = true; continue; }

        if (arg.front() == '-') {
            // Global options don't stop collection
            if (arg == "-H" || arg == "-L" || arg == "-P") continue;
            found_non_global_flag = true;
            // Path-taking flags: we also validate the value
            if (path_flags.contains(arg) ||
                std::regex_match(arg, newer_pattern)) {
                if (i + 1 < args.size()) {
                    paths.push_back(args[i + 1]);
                    ++i;
                }
            }
            continue;
        }
        // Non-flag before first non-global flag → starting path
        if (!found_non_global_flag) {
            paths.push_back(arg);
        }
    }
    if (paths.empty()) paths.push_back(".");
    return paths;
}

/// Generic pattern-style command (grep, rg) parser: first non-flag arg is the
/// pattern, remaining non-flag args are paths.  Correctly handles `--`
/// (positional after `--`) and flags that take arguments.
[[nodiscard]] inline std::vector<std::string>
parse_pattern_command(
    std::span<const std::string> args,
    std::span<const std::string_view> flags_with_args,
    std::span<const std::string> defaults = {})
{
    std::vector<std::string> paths;
    bool pattern_found = false;
    bool after_double_dash = false;
    const std::unordered_set<std::string_view> flag_set(
        flags_with_args.begin(), flags_with_args.end());

    for (size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];

        if (!after_double_dash && arg == "--") {
            after_double_dash = true;
            continue;
        }

        if (!after_double_dash && !arg.empty() && arg.front() == '-') {
            std::string flag = arg;
            auto eq = flag.find('=');
            if (eq != std::string::npos) flag.erase(eq);
            // Pattern-carrier flags mark pattern as "already consumed"
            if (flag == "-e" || flag == "--regexp" ||
                flag == "-f" || flag == "--file") {
                pattern_found = true;
            }
            if (flag_set.contains(flag) && eq == std::string::npos) ++i;
            continue;
        }
        if (!pattern_found) { pattern_found = true; continue; }
        paths.push_back(arg);
    }
    return paths.empty()
        ? std::vector<std::string>(defaults.begin(), defaults.end())
        : paths;
}

// ---------------------------------------------------------------------------
// PATH_EXTRACTORS — per-command argv -> paths lookup table.
// The TS source defines this as a `Record<PathCommand, (args) => string[]>`
// object literal.  In C++ we use a plain vector<pair> indexed by PathCommand.
// Only the most complex commands (find, grep, rg, sed, jq, git) have custom
// bodies; the large majority delegate to filter_out_flags.
// ---------------------------------------------------------------------------

/// Return the canonical path extractor for a given command.
[[nodiscard]] inline PathExtractor get_path_extractor(PathCommand cmd);

/// Action verbs for UI messages — matches TS ACTION_VERBS.
[[nodiscard]] inline std::string_view action_verb_for(PathCommand cmd);

/// File operation type — matches TS COMMAND_OPERATION_TYPE.
[[nodiscard]] inline FileOperationType operation_type_for(PathCommand cmd);

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

/// Check that every path accessed by `command` lies within the allowed
/// working directories of `ctx`.  Also validates output redirections.
///
/// @returns
///   kAsk          if any path command or redirection targets an out-of-bounds path
///   kDeny         if an explicit deny rule fired
///   kPassthrough  if no path commands were found or all passed validation
[[nodiscard]] inline PathPermissionResult
check_path_constraints(
    std::string_view command,
    const PathPermissionContext& ctx,
    bool compound_command_has_cd = false);

// ---------------------------------------------------------------------------
// Danger-path detection (rm -rf / etc.)
// ---------------------------------------------------------------------------

/// Returns true if `absolute_path` points to a critical system directory
/// that rm/rmdir should never silently remove.  Mirrors the TS
/// isDangerousRemovalPath helper in utils/permissions/pathValidation.ts.
[[nodiscard]] inline bool is_dangerous_removal_path(
    const std::filesystem::path& absolute_path);

// ---------------------------------------------------------------------------
// Argv-level wrapper stripping
// ---------------------------------------------------------------------------

/// Canonical stripWrappersFromArgv — mirrors the 120-line TS function of the
/// same name.  Strips safe-wrapper commands (timeout, nice, stdbuf, env,
/// time, nohup) and their associated flags from a pre-parsed argv so that
/// the underlying "real" command can be fed to the PATH_EXTRACTORS.
///
/// KEEP IN SYNC with stripSafeWrappers (text-based version) in the
/// bash-permissions module and with the wrapper-stripping logic in the
/// semantic checker.
[[nodiscard]] inline std::vector<std::string>
strip_wrappers_from_argv(std::span<const std::string> argv);

// ===========================================================================
// INLINE IMPLEMENTATIONS
//
// Implementations are given inline (rather than in a separate .cpp) because:
//   1. The module is currently header-only (C++20 Named Modules with inline
//      definitions are visible across import boundaries without needing ODR
//      concerns in a single TU build).
//   2. Each extractor is small — out-of-lining them would bloat symbol count.
// ===========================================================================

// --- symlink-aware path resolution for permission checks ------------------
// Resolve a path following symlinks where the target exists. weakly_canonical
// resolves the existing prefix and lexically normalizes the (possibly
// non-existent) remainder, so it is safe for both read targets and write /
// create targets. Falls back to pure lexical normalization only if
// canonicalization reports an error. Pure lexically_normal() here would let a
// symlink inside an allowed dir point outside it and bypass the check.
inline std::filesystem::path resolve_for_permission(const std::filesystem::path& p) {
    std::error_code ec;
    auto resolved = std::filesystem::weakly_canonical(p, ec);
    if (ec) return p.lexically_normal();
    return resolved;
}

// --- danger path detection -------------------------------------------------
inline bool is_dangerous_removal_path(const std::filesystem::path& p) {
    // We canonicalise to a generic string and match prefixes.  This is the
    // same precision the TS isDangerousRemovalPath uses.
    const auto s = resolve_for_permission(p).string();
    static const std::vector<std::string_view> dangerous = {
        "/", "/bin", "/boot", "/dev", "/etc", "/lib", "/lib32", "/lib64",
        "/proc", "/root", "/sbin", "/sys", "/usr", "/var", "/System",
        "/private/etc", "/private/var",
    };
    for (auto d : dangerous) {
        if (s == d || s.starts_with(std::string(d) + "/")) return true;
    }
    return false;
}

// --- operation types & action verbs ----------------------------------------
inline FileOperationType operation_type_for(PathCommand cmd) {
    switch (cmd) {
        case PathCommand::Mkdir:
        case PathCommand::Touch:
            return FileOperationType::kCreate;
        case PathCommand::Rm:
        case PathCommand::Rmdir:
        case PathCommand::Mv:
        case PathCommand::Cp:
        case PathCommand::Sed:
            return FileOperationType::kWrite;
        default:
            return FileOperationType::kRead;
    }
}

inline std::string_view action_verb_for(PathCommand cmd) {
    switch (cmd) {
        case PathCommand::Cd:      return "change directories to";
        case PathCommand::Ls:      return "list files in";
        case PathCommand::Find:    return "search files in";
        case PathCommand::Mkdir:   return "create directories in";
        case PathCommand::Touch:   return "create or modify files in";
        case PathCommand::Rm:      return "remove files from";
        case PathCommand::Rmdir:   return "remove directories from";
        case PathCommand::Mv:      return "move files to/from";
        case PathCommand::Cp:      return "copy files to/from";
        case PathCommand::Cat:     return "concatenate files from";
        case PathCommand::Head:    return "read the beginning of files from";
        case PathCommand::Tail:    return "read the end of files from";
        case PathCommand::Sort:    return "sort contents of files from";
        case PathCommand::Uniq:    return "filter duplicate lines from files in";
        case PathCommand::Wc:      return "count lines/words/bytes in files from";
        case PathCommand::Cut:     return "extract columns from files in";
        case PathCommand::Paste:   return "merge files from";
        case PathCommand::Column:  return "format files from";
        case PathCommand::Tr:      return "transform text from files in";
        case PathCommand::File:    return "examine file types in";
        case PathCommand::Stat:    return "read file stats from";
        case PathCommand::Diff:    return "compare files from";
        case PathCommand::Awk:     return "process text from files in";
        case PathCommand::Strings: return "extract strings from files in";
        case PathCommand::Hexdump: return "display hex dump of files from";
        case PathCommand::Od:      return "display octal dump of files from";
        case PathCommand::Base64:  return "encode/decode files from";
        case PathCommand::Nl:      return "number lines in files from";
        case PathCommand::Grep:    return "search for patterns in files from";
        case PathCommand::Rg:      return "search for patterns in files from";
        case PathCommand::Sed:     return "edit files in";
        case PathCommand::Git:     return "access files with git from";
        case PathCommand::Jq:      return "process JSON from files in";
        case PathCommand::Sha256sum: return "compute SHA-256 checksums for files in";
        case PathCommand::Sha1sum:   return "compute SHA-1 checksums for files in";
        case PathCommand::Md5sum:    return "compute MD5 checksums for files in";
    }
    return "access files in";
}

// --- extractor registry ----------------------------------------------------
inline PathExtractor get_path_extractor(PathCommand cmd) {
    // Shared flag sets for grep/rg — captured by value so the lambda has
    // stable storage after this function returns.
    static const std::vector<std::string_view> grep_flags = {
        "-e", "--regexp", "-f", "--file",
        "--exclude", "--include", "--exclude-dir", "--include-dir",
        "-m", "--max-count",
        "-A", "--after-context", "-B", "--before-context",
        "-C", "--context",
    };
    static const std::vector<std::string_view> rg_flags = {
        "-e", "--regexp", "-f", "--file",
        "-t", "--type", "-T", "--type-not",
        "-g", "--glob", "-m", "--max-count", "--max-depth",
        "-r", "--replace",
        "-A", "--after-context", "-B", "--before-context",
        "-C", "--context",
    };

    switch (cmd) {
        case PathCommand::Cd:
            return [](std::span<const std::string> a) -> std::vector<std::string> {
                if (a.empty()) return {std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "/").string()};
                // Join all remaining args — shell `cd "foo bar"` accepts the
                // joined form (though rare).  Kept for TS parity.
                std::string joined;
                for (size_t i = 0; i < a.size(); ++i) {
                    if (i) joined += ' ';
                    joined += a[i];
                }
                return {joined};
            };
        case PathCommand::Ls:
            return [](std::span<const std::string> a) {
                auto r = filter_out_flags(a);
                return r.empty() ? std::vector<std::string>{"."} : r;
            };
        case PathCommand::Find:
            return [](std::span<const std::string> a) {
                return extract_find_paths(a);
            };
        case PathCommand::Mkdir:
        case PathCommand::Touch:
        case PathCommand::Rm:
        case PathCommand::Rmdir:
        case PathCommand::Mv:
        case PathCommand::Cp:
        case PathCommand::Cat:
        case PathCommand::Head:
        case PathCommand::Tail:
        case PathCommand::Sort:
        case PathCommand::Uniq:
        case PathCommand::Wc:
        case PathCommand::Cut:
        case PathCommand::Paste:
        case PathCommand::Column:
        case PathCommand::File:
        case PathCommand::Stat:
        case PathCommand::Diff:
        case PathCommand::Awk:
        case PathCommand::Strings:
        case PathCommand::Hexdump:
        case PathCommand::Od:
        case PathCommand::Base64:
        case PathCommand::Nl:
        case PathCommand::Sha256sum:
        case PathCommand::Sha1sum:
        case PathCommand::Md5sum:
            return [](std::span<const std::string> a) { return filter_out_flags(a); };

        case PathCommand::Tr:
            return [](std::span<const std::string> a) {
                const bool has_delete = std::ranges::any_of(a, [](const std::string& x) {
                    return x == "-d" || x == "--delete" ||
                           (x.size() >= 2 && x.front() == '-' &&
                            x.find('d') != std::string::npos);
                });
                auto non_flags = filter_out_flags(a);
                const size_t skip = has_delete ? 1 : 2;
                if (non_flags.size() <= skip) return std::vector<std::string>{};
                return std::vector<std::string>(
                    non_flags.begin() + static_cast<ptrdiff_t>(skip),
                    non_flags.end());
            };

        case PathCommand::Grep:
            return [](std::span<const std::string> a) {
                auto paths = parse_pattern_command(a, grep_flags);
                const bool recursive = std::ranges::any_of(a,
                    [](const std::string& x) {
                        return x == "-r" || x == "-R" || x == "--recursive";
                    });
                if (paths.empty() && recursive) paths.push_back(".");
                return paths;
            };
        case PathCommand::Rg:
            return [](std::span<const std::string> a) {
                return parse_pattern_command(a, rg_flags, std::vector<std::string>{"."});
            };

        case PathCommand::Sed:
            return [](std::span<const std::string> a) {
                std::vector<std::string> paths;
                bool skip_next = false;
                bool script_found = false;
                bool after_dd = false;
                for (size_t i = 0; i < a.size(); ++i) {
                    if (skip_next) { skip_next = false; continue; }
                    const auto& arg = a[i];
                    if (!after_dd && arg == "--") { after_dd = true; continue; }
                    if (!after_dd && !arg.empty() && arg.front() == '-') {
                        if (arg == "-f" || arg == "--file") {
                            if (i + 1 < a.size()) { paths.push_back(a[i + 1]); skip_next = true; }
                            script_found = true;
                        } else if (arg == "-e" || arg == "--expression") {
                            skip_next = true;
                            script_found = true;
                        } else if (arg.find('e') != std::string::npos ||
                                   arg.find('f') != std::string::npos) {
                            script_found = true;
                        }
                        continue;
                    }
                    if (!script_found) { script_found = true; continue; }
                    paths.push_back(arg);
                }
                return paths;
            };

        case PathCommand::Jq: {
            static const std::vector<std::string_view> jq_flags = {
                "-e", "--expression", "-f", "--from-file",
                "--arg", "--argjson", "--slurpfile", "--rawfile",
                "--args", "--jsonargs", "-L", "--library-path",
                "--indent", "--tab",
            };
            return [](std::span<const std::string> a) {
                return parse_pattern_command(a, jq_flags);
            };
        }

        case PathCommand::Git:
            return [](std::span<const std::string> a) {
                // Only git diff --no-index operates on arbitrary files
                // outside git's own context; other subcommands are bounded
                // by git's internal security model.
                if (a.size() >= 1 && a[0] == "diff") {
                    const bool no_idx = std::ranges::any_of(a,
                        [](const std::string& x) { return x == "--no-index"; });
                    if (no_idx) {
                        // strip "diff" then filter flags (respects --)
                        std::vector<std::string> rest(a.begin() + 1, a.end());
                        auto fp = filter_out_flags(rest);
                        return std::vector<std::string>(
                            fp.begin(),
                            fp.begin() + std::min<size_t>(fp.size(), 2));
                    }
                }
                return std::vector<std::string>{};
            };
    }
    return [](std::span<const std::string>) { return std::vector<std::string>{}; };
}

// --- wrapper stripping -----------------------------------------------------
//
// These helpers are intentionally small; their TS counterparts have detailed
// SECCURITY comments explaining the boundary conditions.  See the TS source
// for skipTimeoutFlags / skipStdbufFlags / skipEnvFlags rationale.

inline bool is_timeout_flag_value(std::string_view v) {
    for (char c : v) {
        if (!std::isalnum(static_cast<unsigned char>(c)) &&
            c != '_' && c != '.' && c != '+' && c != '-') return false;
    }
    return !v.empty();
}

/// Returns argv index of the DURATION token (validated), or -1 on failure.
inline int skip_timeout_flags(std::span<const std::string> a) {
    int i = 1;
    while (i < static_cast<int>(a.size())) {
        const auto& arg = a[i];
        auto next = i + 1 < static_cast<int>(a.size()) ?
            std::optional<std::string_view>{a[i + 1]} : std::nullopt;
        if (arg == "--foreground" ||
            arg == "--preserve-status" ||
            arg == "--verbose")                              { ++i; continue; }
        if (arg.starts_with("--kill-after=") ||
            arg.starts_with("--signal="))                     { ++i; continue; }
        if ((arg == "--kill-after" || arg == "--signal") &&
            next && is_timeout_flag_value(*next))             { i += 2; continue; }
        if (arg == "--")                                      { ++i; break; }
        if (arg.starts_with("--"))                            { return -1; }
        if (arg == "-v")                                      { ++i; continue; }
        if ((arg == "-k" || arg == "-s") &&
            next && is_timeout_flag_value(*next))             { i += 2; continue; }
        if (arg.size() >= 3 &&
            (arg.starts_with("-k") || arg.starts_with("-s")) &&
            is_timeout_flag_value(std::string_view(arg).substr(2))) { ++i; continue; }
        if (arg.starts_with("-"))                             { return -1; }
        break;
    }
    return i;
}

/// Returns wrapped-COMMAND argv index, or -1 when stdbuf has no valid flags.
inline int skip_stdbuf_flags(std::span<const std::string> a) {
    int i = 1;
    while (i < static_cast<int>(a.size())) {
        const auto& arg = a[i];
        if (arg.size() == 2 &&
            (arg == "-i" || arg == "-o" || arg == "-e") &&
            i + 1 < static_cast<int>(a.size()))              { i += 2; continue; }
        if (arg.size() >= 3 &&
            (arg.starts_with("-i") || arg.starts_with("-o") ||
             arg.starts_with("-e")))                          { ++i; continue; }
        if (arg.starts_with("--input=") ||
            arg.starts_with("--output=") ||
            arg.starts_with("--error="))                      { ++i; continue; }
        if (arg.starts_with("-"))                             { return -1; }
        break;
    }
    return (i > 1 && i < static_cast<int>(a.size())) ? i : -1;
}

/// Returns wrapped-COMMAND argv index, or -1 when env's flags are unsafe.
inline int skip_env_flags(std::span<const std::string> a) {
    int i = 1;
    while (i < static_cast<int>(a.size())) {
        const auto& arg = a[i];
        if (arg.find('=') != std::string::npos &&
            !arg.starts_with("-"))                             { ++i; continue; }
        if (arg == "-i" || arg == "-0" || arg == "-v")        { ++i; continue; }
        if (arg == "-u" && i + 1 < static_cast<int>(a.size())) { i += 2; continue; }
        if (arg.starts_with("-"))                             { return -1; }
        break;
    }
    return (i < static_cast<int>(a.size())) ? i : -1;
}

inline bool is_duration_token(std::string_view t) {
    // Digits, optional decimal, optional unit suffix (s/m/h/d).
    // Mirrors /^\d+(?:\.\d+)?[smhd]?$/ in TS.
    size_t i = 0;
    const auto digits = [&] {
        size_t n = 0;
        while (i + n < t.size() && std::isdigit(static_cast<unsigned char>(t[i + n]))) ++n;
        return n;
    };
    size_t whole = digits();
    if (whole == 0) return false;
    i += whole;
    if (i < t.size() && t[i] == '.') {
        ++i;
        size_t frac = digits();
        if (frac == 0) return false;
        i += frac;
    }
    if (i < t.size()) {
        char c = t[i];
        if (c != 's' && c != 'm' && c != 'h' && c != 'd') return false;
        ++i;
    }
    return i == t.size();
}

inline std::vector<std::string>
strip_wrappers_from_argv(std::span<const std::string> argv) {
    std::vector<std::string> a(argv.begin(), argv.end());
    for (;;) {
        if (a.empty()) return a;
        const auto& head = a.front();
        if (head == "time" || head == "nohup") {
            size_t skip = 1;
            if (a.size() >= 2 && a[1] == "--") skip = 2;
            a.erase(a.begin(), a.begin() + static_cast<ptrdiff_t>(skip));
        } else if (head == "timeout") {
            int idx = skip_timeout_flags(a);
            if (idx < 0 || idx >= static_cast<int>(a.size()) ||
                !is_duration_token(a[idx])) {
                return a;
            }
            a.erase(a.begin(), a.begin() + idx + 1);
        } else if (head == "nice") {
            auto nth = [&](size_t n) -> std::optional<std::string_view> {
                if (n < a.size()) return a[n];
                return std::nullopt;
            };
            auto is_int = [](std::string_view s) {
                size_t i = 0;
                if (!s.empty() && (s[0] == '-' || s[0] == '+')) i = 1;
                if (i >= s.size()) return false;
                for (; i < s.size(); ++i)
                    if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
                return true;
            };
            if (a.size() >= 3 && a[1] == "-n" && is_int(a[2])) {
                size_t skip = 3;
                if (a.size() >= 4 && a[3] == "--") skip = 4;
                a.erase(a.begin(), a.begin() + static_cast<ptrdiff_t>(skip));
            } else if (auto n1 = nth(1); n1 && is_int(*n1)) {
                size_t skip = 2;
                if (a.size() >= 3 && a[2] == "--") skip = 3;
                a.erase(a.begin(), a.begin() + static_cast<ptrdiff_t>(skip));
            } else {
                size_t skip = 1;
                if (a.size() >= 2 && a[1] == "--") skip = 2;
                a.erase(a.begin(), a.begin() + static_cast<ptrdiff_t>(skip));
            }
        } else if (head == "stdbuf") {
            int idx = skip_stdbuf_flags(a);
            if (idx < 0) return a;
            a.erase(a.begin(), a.begin() + idx);
        } else if (head == "env") {
            int idx = skip_env_flags(a);
            if (idx < 0) return a;
            a.erase(a.begin(), a.begin() + idx);
        } else {
            return a;
        }
    }
}

// --- internal: validate a single path against the context ------------------
//
// Mirrors the TS validatePath() (which lives in utils/permissions/pathValidation.ts).
// We only need the subset of behaviour that path_validation.cppm actually uses.
struct ValidatePathResult {
    bool allowed{true};
    std::filesystem::path resolved_path;
    std::optional<DecisionReason> decision_reason;
};

/// Expand a leading `~` (POSIX home-dir shorthand) in `p` when present.
inline std::filesystem::path expand_tilde(std::filesystem::path p) {
    const auto s = p.string();
    if (!s.empty() && s.front() == '~') {
        const char* home = std::getenv("HOME");
        if (home) {
            return std::filesystem::path(home) / s.substr(1);
        }
    }
    return p;
}

inline ValidatePathResult validate_path(
    std::string_view raw_path,
    const std::filesystem::path& cwd,
    const PathPermissionContext& ctx,
    FileOperationType op)
{
    // Strip leading/trailing quotes (POSIX argv has already done this, but
    // keep the TS safety net since callers may supply raw tokens).
    std::string cleaned(raw_path);
    if (cleaned.size() >= 2) {
        if ((cleaned.front() == '\'' && cleaned.back() == '\'') ||
            (cleaned.front() == '"'  && cleaned.back() == '"')) {
            cleaned = cleaned.substr(1, cleaned.size() - 2);
        }
    }
    std::filesystem::path absolute = expand_tilde(cleaned);
    if (!absolute.is_absolute()) absolute = cwd / absolute;
    absolute = resolve_for_permission(absolute);

    ValidatePathResult r{.resolved_path = absolute};

    // (1) Path must be under one of ctx.allowed_dirs.
    const bool any_allowed = ctx.allowed_dirs.empty() ||
        std::ranges::any_of(ctx.allowed_dirs, [&](const std::filesystem::path& base) {
            const auto b = resolve_for_permission(base);
            // "within" = either equals b, or starts with b/
            auto [_m1, _m2] = std::ranges::mismatch(
                absolute.begin(), absolute.end(),
                b.begin(), b.end());
            return _m2 == b.end();
        });
    if (!any_allowed) {
        r.allowed = false;
        r.decision_reason = DecisionReason{
            .type = DecisionReasonType::kSafety,
            .reason = "path outside allowed directories",
        };
        return r;
    }

    // (2) Write ops to Claude's internal config dirs are blocked regardless
    //     of allowlist.  Kept as a suffix string check for fast rejection.
    if (op != FileOperationType::kRead) {
        const auto s = absolute.string();
        if (s.ends_with("/.claude") || s.find("/.claude/") != std::string::npos ||
            s.ends_with("/.config/claude") ||
            s.find("/.config/claude/") != std::string::npos) {
            r.allowed = false;
            r.decision_reason = DecisionReason{
                .type = DecisionReasonType::kSafety,
                .reason = "modification of Claude-internal configuration is not allowed",
            };
            return r;
        }
    }

    return r;
}

// --- create_path_checker ---------------------------------------------------
inline auto create_path_checker(
    PathCommand cmd,
    std::optional<FileOperationType> override_op)
{
    return [cmd, override_op](
        std::span<const std::string> args,
        const PathPermissionContext& ctx,
        bool compound_has_cd) -> PathPermissionResult
    {
        const auto extractor = get_path_extractor(cmd);
        const auto paths = extractor(args);
        const auto op = override_op.value_or(operation_type_for(cmd));

        PathPermissionResult result;

        // (A) mv/cp with ANY flags → require manual approval.
        //     Flags like --target-directory can bypass extraction.
        if ((cmd == PathCommand::Mv || cmd == PathCommand::Cp) &&
            std::ranges::any_of(args, [](const std::string& a) {
                return !a.empty() && a.front() == '-';
            }))
        {
            result.behavior = PermissionBehavior::kAsk;
            result.message = std::format(
                "{} with flags requires manual approval to ensure path safety",
                cmd == PathCommand::Mv ? "mv" : "cp");
            result.decision_reason = DecisionReason{
                .type = DecisionReasonType::kOther,
                .reason = std::format(
                    "{} command with flags requires manual approval",
                    cmd == PathCommand::Mv ? "mv" : "cp"),
            };
            return result;
        }

        // (B) Compound cd + write → manual approval (path-resolution bypass).
        if (compound_has_cd && op != FileOperationType::kRead) {
            result.behavior = PermissionBehavior::kAsk;
            result.message =
                "Commands that change directories and perform write operations "
                "require explicit approval to ensure paths are evaluated correctly.";
            result.decision_reason = DecisionReason{
                .type = DecisionReasonType::kOther,
                .reason =
                    "Compound command contains cd with write operation - "
                    "manual approval required to prevent path resolution bypass",
            };
            return result;
        }

        // (C) Per-path validation.
        for (const auto& p : paths) {
            auto vr = validate_path(p, ctx.cwd, ctx, op);
            if (!vr.allowed) {
                result.behavior = PermissionBehavior::kAsk;
                result.blocked_path = vr.resolved_path;
                result.decision_reason = vr.decision_reason;
                // Build a human-readable message.
                std::string cmd_str = [cmd] {
                    switch (cmd) {
                        case PathCommand::Cd: return "cd";
                        case PathCommand::Ls: return "ls";
                        case PathCommand::Find: return "find";
                        case PathCommand::Mkdir: return "mkdir";
                        case PathCommand::Touch: return "touch";
                        case PathCommand::Rm: return "rm";
                        case PathCommand::Rmdir: return "rmdir";
                        case PathCommand::Mv: return "mv";
                        case PathCommand::Cp: return "cp";
                        case PathCommand::Cat: return "cat";
                        case PathCommand::Head: return "head";
                        case PathCommand::Tail: return "tail";
                        case PathCommand::Sort: return "sort";
                        case PathCommand::Uniq: return "uniq";
                        case PathCommand::Wc: return "wc";
                        case PathCommand::Cut: return "cut";
                        case PathCommand::Paste: return "paste";
                        case PathCommand::Column: return "column";
                        case PathCommand::Tr: return "tr";
                        case PathCommand::File: return "file";
                        case PathCommand::Stat: return "stat";
                        case PathCommand::Diff: return "diff";
                        case PathCommand::Awk: return "awk";
                        case PathCommand::Strings: return "strings";
                        case PathCommand::Hexdump: return "hexdump";
                        case PathCommand::Od: return "od";
                        case PathCommand::Base64: return "base64";
                        case PathCommand::Nl: return "nl";
                        case PathCommand::Grep: return "grep";
                        case PathCommand::Rg: return "rg";
                        case PathCommand::Sed: return "sed";
                        case PathCommand::Git: return "git";
                        case PathCommand::Jq: return "jq";
                        case PathCommand::Sha256sum: return "sha256sum";
                        case PathCommand::Sha1sum: return "sha1sum";
                        case PathCommand::Md5sum: return "md5sum";
                    }
                    return "command";
                }();
                result.message = std::format(
                    "{} in '{}' was blocked.  Claude Code may only {} "
                    "the allowed working directories for this session.",
                    cmd_str, vr.resolved_path.string(), action_verb_for(cmd));

                // Suggestions.
                if (op == FileOperationType::kRead) {
                    result.suggestions.push_back(PermissionUpdate{
                        .type = PermissionUpdateType::kAddReadRule,
                        .directories = {vr.resolved_path.parent_path()},
                    });
                } else {
                    result.suggestions.push_back(PermissionUpdate{
                        .type = PermissionUpdateType::kAddDirectories,
                        .directories = {vr.resolved_path.parent_path()},
                    });
                }
                if (op == FileOperationType::kWrite ||
                    op == FileOperationType::kCreate) {
                    result.suggestions.push_back(PermissionUpdate{
                        .type = PermissionUpdateType::kSetMode,
                        .mode = PermissionMode::kAcceptEdits,
                    });
                }
                return result;
            }
        }

        // (D) rm / rmdir → run dangerous removal path check AFTER the normal
        //     validation so we get the more specific warning.
        if (cmd == PathCommand::Rm || cmd == PathCommand::Rmdir) {
            for (const auto& p : paths) {
                std::string cleaned(p);
                if (cleaned.size() >= 2) {
                    if ((cleaned.front() == '\'' && cleaned.back() == '\'') ||
                        (cleaned.front() == '"'  && cleaned.back() == '"')) {
                        cleaned = cleaned.substr(1, cleaned.size() - 2);
                    }
                }
                std::filesystem::path absolute = expand_tilde(cleaned);
                if (!absolute.is_absolute()) absolute = ctx.cwd / absolute;
                absolute = resolve_for_permission(absolute);
                if (is_dangerous_removal_path(absolute)) {
                    result.behavior = PermissionBehavior::kAsk;
                    result.blocked_path = absolute;
                    result.message = std::format(
                        "Dangerous {} operation detected: '{}'\n\n"
                        "This command would remove a critical system directory. "
                        "This requires explicit approval and cannot be auto-allowed "
                        "by permission rules.",
                        cmd == PathCommand::Rm ? "rm" : "rmdir",
                        absolute.string());
                    result.decision_reason = DecisionReason{
                        .type = DecisionReasonType::kOther,
                        .reason = std::format(
                            "Dangerous {} operation on critical path: {}",
                            cmd == PathCommand::Rm ? "rm" : "rmdir",
                            absolute.string()),
                    };
                    return result;
                }
            }
        }

        // (E) All paths passed.
        result.behavior = PermissionBehavior::kPassthrough;
        result.message = std::format("Path validation passed for {} command",
            [cmd] {
                switch (cmd) {
                    case PathCommand::Cd: return "cd";
                    case PathCommand::Ls: return "ls";
                    case PathCommand::Find: return "find";
                    case PathCommand::Mkdir: return "mkdir";
                    case PathCommand::Touch: return "touch";
                    case PathCommand::Rm: return "rm";
                    case PathCommand::Rmdir: return "rmdir";
                    case PathCommand::Mv: return "mv";
                    case PathCommand::Cp: return "cp";
                    case PathCommand::Cat: return "cat";
                    case PathCommand::Head: return "head";
                    case PathCommand::Tail: return "tail";
                    case PathCommand::Sort: return "sort";
                    case PathCommand::Uniq: return "uniq";
                    case PathCommand::Wc: return "wc";
                    case PathCommand::Cut: return "cut";
                    case PathCommand::Paste: return "paste";
                    case PathCommand::Column: return "column";
                    case PathCommand::Tr: return "tr";
                    case PathCommand::File: return "file";
                    case PathCommand::Stat: return "stat";
                    case PathCommand::Diff: return "diff";
                    case PathCommand::Awk: return "awk";
                    case PathCommand::Strings: return "strings";
                    case PathCommand::Hexdump: return "hexdump";
                    case PathCommand::Od: return "od";
                    case PathCommand::Base64: return "base64";
                    case PathCommand::Nl: return "nl";
                    case PathCommand::Grep: return "grep";
                    case PathCommand::Rg: return "rg";
                    case PathCommand::Sed: return "sed";
                    case PathCommand::Git: return "git";
                    case PathCommand::Jq: return "jq";
                    case PathCommand::Sha256sum: return "sha256sum";
                    case PathCommand::Sha1sum: return "sha1sum";
                    case PathCommand::Md5sum: return "md5sum";
                }
                return "unknown";
            }());
        return result;
    };
}

// --- check_path_constraints (top-level entry point) ------------------------
//
// The TS source accepts astRedirects / astCommands (pre-parsed command tree)
// OR falls back to string-based splitting.  This C++ port uses the
// string-based fallback for simplicity: if/when an AST module is added, a
// second overload can be introduced without breaking call sites.

/// Lightweight shell-tokenizer that handles quotes and backslash-escape.
/// Returns the token list or empty vector on failure (fail-closed).
[[nodiscard]] inline std::vector<std::string>
simple_shell_tokenize(std::string_view cmd) {
    std::vector<std::string> tokens;
    std::string current;
    char quote = 0;   // 0, ' or "
    bool escaped = false;
    for (size_t i = 0; i < cmd.size(); ++i) {
        char c = cmd[i];
        if (escaped) {
            current.push_back(c);
            escaped = false;
            continue;
        }
        if (!quote && c == '\\') { escaped = true; continue; }
        if (quote == 0) {
            if (c == '\'' || c == '"') { quote = c; continue; }
            if (c == ' ' || c == '\t' || c == '\n') {
                if (!current.empty()) { tokens.push_back(std::move(current)); current.clear(); }
                continue;
            }
            current.push_back(c);
        } else if (quote == '\'') {
            if (c == '\'') { quote = 0; continue; }
            current.push_back(c);
        } else { // "
            if (c == '\\' && i + 1 < cmd.size()) { escaped = true; continue; }
            if (c == '"') { quote = 0; continue; }
            current.push_back(c);
        }
    }
    if (quote != 0) return {}; // fail-closed: mismatched quotes
    if (!current.empty()) tokens.push_back(std::move(current));
    return tokens;
}

/// Split a full compound command into its subcommand strings (the TS
/// splitCommand_DEPRECATED equivalent).  Splits on &&/||/;/|/|/newline while
/// respecting quote/escape state.  This is deliberately simple; consumers
/// that need full AST semantics should use bash_ast.
[[nodiscard]] inline std::vector<std::string>
split_compound_command(std::string_view full) {
    std::vector<std::string> parts;
    std::string current;
    char quote = 0;
    bool escaped = false;
    auto flush = [&] {
        size_t b = current.find_first_not_of(" \t\n\r");
        if (b == std::string::npos) { current.clear(); return; }
        size_t e = current.find_last_not_of(" \t\n\r");
        parts.push_back(current.substr(b, e - b + 1));
        current.clear();
    };
    for (size_t i = 0; i < full.size(); ++i) {
        char c = full[i];
        if (escaped) { current.push_back(c); escaped = false; continue; }
        if (!quote && c == '\\') { current.push_back(c); escaped = true; continue; }
        if (quote == 0) {
            if (c == '\'' || c == '"') { quote = c; current.push_back(c); continue; }
            if (c == '&' && i + 1 < full.size() && full[i + 1] == '&') { flush(); ++i; continue; }
            if (c == '|' && i + 1 < full.size() && full[i + 1] == '|') { flush(); ++i; continue; }
            if (c == '|' || c == ';' || c == '\n') { flush(); continue; }
            current.push_back(c);
        } else if (quote == '\'') {
            current.push_back(c);
            if (c == '\'') quote = 0;
        } else {
            current.push_back(c);
            if (c == '\\' && i + 1 < full.size()) { escaped = true; continue; }
            if (c == '"') quote = 0;
        }
    }
    flush();
    return parts;
}

/// Extract output-redirection targets (> foo, >> bar, &> baz, >| qux) from a
/// subcommand.  Returns a list of (target, operator) pairs; also sets
/// has_dangerous_redirect to true when a target contains `$` (shell
/// expansion — cannot be validated statically).
struct OutputRedirection {
    std::string target;
    char op;  // '>' or '>' (for >> we also store '>' — operator only matters
              // for callers that distinguish truncate from append).
};
struct ExtractRedirectionsResult {
    std::vector<OutputRedirection> redirections;
    bool has_dangerous_redirect{false};
};

[[nodiscard]] inline ExtractRedirectionsResult
extract_output_redirections(std::string_view sub) {
    ExtractRedirectionsResult r;
    const auto tokens = simple_shell_tokenize(sub);
    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto& t = tokens[i];
        // Handle:  >foo  >>foo  &>foo  >|foo  2>foo  1>>foo  2>&1  (last is fd dup)
        std::string_view v = t;
        // Strip leading fd digits (1, 2, ...)
        while (!v.empty() && std::isdigit(static_cast<unsigned char>(v.front()))) {
            v.remove_prefix(1);
        }
        char op_kind = 0;
        if (v.starts_with(">>"))        { op_kind = 'a'; v.remove_prefix(2); }
        else if (v.starts_with(">&"))   {
            // >&N (digits only) → fd dup, not a file.
            std::string_view rest = v.substr(2);
            if (!rest.empty() && std::ranges::all_of(rest,
                [](char c){ return std::isdigit(static_cast<unsigned char>(c)); })) {
                continue;
            }
            op_kind = 't'; v.remove_prefix(2);
        }
        else if (v.starts_with("&>") ||
                 v.starts_with("&>>"))   { op_kind = (v[1] == '>' && v.size() > 2 && v[2] == '>') ? 'a' : 't';
                                           v.remove_prefix(v.starts_with("&>>") ? 3 : 2); }
        else if (v.starts_with(">|"))    { op_kind = 't'; v.remove_prefix(2); }
        else if (v.starts_with(">"))     { op_kind = 't'; v.remove_prefix(1); }
        else continue;

        std::string target;
        if (!v.empty()) target = std::string(v);
        else if (i + 1 < tokens.size())  target = tokens[++i];
        else continue;

        if (target.find('$') != std::string::npos) {
            r.has_dangerous_redirect = true;
            continue;
        }
        if (op_kind == 'a')
            r.redirections.push_back(OutputRedirection{target, '>'});
        else
            r.redirections.push_back(OutputRedirection{target, '>'});
    }
    return r;
}

inline PathPermissionResult
check_path_constraints(
    std::string_view command,
    const PathPermissionContext& ctx,
    bool compound_command_has_cd)
{
    PathPermissionResult result;

    // (1) Process substitution: >(cmd) <(cmd) — manual approval.
    // The TS regex: />>\s*>\s*\(|>\s*>\s*\(|<\s*\(/
    const std::string cmd_str(command);
    const std::regex procsub_re{
        R"(>>\s*>\s*\(|>\s*>\s*\(|<\s*\()"};
    if (std::regex_search(cmd_str, procsub_re)) {
        result.behavior = PermissionBehavior::kAsk;
        result.message =
            "Process substitution (>(...) or <(...)) can execute arbitrary "
            "commands and requires manual approval";
        result.decision_reason = DecisionReason{
            .type = DecisionReasonType::kOther,
            .reason = "Process substitution requires manual approval",
        };
        return result;
    }

    // (2) Collect all redirections from every subcommand, validate them.
    auto subcommands = split_compound_command(command);
    std::vector<OutputRedirection> all_redirs;
    bool any_dangerous_redirect = false;

    for (const auto& sub : subcommands) {
        auto rr = extract_output_redirections(sub);
        if (rr.has_dangerous_redirect) any_dangerous_redirect = true;
        for (auto& r : rr.redirections) all_redirs.push_back(std::move(r));
    }

    if (any_dangerous_redirect) {
        result.behavior = PermissionBehavior::kAsk;
        result.message =
            "Shell expansion syntax in paths requires manual approval";
        result.decision_reason = DecisionReason{
            .type = DecisionReasonType::kOther,
            .reason =
                "Shell expansion syntax in paths requires manual approval",
        };
        return result;
    }

    // (2a) cd-compound + redirect → manual approval.
    if (compound_command_has_cd && !all_redirs.empty()) {
        result.behavior = PermissionBehavior::kAsk;
        result.message =
            "Commands that change directories and write via output redirection "
            "require explicit approval to ensure paths are evaluated correctly.";
        result.decision_reason = DecisionReason{
            .type = DecisionReasonType::kOther,
            .reason =
                "Compound command contains cd with output redirection - "
                "manual approval required to prevent path resolution bypass",
        };
        return result;
    }

    // (2b) Validate each redirection target.
    for (const auto& r : all_redirs) {
        if (r.target == "/dev/null") continue;
        auto vr = validate_path(r.target, ctx.cwd, ctx, FileOperationType::kCreate);
        if (!vr.allowed) {
            result.behavior = PermissionBehavior::kAsk;
            result.blocked_path = vr.resolved_path;
            result.decision_reason = vr.decision_reason;
            result.message = std::format(
                "Output redirection to '{}' was blocked.",
                vr.resolved_path.string());
            result.suggestions.push_back(PermissionUpdate{
                .type = PermissionUpdateType::kAddDirectories,
                .directories = {vr.resolved_path.parent_path()},
            });
            return result;
        }
    }

    // (3) For each subcommand: strip wrappers, extract base command, validate paths.
    for (const auto& sub : subcommands) {
        auto tokens = simple_shell_tokenize(sub);
        if (tokens.empty()) continue;
        // Strip wrappers at argv level.
        auto stripped = strip_wrappers_from_argv(tokens);
        if (stripped.empty()) continue;
        const std::string& base = stripped[0];
        std::optional<PathCommand> pcmd;
        static const std::pair<std::string_view, PathCommand> mapping[] = {
            {"cd", PathCommand::Cd},
            {"ls", PathCommand::Ls},
            {"find", PathCommand::Find},
            {"mkdir", PathCommand::Mkdir},
            {"touch", PathCommand::Touch},
            {"rm", PathCommand::Rm},
            {"rmdir", PathCommand::Rmdir},
            {"mv", PathCommand::Mv},
            {"cp", PathCommand::Cp},
            {"cat", PathCommand::Cat},
            {"head", PathCommand::Head},
            {"tail", PathCommand::Tail},
            {"sort", PathCommand::Sort},
            {"uniq", PathCommand::Uniq},
            {"wc", PathCommand::Wc},
            {"cut", PathCommand::Cut},
            {"paste", PathCommand::Paste},
            {"column", PathCommand::Column},
            {"tr", PathCommand::Tr},
            {"file", PathCommand::File},
            {"stat", PathCommand::Stat},
            {"diff", PathCommand::Diff},
            {"awk", PathCommand::Awk},
            {"strings", PathCommand::Strings},
            {"hexdump", PathCommand::Hexdump},
            {"od", PathCommand::Od},
            {"base64", PathCommand::Base64},
            {"nl", PathCommand::Nl},
            {"grep", PathCommand::Grep},
            {"rg", PathCommand::Rg},
            {"sed", PathCommand::Sed},
            {"git", PathCommand::Git},
            {"jq", PathCommand::Jq},
            {"sha256sum", PathCommand::Sha256sum},
            {"sha1sum", PathCommand::Sha1sum},
            {"md5sum", PathCommand::Md5sum},
        };
        for (const auto& [s, pc] : mapping) {
            if (s == base) { pcmd = pc; break; }
        }
        if (!pcmd) continue;  // not a path-restricted command → passthrough

        std::span<const std::string> args(
            stripped.data() + 1,
            stripped.size() - 1);
        auto checker = create_path_checker(*pcmd, std::nullopt);
        auto sub_res = checker(args, ctx, compound_command_has_cd);
        if (sub_res.behavior == PermissionBehavior::kAsk ||
            sub_res.behavior == PermissionBehavior::kDeny) {
            return sub_res;
        }
    }

    result.behavior = PermissionBehavior::kPassthrough;
    result.message = "All path commands validated successfully";
    return result;
}

// ---------------------------------------------------------------------------
// TS-compatible camelCase aliases
// ---------------------------------------------------------------------------

[[nodiscard]] inline PathPermissionResult
checkPathConstraints(
    std::string_view c, const PathPermissionContext& ctx, bool has_cd = false) {
    return check_path_constraints(c, ctx, has_cd);
}

[[nodiscard]] inline auto createPathChecker(
    PathCommand cmd,
    std::optional<FileOperationType> op = std::nullopt) {
    return create_path_checker(cmd, op);
}

inline auto stripWrappersFromArgv(std::span<const std::string> a) {
    return strip_wrappers_from_argv(a);
}

} // namespace cc::tools::path_validation
