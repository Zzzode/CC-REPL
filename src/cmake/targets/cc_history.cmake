# ─── cc_history: Canonical Conversation History ────────────────────────────────
add_library(cc_history)
target_sources(cc_history
    PUBLIC FILE_SET CXX_MODULES FILES
        types/history.cppm
)
target_link_libraries(cc_history PUBLIC cc_utils)
