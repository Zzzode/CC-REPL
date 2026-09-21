# ─── cc_server: Server ────────────────────────────────────────────────────────
add_library(cc_server)
target_sources(cc_server
    PUBLIC FILE_SET CXX_MODULES FILES
        server/server_main.cppm
        server/server_routes.cppm
        server/types.cppm
)
target_link_libraries(cc_server PUBLIC cc_utils cc_session cc_query cc_tools OpenSSL::Crypto)
