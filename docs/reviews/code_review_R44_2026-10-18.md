# LogX 方案评审报告 R44（T5.8 生命周期堆控制块实施计划 v1.0）

- **评审日期**: 2026-10-18
- **评审对象**: [`docs/lifecycle_control_block_plan.md`](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md) v1.0（2026-10-18，`e60d44a` 入库，333 行）
- **代码基线**: `e60d44a`
- **评审方式**: 计划逐节核对 + 现有 close 五路径（外部主干 / 内部双检 / OpGuard / emit_event / flusher_main）全时序推演 + 测试内部依赖盘点 + `build-full` 测试清单核对
- **评审性质**: 方案评审（未实施）。P0/P1/P2 表示“按现稿实施会造成的后果等级”，不是已入库代码缺陷。

---

## 一、总体结论

**方向正确，但 v1.0 不能直接进入实施。** 把生命周期元数据从 `Instance` 拆到独立控制块、把销毁收敛到单一归零点、用 RAII 统一入口，能够实质降低 R41~R43 期间的 4 态仲裁维护成本；`OpGuard` 也能顺手修掉现有 `logx_flush`/`logx_export` 的“先查 closing 后 fetch_add”窗口，以及 `logx_write` 先读 `staging` 再增计数的顺序问题。这些收益成立。

但现稿存在 3 个 P0 阻断项：

1. **R44-01：尾读 UAF 只是从 `Instance` 搬到了 Control Block。** 弱引用保护不了“调用方直接传入的裸句柄在第一次解引用时”的窗口；R41 并发写/close 与并发双 close 都是现实触发面。
2. **R44-02：close 的 flush/stop 顺序会让在途写丢数据。** 计划在 `flush_blocking` 之前置 `flusher_exit`，且 `dispose_instance` 完全不 flush；递延路径下 Flusher 最终 drain 之后提交的数据无人落盘。
3. **R44-03：引用协议/启动引用/快速路径判定不闭合。** `acquire_strong` 的 CAS 成功序、缺少二次 closing 检查、初始化 `strong=2/weak=2` 对 Flusher 启动失败的假设、`strong==2` 快速路径与 `out_closed_immediately` 的推导都需要重写。

另有 4 个 P1：`EventGuard` 会吞关闭期事件且是 R43 用例不死的隐式依赖；Flusher 自毁泄漏线程句柄；C++ 句柄类型/API/测试迁移量被低估；`logx_open` 失败路径完全缺失。6 个 P2 主要是 TID 复用、on_destroy 参数语义、性能门禁、测试基线与文档结构。

### 1.1 级别汇总

| 级别 | 数量 | 含义 |
|---|---:|---|
| P0 阻断 | 3 | 按现稿实施会违背计划目标（UAF 未根治 / 数据丢失 / 协议不闭合），必须在动工前修订方案 |
| P1 重要 | 4 | 会造成事件丢失、资源泄漏或大范围迁移失败，需在 v1.1 补齐 |
| P2 打磨 | 6 | 性能/可验证性/文档/验收口径问题，可与实施并行 |
| 已排除疑点 | 6 | 经核对不成立或已被计划正确覆盖的担忧，见 §五 |

### 1.2 值得保留的部分

- CB 独立于 `Instance`、单一 `dispose_instance` 归零归属，是正确的结构方向；即使最终选择墓碑模型，CB 仍有维护价值。
- `OpGuard` 统一“获取引用 → 使用 → 释放”后，`logx_flush`/`logx_export` 现有 check-then-increment 窗口与 `logx_write` 的 `staging` 先读问题都能被自然消除（见 [logx_c.cpp:538](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L538)、[logx_c.cpp:697](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L697)）。
- 热路径不需要新增堆分配；CB 只在 open 时分配一次，`OpGuard` 为栈对象。
- JNI/Kotlin 以 `jlong` 透传句柄，运行时无需改动（见 §五 E-1）；C ABI 函数签名与二进制布局不变（见 §五 E-2）。

---

## 二、P0 阻断项

### R44-01（P0，生命周期）尾读 UAF 未根除：裸句柄入口没有引用保护

**位置**

- 计划 §2.1 弱引用拓扑：[lifecycle_control_block_plan.md:53](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md#L53)
- `acquire_strong`：[lifecycle_control_block_plan.md:92](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md#L92)
- 计划 §4 “彻底根除”结论：[lifecycle_control_block_plan.md:296](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md#L296)
- 现有契约：[logx_c.h:11](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_c.h#L11)
- R41 并发写/close 用例：[test_r19_regression.cpp:628](file:///d:/Project/Android_Project/LogX/logx/tests/unit/test_r19_regression.cpp#L628)

**现象**

`weak_count` 只能保护“已经持有弱/强引用的路径”。`logx_instance_t*` 是调用方直接传入的裸指针；`acquire_strong()` 在第一次读 `cb->is_closing` / `cb->strong_count` 时还没有任何引用。按计划的拓扑：

1. close 释放主句柄的组弱引用；
2. Flusher 退出时释放自己的弱引用；
3. `weak_count == 0`，`delete cb` 在 close 内发生；
4. 一个并发进入的 `logx_write` / `logx_close` 读到的就是已释放内存。

CB 的“存活期超越 Instance”只对已经拿到引用的人成立，对裸句柄入口不成立。这是把 R41-05 的尾读从 `Instance` 平移到 CB，而不是根除。

**触发链 / 现实可达性**

- R41 用例的写线程在 `logx_close_ex` 返回后才看到 `stop_writers`（[test_r19_regression.cpp:662](file:///d:/Project/Android_Project/LogX/logx/tests/unit/test_r19_regression.cpp#L662)），仍可能有一次 `logx_write` 在 close 返回后进入；这正是契约允许的“活跃调用栈内 close → 递延销毁”场景。若 CB 已释放，写线程读 `is_closing` 即 UAF。
- 两个线程并发 `logx_close`：胜者释放 CB 后，败者在 `is_closing.exchange` 处读已释放内存；计划中“幂等返回”的写法并不安全。
- 一次 close 与一个刚进入 `acquire_strong` 的写线程交错：写线程读 `is_closing=false` 后被抢占，close 完成 flush/join/`release_strong` 并删除 CB，写线程恢复后读 `strong_count` 即 UAF。

**后果**

计划宣称“根治 R41-05”，但 ASan 下仍可抓到 heap-use-after-free。普通构建可能因 allocator 保留内存而假绿，不能作为有效证据。

**方案对比**

| 方案 | 做法 | 优点 | 代价 |
|---|---|---|---|
| A. 收窄契约 | 明确 close 不得与同句柄其他 API 并发；由调用方外部同步 | 最简单；Kotlin 层已通过 `executeOp`/`activeQuickOps` 做到（[LogX.kt:172](file:///d:/Project/Android_Project/LogX/logx/android/kotlin/LogX.kt#L172)、[LogX.kt:579](file:///d:/Project/Android_Project/LogX/logx/android/kotlin/LogX.kt#L579)） | 破坏现有 C ABI 契约与 R41 用例；必须改文档和测试；治标不治本 |
| B. 句柄注册表/epoch | 全局表按句柄查找，close 等读者退出后再释放 CB | 可安全释放，语义完整 | 热路径增加查找/epoch 开销；实现复杂 |
| C. CB 墓碑（永不释放） | close 后保留 CB，只销毁 `Instance`；陈旧句柄永远返回 `STATE` | 裸句柄入口天然安全；热路径零额外开销；顺带解决地址复用 ABA；可删除 `weak_count` 整套机制 | 每次 open/close 泄漏一个很小 CB（约几十字节）；需明确接受 |
| D. 代际槽表 | 固定槽位 + generation，close 递增代际，槽位复用 | 安全且有界 | 实现量最大；容量上限与回收策略要设计 |

**建议**

- 若坚持“零契约变更”（计划 §1.2 的目标），选 **C（墓碑）**。Android 使用形态基本是进程级单实例，CB 只有几十字节；单测 6 轮 open/close 的累计开销可忽略。墓碑模型下 `weak_count` 可以整体删除，只剩 `strong_count + is_closing + inst`，计划反而更简单。
- 若后续确实有高频 open/close 场景，再升级为 **D（代际槽表）**。
- 只有明确愿意修改 C ABI 契约、并重写 R41 语义时才选 **A**。

**需补测试**

- ASan 下“并发 close + 持续 write/flush/export/stats/set_error_handler”压力用例，覆盖 close 返回后仍有一次进入的形态。
- 并发双 close（两个线程同时调用同一句柄）。
- 陈旧句柄 close/write 在墓碑模型下返回 `STATE` 且不崩溃。
- 10k 轮 open/close 的 CB 计数/内存增长上界（若选墓碑）。

**关联项**

R44-03（引用协议）与 R44-06（句柄类型）都以本项决策为前提。

---

### R44-02（P0，持久化）close 的 flush/stop 顺序与递延路径会丢数据

**位置**

- 计划 close 顺序：[lifecycle_control_block_plan.md:218](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md#L218)（`is_closing`）→ [plan:225](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md#L225)（`flusher_exit`）→ [plan:237](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md#L237)（`flush_blocking`）
- 计划 dispose 不 flush：[lifecycle_control_block_plan.md:259](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md#L259)
- 现有实现的相反顺序：[logx_c.cpp:306](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L306)（先 `flush_blocking`，后 `flusher_exit`）
- Flusher 退出只做一次最终 drain：[flusher.cpp:1027](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L1027)
- `flush_blocking` 依赖 Flusher 存活：[flusher.cpp:1307](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L1307)

**现象**

现稿在 close 一开始就置 `flusher_exit=true` 并唤醒。Flusher 在 [flusher.cpp:1034](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L1034) 退出循环，只做一次 `drain_once` / `durability_sync` / `close_current`，随后 `flusher_running=false`。此时：

- 一个 close 前已经越过入参检查的在途写线程（契约明确允许）可能在最终 drain **之后**才提交 staging；
- 最后一个强引用释放时，计划中的 `dispose_instance` 完全不做 flush；
- 这份数据永久丢失。

此外，`flush_blocking` 一旦看到 `flusher_running=false` 就直接返回 false（[flusher.cpp:1317](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L1317)），计划忽略了返回值；close 仍会 join 并销毁，可能把未落盘数据一起删掉。

**快速路径的额外窗口**

[plan:234](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md#L234) 的 `strong_count == 2` 检查与实际进入者不是原子的：

- 检查后仍可能有写线程提升引用并写入；
- 此时 close 的 `release_strong` 不是最后一个，但 [plan:245](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md#L245) 已无条件置 `out_closed_immediately=true`；
- 最后一个进入者释放时可能在没有 Flusher 的情况下 dispose，数据同样丢失。

**后果**

- `close` 返回后“已越过检查的在途写”可能丢失，违反现有递延销毁语义；
- `out_closed_immediately` 可能误报，Kotlin/JNI 侧若据此释放上下文（当前 JNI 未使用该参数，但 C ABI 契约允许）会出问题；
- `flush_blocking` 的失败被静默吞掉，用户无法从 close 返回值感知。

**方案对比**

| 方案 | 做法 | 优点 | 代价 |
|---|---|---|---|
| A. 纯引用计数 quiescence | 递延路径不置 `flusher_exit`；Flusher 观察到“只剩自己一个强引用”后做最终 drain 再退出；同步路径由 close 驱动 flush+join | 不增加计数器；与计划的 refcount 模型一致 | 同步快速路径仍需额外握手；瞬时进入者的判定很难只靠两个独立原子严谨证明 |
| B. 强引用 + 单一 `active_ops` 计数 | 保留一个仅用于关停 quiescence 的 in-flight 计数；close 在 `ops==0 && in_callback==0` 才同步，否则递延；Flusher 等 `ops==0` 后最终 drain | 判定精确、可验证；仍可删除 `close_pending/close_on_flusher` 与 Dekker 双检 | 与“废弃 active_ops”的口号相反；热路径多一对加减（与现有成本同量级） |
| C. 保持现稿顺序，在 dispose 补 flush | 由最后一个释放者尝试 flush | 改动小 | Flusher 已退出时无法 flush；只能等待超时，仍丢数据 |

**建议**

- 选 **B**。保留 `active_ops` 不等于回到旧的 4 态纠缠：它只承担“是否还有在途操作”的精确计数，`close_pending/close_on_flusher` 和两处 Dekker 双检仍然删除。这样关停协议从“猜引用拓扑”变成“精确 quiescence”。
- 若坚持纯引用计数，必须选 **A**，并明确同步快速路径的额外握手；同时把 `out_closed_immediately` 改为由 `release_strong` 的返回值（是否真正归零并 dispose）推导，绝不能按分支直接置 true。
- 无论选哪种，`dispose_instance` 要么保证 Flusher 已完成最终 drain，要么在无法完成时保留失败事件并保证数据不被静默删除。

**需补测试**

- close 与持续 write 并发，close 返回后写入线程立即停止，重启实例并导出，断言 close 前的所有成功写入均可恢复。
- Flusher 正阻塞在 `key_provider` / `emit_event` 回调时，另一线程 close，不丢数据、不死锁。
- close 期间 Flusher 最终 drain 发生 IO 错误，事件必须到达用户回调（与 R44-04 联动）。
- `out_closed_immediately` 与 `on_destroy` 的时序矩阵：true 时 on_destroy 已执行；false 时由后续线程执行且恰好一次。

---

### R44-03（P0，并发协议）acquire/release 内存序、启动引用与快速路径判定不闭合

**位置**

- `acquire_strong`：[lifecycle_control_block_plan.md:92](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md#L92)
- 初始引用计数：[lifecycle_control_block_plan.md:78](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md#L78)
- Flusher 引用“自然持有”：[lifecycle_control_block_plan.md:143](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md#L143)
- 快速路径 `strong==2`：[lifecycle_control_block_plan.md:234](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md#L234)
- `thread_start`：[logx_c.cpp:472](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L472)

**3.1 CAS 成功序与二次检查**

现稿 CAS 成功序是 `memory_order_acquire`，但这是一次 RMW 递增，至少应为 `acq_rel`（或 release），否则不能把本次递增发布给并发的归零线程。循环中的 `is_closing.load(relaxed)` 也缺少获取语义。更关键的是 CAS 成功后没有再次检查 `is_closing`：close 置位后仍可能“成功”拿到引用并继续使用实例，破坏 close 的线性化语义。

**3.2 启动时的引用归属不成立**

计划直接初始化 `strong=2 / weak=2`，并称 Flusher 启动即自然持有。但 `thread_start` 在 [logx_c.cpp:472](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L472) 才调用，在此之前的所有失败路径（mutex/event 创建、recovery、thread_start 失败）都还没有 Flusher 引用。正确做法：

- CB 初始化为 `strong=1 / weak=1`（只代表主句柄）；
- `thread_start` 之前由父线程显式获取 Flusher 的强/弱引用，线程启动后接管；
- 任意 open 失败路径按同一套 `release_strong/release_weak` 回收；
- 不要在构造函数里假设 Flusher 一定存在。

计划 §3.1 只写了成功路径，**open 失败路径完全缺失**（拆分到 R44-07）。

**3.3 快速路径 hardcode 与 out flag**

`strong_count == 2` 把“主句柄 + Flusher loop”写死。以下任一变化都会让它失真：

- 保留 `EventGuard` 时，回调期间强引用是 3；
- 将来增加 reaper/回调引用/诊断引用；
- 瞬时进入者在检查后增加引用。

应使用具名常量并写清拓扑，或直接使用 R44-02 的精确 quiescence 计数。`out_closed_immediately` 必须由 `release_strong` 的结果推导。

**3.4 release 顺序未定义**

计划未给出 `release_strong` 的完整实现。需要明确：

- `fetch_sub` 的序（建议 `acq_rel`，或 release + acquire fence）；
- `prev==1` 时先 `dispose_instance`，再释放“强引用组”的弱引用，保证 dispose 期间 CB 存活；
- Flusher 自身弱引用的释放时机；
- 若 `dispose_instance` 在 Flusher 线程执行，先交线程句柄（R44-05），再释放最后一个弱引用。

**建议**

- 采用 R44-02 方案 B 的精确 quiescence 计数，同时保留强引用负责 `Instance` 生命周期；两套计数职责分离。
- `acquire_strong`：初次检查 acquire → CAS 成功 acq_rel → 成功后再次 acquire 检查 `is_closing`，为 true 则 release 并失败。
- 启动引用按 3.2 改为 1/1 + 显式获取 Flusher 引用。
- `release_strong` / `release_weak` 的完整顺序写进 §2.2，并配不变量。

---

## 三、P1 重要项

### R44-04（P1，事件语义）`EventGuard` 会吞掉关闭期事件；移除它又会触发 R43 死锁

**位置**

- EventGuard：[lifecycle_control_block_plan.md:186](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md#L186)
- 关闭期拒绝：[lifecycle_control_block_plan.md:191](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md#L191)
- 当前 emit_event 不按 closing 丢弃：[logx_internal.h:111](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_internal.h#L111)
- R43 用例：[test_r19_regression.cpp:774](file:///d:/Project/Android_Project/LogX/logx/tests/unit/test_r19_regression.cpp#L774)

**现象**

- `is_closing` 为 true 时 EventGuard 直接返回，close 期间最终 drain 的 IO/恢复错误会被丢弃；当前实现不会因 `closing=true` 丢事件，这是行为回退。
- 结构上，`emit_event` 第一行就读取 `inst->cb`，然后才 `acquire_strong`。如果调用者没有外层引用、`inst` 已失效，`inst->cb` 本身已经 UAF，EventGuard 保护不了它。而所有正常调用点（write/flush/export 的 OpGuard、Flusher loop、open 阶段主引用）本来就持有引用，所以 EventGuard 是冗余的。
- 但 R43 用例中 Flusher 阻塞在用户回调里，close 在主线程调用；正是 EventGuard 多出的 1 个强引用把 `strong_count` 从 2 变成 3，使计划走递延分支。若直接删除 EventGuard，close 会看到 `strong==2` 走快速路径，`flush_blocking` 等 Flusher drain（但 Flusher 正阻塞在回调里），随后 `thread_join` 等回调返回；测试要等 close 返回后才放行回调，直接死锁。

**建议**

- 删除 EventGuard，由外层引用保证生命周期；增加 `flusher_in_user_callback`/`flusher_busy` 原子标志，在 Flusher 调用 `emit_event`、`key_provider` 等任何用户回调前后置位/清位；同步快速路径在 busy 为 true 时必须递延。
- 若保留 EventGuard，至少允许 `is_closing` 期间继续发事件，并显式加入 busy 检查，不能依赖“多一个强引用”的副作用来规避死锁。

**需补测试**

- close 期间最终 drain 的 IO 错误事件必须到达用户回调。
- Flusher 回调内 close（R43）与“回调进行中由另一线程 close”两种形态。
- 删除 EventGuard 后运行 R43，确认不死锁且 `on_destroy` 执行。

---

### R44-05（P1，资源泄漏）递延路径中 Flusher 自毁会泄漏线程句柄

**位置**

- 计划 dispose 在 Flusher 线程跳过 join：[lifecycle_control_block_plan.md:265](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md#L265)
- PAL 仅有 start/join：[logx_pal.h](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_pal.h)
- CountingPal 的 thread_destroy 只在 join 成功时递增：[test_observability.cpp:327](file:///d:/Project/Android_Project/LogX/logx/tests/unit/test_observability.cpp#L327)
- 配对断言：[test_observability.cpp:599](file:///d:/Project/Android_Project/LogX/logx/tests/unit/test_observability.cpp#L599)

**现象**

强引用纯归零模型中，递延 close 后 Flusher 往往是最后一个强引用持有者：

- close 已释放主引用，外部 op 全部退栈后，Flusher loop 是最后一个；
- Flusher 退出时 `release_strong(is_flusher_thread=true)` → `dispose_instance` 跳过 join；
- 线程返回后句柄无人回收；
- `CountingPal` 的 `thread_create/thread_destroy` 在 R43 形态下不配对；现有用例只覆盖正常同步 close，覆盖不到该路径。

**建议**

- 给 PAL 增加 `thread_detach`（Win32 自线程 `CloseHandle` / POSIX `pthread_detach`），或引入轻量 reaper。
- 无论选哪种，CB 应保存线程句柄，确保 Flusher 自毁时能释放；不能在 `Instance` 删除后丢失句柄。
- 新增“Flusher 回调内 close / 递延 close”的线程句柄配对断言。

---

### R44-06（P1，迁移）C++ 句柄类型、未受保护 API 与测试迁移量被低估

**位置**

- C++ typedef：[logx_c.h:134](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_c.h#L134)
- `logx_set_error_handler` 无生命周期守卫：[logx_c.cpp:721](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L721)
- `logx_get_stats` 无生命周期守卫：[logx_c.cpp:733](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L733)
- `logx_write` 的 closing 检查：[logx_c.cpp:566](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L566) 至 [logx_c.cpp:688](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L688)
- 测试内直接转换示例：[test_robustness.cpp:214](file:///d:/Project/Android_Project/LogX/logx/tests/unit/test_robustness.cpp#L214)、[test_r19_regression.cpp:2337](file:///d:/Project/Android_Project/LogX/logx/tests/unit/test_r19_regression.cpp#L2337)

**现象 / 风险**

- `logx_c.h` 在 C++ 下把 `logx_instance_t` 定义为 `logx::Instance`。句柄改成 CB 后，任何仍写 `static_cast<Instance*>(h)` 的代码都会静默把 CB 解释成 Instance，造成内存破坏。必须把 C++ 侧也改成不透明前向声明类型（如 `struct logx_instance`），让错误转换直接编译失败；测试通过专用 accessor 取内部指针。
- 现有测试至少 28 处 `static_cast<Instance*>`：`test_r19_regression.cpp` 8 处、`test_robustness.cpp` 18 处、`test_crash_recovery.cpp` 2 处；另有 6 处直接写 `inst->closing`、1 处读 `inst->flusher_tid`（[test_r19_regression.cpp:2508](file:///d:/Project/Android_Project/LogX/logx/tests/unit/test_r19_regression.cpp#L2508)），以及 [test_crash_recovery.cpp:64](file:///d:/Project/Android_Project/LogX/logx/tests/stress/test_crash_recovery.cpp#L64) 的手工拆解式“模拟崩溃”。
- `logx_set_error_handler` / `logx_get_stats` 目前完全不参与生命周期保护；新模型应让它们也走引用守卫，否则 close 并发时仍然 UAF。`logx_get_stats` 是 `void`，无效句柄时应把输出清零。
- `logx_write` 内部还有 7 处 `inst->closing` 检查需要统一改为缓存的 `cb->is_closing`；计划 §3.2 说“后续逻辑完全不变”并不成立。

**建议**

- C++ typedef 改不透明类型 + 测试专用 accessor（或 test seam 头），逐项迁移 28 处 cast、6 处 closing 写入、1 处 flusher_tid 读取、crash 模拟用例。
- 为 `logx_set_error_handler` / `logx_get_stats` 增加引用守卫与关闭后行为（STATE / 清零）。
- 在 WBS 中把测试迁移提前到句柄类型变更的同一步，避免中途无法编译。

---

### R44-07（P1，实施完整性）`logx_open` 失败路径与弱引用初始值未定义

**位置**

- 计划成功路径：[lifecycle_control_block_plan.md:139](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md#L139)
- 现有 open 失败路径：[logx_c.cpp:347](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L347)

**现象**

计划 §3.1 只描述成功路径；现有 open 在 mutex/event 创建、recovery、thread_start 失败时有多条手工清理路径。采用 `strong=2/weak=2` 初始化后，这些路径如果直接 `delete cb`，会与引用计数不一致；如果按 `release_strong/release_weak` 回收，又没有 Flusher 引用可释放。必须在 v1.1 明确：

- 初始 `strong=1/weak=1`；
- `thread_start` 前获取 Flusher 引用，失败时释放；
- 每条失败路径的 staging/event/mutex/CB 清理顺序；
- open 失败是否调用 on_destroy（当前行为是不调用，由 JNI 自行释放上下文，应保持）。

**建议**

- 把 open 的失败路径写成清单或 RAII 守卫，作为 §3.1 的一部分；WBS Step 1 必须包含该项。
- 新增 thread_start 注入失败的定向测试，断言不会泄漏 CB/线程引用。

---

## 四、P2 打磨项

### R44-08（P2，可移植性）TID 复用导致 `on_flusher` 误判

计划用 OS TID 判定 `on_flusher`（[lifecycle_control_block_plan.md:204](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md#L204)）。线程退出后 TID 可能被系统复用；CB 仍存活时，另一个线程可能被误判为 Flusher，影响 `out_closed_immediately`/自毁路径。建议 Flusher 线程用 `thread_local InstanceControlBlock*` 标记，或在 join 后清空 `flusher_tid`。

### R44-09（P2，API 语义）`on_destroy` 的 `err_user` 快照与现状不同

当前 `destroy_instance` 读取最新的 `inst->err_user` 传给 on_destroy（[logx_c.cpp:334](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L334)）；计划在 CB 中只在 open 时快照 `on_destroy_err_user`（[lifecycle_control_block_plan.md:87](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md#L87)）。JNI 当前不调用 set_error_handler，因此 Android 不受影响；C ABI 语义应二选一并写进契约：要么在 set_handler 时同步更新 CB（受 `err_mu` 保护），要么明确“on_destroy 始终收到 open 时的 cfg.err_user”。

### R44-10（P2，对象清理）墓碑/销毁后的 `pal` 与 `inst` 指针

若采用墓碑模型，`dispose_instance` 后必须把 `cb->inst`、`cb->pal` 置空，避免陈旧句柄/二次 close 意外解引用已析构的 Pal；同时明确 `inst` 指针写只发生在 dispose 胜利者、读侧只在强引用保护下访问。若保留可释放 CB 模型，也要写清 `inst=nullptr` 与释放 CB 的顺序。

### R44-11（P2，性能）CAS 递增与性能门禁

计划声称“单次原子操作提升强引用、吞吐与纳秒级延迟无劣化”，但没有量化门禁。50 线程争用下 CAS 循环可能比现有 `fetch_add` 更差；若采用墓碑模型，可以改用 `fetch_add + 关闭后回退` 避免 CAS 循环。建议加固定机器上的前后对比（吞吐 + p99），保留 `logx_hotpath_alloc_tests` 的零分配断言。

### R44-12（P2，验收口径）测试基线 196 与 sanitizer 可执行性

计划三处写 `197 项`（如 [lifecycle_control_block_plan.md:312](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md#L312)）。当前 `build-full`（zstd + AESGCM）二进制 `--gtest_list_tests` 实测为 **196**；设备端 `197/197` 同样需要重定。不同 feature 组合下数量还会变化，验收标准应写成“clean 重建后 `--gtest_list_tests` 实数”，不要硬编码。另：MSVC 有 ASan、无 TSan；Android 设备 TSan 需单独配置且显著放大运行时间，计划 §5 的“ASan/TSan”应写清平台、工具链与运行套件。

### R44-13（P2，文档结构）不变量、时序图、WBS 分阶段与 ABI 措辞

建议 v1.1 增加：

- §2.3 不变量（I1 强引用>0 时 `inst` 有效；I2 `is_closing` 单调；I3 Flusher 引用在 `thread_start` 前获取；I4 `on_destroy` 恰好一次且最后；I5 句柄在 close 返回/on_destroy 后的有效性）。
- close 三条路径时序图（同步 / 递延 / Flusher 回调内）与“谁释放哪个引用”的所有权表。
- open 失败路径清单。
- 测试迁移表（28 处 cast、6 处 closing 写入、crash 模拟、R43）。
- WBS 分两阶段：先做“CB 机械迁移 + 句柄类型/测试适配”，保持旧生命周期逻辑不变并全绿；再做“引用计数/关停协议替换”。这样并发测试失败时能定位到第二阶段。
- 风险与回滚策略、符号扫描清单。
- 文案修正：“C ABI / JNI 零改动”应改为“C ABI 二进制不变、JNI 不变；C++ 源码级不透明句柄类型需调整，测试及内部消费方需迁移”。

---

## 五、已排除疑点

以下担忧经核对**不成立或已被计划正确覆盖**，与发现项同等记录依据：

| 编号 | 疑点 | 排除依据 |
|---|---|---|
| E-1 | “句柄改成 CB 会破坏 JNI/Kotlin” | JNI 只把句柄作为 `jlong` 透传（[logx_jni.cpp:452](file:///d:/Project/Android_Project/LogX/logx/android/cpp/logx_jni.cpp#L452)、[logx_jni.cpp:458](file:///d:/Project/Android_Project/LogX/logx/android/cpp/logx_jni.cpp#L458)）；Kotlin 通过 `executeOp`/`close` 的计数门控保证 nativeClose 不与 API 并发（[LogX.kt:172](file:///d:/Project/Android_Project/LogX/logx/android/kotlin/LogX.kt#L172)、[LogX.kt:579](file:///d:/Project/Android_Project/LogX/logx/android/kotlin/LogX.kt#L579)）。运行时无需改动。 |
| E-2 | “C ABI 二进制被破坏” | `logx_instance_t*` 仍是不透明指针，所有函数签名与结构体布局不变；变化只在 C++ 源码级 typedef 与内部转换。二进制 ABI 不受影响。 |
| E-3 | “热路径新增堆分配” | CB 只在 open 分配一次；OpGuard/EventGuard 均为栈对象；`logx_write` 路径没有新增 `new`。计划 §1.2 的这一收益成立。 |
| E-4 | “单一 dispose 归属本身不可行” | 在引用计数正确、无裸句柄窗口的前提下，最后一个强引用释放者执行 dispose 是标准且可行的；问题在入口保护与关停顺序，不在“单一归属”这个想法。 |
| E-5 | “`logx_flush`/`logx_export` 的 check-then-increment 是计划新问题” | 这是现有缺陷；计划的 OpGuard 统一入口正好可以修掉（[logx_c.cpp:697](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L697) 起）。应作为收益记录，而不是反对项。 |
| E-6 | “`on_destroy` 会重复执行” | 只要 `dispose_instance` 仅由 `strong_count` 归零胜出者调用一次、且 CB 不在 dispose 前释放，现有 JNI trampoline 也只在销毁路径调用一次；该结构可保留。需补的是一次性测试，而不是方案重写。 |

---

## 六、验证局限（必须说明）

1. 本轮是**方案评审**，没有实施分支；未对计划中的新协议做编译、运行、ASan/TSan 或模型检查验证。所有并发结论来自代码与计划的交错推演，不是形式化证明。
2. 测试数量 196 由现有 `build-full`（zstd + AESGCM 开启）二进制 `--gtest_list_tests` 实测；未做 clean 重建，计划 197 的口径差异已确认存在，但最终基线仍需以 clean 重建后的实数为准。不同 feature 组合（AESGCM/ZSTD 开关）数量会变化。
3. 未在 Android NDK/真机验证；计划中的 61/61 TU、真机 197/197 均未复跑。
4. 未运行 MSVC ASan/TSan；TSan 在 MSVC 不可用，Android 设备 TSan 的可用性与运行时间未验证。
5. 行号基于代码基线 `e60d44a` 与计划 v1.0；计划后续修改会使锚点漂移。
6. `test_observability` 的线程句柄配对结论来自源码阅读（`thread_destroy` 仅在 join 成功时递增），未在“Flusher 自毁”场景下实跑复现。
7. 现有 R41 用例在普通构建下可能因 allocator 保留内存而假绿；本轮没有在 ASan 下复跑，因此 R44-01 的 UAF 是“存在窗口/机制成立”，不是已观测崩溃记录。

---

## 七、建议的 v1.1 修订与实施顺序

### 7.1 方案修订（动工前）

1. **先拍板 R44-01/R44-02 两项**（见 §八），据此确定 CB 是否可释放、`weak_count` 是否保留、关停是纯引用计数还是带 `active_ops` 精确 quiescence。
2. 按拍板结果重写 §2.1/§2.2 的引用拓扑与 `release_strong/release_weak` 完整顺序。
3. 按 R44-03 补 `acquire_strong` 内存序、CAS 后二次检查、`out_closed_immediately` 由 release 结果推导。
4. 按 R44-07 补 open 失败路径与启动引用归属。
5. 按 R44-04/R44-05 决定 EventGuard/flusher_busy 与线程 detach/reaper。
6. 按 R44-06 把 C++ typedef 改为不透明类型，准备测试 accessor 与迁移表。
7. 按 R44-12 重定测试基线，按 R44-13 补不变量/时序图/WBS 分阶段。

### 7.2 实施顺序（建议两阶段）

- **Phase 1（机械迁移，行为不变）**：CB 结构 + 句柄类型 + 测试 accessor + 测试迁移；保留现有 `active_ops/closing` 语义（可暂时通过 CB 访问），全量 clean 构建 + 四套件全绿。目标是先把 ABI/类型/测试面收口。
- **Phase 2（协议替换）**：按拍板结果替换为强引用 + quiescence/active_ops 关停协议；移除 `close_pending/close_on_flusher` 与 Dekker 双检；补齐 ASan 并发压力、Flusher 回调 close、线程句柄配对、关闭期事件等新用例。
- **Phase 3（收口）**：NDK clean 构建、真机四套件、性能门禁、文档/CHANGELOG/计划表同步。

这样并发测试若失败，可明确定位在 Phase 2，而不是混在句柄迁移里。

---

## 八、待拍板清单

| # | 决策 | 可选方案 | 建议 |
|---|---|---|---|
| 1 | 并发 close 的契约 | A 收窄契约，要求调用方外部同步；B 句柄注册表/epoch；C CB 墓碑；D 代际槽表 | **C**（保留现有契约、零热路径开销、可顺带删除 weak 机制）；若明确接受契约破坏再选 A |
| 2 | 关停/flush 的归属 | A 纯引用计数 quiescence；B 强引用 + 单一 `active_ops` 计数；C 现稿顺序 + dispose 补 flush | **B**；纯引用计数也能做，但“同步快速路径 + 瞬时进入者”需要额外握手，B 更可验证 |
| 3 | `EventGuard` | A 删除 + `flusher_busy`；B 保留但允许关闭期发事件；C 保持现稿 | **A**；B 可作为最小改动备选 |
| 4 | Flusher 自毁时的线程回收 | A PAL `thread_detach`；B 独立 reaper；C 保证最后一个释放者永远是非 Flusher 线程 | **A**；B 更重，C 难以在所有路径保证 |
| 5 | 句柄/测试迁移 | A C++ typedef 改不透明类型 + 测试 accessor；B 暂时保留 `Instance` typedef | **A**；B 会留下静默错转型风险 |
| 6 | 验收基线 | A 以 clean 重建后的实际测试数为准（当前 196）+ 明确 ASan/TSan 平台 + 性能门禁；B 沿用文档中的 197 | **A** |

第 1、2 项是其余修订的前提；未拍板前不建议进入 WBS Step 1。

---

## 九、评审动作记录

- 已通读计划 v1.0 全部 333 行，核对现有实现 `e60d44a` 的 close 五路径、Flusher 退出段、`flush_blocking`、open 失败路径、JNI/Kotlin 句柄门控与测试内部依赖。
- 已用 `build-full` 现有二进制核对单元测试数量：`--gtest_list_tests` = 196（计划写 197）。
- 本轮**未修改计划正文、未修改任何产品代码或测试**；仅新增本报告并登记 [`README.md`](file:///d:/Project/Android_Project/LogX/logx/docs/reviews/README.md) 索引行。
- 后续若用户拍板，下一轮评审对象为“按 §八 形成的 v1.1 计划 + Phase 1 机械迁移批次”；重点复核 P0-1/P0-2 的协议是否真正闭合。
