module;
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>
export module cc.services.plugins.cli_commands;

export namespace cc::services::plugins {

// Plugin command descriptor
struct PluginCommand {
    std::string name;
    std::string description;
};

// Handle a plugin CLI subcommand
auto handle_plugin_command(std::string_view subcommand, std::span<const std::string> args)
    -> std::expected<std::string, std::string> {
    if (subcommand == "list") {
        return std::string("No plugins installed.");
    }
    if (subcommand == "install") {
        if (args.empty()) {
            return std::unexpected("Usage: plugin install <plugin-id>");
        }
        return std::string("Installed plugin: ") + std::string(args[0]);
    }
    if (subcommand == "uninstall") {
        if (args.empty()) {
            return std::unexpected("Usage: plugin uninstall <plugin-id>");
        }
        return std::string("Uninstalled plugin: ") + std::string(args[0]);
    }
    if (subcommand == "update") {
        if (args.empty()) {
            return std::unexpected("Usage: plugin update <plugin-id>");
        }
        return std::string("Updated plugin: ") + std::string(args[0]);
    }
    return std::unexpected("Unknown plugin command: " + std::string(subcommand));
}

// Get available plugin commands
auto get_plugin_commands() -> std::vector<PluginCommand> {
    return {
        {"list", "List installed plugins"},
        {"install", "Install a plugin by ID"},
        {"uninstall", "Remove an installed plugin"},
        {"update", "Update a plugin to latest version"},
    };
}

} // namespace cc::services::plugins
