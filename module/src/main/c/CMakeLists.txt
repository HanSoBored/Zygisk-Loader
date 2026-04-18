cmake_minimum_required(VERSION 3.22.1)
project(zygisk-loader)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fvisibility=hidden -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wdouble-promotion -Wnull-dereference -Wformat=2 -Wstrict-prototypes -Wold-style-definition -Wmissing-prototypes -Wimplicit-fallthrough -Wvla -Werror=implicit-function-declaration -Werror=int-conversion -Os -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables")

option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(${MODULE_NAME} SHARED main.c)

if(ENABLE_SANITIZERS)
    target_compile_options(${MODULE_NAME} PRIVATE -fsanitize=address,undefined -g)
    target_link_options(${MODULE_NAME} PRIVATE -fsanitize=address,undefined)
endif()

target_link_libraries(${MODULE_NAME} log dl android)
