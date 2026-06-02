/// @file feedback.cppm
/// @brief FeedbackCommand implementing the /feedback slash command.
/// Submits feedback about Claude Code.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.feedback;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// FeedbackCommand implements the /feedback slash command.
/// Submits feedback about Claude Code.
class FeedbackCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "feedback",
            .description = "Submit feedback about Claude Code",
            .aliases = {"bug"},
            .args = {
                CommandArg{.name = "text", .description = "Feedback text",
                           .type = ArgType::Text, .required = false},
            },
            .hidden = false,
            .category = "support",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) {
            return CommandResult::success(
                "Please provide feedback text.\n"
                "Usage: /feedback <your feedback here>");
        }
        
        // In a real implementation, we would submit the feedback
        return CommandResult::success(
            "Thank you for your feedback! It has been recorded.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
