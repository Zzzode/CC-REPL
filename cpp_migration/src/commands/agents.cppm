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
#include <array>
#include <sstream>

export module cc.commands.agents;

import cc.types.types;
import cc.commands.command;
import cc.tools.agent_runtime;

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
        (void)ctx;
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto opts = parse_options(ctx.args);
        
        if (!opts.subcommand) {
            return CommandResult::success("Available commands:\n  /agents list - List available agents\n  /agents use <agent> - Select an agent\n  /agents configure <agent> - Configure an agent");
        }

        auto agents = cc::tools::agent_runtime::get_all_agent_definitions();
        
        if (*opts.subcommand == "list") {
            return CommandResult::success(format_agent_list(agents));
        }
        
        if (*opts.subcommand == "use" && opts.agent_name) {
            auto* agent = find_agent(agents, *opts.agent_name);
            if (!agent) {
                return CommandResult::fail(std::format("Unknown agent: {}", *opts.agent_name));
            }
            return CommandResult::success(std::format(
                "Agent '{}' is available from {}. Invoke it with Agent subagent_type='{}'.",
                agent->agent_type,
                agent->source,
                agent->agent_type));
        }
        
        if (*opts.subcommand == "configure" && opts.agent_name) {
            auto* agent = find_agent(agents, *opts.agent_name);
            if (!agent) {
                return CommandResult::fail(std::format("Unknown agent: {}", *opts.agent_name));
            }
            return CommandResult::success(format_agent_details(*agent));
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
        for (const auto& agent : cc::tools::agent_runtime::get_all_agent_definitions()) {
            if (std::string_view(agent.agent_type).starts_with(partial)) {
                suggestions.push_back(agent.agent_type);
            }
        }
        return suggestions;
    }

private:
    [[nodiscard]] static const cc::tools::agent_runtime::AgentDefinition* find_agent(
        const std::vector<cc::tools::agent_runtime::AgentDefinition>& agents,
        std::string_view agent_name
    ) {
        for (const auto& agent : agents) {
            if (agent.agent_type == agent_name) return &agent;
        }
        return nullptr;
    }

    [[nodiscard]] static std::string format_agent_list(
        const std::vector<cc::tools::agent_runtime::AgentDefinition>& agents
    ) {
        if (agents.empty()) return "No agents available.";

        std::string output = "Available agents:\n";
        for (const auto& agent : agents) {
            output += std::format("  - {} [{}] {}\n", agent.agent_type, agent.source, agent.when_to_use);
        }
        return output;
    }

    [[nodiscard]] static std::string format_agent_details(
        const cc::tools::agent_runtime::AgentDefinition& agent
    ) {
        std::ostringstream out;
        out << "Agent: " << agent.agent_type << "\n";
        out << "Source: " << agent.source << "\n";
        out << "Description: " << agent.when_to_use << "\n";
        out << "Model: " << agent.model << "\n";
        if (agent.path) {
            out << "Path: " << *agent.path << "\n";
        }
        if (agent.max_turns) {
            out << "Max turns: " << *agent.max_turns << "\n";
        }
        out << "Tools:";
        if (agent.tools.empty()) {
            out << " inherit\n";
        } else {
            out << "\n";
            for (const auto& tool : agent.tools) {
                out << "  - " << tool << "\n";
            }
        }
        return out.str();
    }

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
