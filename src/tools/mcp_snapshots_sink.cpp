// Implementation unit for cc.tools.mcp — RFC-0001 B6 snapshot-sink storage.
// The two bodies live here (not in the .cppm) so the inline-definition
// ratchet on cc.tools.mcp does not grow; the single function-local static
// below is the one strong symbol every TU binds to, matching the
// core_settings_mcp_loader / global_mcp_router anchor style.
module;

module cc.tools.mcp;

import std;

namespace cc::tools::detail {

McpSnapshotsSink& mcp_snapshots_sink() {
    static McpSnapshotsSink sink;
    return sink;
}

}  // namespace cc::tools::detail

namespace cc::tools {

void set_mcp_snapshots_sink(McpSnapshotsSink sink) {
    detail::mcp_snapshots_sink() = std::move(sink);
}

}  // namespace cc::tools
