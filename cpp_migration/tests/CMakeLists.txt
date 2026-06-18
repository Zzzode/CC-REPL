# CC-REPL Test Suite


include(FetchContent)


FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# Disable C++ module scanning for GoogleTest (it doesn't use modules)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
FetchContent_MakeAvailable(googletest)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)


include(GoogleTest)


set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CC_REPL_TEST_DISCOVERY_TIMEOUT 30 CACHE STRING "Timeout in seconds for GoogleTest discovery")


add_executable(test_utils
    test_utils.cpp
    utils/io_shell_memory_test.cpp
    utils/permissions_test.cpp
    utils/settings_validation_test.cpp
)
target_link_libraries(test_utils
    PRIVATE
        cc_core
        GTest::gtest_main
)
gtest_discover_tests(test_utils DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_core test_core.cpp)
target_link_libraries(test_core
    PRIVATE
        cc_core
        GTest::gtest_main
)
gtest_discover_tests(test_core DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_state test_state.cpp)
target_link_libraries(test_state
    PRIVATE
        cc_core
        GTest::gtest_main
)
gtest_discover_tests(test_state DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_tools test_tools.cpp)
target_link_libraries(test_tools
    PRIVATE
        cc_core
        GTest::gtest_main
)
gtest_discover_tests(test_tools DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_hooks test_hooks.cpp)
target_link_libraries(test_hooks
    PRIVATE
        cc_core
        GTest::gtest_main
)
gtest_discover_tests(test_hooks DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_services test_services.cpp)
target_link_libraries(test_services
    PRIVATE
        cc_core
        cc_cli
        cc_remote
        cc_utils
        GTest::gtest_main
)
target_compile_options(test_services PRIVATE
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-missing-designated-field-initializers>
)
gtest_discover_tests(test_services DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

# ---------------------------------------------------------------------------
# Phase 3-E2E: SSE Mock + WithRetry unit tests
# ---------------------------------------------------------------------------
add_executable(test_sse_mock test_sse_mock.cpp)
target_link_libraries(test_sse_mock
    PRIVATE
        cc_services
        cc_utils
        httplib::httplib
        GTest::gtest_main
)
gtest_discover_tests(test_sse_mock DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

add_executable(test_with_retry test_with_retry.cpp)
target_link_libraries(test_with_retry
    PRIVATE
        cc_services
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_with_retry DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

# ---------------------------------------------------------------------------
# Phase 3-D: MCP stdio transport + mock echo server
# ---------------------------------------------------------------------------
add_executable(test_mcp_stdio test_mcp_stdio.cpp)
target_link_libraries(test_mcp_stdio
    PRIVATE
        cc_services
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_mcp_stdio DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

add_executable(test_ui test_ui.cpp)
target_link_libraries(test_ui
    PRIVATE
        cc_core
        GTest::gtest_main
)
gtest_discover_tests(test_ui DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_tasks test_tasks.cpp)
target_link_libraries(test_tasks
    PRIVATE
        cc_core
        GTest::gtest_main
)
gtest_discover_tests(test_tasks DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_commands test_commands.cpp)
target_link_libraries(test_commands
    PRIVATE
        cc_core
        GTest::gtest_main
)
gtest_discover_tests(test_commands DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


# ---------------------------------------------------------------------------
# P1-05 / P1-06: history, task_types, teammate_view_helpers, interactive_helpers
# ---------------------------------------------------------------------------
add_executable(test_bootstrap test_bootstrap.cpp)
target_link_libraries(test_bootstrap
    PRIVATE
        cc_core
        GTest::gtest_main
)
gtest_discover_tests(test_bootstrap DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


# ---------------------------------------------------------------------------
# P0-01 / P2-02: Migrations orchestration + atomicity fixture suite
# ---------------------------------------------------------------------------
add_executable(test_migrations test_migrations.cpp)
target_link_libraries(test_migrations
    PRIVATE
        cc_core
        GTest::gtest_main
)
target_compile_options(test_migrations PRIVATE
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-missing-designated-field-initializers>
)
gtest_discover_tests(test_migrations DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_bridge test_bridge.cpp)
target_link_libraries(test_bridge
    PRIVATE
        cc_core
        cc_daemon
        GTest::gtest_main
)
target_compile_options(test_bridge PRIVATE
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-missing-designated-field-initializers>
)
add_dependencies(test_bridge cc_repl)
target_compile_definitions(test_bridge
    PRIVATE
        CC_REPL_NATIVE_BINARY_PATH="$<TARGET_FILE:cc_repl>"
)
gtest_discover_tests(test_bridge DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_skills test_skills.cpp)
target_link_libraries(test_skills
    PRIVATE
        cc_core
        GTest::gtest_main
)
gtest_discover_tests(test_skills DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

# ---------------------------------------------------------------------------
# Google Benchmark micro-benchmark suite
# ---------------------------------------------------------------------------
add_subdirectory(benchmarks)

# Phase 3-B/C: tools_smoke + tools_e2e
#
# These two executables exercise the real bash fork/exec and filesystem
# primitives in cc.tools.bash.impl / cc.tools.files.impl with real shell
# and filesystem side-effects.  Instead of linking through `cc_tools`
# (which transitively pulls in cc_services and can be broken by unrelated
# module scan issues like services/api/sse_client.ddi) we build them via
# the standalone helper script scripts/build_tools_standalone.bash which
# precompiles only the two impl_* .cppm units and links against the
# system C++ runtime.  We still register them as CTest tests so the
# standard `ctest -R tools_*` / `ctest --output-on-failure` invocation
# works consistently.
#
# Both binaries land in ${CMAKE_BINARY_DIR}:
#   ${CMAKE_BINARY_DIR}/tools_smoke  — fine-grained CHECK assertions
#   ${CMAKE_BINARY_DIR}/tools_e2e    — Bash × File cross-tool scenario
set(CC_TOOLS_STANDALONE_BUILDER
    "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/build_tools_standalone.bash")

add_custom_command(
    OUTPUT
        "${CMAKE_BINARY_DIR}/tools_smoke"
        "${CMAKE_BINARY_DIR}/tools_e2e"
    COMMAND
        ${CMAKE_COMMAND} -E env
            "CC_TOOLS_STANDALONE_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "CXX=${CMAKE_CXX_COMPILER}"
            bash "${CC_TOOLS_STANDALONE_BUILDER}" all
    DEPENDS
        "${CC_TOOLS_STANDALONE_BUILDER}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools_smoke.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools_e2e.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/tools/bash/impl_bash.cppm"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/tools/files/impl_files.cppm"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
    COMMENT "[Phase 3-B/C] Building standalone tools_smoke + tools_e2e"
    VERBATIM
)
add_custom_target(tools_standalone DEPENDS
    "${CMAKE_BINARY_DIR}/tools_smoke"
    "${CMAKE_BINARY_DIR}/tools_e2e")

# A "setup" CTest test that drives the custom-target build so that
# `ctest -R tools` works without an explicit `cmake --build ... --target
# tools_standalone` step.  We also declare it a FIXTURES_SETUP so the two
# actual assertions only run after the binaries are up to date.
add_test(
    NAME tools_standalone_setup
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target tools_standalone
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
set_tests_properties(tools_standalone_setup PROPERTIES
    TIMEOUT 300
    LABELS "phase3;tools;setup"
    FIXTURES_SETUP tools_standalone_fixture
)

add_test(
    NAME tools_smoke
    COMMAND "${CMAKE_BINARY_DIR}/tools_smoke"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
set_tests_properties(tools_smoke PROPERTIES
    TIMEOUT 60
    LABELS "phase3;tools;smoke"
    FIXTURES_REQUIRED tools_standalone_fixture
)

add_test(
    NAME tools_e2e
    COMMAND "${CMAKE_BINARY_DIR}/tools_e2e"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
set_tests_properties(tools_e2e PROPERTIES
    TIMEOUT 60
    LABELS "phase3;tools;e2e"
    FIXTURES_REQUIRED tools_standalone_fixture
)

# ---------------------------------------------------------------------------
# Phase 3-G PermissionResolver + PermissionGate standalone smoke runner.
# Exercises all 5 acceptance cases + glob + ABI + gate wiring.
# ---------------------------------------------------------------------------
add_executable(phase3_permission_smoke
    smoke/phase3_permission_smoke.cpp
)
target_link_libraries(phase3_permission_smoke
    PRIVATE
        cc_core
        cc_hooks
        cc_utils
)
set_target_properties(phase3_permission_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_test(NAME Phase3_Permission_Smoke COMMAND phase3_permission_smoke)
set_tests_properties(Phase3_Permission_Smoke PROPERTIES TIMEOUT 10 LABELS "phase3;permission;hooks")

# Phase 3-E2E: end-to-end integration tests (3 binaries, plain .cpp main,
# no GoogleTest dep — keep them lightweight).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/e2e/CMakeLists.txt)
  add_subdirectory(e2e)
endif()

# ---------------------------------------------------------------------------
# Fuzz targets (only when ENABLE_FUZZING is ON)
# ---------------------------------------------------------------------------
if(ENABLE_FUZZING)
  add_subdirectory(fuzz)
endif()
