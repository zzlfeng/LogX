# Unit tests via gtest. 关闭加密/压缩运行;M3 阶段补回 zstd/AES-GCM 用例。
cmake_minimum_required(VERSION 3.16)
project(logx_unit_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(GTest REQUIRED)

add_executable(logx_unit_tests
    test_format.cpp
    test_staging.cpp
    test_recovery.cpp
    test_flusher.cpp
)
target_link_libraries(logx_unit_tests
    PRIVATE
    logx_core
    GTest::gtest
    GTest::gtest_main
)
if(NOT WIN32)
    target_link_libraries(logx_unit_tests PRIVATE pthread)
endif()
target_include_directories(logx_unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/../core/include
)
add_test(NAME logx_unit_tests COMMAND logx_unit_tests)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(logx_unit_tests PRIVATE -Wall -Wextra)
endif()