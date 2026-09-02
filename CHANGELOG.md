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

---

## 一、首次构建修复(`d454dee`)

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

## 二、Windows(MSVC)平台移植(`fa24e2f`)

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

## 三、Android(NDK)交叉编译(`30efff8`)

用 Android NDK 29.0.14206865 自带 CMake toolchain 命令行交叉编译
(arm64-v8a / android-24 / c++_shared),打通 Android 产物链路。
**NDK 目录(`D:\DevEnv\Android\SDK\ndk`)全程只读引用,未做任何改动;**
全部产物落在 `logx/build-android/`(已在 `.gitignore`)。

### 产物(均验证为 ELF64 / AArch64)

| 产物 | 说明 |
|---|---|
| `build-android/core/liblogx_core.a` | core 静态库,平台分支自动走 `pal_posix.cpp` |
| `build-android/liblogx.so` | 新增 SHARED 目标:JNI 胶水层 + core,导出 4 个 JNI 符号(`nativeInit`/`nativeClose`/`nativeWrite`/`nativeFlush`),依赖 `libc++_shared` |
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

## 四、验证结果

- Windows / MSVC 2022(19.43)/ x64,**Release 构建全绿**
- `ctest -C Release`:**21/21 单测通过**(100%)
- 注意:Windows 下需用 Release 配置构建
  (`cmake --build . -j --config Release`),原因见下
- Android / NDK 29.0.14206865 / arm64-v8a:**交叉编译全绿**,单测在 Redmi K30 Pro
  (arm64-v8a) 真机运行 **21/21 通过**(5 个套件,5ms 总耗时)

---

## 五、已知问题与后续事项

### 构建相关

1. **Debug 配置无法链接测试**:Anaconda 提供的 GTest 是 Release(/MD) 构建,
   Debug(/MDd) 下报 `RuntimeLibrary` / `_ITERATOR_DEBUG_LEVEL` 不匹配。
   解决:Release 配置构建,或安装原生 MSVC 版 GTest(vcpkg/conda-forge msvc)。
2. **`pal_win32.cpp` 使用 ANSI(A) API**:非当前代码页可表示的路径(如 UTF-8 中文路径)
   不可用。后续应转 W API(MultiByteToWideChar + `CreateFileW` 等)。
3. **Linux 桌面构建尚未验证**:`pal_posix.cpp` 新增的 5 个方法、
   GCC/Clang 桌面编译未实测(Android/NDK 已实测,见"三";
   `pal_posix` 与 `pal_win32` 已按新增接口对齐)。
4. **git autocrlf**:Windows 上提交时提示 LF→CRLF(默认 `core.autocrlf=true`)。
   如需统一 LF,可配 `core.autocrlf=false` 或加 `.gitattributes`。
5. ~~**Android 单测未在设备运行**~~(2026-09-02 已解决):交叉编译出的
   `logx_unit_tests` 已在 Redmi K30 Pro(arm64-v8a) 真机运行,21/21 通过。
   运行方式(已验证):
   `adb push build-android/tests/unit/logx_unit_tests /data/local/tmp/ &&
    adb push $NDK/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/
    aarch64-linux-android/libc++_shared.so /data/local/tmp/ &&
    adb shell "cd /data/local/tmp && LD_LIBRARY_PATH=/data/local/tmp ./logx_unit_tests"`
   (c++_shared 产物运行时需同目录提供 `libc++_shared.so`)。同时有 3 处无害
   unused 警告(`recovery.cpp`/`format.cpp`),不影响产物。

### 设计/实现遗留(README §"已知未完成项"的延续)

1. **M3**:zstd 单文件版 / mbedtls AES-GCM 真接入(开关已就绪,codec 中
   `LOGX_WITH_ZSTD` 路径已接真实调用,AES-GCM 为占位返回 false)。
2. **M3**:JNI `keyProvider` trampoline(全局引用 + `AttachCurrentThread`)。
3. **M4**:50 线程压测 / monkey 72h 稳定性 / 断电注入(QEMU 快照)。
4. **恢复对账 `bytes_since_hdr` 未实现**(`recovery.cpp` 注释 TODO):
   `S_file > S_hdr`(块已写 Header 未提交)时回推 F 用的是"信任文件 seqno"近似,
   未按 raw_len 累加回推。
5. **Header 语义**:`resume_seqno` 写入 Header 作为"水位",实际 drain 时再 +1 作为
   块 seqno(首块从 2 开始而非 1)。当前无正确性问题(seqno 唯一性更宽松),
   但语义上"已落盘块数"与"下一块序号"未完全对齐,后续可统一。
6. **`test_flusher` 的 `write_block_to_file` 为占位实现**(统一到 `write_and_record`)。
7. **多进程隔离**:设计为单进程日志库,多进程按进程名后缀隔离目录
   (Windows 下多进程 mmap 共享未验证)。

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
