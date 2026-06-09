// destructive_command_warning.cppm
// Detects potentially destructive bash commands (rm, git reset --hard, etc.)
// and returns a human-readable warning string for the permission dialog.
// This is purely informational — it does not affect permission logic or
// auto-approval behaviour. Ported from src/tools/BashTool/destructiveCommandWarning.ts

module;

#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <regex>

export module cc.tools.destructive_command_warning;

export namespace cc::tools::bash_validation {

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

/// Checks if a bash command matches known destructive patterns.
/// Returns a human-readable warning string, or std::nullopt if no destructive
/// pattern is detected.
///
/// The first matching pattern wins. Patterns are evaluated in priority order
/// (see get_destructive_patterns()).
///
/// @param command The raw command string (may be a compound command with
///                &&, |, ;, etc.).
/// @returns Warning string or std::nullopt.
[[nodiscard]] inline std::optional<std::string>
get_destructive_command_warning(std::string_view command) {
    const auto& patterns = get_destructive_patterns();
    const std::string cmd(command);
    for (const auto& entry : patterns) {
        if (std::regex_search(cmd, entry.pattern)) {
            return entry.warning;
        }
    }
    return std::nullopt;
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
