# logx-aar 精简构建:单源复用仓库 core(含 vendored zstd/mbedtls 接线)与 JNI 胶水,
# 与根 CMakeLists.txt 的 ANDROID 分支同源,零拷贝零陈旧产物。
cmake_minimum_required(VERSION 3.22)
project(logx_aar C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 本文件位于 android/logx-aar/logx/src/main/cpp/,上溯 6 级为仓库根。
get_filename_component(LOGX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../" ABSOLUTE)

# G2: AGP 对 release 变体传入 RelWithDebInfo,体积门禁要求有效配置为 MinSizeRel。
option(LOGX_AAR_NATIVE_MIN_SIZE "Force MinSizeRel for AAR release" OFF)
# D4-A: release 必须保留未 strip 符号;AGP 打包阶段仍会 strip。
option(LOGX_AAR_KEEP_SYMBOLS "Keep -g in MinSizeRel objects (release MUST be ON)" ON)

if(LOGX_AAR_NATIVE_MIN_SIZE AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "" FORCE)
    # CMake 3.22 的 MinSizeRel 默认是 -Os;G2/D4-A 要求 -Oz,故显式固定。
    # AGP/NDK 基线 flags 自带 -g,OFF 分支必须显式 -g0 才能形成有效 A/B。
    set(_logx_minsize_flags "-Oz -DNDEBUG")
    if(LOGX_AAR_KEEP_SYMBOLS)
        string(APPEND _logx_minsize_flags " -g")
    else()
        string(APPEND _logx_minsize_flags " -g0")
    endif()
    set(CMAKE_C_FLAGS_MINSIZEREL "${_logx_minsize_flags}" CACHE STRING "" FORCE)
    set(CMAKE_CXX_FLAGS_MINSIZEREL "${_logx_minsize_flags}" CACHE STRING "" FORCE)
endif()

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
