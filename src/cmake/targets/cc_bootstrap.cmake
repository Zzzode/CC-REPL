# ─── cc_bootstrap: Startup Bootstrap ─────────────────────────────────────────
add_library(cc_bootstrap)
target_sources(cc_bootstrap
    PUBLIC FILE_SET CXX_MODULES FILES
        bootstrap/interactive_helpers.cppm
)
target_link_libraries(cc_bootstrap PUBLIC cc_utils cc_state cc_config cc_services cc_hooks)
