/// @file color.cppm
/// @brief ColorCommand implementing the /color slash command.
/// Sets the session color for agents via agent_color_manager.
/// Faithful port of src/commands/color/color.ts.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>
#include <cctype>

export module cc.commands.color;

import cc.types.types;
import cc.commands.command;
import cc.state.app_state;
import cc.tools.agent_color_manager;
import cc.tools.agent.utils;

export namespace cc::commands {

using namespace cc::core;
using cc::tools::agent_color_manager::AgentColor;
using cc::tools::agent_color_manager::parse_color_name;
using cc::tools::agent_color_manager::set_agent_color;
using cc::tools::agent_color_manager::get_agent_color;
using cc::tools::agent_color_manager::agent_color_name;

/// The agent type key used for the main session color.
/// TS uses "standaloneAgentContext.color"; CPP maps this to the
/// "general-purpose" agent type which represents the main conversation agent.
inline constexpr std::string_view kSessionAgentType = "general-purpose";

/// Color names that reset to default (no color).
/// TS REF: src/commands/color/color.ts RESET_ALIASES
inline constexpr std::array<std::string_view, 5> kResetAliases = {
    "default", "reset", "none", "gray", "grey"
};

/// ColorCommand implements the /color slash command.
/// Sets the session color for agents via agent_color_manager.
class ColorCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "color",
            .description = "Set the prompt bar color for this session",
            .args = {
                CommandArg{.name = "color", .description = "Color name, or 'default'/'reset' to reset",
                           .type = ArgType::Choice, .required = false,
                           .choices = {"red", "orange", "yellow", "green", "blue", "purple", "pink", "cyan",
                                       "default", "reset", "none", "gray", "grey"}},
            },
            .category = "customization",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (!ctx.args.empty()) {
            const auto& color = ctx.args[0];
            if (!is_valid_color(color)) {
                return std::unexpected(Error::make(
                    ErrorCode::InvalidRequest,
                    std::format("Invalid color '{}'. Available colors: red, orange, yellow, green, blue, purple, pink, cyan, default", color)));
            }
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        // Teammate guard: TS REF color.ts — teammates cannot set their own color
        if (cc::tools::agent::utils::current_session_is_teammate()) {
            return CommandResult::fail(
                "Cannot set color: This session is a swarm teammate. "
                "Teammate colors are assigned by the team leader.");
        }

        if (ctx.args.empty()) {
            return show_current_color();
        }

        const auto& color_arg = to_lower(ctx.args[0]);

        // Handle reset aliases
        if (is_reset_alias(color_arg)) {
            set_agent_color(kSessionAgentType, std::nullopt);
            return CommandResult::success("Session color reset to default");
        }

        // Parse and validate color name
        auto parsed = parse_color_name(color_arg);
        if (!parsed) {
            return CommandResult::fail(std::format(
                "Invalid color \"{}\". Available colors: red, orange, yellow, green, blue, purple, pink, cyan, default",
                color_arg));
        }

        // Set the color for the session agent
        set_agent_color(kSessionAgentType, parsed);

        return CommandResult::success(std::format("Session color set to: {}", color_arg));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;

        static constexpr std::array colors = {
            "red", "orange", "yellow", "green", "blue", "purple", "pink", "cyan",
            "default", "reset", "none", "gray", "grey"
        };

        std::string lower_partial;
        lower_partial.reserve(partial.size());
        for (unsigned char ch : partial) lower_partial.push_back(static_cast<char>(std::tolower(ch)));

        for (const auto& color : colors) {
            if (std::string_view(color).starts_with(lower_partial)) {
                suggestions.emplace_back(color);
            }
        }

        return suggestions;
    }

private:
    /// Convert a string to lowercase for case-insensitive comparison.
    [[nodiscard]] static std::string to_lower(std::string_view s) {
        std::string result;
        result.reserve(s.size());
        for (unsigned char ch : s) result.push_back(static_cast<char>(std::tolower(ch)));
        return result;
    }

    /// Check if a color name is a reset alias (case-insensitive).
    [[nodiscard]] static bool is_reset_alias(std::string_view color) {
        auto lower = to_lower(color);
        return std::ranges::find(kResetAliases, lower) != kResetAliases.end();
    }

    /// Check if a color name is valid (case-insensitive).
    [[nodiscard]] static bool is_valid_color(std::string_view color) {
        auto lower = to_lower(color);
        if (is_reset_alias(lower)) return true;
        return parse_color_name(lower).has_value();
    }

    /// Show the current session color and available options.
    [[nodiscard]] static Result<CommandResult> show_current_color() {
        auto current = get_agent_color(kSessionAgentType);
        std::string current_str = current
            ? std::string(agent_color_name(*current))
            : "default (no color)";

        return CommandResult::success(std::format(
            "Current session color: {}\n\n"
            "Available colors: red, orange, yellow, green, blue, purple, pink, cyan, default",
            current_str));
    }
};

} // namespace cc::commands
