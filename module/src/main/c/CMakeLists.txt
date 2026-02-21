cmake_minimum_required(VERSION 3.22.1)
project(zygisk-loader)

# Mengatur agar biner yang dihasilkan kecil & teroptimasi
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fvisibility=hidden -Wall -Wextra -Os -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables")

add_library(${MODULE_NAME} SHARED main.c)

# Log dan Android dlfcn & jni
target_link_libraries(${MODULE_NAME} log dl android)
