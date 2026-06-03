/// @file mobile.cppm
/// @brief MobileCommand implementing the /mobile slash command.
module;

#include <string>
#include <vector>

export module cc.commands.mobile;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

class MobileCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "mobile",
            .description = "Show Claude mobile app download links",
            .args = {},
            .category = "integrations",
            .aliases = {"ios", "android"},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        return CommandResult::success(
            "Claude mobile app\n"
            "iOS: https://apps.apple.com/app/claude-by-anthropic/id6473753684\n"
            "Android: https://play.google.com/store/apps/details?id=com.anthropic.claude");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
