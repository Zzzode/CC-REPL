/// @file color.cppm
/// @brief ColorCommand implementing the /color slash command.
/// Sets the session color for agents.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.color;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// ColorCommand implements the /color slash command.
/// Sets the session color for agents.
class ColorCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "color",
            .description = "Set the session color",
            .aliases = {},
            .args = {
                CommandArg{.name = "color", .description = "Color name, or 'default'/'reset'/'none' to reset",
                           .type = ArgType::Choice, .required = false,
                           .choices = {"red", "orange", "yellow", "green", "blue", "purple", "pink",
                                       "default", "reset", "none"}},
            },
            .hidden = false,
            .category = "customization",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (!ctx.args.empty()) {
            const auto& color = ctx.args[0];
            if (!is_valid_color(color)) {
                return std::unexpected(Error::make(
                    ErrorCode::InvalidRequest,
                    std::format("Invalid color '{}'. Available colors: red, orange, yellow, green, blue, purple, pink, default", color)));
            }
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) {
            return show_available_colors();
        }

        const auto& color = ctx.args[0];
        if (color == "default" || color == "reset" || color == "none") {
            return reset_color();
        }

        return set_color(color);
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        
        static constexpr std::array colors = {
            "red", "orange", "yellow", "green", "blue", "purple", "pink",
            "default", "reset", "none"
        };
        
        for (const auto& color : colors) {
            if (std::string_view(color).starts_with(partial)) {
                suggestions.emplace_back(color);
            }
        }
        
        return suggestions;
    }

private:
    [[nodiscard]] static bool is_valid_color(std::string_view color) {
        static constexpr std::array valid_colors = {
            "red", "orange", "yellow", "green", "blue", "purple", "pink",
            "default", "reset", "none"
        };
        return std::ranges::find(valid_colors, color) != valid_colors.end();
    }

    [[nodiscard]] static Result<CommandResult> show_available_colors() {
        return CommandResult::success(
            "Please provide a color. Available colors: red, orange, yellow, green, blue, purple, pink, default");
    }

    [[nodiscard]] static Result<CommandResult> reset_color() {
        return CommandResult::success("Session color reset to default");
    }

    [[nodiscard]] static Result<CommandResult> set_color(std::string_view color) {
        return CommandResult::success(
            std::format("Session color set to: {}", color));
    }
};

} // namespace cc::commands
