# FindModules.cmake
# Helper module to configure C++20/23 module support across compilers.
# Provides detection, validation, and fallback logic for named modules.

# ─── Minimum Version Gate ─────────────────────────────────────────────────────
# CMake 3.28+ is required for native C++ module scanning (P1689 dependency format)
if(CMAKE_VERSION VERSION_LESS "3.28")
    message(FATAL_ERROR
        "C++23 Modules require CMake >= 3.28. Current version: ${CMAKE_VERSION}")
endif()

# ─── Compiler Detection & Validation ─────────────────────────────────────────
# Validate that the compiler supports C++20/23 named modules
function(cc_check_modules_support)
    set(_modules_supported FALSE)

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # GCC 14+ has production-ready module support
        if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "14.0")
            set(_modules_supported TRUE)
            message(STATUS "[Modules] GCC ${CMAKE_CXX_COMPILER_VERSION} - full module support")
        elseif(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "11.0")
            set(_modules_supported TRUE)
            message(WARNING
                "[Modules] GCC ${CMAKE_CXX_COMPILER_VERSION} - experimental module support. "
                "Consider upgrading to GCC 14+ for production use.")
        else()
            message(FATAL_ERROR
                "[Modules] GCC ${CMAKE_CXX_COMPILER_VERSION} does not support C++20 modules. "
                "Minimum required: GCC 11.0")
        endif()

    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # Clang 17+ has good module support
        if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "17.0")
            set(_modules_supported TRUE)
            message(STATUS "[Modules] Clang ${CMAKE_CXX_COMPILER_VERSION} - full module support")
        elseif(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "15.0")
            set(_modules_supported TRUE)
            message(WARNING
                "[Modules] Clang ${CMAKE_CXX_COMPILER_VERSION} - partial module support. "
                "Consider upgrading to Clang 17+ for production use.")
        else()
            message(FATAL_ERROR
                "[Modules] Clang ${CMAKE_CXX_COMPILER_VERSION} does not support C++20 modules. "
                "Minimum required: Clang 15.0")
        endif()

    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        # AppleClang 15.0.3+ (Xcode 15.3+) has initial module support
        if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "15.0.3")
            set(_modules_supported TRUE)
            message(STATUS "[Modules] AppleClang ${CMAKE_CXX_COMPILER_VERSION} - module support available")
        else()
            message(FATAL_ERROR
                "[Modules] AppleClang ${CMAKE_CXX_COMPILER_VERSION} does not fully support C++20 modules. "
                "Minimum required: AppleClang 15.0.3 (Xcode 15.3)")
        endif()

    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        # MSVC 19.34+ (VS 2022 17.4+) has module support
        if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "19.34")
            set(_modules_supported TRUE)
            message(STATUS "[Modules] MSVC ${CMAKE_CXX_COMPILER_VERSION} - module support available")
        else()
            message(FATAL_ERROR
                "[Modules] MSVC ${CMAKE_CXX_COMPILER_VERSION} does not support C++20 modules. "
                "Minimum required: MSVC 19.34 (VS 2022 17.4)")
        endif()

    else()
        message(FATAL_ERROR
            "[Modules] Unknown compiler '${CMAKE_CXX_COMPILER_ID}'. "
            "Supported compilers: GCC 11+, Clang 15+, AppleClang 15.0.3+, MSVC 19.34+")
    endif()

    # Export result to parent scope
    set(CC_MODULES_SUPPORTED ${_modules_supported} PARENT_SCOPE)
endfunction()

# ─── Configure Module Build Directories ──────────────────────────────────────
# Set up BMI (Binary Module Interface) output directories
function(cc_configure_module_paths)
    # Directory for generated BMI files (.gcm for GCC, .pcm for Clang, .ifc for MSVC)
    set(CC_MODULE_BMI_DIR "${CMAKE_BINARY_DIR}/modules_bmi" PARENT_SCOPE)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/modules_bmi")

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # GCC stores module files as .gcm
        set(CMAKE_CXX_MODULE_BMI_DIRECTORY "${CMAKE_BINARY_DIR}/modules_bmi" PARENT_SCOPE)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Clang stores module files as .pcm
        set(CMAKE_CXX_MODULE_BMI_DIRECTORY "${CMAKE_BINARY_DIR}/modules_bmi" PARENT_SCOPE)
    endif()
endfunction()

# ─── Utility: Add a module library with standard configuration ────────────────
# Convenience wrapper for declaring a module library with FILE_SET CXX_MODULES
function(cc_add_module_library TARGET_NAME)
    cmake_parse_arguments(ARG "" "" "MODULE_FILES;DEPENDENCIES" ${ARGN})

    if(NOT ARG_MODULE_FILES)
        message(FATAL_ERROR "cc_add_module_library(${TARGET_NAME}): MODULE_FILES is required")
    endif()

    add_library(${TARGET_NAME})
    target_sources(${TARGET_NAME}
        PUBLIC FILE_SET CXX_MODULES FILES
            ${ARG_MODULE_FILES}
    )

    if(ARG_DEPENDENCIES)
        target_link_libraries(${TARGET_NAME} PUBLIC ${ARG_DEPENDENCIES})
    endif()

    # Apply module-specific compile features
    target_compile_features(${TARGET_NAME} PUBLIC cxx_std_23)
endfunction()

# ─── Run Checks ──────────────────────────────────────────────────────────────
cc_check_modules_support()
cc_configure_module_paths()

if(NOT CC_MODULES_SUPPORTED)
    message(FATAL_ERROR
        "[Modules] C++20/23 module support could not be confirmed for this toolchain.")
endif()

message(STATUS "[Modules] Configuration complete. BMI directory: ${CMAKE_BINARY_DIR}/modules_bmi")
