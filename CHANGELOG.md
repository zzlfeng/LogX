# LogX 开发记录与变更日志

> 记录构建历程、已修复问题与已知遗留项。对应方案文档 `LogX日志库最终实施方案.md`。
> 重要背景:本仓库代码在 2026-09-01 之前**从未在任何平台编译/运行过**,
> 首次构建(Windows/MSVC)集中暴露了大量真实 bug 与测试断言问题。

## 提交记录

| 提交 | 内容 |
|---|---|
| `2bbed42` | feat: LogX mmap 崩溃安全日志库 M1/M2 阶段核心实现(初始提交) |
| `d454dee` | fix: 修复首次编译暴露的 4 处真实 bug(MSVC 构建验证) |
| `fa24e2f` | feat: Windows(MSVC) 平台移植 + 核心零平台依赖落地,21 个单测全绿 |
| `30efff8` | build: 打通 Android NDK 交叉编译,新增 liblogx.so(JNI)并修复 3 处真实 bug |
| `e819ac2` | feat: 恢复对账 Σraw_len 精确回推 + zstd 真接入(M3 首步),32 单测全绿 |
| (本次) | fix & feat: 实施方案 v5 定稿升级 + JNI 补齐与零堆分配优化 + 多进程自动隔离,全单测通过 |

---

## 〇、实施方案 v5 定稿 + JNI 优化与补齐(本次)

对应方案文档评审优化定稿(v5, 2026-09-02)以及 Kotlin Facade 与 JNI 边界的契约闭环。

### 1. 方案文档升级至 v5 评审优化定稿版 (`LogX日志库最终实施方案.md`)

- **[P0] 环形坐标系规范化**(§3.1 / §5):
  - 修复伪代码中相对偏移 `[0, kDataSize)` 与物理基址（128B）混用的 Bug；
  - 统一规范 Header 与运行时变量使用环内相对偏移 `[0, 130944)`，物理地址统一由 `pos_ptr(pos) = mmap_base + 128 + pos` 换算。
- **[P0] 双 Header 写入增加内存屏障**(§4.2 / §10):
  - 在写完 Header A（含 CRC）后插入 `std::atomic_thread_fence(std::memory_order_seq_cst)` 内存屏障再写 Header B，消除 ARM 弱内存模型下由于 Store Buffer 重排导致的 Header A 写入落后可见性风险。
- **[P0] `LogX.flush()` 水位线与防饥饿控制**(§4.2 / §7):
  - 同步 `flush()` 引入目标提交水位（Target Watermark: `target_seqno`）机制，条件变量仅等待至 `committed_seqno >= target_seqno` 即返回，彻底避免高频写入下 `flush()` 陷入无限期阻塞或饥饿。
- **[P1] 多进程 Safe-by-Default 自动隔离**(§6 / §7 / §10):
  - 框架层在 `LogX.init()` 内部自动根据当前进程名后缀隔离 `logDir` 与 `stagingDir`（如 `:push` 进程自动使用 `logs_push/` 和 `staging_push.mmap`），无需业务方手动配置，从根源杜绝跨进程 mmap 覆写风险。
- **[P1] `LogX.export()` 协程化生命周期管理**(§7):
  - 导出接口提供 Kotlin `suspend fun export(...)`，明文临时文件在协程作用域（含异步网络上传）完全结束或异常退出后再执行删除，彻底消除异步上传时的 `FileNotFoundException`。
- **[P1] 切分序号升级为 2B (uint16_t)**(§3.1 / §4.4):
  - Header reserved 区域划出的追加目标缓存中，切分序号由 1B 升级为 2B（上限 65535，支撑 655GB/天），消除 1B 达 255 停写的边界风险。
- **[P1] JNI 零堆内存分配与性能调优**(§4.1 / §7 / §10):
  - 引入 `GetStringUTFRegion` 栈缓冲与 `GetPrimitiveArrayCritical`，实现热路径零堆分配与零 GC 抖动。
- **[P2] ARMv8 硬件加速与 Flusher 聚合优化**(§2 / §4.1 / §10):
  - 引入 ARMv8 Crypto Extensions 硬件加速 AES-GCM 指令（`__ARM_FEATURE_CRYPTO`）；Flusher 增加 50~100ms Debounce 超时聚合。
- **[P2] 测试矩阵补全**(§9):
  - 新增 32 位 ARM 未对齐内存访问测试与跨天极端时钟跳变（23:59:59 回拨至 00:00:01）测试用例。

### 2. JNI 胶水层与 Kotlin Facade 补齐与优化 (`logx_jni.cpp` / `LogX.kt`)

- **[P0 修复] 补齐缺失的 `nativeExport` JNI 实现** (`logx_jni.cpp`):
  - 原 `logx_jni.cpp` 缺少 `Java_com_example_logx_LogX_nativeExport` 的实现，导致 Kotlin 调用 `LogX.export()` 时报 `UnsatisfiedLinkError`；
  - 实现了 `Java_com_example_logx_LogX_nativeExport` 并封装 `export_trampoline` 回调桥，正确打通从 C 层 `logx_export` 到 Kotlin `ExportCallback.onExport` 的链路，并在回调完成后及时释放 LocalRef。
- **[性能优化] JNI 写入路径零堆分配** (`logx_jni.cpp`):
  - `nativeWrite` 对 `jTag` 优先使用栈缓冲 `char tag_buf[128]` 配合 `env->GetStringUTFRegion`（绝大多数 Tag $< 40$ 字符），避免调用 `GetStringUTFChars` 产生 malloc 堆分配；
  - 对 `jPayload` 采用 `env->GetPrimitiveArrayCritical` 与 `JNI_ABORT` 释放，避免数组拷贝与 ART GC 标记。
- **[健壮性] 句柄安全防卫** (`logx_jni.cpp`):
  - 所有 JNI 函数增加 `handle != 0` 及关键入参校验，防止未初始化或 close 后的野指针调用。
- **[Kotlin Facade 增强]** (`LogX.kt`):
  - 增加 `getProcessSuffix` 多进程名识别分流逻辑；
  - 增加 `suspend fun export(...)` 协程扩展函数。

### 3. 测试与验证

- 修复 `test_codec.cpp` 中的 `ZstdFlagFailsWhenNotBuilt` 单测断言（直接校验未启用 real zstd 时 `compress_block` 的失败行为）。
- Windows (MSVC 2022 / x64 Release): **28/28 单测 100% 通过**。

---

## 一、恢复精确回推 + zstd 真接入(`e819ac2`)

对应遗留项"三-1(恢复对账 `bytes_since_hdr` 未实现)"与"M3 zstd 单文件版"。

### 恢复对账:Σraw_len 精确回推(`recovery.cpp` / `flusher.cpp`)

- **`S_file > S_hdr` 分支落地**(spec §4.3):新增 `sum_raw_len_since()`
  (`flusher.cpp`),从文件中 seqno ∈ (S_hdr, S_file] 的块逐块累加块头自描述的
  `raw_len`,F = (F + Σraw_len) mod 数据区大小,seqno = S_file。此前该分支是
  占位(`bytes_since_hdr = 0`,F 不推进),staging 中被跨过的已落盘数据会被
  Flusher 下一轮重复压块(重复块风险)。k 块回溯对块头只读不修,无截断副作用。
- **不变量收尾**:恢复出口前清零 (a) 跨过区间 [F_orig, F)(崩溃可能落在
  §4.2 第 4→5 步之间,该区间未清零)与 (b) 撕裂尾巴 [W', W_crash),
  使"除 [flushed_len, write_pos) 外全环为零"在出口处成立(spec §4.3 收尾要求)。
- **`truncated_blocks` 计数修复**:
  - `S_file < S_hdr` 且文件尾干净(断电常态分支)此前完全不计数;
  - `S_file > S_hdr` 时 `S_hdr - seqno_file` uint32 下溢(打出 ~40 亿)。
  现按 `lost = max(0, S_hdr - S_file)` 计数,truncated 时另计 1 兜底。
- **裸 write_pos 消毒**:`write_records` 把写指针裸写在 Header A[16](CRC 外),
  撕裂可能留下 ≥ size 的垃圾值;恢复侧越界一律钳为 0(宁丢不脏),
  否则会按越界值分配 ~4GB 扫描缓冲。
- **resume_seqno 语义对齐**(遗留项 5 部分收敛):统一为"下一块序号",
  Flusher 首块取 resume + 1;截断后续写 resume = max(S_hdr,S_file) + 1
  (首块 = max + 2,跳 1 空隙防 nonce 重用);全新启动 resume = 0(首块 = 1)。

### zstd 真接入(M3 第一步)

- **vendor 源码**:`third_party/zstd/lib`(v1.5.7,官方 tarball 剔除
  tests/programs/contrib 等,零源码改动)。单线程配置(ZSTD_MULTITHREAD 未定义)。
- **CMake 接线**:`core/CMakeLists.txt` 在 `LOGX_WITH_ZSTD=ON` 时把 zstd 的
  24 个 .c 编进 `logx_core`。两个坑:
  - `project(logx_core C CXX)` 必须显式启用 C 语言,否则 .c 被当 None
    不参与编译,链接期报 `ZSTD_*` 未解析;
  - 解压侧需补 `zstd_ddict.c`(`zstd_decompress.c` 引用,即使不用字典)。
- **解压上限防御**(`codec.cpp`):块内容契约上限 = `kBlockMaxRaw` + 64B;
  `ZSTD_getFrameContentSize` 声明值超限直接拒绝(坏块伪造超大声明值
  → `resize` 内存炸弹)。zstd 默认帧不带校验和,内容完整性靠块级
  CRC32(spec §3.2 保留它的原因)。
- **压缩降级链**(`encode_block`):压缩失败自动回退 flag=none 继续写,
  不因 codec 故障丢日志/阻塞(spec §6 降级矩阵)。
- **`LOGX_WITH_ZSTD=0` 时 flag=zstd 编码失败**(flusher 按块 IO 错误处理,
  不静默落明文,避免"看起来压缩了其实没压"的格式混淆)。

### 测试

- `test_recovery` 新增 4:`BlocksWrittenHeaderNotCommitted_BackfillExact`
  (Σraw_len 精确回推 + 收尾清零,按 Flusher 真实时序"写块→清零→提交"构造)、
  `_RemainderRescanned`(在飞记录被清、坐标收敛)、
  `TruncatedFile_LostCountNoUnderflow`(丢块计数 = S_hdr - S_file)、
  `CorruptWritePos_ClampedToZero`(越界 write_pos 钳制)。
- 新增 `test_codec`(7 个):zstd 高压缩数据往返(体积 < 1/4)、不可压数据
  往返、坏帧头拒绝、全零垃圾拒绝、encode/decode 端到端、unknown algo 失败、
  未接 zstd 时 flag=zstd 编码失败。
- 命令行 `adb push` 在 Git Bash 下需 `MSYS_NO_PATHCONV=1`,否则路径被
  MSYS 转换(`C:/ProgramFiles/Git/data/...`),推到别处后误以为设备在跑新
  二进制(设备上残留旧文件 + md5 校验发现)。

### 验证

- Windows / MSVC 2022 / x64 Release:**32/32 通过**(`-DLOGX_WITH_ZSTD=ON`)
- Android / NDK 29 / arm64-v8a(-DLOGX_WITH_ZSTD=ON):**32/32 通过**
  (Redmi K30 Pro 真机)
- 注:默认 `LOGX_WITH_ZSTD=OFF` 行为不变(flag=zstd 编码失败路径);
  实际启用压缩需 `-DLOGX_WITH_ZSTD=ON` 且实例配置 `compress_algo = kAlgoZstd`。

---

## 二、首次构建修复(`d454dee`)

按以下命令在 Windows/MSVC 首次编译,暴露 100+ 解析错误与 4 处真实 bug:

```bash
cmake .. -DLOGX_BUILD_TESTS=ON
cmake --build . -j
```

### 根因诊断:UTF-8/GBK 解析错乱

源码为 UTF-8 无 BOM,MSVC 默认按系统代码页(中文系统 = 936/GBK)读取,
中文注释被 GBK 误读后会把**下一行的 `constexpr` 声明吞掉**(伴随 C4819 警告),
表现为 `kStagingFileSize 未声明` 等一串无法理解的级联错误。
用最小复现文件(两行中文注释 + constexpr 声明)确认,`/utf-8` 验证有效。

### 修复清单

| 文件 | 修复 | 影响平台 |
|---|---|---|
| `CMakeLists.txt` | MSVC 编译选项加 `/utf-8` | 仅 MSVC |
| `core/include/logx_staging.h` | `FileHeader` 显式 1 字节对齐打包:自然对齐下 sizeof=36B,违反规范 §3.2 的 32B(static_assert 必炸) | **全平台**(NDK 同样会炸) |
| `core/src/staging.cpp` | 补 `#include "logx_format.h"`:`kMagicPad` 定义在该头,原文件缺失导致未声明 | **全平台** |
| `core/src/format.cpp` | `std::min<uint64_t>` 显式模板参数:`uint32_t` 与 `uint64_t` 混用无匹配重载 | **全平台** |

---

## 三、Windows(MSVC)平台移植(`fa24e2f`)

按方案文档 §12"重写 PAL 即可适配平台"完成 Windows 支持,
同时把 `flusher` / `recovery` / `logx_c` 里的 POSIX 直调
(`open`/`write`/`pread`/`fstat`/`ftruncate`/`unlink`/`nanosleep`/`dirent`)
全部收口到 PAL —— 这些直调此前违反"核心零平台依赖"的设计声明。

### PAL 接口扩展(`logx_pal.h`,posix 与 win32 同步实现)

| 方法 | 用途 |
|---|---|
| `file_pread(fd, buf, len, offset)` | 循环短读,EOF 返回已读字节数(scan_one/export 读文件头与块头) |
| `file_size(fd)` / `file_size_path(path)` | 取文件大小(替代 `fstat`/`stat`) |
| `sleep_ms(ms)` | Flusher 同步轮询退避(替代 `nanosleep`) |
| `file_rmdir(path)` | 删除空目录(测试清理用) |

### `pal_win32.cpp`(新增,约 460 行)

- **文件**:`CreateFileA` + `_open_osfhandle` 暴露为 CRT fd;`WriteFile`/`ReadFile`
  + OVERLAPPED 实现 pwrite/pread;`FlushFileBuffers` 实现 fsync/目录 fsync;
  `CreateDirectoryA` 递归建目录;`FindFirstFileA` 列目录(输出统一 `/` 分隔)
- **mmap**:`CreateFileMapping` + `MapViewOfFile`(MAP_SHARED 语义),
  `msync(MS_SYNC)` = `FlushViewOfFile`
- **互斥** = CRITICAL_SECTION;**事件** = 自动重置事件(与 posix 版 signal/wait 语义一致)
- **线程** = `_beginthreadex`;**原子** = `InterlockedExchangeAdd64`/`InterlockedCompareExchange64`
- **时间** = `QueryPerformanceCounter` / `GetSystemTimeAsFileTime` / `GetLocalTime`
- **随机** = `rand_s`(系统 RNG,需在包含任何 CRT 头之前 `#define _CRT_RAND_S`)
- **休眠** = `Sleep`

### 构建系统

- `core/CMakeLists.txt`:`WIN32` 选 `pal_win32.cpp`,否则 `pal_posix.cpp`
- `tests/unit/CMakeLists.txt`:pthread 链接仅限非 Windows;补 `add_test` 注册
  (**此前 ctest 从未能运行**)

### 测试修复(测试首次运行,断言/构造与实现契约对齐)

- `test_format`:偏移断言修正(taglen@21 / tag@23 / msg@30);
  PAD 用例改为"分片夹 PAD 跨 PAD 重组"(原用例期望两条整条拼接,违反单条日志语义);
  `format_record` 输出缓冲按 ≥ `kMaxRecordTotal` 契约
- `test_staging`:`FullRingDrops` 改为填满 `size-1` 再拒写;回绕用例补
  `commit_header_locked` 推进 f + 清零(模拟 Flusher 消费)——否则会计空隙下
  跨回绕必然 `kFull`,测试意图无法成立
- `test_recovery`:撕裂半条构造真正截断(声明 4B 只给 2B);文件 IO 全部改走 PAL;
  临时路径去 `/tmp` 依赖(Windows 无 `/tmp`)

### 顺带修复的真实实现 bug(任何平台都会触发)

| 位置 | 问题 |
|---|---|
| `logx_c.h` | C ABI 句柄 `logx_instance_t`(`struct logx_instance`)与内部引擎类型 `Instance` 是互不转换的两个类型;C++ 分支现在把句柄 typedef 为 `logx::Instance` |
| `staging.cpp` | **PAD 回绕写入越界**:`this_pad` 未扣 6B 记录头,`memcpy`/`memset` 会写穿环尾(内存破坏风险) |
| `staging.cpp` | 全零文件(全新)被误判为"双 Header 损坏重建"(`rebuilt=true`),绕过文件对账 |
| `staging.cpp` | `debug_check_invariant` 跨回绕时把活跃段 `[0, w)` 当脏数据检查 |
| `format.cpp` | `parse_record` 遇 LGXP PAD 直接失败(spec 要求跳过,分片跨 PAD 连续) |
| `recovery.cpp` | 无文件(全新启动)被 `reconcile` 置 `truncated`,导致 resume_seqno 错误 +2 |
| `recovery.cpp` | 截断丢失计数只在 `S_file < S_hdr` 分支触发;`S_file == S_hdr` 时的半块尾巴不计数 |
| `flusher.cpp` | `scan_dir_for_target` 的 `scan_one` 有截断副作用,把候选文件提前"修好",对账丢失 truncated 信号(`scan_one` 增加 `repair` 参数:目录评估只读,确定追加目标才修复) |
| `flusher.cpp` / `logx_c.cpp` | 依赖传递包含才可用的 `<cstdlib>`/`<algorithm>` 等,补显式 include |

---

## 四、Android(NDK)交叉编译(`30efff8`)

用 Android NDK 29.0.14206865 自带 CMake toolchain 命令行交叉编译
(arm64-v8a / android-24 / c++_shared),打通 Android 产物链路。
**NDK 目录(`D:\DevEnv\Android\SDK\ndk`)全程只读引用,未做任何改动;**
全部产物落在 `logx/build-android/`(已在 `.gitignore`)。

### 产物(均验证为 ELF64 / AArch64)

| 产物 | 说明 |
|---|---|
| `build-android/core/liblogx_core.a` | core 静态库,平台分支自动走 `pal_posix.cpp` |
| `build-android/liblogx.so` | 新增 SHARED 目标:JNI 胶水层 + core,导出 5 个 JNI 符号(`nativeInit`/`nativeClose`/`nativeWrite`/`nativeFlush`/`nativeExport`),依赖 `libc++_shared` |
| `build-android/tests/unit/logx_unit_tests` | Android 可执行文件(gtest 经 FetchContent 交叉编译),已在 Redmi K30 Pro 真机通过 21/21 |

### 构建命令

```bash
cmake -S . -B build-android \
  -DCMAKE_TOOLCHAIN_FILE="D:/DevEnv/Android/SDK/ndk/29.0.14206865/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release \
  -DLOGX_BUILD_TESTS=ON -G Ninja
cmake --build build-android -j
```

> 换 ABI(如 `armeabi-v7a` / `x86_64`)只需改 `-DANDROID_ABI`。

### 修复的真实 bug(非 NDK 特有,任何 POSIX 平台都会触发)

| 位置 | 问题 | 修复 |
|---|---|---|
| `core/src/pal_posix.cpp` | `Event` 误用 `std::lock_guard<pthread_mutex_t>`:`pthread_mutex_t` 是 C 类型,无 `lock()`/`unlock()` 成员,libc++ 编译报错 | 改手动 `pthread_mutex_lock/unlock`(`pthread_cond_wait` 自行原子释放/重取锁,语义不变) |
| `core/CMakeLists.txt` | Android 上链接裸 `pthread` 失败:bionic 已并入 libc,无 `libpthread.so` | 改 `find_package(Threads)` + `Threads::Threads`(Android 下解析为 `-pthread` 编译选项而非 `-lpthread`) |
| `tests/unit/CMakeLists.txt` | 同上 pthread 链接问题 | 同上 |
| `tests/unit/CMakeLists.txt` | `find_package(GTest REQUIRED)` 在交叉编译下找不到 Android 版 gtest | 改"系统无 gtest 时 FetchContent 拉取 googletest v1.14.0 交叉编译"的降级通道 |

> 注:`pal_posix.cpp` 此前从未在非 MSVC 平台编译过(Windows 走 `pal_win32.cpp`),
> `lock_guard<pthread_mutex_t>` 在 Linux 桌面用 g++ 同样会报错,属全平台修复。

---

## 五、验证结果

- Windows / MSVC 2022(19.43)/ x64,**Release 构建全绿**
- `ctest -C Release`:**28/28 单测通过**(100%)
- 注意:Windows 下需用 Release 配置构建
  (`cmake --build . -j --config Release`),原因见下
- Android / NDK 29.0.14206865 / arm64-v8a:**交叉编译全绿**,单测在 Redmi K30 Pro
  (arm64-v8a) 真机运行通过

---

## 六、已知问题与后续事项

### 构建相关

1. **Debug 配置无法链接测试**:Anaconda 提供的 GTest 是 Release(/MD) 构建,
   Debug(/MDd) 下报 `RuntimeLibrary` / `_ITERATOR_DEBUG_LEVEL` 不匹配。
   解决:Release 配置构建,或安装原生 MSVC 版 GTest(vcpkg/conda-forge msvc)。
2. **`pal_win32.cpp` 使用 ANSI(A) API**:非当前代码页可表示的路径(如 UTF-8 中文路径)
   不可用。后续应转 W API(MultiByteToWideChar + `CreateFileW` 等)。
3. **Linux 桌面构建尚未验证**:`pal_posix.cpp` 新增的 5 个方法、
   GCC/Clang 桌面编译未实测(Android/NDK 已实测,见"四";
   `pal_posix` 与 `pal_win32` 已按新增接口对齐)。
4. **git autocrlf**:Windows 上提交时提示 LF→CRLF(默认 `core.autocrlf=true`)。
   如需统一 LF,可配 `core.autocrlf=false` 或加 `.gitattributes`。

### 设计/实现遗留(README §"已知未完成项"的延续)

1. **M3**:mbedtls AES-GCM / ARMv8 Crypto Extensions 真接入(开关已就绪,codec 中
   `LOGX_WITH_AESGCM` 路径为占位返回 false;zstd 已接入,见"一")。
2. **M3**:JNI `keyProvider` trampoline(全局引用 + `AttachCurrentThread`)。
3. **M4**:50 线程压测 / monkey 72h 稳定性 / 断电注入(QEMU 快照)。
4. ~~**恢复对账 `bytes_since_hdr` 未实现**~~(2026-09-02 已解决):见"一",
   `S_file > S_hdr` 分支按块头 `raw_len` 逐块累加精确回推 F,含不变量收尾。
5. ~~**JNI 缺失 nativeExport**~~(2026-09-02 已解决):见"〇",已补齐 nativeExport 并优化 JNI 零堆分配。
6. ~~**多进程安全隔离**~~(2026-09-02 已解决):见"〇",Kotlin Facade 默认按进程名后缀分流目录。
7. **`test_flusher` 的 `write_block_to_file` 为占位实现**(统一到 `write_and_record`)。

---

## 附:构建命令速查

### Windows(桌面,MSVC)

```bash
# 配置(自动选 MSVC + pal_win32)
cmake .. -DLOGX_BUILD_TESTS=ON

# 构建(必须 Release,Debug 与 Anaconda GTest 不匹配)
cmake --build . -j --config Release

# 测试
ctest -C Release --output-on-failure
```

### Android(NDK 交叉编译)

```bash
# 配置(自动选 Clang + pal_posix;NDK 目录只读引用)
cmake -S . -B build-android \
  -DCMAKE_TOOLCHAIN_FILE="D:/DevEnv/Android/SDK/ndk/29.0.14206865/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release \
  -DLOGX_BUILD_TESTS=ON -G Ninja

# 构建(.a + liblogx.so + logx_unit_tests)
cmake --build build-android -j

# 在设备/模拟器上运行单测(可选)
# adb push build-android/tests/unit/logx_unit_tests /data/local/tmp/
# adb shell /data/local/tmp/logx_unit_tests
```
