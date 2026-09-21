# ─── cc_coordinator: Multi-Agent Orchestration ────────────────────────────────
add_library(cc_coordinator)
target_sources(cc_coordinator
    PUBLIC FILE_SET CXX_MODULES FILES
        coordinator/coordinator_types.cppm
        coordinator/swarm.cppm
)
target_link_libraries(cc_coordinator PUBLIC cc_utils cc_state cc_entrypoints)
