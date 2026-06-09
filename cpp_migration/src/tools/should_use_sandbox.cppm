// should_use_sandbox.cppm
// Decides whether a given bash command should execute inside the sandbox.
//
// Sandboxing decision tree (mirrors TS shouldUseSandbox + containsExcludedCommand):
//
//   1. If sandboxing is globally disabled → no.
//   2. If caller explicitly set dangerously_disable_sandbox AND unsandboxed
//      commands are allowed by policy → no.
//   3. If there is no command → no.
//   4. If the command (or any subcommand) matches a user-configured
//      excluded-command pattern → no.  The excluded list can contain:
//        - prefix patterns: "bazel"  matches "bazel build //..."
//        - exact patterns:  "make"   matches only bare "make"
//        - wildcard patterns: "npm run *" matches prefix + any suffix
//      Patterns are matched with wrapper commands (timeout, env, ...) and
//      leading environment variables stripped.
//   5. Otherwise → yes, sandbox.
//
// Ported from src/tools/BashTool/shouldUseSandbox.ts.

module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <array>
#include <unordered_set>
#include <span>
#include <algorithm>
#include <cctype>
#include <functional>
#include <regex>
#include <ranges>

export module cc.tools.should_use_sandbox;

import cc.tools.bash_security;

export namespace cc::tools::sandbox {

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/// The partial input object that should_use_sandbox() reads.  The TS version
/// takes a Partial<SandboxInput> (command + dangerously_disable_sandbox); we
/// mirror that shape exactly.
struct SandboxInput {
    std::optional<std::string> command;
    bool dangerously_disable_sandbox{false};
};

/// Type of a user-configured excluded-command rule.  Mirrors TS
/// bashPermissionRule() which inspects a pattern string for `:` and `*`.
enum class PermissionRuleType {
    kPrefix,   // e.g. "bazel" — any command starting with "bazel " or equal to "bazel"
    kExact,    // e.g. "make=" — the "=" suffix pins to exact match
    kWildcard, // e.g. "npm run *:test"  or any pattern containing '*'
};

struct BashPermissionRule {
    PermissionRuleType type;
    std::string prefix;   // for kPrefix / kExact
    std::string command;  // for kExact
    std::string pattern;  // for kWildcard (original pattern)
};

/// Platform / global sandbox configuration.  The TS module reads these from
/// SandboxManager; callers must supply the equivalent snapshot via this
/// struct so the function stays pure.
struct SandboxRuntimeConfig {
    bool sandboxing_enabled{true};
    bool unsandboxed_commands_allowed{true};
    /// User-facing excluded commands (from settings.sandbox.excludedCommands).
    /// Each raw string is parsed via parse_permission_rule().
    std::vector<std::string> user_excluded_commands;
};

// ---------------------------------------------------------------------------
// Helpers (exposed inline for testability)
// ---------------------------------------------------------------------------

/// Parse a raw user-supplied excluded-command rule string into its structured
/// equivalent.  Mirrors TS bashPermissionRule() in bashPermissions.ts.
[[nodiscard]] inline BashPermissionRule
parse_permission_rule(std::string_view raw_pattern) {
    const std::string pattern(raw_pattern);
    BashPermissionRule r{.pattern = pattern};

    // Wildcard rule: pattern contains '*' (possibly combined with '=' exact-match suffix).
    if (pattern.find('*') != std::string::npos) {
        r.type = PermissionRuleType::kWildcard;
        return r;
    }

    // Exact-match rule: trailing '=' suffix strips to the command name.
    if (!pattern.empty() && pattern.back() == '=') {
        r.type = PermissionRuleType::kExact;
        r.command = pattern.substr(0, pattern.size() - 1);
        return r;
    }

    // Default: prefix rule.
    r.type = PermissionRuleType::kPrefix;
    r.prefix = pattern;
    return r;
}

/// Match a wildcard pattern containing '*' against a candidate string.
/// '*' matches any sequence of non-separator characters (does NOT cross
/// space boundaries).  Mirrors TS matchWildcardPattern.
[[nodiscard]] inline bool
match_wildcard_pattern(std::string_view pattern, std::string_view candidate) {
    // Simple DP-style matcher.  Since '*' can appear any number of times we
    // use two-pointer greedy matching (correct when '*' is the only metachar).
    size_t pi = 0, ci = 0, star_pi = std::string_view::npos, star_ci = 0;
    while (ci < candidate.size()) {
        if (pi < pattern.size() && pattern[pi] == '*') {
            star_pi = pi++;
            star_ci = ci;
            continue;
        }
        if (pi < pattern.size() &&
            (pattern[pi] == candidate[ci] ||
             pattern[pi] == '?' /* treat ? as single-char glob too */)) {
            ++pi; ++ci;
            continue;
        }
        if (star_pi != std::string_view::npos) {
            pi = star_pi + 1;
            ++star_ci;
            ci = star_ci;
            continue;
        }
        return false;
    }
    while (pi < pattern.size() && pattern[pi] == '*') ++pi;
    return pi == pattern.size();
}

/// Strip leading environment-variable assignments (FOO=bar BAZ=qux ...) from
/// the front of a command string.  Mirrors TS stripAllLeadingEnvVars.
/// Known binary-hijack variable names are treated as heuristic matches — any
/// name that looks like an uppercase NAME=value assignment is stripped.
[[nodiscard]] inline std::string
strip_all_leading_env_vars(std::string_view command) {
    // The TS uses BINARY_HIJACK_VARS as a known-set; in C++ we use a general
    // regex /^[A-Z_][A-Z0-9_]*=\S+\s*/ to match common env-var assignments.
    // This is a superset of TS behaviour.
    std::string s(command);
    static const std::regex env_assignment{
        R"(^[A-Z_][A-Za-z0-9_]*(?:_[A-Za-z0-9_]+)*=(\S+)(?:\s+|$))"};
    for (;;) {
        std::smatch m;
        if (!std::regex_search(s, m, env_assignment)) break;
        // Don't strip if the supposed "env var" looks like a flag (e.g. "-n=5").
        if (m[0].str().front() == '-') break;
        s = m.suffix().str();
    }
    return s;
}

/// Strip "safe wrapper" prefixes from a command string:
///   time, nohup, nice [-n N], timeout [-k SIG] DURATION, stdbuf, env
///
/// Mirrors TS stripSafeWrappers.  The TS text version is narrower than the
/// argv-level version in path_validation; we match the text version here
/// because we operate on raw subcommand strings.
[[nodiscard]] inline std::string
strip_safe_wrappers(std::string_view command) {
    std::string s(command);
    for (;;) {
        auto ws = s.find_first_not_of(" \t");
        if (ws == std::string::npos) return s;
        std::string_view v(s); v.remove_prefix(ws);
        if (v.starts_with("time "))     { s = std::string(v.substr(5)); continue; }
        if (v == "time")                { s.clear(); return s; }
        if (v.starts_with("nohup "))    { s = std::string(v.substr(6)); continue; }
        if (v == "nohup")               { s.clear(); return s; }
        // timeout [flags] DURATION
        if (v.starts_with("timeout") && (v.size() == 7 || v[7] == ' ')) {
            size_t i = 7;
            while (i < v.size() && v[i] == ' ') ++i;
            // Skip [-s SIG] [-k SIG] [-v] [--preserve-status] etc.
            static const std::regex flag_re{
                R"((--[a-z-]+(?:=[A-Za-z0-9_.+-]+)?|-[vks][A-Za-z0-9_.+-]*|--)\s*)"};
            std::string tail(v.substr(i));
            std::smatch m;
            while (!tail.empty() &&
                   std::regex_search(tail, m, flag_re) &&
                   m.position() == 0) {
                tail = m.suffix().str();
            }
            // Now tail should start with DURATION (digits, optional decimal, optional s/m/h/d)
            size_t d_end = 0;
            while (d_end < tail.size() && std::isdigit(static_cast<unsigned char>(tail[d_end]))) ++d_end;
            if (d_end > 0 && d_end < tail.size() && tail[d_end] == '.') {
                ++d_end;
                while (d_end < tail.size() && std::isdigit(static_cast<unsigned char>(tail[d_end]))) ++d_end;
            }
            if (d_end == 0) return s; // not a valid timeout call
            if (d_end < tail.size() &&
                (tail[d_end] == 's' || tail[d_end] == 'm' ||
                 tail[d_end] == 'h' || tail[d_end] == 'd')) {
                ++d_end;
            }
            s = tail.substr(d_end);
            continue;
        }
        // nice [-n N] / nice -N
        if (v.starts_with("nice") && (v.size() == 4 || v[4] == ' ')) {
            size_t i = 4;
            while (i < v.size() && v[i] == ' ') ++i;
            std::string_view rest = v.substr(i);
            if (rest.starts_with("-n")) {
                i += 2;
                while (i < v.size() && v[i] == ' ') ++i;
                // consume an integer (optional sign, digits)
                if (i < v.size() && (v[i] == '-' || v[i] == '+')) ++i;
                while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) ++i;
                s = std::string(v.substr(i));
                continue;
            }
            if (rest.size() >= 2 && rest[0] == '-' && std::isdigit(static_cast<unsigned char>(rest[1]))) {
                size_t j = 2;
                while (j < rest.size() && std::isdigit(static_cast<unsigned char>(rest[j]))) ++j;
                s = std::string(rest.substr(j));
                continue;
            }
            s = std::string(rest);
            continue;
        }
        break;
    }
    return s;
}

// ---------------------------------------------------------------------------
// Excluded-command detection (the "meat" of the decision)
// ---------------------------------------------------------------------------

/// Split a full command into its subcommand strings for excluded-command
/// checking.  Uses the same &&/||/;/|/newline splitting logic as the
/// mode_validation module (kept re-implemented locally so this module has
/// no extra imports beyond bash_security).
[[nodiscard]] inline std::vector<std::string>
split_subcommands_for_exclude(std::string_view input) {
    std::vector<std::string> result;
    std::string current;
    char quote = 0;
    bool escaped = false;

    auto flush = [&] {
        size_t b = current.find_first_not_of(" \t\n\r");
        if (b == std::string::npos) { current.clear(); return; }
        size_t e = current.find_last_not_of(" \t\n\r");
        result.push_back(current.substr(b, e - b + 1));
        current.clear();
    };

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (escaped) { current.push_back(c); escaped = false; continue; }
        if (!quote && c == '\\') { current.push_back(c); escaped = true; continue; }
        if (quote == 0) {
            if (c == '\'' || c == '"') { quote = c; current.push_back(c); continue; }
            if (c == '&' && i + 1 < input.size() && input[i + 1] == '&') { flush(); ++i; continue; }
            if (c == '|' && i + 1 < input.size() && input[i + 1] == '|') { flush(); ++i; continue; }
            if (c == '|' || c == ';' || c == '\n') { flush(); continue; }
            current.push_back(c);
        } else if (quote == '\'') {
            current.push_back(c);
            if (c == '\'') quote = 0;
        } else {
            current.push_back(c);
            if (c == '\\' && i + 1 < input.size()) { escaped = true; continue; }
            if (c == '"') quote = 0;
        }
    }
    flush();
    return result;
}

/// Build the set of candidate strings to match against excluded patterns
/// for a single subcommand.  Iteratively strips leading env vars and safe
/// wrappers until a fixed-point is reached (handles interleaved patterns
/// like `timeout 300 FOO=bar bazel run`).
[[nodiscard]] inline std::vector<std::string>
build_match_candidates(std::string_view trimmed_subcmd) {
    std::vector<std::string> candidates;
    std::unordered_set<std::string> seen;
    candidates.push_back(std::string(trimmed_subcmd));
    seen.insert(candidates.front());

    size_t start_idx = 0;
    while (start_idx < candidates.size()) {
        const size_t end_idx = candidates.size();
        for (size_t i = start_idx; i < end_idx; ++i) {
            const auto& cmd = candidates[i];
            auto env_stripped = strip_all_leading_env_vars(cmd);
            if (seen.insert(env_stripped).second) {
                candidates.push_back(std::move(env_stripped));
            }
            auto wrapper_stripped = strip_safe_wrappers(cmd);
            if (seen.insert(wrapper_stripped).second) {
                candidates.push_back(std::move(wrapper_stripped));
            }
        }
        start_idx = end_idx;
    }
    return candidates;
}

/// Trim a string_view's leading whitespace.
inline std::string_view trim_leading(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    return s;
}

/// Extract the base command (first whitespace-token) of a trimmed command.
inline std::string_view base_command_of(std::string_view trimmed) {
    auto pos = trimmed.find_first_of(" \t\n\r");
    if (pos == std::string_view::npos) return trimmed;
    return trimmed.substr(0, pos);
}

/// Returns true iff the command should NOT be sandboxed because it matches
/// a user-configured excluded-command pattern.
[[nodiscard]] inline bool
contains_excluded_command(
    std::string_view command,
    std::span<const std::string> user_excluded_patterns)
{
    if (user_excluded_patterns.empty()) return false;

    auto subcommands = split_subcommands_for_exclude(command);
    for (const auto& sub : subcommands) {
        std::string_view trimmed(sub);
        trimmed = trim_leading(trimmed);

        // Fixed-point: try all wrapper/env-stripped candidates.
        const auto candidates = build_match_candidates(trimmed);

        for (const auto& raw_pattern : user_excluded_patterns) {
            const auto rule = parse_permission_rule(raw_pattern);
            for (const auto& cand : candidates) {
                switch (rule.type) {
                    case PermissionRuleType::kPrefix:
                        if (cand == rule.prefix ||
                            cand.starts_with(rule.prefix + " ")) {
                            return true;
                        }
                        break;
                    case PermissionRuleType::kExact:
                        if (cand == rule.command) return true;
                        break;
                    case PermissionRuleType::kWildcard:
                        if (match_wildcard_pattern(rule.pattern, cand)) {
                            return true;
                        }
                        break;
                }
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------

/// Decide whether to sandbox a command.
///
/// @param input              Command + dangerously_disable_sandbox flag.
/// @param runtime            Snapshot of runtime sandbox config.
///
/// @returns true when the command MUST run inside the sandbox.
[[nodiscard]] inline bool
should_use_sandbox(
    const SandboxInput& input,
    const SandboxRuntimeConfig& runtime)
{
    if (!runtime.sandboxing_enabled) return false;

    if (input.dangerously_disable_sandbox &&
        runtime.unsandboxed_commands_allowed) {
        return false;
    }

    if (!input.command || input.command->empty()) return false;

    if (contains_excluded_command(*input.command,
                                   runtime.user_excluded_commands)) {
        return false;
    }

    return true;
}

/// Overload taking the same parameters as the TS default export — command
/// string + optional dangerously_disable_sandbox, with sensible runtime
/// defaults.  Useful for call sites that don't want to construct a full
/// SandboxRuntimeConfig.
[[nodiscard]] inline bool
should_use_sandbox(
    std::optional<std::string> command,
    bool dangerously_disable_sandbox = false)
{
    SandboxInput input{
        .command = std::move(command),
        .dangerously_disable_sandbox = dangerously_disable_sandbox,
    };
    return should_use_sandbox(input, SandboxRuntimeConfig{});
}

// ---------------------------------------------------------------------------
// TS-compatible camelCase aliases
// ---------------------------------------------------------------------------

[[nodiscard]] inline bool shouldUseSandbox(
    const SandboxInput& in,
    const SandboxRuntimeConfig& cfg) {
    return should_use_sandbox(in, cfg);
}

[[nodiscard]] inline bool shouldUseSandbox(
    std::optional<std::string> command,
    bool disable = false) {
    return should_use_sandbox(std::move(command), disable);
}

[[nodiscard]] inline bool containsExcludedCommand(
    std::string_view c,
    std::span<const std::string> p) {
    return contains_excluded_command(c, p);
}

inline BashPermissionRule bashPermissionRule(std::string_view p) {
    return parse_permission_rule(p);
}

inline bool matchWildcardPattern(std::string_view p, std::string_view c) {
    return match_wildcard_pattern(p, c);
}

} // namespace cc::tools::sandbox
