# T-Z7 `c++_static` vs `c++_shared` 体积与性能对照（2026-10-18）

**状态：实验完成。性能无实质差异；`c++_static` 使 `liblogx.so` 体积 +90.2%，
G2 单文件门禁 FAIL，但总 native 载荷 −58.4%。未改产品代码，未提交。**

> **后续决策（2026-10-18）**：用户拍板**继续使用 `c++_shared`**。实验用
> `-PlogxStl` 开关已回退，默认 AAR 已重建并复核：`liblogx.so` **385,112 B**
> （SHA-256 `0a178d6e...390b5b2f`），AAR SHA-256 `0a356a92...d8f2a44`，
> 与 T-Z6 B2 产物逐字节一致。`c++_static` 仅保留为本次实验记录，不进入交付配置。

## 目的与配置

用户要求验证：在已收口的 G2 配置上改用静态 C++ 标准库，体积与性能各差多少。

除 STL 外，两种构建完全一致：

- `CMAKE_BUILD_TYPE=MinSizeRel`（`-Oz -DNDEBUG -g`，D4-A 保符号）；
- 6 个 `ZSTD_EXCLUDE_*` + `ZSTD_STRIP_ERROR_STRINGS` + `ZSTD_NO_TRACE`，
  `third_party/zstd` 0-diff；
- `LOGX_HOTPATH_O2=ON`（D6-B2：4 个写入热路径 TU 单独 `-O2`）；
- NDK 29.0.14206865 / arm64-v8a / minSdk 21 / AGP 8.5.1 / Gradle 8.9；
- AAR `assembleRelease`，AGP 打包 strip。

`cmake_proof.txt` 实证：`ANDROID_STL=c++_static`、`MinSizeRel`、
4 个热路径 TU FLAGS 末尾 `-O2`、`zstd_compress.c` 仅 `-Oz -DNDEBUG -g`、
链接参数 `-static-libstdc++`、无 `libc++_shared.so` 依赖。

## 体积

| 项 | `c++_shared`（B2） | `c++_static` | 差异 |
|---|---:|---:|---:|
| AAR 内 strip 后 `liblogx.so` | 385,112 B | **732,344 B** | **+347,232 B（+90.2%）** |
| AAR 内 `libc++_shared.so` | 1,374,336 B | 0（无此文件） | −1,374,336 B |
| 总 native 载荷 | 1,759,448 B | **732,344 B** | **−1,027,104 B（−58.4%）** |
| AAR zip | 676,936 B | **328,090 B** | −348,846 B（−51.5%） |
| G2 门禁（`liblogx.so` < 512,000 B） | PASS，余量 126,888 B | **FAIL，超出 220,344 B** | — |
| 动态定义符号数 | 429 | 1,110 | +681 |
| 重型 matcher 符号 | 0 | 0 | 不变 |
| ELF text（llvm-size） | 377,378 B | 706,385 B | +329,007 B |

`c++_static` 的 `.so` NEEDED 仅 `liblog.so`/`libm.so`/`libdl.so`/`libc.so`；
打包 SO 无 `.debug_*`、无 `.symtab`。

**关键权衡**：单文件门禁看 `liblogx.so`，静态 STL 必然 FAIL；若看随 AAR 一起
分发的总 native 载荷，静态 STL 小 1.0 MB（−58.4%）。后者是否更有意义取决于
宿主 App 是否已有其他 native 库共用 `libc++_shared.so`：若已有，共享 STL 的
1.37 MB 是摊薄成本，静态 STL 反而让每个库各带一份 STL。

## 性能

真机 Redmi K30 Pro（骁龙 865 / Android 12），固定性能模式 +
`deviceidle disable all` + 保持亮屏；同一 `logx_bench` 二进制
（`20000 50000 10000`）；run1-3 交错取中位，run4 逆序确认。

| 指标（3 轮中位） | `c++_shared` | `c++_static` | 差异 |
|---|---:|---:|---:|
| 纯路径 none P50 | 3,646 ns | 3,698 ns | +52 ns（+1.4%） |
| 纯路径 none P99 | 12,864 ns | 13,125 ns | +261 ns（+2.0%） |
| 生产口径 zstd P50 | 3,906 ns | 3,854 ns | −52 ns（−1.3%） |
| 生产口径 zstd P99 | 47,917 ns | 52,552 ns | +4,635 ns（+9.7%） |
| Stage B none 吞吐 | 109,049 logs/s | 98,237 logs/s | −9.9%（噪声） |
| Stage B zstd 吞吐 | 73,279 logs/s | 80,357 logs/s | +9.7%（噪声） |
| flush P99 | 8.83 ms | 8.91 ms | +0.9% |
| 压缩比 | 1.52x | 1.52x | 不变 |

逆序轮：纯路径 P50 static 3,646 / shared 3,698 ns；生产 zstd P50 两者均
3,854 ns。P50 差异仅一个 52 ns 量化档且方向翻转，**没有可辨识的性能差异**。
P99 与吞吐的 10% 量级波动方向相反，属轮间噪声；所有轮 `真实丢失=0`。

## 结论与建议

1. **性能不是选择依据**：静态/共享 STL 在写入热路径与压缩比上等价。
2. **若坚持 G2 单文件门禁**：保持 `c++_shared`（385,112 B PASS）。
3. **若目标是减小总 native 载荷**：`c++_static` 可把 1.76 MB 降到 0.73 MB，
   但需同时：
   - 重新定义门禁（现有 512,000 B 按 `liblogx.so` 计，静态必 FAIL）；
   - 处理静态 STL 导出的 1,110 个动态符号（可加
     `-Wl,--exclude-libs,libc++_static.a` 或可见性脚本），否则与宿主 App
     的 `libc++_shared.so` 存在符号介入/冲突面；
   - 若宿主 App 已有其他 native 库使用 `c++_shared`，静态 STL 会重复携带，
     总包体收益需按 App 实际依赖重新评估。

## 口径与局限

- 静态库通过 AAR 真实产物验证；benchmark harness 本身仍链接
  `libc++_shared.so`（C ABI 隔离，无 C++ 对象跨界），因此进程内同时存在
  静态 STL 与共享 STL。真机纯静态 App 的绝对值可能有细微差异，但不足以
  改变“无实质性能差异”的结论。
- 未重跑 197 项设备单测；benchmark 覆盖真实写/flush/压缩路径，
  8 轮共 60 万级写入 0 丢失、压缩比一致。若决定切换 STL，应按 T-Z4 流程
  重跑四套件回归。
- 构建脚本临时新增 `-PlogxStl` 开关（默认 `c++_shared`），当前未提交。

## 证据文件

- `build_aar_static.log`、`cmake_proof.txt`
- `aar_listing.txt`、`readelf_sections_static.txt`、`readelf_dynamic.txt`、
  `nm_static_stripped.txt`、`symbols.txt`
- `size.txt`、`medians.txt`、`parse_metrics.awk`
- `bench_shared_run1-3.log`、`bench_static_run1-3.log`、
  `bench_{shared,static}_run4_rev.log`、smoke 日志
