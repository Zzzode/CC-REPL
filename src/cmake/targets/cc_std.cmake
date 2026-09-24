# ─── cc_std: the C++ standard library named module (`import std;`) ──────────
#
# libc++ ships the module unit as `<prefix>/share/libc++/v1/std.cppm`. We do
# NOT vendor that generated file: it is tied to the exact libc++ version.
# Instead we locate the one matching the configured compiler and compile it
# as a normal CXX_MODULES source. Override with -DLOOM_LIBCXX_MODULE_DIR=.
#
# Every other target links cc_std (see link_libraries() before the target
# includes below), so its BMI is on each TU's module map.

set(_cc_std_candidates)

# Explicit override wins.
if(LOOM_LIBCXX_MODULE_DIR)
  list(APPEND _cc_std_candidates "${LOOM_LIBCXX_MODULE_DIR}/std.cppm")
endif()

# Derive <prefix>/share/libc++/v1 from <prefix>/bin/clang++. This is the
# authoritative lookup: it tracks whatever toolchain the compiler lives in
# (Homebrew llvm@N, Apple LLVM, a custom prefix), so it cannot silently pick
# a different installed LLVM.
get_filename_component(_cc_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(_cc_prefix "${_cc_compiler_dir}/.." ABSOLUTE)
list(APPEND _cc_std_candidates
    "${_cc_prefix}/share/libc++/v1/std.cppm")

# Fixed system layouts only (no versionless Homebrew glob — on a multi-LLVM
# box /opt/homebrew/opt/llvm or .../opt/llvm may point at a DIFFERENT LLVM
# than CMAKE_CXX_COMPILER, which would compile std.cppm with the wrong
# libc++). The compiler-derived path above covers Homebrew.
file(GLOB _cc_std_glob
    /usr/share/libc++/v1/std.cppm
    /usr/local/share/libc++/v1/std.cppm
    /usr/local/opt/llvm/share/libc++/v1/std.cppm
    /opt/homebrew/opt/llvm@22/share/libc++/v1/std.cppm)
list(APPEND _cc_std_candidates ${_cc_std_glob})

set(LOOM_STD_CPPM "")
foreach(_c IN LISTS _cc_std_candidates)
  if(EXISTS "${_c}" AND LOOM_STD_CPPM STREQUAL "")
    set(LOOM_STD_CPPM "${_c}")
  endif()
endforeach()

if(NOT LOOM_STD_CPPM)
  message(FATAL_ERROR
    "Could not locate libc++ std.cppm for compiler ${CMAKE_CXX_COMPILER}. "
    "Pass -DLOOM_LIBCXX_MODULE_DIR=<dir containing std.cppm>.")
endif()
message(STATUS "cc_std: using ${LOOM_STD_CPPM}")
get_filename_component(_cc_std_dir "${LOOM_STD_CPPM}" DIRECTORY)

add_library(cc_std STATIC)
# The unit lives outside the source tree, which a FILE_SET cannot reference
# directly; stage a copy in the build tree at configure time.
set(_cc_std_staged "${CMAKE_CURRENT_BINARY_DIR}/cc_std/std.cppm")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cc_std")
configure_file("${LOOM_STD_CPPM}" "${_cc_std_staged}" COPYONLY)
# Re-copy if the toolchain's std.cppm changes in place (configure_file's
# implicit dependency covers the staged input; declare it explicitly too).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${LOOM_STD_CPPM}")
target_sources(cc_std
    PUBLIC
    FILE_SET cxx_modules
    TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_BINARY_DIR}/cc_std"
    FILES "${_cc_std_staged}")
# The unit declares `export std;`; suppress the reserved-name diagnostic and
# stop implicit module maps so only this BMI provides std. It is vendor-
# generated code compiled OUTSIDE -isystem, so silence all warnings for this
# one target — a future toolchain must not be able to -Werror our build on a
# warning inside its own std module implementation.
target_compile_options(cc_std PRIVATE
    -Wno-reserved-module-identifier
    -fno-implicit-module-maps
    -w)
# ALSO pin the same suppressions per source file. CMake 4.x compiles module
# BMIs via a synthesized `target@synth` precompile rule that — unlike 3.31's
# scanned-object rule — does not inherit the target's PRIVATE compile options
# (observed on macos-14 with Homebrew cmake 4: the std.cppm BMI rule carried
# the global -Werror but not this target's -w, and clang promoted
# -Wreduced-bmi-output-overrided to a hard error). Per-source COMPILE_FLAGS
# reach every compile of this source on both generators.
set_source_files_properties("${_cc_std_staged}" PROPERTIES
    COMPILE_FLAGS "-Wno-reserved-module-identifier -fno-implicit-module-maps -w")
# std.cppm includes sibling fragments (`std/algorithm.inc`, ...) that stay
# in the toolchain share dir; make them resolvable for this target only.
target_include_directories(cc_std SYSTEM PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/cc_std"
    "${_cc_std_dir}")
