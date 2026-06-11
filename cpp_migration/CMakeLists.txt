cmake_minimum_required(VERSION 3.28)

project(CC-REPL
    VERSION 1.0.0
    DESCRIPTION "CC-REPL: A C++23 Modules-based CLI REPL"
    LANGUAGES CXX
)

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

# Disable module scanning for FetchContent_MakeAvailable
set(CMAKE_CXX_SCAN_FOR_MODULES_OLD2 ${CMAKE_CXX_SCAN_FOR_MODULES})
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
FetchContent_MakeAvailable(yyjson libuv httplib ftxui)
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
