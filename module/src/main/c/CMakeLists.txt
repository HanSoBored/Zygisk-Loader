cmake_minimum_required(VERSION 3.22.1)
# CMake 3.22.1 matches the NDK r26+ bundle. If using an older NDK,
# update the minimum version to match your NDK's bundled CMake.
project(zygisk-loader)

# C17 is the highest C standard fully supported by NDK r26+'s compiler.
# C23 features like typeof(), bool, and static_assert are available as
# extensions in C17 mode but are not required for this codebase.
# C23 was tried first but caused compatibility issues with some NDK headers
# (e.g., stdbit.h, unreachable()) — C17 is the pragmatic choice.
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(${MODULE_NAME} SHARED main.c)

target_compile_options(${MODULE_NAME} PRIVATE
    -fvisibility=hidden
    -fstack-protector-strong
    # Warnings
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wnull-dereference
    -Wformat=2
    -Wstrict-prototypes
    -Wold-style-definition
    -Wmissing-prototypes
    -Wimplicit-fallthrough
    -Wvla
    -Werror=implicit-function-declaration
    -Werror=int-conversion
    # Optimization
    -Os
    # Code generation
    # Note: -fno-exceptions is a C++ flag with no effect on C compilation.
    # It's included for consistency in case C++ sources are added later.
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
)

# Debug builds always get debug symbols, regardless of sanitizer status
string(TOLOWER "${CMAKE_BUILD_TYPE}" _build_type_lower)
if(_build_type_lower STREQUAL "debug")
    target_compile_options(${MODULE_NAME} PRIVATE -g)
endif()

# Sanitizers are Debug-only because:
# 1. ASan/UBSan require debug symbols for meaningful stack traces.
# 2. Release builds are size-optimized (-Os) and run on end-user devices.
# A CI workflow with Debug builds + sanitizers is recommended for pre-release
# memory bug detection.
if(ENABLE_SANITIZERS)
    if(_build_type_lower STREQUAL "debug")
        message(STATUS "Sanitizers enabled for Debug build")
        target_compile_options(${MODULE_NAME} PRIVATE -fsanitize=address,undefined)
        target_link_options(${MODULE_NAME} PRIVATE -fsanitize=address,undefined)
    else()
        message(FATAL_ERROR "ENABLE_SANITIZERS=ON but build type is not Debug — "
                            "sanitizers need a Debug build to produce meaningful output")
    endif()
endif()

target_link_libraries(${MODULE_NAME} log dl android)
