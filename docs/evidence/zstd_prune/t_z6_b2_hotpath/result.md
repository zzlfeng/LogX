# T-Z6 D6-B2 热路径 `-O2` 验证结果（2026-10-17）

**状态：PASS（D6 以 B2 关闭；P50 门禁达标，体积门禁仍 PASS）**

## 决策与实现边界

D6 拍板选 **B2**：仅在 MinSizeRel 下把写入热路径的 4 个 C++ TU
（`logx_c.cpp` / `flusher.cpp` / `format.cpp` / `staging.cpp`）追加 `-O2`，
zstd/mbedtls 源文件继续 `-Oz`。实现为两个默认开启的开关：

- `core/CMakeLists.txt`：`LOGX_HOTPATH_O2`（默认 OFF，仅当
  `CMAKE_BUILD_TYPE=MinSizeRel` 且编译器为 Clang/GNU 时生效，
  `set_source_files_properties(... COMPILE_OPTIONS "-O2")`）；
- `android/logx-aar/logx/build.gradle.kts`：默认传
  `-DLOGX_HOTPATH_O2=ON`（可 `-PlogxHotpathO2=OFF` 做 A/B）。

不对 `logx_core` 整体加 `-O2`，否则会抵消 zstd 裁剪的体积收益；
`third_party/zstd` 仍为 **0-diff**（`git status --short third_party/zstd` 为空）。

## 构建配置证明

工具链：AGP 8.5.1 / Gradle 8.9 / CMake 3.22.1 / NDK 29.0.14206865 / minSdk 21 /
arm64-v8a（host 回归：MSVC 14.43；NDK 回归：26.1.10909125）。

`CMakeCache.txt`：

```text
CMAKE_BUILD_TYPE:STRING=MinSizeRel
CMAKE_CXX_FLAGS_MINSIZEREL:STRING=-Oz -DNDEBUG -g
LOGX_AAR_KEEP_SYMBOLS:BOOL=ON
LOGX_HOTPATH_O2:BOOL=ON
ANDROID_STL:UNINITIALIZED=c++_shared
```

`hotpath_flags_proof.txt`（AAR `build.ninja`，clang 以最后一个 `-O` 为准）：
4 个热路径 TU 的 FLAGS 末尾均为 `... -std=c++17 -O2`；作为对照，
`zstd_compress.c` 仅有 `-Oz -DNDEBUG -g`，无 `-O2`。

## 体积与符号

| 项 | 实测 | 判定 |
|---|---:|---|
| AAR 内 `liblogx.so`（strip 后） | **385,112 B** | ✅ < 512,000 B，余量 **126,888 B** |
| 相对 final-A (`-Oz`) | +13,568 B（371,544 → 385,112） | 单列披露 |
| AAR 内 `libc++_shared.so` | 1,374,336 B | 不参与门禁 |
| 总 native 载荷 | **1,759,448 B** | 单列披露 |
| AAR zip | 676,936 B | 仅信息 |
| 未 strip MinSizeRel+`-g` 归档 | 3,048,752 B | D4-A 崩溃栈可读性 |
| AAR SHA-256 | `0a356a925895a49779d406c87fea36e8d236428591e35937caabb44b8d8f2a44` | — |
| `liblogx.so` SHA-256 | `0a178d6ea6c8934917d9b32bec6e5c6e829646f7bdab2b2d8fe86f5e390b5b2f` | — |

- `readelf_sections_b2.txt`：打包 SO 无 `.debug_*`、无 `.symtab`（AGP 已 strip）；
- `nm_b2_stripped.txt`（`llvm-nm -D --defined-only`）：429 个动态符号；
- `nm_b2_unstripped.txt`：重型 matcher
  `ZSTD_compressBlock_(greedy|lazy|lazy2|btlazy2|btopt|btultra*)` 计数为 **0**，
  与 B0 的 64 个形成正向对照；
- `size_b2.txt`：text 377,378 B、data 4,584 B、bss 4,912 B
  （final-A 为 text 363,634 / data 4,744 / bss 6,192）。

## 真机性能（Redmi K30 Pro，Android 12）

方法：固定性能模式 + `deviceidle disable all` + 保持亮屏；同一 `logx_bench`
二进制（`20000 50000 10000`）；baseline / final-A / B2 三轮交错取中位，
另补一轮逆序确认（B2 → final-A → baseline）。原始日志 `bench_*_run*.log`，
汇总见 `medians.txt`。

| 指标（3 轮中位） | baseline | final-A (`-Oz`) | **B2（热路径 `-O2`）** |
|---|---:|---:|---:|
| 纯路径 none P50 | 3,698 ns | 4,896 ns | **3,698 ns** |
| 纯路径 none P99 | 13,125 ns | 15,156 ns | **12,917 ns** |
| 纯路径 zstd P50 | 3,698 ns | 4,895 ns | **3,698 ns** |
| 纯路径 zstd P99 | 12,864 ns | 13,698 ns | **12,448 ns** |
| 生产口径 zstd P50 | 3,855 ns | 5,052 ns | **3,958 ns** |
| 生产口径 zstd P99 | 52,656 ns | 54,584 ns | **50,782 ns** |
| 压缩比 | 1.52x | 1.52x | **1.52x** |

逆序确认轮：B2 纯路径 P50 **3,646 ns**、生产口径 zstd P50 **3,802 ns**；
final-A 分别为 4,792 / 5,104 ns；baseline 为 3,698 / 3,854 ns。
B2 与 baseline 持平，相对 final-A 纯路径 P50 下降 **24.5%**（4,896→3,698）。

### 门禁判定

| 门槛 | 实测 | 判定 |
|---|---|---|
| P50 ≤ 4.0µs（成功） | 纯路径 3,698 ns；生产口径 3,958 ns | ✅ 达标 |
| P50 ≤ 4.4µs（部分） | 同上 | 已优于成功线 |
| 体积 < 512,000 B | 385,112 B | ✅ |
| 重型 matcher 符号 = 0 | 0 | ✅ |

## 回归与消费验证

- Host MSVC Release clean 构建 **57/57**，0 编译警告；ctest **4/4** 套件通过
  （`build_host_clean.log`、`ctest_host.log`）；
- NDK 26.1.10909125 clean 构建 **61/61**，0 警告 0 错误
  （`build_android_ndk26_clean.log`）；
- 真机四套件：unit **197/197**、hotpath **2/2**、stress **4/4**、
  crash_fork **3/3**，退出码全 0（`device_*.log`）；
- AAR 消费端 `:app:assembleDebug` 成功（`build_consumer_b2.log`），
  APK 内 `lib/arm64-v8a/liblogx.so` 为 385,112 B；
- 全部基准轮 `真实丢失: 0 条`，无 drop/block_write_errors 失真标记。

## 噪声与口径说明

- Stage B 吞吐轮间波动极大（baseline 40,382–109,364、final-A 37,320–105,915、
  B2 40,835–99,675 logs/s），本轮**不用于性能判定**；
- flush P99 随执行顺序/热状态漂移（逆序轮 B2 8.45ms、final-A 14.06ms、
  baseline 8.58ms），亦不用于门禁；P50 在 3 轮与逆序轮均稳定，是有效判据；
- B2 的 `liblogx.so` 因 4 个 TU 提升 `-O2` 增加 13,568 B，仍远低于 512,000 B
  门禁，体积余量 126,888 B。
