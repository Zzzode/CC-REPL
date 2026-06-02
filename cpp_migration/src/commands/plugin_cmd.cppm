/// @file plugin_cmd.cppm
/// @brief PluginCommand implementing the /plugin slash command.
/// Install/uninstall plugins, list, enable/disable, show plugin info.
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

export module cc.commands.plugin_cmd;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// Plugin state information
struct PluginInfo {
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    bool enabled = true;
    bool installed = true;
};

/// PluginCommand implements the /plugin slash command.
/// Manages plugin lifecycle: install, uninstall, enable, disable, info.
class PluginCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "plugin",
            .description = "Manage plugins",
            .aliases = {"plugins"},
            .args = {
                CommandArg{.name = "action",
                           .description = "list | install | uninstall | enable | disable | info",
                           .type = ArgType::Choice, .required = false,
                           .choices = {{"list", "install", "uninstall", "enable", "disable", "info"}}},
                CommandArg{.name = "plugin_name", .description = "Plugin name or package specifier",
                           .type = ArgType::Text, .required = false},
            },
            .hidden = false,
            .category = "tools",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};
        auto action = ctx.args[0];
        static constexpr std::array valid = {"list", "install", "uninstall", "enable", "disable", "info"};
        if (std::ranges::find(valid, action) == valid.end()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Invalid action: '{}'. Use: list|install|uninstall|enable|disable|info", action)));
        }
        if (action != "list" && ctx.args.size() < 2) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Usage: /plugin {} <plugin_name>", action)));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) return CommandResult::success(format_list());

        auto action = std::string(ctx.args[0]);

        if (action == "list") return CommandResult::success(format_list());
        if (action == "info") return show_info(std::string(ctx.args[1]));
        if (action == "install") return install_plugin(std::string(ctx.args[1]));
        if (action == "uninstall") return uninstall_plugin(std::string(ctx.args[1]));
        if (action == "enable") return set_enabled(std::string(ctx.args[1]), true);
        if (action == "disable") return set_enabled(std::string(ctx.args[1]), false);

        return CommandResult::success(format_list());
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        for (auto s : {"list", "install", "uninstall", "enable", "disable", "info"}) {
            if (std::string_view(s).starts_with(partial)) {
                suggestions.emplace_back(s);
            }
        }
        for (const auto& p : plugins_) {
            if (p.name.starts_with(partial)) {
                suggestions.push_back(p.name);
            }
        }
        return suggestions;
    }

private:
    std::vector<PluginInfo> plugins_;

    [[nodiscard]] std::string format_list() const {
        if (plugins_.empty()) {
            return "No plugins installed.\nUse /plugin install <name> to add one.";
        }
        std::string out = "Installed plugins:\n";
        for (const auto& p : plugins_) {
            auto status = p.enabled ? "●" : "○";
            out += std::format("  {} {} v{} by {} — {}\n",
                status, p.name, p.version, p.author, p.description);
        }
        return out;
    }

    [[nodiscard]] Result<CommandResult> show_info(const std::string& name) const {
        auto it = std::ranges::find_if(plugins_, [&](const auto& p) { return p.name == name; });
        if (it == plugins_.end()) {
            return std::unexpected(Error::make(ErrorCode::ToolNotFound,
                std::format("Plugin '{}' not found.", name)));
        }
        return CommandResult::success(std::format(
            "Plugin: {}\n  Version: {}\n  Author: {}\n  Status: {}\n  Description: {}",
            it->name, it->version, it->author,
            it->enabled ? "enabled" : "disabled", it->description));
    }

    [[nodiscard]] Result<CommandResult> install_plugin(const std::string& name) {
        // Check if already installed
        if (std::ranges::any_of(plugins_, [&](const auto& p) { return p.name == name; })) {
            return CommandResult::success(std::format("Plugin '{}' is already installed.", name));
        }
        // In production: download and install plugin package
        plugins_.push_back(PluginInfo{
            .name = name, .version = "1.0.0", .author = "unknown",
            .description = "Newly installed plugin", .enabled = true, .installed = true});
        return CommandResult::success(std::format("Plugin '{}' installed successfully.", name));
    }

    [[nodiscard]] Result<CommandResult> uninstall_plugin(const std::string& name) {
        auto it = std::ranges::find_if(plugins_, [&](const auto& p) { return p.name == name; });
        if (it == plugins_.end()) {
            return std::unexpected(Error::make(ErrorCode::ToolNotFound,
                std::format("Plugin '{}' not found.", name)));
        }
        plugins_.erase(it);
        return CommandResult::success(std::format("Plugin '{}' uninstalled.", name));
    }

    [[nodiscard]] Result<CommandResult> set_enabled(const std::string& name, bool enabled) {
        auto it = std::ranges::find_if(plugins_, [&](const auto& p) { return p.name == name; });
        if (it == plugins_.end()) {
            return std::unexpected(Error::make(ErrorCode::ToolNotFound,
                std::format("Plugin '{}' not found.", name)));
        }
        it->enabled = enabled;
        return CommandResult::success(std::format("Plugin '{}' {}.",
            name, enabled ? "enabled" : "disabled"));
    }
};

} // namespace cc::commands
