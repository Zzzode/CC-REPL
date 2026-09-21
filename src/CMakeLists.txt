# ─── LOOM Module Library Targets ───────────────────────────────────────────
# Each target corresponds to a C++23 module partition of the system.
# All .cppm files in each directory are registered as CXX_MODULES sources.

# ─── NOTE: Ninja Job Pools are now configured at the TOP LEVEL CMakeLists.txt
#     (project root), immediately after the project() call.  The GLOBAL
#     set_property(JOB_POOLS) and the CMAKE_NINJA_JOB_POOL_* variables live
#     there so that ALL sibling subdirectories (src/, tests/, benchmarks/)
#     observe the same LOOM_SCAN_JOBS cap. This keeps P1689 depfile
#     scanning bounded for every target under this project. Do NOT re-declare JOB_POOLS
#     here; APPENDing to the GLOBAL property twice just duplicates entries.
# ─────────────────────────────────────────────────────────────────────────────

# ─── Optional tree-sitter dependency for bash AST danger classifier ───────────
option(CC_ENABLE_TREE_SITTER "Enable tree-sitter bash parsing for AST danger classifier" OFF)
if(CC_ENABLE_TREE_SITTER)
    # tree-sitter is wired with the deprecated FetchContent_Populate + manual
    # library target pattern. CMake 4.x sets CMP0169 to NEW which forbids
    # FetchContent_Populate; allow it here until the wiring is modernized.
    cmake_policy(SET CMP0169 OLD)
    include(FetchContent)
    FetchContent_Declare(
        tree_sitter
        GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
        GIT_TAG        v0.22.6
    )
    FetchContent_Declare(
        tree_sitter_bash
        GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-bash.git
        GIT_TAG        v0.23.1
    )
    FetchContent_GetProperties(tree_sitter)
    if(NOT tree_sitter_POPULATED)
        FetchContent_Populate(tree_sitter)
    endif()
    FetchContent_GetProperties(tree_sitter_bash)
    if(NOT tree_sitter_bash_POPULATED)
        FetchContent_Populate(tree_sitter_bash)
    endif()

    # Build tree-sitter core as a static library from source.
    add_library(tree-sitter STATIC
        ${tree_sitter_SOURCE_DIR}/lib/src/alloc.c
        ${tree_sitter_SOURCE_DIR}/lib/src/get_changed_ranges.c
        ${tree_sitter_SOURCE_DIR}/lib/src/language.c
        ${tree_sitter_SOURCE_DIR}/lib/src/lexer.c
        ${tree_sitter_SOURCE_DIR}/lib/src/lib.c
        ${tree_sitter_SOURCE_DIR}/lib/src/node.c
        ${tree_sitter_SOURCE_DIR}/lib/src/parser.c
        ${tree_sitter_SOURCE_DIR}/lib/src/query.c
        ${tree_sitter_SOURCE_DIR}/lib/src/stack.c
        ${tree_sitter_SOURCE_DIR}/lib/src/subtree.c
        ${tree_sitter_SOURCE_DIR}/lib/src/tree.c
        ${tree_sitter_SOURCE_DIR}/lib/src/tree_cursor.c
        ${tree_sitter_SOURCE_DIR}/lib/src/wasm_store.c
    )
    target_include_directories(tree-sitter PUBLIC
        ${tree_sitter_SOURCE_DIR}/lib/include
    )
    set_target_properties(tree-sitter PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        POSITION_INDEPENDENT_CODE ON
    )
    if(MSVC)
        target_compile_options(tree-sitter PRIVATE /wd4101 /wd4244 /wd4267 /wd4457)
    else()
        target_compile_options(tree-sitter PRIVATE -w)
    endif()

    # Build tree-sitter-bash language parser as a static library.
    add_library(tree-sitter-bash STATIC
        ${tree_sitter_bash_SOURCE_DIR}/src/parser.c
        ${tree_sitter_bash_SOURCE_DIR}/src/scanner.c
    )
    target_include_directories(tree-sitter-bash PUBLIC
        ${tree_sitter_bash_SOURCE_DIR}/src
    )
    target_link_libraries(tree-sitter-bash PUBLIC tree-sitter)
    set_target_properties(tree-sitter-bash PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        POSITION_INDEPENDENT_CODE ON
    )
    if(MSVC)
        target_compile_options(tree-sitter-bash PRIVATE /wd4101 /wd4244 /wd4267)
    else()
        target_compile_options(tree-sitter-bash PRIVATE -w)
    endif()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_utils.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_state.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_task_types.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_history.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_constants.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_types.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_config.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_context.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_keybindings.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_memdir.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_coordinator.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_query.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_session.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_entrypoints.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_bootstrap.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_tasks.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_skills_core.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_tools.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_commands.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_services.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_hooks.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_cli.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_bridge.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_skills.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_ui.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_vim.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_daemon.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_buddy.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_migrations.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_server.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_schemas.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_benchmarks.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_plugins.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/cc_core.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/targets/loom.cmake")
