# ─── cc_memdir: Memory Directory ──────────────────────────────────────────────
add_library(cc_memdir)
target_sources(cc_memdir
    PUBLIC FILE_SET CXX_MODULES FILES
        memdir/memdir.cppm
        memdir/memory.cppm
        memdir/paths.cppm
)
target_link_libraries(cc_memdir PUBLIC cc_utils cc_constants)
