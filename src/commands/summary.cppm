/// @file summary.cppm
/// @brief SummaryCommand implementing the /summary slash command.
/// Generates a conversation summary.
module;


export module cc.commands.summary;

import std;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// SummaryCommand implements the /summary slash command.
/// Generates a conversation summary.
class SummaryCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "summary",
            .description = "Generate conversation summary",
            .args = {},
            .category = "conversation",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        return CommandResult::success(
            "The /summary command is not available as a built-in in this native build. "
            "Use /compact to compress the transcript, or export the session and ask Loom to summarize it.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
