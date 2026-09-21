# ─── cc_constants: Constants ──────────────────────────────────────────────────
add_library(cc_constants)
target_sources(cc_constants
    PUBLIC FILE_SET CXX_MODULES FILES
        constants/constants.cppm
        constants/cost_tracker.cppm
        constants/figures.cppm
        constants/output_styles.cppm
        constants/paths.cppm
        constants/product.cppm
        constants/prompts.cppm
        constants/spinner_verbs.cppm
        services/api/sse_client.cppm
        constants/xml.cppm
)
target_link_libraries(cc_constants PUBLIC cc_utils cc_types)
