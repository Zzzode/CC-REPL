// PR Comments command - fetches and summarizes PR comments for the current branch
module;
#include <string>
#include <string_view>
export module cc.commands.pr_comments;
export namespace cc::commands::pr_comments {

struct CommandResponse { bool ok{true}; bool inject{false}; std::string message; };

[[nodiscard]] inline auto name() -> std::string_view { return "pr-comments"; }

[[nodiscard]] inline auto run(std::string_view pr = {}) -> CommandResponse {
    std::string prompt = "Fetch and summarize the PR comments for the current branch.\n\n";

    if (!pr.empty()) {
        prompt += "Run: gh pr view " + std::string(pr) + " --comments\n";
        prompt += "Then also run: gh pr diff " + std::string(pr) + "\n\n";
    } else {
        prompt += "Run: gh pr view --comments\n\n";
    }

    prompt += "Then summarize the key discussion points, unresolved threads, and review status.\n"
              "Use the Bash tool to run these commands.";

    return {.ok = true, .inject = true, .message = std::move(prompt)};
}

}
