# ─── cc_task_types: Canonical Task Data Model ─────────────────────────────────
add_library(cc_task_types)
target_sources(cc_task_types
    PUBLIC FILE_SET CXX_MODULES FILES
        types/task_types.cppm
)
target_link_libraries(cc_task_types PUBLIC cc_utils)
