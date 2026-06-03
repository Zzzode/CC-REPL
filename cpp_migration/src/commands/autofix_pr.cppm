module;
#include <format>
#include <string>
#include <string_view>
export module cc.commands.autofix_pr;

import cc.utils.exec_sync;

export namespace cc::commands::autofix_pr {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "autofix_pr"; }

[[nodiscard]] inline bool is_safe_pr_ref(std::string_view pr) {
    for (char ch : pr) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
                        ch == '-' || ch == '/' || ch == ':' || ch == '#';
        if (!ok) return false;
    }
    return true;
}

[[nodiscard]] inline auto run(std::string_view pr = {}) -> CommandResponse {
    auto git_root = cc::utils::exec_sync("git rev-parse --show-toplevel");
    if (!git_root) return {.ok = false, .message = "autofix-pr requires a Git repository"};
    if (!is_safe_pr_ref(pr)) {
        return {.ok = false, .message = "autofix-pr PR reference contains unsupported characters"};
    }

    auto gh_status = cc::utils::exec_sync("gh auth status");
    if (!gh_status) {
        return {.ok = false, .message = "autofix-pr requires GitHub CLI authentication"};
    }

    auto pr_view = pr.empty()
        ? cc::utils::exec_sync("gh pr view --json number,url,reviewDecision")
        : cc::utils::exec_sync("gh pr view " + std::string(pr) + " --json number,url,reviewDecision");
    if (!pr_view) {
        return {.ok = false, .message = "No pull request context found for autofix-pr"};
    }

    auto comments = pr.empty()
        ? cc::utils::exec_sync("gh pr view --json comments,reviews")
        : cc::utils::exec_sync("gh pr view " + std::string(pr) + " --json comments,reviews");

    return {.ok = true, .message = std::format(
        "Autofix PR preflight complete\n"
        "Repository: {}\n"
        "Pull request: {}\n"
        "Review context: {}\n"
        "Next: inspect unresolved comments, edit files, run tests, then commit intentionally.",
        *git_root,
        *pr_view,
        comments ? "available" : "unavailable")};
}
}
