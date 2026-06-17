/// @file ultraplan.cppm
/// @brief UltraplanCommand implementing the /ultraplan slash command.
/// Ultraplan command for expanded planning flows.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.ultraplan;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// UltraplanCommand implements the /ultraplan slash command.
/// Ultraplan command for expanded planning flows.
class UltraplanCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "ultraplan",
            .description = "Ultraplan command",
            .args = {},
            .category = "planning",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        return CommandResult::success(
            "The /ultraplan command is not enabled in this build. "
            "It spawns a remote Claude-on-the-Web plan session, which is gated behind an external flag. "
            "For local planning, use /plan to enter plan mode instead.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
