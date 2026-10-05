# logx 最近 2 次提交 · 代码复审报告 (R4 复审)

> **复审对象**: `logx` 仓库 `master` 分支最近 2 次提交
> - `5828b3f` — docs(changelog): 补齐 516d091 R4 提交记录行
> - `516d091` — fix(core,android): R4 审查批次收口——T5.5 外部审查 6 项发现闭环(5实1误)
>
> **复审日期**: 2026-09-13
> **复审基准**: 上一轮审查报告 `code_review_2026-09-13.md`（对象 `28acf7f`/`cf08e75`）
> **范围**: `516d091` 共 15 文件 / +168 −26；`5828b3f` 1 文件 / +1
> **方法**: 逐文件 diff 审阅 + 跨语言链路核对 + 边界值数值穷举 + 交叉引用检索

---

## 一、总体结论

**两次提交无阻塞性缺陷。** `516d091` 是对上一轮 5 项发现的定向收口，**5 项均已落实**；其中我上轮关于"失真告警不触发"的影响推断**经独立核实确属错误**，提交方的"证伪"成立。修复工程质量高：ABI 尺寸保持不变、匿名命名空间迁移经检索确认无外部引用、缓冲区缩容经 **19,795,743 组穷举**证明安全。

新发现 **1 项 P2 + 5 项 P3**，均非阻塞：核心是一项**提交进仓库的机器绝对路径**（构建可移植性），其余为测试断言冗余、文档一致性等。

| 提交 | 性质 | 风险 |
|---|---|---|
| `5828b3f` | 纯文档（CHANGELOG 补 1 行） | 无 |
| `516d091` | 缺陷修复批次（15 文件） | 无阻塞；1×P2 + 5×P3 |

---

## 二、提交 `5828b3f` — docs(changelog)

**改动**：`CHANGELOG.md` 提交表补入 `516d091` 一行。**无代码风险**。

**核实**：全表提交号去重检索 —— **无重复行**；新行对 `516d091` 的概括（"5实1误;出参守卫/truncatedLogs 接线/msg 契约/匿名空间/契约注释"）与提交实际内容一致。上一轮指出的 `9b1af14` 缺行问题已在 `516d091` 内一并补上（R4-6），表格历史链现已连续。

---

## 三、提交 `516d091` — R4 审查批次收口

### 3.1 逐项验证：上轮发现是否真正闭环

| 上轮发现 | 本提交处置 | 独立核实结论 |
|---|---|---|
| **P1** 出参无 NULL 守卫，与 `logx_internal.h` 契约冲突 | R4-1：4 处赋值加守卫 | ✅ **成立**。`format.cpp:219-220`（入口）、`283-284`（出口）覆盖全部 4 处；早退分支不解引用出参。契约方 `logx_internal.h:101`「可为 NULL」现为真 |
| **P2** Android 未接线新指标 | R4-2：JNI 5→6 元素 + Kotlin + tester 全链路 | ✅ **成立且已完成**（链路核对见 §3.2） |
| **P2** 截断返回 OK → 计入 writeOk → 告警不触发 | 提交方判定此为**误报** | ✅ **我方推断确属错误，证伪成立**（见 §3.3） |
| **P2** `logx_error_fn` 的 `msg` 无生命周期约定 | R4-3：`logx_c.h` typedef 补契约注释 | ✅ **成立**。注释明确"仅回调期间有效，留存须自行拷贝"，与截断路径栈缓冲 `char msg[64]` 一致 |
| **P3** 内部符号具外部链接却无声明 | R4-4：收入匿名命名空间 | ✅ **成立且安全**。全仓库检索：`solve_truncated_msg_budget`/`kMarkerReserve` **仅** `format.cpp` 内引用，无测试或其他 TU 依赖，迁移无副作用 |
| **P3** `max_total_bytes==0` 文档与实现不符 | R4-5：改注释为实际语义 | ✅ **成立**。新注释与 `format.cpp:234/242-243` 行为一致（`0` → 不截断；超 60KB → `kCannotFit`） |
| **P3** CHANGELOG 缺 `9b1af14` 行 | R4-6：补行 | ✅ **成立** |

### 3.2 R4-2 全链路核对（Android 接线）

| 环节 | 位置 | 核实 |
|---|---|---|
| C 结构体字段 | `core/include/logx_c.h:155` | `truncated_logs` 占原 `reserved[0]` |
| ABI 尺寸 | `logx_stats_t` | 6 命名字段 + `reserved[3]` = **9×8 = 72B 不变**，符合其自身 E-07 契约 ✅ |
| 原子计数写入 | `core/src/logx_c.cpp:449` | `truncated_logs.fetch_add(1)`（仅截断路径） |
| get_stats 出参 | `core/src/logx_c.cpp:530` | `out->truncated_logs = ...load()` |
| JNI | `logx_jni.cpp:462-472` | `NewLongArray(6)` + `SetLongArrayRegion(arr,0,6,buf)` ✅ |
| Kotlin 解包 | `LogX.kt` `getStats()` | `truncatedLogs = arr[5]` ✅ |
| 探针分流 | `LogXProbe.kt:108-110` | `ERR_TOO_LARGE` 下按 msg 前缀分流 |
| 压测指标 | `StressRunner.kt` | `truncatedCount` 计入 `drops` → `distorted` ✅ |
| 单写路径 | `TesterViewModel.kt:230-235` | 新增 `R.Truncated` 分支，`truncatedLogs` 递增 |
| UI/对账 | `MetricsPanel.kt` / `Preset 8,9` | 标签消歧（`truncatedBlocks` vs `truncatedLogs`）；Preset 期望文案改为截断语义 ✅ |

**既有测试无回归**：`Robustness, StatsReservedZeroContract` 使用 `sizeof(st.reserved)/sizeof(st.reserved[0])` 自适应数组长度，`reserved[4]→[3]` 不影响其通过。

**Preset 期望值验算**（独立复算）：
- Preset 8（`maxLogLength=65536`，payload 70000B）：`user_total≈70029 > 60KB` → 截断，`budget=61411`，落环总长 `61449 ≤ 65536` → **返回 OK、计数 +1** ✅ 与文案一致
- Preset 9（`maxLogLength=16384`，payload 30000B）：`budget=16349`，落环总长 `16376 ≤ 16384` → **返回 OK、截断保前缀** ✅ 与文案一致

### 3.3 关于"1 误"的独立核实（我方上轮错误）

上一轮我断言："截断返 `LOGX_OK` → 计入 `writeOk` → 失真告警不触发"。**该推断错误**，提交方证伪成立。证据链：

1. `LogXProbe.write()`（`LogXProbe.kt:93-102`）在调用前将 `Capture` 置入 **ThreadLocal 哨兵**，`LogX.log()` 返回后清除。
2. C++ `logx_c.cpp:452-454` 在**截断路径同样同步发出** `LOGX_EV_DROP / LOGX_ERR_TOO_LARGE`（msg `"log truncated (dropped N bytes)"`），经 JNI trampoline 在**调用方线程**回调 `onEvent`。
3. `onEvent`（`:81-85`）命中哨兵 → `hit=true, code=ERR_TOO_LARGE`。
4. 修复前分类为 `c.code == ERR_TOO_LARGE -> R.TooLarge(dt)` → 计入 `dropTooLarge` → `drops>0` → **`isDistorted=true`，告警照样触发**。

**结论**：告警从未被掩盖；真实问题是截断（有损写入）被误标为 `DROP: log too large`。R4-2 "接线该做、理由修正"的处置恰当。此处的教训是：**分类依据是事件而非返回值**，我上轮未核对 `LogXProbe` 的捕获机制即下因果，属推断越界。

### 3.4 独立数值验证（本次唯一潜在安全项：`trunc_buf` 缩容）

R4-7 将 `format.cpp:250` 的 `trunc_buf` 由 64KB 缩至 **60KB(61440)**。因写入目标缩小，须重新证明无溢出。独立复刻 `solve_truncated_msg_budget` + 拼装逻辑穷举：

```
扫描组合总数          : 19,800,066
进入截断路径组合数    : 19,795,743
[A] max(prefix_len+marker_len) = 61407 字节   （最坏 max_total=61458, tag_len=0）
    trunc_buf 容量             = 61440 字节
    安全余量                   = 33 字节  => 安全
[B] 落环总长不变量 total <= max_total : 违反 0 次 => 成立
```

**结论**：缩容**安全**。上界可解析证明：`写入 ≤ (budget−48) + marker_len ≤ 61440−27−48+45 = 61410 < 61440`（`marker_len ≤ 45`，因 `payload_len` 为 `uint32_t` 故丢弃字节数 ≤ 10 位十进制）。

---

## 四、本次新发现问题

| 级别 | 问题 | 位置 | 说明 |
|---|---|---|---|
| **P2** | **提交进仓库的机器绝对路径** | `android/kotlin-gradle/gradle.properties:7` | 新增 `org.gradle.java.home=D\:/DevEnv/Java/jdk-17.0.4.1`。该文件已被 git 跟踪，任何不具备此精确路径的开发机或 CI **必然构建失败**。建议改用 `JAVA_HOME` 环境或 CI 注入，勿入库 |
| **P3** | 新增测试断言冗余、"四态"名不副实 | `tests/unit/test_format.cpp:391-396` | 第三个断言与第一个**逐字节相同**（同一 `big(600,'L')` + `max_total=500` + `nullptr,nullptr`），注释却称"单 NULL 混用"。真正未覆盖的是 `(&truncated, nullptr)` 与 `(nullptr, &dropped)` 两种混用态。CHANGELOG 与提交信息称"四态单测"，实为 2 态 + 1 重复 |
| **P3** | 分流依赖错误文案，与文件自述原则矛盾 | `LogXProbe.kt:108-110` vs 头注释 `:35-36` | 头注释声明"**彻底消除了对错误消息文案的脆弱依赖**"，但新增分流恰恰用 `msg.startsWith("log truncated")`。因截断/病态丢弃共用 `ERR_TOO_LARGE` 码，属受限取舍；建议后续以独立错误码消除文本耦合 |
| **P3** | 文档头部元数据三元组重复 | `docs/LogX后续实施计划表.md:4-6 与 9-11` | "制定日期/基准方案/变更日志"各出现 **3 次**；本次提交在 v1.13 块新增一份，与 v1.12 块重复（属既有模式的延续） |
| **P3** | R4-5 新注释口径不够精确 | `core/include/logx_format.h:48-50` | 文案写"用户**载荷** >60KB 硬上限"，实际 `format_record` 门槛是 `user_total = 27 + tag_len + payload_len > 60KB`（`format.cpp:110-111`）——长 tag 会提前触限 |
| **P3** | Kotlin 解包无长度守卫 | `android/kotlin/LogX.kt` `getStats()` | `arr[5]` 直接索引，未 `require(arr.size >= 6)`。Kotlin 与 `.so` 同包发布时无实害，但版本错配（旧 `.so` + 新 Kotlin）会抛 `ArrayIndexOutOfBoundsException` |
| 观察 | 缩容后余量偏薄、措辞偏乐观 | `core/src/format.cpp:248-249` | 注释称 61440"绰绰有余"，实测最坏写入 61407，**余量仅 33B**。数学正确，但安全完全依赖 `marker_len ≤ 45` 与 `payload_len` 为 `uint32_t` 两条前提；建议加 `static_assert` 固化该不变量 |

---

## 五、验证局限（必须说明）

与上一轮相同：**本机无法编译与运行测试套件**——环境安全策略禁用 `cmd.exe`，MSVC 的 `INCLUDE` 环境无法经 `build_tmp.bat`（内部调 `vcvars64.bat`）装配。因此提交信息中「三矩阵 123/129/142 单测 + 压测 4/4×3 全绿」「arm64 交叉 0 错误」「logx-tester assembleDebug 通过」**本次未能独立复现**，建议在可用环境中复跑确认。

本报告的全部结论建立在：静态代码审阅、跨语言链路交叉引用检索、边界值数值穷举（19,795,743 组）之上，不含对编译产物的运行时验证。

---

## 六、结论

`516d091` 是一次**高质量的定向收口**：5 项发现全部落实且经核实无误，ABI 兼容规范（72B 尺寸不变、`reserved` 递减），匿名空间迁移与缓冲区缩容均经检索/穷举证明安全。提交方对我方误报的证伪**准确且论证完整**——这一点值得肯定：审查意见并未被无条件采纳。

待收口项：**P2** 入库的机器绝对路径（构建可移植性）应优先清理；**P3** 六项为测试/文档一致性与文本耦合，建议下一批次一并处理。
