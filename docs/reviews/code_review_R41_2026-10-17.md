# LogX 代码审查报告 R41（R40 修复与 4 项拍板落地批次独立复审）

- **审查日期**: 2026-10-17
- **审查对象**: `a4b44e3..HEAD` 共 5 个提交
  - [`066dcd9`](https://github.com/zzlfeng/LogX/commit/066dcd9) fix(jni,pal,test): R40-01 代理对下溢 + R40-02 macOS 守卫（含 5 项 CESU-8 单测）
  - [`8494cb6`](https://github.com/zzlfeng/LogX/commit/8494cb6) fix(decoder): D-3 落地——清 carry 补齐断裂分片流告警
  - [`4041caf`](https://github.com/zzlfeng/LogX/commit/4041caf) fix(pal): P-2 落地——Win32 互斥锁 SRWLOCK 化
  - [`7baae2b`](https://github.com/zzlfeng/LogX/commit/7baae2b) fix(core,jni): J-1/F-2 落地——递延 close 生命周期闭环与 on_destroy
  - [`5a65567`](https://github.com/zzlfeng/LogX/commit/5a65567) fix(core,jni,test): F-12/J-6 落地——kid 密钥契约收敛与 JNI 多槽缓冲
- **审查方式**: 逐提交 diff 推导 + 生命周期/并发时序全链路推演（close 三态：同步 / Flusher 递延 / 调用栈递延）+ Windows 本地全量重建与 `ctest` 回归。
- **基线**: `5a65567`（领先 `origin/master` 8 个 commit）

---

## 一、总体结论

1. **5 个提交中 3 个完全通过**：R40 两项修复（066dcd9）、D-3（8494cb6）、P-2（4041caf）质量合格，无新发现。
2. **J-1/F-2 落地（7baae2b）引入 1 条高危回归**：`flusher_main` 新增的 `close_pending` 消费缺少活跃引用计数守卫，与 OpGuard/emit_event 的认领协议构成竞态，重新打开了本应被消除的 UAF 窗口（R41-01）。
3. **F-12/J-6 落地（5a65567）核心契约正确**，但 JNI TLS 多槽缓存存在跨实例陈旧密钥污染（R41-03，中危）与槽位轮转覆盖在持指针的理论隐患（R41-04，低危）。
4. **回归验证**：Windows MSVC x64 `build-full` 全量重建成功，`ctest` 4/4 全绿（unit 184 项含本批新增 15 项用例、hotpath 零分配、50 线程压测、crash-fork）。

---

## 二、新发现缺陷详解

### 🚨 R41-01（高危/并发 UAF）`flusher_main` 消费 `close_pending` 无引用计数守卫

- **位置**: [flusher.cpp `flusher_main` 退出段](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L1274-L1282)
- **成因链**:
  1. [logx_c.cpp `logx_close_ex`](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L501-L507)：仅当 `in_callback > 0 || active_ops > 0` 时才置 `close_pending`，并注释约定「由最外层 OpGuard 析构完成递延销毁」；
  2. 本批在 `flusher_main` 退出时新增 `close_pending.exchange(false)` 认领并 `destroy_instance(inst, /*is_flusher_thread=*/true)`，**未校验 `active_ops == 0 && in_callback == 0`**；
  3. 时序：线程 U2 正在 `logx_write`（持 OpGuard，`active_ops=1`）→ U1 调 `logx_close_ex` 走递延分支（置 `close_pending` + `flusher_exit` + signal）→ Flusher 立即退出循环并赢得 `close_pending` 认领 → `delete staging / mu / inst` → **U2 仍在写入已释放的 staging → UAF**。
- **竞态性质**: 与 [OpGuard 析构](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L288-L294) / [emit_event 消费](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_internal.h#L121-L131) 的认领是掷硬币——op 持有者赢则安全，flusher 赢则提前销毁。`close_pending` 只会在存在活跃持有者时置位，flusher 侧消费**既冗余又有害**。
- **测试为何全绿**: 窗口为微秒级，单测无「close 递延 + 并发写」的定向压力用例；50 线程压测不覆盖 close 竞态。
- **修复方案**: 删除 `flusher_main` 中 `|| close_pending` 分支，仅保留 `close_on_flusher` 认领；`close_pending` 的闭环完全交还给 OpGuard 析构与 `emit_event` 尾段（两者均已带计数守卫）。
- **备注**: 违反项目硬约束「Flusher 线程路径不得触发递延销毁」的原始动机（R19 F-2 只豁免 join 自身），与 R40 处方（方案 A：out_closed 信号 + JNI 侧递延）一致。

### ⚠️ R41-02（中危/存量缺口）`close_on_flusher` 自毁路径同样无活跃引用守卫

- **位置**: 同上，[flusher.cpp L1276-L1281](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L1276-L1281)
- **分析**: 错误回调在 Flusher 线程内调用 `logx_close_ex` → `close_on_flusher` 认领自毁，但此刻其它用户线程可能仍持 `active_ops`（正写 staging）→ 同类 UAF。该缺口系 R19 F-2 存量，本批 J-1/F-2「闭环生命周期」未覆盖此场景，与 R41-01 同根。
- **修复方案**: `close_on_flusher` 认领前校验 `active_ops == 0 && in_callback == 0`；若非零则改置 `close_pending`（Flusher 已退出，由最后 op 的 OpGuard 消费并执行 `destroy_instance(false)`，其 `thread_join` 对已退出线程合法）。

### ⚠️ R41-03（中危/密钥污染）JNI TLS 密钥槽缓存无属主键，跨实例返回陈旧密钥

- **位置**: [logx_jni.cpp `jni_key_provider_trampoline`](file:///d:/Project/Android_Project/LogX/logx/android/cpp/logx_jni.cpp#L176-L246)
- **分析**: `g_tls_key_cache` 仅以 `kid` 为键、`thread_local` 且实例销毁时不清理。导出走 Kotlin `Dispatchers.IO` 线程池（线程复用）：实例 A（kid=7→密钥 K1）关闭后，实例 B 以同 kid=7、新密钥 K2 重开，B 的导出命中复用线程上的缓存 → 返回 K1 → 解密失败或错钥。tester 的 `applyAndRestart`（改密钥重启）正是现实触发路径。`jni_destroy_trampoline` 只在销毁线程上清理，无法覆盖其它线程的 TLS 残留。
- **修复方案**: `TlsKeySlot` 增加 `JniKeyProviderContext* owner` 字段，命中与写入均要求 `owner == ctx && kid` 双匹配。

### 🔍 R41-04（低危/契约缝隙）TLS 槽轮转淘汰可能覆盖 core 仍在持有的密钥指针

- **位置**: 同上（`kMaxTlsKeySlots = 4`，`g_tls_next` 轮转复用）
- **分析**: [logx_c.h J-6 规约](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_c.h#L105-L108) 要求「不同 kid 必须拥有互不覆盖的独立存储区域」，但同线程并发活跃 kid > 4 时，轮转淘汰会覆写 `ActiveFileTarget.key` 仍指向的槽 → 指针内容变为他钥。当前单 kid 配置不可达；M6 多密钥轮换启动前必须处理（扩槽 + 仅淘汰无在持实例引用的槽，或契约明文限制 ≤4）。

### ℹ️ R41-05（提示/存量）认领模式理论竞态

- **位置**: [OpGuard 析构](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L287-L294) 与 [emit_event 尾段](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_internal.h#L121-L131)
- **分析**: 认领赢家 `destroy_instance` 释放 `inst` 后，败方线程可能仍在读取 `inst->close_pending`（fetch_sub 之后的尾读）→ 理论 UAF 窗口。系 R39 F-1「先自增后校验」既有模式的残留，非本批引入；根治需带外共享控制块（如堆上 `atomic` 认领标志），建议登记为已知限制，M 系列专项再议。

---

## 三、逐项核验通过清单

| 提交 | 审查项 | 结论 | 说明 |
|---|---|---|---|
| 066dcd9 | R40-01 代理对修复 | ✅ | `cp = 0x10000 + (hi<<10) + lo` 数学正确（hi/lo 已是 10 位偏移）；5 项边界单测覆盖 U+10000 / U+10FFFF / 双 Emoji / 混合往返 |
| 066dcd9 | R40-02 macOS 守卫 | ✅ | `#if !defined(__APPLE__)` 后 `monotonic_cond_` 恒 false，`wait_timeout` 与 CLOCK_REALTIME 回退路径自洽 |
| 8494cb6 | D-3 清 carry 告警 | ✅ | `feed(is_whole)` / `drop_carry(reason)` 双路径告警齐备；`decode_file_records` 将 warning 挂入 errors 诊断，排障可追溯；`in_split` 未置位时静默，无虚报 |
| 4041caf | P-2 SRWLOCK 化 | ✅ | PAL 互斥接口仅 `lock/unlock`（无 timed/try 缺口）；全 core 审查未见持锁再取锁路径（`emit_event` 均在锁外、`flush_blocking` 仅短临界区读计数）；`logx_pal.h` 契约注释同步为非递归；`SRWLOCK_INIT` + `InitializeSRWLock` 冗余但无害 |
| 7baae2b | J-1/F-2 契约面 | ✅ | `logx_close_ex` 三态与 `out_closed_immediately` 语义正确；`on_destroy` 在 `delete inst` 后触发且携带 key_user/err_user；JNI trampoline 的 GetEnv/Attach/Detach 与 GlobalRef 释放正确；`nativeClose` 不再提前释放上下文（符合硬约束）；OpGuard 新增 `in_callback==0` 守卫正确；emit_event 消费路径三计数守卫正确 |
| 5a65567 | F-12 kid 绑定 | ✅ | 文件头 `kid` 写读偏移一致（`put_u16(hdr+10)` / `get_u16(hdr+10)`）；续写以文件头 kid 派生密钥 + DEGRADED 告警；`drain_once` 重试探测改用 `c->target.kid`；单测覆盖 kid 不一致续写 + export 解密闭环 |
| 5a65567 | logx_c.h 契约文档 | ✅ | kid 单实例不可变、多 KID 轮换延至 M6 的表述与实现一致 |

---

## 四、自动化测试套件验证记录

Windows MSVC x64（`build-full`，zstd + AESGCM 开启）全量重建后：

```text
1/4 Test #1: logx_unit_tests ..................   Passed   15.13 sec
2/4 Test #2: logx_hotpath_alloc_tests .........   Passed    0.32 sec
3/4 Test #3: logx_stress_tests ................   Passed    1.09 sec
4/4 Test #4: logx_crash_fork_tests ............   Passed    2.06 sec

100% tests passed, 0 tests failed out of 4
```

---

## 五、行动项清单 (Action Items)

- [x] **ACT-1（P0，必须）**: 修复 R41-01——删除 `flusher_main` 对 `close_pending` 的消费，递延销毁认领唯一归属 OpGuard/emit_event；补齐 `R41_DeferredClose_ConcurrentWrites_NoUAF` 10 轮高并发写定向单测。
- [x] **ACT-2（P1）**: 修复 R41-02——`close_on_flusher` 认领前增加 `active_ops == 0 && in_callback == 0` 守卫，非零时转 `close_pending` 移交并提供双检兜底。
- [x] **ACT-3（P1）**: 修复 R41-03——抽出 `jni_key_cache.h`，`TlsKeyCache` 增加 `owner` 属主指针（ctx+kid 双匹配），并在销毁时主动清理当前线程槽位且安全清零内存。
- [x] **ACT-4（P2/M6 前置）**: 处理 R41-04——`logx_c.h` 注释补充说明当前单线程活跃不同 kid 数量 ≤ 4，且 `allocate_slot` 优先复用无效空槽；更大规模动态多 KID 轮换规划于 M6 专项落地。
- [x] **ACT-5（登记）**: R41-05 认领模式理论竞态已登记为已知限制（无外部堆控制块时的既有模式），待生命周期专项统一根治。
