/// @file status.cppm
/// @brief StatusCommand implementing the /status slash command.
/// Shows Claude Code status.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.status;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// StatusCommand implements the /status slash command.
/// Shows Claude Code status.
class StatusCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "status",
            .description = "Show Claude Code status including version, model, account, API connectivity, and tool statuses",
            .aliases = {},
            .args = {},
            .hidden = false,
            .category = "info",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        return CommandResult::success(
            "Claude Code Status:\n"
            "Version: 0.1.0\n"
            "Model: Not configured\n"
            "Status: Offline (C++ Migration Demo)");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
