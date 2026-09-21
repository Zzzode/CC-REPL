# ─── cc_session: Session Management ──────────────────────────────────────────
add_library(cc_session)
target_sources(cc_session
    PUBLIC FILE_SET CXX_MODULES FILES
        session/history.cppm
        session/session.cppm
        session/storage.cppm
)
target_link_libraries(cc_session PUBLIC cc_utils cc_state cc_config yyjson)
