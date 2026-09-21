/// @file env.cppm
/// @brief EnvCommand implementing the /env slash command.
/// Environment management command registration shim.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.env;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// EnvCommand implements the /env slash command.
/// Environment management command registration shim.
class EnvCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "env",
            .description = "Environment management",
            .args = {},
            .category = "configuration",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (!ctx.args.empty() && (ctx.args.front() == "help" || ctx.args.front() == "--help")) {
            return CommandResult::success(
                "The /env command is disabled by default. Remote environment controls are available through /remote-env."
            );
        }
        return CommandResult::success("The /env command is disabled in this build; use /remote-env for remote environment setup.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions{"help", "remote-env"};
        std::erase_if(suggestions, [partial](const auto& value) {
            return !partial.empty() && !value.starts_with(partial);
        });
        return suggestions;
    }
};

} // namespace cc::commands
