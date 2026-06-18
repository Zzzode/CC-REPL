// destructive_command_warning.cppm
// Detects potentially destructive bash commands (rm, git reset --hard, etc.)
// and returns a human-readable warning string for the permission dialog.
// This is purely informational — it does not affect permission logic or
// auto-approval behaviour. Ported from src/tools/BashTool/destructiveCommandWarning.ts
//
// Two-tier detection:
//   1. AST-based (preferred): uses tree-sitter bash parser when CC_HAS_TREE_SITTER=1
//      for precise structural detection with fewer false positives.
//   2. Regex fallback (always available): pattern-matching on the raw command
//      string. Used when tree-sitter is disabled or parsing fails.

module;

#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <regex>
#include <filesystem>

export module cc.tools.destructive_command_warning;

import cc.utils.tree_sitter.bash;

export namespace cc::tools::bash_validation {

/// Severity of a detected dangerous pattern.
/// Mirrors cc::utils::tree_sitter::bash::Severity for call-site convenience.
enum class DangerSeverity {
    None = 0,
    Low,
    Medium,
    High,
    Critical,
};

/// A single matchable destructive pattern paired with its warning message.
struct DestructivePattern {
    std::regex pattern;
    std::string warning;
};

/// Compiled static storage for all known destructive patterns.
///
/// NOTE: These are std::regex because the TS source uses JS RegExp with word
/// boundaries, lookaheads, alternations and flags (icase) that cannot be
/// expressed reliably with plain string find() calls.
///
/// Patterns are ordered by sensitivity: git data-loss ops first, then git
/// safety-bypass, then file deletion, then database, then infrastructure.
inline const std::vector<DestructivePattern>& get_destructive_patterns() {
    static const std::vector<DestructivePattern> patterns = [] {
        using re = std::regex;
        auto ic = std::regex::icase;
        auto ec = std::regex_constants::ECMAScript;

        std::vector<DestructivePattern> v;
        v.reserve(20);

        // ------------------------------------------------------------------
        // Git — data loss / hard to reverse
        // ------------------------------------------------------------------

        // git reset --hard
        v.push_back(DestructivePattern{
            re{R"(\bgit\s+reset\s+--hard\b)"},
            "Note: may discard uncommitted changes"
        });

        // git push --force / -f / --force-with-lease
        v.push_back(DestructivePattern{
            re{R"(\bgit\s+push\b[^;&|\n]*[ \t](--force|--force-with-lease|-f)\b)"},
            "Note: may overwrite remote history"
        });

        // git clean -fd / -f (but not dry-run variants)
        v.push_back(DestructivePattern{
            re{R"(\bgit\s+clean\b(?![^;&|\n]*(?:-[a-zA-Z]*n|--dry-run))[^;&|\n]*-[a-zA-Z]*f)"},
            "Note: may permanently delete untracked files"
        });

        // git checkout -- .  (discard all working tree changes)
        v.push_back(DestructivePattern{
            re{R"(\bgit\s+checkout\s+(--\s+)?\.[ \t]*($|[;&|\n]))"},
            "Note: may discard all working tree changes"
        });

        // git restore -- .  (discard all working tree changes)
        v.push_back(DestructivePattern{
            re{R"(\bgit\s+restore\s+(--\s+)?\.[ \t]*($|[;&|\n]))"},
            "Note: may discard all working tree changes"
        });

        // git stash drop / clear
        v.push_back(DestructivePattern{
            re{R"(\bgit\s+stash[ \t]+(drop|clear)\b)"},
            "Note: may permanently remove stashed changes"
        });

        // git branch -D / --delete --force
        v.push_back(DestructivePattern{
            re{R"(\bgit\s+branch\s+(-D[ \t]|--delete\s+--force|--force\s+--delete)\b)"},
            "Note: may force-delete a branch"
        });

        // ------------------------------------------------------------------
        // Git — safety bypass
        // ------------------------------------------------------------------

        // git (commit|push|merge) --no-verify
        v.push_back(DestructivePattern{
            re{R"(\bgit\s+(commit|push|merge)\b[^;&|\n]*--no-verify\b)"},
            "Note: may skip safety hooks"
        });

        // git commit --amend
        v.push_back(DestructivePattern{
            re{R"(\bgit\s+commit\b[^;&|\n]*--amend\b)"},
            "Note: may rewrite the last commit"
        });

        // ------------------------------------------------------------------
        // File deletion
        // Dangerous-path guarding (e.g. rm -rf /) is handled separately by
        // checkDangerousRemovalPaths() in path_validation; this section covers
        // the rm flag variants for the UI warning only.
        // ------------------------------------------------------------------

        // rm -rf / -rRf / -fRr  (combined recursive + force)
        v.push_back(DestructivePattern{
            re{R"((^|[;&|\n]\s*)rm\s+-[a-zA-Z]*[rR][a-zA-Z]*f|(^|[;&|\n]\s*)rm\s+-[a-zA-Z]*f[a-zA-Z]*[rR])"},
            "Note: may recursively force-remove files"
        });

        // rm -r / -R  (recursive only)
        v.push_back(DestructivePattern{
            re{R"((^|[;&|\n]\s*)rm\s+-[a-zA-Z]*[rR])"},
            "Note: may recursively remove files"
        });

        // rm -f  (force only)
        v.push_back(DestructivePattern{
            re{R"((^|[;&|\n]\s*)rm\s+-[a-zA-Z]*f)"},
            "Note: may force-remove files"
        });

        // ------------------------------------------------------------------
        // Database
        // ------------------------------------------------------------------

        // DROP / TRUNCATE (TABLE|DATABASE|SCHEMA) — case-insensitive
        v.push_back(DestructivePattern{
            re{R"(\b(DROP|TRUNCATE)\s+(TABLE|DATABASE|SCHEMA)\b)", ic},
            "Note: may drop or truncate database objects"
        });

        // DELETE FROM table (no WHERE clause risk)
        v.push_back(DestructivePattern{
            re{R"(\bDELETE\s+FROM\s+\w+[ \t]*(;|"|'|\n|$))", ic},
            "Note: may delete all rows from a database table"
        });

        // ------------------------------------------------------------------
        // Infrastructure
        // ------------------------------------------------------------------

        // kubectl delete
        v.push_back(DestructivePattern{
            re{R"(\bkubectl\s+delete\b)"},
            "Note: may delete Kubernetes resources"
        });

        // terraform destroy
        v.push_back(DestructivePattern{
            re{R"(\bterraform\s+destroy\b)"},
            "Note: may destroy Terraform infrastructure"
        });

        return v;
    }();
    return patterns;
}

/// Detailed result of a dangerous-command classification.
struct DangerClassification {
    bool is_dangerous = false;
    DangerSeverity severity = DangerSeverity::None;
    std::vector<std::string> matched_patterns;
    std::string summary;
    bool used_ast = false;     // true if tree-sitter AST detection was used
    bool parse_error = false;  // true if AST parsing failed (regex fallback used)
};

/// Classify a bash command's dangerousness with full detail.
///
/// When CC_HAS_TREE_SITTER=1 and the command parses cleanly, AST-based
/// detection is used first for higher precision.  If parsing fails or
/// tree-sitter is disabled, regex-based detection is used as a fallback.
[[nodiscard]] inline auto classify_dangerous_command(std::string_view command)
    -> DangerClassification {
    DangerClassification out;

#if CC_HAS_TREE_SITTER
    // Attempt AST-based classification for higher precision.
    auto ast_result = cc::utils::tree_sitter::bash::classify_dangerous(
        std::filesystem::path{}, command);
    if (!ast_result.parse_error) {
        out.used_ast = true;
        out.parse_error = false;
        if (ast_result.is_dangerous) {
            out.is_dangerous = true;
            switch (ast_result.severity) {
                case cc::utils::tree_sitter::bash::Severity::Critical:
                    out.severity = DangerSeverity::Critical; break;
                case cc::utils::tree_sitter::bash::Severity::High:
                    out.severity = DangerSeverity::High; break;
                case cc::utils::tree_sitter::bash::Severity::Medium:
                    out.severity = DangerSeverity::Medium; break;
                case cc::utils::tree_sitter::bash::Severity::Low:
                    out.severity = DangerSeverity::Low; break;
                default:
                    out.severity = DangerSeverity::None; break;
            }
            out.matched_patterns = ast_result.matched_patterns;
            out.summary = ast_result.summary;
            return out;
        }
        // AST found nothing dangerous — still run regex as a safety net
        // for patterns not yet covered by the query catalogue.
    } else {
        out.parse_error = true;
    }
#else
    out.parse_error = true;  // tree-sitter disabled → treat as "parse error"
#endif

    // Regex fallback: iterate patterns in priority order.
    const auto& patterns = get_destructive_patterns();
    const std::string cmd(command);
    for (const auto& entry : patterns) {
        if (std::regex_search(cmd, entry.pattern)) {
            out.is_dangerous = true;
            // Map regex warnings to a default severity.
            if (out.severity < DangerSeverity::Medium) {
                out.severity = DangerSeverity::Medium;
            }
            out.matched_patterns.push_back(entry.warning);
            if (out.summary.empty()) {
                out.summary = entry.warning;
            }
            // Keep going to collect all matching patterns, but cap the list.
            if (out.matched_patterns.size() >= 5) break;
        }
    }

    return out;
}

/// Checks if a bash command matches known destructive patterns.
/// Returns a human-readable warning string, or std::nullopt if no destructive
/// pattern is detected.
///
/// When CC_HAS_TREE_SITTER=1, AST-based detection is attempted first for
/// higher precision; regex patterns are used as a fallback.
///
/// @param command The raw command string (may be a compound command with
///                &&, |, ;, etc.).
/// @returns Warning string or std::nullopt.
[[nodiscard]] inline std::optional<std::string>
get_destructive_command_warning(std::string_view command) {
    auto cls = classify_dangerous_command(command);
    if (!cls.is_dangerous) return std::nullopt;
    return cls.summary;
}

/// Convenience alias: mirrors the TS getDestructiveCommandWarning() name for
/// call sites that have not been renamed yet.
[[nodiscard]] inline std::optional<std::string>
getDestructiveCommandWarning(std::string_view command) {
    return get_destructive_command_warning(command);
}

/// Boolean fast-path: returns true when get_destructive_command_warning()
/// would produce a message. Avoids allocating the warning string.
[[nodiscard]] inline bool is_destructive_command(std::string_view command) {
    return get_destructive_command_warning(command).has_value();
}

} // namespace cc::tools::bash_validation
