/// @file good_claude.cppm
/// @brief GoodClaudeCommand implementing the hidden /good-claude slash command.
module;

#include <string>
#include <vector>

export module cc.commands.good_claude;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

class GoodClaudeCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "good-claude",
            .description = "Record positive feedback for Claude",
            .args = {},
            .category = "feedback",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        return CommandResult::success(
            "The /good-claude feedback command is not available in this native build. "
            "Feedback submission requires the analytics backend, which is not reachable here.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
