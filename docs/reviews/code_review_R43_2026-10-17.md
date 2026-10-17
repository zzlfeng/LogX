# LogX 代码审查报告 R43（R42 修复批次独立复审）

- **审查日期**: 2026-10-17
- **审查对象**: `2e75052..HEAD` 共 2 个提交
  - [`2783c90`](https://github.com/zzlfeng/LogX/commit/2783c90) fix(core,jni,test): R42 审查发现落地——建立 seq_cst 全序屏障消除 Dekker 竞态（ACT-1）、引入 generation 根治跨线程 ABA 假命中（ACT-2）、满槽显式返回 nullptr（ACT-3）、.kotlin 忽略（ACT-4）
  - [`bacbc31`](https://github.com/zzlfeng/LogX/commit/bacbc31) docs(review): R42 报告行动项登记
- **审查方式**: 逐提交 diff 推导 + close 三态认领协议（OpGuard / emit_event / flusher_main）Dekker 配对全时序推演 + JNI TLS 缓存槽位接管路径推演 + Windows 全量重建与 `ctest` 回归 + JniKeyCache 定向复跑。
- **基线**: `bacbc31`

---

## 一、总体结论

1. **ACT-1 seq_cst 收敛正确且完整**：读侧（`emit_event` 的 `fetch_sub(in_callback, seq_cst)`、`OpGuard` 的 `fetch_sub(active_ops, seq_cst)`）与写侧（`logx_close_ex` / `flusher_main` 的 `store(close_pending, seq_cst)` → 加载计数）全部进入 S 全序，逐情形推演未发现「抢先销毁 UAF」与「双双错过泄漏」两个 R42-01 窗口残留；`exchange` 保证认领唯一。R42-01 判定通过。
2. **ACT-2 generation 三元匹配正确**：全局 `gen` 自 1 起永不取 0，`(owner, gen, kid)` 匹配 + 惰性清扫同 owner 陈旧代数槽，ABA 假命中根治成立；`gen` 在 ctx 发布前写入，经 `logx_open` 线程创建具备 happens-before。R42-02 判定通过。
3. **ACT-3 满槽 nullptr 路径干净**：不破坏在持槽位、无 local ref 泄漏、正确触发 core 缺钥降级。R42-03 判定通过。
4. **复审新捕获 1 条中危遗留缺陷（R43-01）**：`close_pending` 消费协议存在 flusher 线程盲区——当最后一个回调持有者恰为 Flusher 线程时，三处认领点全部拒领，实例与 JNI 全局引用永久泄漏（方向为泄漏而非 UAF）。该缺陷先于本批存在（R41-01 拒领纪律引入），本批 seq_cst 收敛未触及。
5. **复审当场修正 2 条低危**（密钥残留清零 + 测试覆盖回退，见 §三），另登记 3 条提示项。
6. **回归验证**：Windows MSVC x64 `build-full`（zstd + AESGCM）全量重建成功（56/56），`ctest` 4/4 全绿；JniKeyCache 5/5 定向复跑通过（含当场新增 2 用例）。

---

## 二、新发现缺陷详解

### ⚠️ R43-01（中危/递延销毁盲区）`close_pending` 在「最后持有者为 Flusher 线程」时无人认领 → 实例与 JNI 全局引用永久泄漏

- **位置**: [logx_internal.h `emit_event` L122-L134](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_internal.h#L122-L134)、[flusher.cpp `flusher_main` 退出段 L1281-L1298](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L1281-L1298)、[logx_c.cpp `logx_close_ex` 递延分支 L505-L511](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L505-L511)
- **成因链**（触发前提：`active_ops == 0` 且 Flusher 线程正处于 `emit_event` 回调内，即 `in_callback ≥ 1` 全部由 Flusher 贡献——`flusher.cpp` 内 IO/DEGRADED/RECOVERY 事件发射点众多，如 L164、L210、L604，现实可达）:
  1. 外部线程调用 `logx_close_ex`：非 flusher 线程 → 检查 `in_callback > 0 || active_ops > 0` 命中（`in_callback=1`）→ 置 `close_pending=true`、`flusher_exit=true`、signal，返回 `LOGX_OK`，**不 join、不销毁**；
  2. Flusher 线程 `emit_event` 退出点：`rem==0`、`active_ops==0`、`close_pending==true`，但 `on_flusher==true` → 依 R41-01 纪律拒领；
  3. Flusher 主循环因 `flusher_exit` 退出，进入退出段：仅消费 `close_on_flusher`（此场景恒为 false）→ 跳过；
  4. 线程结束。`close_pending` 永久无人消费 → `destroy_instance` 永不执行 → **实例内存/staging/文件句柄泄漏 + `jni_destroy_trampoline` 不执行 → Java `g_provider` 全局引用与 `kp_ctx`/`err_ctx` 泄漏**。
- **性质**: 资源泄漏（非 UAF），方向安全但不可恢复；tester `applyAndRestart` 反复 close→reopen 场景下每次命中都稳定泄漏一个实例的全部资源。
- **修复方案**（择一）:
  - 首选：`emit_event` 退出点当 `active_ops==0 && close_pending` 时允许 Flusher 自领，按线程身份传 `destroy_instance(inst, /*is_flusher_thread=*/on_flusher)`——自毁路径已支持（不 join 自身、跳过 `flush_blocking`），与 `close_on_flusher` 语义合流；R41-01「严禁消费外部 close_pending」纪律可收窄为「存在外部活跃 op 时严禁」，因该纪律本意是防外部 op 尾读 UAF，`active_ops==0` 时外部仅剩已返回的 close 调用方（递延语义下不再触碰 inst）；
  - 或：`logx_close_ex` 区分「`in_callback` 全部由 Flusher 贡献」（需 per-线程归属计数）直接走 `close_on_flusher` 通道——改动面更大，不推荐。
- **回归探针建议**: 新增定向用例——写入线程全部收敛（`active_ops==0`）后，由 Flusher 事件发射路径（如构造 IO 失败）持有回调期间并发 close，断言 `on_destroy` 最终执行、实例回收。

---

## 三、当场修正项（复审期间落地，未提交）

| 项 | 级别 | 位置 | 修正 |
|---|---|---|---|
| 跨 owner 驱逐槽位明文密钥残留 | 低危 | [jni_key_cache.h `populate_slot`](file:///d:/Project/Android_Project/LogX/logx/android/cpp/jni_key_cache.h#L87-L92) | 接管槽位前无条件 `memset` 清零旧密钥字节，消除「被驱逐属主明文残留至 `GetByteArrayRegion` 覆写前」窗口；头注释契约同步补注 |
| 测试覆盖回退 | 低危 | [test_jni_string.cpp](file:///d:/Project/Android_Project/LogX/logx/tests/unit/test_jni_string.cpp#L389-L456) | 补 2 用例：`CrossOwnerSameKidIndependentSlots`（恢复 R42 重构删除的跨 owner 同 kid 独立槽/互不干扰/单方失效场景）与 `CrossOwnerEvictionZeroesEvictedKeyBeforeHandover`（断言驱逐接管时旧密钥先清零、幸存槽完好） |

## 四、提示项（不强制）

- **P-1**: `emit_event` / `OpGuard` / `flusher_main` 三处在全部操作已 seq_cst 后追加的独立 `atomic_thread_fence(seq_cst)` 为冗余（无害），可保留作意图文档，建议注释注明「冗余、防御性保留」。
- **P-2**: `in_callback` / `active_ops` 递增侧仍为 `acq_rel`——合法（递增为保守方向，Dekker 配对建立在 seq_cst 递减侧），建议注释说明理由防后人误改。
- **P-3**: `invalidate_owner(owner, gen==0)` 通配语义实际不可达（`gen` 恒 ≥1），可留可删；`jni_key_cache.h` 容量契约措辞已在当场修正中更正为「全局跨 owner 共享容量」。

---

## 五、逐项核验通过清单

| 提交 | 审查项 | 结论 | 说明 |
|---|---|---|---|
| 2783c90 | ACT-1 seq_cst 全序屏障 | ✅ | Dekker 双窗口封死；`exchange` 认领唯一；flusher 兜底双检与 OpGuard 竞态逐步推演无漏认领路径 |
| 2783c90 | ACT-2 generation 三元匹配 | ✅ | ABA 假命中根治；`gen` 发布时序安全；满槽 nullptr 不破坏在持槽 |
| 2783c90 | ACT-3 满槽显式降级 | ✅ | trampoline nullptr 路径无泄漏、attach/detach 对称保持 |
| 2783c90 | ACT-4 `.kotlin/` 忽略 | ✅ | 无问题 |
| 2783c90 | 新增/改写单测 3 项 | ⚠️ 部分 | ABA/满槽/失效清零断言齐备；但删除了跨 owner 独立槽场景（当场已补，见 §三） |
| bacbc31 | R42 报告行动项勾选 | ✅ | 勾选与 `2783c90` 实际落地一致，无虚报 |

---

## 六、自动化测试套件验证记录

Windows MSVC x64（`build-full`，zstd + AESGCM 开启；环境前置：`Launch-VsDevShell.ps1 -Arch amd64`）全量重建后：

```text
1/4 Test #1: logx_unit_tests ..................   Passed   16.48 sec
2/4 Test #2: logx_hotpath_alloc_tests .........   Passed    0.23 sec
3/4 Test #3: logx_stress_tests ................   Passed    0.88 sec
4/4 Test #4: logx_crash_fork_tests ............   Passed    1.96 sec

100% tests passed, 0 tests failed out of 4
```

JniKeyCache 定向复跑（含当场新增 2 用例）：

```text
[==========] 5 tests from 1 test suite ran.
[  PASSED  ] 5 tests.
```

---

## 七、行动项清单 (Action Items)

- [ ] **ACT-1（P1）**: 修复 R43-01——`emit_event` 退出点允许 Flusher 在 `active_ops==0` 时自领 `close_pending`（`destroy_instance(inst, is_flusher_thread=true)`），或等价方案；新增「Flusher 回调内递延 close」定向回归用例断言 `on_destroy` 执行。
- [ ] **ACT-2（登记）**: 提示项 P-1/P-2 注释补注（冗余 fence 说明、递增侧 acq_rel 理由），可并入下次代码批次顺带处理。
