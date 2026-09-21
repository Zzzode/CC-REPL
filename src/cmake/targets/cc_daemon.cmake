# ─── cc_daemon: Daemon System ─────────────────────────────────────────────────
add_library(cc_daemon)
target_sources(cc_daemon
    PUBLIC FILE_SET CXX_MODULES FILES
        daemon/daemon_client.cppm
        daemon/daemon_server.cppm
        daemon/worker_registry.cppm
)
target_link_libraries(cc_daemon PUBLIC cc_utils cc_bridge)
