# T-Z4a 真机回归结果（2026-10-17）

**状态：PASS**（首次失败为设备休眠环境伪影，唤醒后全绿）

## 构建

- NDK **26.1.10909125**，arm64-v8a，clean-first：`build_android_ndk26_clean.log`
  完成 **61/61** 目标，**0 警告 0 错误**（不含 toolchain 自身的
  CMake Deprecation Warning）。
- Host MSVC 侧对应结果为 `build_host_clean.log`（0 警告 0 错误）与
  `ctest_host.log`（4/4 套件通过）。

## 设备结果（Redmi K30 Pro，Android 12）

| 套件 | 结果 | 日志 |
|---|---|---|
| `logx_unit_tests` | **197/197** PASS | `device_unit_tests.log` |
| `logx_hotpath_alloc_tests` | **2/2** PASS | `device_hotpath_tests.log` |
| `logx_stress_tests` | **4/4** PASS | `device_stress_tests.log` |
| `logx_crash_fork_tests` | **3/3** PASS | `device_crash_fork_tests.log` |

设备单测套件包含 `ZstdCascadeSmoke.AllLevelsOnLogxBlockSize` 与
`BoundarySizesAtKeyLevels`，均 PASS。

## 设备休眠伪影（已排除）

首轮 `Flush.TimeoutWhenFlusherStalled_EmitsEventAndReturns` 实测仅 1.1–1.7s
（预期 ≥4,800ms）。根因：`flush_blocking` 使用 `CLOCK_REALTIME`/墙钟计时，
而 gtest 使用 `steady_clock`；设备休眠期间单调钟暂停、墙钟继续，造成超时被
“提前”触发。唤醒设备（`input keyevent KEYCODE_WAKEUP`）并保持亮屏后，
四套件全绿，产品与裁剪均无回归。
