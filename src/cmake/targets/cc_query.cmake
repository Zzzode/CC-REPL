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
target_link_libraries(cc_query PUBLIC cc_utils cc_state cc_config CURL::libcurl)
