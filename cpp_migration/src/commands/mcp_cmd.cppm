/// @file mcp_cmd.cppm
/// @brief McpCommand implementing the /mcp slash command.
/// Lists MCP servers, adds/removes configurations, shows capabilities, restarts connections.
module;

#include <cstdint>
#include <cstddef>
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
#include <utility>

export module cc.commands.mcp_cmd;

import cc.types.types;
import cc.commands.command;
import cc.config.config;
import cc.tools.mcp;

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
                           .choices = {"list", "add", "remove", "show", "restart"}},
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
    bool config_loaded_ = false;

    [[nodiscard]] VoidResult ensure_config_loaded() {
        if (config_loaded_) return {};
        auto loaded = config_manager_.load();
        if (!loaded) return loaded;
        config_loaded_ = true;
        return {};
    }

    [[nodiscard]] VoidResult sync_native_runtime() {
        if (auto loaded = ensure_config_loaded(); !loaded) return loaded;

	std::vector<cc::tools::NativeMcpConfiguredServer> servers;
	for (const auto& server : config_manager_.settings().mcp_servers) {
	    servers.push_back(cc::tools::to_native_mcp_server(server));
	}

        auto synced = cc::tools::sync_native_mcp_servers(std::move(servers));
        if (!synced) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError,
                synced.error()
            ));
        }
        return {};
    }

    /// Parse action string to enum
    [[nodiscard]] static std::optional<McpAction> parse_action(std::string_view str) {
        if (str == "list" || str == "ls") return McpAction::List;
        if (str == "add")                 return McpAction::Add;
        if (str == "remove" || str == "rm") return McpAction::Remove;
        if (str == "show" || str == "info") return McpAction::Show;
	if (str == "restart")             return McpAction::Restart;
	return std::nullopt;
    }

    [[nodiscard]] static bool starts_with_http_url(std::string_view value) {
	return value.starts_with("http://") || value.starts_with("https://");
    }

    [[nodiscard]] static bool is_remote_transport(std::string_view transport) {
	return transport == "sse" || transport == "http" ||
	       transport == "streamable-http" || transport == "streamableHttp";
    }

    static void normalize_transport(McpServerConfig& config) {
	if (config.transport == "streamable-http" || config.transport == "streamableHttp") {
	    config.transport = "http";
	}
	if (config.transport.empty()) {
	    config.transport = config.url ? "http" : "stdio";
	}
    }

    [[nodiscard]] static McpOAuthConfig& ensure_oauth(McpServerConfig& config) {
	if (!config.oauth) config.oauth = McpOAuthConfig{};
	return *config.oauth;
    }

    [[nodiscard]] static std::optional<std::pair<std::string, std::string>>
    parse_header_assignment(std::string_view value) {
	const auto eq = value.find('=');
	if (eq == std::string_view::npos || eq == 0) return std::nullopt;
	return std::pair{std::string(value.substr(0, eq)), std::string(value.substr(eq + 1))};
    }

    [[nodiscard]] static Result<int> parse_port(std::string_view value) {
	int port = 0;
	for (char ch : value) {
	    if (ch < '0' || ch > '9') {
		return std::unexpected(Error::make(
		    ErrorCode::InvalidRequest,
		    std::format("Invalid callback port: {}", value)));
	    }
	    port = port * 10 + (ch - '0');
	}
	if (port <= 0 || port > 65535) {
	    return std::unexpected(Error::make(
		ErrorCode::InvalidRequest,
		std::format("Invalid callback port: {}", value)));
	}
	return port;
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
        if (auto synced = sync_native_runtime(); !synced) {
            return std::unexpected(synced.error());
        }

        const auto& configured_servers = config_manager_.settings().mcp_servers;
	if (configured_servers.empty() && connected_servers_.empty()) {
	    return CommandResult::success(
		"No MCP servers configured.\n"
		"Use `/mcp add <name> <command> [args...]` or `/mcp add <name> http --url <url>` to add a server."
	    );
	}

	std::string output = "MCP Servers:\n\n";
	output += std::format("  {:<20} {:<8} {:<12} {:<6} {:<6} {:<6}\n",
			     "Name", "Type", "Status", "Tools", "Res.", "Prompts");
	output += std::string(70, '-') + "\n";

        for (const auto& config : configured_servers) {
            const auto status = cc::tools::native_mcp_status(config.name);
            const auto status_text = status ? status->status : std::string("not started");
            const auto tools = status ? status->tools.size() : std::size_t{0};
            const auto resources = status ? status->resources.size() : std::size_t{0};
            const auto prompts = status ? status->prompts.size() : std::size_t{0};
	    output += std::format("  {:<20} {:<8} {:<12} {:<6} {:<6} {:<6}\n",
		config.name,
		config.transport.empty() ? std::string("stdio") : config.transport,
		status_text,
		tools,
                resources,
                prompts
            );
        }
        return CommandResult::success(std::move(output));
    }

    /// Add a new MCP server configuration
    [[nodiscard]] Result<CommandResult> execute_add(std::span<const std::string> args) {
	// args: ["add", name, command, ...cmd_args] or ["add", name, transport, --url, url]
	if (args.size() < 3) {
	    return CommandResult::fail(
		"Usage: /mcp add <name> <command> [args...] or /mcp add <name> <sse|http> --url <url> [--header K=V] [--headers-helper <command>] [--oauth-metadata-url <url>] [--oauth-client-id <id>] [--oauth-callback-port <port>] [--oauth-xaa]"
	    );
	}
	if (auto loaded = ensure_config_loaded(); !loaded) {
	    return std::unexpected(loaded.error());
	}

	McpServerConfig config;
	config.name = args[1];
	std::size_t index = 2;
	if (args[index] == "--transport" || args[index] == "-t") {
	    if (index + 1 >= args.size()) return CommandResult::fail("--transport requires a value");
	    config.transport = args[index + 1];
	    index += 2;
	} else if (is_remote_transport(args[index])) {
	    config.transport = args[index];
	    index += 1;
	} else if (starts_with_http_url(args[index])) {
	    config.transport = "http";
	    config.url = args[index];
	    index += 1;
	} else {
	    config.transport = "stdio";
	    config.command = args[index];
	    index += 1;
	}
	normalize_transport(config);

	while (index < args.size()) {
	    const auto& token = args[index];
	    if (token == "--transport" || token == "-t") {
		if (index + 1 >= args.size()) return CommandResult::fail("--transport requires a value");
		config.transport = args[index + 1];
		normalize_transport(config);
		index += 2;
	    } else if (token == "--url") {
		if (index + 1 >= args.size()) return CommandResult::fail("--url requires a value");
		config.url = args[index + 1];
		if (config.transport == "stdio") config.transport = "http";
		index += 2;
	    } else if (token == "--header") {
		if (index + 1 >= args.size()) return CommandResult::fail("--header requires K=V");
		auto header = parse_header_assignment(args[index + 1]);
		if (!header) return CommandResult::fail("--header requires K=V");
		config.headers[header->first] = header->second;
		index += 2;
	    } else if (token == "--headers-helper") {
		if (index + 1 >= args.size()) return CommandResult::fail("--headers-helper requires a command");
		config.headers_helper = args[index + 1];
		index += 2;
	    } else if (token == "--oauth-metadata-url") {
		if (index + 1 >= args.size()) return CommandResult::fail("--oauth-metadata-url requires a URL");
		ensure_oauth(config).auth_server_metadata_url = args[index + 1];
		index += 2;
	    } else if (token == "--oauth-client-id") {
		if (index + 1 >= args.size()) return CommandResult::fail("--oauth-client-id requires a value");
		ensure_oauth(config).client_id = args[index + 1];
		index += 2;
	    } else if (token == "--oauth-callback-port") {
		if (index + 1 >= args.size()) return CommandResult::fail("--oauth-callback-port requires a port");
		auto port = parse_port(args[index + 1]);
		if (!port) return std::unexpected(port.error());
		ensure_oauth(config).callback_port = *port;
		index += 2;
	    } else if (token == "--oauth-xaa") {
		ensure_oauth(config).xaa = true;
		index += 1;
	    } else if (config.transport == "stdio") {
		config.args.push_back(token);
		index += 1;
	    } else if (!config.url && starts_with_http_url(token)) {
		config.url = token;
		index += 1;
	    } else {
		return CommandResult::fail(std::format("Unexpected argument for remote MCP server: {}", token));
	    }
	}
	normalize_transport(config);
	if (config.transport == "stdio" && config.command.empty()) {
	    return CommandResult::fail("stdio MCP servers require a command");
	}
	if (is_remote_transport(config.transport) && (!config.url || config.url->empty())) {
	    return CommandResult::fail("remote MCP servers require --url <url>");
	}

	auto& servers = config_manager_.settings_mut().mcp_servers;
        auto existing = std::ranges::find_if(servers, [&](const auto& server) {
            return server.name == config.name;
        });
        if (existing == servers.end()) {
            servers.push_back(std::move(config));
        } else {
            *existing = std::move(config);
        }
        if (auto result = config_manager_.save(); !result) {
            return std::unexpected(result.error());
        }
        if (auto synced = sync_native_runtime(); !synced) {
            return std::unexpected(synced.error());
        }

        return CommandResult::success(
            std::format("Saved MCP server '{}'. Use `/mcp show {}` to inspect the native configuration.", args[1], args[1])
        );
    }

    /// Remove an MCP server configuration
    [[nodiscard]] Result<CommandResult> execute_remove(std::string_view name) {
        if (auto loaded = ensure_config_loaded(); !loaded) {
            return std::unexpected(loaded.error());
        }

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
        if (auto synced = sync_native_runtime(); !synced) {
            return std::unexpected(synced.error());
        }

        return CommandResult::success(std::format("Removed MCP server '{}'", name));
    }

    /// Show detailed capabilities of a server
    [[nodiscard]] Result<CommandResult> execute_show(std::string_view name) {
        if (auto synced = sync_native_runtime(); !synced) {
            return std::unexpected(synced.error());
        }

        const auto& configured_servers = config_manager_.settings().mcp_servers;
        auto config_it = std::ranges::find_if(configured_servers, [name](const auto& s) {
            return s.name == name;
        });
        if (config_it == configured_servers.end()) {
            return CommandResult::fail(std::format("MCP server '{}' not configured", name));
        }

	const auto status = cc::tools::native_mcp_status(name);

	std::string output = std::format("MCP Server: {}\n", config_it->name);
	output += std::format("Status: {}\n", status ? status->status : std::string("not started"));
	output += std::format("Type: {}\n", config_it->transport.empty() ? std::string("stdio") : config_it->transport);
	if (is_remote_transport(config_it->transport)) {
	    output += std::format("URL: {}\n", config_it->url.value_or(std::string{}));
	    if (!config_it->headers.empty()) {
		output += "Headers:\n";
		for (const auto& [key, value] : config_it->headers) {
		    output += std::format("  {}: {}\n", key, value);
		}
	    }
	    if (config_it->headers_helper) {
		output += std::format("Headers helper: {}\n", *config_it->headers_helper);
	    }
	    if (config_it->oauth) {
		std::vector<std::string> oauth_parts;
		if (config_it->oauth->auth_server_metadata_url) oauth_parts.push_back("metadata-url configured");
		if (config_it->oauth->client_id) oauth_parts.push_back("client-id configured");
		if (config_it->oauth->callback_port) {
		    oauth_parts.push_back(std::format("callback-port {}", *config_it->oauth->callback_port));
		}
		if (config_it->oauth->xaa) oauth_parts.push_back("xaa");
		output += "OAuth: ";
		for (std::size_t i = 0; i < oauth_parts.size(); ++i) {
		    if (i > 0) output += ", ";
		    output += oauth_parts[i];
		}
		output += "\n";
	    }
	} else {
	    output += std::format("Command: {}", config_it->command);
	    for (const auto& arg : config_it->args) {
		output += std::format(" {}", arg);
	    }
	    output += "\n";
	}
	output += "\n";
        if (status && status->error) {
            output += std::format("Last error: {}\n\n", *status->error);
        }
        if (status && status->server_info) {
            output += std::format("Server: {}\n", *status->server_info);
        }
        if (status && status->capabilities) {
            output += std::format("Capabilities: {}\n\n", *status->capabilities);
        }

        if (!status || status->status != "ready") {
            output += "Runtime capabilities are unavailable until this server is started with `/mcp restart` or first used by an MCP tool.\n";
            return CommandResult::success(std::move(output));
        }

        // Tools
        output += std::format("Tools ({}):\n", status->tools.size());
        for (const auto& tool : status->tools) {
            output += std::format("  - {}: {}\n", tool.name, tool.description);
        }

        // Resources
        output += std::format("\nResources ({}):\n", status->resources.size());
        for (const auto& res : status->resources) {
            output += std::format("  - {} [{}]: {}\n", res.name, res.mime_type, res.uri);
        }

        // Prompts
        output += std::format("\nPrompts ({}):\n", status->prompts.size());
        for (const auto& prompt : status->prompts) {
            output += std::format("  - {}: {} ({} args)\n",
                prompt.name, prompt.description, prompt.arguments.size());
        }

        return CommandResult::success(std::move(output));
    }

    /// Restart a server connection
    [[nodiscard]] Result<CommandResult> execute_restart(std::string_view name) {
        if (auto synced = sync_native_runtime(); !synced) {
            return std::unexpected(synced.error());
        }

        const auto& servers = config_manager_.settings().mcp_servers;
        auto config_it = std::ranges::find_if(servers, [name](const auto& s) {
            return s.name == name;
        });

        if (config_it == servers.end()) {
            return CommandResult::fail(std::format("MCP server '{}' not configured", name));
        }

        auto restarted = cc::tools::restart_native_mcp_server(name);
        if (!restarted) {
            return CommandResult::fail(restarted.error());
        }

        return CommandResult::success(std::format(
            "MCP server '{}' restarted: {} (tools={}, resources={}, prompts={})",
            name,
            restarted->status,
            restarted->tools.size(),
            restarted->resources.size(),
            restarted->prompts.size()
        ));
    }
};

} // namespace cc::commands
