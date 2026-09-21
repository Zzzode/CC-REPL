# ─── cc_buddy: Buddy Companion ────────────────────────────────────────────────
add_library(cc_buddy)
target_sources(cc_buddy
    PUBLIC FILE_SET CXX_MODULES FILES
        buddy/buddy_companion.cppm
        buddy/buddy_hooks.cppm
        buddy/buddy_prompt.cppm
        buddy/buddy_types.cppm
)
target_link_libraries(cc_buddy PUBLIC cc_utils)
