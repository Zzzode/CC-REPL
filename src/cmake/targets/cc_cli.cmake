# ─── cc_cli: CLI Transports ───────────────────────────────────────────────────
add_library(cc_cli)
target_sources(cc_cli
    PUBLIC FILE_SET CXX_MODULES FILES
        cli/ccr_client.cppm
        cli/handlers/agents.cppm
        cli/handlers/plugins_handler.cppm
        cli/sse_transport.cppm
        cli/transports.cppm
        cli/update.cppm
        cli/websocket_transport.cppm
)
target_link_libraries(cc_cli
    PUBLIC
        cc_utils
        yyjson
        uv_a
        httplib::httplib
        # SSETransport performs a real TLS handshake for https:// streams.
        OpenSSL::SSL
        OpenSSL::Crypto
)
