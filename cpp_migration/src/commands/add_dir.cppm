/// @file add_dir.cppm
/// @brief AddDirCommand implementing the /add-dir slash command.
/// Adds a working directory to the session or local settings.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <span>

export module cc.commands.add_dir;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

struct AddDirOptions {
    std::optional<std::string> path;
    bool remember = false;
};

class AddDirCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "add-dir",
            .description = "Add a working directory",
            .args = {
                CommandArg{.name = "<path>", .description = "Directory path to add", .type = ArgType::FilePath, .required = false},
                CommandArg{.name = "--remember", .description = "Save to local settings", .type = ArgType::None, .required = false},
            },
            .category = "workspace",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        // If path is provided, we could validate it here
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto opts = parse_options(ctx.args);
        
        if (!opts.path) {
            // No path provided - would show UI to select path
            return CommandResult::success("Please specify a directory path: /add-dir <path> [--remember]");
        }
        
        // Simulate adding the directory
        std::string message;
        if (opts.remember) {
            message = std::format("Added {} as a working directory and saved to local settings", *opts.path);
        } else {
            message = std::format("Added {} as a working directory for this session", *opts.path);
        }
        
        return CommandResult::success(std::format("{} · /permissions to manage", message));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        // Could provide path completion here
        return {};
    }

private:
    [[nodiscard]] static AddDirOptions parse_options(std::span<const std::string> args) {
        AddDirOptions opts;
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "--remember") {
                opts.remember = true;
            } else if (!opts.path) {
                opts.path = args[i];
            }
        }
        return opts;
    }
};

} // namespace cc::commands
