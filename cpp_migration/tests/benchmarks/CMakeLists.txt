# ─── Google Benchmark micro-benchmark suite ──────────────────────────────────

add_executable(bench_core bench_core.cpp)

# Disable module scanning for benchmark library headers (they don't use modules)
set_target_properties(bench_core PROPERTIES
    CXX_SCAN_FOR_MODULES ON
)

target_link_libraries(bench_core
    PRIVATE
        cc_services
        cc_tools
        cc_utils
        benchmark::benchmark
        benchmark::benchmark_main
)
