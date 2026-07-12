/// @file hooks.cppm
/// @brief HooksCommand implementing the /hooks slash command.
/// Hooks configuration command registration and summaries.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.hooks;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// HooksCommand implements the /hooks slash command.
/// Hooks configuration command registration and summaries.
class HooksCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "hooks",
            .description = "View hook configurations for tool events",
            .args = {},
            .category = "configuration",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (!ctx.args.empty() && ctx.args.front() == "list") {
            return CommandResult::success("Hook events: PreToolUse, PostToolUse, Notification, Stop, SubagentStop");
        }
        // Return "UI:hooks" metadata so the app shell opens the HooksConfig
        // modal dialog via PushFromCommandMetadata.
        return CommandResult{
            true,
            "Hook configuration viewer opened.",
            "UI:hooks",
            CommandStatus::Succeeded,
        };
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions{"list", "tool-events", "notifications"};
        std::erase_if(suggestions, [partial](const auto& value) {
            return !partial.empty() && !value.starts_with(partial);
        });
        return suggestions;
    }
};

} // namespace cc::commands
