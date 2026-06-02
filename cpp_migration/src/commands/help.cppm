/// @file help.cppm
/// @brief HelpCommand implementing the /help slash command.
/// Lists all available commands with descriptions, shows detailed help
/// for specific commands, displays keyboard shortcuts and examples.
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

export module cc.commands.help;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// Keyboard shortcut definition
struct KeyboardShortcut {
    std::string_view keys;
    std::string_view description;
};

/// Usage example for a command
struct CommandExample {
    std::string_view input;
    std::string_view description;
};

/// HelpCommand implements the /help slash command.
/// Displays available commands, detailed help, shortcuts, and examples.
class HelpCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "help",
            .description = "Show available commands and usage information",
            .aliases = {"h", "?"},
            .args = {
                CommandArg{.name = "command", .description = "Specific command to get help for",
                           .type = ArgType::Text, .required = false},
                CommandArg{.name = "--shortcuts", .description = "Show keyboard shortcuts",
                           .type = ArgType::None, .required = false},
                CommandArg{.name = "--examples", .description = "Show usage examples",
                           .type = ArgType::None, .required = false},
            },
            .hidden = false,
            .category = "session",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        // Check for --shortcuts or --examples flags
        for (const auto& arg : ctx.args) {
            if (arg == "--shortcuts" || arg == "-k") {
                return CommandResult::success(format_shortcuts());
            }
            if (arg == "--examples" || arg == "-e") {
                return CommandResult::success(format_examples());
            }
        }

        // If a specific command is given, show detailed help
        if (!ctx.args.empty() && !ctx.args[0].starts_with("-")) {
            return show_command_help(ctx.args[0]);
        }

        // Default: list all commands
        return CommandResult::success(format_command_list());
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;

        // Suggest command names for detailed help
        for (const auto& name : registered_command_names()) {
            if (name.starts_with(partial)) {
                suggestions.emplace_back(name);
            }
        }

        // Suggest flags
        static constexpr std::array flags = {"--shortcuts", "--examples"};
        for (auto flag : flags) {
            if (std::string_view(flag).starts_with(partial)) {
                suggestions.emplace_back(flag);
            }
        }
        return suggestions;
    }

    /// Set the registry for listing commands (injected by the command registry)
    void set_command_definitions(std::vector<const CommandDefinition*> defs) {
        command_defs_ = std::move(defs);
    }

private:
    std::vector<const CommandDefinition*> command_defs_;

    /// Format the complete command list grouped by category
    [[nodiscard]] std::string format_command_list() const {
        std::string output = "Available Commands:\n\n";

        // Group by category
        std::string current_category;
        for (const auto* def : command_defs_) {
            auto category = def->category.empty() ? std::string("general") : def->category;
            if (category != current_category) {
                current_category = category;
                output += std::format("  [{}]\n", current_category);
            }

            // Format: /name (aliases) - description
            std::string aliases_str;
            if (!def->aliases.empty()) {
                aliases_str = " (";
                for (std::size_t i = 0; i < def->aliases.size(); ++i) {
                    if (i > 0) aliases_str += ", ";
                    aliases_str += "/" + def->aliases[i];
                }
                aliases_str += ")";
            }

            output += std::format("    /{}{:<16} {}\n",
                def->name, aliases_str, def->description);
        }

        output += "\nType /help <command> for detailed usage.\n";
        output += "Type /help --shortcuts for keyboard shortcuts.\n";
        output += "Type /help --examples for usage examples.\n";
        return output;
    }

    /// Show detailed help for a specific command
    [[nodiscard]] Result<CommandResult> show_command_help(std::string_view name) const {
        // Strip leading '/' if present
        if (name.starts_with("/")) name = name.substr(1);

        // Find the command definition
        const CommandDefinition* target = nullptr;
        for (const auto* def : command_defs_) {
            if (def->name == name || std::ranges::find(def->aliases, std::string(name)) != def->aliases.end()) {
                target = def;
                break;
            }
        }

        if (!target) {
            return CommandResult::fail(
                std::format("Unknown command: '{}'. Type /help for a list of commands.", name)
            );
        }

        // Format detailed help
        std::string output = std::format("/{} - {}\n\n", target->name, target->description);

        // Aliases
        if (!target->aliases.empty()) {
            output += "Aliases: ";
            for (std::size_t i = 0; i < target->aliases.size(); ++i) {
                if (i > 0) output += ", ";
                output += "/" + target->aliases[i];
            }
            output += "\n\n";
        }

        // Arguments
        if (!target->args.empty()) {
            output += "Arguments:\n";
            for (const auto& arg : target->args) {
                auto required_tag = arg.required ? " (required)" : "";
                output += std::format("  {}{}\n    {}\n",
                    arg.name, required_tag, arg.description);

                // Show choices if applicable
                if (!arg.choices.empty()) {
                    output += "    Choices: ";
                    for (std::size_t i = 0; i < arg.choices.size(); ++i) {
                        if (i > 0) output += ", ";
                        output += arg.choices[i];
                    }
                    output += "\n";
                }
            }
        }

        // Category
        if (!target->category.empty()) {
            output += std::format("\nCategory: {}\n", target->category);
        }

        return CommandResult::success(std::move(output));
    }

    /// Format keyboard shortcuts
    [[nodiscard]] static std::string format_shortcuts() {
        static constexpr std::array<KeyboardShortcut, 12> shortcuts = {{
            {"Ctrl+C",     "Cancel current operation / interrupt generation"},
            {"Ctrl+D",     "Exit the CLI (same as /exit)"},
            {"Ctrl+L",     "Clear screen (same as /clear)"},
            {"Ctrl+R",     "Search command history"},
            {"Ctrl+A",     "Move cursor to beginning of line"},
            {"Ctrl+E",     "Move cursor to end of line"},
            {"Ctrl+W",     "Delete word before cursor"},
            {"Ctrl+U",     "Delete from cursor to beginning of line"},
            {"Tab",        "Auto-complete command or argument"},
            {"Up/Down",    "Navigate command history"},
            {"Esc",        "Cancel current input / dismiss popup"},
            {"Shift+Enter","Insert newline in multi-line input"},
        }};

        std::string output = "Keyboard Shortcuts:\n\n";
        for (const auto& [keys, desc] : shortcuts) {
            output += std::format("  {:<14} {}\n", keys, desc);
        }
        return output;
    }

    /// Format usage examples
    [[nodiscard]] static std::string format_examples() {
        static constexpr std::array<CommandExample, 10> examples = {{
            {"/commit",                    "Generate commit message from staged changes"},
            {"/commit --all",              "Stage all changes and generate commit"},
            {"/commit -m \"fix: typo\"",   "Commit with a manual message"},
            {"/review",                    "Review all uncommitted changes"},
            {"/review --branch main",      "Review changes vs main branch"},
            {"/config set model.default_model claude-sonnet-4-20250514", "Change model"},
            {"/compact --dry-run",         "Preview compaction without applying"},
            {"/mcp add filesystem npx -y @anthropic/mcp-fs", "Add an MCP server"},
            {"/doctor",                    "Run system diagnostics"},
            {"/clear --reset",             "Clear screen and reset conversation"},
        }};

        std::string output = "Usage Examples:\n\n";
        for (const auto& [input, desc] : examples) {
            output += std::format("  {}\n    {}\n\n", input, desc);
        }
        return output;
    }

    /// Get all registered command names (for completion)
    [[nodiscard]] std::vector<std::string> registered_command_names() const {
        std::vector<std::string> names;
        for (const auto* def : command_defs_) {
            names.push_back(def->name);
            for (const auto& alias : def->aliases) {
                names.push_back(alias);
            }
        }
        return names;
    }
};

} // namespace cc::commands
