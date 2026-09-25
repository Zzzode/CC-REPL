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
# RFC 0001 Phase C batch 10 — selectors module implementation units. Never
# add these to the FILE_SET CXX_MODULES list above: they are module impl
# units (`module cc.state.selectors;`), not interface units.
target_sources(cc_state
    PRIVATE
        state/selectors_core.cpp
        state/selectors_bridge.cpp
        state/selectors_ui_tasks.cpp
        state/selectors_companion_mcp.cpp
        state/selectors_conversation.cpp
        state/selectors_features.cpp
)
target_link_libraries(cc_state
    PUBLIC
        cc_utils
        cc_types
        cc_task_types
        yyjson
)
