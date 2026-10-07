# R32 审查报告：近期代码提交与 B-10/B-8 落地综合独立审查

> **审查日期**: 2026-10-07（首版入库后经核验勘误修正）
> **审查对象**:
> - 提交 `a213762`「fix(core,test,docs): B-1 复审收口——P3-1 单调钟 + P3-2 closing 丢弃记账统一 + 计划表 v1.40 增录 B-10」
> - 提交 `4cc998a`「fix(test): B-1 收口用例丢唤醒窗口加固——解除 Flusher 阻塞改持锁 store+notify」
> - 提交 `d636559`「feat(recovery,test): Backlog B-10 方案 A1 落地 + B-8 闭环(方案 C)——残留分卷 seqno 倒退防御与虚报链消解实证」
> - 关联文档提交 `feba2c2` / `7521c66`（文档与计划表状态对齐）
> **审查方式**: 全量 diff 逐行审读 + 状态机与并发时序推演 + Nonce 构造与文件生命周期核算 + 全量测试独立复跑（MSVC Release 四套件）+ 边界推演
> **裁定**: **改动成立，逻辑严密，测试覆盖完整，可保留**。各项提交均有效解决了对应缺陷（消除丢唤醒竞态、规避 NTP 跳跃死等、统一 closing 丢弃指标、维护磁盘分卷间序号单调不变量）。本审查进一步识别出 2 项多写者极端边界下的潜在盲区（F-1/F-2，均定级 P3），并给出演进优化方案。
> **特别勘误说明**: 本审查纠正了此前沿袭自提交信息与注释的「B-10 防 AES-GCM Nonce 复用」错误定性——由于每文件拥有独立 8B 随机 salt，跨分卷 seqno 倒退并不构成 Nonce 复用；B-10 真实价值在于维护全局 seqno 单调递增的系统不变量。

---

## 一、 审查对象概述

本次审查覆盖近期完成的 3 次核心功能/修复代码提交：

1. **`a213762` (B-1 复审收口)**：
   - **P3-1**: `logx_write` 重试等待时钟源由 `wallclock_ms()` 改为单调时钟 `pal->monotonic_ns() / 1000000ULL`，辅以 `bool retry_started` 消除 0 哨兵歧义，彻底杜绝 NTP 时间跳变影响退避周期；
   - **P3-2**: 重试循环内 `closing` 状态跳出由原直接 `return LOGX_OK` 改为 `break`，统一交由 `st == kFull` 执行 `drop_no_space` 递增与 `LOGX_EV_DROP` 事件通知，消除关闭期间丢弃日志的“无痕静默黑洞”；
   - 新增专用回归用例 `B1_NoSpace_ClosingDuringRetry_CountsDropAndReturnsOk` 与 `ClosingOnRetryPal` 测试桩。

2. **`4cc998a` (B-1 单测 Flusher 阻塞唤醒加固)**：
   - 针对 `B1_NoSpace_ClosingDuringRetry_CountsDropAndReturnsOk` 退出阶段，将解除 Flusher 阻塞的操作从锁外修改改为在 `stall_mu` 保护下执行 `store(false)` 与 `notify_all()`，消除测试退出时条件变量丢失唤醒导致 `logx_close` join 挂死的竞态。

3. **`d636559` (B-10 方案 A1 落地 + B-8 闭环方案 C)**：
   - **B-10 方案 A1**: 在 `reconcile` 的 `is_target_newer` 分支增加异常残留高分卷防御门：当选中追加目标非空且其尾块 seqno 低于 ATC hint 文件尾块（表现为多写者或外部异常文件残留特征）时，发出 `LOGX_EV_RECOVERY` 警告事件，并以 `ReconcileResult::seqno_floor` 钳制 `resume_seqno` 下限，防止磁盘全局 seqno 倒退；保持 B-7 的 `(date, split)` 二元组决选语义不变；
   - **B-8 方案 C**: 实验证实原始虚报链已被 B-7 从根源消解（ATC 提示与实际尾块持有文件错配消除），在 `logx_c.h` 中补充 `truncated_blocks` 的口径注记（表明其为缺口诊断指标，非精确业务 SLA）；
   - 新增针对 B-8 与 B-10 的专属回归用例与反空洞绿变异对照。

---

## 二、 测试验证与构建复跑核验

在本地环境（Windows / MSVC 14.43 x64 Release, `LOGX_WITH_ZSTD=ON`, `LOGX_WITH_AESGCM=ON`）执行完整四套件测试复跑：

| 测试目标 | 套件说明 | 用例数 / 状态 | 耗时 |
|---|---|---|---|
| `logx_unit_tests` | 全量单元测试（含 B-1, B-7, B-8, B-10 专属用例） | **172 / 172 PASSED** | 17.08s |
| `logx_hotpath_alloc_tests` | 热路径零堆分配测试 | **2 / 2 PASSED** | 0.26s |
| `logx_stress_tests` | 50 线程高并发与压力注入测试 | **4 / 4 PASSED** | 1.18s |
| `logx_crash_fork_tests` | 崩溃注入与恢复对账测试 | **3 / 3 PASSED** | 2.38s |

**测试结论**：**100% 全部通过（0 失败、0 错误、0 警告）**。

---

## 三、 逐项改动关键推演与深度评估

### 3.1 提交 `d636559`：B-10 异常残留分卷防御机制推演与密码学定性勘误

```cpp
// recovery.cpp:182-197
if (is_target_newer) {
    sr = scan_append_target(inst, best);
    if (!hint.empty() && sr.ok && sr.seqno > 0) {
        FileScanResult hs = scan_append_target(inst, hint);
        if (hs.ok && hs.seqno > sr.seqno) {
            r.seqno_floor = hs.seqno;
            emit_event(inst, LOGX_EV_RECOVERY, -1,
                       "target seqno regression vs hint; resume clamped");
        }
    }
}
// recovery.cpp:346
resume_seqno = std::max(resume_seqno, rec.seqno_floor);
```

#### 关键定性勘误（关于「密码学 Nonce 复用」的前置假定辨析）：
提交信息、`recovery.cpp:151-152` 注释以及计划表 v1.43 均声明：“防御动机核心是防止磁盘 seqno 重复导致 AES-GCM nonce 复用”。**经下探实现层核验，该密码学危害断言不成立**：
1. **Nonce 派生规则**：据 `codec.cpp:56 make_nonce`，12 字节 Nonce 的构成是 `salt(8B) ‖ seqno(4B)`；
2. **Salt 生命周期**：据 `flusher.cpp:573-600`（或 `append_block` 路径），新日志文件在创建时通过安全随机数独立生成 8 字节 salt（碰撞概率 $2^{-64}$），旧文件续写时则从 32B 文件头回读（`flusher.cpp:612-617`）；
3. **推演结论**：B-10 防御的是**跨分卷**的序号倒退。由于撞号的两块分属不同的物理文件，其 `salt` 互相独立，拼装出的 12B Nonce 天然不同，**在密码学上根本不构成 AES-GCM Nonce 复用**。
4. **真正的 Nonce 复用窗口**：仅在多写者未经协调并发续写**同一个日志文件**（共享相同的文件头 salt、相同密钥且生成相同 seqno）时发生，而 B-10 基于 `best` vs `hint` 的跨文件防御门对此场景天然不覆盖。
5. **B-10 的真实价值**：维持**「磁盘 seqno 全局单调递增」这一核心系统不变量**。若发生跨卷 seqno 倒退，将导致：
   - 离线/导出解码器按 seqno 排序合并时日志乱序或被去重丢失；
   - 恢复期对账算法（`sum_raw_len_since`）与 B-8 类截断块判定失真。
   因此，B-10 属于**数据流可读性与恢复对账健壮性防御**，而非密码学安全漏洞修复。

---

### 3.2 提交 `4cc998a`：条件变量丢唤醒加固推演

```cpp
// test_r19_regression.cpp
{
    std::lock_guard<std::mutex> lk(mock_pal.stall_mu);
    mock_pal.stall_write.store(false, std::memory_order_release);
    mock_pal.stall_cv.notify_all();
}
ii->closing.store(false, std::memory_order_release);
logx_close(inst);
```

#### 正确性评估：
- **标准同步范式**：在 `std::condition_variable` 同步中，若在未持有互斥锁的情况下修改谓词并 `notify`，可能出现等待线程已评估谓词为阻塞但尚未进入内核等待状态的瞬态微小窗口，此时发出的通知会丢失。改为持锁 `store` + `notify_all`，彻底切断了丢失通知的窗口，完全正确。

---

### 3.3 提交 `a213762`：单调时钟与 closing 统一记账推演

```cpp
// logx_c.cpp:559-579
uint64_t now_ms = inst->pal->monotonic_ns() / 1000000ULL;
if (!retry_started) {
    retry_started = true;
    start_retry_ms = now_ms;
} else if (now_ms >= start_retry_ms + inst->staging_wait_ms) {
    break;  // 超时
}
// ...
if (inst->closing.load(std::memory_order_acquire)) {
    break;
}
```

#### 正确性评估：
- **时钟跳跃防御**：移动设备（尤其 Android 真机）NTP 阶跃十分常见，使用 `pal->monotonic_ns()` 作为时钟基准是工业级实现的唯一正确选择；`retry_started` 标志则规避了 0 毫秒哨兵值的歧义。
- **可观测性闭环**：在 `closing` 时执行 `break` 流转至 `kFull` 处理段，既完成了 `drop_no_space` 累加和 `LOGX_EV_DROP` 事件通知，又在函数退出前通过检查 `closing` 返回 `LOGX_OK`，既不给外部调用方报出非预期错误，又杜绝了内部静默黑洞。

---

## 四、 审查发现清单（Findings）

| 编号 | 级别 | 影响模块 / 文件 | 现象与成因 | 建议与处置 |
|---|---|---|---|---|
| **F-1** | **P3** | `core/src/recovery.cpp:190` | **空残留高分卷且 Header 撕裂时的 seqno 倒退漏防**<br>条件 `sr.seqno > 0` 遵循 R19 F-13 的既定设计意图（豁免空分卷被误判为损坏并虚报截断）。但在外部异常残留高分卷为空且 Staging Header 严重撕裂落后（`S_hdr < hint.seqno`）的极端组合下，由于 `sr.seqno == 0`，代码走 `rec.is_new_target = true`（:216），并在 `run_recovery` 中走 `resume_seqno = S_hdr`（:338-339），绕过了 B-10 的 `seqno_floor`，导致新分卷从较小的 `S_hdr` 启写，打破全局单调递增。 | 见优化方案 1：升级为目录全局最大序号钳制 |
| **F-2** | **P3** | `core/src/recovery.cpp:190-197` | **多级分卷或 hint 为空时的倒退漏防**<br>当前仅将选中的 `best` 与单个 `hint` 进行比对。若目录中存在 3 个以上历史分卷（且最大序号在更早分卷），或 `hdr.target.date == 0`（全新启动/Header 重建导致 hint 为空），该防御门无法感知全目录历史最大已写入序号。 | 见优化方案 1：在目录扫描阶段直接采集 `dir_max_seqno` |
| **F-3** | **Nit** | `tests/unit/test_r19_regression.cpp:1963` | **测试中重置 `closing = false` 缺少意图注释**<br>`logx_close` 内部使用 `closing.exchange(true)` 保证幂等，测试桩中若不先重置为 `false` 则无法真正执行退出清理。此处缺少说明容易让后续维护者产生困惑。 | 在代码行上方补充简短注释说明 |

---

## 五、 已排除疑点（逐条辨析与核验依据）

与发现项同等记录，以下为本轮复审深入推演但确证不构成缺陷的疑点：

1. **疑点 1: `now_ms >= start_retry_ms + inst->staging_wait_ms` 是否存在 64 位整型溢出？**
   - **辨析核验**：`monotonic_ns() / 1000000ULL` 换算为毫秒，`uint64_t` 最大值约为 $1.84 \times 10^{19}$ 毫秒，折合约 5.8 亿年。即使进程常驻，`start_retry_ms` 加上有限的超时配置（通常毫秒至秒级）在人类文明时间尺度内绝无溢出风险。
   - **结论**：排除，运算绝对安全。

2. **疑点 2: `4cc998a` 持锁 store+notify 是否会在生产环境下引入锁争用？**
   - **辨析核验**：`ClosingOnRetryPal` 仅为单测专用 Mock PAL 桩，其内部的 `stall_mu` 是测试内部为了精确控制 Flusher 暂停与继续而设的辅助互斥锁，零入侵产品代码（`Instance` / `FlusherCtx`），生产路径无此锁。
   - **结论**：排除，仅属测试加固，零生产副作用。

3. **疑点 3: B-10 对 `hint` 执行 `scan_append_target` 是否会意外截断完好的历史文件？**
   - **辨析核验**：`scan_append_target` 内部调用 `scan_one(..., repair=true)`。对于已经正常落盘的旧分卷，其尾块完整且 CRC 正确，`scan_one` 扫描确认完好后原样返回，仅在尾部存在损坏/撕裂半块时才会截断坏尾。
   - **结论**：排除，符合 LogX 自动修复撕裂尾块的既定纪律。

---

## 六、 验证局限（必须说明）

根据项目审查准则，本轮审查记录以下验证局限：

1. **平台范围局限**：全量四套件测试（172/172 单测、2/2 hotpath、4/4 压力、3/3 崩溃）均在 Windows / MSVC 14.43 x64 环境下复跑；Android arm64 交叉编译与真机（Redmi K30 Pro）执行依赖近期批次（提交 `8140ec5` / 计划表 v1.42）验证基线，本审查会话内未重新连接真机复跑。
2. **多写者实测局限**：B-10 涉及的多写者残留场景主要通过单元测试注入构造（`B10_ResidualHigherSplitSeqnoRegression_ClampedAndWarned`），未在真实多进程并发写同一目录的操作系统底层高频竞争下进行端到端长周期压测。

---

## 七、 更优修改与演进方案（Better Alternatives）

### 方案 1：B-10 升级为“全局目录最大序号防御门（Global Max Seqno Floor）”【建议纳入 Backlog】

- **现有痛点**：
  当前逻辑需额外触发一次 `scan_append_target(inst, hint)`（重复磁盘 IO），且受限于 `sr.seqno > 0` 和单个 `hint`，留下了 F-1、F-2 两个防御死角。
- **更优设计**：
  `scan_dir_for_target(inst)` 原本就会对目录下的每个分卷执行 `scan_one(..., repair=false)`。可以在遍历目录时顺带记录目录下**所有分卷的最大有效块序号**：
  ```cpp
  // flusher.cpp: scan_dir_for_target 可扩展返回结构体
  struct DirTargetInfo {
      std::string best_path;
      uint32_t    max_dir_seqno = 0; // 目录下所有分卷尾块的最大序号
  };
  ```
  在 `recovery.cpp` 对账时：
  ```cpp
  // 无论 hint 是否为空、无论 best 是否为空分卷：
  if (dir_info.max_dir_seqno > sr.seqno) {
      r.seqno_floor = dir_info.max_dir_seqno;
      emit_event(inst, LOGX_EV_RECOVERY, -1,
                 "target seqno regression vs directory max; resume clamped");
  }
  ```
- **核心收益**：
  1. **零额外 IO 开销**：消除对 `hint` 的二次磁盘扫描；
  2. **根除所有防御盲区**：天然免疫空分卷残留、多分卷跨度撕裂、无 hint 启动等全部极端场景。

---

### 方案 2：B-1 NO_SPACE 有界退避由轮询 Sleep 演进为事件通知驱动（Event-Driven Backoff）

- **现状分析**：
  目前在空间不足时，写入线程锁外调用 `pal->sleep_ms(2)` 轮询。小并发下极简可靠，但在极端并发突发打满暂存区时，多线程每 2ms 密集唤醒争抢 `inst->mu`，存在锁抖动与无效唤醒开销。
- **演进建议**：
  在 `Instance` 中引入 `staging_space_ready_event`。后台 Flusher 每次消费释放环形缓冲区后触发信号；写入线程通过 `ev_wait_timeout(..., remaining_wait_ms)` 等待空间释放。可作为后续高并发深度优化备选。

---

## 八、 审查裁定

1. **当前实现评定**：近期 3 次提交功能闭环，逻辑与测试质量扎实，**在标准单写者契约下完全正确且安全**；
2. **入库与合并建议**：改动可直接保留合入；
3. **后续演进建议**：建议将 **方案 1（全局目录最大序号防御门）** 记入实施计划表 Backlog，作为后续日志恢复健壮性的终极收口。
