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
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        return CommandResult::success(
            "The /teleport command is not available in this native build. "
            "Remote session handoff requires the teleport service and a connected Claude-on-the-Web account.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
