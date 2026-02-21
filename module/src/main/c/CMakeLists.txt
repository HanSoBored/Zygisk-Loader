cmake_minimum_required(VERSION 3.22.1)
project(zygisk-loader)

# Set the resulting binary to be small & optimized
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fvisibility=hidden -Wall -Wextra -Os -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables")

add_library(${MODULE_NAME} SHARED main.c)

# Log and Android dlfcn & jni
target_link_libraries(${MODULE_NAME} log dl android)
