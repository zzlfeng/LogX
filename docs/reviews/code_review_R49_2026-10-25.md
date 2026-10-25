# R49 审查报告 — T5.8 WSL Ubuntu ASan+TSan 批次（`1cb5c44`）

> **审查对象**: `1cb5c44` test(wsl,tsan,asan): T5.8 WSL Ubuntu 双插桩全量闭环与数据竞争根除（11 文件 / +307 / −388）
> **基线**: `2dbb3fa` docs(test): 升级 T5.8 为 Android 真机与 Windows 本地双端 ASan 全量验证报告
> **审查方法**: 逐文件通读 + 全仓调用点核对 + 产物时间戳审计 + MSVC ASan 重建/链接/定向用例实跑 + 三套工具链 Sanitizer 符号实证
> **审查日期**: 2026-10-25
> **审查结论**: 核心数据竞争修复**成立且充分**；**无 P0/P1**，可在修正 P2 文档口径后入库。新发现 **3×P2（证据/口径）+ 7×P3**。本报告不改动源码。
> **后续状态（2026-10-25 代码收口批）**：P2-1 / P2-2 / P2-3 与 P3-1~P3-7 **全部闭环**；代码批同时修复 WSL Release 构建暴露的 2 项补遗缺陷（N-1 `<condition_variable>` 缺失、N-2 aes.c 未引用函数告警），并在 Windows Release/ASan、WSL Release/ASan/TSan、Android ASan 全量复跑。详见 §七、§八。

---

## 一、总体结论

1. **`ensure_append_target` 竞争修复正确**：`read_target_cache` 纳入 `inst->mu` 后，与写侧 `write_records` 的 `put_u32(base_ + 16, w)` 形成同一互斥域，TSan 报的 `write_pos` 竞争根除；加锁落在冷路径（首次开文件/跨天/轮换），无热路径损耗。
2. **无死锁/锁序风险**：`ensure_append_target` 全仓唯一调用点为 `drain_once:938`，此时 `:870` 已释放 `inst->mu`；`mutex_lock` 内仅 `read_target_cache`，不触发 `emit_event`，保持 `inst->mu → err_mu` 锁序，且不嵌套（Win32 SRWLOCK 非递归）。
3. **`__lsan_ignore_object` 跨端链接安全（已实证）**：NDK 26.1 / 29 与 MSVC ASan 运行时均导出该符号；本次审查实际重建 `build-host-asan`（34 步 0 警告）、链接测试可执行，并跑通 `ControlBlockLifetime.OpenCloseMemoryBounded`（43.5s，PASS）。
4. **aes.c 守卫正确且不影响 ARM-CE**：非 arm64 剔除对 `aesce_setkey_enc` / `aesce_crypt_ecb` 的引用（修复 x86 编译错误）；arm64 上 `MBEDTLS_AESCE_C && __aarch64__` 仍成立，硬件加速路径不变。
5. **主要问题集中在"证据新鲜度与文档口径"**：`847` 聚合中的 Android/Windows ASan 数据产自本提交之前的构建产物；WSL 矩阵的 Release 行无对应构建；plan §7.3 未同步 TSan/WSL 证据。

### 变更清单

| 文件 | 类别 | 变更 |
|---|---|---|
| `core/src/flusher.cpp` | 核心修复 | `ensure_append_target` 中 `read_target_cache(&tc)` 纳入 `inst->mu` |
| `core/include/logx_staging.h` | 契约注释 | `read_target_cache` 标注"并发写入场景调用方应持有 `inst->mu`" |
| `core/src/logx_c.cpp` | Sanitizer 适配 | `__has_feature` 回退宏；ASan 下 `__lsan_ignore_object(cb)` 登记控制块墓碑 |
| `core/CMakeLists.txt` | 构建 | 新增 `ZSTD_DISABLE_ASM=1`（x86_64 未编入 `huf_decompress_amd64.S`） |
| `third_party/mbedtls/src/aes.c` | 移植性 | `CE_MODE_ENABLED()` 分支（setkey/ecb）以 `LOGX_AESCE_HOOK` 守卫；`mbedtls_aesce_has_support` 改 `static inline` |
| `tests/unit/test_hotpath_alloc.cpp` | TSan 适配 | TSan 下禁用自定义 `new/delete` 探针并 `GTEST_SKIP` 两用例；`__has_feature` 回退 |
| `tests/unit/test_r19_regression.cpp` | TSan 适配 | `w_now()`/`f_now()` 持锁读 Header；内存阈值增加 ASan/TSan 分支（桌面 150MB） |
| `tests/unit/test_robustness.cpp` | TSan 适配 | `MockClockPal` 时间字段升级 `std::atomic<uint32_t>` |
| `CHANGELOG.md` / `docs/LogX后续实施计划表.md` / `docs/test_report_T5.8_android_real_device_and_asan.md` | 台账 | 第 25 条记录、计划表 v1.73、报告 v2.0 |

---

## 二、核心修复独立核实

| 核实点 | 方法 | 结论 |
|---|---|---|
| 竞争域是否闭合 | 核对 `write_records` 写 Header 的调用上下文 | ✅ `staging.cpp:375` 的 `put_u32(base_+16,w)` 在 `logx_write` 持锁区间（`logx_c.cpp:678-688`）内；`commit_header_locked` 全部由 Flusher 持锁调用 |
| 是否引入重入/死锁 | 全仓检索调用点 + 锁序核对 | ✅ 唯一调用点 `flusher.cpp:938`，在 `:870` 解锁之后；锁内无回调；无 `ensure_append_target` 递归 |
| 是否影响热路径 | 分支分析 | ✅ 仅在 `target.path` 为空（首开/跨天/切分）时加锁；稳态 `:727-730` 提前返回，零锁开销 |
| 其他 Header 读者 | 检索 `pick_header` | ✅ 仅 `Staging::open`（启动期、线程未起）与 `read_target_cache` 两处 |
| 修复是否覆盖报告所述根因 | 对照 TSan 栈（`staging.cpp:125←132←164←flusher.cpp:743`） | ✅ 与代码一致，偏移 16 即 `write_pos` |

---

## 三、独立验证（本次审查新增实证）

### 3.1 MSVC ASan 重建/链接/定向用例（本审查执行）

| 步骤 | 命令/对象 | 结果 |
|---|---|---|
| 重建核心库 | `ninja -C build-host-asan logx_core` | **34/34 步，warning = 0**（CMake 因 `ZSTD_DISABLE_ASM` 自动重配置） |
| 链接测试可执行 | `ninja ... logx_unit_tests logx_hotpath_alloc_tests` | **13/13 步通过**，`__lsan_ignore_object` 导入无未解析符号 |
| LSan 分支是否生效 | `dumpbin /symbols logx_c.cpp.obj` | `UNDEF __lsan_ignore_object` ⇒ MSVC `/fsanitize=address` 确定义 `__SANITIZE_ADDRESS__`，ignore 分支被编译 |
| 运行时导出核对 | `dumpbin /exports clang_rt.asan_dynamic-x86_64.dll` | 导出 `__lsan_ignore_object`（序号 322） |
| 内存有界用例 | `--gtest_filter=ControlBlockLifetime.OpenCloseMemoryBounded` | **PASS（43,459 ms）**，走 MSVC 4GB 分支 |

### 3.2 Android ASan 符号可用性

| NDK | `sanitizer/lsan_interface.h` | `libclang_rt.asan-aarch64-android.so` |
|---|:--:|---|
| 26.1.10909125（报告基线） | 存在 | `T __lsan_ignore_object`（0x63da8） |
| 29.0.14206865（AAR `ndkVersion`） | 存在 | `T __lsan_ignore_object`（0x74e78） |

→ Android ASan（含 AAR 构建）不会出现未解析符号；两端 LSan 实际为桩实现，调用无副作用。

### 3.3 产物时间戳审计（相对提交 13:12:31）

| 产物 | 时间 | 判定 |
|---|---|---|
| `build-wsl-asan/.ninja_log` | 13:04:28 | ✅ 改动后构建（支撑 212/212） |
| `build-wsl-tsan/.ninja_log` | 13:01:14 | ✅ 改动后构建（支撑 210/210） |
| `build-full` 5 个相关 `.obj` | 13:06:39–13:06:48 | ✅ 改动后构建（支撑 MSVC Release 0 警告） |
| `build-host-asan/logx_c.cpp.obj` | 12:12:27 | ⚠️ **改动前**（本次审查已重建至最新代码） |
| `build-android-asan/liblogx.so` / `logx_c.cpp.o` | 11:42:20 / 11:35:06 | ❌ **改动前**（Android ASan 尚未随本提交复跑） |

---

## 四、新发现问题

### P2（文档/证据口径，建议本次修掉）

**P2-1｜WSL "标准 Release 功能套件 212/212" 无证据**
- 位置：`docs/test_report_T5.8_android_real_device_and_asan.md:28`（门禁矩阵 WSL 列）。
- 事实：仓库内 WSL 仅 `build-wsl-asan`、`build-wsl-tsan` 两个构建目录，无 Release 构建产物/运行记录；报告 §六 亦只记录 ASan 与 TSan。
- 影响：门禁矩阵存在无法复现的数据行，削弱"三端 847"的可信度。
- 建议：补跑 WSL Release 四套件并落盘记录，或将单元格改为"未执行（以 ASan 构建替代）"。

**P2-2｜"三端 847 项"混入本提交之前的产物**
- 位置：报告 `:12`、`:38`、`:224`；计划表 v1.73 同名表述。
- 事实：Android ASan 213 项来自 11:42 产物、Windows ASan 212 项来自 12:12 产物，均早于本提交（13:12）的 `logx_c.cpp` / `aes.c` / CMake 改动；仅 WSL 双套件（13:01/13:04）与 Windows Release（13:06）属本次代码。
- 缓解：本次审查已复建 MSVC ASan 并跑通关键用例，Windows 侧证据已刷新；**Android ASan 仍是旧产物**。
- 建议：重跑 Android ASan 四套件后再保留 847；否则改为"WSL 422 + Windows ASan 212（已复验）+ Android ASan 213（前次产物，待复跑）"。

**P2-3｜plan §7.3 未同步，且报告引用 §7.3 的表述不成立**
- 位置：`docs/lifecycle_control_block_plan.md:5`（状态行）与 `:481`（ASan 证据仍为"双端 425 项"）；报告 `:38` 称"§7.3 所列之 TSan 线程竞态检测全部门禁"。
- 事实：本次提交未改 plan/decisions（仍 v1.7），§7.3 门禁表**没有 TSan 行**，也无 WSL 证据。
- 建议：plan 升 v1.8（新增 WSL ASan+LSan 与 TSan 门禁行、更新计数），decisions 同步；报告改为"新增 TSan 门禁"或按 §7.3 实际条目表述。

### P3

**P3-1｜`LOGX_HAS_TSAN` 是死条件**
- 位置：`tests/unit/test_hotpath_alloc.cpp:52`（`#if !defined(LOGX_HAS_TSAN)`）。
- 事实：全仓无任何 `LOGX_HAS_TSAN` 定义点；真正生效的是同文件 `:108` 的 `LOGX_SKIP_CUSTOM_NEW_DELETE`。
- 风险：若后续仅在 CMake 定义 `LOGX_HAS_TSAN`，`probe_account`/`probe_malloc` 会消失而 `operator new` 仍在 → 编译失败。
- 建议：删除该包装，或与 `LOGX_SKIP_CUSTOM_NEW_DELETE` 合并为同一宏。

**P3-2｜`__lsan_ignore_object` 注册早于失败路径的 `delete cb`**
- 位置：`core/src/logx_c.cpp:446-449`；失败路径 `:453`、`:500` 会 `delete cb`。
- 事实：LSan 对该接口的约定是"仅用于永不释放的对象"；对象释放后注册地址仍留在 ignore 集合，若地址被复用可能掩盖其他对象的真实泄漏。
- 建议：把 `__lsan_ignore_object(cb)` 移到成功返回前（`*out_inst = reinterpret_cast<...>(cb);` 之前，`:537` 附近）。

**P3-3｜CMake 注释与新增定义自相矛盾**
- 位置：`core/CMakeLists.txt:120` 新增 `ZSTD_DISABLE_ASM=1`；`:128-129` 仍写"ZSTD_DISABLE_ASM 与上游默认一致，无需额外定义"。
- 建议：改写为"本工程仅编入 zstd `.c` 源、未编入 `huf_decompress_amd64.S`，故 x86_64 显式禁用汇编以避免未定义符号"。

**P3-4｜CHANGELOG 提交表使用 `pending` 占位哈希**
- 位置：`CHANGELOG.md:160`。
- 事实：与 R8-2 已确立的"docs 提交带真实哈希"规程冲突；本提交同时含代码与文档，无法自指。
- 建议：下一批 docs 提交补登 `1cb5c44` 真实哈希（与历史勘误行同法）。

**P3-5｜报告两处措辞与宏名不精确**
- 位置：报告 `:202`/`:258` 将跳过原因写作"与 `libclang_rt.tsan_cxx` 发生符号冲突"；`core/src/logx_c.cpp:20` 的 `LOGX_HAS_LSAN`。
- 事实：跳过根因是 TSan 需接管全局 `new/delete` 以跟踪分配，自定义替换会破坏其拦截；`LOGX_HAS_LSAN` 在 Android/MSVC 上仅表示"ASan + 头文件存在"，两端 LSan 为桩。
- 建议：措辞改为"TSan 分配拦截器接管全局 new/delete"；宏名/注释注明"头文件可用即调用，Android/MSVC 为桩实现"。

**P3-6｜桌面 150MB 内存阈值是单机经验值**
- 位置：`tests/unit/test_r19_regression.cpp:3118-3120`。
- 风险：跨机器/`ASAN_OPTIONS`（`quarantine_size_mb`）差异可能造成偶发误报或拦截力下降。
- 建议：用例内固定 `ASAN_OPTIONS=quarantine_size_mb=...`（或按实测增量乘以稳定系数推导），并在注释中记录推导依据。

**P3-7｜`ZSTD_DISABLE_ASM` 的全局作用域（信息项）**
- 事实：该定义同时关闭 Linux x86_64 Release 的 huff0 汇编内核，解压略有性能损耗；Windows（`portability_macros.h:143`）与 Android arm64 本已禁用，无变化。
- 建议：若仅为补齐链接，可选择把 `huf_decompress_amd64.S` 加入源列表；若接受损耗，请在 CMake 注释中显式说明取舍。

---

## 五、验证局限（必读）

1. **未执行 WSL ASan/TSan 套件**（审查当时）：依据为 `.ninja_log` 时间戳 + 报告记录；已在 §八 收口批按当前代码重跑 212/210，0 竞态结论得到复核。
2. **Android ASan 未复跑**（审查当时）：仅做符号可用性实证；已在 §八 收口批重建并推送真机，四套件 213/213。
3. **性能门禁未复测**：WSL P50 0.469µs、真机 3.750µs 均沿用报告数据，本审查未运行 `logx_bench`。
4. **MSVC ASan 仅定向用例**（审查当时）：实跑 `OpenCloseMemoryBounded`；已在 §八 收口批全量复跑 212/212。
5. **审查动作对工作区的影响**：审查当时仅重建 `build-host-asan`（含一次 CMake 重配置），未修改源码；代码批改动见 §八。
6. **审查对象为已提交批次**：`1cb5c44` 已在本地提交但未推送；本报告与 README 索引已随 `f2e5ae2` 入库，代码收口批待后续提交。

---

## 六、建议与后续

**入库判定**：✅ 可入库（无 P0/P1）；建议按以下顺序处理后再推送。

1. **P2-1/P2-2**（证据）：优先复跑 Android ASan 四套件；同步修正 WSL Release 行与 847 口径。
2. **P2-3**（台账）：plan v1.8 + decisions 同步 TSan/WSL 门禁；修正报告"§7.3 所列"表述。
3. **P3-1/P3-2/P3-3**（代码小改，各 1–3 行）：死宏清理、ignore 位置后移、CMake 注释改写。
4. **P3-4**（台账）：`CHANGELOG` 补登 `1cb5c44` 真实哈希。
5. **P3-5/P3-6/P3-7**（文档/测试稳健性）：措辞与宏名注释、阈值环境固定、zstd 汇编取舍说明。

**提交建议**：`fix(core,test,docs)` 单批——`logx_c.cpp`（P3-2）、`test_hotpath_alloc.cpp`（P3-1）、`CMakeLists.txt`（P3-3）、上述三份台账（P2-1/P2-2/P2-3、P3-4/P3-5/P3-7）与本报告 + `reviews/README.md` 索引。

---

## 七、收口记录（2026-10-25 文档批）

> 本节为**文档收口**记录（未改动源码）；代码类 P3 与 WSL Release 补遗已由 §八 代码批闭环。

| 项 | 处置 | 落地位置 |
|---|:--:|---|
| **P2-1** | ✅ 文档批标注"未执行"；**代码批已新增 `build-wsl-release` 并补跑 212/212**（§八） | 报告 §一 矩阵、§十一 |
| **P2-2** | ✅ 文档批改分来源表述；**代码批已将 Android/Windows/WSL 全部按当前代码复跑**（动态插桩 847 项全绿，§八） | 报告 §十一、计划表 v1.75、CHANGELOG 第 26 条 |
| **P2-3** | ✅ plan 升 v1.8：§7.3 新增 WSL ASan+LSan 212/212 与 TSan 210/210 两行，并如实标注 Android/Windows ASan 产物时效；decisions 升 v1.8（决策结论不变） | `lifecycle_control_block_plan.md`、`lifecycle_control_block_decisions.md` |
| **P3-4** | ✅ CHANGELOG 提交表 `pending` → `1cb5c44` | `CHANGELOG.md:160` |
| **P3-5** | ✅ TSan 跳过根因由"符号冲突"改为"TSan 接管全局分配函数"（两处）；LSan 桩实现的说明在报告 §十一 登记 | 报告 §六 |
| **P3-1** | ✅ 已删除 `LOGX_HAS_TSAN` 死条件；`LOGX_SKIP_CUSTOM_NEW_DELETE` 统一在文件头部定义，探针函数与 `operator new/delete` 同受该宏控制 | `tests/unit/test_hotpath_alloc.cpp:4-8`、`:57`、`:113` |
| **P3-2** | ✅ `__lsan_ignore_object(cb)` 已移至全部 open 失败路径之后、成功返回之前；WSL ASan/LSan 全量复跑 0 泄漏 | `core/src/logx_c.cpp:533-538` |
| **P3-3** | ✅ CMake 注释改写：说明 `.S` 未编入、禁用汇编的链接必要性、性能取舍与恢复路径 | `core/CMakeLists.txt:111-114`、`:132-133` |
| **P3-6** | ✅ ASan 与 TSan 分支拆分：桌面 ASan 取 quarantine 硬上界 256MB + 17% = **300MB**；TSan 保留 150MB；Android 40MB | `tests/unit/test_r19_regression.cpp:3113-3129` |
| **P3-7** | ✅ 保留 `ZSTD_DISABLE_ASM`，并在 CMake 注明"若编入 `huf_decompress_amd64.S` 可移除以恢复 x86_64 汇编加速" | `core/CMakeLists.txt:111-114`、`:132-133` |

---

## 八、代码收口与全端复跑实证（2026-10-25）

### 1. 代码改动清单

| 项 | 文件 | 变更 |
|---|---|---|
| P3-1 | `tests/unit/test_hotpath_alloc.cpp` | TSan 宏提前定义并统一；删除死条件 `LOGX_HAS_TSAN` |
| P3-2 | `core/src/logx_c.cpp` | `__lsan_ignore_object(cb)` 移至成功返回前 |
| P3-3 / P3-7 | `core/CMakeLists.txt` | 注释改写：链接必要性、性能取舍与恢复路径 |
| P3-6 | `tests/unit/test_r19_regression.cpp` | ASan 300MB（quarantine 硬上界推导）/ TSan 150MB / Android 40MB |
| N-1 | `tests/unit/test_r19_regression.cpp` | 补 `<condition_variable>` 显式包含 |
| N-2 | `third_party/mbedtls/src/aes.c` | 删除非 aarch64 未引用的 `mbedtls_aesce_has_support` 桩 |

### 2. 补遗缺陷（WSL Release 首编暴露，已修复）

- **N-1（P2，构建阻断）**：`test_r19_regression.cpp` 使用 `std::condition_variable` 却未直接包含 `<condition_variable>`。MSVC 与既有 WSL ASan/TSan 构建经 gtest 头间接获得该声明；Clang 18 Release + libstdc++ + 源码 gtest 下编译失败。已补显式包含。
- **N-2（P3，0 警告门禁）**：`aes.c` 非 aarch64 分支保留的 `mbedtls_aesce_has_support` 桩在 `-Wall -Wextra` 下触发 `-Wunused-function`（CE 调用点均已在 `LOGX_AESCE_HOOK` 内剔除，桩无引用）。已删除桩，仅保留 `CE_MODE_ENABLED() 0`。

### 3. 全端复跑结果（当前代码）

| 平台 | 构建 | 套件项 | 结果 |
|---|---|---|---:|
| Windows | MSVC Release（`build-full`） | 203+2+4+3 | **212/212** |
| Windows | MSVC ASan（`build-host-asan`） | 203+2+4+3 | **212/212** |
| WSL Ubuntu 24.04 | Clang 18 Release（`build-wsl-release`，本批新增） | 203+2+4+3 | **212/212**，clean 构建 26 步 0 警告 |
| WSL Ubuntu 24.04 | Clang 18 ASan+LSan（`build-wsl-asan`） | 203+2+4+3 | **212/212**，LSan 0 泄漏 |
| WSL Ubuntu 24.04 | Clang 18 TSan（`build-wsl-tsan`） | 203+0+4+3（2 skipped） | **210/210**，0 竞态警告 |
| Redmi K30 Pro | NDK 26.1 Clang ASan（`build-android-asan`） | 204+2+4+3 | **213/213** |
| **合计** | 三端六套 | — | **1271 项通过 + 2 项合理跳过** |

> 计数说明：Windows Release 212 与 WSL Release 212 为功能套件（非插桩）；动态插桩合计仍为 **847**（Windows ASan 212 + WSL ASan 212 + WSL TSan 210 + Android ASan 213）。

### 4. Android 调度抖动处置记录

- 首轮全量 203/204，唯一失败 `Observability.LatencyJitter_StddevCvWithinSanityBound`（CV=58.98 > 50，max=248ms）；
- 同窗口以**改动前 Android Release 部署**跑同一用例，CV=73.09 同样失败 ⇒ 设备调度噪声，非本批回归；
- 隔离重跑 2 次（CV=65.66 失败 / CV=2.715 通过）、全量重跑 204/204（CV=4.957），最终按 213/213 计。

### 5. 复跑命令（可复现）

```bash
# WSL Release（本批新增，必须强制源码 gtest，避免命中宿主机 Anaconda GTest）
cmake -S . -B build-wsl-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DLOGX_BUILD_TESTS=ON -DLOGX_BUILD_STRESS=ON -DLOGX_WITH_ZSTD=ON -DLOGX_WITH_AESGCM=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_GTest=TRUE \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<repo>/build-android-tests/_deps/googletest-src
ninja -C build-wsl-release
```

> 注：默认 configure 会命中 `/mnt/c/.../Anaconda3/Library/lib/cmake/GTest`（Windows 侧包），必须显式禁用；该环境差异也是 N-1 被长期掩盖的原因之一。

