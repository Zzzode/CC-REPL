# ─── cc_config: Configuration ─────────────────────────────────────────────────
add_library(cc_config)
target_sources(cc_config
    PUBLIC FILE_SET CXX_MODULES FILES
        config/mcp_types.cppm
        config/config.cppm
        config/feature_flags.cppm
        config/model_config.cppm
        config/settings.cppm
)
target_link_libraries(cc_config PUBLIC cc_utils cc_types cc_constants)
