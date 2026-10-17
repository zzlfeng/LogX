# LogX 代码审查报告 R42（R41 修复批次独立复审）

- **审查日期**: 2026-10-17
- **审查对象**: `9704832..HEAD` 共 2 个提交
  - [`4c25912`](https://github.com/zzlfeng/LogX/commit/4c25912) fix(core,jni,test): R41 审查发现落地——flusher 递延销毁认领收敛（ACT-1/2）、JNI 密钥缓存抽出 `jni_key_cache.h` 属主隔离与空槽优先复用（ACT-3/4）、`logx_c.h` 契约补注 + 新增 JniKeyCache 3 项与 R41 递延 close 并发写压力单测
  - [`1b2b2fd`](https://github.com/zzlfeng/LogX/commit/1b2b2fd) docs(review): R41 报告行动项登记——ACT-1~ACT-5 全部落地勾选并补注实现细节与 M6 规划
- **审查方式**: 逐提交 diff 推导 + close 三态认领协议（OpGuard / emit_event / flusher_main）全链路时序推演 + JNI TLS 缓存跨线程生命周期推演 + Windows 本地全量重建与 `ctest` 回归 + 新增用例定向复跑。
- **基线**: `1b2b2fd`（领先 `origin/master` 10 个 commit）

---

## 一、总体结论

1. **ACT-1 主修复正确且完整**：`flusher_main` 不再无条件消费外部 `close_pending`，R41-01 高危（flusher 提前自毁 → 并发写线程 UAF）确实消除；新增 `R41_DeferredClose_ConcurrentWrites_NoUAF` 10 轮 × 4 线程定向压力用例覆盖到位。
2. **ACT-2 的双检兜底引入 1 条新中危竞态（R42-01）**：flusher 侧兜底认领与 OpGuard 析构认领协议在 acquire/release 内存序下构成经典 Dekker 形态——存在「双双错过 → 实例泄漏」与「flusher 抢先销毁 → op 尾读/尾换已释放 inst」两个窄窗口，实质扩大了 R41-05 已登记竞态族的暴露面（新增 flusher 为第三个认领者）。
3. **ACT-3 属主隔离在"同线程"场景成立，但跨线程失效不彻底（R42-02，中危）**：`invalidate_owner` 只清理执行销毁的当前线程 TLS，而槽位实际由 key_provider 调用线程（Flusher / export 线程池）填充；销毁线程 ≠ 缓存线程时清理近似 no-op，悬垂 owner 可被新实例地址复用假命中。
4. **ACT-4 契约补注与实现一致**，但满槽轮转仍会静默覆盖在持槽位（R42-03，低危），超限表现为错钥而非降级。
5. **回归验证**：Windows MSVC x64 `build-full`（zstd + AESGCM）全量重建成功，`ctest` 4/4 全绿；新增 4 项用例（JniKeyCache×3 + R41 压力）定向复跑通过。

---

## 二、新发现缺陷详解

### ⚠️ R42-01（中危/并发竞态）flusher 双检兜底与 OpGuard 认领协议缺全序屏障

- **位置**: [flusher.cpp `flusher_main` 退出段 L1288-L1295](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L1288-L1295) 与 [logx_c.cpp `OpGuard` 析构 L288-L294](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L288-L294)
- **成因链**（移交路径：flusher 判 `active_ops>0` → 置 `close_pending` → 双检）：
  1. **UAF 窗口**: 外部 op 已 `fetch_sub(active_ops)` 归零、尚未执行自身 `exchange(close_pending)`；flusher 双检读到 `active_ops==0 && in_callback==0` → exchange 抢先赢 → `destroy_instance(true)` → `delete inst` → op 随后对已释放内存做 `in_callback.load` / `close_pending.exchange`。
  2. **漏认领窗口（泄漏）**: 两侧均为 acquire/release 的「store 后 load」，属经典 Dekker store-buffer 重排形态——op 的 `load(close_pending)` 读到旧 false、flusher 的 `load(active_ops)` 读到旧 >0，双双错过 → 无人认领，`on_destroy` 永不执行、实例泄漏。
- **测试为何全绿**: 窗口为纳秒~微秒级；`R41_DeferredClose_ConcurrentWrites_NoUAF` 的 `EXPECT_TRUE(destroyed)` 理论上会在漏认领窗口下偶发失败（可作为该缺陷的回归探针）。
- **修复方案**: flusher 侧在 `close_pending.store(true)` 与双检 load 之间、OpGuard 侧在 `fetch_sub` 与 `close_pending.load` 之间各加 `std::atomic_thread_fence(std::memory_order_seq_cst)`（或将相关计数/标志原子操作统一为 seq_cst）。全序下保证「至少一方看到对方写入」：flusher 见 0 则 op 必见 true 自行认领；flusher 见 >0 则移交对象尚未退栈、后续必见 `close_pending`。
- **备注**: 与 R41-05 同族，但 R41-05 登记时认领者仅 OpGuard/emit_event 两方，本批双检把 flusher 加入认领方，UAF 窗口从「败方尾读」升级为「跨线程抢先销毁」，建议不并入既有登记、单独收敛。

### ⚠️ R42-02（中危/密钥污染）`invalidate_owner` 仅清当前线程 TLS，跨线程槽位残留悬垂 owner 与密钥

- **位置**: [logx_jni.cpp `jni_destroy_trampoline` L244-L247](file:///d:/Project/Android_Project/LogX/logx/android/cpp/logx_jni.cpp#L244-L247)、[jni_key_cache.h](file:///d:/Project/Android_Project/LogX/logx/android/cpp/jni_key_cache.h)
- **成因链**:
  1. 缓存槽由**调用 `key_provider` 的线程**填充——Flusher 线程（开文件/轮转）与 export 调用线程（Kotlin `Dispatchers.IO` 线程池，线程长期复用）；
  2. `jni_destroy_trampoline` 运行在**认领销毁的线程**（外部最后 op 线程或 flusher 线程），二者通常不是同一线程 → `invalidate_owner(kp_ctx)` 对真正持有槽位的线程近似 no-op；
  3. `kp_ctx` 释放后，其它线程槽位仍持有（悬垂 owner、kid、明文密钥）。新实例的 `JniKeyProviderContext` 若被分配器复用同地址且 kid 序列相同——**tester 的 `applyAndRestart`（close→reopen，新随机密钥）正是现实触发路径**——`lookup(ctx, kid)` 假命中旧密钥 → 加/解密错钥、静默数据损坏；
  4. 同时密钥材料长期残留他线程 TLS 未清零，属安全卫生缺口。
- **修复方案**（择一）:
  - 首选：`JniKeyProviderContext` 增加进程级单调递增 `generation`，槽位存 `(owner, gen, kid)`，`lookup` 三元匹配；销毁侧无需跨线程广播；
  - 或：槽位存 `owner` + 进程级共享「已销毁 ctx 墓碑集合（带 gen）」，lookup 命中前查墓碑；
  - 最低要求：将「跨线程 TLS 残留」明文登记为已知限制并说明 M6 根治计划（与 R41-04 合并处理）。

### 🔍 R42-03（低危/契约缝隙）`allocate_slot` 满槽轮转仍会静默覆盖在持槽位

- **位置**: [jni_key_cache.h L45-L51](file:///d:/Project/Android_Project/LogX/logx/android/cpp/jni_key_cache.h#L45-L51)
- **分析**: 4 槽全 valid 时 `next_++ % N` 直接覆写。[logx_c.h J-6 规约](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_c.h#L104-L109) 要求「不同 kid 必须拥有互不覆盖的独立存储区域」，core 侧（如 `ActiveFileTarget.key`）可能仍持有被驱逐槽的指针——第 5 个 (owner,kid) 组合出现时旧指针内容被静默替换为他钥。ACT-4 已在契约注释声明「单线程活跃不同 kid ≤ 4」，但代码未强制该前提。
- **修复方案**: 无空槽时返回 `nullptr`（core 已有缺钥降级路径与 DEGRADED 告警），使超限表现为**显式降级**而非静默损坏；或至少禁止驱逐其它 owner 仍活跃的槽。M6 多 KID 轮换专项时一并根治。

### ℹ️ R42-04（提示/工程卫生）

- `android/logx-tester/.kotlin/` 为 Kotlin 构建产物，当前未跟踪且未入 .gitignore，存在被 `git add .` 误提交的风险，建议登记忽略。
- 裸 PowerShell 重建 `build-full` 需先 `Launch-VsDevShell.ps1 -Arch amd64` 注入 MSVC INCLUDE，否则 zstd/mbedtls C 单元报 `limits.h/stddef.h` 找不到——验证记录宜注明环境前置，避免复现歧义。

---

## 三、逐项核验通过清单

| 提交 | 审查项 | 结论 | 说明 |
|---|---|---|---|
| 4c25912 | ACT-1 flusher 不再消费外部 `close_pending` | ✅ | 认领唯一归属 OpGuard/emit_event；R41-01 时序（U2 写入中 + U1 递延 close + flusher 抢先销毁）已不可达 |
| 4c25912 | ACT-2 `close_on_flusher` 认领前计数守卫 | ⚠️ 部分 | 守卫本体正确（非零转 `close_pending` 移交）；但双检兜底引入 R42-01 新窗口 |
| 4c25912 | ACT-3 `jni_key_cache.h` owner+kid 双匹配 | ⚠️ 部分 | 同线程跨实例污染（R41-03 直接场景）已杜绝；单测覆盖属主隔离/失效清零/空槽复用；跨线程残留见 R42-02 |
| 4c25912 | ACT-4 契约补注 + 空槽优先复用 | ✅ | `logx_c.h` 表述与实现一致；空槽优先策略正确（`invalidate_owner` 后槽位可复用）；满槽驱逐见 R42-03 |
| 4c25912 | 新增单测 4 项 | ✅ | `JniKeyCache.OwnerIsolationAndHit / InvalidateOwnerAndZeroMemory / SlotReuseAndRoundRobin` 逻辑断言齐备；`R41_DeferredClose_ConcurrentWrites_NoUAF` 10 轮 × 4 线程、`stop_writers` 收敛、`destroyed` 闭环断言正确 |
| 4c25912 | `jni_key_provider_trampoline` 其余路径 | ✅ | len≠32 / Java 异常 / GetEnv 失败均返回 nullptr 不入槽；attach/detach 对称性保持（R16 F-3 纪律未破坏） |
| 1b2b2fd | R41 报告行动项勾选 | ✅ | 勾选内容与 `4c25912` 实际落地一致，无虚报 |

---

## 四、自动化测试套件验证记录

Windows MSVC x64（`build-full`，zstd + AESGCM 开启；环境前置：`Launch-VsDevShell.ps1 -Arch amd64`）全量重建后：

```text
1/4 Test #1: logx_unit_tests ..................   Passed   17.26 sec
2/4 Test #2: logx_hotpath_alloc_tests .........   Passed    0.25 sec
3/4 Test #3: logx_stress_tests ................   Passed    1.14 sec
4/4 Test #4: logx_crash_fork_tests ............   Passed    2.14 sec

100% tests passed, 0 tests failed out of 4
```

新增用例定向复跑：

```text
[  PASSED  ] 4 tests.   (JniKeyCache×3 + R19Regression.R41_DeferredClose_ConcurrentWrites_NoUAF 461 ms)
```

---

## 五、行动项清单 (Action Items)

- [x] **ACT-1（P0，必须）**: 修复 R42-01——flusher 双检与 OpGuard/emit_event 认领两侧统一采用 `seq_cst` 并补充 `std::atomic_thread_fence(seq_cst)`，封死 Dekker 重排导致的「抢先销毁 UAF」与「双双错过泄漏」两窗口；压力用例验证全绿。
- [x] **ACT-2（P1）**: 修复 R42-02——`JniKeyProviderContext` 引入全局单调原子 `generation`，`TlsKeyCache` 槽位升级为 `(owner, gen, kid)` 三元匹配并增加惰性清扫机制，彻底杜绝跨线程复用与 ABA 地址复用导致的假命中。
- [x] **ACT-3（P2/M6 前置）**: 处理 R42-03——`allocate_slot` 满槽容量耗尽时显式返回 `nullptr`，让 core 走缺钥降级与 DEGRADED 告警，杜绝静默覆盖在持指针；新增单测覆盖断言。
- [x] **ACT-4（登记）**: 修复 R42-04——`.kotlin/` 已加入 `.gitignore`；验证记录与开发环境前置已规范对齐。
