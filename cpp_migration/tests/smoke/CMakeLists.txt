# Smoke tests for the loom executable.
# These tests are only registered when the loom target exists (Phase C-d).
# If the target is defined but the binary is not yet built, tests report SKIP
# (exit code 125) instead of FAIL so the CTest dashboard stays green.

if(TARGET loom)
  set(_SMOKE_RUNNER   "${CMAKE_CURRENT_SOURCE_DIR}/run_smoke.cmake")
  set(_SMOKE_SKIP_RC  125)

  add_test(NAME test_loom_dry_run
    COMMAND ${CMAKE_COMMAND}
      -D LOOM_BIN=$<TARGET_FILE:loom>
      -D "LOOM_ARGS=--dry-run"
      -D EXPECT_EXIT=0
      -D SKIP_RC=${_SMOKE_SKIP_RC}
      -P ${_SMOKE_RUNNER}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(test_loom_dry_run PROPERTIES
    TIMEOUT         10
    SKIP_RETURN_CODE ${_SMOKE_SKIP_RC})

  add_test(NAME test_loom_version
    COMMAND ${CMAKE_COMMAND}
      -D LOOM_BIN=$<TARGET_FILE:loom>
      -D "LOOM_ARGS=--version"
      -D EXPECT_EXIT=0
      -D "MATCH_STDOUT=[0-9]+\\.[0-9]+\\.[0-9]+"
      -D SKIP_RC=${_SMOKE_SKIP_RC}
      -P ${_SMOKE_RUNNER}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(test_loom_version PROPERTIES
    TIMEOUT         5
    SKIP_RETURN_CODE ${_SMOKE_SKIP_RC})
endif()
