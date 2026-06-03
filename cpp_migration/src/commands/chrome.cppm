/// @file chrome.cppm
/// @brief ChromeCommand implementing the /chrome slash command.
module;

#include <string>
#include <vector>

export module cc.commands.chrome;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

class ChromeCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "chrome",
            .description = "Show Claude in Chrome setup information",
            .args = {},
            .category = "integrations",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        return CommandResult::success(
            "Claude in Chrome setup\n"
            "Install: https://claude.ai/chrome\n"
            "Reconnect: https://clau.de/chrome/reconnect\n"
            "Permissions: https://clau.de/chrome/permissions\n"
            "CLI flags: --chrome or --no-chrome");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
