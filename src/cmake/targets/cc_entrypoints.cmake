# ─── cc_entrypoints: CLI Entry Points ─────────────────────────────────────────
add_library(cc_entrypoints)
target_sources(cc_entrypoints
    PUBLIC FILE_SET CXX_MODULES FILES
        entrypoints/control_schemas.cppm
        entrypoints/control_types.cppm
        entrypoints/core_schemas.cppm
        entrypoints/core_types.cppm
        entrypoints/mcp_entrypoint.cppm
        entrypoints/runtime_types.cppm
        entrypoints/sandbox_types.cppm
        entrypoints/sdk_types.cppm
        entrypoints/settings_types.cppm
)
target_link_libraries(cc_entrypoints PUBLIC cc_utils cc_state cc_config)
