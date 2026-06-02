/// @file mcp_cmd.cppm
/// @brief McpCommand implementing the /mcp slash command.
/// Lists MCP servers, adds/removes configurations, shows capabilities, restarts connections.
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
#include <unordered_map>
#include <array>

export module cc.commands.mcp_cmd;

import cc.types.types;
import cc.commands.command;
import cc.config.config;

export namespace cc::commands {

using namespace cc::core;

/// Runtime state of an MCP server connection.
/// Kept command-local so cc_commands can render /mcp command state without
/// linking the larger services target.
enum class McpServerState : std::uint8_t {
    NotStarted,
    Starting,
    Initializing,
    Ready,
    ShuttingDown,
    Stopped,
    Error,
};

struct McpServerCapabilities {
    bool tools = false;
    bool resources = false;
    bool prompts = false;
    bool logging = false;
};

struct McpToolInfo {
    std::string name;
    std::string description;
};

struct McpResourceInfo {
    std::string uri;
    std::string name;
    std::string mime_type;
};

struct McpPromptInfo {
    std::string name;
    std::string description;
    std::vector<std::string> arguments;
};

/// Subcommand for /mcp
enum class McpAction : std::uint8_t {
    List,       // List all connected MCP servers
    Add,        // Add a new MCP server configuration
    Remove,     // Remove an MCP server configuration
    Show,       // Show a server's capabilities
    Restart,    // Restart a server connection
};

/// Runtime state of an MCP server connection
struct McpServerStatus {
    std::string name;
    McpServerState state;
    std::optional<McpServerCapabilities> capabilities;
    std::vector<McpToolInfo> tools;
    std::vector<McpResourceInfo> resources;
    std::vector<McpPromptInfo> prompts;
};

/// McpCommand implements the /mcp slash command.
/// Manages MCP server connections and configurations.
class McpCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "mcp",
            .description = "Manage Model Context Protocol server connections",
            .aliases = {},
            .args = {
                CommandArg{.name = "action", .description = "Subcommand: list, add, remove, show, restart",
                           .type = ArgType::Choice, .required = false,
                           .choices = {{"list", "add", "remove", "show", "restart"}}},
                CommandArg{.name = "server", .description = "Server name (for add/remove/show/restart)",
                           .type = ArgType::Text, .required = false},
            },
            .hidden = false,
            .category = "tools",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};  // Default to 'list'

        auto action = parse_action(ctx.args[0]);
        if (!action) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                std::format("Unknown MCP action: '{}'. Use: list, add, remove, show, restart",
                           ctx.args[0])
            ));
        }

        // Actions that require a server name
        if (*action != McpAction::List && ctx.args.size() < 2) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                std::format("/mcp {} requires a server name", ctx.args[0])
            ));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto action = ctx.args.empty()
            ? McpAction::List
            : parse_action(ctx.args[0]).value_or(McpAction::List);

        switch (action) {
            case McpAction::List:    return execute_list();
            case McpAction::Add:     return execute_add(ctx.args);
            case McpAction::Remove:  return execute_remove(ctx.args[1]);
            case McpAction::Show:    return execute_show(ctx.args[1]);
            case McpAction::Restart: return execute_restart(ctx.args[1]);
        }
        return CommandResult::fail("Unknown MCP action");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        static constexpr std::array actions = {"list", "add", "remove", "show", "restart"};
        std::vector<std::string> suggestions;
        for (auto act : actions) {
            if (std::string_view(act).starts_with(partial)) {
                suggestions.emplace_back(act);
            }
        }
        // Also suggest known server names
        for (const auto& server : connected_servers_) {
            if (server.name.starts_with(partial)) {
                suggestions.push_back(server.name);
            }
        }
        return suggestions;
    }

private:
    std::vector<McpServerStatus> connected_servers_;
    ConfigManager config_manager_;

    /// Parse action string to enum
    [[nodiscard]] static std::optional<McpAction> parse_action(std::string_view str) {
        if (str == "list" || str == "ls") return McpAction::List;
        if (str == "add")                 return McpAction::Add;
        if (str == "remove" || str == "rm") return McpAction::Remove;
        if (str == "show" || str == "info") return McpAction::Show;
        if (str == "restart")             return McpAction::Restart;
        return std::nullopt;
    }

    /// Convert server state to display string
    [[nodiscard]] static std::string_view state_label(McpServerState state) {
        using enum McpServerState;
        switch (state) {
            case NotStarted:   return "not started";
            case Starting:     return "starting...";
            case Initializing: return "initializing...";
            case Ready:        return "ready";
            case ShuttingDown: return "shutting down...";
            case Stopped:      return "stopped";
            case Error:        return "error";
        }
        return "unknown";
    }

    /// List all connected MCP servers
    [[nodiscard]] Result<CommandResult> execute_list() {
        if (connected_servers_.empty()) {
            return CommandResult::success(
                "No MCP servers configured.\n"
                "Use `/mcp add <name> <command> [args...]` to add a server."
            );
        }

        std::string output = "MCP Servers:\n\n";
        output += std::format("  {:<20} {:<12} {:<6} {:<6} {:<6}\n",
                             "Name", "Status", "Tools", "Res.", "Prompts");
        output += std::string(60, '-') + "\n";

        for (const auto& server : connected_servers_) {
            output += std::format("  {:<20} {:<12} {:<6} {:<6} {:<6}\n",
                server.name,
                state_label(server.state),
                server.tools.size(),
                server.resources.size(),
                server.prompts.size()
            );
        }
        return CommandResult::success(std::move(output));
    }

    /// Add a new MCP server configuration
    [[nodiscard]] Result<CommandResult> execute_add(std::span<const std::string> args) {
        // args: ["add", name, command, ...cmd_args]
        if (args.size() < 3) {
            return CommandResult::fail("Usage: /mcp add <name> <command> [args...]");
        }

        McpServerConfig config{
            .name = args[1],
            .command = args[2],
            .args = {},
            .env = {},
        };

        // Collect additional command arguments
        for (std::size_t i = 3; i < args.size(); ++i) {
            config.args.push_back(args[i]);
        }

        // Add to configuration
        config_manager_.settings_mut().mcp_servers.push_back(std::move(config));
        if (auto result = config_manager_.save(); !result) {
            return std::unexpected(result.error());
        }

        return CommandResult::success(
            std::format("Added MCP server '{}'. Use `/mcp restart {}` to connect.", args[1], args[1])
        );
    }

    /// Remove an MCP server configuration
    [[nodiscard]] Result<CommandResult> execute_remove(std::string_view name) {
        auto& servers = config_manager_.settings_mut().mcp_servers;
        auto it = std::ranges::find_if(servers, [name](const auto& s) {
            return s.name == name;
        });

        if (it == servers.end()) {
            return CommandResult::fail(std::format("MCP server '{}' not found", name));
        }

        servers.erase(it);

        // Also remove from active connections
        std::erase_if(connected_servers_, [name](const auto& s) { return s.name == name; });

        if (auto result = config_manager_.save(); !result) {
            return std::unexpected(result.error());
        }

        return CommandResult::success(std::format("Removed MCP server '{}'", name));
    }

    /// Show detailed capabilities of a server
    [[nodiscard]] Result<CommandResult> execute_show(std::string_view name) {
        auto it = std::ranges::find_if(connected_servers_, [name](const auto& s) {
            return s.name == name;
        });

        if (it == connected_servers_.end()) {
            return CommandResult::fail(std::format("MCP server '{}' not connected", name));
        }

        std::string output = std::format("MCP Server: {}\n", it->name);
        output += std::format("Status: {}\n\n", state_label(it->state));

        // Tools
        output += std::format("Tools ({}):\n", it->tools.size());
        for (const auto& tool : it->tools) {
            output += std::format("  - {}: {}\n", tool.name, tool.description);
        }

        // Resources
        output += std::format("\nResources ({}):\n", it->resources.size());
        for (const auto& res : it->resources) {
            output += std::format("  - {} [{}]: {}\n", res.name, res.mime_type, res.uri);
        }

        // Prompts
        output += std::format("\nPrompts ({}):\n", it->prompts.size());
        for (const auto& prompt : it->prompts) {
            output += std::format("  - {}: {} ({} args)\n",
                prompt.name, prompt.description, prompt.arguments.size());
        }

        return CommandResult::success(std::move(output));
    }

    /// Restart a server connection
    [[nodiscard]] Result<CommandResult> execute_restart(std::string_view name) {
        // Find in configuration
        const auto& servers = config_manager_.settings().mcp_servers;
        auto config_it = std::ranges::find_if(servers, [name](const auto& s) {
            return s.name == name;
        });

        if (config_it == servers.end()) {
            return CommandResult::fail(std::format("MCP server '{}' not configured", name));
        }

        auto it = std::ranges::find_if(connected_servers_, [name](const auto& s) {
            return s.name == name;
        });

        if (it != connected_servers_.end()) {
            it->state = McpServerState::Starting;
        }

        return CommandResult::success(std::format("Restarting MCP server '{}'...", name));
    }
};

} // namespace cc::commands
