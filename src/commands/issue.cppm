/// @file issue.cppm
/// @brief IssueCommand implementing the /issue slash command.
/// Issue management command for tracker workflows.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.issue;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// IssueCommand implements the /issue slash command.
/// Issue management command for tracker workflows.
class IssueCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "issue",
            .description = "Issue management",
            .args = {},
            .category = "tools",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        return CommandResult::success(
            "The /issue command is not available in this native build. "
            "Use the GitHub CLI (`gh issue list|create|view <n>`) or the github-issues skill for issue tracking.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
