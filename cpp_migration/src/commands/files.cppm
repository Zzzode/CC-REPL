/// @file files.cppm
/// @brief FilesCommand implementing the /files slash command.
/// Shows all files currently in context.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.files;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// FilesCommand implements the /files slash command.
/// Shows all files currently in context.
class FilesCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "files",
            .description = "Show all files currently in context",
            .args = {},
            .category = "context",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        return CommandResult::fail("Context file provider is not configured.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
