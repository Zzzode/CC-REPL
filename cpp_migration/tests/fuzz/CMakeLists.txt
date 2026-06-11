# Fuzz targets for CC-REPL
# Only built when ENABLE_FUZZING=ON

# Fuzz target: SSE parser
add_executable(fuzz_sse_parser fuzz_sse_parser.cpp)
target_compile_options(fuzz_sse_parser PRIVATE -fsanitize=fuzzer)
target_link_options(fuzz_sse_parser PRIVATE -fsanitize=fuzzer)
target_link_libraries(fuzz_sse_parser PRIVATE cc_services cc_utils)

# Fuzz target: JSON parse + message extraction
add_executable(fuzz_json_parse fuzz_json_parse.cpp)
target_compile_options(fuzz_json_parse PRIVATE -fsanitize=fuzzer)
target_link_options(fuzz_json_parse PRIVATE -fsanitize=fuzzer)
target_link_libraries(fuzz_json_parse PRIVATE cc_utils)
