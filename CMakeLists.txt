# 顶层 CMake:聚合 core + 单测。
cmake_minimum_required(VERSION 3.16)
project(logx CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(LOGX_BUILD_TESTS "Build unit tests" ON)
option(LOGX_WITH_ZSTD   "Link real zstd"   OFF)
option(LOGX_WITH_AESGCM "Link real AES-GCM (mbedtls)" OFF)

add_subdirectory(core)

if(LOGX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
endif()