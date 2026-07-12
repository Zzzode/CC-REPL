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
import cc.state.app_state;
import cc.state.store;

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
            return show_current_status(ctx);
        }

        const auto& arg = ctx.args[0];
        if (arg == "off" || arg == "unset") {
            return disable_advisor(ctx);
        }

        return set_advisor_model(ctx, arg);
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
    [[nodiscard]] static Result<CommandResult> show_current_status(const CommandContext& ctx) {
        using cc::state::AppState;
        const auto* state = static_cast<const AppState*>(ctx.get_app_state());

        if (state && state->advisor_model.has_value()) {
            return CommandResult::success(
                std::format("Advisor: {}\nUse \"/advisor off\" to disable.", *state->advisor_model));
        }
        return CommandResult::success(
            "Advisor: not set\nUse \"/advisor <model>\" to enable (e.g., \"/advisor opus\").");
    }

    [[nodiscard]] static Result<CommandResult> disable_advisor(const CommandContext& ctx) {
        using cc::state::ActionType;
        ctx.dispatch_action(static_cast<int>(ActionType::SetAdvisorModel), nullptr);
        return CommandResult::success("Advisor disabled.");
    }

    [[nodiscard]] static Result<CommandResult> set_advisor_model(const CommandContext& ctx,
                                                                  std::string_view model) {
        if (model.empty()) {
            return CommandResult::fail("Invalid model name");
        }

        using cc::state::ActionType;
        std::string model_str{model};
        ctx.dispatch_action(static_cast<int>(ActionType::SetAdvisorModel), &model_str);
        return CommandResult::success(
            std::format("Advisor set to {}.", model));
    }
};

} // namespace cc::commands
