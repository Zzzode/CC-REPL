/// @file status.cppm
/// @brief StatusCommand implementing the /status slash command.
/// Shows Loom status.
module;


export module cc.commands.status;

import std;

import cc.types.types;
import cc.commands.command;
import cc.constants.product;

export namespace cc::commands {

using namespace cc::core;

/// StatusCommand implements the /status slash command.
/// Shows Loom status.
class StatusCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "status",
            .description = "Show Loom status including version, model, account, API connectivity, and tool statuses",
            .args = {},
            .category = "info",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        return CommandResult::success(
            std::string("Loom Status:\n") +
            "Version: " + std::string(cc::constants::product::LOOM_VERSION) + "\n" +
            "Model: Not configured\n" +
            "Status: Offline (C++ Migration Demo)");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
