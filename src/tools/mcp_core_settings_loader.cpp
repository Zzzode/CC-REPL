// Implementation unit for cc.tools.mcp — RFC-0001 B4 core-settings MCP loader
// sink storage. The two bodies live here (not in the .cppm) so the
// inline-definition ratchet on cc.tools.mcp does not grow; the single
// function-local static below is the one strong symbol every TU binds to,
// matching the NativeMcpRuntime::instance / global_mcp_router anchor style.
module;

module cc.tools.mcp;

import std;

namespace cc::tools::detail {

CoreSettingsMcpServersLoader& core_settings_mcp_loader_slot() {
    static CoreSettingsMcpServersLoader loader;
    return loader;
}

}  // namespace cc::tools::detail

namespace cc::tools {

void set_core_settings_mcp_loader(CoreSettingsMcpServersLoader loader) {
    detail::core_settings_mcp_loader_slot() = std::move(loader);
}

}  // namespace cc::tools
