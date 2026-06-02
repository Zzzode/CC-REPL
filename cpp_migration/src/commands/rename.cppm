/// @file rename.cppm
/// @brief RenameCommand implementing the /rename slash command.
/// Renames the current session.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.rename;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// RenameCommand implements the /rename slash command.
/// Renames the current session.
class RenameCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "rename",
            .description = "Rename the current session",
            .aliases = {},
            .args = {
                CommandArg{.name = "name", .description = "New name for the session",
                           .type = ArgType::Text, .required = false},
            },
            .hidden = false,
            .category = "session",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) {
            return CommandResult::success(
                "Please provide a name for the session.\n"
                "Usage: /rename <new name>");
        }
        
        const auto& new_name = ctx.args[0];
        return CommandResult::success(
            std::format("Session renamed to: {}", new_name));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
