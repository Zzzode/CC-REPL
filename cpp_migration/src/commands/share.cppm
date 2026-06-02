/// @file share.cppm
/// @brief ShareCommand implementing the /share slash command.
/// Shares the current session.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.share;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// ShareCommand implements the /share slash command.
/// Shares the current session.
class ShareCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "share",
            .description = "Share current session",
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
        return CommandResult::success("Preparing current session for sharing. Export the session transcript before publishing externally.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
