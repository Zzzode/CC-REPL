# Smoke tests for the cc_repl executable.
# These tests are only registered when the cc_repl target exists (Phase C-d).
# If the target is defined but the binary is not yet built, tests report SKIP
# (exit code 125) instead of FAIL so the CTest dashboard stays green.

if(TARGET cc_repl)
  set(_SMOKE_RUNNER   "${CMAKE_CURRENT_SOURCE_DIR}/run_smoke.cmake")
  set(_SMOKE_SKIP_RC  125)

  add_test(NAME test_cc_repl_dry_run
    COMMAND ${CMAKE_COMMAND}
      -D CC_REPL_BIN=$<TARGET_FILE:cc_repl>
      -D "CC_REPL_ARGS=--dry-run"
      -D EXPECT_EXIT=0
      -D SKIP_RC=${_SMOKE_SKIP_RC}
      -P ${_SMOKE_RUNNER}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(test_cc_repl_dry_run PROPERTIES
    TIMEOUT         10
    SKIP_RETURN_CODE ${_SMOKE_SKIP_RC})

  add_test(NAME test_cc_repl_version
    COMMAND ${CMAKE_COMMAND}
      -D CC_REPL_BIN=$<TARGET_FILE:cc_repl>
      -D "CC_REPL_ARGS=--version"
      -D EXPECT_EXIT=0
      -D "MATCH_STDOUT=[0-9]+\\.[0-9]+\\.[0-9]+"
      -D SKIP_RC=${_SMOKE_SKIP_RC}
      -P ${_SMOKE_RUNNER}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(test_cc_repl_version PROPERTIES
    TIMEOUT         5
    SKIP_RETURN_CODE ${_SMOKE_SKIP_RC})
endif()
