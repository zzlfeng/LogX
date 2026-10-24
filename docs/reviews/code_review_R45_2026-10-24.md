# LogX 代码审查报告 R45（T5.8 生命周期堆控制块重构批次 `431d5ee` 独立审查）

- **审查日期**: 2026-10-24
- **审查对象**: [`431d5ee`](https://github.com/zzlfeng/LogX/commit/431d5ee) `refactor(core,pal,test,docs): T5.8 生命周期堆控制块重构落地——CB 墓碑模型根治尾读 UAF，收敛 Dekker 竞态与原生句柄泄漏`（18 个文件，+1058 / -429）
- **关联文档**:
  - [`docs/lifecycle_control_block_decisions.md`](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_decisions.md) v1.1
  - [`docs/lifecycle_control_block_plan.md`](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md) v1.1
  - [`docs/reviews/code_review_R44_2026-10-18.md`](file:///d:/Project/Android_Project/LogX/logx/docs/reviews/code_review_R44_2026-10-18.md)
- **代码基线**: `431d5ee`
- **审查方式**: 逐文件 diff 核对 + 对照 R44 6 项裁决逐条验收 + `OpGuard` / `logx_close_ex` / `emit_event` / `check_deferred_dispose_on_exit` / `flusher_main` 全交错时序与内存序推演 + 新增测试用例有效性（R18-2 反空洞绿）核查。

---

## 一、总体结论

**不建议将 `431d5ee` 直接作为 T5.8 收口基线；本批次捕获 4 项 P0 阻断级缺陷、4 项 P1 重要缺陷与 3 项 P2 打磨项。**

### 1.1 已达成的正向收益
1. **CB 墓碑结构落地**：[`InstanceControlBlock`](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_internal.h#L29-L44) 在 `logx_open` 独立堆分配，销毁时仅 `delete inst` 并将 `cb->inst` 原子置空、保留 CB 墓碑，使陈旧裸句柄在完成关闭后的再次访问不再解引用已释放的 `Instance`。
2. **编译期强类型隔离与测试 Seam 迁移**：[`logx_c.h:135-136`](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_c.h#L135-L136) 将 C++ 侧 `logx_instance_t` 改为不透明 `struct logx_instance`，阻断裸 `static_cast<Instance*>`；存量测试通过 [`test_accessor.h`](file:///d:/Project/Android_Project/LogX/logx/core/include/test_accessor.h) 完成迁移。
3. **PAL `thread_detach` 原语就位**：[`logx_pal.h:82`](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_pal.h#L82)、[`pal_posix.cpp:313-319`](file:///d:/Project/Android_Project/LogX/logx/core/src/pal_posix.cpp#L313-L319)（`pthread_detach` + `delete pt`）与 [`pal_win32.cpp:403-407`](file:///d:/Project/Android_Project/LogX/logx/core/src/pal_win32.cpp#L403-L407)（`CloseHandle`）实现正确。
4. **`logx_open` 失败路径统一收口**：[`cleanup_open_failure`](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L468-L476) 完整回收 `staging`/`events`/`mutex`/`inst`/`cb`（对齐 R44-07）。

### 1.2 核心问题摘要
本批次在把状态变量从 `Instance` 搬到 `InstanceControlBlock` 时，**误以为“迁入控制块即消除了 Dekker 竞态，可将 `seq_cst` 降级为 `acquire/release/relaxed`”**，并在 `OpGuard` 早退回滚、`logx_close_ex` 递延路径及新增单测上引入了严重回归：

| 级别 | 编号 | 概要 | 后果 |
|:---:|:---:|---|---|
| **P0** | **R45-01** | `OpGuard` 构造期二次检查回退未触发 `check_deferred_dispose_on_exit` | `Instance`/mmap/线程句柄**永久泄漏**，`on_destroy` 永不执行 |
| **P0** | **R45-02** | 拆除 `seq_cst` 导致 4 组跨变量 Store-Load 重排（R42-01 回归） | ARM64 下**在途操作 UAF** 或**双方漏认领永久泄漏** |
| **P0** | **R45-03** | `logx_close_ex` 递延路径 `close_requested` 置位后裸读 `inst` 且过早置 `flusher_exit=true` | `logx_close_ex` 内 **UAF** + **在途写无法刷盘丢数据**（R44-02 回归） |
| **P0** | **R45-04** | `logx_set_error_handler` / `logx_get_stats` 未走 `OpGuard`（仅裸查） | 与并发 `close` 发生 **Check-Then-Use UAF**（违反 R44-06） |
| **P1** | **R45-05** | `FlusherSelfDestroyThreadDetachNoLeak` 实为测试主线程触发，未执行 `thread_detach` | **空洞绿测试**（删掉 `thread_detach` 仍 100% 通过，违反 R18-2） |
| **P1** | **R45-06** | Flusher 线程调用 `key_provider` 未纳入 `in_callback` 门禁 | 回调阻塞期并发 `close` 误走同步路径导致 `thread_join` **死锁** |
| **P1** | **R45-07** | `logx_set_error_handler` 未同步更新 `cb->on_destroy_err_user`（R44-09） | 动态更换回调后 `on_destroy` 收到旧 `err_user` 指针 |
| **P1** | **R45-08** | `logx_internal.h` 残留二参 `destroy_instance` 死声明；`test_simulate_crash_destroy` 缺唤醒且 `delete cb` | 头文件污染、崩溃模拟白等 75ms 超时、违反墓碑不变量 |
| **P2** | **R45-09** | `current_tid == 0` 误判 `on_flusher` 及 `flusher_tid` 退出未清零（R44-08） | 极端/Mock PAL 下误走自毁分支或 TID 复用误判 |
| **P2** | **R45-10** | `OpenCloseMemoryBounded` 仅 2000 轮且无内存/尺寸断言 | 未达 R44-01 / 决策一要求的 10k 轮与内存有界断言 |
| **P2** | **R45-11** | `lifecycle_control_block_plan.md` 伪代码与实装代码及状态字段脱节 | 文档与代码不一致 |

---

## 二、P0 阻断级缺陷详解

### R45-01（P0，永久泄漏）`OpGuard` 构造期二次检查回退未触发递延认领 → `Instance` 永久泄漏且 `on_destroy` 永不执行

- **位置**: [logx_c.cpp:288-316](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L288-L316)
  ```cpp
  explicit OpGuard(logx_instance_t* h) {
      if (!h) return;
      auto* c = reinterpret_cast<InstanceControlBlock*>(h);
      if (c->is_closing.load(std::memory_order_acquire)) return;

      c->active_ops.fetch_add(1, std::memory_order_acquire);
      if (c->is_closing.load(std::memory_order_relaxed)) {
          c->active_ops.fetch_sub(1, std::memory_order_release);
          return;
      }

      Instance* i = c->inst.load(std::memory_order_acquire);
      if (!i) {
          c->active_ops.fetch_sub(1, std::memory_order_release);
          return;
      }

      cb = c;
      inst = i;
  }
  ```
- **复现交错（纯顺序一致性即可触发，无需硬件重排）**:
  1. 初始状态：`active_ops == 0, in_callback == 0, is_closing == false`。
  2. 写线程 T1 进入 `OpGuard`：通过 [L291](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L291) `is_closing == false` 检查，执行 [L293](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L293) `c->active_ops.fetch_add(1)`，此时 **`active_ops == 1`**。
  3. 关闭线程 T2 调用 [`logx_close_ex`](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L516-L571)：
     - [L522](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L522) 置 `is_closing = true`；
     - [L537](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L537) 检查 `active_ops == 0` 失败（读到 T1 的 `1`），转入递延关闭；
     - [L550-L554](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L550-L554) 置 `close_requested = true`、`flusher_exit = true` 并唤醒 Flusher；
     - [L559](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L559) 执行兜底双检：`active_ops.load()` **仍为 `1`**（T1 尚未执行 L295），双检不命中，`logx_close_ex` 返回 `LOGX_OK`；
     - Flusher 线程退出主循环，在 [`flusher_main` L1280](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L1280) 同样看到 `active_ops == 1` 不满足静止条件，直接 `return nullptr` 结束线程。
  4. 写线程 T1 继续执行 [L294-L296](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L294-L296)：读到 `is_closing == true`，执行 `c->active_ops.fetch_sub(1)` 将 `active_ops` 从 `1` 减为 `0`，然后直接 `return;`。由于 `this->cb` 仍为 `nullptr`，`~OpGuard()` 中 `if (cb)` 为假，**不调用 `check_deferred_dispose_on_exit(c)`**。
- **后果**:
  `close_requested == true`、`active_ops == 0`、`in_callback == 0`，但所有相关线程均已退出，`destroy_instance` 永不执行。`Instance`、Staging 映射与 fd、互斥量/事件、Flusher 线程句柄全部永久泄漏，且 `on_destroy` 回调永不触发。
- **修复建议**:
  在 `fetch_add(1)` 前先令 `cb = c;`，构造失败时不在构造函数内裸 `fetch_sub`，而是统一交由 `~OpGuard()` 执行递减与 `check_deferred_dispose_on_exit(cb)`；或在两处早退 `fetch_sub(1)` 归零时显式调用 `check_deferred_dispose_on_exit(c)`。

---

### R45-02（P0，并发内存序）拆除 `seq_cst` 导致 4 组 Dekker Store-Load 重排（ARM64 下在途操作 UAF 与漏认领泄漏）

- **位置**:
  - `OpGuard` 构造与析构：[logx_c.cpp:291-315](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L291-L315)
  - `logx_close_ex` 快速路径与递延双检：[logx_c.cpp:522-568](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L522-L568)
  - `check_deferred_dispose_on_exit` 与 `emit_event`：[logx_internal.h:120-160](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_internal.h#L120-L160)
  - `flusher_main` 退出段：[flusher.cpp:1279-1289](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L1279-L1289)
- **成因分析**:
  `is_closing`、`close_requested`、`active_ops`、`in_callback` 是 **4 个独立的原子变量**。它们之间的静止握手全部属于经典的 **Dekker Store-Then-Load 对偶模式**（线程 1 写变量 X 后读变量 Y；线程 2 写变量 Y 后读变量 X）。在 C++11 内存模型及 ARM64（Android 主力架构）硬件下，`release`/`acq_rel` 写与后续对**另一地址**的 `acquire`/`relaxed` 读**完全允许发生 Store-Load 重排**：

  | 对偶组 | 线程 A（先写 X 后读 Y） | 线程 B（先写 Y 后读 X） | Store-Load 重排后果 |
  |---|---|---|---|
  | **组 1：入口拦截 vs 同步快路径** | `OpGuard`: `active_ops.fetch_add(1, acquire)`（甚至无 release）→ `is_closing.load(relaxed)` → `inst.load(acquire)` | `logx_close_ex`: `is_closing.exchange(true, acq_rel)` → `active_ops.load(acquire)` | T1 读到 `is_closing==false` 与有效 `inst` 进入业务操作；T2 读到 `active_ops==0` 走同步快路径 `delete inst` ⇒ **在途 `logx_write/flush/export` 直接 UAF** |
  | **组 2：`OpGuard` 退出 vs `close` 递延双检** | `~OpGuard`: `active_ops.fetch_sub(1, acq_rel)` → `close_requested.load(acquire)` | `logx_close_ex`: `close_requested.store(true, release)` → `active_ops.load(acquire)` | T1 读到 `close_requested==false`；T2 读到 `active_ops==1` ⇒ **双方漏认领，实例永久泄漏** |
  | **组 3：`emit_event` 退出 vs `close` 递延双检** | `emit_event`: `in_callback.fetch_sub(1, acq_rel)` → `close_requested.load(acquire)` | `logx_close_ex`: `close_requested.store(true, release)` → `in_callback.load(acquire)` | 同上，**双方漏认领，实例永久泄漏** |
  | **组 4：`OpGuard` 退出 vs `emit_event` 退出** | `~OpGuard`: `active_ops.fetch_sub(1, acq_rel)` → `in_callback.load(acquire)` | `emit_event`: `in_callback.fetch_sub(1, acq_rel)` → `active_ops.load(acquire)` | T1 读到 `in_callback==1`；T2 读到 `active_ops==1` ⇒ **双方互等对方认领，实例永久泄漏** |

- **修复建议（二选一）**:
  - **方案 A（最小改动，恢复 R42-01 纪律）**：将上述 Store-Load 配对点的原子操作恢复为 `std::memory_order_seq_cst`（特别是 `OpGuard` 构造的 `fetch_add` + `is_closing.load`、析构的 `fetch_sub`、`emit_event` 的 `fetch_sub`、`check_deferred_dispose_on_exit` 的 `load`、`logx_close_ex` 与 `flusher_main` 的状态读写）。
  - **方案 B（真正消除 Dekker 多变量屏障）**：将 `is_closing`（1 bit）、`close_requested`（1 bit）、`active_ops`（30 bits）、`in_callback`（30 bits）合并进 `InstanceControlBlock` 内的**单一 `std::atomic<uint64_t> state{0}`**。单原子变量上的 `fetch_add` / `fetch_sub` / ` fetch_or` 在单一修改序（Modification Order）中天然全序，使用标准 `acq_rel` 即可彻底根除跨变量 Store-Load 重排。

---

### R45-03（P0，UAF + 丢数据）`logx_close_ex` 递延路径在 `close_requested` 置位后裸读 `inst`，且过早置 `flusher_exit=true` 导致在途写丢数据（R44-02 回归）

- **位置**: [logx_c.cpp:549-555](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L549-L555)
  ```cpp
  // ---- 递延关闭路径 (非零即递延，绝不自旋等待) ----
  cb->close_requested.store(true, std::memory_order_release);

  if (inst) {
      inst->flusher_exit.store(true, std::memory_order_release);
      if (inst->flush_event && cb->pal) cb->pal->ev_signal(inst->flush_event);
  }
  ```
- **现象 1（`logx_close_ex` 自身 UAF）**:
  局部变量 `inst` 是在 [L526](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L526) 读取的裸指针，且 `logx_close_ex` 未持有 `active_ops`。当 [L550](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L550) 执行 `cb->close_requested.store(true)` 的瞬间，另一个正在退出的写线程（`~OpGuard`）或回调线程即可在 [`check_deferred_dispose_on_exit`](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_internal.h#L130-L134) 中抢先消费 `close_requested` 与 `cb->inst`，调用 `destroy_instance` 执行 `delete inst`、销毁 `flush_event` 并置 `cb->pal = nullptr`。随后 `logx_close_ex` 执行 [L553-L554](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L553-L554) 访问 `inst->flusher_exit`、`inst->flush_event` 和 `cb->pal`，直接触发 **Heap-Use-After-Free 与空指针解引用**。
- **现象 2（过早停转 Flusher 导致在途写数据未落盘，复现 R44-02）**:
  当外部线程（`!on_flusher`）调用 `logx_close_ex` 时，若存在已越过 [`logx_write` L626 `is_closing` 检查](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L626)的在途写线程（`active_ops > 0`）：
  1. [L553-L554](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L553-L554) 立即置 `inst->flusher_exit = true` 并唤醒 Flusher；
  2. Flusher 线程立即跳出 [`flusher_loop`](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L1034-L1055)，置 `flusher_running = false`，并在 [`flusher_main` L1280](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L1280) 因 `active_ops > 0` 不认领销毁而直接退出线程；
  3. 在途写线程随后完成 `staging->write_records` 写入并在 `~OpGuard` 中触发 `destroy_instance(cb, target, /*is_flusher_thread=*/false)`；
  4. [`destroy_instance` L328](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L328) 调用 `flush_blocking(inst)` 试图排空 staging，但 [`flush_blocking` L1303](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L1303-L1306) 看到 `flusher_running == false` 立即发射 `"flush skipped: flusher not running"` 错误事件并返回 `false`；
  5. `destroy_instance` 随即 `delete inst->staging`，在途写入的数据未能刷入 `.log` 文件（若处于堆降级模式则数据永久丢失）。
- **修复建议**:
  1. 在 `logx_close_ex` 递延路径中，当 `!on_flusher` 时**不得**置 `inst->flusher_exit = true`——保持 Flusher 存活，直到最后一个非 Flusher 线程在 `destroy_instance` 中完成 `flush_blocking(inst)` 后再由 `destroy_instance` 置 `flusher_exit = true` 并 `thread_join`；
  2. 当静止点落在 Flusher 线程自身（① `logx_close_ex` 内 `on_flusher == true`；或 ② [`check_deferred_dispose_on_exit`](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_internal.h#L129) 内 `on_flusher == true` 且 `active_ops == 0 && in_callback == 0`）时，在 Flusher 线程栈内（此时 `inst` 受 Flusher 自身存活保护）置 `inst->flusher_exit = true` 并 signal `flush_event`，让 `flusher_loop` 正常走完尾轮 `drain_once` + `durability_sync` 后在 `flusher_main` 退出段自毁。

---

### R45-04（P0，并发 UAF）`logx_set_error_handler` 与 `logx_get_stats` 未使用 `OpGuard`，存在 Check-Then-Use UAF（违反 R44-06）

- **位置**: [logx_c.cpp:753-790](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L753-L790)
  ```cpp
  logx_status_e logx_set_error_handler(logx_instance_t* h, logx_error_fn fn, void* user) {
      if (!h) return LOGX_ERR_INVALID_ARG;
      auto* cb = reinterpret_cast<InstanceControlBlock*>(h);
      if (cb->is_closing.load(std::memory_order_acquire)) return LOGX_ERR_STATE;
      Instance* inst = cb->inst.load(std::memory_order_acquire);
      if (!inst) return LOGX_ERR_STATE;
      std::lock_guard<std::mutex> lk(inst->err_mu);
      inst->err_fn = fn;
      inst->err_user = user;
      return LOGX_OK;
  }
  ```
- **现象**:
  1. 两个函数只做了 `cb->is_closing` 与 `cb->inst` 的瞬时加载，**未通过 `OpGuard` 递增 `active_ops`**。一旦线程 A 通过 [L758](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L758)（或 `logx_get_stats` 的 [L777](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L777)）拿到非空 `inst` 后被抢占，线程 B 调用 `logx_close` 会因 `active_ops == 0` 立即执行 `delete inst`；线程 A 恢复后对已析构的 `inst->err_mu` 加锁或读取 `inst->drop_no_space`，触发 **Heap-Use-After-Free**。
  2. 此外，`logx_set_error_handler(nullptr, ...)` 原返回 `LOGX_ERR_STATE`，本提交在 [L754](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L754) 改为了 `LOGX_ERR_INVALID_ARG`，与 `logx_c.h:37` 及 `logx_write/flush/export` 的空句柄错误码契约不一致。
- **修复建议**:
  两函数均使用 `OpGuard guard{h};` 保护整个访问区间；`logx_set_error_handler` 在 `!h` 时保持返回 `LOGX_ERR_STATE`。

---

## 三、P1 重要缺陷详解

### R45-05（P1，空洞绿测试）`FlusherSelfDestroyThreadDetachNoLeak` 在主线程调用 `emit_event`，未覆盖 `thread_detach` 路径（违反 R18-2）

- **位置**: [test_observability.cpp:634-695](file:///d:/Project/Android_Project/LogX/logx/tests/unit/test_observability.cpp#L634-L695)、[test_observability.cpp:333-342](file:///d:/Project/Android_Project/LogX/logx/tests/unit/test_observability.cpp#L333-L342)
- **现象**:
  1. 用例在 [L681](file:///d:/Project/Android_Project/LogX/logx/tests/unit/test_observability.cpp#L681) 由**测试主线程**直接调用 `emit_event(logx::test::test_get_instance(inst), ...)`。此时 `current_tid` 是主线程 TID，`on_flusher` 为 **`false`**！
  2. 回调内 `logx_close` 因 `in_callback == 1` 置 `close_requested = true` 返回；随后主线程 `emit_event` 退出时执行 [`check_deferred_dispose_on_exit`](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_internal.h#L129-L135)，因 `!on_flusher` 为真，直接在主线程调用了 `destroy_instance(cb, target, /*is_flusher_thread=*/false)` → 走的是 **`pal->thread_join`**，**完全没有执行 `pal->thread_detach`**。
  3. 同时 [`CountingPal`](file:///d:/Project/Android_Project/LogX/logx/tests/unit/test_observability.cpp#L333-L342) 把 `thread_join` 与 `thread_detach` 混计入同一个 `c.thread_destroy`。若将 [`logx_c.cpp:338`](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L338) 的 `pal->thread_detach` 删除，该测试依然 100% 通过，违反 **R18-2（撤销目标修复后该用例必须确定性复红）**。
- **修复建议**:
  1. 在 `CountingPal` 中拆分或新增 `thread_detach_count` 原子计数，并在该用例中断言 `EXPECT_EQ(counting_pal.c.thread_detach_count.load(), 1)`；
  2. 通过真实后台刷盘使 **Flusher 线程** 自身进入 `emit_event` 回调并在回调内调用 `logx_close(c->inst)`，断言 `in_flusher_cb` 触发时的 TID 等于 `test_get_flusher_tid(inst)`。

---

### R45-06（P1，死锁隐患）Flusher 线程调用 `key_provider` 未纳入 `in_callback` 门禁

- **位置**: [logx_internal.h:33](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_internal.h#L33)、[flusher.cpp:585](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L585)、[flusher.cpp:641](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L641)、[flusher.cpp:967](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L967)
- **现象**:
  [`InstanceControlBlock::in_callback`](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_internal.h#L33) 注释声明 `// 在途回调计数 (emit_event / key_provider)`，R44-02/R44-04 亦明确要求覆盖 `key_provider`，但实现中仅 `emit_event` 递增了 `in_callback`。当 Flusher 在建文件或续写探测时阻塞在用户的 `key_provider` 回调中，外部线程调用 `logx_close` 会看到 `active_ops == 0 && in_callback == 0` 而误入同步快速路径，在 `flush_blocking` 超时（5s）后卡死在 `thread_join` 上（复现 R43 同构死锁）。
- **修复建议**:
  在 `logx_internal.h` 封装 `invoke_key_provider(Instance* inst, uint16_t kid)`，与 `emit_event` 对称维护 `cb->in_callback` 并在归零退出时调用 `check_deferred_dispose_on_exit(cb)`。

---

### R45-07（P1，API 语义回退）`logx_set_error_handler` 未同步更新 `cb->on_destroy_err_user`（R44-09 未闭环）

- **位置**: [logx_c.cpp:359](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L359)、[logx_c.cpp:456](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L456)、[logx_c.cpp:759-762](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L759-L762)
- **现象**:
  `cb->on_destroy_err_user` 仅在 `logx_open` 时快照了 `cfg->err_user`（[L456](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L456)）。调用方后续通过 `logx_set_error_handler(h, fn, new_user)` 更新错误回调上下文时，仅修改了 `inst->err_user`，而 [`destroy_instance` L359](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L359) 取的是 `cb->on_destroy_err_user`，导致 `on_destroy(key_user, err_user)` 拿到的仍是旧指针。
- **修复建议**:
  在 `logx_set_error_handler` 的 `inst->err_mu` 锁保护下同步更新 `cb->on_destroy_err_user = user;`。

---

### R45-08（P1，代码卫生与测试辅助缺陷）`logx_internal.h` 残留旧版 `destroy_instance` 死声明；`test_simulate_crash_destroy` 缺唤醒且违规 `delete cb`

- **位置**: [logx_internal.h:332](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_internal.h#L332)、[test_accessor.h:82-101](file:///d:/Project/Android_Project/LogX/logx/core/include/test_accessor.h#L82-L101)
- **现象**:
  1. [`logx_internal.h:332`](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_internal.h#L332) 残留了已删除定义的二参声明 `void destroy_instance(Instance* inst, bool is_flusher_thread);`（新声明在 [L117](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_internal.h#L117)）。
  2. [`test_simulate_crash_destroy`](file:///d:/Project/Android_Project/LogX/logx/core/include/test_accessor.h#L82-L101) 在 `raw_inst->flusher_exit.store(true)` 后未调用 `ev_signal(raw_inst->flush_event)`，导致 `thread_join` 每次白等 Flusher 轮询超时（75ms，堆降级时 5000ms）；且在 `thread_join` 之前便执行了 `cb->inst.exchange(nullptr)`、末尾执行了 `delete cb;`（若 Flusher 尾轮 `drain_once` 触发 `emit_event` 访问 `inst->cb` 将引发竞态）。
  3. `test_accessor.h` 放在了生产头目录 `core/include/` 下，偏离了决策五“置于 `tests/` 下”的边界隔离初衷。

---

## 四、P2 打磨与文档一致性项

### R45-09（P2，边界防御）`current_tid == 0` 误判与 `flusher_tid` 退出未清零（R44-08）
- **位置**: [logx_c.cpp:531-532](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L531-L532)、[logx_internal.h:125-126](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_internal.h#L125-L126)
- **说明**: 若 Flusher 线程尚未执行到 [`flusher_main` L1274](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L1274)（此时 `cb->flusher_tid == 0`），而 `cb->pal` 为空或自定义 Mock PAL 的 `tid()` 返回 `0`，`0 == 0` 会将外部线程误判为 `on_flusher = true`。此外 `destroy_instance` 未将 `cb->flusher_tid` 清零。建议判据改为 `current_tid != 0 && cb->flusher_tid.load(std::memory_order_acquire) == current_tid`，并在 `destroy_instance` 中清零 `cb->flusher_tid`。

### R45-10（P2，测试门禁）`OpenCloseMemoryBounded` 未对齐 10k 轮与内存上界断言要求
- **位置**: [test_r19_regression.cpp:2879-2900](file:///d:/Project/Android_Project/LogX/logx/tests/unit/test_r19_regression.cpp#L2879-L2900)
- **说明**: R44-01 与 [`lifecycle_control_block_plan.md:375`](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md#L375) 均要求“10k 轮 open/close 墓碑内存上界断言”，当前实现仅运行 2000 轮且无任何尺寸或内存上界断言。建议补充 `static_assert(sizeof(InstanceControlBlock) <= 64, ...)` 及进程工作集/分配增量的有界性断言。

### R45-11（P2，文档同步）计划/裁决文档状态及代码示例与实装代码脱节
- **位置**: [`docs/lifecycle_control_block_plan.md`](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_plan.md)、[`docs/lifecycle_control_block_decisions.md`](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_decisions.md)
- **说明**: `plan.md` §2.2 / §3.3~§3.6 展示的仍是无 `close_requested` 的 `cb->dispose_instance` 成员函数方案，与实装的 `close_requested` + 自由函数 `destroy_instance(cb, inst, ...)` 不一致；两份文档头部状态仍标注“待用户确认 / 正在分阶段实施”。

---

## 五、R44 裁决项对账清单

| R44 决策项 | 裁决要求 | `431d5ee` 落地状态 | 复审结论 |
|:---:|---|:---:|---|
| **决策一 (R44-01)** | CB 墓碑模型 + `cb->inst` 原子化 + `exchange(nullptr)` 认领 + `cb->pal` 置空 | ⚠️ 部分 | CB 墓碑与 `inst.exchange(nullptr)` 已落地；但因 **R45-01 / R45-02 / R45-03 / R45-04**，并发 `close` 下的 UAF 与永久泄漏仍未闭合 |
| **决策二 (R44-02)** | `active_ops == 0 && in_callback == 0` 精确静止判定，先 `flush_blocking` 后停 Flusher | ❌ 未闭合 | 同步路径顺序正确；但递延路径过早置 `flusher_exit=true`（**R45-03**）导致在途写无法 `flush_blocking`，且 `key_provider` 未计入 `in_callback`（**R45-06**） |
| **决策三 (R44-04)** | 允许关闭期发事件 + 回调在飞门禁 | ⚠️ 部分 | `emit_event` 允许关闭期穿透；但退出递延认领缺 `seq_cst`（**R45-02**）且漏 `key_provider`（**R45-06**） |
| **决策四 (R44-05)** | PAL 扩展 `thread_detach`，Flusher 自毁时回收句柄 + 配对单测 | ⚠️ 部分 | PAL 与 `destroy_instance` 实现正确；但新增单测在主线程触发，完全未走 `thread_detach`（**R45-05**） |
| **决策五 (R44-06)** | C++ 不透明句柄 + `test_accessor.h` + `set_error_handler`/`get_stats` 生命周期守卫 | ❌ 未闭合 | 不透明句柄与测试强转迁移完成；但 `set_error_handler`/`get_stats` 未用 `OpGuard` 仍有 UAF（**R45-04**），且 `on_destroy_err_user` 未同步（**R45-07**） |
| **决策六 (R44-12)** | 196 基线 + MSVC/NDK clean 构建 + ASan + 真机四套件与 P50 ≤ 4.2µs | ⏳ 待收口 | 需在修复 P0/P1 后执行 Phase 3 双端 clean 构建、ASan 压测与真机基准验收 |

---

## 六、行动项清单 (Action Items)

- [ ] **ACT-1（P0）**: 修复 **R45-01** —— `OpGuard` 构造期在 `active_ops.fetch_add(1)` 之后的任何早退回滚分支（`is_closing` 或 `!inst`），当递减归零时必须触发 `check_deferred_dispose_on_exit`（或提前绑定 `cb` 统一由 `~OpGuard` 处理）。
- [ ] **ACT-2（P0）**: 修复 **R45-02** —— 恢复 `OpGuard` / `emit_event` / `check_deferred_dispose_on_exit` / `logx_close_ex` / `flusher_main` 跨变量 Dekker 配对点的 `std::memory_order_seq_cst` 全序屏障，或将 4 个状态量合并为单一 `std::atomic<uint64_t>` 状态字。
- [ ] **ACT-3（P0）**: 修复 **R45-03** —— `logx_close_ex` 递延路径严禁在 `close_requested.store(true)` 之后解引用裸 `inst`/`cb->pal`；非 Flusher 线程在 `active_ops > 0` 时不得提前置 `inst->flusher_exit = true`，改由最终非 Flusher 销毁者在 `destroy_instance` 的 `flush_blocking` 之后置位，或由最后静止在 Flusher 线程上的退出分支（`on_flusher`）置位唤醒。
- [ ] **ACT-4（P0）**: 修复 **R45-04 & R45-07** —— `logx_set_error_handler` 与 `logx_get_stats` 接入 `OpGuard`；`logx_set_error_handler(nullptr, ...)` 恢复返回 `LOGX_ERR_STATE`，并在 `err_mu` 锁内同步更新 `cb->on_destroy_err_user = user`。
- [ ] **ACT-5（P1）**: 修复 **R45-05** —— 重写 `FlusherSelfDestroyThreadDetachNoLeak`，由 Flusher 后台线程真实触发回调并在回调内 `logx_close`；`CountingPal` 增加独立 `thread_detach_count` 断言，确保注释掉 `thread_detach` 时用例必红（满足 R18-2）。
- [ ] **ACT-6（P1）**: 修复 **R45-06** —— 将 `inst->key_provider` 调用封装进 `in_callback` 门禁保护，并补充“Flusher 阻塞于 `key_provider` 期间并发 `close` 不死锁”回归单测。
- [ ] **ACT-7（P1/P2）**: 修复 **R45-08 ~ R45-11** —— 删除 `logx_internal.h:332` 死声明；修复 `test_simulate_crash_destroy` 唤醒与墓碑生命周期；加固 `current_tid != 0` 判据与 `flusher_tid` 清零；对齐 `OpenCloseMemoryBounded` 断言与计划文档描述。

