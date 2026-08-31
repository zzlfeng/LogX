# logx core: 纯 C++17 静态库,零平台依赖(经 PAL 抽象)。
# 编译选项约定:用宏切换 zstd / AES-GCM 真实接入(M3 阶段)。

cmake_minimum_required(VERSION 3.16)
project(logx_core CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_library(logx_core STATIC
    src/staging.cpp
    src/codec.cpp
    src/flusher.cpp
    src/recovery.cpp
    src/format.cpp
    src/pal_posix.cpp
    src/logx_c.cpp
)

target_include_directories(logx_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_libraries(logx_core PUBLIC pthread)
endif()

# ---- 构建开关 ----
# LOGX_WITH_ZSTD: 接入实 zstd(否则 flag=zstd 的块走 noop 编解码,文件大小不缩水)
# LOGX_WITH_AESGCM: 接入实 AES-GCM(否则 flag=enc 的块以明文落盘,仅占位用于接口验收)
option(LOGX_WITH_ZSTD "Link real zstd" OFF)
option(LOGX_WITH_AESGCM "Link real AES-GCM (mbedtls)" OFF)

if(LOGX_WITH_ZSTD)
    target_compile_definitions(logx_core PUBLIC LOGX_WITH_ZSTD=1)
    # find_package(ZSTD) 由上游工程负责;此处不强制
endif()
if(LOGX_WITH_AESGCM)
    target_compile_definitions(logx_core PUBLIC LOGX_WITH_AESGCM=1)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(logx_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()