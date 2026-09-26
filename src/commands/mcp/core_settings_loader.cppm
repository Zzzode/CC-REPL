/// @file core_settings_loader.cppm
/// @brief RFC-0001 B4 composition root: bridges the core settings layer
/// (cc::core::ConfigManager) into the native MCP runtime through the loader
/// sink declared in cc.tools.mcp. This module is the ONLY place allowed to
/// know both cc.config.config and cc.tools.mcp, which deletes the
/// cc.tools.mcp -> cc.config.config upward edge (tools rank 8 -> config rank 1).
///
/// The lambda body below is the verbatim ConfigManager block that used to
/// live in NativeMcpRuntime::ensure_loaded_from_config. The production binary
/// calls install_core_settings_mcp_loader() once from main(), before any path
/// can reach the native runtime; test binaries that need the core layer must
/// install it explicitly and reset the slot to null in cleanup.
module;

export module cc.commands.mcp.core_settings_loader;

import std;

import cc.config.config;
import cc.tools.mcp;

export namespace cc::commands {

/// Install the production core-settings MCP loader into the cc.tools.mcp
/// sink. Idempotent: installing again simply replaces the previous loader.
/// The loader is invoked lazily on the native runtime's first config load and
/// must propagate a failed ConfigManager::load() verbatim.
inline void install_core_settings_mcp_loader() {
    cc::tools::set_core_settings_mcp_loader(
        []() -> std::expected<std::vector<cc::tools::NativeMcpConfiguredServer>, std::string> {
            cc::core::ConfigManager config;
            auto loaded = config.load();
            if (!loaded) return std::unexpected(loaded.error().message);

            std::vector<cc::tools::NativeMcpConfiguredServer> servers;
            for (const auto& server : config.settings().mcp_servers) {
                servers.push_back(cc::tools::to_native_mcp_server(server));
            }
            return servers;
        });
}

}  // namespace cc::commands
