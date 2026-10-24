# R47 代码审查报告 — 提交 677b70d（R46 12 项发现收口批次）

> **审查对象**: `677b70d` fix(core,jni,test,docs): R46 审查 12 项发现全量收口（P2-01~04 & P3-01~08）与报告历史基线勘误
> **父提交**: `4b1b16d`（R46 报告入库后的文档同步）
> **审查方法**: 逐 hunk 通读 + 全仓按模式检索 + MSVC 与 NDK 双端 clean 编译 + 四套件实跑 + **5 组变异实验** + 尺寸/历史基线探针
> **审查日期**: 2026-10-24
> **审查结论**: **可入库，无 P0/P1 阻断项**；12 项发现中 **11 项实质闭环、1 项（P2-03）门禁仍有未被覆盖的泄漏形态**，另新捕获 **2×P2 + 5×P3**（均为文档口径与断言强度类，非运行时缺陷）。

---

## 一、总体结论

1. **R46 的 4 项 P2 中 3 项（P2-01/02/04）实现成立且经变异实证**；P2-02 的"历史基线勘误"经 git 历史独立核对**属实**（非自我辩解）。
2. **P2-03 门禁强度不足**：新增的 5 类资源配对断言有效（删 `delete inst->staging` 必红，已实证），但"10k 轮进程内存增量 ≤ 8MB"阈值过宽——**删除 `delete inst` 后实测增量 6.02MB，断言仍通过**，而这正是 R46 P2-03 原文点名的失效形态。
3. **P3-01/02/03/04/05/06/07 全部成立**；P3-08 **仅在文档契约层面闭环**，新增的 `nativeClose` 主动清扫在"单实例递延关闭"这一主场景下对被关闭实例自身的槽位无效（代数水位尚未推进），代码注释"待销毁槽位"表述不成立。
4. **文档状态口径超出自身发布门禁**：计划文档 header 声称"已全量收口完成…仅待真机 P50"，但同一文档 §7.3 列出的发布前置条件还含 ASan/TSAN 复跑等未执行项。
5. **未发现新的 UAF / 死锁 / 漏认领 / 内存序回退**；`cb->pal` 原子化未改变 CB 布局（实测 `sizeof=64 / alignof=8 / off(inst)=24 / off(pal)=32`）。

### 提交清单

| 项 | 值 |
|---|---|
| SHA | `677b70d` |
| 父提交 | `4b1b16d` |
| 改动规模 | 14 文件 / +307 / −101 |
| 触及层 | core（`flusher.cpp`、`logx_c.cpp`、`logx_internal.h`、`logx_c.h`）、JNI（`jni_key_cache.h`、`logx_jni.cpp`）、tests（`test_accessor.h`、2 个用例文件）、docs（plan/decisions/CHANGELOG/R45·R46 报告） |
| 分支 | `master` |
| 工作区 | 审查前后 `git status --short` 均为空 |

---

## 二、R46 十二项发现逐项核实

| 编号 | R46 定级 | 声称处置 | 独立核实结论 |
|---|:--:|---|---|
| P2-01 | P2 | `append_block` 改走 `invoke_key_provider` | ✅ **成立**。语义等价（`!invoke_key_provider` ≡ `!kp \|\| !kp(kid,user)`）；**变异实证**：回退为裸调用后，新增断言 `max_in_callback_seen` 实测 `0 vs 1` 必红 |
| P2-02 | P2 | `logx_c.h` 补空句柄/墓碑句柄返回码契约 + 报告基线勘误 | ✅ **成立**。8 个公开 API 逐一比对源码，契约文案与实现完全一致；**历史基线独立核对成立**（见 §3.5） |
| P2-03 | P2 | `FastCountingPal` 五类资源净零 + 进程内存 ≤ 8MB | ⚠️ **部分成立**。资源配对断言有效；内存断言**未覆盖 Instance 级泄漏**（见新发现 **P2-N1**） |
| P2-04 | P2 | `test_inject_key_provider_quiesced` 先静止 Flusher 再注入 | ✅ **成立**。`flusher_exit`+`thread_join` 后才写非原子字段，重启 Flusher；20 轮重复无抖动；写点与读点在时间上不再重叠 |
| P3-01 | P3 | 补 `check_deferred` Flusher 分支 join 不变量注释 | ✅ **成立且推理正确**。`destroy_instance` 在 `delete inst`（`:368`）前必先 `thread_join(inst->flusher_thread)`（`:344`），不变量表述与实现一致 |
| P3-02 | P3 | `cb->pal` → `std::atomic<Pal*>` | ✅ **成立且完整**。全仓仅 3 处访问（`internal.h:128`、`logx_c.cpp:372/537`），全部改造，无遗漏；尺寸/偏移未变 |
| P3-03 | P3 | `on_destroy` 文案改为"Flusher 主循环已退出" | ✅ **成立**。新文案与"执行线程可能是正在退出途中的 Flusher 线程"已不矛盾 |
| P3-04 | P3 | 计划文档删除已删字段 | ✅ **成立**（本批次前已闭环，本次复核无回退；代码层 0 处残留） |
| P3-05 | P3 | R45 ACT 勾选 + CHANGELOG 登记 | ✅ **成立**。ACT `[x]` **8/8**、`[ ]` 0；CHANGELOG 第 24 条已改写并补提交表行 `1bc549a~4b1b16d` |
| P3-06 | P3 | 64B 断言文案如实化 | ✅ **成立（按"修正文案"方案）**。断言改为 size 上界表述；实测仍 `alignof=8`，即"独占缓存行"的原始性能意图已放弃并被如实记录，属可接受取舍 |
| P3-07 | P3 | `*out_closed_immediately` 移入 `if (target != nullptr)` | ✅ **成立**。入参在 `:524` 已初始化 `false`，迁移后无未初始化读；与 `:582-588` 兜底分支完全对称 |
| P3-08 | P3 | 新增 `scrub_by_watermark` + `nativeClose`/`nativeOpen` 失败路径主动清扫 | ⚠️ **部分成立（契约 ① 成立、机制 ② 在主场景无效）**，见新发现 **P3-N1** |

**统计**：完全成立 **10** 项、部分成立 **2** 项（P2-03 / P3-08）、未完成 **0** 项。

---

## 三、独立验证（实证）

### 3.1 编译

| 目标 | 环境 | 结果 |
|---|---|---|
| MSVC（`build-full`，Ninja） | MSVC 14.43.34808 + SDK 10.0.22621.0 | `ninja -t clean` 后全量 **55 步全绿，warning 计数 = 0** |
| Android（`build-android-tests`，arm64-v8a） | NDK 26.1.10909125 | `ninja -t clean` 后 **61/61 步全绿，warning 计数 = 0**（53 编译 + 8 链接） |

### 3.2 实跑四套件（均在**工作区外**的临时目录运行，规避已知 `CreateDirectoryW=5` 假红）

| 套件 | 结果 | 备注 |
|---|---|---|
| `logx_unit_tests` | **203/203 通过**（25.4s，另两次 26.9s / 39.8s 均全绿） | 含两个被改造用例 |
| `logx_hotpath_alloc_tests` | **2/2 通过** | |
| `logx_stress_tests` | **4/4 通过** | 50 线程重压 + 3 项崩溃恢复 |
| `logx_crash_fork_tests` | **3/3 通过** | 50 轮 fork 强杀对账 |
| 合计 | **212 项全通过**，与文档"203 单测 + 2 热路径 + 4 压测 + 3 崩溃恢复"逐项吻合 | |

并发用例稳定性：`ControlBlockLifetime.FlusherBlockedInKeyProviderConcurrentCloseNoDeadlock` **重复 20 轮 20/20 通过**。

### 3.3 变异实验（判定"断言是否真的绑定行为"）

| # | 变异 | 预期 | 实测 | 判定 |
|---|---|---|---|---|
| M1 | 注释 `delete inst->staging` | 必红 | **红**：`fd 8189/0`、`mmap 8189/0`、内存增量 `252,801,024 > 8,388,608` | ✅ 资源+内存断言均生效 |
| M2 | `append_block` 回退为裸 `key_provider` 调用 | 必红 | **红**：`max_in_callback_seen 0 vs 1`（`:3142`） | ✅ P2-01 用例强绑定 |
| M3 | 仅把内存阈值 8MB 改为 0（不改逻辑） | 打印实测值 | 正常路径增量 = **1,118,208 B ≈ 1.07 MiB** | 阈值余量 7.4× |
| M4 | 注释 `delete inst`（R46 原文点名的失效形态） | 必红 | **绿（通过）** | ❌ **未被捕获 → P2-N1** |
| M5 | M4 + 阈值归零以读实测值 | — | 增量 = **6,316,032 B ≈ 6.02 MiB < 8 MiB** | 泄漏被阈值吞掉 |

M4/M5 是本次审查最关键的一条：`sizeof(Instance)=352B` + 堆字符串 ≈ 628B/轮 × 10050 轮 ≈ 6.0MB，恰好落在 8MB 门禁之内。

### 3.4 布局探针（MSVC，`/std:c++17 /utf-8 /MD`）

```
sizeof(InstanceControlBlock)=64  alignof=8  offsetof(inst)=24  offsetof(pal)=32
sizeof(Instance)=352
```

`std::atomic<Pal*>` 与 `Pal*` 同尺寸，CB 仍为 **64B（恰好等于断言上限，零余量）**，与官方声称一致。

### 3.5 历史基线核对（P2-02 勘误是否成立）

| 版本 | `logx_set_error_handler(nullptr, …)` | 证据 |
|---|---|---|
| `431d5ee~1`（重构前） | `LOGX_ERR_STATE` | `git show 431d5ee~1:core/src/logx_c.cpp:723` |
| `431d5ee`（重构） | `LOGX_ERR_INVALID_ARG` | `:754` |
| `0ef345f` | `LOGX_ERR_STATE`（恢复） | `:775` |

→ R46 原报告把 `431d5ee` 当基线、将 P2-02 定性为"公开行为变更"，**确属误判**；本提交的勘误方向与事实一致。同时逐一核对 `logx_open/close/close_ex/write/flush/export/set_error_handler/get_stats` 的空句柄与墓碑句柄实际返回码，与新写入 `logx_c.h:149-153` 的契约**逐条相符**。

### 3.6 全仓按模式检索

- `cb->pal` 访问点：**3 处，全部原子化**，无普通读写残留。
- 裸 `key_provider(` 调用：仅剩 `logx_open:428` 的 open 期探测（实例尚未创建，合规）与 `invoke_key_provider` 内部。
- `single 64-byte cache line` / `cb->flusher_thread`：代码层 **0 处**，仅存于历史报告与勘误说明（属预期）。

---

## 四、新发现问题

### P2-N1（门禁强度不足，P2-03 未完全闭环）8MB 阈值放过了 Instance 级泄漏

- **位置**: `tests/unit/test_r19_regression.cpp:3071-3077`（阈值）、`:3060-3064`（断言说明注释）
- **问题**: R46 P2-03 的核心反例是"删掉 `delete inst` 也照样通过"。本次实测：正常路径增量 **1.07MB**，删 `delete inst` 后增量 **6.02MB**，仍 **< 8MB** → 用例**通过**。5 类资源计数也无法覆盖（`mutex/event/thread` 在 `delete inst` 之前已释放，计数平衡）。
- **影响**: 门禁仍无法拦截"每轮泄漏一个 `Instance`（352B 结构体 + 堆字符串）"这一类最常见的回归；8MB 相当于允许 ~800B/轮 的泄漏。
- **建议**: ① 阈值收紧到 **≤ 3MB**（正常 1.07MB，仍留 ~2.8× 余量；Android 若 RSS 抖动大可分支给 4MB）；或 ② 更彻底：在 `test_accessor`/`Instance` 上加分配计数（`new`/`delete` 净零），与进程内存断言并用。建议同时把注释改为"注释掉整个 `destroy_instance` 必红；仅删 `delete inst` 由计数断言兜底"。

### P2-N2（状态口径超越自身发布门禁）"已全量收口完成"与 §7.3 前置条件冲突

- **位置**: `docs/lifecycle_control_block_plan.md:5`（header 状态）vs `:454`（发布判定前置条件）；`CHANGELOG.md:717`（"…全绿通过"）
- **问题**: header 写"代码与双端编译已全量收口完成…**仅待**真机复跑 P50 ≤ 4.2µs 确认后恢复正式发布状态"，但同文档 §7.3 明列发布前置条件为"4×P2 闭环 + NDK clean + **ASan/TSAN 复跑** + 真机四套件与 P50 重测"。本次审查确认 ASan/TSAN **未执行**（提交信息亦未声称），故"仅待 P50"的收尾口径不成立。
- **影响**: 与 R45→R46 两轮反复纠正的"表述偏乐观"属同一类问题；若据此宣布专项收口，会把动态检测这一未执行门禁静默跳过。
- **建议**: header 改为"代码与双端编译收口完成；**发布门禁余 ASan/TSAN + 真机四套件 + P50**"；或把 §7.3 已具备/未具备项分成两栏勾选。

### P3-N1（机制与注释不符）`nativeClose` 的水位清扫对"被关闭实例自身"无效

- **位置**: `android/cpp/logx_jni.cpp:473-475`、`android/cpp/jni_key_cache.h:92-98`
- **问题**: `scrub_by_watermark` 清的是 `gen <= watermark` 的槽位，而 watermark 只在 `jni_destroy_trampoline` 真正销毁时推进。递延关闭时 `logx_close` 立即返回、销毁尚未发生 → 被关闭实例的 `gen > watermark` → **调用线程里该实例的明文密钥不会被清**。仅当"更晚的实例已被销毁"（多实例乱序）时才碰巧清到。故 `jni_key_cache.h:93` 的"已销毁或**待销毁**槽位"表述不成立。
- **影响**: 单实例（最常见）递延关闭场景下，本项新增清扫的净收益接近 0；实际保障来自 `logx_c.h:129-131` 已如实写明的"惰性回收、空闲线程可能长期驻留"契约（该契约本身是合格的闭环方式）。
- **建议**: 修正注释为"仅清扫水位以内的**旧代数**槽位；被关闭实例自身的槽位需待销毁推进水位后由下次探测清除"；若确需即时清除，应在 core 侧把 `gen` 暴露给 close 路径（如 `logx_close_ex` 增加出参或新增内部查询），由 JNI 按 `(owner, gen)` 精确 `invalidate_owner`。

### P3-N2（报告内部口径并存）R46 报告正文未随收口台账同步

- **位置**: `docs/reviews/code_review_R46_2026-10-24.md:7`、`:14-17`、`:34-49` vs `:210-217`
- **问题**: §六台账已把 12 项全部标 ✅，但 header 仍写"仍需 4 项 P2 + 8 项 P3 收口"，§二逐项表仍写"⚠️ 部分成立…见 P2-01"。同一文件内"未闭环"与"已闭环"并存。
- **建议**: 在 header 与 §二加一行"〔R47 收口状态：见 §六 台账〕"，或把 §六标题明确为"收口台账（`677b70d` 后状态）"以划清快照边界。

### P3-N3（文档小节状态不一致）§7.3 仍为"建议顺序"

- **位置**: `docs/lifecycle_control_block_plan.md:450-452`
- **问题**: §7.1/§7.2 已全部 ✅，§7.3 标题仍为"收口顺序建议（沿用 R46 §六）"，读起来像是待执行清单。
- **建议**: 改为"§7.3 收口顺序（已按此顺序执行完毕）"。

### P3-N4（内存序配对不对称）`logx_open` 对 `cb->pal` 用 `relaxed` 写，读侧统一 `acquire`

- **位置**: `core/src/logx_c.cpp:469`（`store(inst->pal, relaxed)`）vs `:333`、`:537`、`logx_internal.h:128`（`load(acquire)`）
- **问题**: 非缺陷（句柄必经受同步通道发布，实践中可见性成立），但与本轮"消除形式竞争"的目标不一致：读侧声明了 `acquire` 却无 `release` 与之配对，后续维护者难以判断意图。销毁路径已用 `release`，开路径却用 `relaxed`。
- **建议**: 改为 `std::memory_order_release`（零成本，语义自洽）。

### P3-N5（既有问题，本次沿用）`append_block` 忽略 `kid` 入参

- **位置**: `core/src/flusher.cpp:1125`（`(void)kid;`）与 `:1154`（`invoke_key_provider(inst, inst->kid)`）
- **问题**: 函数签名接受 `kid` 却始终用 `inst->kid` 探测。当前仅测试可达，且测试传 `kid=1` 与 `inst->kid` 相同，未暴露；若未来生产化，按调用方 `kid` 加分卷/多 kid 的场景会以错误 kid 探测并误判降级。
- **建议**: 或使用入参 `kid`（与 `write_and_record` 的 `kid` 语义一致），或在注释中明确"`kid` 入参保留未用，语义以 `inst->kid` 为准"并删除参数。

### 观察项（不计级）

1. `sizeof(InstanceControlBlock)==64` **恰好等于断言上限**，零余量：任何字段新增都会立即破坏编译（设计使然，但升级 ABI 时需注意）。
2. `scrub_by_watermark` 使用**全局水位**（非按实例）：多实例并存时可能驱逐仍存活实例的缓存槽，表现为一次 key_provider 重取（性能级，无正确性影响）。
3. `nativeOpen` 失败分支把全局水位推进到"从未成功创建的代数"，范围略宽于必要，同样只造成存活实例缓存被驱逐（性能级）。
4. `append_block` 内 `emit_event` 早已具备"回调退出点可能就地销毁实例"的性质，本次补齐门禁后新增的 `invoke_key_provider` 退出点与之同构——**非本次引入的新风险面**（append_block 仍不持 `OpGuard`，若生产化需整体加保护）。

---

## 五、验证局限（必读）

1. **动态检测仍未执行**：未跑 ASan/TSAN/Valgrind。并发正确性依据为静态全序推演 + 20 轮重复 + 压力/崩溃套件，不等于证明（与 R46 相同）。
2. **变异实验共 5 组，均在审查期间临时改动并已 `git checkout` 还原**；审查结束时 `git status --short` 为空、HEAD 仍为 `677b70d`，产物已重新按 HEAD 构建。
3. **测试 exe 从工作区外运行**（`%TEMP%\logx_r47`）。工作区内运行会出现 2 个已知假红（`CreateDirectoryW=5`），该项为本机镜像路径权限限制，不计为缺陷。
4. **性能门禁未复跑**：`P50 ≤ 4.2µs` 与 AAR 体积门禁未测；`logx_set_error_handler`/`get_stats` 的额外 `active_ops` 原子加减对热路径的影响本次未量化。
5. **Android 真机未验证**：NDK 仅 clean 交叉编译通过（arm64-v8a，0 警告），JNI 行为由桌面 TU 单测覆盖。
6. **`append_block` 可达性**结论依据全仓检索（`core/` + `tests/`），若存在仓库外宿主直接链接该符号，相关风险等级需上调。

---

## 六、结论与下一步

**结论**：`677b70d` 在 R46 的 12 项上**方向正确、10 项完全成立、2 项部分成立**，无 P0/P1，可入库。核心代码改动（`cb->pal` 原子化、`append_block` 门禁、测试静止态注入、`out_closed_immediately` 对称化）均经编译 + 实跑 + 变异验证。

**建议收口顺序**：

| 顺序 | 项 | 理由 | 工作量 |
|:--:|---|---|:--:|
| 1 | **P2-N1** 阈值收紧至 ≤3MB 或加 `Instance` 分配计数 | 否则 P2-03 门禁对最常见泄漏形态仍然无效（本次唯一实质缺陷） | 小 |
| 2 | **P2-N2** 状态口径拆分为"已具备/未具备" | 避免把 ASan/TSAN 门禁静默跳过 | 小 |
| 3 | **P3-N1** 修正 `nativeClose` 清扫注释 | 注释与机制不符，易误导后续维护 | 极小 |
| 4 | **P3-N2/N3** 报告与计划小节口径统一 | 消除同一文档内"未闭环/已闭环"并存 | 极小 |
| 5 | **P3-N4/N5** `release` 写 + `kid` 入参语义 | 语义自洽 | 极小 |

**待办提醒**：`677b70d` 自身的 CHANGELOG 提交表行尚未登记（与既有惯例一致，留待下批补记）。
