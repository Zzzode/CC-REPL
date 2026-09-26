/// @file mcp_connectivity.cppm
/// @brief RFC-0001 B7 interim MCP-connectivity bridge.
///
/// Consumes the B6 NativeMcpRuntime snapshot sink
/// (cc::tools::set_mcp_snapshots_sink) and projects every emitted snapshot
/// vector into the EXISTING cc.hooks.remaining_notifs MCP connectivity slot
/// via set_raw_mcp_connectivity, so the current get_mcp_connectivity_status()
/// / has_mcp_connectivity_issues() readers see the data unchanged.
///
/// INTERIM DOUBLE-PUBLISH (B7): the old direct projection
/// (inject_mcp_connectivity_from_manager, called inside
/// NativeMcpRuntime::all_statuses) still runs. Both legs publish from the
/// SAME snapshot vector and carry identical data by construction. B8 deletes
/// the old hook/tools legs in one atomic commit and the sink installed here
/// becomes the sole feed; nothing in this module is designed to outlive that
/// cut except the slot setter it already shares.
///
/// Single-sink design: one std::function installed exactly once at the
/// composition root (main()). The hook gains no provider/refresh API —
/// set_raw_mcp_connectivity is already the slot setter the notif readers
/// consume.
module;

#include <cstdint>

export module cc.bootstrap.mcp_connectivity;

// graph_check parser hazard: qualify cc.tools symbols WITHOUT a leading "::"
// (write cc::tools::set_mcp_snapshots_sink, never ::cc::tools::...). A
// leading-colon chain defeats the namespace-path evidence heuristic and
// makes this import look like a NEW dead import. Un-prefixed cc:: qualifiers
// are used consistently throughout this file.
import std;

import cc.hooks.remaining_notifs;
import cc.services.mcp.types;
import cc.services.mcp.connection_manager;
import cc.tools.mcp;

export namespace cc::bootstrap::mcp_connectivity {

// Verbatim copy of the hook-local to_mcp_server_status() 5-arm switch
// (src/hooks/notifs/remaining_notifs.cppm); B8 deletes the hook copy and
// this becomes the only mapping. Kept character-for-character identical so
// the interim double-publish cannot diverge.
inline auto to_hook_status(cc::services::mcp::ConnectionStatus s)
    -> cc::hooks::notifs::McpServerStatus {
    using CS = cc::services::mcp::ConnectionStatus;
    switch (s) {
        case CS::Connected:    return cc::hooks::notifs::McpServerStatus::Connected;
        case CS::Connecting:   return cc::hooks::notifs::McpServerStatus::Connecting;
        case CS::NeedsAuth:    return cc::hooks::notifs::McpServerStatus::Error;   // surfaced for auth nudge
        case CS::Error:        return cc::hooks::notifs::McpServerStatus::Error;
        case CS::Disconnected:
        default:               return cc::hooks::notifs::McpServerStatus::Disconnected;
    }
}

// Verbatim projection body from inject_mcp_connectivity_from_manager():
// both ids copy snap.name, last_error is copied, and every row shares one
// now-ms stamp. The clock is the hook module's OWN detail::now_ms() (already
// exported and kept exported through B8), reused rather than replicated so
// the two interim legs cannot read two different clocks.
inline auto project_connectivity(
    std::vector<cc::services::mcp::McpServerSnapshot> snapshots
) -> std::vector<cc::hooks::notifs::McpConnectivityInfo> {
    std::vector<cc::hooks::notifs::McpConnectivityInfo> infos;
    infos.reserve(snapshots.size());
    const int64_t now = cc::hooks::notifs::detail::now_ms();
    for (const auto& snap : snapshots) {
        cc::hooks::notifs::McpConnectivityInfo info;
        info.server_id    = snap.name;
        info.display_name = snap.name;
        info.state        = to_hook_status(snap.status);
        info.last_error   = snap.last_error;
        info.last_seen_ms = now;
        infos.push_back(std::move(info));
    }
    return infos;
}

// Install the single bridge sink on the tools runtime. The lambda is
// captureless and must not re-install sinks (it runs on the runtime status
// read path, after NativeMcpRuntime::mutex_ has been released). Idempotent
// setter; call exactly once at the composition root before any status read.
inline void wire_mcp_connectivity() {
    cc::tools::set_mcp_snapshots_sink(
        [](std::vector<cc::services::mcp::McpServerSnapshot> snaps) {
            cc::hooks::notifs::set_raw_mcp_connectivity(
                project_connectivity(std::move(snaps)));
        });
}

} // export namespace cc::bootstrap::mcp_connectivity
