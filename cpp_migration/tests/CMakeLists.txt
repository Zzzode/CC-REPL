# ─── CC-REPL Test Suite ────────────────────────────────────────────────────────
# 使用 GoogleTest 框架进行单元测试

include(FetchContent)

# 获取 GoogleTest v1.14.0
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)
# 防止 GoogleTest 覆盖父项目的编译器/链接器设置 (Windows)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# Disable C++ module scanning for GoogleTest (it doesn't use modules)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
FetchContent_MakeAvailable(googletest)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

# 启用 CTest 集成
include(GoogleTest)

# ─── test_utils: 工具模块测试 ─────────────────────────────────────────────────
add_executable(test_utils
    test_utils.cpp
    utils/io_shell_memory_test.cpp
    utils/permissions_test.cpp
    utils/settings_validation_test.cpp
)
target_link_libraries(test_utils
    PRIVATE
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_utils)

# ─── test_core: 核心引擎测试 ──────────────────────────────────────────────────
add_executable(test_core test_core.cpp)
target_link_libraries(test_core
    PRIVATE
        cc_core
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_core)

# ─── test_state: 状态管理测试 ─────────────────────────────────────────────────
add_executable(test_state test_state.cpp)
target_link_libraries(test_state
    PRIVATE
        cc_state
        cc_session
        cc_cli
        cc_services
        cc_constants
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_state)

# ─── test_tools: 工具系统测试 ─────────────────────────────────────────────────
add_executable(test_tools test_tools.cpp)
target_link_libraries(test_tools
    PRIVATE
        cc_tools
        cc_core
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_tools)

# ─── test_hooks: Hooks 系统测试 ───────────────────────────────────────────────
add_executable(test_hooks test_hooks.cpp)
target_link_libraries(test_hooks
    PRIVATE
        cc_hooks
        cc_core
        cc_state
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_hooks)

# ─── test_services: Services 层测试 ───────────────────────────────────────────
add_executable(test_services test_services.cpp)
target_link_libraries(test_services
    PRIVATE
        cc_services
        cc_core
        cc_state
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_services)

# ─── test_ui: UI 模块测试 ─────────────────────────────────────────────────────
add_executable(test_ui test_ui.cpp)
target_link_libraries(test_ui
    PRIVATE
        cc_ui
        cc_core
        cc_state
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_ui)

# ─── test_tasks: Task 系统测试 ────────────────────────────────────────────────
add_executable(test_tasks test_tasks.cpp)
target_link_libraries(test_tasks
    PRIVATE
        cc_tasks
        cc_core
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_tasks)

# ─── test_commands: 命令系统测试 ──────────────────────────────────────────────
add_executable(test_commands test_commands.cpp)
target_link_libraries(test_commands
    PRIVATE
        cc_commands
        cc_tools
        cc_core
        cc_state
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_commands)

# ─── test_bridge: Bridge 模块测试 ─────────────────────────────────────────────
add_executable(test_bridge test_bridge.cpp)
target_link_libraries(test_bridge
    PRIVATE
        cc_bridge
        cc_core
        cc_state
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_bridge)

# ─── test_skills: Skills 模块测试 ─────────────────────────────────────────────
add_executable(test_skills test_skills.cpp)
target_link_libraries(test_skills
    PRIVATE
        cc_skills
        cc_tools
        cc_core
        cc_state
        cc_utils
        GTest::gtest_main
)
gtest_discover_tests(test_skills)
