/// @file summary.cppm
/// @brief SummaryCommand implementing the /summary slash command.
/// Generates a conversation summary.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.summary;

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
            .hidden = true,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        return CommandResult::success("Conversation summary requested. The current transcript can be summarized or exported from session state.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
