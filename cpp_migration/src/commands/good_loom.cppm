/// @file good_loom.cppm
/// @brief GoodLoomCommand implementing the hidden /good-loom slash command.
module;

#include <string>
#include <vector>

export module cc.commands.good_loom;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

class GoodLoomCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "good-loom",
            .description = "Record positive feedback for Loom",
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
            "The /good-loom feedback command is not available in this native build. "
            "Feedback submission requires the analytics backend, which is not reachable here.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
