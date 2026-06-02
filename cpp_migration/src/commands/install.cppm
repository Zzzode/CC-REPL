/// @file install.cppm
/// @brief InstallCommand implementing the /install slash command.
/// Installs Claude Code system-wide.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.install;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// InstallCommand implements the /install slash command.
/// Installs Claude Code system-wide.
class InstallCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "install",
            .description = "Install Claude Code system-wide",
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
        return CommandResult::success("Install command selected. Use the packaged installer or build output under cpp_migration/build/bin.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
