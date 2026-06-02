/// @file upgrade.cppm
/// @brief UpgradeCommand implementing the /upgrade slash command.
/// Upgrades Claude Code.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.upgrade;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// UpgradeCommand implements the /upgrade slash command.
/// Upgrades Claude Code.
class UpgradeCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "upgrade",
            .description = "Upgrade Claude Code to latest version",
            .args = {},
            .category = "maintenance",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        return CommandResult::success("Upgrade command selected. Check the package manager or release channel for available updates.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
