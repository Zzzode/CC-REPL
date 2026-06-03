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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

export module cc.commands.plugin_cmd;

import cc.types.types;
import cc.commands.command;
import cc.utils.plugin_lifecycle;

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
                           .choices = {"list", "install", "uninstall", "enable", "disable", "info"}},
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

    [[nodiscard]] static std::filesystem::path plugins_dir() {
        if (const char* home = std::getenv("HOME")) {
            return std::filesystem::path(home) / ".cc-repl" / "plugins";
        }
        return std::filesystem::temp_directory_path() / "cc-repl" / "plugins";
    }

    [[nodiscard]] static std::optional<std::string> extract_json_string(std::string_view json, std::string_view key) {
        auto marker = std::string{"\""} + std::string(key) + "\":\"";
        auto pos = json.find(marker);
        if (pos == std::string_view::npos) return std::nullopt;
        pos += marker.size();
        std::string value;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) ++pos;
            value.push_back(json[pos++]);
        }
        return value;
    }

    [[nodiscard]] static std::vector<PluginInfo> load_installed_plugins() {
        namespace fs = std::filesystem;
        std::vector<PluginInfo> plugins;
        auto root = plugins_dir();
        std::error_code ec;
        if (!fs::exists(root, ec)) return plugins;

        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (!entry.is_directory()) continue;
            auto name = entry.path().filename().string();
            if (name.starts_with(".")) continue;

            PluginInfo info{
                .name = name,
                .version = "unknown",
                .author = "unknown",
                .description = "Installed plugin",
                .enabled = !fs::exists(entry.path() / ".disabled", ec),
                .installed = true,
            };

            auto manifest_path = entry.path() / "manifest.json";
            if (fs::exists(manifest_path, ec)) {
                std::ifstream input(manifest_path);
                std::string manifest((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
                if (auto version = extract_json_string(manifest, "version")) info.version = *version;
                if (auto author = extract_json_string(manifest, "author")) info.author = *author;
                if (auto description = extract_json_string(manifest, "description")) info.description = *description;
            }

            plugins.push_back(std::move(info));
        }
        std::ranges::sort(plugins, {}, &PluginInfo::name);
        return plugins;
    }

    [[nodiscard]] std::string format_list() const {
        auto plugins = load_installed_plugins();
        if (plugins.empty()) {
            return "No plugins installed.\nUse /plugin install <name> to add one.";
        }
        std::string out = "Installed plugins:\n";
        for (const auto& p : plugins) {
            auto status = p.enabled ? "●" : "○";
            out += std::format("  {} {} v{} by {} — {}\n",
                status, p.name, p.version, p.author, p.description);
        }
        return out;
    }

    [[nodiscard]] Result<CommandResult> show_info(const std::string& name) const {
        auto plugins = load_installed_plugins();
        auto it = std::ranges::find_if(plugins, [&](const auto& p) { return p.name == name; });
        if (it == plugins.end()) {
            return std::unexpected(Error::make(ErrorCode::ToolNotFound,
                std::format("Plugin '{}' not found.", name)));
        }
        return CommandResult::success(std::format(
            "Plugin: {}\n  Version: {}\n  Author: {}\n  Status: {}\n  Description: {}",
            it->name, it->version, it->author,
            it->enabled ? "enabled" : "disabled", it->description));
    }

    [[nodiscard]] Result<CommandResult> install_plugin(const std::string& name) {
        auto plugins = load_installed_plugins();
        if (std::ranges::any_of(plugins, [&](const auto& p) { return p.name == name; })) {
            return CommandResult::success(std::format("Plugin '{}' is already installed.", name));
        }
        auto result = cc::utils::plugins::install_plugin(name);
        if (!result) {
            return std::unexpected(Error::make(ErrorCode::InternalError, result.error()));
        }
        plugins_ = load_installed_plugins();
        return CommandResult::success(std::format(
            "Plugin '{}' installed successfully in {}ms.",
            result->plugin_id,
            result->duration.count()));
    }

    [[nodiscard]] Result<CommandResult> uninstall_plugin(const std::string& name) {
        auto result = cc::utils::plugins::uninstall_plugin(name);
        if (!result) {
            return std::unexpected(Error::make(ErrorCode::ToolNotFound, result.error()));
        }
        plugins_ = load_installed_plugins();
        return CommandResult::success(std::format("Plugin '{}' uninstalled.", name));
    }

    [[nodiscard]] Result<CommandResult> set_enabled(const std::string& name, bool enabled) {
        namespace fs = std::filesystem;
        auto plugin_path = plugins_dir() / name;
        std::error_code ec;
        if (!fs::exists(plugin_path, ec)) {
            return std::unexpected(Error::make(ErrorCode::ToolNotFound,
                std::format("Plugin '{}' not found.", name)));
        }
        auto marker = plugin_path / ".disabled";
        if (enabled) {
            fs::remove(marker, ec);
            if (ec) {
                return std::unexpected(Error::make(ErrorCode::InternalError,
                    std::format("Failed to enable plugin '{}': {}", name, ec.message())));
            }
        } else {
            std::ofstream output(marker);
            if (!output.is_open()) {
                return std::unexpected(Error::make(ErrorCode::InternalError,
                    std::format("Failed to disable plugin '{}'.", name)));
            }
            output << "disabled\n";
        }
        plugins_ = load_installed_plugins();
        return CommandResult::success(std::format("Plugin '{}' {}.",
            name, enabled ? "enabled" : "disabled"));
    }
};

} // namespace cc::commands
