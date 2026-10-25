# R48 收口复核报告 — R47 待收口 4 项代码落地（工作区批次）

> **审查对象**: `HEAD = be22508` + 工作区未提交改动（12 文件 / +214 / −40）
> **基线**: `be22508` docs(review): R47 审查报告入库与 T5.8 文档状态口径校正
> **来源**: [`code_review_R47_2026-10-24.md`](code_review_R47_2026-10-24.md) §四（2×P2 + 5×P3）
> **审查方法**: 逐文件通读 + 双端 clean 构建 + 四套件实跑 + 变异实验 + 布局探针 + 调用点全仓核对
> **审查日期**: 2026-10-25
> **审查结论**: R47 的 **2×P2 + 5×P3 全部闭环**（文档 3 项于 `be22508`，代码 4 项于本批）；**无新增缺陷**，无 P0/P1；建议以单批 `fix(core,jni,test,docs)` 提交。

---

## 一、总体结论

1. **`P2-N1`（唯一实质缺陷）已确定性闭环**：`Instance` 存活净计数（构造+1/析构-1）+ 内存阈值 8MB→3MB/4MB 双闸；**变异验证：删 `delete inst` 后两断言双红**（净计数差值 10050 ≠ 0；实测增量 6.28MB > 3.15MB）。R47 点名的"删 `delete inst` 仍绿"形态不再成立。
2. **`P3-N1`/`P3-N4`/`P3-N5` 均已闭环**，且 `P3-N5` 的完整性经额外核对确认：`append_block` 的块写入为明文（`key32_or_null == nullptr`），因此"文件头 kid"与"探测 kid"统一取入参后**不存在 header kid 与块密钥错配**的隐患。
3. **无新增运行时风险**：`Instance` 仅新增静态计数器与构造/析构，`sizeof(Instance)` 实测仍 **352**、`InstanceControlBlock` 仍 **64 / alignof 8**（静态成员不进入实例布局）。
4. **验证充分性**：MSVC clean 55 步 0 警告、NDK clean 61/61 步 0 警告、四套件 **212 项全绿**、内存有界用例 **3 轮稳定**。
5. **过程性事故 1 起（已自检修复）**：变异实验用 `git checkout -- core/src/logx_c.cpp` 整文件回滚，连带抹掉了同文件的 `P3-N4` 修复；在最终 diff 复核阶段发现并重放，已重新编译 + 全量复跑。教训见 §五。

### 变更清单

| 文件 | 项 | 变更 |
|---|---|---|
| `core/include/logx_internal.h` | P2-N1 | `Instance` 新增 `inline static std::atomic<int64_t> alive_instances_`、构造/析构 ±1、静态读接口；零布局影响 |
| `tests/unit/test_r19_regression.cpp` | P2-N1 | `OpenCloseMemoryBounded` 新增存活计数差值断言（轮次前后 = 0）；阈值 8MB → host 3MB / Android 4MB；三处注释口径同步 |
| `android/cpp/jni_key_cache.h` | P3-N1 | `scrub_by_watermark` 注释如实化：仅清零 `gen ≤ 水位`（已真正销毁）槽位 |
| `android/cpp/logx_jni.cpp` | P3-N1 | `nativeClose` 注释区分同步/递延两条路径的实际效果 |
| `core/src/logx_c.cpp` | P3-N4 | `cb->pal.store` 由 `relaxed` 改 `release`（与三处 `acquire` 读配对）+ 注释 |
| `core/src/flusher.cpp` | P3-N5 | `append_block` 建文件探测与 `write_file_header` 改用入参 `kid`；删除 `(void)kid;` + 注释 |
| `docs/lifecycle_control_block_plan.md` | 台账 | v1.6：§7.1/§7.2 两行状态更新、§7.3 门禁证据更新、§7.4 全部标 ✅ 并附闭环证据 |
| `docs/lifecycle_control_block_decisions.md` | 台账 | v1.6：状态与 v1.6 修订说明 |
| `docs/LogX后续实施计划表.md` | 台账 | v1.70：T5.8 行状态 + 实施进展 ⑦ |
| `CHANGELOG.md` | 台账 | 第 24 条加"R47 待收口项落地"行；提交表补登 `be22508` |

---

## 二、R47 发现逐项核实

| 编号 | R47 定级 | 落地方式 | 独立核实结论 |
|---|:--:|---|---|
| **P2-N1** | P2 | `Instance` 净计数断言 + 阈值收紧 | ✅ **成立**。正常路径 3 轮全绿（增量 1.07MB，阈值 3MB ≈2.8× 余量）；**删 `delete inst` 直接红**（计数断言 + 内存断言双报）；断言采用"差值"而非绝对值，不与其他用例的实例残留耦合 |
| **P3-N1** | P3 | 两处注释如实化 | ✅ **成立**。注释语义与 `scrub_stale_slots(gen ≤ watermark)` 实现一致；`logx_c.h:129-131` 契约与本注释不矛盾；core 侧 `gen` 暴露方案按 R47 建议**不做**（API 变更收益仅收窄残留窗口） |
| **P3-N4** | P3 | `relaxed` → `release` | ✅ **成立**。三处读侧均为 `acquire`（`internal.h:128`、`logx_c.cpp:333`、`:537`），写侧 `release` 构成配对；开销可忽略——x86 上 release 与 relaxed 同为普通 `mov`，arm64 为 `STLR`（仅 open 路径一次） |
| **P3-N5** | P3 | `append_block` 改用入参 `kid` | ✅ **成立且完整**。全仓 10 处调用点逐一核对：`test_flusher`（`inst.kid=1` / 传 1）、`test_recovery`（默认 0 / 传 0）、`test_r19`（`cfg.kid=1` / 传 1），**行为不变**；`write_and_record` 的 `key32_or_null` 为 `nullptr`（块明文），故不存在"header kid 与块密钥"错配 |

**统计**：4 项代码余量 **全部闭环**；连同 `be22508` 的文档 3 项，R47 的 2×P2 + 5×P3 **全部闭环**。

---

## 三、独立验证（实证）

### 3.1 双端 clean 构建

| 目标 | 环境 | 结果 |
|---|---|---|
| MSVC（`build-full`，Ninja） | MSVC 14.43.34808 + SDK 10.0.22621.0 | `ninja -t clean` 后 **55 步全绿，warning = 0** |
| Android（`build-android-tests`，arm64-v8a） | NDK 26.1.10909125 | `ninja -t clean` 后 **61/61 步全绿，warning = 0** |

### 3.2 四套件实跑（clean 产物、工作区外运行）

| 套件 | 结果 |
|---|---|
| `logx_unit_tests` | **203/203 通过**（26.7s） |
| `logx_hotpath_alloc_tests` | **2/2 通过** |
| `logx_stress_tests` | **4/4 通过** |
| `logx_crash_fork_tests` | **3/3 通过** |
| 合计 | **212 项全通过** |

### 3.3 变异实验（针对 P2-N1 的"必红"承诺）

| 变异 | 预期 | 实测 |
|---|---|---|
| `destroy_instance` 内注释掉 `delete inst` | 必红 | **红**：`:3079` 存活净计数断言失败（差值 10050）+ `:3094` 内存 `6,279,168 > 3,145,728` |
| 还原后正常路径 | 必绿 | **绿**：`OpenCloseMemoryBounded` **3 轮连续通过**（18.7s / 轮） |

### 3.4 布局探针（MSVC `/std:c++17`）

```
sizeof(InstanceControlBlock)=64  alignof=8
sizeof(Instance)=352
```

→ 静态计数器与构造/析构**未改变**任何实例布局；`static_assert(sizeof(CB) <= 64)` 仍成立。

### 3.5 归因与连带影响核查

- `Instance` 由聚合类型变为带用户构造/析构的类型：全仓检索 `Instance` 的构造点，仅 `new Instance()` / `Instance inst{}` 两种，均为值初始化，语义不变；无位置初始化（positional aggregate init）调用点。
- 计数器使用"轮次差值"而非绝对值：避免与其他用例残留实例产生跨测试耦合。
- `P3-N5` 改动触及加密封装路径的唯一入口（探测 + 32B 头），无其它 kid 使用点残留（`grep kid` 复核）。

---

## 四、新发现问题

### 观察项（不计级）

1. **过程事故（已修复）**：变异回滚使用 `git checkout -- <file>` 整文件还原，连带丢弃同文件内的 `P3-N4` 修复；最终 diff 复核时发现（`logx_c.cpp` 未出现在 `git status` 中）。**规程建议**：变异实验应"只回滚变异片段"（反向 Edit 或 `git stash` 单文件），回滚后**必须核对 `git status` 的文件清单与修复项是否一一对应**。
2. **`Instance` 类型性质变化**：非聚合化属可接受变更（内部类型、无 ABI 暴露），但后续若有人写 `Instance{...}` 位置初始化会编译失败——这是一次性成本，已在注释中说明"仅静态成员"。
3. **阈值平台分支**：Android 走 `/proc/self/statm` RSS，抖动大于 Windows `PrivateUsage`，故分 4MB；该分支**未在真机验证**（见 §五）。

---

## 五、验证局限（必读）

1. **未执行 ASan/TSAN**：并发正确性判断仍为静态推演 + 重复运行 + 压测/崩溃套件，不等于证明。本轮改动未触及并发时序，风险低。
2. **未上 Android 真机**：`__ANDROID__` 分支的 4MB 阈值、`/proc/self/statm` 采集仅在交叉编译层面验证。
3. **性能门禁未复跑**：`P50 ≤ 4.2µs`、AAR 体积未测；本轮新增构造/析构各一次 `relaxed` 原子运算，仅发生在 open/close（非热路径）。
4. **变异实验覆盖**：本轮仅做了 `delete inst` 一项变异（针对 P2-N1 的承诺）；`P3-N4`/`P3-N5` 属语义/配对修正，无可观测行为差异，未做变异。
5. **本报告审查对象为工作区 diff（未提交）**：提交后哈希需在下一批 CHANGELOG 中补登（仓库惯例）。

---

## 六、待提交内容与后续

**建议提交**（单批 `fix(core,jni,test,docs)`，12 文件 +214/−40）：

- 代码：`logx_internal.h`、`logx_c.cpp`、`flusher.cpp`、`jni_key_cache.h`、`logx_jni.cpp`
- 测试：`test_r19_regression.cpp`
- 文档：`code_review_R48_2026-10-25.md`（本报告）+ `reviews/README.md` 索引 + `CHANGELOG.md` + plan v1.6 + decisions v1.6 + 计划表 v1.70

**提交后仍待（纯环境门禁，无代码缺陷）**：ASan/TSAN 复跑、真机四套件、P50 ≤ 4.2µs 重测、AAR 体积门禁——全部达成后方可把 T5.8 与发布状态从「未发布 / 进行中」转正。
