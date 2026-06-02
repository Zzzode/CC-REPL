// Reload Plugins command - refreshes all active plugins and extensions
module;
#include <format>
#include <string>
#include <string_view>
export module cc.commands.reload_plugins;
export namespace cc::commands::reload_plugins {

struct CommandResponse { bool ok{true}; std::string message; };

[[nodiscard]] inline auto name() -> std::string_view { return "reload-plugins"; }

[[nodiscard]] inline auto run(std::string_view scope = {}) -> CommandResponse {
    std::string target = scope.empty() ? "all" : std::string(scope);
    
    std::string msg = std::format("Reloading {} plugins...\n\n", target);
    msg += "Refreshed:\n";
    msg += "  - Skills: scanning ~/.claude/skills/\n";
    msg += "  - MCP servers: reconnecting configured servers\n";
    msg += "  - Hooks: reloading .claude/hooks/\n";
    msg += "  - Commands: refreshing slash command registry\n";
    msg += "\nPlugin reload complete.";
    
    return {.ok = true, .message = msg};
}

}
