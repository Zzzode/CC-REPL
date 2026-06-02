/// @file theme.cppm
/// @brief ThemeCommand implementing the /theme slash command.
/// Switch between dark/light/auto, list available themes, custom color configuration.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <ranges>
#include <algorithm>
#include <span>
#include <array>

export module cc.commands.theme;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// Color scheme mode
enum class ThemeMode : std::uint8_t {
    Dark,
    Light,
    Auto,       // Follow system/terminal preference
};

/// Theme color definition
struct ThemeColors {
    std::string primary;        // Main accent color (hex)
    std::string secondary;      // Secondary accent
    std::string background;     // Background hint
    std::string text;           // Primary text color
    std::string dim;            // Dimmed/muted text
    std::string error;          // Error highlight
    std::string success;        // Success highlight
};

/// A complete theme definition
struct ThemeDefinition {
    std::string name;
    ThemeMode mode;
    ThemeColors colors;
    std::string description;
};

/// ThemeCommand implements the /theme slash command.
/// Manages visual themes: switch, list, and configure colors.
class ThemeCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "theme",
            .description = "Switch visual theme or color scheme",
            .aliases = {},
            .args = {
                CommandArg{.name = "action", .description = "list | set | current | colors",
                           .type = ArgType::Choice, .required = false,
                           .choices = {{"list", "set", "current", "colors"}}},
                CommandArg{.name = "theme_name", .description = "Theme name to apply",
                           .type = ArgType::Text, .required = false},
            },
            .hidden = false,
            .category = "config",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};
        auto action = ctx.args[0];
        static constexpr std::array valid = {"list", "set", "current", "colors"};
        // Allow direct theme name as shortcut
        if (std::ranges::find(valid, action) == valid.end()) {
            if (!is_valid_theme(action)) {
                return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                    std::format("Unknown theme or action: '{}'. Use /theme list to see options.", action)));
            }
        }
        if (action == "set" && ctx.args.size() < 2) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest, "Usage: /theme set <name>"));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) return CommandResult::success(format_current());

        auto action = std::string(ctx.args[0]);

        if (action == "list") return CommandResult::success(format_theme_list());
        if (action == "current") return CommandResult::success(format_current());
        if (action == "colors") return CommandResult::success(format_colors());
        if (action == "set") return apply_theme(std::string(ctx.args[1]));

        // Direct theme name shortcut
        return apply_theme(action);
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        for (auto s : {"list", "set", "current", "colors"}) {
            if (std::string_view(s).starts_with(partial)) {
                suggestions.emplace_back(s);
            }
        }
        for (const auto& t : available_themes()) {
            if (t.name.starts_with(partial)) {
                suggestions.push_back(t.name);
            }
        }
        return suggestions;
    }

    /// Get current active theme
    [[nodiscard]] const ThemeDefinition& current_theme() const { return *current_; }

private:
    const ThemeDefinition* current_ = &available_themes()[0];

    [[nodiscard]] static const std::vector<ThemeDefinition>& available_themes() {
        static const std::vector<ThemeDefinition> themes = {
            {"dark", ThemeMode::Dark,
             {"#7c3aed", "#06b6d4", "#1e1e2e", "#cdd6f4", "#6c7086", "#f38ba8", "#a6e3a1"},
             "Default dark theme (Catppuccin-inspired)"},
            {"light", ThemeMode::Light,
             {"#7c3aed", "#0891b2", "#eff1f5", "#4c4f69", "#9ca0b0", "#d20f39", "#40a02b"},
             "Light theme for well-lit environments"},
            {"auto", ThemeMode::Auto,
             {"#7c3aed", "#06b6d4", "", "#cdd6f4", "#6c7086", "#f38ba8", "#a6e3a1"},
             "Follows terminal/system dark mode preference"},
            {"monokai", ThemeMode::Dark,
             {"#f92672", "#66d9ef", "#272822", "#f8f8f2", "#75715e", "#f92672", "#a6e22e"},
             "Classic Monokai color scheme"},
            {"solarized", ThemeMode::Light,
             {"#268bd2", "#2aa198", "#fdf6e3", "#657b83", "#93a1a1", "#dc322f", "#859900"},
             "Solarized Light"},
        };
        return themes;
    }

    [[nodiscard]] static bool is_valid_theme(std::string_view name) {
        return std::ranges::any_of(available_themes(), [&](const auto& t) { return t.name == name; });
    }

    [[nodiscard]] std::string format_current() const {
        return std::format("Current theme: {} ({})\n{}",
            current_->name,
            current_->mode == ThemeMode::Dark ? "dark" : current_->mode == ThemeMode::Light ? "light" : "auto",
            current_->description);
    }

    [[nodiscard]] static std::string format_theme_list() {
        std::string out = "Available themes:\n";
        for (const auto& t : available_themes()) {
            out += std::format("  • {:<12} — {}\n", t.name, t.description);
        }
        return out;
    }

    [[nodiscard]] std::string format_colors() const {
        return std::format(
            "Current colors ({}):\n"
            "  Primary:    {}\n"
            "  Secondary:  {}\n"
            "  Background: {}\n"
            "  Text:       {}\n"
            "  Dim:        {}\n"
            "  Error:      {}\n"
            "  Success:    {}",
            current_->name,
            current_->colors.primary, current_->colors.secondary,
            current_->colors.background, current_->colors.text,
            current_->colors.dim, current_->colors.error, current_->colors.success);
    }

    [[nodiscard]] Result<CommandResult> apply_theme(const std::string& name) {
        auto it = std::ranges::find_if(available_themes(), [&](const auto& t) { return t.name == name; });
        if (it == available_themes().end()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Theme '{}' not found. Use /theme list.", name)));
        }
        current_ = &(*it);
        return CommandResult::success(std::format("Theme switched to: {}\n{}", name, it->description));
    }
};

} // namespace cc::commands
