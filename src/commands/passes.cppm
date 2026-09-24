/// @file passes.cppm
/// @brief PassesCommand implementing the /passes slash command.
/// Guest passes command registration and visit handling.
module;


export module cc.commands.passes;

import std;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// PassesCommand implements the /passes slash command.
/// Guest passes command registration and visit handling.
class PassesCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "passes",
            .description = "Share a free week of Loom with friends",
            .args = {},
            .category = "configuration",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        return CommandResult::success(
            "Guest passes require the referral backend API, which is not reachable from this native build. "
            "Manage your passes in your account settings, or share a free week of Loom "
            "from your account page.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
