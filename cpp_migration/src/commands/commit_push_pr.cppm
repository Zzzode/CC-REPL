module;
#include <format>
#include <string>
#include <string_view>
export module cc.commands.commit_push_pr;

import cc.utils.exec_sync;

export namespace cc::commands::commit_push_pr {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "commit_push_pr"; }

[[nodiscard]] inline auto run(std::string_view branch = {}) -> CommandResponse {
    auto git_root = cc::utils::exec_sync("git rev-parse --show-toplevel");
    if (!git_root) return {.ok = false, .message = "commit-push-pr requires a Git repository"};

    auto current_branch = cc::utils::exec_sync("git branch --show-current");
    auto upstream = cc::utils::exec_sync("git rev-parse --abbrev-ref --symbolic-full-name @{u}");
    auto status = cc::utils::exec_sync_lines("git status --short");
    auto gh_status = cc::utils::exec_sync("gh auth status");

    const auto dirty_count = status ? status->size() : 0;
    return {.ok = true, .message = std::format(
        "Commit-push-PR preflight\n"
        "Repository: {}\n"
        "Branch: {}\n"
        "Requested branch: {}\n"
        "Upstream: {}\n"
        "Changed paths: {}\n"
        "GitHub CLI: {}\n"
        "Next: review diff, run tests, commit selected changes, push, then open a PR.",
        *git_root,
        current_branch ? *current_branch : "<unknown>",
        branch.empty() ? "<current>" : std::string(branch),
        upstream ? *upstream : "<none>",
        dirty_count,
        gh_status ? "authenticated" : "not authenticated")};
}
}
