/// @file fast.cppm
/// @brief FastCommand implementing the /fast slash command.
/// Toggles fast mode.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.fast;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// FastCommand implements the /fast slash command.
/// Toggles fast mode.
class FastCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "fast",
            .description = "Toggle fast mode",
            .aliases = {},
            .args = {
                CommandArg{.name = "state", .description = "Explicitly set state: on or off",
                           .type = ArgType::Choice, .required = false,
                           .choices = {{"on", "off"}}},
            },
            .hidden = false,
            .category = "mode",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        static bool fast_mode = false;
        
        if (!ctx.args.empty()) {
            const auto& state = ctx.args[0];
            if (state == "on") {
                fast_mode = true;
                return CommandResult::success("⚡ Fast mode ON");
            } else if (state == "off") {
                fast_mode = false;
                return CommandResult::success("Fast mode OFF");
            }
        }
        
        // Toggle if no argument
        fast_mode = !fast_mode;
        if (fast_mode) {
            return CommandResult::success("⚡ Fast mode ON");
        } else {
            return CommandResult::success("Fast mode OFF");
        }
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        
        static constexpr std::array states = {"on", "off"};
        
        for (const auto& state : states) {
            if (std::string_view(state).starts_with(partial)) {
                suggestions.emplace_back(state);
            }
        }
        
        return suggestions;
    }
};

} // namespace cc::commands
