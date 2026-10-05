# logx 最近 4 次提交 · 代码审查报告 (R5 收口 + T5.6 新功能)

> **审查对象**: `logx` 仓库 `master` 分支最近 4 次提交
> | # | 提交 | 类型 | 规模 |
> |---|---|---|---|
> | 1 | `3278844` fix(core,android): R5 复审批次收口——R4 外部复审 6 项发现闭环(6实0误) | 缺陷修复 | 9 文件 / +118 −24 |
> | 2 | `3d111bc` docs(changelog): 补齐 3278844 R5 提交记录行 | 文档 | 1 文件 / +1 |
> | 3 | `f3b64c8` feat(core,android): T5.6 事件种类扩展——截断独立事件 LOGX_EV_TRUNCATED | **新功能** | 13 文件 / +109 −36 |
> | 4 | `43d359a` docs(changelog): 补齐 f3b64c8 T5.6 提交记录行 | 文档 | 1 文件 / +1 |
>
> **审查日期**: 2026-09-13
> **审查基准**: `code_review_R4_2026-09-13.md`（上一轮 R4 复审报告）
> **方法**: 逐文件 diff 审阅 + 跨语言链路核对 + 枚举 ABI 与 ordinal 分析 + 全局残留假设检索 + 边界值数值复核

---

## 一、总体结论

**4 次提交无阻塞性缺陷。** `3278844` 对我上轮 6 项发现的收口**方向全部正确**，但其中 **R5-7 在收口我的"观察项"时引入了一处新缺陷**（`static_assert` 下界偏松）。`f3b64c8` 是本批次唯一**新功能**（新增事件种类），工程质量高：枚举追加 ABI 安全、彻底消除了我上轮指出的文案耦合、跨语言链路全同步，且未被注意的两处风险（压测事件处理器、Kotlin ordinal 依赖）经核查**均不适用**。

| 提交 | 结论 |
|---|---|
| `3278844` | 7 项中 6 项正确落实；**R5-7 引入 1×P3**（安全断言下界偏松） |
| `3d111bc` | 无风险 |
| `f3b64c8` | 无阻塞；实现正确，1×P3 观察（对外语义变更的迁移提示） |
| `43d359a` | 无风险 |

---

## 二、提交 `3278844` — R5 复审批次收口

### 2.1 逐项验证

| 项 | 上轮发现 | 处置 | 独立核实结论 |
|---|---|---|---|
| **R5-1** | **P2** 入库的机器绝对路径 | 迁出至用户级 `~/.gradle/gradle.properties` | ✅ `gradle.properties` 该行已删除（现 3 行）；README 补 JDK17 定位两方案。**可移植性恢复** |
| **R5-2** | P3 测试断言冗余、非真四态 | 补 `(&t,nullptr)`/`(nullptr,&d)` 混用态 | ✅ 现为真四态：`EXPECT_TRUE(truncated_only)` + `EXPECT_GT(dropped_only, 0u)` |
| **R5-3** | P3 文案耦合与自述矛盾 | 改限定表述 + 立案 Backlog B-3 | ✅ 头注释明确"ERR_TOO_LARGE 共码分流为已知文本耦合"，与 `NO_SPACE`/`INVALID_ARG` 的"已消除"区分开 |
| **R5-4** | P3 文档元数据三元组重复 3 份 | 去重 | ✅ 计数由 3 降为 **1** |
| **R5-5** | P3 `>60KB` 口径不精确 | 改为 `user_total = 27B 头 + tag + payload` | ✅ 与 `format.cpp:110-111` 实现一致 |
| **R5-6** | P3 `getStats()` 无长度守卫 | 加 `arr.size < 6` 守卫 | ✅ 返回 null，与"init 前/close 后返回 null"既有契约口径统一 |
| **R5-7** | 观察项：建议 `static_assert` 固化不变量 | marker 常量化 + 2 条 assert | ⚠️ **部分正确**：assert #1 正确；**assert #2 下界偏松 64B**（见 §4.1） |

### 2.2 正确性确认

- **assert #1**（`format.cpp:184-186`）：`kMarkerReserve(48) >= 28 + 10 + 7 = 45` → 正确固化"marker 长度上界"前提。
- **MARKER 常量化**：`snprintf("%s%llu%s", kMarkerHead, ..., kMarkerTail)` 使字面量与断言**同源**，消除二者漂移风险 —— 这是良性改进。

---

## 三、提交 `3d111bc` 与 `43d359a` — CHANGELOG

**改动**：各补 1 行提交表记录。**无代码风险**。

**核实**：全表提交号去重检索 **无重复**；两行描述与对应提交实际内容一致（`3278844` 行 7 项列举完整；`f3b64c8` 行概括了枚举追加、Kotlin/探针/tester 接线与测试同步）。

---

## 四、提交 `f3b64c8` — T5.6 事件种类扩展（新功能）

### 4.1 核心变更与验证

| 维度 | 变更 | 独立核实 |
|---|---|---|
| C 枚举 | 追加 `LOGX_EV_TRUNCATED = 5` | ✅ **末尾追加**，既有值 1..4 不变，底层类型/尺寸不变，按值传递 → **ABI 安全** |
| 事件语义 | `(DROP, TOO_LARGE)` → `(TRUNCATED, LOGX_OK)` | ✅ 返回值仍 `LOGX_OK`、`truncated_logs` 计数与"调用方线程锁外同步"（P1-1/J-04）契约不变 |
| 病态丢弃 | 独占 `DROP / TOO_LARGE` | ✅ `logx_c.cpp:445` 不变，与截断按 kind 天然分流 |
| Kotlin 映射 | `1→DEGRADED … 5→TRUNCATED, else→IO` | ✅ 与 C 枚举**逐值对齐**；`LogX.kt:362-367` |
| Kotlin ordinal | 新值插入枚举**末尾** | ✅ 全仓库检索 `.ordinal` / `EventKind.values` → **零依赖**，无序号错位风险 |
| 探针分类 | 改 **kind 优先**：`c.kind == TRUNCATED → R.Truncated` | ✅ `LogXProbe.kt:120`；**R5-3 记载的文案耦合已彻底消除** |
| C++ 内部分发 | — | ✅ 全仓库无 `switch(kind)`，无遗漏分支 |
| 其它语言绑定 | — | ✅ 检索 `*.py/*.java/*.cs/*.rs/*.go` 无事件枚举绑定，**无同步缺口** |
| `code == 0` 处理 | 新事件 code 取 `LOGX_OK = 0` | ✅ 检索无任何 `code == 0 / != 0` 特殊判断，传 0 无副作用 |

**测试同步核对**：
- `test_flusher.cpp:1227-1228` 过滤器改 `LOGX_EV_TRUNCATED` ✅
- `test_j04_regression.cpp:132-143` 断言改 `(LOGX_EV_TRUNCATED, LOGX_OK)`，kind+code 双确认、同步/调用方线程断言保留 ✅；`ErrorProbe.kinds` 为**既有字段**（`:41`），非缺失
- `LogXPureLogicTest.kt` 映射扩 `kindInt=5`，断言数 5→6 ✅
- **潜在遗漏已排除**：`test_concurrent.cpp:57` 的事件处理器仍只认 `LOGX_EV_DROP`，但该压测 `payload ≤ 1500B` 远小于 `max_log_length = 20480` → **不触发截断**，无需同步（非缺陷）

**文档同步**：spec §3.1 截断事件表述改写为 `LOGX_EV_TRUNCATED`；计划表升 v1.15，T5.6 销号、B-3 闭环 ✅；CHANGELOG 条目完整 ✅。

### 4.2 跨层一致性

`LogXProbe.kt:30-36` 头注释、`logx_c.h:47` 枚举注释、`:120` `logx_write` 契约、CHANGELOG、spec §3.1 —— **五处表述一致**，未见残留的"截断=DOP/TOO_LARGE"旧描述（旧表述仅存于 v1.12 历史修订说明，属正常历史留存）。

---

## 五、本次新发现问题

### 5.1 P3 — `static_assert` 下界偏松，未固化真实安全前提（`3278844` 引入）

`core/src/format.cpp:213-219`：

```cpp
constexpr uint32_t kTruncBufSize = 60 * 1024;                       // 61440
static_assert(kTruncBufSize >=
                  60u * 1024u - 27u - 64u - kMarkerReserve +        // ← 多减了 64
                  (sizeof(kMarkerHead) - 1 + kMaxU32Digits + sizeof(kMarkerTail) - 1),
              "trunc_buf must hold worst-case truncated msg (prefix + max marker)");
```

- 断言要求的下界 = `61440 − 27 − 64 − 48 + 45` = **61346**
- 真实最坏 `msg_len` = `(61440 − 27 − tag_len − 48) + marker`；**`tag_len = 0` 时**（空 tag 可达：`tag_len_bounded` 与 `logx_write:425` 均只拒 NULL）→ `(61413 − 48) + 45` = **61410**
- 即断言**比真实需求宽松 64 字节**（恰为被减去的 tag 上限）

**后果**：该断言无法阻止未来把 `kTruncBufSize` 误缩到 61350 —— 届时 `tag_len=0` 场景将**溢出写穿缓冲**（最多 61410 > 61350）。与其设立的初衷"固化 kTruncBufSize 安全前提"相悖。**非当前崩溃**（61440 ≥ 61410 仍安全），属防护机制失效。

**旁证矛盾**：同文件 `:213-214` 注释称"budget_max = 61440 − 27 − **64(tag 上限)**"，而 `:260-262` 注释又写"实际最坏写入 **61407**(max_total = 61458, **tag=0**)" —— 作者已知真实最坏为 tag=0，断言却仍按 tag=64 推导，**两处口径互相矛盾**。（我的独立穷举复核恰为 `max_total=61458, tag=0 → 61407`，与该注释一致。）

**建议**：断言应为 `kTruncBufSize >= 60*1024 - 27 - kMarkerReserve + 45`（= 61410），即**不减去 tag 上限**（取 tag_len 最小值 0）。

### 5.2 P3（观察）— 对外语义变更为破坏性，缺少下游迁移提示

T5.6 将截断事件由 `LOGX_EV_DROP` 改为 `LOGX_EV_TRUNCATED`，**既有按 `kind == LOGX_EV_DROP` 过滤截断的第三方监控将不再收到事件**。Backlog B-3 原文已将其列为决策点（"须同步通告"），提交经用户拍板采纳方案B，但 CHANGELOG 未显式给出"截断事件已移出 DROP 过滤器"的**迁移提示**。对内部唯一消费方（本仓库 tester）无影响；若存在外部消费方，建议在 release note 中显式标注。

---

## 六、验证局限（必须说明）

同前两轮：**本机无法编译与运行测试套件**（环境禁用 `cmd.exe`，MSVC `INCLUDE` 环境无法经 `build_tmp.bat` 装配）。因此提交信息中「三矩阵 123/129/142 单测 + 压测 4/4×3 全绿」「kotlin-gradle/logx-tester 构建通过」「arm64 交叉 0 错误」**本次均未能独立复现**，建议在可用环境复跑。

本报告结论建立在：静态代码审阅、跨语言链路交叉引用检索、枚举 ABI/ordinal 分析、全局残留假设检索、以及边界值数值复核之上。

---

## 七、结论

- `3278844`：R5 收口**方向全部正确**，6 项落实无误；唯 **R5-7 的 `static_assert` 因多减 tag 上限而偏松 64B**，使其保护作用落空（P3，建议修正为不减去 tag 上限）。
- `3d111bc` / `43d359a`：纯文档，无风险，表格无重复。
- `f3b64c8`：新功能实现**正确且完整** —— 枚举 ABI 安全、我上轮指出的文案耦合被彻底根除、跨语言/测试/文档全同步，两处潜在遗漏（压测事件处理器、Kotlin ordinal）经核查不适用。仅建议补一条对外迁移提示。

**待收口**：1×P3（`static_assert` 下界）+ 1×P3 观察（迁移提示）。均非阻塞，可在下一批次一并处理。
