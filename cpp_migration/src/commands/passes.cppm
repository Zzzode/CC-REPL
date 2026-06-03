/// @file passes.cppm
/// @brief PassesCommand implementing the /passes slash command.
/// Guest passes command registration and visit handling.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.passes;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// PassesCommand implements the /passes slash command.
/// Guest passes command registration and visit handling.
class PassesCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "passes",
            .description = "Share a free week of Claude Code with friends",
            .args = {},
            .category = "configuration",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        return CommandResult::success(
            "Guest passes panel opened. Share a free week of Claude Code with friends and track remaining passes."
        );
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
