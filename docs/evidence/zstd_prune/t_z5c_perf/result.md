# T-Z5c G2 性能与压缩比复测结果（2026-10-17）

**状态：功能/压缩比 PASS；纯路径 P50 回退 +32.9%，待拍板**

## 方法

- 设备：Redmi K30 Pro（骁龙 865，Android 12），已唤醒并保持亮屏；
- 稳定化：`cmd power set-fixed-performance-mode-enabled true`、
  `cmd deviceidle disable all`；
- 工具：`tests/bench/logx_bench.cpp`（Stage A 写入延迟 / B 吞吐与压缩比 / C flush / D NO_SPACE）；
- 对比：baseline（改造前 AAR，RelWithDebInfo + `c++_static`，980,864 B）vs
  final（G2 AAR，MinSizeRel `-Oz` + `c++_shared` + 裁剪，371,544 B）；
- 3 轮交错执行取中位；首轮曾因设备休眠（`steady_clock` 暂停、墙钟继续）失真，已剔除并唤醒后重跑；
- 归因：同机同法补测 B0（MinSizeRel + static + 未裁剪，868,096 B）与
  B1static（MinSizeRel + static + 裁剪，719,192 B）。

## 主对比（3 轮交错，中位）

| 指标 | baseline | final | 变化 |
|---|---:|---:|---:|
| 纯路径 none P50 | 3,646 ns | 4,844 ns | **+32.9%** |
| 纯路径 none P99 | 13,073 ns | 14,531 ns | +11.2% |
| 纯路径 zstd P50 | 3,646 ns | 4,844 ns | +32.9% |
| 纯路径 zstd P99 | 12,604 ns | 14,010 ns | +11.2% |
| 生产口径 zstd P50 | 3,802 ns | 5,052 ns | +32.9% |
| 生产口径 zstd P99 | 51,614 ns | 50,104 ns | −2.9% |
| Stage B none 吞吐 | 103,383 logs/s | 111,605 logs/s | +7.9%（波动大） |
| Stage B zstd 吞吐 | 67,823 logs/s | 72,309 logs/s | +6.6%（波动大） |
| 压缩比（混合负载） | 1.52x | 1.52x | 不变 |
| flush P99 | 10.04 ms | 10.48 ms | +4.4% |

详细中位与逐轮日志见 `medians.txt`、`bench_i_*.log`、`bench_*.log`。

## 归因（3 轮中位，同一设备/方法）

| 构建 | pure none P50 | pure zstd P50 | Stage B zstd |
|---|---:|---:|---:|
| baseline（RelWithDebInfo + static） | 3,646 ns | 3,646 ns | 76,740 logs/s |
| B0（MinSizeRel + static，未裁剪） | 4,896 ns | 4,896 ns | 69,000 logs/s |
| B1static（MinSizeRel + static，裁剪） | 4,896 ns | 4,895 ns | 69,079 logs/s |
| final（MinSizeRel + shared，裁剪） | 4,844 ns | 4,844 ns | 66,192 logs/s |

**结论**：P50 从 3.6µs 升至 4.8µs，完全来自 MinSizeRel `-Oz` 构建 profile；
zstd 裁剪与 `c++_shared` 没有额外影响（B0/B1static/final 中位相同）。吞吐与
flush P99 在轮间噪声范围内，压缩比保持 1.52x。

## 待拍板

按 `docs/benchmark_report.md` §7 的回归纪律，P50 变化 +32.9% 已越过 30% 阈值，
但绝对值为 4.8µs、P99 未劣化、吞吐未下降。计划 §2.5 选项 D 提供了
“热路径源文件单独 `-O2`”的缓解路径。当前**未改动任何产品代码**，等待选择：

1. **接受**：以体积优先（371,544 B，门禁余量 140,456 B），登记 P50 回退；
2. **缓解**：对热路径（`format.cpp`/`logx_c.cpp` 中写入确定段）单独 `-O2`，
   重新测量体积与 P50——属于构建配置变更，需单独批准。

原始日志：`bench_baseline_run1-3.log`、`bench_final_run1-3.log`、
`bench_i_{baseline,final}_1-3.log`、`bench_attr_{baseline,b0,b1static,final}_1-3.log`；
构建 profile 与库文件在 `baseline/`、`b0/`、`b1static/`、`final/`、`libs/`。
