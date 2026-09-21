# ─── cc_state: State Management ───────────────────────────────────────────────
add_library(cc_state)
target_sources(cc_state
    PUBLIC FILE_SET CXX_MODULES FILES
        state/app_state.cppm
        state/ftxui_integration.cppm
        state/on_change_app_state.cppm
        state/persistence.cppm
        state/selectors.cppm
        state/store.cppm
        state/teammate_view_helpers.cppm
)
target_link_libraries(cc_state
    PUBLIC
        cc_utils
        cc_types
        cc_task_types
        yyjson
)
