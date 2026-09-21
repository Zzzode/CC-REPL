# ─── cc_benchmarks: Pare Benchmark System ─────────────────────────────────────
add_library(cc_benchmarks)
target_sources(cc_benchmarks
    PUBLIC FILE_SET CXX_MODULES FILES
        benchmarks/pare/case_loader.cppm
        benchmarks/pare/cli.cppm
        benchmarks/pare/evaluator.cppm
        benchmarks/pare/execute_ref.cppm
        benchmarks/pare/metrics.cppm
        benchmarks/pare/run.cppm
        benchmarks/pare/schema.cppm
        benchmarks/pare/workspace.cppm
)
target_link_libraries(cc_benchmarks PUBLIC cc_utils yyjson)
