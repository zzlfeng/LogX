# LogX 代码审查报告 — 最近 2 次提交

- **审查对象**: `D:\Project\Android_Project\LogX\logx`（C/C++ 崩溃安全日志库）
- **审查范围**: `cf08e75`、`28acf7f`
- **审查日期**: 2026-09-13
- **审查方式**: 逐文件 diff 审读 + 跨语言调用链追踪 + 独立算法复刻验证
- **结论摘要**: 未发现会导致崩溃、数据损坏或安全漏洞的缺陷。核心算法不变量经 643,215 组
  组合验证成立。发现 1 项文档与实现不符（潜在 NULL 解引用）、1 项 Android 侧功能未接线、
  1 项 API 契约未文档化、2 项代码规范/文档一致性问题。

---

## 一、提交 `cf08e75`

**标题**: `docs(changelog): 补齐 28acf7f T5.5 提交记录行`
**改动**: `CHANGELOG.md`，1 行新增（表格中补入 `28acf7f` 行）。

```diff
+| `28acf7f` | feat(core): T5.5 超长日志截断保前缀——B-1 方案B 落地,超长不再整条丢弃(预算求解+marker+truncated_logs 计数,顺带修正 >60KB 误报 INVALID_ARG) |
```

**审查发现**

| 项 | 结论 |
|---|---|
| 逻辑错误 | 无（纯文档） |
| 内容准确性 | 该行对 `28acf7f` 的描述与提交实际内容一致 |
| 重复行 | 无；全表 `28acf7f` 仅出现 1 次 |
| 规范性 | 无 |
| 安全性 | 无（不涉及代码） |

**唯一观察（P3，文档完整性）**：CHANGELOG 提交表仍缺 `9b1af14`
（`docs: 记录 LogX.kt 状态机复杂度上限备忘与后续并发测试门禁规范 (B-2)`，向 CHANGELOG
写入 19 行但未为自己建表格行）的记录行。同一补齐模式下 `d2ab63f` 已为 `7621993` 补行，
但 `9b1af14` 被遗漏，表格历史链存在一处断点。

---

## 二、提交 `28acf7f`

**标题**: `feat(core): T5.5 超长日志截断保前缀——B-1 方案B 落地`
**规模**: 13 文件，+604 / −52

### 2.1 主要改动内容

| 层次 | 文件 | 改动要点 |
|---|---|---|
| 语义 | `core/src/logx_c.cpp` | `logx_write` 超长日志由「整条丢弃 + `LOGX_ERR_TOO_LARGE`」改为「截断保前缀 + 照常落环 + 返回 `LOGX_OK`」；新增 `truncated_logs` 计数与 `"log truncated (dropped N bytes)"` 事件（调用方线程、锁外同步发出） |
| 算法 | `core/src/format.cpp` | 新增 `tag_len_bounded`（从 `format_record` 抽出复用）、`solve_truncated_msg_budget`（预算求解）、`format_record_bounded`（三态：`kOk`/`kInvalidInput`/`kCannotFit`） |
| 协议 | `core/include/logx_internal.h` | `build_records` 新增 `bool* out_truncated`、`uint64_t* out_dropped` 出参；返回码新增 `kBuildRecordsCannotFit = -2` |
| 装配 | `core/src/flusher.cpp` | `build_records` 改调 `format_record_bounded`，把 `kCannotFit` 映射为 `kBuildRecordsCannotFit` |
| ABI | `core/include/logx_c.h` | `logx_stats_t.reserved[0]` → `truncated_logs`，`reserved[3]`（`sizeof` 不变，二进制兼容）；修订 `logx_write` 返回码语义注释 |
| 内部 | `core/include/logx_internal.h` | `Instance` 新增 `std::atomic<uint64_t> truncated_logs` |
| 测试 | `tests/unit/*` | 新增 `FormatBounded` 7 项；改写 E-02 / `ErrorMonitoring` / J-04 三处旧丢弃断言；新增 J-04 病态上限孪生用例 |
| 文档 | `docs/*`、`CHANGELOG.md` | spec §3.1/§5/§7 语义同步；计划表升 v1.12，T5.5 销号 |

**顺带修复的既有缺陷**（经核对为真实缺陷）: 旧 `format_record` 对
`user_total > 60KB` 返回 `false`，而 `build_records` 把所有 `false` 统一映射为
`kBuildRecordsInvalidInput` → `logx_write` 误报 `LOGX_ERR_INVALID_ARG`。
新实现把「超长」与「输入非法」正确分离，该误分类已消除。

### 2.2 核心算法正确性（已验证）

`format_record_bounded` 的三态判定与 `solve_truncated_msg_budget` 的预算上界是本次
风险最高的部分，已用独立复刻程序做穷举验证：

- 组合空间：`tag_len ∈ {0,1,8,63,64}` × `max_total ∈ 1..399 ∪ {1000,4096,8192,20000,20480,20486,40000,61440,61446,61461,65535,65536}` × `payload_len`（0..299 全量 + 边界点 20000/20479/20480/20481/39999/40000/60000/61439/61440/61441/100000/500000）
- 共 **643,215** 组
- 结果：
  - **不变量违反（落环总长 > max_total）：0 例** ✅
  - **`out_dropped` 无符号下溢（prefix_len > payload_len）：0 例** ✅

数学上该上界可证：预算 `b = cap(s*) − (27+tag)`，其中
`cap(s*) = min(20480·s*, max_total − 6s*, 60KB)`；截断后 `user_total' ≤ fixed + b = cap(s*) ≤ 20480·s*`，
故实际片数 `s' = ⌈user_total'/20480⌉ ≤ s*`，得
`total' = user_total' + 6s' ≤ (max_total − 6s*) + 6s* = max_total`。**结论：算法正确**。

其他已核对的正确性要点：

1. **热路径成本未变**：快路径恰好一次 `format_record`，仅超限才进截断分支；注释声明属实。
2. **缓冲区安全**：`trunc_buf[64KB]` 写入上界 = `prefix_len + marker_len ≤ budget ≤ 61440`，无溢出。
3. **marker 长度预算充裕**：`"..."(3) + "[LogX truncated, dropped "(25) + 10 位数字 + " bytes]"(7) = 45 ≤ kMarkerReserve(48)`，`snprintf` 返回值已做 63 字节钳制，与 `marker[64]` 匹配。
4. **计数口径正确**：`drop_too_large` 仅在 `kCannotFit` 递增；截断走 `truncated_logs`；二者互斥。
5. **锁纪律未破坏**：截断事件在 `mutex_lock` 之前发出，与既有的 "staging full" 路径同为锁外同步，P1-1 / J-04 契约保持。

### 2.3 审查发现

#### 【P1】`format_record_bounded` 对可空出参无条件解引用 — 文档与实现冲突

- **位置**: `core/src/format.cpp:213-214`
- **问题**: 函数开头即 `*out_truncated = false; *out_dropped = 0;`，未做空指针判断；
  但 `core/include/logx_internal.h:100-101` 明确写 `out_truncated ... `out_dropped` 为丢弃字节数,**可为 NULL**`。
- **影响**: 调用方若按文档传 `NULL`，将触发空指针解引用（崩溃）。
- **实际风险**: 当前唯一调用方 `logx_write`（`core/src/logx_c.cpp:437`）传非空，**暂无线上崩溃**；属契约缺陷 + 未来陷阱。
- **建议**: 二选一——(a) 函数内加 `if (out_truncated) *out_truncated = false;` 守卫；
  (b) 修订 `logx_internal.h` 注释，明确「必须非空」。

#### 【P2】新指标未接线到 Android 层，压力测试会掩盖数据丢失

- **位置**: `android/cpp/logx_jni.cpp:448-473`、`android/kotlin/LogX.kt:76-91`
- **问题**: C ABI 已新增 `truncated_logs`，但
  - JNI `nativeGetStats` 仍只返回 **5 元素**数组 `[dropNoSpace, dropTooLarge, truncatedBlocks, padRecords, blockWriteErrors]`；
  - Kotlin `Stats` data class 无 `truncated_logs` 对应字段。
  - 两处注释「与 `logx_stats_t` 非 reserved 字段**一一对应**」在本次提交后已**不成立**（新增了一个非 reserved 字段却未映射）。
- **影响**: 截断指标在 Android 端完全不可见。更实质的是 `android/logx-tester` 的
  `TesterViewModel.kt:219-226` 依据 `logx_write` **返回值**分类，截断现在返回 `LOGX_OK` →
  被计入 `writeOk`；`TesterViewModel.kt:362` 的「数据失真」告警仅看
  `dropNoSpace/dropTooLarge/invalidInput`，**不会触发**。等于压测中真实丢数据却报 100% 成功、
  吞吐结论仍被判定有效。
- **说明**: 提交信息「Kotlin/JNI/Python 解码器零改动」作为事实陈述正确（`sizeof` 未变、
  旧解码器不会读错），但代价是该能力未在 Android 侧暴露。
- **建议**: JNI 数组扩为 6 元素并同步 `LogX.kt` 的 `Stats` 与 `arr[...]` 解包；
  在 `TesterViewModel` 的失真判定中纳入 `truncated_logs`。

#### 【P2】错误回调 `msg` 生命周期契约未文档化，且本次由「静态存储期」变为「栈存储期」

- **位置**: `core/include/logx_c.h:50-53`、`core/src/logx_c.cpp:451-454`
- **问题**: `logx_error_fn` 的 `msg` 参数在头文件中**没有任何生命周期约定**。截断路径传入的是
  栈上 `char msg[64]`（`logx_c.cpp:451`），而旧路径的 `"log too large"` / `"staging full"`
  是字符串字面量（静态存储期）。回调若留存该指针，旧代码无害、新代码变成悬垂指针
  （use-after-free）。
- **实际风险**: 现有唯一消费方 `jni_error_trampoline`（`android/cpp/logx_jni.cpp:116`）经
  `make_jstring_utf16` **同步复制**为 Java String，安全；但对外 C ABI 契约已实质变化。
- **建议**: 在 `logx_c.h` 补一行「`msg` 仅在回调期间有效，回调返回后即失效，不得留存」，
  与本次改动配套。

#### 【P3】内部函数具外部链接却无声明

- **位置**: `core/src/format.cpp:176`（`kMarkerReserve`）、`core/src/format.cpp:185`（`solve_truncated_msg_budget`）
- **问题**: 二者位于 `namespace logx` 而非匿名命名空间，且未在任何头文件声明 →
  外部链接 + 无声明。同文件的 `tag_len_bounded` 本次已被正确收进匿名命名空间（`format.cpp:20-75`），风格不一致。
- **影响**: 触发 `-Wmissing-declarations` 类告警；未来有同名符号时存在 ODR / 链接冲突隐患。
- **建议**: 移入匿名命名空间，或加 `static`。

#### 【P3】头文件声明 `max_total_bytes == 0 表示不限制` 与实现不符

- **位置**: `core/include/logx_format.h:48-49` vs `core/src/logx_c.cpp:292-294`、`core/src/format.cpp:236-238`
- **问题**: 头文件声明 `0 = 不限制，但仍受 60KB 硬上限约束，超限同样按截断处理`。实际行为：
  - `logx_open` 直接拒绝 `max_log_length == 0`（`logx_c.cpp:292`）；
  - 即使直接调用 `format_record_bounded(..., max_total_bytes = 0, ...)`，一旦 `format_record`
    因 >60KB 失败，`solve_truncated_msg_budget(0, ...)` 会因 `0 < 27+tag+6` 返回 `false`
    → 返回 `kCannotFit` → **丢弃**，而非文档承诺的截断。
- **影响**: 无生产影响（该路径不可达），但属文档虚设契约，会误导后续直接使用该 API 的调用者。
- **建议**: 删除该句，或让 `max_total_bytes == 0` 分支显式处理 60KB 硬上限截断。

### 2.4 安全性与代码规范

| 维度 | 评估 |
|---|---|
| 内存安全 | 无越界/溢出；`memcpy` 长度均受 `budget ≤ 61440` 约束；已穷举验证无下溢 |
| 整数安全 | `solve_truncated_msg_budget` 全程 `uint64_t` 中间量，避免 `20*1024*s` 溢出；`payload_len − prefix_len` 已验证非负 |
| 并发安全 | `truncated_logs` 为 `std::atomic`，`fetch_add` 无锁；事件在锁外发出，未引入新锁序 |
| 资源占用 | ⚠️ 线程级 TLS 由 64KB 增至 128KB（`scratch` + `trunc_buf` 各 64KB，见 `format.cpp:119` 与 `242`）。多线程场景下每线程翻倍，建议关注（`trunc_buf` 可缩小至 61440 的紧致常量，或与 `scratch` 合并复用） |
| ABI 兼容 | `logx_stats_t` 尺寸不变，`reserved[0]` 转正符合 E-07 纪律；旧调用方按旧 `sizeof` 分配仍安全 |
| 代码规范 | 命名/注释密度与本库既有风格一致；中文注释质量高，契约说明详尽；仅 P3 项外部链接与文档不一致 |

### 2.5 验证方法与其局限

- **已完成**: 逐文件 diff 审读、跨 C/C++/JNI/Kotlin 调用链追踪、核心算法独立复刻穷举（643,215 组）。
- **未完成**: **本机未能实际编译与运行测试套件**。原因：环境安全策略禁用 `cmd.exe`
  （Bash 与 PowerShell 通道均拒绝），MSVC 的 `INCLUDE`/`LIB` 环境无法经
  `build-full/build_tmp.bat`（内部调用 `vcvars64.bat`）装配，`ninja` 因找不到
  `stddef.h`/`limits.h`/`atomic` 而失败。
- **因此**: 提交信息中「三矩阵 122/128/141 单测 + 4/4 压测全绿、Android arm64 交叉编译 0 错误」
  的声称**本次未能独立复现**。建议在可用的构建环境中重新跑一次，或将构建入口改为
  不依赖 `cmd.exe` 的形式（如在 shell 内显式导出 MSVC 环境变量）以便后续自动复核。

---

## 三、总体结论与建议

| 优先级 | 事项 | 位置 |
|---|---|---|
| P1 | 为 `out_truncated`/`out_dropped` 加空指针守卫，或修订「可为 NULL」注释 | `format.cpp:213` / `logx_internal.h:100` |
| P2 | JNI + Kotlin 接线 `truncated_logs`，并纳入 tester 失真判定 | `logx_jni.cpp:448` / `LogX.kt:80` / `TesterViewModel.kt:362` |
| P2 | 文档化 `logx_error_fn` 的 `msg` 生命周期 | `logx_c.h:50` |
| P3 | `kMarkerReserve`/`solve_truncated_msg_budget` 收入匿名命名空间 | `format.cpp:176,185` |
| P3 | 修正 `max_total_bytes == 0` 的虚设契约，或实现之 | `logx_format.h:48` |
| P3 | CHANGELOG 补 `9b1af14` 记录行 | `CHANGELOG.md` |

**总评**: `28acf7f` 是一次工程质量较高的提交——语义变更闭环、算法有明确上界且经独立验证、
ABI 兼容处理规范、测试改写与新增到位、文档同步充分。上述发现均**不阻塞合入**，
其中 P2 两项涉及可观测性与对外契约，建议在下一批次一并收口。
