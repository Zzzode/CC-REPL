# ─── loom: Main Executable ─────────────────────────────────────────────────
add_executable(loom)
target_sources(loom
    PRIVATE
        main.cpp
)
target_link_libraries(loom
    PRIVATE
        cc_core
        cc_daemon
        cc_server
)
set_target_properties(loom PROPERTIES
    OUTPUT_NAME "loom"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ─── Pare Benchmark Executable ────────────────────────────────────────────────
add_executable(pare_benchmark)
target_sources(pare_benchmark
    PRIVATE
        benchmarks/pare/pare_benchmark_main.cpp
)
target_link_libraries(pare_benchmark
    PRIVATE
        cc_benchmarks
)
set_target_properties(pare_benchmark PROPERTIES
    OUTPUT_NAME "pare-benchmark"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
