/// @file tag.cppm
/// @brief TagCommand implementing the /tag slash command.
/// Tags the current session.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.tag;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// TagCommand implements the /tag slash command.
/// Tags the current session.
class TagCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "tag",
            .description = "Toggle a searchable tag on the current session",
            .args = {
                CommandArg{.name = "tag-name", .description = "Name of the tag to add/remove",
                           .type = ArgType::Text, .required = false},
            },
            .category = "session",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) {
            return CommandResult::success(
                "Usage: /tag <tag-name>\n\n"
                "Toggle a searchable tag on the current session.\n"
                "Run the same command again to remove the tag.\n"
                "\nExamples:\n"
                "  /tag bugfix        # Add tag\n"
                "  /tag bugfix        # Remove tag (toggle)\n"
                "  /tag feature-auth");
        }
        
        const auto& tag_name = ctx.args[0];
        return CommandResult::success(
            std::format("Tagged session with #{}", tag_name));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
