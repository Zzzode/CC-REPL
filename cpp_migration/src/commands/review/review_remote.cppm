/// @file review_remote.cppm
/// @brief ReviewRemoteCommand implementing the /review remote slash command.
/// Parses GitHub PR URLs, fetches the PR diff (via gh CLI or GitHub API fallback),
/// and injects a structured review prompt into the LLM query loop via query_engine.
///
/// LLM calls are NOT made directly here — the command injects a prompt, and the
/// query_engine public entry point handles the actual API call + tool loop.
module;

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <ranges>
#include <algorithm>
#include <span>
#include <array>
#include <regex>
#include <sstream>
#include <utility>

export module cc.commands.review.review_remote;

import cc.types.types;
import cc.commands.command;
import cc.utils.exec_sync;
import cc.utils.shell;
import cc.utils.find_executable;
import cc.utils.detect_repository;
import cc.services.analytics.growthbook;

export namespace cc::commands {

using namespace cc::core;

// ============================================================
// Data types
// ============================================================

/// A parsed GitHub PR identifier
struct ParsedPR {
    std::string owner;
    std::string repo;
    std::uint64_t number{0};
    std::string raw_url;  ///< Original URL or number string
};

/// A single structured review comment returned from the LLM (parsed post-hoc)
struct ReviewComment {
    std::string file;
    std::uint32_t line{0};
    std::string severity;  ///< critical | high | medium | low | info
    std::string message;
    std::string suggested_fix;
};

// ============================================================
// PR URL parsing
// ============================================================

/// Parse a GitHub PR URL or bare PR number into a ParsedPR.
/// Accepts:
///   - "https://github.com/OWNER/REPO/pull/N"
///   - "github.com/OWNER/REPO/pull/N"
///   - "OWNER/REPO#N"
///   - "<digits>"  (PR number, requires repo auto-detect)
/// Returns nullopt on parse failure.
[[nodiscard]] inline std::optional<ParsedPR> parse_pr_input(std::string_view input) {
    auto trimmed = std::string(input);
    // trim whitespace
    auto f = trimmed.find_first_not_of(" \t\n\r");
    if (f == std::string::npos) return std::nullopt;
    auto l = trimmed.find_last_not_of(" \t\n\r");
    trimmed = trimmed.substr(f, l - f + 1);

    ParsedPR result;
    result.raw_url = trimmed;

    // 1) Bare number: "123"
    if (std::ranges::all_of(trimmed, [](unsigned char c) { return std::isdigit(c); })) {
        // Try to detect the current repo context
        if (auto repo_root = cc::utils::detect_repo_root()) {
            const auto name = repo_root->filename().string();
            const auto parent = repo_root->parent_path().filename().string();
            result.owner = parent.empty() ? "local" : parent;
            result.repo = name;
            try { result.number = std::stoull(trimmed); } catch (...) { return std::nullopt; }
            return result;
        }
        return std::nullopt;
    }

    // 2) OWNER/REPO#N
    {
        std::regex re(R"(^([A-Za-z0-9_.-]+)/([A-Za-z0-9_.-]+)#(\d+)$)");
        std::smatch m;
        if (std::regex_match(trimmed, m, re)) {
            result.owner = m[1];
            result.repo = m[2];
            try { result.number = std::stoull(m[3].str()); } catch (...) { return std::nullopt; }
            return result;
        }
    }

    // 3) GitHub URL (with or without protocol)
    {
        std::regex re(R"((?:https?://)?github\.com/([A-Za-z0-9_.-]+)/([A-Za-z0-9_.-]+)/pull/(\d+))");
        std::smatch m;
        if (std::regex_search(trimmed, m, re)) {
            result.owner = m[1];
            result.repo = m[2];
            try { result.number = std::stoull(m[3].str()); } catch (...) { return std::nullopt; }
            return result;
        }
    }

    return std::nullopt;
}

// ============================================================
// Diff fetching (gh CLI first, fallback to GitHub raw diff)
// ============================================================

/// Fetch the diff content for a given PR.
/// Primary: `gh pr diff <number> --repo owner/repo` (requires GitHub CLI).
/// Fallback: `https://patch-diff.githubusercontent.com/raw/owner/repo/pull/N.diff`.
/// Returns the unified diff string or an error.
[[nodiscard]] inline Result<std::string> fetch_pr_diff(const ParsedPR& pr) {
    // --- Primary: gh CLI ---
    if (cc::utils::find_executable("gh")) {
        std::string cmd = std::format(
            "gh pr diff {} --repo {}/{}",
            pr.number, pr.owner, pr.repo
        );
        auto out = cc::utils::exec_sync(cmd);
        if (out && !out->empty()) {
            return *out;
        }
    }

    // --- Fallback: GitHub raw diff URL via curl ---
    auto fallback_url = std::format(
        "https://patch-diff.githubusercontent.com/raw/{}/{}/pull/{}.diff",
        pr.owner, pr.repo, pr.number
    );
    auto curl = cc::utils::exec_sync(std::format(
        "curl -sSL --max-time 30 {}", fallback_url
    ));
    if (curl && !curl->empty() && !curl->starts_with("Not Found")) {
        return *curl;
    }

    return std::unexpected(Error::make(
        ErrorCode::ToolExecutionFailed,
        std::format(
            "Cannot fetch diff for PR #{} in {}/{}:\n"
            "  - GitHub CLI (gh) not installed or request failed\n"
            "  - GitHub raw diff endpoint also failed.\n"
            "Suggestion: install `gh` with `brew install gh` and run `gh auth login`.",
            pr.number, pr.owner, pr.repo
        )
    ));
}

// ============================================================
// Review prompt builder (injected into query_engine)
// ============================================================

/// Build the review prompt sent to the LLM. The prompt instructs the model
/// to produce structured JSON/Markdown review comments for downstream parsing.
[[nodiscard]] inline std::string build_review_prompt(
    const ParsedPR& pr,
    const std::string& diff_content
) {
    std::ostringstream out;
    out << "## Remote Code Review: PR #" << pr.number << " in " << pr.owner << "/" << pr.repo << "\n\n"
        << "You are conducting a code review of the following GitHub pull request.\n\n"
        << "---\n\n"
        << "### Diff Content\n\n```diff\n" << diff_content << "\n```\n\n"
        << "---\n\n"
        << R"(### Review Guidelines

1. **Accuracy over volume**: Only flag issues where you are >80% confident of a real problem.
2. **Severity levels**: `critical`, `high`, `medium`, `low`, `info`.
3. **Categories**: security, correctness, performance, readability, maintainability, concurrency, compatibility.
4. **Structure**: For each finding produce a section with:
   - File and line number (reference the diff hunk header)
   - Severity tag
   - Clear description
   - Suggested fix (concrete code change when possible)
5. **Positive feedback**: Note well-done patterns at `info` level.
6. **Output format**: Markdown with a final summary table of:
   `| Severity | File:Line | Category | Summary |`

### Final Instruction

Produce your review now. Start with an executive summary (1-3 sentences), then list each finding, and end with the markdown summary table.)";
    return out.str();
}

// ============================================================
// Command class
// ============================================================

/// ReviewRemoteCommand implements the `/review remote <PR>` slash command.
class ReviewRemoteCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "review-remote",
            .description = "Review a remote GitHub PR by URL or number",
            .aliases = {"pr-review"},
            .args = {
                CommandArg{
                    .name = "pr",
                    .description = "PR URL (github.com/OWNER/REPO/pull/N), OWNER/REPO#N, or bare PR number",
                    .type = ArgType::Text,
                    .required = true,
                },
                CommandArg{
                    .name = "--format",
                    .description = "Output format: markdown (default) or json",
                    .type = ArgType::Choice,
                    .required = false,
                    .choices = {"markdown", "json"},
                    .default_value = "markdown",
                },
            },
            .hidden = false,
            .category = "git",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                "Usage: /review remote <PR_URL|PR_NUMBER>"
            ));
        }
        // Re-join args into a single string in case the URL was split by shell
        std::string joined;
        for (std::size_t i = 0; i < ctx.args.size(); ++i) {
            if (i > 0) joined += ' ';
            joined += ctx.args[i];
        }
        if (!parse_pr_input(joined)) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidInput,
                std::format(
                    "Cannot parse PR input: '{}'.\n"
                    "Accepted formats:\n"
                    "  - Full URL:  https://github.com/owner/repo/pull/123\n"
                    "  - Short:     owner/repo#123\n"
                    "  - Bare:      123  (repo auto-detected from current directory)",
                    joined
                )
            ));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        std::string joined;
        for (std::size_t i = 0; i < ctx.args.size(); ++i) {
            if (i > 0) joined += ' ';
            joined += ctx.args[i];
        }

        auto pr = parse_pr_input(joined);
        // validate() already succeeded, so pr must have a value
        if (!pr) {
            return std::unexpected(Error::make(ErrorCode::InternalError, "PR parse failed after validation"));
        }

        // Fetch the diff
        auto diff = fetch_pr_diff(*pr);
        if (!diff) return std::unexpected(diff.error());

        if (diff->empty()) {
            return CommandResult::fail(std::format(
                "PR #{} in {}/{} has an empty diff — nothing to review.",
                pr->number, pr->owner, pr->repo
            ));
        }

        // Build the review prompt and inject it into the query engine.
        // LLM API calls happen inside query_engine.query() — NOT duplicated here.
        auto prompt = build_review_prompt(*pr, *diff);
        return CommandResult::inject(std::move(prompt));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        static constexpr std::array flags = {"--format=markdown", "--format=json"};
        for (auto flag : flags) {
            if (std::string_view(flag).starts_with(partial)) {
                suggestions.emplace_back(flag);
            }
        }
        return suggestions;
    }
};

} // namespace cc::commands
