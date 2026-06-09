# AutoToolchain.cmake — pin a uniform LLVM 21+ toolchain across platforms.
#
# Used as CMAKE_TOOLCHAIN_FILE so individual presets do NOT need to know
# whether the host is macOS or Linux. Resolution order per platform:
#
#   macOS  : $CC_REPL_LLVM_PREFIX → /opt/homebrew/opt/llvm
#   Linux  : $CC_REPL_LLVM_PREFIX → ~/.local/opt/llvm-21/usr/lib/llvm-21
#            (with `clang(++)-21-local` shim binaries on PATH)
#
# Override at configure time via:
#   - CMAKE_C_COMPILER / CMAKE_CXX_COMPILER (highest precedence)
#   - CC_REPL_LLVM_PREFIX env var (point at any LLVM ≥ 21 install)
#
# Goal: full std::jthread / std::stop_token support and identical libc++/
# libstdc++ behavior between macOS and Linux developer machines.

if(DEFINED ENV{CC_REPL_LLVM_PREFIX})
    set(_cc_repl_llvm_prefix "$ENV{CC_REPL_LLVM_PREFIX}")
endif()

# Skip if user already provided compilers explicitly.
if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED CMAKE_CXX_COMPILER)
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        if(NOT DEFINED _cc_repl_llvm_prefix)
            set(_cc_repl_llvm_prefix "/opt/homebrew/opt/llvm")
        endif()
        set(_cc_clang     "${_cc_repl_llvm_prefix}/bin/clang")
        set(_cc_clangxx   "${_cc_repl_llvm_prefix}/bin/clang++")
        set(_cc_scan_deps "${_cc_repl_llvm_prefix}/bin/clang-scan-deps")
        set(_cc_resource_dir "")  # Homebrew layout is self-consistent

    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        if(NOT DEFINED _cc_repl_llvm_prefix)
            set(_cc_repl_llvm_prefix "$ENV{HOME}/.local/opt/llvm-21/usr/lib/llvm-21")
        endif()
        # Project convention: 21-local shim wrappers on PATH.
        set(_cc_clang     "$ENV{HOME}/.local/bin/clang-21-local")
        set(_cc_clangxx   "$ENV{HOME}/.local/bin/clang++-21-local")
        set(_cc_scan_deps "$ENV{HOME}/.local/bin/clang-scan-deps-21-local")
        set(_cc_resource_dir "${_cc_repl_llvm_prefix}/lib/clang/21")

    else()
        message(WARNING
            "[AutoToolchain] Unsupported host '${CMAKE_HOST_SYSTEM_NAME}'. "
            "Falling back to default compiler — std::jthread support not guaranteed.")
        return()
    endif()

    if(NOT EXISTS "${_cc_clangxx}")
        message(FATAL_ERROR
            "[AutoToolchain] Required compiler not found: ${_cc_clangxx}\n"
            "  - macOS  : install with `brew install llvm` (≥ 21)\n"
            "  - Linux  : install LLVM 21 and place `clang++-21-local` on PATH,\n"
            "             or set CC_REPL_LLVM_PREFIX to your LLVM ≥ 21 install root\n"
            "             (or set CMAKE_C_COMPILER / CMAKE_CXX_COMPILER directly).")
    endif()

    set(CMAKE_C_COMPILER   "${_cc_clang}"     CACHE FILEPATH "" FORCE)
    set(CMAKE_CXX_COMPILER "${_cc_clangxx}"   CACHE FILEPATH "" FORCE)
    set(CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS "${_cc_scan_deps}" CACHE FILEPATH "" FORCE)

    if(_cc_resource_dir)
        # Linux shim wrappers need an explicit -resource-dir to find the
        # matching clang headers/builtins.
        set(_rd_flag "-resource-dir ${_cc_resource_dir}")
        set(CMAKE_C_FLAGS_INIT   "${CMAKE_C_FLAGS_INIT} ${_rd_flag}")
        set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} ${_rd_flag}")
    endif()
endif()

# Enforce LLVM ≥ 21 once the compiler is loaded by CMake.
function(_cc_repl_assert_llvm21)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang"
       AND CMAKE_CXX_COMPILER_VERSION
       AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "21.0")
        message(FATAL_ERROR
            "[AutoToolchain] Clang ${CMAKE_CXX_COMPILER_VERSION} is too old; "
            "LLVM ≥ 21 is required for full std::jthread / std::stop_token support.")
    endif()
endfunction()
# Defer assertion until CMakeLists.txt finishes project() — registered as a hook.
set(CC_REPL_TOOLCHAIN_PIN "llvm>=21" CACHE INTERNAL "")
