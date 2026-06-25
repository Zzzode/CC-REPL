/// @file advisor.cppm
/// @brief AdvisorCommand implementing the /advisor slash command.
/// Configures the advisor model for enhanced assistance.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.advisor;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// AdvisorCommand implements the /advisor slash command.
/// Configures the advisor model for enhanced assistance.
class AdvisorCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "advisor",
            .description = "Configure the advisor model",
            .args = {
                CommandArg{.name = "model", .description = "Model name to use as advisor, or 'off' to disable",
                           .type = ArgType::Text, .required = false},
            },
            .category = "configuration",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (!ctx.args.empty()) {
            const auto& arg = ctx.args[0];
            if (arg.empty()) {
                return std::unexpected(Error::make(
                    ErrorCode::InvalidRequest,
                    "Model name cannot be empty"));
            }
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) {
            return show_current_status();
        }

        const auto& arg = ctx.args[0];
        if (arg == "off" || arg == "unset") {
            return disable_advisor();
        }

        return set_advisor_model(arg);
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        
        static constexpr std::array models = {
            "claude-3-opus", "claude-3-sonnet", "claude-3-haiku",
            "claude-3-5-opus", "claude-3-5-sonnet",
            "off", "unset"
        };
        
        for (const auto& model : models) {
            if (std::string_view(model).starts_with(partial)) {
                suggestions.emplace_back(model);
            }
        }
        
        return suggestions;
    }

private:
    [[nodiscard]] static Result<CommandResult> show_current_status() {
        // In a real implementation, we would check the current state
        return CommandResult::success(
            "Advisor: not set\nUse \"/advisor <model>\" to enable (e.g., \"/advisor opus\").");
    }

    [[nodiscard]] static Result<CommandResult> disable_advisor() {
        return CommandResult::success("Advisor disabled.");
    }

    [[nodiscard]] static Result<CommandResult> set_advisor_model(std::string_view model) {
        // Validate model name (in real implementation, we'd have actual validation)
        if (model.empty()) {
            return CommandResult::fail("Invalid model name");
        }

        return CommandResult::success(
            std::format("Advisor set to {}.", model));
    }
};

} // namespace cc::commands
