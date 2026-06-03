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
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_utils DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_core test_core.cpp)
target_link_libraries(test_core
    PRIVATE
        cc_core
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_core DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_state test_state.cpp)
target_link_libraries(test_state
    PRIVATE
        cc_core
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_state DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_tools test_tools.cpp)
target_link_libraries(test_tools
    PRIVATE
        cc_core
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_tools DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_hooks test_hooks.cpp)
target_link_libraries(test_hooks
    PRIVATE
        cc_core
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_hooks DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_services test_services.cpp)
target_link_libraries(test_services
    PRIVATE
        cc_core
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_services DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_ui test_ui.cpp)
target_link_libraries(test_ui
    PRIVATE
        cc_core
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_ui DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_tasks test_tasks.cpp)
target_link_libraries(test_tasks
    PRIVATE
        cc_core
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_tasks DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_commands test_commands.cpp)
target_link_libraries(test_commands
    PRIVATE
        cc_core
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_commands DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_bridge test_bridge.cpp)
target_link_libraries(test_bridge
    PRIVATE
        cc_core
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_bridge DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})


add_executable(test_skills test_skills.cpp)
target_link_libraries(test_skills
    PRIVATE
        cc_core
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_skills DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})
