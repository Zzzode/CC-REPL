/// @file brief.cppm
/// @brief BriefCommand implementing the /brief slash command.
/// Toggles brief-only mode.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.brief;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// BriefCommand implements the /brief slash command.
/// Toggles brief-only mode.
class BriefCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "brief",
            .description = "Toggle brief-only mode",
            .args = {},
            .category = "mode",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        // In a real implementation, we would toggle the state
        static bool brief_mode = false;
        brief_mode = !brief_mode;
        
        if (brief_mode) {
            return CommandResult::success("Brief-only mode enabled");
        } else {
            return CommandResult::success("Brief-only mode disabled");
        }
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
