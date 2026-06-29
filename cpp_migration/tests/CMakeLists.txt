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
set(_CC_REPL_TEST_WERROR_OLD ${CMAKE_COMPILE_WARNING_AS_ERROR})
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
FetchContent_MakeAvailable(googletest)
set(CMAKE_COMPILE_WARNING_AS_ERROR ${_CC_REPL_TEST_WERROR_OLD})
unset(_CC_REPL_TEST_WERROR_OLD)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)


include(GoogleTest)


# NOTE: The test TUs below import C++20 named modules (`import cc.xxx.yyy;`)
# extensively, so CMAKE_CXX_SCAN_FOR_MODULES MUST remain ON at directory scope.
# Only the googletest vendor targets (which do not export or import modules)
# have scanning explicitly disabled via set_target_properties below.  Module
# scanning was previously disabled at directory scope under the incorrect
# assumption that tests did not import modules — that caused every test TU to
# fail with "fatal error: module 'cc.*' not found" because BMI discovery was
# skipped even though the libraries were built correctly.
set(CC_REPL_TEST_DISCOVERY_TIMEOUT 30 CACHE STRING "Timeout in seconds for GoogleTest discovery")

# Turn scanning OFF for the googletest vendor targets — they contain no module
# interfaces or imports and would otherwise waste scanner time / confuse BMI
# dependency tracking on some CMake/Ninja combos.
foreach(_gt_target IN ITEMS gtest gtest_main gmock gmock_main)
    if(TARGET ${_gt_target})
        set_target_properties(${_gt_target} PROPERTIES CXX_SCAN_FOR_MODULES OFF)
    endif()
endforeach()

# Helper: mark a test executable target as C++-module-importing so that CMake
# scans its TUs for `import` statements and propagates upstream BMI files.
function(cc_test_target_enable_modules _name)
    if(TARGET ${_name})
        set_target_properties(${_name} PROPERTIES CXX_SCAN_FOR_MODULES ON)
    endif()
endfunction()


add_executable(test_utils
    test_utils.cpp
    utils/io_shell_memory_test.cpp
    utils/permissions_test.cpp
    utils/settings_validation_test.cpp
)
target_link_libraries(test_utils
    PRIVATE
        cc_core
        cc_utils           # all 50+ cc.utils.* imports
        cc_ui              # cc.ui.terminal_io
        cc_plugins         # cc.plugins.marketplace
        cc_memdir          # core.memdir
        cc_screens         # core.screens
        GTest::gtest_main
)
gtest_discover_tests(test_utils DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_core test_core.cpp)
target_link_libraries(test_core
    PRIVATE
        cc_core
        cc_types           # import cc.types.types
        cc_config          # import cc.config.*
        cc_constants       # import cc.constants.constants
        cc_coordinator     # import cc.coordinator.types
        cc_tasks           # import cc.tasks.task_graph
        GTest::gtest_main
)
gtest_discover_tests(test_core DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_state test_state.cpp)
target_link_libraries(test_state
    PRIVATE
        cc_core
        cc_state           # all cc.state.* imports
        cc_session         # cc.session.history
        cc_types           # cc.types.types
        cc_cli             # cc.cli.update
        cc_constants       # cc.constants.prompts
        cc_services        # cc.services.mcp.*
        cc_hooks           # cc.hooks.tool_permissions
        cc_ui              # cc.ui.repl_screen / cc.ui.* (broad coverage)
        GTest::gtest_main
)
gtest_discover_tests(test_state DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_tools test_tools.cpp)
target_link_libraries(test_tools
    PRIVATE
        cc_core
        cc_tools           # all 40+ cc.tools.* imports
        cc_hooks           # cc.hooks.tool_permissions
        cc_services        # cc.services.api.client, cc.services.mcp.*
        cc_utils           # cc.utils.* helpers
        GTest::gtest_main
)
gtest_discover_tests(test_tools DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_hooks test_hooks.cpp)
target_link_libraries(test_hooks
    PRIVATE
        cc_core
        cc_hooks           # all cc.hooks.* imports
        cc_utils           # cc.utils.hooks_*
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
gtest_discover_tests(test_services DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

# ---------------------------------------------------------------------------
# Phase 3-E2E: SSE Mock + WithRetry unit tests
# ---------------------------------------------------------------------------
add_executable(test_sse_mock test_sse_mock.cpp)
target_link_libraries(test_sse_mock
    PRIVATE
        cc_core
        cc_services
        cc_utils
        httplib::httplib
        GTest::gtest_main
)
gtest_discover_tests(test_sse_mock DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

add_executable(test_with_retry test_with_retry.cpp)
target_link_libraries(test_with_retry
    PRIVATE
        cc_core
        cc_services            # cc.services.api.with_retry*, cc.services.oauth
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_with_retry DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

# ---------------------------------------------------------------------------
# Phase P0-5: Enterprise Auth (SigV4 / GCP ADC / Azure DefaultAzureCredential
# / Provider selector).  Link OpenSSL::Crypto for the GCP service-account
# RS256 self-signed JWT signer.
# ---------------------------------------------------------------------------
add_executable(test_enterprise_auth test_enterprise_auth.cpp)
target_link_libraries(test_enterprise_auth
    PRIVATE
        cc_core
        cc_services
        cc_utils
        httplib::httplib
        OpenSSL::Crypto
        GTest::gtest_main
)
gtest_discover_tests(test_enterprise_auth DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

# ---------------------------------------------------------------------------
# Phase 3-D: MCP stdio transport + mock echo server
# ---------------------------------------------------------------------------
add_executable(test_mcp_stdio test_mcp_stdio.cpp)
target_link_libraries(test_mcp_stdio
    PRIVATE
        cc_core
        cc_services
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_mcp_stdio DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

add_executable(test_ui test_ui.cpp)
target_link_libraries(test_ui
    PRIVATE
        cc_core
        cc_ui           # defense-in-depth: UI modules must be discoverable
        GTest::gtest_main
)
gtest_discover_tests(test_ui DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

# ---------------------------------------------------------------------------
# M7 Dialog #1 (ToolPermission) renderer + event handler regression suite.
# Faithful-port contract: keyboard mapping, golden chrome, and queue
# suppression policy are all covered here so any UI drift is caught in CI.
# ---------------------------------------------------------------------------
add_executable(test_dialog_toolpermission test_dialog_toolpermission.cpp)
target_link_libraries(test_dialog_toolpermission
    PRIVATE
        cc_core
        cc_ui
        GTest::gtest_main
)
gtest_discover_tests(test_dialog_toolpermission
    DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


# ---------------------------------------------------------------------------
# M7 Dialog System core — payload serde, DialogQueue, DialogRendererRegistry,
# Elicitation/SandboxPermission/SettingsView routing tests
# ---------------------------------------------------------------------------
add_executable(test_dialog_system test_dialog_system.cpp)
target_link_libraries(test_dialog_system
    PRIVATE
        cc_core
        cc_ui                  # cc.ui.dialogs.* + cc.ui.design.theme
        GTest::gtest_main
)
cc_test_target_enable_modules(test_dialog_system)
gtest_discover_tests(test_dialog_system
    DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


# ---------------------------------------------------------------------------
# M7 Dialog #3 (PromptDialog SELECT/FORM) renderer + event handling tests
# ---------------------------------------------------------------------------
add_executable(test_prompt_dialog test_prompt_dialog.cpp)
target_link_libraries(test_prompt_dialog
    PRIVATE
        cc_core
        cc_ui
        GTest::gtest_main
)
gtest_discover_tests(test_prompt_dialog
    DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


# ---------------------------------------------------------------------------
# M7 Dialog #5 (CostThreshold) — enum Action, callback wiring, visual goldens
# ---------------------------------------------------------------------------
add_executable(test_cost_threshold_dialog test_cost_threshold_dialog.cpp)
target_link_libraries(test_cost_threshold_dialog
    PRIVATE
        cc_core
        cc_ui
        GTest::gtest_main
)
gtest_discover_tests(test_cost_threshold_dialog
    DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_tasks test_tasks.cpp)
target_link_libraries(test_tasks
    PRIVATE
        cc_core
        cc_tasks               # cc.tasks.*
        cc_tools               # cc.tools.agent_* / team / spawn_multi_agent
        cc_utils               # cc.utils.swarm_backends
        GTest::gtest_main
)
gtest_discover_tests(test_tasks DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_commands test_commands.cpp)
target_link_libraries(test_commands
    PRIVATE
        cc_core
        cc_commands            # cc.commands.*
        cc_query               # cc.query.query_engine
        cc_types               # cc.types.types
        GTest::gtest_main
)
gtest_discover_tests(test_commands DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

# ---------------------------------------------------------------------------
# Statusline (commands + utils/statusline_runner) unit tests
# ---------------------------------------------------------------------------
add_executable(test_statusline test_statusline.cpp)
target_link_libraries(test_statusline
    PRIVATE
        cc_core
        cc_commands            # cc.commands.statusline
        cc_utils               # cc.utils.statusline_runner / cc.utils.json
        GTest::gtest_main
)
gtest_discover_tests(test_statusline DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


# ---------------------------------------------------------------------------
# P1-05 / P1-06: history, task_types, teammate_view_helpers, interactive_helpers
# ---------------------------------------------------------------------------
add_executable(test_bootstrap test_bootstrap.cpp)
target_link_libraries(test_bootstrap
    PRIVATE
        cc_core
        cc_bootstrap           # cc.bootstrap.interactive
        cc_history             # cc.history
        cc_task_types          # cc.task_types
        cc_state               # cc.state.teammate_view_helpers
        cc_utils               # cc.utils.json
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
        cc_migrations          # all cc.migrations.* imports
        cc_utils               # cc.utils.json
        GTest::gtest_main
)
gtest_discover_tests(test_migrations DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_bridge test_bridge.cpp)
target_link_libraries(test_bridge
    PRIVATE
        cc_core
        cc_daemon
        GTest::gtest_main
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
        cc_skills              # cc.skills.skill, cc.skills.bundled
        cc_skills_core
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

# ===========================================================================
# Migration fix-batch regression suites (test_fix_*)
# ===========================================================================

# B2: cc.utils.hooks_execution wired into QueryEngine tool-call dispatch
add_executable(test_fix_hooks_integration test_fix_hooks_integration.cpp)
target_link_libraries(test_fix_hooks_integration
    PRIVATE
        cc_core
        GTest::gtest_main
)
gtest_discover_tests(test_fix_hooks_integration DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

# B1-qe-tests: QueryEngine coverage (output_config, fallback models, memory, skills)
add_executable(test_fix_query_engine test_fix_query_engine.cpp)
target_link_libraries(test_fix_query_engine
    PRIVATE
        cc_core
        GTest::gtest_main
)
gtest_discover_tests(test_fix_query_engine DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

# state-layer: M2 fsync atomic write + M1 undo/redo regression
add_executable(test_fix_state test_fix_state.cpp)
target_link_libraries(test_fix_state
    PRIVATE
        cc_core
        cc_state
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_fix_state DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

# plugin-H1-H2: marketplace fetch/add/remove + manifest validation
add_executable(test_fix_plugin test_fix_plugin.cpp)
target_link_libraries(test_fix_plugin
    PRIVATE
        cc_core
        cc_utils
        cc_plugins
        GTest::gtest_main
)
gtest_discover_tests(test_fix_plugin DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

# H5: yoga layout engine subset regression (documentation-only fix, 18 tests)
add_executable(test_fix_yoga test_fix_yoga.cpp)
target_link_libraries(test_fix_yoga
    PRIVATE
        cc_core
        GTest::gtest_main
)
gtest_discover_tests(test_fix_yoga DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

# M4 + M11: LSP action parity + send_request<T> static_assert
add_executable(test_fix_lsp_tool test_fix_lsp_tool.cpp)
target_link_libraries(test_fix_lsp_tool
    PRIVATE
        cc_core
        GTest::gtest_main
)
gtest_discover_tests(test_fix_lsp_tool DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

# M6: API stubs -> real HTTP (first_token_date, ultrareview_quota, overage_credit)
add_executable(test_fix_api test_fix_api.cpp)
target_link_libraries(test_fix_api
    PRIVATE
        cc_core
        cc_services
        cc_constants
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_fix_api DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

# M7: notification backend wiring (mcp connectivity + teammate shutdowns)
add_executable(test_fix_notifs test_fix_notifs.cpp)
target_link_libraries(test_fix_notifs
    PRIVATE
        cc_core
        cc_hooks
        cc_services
        GTest::gtest_main
)
gtest_discover_tests(test_fix_notifs DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

# misc-low: oauth_port bind-probe, secret_scanner gitleaks, in_process_transport, PKCE CSPRNG
add_executable(test_fix_misc_low test_fix_misc_low.cpp)
target_link_libraries(test_fix_misc_low
    PRIVATE
        cc_core
        cc_services
        GTest::gtest_main
)
gtest_discover_tests(test_fix_misc_low DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

# ── Mark ALL test executables as module-importing so CMake scans them for
#    `import cc.*` statements and propagates upstream BMI files.  Any
#    individual target that truly does not import modules (e.g. a legacy
#    pre-module TU) can override with:
#        set_target_properties(<name> PROPERTIES CXX_SCAN_FOR_MODULES OFF)
#    after this block.
foreach(_cc_test IN ITEMS
    test_utils
    test_core
    test_state
    test_tools
    test_hooks
    test_services
    test_sse_mock
    test_with_retry
    test_enterprise_auth
    test_mcp_stdio
    test_ui
    test_dialog_system
    test_tasks
    test_commands
    test_statusline
    test_bootstrap
    test_migrations
    test_bridge
    test_skills
    phase3_permission_smoke
    bench_core
    test_fix_hooks_integration
    test_fix_query_engine
    test_fix_state
    test_fix_plugin
    test_fix_yoga
    test_fix_lsp_tool
    test_fix_api
    test_fix_notifs
    test_fix_misc_low
)
    if(TARGET ${_cc_test})
        set_target_properties(${_cc_test} PROPERTIES CXX_SCAN_FOR_MODULES ON)
    endif()
endforeach()
