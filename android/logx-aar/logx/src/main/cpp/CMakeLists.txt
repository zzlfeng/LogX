# logx-aar 精简构建:单源复用仓库 core(含 vendored zstd/mbedtls 接线)与 JNI 胶水,
# 与根 CMakeLists.txt 的 ANDROID 分支同源,零拷贝零陈旧产物。
cmake_minimum_required(VERSION 3.22)
project(logx_aar C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 本文件位于 android/logx-aar/logx/src/main/cpp/,上溯 6 级为仓库根。
get_filename_component(LOGX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../" ABSOLUTE)

# 根 CMakeLists 的 ANDROID 默认段在此等价复刻:AGP 工具链恒定义 ANDROID,
# 而此变量若不预设,core/CMakeLists 的 option() 会读到空默认,静默落 OFF。
set(LOGX_WITH_ZSTD_DEFAULT ON)
set(LOGX_WITH_AESGCM_DEFAULT ON)
set(LOGX_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
set(LOGX_BUILD_STRESS OFF CACHE BOOL "" FORCE)
set(LOGX_BUILD_BENCH  OFF CACHE BOOL "" FORCE)

add_subdirectory(${LOGX_ROOT}/core ${CMAKE_CURRENT_BINARY_DIR}/core_build)

add_library(logx SHARED ${LOGX_ROOT}/android/cpp/logx_jni.cpp)
target_link_libraries(logx PRIVATE logx_core log)
