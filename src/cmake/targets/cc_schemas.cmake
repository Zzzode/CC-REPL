# ─── cc_schemas: Validation Schemas ───────────────────────────────────────────
add_library(cc_schemas)
target_sources(cc_schemas
    PUBLIC FILE_SET CXX_MODULES FILES
        schemas/validation_schemas.cppm
)
target_link_libraries(cc_schemas PUBLIC cc_utils)
