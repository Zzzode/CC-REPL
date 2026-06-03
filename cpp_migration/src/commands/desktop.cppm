/// @file desktop.cppm
/// @brief DesktopCommand implementing the /desktop slash command.
module;

#include <string>
#include <vector>

export module cc.commands.desktop;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

class DesktopCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "desktop",
            .description = "Continue the current session in Claude Desktop",
            .args = {},
            .category = "integrations",
            .aliases = {"app"},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        return CommandResult::success(
            "Claude Desktop handoff\n"
            "Learn more: https://clau.de/desktop\n"
            "macOS download: https://claude.ai/api/desktop/darwin/universal/dmg/latest/redirect\n"
            "Windows download: https://claude.ai/api/desktop/win32/x64/exe/latest/redirect");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
