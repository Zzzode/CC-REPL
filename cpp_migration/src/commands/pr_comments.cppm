// PR Comments command - moved to plugin, provides fallback guidance
module;
#include <string>
#include <string_view>
export module cc.commands.pr_comments;
export namespace cc::commands::pr_comments {

struct CommandResponse { bool ok{true}; std::string message; };

[[nodiscard]] inline auto name() -> std::string_view { return "pr-comments"; }

[[nodiscard]] inline auto run(std::string_view pr = {}) -> CommandResponse {
    std::string msg = "This command has moved to a plugin.\n\n"
        "To review PR comments, you can:\n"
        "1. Install the code-review skill: `npx skills install code-review`\n"
        "2. Or use the gh CLI directly:\n";
    
    if (!pr.empty()) {
        msg += "   `gh pr view " + std::string(pr) + " --comments`\n"
               "   `gh pr diff " + std::string(pr) + "`\n";
    } else {
        msg += "   `gh pr view --comments`\n"
               "   `gh pr diff`\n";
    }
    
    msg += "\nFor a full code review, ask me to review the current PR diff.";
    
    return {.ok = true, .message = msg};
}

}
