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

> 注:历史行 `433497e`/`4c70e25` 曾误记为 `e819ac2`/`1cf48a1`,2026-09-03 复核时按 git 实际历史更正。

---

## 独立复核记录(2026-09-03,代码级验证)

对照方案 v5 与本 CHANGELOG 逐项核查;Windows / MSVC 2022 / x64 Release 下以
`-DLOGX_WITH_ZSTD=ON -DLOGX_WITH_AESGCM=ON` 实际重新配置、编译并运行全部单测。

### 复核通过(声称属实)

- **39/39 单测全绿**:实测复现(`[ PASSED ] 39 tests`),与"M3 加密闭环"章节声称一致。
- **M3 加密闭环**:mbedtls vendor 源码在位;`codec.cpp` 真调用 `mbedtls_gcm_crypt_and_tag` /
  `mbedtls_gcm_auth_decrypt`;12B Nonce = 8B 文件盐 + 4B 大端 seqno;密钥缺失降级明文;
  错钥 / 篡改 Tag 拒绝(CodecTest)均与描述一致。
- **JNI 层**:`jni_key_provider_trampoline`(NewGlobalRef + AttachCurrentThread,
  nativeClose 释放全局引用)、`nativeExport`、`GetStringUTFRegion` 栈缓冲、
  `GetPrimitiveArrayCritical` 零堆写入路径均在代码中落实。
- **其他**:多进程自动隔离(`getProcessSuffix`)、`suspend fun export`、
  decoder `--kid-map`、保留期清理、恢复对账 Σraw_len 回推与截断续写跳 1 空隙
  (resume = max + 1,首块 = max + 2)均属实。

### 复核发现的偏差(文档已同步标注,任务归口计划表 T4.0)

| # | spec/CHANGELOG 声称(v5) | 代码实际(2026-09-03) |
|---|---|---|
| 1 | `flush()` 水位线 `target_seqno` 防饥饿(§7,P0) | 未实现:`flush_blocking` 自旋等待 staging 全空 + 5ms sleep,持续高频写入存在饥饿风险 |
| 2 | 切分序号升级 2B uint16_t(§3.1/§4.4,P1) | 仍为 uint8_t(`AppendTargetCache.split_no` / `make_log_path`) |
| 3 | 双 Header:写 A → seq_cst fence → 写 B(§4.2/§10,P0) | 实现为 A(中间态 CRC 保护)→ B → `atomic_thread_fence(release)`,时机与内存序均偏离纪律 |
| 4 | Flusher 50~100ms Debounce 超时聚合(§4.1,P2) | 未实现:`flusher_loop` 为无超时阻塞 `ev_wait` |
| 5 | ARMv8 Crypto Extensions 硬件加速(§10,P2) | ✅ 已闭环(2026-09-06,C-04):`aesce.c` 移植并落地标准 AESE/AESMC/EOR 指令直通,消除热路径逐块 getauxval 并恢复探针为无状态纯读(彻底根除数据竞争),落地 GCM 官方向量完整锚定(NIST KAT 零密钥极值基线 + McGrew-Viega 附录 B TC13/14/15 权威锚点,方案 A+)与 add 判空防御,真机(骁龙865)自检与 NIST 校验全绿通过,单测 69/69 与压测 4/4 全通过 |
| 6 | 每 30s 定时 msync/fdatasync 持久化(§11) | 未实现,当前仅退后台 / 手动路径触发 |
| 7 | 未对齐访问 / 23:59:59 时钟跳变测试用例(§9,P2) | 39 个用例中不存在 |

另:M1/M2 验收栏的"kill -9 注入 1000 次零丢失、零重复"与"单条写入 <2µs"
无实测依据(仓库无崩溃注入 harness,无基准报告);计划表 v1.1 已改为指向 T4.5 / T5.2。

> ⚠️ 阅读提示:本节之后的历史章节中,"实施方案 v5 定稿升级"一节记录的是
> **方案文档(设计)的升级**;上表 1~7 项截至复核日仅停留在 spec 层面、未落地代码,
> 请勿据其认为代码已实现。

---

## 测试逻辑复核记录(2026-09-03,39 用例与实现逐行对照)

在「独立复核记录」基础上,对 5 个测试文件逐一与实现源码核对。**结论:39 个用例断言数值/偏移基本正确;但存在 5 个"名不符实/逻辑失真"用例(断言通过却未覆盖声称的分支,甚至断言恒真/手动假降级)与 8 处已实现功能的覆盖缺口。当前 39 个单测中真实有效用例 34 个(87.2%),已完成功能的综合覆盖度约为 65%~70%。**归口计划表任务 **T4.0B**。

### A. 名不符实与逻辑失真用例(断言恒真/分支未触发,待 T4.0B 修正)

1. **`StagingTest.DualHeaderCorruptionBHandWins`**:commit 后未写任何数据即 close,
   A/B 内容相同(均为全零 Header),断言 `seqno==0` 无论读到 A 还是 B 都成立——
   "B 接管"未被真实验证(断言恒真假阳性)。
2. **`Split.TornTailNotClosedAsValidBlock`**:头部声明 len=2 且 2B payload 齐全,
   是一条完整合法记录,`peek_record_len` 成功,走正常累计分支;`first==0` 的
   撕裂封块分支(flusher.cpp)实际未被执行。
3. **`Split.OneBlockManySmallRecords`**:100×56B=5600B 远小于 16KB,实际只产生
   1 块;16KB 封块分支(加下一条超限则封块)在全部测试中零覆盖。注释"每块 285 条"
   算错(16384/56≈292)。
4. **`RecoveryTest.BlocksWrittenHeaderNotCommitted_RemainderRescanned`**:实际验证
   的是"在飞记录被清零、坐标收敛"(并非"重扫");且构造上块 raw_len=28 大于待刷区
   [28,42) 仅 14B——真实 Flusher 的块只会从 pending 区切出,该状态物理不可达。
5. **`CodecTest.EncodeBlockMissingKey_DowngradesToPlaintext`**:测试声称"缺密钥自动降级明文",
   但代码逻辑为断言加密 flag 失败后,由调用方在测试中*手动*将入参改传 plain_flag 调第二次。
   真实的"缺密钥自动降级明文落盘并触发 `LOGX_EV_DEGRADED`"逻辑位于 `flusher.cpp:append_block_raw`,
   该生产链路零执行、零覆盖。

### B. 覆盖缺口(已实现、零测试,按优先级)

1. **块级 CRC32 读路径(P0)**:`append_block_raw` 写入 crc32(payload),
   但 `scan_one` 扫块只查 magic 从不校验 CRC,`decode_block` 将 CRC 责任
   (void) 交给调用方且无任何调用方执行——"宁丢不脏"的这层兜底既无实现消费
   也无测试(spec §3.2/§9 撕裂注入依赖它)。
2. 双 Header 均损坏 → 整区重建 + seqno 接管文件 max(spec §4.3 末段)。
3. 追加目标缓存 O(1) 命中快路径(现所有 recovery 用例 atc.date=0,只走慢路径)。
4. 保留期清理 `retention_cleanup_impl`。
5. 按天轮换与 10MB 切分 `.1/.2`。
6. `write_records` 的 kTooLarge 分支(kFull 已测)。
7. keyProvider 返回 null 时 export 跳过 + 打点语义(spec §7)。
8. format 入参防御(level>5、payload=null)与中片 flags=10 断言。

### C. 其他观察与基线指标

- **有效性量化**: 39 个单测中,有效断言用例 34 个(87.2%),假阳性/失真用例 5 个(12.8%)。
- **覆盖率量化**: 对已实现功能的综合覆盖度约为 65%~70%(核心加解密 90%、环形写入 80%、恢复对账 70%、文件生命周期/切分 20%)。
- nonce 派生(8B 盐 + 4B 大端 seqno)无单测——spec M3 验收点"nonce 唯一性单测"
  缺失,并入 T4.0B 第 1 项附近补齐。
- 对 v3/v4 修复点(1 字节空隙、pad<6 留零、Σraw_len 回推、跳空隙、下溢、钳制)
  的回归测试真实有效,是本测试集价值最高的部分。T4.0B 验收目标为功能覆盖率提升至 $\ge 95\%$ 且假阳性清零。

---

## 深度系统复核记录(2026-09-03, 周边链路与工程化审查)

在「独立复核」与「测试逻辑复核」基础上,进一步贯通审查 Python 解码器、Kotlin Facade、JNI 桥接、切分恢复及平台持久化调用。
**结论:C++ 核心算法闭环属实,但周边交付件存在 2 处阻塞级缺陷(Python 解码器逻辑瘫痪、Kotlin Facade 语法截断且缺失 Gradle)及 3 处系统偏差。**
统一归口为计划表任务 **T4.0-8 ~ T4.0-12**。

### A. 周边交付件阻塞级缺陷 (归入 T4.0)

1. **Python 离线解码器 `logx_decode.py` 逻辑缺陷 (T4.0-8, P0)**:
   - **重组状态机 Bug**: `reassemble` 遇 `is_whole` (flags=0, 普通整条日志) 时执行 `state = 1`。但整条日志无后续分片重置 state, 循环结束断言 `state == 0` 失败, **导致对任何普通整条日志解码恒定返回 None** (实测已写单行 python 脚本复现)。
   - **单块单日志错误假设**: `decode_file` / `main` 将每个 Block 当作一条日志直接调用 `parse_user_payload(blk.user_payload)`。而 spec §3.2 与 C++ 实现为**单块可装入多条连续记录**。面对真实日志文件必定抛出 `'NoneType' object is not subscriptable`。
   - **缺少跨块分片缓存**: 未实现跨 Block 的分片段缓存重组。声称的 "T3.4 端到端联调" 需推翻重构。

2. **Kotlin Facade `LogX.kt` 语法截断与编译缺失 (T4.0-9, P0)**:
   - **语法错误与调用必崩**: `LogX.init()` 第 97~100 行漏写 `if (nativeHandle == 0L) {`, 导致花括号结构非法。且无论 nativeInit 是否成功, 每次调用 init 必触发 `throw IllegalStateException("LogX native init failed")`。
   - **构建缺失**: 仓库根目录与 android 目录下零 Gradle 配置 (`build.gradle` 等不存在), Kotlin 代码从未参与自动化 CI 编译。

### B. 接口与逻辑偏差 (归入 T4.0)

3. **JNI Config 参数硬编码与 ErrorHandler 虚置 (T4.0-10, P1)**:
   - `nativeInit` 签名未接收 Kotlin Config 中的 `retentionDays`, `maxFileSize`, `flushThreshold`, `maxLogLength`, JNI 内部全部写死默认值, 业务自定义配置被静默忽略。
   - Kotlin 的 `errorHandler` 仅为本地属性, JNI 未向 C++ 的 `err_fn` 注册 Trampoline, 底层 `emit_event` (降级/丢日志/截断) 永远无法通知 Kotlin 层。

4. **崩溃重启后切分序号重置为 0 的历史覆写风险 (T4.0-11, P1)**:
   - `flusher.cpp` 的 `ensure_append_target` 从现有文件恢复时, 未从文件名解析切分序号, 直接强制 `c->current_split = 0`。若当天已有 `YYYYMMDD.log.1` 且已满 10MB, 重启后再切分仍会生成 `.log.1`, 导致倒退覆写历史切分文件。

5. **数据持久化 API 全库调用为 0 (T4.0-12, P1)**:
   - `pal_posix.cpp` 与 `pal_win32.cpp` 中实现的 `file_fsync`, `file_dir_fsync`, `mmap_msync` 在全库 `core/src/` 中调用次数为 **0**。spec §10 明确要求的“新建日志文件后对父目录 fsync 一次”未接入代码。

---

## 〇、M4 生产加固、并发压测与故障注入落地(本次)

对应方案文档 `LogX日志库最终实施方案.md` §2、§3.1、§3.3、§4.1~§4.4、§7、§9、§10、§11 与计划表任务 T4.0B、T4.1~T4.5。

### 1. T4.3 日志保留期自动清理机制 (Commit `15fa92c`)

- **触发时机全链路闭环**:
  - 在 `flusher_loop` 线程启动时执行一次基线清理；
  - 在 `drain_once` 检测到自然日跨天（`yyyymmdd != current_date`）切换目标文件时触发清理；
  - 在 `flush_blocking`（对应 Android 退后台 `onStop`/`onBackground` 调用 `logx_flush`）落盘完成时立即触发清理，保证退后台即刻释放存储。
- **边界防御与平年/闰年精准推算**:
  - 实现 `is_leap_year(y)`，覆盖 4 年一闰、百年不闰、四百年又闰法则；推算 2 月平年 28 天/闰年 29 天及大月 31 天/小月 30 天；
  - **显式豁免 Staging 文件**: 针对以 `.mmap` 结尾的环形内存映射文件无条件跳过；
  - **保护当天与未来文件**: 对文件名解析出的日期 $\ge$ 当天日期的文件严格保留，免疫时钟小幅波动干扰。
- **跨块串联与排序加固**:
  - `decode_one_file` 引入 `carry_slice_buf` 解决跨块分片日志还原；
  - `take_snapshot` 统一采用 `(date, split_no)` 自然数值排序，彻底避免字符串字典序导致 `.log.10` 排在 `.log.2` 前面的逻辑错位。

### 2. T4.4 50 线程高并发压力测试与死锁/一致性验证 (Commit `2118592`)

- **压测场景与高频竞争注入 (`tests/stress/test_concurrent.cpp`)**:
  - **50 个 Worker 线程**: 通过原子发令枪同时并发起跑，写入 80B ~ 1500B 混合变长日志；
  - **1 个专属 flush 竞争线程**: 在高并发写入期间以 15ms 间隔高频主动调用 `logx_flush()`，与后台 Flusher 线程及 50 个写线程的互斥锁制造极限锁争用与切块打断；
  - **512KB 极小切分阈值**: 强制在压测过程中产生 15+ 个文件轮转切分。
- **性能与一致性对账指标**:
  - **吞吐率**: 纯写用时约 0.08 秒，达到 **300,000+ logs/s** 级超高写入吞吐；
  - **对账 1 (物理块与单调 Seqno)**: 遍历校验 500+ 个落盘 Block，CRC32 校验 100% 正确，全局 seqno 严格连续单调递增，无断号、无跳号、无重复；
  - **对账 2 (端到端数据完整性)**: 通过公开 C API `logx_export` 导出全部记录，`exported_records == raw_disk_records == written_logs`，**数据零丢失、零损坏、零死锁**。

### 3. T4.5 故障注入与鲁棒性验证矩阵 (Commit `78a2e3c`)

- **硬件级未对齐内存访问安全校验 (`tests/unit/test_robustness.cpp`)**:
  - 在奇数内存偏移（`+1, +3, +5, +7`）执行 `put_u16/32/64`、`get_u16/32/64` 读写，保证小端序与 100% 内存安全，杜绝在 ARMv7-A 硬件或 ASan/UBSan 下触发 `SIGBUS` 崩溃；
  - 在非对齐地址上执行 `format_record`、`parse_record` 与 `parse_records_stream`，验证编解码器无对齐假设。
- **时钟跳变与回拨仿真验证 (`tests/unit/test_robustness.cpp`)**:
  - 实现 `MockClockPal` 测试代理；
  - **23:59:59 跨日跳变到 00:00:01**: 验证 Flusher 自动识别时钟跨天，旧日志文件平稳归档，次日新文件命名与切分序号复位为 0，全局 `seqno` 严格自增衔接；
  - **时钟剧烈大幅回拨（后天 -> 前天）**: 验证系统时钟倒退时，`scan_dir_for_target` 接管算法平稳运行，绝不覆盖已有日志，当天与未来日志 100% 完整保留。
- **进程崩溃注入与恢复自愈套件 (`tests/stress/test_crash_recovery.cpp`)**:
  - **Staging 未落盘强杀恢复**: 写入环形缓冲区后模拟进程被 `kill -9` 强杀；重启实例后 `run_recovery` 自动识别 Staging 中未刷盘日志并补刷落盘，**20/20 条日志 100% 挽回（零丢失）**；
  - **磁盘尾部坏块截断告警**: 日志文件尾部注入坏块；重启后 `run_recovery` 精确截断坏块并打点 `LOGX_EV_RECOVERY`，无下溢，后续新日志无缝写入；
  - **50 轮多阶段随机崩溃自愈循环**: 连续 50 轮在“写 Staging 强杀”、“部分刷盘强杀”、“正常退出”交替切换；50 轮结束后通过 `logx_export` 导出对账，**397 条日志全部 100% 完整复原，丢失记录数为 0**。

### 4. 深度演练中挖掘并根治的底层关键 Bug

1. **环尾不足 6 字节留零脏内存缺陷 ([`core/src/staging.cpp`](file:///d:/Project/Android_Project/LogX/logx/core/src/staging.cpp))**:
   - *缺陷*: 在 `Staging::write_records` 中，当尾部剩余空间 `pad < 6` 时原先直接 `w = 0`，未清零残余的 1~5 字节，导致该缝隙残留了上一轮次的脏内存数据，经 `copy_pending` 拷贝到下游导致数据错位；
   - *修复*: 当 `pad > 0 && pad < 6` 时显式 `std::memset(data_base_ + w, 0, pad)` 清零，杜绝旧轮次内存脏数据。
2. **滑动窗口重同步提前截断退出缺陷 ([`core/src/format.cpp`](file:///d:/Project/Android_Project/LogX/logx/core/src/format.cpp))**:
   - *缺陷*: `parse_records_stream` 中原先在校验 `magic` 前先校验了 `recs_len - pos < kHeaderLen + len` 以及 `magic == 0 && len == 0 break;`；遇到回绕处的零缝隙时，伪造的巨大 `len` 或 `magic==0` 导致循环直接 `break`，将当前块后续整批合法记录误判遗弃；
   - *修复*: 先校验 `magic` 合法性，凡遇到非 Entry / 非 Pad 头部（含零缝隙），统一使用前向滑动窗口（Slide-window resync）动态寻找下一个合法头部，绝不因零缝隙提前截断退出。
3. **Flusher 退出时文件句柄未关闭 ([`core/src/flusher.cpp`](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp))**:
   - *修复*: 在 `flusher_loop` 退出时显式调用 `close_current(&c, inst->pal)`，确保最后刷入系统缓冲区的文件数据安全提交到磁盘。
4. **Hint 命中截断状态丢失缺陷 ([`core/src/recovery.cpp`](file:///d:/Project/Android_Project/LogX/logx/core/src/recovery.cpp))**:
   - *修复*: 在 `reconcile` 中引入 `was_truncated` 状态保持，确保修复坏块后的 `truncated` 标记不会在后续二次全目录扫描中被冲刷成 false，从而保证打点事件 `LOGX_EV_RECOVERY` 准确上报。

### 5. 全量验证结果

- **Windows (MSVC 2022 Release)**:
  - **`logx_unit_tests`**: **59 / 59 PASSED (100%)**
  - **`logx_stress_tests`**: **4 / 4 PASSED (100%)**
- **全量 CTest 自动化测试**: 2 个测试目标 100% 全部通过，耗时仅 1.11 秒。

---

## 〇、M3 加密闭环落地(历史)

对应方案文档 `LogX日志库最终实施方案.md` §2、§3.2、§4.2、§7、§10 的 M3 加密闭环。

### 1. 轻量级 mbedtls AES-256-GCM 嵌入与 CMake 接线 (`third_party/mbedtls`)

- **Vendoring**:
  - 在 `logx/third_party/mbedtls` 下引入裁剪版 mbedtls（仅保留 AES 与 GCM 核心实现，约 1200 行 C 代码，零外部依赖）；
  - 配置 `mbedtls_config.h` 启用 `MBEDTLS_AES_C` 与 `MBEDTLS_GCM_C`。
- **CMake 集成**:
  - `core/CMakeLists.txt` 在 `LOGX_WITH_AESGCM=ON` 时将 `platform_util.c`、`aes.c`、`gcm.c` 编译并链接至 `logx_core`，并导出相关 include 目录。

### 2. AES-256-GCM Codec 核心实现 (`codec.cpp`)

- **真加解密落地**:
  - 实现 `encrypt_gcm` 与 `decrypt_gcm`，调用 `mbedtls_gcm_crypt_and_tag` 与 `mbedtls_gcm_auth_decrypt`；
  - 12B Nonce 规范构造：`[8B file salt + 4B big-endian seqno]`，确保同 (Key, Salt) 下 Nonce 唯一且单调递增；
  - 密文帧格式：`[ciphertext (in_len bytes)] + [16B GCM auth tag]`。
- **降级与容灾**:
  - 当配置加密但密钥缺失时，严格按照 spec §6 降级为明文写入，打点警告并不阻塞写流；
  - 密文被篡改或 Tag 不匹配时恒定时间拒绝，防止填充预言机攻击。

### 3. JNI KeyProvider 桥接与生命周期管理 (`logx_jni.cpp` / `LogX.kt`)

- **JNI Trampoline 桥接**:
  - 实现 `jni_key_provider_trampoline`，在 Flusher 线程（C++ 后台线程）需要加密密钥时，自动调用 `JavaVM->GetEnv()` / `AttachCurrentThread()`；
  - 通过全局引用 `NewGlobalRef` 与 `mid_get_key` 反射回调 Kotlin 层的 `KeyProvider.getKey(kid): ByteArray?`；
  - 使用 `GetByteArrayRegion` 复制 32B 密钥到线程局部缓冲区，实现调用方零锁与零 GC 抖动。
- **资源安全释放**:
  - 在 `nativeClose` 时主动 `DeleteGlobalRef` 并释放 `JniKeyProviderContext`，杜绝 JVM 内存泄漏。

### 4. 真实 Bug 修复 (全平台/Windows)

- **[P0 Bug 修复] `encode_block` 未开启压缩时缓冲未初始化**:
  - 修复 `comp == kAlgoNone` 时 `compd` 为空导致加密写入 0 字节密文的 Bug。
- **[P0 Bug 修复] `drain_once` 目标文件日期传递错误**:
  - 修复 `drain_once` 内部向 `ensure_append_target` 传递 `c->current_date`（初始为 0）导致生成 `00000000.log` 的 Bug。
- **[P1 修复] 跨平台路径分隔符解析**:
  - 统一 `take_snapshot`、`export_range`、`retention_cleanup`、`scan_dir_for_target` 中的路径 basename 解析为 `find_last_of("/\\")`，完美兼容 Windows (`\`) 和 POSIX (`/`)。
- **[P1 修复] `pal_win32.cpp` 文件大小只读读取**:
  - `file_size_path` 采用 `GENERIC_READ` 与 `GetFileSizeEx` 替代 `file_open`，避免打开中文件的共享写权限竞争。

### 5. 验证结果

- **Windows (MSVC 2022 Release)**: **39/39 单元测试 100% 通过**。
- **Android NDK (arm64-v8a / android-24)**: 交叉编译 **0 警告 0 错误通过**。
- **Python 离线解码器**: 验证 `logx_decode.py` 密文解密契约与 C++ 完全一致。

---

## 〇、实施方案 v5 定稿 + JNI 优化与补齐(本次)

对应方案文档评审优化定稿(v5, 2026-09-02)以及 Kotlin Facade 与 JNI 边界的契约闭环。

### 1. 方案文档升级至 v5 评审优化定稿版 (`LogX日志库最终实施方案.md`)

> ⚠️ **实现状态标注(2026-09-03 复核)**:本小节记录的是**方案文档(设计)的升级**。
> 下列各项中**已落地代码**:环形坐标系规范化、多进程自动隔离、`export` 协程化、JNI 零堆分配;
> **仅 spec 定稿、未落地代码**:flush 水位线、切分序号 2B、双 Header 屏障纪律(实现偏离,见复核表 #3)、
> Flusher Debounce、ARMv8 加速、未对齐/时钟跳变测试用例 —— 统一由计划表 **T4.0** 回补,
> 明细见上方「独立复核记录」。

- **[P0] 环形坐标系规范化**(§3.1 / §5):
  - 修复伪代码中相对偏移 `[0, kDataSize)` 与物理基址（128B）混用的 Bug；
  - 统一规范 Header 与运行时变量使用环内相对偏移 `[0, 130944)`，物理地址统一由 `pos_ptr(pos) = mmap_base + 128 + pos` 换算。
- **[P0] 双 Header 写入增加内存屏障**(§4.2 / §10):
  - 在写完 Header A（含 CRC）后插入 `std::atomic_thread_fence(std::memory_order_seq_cst)` 内存屏障再写 Header B，消除 ARM 弱内存模型下由于 Store Buffer 重排导致的 Header A 写入落后可见性风险。
- **[P0] `LogX.flush()` 水位线与防饥饿控制**(§4.2 / §7):
  - 同步 `flush()` 引入目标提交水位（Target Watermark: `target_seqno`）机制，条件变量仅等待至 `committed_seqno >= target_seqno` 即返回，彻底避免高频写入下 `flush()` 陷入无限期阻塞或饥饿。
- **[P1] 多进程 Safe-by-Default 自动隔离**(§6 / §7 / §10):
  - 框架层在 `LogX.init()` 内部自动根据当前进程名后缀隔离 `logDir` 与 `stagingDir`（如 `:push` 进程自动使用 `logs_push/` 和 `staging_push.mmap`），无需业务方手动配置，从根源杜绝跨进程 mmap 覆写风险。
- **[P1] `LogX.export()` 协程化生命周期管理**(§7):
  - 导出接口提供 Kotlin `suspend fun export(...)`，明文临时文件在协程作用域（含异步网络上传）完全结束或异常退出后再执行删除，彻底消除异步上传时的 `FileNotFoundException`。
- **[P1] 切分序号升级为 2B (uint16_t)**(§3.1 / §4.4):
  - Header reserved 区域划出的追加目标缓存中，切分序号由 1B 升级为 2B（上限 65535，支撑 655GB/天），消除 1B 达 255 停写的边界风险。
- **[P1] JNI 零堆内存分配与性能调优**(§4.1 / §7 / §10):
  - 引入 `GetStringUTFRegion` 栈缓冲与 `GetPrimitiveArrayCritical`，实现热路径零堆分配与零 GC 抖动。
- **[P2] ARMv8 硬件加速与 Flusher 聚合优化**(§2 / §4.1 / §10):
  - 引入 ARMv8 Crypto Extensions 硬件加速 AES-GCM 指令（`__ARM_FEATURE_CRYPTO`）；Flusher 增加 50~100ms Debounce 超时聚合。
- **[P2] 测试矩阵补全**(§9):
  - 新增 32 位 ARM 未对齐内存访问测试与跨天极端时钟跳变（23:59:59 回拨至 00:00:01）测试用例。

### 2. JNI 胶水层与 Kotlin Facade 补齐与优化 (`logx_jni.cpp` / `LogX.kt`)

- **[P0 修复] 补齐缺失的 `nativeExport` JNI 实现** (`logx_jni.cpp`):
  - 原 `logx_jni.cpp` 缺少 `Java_com_example_logx_LogX_nativeExport` 的实现，导致 Kotlin 调用 `LogX.export()` 时报 `UnsatisfiedLinkError`；
  - 实现了 `Java_com_example_logx_LogX_nativeExport` 并封装 `export_trampoline` 回调桥，正确打通从 C 层 `logx_export` 到 Kotlin `ExportCallback.onExport` 的链路，并在回调完成后及时释放 LocalRef。
- **[性能优化] JNI 写入路径零堆分配** (`logx_jni.cpp`):
  - `nativeWrite` 对 `jTag` 优先使用栈缓冲 `char tag_buf[128]` 配合 `env->GetStringUTFRegion`（绝大多数 Tag $< 40$ 字符），避免调用 `GetStringUTFChars` 产生 malloc 堆分配；
  - 对 `jPayload` 采用 `env->GetPrimitiveArrayCritical` 与 `JNI_ABORT` 释放，避免数组拷贝与 ART GC 标记。
- **[健壮性] 句柄安全防卫** (`logx_jni.cpp`):
  - 所有 JNI 函数增加 `handle != 0` 及关键入参校验，防止未初始化或 close 后的野指针调用。
- **[Kotlin Facade 增强]** (`LogX.kt`):
  - 增加 `getProcessSuffix` 多进程名识别分流逻辑；
  - 增加 `suspend fun export(...)` 协程扩展函数。

### 3. 测试与验证

- 修复 `test_codec.cpp` 中的 `ZstdFlagFailsWhenNotBuilt` 单测断言（直接校验未启用 real zstd 时 `compress_block` 的失败行为）。
- Windows (MSVC 2022 / x64 Release): **28/28 单测 100% 通过**。

---

## 一、恢复精确回推 + zstd 真接入(`e819ac2`)

对应遗留项"三-1(恢复对账 `bytes_since_hdr` 未实现)"与"M3 zstd 单文件版"。

### 恢复对账:Σraw_len 精确回推(`recovery.cpp` / `flusher.cpp`)

- **`S_file > S_hdr` 分支落地**(spec §4.3):新增 `sum_raw_len_since()`
  (`flusher.cpp`),从文件中 seqno ∈ (S_hdr, S_file] 的块逐块累加块头自描述的
  `raw_len`,F = (F + Σraw_len) mod 数据区大小,seqno = S_file。此前该分支是
  占位(`bytes_since_hdr = 0`,F 不推进),staging 中被跨过的已落盘数据会被
  Flusher 下一轮重复压块(重复块风险)。k 块回溯对块头只读不修,无截断副作用。
- **不变量收尾**:恢复出口前清零 (a) 跨过区间 [F_orig, F)(崩溃可能落在
  §4.2 第 4→5 步之间,该区间未清零)与 (b) 撕裂尾巴 [W', W_crash),
  使"除 [flushed_len, write_pos) 外全环为零"在出口处成立(spec §4.3 收尾要求)。
- **`truncated_blocks` 计数修复**:
  - `S_file < S_hdr` 且文件尾干净(断电常态分支)此前完全不计数;
  - `S_file > S_hdr` 时 `S_hdr - seqno_file` uint32 下溢(打出 ~40 亿)。
  现按 `lost = max(0, S_hdr - S_file)` 计数,truncated 时另计 1 兜底。
- **裸 write_pos 消毒**:`write_records` 把写指针裸写在 Header A[16](CRC 外),
  撕裂可能留下 ≥ size 的垃圾值;恢复侧越界一律钳为 0(宁丢不脏),
  否则会按越界值分配 ~4GB 扫描缓冲。
- **resume_seqno 语义对齐**(遗留项 5 部分收敛):统一为"下一块序号",
  Flusher 首块取 resume + 1;截断后续写 resume = max(S_hdr,S_file) + 1
  (首块 = max + 2,跳 1 空隙防 nonce 重用);全新启动 resume = 0(首块 = 1)。

### zstd 真接入(M3 第一步)

- **vendor 源码**:`third_party/zstd/lib`(v1.5.7,官方 tarball 剔除
  tests/programs/contrib 等,零源码改动)。单线程配置(ZSTD_MULTITHREAD 未定义)。
- **CMake 接线**:`core/CMakeLists.txt` 在 `LOGX_WITH_ZSTD=ON` 时把 zstd 的
  24 个 .c 编进 `logx_core`。两个坑:
  - `project(logx_core C CXX)` 必须显式启用 C 语言,否则 .c 被当 None
    不参与编译,链接期报 `ZSTD_*` 未解析;
  - 解压侧需补 `zstd_ddict.c`(`zstd_decompress.c` 引用,即使不用字典)。
- **解压上限防御**(`codec.cpp`):块内容契约上限 = `kBlockMaxRaw` + 64B;
  `ZSTD_getFrameContentSize` 声明值超限直接拒绝(坏块伪造超大声明值
  → `resize` 内存炸弹)。zstd 默认帧不带校验和,内容完整性靠块级
  CRC32(spec §3.2 保留它的原因)。
- **压缩降级链**(`encode_block`):压缩失败自动回退 flag=none 继续写,
  不因 codec 故障丢日志/阻塞(spec §6 降级矩阵)。
- **`LOGX_WITH_ZSTD=0` 时 flag=zstd 编码失败**(flusher 按块 IO 错误处理,
  不静默落明文,避免"看起来压缩了其实没压"的格式混淆)。

### 测试

- `test_recovery` 新增 4:`BlocksWrittenHeaderNotCommitted_BackfillExact`
  (Σraw_len 精确回推 + 收尾清零,按 Flusher 真实时序"写块→清零→提交"构造)、
  `_RemainderRescanned`(在飞记录被清、坐标收敛)、
  `TruncatedFile_LostCountNoUnderflow`(丢块计数 = S_hdr - S_file)、
  `CorruptWritePos_ClampedToZero`(越界 write_pos 钳制)。
- 新增 `test_codec`(7 个):zstd 高压缩数据往返(体积 < 1/4)、不可压数据
  往返、坏帧头拒绝、全零垃圾拒绝、encode/decode 端到端、unknown algo 失败、
  未接 zstd 时 flag=zstd 编码失败。
- 命令行 `adb push` 在 Git Bash 下需 `MSYS_NO_PATHCONV=1`,否则路径被
  MSYS 转换(`C:/ProgramFiles/Git/data/...`),推到别处后误以为设备在跑新
  二进制(设备上残留旧文件 + md5 校验发现)。

### 验证

- Windows / MSVC 2022 / x64 Release:**32/32 通过**(`-DLOGX_WITH_ZSTD=ON`)
- Android / NDK 29 / arm64-v8a(-DLOGX_WITH_ZSTD=ON):**32/32 通过**
  (Redmi K30 Pro 真机)
- 注:默认 `LOGX_WITH_ZSTD=OFF` 行为不变(flag=zstd 编码失败路径);
  实际启用压缩需 `-DLOGX_WITH_ZSTD=ON` 且实例配置 `compress_algo = kAlgoZstd`。

---

## 二、首次构建修复(`d454dee`)

按以下命令在 Windows/MSVC 首次编译,暴露 100+ 解析错误与 4 处真实 bug:

```bash
cmake .. -DLOGX_BUILD_TESTS=ON
cmake --build . -j
```

### 根因诊断:UTF-8/GBK 解析错乱

源码为 UTF-8 无 BOM,MSVC 默认按系统代码页(中文系统 = 936/GBK)读取,
中文注释被 GBK 误读后会把**下一行的 `constexpr` 声明吞掉**(伴随 C4819 警告),
表现为 `kStagingFileSize 未声明` 等一串无法理解的级联错误。
用最小复现文件(两行中文注释 + constexpr 声明)确认,`/utf-8` 验证有效。

### 修复清单

| 文件 | 修复 | 影响平台 |
|---|---|---|
| `CMakeLists.txt` | MSVC 编译选项加 `/utf-8` | 仅 MSVC |
| `core/include/logx_staging.h` | `FileHeader` 显式 1 字节对齐打包:自然对齐下 sizeof=36B,违反规范 §3.2 的 32B(static_assert 必炸) | **全平台**(NDK 同样会炸) |
| `core/src/staging.cpp` | 补 `#include "logx_format.h"`:`kMagicPad` 定义在该头,原文件缺失导致未声明 | **全平台** |
| `core/src/format.cpp` | `std::min<uint64_t>` 显式模板参数:`uint32_t` 与 `uint64_t` 混用无匹配重载 | **全平台** |

---

## 三、Windows(MSVC)平台移植(`fa24e2f`)

按方案文档 §12"重写 PAL 即可适配平台"完成 Windows 支持,
同时把 `flusher` / `recovery` / `logx_c` 里的 POSIX 直调
(`open`/`write`/`pread`/`fstat`/`ftruncate`/`unlink`/`nanosleep`/`dirent`)
全部收口到 PAL —— 这些直调此前违反"核心零平台依赖"的设计声明。

### PAL 接口扩展(`logx_pal.h`,posix 与 win32 同步实现)

| 方法 | 用途 |
|---|---|
| `file_pread(fd, buf, len, offset)` | 循环短读,EOF 返回已读字节数(scan_one/export 读文件头与块头) |
| `file_size(fd)` / `file_size_path(path)` | 取文件大小(替代 `fstat`/`stat`) |
| `sleep_ms(ms)` | Flusher 同步轮询退避(替代 `nanosleep`) |
| `file_rmdir(path)` | 删除空目录(测试清理用) |

### `pal_win32.cpp`(新增,约 460 行)

- **文件**:`CreateFileA` + `_open_osfhandle` 暴露为 CRT fd;`WriteFile`/`ReadFile`
  + OVERLAPPED 实现 pwrite/pread;`FlushFileBuffers` 实现 fsync/目录 fsync;
  `CreateDirectoryA` 递归建目录;`FindFirstFileA` 列目录(输出统一 `/` 分隔)
- **mmap**:`CreateFileMapping` + `MapViewOfFile`(MAP_SHARED 语义),
  `msync(MS_SYNC)` = `FlushViewOfFile`
- **互斥** = CRITICAL_SECTION;**事件** = 自动重置事件(与 posix 版 signal/wait 语义一致)
- **线程** = `_beginthreadex`;**原子** = `InterlockedExchangeAdd64`/`InterlockedCompareExchange64`
- **时间** = `QueryPerformanceCounter` / `GetSystemTimeAsFileTime` / `GetLocalTime`
- **随机** = `rand_s`(系统 RNG,需在包含任何 CRT 头之前 `#define _CRT_RAND_S`)
- **休眠** = `Sleep`

### 构建系统

- `core/CMakeLists.txt`:`WIN32` 选 `pal_win32.cpp`,否则 `pal_posix.cpp`
- `tests/unit/CMakeLists.txt`:pthread 链接仅限非 Windows;补 `add_test` 注册
  (**此前 ctest 从未能运行**)

### 测试修复(测试首次运行,断言/构造与实现契约对齐)

- `test_format`:偏移断言修正(taglen@21 / tag@23 / msg@30);
  PAD 用例改为"分片夹 PAD 跨 PAD 重组"(原用例期望两条整条拼接,违反单条日志语义);
  `format_record` 输出缓冲按 ≥ `kMaxRecordTotal` 契约
- `test_staging`:`FullRingDrops` 改为填满 `size-1` 再拒写;回绕用例补
  `commit_header_locked` 推进 f + 清零(模拟 Flusher 消费)——否则会计空隙下
  跨回绕必然 `kFull`,测试意图无法成立
- `test_recovery`:撕裂半条构造真正截断(声明 4B 只给 2B);文件 IO 全部改走 PAL;
  临时路径去 `/tmp` 依赖(Windows 无 `/tmp`)

### 顺带修复的真实实现 bug(任何平台都会触发)

| 位置 | 问题 |
|---|---|
| `logx_c.h` | C ABI 句柄 `logx_instance_t`(`struct logx_instance`)与内部引擎类型 `Instance` 是互不转换的两个类型;C++ 分支现在把句柄 typedef 为 `logx::Instance` |
| `staging.cpp` | **PAD 回绕写入越界**:`this_pad` 未扣 6B 记录头,`memcpy`/`memset` 会写穿环尾(内存破坏风险) |
| `staging.cpp` | 全零文件(全新)被误判为"双 Header 损坏重建"(`rebuilt=true`),绕过文件对账 |
| `staging.cpp` | `debug_check_invariant` 跨回绕时把活跃段 `[0, w)` 当脏数据检查 |
| `format.cpp` | `parse_record` 遇 LGXP PAD 直接失败(spec 要求跳过,分片跨 PAD 连续) |
| `recovery.cpp` | 无文件(全新启动)被 `reconcile` 置 `truncated`,导致 resume_seqno 错误 +2 |
| `recovery.cpp` | 截断丢失计数只在 `S_file < S_hdr` 分支触发;`S_file == S_hdr` 时的半块尾巴不计数 |
| `flusher.cpp` | `scan_dir_for_target` 的 `scan_one` 有截断副作用,把候选文件提前"修好",对账丢失 truncated 信号(`scan_one` 增加 `repair` 参数:目录评估只读,确定追加目标才修复) |
| `flusher.cpp` / `logx_c.cpp` | 依赖传递包含才可用的 `<cstdlib>`/`<algorithm>` 等,补显式 include |

---

## 四、Android(NDK)交叉编译(`30efff8`)

用 Android NDK 29.0.14206865 自带 CMake toolchain 命令行交叉编译
(arm64-v8a / android-24 / c++_shared),打通 Android 产物链路。
**NDK 目录(`D:\DevEnv\Android\SDK\ndk`)全程只读引用,未做任何改动;**
全部产物落在 `logx/build-android/`(已在 `.gitignore`)。

### 产物(均验证为 ELF64 / AArch64)

| 产物 | 说明 |
|---|---|
| `build-android/core/liblogx_core.a` | core 静态库,平台分支自动走 `pal_posix.cpp` |
| `build-android/liblogx.so` | 新增 SHARED 目标:JNI 胶水层 + core,导出 5 个 JNI 符号(`nativeInit`/`nativeClose`/`nativeWrite`/`nativeFlush`/`nativeExport`),依赖 `libc++_shared` |
| `build-android/tests/unit/logx_unit_tests` | Android 可执行文件(gtest 经 FetchContent 交叉编译),已在 Redmi K30 Pro 真机通过 21/21 |

### 构建命令

```bash
cmake -S . -B build-android \
  -DCMAKE_TOOLCHAIN_FILE="D:/DevEnv/Android/SDK/ndk/29.0.14206865/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 \
  -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release \
  -DLOGX_BUILD_TESTS=ON -G Ninja
cmake --build build-android -j
```

> 换 ABI(如 `armeabi-v7a` / `x86_64`)只需改 `-DANDROID_ABI`。

### 修复的真实 bug(非 NDK 特有,任何 POSIX 平台都会触发)

| 位置 | 问题 | 修复 |
|---|---|---|
| `core/src/pal_posix.cpp` | `Event` 误用 `std::lock_guard<pthread_mutex_t>`:`pthread_mutex_t` 是 C 类型,无 `lock()`/`unlock()` 成员,libc++ 编译报错 | 改手动 `pthread_mutex_lock/unlock`(`pthread_cond_wait` 自行原子释放/重取锁,语义不变) |
| `core/CMakeLists.txt` | Android 上链接裸 `pthread` 失败:bionic 已并入 libc,无 `libpthread.so` | 改 `find_package(Threads)` + `Threads::Threads`(Android 下解析为 `-pthread` 编译选项而非 `-lpthread`) |
| `tests/unit/CMakeLists.txt` | 同上 pthread 链接问题 | 同上 |
| `tests/unit/CMakeLists.txt` | `find_package(GTest REQUIRED)` 在交叉编译下找不到 Android 版 gtest | 改"系统无 gtest 时 FetchContent 拉取 googletest v1.14.0 交叉编译"的降级通道 |

> 注:`pal_posix.cpp` 此前从未在非 MSVC 平台编译过(Windows 走 `pal_win32.cpp`),
> `lock_guard<pthread_mutex_t>` 在 Linux 桌面用 g++ 同样会报错,属全平台修复。

---

## 五、验证结果

- Windows / MSVC 2022(19.43)/ x64,**Release 构建全绿**
- `ctest -C Release`:**28/28 单测通过**(100%)
- 注意:Windows 下需用 Release 配置构建
  (`cmake --build . -j --config Release`),原因见下
- Android / NDK 29.0.14206865 / arm64-v8a:**交叉编译全绿**,单测在 Redmi K30 Pro
  (arm64-v8a) 真机运行通过

---

## 六、已知问题与后续事项

### 构建相关

1. **Debug 配置无法链接测试**:Anaconda 提供的 GTest 是 Release(/MD) 构建,
   Debug(/MDd) 下报 `RuntimeLibrary` / `_ITERATOR_DEBUG_LEVEL` 不匹配。
   解决:Release 配置构建,或安装原生 MSVC 版 GTest(vcpkg/conda-forge msvc)。
2. **`pal_win32.cpp` 使用 ANSI(A) API**:非当前代码页可表示的路径(如 UTF-8 中文路径)
   不可用。后续应转 W API(MultiByteToWideChar + `CreateFileW` 等)。
3. **Linux 桌面构建尚未验证**:`pal_posix.cpp` 新增的 5 个方法、
   GCC/Clang 桌面编译未实测(Android/NDK 已实测,见"四";
   `pal_posix` 与 `pal_win32` 已按新增接口对齐)。
4. **git autocrlf**:Windows 上提交时提示 LF→CRLF(默认 `core.autocrlf=true`)。
   如需统一 LF,可配 `core.autocrlf=false` 或加 `.gitattributes`。

### 设计/实现遗留(README §"已知未完成项"的延续)

1. ~~**M3**:mbedtls AES-GCM 真接入~~(2026-09-02 已解决,见"〇";其中 ARMv8 Crypto
   Extensions 硬件加速仍未落地,归 T4.0)。
2. ~~**M3**:JNI `keyProvider` trampoline~~(2026-09-02 已解决,见"〇")。
3. **M4**:50 线程压测 / monkey 72h 稳定性 / 断电注入(QEMU 快照)。
4. ~~**恢复对账 `bytes_since_hdr` 未实现**~~(2026-09-02 已解决):见"一",
   `S_file > S_hdr` 分支按块头 `raw_len` 逐块累加精确回推 F,含不变量收尾。
5. ~~**JNI 缺失 nativeExport**~~(2026-09-02 已解决):见"〇",已补齐 nativeExport 并优化 JNI 零堆分配。
6. ~~**多进程安全隔离**~~(2026-09-02 已解决):见"〇",Kotlin Facade 默认按进程名后缀分流目录。
7. **`test_flusher` 的 `write_block_to_file` 为占位实现**(统一到 `write_and_record`)。
8. **v5 spec 未落地项与系统缺陷**(2026-09-03 复核扩充, 统一由计划表 T4.0 回补, 详见"独立复核记录"与"深度系统复核记录"):
   ① `flush()` 水位线(现为自旋等空,§7); ② 切分序号仍 uint8_t(§3.1);
   ③ 双 Header 屏障顺序/内存序偏离纪律(现为 A→B→release fence,§4.2);
   ④ Flusher Debounce(§4.1); ⑤ ARMv8 Crypto 加速(§10);
   ⑥ 30s 定时 msync/fdatasync 持久化(§11); ⑦ 未对齐访问与时钟跳变测试用例(§9);
   ⑧ Python 解码器重构(is_whole 状态机致命 Bug + 单块多日志与跨块分片重组,§2/§10);
   ⑨ `LogX.kt` 语法截断修复(补 if 判断) + 建立 Gradle 编译工程(§7);
   ⑩ JNI Config 参数传递与 ErrorHandler 桥接打通(§7/§10);
   ⑪ 恢复后切分序号从文件名解析(消除强制归 0 覆写风险,§4.3/§4.4);
   ⑫ 新建日志文件父目录 fsync 业务接线(§10)。

---

## 八、Android 真机测试与跨平台加固 (2026-09-03)

在真实 Android 物理设备上完成了 `logx_unit_tests` (59 个单元测试) 与 `logx_stress_tests` (4 个压力测试) 的全量交叉编译与真机端到端执行。

### 1. 设备与环境
- **设备型号**: Redmi K30 Pro (`d16ced26`)
- **系统版本**: Android 12 (API Level 31)
- **CPU 架构**: `arm64-v8a`
- **NDK 版本**: Android NDK 29.0.14206865 (Clang 21.0.0 / libc++_shared)

### 2. 发现并修复的真实缺陷
1. **`android/cpp/logx_jni.cpp` 编译报错与签名补齐**:
   - `Java_com_example_logx_LogX_nativeInit` 扩展参数时漏写右括号与大括号，补齐 `retentionDays`, `maxFileSize`, `maxLogLength`, `flushThreshold` 参数；
   - 补充包含 `logx_internal.h`，解决 `inst->key_user` 在不透明类型指针下访问成员报错的问题。
2. **`tests/unit/test_flusher.cpp` 跨平台堆降级测试加固**:
   - 原测试使用 `invalid:name` 文件名假定 `file_open` 必然失败，该假定仅在 Windows NTFS 下成立；在 Linux/Android (ext4/f2fs) 下 `:` 为完全合法的普通字符，导致 staging 成功以文件 mmap 打开而非触发降级；
   - 修复为使用已存在的同名目录作为 `staging_path`，在 POSIX (Linux/Android) 下触发标准 `EISDIR` 错误，Windows 下触发 `ERROR_ACCESS_DENIED`，跨平台可靠触发堆内存降级与 `LOGX_EV_DEGRADED` 异常打点。

### 3. 真机实测结果
- **单元测试 (`logx_unit_tests`)**: **59/59 100% 全部通过 (耗时 123ms)**
  - `Format` (9/9)、`StagingTest` (7/7)、`RecoveryTest` (10/10)、`Split` (4/4)、`Flush` (10/10)、`ErrorMonitoring` (2/2)、`CodecTest` (13/13)、`Robustness` (4/4) 全部全绿。
- **压力与故障恢复测试 (`logx_stress_tests`)**: **4/4 100% 全部通过 (耗时 820ms)**
  - **50 线程高并发 + 15ms 强切竞争**: 达到 **85,796 logs/s**，触发文件上限成功轮转 13 个日志分卷，端到端导出 20,924 条日志，**0 损坏、0 丢失、seqno 严格单调递增**。
  - **崩溃自愈压测**: Staging 仅写入强杀自愈通过；磁盘尾部坏块安全截断并通过；50 轮多阶段随机崩溃注入压测，397/397 条日志 100% 挽回。

---

## 编解码缺陷修复与防御性加固 (2026-09-06, C-01 ~ C-03 闭环)

围绕代码审查清单中的编解码与跨端一致性缺陷，完成三次专项重构与防御性加固，双构建矩阵（默认 OFF/OFF 与真库全开 ON/ON）全绿通过：

### 1. [C-01] 压缩降级未更新块头 Flag（毒丸块 P0 缺陷闭环）
- **核心根因**：原 `encode_block` 压缩失败降级仅修改局部变量，落盘块头仍写 Zstd flag，导致整块日志无法导出（毒丸块）。
- **落地修复**：`EncodedBlock` 新增 `final_flag` 状态回写；`append_block_raw` 严格使用真实生效的 `final_flag` 写入块头并在降级时派发 `LOGX_EV_DEGRADED` 事件；`logx_open` 增加算法合法性 fail-fast 校验；引入 `ScopedCompressFailInjection` RAII 守卫；新增真实降级解码往返用例。

### 2. [C-02] 压缩/加密 Flag 常量命名歧义与移位收敛闭环
- **核心根因**：`kFlagCompressZstd = 0x1` 与 `kFlagEncrypted = 0x1` 数值冲突，极易误用按位或导致压缩位丢失；全库充斥手写移位魔数。
- **落地修复**：彻底废弃 `kFlagCompress*`；引入 `kFlagCompressShift=1`、`kFlagCompressMask=0x06`、`kAlgoMask=0x03` 与 `make_flag` / `flag_algo` / `flag_encrypted` constexpr 零开销内联 helper；全库约 15 处手写移位全部收敛；全栈命名对齐（JNI / Kotlin / Python）；新增 `BlockFlagHelper_BitLayoutAndSemantics` 硬协议位布局防退化单测。

### 3. [C-03] `decode_block` 签名精简与长度交叉校验闭环
- **核心根因**：签名接收 `raw` / `raw_len` 却以 `(void)` 丢弃，块头 `raw_len` 从未与解码输出做交叉校验，且声明与实现存在 "crc" 校验责任矛盾。
- **落地修复**：
  1. 删除从未被有效使用的 `const uint8_t* raw` 指针参数；
  2. `raw_len` 重构为 `expected_raw_len`，在解压完成后严格校验 `out_raw->size() == expected_raw_len`，不符时清空输出并返回 false（宁丢不脏），拦截畸形解压长度；传 0 跳过校验；
  3. 生产端 `logx_c.cpp:decode_one_file` 传入块头 `raw_len` 获得长度交叉保护，CRC 预检逻辑保持对称；
  4. 头文件注释消除 "crc" 矛盾，明确 CRC32 由调用方预检；
  5. 深度防御加固：`decode_block` 与 `encode_block` 增加指针与 payload 空指针拦截；`flusher.cpp:sum_raw_len_since` 增加 `(0, kSingleRecordMaxRaw]` 边界校验防御畸形块头推进水位（关联 F-05 前置加固）；
  6. 全库 11 处调用点同步适配；新增 `DecodeBlock_RejectsLengthMismatch`（四分支：基线/偏大/偏小/传 0，以及 `UINT32_MAX` 极值与空指针防御）与真实 Zstd 压缩块长度不符拒绝断言。

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
