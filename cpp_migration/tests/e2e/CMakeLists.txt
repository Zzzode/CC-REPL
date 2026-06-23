# tests/e2e/CMakeLists.txt
#
# End-to-end integration tests for Phase 3.
#
#   E2E_SSE_Mock         — feed a synthetic SSE byte stream into the parser
#                          and validate event/delta counts (no network).
#   E2E_Bash_File        — compose BashTool (find) + File* (read/edit/write)
#                          on a real temporary directory.
#   E2E_REPL_DryRun      — fork/exec the real `cc-repl --dry-run` binary
#                          and assert stdout keywords (StatusBar/ReplScreen/Esc).
#
# All three tests are plain .cpp files (not C++23 named modules) with their
# own main() so they remain reproducible without module BMI visibility
# between TUs.

get_filename_component(_E2E_REAL_BIN "${CMAKE_BINARY_DIR}/bin/cc-repl" ABSOLUTE)
find_package(Threads REQUIRED)

# ─── E2E-1: SSE mock parser ──────────────────────────────────────────────────
add_executable(e2e_sse_mock e2e_sse_mock.cpp)
set_target_properties(e2e_sse_mock PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_compile_features(e2e_sse_mock PRIVATE cxx_std_20)
add_test(NAME E2E_SSE_Mock COMMAND e2e_sse_mock)
set_tests_properties(E2E_SSE_Mock PROPERTIES TIMEOUT 10)

# ─── E2E-2: REPL --dry-run (fork/exec) ───────────────────────────────────────
add_executable(e2e_repl_dry_run e2e_repl_dry_run.cpp)
set_target_properties(e2e_repl_dry_run PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_compile_features(e2e_repl_dry_run PRIVATE cxx_std_20)
target_link_libraries(e2e_repl_dry_run PRIVATE Threads::Threads)
if(TARGET cc_repl)
  add_dependencies(e2e_repl_dry_run cc_repl)
endif()
# Pass the resolved binary location through env so the test doesn't need to
# guess relative paths (the argv0-based heuristic is backup).
add_test(NAME E2E_REPL_DryRun COMMAND e2e_repl_dry_run)
set_tests_properties(E2E_REPL_DryRun PROPERTIES
    TIMEOUT                          30
    ENVIRONMENT                      "CC_REPL_BINARY=${_E2E_REAL_BIN}"
    SKIP_RETURN_CODE                 125)

# ─── E2E-3: BashTool + FileTool composition ──────────────────────────────────
add_executable(e2e_bash_file e2e_bash_file_integration.cpp)
set_target_properties(e2e_bash_file PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_compile_features(e2e_bash_file PRIVATE cxx_std_20)
target_link_libraries(e2e_bash_file PRIVATE Threads::Threads)
add_test(NAME E2E_Bash_File COMMAND e2e_bash_file)
set_tests_properties(E2E_Bash_File PROPERTIES TIMEOUT 30)
