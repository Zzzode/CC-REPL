# ─── cc_bootstrap: Startup Bootstrap ─────────────────────────────────────────
add_library(cc_bootstrap)
target_sources(cc_bootstrap
    PUBLIC FILE_SET CXX_MODULES FILES
        bootstrap/interactive_helpers.cppm
        bootstrap/mcp_connectivity.cppm
)
# RFC-0001 B7: mcp_connectivity.cppm imports cc.tools.mcp (the B6 snapshot
# sink setter). cc_tools is included LATER in src/CMakeLists.txt (:112 vs
# this file's :109); a forward-declared target reference is legal and
# resolves at generate time.
target_link_libraries(cc_bootstrap PUBLIC cc_utils cc_state cc_config cc_services cc_hooks cc_tools)
