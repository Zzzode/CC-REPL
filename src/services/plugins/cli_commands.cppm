// ============================================================================
// DEPRECATED / SUPERSEDED — do not add new importers.
//
// This module was a hollow placeholder that returned hardcoded strings
// ("No plugins installed.", "Installed plugin: "+args, ...). The real plugin
// marketplace backend lives in:
//   - cc.utils.plugin_marketplace      (sources, cached manifests, lookups)
//   - cc.utils.plugin_loader           (manifest parse/load)
//   - cc.utils.plugin_manager          (install/uninstall/update lifecycle)
//   - commands/plugin/plugin_manage    (CLI surface that actually wires them)
//
// Zero live importers (grep `cc.services.plugins` confirms). The bodies below
// are kept compiling for safety but now delegate where a trivial delegation
// exists (list -> marketplace data) and return honest errors otherwise instead
// of faking success. Prefer the real backend listed above.
// ============================================================================
module;
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>
export module cc.services.plugins.cli_commands;

import cc.utils.plugin_marketplace;

export namespace cc::services::plugins {

namespace pm = cc::utils::plugin_marketplace;

// Plugin command descriptor
struct PluginCommand {
    std::string name;
    std::string description;
};

// Handle a plugin CLI subcommand.
//
// `list` now returns REAL data from the marketplace backend (cache-only,
// no network) so it is not misleading. `install`/`uninstall`/`update` have
// no trivial single-call delegation in cc.utils.plugin_marketplace (which
// handles marketplace sources/lookups, not per-plugin lifecycle); those
// routes live in cc.utils.plugin_manager / commands/plugin/plugin_manage,
// so we return an honest "superseded" error instead of a fake success string.
auto handle_plugin_command(std::string_view subcommand, std::span<const std::string> args)
    -> std::expected<std::string, std::string> {
    if (subcommand == "list") {
        // Delegate to the real backend: enumerate available plugins across
        // all known marketplaces (cache-only reads, no network fetch here).
        auto available = pm::get_all_available_plugins();
        if (available.empty()) {
            return std::string("No plugins available.");
        }
        std::string out;
        out.reserve(available.size() * 32);
        for (const auto& [name, entry] : available) {
            out += name;
            if (entry.version) {
                out += " @ ";
                out += *entry.version;
            }
            if (entry.description) {
                out += " - ";
                out += *entry.description;
            }
            out += '\n';
        }
        if (!out.empty()) out.pop_back(); // drop trailing newline
        return out;
    }
    if (subcommand == "install" || subcommand == "uninstall" || subcommand == "update") {
        // Superseded: per-plugin lifecycle lives in cc.utils.plugin_manager and
        // is wired through commands/plugin/plugin_manage. This shim intentionally
        // does NOT fake a success string.
        (void)args;
        return std::unexpected(
            std::string("cc.services.plugins::handle_plugin_command('")
            + std::string(subcommand)
            + "') is superseded; use cc.utils.plugin_manager via "
              "commands/plugin/plugin_manage instead.");
    }
    return std::unexpected("Unknown plugin command: " + std::string(subcommand));
}

// Get available plugin commands (metadata only; harmless to keep).
auto get_plugin_commands() -> std::vector<PluginCommand> {
    return {
        {"list", "List available plugins (delegates to marketplace backend)"},
        {"install", "Install a plugin (superseded - see commands/plugin/plugin_manage)"},
        {"uninstall", "Remove an installed plugin (superseded - see commands/plugin/plugin_manage)"},
        {"update", "Update a plugin (superseded - see commands/plugin/plugin_manage)"},
    };
}

} // namespace cc::services::plugins
