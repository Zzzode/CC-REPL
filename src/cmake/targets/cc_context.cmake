# ─── cc_context: UI Context ───────────────────────────────────────────────────
add_library(cc_context)
target_sources(cc_context
    PUBLIC FILE_SET CXX_MODULES FILES
        context/mailbox.cppm
        context/notifications.cppm
)
target_link_libraries(cc_context PUBLIC cc_utils cc_state cc_coordinator)
