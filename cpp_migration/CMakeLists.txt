cmake_minimum_required(VERSION 3.28)

# ─── Toolchain selection (must come BEFORE project()) ─────────────────────────
#
# CMake's C++23 named-modules support relies on `clang-scan-deps` to discover
# per-TU import graphs (P1689 format).  Apple's Xcode toolchain ships clang
# (AppleClang) but does *not* ship `clang-scan-deps`, so CMake detects "the
# compiler does not provide a way to discover the import graph dependencies"
# and refuses to generate anything that consumes or produces named modules.
#
# Prefer a Homebrew-installed LLVM toolchain (`brew install llvm`) which ships
# a matching `clang-scan-deps` alongside `clang++`.  The user can override
# this by passing -DCMAKE_CXX_COMPILER=... / -DCMAKE_C_COMPILER=... on the
# cmake command line, in which case the cache values win.
if(NOT DEFINED CMAKE_CXX_COMPILER AND EXISTS "/opt/homebrew/opt/llvm/bin/clang++")
    set(CMAKE_CXX_COMPILER "/opt/homebrew/opt/llvm/bin/clang++")
    set(CMAKE_C_COMPILER   "/opt/homebrew/opt/llvm/bin/clang")
endif()

project(CC-REPL
    VERSION 1.0.0
    DESCRIPTION "CC-REPL: A C++23 Modules-based CLI REPL"
    LANGUAGES CXX C
)

# ─── Ninja Job Pools (eliminate scan-deps BMI races without throttling the
#     rest of the build) ───────────────────────────────────────────────────────
# Background: at high -j (Ninja default = hw.ncpu), the P1689 dependency-scan
# rules (``clang-scan-deps -format=p1689``) for many sibling .cppm files under
# a single target race to (a) produce their ``.ddi.tmp → .ddi`` atomic rename
# and (b) update the shared ``CXX.dd`` dyndep file.  The symptom is transient
# ``mv: <file>.ddi.tmp: No such file or directory`` on ~3% of scans, followed
# by a successful retry.  Rather than force the entire build to -j 1/2 to work
# around it, we cap ONLY the depfile-gen rules to a small pool and leave the
# actual compile + link steps at full concurrency.
#
# NOTE: this block is defined HERE (top-level) and NOT in src/CMakeLists.txt
# because CMAKE_NINJA_JOB_POOL_* are normal CMake variables with directory
# scope; if set only inside src/, they leak to tests/CMakeLists.txt via
# add_subdirectory ordering but their value is lost when processing other
# sibling subdirectories.  Setting them at the top level guarantees every
# target under this project (src + tests + benchmarks) observes the cap.
if(CMAKE_GENERATOR MATCHES "Ninja")
    include(ProcessorCount)
    ProcessorCount(NPROC)
    if(NOT NPROC OR NPROC LESS 1)
        set(NPROC 4)
    endif()
    # Link jobs are memory heavy — cap at 2.  Compile: all cores.
    # Depfile scan: cap at 2 (removes the P1689 rename race entirely).
    if(NPROC GREATER 16)
        set(_LINK_POOL 4)
    else()
        set(_LINK_POOL 2)
    endif()
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS
        compile_jobs=${NPROC}
        link_jobs=${_LINK_POOL}
        scan_modules=2
    )
    # Assign the pools.  Names here are the CMake canonical ones:
    #   JOB_POOL_COMPILE → every <LANG>_COMPILE_OBJECT rule
    #   JOB_POOL_LINK    → every <LANG>_LINK_* rule
    #   CMAKE_NINJA_JOB_POOL_CXX_MODULE_DEPFILE_GEN (3.28+) → depfile scans
    set(CMAKE_JOB_POOL_COMPILE compile_jobs)
    set(CMAKE_JOB_POOL_LINK    link_jobs)
    # CMake 3.28+ exposes the depfile-gen pools directly.  On older versions
    # these set() calls are harmless — the variables are simply unused.  Set at
    # this (top-level) scope so child directories (src / tests / benchmarks)
    # inherit the cap naturally via directory variable scope.
    set(CMAKE_NINJA_JOB_POOL_CXX_MODULE_DEPFILE_GEN scan_modules)
    set(CMAKE_NINJA_JOB_POOL_C_MODULE_DEPFILE_GEN   scan_modules)
    unset(_LINK_POOL)
    message(STATUS "[Ninja pools] compile=${NPROC}  link=2/4  scan_modules=2")
endif()

# ─── C++23 Standard ───────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Suppress macOS ld64 duplicate library warnings ──────────────────────────
if(APPLE)
    add_link_options("LINKER:-no_warn_duplicate_libraries")
endif()

# ─── C++20/23 Modules Support ─────────────────────────────────────────────────
set(CMAKE_CXX_SCAN_FOR_MODULES ON)


# Include custom module helpers
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FindModules)

# ─── Build Type Default ───────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# ─── Compiler Flags for C++23 Named Modules ───────────────────────────────────
# CMake 3.28+ with FILE_SET CXX_MODULES handles module compilation automatically.
# No additional flags are needed for named modules (export module / import).
# Note: Do NOT use -fmodules/-fbuiltin-module-map (Clang header modules) as they
# conflict with C++23 named modules.
if(MSVC)
    add_compile_options(/experimental:module)
endif()

# ─── General Compiler Warnings ────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    add_compile_options(-Wall -Wextra -Wpedantic)
elseif(MSVC)
    add_compile_options(/W4)
endif()

# ─── Find System Packages ─────────────────────────────────────────────────────
# Disable module scanning temporarily for find_package to work
set(CMAKE_CXX_SCAN_FOR_MODULES_OLD ${CMAKE_CXX_SCAN_FOR_MODULES})
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
set(CMAKE_CXX_SCAN_FOR_MODULES ${CMAKE_CXX_SCAN_FOR_MODULES_OLD})

# ─── FetchContent for Header-Only / Build-From-Source Dependencies ────────────
include(FetchContent)

# yyjson - high-performance JSON library
FetchContent_Declare(
    yyjson
    GIT_REPOSITORY https://github.com/ibireme/yyjson.git
    GIT_TAG        0.10.0
    GIT_SHALLOW    TRUE
)

# libuv - cross-platform async I/O library
FetchContent_Declare(
    libuv
    GIT_REPOSITORY https://github.com/libuv/libuv.git
    GIT_TAG        v1.48.0
    GIT_SHALLOW    TRUE
)
set(LIBUV_BUILD_TESTS OFF CACHE INTERNAL "")

# cpp-httplib - header-only HTTP library
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.0
    GIT_SHALLOW    TRUE
)

# FTXUI - terminal UI library
FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG        v5.0.0
    GIT_SHALLOW    TRUE
)
set(FTXUI_BUILD_DOCS OFF CACHE INTERNAL "")
set(FTXUI_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(FTXUI_BUILD_TESTS OFF CACHE INTERNAL "")

# Google Benchmark - micro-benchmarking library
FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.1
    GIT_SHALLOW    TRUE
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE INTERNAL "")
set(BENCHMARK_ENABLE_INSTALL OFF CACHE INTERNAL "")
set(BENCHMARK_ENABLE_WERROR OFF CACHE INTERNAL "")

# Disable module scanning for FetchContent_MakeAvailable
set(CMAKE_CXX_SCAN_FOR_MODULES_OLD2 ${CMAKE_CXX_SCAN_FOR_MODULES})
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
FetchContent_MakeAvailable(yyjson libuv httplib ftxui benchmark)
set(CMAKE_CXX_SCAN_FOR_MODULES ${CMAKE_CXX_SCAN_FOR_MODULES_OLD2})

# ─── Subdirectories ───────────────────────────────────────────────────────────
add_subdirectory(src)

# ─── Unit Tests (GoogleTest) ──────────────────────────────────────────────────
option(CC_BUILD_TESTS "Build unit tests" ON)
if(CC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ─── Install Configuration ────────────────────────────────────────────────────
# include(GNUInstallDirs)
# include(CMakePackageConfigHelpers)
# 
# # Install the main executable
# install(TARGETS cc_app
#     RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
# )
# 
# # Install library targets for downstream consumption
# install(TARGETS
#     cc_core
#     cc_tools
#     cc_commands
#     cc_services
#     cc_ui
#     cc_state
#     cc_bridge
#     cc_skills
#     cc_utils
#     EXPORT CC-REPLTargets
#     LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
#     ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
#     INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
# )
# 
# # Generate and install package config
# install(EXPORT CC-REPLTargets
#     FILE CC-REPLTargets.cmake
#     NAMESPACE CC-REPL::
#     DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CC-REPL
# )
# 
# configure_package_config_file(
#     "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CC-REPLConfig.cmake.in"
#     "${CMAKE_CURRENT_BINARY_DIR}/CC-REPLConfig.cmake"
#     INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CC-REPL
# )
# 
# write_basic_package_version_file(
#     "${CMAKE_CURRENT_BINARY_DIR}/CC-REPLConfigVersion.cmake"
#     VERSION ${PROJECT_VERSION}
#     COMPATIBILITY SameMajorVersion
# )
# 
# install(FILES
#     "${CMAKE_CURRENT_BINARY_DIR}/CC-REPLConfig.cmake"
#     "${CMAKE_CURRENT_BINARY_DIR}/CC-REPLConfigVersion.cmake"
#     DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CC-REPL
# )
