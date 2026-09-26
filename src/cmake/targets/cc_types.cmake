# ─── cc_types: Shared Type Definitions ────────────────────────────────────────
add_library(cc_types)
target_sources(cc_types
    PUBLIC FILE_SET CXX_MODULES FILES
        types/command.cppm
        types/hooks.cppm
        types/logs.cppm
        types/permissions.cppm
        types/plugin.cppm
        types/timestamp.cppm
        types/tool_types.cppm
        types/types.cppm
)
target_link_libraries(cc_types PUBLIC cc_utils)
