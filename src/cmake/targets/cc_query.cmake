# ─── cc_query: Query Engine ───────────────────────────────────────────────────
add_library(cc_query)
target_sources(cc_query
    PUBLIC FILE_SET CXX_MODULES FILES
        query/config.cppm
        query/wire_protocol.cppm
        query/wire_anthropic.cppm
        query/wire_openai.cppm
        query/query_engine.cppm
        query/stop_hooks.cppm
        query/token_budget.cppm
)
# RFC 0001 Phase C batch 5 — query_engine module implementation units. Never
# add these to the FILE_SET CXX_MODULES list above: they are module impl
# units (`module cc.query.query_engine;`), not interface units. In
# particular query_engine_http.cpp is the ONLY TU that textually includes
# <httplib.h>, keeping the third-party closure out of the interface BMI.
target_sources(cc_query
    PRIVATE
        query/query_engine_ctor.cpp
        query/query_engine_system_prompt.cpp
        query/query_engine_loop.cpp
        query/query_engine_http.cpp
        query/query_engine_wire.cpp
        query/query_engine_json.cpp
        query/query_engine_tools.cpp
        query/query_engine_conversation.cpp
        query/query_engine_compaction.cpp
        query/query_engine_util.cpp
)
target_link_libraries(cc_query PUBLIC cc_utils cc_state cc_config CURL::libcurl)
