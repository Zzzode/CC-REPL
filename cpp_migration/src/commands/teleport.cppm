/// @file teleport.cppm
/// @brief TeleportCommand implementing the /teleport slash command.
/// Teleports between sessions.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.teleport;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// TeleportCommand implements the /teleport slash command.
/// Teleports between sessions.
class TeleportCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "teleport",
            .description = "Teleport between sessions",
            .args = {},
            .category = "session",
            .aliases = {},
            .hidden = true,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        return CommandResult::success("Teleport requested. Select or provide a target session to continue work elsewhere.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
