/// @file plugin_cmd.cppm
/// @brief PluginCommand — implementation of the `/plugin` slash command.
///
/// Provides the full subcommand surface area translated from the TS
/// `src/commands/plugin/` tree:
///
///   • /plugin (no args)            → text overview + hint for TUI entry
///   • /plugin help                 → full help text
///   • /plugin install [<spec>]     → install a plugin / browse install UI
///   • /plugin manage               → manage installed plugins (text list)
///   • /plugin enable  <plugin>     → enable  an installed plugin
///   • /plugin disable <plugin>     → disable an installed plugin
///   • /plugin uninstall <plugin>   → remove an installed plugin
///   • /plugin validate <path>      → validate a manifest file or directory
///   • /plugin marketplace <sub>    → add / remove / update / list marketplaces
///   • /plugin info <plugin>        → show plugin metadata (kept for compat)
///   • /plugin list                 → synonym for "manage" (text list)
///
/// Aliases: /plugins, /marketplace  (registered via definition().aliases)
///
/// Subcommands that require interactive selection (e.g. browse install UI,
/// tabbed settings) delegate to Phase 4's FTXUI layer by returning a
/// `CommandResult` whose `metadata` field contains the string
/// `"UI:plugins:<view>"` — the runtime detects this and spawns the dialog.
/// No FTXUI code lives in this module.
///
/// Heavy plugin work (actual install/uninstall/validation) is delegated to
/// the `cc.utils.plugin_*` modules.  This layer is only the command I/O.

module;

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

export module cc.commands.plugin_cmd;

import cc.types.types;
import cc.commands.command;
import cc.commands.plugin_parse_args;
import cc.commands.plugin_helpers;
import cc.commands.plugin_manage;
import cc.commands.plugin_ui_data;
import cc.utils.plugin_lifecycle;
import cc.utils.plugin_validation;
import cc.utils.plugin_manager;
import cc.utils.plugin_marketplace;

export namespace cc::commands {

using namespace cc::core;
using namespace cc::commands::plugin;
using namespace cc::commands::plugin_helpers;
using namespace cc::commands::plugin_manage;
using namespace cc::commands::plugin_ui;

// ─────────────────────────────────────────────────────────────────────────────
//  Lightweight local PluginInfo (kept — used for the text "list" output
//  and completion suggestions; richer queries go through plugin_manager).
// ─────────────────────────────────────────────────────────────────────────────
struct PluginInfo {
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    bool enabled = true;
    bool installed = true;
};

// ─────────────────────────────────────────────────────────────────────────────
//  PluginCommand  —  implements ICommand via CommandModel (see command.cppm).
// ─────────────────────────────────────────────────────────────────────────────
class PluginCommand {
public:
    // ── metadata ──────────────────────────────────────────────────────────
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "plugin",
            .description = "Manage Claude Code plugins (install, enable, validate, marketplaces)",
            .args = {
                CommandArg{
                    .name = "subcommand",
                    .description =
                        "help | install | uninstall | enable | disable | manage | "
                        "list | validate | marketplace | info",
                    .type = ArgType::Choice,
                    .required = false,
                    .choices = {"help",    "install", "uninstall", "enable", "disable",
                                "manage",  "list",    "info",      "validate", "marketplace"},
                },
                CommandArg{
                    .name = "target",
                    .description = "Plugin name, plugin@marketplace, marketplace URL, validate path",
                    .type = ArgType::Text,
                    .required = false,
                },
            },
            .category = "tools",
            .aliases = {"plugins", "marketplace"},
            .hidden = false,
        };
    }

    // ── validate ──────────────────────────────────────────────────────────
    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        // Anything with 0 args is valid (shows menu / help).
        if (ctx.args.empty()) return {};

        static constexpr std::array<std::string_view, 11> valid = {
            "help", "--help", "-h",
            "install", "i",
            "manage", "list", "info",
            "uninstall", "enable", "disable",
            // "validate" and "marketplace" are handled inline because they
            // carry sub-actions.  We accept them here to keep the gate loose.
        };
        static constexpr std::array<std::string_view, 4> also_valid = {
            "validate", "marketplace", "market", "m",
        };

        const auto action = [&] {
            std::string s{ctx.args[0]};
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }();

        auto found = std::ranges::find(valid, action);
        if (found != valid.end()) return {};
        auto found2 = std::ranges::find(also_valid, action);
        if (found2 != also_valid.end()) return {};

        return std::unexpected(Error::make(
            ErrorCode::InvalidRequest,
            std::format("Unknown /plugin subcommand: '{}'. Try /plugin help.", ctx.args[0])
        ));
    }

    // ── execute ───────────────────────────────────────────────────────────
    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        // Rebuild the raw arg string after the command name so parse_plugin_args
        // sees exactly what the TS implementation receives.
        const std::string raw = join_args(ctx.args);
        const ParsedArgs cmd = parse_plugin_args(
            raw.empty() ? std::optional<std::string_view>{}
                        : std::optional<std::string_view>{raw}
        );

        switch (cmd.type) {
            case SubcommandType::Help:
                return CommandResult::success(std::string{k_plugin_help_text});

            case SubcommandType::Menu:
                return CommandResult::success(menu_text());

            case SubcommandType::Install:
                return cmd_install(cmd);

            case SubcommandType::Manage:
                return cmd_manage_list(/*verbose=*/true);

            case SubcommandType::Uninstall:
                return cmd_uninstall(cmd);

            case SubcommandType::Enable:
                return cmd_set_enabled(cmd, true);

            case SubcommandType::Disable:
                return cmd_set_enabled(cmd, false);

            case SubcommandType::Validate:
                return cmd_validate(cmd);

            case SubcommandType::Marketplace:
                return cmd_marketplace(cmd);
        }
        return CommandResult::success(menu_text());
    }

    // ── completion ────────────────────────────────────────────────────────
    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        // Subcommand names
        for (auto s : {"help", "install", "i", "uninstall", "enable", "disable",
                        "manage", "list", "info", "validate", "marketplace", "market"}) {
            if (std::string_view(s).starts_with(partial)) {
                suggestions.emplace_back(s);
            }
        }
        // Marketplace sub-actions
        if (std::string_view("marketplace").starts_with(partial) ||
            std::string_view("market").starts_with(partial)) {
            for (auto s : {"add", "remove", "rm", "update", "list"}) {
                if (std::string_view(s).starts_with(partial)) suggestions.emplace_back(s);
            }
        }
        // Installed plugin names (from disk scan — small, cheap)
        for (const auto& p : load_installed_plugins()) {
            if (std::string_view(p.name).starts_with(partial)) {
                suggestions.push_back(p.name);
            }
        }
        return suggestions;
    }

private:
    // ────────────────────────────────────────────────────────────────────────
    //  Subcommand handlers
    // ────────────────────────────────────────────────────────────────────────

    // ── menu overview ────────────────────────────────────────────────────
    [[nodiscard]] std::string menu_text() const {
        const auto installed = load_installed_plugins();
        std::ostringstream os;
        os << "Plugin manager\n"
           << "──────────────\n"
           << "Installed: " << installed.size() << " plugin"
           << (installed.size() == 1 ? "" : "s") << "\n"
           << "Enabled:   "
           << std::ranges::count_if(installed, &PluginInfo::enabled) << "\n\n"
           << "Run /plugin help for the full list of subcommands.\n\n"
           << "Hint: run without arguments in interactive mode to open the\n"
           << "      tabbed plugin settings (TUI — Phase 4).\n"
           << "UI:plugins:discover-plugins\n";
        return os.str();
    }

    // ── install ──────────────────────────────────────────────────────────
    [[nodiscard]] Result<CommandResult> cmd_install(const ParsedArgs& cmd) const {
        // Case 1: bare "install" with no target → open TUI (Phase 4 delegation)
        if (!cmd.plugin_name && !cmd.marketplace) {
            return CommandResult{
                true,
                "Opening plugin discovery…\nUI:plugins:discover-plugins",
                std::string{"UI:plugins:discover-plugins"},
                CommandStatus::Succeeded,
            };
        }

        // Case 2: marketplace only (URL/path without plugin name)
        if (cmd.marketplace && !cmd.plugin_name) {
            auto plan = classify_marketplace_input(*cmd.marketplace);
            if (!plan.ok) {
                return CommandResult::fail(
                    "Could not determine marketplace source: " + plan.error_message
                );
            }
            std::ostringstream os;
            os << "Opening marketplace browser for " << *cmd.marketplace << '\n'
               << "  (normalized name: " << plan.normalized_name << ", kind: "
               << plan.source_kind << ")\n"
               << "UI:plugins:browse-marketplace:" << plan.normalized_name;
            return CommandResult{
                true,
                os.str(),
                std::string{"UI:plugins:browse-marketplace:"} + plan.normalized_name,
                CommandStatus::Succeeded,
            };
        }

        // Case 3: plugin name (and optionally marketplace) — try direct install
        const std::string plugin_id =
            cmd.marketplace
                ? (*cmd.plugin_name + '@' + *cmd.marketplace)
                : std::string{*cmd.plugin_name};

        try {
            cc::utils::plugins::PluginInstallOptions opts;
            // Don't force unless we know it's already installed (handled below).
            auto r = cc::utils::plugins::install_plugin(plugin_id, opts);
            if (!r) {
                return CommandResult::fail(
                    "Install failed for '" + plugin_id + "': " + r.error().message
                );
            }
            std::ostringstream os;
            os << "Installed '" << r->plugin_id << "' v"
               << r->installed_version.major << '.'
               << r->installed_version.minor << '.'
               << r->installed_version.patch
               << " in " << r->duration.count() << "ms.";
            if (!r->warnings.empty()) {
                os << "\nWarnings:";
                for (const auto& w : r->warnings) os << "\n  • " << w;
            }
            return CommandResult::success(os.str());
        } catch (const std::exception& e) {
            return CommandResult::fail(
                "Install failed for '" + plugin_id + "': " + e.what()
            );
        }
    }

    // ── manage / list (text mode) ────────────────────────────────────────
    [[nodiscard]] Result<CommandResult> cmd_manage_list(bool verbose) const {
        auto plugins = load_installed_plugins();
        if (plugins.empty()) {
            return CommandResult::success(
                "No plugins installed.\nUse /plugin install <name> to add one.\n"
                "Or open the TUI: UI:plugins:manage-plugins"
            );
        }
        std::ostringstream os;
        os << "Installed plugins (" << plugins.size() << "):\n\n";
        for (const auto& p : plugins) {
            const char* dot = p.enabled ? "\xe2\x97\x8f" : "\xe2\x97\x8b"; // ● / ○
            os << ' ' << dot << ' ' << p.name;
            if (verbose) {
                os << "  v" << p.version
                   << "  by " << p.author;
                if (!p.description.empty()) os << "  — " << p.description;
            }
            os << "  [" << (p.enabled ? "enabled" : "disabled") << ']';
            os << '\n';
        }
        os << "\nTip: /plugin <enable|disable|uninstall> <name>\n"
           << "     Open interactive UI: UI:plugins:manage-plugins";
        return CommandResult::success(os.str());
    }

    // ── uninstall ────────────────────────────────────────────────────────
    [[nodiscard]] Result<CommandResult> cmd_uninstall(const ParsedArgs& cmd) const {
        if (!cmd.target_plugin) {
            return CommandResult{
                true,
                "Pick a plugin to uninstall.\nUI:plugins:manage-plugins?action=uninstall",
                std::string{"UI:plugins:manage-plugins?action=uninstall"},
                CommandStatus::Succeeded,
            };
        }
        auto r = cc::utils::plugin_manager::remove_installed_plugin(*cmd.target_plugin);
        if (!r) {
            return CommandResult::fail(
                "Plugin '" + *cmd.target_plugin + "' is not installed."
            );
        }
        return CommandResult::success("Plugin '" + *cmd.target_plugin + "' uninstalled.");
    }

    // ── enable / disable ─────────────────────────────────────────────────
    [[nodiscard]] Result<CommandResult> cmd_set_enabled(const ParsedArgs& cmd,
                                                         bool enable) const {
        if (!cmd.target_plugin) {
            const std::string view =
                enable ? "manage-plugins?action=enable"
                       : "manage-plugins?action=disable";
            return CommandResult{
                true,
                std::string{"Pick a plugin to "} + (enable ? "enable" : "disable")
                    + ".\nUI:plugins:" + view,
                "UI:plugins:" + view,
                CommandStatus::Succeeded,
            };
        }
        // Delegate to plugin_manager.  The manager module owns the
        // enabled/disabled storage semantics.
        const std::string& id = *cmd.target_plugin;
        namespace pm = cc::utils::plugin_manager;
        if (!pm::is_plugin_installed(id)) {
            return CommandResult::fail("Plugin '" + id + "' is not installed.");
        }
        // PluginScope::User is the common case for CLI operations;
        // finer-grained scope selection is done in the TUI.
        try {
            // The enabled state lives in the installed plugin metadata;
            // plugin_loader / plugin_manager expose this via the plugin
            // lifecycle layer.  We use plugin_lifecycle for this op.
            auto r = cc::utils::plugins::set_plugin_enabled(id, enable);
            if (!r) {
                return CommandResult::fail(
                    std::string{enable ? "Enable" : "Disable"}
                        + " failed for '" + id + "': " + r.error().message
                );
            }
        } catch (const std::exception& e) {
            return CommandResult::fail(
                std::string{enable ? "Enable" : "Disable"}
                    + " failed for '" + id + "': " + e.what()
            );
        }
        return CommandResult::success(
            "Plugin '" + id + "' " + (enable ? "enabled" : "disabled") + "."
        );
    }

    // ── validate ─────────────────────────────────────────────────────────
    [[nodiscard]] Result<CommandResult> cmd_validate(const ParsedArgs& cmd) const {
        if (!cmd.validate_path) {
            return CommandResult::success(std::string{k_validate_usage});
        }
        const auto out = run_validation(*cmd.validate_path);
        // exit_code is embedded in the message so the caller can see it even
        // in command pipelines.
        auto result = CommandResult::success(out.text);
        if (out.exit_code != 0) {
            result.ok = false;
            result.status = CommandStatus::Failed;
        }
        return result;
    }

    // ── marketplace (add/remove/update/list) ─────────────────────────────
    [[nodiscard]] Result<CommandResult> cmd_marketplace(const ParsedArgs& cmd) const {
        namespace pm = cc::utils::plugin_marketplace;

        switch (cmd.market_action) {
            case MarketplaceAction::None:
                return CommandResult{
                    true,
                    std::string{"Opening marketplace management…\n"} +
                        "UI:plugins:manage-marketplaces",
                    std::string{"UI:plugins:manage-marketplaces"},
                    CommandStatus::Succeeded,
                };

            case MarketplaceAction::List: {
                auto cfg = pm::load_known_marketplaces_config_safe();
                if (cfg.empty()) {
                    return CommandResult::success(
                        "No marketplaces configured.\n"
                        "Add one with /plugin marketplace add <source>."
                    );
                }
                std::ostringstream os;
                os << "Configured marketplaces (" << cfg.size() << "):\n\n";
                for (const auto& [name, info] : cfg) {
                    os << "  • " << name;
                    if (info.last_updated) os << "  (updated: " << *info.last_updated << ')';
                    os << '\n';
                }
                os << "\nTip: /plugin marketplace <add|remove|update> <name>\n"
                   << "     Open interactive UI: UI:plugins:manage-marketplaces";
                return CommandResult::success(os.str());
            }

            case MarketplaceAction::Add: {
                if (cmd.market_target.empty()) {
                    return CommandResult{
                        true,
                        "Enter a marketplace source to add.\n"
                        "UI:plugins:add-marketplace",
                        std::string{"UI:plugins:add-marketplace"},
                        CommandStatus::Succeeded,
                    };
                }
                auto plan = classify_marketplace_input(cmd.market_target);
                if (!plan.ok) {
                    return CommandResult::fail(plan.error_message);
                }
                // Build a MarketplaceSource variant and pass to plugin_marketplace.
                pm::MarketplaceSource src;
                if (plan.source_kind == "github") {
                    pm::GitHubMarketplaceSource gh;
                    gh.repo = plan.source_detail;
                    src = gh;
                } else if (plan.source_kind == "git") {
                    pm::GitMarketplaceSource g;
                    g.url = plan.source_detail;
                    src = g;
                } else if (plan.source_kind == "url") {
                    pm::UrlMarketplaceSource u;
                    u.url = plan.source_detail;
                    src = u;
                } else if (plan.source_kind == "file") {
                    pm::FileMarketplaceSource f;
                    f.path = plan.source_detail;
                    src = f;
                } else { // directory
                    pm::DirectoryMarketplaceSource d;
                    d.path = plan.source_detail;
                    src = d;
                }
                auto r = pm::add_marketplace_source(plan.normalized_name, src);
                if (!r) {
                    return CommandResult::fail(
                        "Failed to add marketplace '" + plan.normalized_name
                            + "': " + r.error()
                    );
                }
                return CommandResult::success(
                    "Successfully added marketplace: " + plan.normalized_name
                        + "  (" + plan.source_kind + ')'
                );
            }

            case MarketplaceAction::Remove: {
                if (cmd.market_target.empty()) {
                    return CommandResult{
                        true,
                        "Pick a marketplace to remove.\n"
                        "UI:plugins:manage-marketplaces?action=remove",
                        std::string{"UI:plugins:manage-marketplaces?action=remove"},
                        CommandStatus::Succeeded,
                    };
                }
                auto r = pm::remove_marketplace(cmd.market_target);
                if (!r) {
                    return CommandResult::fail(
                        "Failed to remove marketplace '" + cmd.market_target
                            + "': " + r.error()
                    );
                }
                return CommandResult::success(
                    "Marketplace '" + cmd.market_target + "' removed."
                );
            }

            case MarketplaceAction::Update: {
                if (cmd.market_target.empty()) {
                    // Update all — trigger TUI (user can see progress)
                    return CommandResult{
                        true,
                        "Updating all marketplaces…\n"
                        "UI:plugins:manage-marketplaces?action=update",
                        std::string{"UI:plugins:manage-marketplaces?action=update"},
                        CommandStatus::Succeeded,
                    };
                }
                // Individual fetch via plugin_marketplace.
                auto r = pm::fetch_marketplace(cmd.market_target);
                if (!r) {
                    return CommandResult::fail(
                        "Update failed for '" + cmd.market_target
                            + "': " + r.error()
                    );
                }
                return CommandResult::success(
                    "Updated marketplace '" + cmd.market_target
                        + "' — " + std::to_string(r->plugins.size()) + " plugins available."
                );
            }
        }
        return CommandResult::fail("Unknown marketplace action.");
    }

    // ────────────────────────────────────────────────────────────────────────
    //  Utilities
    // ────────────────────────────────────────────────────────────────────────

    /// Join a vector of args with spaces (reconstructs the string that
    /// follows "/plugin " on the command line).
    [[nodiscard]] static std::string join_args(const std::vector<std::string>& args) {
        std::ostringstream os;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i) os << ' ';
            os << args[i];
        }
        return os.str();
    }

    [[nodiscard]] static std::filesystem::path plugins_dir() {
        if (const char* home = std::getenv("HOME")) {
            return std::filesystem::path(home) / ".cc-repl" / "plugins";
        }
        return std::filesystem::temp_directory_path() / "cc-repl" / "plugins";
    }

    [[nodiscard]] static std::optional<std::string> extract_json_string(
        std::string_view json, std::string_view key
    ) {
        const auto marker = std::string{"\""} + std::string(key) + "\":\"";
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

    /// Disk scan of installed plugins (kept for completion / text lists).
    /// The rich "what plugins are loaded and active?" query uses
    /// cc.utils.plugin_manager; this is a best-effort offline view.
    [[nodiscard]] static std::vector<PluginInfo> load_installed_plugins() {
        namespace fs = std::filesystem;
        std::vector<PluginInfo> plugins;
        const auto root = plugins_dir();
        std::error_code ec;
        if (!fs::exists(root, ec)) return plugins;

        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (!entry.is_directory()) continue;
            const auto name = entry.path().filename().string();
            if (name.starts_with(".")) continue;

            PluginInfo info{
                .name        = name,
                .version     = "unknown",
                .author      = "unknown",
                .description = "Installed plugin",
                .enabled     = !fs::exists(entry.path() / ".disabled", ec),
                .installed   = true,
            };

            const auto manifest = entry.path() / "manifest.json";
            if (fs::exists(manifest, ec)) {
                std::ifstream in(manifest);
                std::string buf((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                if (auto v = extract_json_string(buf, "version"))     info.version     = *v;
                if (auto a = extract_json_string(buf, "author"))      info.author      = *a;
                if (auto d = extract_json_string(buf, "description")) info.description = *d;
            }

            plugins.push_back(std::move(info));
        }
        std::ranges::sort(plugins, {}, &PluginInfo::name);
        return plugins;
    }
};

} // namespace cc::commands
