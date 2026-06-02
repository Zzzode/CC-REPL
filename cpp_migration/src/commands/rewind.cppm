/// @file rewind.cppm
/// @brief RewindCommand implementing the /rewind slash command.
/// Rewinds code and/or conversation to an earlier checkpoint.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.rewind;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// RewindCommand implements the /rewind slash command.
/// Rewinds code and/or conversation to an earlier checkpoint.
class RewindCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "rewind",
            .description = "Restore the code and/or conversation to a previous point",
            .args = {},
            .category = "conversation",
            .aliases = {"checkpoint"},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) {
            return CommandResult::success("Opening checkpoint selector for code and conversation rewind.");
        }
        return CommandResult::success(std::format("Opening checkpoint selector at hint '{}'.", ctx.args.front()));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions{"conversation", "code", "both"};
        std::erase_if(suggestions, [partial](const auto& value) {
            return !partial.empty() && !value.starts_with(partial);
        });
        return suggestions;
    }
};

} // namespace cc::commands
