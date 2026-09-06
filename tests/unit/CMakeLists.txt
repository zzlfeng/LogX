# Unit tests via gtest. 关闭加密/压缩运行;M3 阶段补回 zstd/AES-GCM 用例。
cmake_minimum_required(VERSION 3.16)
project(logx_unit_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 优先复用环境里已有的 Android/桌面 gtest;找不到时通过 FetchContent
# 拉取 googletest 源码并在构建目录内交叉编译(产物不落 NDK,不污染 NDK)。
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    message(STATUS "GTest not found, fetching googletest via FetchContent")
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(logx_unit_tests
    test_format.cpp
    test_staging.cpp
    test_recovery.cpp
    test_flusher.cpp
    test_codec.cpp
    test_robustness.cpp
)
target_link_libraries(logx_unit_tests
    PRIVATE
    logx_core
    GTest::gtest
    GTest::gtest_main
)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(logx_unit_tests PRIVATE Threads::Threads)
endif()
target_include_directories(logx_unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    # C-04 真机用例直接调用 vendored mbedtls C ABI(aesce.c 符号经 logx_core
    # 静态链接进本测试);include 路径在 logx_core 为 PRIVATE,此处需显式补。
    ${CMAKE_SOURCE_DIR}/third_party/mbedtls/include
)
add_test(NAME logx_unit_tests COMMAND logx_unit_tests)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(logx_unit_tests PRIVATE -Wall -Wextra)
endif()