/// @file agents.cppm
/// @brief AgentsCommand implementing the /agents slash command.
/// Manages agent configurations.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <span>

export module cc.commands.agents;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

struct AgentsOptions {
    std::optional<std::string> subcommand;
    std::optional<std::string> agent_name;
};

class AgentsCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "agents",
            .description = "Manage agent configurations",
            .args = {
                CommandArg{.name = "list", .description = "List available agents", .type = ArgType::None, .required = false},
                CommandArg{.name = "use", .description = "Select an agent to use", .type = ArgType::Text, .required = false},
                CommandArg{.name = "configure", .description = "Configure an agent", .type = ArgType::Text, .required = false},
            },
            .category = "agents",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto opts = parse_options(ctx.args);
        
        if (!opts.subcommand) {
            return CommandResult::success("Available commands:\n  /agents list - List available agents\n  /agents use <agent> - Select an agent\n  /agents configure <agent> - Configure an agent");
        }
        
        if (*opts.subcommand == "list") {
            return CommandResult::success("Available agents: default, fast, expert");
        }
        
        if (*opts.subcommand == "use" && opts.agent_name) {
            return CommandResult::success(std::format("Now using agent: {}", *opts.agent_name));
        }
        
        if (*opts.subcommand == "configure" && opts.agent_name) {
            return CommandResult::success(std::format("Configuring agent: {}", *opts.agent_name));
        }
        
        return CommandResult::fail("Invalid command");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        static constexpr std::array subcommands = {"list", "use", "configure"};
        for (auto cmd : subcommands) {
            if (std::string_view(cmd).starts_with(partial)) {
                suggestions.emplace_back(cmd);
            }
        }
        return suggestions;
    }

private:
    [[nodiscard]] static AgentsOptions parse_options(std::span<const std::string> args) {
        AgentsOptions opts;
        if (!args.empty()) {
            opts.subcommand = args[0];
            if (args.size() > 1) {
                opts.agent_name = args[1];
            }
        }
        return opts;
    }
};

} // namespace cc::commands
