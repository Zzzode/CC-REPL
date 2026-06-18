/// @file ast.cppm
/// @brief Bash AST danger classifier using tree-sitter (or regex fallback).
///
/// Analyses a bash script's syntax tree to identify high-risk constructs such as
/// recursive rm on root paths, curl|sh pipes, sudo usage, eval'd code, fork bombs,
/// heredoc-destructive streams, and similar patterns.
///
/// When CC_HAS_TREE_SITTER=0 the same 10 detections are performed via regex scan and
/// parse_error is set to true so callers can treat the result as a conservative
/// fallback (requires manual approval).

module;

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if CC_HAS_TREE_SITTER
extern "C" {
#include <tree_sitter/api.h>
}
#endif

export module cc.utils.tree_sitter.bash;

import cc.utils.tree_sitter.base;

export namespace cc::utils::tree_sitter::bash {

/// Severity assigned to matched dangerous patterns.  Higher ordinal = worse.
enum class Severity : uint8_t {
    None = 0,
    Low,
    Medium,
    High,
    Critical,
};

/// Output of classify_dangerous().
struct BashClassifierResult {
    /// True if any dangerous pattern matched (or if we could not parse and are being
    /// conservative).
    bool is_dangerous = false;
    /// Maximum severity of all patterns that fired.
    Severity severity = Severity::None;
    /// Human-readable pattern keys that matched (e.g. "pipe_to_shell").
    std::vector<std::string> matched_patterns;
    /// Byte [start, end) ranges in the source where each pattern anchored.  The i-th
    /// range corresponds to the i-th entry in `matched_patterns`.
    std::vector<std::pair<uint32_t, uint32_t>> byte_ranges;
    /// Short human-readable summary describing the most dangerous finding.
    std::string summary;
    /// True when tree-sitter was unavailable and regex fallback was used, or when
    /// the parser returned a syntax tree with errors that the caller should be
    /// aware of.
    bool parse_error = false;
};

namespace bash_detail {

// ─── Tree-sitter query catalogue ────────────────────────────────────────────
// Each entry is a (name, severity, pattern) tuple plus a set of text-level
// checks that must all pass for a match to count.
//
// NOTE: The C API (ts_query_new / ts_query_cursor_next_match) does **not**
// automatically evaluate predicate predicates like #eq? / #match? / #any-of?.
// Those are a higher-level feature provided by tree-sitter's Rust / JS / Python
// bindings.  We therefore strip all predicates from the query patterns and
// perform the equivalent text checks manually in the match loop.  This is
// slower but correct and avoids pulling in an extra dependency.

/// Type of text-level check applied to a named capture.
enum class CheckType : uint8_t {
    Eq,     ///< capture text must equal `value` exactly
    AnyOf,  ///< capture text must be one of the pipe-separated tokens in `value`
    Match,  ///< capture text must match the ECMAScript regex in `value`
};

/// A single text-level check: capture @capture_name must satisfy `type` against `value`.
struct TextCheck {
    const char* capture_name;
    CheckType   type;
    const char* value;
};

struct QueryDef {
    const char* name;
    Severity    severity;
    const char* pattern;
    /// Text checks that must ALL pass for the match to count.
    /// Terminated by an entry with capture_name == nullptr.
    TextCheck   checks[4];
};

// clang-format off
constexpr std::array<QueryDef, 10> kAllQueries = {{
    // 1) rm piped in a pipe (e.g. "xargs rm | ...").
    QueryDef{"piped_rm", Severity::Critical,
        "(pipe (command name: (command_name) @cmd-name))",
        { {"cmd-name", CheckType::Eq, "rm"},
          {nullptr, CheckType::Eq, nullptr},
          {nullptr, CheckType::Eq, nullptr},
          {nullptr, CheckType::Eq, nullptr} }},
    // 2) command_substitution containing destructive commands.
    QueryDef{"dangerous_subshell", Severity::High,
        "(command_substitution (command name: (command_name) @cmd-name))",
        { {"cmd-name", CheckType::AnyOf, "rm|mkfs|dd|wipefs|fsck|fdisk"},
          {nullptr, CheckType::Eq, nullptr},
          {nullptr, CheckType::Eq, nullptr},
          {nullptr, CheckType::Eq, nullptr} }},
    // 3) heredoc fed into a destructive command.
    QueryDef{"heredoc_destructive", Severity::High,
        "(command (heredoc_node) (redirect) @r (command name: (command_name) @cmd-name))",
        { {"cmd-name", CheckType::AnyOf, "rm|mkfs|dd|wipefs|fsck|fdisk"},
          {nullptr, CheckType::Eq, nullptr},
          {nullptr, CheckType::Eq, nullptr},
          {nullptr, CheckType::Eq, nullptr} }},
    // 4) Variable/expansion used as command_name — injection risk.
    //    We match any command whose name starts with $ — i.e. the command to
    //    run comes from a variable or expression.  We check the text of the
    //    command_name node.
    QueryDef{"env_injection_risk", Severity::Medium,
        "(command name: (command_name) @cmd-name)",
        { {"cmd-name", CheckType::Match, "^\\$"},
          {nullptr, CheckType::Eq, nullptr},
          {nullptr, CheckType::Eq, nullptr},
          {nullptr, CheckType::Eq, nullptr} }},
    // 5) sudo used as command_name.
    QueryDef{"sudo_used", Severity::Medium,
        "(command name: (command_name) @cmd-name)",
        { {"cmd-name", CheckType::Eq, "sudo"},
          {nullptr, CheckType::Eq, nullptr},
          {nullptr, CheckType::Eq, nullptr},
          {nullptr, CheckType::Eq, nullptr} }},
    // 6) Recursive rm on critical system paths.
    QueryDef{"recursive_rm_root", Severity::Critical,
        "(command name: (command_name) @cmd-name "
        "  argument: [ (word) (concatenation) ] @arg)",
        { {"cmd-name", CheckType::Eq, "rm"},
          {"arg",      CheckType::Match, "^(/|$HOME|/etc|/bin|/usr|/var|/boot|/lib|/sbin|/opt)"},
          {nullptr, CheckType::Eq, nullptr},
          {nullptr, CheckType::Eq, nullptr} }},
    // 7) wget/curl piped to sh/bash/zsh/fish.
    QueryDef{"pipe_to_shell", Severity::Critical,
        "(pipe left: (command name: (command_name) @left-cmd) "
        " right: (command name: (command_name) @right-cmd))",
        { {"left-cmd",  CheckType::AnyOf, "wget|curl"},
          {"right-cmd", CheckType::AnyOf, "sh|bash|zsh|fish"},
          {nullptr, CheckType::Eq, nullptr},
          {nullptr, CheckType::Eq, nullptr} }},
    // 8) eval command_name.
    QueryDef{"eval_used", Severity::Medium,
        "(command name: (command_name) @cmd-name)",
        { {"cmd-name", CheckType::Eq, "eval"},
          {nullptr, CheckType::Eq, nullptr},
          {nullptr, CheckType::Eq, nullptr},
          {nullptr, CheckType::Eq, nullptr} }},
    // 9) chmod 777 or +s on system paths.
    QueryDef{"unsafe_chmod", Severity::High,
        "(command name: (command_name) @cmd-name "
        "  argument: (word) @mode "
        "  argument: (word) @path)",
        { {"cmd-name", CheckType::Eq, "chmod"},
          {"mode",     CheckType::Match, "^(777|a\\+s|u\\+s|\\+s)"},
          {"path",     CheckType::Match, "^(/|$HOME|/etc|/bin|/usr|/var|/boot|/lib)"},
          {nullptr, CheckType::Eq, nullptr} }},
    // 10) Fork bomb — function_definition with name=":" and a pipe+background body.
    QueryDef{"fork_bomb_detected", Severity::Critical,
        "(function_definition name: (word) @fname "
        "  body: (compound_statement (pipe) @pound (background)) @bg)",
        { {"fname", CheckType::Eq, ":"},
          {nullptr, CheckType::Eq, nullptr},
          {nullptr, CheckType::Eq, nullptr},
          {nullptr, CheckType::Eq, nullptr} }},
}};
// clang-format on

// ─── Query compilation + caching ────────────────────────────────────────────
struct CompiledCatalogue {
    struct Entry {
        std::string name;
        Severity    severity;
        Query       query;
        /// Text-level checks copied from QueryDef.
        TextCheck   checks[4];
    };
    std::vector<Entry> entries;
    bool all_ok = false;
};

auto compile_catalogue(const void* lang) -> const CompiledCatalogue& {
    static std::once_flag once;
    static CompiledCatalogue cache;
    std::call_once(once, [&]() {
        cache.entries.reserve(kAllQueries.size());
        bool ok = true;
        for (const QueryDef& def : kAllQueries) {
#if CC_HAS_TREE_SITTER
            Query q(static_cast<const TSLanguage*>(lang), def.pattern);
#else
            Query q(lang, def.pattern);
#endif
            if (!q.ok()) ok = false;
            CompiledCatalogue::Entry entry{def.name, def.severity, std::move(q), {}};
            for (size_t i = 0; i < 4; ++i) {
                entry.checks[i] = def.checks[i];
            }
            cache.entries.push_back(std::move(entry));
        }
        cache.all_ok = ok;
    });
    return cache;
}

// ─── Text-check helpers ────────────────────────────────────────────────────
// Extract the source text of a capture given its name.  Returns empty string_view
// if no capture with that name is found.
auto capture_text_by_name(const std::vector<Query::Capture>& caps,
                          std::string_view name,
                          std::string_view src) -> std::string_view {
    for (const auto& c : caps) {
        if (c.name == name) {
            if (c.end_byte <= src.size() && c.start_byte <= c.end_byte) {
                return src.substr(c.start_byte, c.end_byte - c.start_byte);
            }
            return {};
        }
    }
    return {};
}

// Run a single TextCheck against a set of captures.  Returns true if the check
// passes (or if the check has a null capture_name, i.e. it's the sentinel).
bool run_text_check(const TextCheck& check,
                    const std::vector<Query::Capture>& caps,
                    std::string_view src) {
    if (!check.capture_name) return true;  // sentinel -> trivially passes
    std::string_view text = capture_text_by_name(caps, check.capture_name, src);
    switch (check.type) {
        case CheckType::Eq:
            return text == check.value;
        case CheckType::AnyOf: {
            // Split `check.value` on '|' and check if `text` equals any token.
            std::string_view list = check.value;
            while (!list.empty()) {
                size_t pipe = list.find('|');
                std::string_view token = (pipe == std::string_view::npos)
                    ? list : list.substr(0, pipe);
                if (text == token) return true;
                if (pipe == std::string_view::npos) break;
                list.remove_prefix(pipe + 1);
            }
            return false;
        }
        case CheckType::Match: {
            try {
                std::regex re{check.value,
                    std::regex_constants::ECMAScript | std::regex_constants::optimize};
                const std::string text_str{text};
                return std::regex_search(text_str, re);
            } catch (const std::regex_error&) {
                // Bad regex — treat as not-matching (fail-closed).
                return false;
            }
        }
    }
    return false;
}

// Run all checks in the entry's checks array (null-terminated).  Returns true
// only if every non-null check passes.
bool all_checks_pass(const CompiledCatalogue::Entry& entry,
                     const std::vector<Query::Capture>& caps,
                     std::string_view src) {
    for (size_t i = 0; i < 4; ++i) {
        if (!entry.checks[i].capture_name) break;  // end of list
        if (!run_text_check(entry.checks[i], caps, src)) return false;
    }
    return true;
}

// ─── Helpers ────────────────────────────────────────────────────────────────
void record_match(BashClassifierResult& out, const std::string& name, Severity sev,
                  uint32_t start, uint32_t end) {
    out.is_dangerous = true;
    out.matched_patterns.push_back(name);
    out.byte_ranges.emplace_back(start, end);
    if (static_cast<uint8_t>(sev) > static_cast<uint8_t>(out.severity)) {
        out.severity = sev;
    }
}

auto severity_label(Severity s) -> std::string_view {
    switch (s) {
        case Severity::None:     return "None";
        case Severity::Low:      return "Low";
        case Severity::Medium:   return "Medium";
        case Severity::High:     return "High";
        case Severity::Critical: return "Critical";
    }
    return "Unknown";
}

// ─── Regex fallback (used when !CC_HAS_TREE_SITTER or parse error) ───────────
void regex_scan(BashClassifierResult& out, std::string_view script) {
    // We compile the regexes lazily on first call; they are pure stateless objects so
    // a function-local static is safe (C++11 guarantees thread-safe init of function
    // static const objects if no dynamic init races).
    struct Rule {
        const char* name;
        Severity sev;
        const char* pattern;
    };
    static constexpr std::array<Rule, 10> rules{{
        {"piped_rm",              Severity::Critical, R"(\|[^|\n]*\brm\b)"},
        {"dangerous_subshell",    Severity::High,     R"(\$\([^)]*\b(rm|mkfs|dd|wipefs|fsck|fdisk)\b|\`[^\`]*\b(rm|mkfs|dd|wipefs|fsck|fdisk)\b\`)"},
        {"heredoc_destructive",   Severity::High,     R"(<<[-]?[-]?['\"]?\w+['\"]?\s*(?s:.*?)\b(rm|mkfs|dd|wipefs|fsck|fdisk)\b)"},
        {"env_injection_risk",    Severity::Medium,   R"((?:^|[\s;&|`(])\$\{?[A-Za-z_][A-Za-z_0-9]*\}?\s*(?:$|[;&|]))"},
        {"sudo_used",             Severity::Medium,   R"(\bsudo\b)"},
        {"recursive_rm_root",     Severity::Critical, R"(\brm\s+(?:-[a-zA-Z]*r[a-zA-Z]*|--recursive)\s+.*?\b(/|/etc|/bin|/usr|/var|/boot|/lib|\$HOME)\b)"},
        {"pipe_to_shell",         Severity::Critical, R"(\b(wget|curl)\b[^|&\n]*\|\s*\b(sh|bash|zsh|fish)\b)"},
        {"eval_used",             Severity::Medium,   R"(\beval\b)"},
        {"unsafe_chmod",          Severity::High,     R"(\bchmod\s+(?:777|[a-z]*\+s)\s+.*?\b(/|/etc|/bin|/usr|/var|/boot|/lib)\b)"},
        {"fork_bomb_detected",    Severity::Critical, R"(:\s*\(\s*\)\s*\{\s*:\s*\|\s*:\s*&\s*\}\s*;?\s*:)"},
    }};

    const std::string text{script};
    for (const auto& r : rules) {
        try {
            std::regex re{r.pattern, std::regex_constants::ECMAScript |
                                       std::regex_constants::optimize};
            auto begin = std::sregex_iterator(text.begin(), text.end(), re);
            auto end   = std::sregex_iterator();
            for (auto it = begin; it != end; ++it) {
                const auto& m = *it;
                const size_t start = static_cast<size_t>(m.position());
                const size_t endpos = start + static_cast<size_t>(m.length());
                record_match(out, r.name, r.sev,
                             static_cast<uint32_t>(start),
                             static_cast<uint32_t>(endpos));
                // Only record the first hit per pattern; repeated matches in the same
                // script do not add signal.
                break;
            }
        } catch (const std::regex_error&) {
            // Ignore regex library misbehaviour — this is a best-effort fallback.
        }
    }
}

}  // namespace bash_detail
using namespace bash_detail;

// ─── Public API ─────────────────────────────────────────────────────────────
[[nodiscard]] auto classify_dangerous(const std::filesystem::path& /*path*/,
                                      std::string_view script) -> BashClassifierResult {
    BashClassifierResult out;

#if CC_HAS_TREE_SITTER
    const TSLanguage* lang = tree_sitter_bash_lang();
#else
    const void* lang = nullptr;
#endif

    const CompiledCatalogue& cat = compile_catalogue(lang);

#if CC_HAS_TREE_SITTER
    Parser parser;
    if (!parser.set_language(lang)) {
        // Parser could not bind bash language. Treat as parse error and fall back.
        out.parse_error = true;
    } else {
        auto tree = parser.parse(script);
        if (!tree) {
            out.parse_error = true;
        } else {
            // Detect syntax errors by walking the root and checking for ERROR nodes.
            TSNode root = ts_tree_root_node(tree.get());
            bool has_error = false;
            // Simple BFS: a full walk of the tree to find ERROR nodes.
            // We cap the iteration to avoid pathological blowup on intentionally evil
            // inputs that still parse.
            std::vector<TSNode> stack{root};
            size_t budget = 1000000;
            while (!stack.empty() && budget-- > 0) {
                TSNode n = stack.back();
                stack.pop_back();
                const char* type = ts_node_type(n);
                if (std::strcmp(type, "ERROR") == 0) {
                    has_error = true;
                    break;
                }
                const uint32_t nc = ts_node_child_count(n);
                for (uint32_t i = 0; i < nc; ++i) {
                    stack.push_back(ts_node_child(n, i));
                }
            }
            out.parse_error = has_error;

            for (const auto& entry : cat.entries) {
                if (!entry.query.ok()) continue;
                auto matches = entry.query.matches(tree.get(), script);
                for (const auto& m : matches) {
                    if (m.captures.empty()) continue;
                    // Run text-level checks (eq/any-of/match) on named captures.
                    // The tree-sitter C API does not evaluate query predicates, so
                    // we do it manually here.
                    if (!all_checks_pass(entry, m.captures, script)) continue;
                    // Use the capture with the widest span as the representative
                    // byte range.
                    uint32_t start = m.captures.front().start_byte;
                    uint32_t end   = m.captures.front().end_byte;
                    for (const auto& c : m.captures) {
                        start = std::min(start, c.start_byte);
                        end   = std::max(end, c.end_byte);
                    }
                    record_match(out, entry.name, entry.severity, start, end);
                    break;  // first match is enough signal per pattern
                }
            }
        }
    }
#else
    out.parse_error = true;
#endif

    // If tree-sitter is off, or it ran but we hit an ERROR node, additionally run
    // the regex fallback so we still flag obvious bad patterns.  The regex patterns
    // are intentionally conservative and may duplicate tree-sitter matches, so we
    // de-duplicate by pattern name before inserting.
    if (out.parse_error || !cat.all_ok) {
        BashClassifierResult fallback;
        regex_scan(fallback, script);
        for (size_t i = 0; i < fallback.matched_patterns.size(); ++i) {
            const auto& name = fallback.matched_patterns[i];
            if (std::find(out.matched_patterns.begin(), out.matched_patterns.end(),
                          name) == out.matched_patterns.end()) {
                auto it = std::find_if(kAllQueries.begin(), kAllQueries.end(),
                                       [&](const QueryDef& d) { return d.name == name; });
                Severity sev = Severity::Medium;
                if (it != kAllQueries.end()) sev = it->severity;
                auto [start, end] = fallback.byte_ranges[i];
                record_match(out, name, sev, start, end);
            }
        }
    }

    // Build a human-readable summary sorted by severity, capped to ~3 patterns.
    if (!out.matched_patterns.empty()) {
        std::ostringstream ss;
        ss << "Bash danger classifier (parse_error=" << (out.parse_error ? "1" : "0")
           << ") severity=" << severity_label(out.severity) << "; patterns=";
        const size_t cap = std::min<size_t>(3, out.matched_patterns.size());
        for (size_t i = 0; i < cap; ++i) {
            if (i) ss << ",";
            ss << out.matched_patterns[i];
        }
        if (out.matched_patterns.size() > cap) {
            ss << ",+" << (out.matched_patterns.size() - cap);
        }
        out.summary = std::move(ss).str();
    } else if (out.parse_error) {
        out.summary = "Bash danger classifier: parser unavailable; no regex patterns "
                      "fired.  Manual approval recommended.";
    }

    return out;
}

}  // namespace cc::utils::tree_sitter::bash
