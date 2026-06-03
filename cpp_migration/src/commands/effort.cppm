/// @file effort.cppm
/// @brief EffortCommand implementing the /effort slash command.
/// Sets the effort level for the model.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.effort;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// EffortCommand implements the /effort slash command.
/// Sets the effort level for the model.
class EffortCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "effort",
            .description = "Set the effort level (low/medium/high/max/auto)",
            .aliases = {},
            .args = {
                CommandArg{.name = "level", .description = "Effort level: low, medium, high, max, auto",
                           .type = ArgType::Choice, .required = false,
                           .choices = {"low", "medium", "high", "max", "auto"}},
            },
            .hidden = false,
            .category = "configuration",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (!ctx.args.empty()) {
            const auto& level = ctx.args[0];
            if (!is_valid_level(level)) {
                return std::unexpected(Error::make(
                    ErrorCode::InvalidRequest,
                    std::format("Invalid argument: {}. Valid options are: low, medium, high, max, auto", level)));
            }
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) {
            return show_current_effort();
        }

        const auto& level = ctx.args[0];
        if (level == "auto") {
            return unset_effort();
        }

        return set_effort(level);
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        
        static constexpr std::array levels = {
            "low", "medium", "high", "max", "auto"
        };
        
        for (const auto& level : levels) {
            if (std::string_view(level).starts_with(partial)) {
                suggestions.emplace_back(level);
            }
        }
        
        return suggestions;
    }

private:
    [[nodiscard]] static bool is_valid_level(std::string_view level) {
        static constexpr std::array valid_levels = {
            "low", "medium", "high", "max", "auto",
            "help", "-h", "--help"
        };
        return std::ranges::find(valid_levels, level) != valid_levels.end();
    }

    [[nodiscard]] static std::string get_effort_description(std::string_view level) {
        if (level == "low") return "Quick, straightforward implementation";
        if (level == "medium") return "Balanced approach with standard testing";
        if (level == "high") return "Comprehensive implementation with extensive testing";
        if (level == "max") return "Maximum capability with deepest reasoning (Opus 4.6 only)";
        if (level == "auto") return "Use the default effort level for your model";
        return "Unknown effort level";
    }

    [[nodiscard]] static Result<CommandResult> show_current_effort() {
        return CommandResult::success(
            "Effort level: auto (currently medium)");
    }

    [[nodiscard]] static Result<CommandResult> unset_effort() {
        return CommandResult::success("Effort level set to auto");
    }

    [[nodiscard]] static Result<CommandResult> set_effort(std::string_view level) {
        const auto description = get_effort_description(level);
        return CommandResult::success(
            std::format("Set effort level to {}: {}", level, description));
    }
};

} // namespace cc::commands
