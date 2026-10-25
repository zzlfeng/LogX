# logx core: 纯 C++17 静态库,零平台依赖(经 PAL 抽象)。
# 编译选项约定:用宏切换 zstd / AES-GCM 真实接入(M3 阶段)。

cmake_minimum_required(VERSION 3.16)
# C 语言必须显式启用:LOGX_WITH_ZSTD=ON 时 zstd 源文件(.c)才会被当作
# 编译项而非 None(否则编进库里的只有声明,链接期报 ZSTD_* 未解析)。
project(logx_core C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(LOGX_CORE_SOURCES
    src/staging.cpp
    src/codec.cpp
    src/flusher.cpp
    src/recovery.cpp
    src/format.cpp
    src/logx_c.cpp
)
# PAL 按平台选择:Windows 用 pal_win32,其余(Android/Linux/macOS)用 pal_posix。
if(WIN32)
    list(APPEND LOGX_CORE_SOURCES src/pal_win32.cpp)
else()
    list(APPEND LOGX_CORE_SOURCES src/pal_posix.cpp)
endif()

add_library(logx_core STATIC ${LOGX_CORE_SOURCES})
set_target_properties(logx_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# D6-B2: MinSizeRel 下将写入热路径 TU 单独提升为 -O2。
# 不能对 logx_core 整体加 -O2: zstd/mbedtls 也在本 target, 会抵消体积裁剪。
# 仅 Clang/GNU 使用 -O2 语法; host MSVC 保持默认, 该选项只由 AAR release 打开。
option(LOGX_HOTPATH_O2 "Compile write hot-path TUs with -O2 under MinSizeRel" OFF)
if(LOGX_HOTPATH_O2 AND CMAKE_BUILD_TYPE STREQUAL "MinSizeRel"
   AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties(
        src/logx_c.cpp
        src/flusher.cpp
        src/format.cpp
        src/staging.cpp
        PROPERTIES COMPILE_OPTIONS "-O2")
endif()

target_include_directories(logx_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Android")
    # Android 上 pthread 并入 libc,不存在 libpthread.so;
    # Threads::Threads 在 Android 下解析为 -pthread 编译选项而非 -lpthread。
    find_package(Threads REQUIRED)
    target_link_libraries(logx_core PUBLIC Threads::Threads)
endif()

# ---- 构建开关 ----
# LOGX_WITH_ZSTD: 接入实 zstd(否则 flag=zstd 的块走 noop 编解码,文件大小不缩水)
# LOGX_WITH_AESGCM: 接入实 AES-GCM(否则 flag=enc 的块以明文落盘,仅占位用于接口验收)
if(ANDROID)
    set(LOGX_WITH_ZSTD_DEFAULT ON)
    set(LOGX_WITH_AESGCM_DEFAULT ON)
else()
    set(LOGX_WITH_ZSTD_DEFAULT OFF)
    set(LOGX_WITH_AESGCM_DEFAULT OFF)
endif()

option(LOGX_WITH_ZSTD "Link real zstd" ${LOGX_WITH_ZSTD_DEFAULT})
option(LOGX_WITH_AESGCM "Link real AES-GCM (mbedtls)" ${LOGX_WITH_AESGCM_DEFAULT})

if(LOGX_WITH_ZSTD)
    target_compile_definitions(logx_core PUBLIC LOGX_WITH_ZSTD=1)
    # 使用仓库内 vendor 的 zstd 源码(third_party/zstd/lib,v1.5.7,零改动)。
    # 单线程配置:ZSTD_MULTITHREAD 未定义,ZSTDmt_compress.c 不参与编译。
    set(LOGX_ZSTD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/zstd/lib)
    if(NOT EXISTS ${LOGX_ZSTD_DIR}/zstd.h)
        message(FATAL_ERROR "LOGX_WITH_ZSTD=ON but ${LOGX_ZSTD_DIR} not found")
    endif()
    target_sources(logx_core PRIVATE
        ${LOGX_ZSTD_DIR}/common/debug.c
        ${LOGX_ZSTD_DIR}/common/entropy_common.c
        ${LOGX_ZSTD_DIR}/common/error_private.c
        ${LOGX_ZSTD_DIR}/common/fse_decompress.c
        ${LOGX_ZSTD_DIR}/common/xxhash.c
        ${LOGX_ZSTD_DIR}/common/zstd_common.c
        ${LOGX_ZSTD_DIR}/compress/fse_compress.c
        ${LOGX_ZSTD_DIR}/compress/hist.c
        ${LOGX_ZSTD_DIR}/compress/huf_compress.c
        ${LOGX_ZSTD_DIR}/compress/zstd_compress.c
        ${LOGX_ZSTD_DIR}/compress/zstd_compress_literals.c
        ${LOGX_ZSTD_DIR}/compress/zstd_compress_sequences.c
        ${LOGX_ZSTD_DIR}/compress/zstd_compress_superblock.c
        ${LOGX_ZSTD_DIR}/compress/zstd_double_fast.c
        ${LOGX_ZSTD_DIR}/compress/zstd_fast.c
        ${LOGX_ZSTD_DIR}/compress/zstd_lazy.c
        ${LOGX_ZSTD_DIR}/compress/zstd_ldm.c
        ${LOGX_ZSTD_DIR}/compress/zstd_opt.c
        ${LOGX_ZSTD_DIR}/compress/zstd_preSplit.c
        ${LOGX_ZSTD_DIR}/decompress/huf_decompress.c
        ${LOGX_ZSTD_DIR}/decompress/zstd_ddict.c
        ${LOGX_ZSTD_DIR}/decompress/zstd_decompress.c
        ${LOGX_ZSTD_DIR}/decompress/zstd_decompress_block.c
    )
    target_include_directories(logx_core PRIVATE ${LOGX_ZSTD_DIR})
    # 源码级裁剪(0-diff):仅用上游 EXCLUDE 宏剔除 Level 5+ 匹配引擎。
    # 不得定义 ZSTD_EXCLUDE_DFAST_BLOCK_COMPRESSOR:LogX Level 3 依赖 dfast。
    # ZSTD_NO_TRACE 会改变 CCtx/DCtx 布局,所有 zstd TU 必须 clean 重建。
    target_compile_definitions(logx_core PRIVATE
        ZSTD_EXCLUDE_GREEDY_BLOCK_COMPRESSOR=1
        ZSTD_EXCLUDE_LAZY_BLOCK_COMPRESSOR=1
        ZSTD_EXCLUDE_LAZY2_BLOCK_COMPRESSOR=1
        ZSTD_EXCLUDE_BTLAZY2_BLOCK_COMPRESSOR=1
        ZSTD_EXCLUDE_BTOPT_BLOCK_COMPRESSOR=1
        ZSTD_EXCLUDE_BTULTRA_BLOCK_COMPRESSOR=1
        ZSTD_STRIP_ERROR_STRINGS=1
        ZSTD_NO_TRACE=1
        ZSTD_DISABLE_ASM=1
    )
    # 单线程:pool.c/threading.c 无引用已移除;若未来定义 ZSTD_MULTITHREAD,
    # 必须恢复这两个文件并接入 zstdmt_compress.c。
    # zstd 以静态方式编入,不导出符号(SHARED 目标 liblogx.so 现有可见性保持)
    if(BUILD_SHARED_LIBS)
        set_target_properties(logx_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
    # Android 交叉编译下 zstd 的汇编文件(huf_decompress_amd64.S 仅 x86_64)
    # 走 C 实现,ZSTD_DISABLE_ASM 与上游默认一致,无需额外定义。
endif()
if(LOGX_WITH_AESGCM)
    target_compile_definitions(logx_core PUBLIC LOGX_WITH_AESGCM=1)
    set(LOGX_MBEDTLS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/mbedtls)
    if(NOT EXISTS ${LOGX_MBEDTLS_DIR}/include/mbedtls/gcm.h)
        message(FATAL_ERROR "LOGX_WITH_AESGCM=ON but ${LOGX_MBEDTLS_DIR} not found")
    endif()
    target_sources(logx_core PRIVATE
        ${LOGX_MBEDTLS_DIR}/src/aes.c
        ${LOGX_MBEDTLS_DIR}/src/gcm.c
        ${LOGX_MBEDTLS_DIR}/src/platform_util.c
    )
    target_include_directories(logx_core PRIVATE ${LOGX_MBEDTLS_DIR}/include)
    # ARMv8 Crypto Extensions(§10/T4.0-5):仅对 aesce.c 启用 +crypto 指令集,
    # 不影响其他翻译单元;aesce.c 自身在非 aarch64/无 crypto 目标下编译为空。
    set(LOGX_CE_SRC ${LOGX_MBEDTLS_DIR}/src/aesce.c)
    target_sources(logx_core PRIVATE ${LOGX_CE_SRC})
    if(ANDROID AND ANDROID_ABI STREQUAL "arm64-v8a")
        set_source_files_properties(${LOGX_CE_SRC} PROPERTIES
            COMPILE_OPTIONS "-march=armv8-a+crypto")
    elseif(NOT ANDROID AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set_source_files_properties(${LOGX_CE_SRC} PROPERTIES
            COMPILE_OPTIONS "-march=armv8-a+crypto")
    endif()
    if(BUILD_SHARED_LIBS)
        set_target_properties(logx_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(logx_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()