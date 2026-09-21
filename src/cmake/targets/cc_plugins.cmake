# ─── cc_plugins: Plugin System ────────────────────────────────────────────────
add_library(cc_plugins)
target_sources(cc_plugins
    PUBLIC FILE_SET CXX_MODULES FILES
        plugins/loader.cppm
        plugins/marketplace.cppm
        plugins/plugin.cppm
)
target_link_libraries(cc_plugins PUBLIC cc_utils cc_types yyjson uv_a)
