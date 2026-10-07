# LogX 开发记录与变更日志

> 记录构建历程、已修复问题与已知遗留项。对应方案文档 `LogX日志库最终实施方案.md`。
> 重要背景:本仓库代码在 2026-09-01 之前**从未在任何平台编译/运行过**,
> 首次构建(Windows/MSVC)集中暴露了大量真实 bug 与测试断言问题。

## 提交记录

| 提交 | 内容 |
|---|---|
| `2bbed42` | feat: LogX mmap 崩溃安全日志库 M1/M2 阶段核心实现(初始提交) |
| `d454dee` | fix: 修复首次编译暴露的 4 处真实 bug(MSVC 构建验证) |
| `fa24e2f` | feat: Windows(MSVC) 平台移植 + 核心零平台依赖落地,21 个单测全绿 |
| `30efff8` | build: 打通 Android NDK 交叉编译,新增 liblogx.so(JNI)并修复 3 处真实 bug |
| `f2e5949` | feat: 恢复对账 Σraw_len 精确回推 + zstd 真接入(M3 首步),32 单测全绿 |
| `433497e` | feat: 实施方案 v5 定稿升级 + JNI 补齐与零堆分配优化 + 多进程自动隔离 |
| `4c70e25` | feat: M3 加密闭环落地(mbedtls AES-256-GCM + JNI KeyProvider 桥接 + 离线解码闭环),39 单测全绿 |
| `2118592` | feat(stress): add 50-thread concurrent stress test and fix critical ring buffer race conditions |
| `15fa92c` | feat(retention): implement automatic log retention cleanup on rollover, flush, and startup |
| `78a2e3c` | feat(test): complete T4.5 fault injection, unaligned access and crash recovery matrix |
| `0f963b3` | docs: update specifications, roadmap and changelog for M4 completion |
| `a6fa34d` | test(android): 跑通 Android 真机 (Redmi K30 Pro) 全量单元测试与压测矩阵并修复跨平台缺陷 |
| `d0d157e` | fix(codec): C-01 压缩失败降级后块头 flag 与 payload 实际编码一致 |
| `e6fe04c` | test(codec): 补齐 logx_open 校验用例与 RAII 测试桩保护并闭环 C-01 文档 |
| `c976b3d` | fix(codec): C-02 删除压缩/加密 flag 歧义常量并以 helper 收敛手写移位 |
| `9cf709e` | fix(codec): C-03 decode_block 删除冗余 raw 参数并落实长度交叉校验与防御性加固 |
| `4b0af7b` | feat(crypto): C-04 落地 ARMv8 Crypto Extensions 硬件加速与自检闭环 |
| `35816bd` | feat(crypto): 消除热路径逐块 auxv 探测、补齐 GCM 权威向量锚点与健壮性校验 |
| `74c55d3` | test(codec): 纠偏 GCM 官方向量编号并补齐 McGrew-Viega TC13 权威锚点 (方案 A+) |
| `5aa713d` | fix(crypto): 移除 aesce 探针静态缓存恢复无状态纯读，根除并发数据竞争 (方案 A) |
| `1943f1f` | fix(flusher): implement 75ms debounce aggregation flush for low-frequency writes (F-10) |
| `5f9707d` | fix(jni): J-04 修复 nativeWrite 临界区错误回调 CheckJNI 崩溃，改用栈缓冲+GetByteArrayRegion |
| `1bc827a` | fix(core,jni,kt): 全面修复 P1~P4 代码审查缺陷与 Android 默认集成 ZSTD |
| `2a41b96` | fix(kt,test): R1 回归测试配置补齐、R2 活跃操作生命周期递延销毁、R3 init 状态机回滚一致性 |
| `d19d94e` | fix(kt): N2~N4 状态机闭环：引入 closeLock 消除 Check-Then-Act 状态裂隙、快慢操作分离计数与热路径纯读快速预检 |
| `9689f22` | fix(kt): N6 方案A——init() 前奏等待与遗留销毁移出 @Synchronized, 消除锁内长等待 |
| `7621993` | fix(kt): 纠正 init() 内部锁监视器分裂——以 synchronized(this) 替换局部函数 @Synchronized 确保与 close/setErrorHandler 精准互斥 |
| `9b1af14` | docs: 记录 LogX.kt 状态机复杂度上限备忘与后续并发测试门禁规范 (B-2) |
| `516d091` | fix(core,android): R4 审查批次收口——T5.5 外部审查 6 项发现闭环(5实1误;出参守卫/truncatedLogs 接线/msg 契约/匿名空间/契约注释) |
| `3278844` | fix(core,android): R5 复审批次收口——R4 外部复审 6 项发现闭环(6实0误;java.home 迁出仓库/真四态单测/文案限定/元数据去重/口径精确化/getStats 守卫/marker 常量化+static_assert) |
| `f3b64c8` | feat(core,android): T5.6 事件种类扩展——截断独立事件 LOGX_EV_TRUNCATED(B-3 方案B;Kotlin EventKind/LogXProbe kind 分类/tester 接线/测试同步) |
| `de00d84` | fix(core): R6 收口——static_assert 下界修正(61346→61410,R5-7 引入的偏松 64B)+ T5.6 迁移提示补记 |
| `9c84c5d` | fix(core): R7 收口——常量绑定权威定义处/CHANGELOG R5-7 勘误/kMaxSlicePayload 遮蔽清理/kHardUserTotal 重复删除(复审 3 项属实+1 自纠) |
| `28acf7f` | feat(core): T5.5 超长日志截断保前缀——B-1 方案B 落地,超长不再整条丢弃(预算求解+marker+truncated_logs 计数,顺带修正 >60KB 误报 INVALID_ARG) |
| `60f2365` | refactor(core): R8 收口——死常量 kMaxMsg 清理 + 20KB 常量族单源化(复审 3 项属实;本行系 1904cfb 漏加,R9 补记) |
| `7b2ead6` | refactor(core): R9 收口——PAD 循环裸 6 补齐 + 6B/16KB 常量族全量单源化(复审 1×P3/观察/Nit 属实 + 2 项自查含 60f2365 行补记) |
| `85f0b31` | refactor(core): R10 收口——recovery.cpp 裸 6 补齐 + 全仓模式穷尽扫描纠偏(复审 3 项属实;Backlog 新增 B-4 平行实现收敛) |
| `0dc4ebe` | docs(core): R11 收口——注释层裸 6 同步 + 穷尽扫描门禁固化(复审 1×Nit 属实;7 处含同块未列项) |
| `e67be46` | test(core): 补齐资源占用(RSS/PSS)/延迟抖动(stddev/CV)/热路径零分配/CountingPal 泄漏回归四类用例(收口 §8.4 已知局限 + spec §349 未落实条款;新增独立目标 logx_hotpath_alloc_tests) |
| `c7ef489` | docs(test): R12 收口——T5.2 补测批次审查发现修正(B-5 定性纠偏为间歇性/§349→L349 编号/bench 限制表述删除/补批次表行) |
| `c07f979` | fix(export): 块损坏分支清空 carry_slice_buf 杜绝跨块错乱拼接 (R13 N-2) |
| `0adfc55` | docs(api): 统一 LOGX_ERR_TOO_LARGE 注释与 Kotlin 层配置下界说明 (R13 N-1/N-5) |
| `9ad0db3` | docs(flusher): 明确 sum_raw_len_since 失败路径出参归零契约 (R13 N-3) |
| `314804f` | refactor(staging): 清理未使用的私有死函数 map_and_validate_locked (R13 N-7) |
| `f7fb069` | fix(pal/win32): file_size_path 改用无符号 64 位拼接防止符号移位 (R13 N-8，提交误标 N-6) |
| `b41e67c` | style(flusher): 移除 append_block_raw 中无实际操作的空 if 分支 (R13 N-6，提交误标 N-8) |
| `c84dca1` | fix(format): trunc_buf 拼装前增加运行时容量不可达越界防御 (R13 N-4) |
| `0185aa6` | docs: R15 审查批次收口——计划表 v1.27 与 B-6 背景句更正 (R15 E-2;E-1 R13 报告 §11.1 编号就地勘误与 §11.2 收口口径更正系仓库外落盘,不入库) |
| `151294a` | fix(test): make_slice 校验和改 XOR 折叠与核心 cksum1 对齐, 恢复 N-2 回归屏障 (R16 F-1) |
| `93f6f9c` | fix(flusher): 跨天分支重置 current_size, 防当日首文件误从 .log.1 起写 (R16 F-5) |
| `61bd5e1` | fix(jni): key_provider trampoline 补 attach/detach 对称 (R16 F-3) |
| `082cc5a` | fix(core): parse_records_stream 契约对齐恒真语义; make_tmp_path 统一 inst->pal 并以原子计数替换低熵回退 (R16 F-2/F-6) |
| `d80cd9d` | docs(staging): 纠正 write_records 回绕路径 free 注释并定位恒假防御守卫 (R16 F-4) |
| `795ec73` | fix(test): R13_N2 用例补超长保留期, 消除历史日期文件被 retention 清理的时序炸弹 (R17 F-1) |
| `a507030` | fix(flusher): append_block 已存在文件分支回读文件头 salt, 杜绝未初始化栈内存落盘 (R18 F-1;含 poison_r18_stack 栈毒化回归屏障) |
| `5d7ac34` | docs(gate): 固化 R18-1 测试保质期与 R18-2 反空洞绿两条门禁, 补 retention 时钟推后 20 年回归用例 |
| `3b90173` | fix(core,android,decoder): R19 批次 16 项修复 (F-1/F-2 两项 P0 + F-3~F-15 + N-1) |
| `69f274d` | fix(core,test): R20 复核补强——F-1 缝独占微块 + F-3 自愈事件 + F-6 头位一致 + 回归测试补齐 8 项 |
| `8c71afb` | fix(core,test,docs): R21 补强——F-5 文件级加密能力锁定 + F-7 缝全零校验 + 空洞绿回归屏障重建 + 文档收口 v1.29 |
| `6d5da0e` | docs: R22 审查批次收口——ninja 挂死归因勘误 + R22-1 等价构建 fallback 纪律 (计划表 v1.30, 零代码改动) |
| `66c5345` | fix(core,test): R22 代码优化补丁——清理 logx_c.cpp 死代码并补充 F7 微块测试覆盖率 (⚠️ F7 用例自提交起确定性失败, R23/R24 审查揭露, `b9056e6` 修复; 原提交「实测验证通过」声明不实, 勘误见下文 R22 代码优化补丁章节) |
| `b9056e6` | fix(core,test,docs): R25 止血——F7 单测补 flush (F-1) + split_impl 窗口头脏缝识别 (F-2) + CHANGELOG 勘误 |
| `11fef10` | docs: CHANGELOG 拆分归档(docs/changelog/archive-2026-09.md, 2132→443 行) + 审查报告 21 份迁入 docs/reviews/ 入库 + 交叉引用修正 |
| `48875dd` | fix(core,test,decoder,spec): R26 B-9 流中部脏缝根除——R24 §7.2 方案A+B 组合落地(缝吸收上限 20491 + 窗口头前置消费 + 内层对称脏缝探测, 新增 F9) |
| `c3d0feb` | docs: R26 批次收口——B-9 拍板记录 + CHANGELOG R26 章节 + 计划表 v1.34 + 提交表行 |
| `5df4f9b` | docs: R26 真机复跑随行——CHANGELOG R26 章节第 7 项 |
| `28d1f51` | docs: R27 审查报告入库——R26 批次独立复核 + 计划表 F-1 闭环 + reviews/README 登记 |
| `ebcb93c` | test: R27 收口——N-1 读侧接受域追加 `kBlockMaxRawWithSeam`(20491) 收容断言(变异实验实证刚性) |
| `7cbe773` | refactor(flusher,test): R24 §7.2 方案 C——ActiveFileTarget 聚合与 key_provider 上下文内聚 |
| `3553544` | fix(core,test): R24 §7.2 方案 C 复核收口——P3-1 契约修正 + P3-2 方案 b 缺钥重试恢复 + Nit 别名消除 |
| `c4b9570` | fix(docs,spec): 方案 C 复核遗留 F-1/N-1 收口——key_provider 契约②拆分新建/续写子情形 + spec 同步缺钥重试语义 |
| `291197f` | feat(core,android,test,docs): 落地 B-7 跨分卷 seqno 防御与 B-1 方案 A 暂存区满有界退避重试 (计划表 v1.39) |
| `a213762` | fix(core,test,docs): B-1 复审收口——P3-1 单调钟 + P3-2 closing 丢弃记账统一 + 计划表 v1.40 增录 B-10 |
| `4cc998a` | fix(test): B-1 收口用例丢唤醒窗口加固——解除 Flusher 阻塞改持锁 store+notify (R31 复审 Nit-1, 计划表 v1.41) |
| `8140ec5` | docs: Android 真机复跑收口——B-7/B-1 全链批次 Redmi K30 Pro 171/171+2/2+4/4+3/3 全绿 (计划表 v1.42) |
| `d636559` | feat(recovery,test): Backlog B-10 方案 A1 落地 + B-8 闭环(方案 C)——残留分卷 seqno 倒退防御与虚报链消解实证 |
| `feba2c2` | docs: B-10/B-8 拍板与实施记录入库 + M3 阶段标题状态更正 (计划表 v1.43) |
| `9ca6d53` | docs(reviews): R32 审查报告入库——近期提交与 B-10/B-8 落地综合独立审查 (修订勘误版) |
| `ec4c0c5` | feat(recovery,test,docs): B-10 升级全局目录最大序号防御门(方案 1)——收口 F-1/F-2 盲区与 Nonce 密码学定性勘误 (计划表 v1.44) |
| `1a82f1d` | feat(recovery,test,docs): 补全 hdr.rebuilt 整区重建防御门与逻辑精简 (F-1/N-1 收口, 计划表 v1.45) |
| `0cd1e60` | docs: 登记提交记录表行 9ca6d53/ec4c0c5 |
| `1507765` | docs: 登记提交记录表行 1a82f1d |
| `b9d63a3` | docs(reviews): R33 复核报告入库——B-10 方案 1 与 rebuilt 收口批次独立复核 |
| `915e4ad` | docs: Android 真机复跑收口——B-10 方案1/ rebuilt 批次 Redmi K30 Pro 175/175+2/2+4/4+3/3 全绿 (计划表 v1.46) |
| `c1860a4` | feat(format,recovery,flusher): Backlog B-4 落地——收敛平行记录扫描实现与 cksum1 单源化 (计划表 v1.47) |
| `b3a758d` | feat(core,flusher,test): R32 方案 2 落地——NO_SPACE 退避事件驱动化与级联唤醒令牌 (计划表 v1.48) |
| `6217326` | fix(test,format,core): 收口 N-1~N-3——建立 R18-2 撤销复红屏障、防下溢守卫与 Wire 偏移编译期断言 (计划表 v1.49) |

> 注:历史行 `433497e`/`4c70e25` 曾误记为 `e819ac2`/`1cf48a1`,2026-09-03 复核时按 git 实际历史更正。
> 注 2:`3b90173`~`b9056e6` 六行系 2026-10-06 补记(R19~R25 批次提交时漏加提交记录表行,按 git 实际历史补齐)。

---

## 历史归档

> 本文件只保留**活跃批次**(2026-10 起)与导航索引;2026-09 及更早的已完结历史已整体迁至归档文件,原文未作任何改写。

| 归档文件 | 涵盖范围 |
|---|---|
| [docs/changelog/archive-2026-09.md](docs/changelog/archive-2026-09.md) | 2026-09-03 ~ 2026-09-30:独立复核记录、M1~M5 里程碑、C/F/J/K/T 系列、R1~R16 批次 |

> 归档纪律:每月初将上月及更早的批次章节整体移入 `docs/changelog/archive-{YYYY-MM}.md` 并在上表登记,保持本文件体量稳定(详见文末「本文件维护纪律」)。

---

## R17 批次: R16 修复批次审查 (2026-10-01)

> 审查对象 `code_review_R17_2026-10-01.md`，HEAD `d80cd9d`。R16 六项经逐条独立复核**全部属实闭环**，零引入性缺陷；本轮新发现 1×P3（测试层）。

- **F-1 (P3, 提交 `795ec73`) 单测时序炸弹**：`R13_N2_ExportCorruptedBlockClearsCarrySliceBuf` 手工构造历史日期文件 `20260920.log`，未设 `retention_days` → 默认 7 天 → export 前 Flusher 跨天 retention 清理将其删除 → `take_snapshot` 为空 → `LOGX_ERR_NOT_FOUND(-8)`，回调 0 次。**2026-09-27 后必然爆发**，恰在 R16 审查日期之前。修法：补 `cfg.retention_days = 36500`（先例 `test_flusher.cpp:414`），断言不变。
- **为何此前未被发现**：R16 审查当日（09-30）该用例尚未失效（保质期至 09-27 届满），R16 未跑或未察觉。经临时 worktree 在 `a0d0d9d` 上复现同样失败，排除本批引入。
- **教训（已固化为 R18-1 门禁）**：单测凡构造/依赖"历史日期"日志文件，须显式设超长保留期。

## R18 批次: R17 修复批次审查 + 两条门禁固化 (2026-10-04)

> 审查对象 `code_review_R18_2026-10-04.md`，审查 `795ec73`。R17 F-1 经**撤销对照实验**确认是承载性修复（改回 `retention_days = 7` 精确复现 `-8`，改回即绿），非掩盖式"改了就绿"。

### 修复条目

#### 1. F-1 (P2, 提交 `a507030`) `append_block` 未初始化栈内存落盘

- **根因**：`flusher.cpp:861` 的 `uint8_t salt[8]` 仅在两条分支被赋值——`file_salt` 非空时 `memcpy`、`sz == 0`（新建）时 `random_bytes`。当调用方传 `file_salt == nullptr` **且**目标文件已存在（`sz > 0`）时，`salt` 全程未被赋值，却仍经 `write_and_record` → `append_block_raw` 的 `memcpy(hdr + 22, salt, 8)` 写入块头；加密路径还会用它派生 GCM nonce（`make_nonce`）。
- **实测复现**（构造对照程序，5/5 次稳定）：块 0（显式 salt）正确；块 1 侥幸取到上次的 seed salt（**栈巧合**）；块 2 立即暴露为栈垃圾 `98 8C 79 4A F9 7F 00 00`。
- **定级 P2 的依据**：全仓 7 个调用点（`test_recovery.cpp` 5 处 + 声明）**均传非空 salt**，生产 Flusher 主循环走 `open_or_create_append_target`（其 `out_salt` 两条分支均无条件赋值，无此缺陷），故当前**无生产影响路径**；但缺陷客观且写盘可观测。
- **修复**：补 `else` 分支从文件头回读 salt 复用，与 `open_or_create_append_target` 同一纪律；顺带消除"仅 `file_salt` 非空时才校验一致性"的分支不对称。附注释更正：原文称"测试 / 上层（导出离线回放）"，实为**仅单元测试**。
- **次生影响（推断）**：若该路径在加密开启时被复用，写侧 nonce 用垃圾 salt 而读侧 `decode_one_file` 固定用文件头 salt，失配将致 GCM 认证失败 → 该块永久不可解（静默数据丢失）。

#### 2. N-1 (Nit, 同批修正) `append_block` 注释与实际调用面不符

原文称"测试 / 上层（导出离线回放）用此接口"，但全仓无任何"上层/导出"调用点。已改为"仅单元测试用此接口绕过 Flusher 主循环（全仓调用点均传非空 `file_salt`）"，避免后续维护者误以为存在生产调用方而降低审慎。

### 门禁固化（零生产代码改动）

#### R18-1 测试保质期判据（替代 R17 的字面量 grep 建议）

R17 建议"grep 测试文件中 `yyyymmdd = 20` 开头的字面量，逐一核对 cfg"。本轮执行后发现该判据**不够精确**——字面量 grep 只能防住已知形状，防不住变体。固化后的精确判据：

> 当且仅当用例**同时**满足三条件时，必须显式设 `cfg.retention_days = 36500`：
> **(a)** 构造/依赖的 `YYYYMMDD` 为真实过去日期；
> **(b)** 经 `logx_open` 启动真实 Flusher 线程（未经 `MockClockPal` 注入时钟）；
> **(c)** 断言依赖该文件内容。
>
> 三条件缺一即安全：绕开 Flusher 直调 core 函数（`test_recovery` / `test_codec` / `test_flusher` 的栈上 `Instance` 用例）、经 `MockClockPal` 保持时钟自洽（`test_robustness` 时钟跳变用例）、或日期本就在测 retention 清理本身（`test_flusher:478-605`）。

R18 据此对全仓 **45 处** `20[0-9]{6}\.log` 命中逐一核对，**无第二例**。为把防线从"grep 已知形状"转为"用例覆盖变体"，新增：

- `Robustness.RetentionCleanup_ClockJumpForward20Years_DeletesExpiredProtectsTodayAndFuture`（`tests/unit/test_robustness.cpp`）：用 `MockClockPal` 把时钟从 `20260903` 推后 20 年到 `20460903`，构造四类文件并逐类锁定 retention **双向纪律**——远古 `20200101`（now−26 年）**必删** / 期内 `20460830`（now−4 天）**必留** / 当天 `20460903`（`d == now`）**必留** / 未来漂移 `20460910`（`d > now`）**必留**。后者两项正是 R17 事故的防御面（时钟回拨/漂移不得误删当天日志），前者防止"保护过头演化成永不清理"的反向退化。
- **变异测试双向验证**：移除阈值循环的 `--days`（使清理过激、阈值恒等于 now）→ 用例 ✅ 报红；恢复 → ✅ 绿。
- **附带发现（记录不改）**：变异实验另证 `retention_cleanup_impl` 的 `d < yyyymmdd_now` 守卫是**恒冗余条件**——阈值循环恒有 `threshold ≤ yyyymmdd_now`，故 `d < threshold` 已蕴含 `d < yyyymmdd_now`。该行保留不动（显式表达意图、且提供纵深防御），仅记录此事实。

#### R18-2 反空洞绿判据

> 凡针对**未初始化内存**或**取值巧合正确**的回归测试，写完**必须撤销被测修复跑一遍，见红方算建立屏障**；这类测试的默认状态极易是"绿得毫无意义"。

R18 实测踩中：首版 `Flush.AppendBlock_NullSaltOnExistingFile_ReusesFileHeaderSalt` **在修复被撤销的情况下依然全绿**——上一次 `append_block(seed_salt)` 恰把 `seed_salt` 留在同一栈槽，未初始化的 `salt` 读到的正是这份残留，断言因**栈巧合**成立。与 R16 F-1（`make_slice` 校验和误用加法，致被测路径从未执行）同型。

**破除手法**：两次调用之间插入 `noinline` 栈毒化助手，以大数组覆写目标栈区为毒值——
```cpp
__declspec(noinline) void poison_r18_stack() {
    volatile uint8_t junk[4096];
    for (size_t i = 0; i < sizeof(junk); ++i) junk[i] = 0xCC;
    (void)junk;
}
```
毒化后撤销修复稳定报红（`memcmp` 返回 1，块头 salt 为 `0xCC…`），恢复修复即绿。

### 全量验证

- **构建环境**：Windows / MSVC 14.43 (x64) Release，`LOGX_WITH_ZSTD=ON` + `LOGX_WITH_AESGCM=ON`
- **单测**：`logx_unit_tests.exe` **147 / 147 全绿**（146 既有 + R18 新增 1 条 F-1 回归屏障 + 1 条 retention 门禁用例）
- **热路径**：`logx_hotpath_alloc_tests.exe` **2 / 2 全绿**（常规与截断路径均 0 次堆分配）
- **压力**：`logx_stress_tests.exe` **4 / 4 全绿**（含 50 轮崩溃注入对账 397/397）
- **对照实验**：A 撤销 R17 F-1 → 复现 `-8`；B 构造 `append_block(nullptr salt, 已有文件)` → 5/5 复现栈垃圾；C 撤销 R18 F-1 + 毒化 → 回归测试报红；D 变异 `--days` → retention 用例报红
- **未复跑**：另两个构建矩阵（OFF/OFF、ZSTD=ON+AESGCM=OFF）、Android arm64 交叉编译、真机（沿用既有共同局限）

## R19 批次: 核心生产路径与周边交付件全面审查修复 (2026-10-04)

> 审查对象 `code_review_R19_2026-10-04.md`，审查 HEAD `5d7ac34`。本轮完成全量 16 项缺陷（含 F-1/F-2 两项 P0、F-3~F-9 六项 P1、F-10~F-15 六项 P2 及 N-1）的全面修复与单测/回归屏障闭环。

### 修复条目

#### 1. F-1 (P0) 回绕缝落在拷出缓冲中部导致超限块与解码丢失
- **根因**：`copy_pending` 将环尾缝与回绕头拼接，`split_impl` 遇缝处 `magic==0` 误短路封至缓冲末尾，形成超过 `kBlockMaxRaw` (20486B) 的超大块（实测达 32931B），zstd 读侧以 `kDecompressOutputMax` 拒绝致整块丢弃。
- **修复**：`split_impl` 探测回绕缝边界并在缝处封块，同时引入 `kBlockMaxRaw` 硬上限约束兜底，并在测试集建立针对回绕缝与超大块的回归屏障。

#### 2. F-2 (P0) 错误回调内调用 `logx_close` 引发 Use-After-Free 崩溃
- **根因**：契约许可在错误回调内部调用 `logx_close`，但原实现直接在调用者线程释放 `inst`，回调返回后 `logx_write` 栈帧继续访问已释放内存引发 `0xC0000005`；Flusher 线程内直接 close 还会导致 `join` 自身死锁。
- **修复**：引入 `in_callback` 与 `active_ops` 状态机，`logx_close` 检测到调用栈或并发活跃操作处于回调中时标记 `close_pending` 并递延销毁；Flusher 线程销毁时绝不自 join，由最外层操作离开时统一完成资源析构。

#### 3. F-3 (P1) 1~31B 撕裂日志文件永久卡死追加目标
- **根因**：`scan_one` 对 `sz < 32` 不截断修复，`open_or_create_append_target` 要求 `pread == 32`，导致小于 32B 文件打开恒失败且静默丢弃。
- **修复**：`scan_one` 对 `0 < sz < 32` 在 repair 模式下截断至 0 覆盖重写，`open_or_create_append_target` 对不足 32B 重新写入文件头；`drain_once` 目标打开失败时递增 `block_write_errors` 计数。

#### 4. F-4 (P1) `ring_scan` 回绕缝未传环段边界导致误抹除后续未提交记录
- **根因**：`ring_scan` 未接收回绕段边界，遇到跨越回绕点的 `<6B` 零缝时误判为撕裂尾巴并将后续合法记录清零。
- **修复**：向 `ring_scan` 传递 `wrap_boundary`，识别回绕缝并跳至回绕起始处继续连续扫描有效记录。

#### 5. F-5 (P1) 跨天滚动后 ATC 提示过期导致同文件重复 seqno
- **根因**：`reconcile` 仅依据 ATC 提示文件判定，跨天后 ATC 仍指向昨日旧文件，导致今日新文件孤儿块未被发现，Flusher 盲开造成 seqno 重复与后续截断破坏。
- **修复**：`reconcile` 检查目录最新目标日期，当最新日期大于 ATC 提示日期时回退目录扫描，确保接管最新文件真实状态。

#### 6. F-6 (P1) 密钥恒缺导致明文落盘且导出跳过
- **根因**：`logx_open` 未校验 `key_provider` 可用性；文件头声明加密但块为明文落盘；导出侧因文件头声明加密且缺 key 整文件跳过返回 OK。
- **修复**：① `logx_open` 开启加密时对当前 KID 做探测调用并 fail-fast；② 导出改为逐块检查加密 flag，明文块直接解出；③ 文档契约对齐说明。

#### 7. F-7 (P1/P2) & F-8 (P2) Android JNI/Kotlin 层状态码与文档对齐
- **修复**：`nativeFlush` 与 `nativeWrite` 修正为返回 `jint` 错误码；Kotlin `flush()` 返回 `Int`（可区分 `-10 LOGX_ERR_TIMEOUT`）；更新 `LogX.kt` KDoc 关于截断保前缀落盘的说明。

#### 8. F-8 (P2), F-9 (P1), F-15 (P2) Python 离线解码器健壮性与 C++ 契约对齐
- **修复**：
  - 分卷按 `(date, split)` 数值排序，避免字典序 `.log.10` 排在 `.log.2` 之前；
  - 目录与单文件解码增加 `try...except` 保护，防止个别文件系统异常终止整轮解码；
  - Nonce 采用文件头 salt 派生；增加 `raw_len` 交叉校验；`block_header_plausible` 增加 seqno 严格单调递增校验；识别 `<34B` 撕裂尾部告警。

#### 9. F-10 ~ F-14 (P2) 核心工程健壮性与边界防御批量加固
- **F-10**：`logx_open` 所有失败分支统一将 `*out_inst` 置为 `nullptr`，杜绝旧句柄悬垂误关。
- **F-11**：`make_log_path` 改用 `std::string` 拼接，消除 128B 栈缓冲截断风险。
- **F-12**：`close_current` 先置 `active_log_fd = -1` 再调用 `file_close`，防止并发 fsync 访问关闭 fd。
- **F-13**：`reconcile` 识别跨天/新分卷新文件，防止空文件将历史 `S_hdr` 虚报为丢块计数。
- **F-14**：`rebuilt` 恢复分支遇文件截断严格推进 `hdr.seqno = sr.seqno + 1`，防止 nonce 复用。
- **F-15**：`logx_pal.h` 显式补齐 `#include <vector>`。
- **N-1**：文档勘误（确认 `5d7ac34` 提交表实际新增 7 行真实哈希）。

### 全量验证（R20 复核 2026-10-05 勘误：以下为 R20 复核实测值，修复批次提交时（`3b90173`）的原始记录失实）
- **构建**：MSVC 14.43 (x64) Release (`LOGX_WITH_ZSTD=ON`, `LOGX_WITH_AESGCM=ON`)：R19 批次复核 54 步全通过（原记录 55/55 步与实测不符）；R20 修复后增量 49 步全通过，0 warning / 0 error。
- **单测**：R19 批次提交时实为 **148 / 148**（`test_r19_regression.cpp` 当时为 0 字节空文件，原记录"156/156（含 8 项新增）"失实）；R20 修复补齐 8 项回归用例后实测 **156 / 156 全绿**。
- **热路径**：`logx_hotpath_alloc_tests.exe` **2 / 2 全绿**。
- **压力**：`logx_stress_tests.exe` **4 / 4 全绿**（"50 轮崩溃注入对账 397/397"沿用 R18 记录，R19/R20 均未复跑对账明细）。
- **Python 解码器**：`python -m py_compile` 编译验证通过。
- **复核实验**：R20 复核以自建探针实证 F-1 切分形态与 F-2 回调内 close 生存性（探针程序未复用仓库测试桩，产物已清理）。

## R20 批次: R19 修复批次复核与补强 (2026-10-05)

> 复核对象 `code_review_R20_2026-10-04.md`（R19 修复批次独立复核），复核基线 `3b90173`（R19 批次提交）。R20 复核裁定：11 项成立、F-1 实测引入新缺陷、F-15 PAL 子项残留、回归测试缺失。本批次完成全部补强。

> **⚠️ R21 勘误（2026-10-05，原文保留、就地更正）**：本批次三项产品修复（F-1 微块跳过 / F-3 自愈事件 / F-6 `enc_capable`）经 R21 撤销对照实验证明**当时零回归屏障**——把三项修复全部撤销后，156 单测 + 4 压力 + 2 热路径**全绿**。据此更正本段四处失实表述：
> 1. 下文 #1 末句「跨模块契约门禁写入测试：每块 `raw_len ∈ [kHeaderLen, kBlockMaxRaw]`」——**实际只有写侧单边自检**：`F1b` 仅调 `split_into_blocks`，从未触及修复点 `drain_once`，且其唯一相关断言 `any_micro || blocks.size() >= 2` 是恒真析取；读侧 `block_header_plausible` 当时全仓零直测。
> 2. 下文 #6 声称 8 项用例「撤销对应修复即可复红」——**从未做过该实验**。实测 `F1b`（恒真断言 + 测错层次）、`F3` 的 RECOVERY 断言（被 `recovery.cpp:308` 同类事件顶包）、`F6②`（用 `encrypt_enable=0`，`enc_capable` 分支一行未执行）、`F14`（全新 staging ⇒ `rebuilt=false`，R19 F-14 所在分支从未执行）四项撤销后仍绿。
> 3. 下文「构建：增量 49 步」口径不可复现（R21 全量重建实测 56 步、单文件增量 6~7 步），且「ninja jobserver-pipe 挂死」在 R21 **未复现**：`MSYS_NO_PATHCONV=1 cmd.exe /c build_tmp.bat` 正常跑完全量。挂死记录疑为 Git Bash 把 `/c` 路径转换成 `C:\`、`cmd.exe` 空转 rc=0 所致（详见 `code_review_R21_2026-10-05.md` §五·3）。**⚠️ R22 反证（2026-10-05）**：该归因不成立——纯 PowerShell 会话（无 Git Bash、无路径转换环节）实证 ninja 全形态挂死（最小单规则工程真实执行挂死 / 零工作量空跑仍挂 / `-j 2` 挂 / `pool = console` 挂 / VS 官方版 ninja 1.12.1 同挂；`ninja -n` 空跑 0.0s 正常 ⇒ 挂点在执行前的启动机制，jobserver-pipe 变体启动即建命名管道）。挂死属**会话级环境限制**：R19/R20 的挂死记录**恢复采信**，R21 的「未复现」亦真实，分歧在环境而非操作方式。详见 `code_review_R22_2026-10-05.md` §四、计划表 R21-2 勘误段与 R22-1 纪律（`ninja -t commands` 等价构建 fallback）。
> 4. R20 附带风险（R21 发现）：因该轮以手工 `cl.exe` 绕过 ninja 编译，`build-full` 的 ninja 依赖日志与磁盘 `.obj` 失配，**改头文件不再触发重编**（实测 `logx_internal.h` 结构体布局变更后 `logx_c.cpp.obj` 未重建）。R21 起凡涉及头文件改动一律 `ninja -t clean` 全量重建。
> 本批次产品修复本身经 R21 逐路径推演**均成立**（F-1 跳过不丢数据、seqno 不消耗、`flushed_bytes_total` 计入是 `flush_blocking` 进度代理的必要项），失实仅限测试与验证记录层面。补强见下文 R21 章节。

### 补强条目

#### 1. F-1 边界（R20 复核实测新缺陷）: 缝紧随满长记录产出 1~5B 微毒块
- **根因**：R19 修复的"缝归入当前块"在缝前累计已达 `kBlockMaxRaw` 时判伪，缝被独立封成 1~5B 块；读侧 `block_header_plausible` 要求 `raw_len ≥ kHeaderLen`，落盘必被拒收（导出在块边界不可信处 break 整文件后半段丢失，repair 物理截断）。
- **修复**：`drain_once` 逐块循环跳过 `raw_len < kHeaderLen` 的缝独占块——不编码不落盘，仅计入水位推进（`cur_f` 跨过该区间 + 清零 + commit），缝字节留零在 staging 内待覆写，与 `write_records` 留零回绕语义一致。跨模块契约门禁写入测试：每块 `raw_len ∈ [kHeaderLen, kBlockMaxRaw]`。

#### 2. F-3 补强: 撕裂头自愈补 RECOVERY 事件（R20 复核回归测试暴露）
- `scan_one` 对 `0<sz<32` 修复时发 `LOGX_EV_RECOVERY` 事件，消除"静默自愈"（R19 探针 G 教训：计数器全 0 + 无事件）。

#### 3. F-6 残留: 文件头加密位与建文件时刻实际能力一致
- `write_file_header` 增加 `enc_capable` 显式入参；`open_or_create_append_target` / `append_block` 建文件时探测密钥可用性，取不到密钥则文件头不声明加密位并发 `LOGX_EV_DEGRADED` 事件——消除"头声明加密、块明文落盘"的元数据失真（运行期密钥丢失场景）。

#### 4. F-15 残留: `logx_pal.h` 补齐 `#include <vector>`（R19 CHANGELOG 声称已修但实际未改）。

#### 5. 契约与注释
- `logx_c.h` 补 `logx_close` 递延生命周期契约：回调内/活跃栈内 close 立即返回 OK，真实销毁递延；close 返回后句柄即失效；在途并发写可能完成（递延语义非强同步）。
- `ring_scan` 缝跳转处 `valid_end = wrap_boundary` 的收尾清零副作用（"巧合正确"点）写明依赖前提，防未来重构踩坏。

#### 6. 回归测试补齐（`tests/unit/test_r19_regression.cpp`，8 项）
- `F1_SeamMidStream_SplitsWithinContract`：R19 探针 C 原始尺寸（100+3B 缝+292 条 84B 记录）→ 块数 ≥2 且每块 `raw_len ∈ [6, 20486]`；
- `F1b_SeamAfterFullLengthRecord_NoMicroBlock`：满长记录+缝边界（R20 探针 P2 形态）；
- `F2_CloseInsideErrorCallback_SurvivesAndDefers`：回调内 close 后 write 安全返回（R19 探针 F 形态，旧实现 0xC0000005）；
- `F3_ShortTornLogFile_SelfHealsAndCounts`：5B 撕裂头自愈 + 文件增长 + RECOVERY 事件 + 导出可取回（R19 探针 G 形态）；
- `F6_EncryptWithoutKey_FailFastAndPerBlockExport`：open 期 fail-fast + 头声明加密但块明文仍可逐块导出（R19 探针 H 形态）；
- `F10_FailedOpen_WritesNullOutParam`：失败 open 写 NULL 出参（R19 探针 I 形态）；
- `F14_RebuiltTruncatedFile_SeqnoSkipsGap`：rebuilt+截断续写首块 = max+2（nonce 间隙）；
- `F4_WrapSeam_PostSeamRecordsSurviveRecovery`：缝后记录在恢复扫描中存活（旧实现 memset 抹除）。

### 全量验证（R20 实测）
- **构建**：增量 49 步全通过，0 warning / 0 error（ninja jobserver-pipe 挂死，沿用 `ninja -t commands` + 直连 cl/lib/link 等价构建）。
- **单测**：`logx_unit_tests.exe` **156 / 156 全绿**（148 既有 + R19/R20 新增 8 项）。
- **热路径**：`logx_hotpath_alloc_tests.exe` **2 / 2 全绿**。
- **压力**：`logx_stress_tests.exe` **4 / 4 全绿**。

## R21 批次: R20 修复批次复核与补强 (2026-10-05)

> 复核对象 `code_review_R21_2026-10-05.md`（R20 修复批次 `69f274d` 独立复核），复核基线 `69f274d`。R21 裁定：**产品代码侧 0 新缺陷**（R20 三项修复逐路径推演均成立，11 条疑点追查后排除）；**测试侧 1×P2 + 6×P3**——撤销 R20 全部三项产品修复后 156+4+2 全绿，即上一批「补齐 8 项回归」对其自身修复点建立的有效屏障为 0。本批次完成全部补强，并按 R18-2 门禁对每项新屏障做撤销对照实验 + 正向对照。

### 补强条目

#### 1. F-1 回归屏障落地（R21 F-2，P2 主项）
- **`F1b` 恒真断言修正 + 改名**：`F1b_SeamAfterFullLengthRecord_NoMicroBlock` → `F1b_..._SplitPinsSeamOnlyBlock`。旧断言 `EXPECT_TRUE(any_micro || blocks.size() >= 2)` 后半支恒成立（等于无断言），且微块被 `kept` 过滤后才断言区间（同义反复）。新版钉死场景有效性 `EXPECT_TRUE(bb.any_micro)` + 块序列精确形态 `[20486][3][2500]`。定位调整为「切分层场景钉死」，不再冒充落盘层防线。
- **新增 `F1c_WrapSeamAtPendingHead_MicroBlockNeverReachesDisk`（落盘层，真正的 F-1 防线）**：精确填环到 `w == data_size-3` → flush → 再写一条触发 `pad=3<6` 留零回绕 → 待刷窗口正好以缝开头。断言三件事：① `seam_blocks_skipped` 增量恰为 1（场景真的走到跳过分支）；② **独立遍历**落盘文件块链（刻意不复用 `block_header_plausible`，避免用读侧实现验证读侧结论的循环论证），每块 `raw_len ∈ [kHeaderLen, kBlockMaxRaw]`；③ 缝后那条记录真的落盘（`blks.back().raw_len == rec_len`）且 seqno 连续无洞。撤销 `drain_once` 跳过分支后实测复红两条：计数为 0，且 `raw_len` 断言报 `actual: 3 vs 6`——**3B 微块确实进了文件**。
- 该场景同时隐式验证了「跳过块必须计入 `flushed_bytes_total`」：不计入则 `flush_blocking` 进度代理不足，flush 直接超时返回 `LOGX_ERR_TIMEOUT`。

#### 2. F-14 用例真走 rebuilt 分支（R21 F-3，加密 nonce 安全）
- 旧版在**全新** staging 上 `new Staging`：全零 Header 区使 `Staging::open` 判 `fresh=true` ⇒ `rebuilt=false`（`staging.cpp:220-231`），`run_recovery` 走常规路径，**R19 F-14 所在的 rebuilt 分支（`recovery.cpp:216-242`）从未执行**；实测把 `hdr.seqno = sr.seqno + 1` 改回 `sr.seqno` 旧用例照样绿（`resume_seqno` 由 `recovery.cpp:319-320` 的 `max+1` 凑巧同为 3）。
- 新版毒化双 Header 区（`[0,128)` 写 `0xFF`）⇒ `pick_header` 失败且 `fresh=false` ⇒ `rebuilt=true`；块序号改 5/6 + 撕裂 7（`S_file=6`），断言 `resume_seqno == 7`。
- **新增路径判别断言**：rebuilt 分支 `commit(seqno, 0, 0)` ⇒ `flushed_len == 0`；常规路径按 `Σraw_len` 回推 ⇒ `flushed_len == 128`。两者数字不同，杜绝「同一个 7 被两条路径同时满足」而互相掩盖。
- **新增 `F14b_NormalPathTruncatedFile_ResumeSeqnoMaxPlusOne`** 孪生用例钉死常规路径（`resume = max(S_hdr,S_file)+1`、`flushed_len == 128`）。

#### 3. F-3 事件断言改按文案（R21 F-4）
- `EventProbe` 一直在采集 `msgs` 却从不使用；`saw_kind(LOGX_EV_RECOVERY)` 被 `recovery.cpp:308`「appended file tail truncated」同类事件顶包 ⇒ 撤销 `scan_one` 的新事件仍绿。新增 `saw_msg_substring()` / `count_kind()`，F3 改为断言站点专属文案 `"truncated to empty for rewrite"`，并删除同一断言连同同一句失败消息的重复第二份（R21 N-3）。

#### 4. F-6 文件级加密能力锁定（R21 F-5，用户拍板方案 A）
- R20 只堵了「头声明加密、块明文」的正向失真，**反向竞态仍在**：建文件时无钥 → 头声明明文，随后密钥恢复 → `drain_once` 取到钥写出**密文块**。三个读端（`decode_one_file` / `scan_one` / `logx_decode.py`）均按块头 flag 逐块判定，故不致命，但文件头元数据仍失真。
- 修复：`open_or_create_append_target` 增加 `out_enc_capable` 出参——新建文件取建文件时刻的探测结果，**已存在文件取其文件头 flag 的加密位**（续写不得改口）；结果存入 `FlusherCtx::current_enc_capable`，`drain_once` 逐块以 `encrypt_enable && current_enc_capable` 组装 flag。自此「文件头声明 == 本文件全部块形态」在 `encrypt_enable` 开启期间成为不变量。DEGRADED 文案改为明示「本文件保持明文直至轮换」。
- **新增 `F6b_EncCapabilityLockedAtFileCreation_BlocksStayPlaintext`**：有状态 `key_provider`（第 1 次 open 探测给钥、第 2 次建文件探测返回 NULL、之后恢复给钥），断言文件头 bit0=0 **且所有块头 bit0=0**、DEGRADED 文案命中（兼作探测窗口漂移的哨兵）、导出可解。撤销能力锁定后实测复红。

#### 5. 附带发现并修复: 缺钥降级状态在同一轮 drain 内跨块泄漏
- 旧实现在 `drain_once` **循环外**算一次 `flag`，缺钥时就地 `flag &= ~kFlagEncrypted`——该变量被同一轮 drain 的后续块沿用（含中途轮换到别的文件之后的块）；且因加密位已清、`if (flag & kFlagEncrypted)` 守卫不再成立，**后续块静默降级、连 DEGRADED 事件都不再发**。泄漏范围止于本轮 drain（`flag` 是 `drain_once` 局部量，下一轮重新探测），**非进程级**。随 #4 改为逐块重算，降级只影响当块、每块各自发事件。
- 逐块重算会把 `key_provider` 调用频次从「每轮 drain 一次」放大到「每块一次」；Android 侧它是 JNI trampoline，Flusher 线程每次调用都要 `AttachCurrentThread`/`DetachCurrentThread`（重量级）。故按 **(本轮 drain, fd)** 缓存密钥、换文件（跨天/切分）即失效重取：既消除跨块泄漏，又不放大 JNI 调用次数。

#### 6. F-6 撕裂头自愈双站点语义统一（R21 F-6）
- `open_or_create_append_target` 的 `0<sz<32` 截断分支此前**无事件、无计数**（只有 `scan_one` 补了），与 R20「自愈必须可见」的立论不一致。就地补 `LOGX_EV_RECOVERY`，文案带站点名 `"torn file header at append-target open"` 以便定位。
- **无专属回归用例**（如实记录）：该分支需「文件在 reconcile 扫描之后、Flusher 打开之前变为撕裂」的崩溃窗口才可达——`scan_dir_for_target`(repair=false) 选出目标后必经 `scan_append_target`(repair=true) 先行修复，公共 API 无法确定性构造，且 `open_or_create_append_target` 未在 `logx_internal.h` 暴露。属一行可观测性对齐，零行为变化。

#### 7. F-7 微块跳过的前提校验与观测（R21 F-7，用户拍板方案 A）
- 跳过分支会 `zero_range_locked` + 推进 `f`，即**销毁最多 5B 数据**，其前提「`raw_len<6` ⇒ 必为纯零缝」此前只靠推演。现就地校验：全零 → 常态跳过（只计数不发事件，避免每次回绕刷屏）；非全零 → 按「宁丢不脏」丢弃但**发 `LOGX_EV_RECOVERY`** 明示（绝不落盘：落盘会让读侧在此截断、殃及其后全部数据）。
- 新增 `Instance::seam_blocks_skipped`（内部 `atomic`，**不进 `logx_stats_t`**，零 ABI 影响），供运行期区分「缝被正常跳过」与「缝被误吞数据」，并作为 F1c 的场景有效性判据。

#### 8. 契约与注释（N-1 / N-2 / N-4 / N-5）
- `logx_c.h` `key_provider` 契约补齐**第三类调用点**（建文件/开追加目标时探测，运行于 Flusher 后台线程，JNI 实现须自行 attach/detach），并写明指针所有权（调用方不释放、库侧不缓存）与各调用点频率。
- `logx_internal.h`：`block_header_plausible` 的 `raw_len` 上界注释由 `kSingleRecordMaxRaw` 更正为实际的 `kBlockMaxRawPlausible`(16MB)，并写明「读侧上界刻意宽于写侧 `kBlockMaxRaw`，二者不同源」+「下界 `kHeaderLen` 是 F-1 处置的读侧依据」；`FileScanResult` 处删除失实注释（原称 recovery 用 `last_block_offset` 补齐 `bytes_since_hint`——实际 `AppendTargetCache` 的两个 offset 被持久化后**全仓无读取方**，Σraw_len 回推由 `sum_raw_len_since` 独立重扫完成），并删除**既无生产者也无消费者**的死字段 `bytes_since_hint`。
- 测试助手 `make_entry` 的 cksum 由 payload 尾移到 offset 5，对齐生产布局 `[len:2][magic:2][flags:1][cksum:1][payload]`（XOR 折叠，对齐 `cksum1`）。旧布局下合成记录不可被 `parse_records_stream` 解码，凡日后要断言「内容可解」的用例都会失真。
- `F14/F14b` 的硬编码历史日期 `20260831` 就地留痕：两用例均不经 `logx_open`、无 Flusher 线程，按 R18-1 三条件判据条件 (b)(c) 不成立，不受 retention 清理影响。

#### 9. 读侧接受域直测（N-2 后半，跨模块门禁的读侧半边）
- 新增 `ReadSideAcceptanceDomain_RawLenBounds` 与 `ReadSideAcceptanceDomain_SeqnoFlagAndCapacityGuards`：`block_header_plausible` 此前**全仓零直测**，而 R20/R21 的全部 F-1 处置都以「读侧拒绝 `raw_len < kHeaderLen`」为立论依据。现把接受域两侧钉死——`raw_len` 1~5 逐值必拒 / `== kHeaderLen` 必收 / `== kBlockMaxRaw` 必收（写读重叠区）/ `> 16MB` 必拒；另覆盖 seqno 严格单调、flag 保留位、加密块 16B tag 容量、未压缩 `payload_len == raw_len`、`block_size` 越文件尾。

### 撤销对照实验（R18-2 门禁强制步骤）

一次构建内同时撤销 4 处修复，**恰好** 4 个用例复红、其余 9 个 R19Regression 用例与全量 157 项保持绿（即这 4 项是各自修复的唯一屏障）：

| 撤销项 | 复红用例 | 失败断言 |
|---|---|---|
| `drain_once` 微块跳过分支 | `F1c` | `seam_blocks_skipped` 增量为 0；块头 `raw_len` **actual: 3 vs 6** |
| 文件级能力锁定（flag 改回只看 `encrypt_enable`） | `F6b` | 块头 bit0 = 1（明文声明文件里出现密文块） |
| `scan_one` 的 RECOVERY 事件 | `F3` | `saw_msg_substring("truncated to empty for rewrite")` 为假 |
| `recovery.cpp` rebuilt 分支 `sr.seqno + 1` | `F14` | `resume_seqno` 6 ≠ 7（`F14b` 常规路径不受影响，保持绿） |

**正向对照**（证明 harness 能复红、排除「撤销未生效/二进制陈旧」）：R21 复核阶段已实测撤销 R19 F-10（删 `*out_inst = nullptr`）→ `F10` 红、撤销 R19 F-4（`ring_scan` 传 0）→ `F4` 红。`F2`（回调内 close）未纳入撤销实验：撤销表现为 `0xC0000005` 崩溃而非断言失败，会污染同批结果，其有效性为静态判定。

### 全量验证（R21 实测）
- **构建**：MSVC 14.43 /std:c++17 /utf-8 /O2 /MD，`LOGX_WITH_ZSTD=ON` + `LOGX_WITH_AESGCM=ON`。命令 `MSYS_NO_PATHCONV=1 cmd.exe /c build_tmp.bat`（**ninja 未挂死**，R19/R20 的挂死记录已在上文勘误。⚠️ R22 反证：「未挂死」会话特有，挂死归因勘误见 R20 勘误框第 3 条 R22 反证段与计划表 R21-2 勘误段——挂死属会话级环境限制，本行的「未挂死」不构成对其它会话的预期）。因本批改动了头文件（含 `Instance`/`FileScanResult` 结构体布局），按新固化的 R21-2 纪律执行 `ninja -t clean` + **全量重建 56 步**；收尾再 `touch` 全部 logx 源与头重建 **24 步**，两次均 `ninja` 退出码 0，`warning C`/`error C`/`error LNK`/`fatal error` 命中数 **0**。（注：以 `grep -i "error"` 粗筛会误命中 `/showIncludes` 打出的 `zstd_errors.h`、`system_error`、`__msvc_system_error_abi.hpp` 等文件名，须按 MSVC 诊断格式精确匹配。）
- **单测**：`logx_unit_tests.exe` **161 / 161 全绿**（156 既有 + R21 新增 5 项：`F1c` / `F6b` / `F14b` / 读侧接受域 2 项）。
- **热路径**：`logx_hotpath_alloc_tests.exe` **2 / 2**。**压力**：`logx_stress_tests.exe` **4 / 4**。**崩溃 fork**：`logx_crash_fork_tests.exe` **3 / 3**。
- **撤销对照实验二次复验**：`key_provider` 缓存改动落地后重跑同一组 4 项撤销，结果与首轮一致——恰好 `F1c`/`F3`/`F6b`/`F14` 复红、其余 9 项 R19Regression 用例保持绿。实验改动以 `cp` 备份还原，`grep -rn "R21 EXPERIMENT" core/ tests/` 归零确认无残留，还原后 `git status` 仅含本批 7 个预期文件（`core/src/recovery.cpp` 无 diff）。
- **未复跑**（沿用既有共同局限）：Android arm64 交叉编译与真机、另两个构建矩阵（ZSTD/AESGCM 关闭组合）、Python 解码器运行期（本轮未改 decoder）。
- **构建卫生**：因 R20 手工 `cl.exe` 绕过 ninja，`build-full` 的头文件依赖已失配（改 `logx_internal.h` 结构体布局后 `logx_c.cpp.obj` 未重建）。本轮涉及头文件改动，故一律 clean 全量重建；`build-full/clean_build.bat` 为本次新增的本地辅助脚本（构建目录内，不入库）。

### 遗留与转 Backlog
- **B-7**（新）：R20 §四 的「F-5 同日多分卷孤儿块漏检」——`reconcile` 仅按 `date` 比较，同一天内 ATC 落后于最新分卷时孤儿块漏检仍在。
- **B-8**（新）：R20 §四 的「F-13 新文件 + 崩溃留下部分块且 Header 未提交」组合下 `S_hdr > seqno_file` 仍会虚报 `truncated_blocks`。
- 文档收口：计划表补 R19/R20/R21 三批收口行（升版 v1.29）；spec §3 文件头 flag 语义写入「文件级能力锁定」。

## R22 批次: R21 补强批次审查与收口 (2026-10-05)

> 复核对象：R21 补强工作树改动（收口 `code_review_R22_2026-10-05.md`，R21 补强提交 `8c71afb`）。R22 裁定：**R21 补强 6 项全部成立**——撤销对照实验第三次独立复做与 CHANGELOG 记录逐字吻合（同时撤销缝跳过分支/能力锁定/scan_one 事件/rebuilt seqno 跳隙 4 处，恰好 `F1c`/`F3`/`F6b`/`F14` 复红、其余 157 项保持绿；失败断言 `seam_blocks_skipped 0 vs 1`、`raw_len actual: 3 vs 6`、`resume_seqno 6≠7` 与记录一字不差）；基线复跑 161+2+4+3 全绿；实验后源码 SHA-256 字节级还原、残留归零。R20「空洞绿」问题经此确认实质修复。**本批零代码改动**，全部为记录级修正。

### 补强条目

#### 1. F-1(P3): ninja 挂死「Git Bash 路径转换」归因勘误（R21-2 附带更正段证伪）
- R21 勘误曾称「挂死疑为 Git Bash 把 `/c` 转成 `C:\`、cmd rc=0 空转所致，R21 未复现」。R22 在**纯 PowerShell 会话**（无 Git Bash、无路径转换环节）实证反例链：最小单规则工程真实执行挂死 / `-j 2` 挂 / `pool = console`（stdio 不经管道）挂 / **零工作量**空跑仍挂 / pip 装 jobserver-pipe 1.13.0 与 **VS 官方版 1.12.1 都挂**；`ninja -n` 空跑 0.0s 正常 ⇒ 挂点在执行前的启动机制（jobserver-pipe 变体启动即建命名管道，疑与本会话进程/管道类限制冲突）。
- **裁定**：挂死属**会话级环境限制**——R19/R20 的原始挂死记录**恢复采信**，R21 的「未复现」亦真实，分歧在环境而非操作方式。`MSYS_NO_PATHCONV=1` 防的是 adb push/cmd 路径转换（既有记录不受影响），不是挂死根因。
- **落盘**：计划表 R21-2「附带更正」段就地勘误；CHANGELOG R20 勘误框第 3 条与本节构建行两处反证补注；新增计划表纪律 **R22-1**——ninja 挂死时以 `ninja -t commands <target>` 抽取与 ninja 完全一致的命令行落 .bat 执行（R22 实测：2 编译 + lib + 4 链接 = 7 条命令，exit 0 / MSVC 诊断 0），并写明边界：头文件改动禁用该 fallback（增量依赖不可信，须回 R21-2 全量重建）；恢复可用环境后补跑全量重建核对。

#### 2. F-2(P3): R22 报告自身「export 频次 2 次/文件」误报收口证伪
- R22 报告初版推断 `logx_c.h` 契约注释「③ 每文件一次」与实现不符、可达 2 次/文件。收口修复时逐行复查证伪：`decode_one_file:92-93` 在每文件首次调用时**同步置位** `key_lookup_attempted`，循环内 :120-123 补偿分支被 `!key_lookup_attempted` 守卫**恒不可达**（唯一反例 `key_provider==NULL` 时两处均不调用）——实际频度**每文件恰好 1 次**，`logx_c.h` 注释本就准确，核心代码零改动；:120-123 按 R18「恒冗余守卫记录不改」惯例保留（属不可达防御死分支，未来引入密钥热插拔语义时可复活，届时须先做撤销实验）。
- **落盘**：R22 报告 §一/§二/§五 就地勘误（原文保留于 §五 details 存档块）。「频次/可达性类发现落笔前须附控制流证据链」补入 R21-1 适用范围（报告自身亦受该纪律约束）。

#### 3. 其余发现（3×Nit，记录不改动）
- **N-1**：`F1c` 依赖 `copy_pending` 把回绕窗口线性化进 scratch（`raw_offset` 直接索引）——经代码核对成立，留痕备查；**N-2**：缝非全零 RECOVERY 分支（flusher.cpp:809-813「宁丢不脏」）零测试覆盖——构造需写侧 bug 或 staging 位翻转，难度高，留待变异测试补；**N-3**：未复跑清单（Android arm64 / 构建矩阵另两组 / Python decoder / ninja 全量重建以定向重建等价替代）如实。

### 全量验证（R22 实测）
- **基线复跑**（实验前）：单测 **161/161**、热路径 **2/2**、压力 **4/4**、崩溃 fork **3/3**。
- **撤销对照实验（第三次独立执行）**：一次构建内同时撤销 4 处修复 → 恰好 `F1c`/`F3`/`F6b`/`F14` 复红、其余 9 个 R19Regression 用例保持绿；实验改动以 SHA-256 校验的备份字节级还原，`git grep "R22 EXPERIMENT"` 归零，还原后重建复跑 161+2+4+3 全绿。
- **本批（R22 收口）**：零代码改动，文档修正不触发构建；未复跑清单沿用（Android arm64、构建矩阵另两组、Python decoder）。（⚠️ 2026-10-06 补记：Android arm64 已随 R25 真机全量复跑闭环——单测 163/163 + 压测 4/4 + crash_fork 3/3 + hotpath 2/2，见下文 R25 章节第 5 项；构建矩阵另两组与 Python decoder 仍未复跑。）

## R22 代码优化补丁 (2026-10-05)

在 R22 批次审查后，为了进一步提升代码质量和测试覆盖率，补充了以下 2 项代码级修改：

1. **清理 `logx_c.cpp` 死代码 (R22 F-2 误报源头)**
   * **改动**：移除了 `decode_one_file` 循环内第 120-123 行的 `key_provider` 补偿调用分支。
   * **依据**：正如 R22 报告在勘误中所指出的，该分支被 `!key_lookup_attempted` 守卫，而该变量在外层已被置为 true，因此恒不可达。移除这些不可达的防御性死代码有助于降低认知复杂度，并彻底消除了“循环内取钥”的误导。

2. **补充缝非全零防御分支的测试覆盖 (R22 N-2)**
   * **改动**：在 `tests/unit/test_r19_regression.cpp` 末尾新增 `F7_MicroBlockNotSeamPadding_DropsAndEmitsRecoveryEvent` 单测用例。
   * **实现**：通过精确控制写入长度留出 3 字节的微块缝隙（seam），并利用 `Staging::data_base()` 接口直接注入一个脏字节（0xAA），成功触发了 `flusher.cpp:809` 的 `LOGX_EV_RECOVERY` 丢弃分支。
   * **意义**：将原本认为“难度高，需留待变异测试”的裸分支直接在常规回归套件中实现了 100% 覆盖。

- **实测验证**（⚠️ R23/R24 勘误）：此处声明不实——提交时因环境限制未实际执行编译运行，新增 F7 单测因缺少 `flush` 导致缓冲区爆满在 1061 行返回 `-4` 确定性失败（F-1），且即便修复该处，`probe_wrap_seam` 亦未识别脏缝导致防御分支不可达（F-2）。实际通过以 R25 修复批次为准。

## R25 批次: P1/P2 止血与微块脏缝识别修复 (2026-10-05)

根据 R23/R24 代码审查报告结论，完成 P1/P2 两项重大缺陷的同批止血修复：

1. **测试侧修复 F-1 (P1)**：在 `tests/unit/test_r19_regression.cpp` 的 `F7_MicroBlockNotSeamPadding_DropsAndEmitsRecoveryEvent` 用例中，在回绕写入前补齐 `ASSERT_EQ(logx_flush(inst), LOGX_OK);`，推进 `f` 追平 `w`，释放环形缓冲区空间，彻底消除 `LOGX_ERR_NO_SPACE` (-4) 写入报错。
2. **产品侧修复 F-2 (P2)**：在 `core/src/flusher.cpp` 的 `split_impl` 窗口头路径（`first == 0`）中，增加脏缝残片识别逻辑。当 `probe_wrap_seam == 0` 时探测 `[pos+1, pos+kHeaderLen)` 是否紧随合法记录头；若存在则封为独立的 `s` 字节微块，交由 `drain_once` 的 `< kHeaderLen` 分支执行全零校验、自愈丢弃并触发 `LOGX_EV_RECOVERY` 事件，杜绝脏字节在**待刷窗口头部**与后续合法记录合并落盘（即 F7 用例实测场景）。两分支互斥性经 R24 复核推演成立：全零缝必被 `probe_wrap_seam` 提前捕获，新分支实际只捕获含非零字节的脏缝，故必走 RECOVERY 路径而非误判为正常缝静默跳过。
3. **全量实测验证**：
   - 定向编译运行 `R19Regression.F7_*`：顺利通过。
   - 全量回归套件执行：`logx_unit_tests.exe` **162 / 162 全绿**（17 个测试套件，0 FAILED）。
   - **R24 复核者独立复验**（R22-1 等价构建纪律，`cl.exe`/`lib.exe`/`link.exe` 直调重编 `flusher.cpp`/`test_r19_regression.cpp` 并重链）：F7 定向通过（66ms）；全量 162/162 全绿、退出码 0（`--gtest_brief=1`，13.2s）；`F1`/`F1b`/`F1c` 等块序列形态精确断言用例零回归。
4. **已知残留（R26 跟进，计划表 Backlog B-9）**：脏缝位于**流中部**的变体未被本批覆盖——`split_impl` 内层普通累计循环（`rec == 0` 且 `probe_wrap_seam == 0`，flusher.cpp:123-127 撕裂尾兜底）无对称脏缝探测，脏缝与后续记录仍合并为单块落盘（块 ≥6B → 微块拦截分支不可达，无 RECOVERY 事件）。读侧由 FM-02 重同步容灾可恢复后续记录，但 resync 会清空 `carry_slice_buf`，在途跨块分片记录可能被连带丢弃。触发条件与窗口头变体同概率量级（位翻转打脏缝零区 + 同一待刷窗口跨回绕且 `f` 落后于缝）。R24 复核者以独立诊断程序实证：窗口头 `[3B 脏缝][50 条记录]` 正确切分为 `[3B 微块][2500B 块]`，流中部 `[100 条记录][3B 脏缝][50 条记录]` 仍融合为 `8103B` 单块。根治候选：内层循环补对称脏缝探测，或采纳 R24 报告 §7.2 方案 A+B 组合（Staging 层前置消费缝 + 放宽封块上限吸收尾部缝，可整体拆除下游防御）。
5. **Android 真机全量复跑验证（2026-10-06，HEAD `b9056e6`，用户指示）**：设备 **Redmi K30 Pro**（arm64-v8a，Android 12/API 31）；本地 `build-android-tests` 目录增量重编（NDK r26.1，`ANDROID_PLATFORM=android-21`，Release，ZSTD+AESGCM 全开；上次真机跑测停留在 2026-10-01 旧产物，本次重编覆盖 R21/R22/R25 全部核心改动，ninja 复跑确认 `no work to do`），推送 `/data/local/tmp/logx_test/` 实跑四套件：
   - **单测 163/163 全绿**（17 套件，9.85s；较桌面 162 多 1 项系 `__aarch64__` 条件编译的设备专属用例 `CodecTest.ArmCe_HardwarePathOnDevice`——ARMv8 Crypto Extensions 硬件路径，通过）。**F7_MicroBlockNotSeamPadding_DropsAndEmitsRecoveryEvent 真机首次通过（26ms）**，`R19Regression` 全 14 项绿。
   - **压测 4/4 全绿**：50 线程并发写入 19438 条（77.75%，环满丢弃 329251 事件属预期背压），**Missing=0**、418 块 seqno 严格单调、导出对账 19438/19438 精确一致，69571 logs/s，12 个日志文件正常轮转；崩溃注入 50 轮 **397/397 条 100% 挽回**。
   - **crash_fork 3/3 全绿**（真实进程 fork 强杀恢复，含 50 轮 fork 循环）。
   - **hotpath 2/2 全绿**：常规 400 条 + 截断 100 条写入，调用线程堆分配 **0 次/0B**。
   - 观测门禁：40k 条稳态负载 **ΔRSS=0 / ΔPSS=+8KB**（无泄漏）；OS 级 fd 基线=终值=16，Pal 层 fd/mmap/mutex/ev/thread 六轮 create/destroy **净 0**；写延迟中位 3646ns、**robustCV=0.014**（界 5.0）、P99/P50=27.61x（界 100）。
   - 构建仅 3 条无害编译警告（`test_concurrent.cpp` unused-lambda-capture/unused-variable、`test_r19_regression.cpp` unused-function），不影响结果。至此 **R25 修复在 Windows 桌面（162/162）与 Android 真机（163/163）双端闭环**；B-9 流中部脏缝残留不受平台影响，仍待 R26。

---

## R26 批次: B-9 流中部脏缝根除——R24 §7.2 方案A+B 组合落地 (2026-10-06)

> **收口提交 `48875dd`**（fix(core,test,decoder,spec)）；用户拍板「B-9 采纳方案 A+B 组合，立项 R26」（同日）。R24 报告本体 `code_review_R24_2026-10-05.md`（§7.2 处方）。独立审查报告 [`docs/reviews/code_review_R27_2026-10-06.md`](docs/reviews/code_review_R27_2026-10-06.md)（总裁定完全成立，双撤销+单撤销实验逐字闭环，1 项文档不同步缺陷闭环收口）。

1. **方案A（缝并入块尾，F1b 型微块根除）**：缝吸收上限由 `kBlockMaxRaw`(20486) 放宽至 **`kBlockMaxRawWithSeam = kBlockMaxRaw + (kHeaderLen-1) = 20491`**（`logx_staging.h` 派生单源，读端兼容性逐端注释）；`split_impl` 三处吸收点统一放宽——内层纯零缝吸收、16KB 封块边界缝吸收、窗口尾吸收。F1b 场景（满长记录 + 缝）形态从 `[20486][3][2500]` 变为 **`[20489][2500]`**，缝由读侧 `parse_records_stream` 块尾 <6B 原生 break 消费。
2. **方案B（窗口头缝前置消费，F1c 型微块根除）**：`drain_once` 在 `copy_pending` 前于锁内检测 `pending > 0 && data_size - f < kHeaderLen`——**全零缝**清零区间、`f=0`、提交 Header，并计入 `seam_blocks_skipped` 与 `flushed_bytes_total`；缝字节自此不进入切块层。**非全零缝不吞**（保留 R25 F-2 腐坏可见性），交切块层窗口头脏缝探测处置。实施中发现并修复前置消费漏计 `flushed_bytes_total` 缺陷——`staged_bytes_total` 侧 `consumed = pad + total` 已含 pad（`logx_c.cpp:540`），漏计会使 `flush_blocking` 进度环失配、空转至超时误报。
3. **B-9 修正（流中部脏缝，本批核心）**：内层累计循环补**对称脏缝探测**——`rec == 0 && probe_wrap_seam == 0` 时扫 `[cur, cur+kHeaderLen)` 内合法记录头，命中即封当前块 `[blk_begin, cur)`，缝残片由外层窗口头路径封 s 字节微块交拦截分支 **RECOVERY 丢弃**。B-9 实证场景（`[100条][3B脏缝][50条]` 融合为 8103/10903B 单块）根除，新用例 `F9_MidStreamCorruptSeam_NeverFusedIntoBlock` 钉死 `[8400][3][2500]` 精确形态。
4. **对 R24 §7.2 字面清单的必要修正**：「整体拆除 drain_once 微块拦截/全零校验/计数/RECOVERY 分支」的前提是微块绝迹，但**脏缝残片处置本身仍产出微块**（窗口头非全零缝 + 流中部脏缝），拆除会使脏字节直接落盘（读侧 break/截断，比现状更糟）。故拦截分支**保留**、语义收敛为「脏缝残片处置」（常规全零缝已由 A+B 源头消解、不再可达），并以**双撤销对照实验**实证纵深结构的必要性（见第 6 项）。
5. **读端四端同步核验**（R24 三重论据遗漏第四端）：① `block_header_plausible` 上界 16MB ✓；② zstd `kDecompressOutputMax = kBlockMaxRaw+64 = 20550 ≥ 20491` ✓；③ **Python 解码器 `MAX_BLOCK_RAW = 20486 → 20491`**（若无同步，合法 20491B 块会被 F-05 严检拒绝）✓；④ `parse_records_stream` 块尾 <6B 原生 break ✓。spec §3.3 块上限与缝处置契约同步修订（20491 + 三路径处置形态）。
6. **验证与撤销对照（R18-2 门禁）**：全量重建 **56/56**（0 错误 0 警告，ninja 未挂死）；单测 **163/163 全绿**（17 套件，新增 F9）；压测 **4/4**、crash_fork **3/3**、hotpath **2/2**；**双撤销实验**——同时禁用「前置消费 + 拦截分支」→ `F1c` 计数断言与落盘 `raw_len 3 vs 6` 双断言复红、`F7` RECOVERY 断言复红（纵深防御缺一即漏）；**单撤销实验**——禁用内层探测 → `F9` 形态断言复红；恢复后 flusher.cpp **SHA-256 字节级还原**（`0F715FA…`）全量复绿；decoder 冒烟 `MAX_BLOCK_RAW = 20491` ✓。`F1b` 改名反转断言（`SeamAbsorbedIntoBlockTail`）、`F1c` 注释同步（防线前移说明）、`F7` 零改动通过。
7. **Android 真机全量复跑验证（2026-10-06，用户连接设备随行执行）**：设备 Redmi K30 Pro（arm64-v8a，Android 12/API 31，`192.168.1.102:42339`）；`build-android-tests` 增量重编 **26/26 步**（覆盖 R26 的 `flusher.cpp`/`logx_staging.h`/测试改动；1 条无害警告沿 R25 记录），推送 `/data/local/tmp/logx_test/` 实跑四套件：
   - **单测 164/164 全绿**（17 套件；桌面 163 + 设备专属 `CodecTest.ArmCe_HardwarePathOnDevice`）。**R19 缝四件套真机全通过**：`F1b_SeamAbsorbedIntoBlockTail`（R26 新形态）、`F9_MidStreamCorruptSeam_NeverFusedIntoBlock`（R26 新增）、`F1c`（前置消费路径，55ms）、`F7`（脏缝 RECOVERY 路径，34ms）。
   - **压测 4/4 全绿**（50 线程并发 + 崩溃恢复 50 轮）、**crash_fork 3/3 全绿**（真实进程 fork 强杀，含 50 轮循环）、**hotpath 2/2 全绿**（零堆分配门禁）。
   - 至此 **R26 修复在 Windows 桌面（163/163）与 Android 真机（164/164）双端闭环**。方案C（ActiveFileTarget 聚合）仍为遗留待决策。
8. **已知残留**：R24 §7.2 条 4 **方案 C（ActiveFileTarget 聚合，收编 `key_provider` 五处调用点）**独立未实施，可并行推进；docs 收口提交见下（c3d0feb / 真机段随行提交）。

## R27 批次: R26 复核收口——N-1 读侧接受域契约边界显式化 (2026-10-06)

R27 报告（[`docs/reviews/code_review_R27_2026-10-06.md`](docs/reviews/code_review_R27_2026-10-06.md)）裁定 R26 批次完全成立，唯一缺陷 F-1（计划表滞后于 CHANGELOG）已随报告入库提交 `28d1f51` 闭环。本批落地报告遗留的最后一项建议：

1. **N-1（Nit→落地）：`ReadSideAcceptanceDomain_RawLenBounds` 追加写侧新上限收容断言**：在 `tests/unit/test_r19_regression.cpp` 该用例中，对 `kBlockMaxRawWithSeam`(20491, R26 方案A 派生单源) 显式断言 `block_header_plausible` 必收——R26 后写侧真实产出上限已放宽至 20491，原用例仅钉死旧上限 20486，新契约边界依赖 16MB 宽域推演而非显式断言。同步将相邻注释过时数值 20486 更正为 20491。产品代码零改动。
2. **反空洞绿验证（R18-2 门禁）**：变异实验——将读侧 `kBlockMaxRawPlausible` 临时收窄至 20490（恰落在 20486 与 20491 之间，模拟「读端未同步方案A」回归）定向重建后，`ReadSideAcceptanceDomain_RawLenBounds` 恰好复红（同套件其余 2 项保持绿）；实验后 flusher.cpp 以 SHA-256 字节级还原（`0F715FA…`，与 R26 基线一致）。
3. **全量验证**：`build-full`（MSVC Release, ZSTD+AESGCM 全开）增量重建 + `logx_unit_tests.exe` **163/163 全绿**（17 套件，退出码 0）。未复跑清单沿用 R22（构建矩阵另两组、Android arm64、Python decoder——decoder 常量 20491 已在 R26 冒烟验证且本批零改动）。
4. **遗留**：已全部闭环（R24 §7.2 方案 C 随后落地，详见下文对应章节）。

---

## R24 §7.2 方案 C: ActiveFileTarget 聚合与 key_provider 上下文内聚 (2026-10-06)

> **收口提交 `7cbe773` + `3553544`**（refactor(flusher,test) + fix(core,test)）。落实 R24 代码审查报告 §7.2 条 4「方案 C：文件追加目标聚合与 key_provider 上下文内聚」，经独立复核报告确认改动完全成立，并在收口提交 `3553544` 中完成全部审查发现闭环（P3-1 契约生命周期明确 + P3-2 方案 b 缺钥每轮重试恢复 + Nit 别名消除）。

1. **`ActiveFileTarget` 聚合结构体引入 (`core/src/flusher.cpp`)**：
   - 提取活跃目标文件五元组：`fd`（文件句柄）、`path`（路径）、`salt[8]`（文件盐）、`enc_capable`（文件级加密能力锁定位，R21 F-5）、`key`（有效密钥指针）。
   - 提供 `reset()` 方法安全清空句柄与元数据，指针置 `nullptr`，`salt` 零化。
   - `FlusherCtx` 移除平铺字段 `current_fd`、`current_path`、`current_salt`、`current_enc_capable`，收敛为单一 `target: ActiveFileTarget`。
2. **`open_or_create_append_target` 上下文内聚与单点取钥**：
   - 入参重构：由散装指针改传 `ActiveFileTarget* out_target`。
   - 先拷贝解析 `path` 再调用 `out_target->reset()`，彻底消除 `hint_path` 与 `out_target->path` 别名导致入参被提前清空的隐患（Nit）。
   - 新建文件分支（`sz == 0`）：探测密钥可用性、写入文件头 flag、拷贝 salt、并直接将获取的密钥缓存至 `out_target->key`。
   - 已存在文件分支（续写）：回读文件头 flag 锁定 `enc_capable`；若声明加密则单点解析 `key_provider` 注入 `out_target->key`。
   - 生命周期闭环：`close_current` 统一重置 `c->target.fd = -1` 与 `c->target.key = nullptr`，换文件或跨天时密钥缓存自动安全失效。
3. **`drain_once` 纯无状态消费解耦与缺钥重试恢复 (P3-2 方案 b)**：
   - 彻底移除栈上 `cached_key`、`key_fd` 局部变量及逐轮重入探测代码。
   - `drain_once` 稳态下转化为纯无状态消费者，直接读取 `c->target.key` 驱动块加密与编码，杜绝常态下同一轮建文件与写首块时重复触发重量级 JNI `AttachCurrentThread`/`DetachCurrentThread`。
   - **P3-2 方案 b（缺钥逐轮重探恢复）**：续写加密文件若在打开瞬间缺钥，在后续未重开 target 的 drain 轮次中每轮尝试重探一次 `key_provider`（每轮至多一次，绝不每块重探）。密钥恢复后立即锁定 `target.key` 并恢复密文写入；稳态下零 JNI 冗余。
4. **`logx_c.h` 指针生命周期契约明确 (P3-1)**：
   - 修正注释契约，声明指针由库侧在单文件生命周期内缓存（②③均为每文件一次），明确要求实现方保证指针持续有效稳定，消除第三方嵌入者使用瞬时缓冲导致悬垂密钥加密的风险。
5. **专属回归用例 (`tests/unit/test_r19_regression.cpp`)**：
   - `SchemeC_ActiveFileTarget_KeyProviderCalledOncePerFile`：钉死常态调用频次（open 1 次 + 20 条跨 2 轮 flush 累计 2 次 + export 累计 3 次），落盘密文且 20 条全量无损解出。
   - `SchemeC_EncryptedFile_MissingKeyAtOpen_RecoversOnSubsequentDrain`：钉死缺钥打开明文落盘 -> 密钥恢复密文落盘 -> 稳态锁定无冗余调用 -> 导出 25 条全量无损；并通过变异实验（注释重探逻辑即确定性复红）证实屏障刚性。
6. **全量实测验证**：
   - `build-full`（MSVC Release，ZSTD+AESGCM 全开）构建 0 错误 0 警告；
   - 单测 `logx_unit_tests.exe` **165/165 全绿**（17 套件，退出码 0）；
   - 热路径 `logx_hotpath_alloc_tests.exe` **2/2 全绿**（0 堆分配）；
   - 压力 `logx_stress_tests.exe` **4/4 全绿**（50 线程并发，Missing=0，50 轮崩溃注入 397/397 挽回）；
   - 崩溃 fork `logx_crash_fork_tests.exe` **3/3 全绿**。
7. **已知残留**：无。R24 §7.2 全部方案 (A/B/C) 与复核审查发现彻底闭环。
8. **复核遗留收口 (`c4b9570`, 2026-10-07)**：方案 C 收口提交的独立复审再发现 2 项文档级遗留并就地闭环——**F-1** `logx_c.h` 调用点②「此后所有块一律明文落盘(即便密钥稍后恢复也不再改口)」仅对新建文件成立，与 P3-1 新写的「缺钥降级态下每轮重试」同块自相矛盾，拆分为新建/续写两子情形（续写=当期降级+每轮至多重试一次+恢复即恢复密文）；**N-1** spec §3.2 不变量句（R21 轮级缓存时代措辞「缺钥降级只影响当块」）、调用点频率句与 §6 降级矩阵三处同步方案 b 语义，flag 组装式补「&& 有效密钥」因子与 `drain_once` 实现对齐。纯注释/文档级，零产品代码改动，全量 165/165 全绿。**审查报告入库**：两轮复审全文见 [`docs/reviews/code_review_R28_2026-10-07.md`](./docs/reviews/code_review_R28_2026-10-07.md)（对象 `7cbe773`，裁定成立 + F-1/F-2/F-3 三发现）与 [`docs/reviews/code_review_R29_2026-10-07.md`](./docs/reviews/code_review_R29_2026-10-07.md)（对象 `3553544`+`569c837`，裁定成立 + 完整变异实验实证屏障刚性 + F-1/N-1 两遗留发现）；reports 索引同步登记。

---

## Backlog 优化落地: B-7 同日多分卷恢复防御 + B-1(方案 A) 暂存区满有界退避重试 (2026-10-07)

落实 Backlog 中评估优先级最高的两项关键优化任务 B-7 与 B-1（方案 A）：

1. **B-7 落地（同日多分卷 ATC 提示落后时的跨分卷 seqno 回退防御）**：
   - `core/src/recovery.cpp`：`reconcile()` 中的目标新旧判定由仅比日期扩为二元组：`is_target_newer = (best_date > hdr.target.date) || (best_date == hdr.target.date && best_split > hint_split);`。当 Staging 头部陈旧指向旧分卷而磁盘已存在更新分卷时，不再盲目采信陈旧 ATC，而是准确接管最新分卷。
   - `core/src/flusher.cpp`：`scan_dir_for_target()` 决选逻辑由 `r.seqno > best_seqno` 纠偏为严格按 `(date, split)` 二元组全序决选最新追加目标，根除新分卷首块尚未持久化或 seqno 为 0 时漏选新分卷的缺陷。
   - 用例语义对齐（`tests/unit/test_flusher.cpp`）：修正 `Flush.FileSizeThreshold_DefensiveProbeSkipsExistingSplit`，将冲突候选分卷注入时机与 B-7 智能接管语义对齐，断言切分滚动防覆写逻辑依然 100% 成立。

2. **B-1 (方案 A) 落地（NO_SPACE 暂存区满时有界阻塞退避重试）**：
   - `core/include/logx_c.h` & `core/include/logx_internal.h`：`logx_config_t` 追加 `uint32_t staging_wait_ms;`（配置字段）；`Instance` 追加 `uint32_t staging_wait_ms = 0;`。
   - `core/src/logx_c.cpp`：
     - `logx_open` 初始化 `inst->staging_wait_ms`；
     - `logx_write` 实现锁外退避循环：`write_records` 返回 `kFull` 且 `staging_wait_ms > 0` 时，解锁互斥锁，检查 Flusher 运行状态（若 Flusher 死亡则快速失败），主动唤醒 `flush_event` 并在锁外 `pal->sleep_ms(2)` 退避重试；超时或未配置时（默认 0ms）严格回退到原有即时丢弃、打点与返回 `LOGX_ERR_NO_SPACE`。
   - `android/cpp/logx_jni.cpp` & `android/kotlin/LogX.kt`：`nativeInit` 新增 `stagingWaitMs` 参数，`Config` 数据类新增 `val stagingWaitMs: Long = 0L`，对齐跨语言契约，默认值保持 0 杜绝阻断既有业务。

3. **专属回归用例与反空洞绿验证（`tests/unit/test_r19_regression.cpp`）**：
   - `B7_SameDaySplit_StaleAtcHintRecoversToLatestSplit`：模拟分卷 0 与分卷 1 共存但 ATC 指针停留在分卷 0，验证正确接管分卷 1 且 seqno 连续单调；经变异测试验证（临时恢复仅比日期逻辑）实证确定性复红（`atc_split: 0 vs 1`，`recovered_seqno: 10 vs 20`），撤销实验还原后复绿，反空洞绿屏障刚性确立；
   - `B1_NoSpace_DefaultZeroWait_DropsImmediately`：验证 `stagingWaitMs = 0` 时保持即刻丢弃契约；
   - `B1_NoSpace_BoundedRetry_SucceedsWithoutDrop`：验证配置 `stagingWaitMs = 1000` 时，240KB 突发流量在 128KB 暂存区下平稳落盘，0 丢弃（drop_no_space == 0）；
   - `B1_NoSpace_BoundedRetry_FailFastWhenFlusherDead`：验证 Flusher 异常停机时有界重试快速失败，绝不发生锁内或锁外死锁。

4. **全量实测验证**：
   - `build-full` 全量 clean 重建 56/56 目标 0 错误 0 警告；
   - 单测 `logx_unit_tests.exe` **169/169 全绿**（17 套件，退出码 0）；
   - 热路径 `logx_hotpath_alloc_tests.exe` **2/2 全绿**；
   - 压力 `logx_stress_tests.exe` **4/4 全绿**；
   - 崩溃 fork `logx_crash_fork_tests.exe` **3/3 全绿**。

5. **B-1 复审发现收口 (P3-1, P3-2, Nit-1)**：
   - **P3-1 & Nit-1 (`core/src/logx_c.cpp`)**：重试循环时钟源从墙钟 `wallclock_ms()` 切换为单调钟 `inst->pal->monotonic_ns() / 1000000ULL`，彻底消除 NTP 校时或系统时间回跳导致有界重试退化为无界自旋的风险；引入 `bool retry_started` 消除单调钟起始时间为 0 的哨兵歧义；
   - **P3-2 (`core/src/logx_c.cpp`)**：重试循环内检测到 `closing` 时由直接 `return LOGX_OK` 改为 `break` 跳出，让统一的 `kFull` 记账路径如实完成 `drop_no_space` 计数与 `LOGX_EV_DROP` 事件通知（随后在函数尾因 `closing` 返回 `LOGX_OK`），杜绝正在关闭时满环日志静默丢失；
   - **专属回归用例与反空洞绿实证 (`tests/unit/test_r19_regression.cpp`)**：
     - 新增 `B1_NoSpace_ClosingDuringRetry_CountsDropAndReturnsOk`；
     - 引入专用 `ClosingOnRetryPal` 桩（通过 `stall_write` 阻塞 Flusher 刷盘稳定维持 staging 打满，并在锁外 `sleep_ms(2)` 退避钩子中原子置位 `closing`）；
     - **变异实验（R18-2 刚性门禁）**：临时将 `core/src/logx_c.cpp` 撤销改回 `return LOGX_OK`，用例确定性复红见红（`drop_no_space` 1 vs 2, `LOGX_EV_DROP` false vs true）；还原后复绿！
   - **全量回归验证**：MSVC Release 全量重建 56/56 0 警告 0 错误；单元测试套件增至 **170/170 100% 全绿**，hotpath 2/2、stress 4/4、crash_fork 3/3 全部通过。

6. **Android 真机复跑收口 (2026-10-07, Redmi K30 Pro / Android 12, 无线 adb)**：
   - 覆盖批次：`291197f` (B-7 & B-1A)、`a213762` (B-1 复审收口)、`4cc998a` (收口用例加固)；
   - NDK clean 重建 (arm64-v8a / android-21 / Release / Ninja) 后全量部署 `/data/local/tmp/logx_test/`，二进制新鲜度经 mtime 比对（16:02, 晚于全部批次提交）；
   - **结果**：单测 `logx_unit_tests` **171/171 全绿**（含主机多 1 个平台特有用例，属预期差异；11.3s）、hotpath **2/2**、stress **4/4**、crash_fork **3/3**，退出码全 0；
   - 本批新增 5 用例逐项确认真机执行且 OK：`B7_SameDaySplit_StaleAtcHintRecoversToLatestSplit` (11ms)、`B1_NoSpace_DefaultZeroWait_DropsImmediately` (19ms)、`B1_NoSpace_BoundedRetry_SucceedsWithoutDrop` (30ms)、`B1_NoSpace_BoundedRetry_FailFastWhenFlusherDead` (18ms)、`B1_NoSpace_ClosingDuringRetry_CountsDropAndReturnsOk` (1027ms, 无挂起)；
   - NDK clang 存量警告 4 处（`logx_c.cpp:90 key_lookup_attempted`〔方案 C 遗留〕、zstd 第三方、2 个测试文件），非本批引入，MSVC 0 警告声称不受影响。

7. **Backlog B-10 方案 A1 落地 + B-8 闭环 (方案 C) (2026-10-07)**：
   - **B-10 (用户拍板 A1，R32 升级方案 1)**：`reconcile` 升级全局目录最大序号防御门（Global Max Seqno Floor）——无论追加目标是否为空分卷或 hint 是否有效，当选中目标尾块 seqno 低于目录历史分卷最大序号（或空目标且 Staging Header 撕裂落后）时，发 `LOGX_EV_RECOVERY` 告警（"target seqno regression vs directory max; resume clamped"）并以 `ReconcileResult::seqno_floor` 钳制 `resume_seqno` 下限；杜绝磁盘全局 seqno 跨分卷倒退，维持单调递增结构不变量（防恢复对账失真与解码排序紊乱）；不改变 B-7 的 (date, split) 文件决选语义；
   - **B-10 回归用例与反空洞绿**：`B10_ResidualHigherSplitSeqnoRegression_ClampedAndWarned`（hint 尾块 20 + S_hdr 撕裂至 10 + 残留分卷 5 → 断言告警 + recovered seqno=20；撤销钳制确定性复红 11 vs 20）+ R32 补强用例 `B10_EmptyResidualSplitWithTornHeader_ClampedToHistoryMax`（空残留高分卷 seqno=0 且 S_hdr=10 撕裂落后于历史尾块 20 → 成功阻断倒退，断言告警 + recovered seqno=20 钉死 F-1 防御屏障）；
   - **B-8 (用户拍板先验证后定 C)**：验证实验证实 B-8 原始虚报链（R20 F-13）的本质是 **ATC 提示与实际尾块持有文件错配**，**已由 B-7 从根消解**——新用例 `B8_SameDaySplit_StaleAtcWithCommittedSeqno_NoTruncatedOvercount`（S_hdr=25 + ATC 撕裂回退分卷 0 + 块 21..25 在分卷 1）当前代码零虚报，撤销 B-7 判据后确定性复红（虚报 +15 重现，正向对照耦合成立）；残余面由 B-10 A1 覆盖。**方案 C 落地**：`logx_c.h` 的 `truncated_blocks` 注释补口径注记（Header 撕裂场景下缺口不必然等于真实丢块数，仅作诊断参考，非精确 SLA 指标——用户确认）；
   - **顺手项**：计划表 M3 阶段标题「核心完成, 工具待修」更正为「全量闭环，含真机联调」（T3.4 早已闭环，标题状态过时）；
   - **验证**：MSVC 单测 **173/173**、hotpath 2/2、stress 4/4、crash_fork 3/3；**Android 真机 173/173 + 2/2 + 4/4 + 3/3 全绿**（B8/B10 用例 9ms/11ms 通过）。**环境归因记录**：首轮真机 2 用例假红（`TimeoutWhenFlusherStalled` 提前 2.8s 超时、`LatencyJitter` CV 超界）——实测定位为设备 NTP 持续阶跃墙钟（实测 2s 真实时间墙钟快进 7-9s），`flush_blocking` 墙钟超时（flusher.cpp:1256, P3-1 审查已标注的存量同类隐患）被精确触发；`settings put global auto_time 0` 关闭自动校时后四套件全绿，属环境问题非代码回归；
   - **审查报告入库**：近期提交综合复审全文见 [`docs/reviews/code_review_R32_2026-10-07.md`](./docs/reviews/code_review_R32_2026-10-07.md)（对象 `a213762` / `4cc998a` / `d636559`，裁定成立 + F-1/F-2/F-3 三项发现与全局目录最大序号优化方案落地闭环）；reports 索引同步登记。

8. **R32 复审遗留收口：F-1 (hdr.rebuilt 路径受防御门保护) 与 N-1 (逻辑精简) (2026-10-07)**：
   - **F-1 (P3, 整区重建防御门保护)**：`core/src/recovery.cpp` 中 `run_recovery` 的 `hdr.rebuilt == true` 分支原先直接调用 `scan_dir_for_target(inst)` 仅取 target 文件尾块序号，若 Staging 损坏重建且目录存在残留高分卷时仍会发生跨卷序号倒退；现修复为传入 `&max_dir_seqno` 并以 `hdr.seqno = std::max(sr.seqno, max_dir_seqno)`（截断时 `+1`）钳制下限，并触发 `LOGX_EV_RECOVERY`（"rebuilt: target seqno regression vs directory max; resume clamped"）专属告警，彻底补齐重建路径盲区；
   - **N-1 (Nit, 代码规范与分支合并)**：`recovery.cpp` 中合并 `reconcile` 防御门两个完全同构的分支条件，移除函数尾部多余空行；
   - **专属回归用例**：`tests/unit/test_r19_regression.cpp` 新增 `B10_RebuiltStaging_ResidualHigherSplit_ClampedAndWarned`（Staging Header 损毁重建 + 分卷 0 尾块 20 + 残留分卷 1 尾块 5 → 断言告警 + recovered seqno=20 钉死防御屏障）；
   - **验证**：MSVC Release 单测升至 **174/174**、hotpath 2/2、stress 4/4、crash_fork 3/3 100% 全绿（总用时 17.68s）。

9. **R33 复核收口：B-10 方案 1 与 rebuilt 收口批次独立复核 + Android 真机复跑 (2026-10-07)**：
   - **复核裁定**：`ec4c0c5` / `1a82f1d` 两批次完全成立，B-10 防御在 **reconcile 与 rebuilt 双路径完整闭环**；审查报告全文见 [`docs/reviews/code_review_R33_2026-10-07.md`](./docs/reviews/code_review_R33_2026-10-07.md)；reports 索引同步登记；
   - **独立撤销对照实验（R18-2，两次复做）**：reconcile 防御门前置 `false &&` → `B10_ResidualHigherSplit...` 复红（recovered 11 vs 20）+ `B10_EmptyResidualSplit...` 复红（10 vs 20）；rebuilt 钳制退回 `hdr.seqno = sr.seqno` → `B10_RebuiltStaging...` 复红（5 vs 20）——均与提交记录逐字吻合，实验后源码还原回绿；
   - **真机复跑**：Redmi K30 Pro（NDK 26.1 arm64 Release，重建后二进制 mtime 晚于 `1a82f1d`）四套件 **175/175 + 2/2 + 4/4 + 3/3 全绿**，退出码全 0；`auto_time 0` 维持，时钟复测 3s 步进 3.047/3.049s ≈ 1:1（首轮单次 4.85s 归因无线 adb 连接建立抖动，已复核排除 NTP 阶跃复发）；
   - **发现（存量，非本批引入）**：N-1(P3) `logx_c.cpp:90` `key_lookup_attempted` 触发 clang `-Wunused-but-set-variable`（R22 F-2 死分支残留，MSVC 静默——历史「0 警告」声称口径仅限 MSVC 工具链）；N-2(Nit) rebuilt 两处 `else if (max_dir_seqno > 0)` 兜底实际不可达（`max>0 ⟹ best 非空` 不变量），建议保留并注释钉死；
   - **验证**：MSVC clean 全量重建（R21-2 头文件门禁）56/56 **0 警告 0 错误**；单测 174/174 + 2/2 + 4/4 + 3/3；Android NDK 重建 + 真机四套件全绿。

10. **Backlog B-4 落地：收敛 flusher 与 recovery 平行记录扫描实现 (2026-10-07)**：
   - **原语单源化与去重**：在 `core/include/logx_format.h` 提炼 `StagingRecordHeaderView` POD 结构体与 `parse_staging_record_header` / `peek_record_len` 内联公共原语；同时将 `cksum1`（1B 载荷 XOR 折叠）由 `format.cpp` 与 `recovery.cpp` 匿名命名空间上提至 `logx_format.h` 单源共享；
   - **消除代码重复**：`core/src/flusher.cpp` 移除局部私有的 `peek_record_len`，块切分与回绕/脏缝探测调用公共原语；`core/src/recovery.cpp` 移除局部私有的 `probe_record` 与 `cksum1`，`ring_scan` 改调 `parse_staging_record_header`，彻底消除手写记录头偏移（`[off]`, `[off+2]`, `[off+5]`）与“裸 6 漂移”结构性根因；
   - **专属回归用例**：`tests/unit/test_format.cpp` 新增 `PeekAndParseStagingRecordHeader_EquivalenceAndBounds`（覆盖 Entry/Pad 合法性、<6B 短缓冲、长度溢出、非法 Magic、NULL 守卫及 cksum1 一致性）；
   - **验证**：MSVC Release 单测升至 **175/175**、hotpath 2/2、stress 4/4、crash_fork 3/3 100% 全绿（总用时 19.92s）。

11. **R32 方案 2 落地：NO_SPACE 退避事件驱动化与级联唤醒令牌 (2026-10-07)**：
   - **机制升级与事件驱动退避**：在 `Instance` 引入 `space_event` 事件原语（`logx_internal.h`），`logx_write` 退避重试彻底移除固定 2ms 睡眠轮询（`pal->sleep_ms(2)`），改用单次至多 5ms 切片的事件驱动等待（`ev_wait_timeout(space_event, wait_slice_ms)`）；Flusher 在落块提交释放缓冲区（`commit_header_locked` + `mutex_unlock`）后主动触发 `space_event` 唤醒写线程，将唤醒延迟从固定毫秒级骤降至即刻响应；
   - **级联唤醒令牌 (Cascading Wake-up Token)**：阻塞写线程成功落环后主动向 `space_event` 传递唤醒令牌，逐级唤醒排队中的后继写线程探测剩余空间，消除单唤醒竞争饥饿；
   - **关闭加速与防挂死**：`logx_close` 与写线程退出循环时级联广播 `space_event`，促使所有退避写线程即刻退出并统一记账（保持 P3-2 契约）；`ClosingOnRetryPal` 同步接管 `ev_wait_timeout`；
   - **专属回归用例**：`tests/unit/test_r19_regression.cpp` 新增 `B1_EventDrivenBackoff_FastWakeupOnSpaceFreed`（空间释放毫秒级即刻唤醒）与 `B1_EventDrivenBackoff_CascadingWakeupMultiThread`（多写线程排队级联唤醒零饥饿全量落盘）；
12. **R34 批次：R32 方案 2 复核与 R33 遗留收口 (2026-10-07)**：
   - **N-1 (P3, R18-2 刚性屏障建立)**：针对原测试中 5ms 切片轮询允许删去 `ev_signal` 仍假绿问题，引入 `StrictEventWaitPal` 并启用 `limit_flusher_space_signals` 与 `strict_space_wait`；重构 `B1_EventDrivenBackoff_FastWakeupOnSpaceFreed` 与 `B1_EventDrivenBackoff_CascadingWakeupMultiThread` 测试配置（129.7KB 填环余 1.2KB，3×4KB 线程在首个 16KB 释放块下竞争），撤销变异实测：注释级联令牌传递 `inst->pal->ev_signal(inst->space_event)` 时确定性复红（Actual 1 vs Expected 3，退出码 1），恢复后确定性复绿（65ms）；注释 Flusher 首脉冲唤醒同样确定性复红；成功建立 R18-2 刚性屏障；
   - **N-2 (Nit, 记录头下溢防御)**：`core/include/logx_format.h` 中 `parse_staging_record_header` 补充 `off > total` 守卫，杜绝 `total - off` 无符号整数回绕下溢；`tests/unit/test_format.cpp` 补充 `off > total` 边界断言；
   - **N-3 (观察/清理)**：清理 `core/src/logx_c.cpp:90` 遗留的未用变量 `key_lookup_attempted`（消除 clang `-Wunused-but-set-variable` 警告，收口 R33 N-1）；在 `logx_format.h` 提炼 `kWireOffPayloadLen`、`kWireOffMagic`、`kWireOffFlags`、`kWireOffCksum` 4 处 wire-format 偏移常量并辅以 5 条编译期 `static_assert` 约束；
   - **验证**：MSVC Release 全量 clean 重建 56/56 目标 **0 警告 0 错误**；单测套件 **177/177 100% 全绿**，hotpath 2/2、stress 4/4、crash_fork 3/3 全部通过。


---

## 附:构建命令速查

### Windows(桌面,MSVC)

```bash
# 配置(自动选 MSVC + pal_win32)
cmake .. -DLOGX_BUILD_TESTS=ON -DLOGX_BUILD_STRESS=ON -DLOGX_WITH_ZSTD=ON -DLOGX_WITH_AESGCM=ON

# 构建(必须 Release,Debug 与 Anaconda GTest 不匹配)
cmake --build . -j --config Release

# 测试
ctest -C Release --output-on-failure
```

### Android(NDK 交叉编译与真机运行)

```bash
# 配置(自动选 Clang + pal_posix; 最低支持 Android 5.0 / API 21; NDK 目录只读引用)
cmake -S . -B build-android \
  -DCMAKE_TOOLCHAIN_FILE="D:/DevEnv/Android/SDK/ndk/29.0.14206865/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 \
  -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release \
  -DLOGX_BUILD_TESTS=ON -DLOGX_BUILD_STRESS=ON \
  -DLOGX_WITH_ZSTD=ON -DLOGX_WITH_AESGCM=ON -G Ninja

# 构建(.a + liblogx.so + 测试二进制)
cmake --build build-android -j

# 推送二进制至设备
adb shell mkdir -p /data/local/tmp/logx_test
adb push D:/DevEnv/Android/SDK/ndk/29.0.14206865/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so /data/local/tmp/logx_test/
adb push build-android/liblogx.so /data/local/tmp/logx_test/
adb push build-android/tests/unit/logx_unit_tests /data/local/tmp/logx_test/
adb push build-android/tests/stress/logx_stress_tests /data/local/tmp/logx_test/
adb shell "chmod 755 /data/local/tmp/logx_test/*"

# 在真机上运行测试
adb shell "cd /data/local/tmp/logx_test && export LD_LIBRARY_PATH=. && ./logx_unit_tests"
adb shell "cd /data/local/tmp/logx_test && export LD_LIBRARY_PATH=. && ./logx_stress_tests"
```

---

## 本文件维护纪律

1. **提交记录表是导航索引,不是权威历史**:完整提交历史以 `git log --oneline` 为权威。新批次收口时只**补一行批次行**(哈希 + 一句话摘要),不再逐提交回填,杜绝注 2 所述的漏行事故。
2. **单批次条目固定格式**:`## R{n} 批次: {主题} (YYYY-MM-DD)`,依次为审查对象与 HEAD、发现清单(P0/P1/P2/P3/Nit 分级)、修复与验证、遗留项;审查报告本体放 `docs/reviews/`,本文件只留结论与关键证据。
3. **滚动归档**:每月初将上月及更早的批次章节原样移入 `docs/changelog/archive-{YYYY-MM}.md` 并在「历史归档」表登记;归档原文不改写,勘误以 `⚠️ 补记` 就地追加。
4. **交叉引用写章节名,不写行号**:行号会随归档漂移,章节名稳定。

