module;
#include <format>
#include <string>
#include <string_view>
export module cc.commands.commit_push_pr;

import cc.utils.exec_sync;

export namespace cc::commands::commit_push_pr {
struct CommandResponse { bool ok{true}; bool inject{false}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "commit_push_pr"; }

[[nodiscard]] inline auto run(std::string_view branch = {}) -> CommandResponse {
    auto git_root = cc::utils::exec_sync("git rev-parse --show-toplevel");
    if (!git_root) return {.ok = false, .message = "commit-push-pr requires a Git repository"};

    auto current_branch = cc::utils::exec_sync("git branch --show-current");
    auto upstream = cc::utils::exec_sync("git rev-parse --abbrev-ref --symbolic-full-name @{u}");
    auto status = cc::utils::exec_sync_lines("git status --short");
    auto gh_status = cc::utils::exec_sync("gh auth status");

    const auto dirty_count = status ? status->size() : 0;

    // Preflight failed — return diagnostic info as a regular success message
    if (!gh_status) {
        return {.ok = true, .message = std::format(
            "Commit-push-PR preflight\n"
            "Repository: {}\n"
            "Branch: {}\n"
            "Requested branch: {}\n"
            "Upstream: {}\n"
            "Changed paths: {}\n"
            "GitHub CLI: not authenticated\n"
            "Please run `gh auth login` first.",
            *git_root,
            current_branch ? *current_branch : "<unknown>",
            branch.empty() ? "<current>" : std::string(branch),
            upstream ? *upstream : "<none>",
            dirty_count)};
    }

    // Preflight passed — inject the workflow prompt
    return {.ok = true, .inject = true, .message = std::format(
        "Please perform the following git workflow:\n"
        "1. Stage all changes: git add -A\n"
        "2. Create a commit with a conventional commit message (format: type(scope): description)\n"
        "3. Push to the current branch's remote\n"
        "4. Create a pull request using gh pr create\n\n"
        "Use the Bash tool to run these commands. For the commit message, analyze the diff first.\n\n"
        "Context:\n"
        "- Repository: {}\n"
        "- Branch: {}\n"
        "- Upstream: {}\n"
        "- Changed paths: {}",
        *git_root,
        current_branch ? *current_branch : "<unknown>",
        upstream ? *upstream : "<none>",
        dirty_count)};
}
}
