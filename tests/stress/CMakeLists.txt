# tests/stress/CMakeLists.txt - 压力与高并发测试套件
cmake_minimum_required(VERSION 3.16)
project(logx_stress_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
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

add_executable(logx_stress_tests
    test_concurrent.cpp
    test_crash_recovery.cpp
)

target_link_libraries(logx_stress_tests
    PRIVATE
    logx_core
    GTest::gtest
    GTest::gtest_main
)

if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(logx_stress_tests PRIVATE Threads::Threads)
endif()

target_include_directories(logx_stress_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/tests
)

add_test(NAME logx_stress_tests COMMAND logx_stress_tests)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(logx_stress_tests PRIVATE -Wall -Wextra)
endif()

# T-01: 独立子进程强杀测试 (进程级真正崩溃与恢复)
add_executable(logx_crash_fork_tests
    test_crash_fork.cpp
)

target_link_libraries(logx_crash_fork_tests
    PRIVATE
    logx_core
    GTest::gtest
)

if(NOT WIN32)
    target_link_libraries(logx_crash_fork_tests PRIVATE Threads::Threads)
endif()

target_include_directories(logx_crash_fork_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/tests
)

add_test(NAME logx_crash_fork_tests COMMAND logx_crash_fork_tests)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(logx_crash_fork_tests PRIVATE -Wall -Wextra)
endif()

