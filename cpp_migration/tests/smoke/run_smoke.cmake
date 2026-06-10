# run_smoke.cmake -- ctest wrapper for cc_repl smoke tests.
#
# Required input variables (passed via -D from the caller):
#   CC_REPL_BIN   - Absolute path to the cc_repl executable
#   CC_REPL_ARGS  - Semicolon-separated list of arguments to pass
#   EXPECT_EXIT   - Expected exit code (typically 0)
#   SKIP_RC       - Return code to emit when binary is missing (for SKIP_RETURN_CODE)
# Optional:
#   MATCH_STDOUT  - If set, stdout must match this regex; otherwise exit-code only

# ── 1. Binary presence (skip cleanly if not yet built) ──────────────────────
if(NOT EXISTS "${CC_REPL_BIN}")
  message(STATUS "SKIP: cc_repl binary not found at ${CC_REPL_BIN}")
  message(STATUS "      (build the cc_repl target first to enable smoke tests)")
  cmake_language(EXIT_CODE "${SKIP_RC}")
endif()

# ── 2. Run the binary ──────────────────────────────────────────────────────
if(CC_REPL_ARGS)
  separate_arguments(_args NATIVE_COMMAND "${CC_REPL_ARGS}")
else()
  set(_args "")
endif()

execute_process(
  COMMAND "${CC_REPL_BIN}" ${_args}
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE  _err
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_STRIP_TRAILING_WHITESPACE
  TIMEOUT 60
)

# ── 3. Emit captured output so ctest -V shows it ───────────────────────────
if(NOT "${_out}" STREQUAL "")
  message("${_out}")
endif()
if(NOT "${_err}" STREQUAL "")
  message(STDERR "${_err}")
endif()

# ── 4. Exit code check ─────────────────────────────────────────────────────
if(NOT _rc EQUAL EXPECT_EXIT)
  message(FATAL_ERROR
    "Expected exit code ${EXPECT_EXIT}, got ${_rc}")
endif()

# ── 5. Optional stdout regex match ─────────────────────────────────────────
if(DEFINED MATCH_STDOUT AND NOT "${MATCH_STDOUT}" STREQUAL "")
  if(NOT _out MATCHES "${MATCH_STDOUT}")
    message(FATAL_ERROR
      "stdout did not match regex \"${MATCH_STDOUT}\"\n"
      "Actual stdout:\n${_out}")
  endif()
  message(STATUS "stdout matched: ${MATCH_STDOUT}")
endif()
