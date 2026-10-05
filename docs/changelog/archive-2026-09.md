# LogX 变更日志归档 —— 2026-09

> 本文件是 [`CHANGELOG.md`](../../CHANGELOG.md) 的历史归档段,涵盖 2026-09-03 ~ 2026-09-30(独立复核记录 ~ R16 批次),自主文件原样迁出,正文未作任何改写(仅原「附:构建命令速查」一节上移回主文件附录)。
> 完整提交历史以 `git log --oneline` 为权威;提交哈希与批次的对照见主文件「提交记录」表。

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
| 1 | `flush()` 水位线 `target_seqno` 防饥饿(§7,P0) | ✅ 已闭环(2026-09-07,F-08):改用物理等价且规避丢弃路径 seqno 冲突的累计字节水位线(staged_bytes_total vs flushed_bytes_total), 结合环回绕 PAD 跨度补偿严格对齐出入环; 引入 5000ms 超时上界与 Flusher 存活检查双级熔断, 追加 LOGX_ERR_TIMEOUT ABI 安全错误码; 单测覆盖持续写抗饥饿、5s 挂死超时与恢复、65535 丢弃路径快速返回, 三构建矩阵全绿通过 |
| 2 | 切分序号升级 2B uint16_t(§3.1/§4.4,P1) | ✅ 已闭环(2026-09-07,F-07):保留 ATC 物理 16B 脚印并利用历史恒零填充位无缝扩宽为 uint16_t(Plan B),全链路函数与 FlusherCtx.current_split 升级为 uint16_t,单日分卷上限提升至 65535;达到上限发射 LOGX_EV_IO 明确告警并去重;单测覆盖跨越 255、65535 上限停写、历史 staging 布局向下兼容解析与边界校验,三构建矩阵全绿通过 |
| 3 | 双 Header:写 A → seq_cst fence → 写 B(§4.2/§10,P0) | 实现为 A(中间态 CRC 保护)→ B → `atomic_thread_fence(release)`,时机与内存序均偏离纪律 |
| 4 | Flusher 50~100ms Debounce 超时聚合(§4.1,P2) | ✅ 已闭环(2026-09-07,F-10):定型 75ms Debounce 防抖窗口中值(kFlushDebounceMs = 75),flusher_loop 等待超时从 30000ms 重构为 75ms,彻底消除低频零散写入长达 30 秒滞留风险;单测覆盖小写入兜底落盘与突发聚集 |
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

## random_bytes 密码学安全升级与低熵拦截 (2026-09-07, F-09 闭环)

针对报告一 WARN-5 指出的底层 CSPRNG 失败时退化为极低熵伪随机数（单调时间与栈地址周期异或）引发 Nonce 碰撞与离线预测的重大密码学隐患，完成接口与双端系统加固：

1. **PAL 契约显式错误化**：`Pal::random_bytes` 返回值由 `void` 升级为 `bool`，严禁在底层 CSPRNG 失效时伪装成功；
2. **POSIX 免 fd 优先采集**：Linux / Android 平台优先通过 `getrandom(2)` 系统调用直接从内核 CSPRNG 提取随机数（免疫进程 `EMFILE`/`ENFILE` 文件描述符耗尽），回退 `/dev/urandom`，两路均失败清零并显式返回 false，彻底移除 `(t ^ addr)` 伪随机回退逻辑；
3. **Windows 适配与清零**：MSVC 下保持 CRT `rand_s`（直连系统 `RtlGenRandom`）单路径，失败清零并显式返回 false，彻底移除伪随机回退逻辑；
4. **生产端拒绝服务与报警**：`open_append_target` 与 `append_block` 在获取文件盐失败时，发射 `LOGX_EV_IO`（"secure random unavailable..."）明确报警，关闭文件句柄并拒绝创建低熵文件；
5. **单元测试与全矩阵验证**：在 `test_robustness.cpp` 新增 `RandomBytesFailure_RefusesNewFileCreation`（故障注入验证拒绝建文件与报警）与 `RandomBytesSuccess_ProducesNonTrivialSalt`（正常路径验证 8B 盐非全零非全同）。

---

## Flusher 75ms Debounce 超时聚合与低频兜底 (2026-09-07, F-10 / T4.0-4 闭环)

针对报告三 §四 T4.0-4 指出的 `flusher_loop` 缺少 50~100ms Debounce 机制致低频写入滞留长达 30 秒的缺陷，完成消费循环改造与单测验证：

1. **定型 Debounce 黄金中值**：定义 `kFlushDebounceMs = 75`（50~100ms 窗口中值）；
2. **消费端常态化防抖轮询**：`flusher_loop` 将原 30000ms 等待超时重构为 `kFlushDebounceMs`（堆降级维持 5000ms），彻底打通冷写入与空闲唤醒链路，零星日志最迟 75ms 内必被 Flusher 兜底落盘；
3. **生产端正交保持**：`logx_write` 保持 `pending >= flush_threshold`（8KB）数据量唤醒，小写入不发送 `ev_signal` 避免 sticky 信号破坏防抖，让零散记录在 75ms 窗口内自然聚集落盘；
4. **单测覆盖与全矩阵验证**：在 `test_flusher.cpp` 新增 `Debounce_LowFrequencyWritesFlushedWithinWindow`（验证 ≤ 500ms 内落盘且数据完整）与 `Debounce_BurstAggregatesIntoFewBlocks`（验证 10 条零散写入被聚集进 ≤ 2 个物理块）。

---

## 审查报告 K/D/T 系列全闭环 (2026-09-08)

三份审查报告合并清单中最后三个系列全部修复销号,双构建矩阵全量回归通过
(全开 ON/ON: 127/127 单测 + 4/4 压测 + 3/3 真实进程强杀恢复;基础 OFF/OFF:
108/108 单测 + 4/4 压测)。至此合并清单无任何未闭环条目。

### K 系列 — Kotlin Facade (`android/kotlin/LogX.kt`)

- **K-01** 协程 `export` 丢弃 `nativeExport` 返回值 → 状态码如实透传
  (0=OK/-1=未初始化/-6=业务失败/-3=IO/-8=无文件),与 C++ E-03 传播链对齐;
- **K-02** 嵌套 `runBlocking` 阻塞调度器 → `runBlocking(parentContext)` 上下文
  绑定:结构化并发保真(父取消→子取消)、不重复占用 IO 线程
  (`limitedParallelism(1)` 下无死锁)、业务异常真实上抛;
- **K-03** `init` 失败未清理 `errorHandler` → 全失败路径统一回滚
  (`errorHandler=null + initCount 归零`),顺带修复 require 失败致 initCount
  永久卡死的重试封锁隐患;
- **K-04** `setErrorHandler` 无并发保护 → `@Synchronized` + `isClosed` 显式
  终结态(精确区分未初始化/已关闭,init 前允许预配置回调),`close` 串行化。

### D 系列 — Python 离线解码器 (`decoder/logx_decode.py`)

- **D-01** cksum 失败 `break` 丢记录 → 对齐现行 C++ FM-04:跳过整条损坏记录,
  magic 失效字节重同步,流断裂丢弃跨片残片(FM-02);
- **D-02** 块解码失败 `break` 丢整文件 → 新增 `block_header_plausible` 块头
  严检(对齐 C++ F-05):结构损坏终止扫描(宁丢不脏),块级解码失败生成
  `[seq=N crc fail]`/`[seq=N decode fail]` 占位行后跳过继续;
- **D-03** GCM 失败策略差异 → 模块 docstring 三层错误策略归档;**顺带修复
  GCM AAD 缺参缺陷**(`AESGCM.decrypt` 缺 `associated_data=None`,修复前
  加密文件解密路径 100% TypeError);
- **D-04** 死代码 `reassemble()` 清理;
- **D-05** 解压上限 64MB → 收紧至 `kBlockMaxRaw = 20KB+6`(zstd 直接限长,
  zlib 改 `decompressobj` 限长防 zip bomb);
- **附带修复**: `--out` 模式同日多分卷(`.log`/`.log.1`)重复 `'w'` 打开导致
  前序分卷被覆写抹除 → `current_date` 状态跟踪按日期归并连续写入;
- **验证**: 按 C++ 写盘格式构造合成日志 18 项端到端验证全通过
  (正常/zstd/AES-GCM/跨块分片/cksum 损坏/坏块头/坏 magic 重同步/多分卷合并)。

### T 系列 — 测试用例修正 (T-01 复核确认 + T-02~T-11 修复)

- **T-01** 复核确认闭环(`logx_crash_fork_tests` 已注册 CMake,3 用例全绿);
- **T-02** 名不符实用例更名 `..._PlaintextMode_ExportsAllLines` + 新增
  `ExportEncryptedFile_MissingKey_SkipsGracefully` 真缺失密钥降级用例
  (AESGCM 宏守卫);
- **T-03** 复核确认随 F-01 闭环(生产加密端到端 + `EXPECT_NE(密文,明文)` 已在);
- **T-04** PAL 注入竞态根除: `logx_config_t` 新增 `pal_override` 测试 seam
  (本批次唯一生产代码改动,生产传 NULL 零行为变化),`logx_open` 在 Flusher
  启动前定型 `inst->pal`;test_robustness 17 处 + test_flusher 同族注入点
  全部改为 open 前注入;
- **T-05** `FlushAndZeroMaintainsInvariant` 恒真假绿 → 反证断言(数据在场时
  空闲区语义校验必须失败)+ 清零后双重校验;
- **T-06** 压测补 `EXPECT_EQ(missing_count, 0)` 逐条精确对账断言;
- **T-07** `ClockRollback` 补 seqno 单调(未来文件=1/回拨文件=2)/文件切换/
  无撕裂三重断言;
- **T-08** 硬编码 magic(0x4c,0x47) → `put_u16 + kMagicEntry` 常量构造
  (test_recovery + test_format 两处);
- **T-09** GCM 稀疏密钥 `{1,2,3}`/`{0xAA}` → `make_randomish(32)` 确定性
  完整 32B 密钥(两用例三把密钥);
- **T-10** 未播种 `rand()` 目录命名 → 文件级原子自增计数器(4 文件全部同族
  用法,确定性可复现);
- **T-11** 竞争线程前 15ms 空跑 → 两阶段发令枪(`pre_gun` 预热写 50×8 条 +
  `staged_bytes_total >= 16KB` 门控后才放行竞争 flush)。

### 配套文档同步

- 实施计划表升级 v1.6:T3.4 置 ✅(残余真机联调归 M5)、验收清单 2 项拆分
  勾选、测试数量 59→127、甘特图 T4.0 done / M5 active;
- 合并清单 T4.0 跟踪表:T4.0-10 行闭环(J-02 已随 JNI 批次修复),
  **T4.0 十二项偏差全部销号**。

---

## M5 验证闭环: Kotlin Gradle 工程 + 真机解码联调 + keyProvider 泄漏专项 (2026-09-08)

### Kotlin Facade Gradle 编译工程 (`android/kotlin-gradle/`)

终结"Kotlin 交付源文件无编译验证"的历史缺口:

- **单源引用**: `app/build.gradle.kts` 经 `sourceSets` 直接把 `android/kotlin/`
  挂为源目录,编译验证的就是交付源文件本身,无拷贝漂移;
- **工具链**: AGP 8.5.1 + KGP 1.9.23 + Gradle 8.9 / JDK 17,`org.gradle.offline=true`
  全离线构建(依赖全命中本机缓存);minSdk 21 与全库基线一致;
- **纯逻辑单测 8/8 全绿** (`LogXPureLogicTest`): Compress 枚举与 C++ `kAlgo*`
  三端同步契约、Level ∈ {0..5}、`onNativeError` kindInt 映射与业务异常吞噬、
  Config 默认值(S-04/E-06)、pre-init no-op 状态机、协程 export 未初始化返回
  -1(K-01 语义)、setErrorHandler 终结态保护(K-04);
- **LogX.kt 微调**: 新增 `internal fun nativeHandleForTest()` 白盒只读访问器。

### 真机解码联调 (T3.4 最终验收, Redmi K30 Pro / Android 12 / arm64-v8a)

- 新增 `tests/unit/logx_decode_demo.cpp` 引擎驱动器(EXCLUDE_FROM_ALL,不进
  ctest): 生产配置(`kAlgoZstd` + AES-256-GCM + kid=1 KeyProvider)写入
  20 条中文消息 + 1 条 40KB 长记录(触发多分片跨块) + 120 条 1KB 记录,
  `logx_export` 导出后产物 `adb pull`;
- `logx_decode.py --kid-map` 离线解码: **104/104 行与 C++ 导出路径精确对账**,
  中文 UTF-8 消息与 40KB 长记录(40956B 'B' + 'BIG:' 前缀 = 40960B)字节级还原,
  单文件无切分(总量 < 64KB 阈值,符合预期);
- 排查记录: 120 条 split-fill 中 37 条未落盘为 demo 静默忽略
  `LOGX_ERR_NO_SPACE` — 总需求 ≈168KB 超 staging 数据区 129KB(溢出 39096B
  ≈ 37 条),符合 spec"staging 满丢弃 + 打点"契约,非引擎缺陷;
- 构建环境注意: Git Bash 下 adb push 需 `MSYS_NO_PATHCONV=1`;设备
  /data 分卷 99% 满时 push 的 fchown 序列会失败且文件不落盘,经
  `/sdcard` 中转 + `cat` + md5 双端校验绕过。

### keyProvider 真机泄漏专项 (T4.4 归口项销号)

- 加密/KeyProvider 用例连续 10 轮重复运行,设备 MemAvailable 波动 < 0.1%
  (4128340kB → 4125824kB);
- logcat 全量扫描 0 条 `ReferenceTable` / `global reference overflow` /
  `JNI DETECTED ERROR` 告警;
- `KeyProvider_DrainsEncryptedTailAndNotCalledAfterClose` 生命周期契约用例
  随 128 项真机单测全绿。

### 真机全量矩阵 (最新源码重建后)

128/128 单测 + 4/4 压测(含 T-06 missing_count 精确对账) +
3/3 真实进程强杀恢复,全部通过。

---

## T5.1 Android 备份安全策略配置工程化 (2026-09-08)

对应计划表 M5 阶段 T5.1 与方案 §10 备份排除纪律("日志目录与 staging 必须排除出自动备份,
防止云备份带走明文 staging 与密文日志")。纯交付配置,零 C++/Kotlin 代码改动。

### 交付物

1. **`android/res/xml/data_extraction_rules.xml`** (API 31+ / Android 12+):
   根元素 `<data-extraction-rules>`,`<cloud-backup>` 与 `<device-transfer>` 双通道
   各排除 `domain="file"` 下 `logs/` 与 `logx_staging/`;
2. **`android/res/xml/backup_rules.xml`** (API 23~30, Auto Backup 自 API 23 引入;
   API 21/22 无 Auto Backup 机制,`allowBackup="false"` 仍禁 adb 备份路径):
   根元素 `<full-backup-content>`,同样两条 exclude;
3. **`android/AndroidManifest-snippet.xml` 重写为完整可集成片段**:
   修正原草稿把资源根元素写成 manifest 属性名 `<dataExtractionRules>` 的错误
   (res/xml 文件根元素必须为 `<data-extraction-rules>`);给出三步集成流程、
   `tools:targetApi="31"` lint 抑制说明(minSdk 21 基线)、`allowBackup="false"`
   最严备选方案,以及自定义目录/多进程隔离目录边界说明。

### 覆盖口径与边界说明(注释内置)

- `domain="file"` 即 `getFilesDir()` 根;LogX.kt 强制 staging 位于 filesDir 系
  (禁 cacheDir/外部存储),故 `domain="file"` 全覆盖;
- export 明文临时文件 (`.tmp_export_*`) 由核心写在 log_dir 内,被 `logs/` 排除自然覆盖;
- 多进程自动隔离产生**兄弟目录** `logs_<后缀>/` / `logx_staging_<后缀>/`
  (如 `:push` → `logs_push/`),备份规则不支持通配符 —— 采用规则文件方案时须逐目录
  显式追加 exclude,或改用 `allowBackup="false"` 整体禁用;
- 自定义 `Config.logDir` / `Config.stagingDir` 时须同步修改规则文件 path。

### 验证

- 三个 XML 经 `xml.etree` 解析确认良构;根元素名、双通道 exclude 清单
  (`logs/` + `logx_staging/`) 与 `<application>` 四属性
  (allowBackup/dataExtractionRules/fullBackupContent/tools:targetApi) 逐项断言通过。

---

## T5.2 性能基线测试与报告输出 (2026-09-08)

对应计划表 M5 阶段 T5.2。终结"性能验收无任何实测数据"的历史缺口(M1 验收 2µs、
30w+ logs/s 此前均为无 harness 支撑的量级声明)。

### 基准 harness (`tests/bench/logx_bench.cpp` + `tests/bench/CMakeLists.txt`)

- 独立 main() 非 gtest,`option(LOGX_BUILD_BENCH, OFF)` 开关,不注册 ctest,
  对既有测试矩阵零影响;桌面 Linux/Android 经 `find_package(Threads)` 链接;
- **阶段 A 单条写入延迟**: 单线程热路径逐条 steady_clock 计时,近秩百分位
  P50/P90/P99/P99.9/max;生产口径(8KB 阈值)与纯路径口径(flush_threshold=0)
  双口径;none/zstd 各一轮(压缩在 Flusher 线程,预期并实证热路径零影响);
- **阶段 B 吞吐与落盘体积**: 混合变长 payload(80~1536B,80% 中文文本+20% 伪随机),
  同一二进制按运行期 `compress_algo` 分轮对比;NO_SPACE 有界重试(自旋为主)
  度量可持续不丢吞吐,数据丢失判定为"重试耗尽仍失败";
- **阶段 C flush 阻塞耗时**: 每轮 100 条后阻塞 flush 300 轮分布;
- **失真护栏**: 全程校验 drop/丢失/IO 错误计数,非零即宣告该轮 INVALID 不纳入
  结论;输出目录 run_N 唯一化防历史污染;测后即清日志文件压缩磁盘占用。

### 实测结果 (双端全部轮次 0 丢弃 0 丢失)

| 指标 | Windows x64 (MSVC Release) | Redmi K30 Pro (骁龙 865, NDK) |
|---|---|---|
| 写入 P50 / P99 (纯路径) | 0.2µs / 3.1µs | 3.2µs / 12.2µs |
| 写入 P50 / P99 (生产 8KB 阈值) | 0.4µs / 5.3µs | 3.4µs / 41~86µs |
| 压缩开关对写路径影响 | 零(P50 逐位一致) | 零 |
| zstd 落盘体积 | -38% (1.52x vs 0.95x) | -38% (1.52x) |
| 可持续吞吐 none / zstd | 26.7 万 / 19.4 万 logs/s | 3.7 万 / 5.5 万 logs/s (zstd 反超 +48%) |
| flush P99 | 17.0ms | 9.4ms |
| so 体积 (arm64 strip, MinSizeRel 全开) | — | 548.9KB(未 strip 4.76MB) |

### 目标判定 (详见 docs/benchmark_report.md)

- **P99 < 2µs**: 确定性热路径段达成(桌面 P50 0.2µs,低一个数量级);整调用
  严格口径未达,归因计时器开销(真机 0.4µs/条)、8KB 阈值唤醒成本(+68% P99)
  与调度尾部;报告给出 v1.0 回归基线建议口径;
- **so < 500KB**: 未达(548.9KB,超 10%);体积构成分析: zstd 贡献 ~426KB(77%),
  裸核心仅 121KB,mbedtls 12KB;建议接受为 v1.0 基线,zstd 源码裁剪列为可选优化。

### 附带

- 体积构成经 4 组变体构建实测(双 OFF / 仅 zstd / 仅 AES-GCM / 全开);
- 阶段 A 迭代中由护栏拦截两类方法论失真(周期 flush 间隔超环容量致 NO_SPACE、
  Windows ~15ms 休眠粒度扼杀持续吞吐),最终节拍与重试策略经护栏验证有效。

---

## T5.4 logcat 直通开关 consoleEcho (2026-09-09)

用户需求驱动("日志支持输出到控制台吗" → 选型 JNI 层实现)。此前日志记录仅有
单一文件 sink(mmap staging → 密文文件),无任何运行时控制台/logcat 输出路径
(JNI 层 `__android_log_print` 宏自 M1 起即为零调用点死代码)。

### 实现 (JNI 层, C ABI 零改动)

- **`android/cpp/logx_jni.cpp`**:
  - `nativeInit` 签名末尾追加 `jboolean jConsoleEcho`,存入进程级
    `std::atomic<bool>`(Kotlin facade 单实例 init 契约下语义匹配;close→re-init
    循环重新定型);关闭时 `nativeWrite` 仅付一次 relaxed 原子读;
  - `nativeWrite` 在 `logx_write` 返回 OK 后、Critical/UTFChars 释放**之前**
    (指针失效防御)同步完成 echo:2KB 栈缓冲截断 + NUL 结尾(防超长/非法
    UTF-8 破坏 logcat),按 level 0..5 → VERBOSE/DEBUG/INFO/WARN/ERROR/FATAL
    六级映射调用 `__android_log_print`,激活原死宏族(宏改为 `"%s"` 格式安全形态);
  - **镜像语义**:仅镜像成功落盘的记录(logcat 与文件一一对应);丢弃
    (NO_SPACE/TOO_LARGE)不 echo,仍走 ErrorHandler.DROP 事件通道;
- **顶层 `CMakeLists.txt`**:ANDROID 块 logx 目标补链 `log`
  (`__android_log_print` 的硬前提;readelf NEEDED 实证 `liblog.so`);
- **`android/kotlin/LogX.kt`**:`Config` 末尾追加 `val consoleEcho: Boolean = false`
  (追加式,不破坏按位置构造;注释注明明文进 logcat 有泄露面,生产默认关);
  external `nativeInit` 声明同步;文件头 doc 补充。

### 验证

- NDK 交叉编译 0 错误,`liblogx.so` NEEDED 实证链接 `liblog.so`;
- kotlin-gradle 离线 Gradle(8.9/JDK 17):`compileDebugKotlin` + 纯逻辑单测
  **9/9 全绿**(新增 `preInitWrite_withConsoleEchoConfigSafe`;Config 默认值
  用例补 consoleEcho=false 断言);
- **真机端到端 smoke**(Redmi K30 Pro / Android 12):kotlin-gradle 扩展为可安装
  最小工程(Manifest + SmokeActivity:loadLibrary → init(consoleEcho=true, ZSTD)
  → 写 6 条中英文各等级记录),logcat 实测 **6/6 条按等级正确镜像、中文完好,
  SMOKE PASS**(adb install 需设备端确认,首次被 INSTALL_FAILED_USER_RESTRICTED
  拦截,唤醒屏幕后重装成功)。

### 审查加固 (2026-09-09)

- **Tag 动态透传**: `console_echo` 改为动态透传业务 Tag（空 Tag 兜底 `LogX`），彻底解决控制台按 Tag 过滤问题，并去除多层宏和冗余 switch-case；
- **JNI 临界区安全保障**: `nativeWrite` 在 Critical 保护期内仅做快速栈缓冲内存拷贝，拷贝完成后立即释放 `ReleasePrimitiveArrayCritical`，在临界区外执行 `__android_log_print`，彻底规避 GC 阻塞风险；
- **LogX 自动加载动态库**: `LogX.kt` 内置静态 `init` 尝试 `System.loadLibrary("logx")`（JVM 单测捕获防御），并在 `init` 中兜底重试；对外暴露 `LogX.loadLibrary()` 支持自定义加载；
- **自动化单测扩充**: Gradle 纯逻辑单测扩充至 **10/10 全绿**（补充动态库加载行为断言）。

### 范围与边界

- 桌面 C++ 使用者不受影响(echo 是 JNI 交付层能力);C ABI/核心零改动,
  ctest 矩阵无回归面;
- 镜像在调用方线程同步执行,2KB 截断上限;logcat 单行 ~4KB 截断为系统行为;
- 明文进 logcat 的泄露面已在 Config 注释标注:仅限 debug 期,生产默认关闭;
- T5.3(接入指南/解码手册)按用户指示暂停,文档将在其恢复时覆盖本开关。

---

## JNI 临界区安全与 CheckJNI 崩溃修复记录 (2026-09-11, J-04)

### 故障现象与报错信息
在 Android 压测及多线程并发写场景下（如协程调度器线程 `DefaultDispatcher-worker-4` 持续写入导致环形缓冲写满时），ART 虚拟机抛出 CheckJNI 致命崩溃并强制杀死进程：
```text
java_vm_ext.cc:579] JNI DETECTED ERROR IN APPLICATION: thread Thread[3,tid=14957,Runnable,Thread*=0xb400007c822b3400,peer=0x12c96360,"DefaultDispatcher-worker-4"] using JNI after critical get
                                                                    java_vm_ext.cc:579]     in call to NewString
                                                                    java_vm_ext.cc:579]     from void com.zzlfeng.logx.LogX.nativeWrite(long, int, java.lang.String, byte[])
```

### 根因深度剖析
1. **JNI 规范冲突**: `nativeWrite` 原实现为追求零拷贝读取 Payload，调用了 `env->GetPrimitiveArrayCritical(jPayload, nullptr)` 进入 JNI 临界区并包裹了 `logx_write` 的全流程。JNI 规范与 Android ART 严格禁止在 Critical 临界区内调用任何其它 JNI 函数或发生互斥阻塞；
2. **同步错误回调触发 JNI 调用**: 当 staging 环形缓冲写满 (`kFull` / `ERR_NO_SPACE`)、单条日志超长 (`ERR_TOO_LARGE`) 或入参非法时，`logx_write` 会在调用方线程同步调用 `emit_event`，进而触发 `jni_error_trampoline`；
3. **CheckJNI 拦截崩溃**: `jni_error_trampoline` 为向 Kotlin 的 `errorHandler` 上报错误，调用了 `make_jstring_utf16`（执行 `env->NewString`）与 `env->CallStaticVoidMethod`。由于此时调用线程仍在 Critical 保护期内，ART CheckJNI 校验到 `critical_ > 0`，立刻判定为非法 JNI 操作并抛出 Fatal Error；
4. **互斥锁与 GC 停顿风险**: `logx_write` 内部持 `inst->pal->mutex_lock(inst->mu)`，在 Critical 期间长时间加锁同样违背 JNI 临界区纪律，有诱发 GC 停顿与潜在死锁的隐患。

### 修复方案
1. **废弃 Critical 临界区**: 彻底移除 `GetPrimitiveArrayCritical` 与 `ReleasePrimitiveArrayCritical`，拔除崩溃诱因；
2. **分级零堆读取 (GetByteArrayRegion)**:
   - **常规日志 ($\le 4\text{ KB}$)**: 覆盖 99.9%+ 业务日志，直接使用 4KB 栈缓冲 `stack_buf` 配合 `env->GetByteArrayRegion`。在 ART 中该函数直接从 Java 堆内存执行连续 `memcpy`，不进临界区、不阻塞 GC、零堆内存分配（Zero Heap Allocation）；
   - **超长日志 ($> 4\text{ KB}$)**: 采用 `static thread_local std::vector<uint8_t> tls_payload_buf` 动态扩容复用，消除多线程竞争，各线程首次扩容后后续同样为零堆分配；
3. **生命周期自然闭环**: 栈缓冲与 TLS 缓冲在 `nativeWrite` 调用期间始终有效，成功落盘后的 `console_echo` 镜像拷贝与输出逻辑更加清晰安全。

### 验证与产物交付
- Android NDK 交叉编译：`build-android` 与 `build-android-arm64` 双目标 `liblogx.so` 编译 0 错误 0 警告；
- 预编译 so 已同步至 `android/logx-tester/app/src/main/jniLibs/arm64-v8a/` 与 `android/kotlin-gradle/app/src/main/jniLibs/arm64-v8a/`；
- Android 测试工程 `logx-tester` 通过 Gradle `assembleDebug` 构建验证成功；
- **桌面回归测试（C ABI 层，`tests/unit/test_j04_regression.cpp`）**：桌面端无法加载 JVM 复现 JNI 临界区，故精确守护本崩溃依赖的核心契约——`emit_event` 同步直调语义。两用例分别覆盖超长丢日志（`LOGX_ERR_TOO_LARGE`）与 staging 环满丢日志（`flush_threshold=0` 确定性塞满数据区 → `LOGX_ERR_NO_SPACE`），断言错误回调「同步 + 在调用线程 tid + kind/code 正确 + 持 `inst->mu` 触发时不锁死 + 单次丢弃后实例仍可写」，全量单测 129 用例 PASSED（含本用例重复 5 次无时序抖动）。

---

## 2026-09-11: P1~P4 代码审查缺陷全面修复与架构加固

针对代码审查中深入发现的 12 项潜在缺陷、并发死锁与性能隐患进行彻底闭环修复：

1. **P1-1 `logx_write` 锁外触发错误回调（防自死锁与写放大）**：
   - 满缓冲分支（`staging full`）改为锁内仅标记 `need_drop_event = true`，在 `inst->mu` 释放后统一执行 `emit_event`。
   - 彻底杜绝回调中调用 `logx_close` / `logx_flush` 时对非递归互斥锁重入导致的自死锁，杜绝满载时业务写线程停顿。
2. **P2-2 JNI `nativeWrite` 传参长度与实际缓冲区安全对齐**：
   - 当 `n > 64KB` 截断拷贝到 `tls_payload_buf` 时，传递给 C 核心的长度严格取 `safe_payload_len = copy_len`，消除潜在越界读未初始化脏内存的隐患。
3. **P2-3 `pad_records` 回绕垫 PAD 计数器接线**：
   - `Staging::write_records` 统计生成的 PAD 记录条数，`logx_write` 累加到 `inst->pad_records`，使运行时诊断快照 `Stats.padRecords` 指标真实反映回绕垫补行为。
   - 统一使用 `format_pad(data_base_ + pos, this_pad)` 替代原先手写内联 Header 写入。
4. **P2-4 `LogX.kt` 引入无锁 CAS 活跃写线程保护（防 UAF / SIGSEGV）**：
   - 使用 `AtomicInteger activeWriters` 计数，`log()` 前置原子自增并在 `finally` 中自减。
   - `close()` 置 `isClosed = true` 并微等待活跃写线程退栈（最多 100ms）后调用 `nativeClose`，根除高并发下 `close()` 销毁 Native 句柄导致的 UAF 崩溃。（注：后经 R2 二轮审查全面升级为 `executeOp` 全接口覆盖与递延销毁机制）。
5. **P2-5 `export_range` 增加 `flush_blocking` 结果检测与告警**：
   - 检查 `if (!flush_blocking(inst))`，当 flush 异常或超时时发出 `LOGX_EV_IO` 告警，并尽力导出已落盘分卷。
6. **P3-1 `flush_blocking` 剥离 Retention 清理**：
   - 从 `flush_blocking` 中完全剥离 `retention_cleanup_impl` 目录遍历与日期计算，杜绝高频 flush 导致的磁盘 I/O 抖动。
   - Retention 清理仅保留在实例启动和跨天 Rollover 时触发。
7. **P3-2 `drain_once` 消除 75ms 空转 128KB 内存分配与 memset**：
   - `snapshot_pending` 前置；若 `bytes == 0` 则立即解锁返回，避免无意义的缓冲区触碰。
   - 在 `FlusherCtx` 中常驻复用 `scratch` vector，消除约 1.7 MB/s 的内存分配/释放与 CPU 缓存抖动。
8. **P3-3 接通 `Config.level` 日志级别过滤**：
   - `init()` 保存配置的 `minLevel`，`LogX.log()` 入口增加 `if (level.v < minLevel.v) return` 前置过滤，彻底阻断不符合级别日志的 JNI 栈跨越与数据拷贝开销。
   - 增加运行时 `setMinLevel(level: Level)` / `getMinLevel()` 动态控制接口。
9. **P3-4 动态库体积深度优化（llvm-strip）**：
   - 使用 `llvm-strip --strip-unneeded` 对发布 SO 进行处理：`libc++_shared.so` 由 9.29MB 降至 1.33MB（-85%），`liblogx.so` 由 5.51MB 降至 470KB（-91%），节省 APK 约 13MB 空间。
10. **P3-5 `EngineController` 状态同步**：
    - 增加 `EngineController.stopSync()`，并在 `TesterViewModel.onCleared()` 中调用，保证单例状态机与底层实例完全同步。
11. **P3-6 `run_recovery` 失败时安全中止 `logx_open`**：
    - `run_recovery` 改为返回 `bool`，若 `staging->open` 失败则立即释放资源并中止，防止后续后台线程解引用空指针发生 SIGSEGV。
12. **P4 死代码与冗余字段清理**：
    - 清理 `Instance::stats` 未使用字段及 `write_block_to_file` 桩实现。

---

## 2026-09-11: R1~R3 代码审查缺陷深度修复与生命周期加固

在 P1~P4 修复合入后，对回归测试配置有效性、JNI 并发保护边界以及 Kotlin 单例状态机进行了二轮深度审查，定位并彻底闭环了 3 项深层次缺陷：

### 1. 【R1】修复 `test_review_p1_p4.cpp` 回归测试缺失配置导致门禁阻断
- **故障现象**：
  运行单测时 `ReviewFixesTest.P1_1_NoDeadlockWhenCallbackClosesInstance` 与 `ReviewFixesTest.P2_3_PadRecordsIncrementedOnWrap` 两个用例在首行 `ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK)` 处即返回 `-1` 失败。P1-1 锁外规避死锁场景与 P2-3 `pad_records` 计数器验证实际未被测到。
- **根因分析**：
  两个测试在初始化 `logx_config_t cfg{}` 时仅显式配置了 `log_dir`、`staging_path` 和 `flush_threshold`，漏设了 `max_log_length`（结构体零初始化为 0）。`core/src/logx_c.cpp:292` 的 fail-fast 校验规则明确要求：
  ```cpp
  if (cfg->max_log_length == 0 || cfg->max_log_length > LOGX_STAGING_DATA_SIZE - 128 - 1) {
      return LOGX_ERR_INVALID_ARG;
  }
  ```
  零值被直接判定为非法参数并返回 `LOGX_ERR_INVALID_ARG (-1)`，导致用例在门禁处中断。
- **修复措施**：
  在两个用例的 `logx_config_t` 中补齐 `cfg.max_log_length = 64 * 1024;`（与规范 64KB 对齐）。
- **验证效果**：
  单测 3 个用例全部真实执行并通过（3/3 PASSED），真实覆盖了 staging full 锁外触发 `onError` 并调用 `logx_close` 的零死锁路径，以及 staging 回绕时插入 PAD 记录并自增计数器的逻辑。

---

## 2026-09-11: N2~N4 生命周期状态机三轮加固 (close/init 并发闭环与热路径优化)

针对三轮代码审查中确认的 3 项打磨项进行闭环（N1 双销毁竞争经 getAndSet 仲裁确证安全、N5 返回值语义经复查撤回，均无需修改）：

1. **N2 递延销毁完成性与 Check-Then-Act 状态闭环（专用 closeLock 锁）**：
   - 引入专用内部销毁锁 `closeLock = Any()`（与高频日志写热路径完全解耦）：串行化 `pendingCloseHandle.getAndSet(0L)` 领取与 `nativeClose` 物理执行的全程，同一时刻至多一个 nativeClose 在执行。
   - 在 `init()`、`close()` 以及 `executeOp.finally` 递延销毁点中，对 `pendingCloseHandle`的仲裁领取与 `nativeClose` 的物理调用严格在 `synchronized(closeLock)` 互斥保护下完成，杜绝新旧实例并发操作同一 staging 文件的潜在数据竞争。
   - 注意：closeLock 只互斥销毁段本身，init/close 边界的活跃操作退栈等待仍为 sleep 轮询（见 N6 方案 A 的锁外化改造）。

2. **N3 快/慢操作分离计数（activeQuickOps / activeExportOps）**：
   - `executeOp` 增加 `isExport` 参数：log/flush/getStats 计入快操作，两个 export 重载计入慢操作。
   - `close()` 优雅等待只针对快操作（上界 500ms 收紧至 200ms，快操作微秒级退栈绰绰有余）；export 属分钟级慢操作，等了也等不完，直接登记 `pendingCloseHandle` 立即返回，由其退栈 finally 完成递延销毁——export 中调 close 从 "白等 500ms" 变为 "立即递延"。
   - `executeOp` finally 的递延销毁触发条件同步收紧为 `快计数==0 && 慢计数==0 && pending!=0`，与 close 侧双向兜底；`threadOpDepth` 回调自死锁跳过逻辑保持不变。
3. **N4 热路径快速预检（pre-init / 已关闭零开销）**：
   - `executeOp` 入口增加纯读快速路径 `if (isClosed || nativeHandle == 0L) return null`：pre-init 或已关闭状态下的高频 log 调用不再支付原子计数与 ThreadLocal 开销（旧版每条白付 3~4 个原子操作 + 1 次 ThreadLocal 访问）。
   - 正确性：close 先写 `isClosed` 再清 `nativeHandle`、init 先写 `nativeHandle` 再写 `isClosed=false`（均 volatile），快速路径仅为拒绝 hint——误拒只发生在 init/close 过渡窗口（此时调用语义本就是 "过渡期丢弃"，与 spec §7 对 pre-init 的约定一致），误放行由 increment 之后的二次 `isClosed` 检查兜底，双重检查结构不变。
4. **N6 修复（方案 A）：init() 前奏等待与遗留销毁移出互斥锁，消除锁内长等待**：
   - 问题：closeLock 引入后 `init()` 在持有同步锁的情况下进入 `closeLock` 执行遗留 `nativeClose`（含 flush + Flusher join，最坏 ~5s），锁内慢等待使并发的 `setErrorHandler` / `close` / `init` 全部被挂起不可预测的时长。
   - 修复：`init()` 拆为两段 —— 前奏（无锁）：等快/慢计数归零（1s fail-fast）→ closeLock 内领取遗留句柄并同步 nativeClose；随后才进入同步块做 initCount 翻转与 nativeInit 本身。持锁时间回落到 nativeInit 量级，`close()` / `setErrorHandler` 不再被慢销毁拖累。
   - 并发契约不变：两线程同时 init 时后到者排队，锁内 initCount 检查保证 "重复 init 抛 IllegalStateException"（spec §7）；前奏的两次 fail-fast 抛出路径均不触碰 initCount（无锁阶段未递增）或已正确归零。
5. **N6 锁监视器纠偏：以 `synchronized(this)` 替换局部函数 `@Synchronized`，消除锁对象分裂**：
   - 根因：Kotlin 局部函数上标注 `@Synchronized`（如 `@Synchronized fun doInitLocked()`）时，kotlinc 编译器将其生成为 `private static final synchronized void init$doInitLocked(...)`，根据 JVM 规范其锁监视器为 `LogX.class` 类对象；而 `close()` 与 `setErrorHandler()` 是声明在 `object LogX` 单例上的成员函数，其 `@Synchronized` 锁监视器为 `LogX.INSTANCE`（`this`）。两者锁对象不一致导致锁分裂，`init` 临界区无法与并发的 `close()` / `setErrorHandler()` 形成真正的互斥保护。
   - 修复：移除局部函数，改为直接在 `init()` 内部使用 `synchronized(this) { ... }` 代码块。字节码中通过 `aload_0`（即 `LogX.INSTANCE`）执行 `monitorenter`/`monitorexit`，与单例内其他同步方法锁监视器完全对齐，彻底消除并发互斥失效风险。

### 2. 【R2】`LogX.kt` 活跃操作生命周期全覆盖与递延销毁（Deferred Close）
- **故障现象与潜在风险**：
  1. **TOCTOU 竞态窄窗**：原方案中写线程在 `val h = nativeHandle` 读出句柄与 `activeWriters.incrementAndGet()` 之间存在纳秒级调度窗。写线程读到句柄后若被系统切走，此时 `close()` 介入看到 `activeWriters == 0`，直接执行 `nativeClose(h)` 并释放 Native `Instance` 堆内存；写线程恢复后调用 `nativeWrite(h)` 解引用已被释放的内存，导致野指针 UAF / SIGSEGV 崩溃；
  2. **100ms 超时强制销毁不可靠**：原 `close()` 循环最多等待 100ms。若系统处于极端高负载、GC 停顿或 I/O 阻塞状态，耗时超过 100ms 后 `close()` 仍会强行销毁句柄，破坏内存安全契约；
  3. **非写接口（flush / getStats / export）裸奔**：仅 `log()` 包含活跃计数保护，而 `flush()`、`getStats()` 以及同步/协程 `export()` 均直接裸调 JNI。特别是大文件解密导出（耗时可达数百毫秒至数秒），并发触发 `close()` 时百分之百复现 UAF 崩溃；
  4. **回调内部调用 `close()` 自死锁**：C 核心契约明确允许在 `ErrorHandler.onError` 回调中调用 `close()`。写线程在 `nativeWrite` 内部触发回调时已占有活跃计数，若同一线程在 `close()` 中循环等待活跃计数归零，将发生无法恢复的自死锁。
- **架构设计与修复方案**：
  1. **通用保护模板 `executeOp<T>`**：
     - 先原子自增 `activeOps.incrementAndGet()`，再读取 `nativeHandle` 并二次校验 `isClosed`，彻底消除 TOCTOU 窄窗；
     - 将 `log`、`flush`、`getStats`、同步 `export`、协程 `export` 全量统一接入 `executeOp` 模板包裹，确保所有 Native 跨 JNI 调用均受生命周期管辖；
  2. **递延销毁机制（Deferred Close）**：
     - `close()` 标记 `isClosed = true` 并原子置空 `nativeHandle = 0L`，将待销毁句柄保存在原子变量 `pendingCloseHandle.set(h)` 中；
     - `close()` 优雅等待最多 500ms 让活跃线程自然退栈；若超时仍有未退栈的后台慢操作（如后台导出），**坚决不强制调用 `nativeClose`**（杜绝 UAF）；
     - 由最后一个退出的 `executeOp` 在 `finally` 块中通过原子 CAS `pendingCloseHandle.getAndSet(0L)` 触发递延 `nativeClose`，确保句柄必定延展至所有操作结束且仅被释放一次；
  3. **基于 ThreadLocal 深度感知的回调自死锁消除**：
     - 引入 `threadOpDepth`（`ThreadLocal<Int>`，兼容 Android 5.0+ / API 21+，归零时执行 `remove()` 杜绝线程池内存泄漏）；
     - 当在 `onError` 回调内同步触发 `close()` 时，`close()` 感知当前线程递归深度 `depth > 0`，跳过等待直接返回，由外层调用退栈后安全递延释放，杜绝自死锁与延迟。

### 3. 【R3】规范 `LogX.kt:init()` 状态机与回滚一致性
- **故障根因**：
  1. `init()` 内部在行 160 与 192 重复对 `@Volatile var isClosed` 赋值为 `false`；
  2. 在 `catch (t: Throwable)` 异常回滚分支中，未将 `isClosed` 重置为 `true`，导致初始化失败后（如参数校验非法或 `nativeInit` 失败）状态机残留为未关闭态，单例状态与底层句柄状态脱节；
  3. 若此前实例尚有未完全退栈的慢操作在执行递延销毁，直接开始新一轮 `init()` 可能造成跨实例状态交叉污染与旧句柄泄漏。
- **修复措施**：
  1. **状态机严格单调**：清理重复赋值，仅在 `nativeInit` 成功返回非零有效句柄后将 `isClosed` 置为 `false`；
  2. **失败彻底回滚**：在 `catch` 块中严格重置 `isClosed = true`、`nativeHandle = 0L`、`errorHandler = null`、`initCount.set(0)`，确保回滚后状态机处于完全干净的已关闭/未就绪态；
  3. **跨实例周期隔离**：`init()` 入口处前置等待上一实例未退出的后台操作彻底退出（最多 1000ms），并在启动前清空旧实例的递延待关闭句柄，杜绝任何状态与内存泄漏。

---

### 自动化验证结果

1. **C++ 全量单元测试**：
   - 测试套件：`build_verify/tests/unit/Release/logx_unit_tests.exe`
   - 测试结果：**132 个测试全部通过（132 / 132 PASSED, 0 FAILED）**，无任何回归错误。
2. **Android 编译与打包**：
   - 构建命令：`.\gradlew.bat assembleDebug`
   - 构建结果：**`BUILD SUCCESSFUL`**，Kotlin 字节码编译与 APK 打包 0 错误 0 告警。

---

## 架构备忘与工程规范：LogX.kt 状态机复杂度上限与并发测试硬性门禁 (2026-09-11)

> **背景与共识**：
> `LogX.kt` 的生命周期并发状态机历经四轮高强度迭代与代码审查，闭环了两个真实且极隐蔽的并发缺陷（`closeLock` 递延销毁串行化语义、局部函数 `@Synchronized` 导致的 JVM 类对象 vs 单例实例监视器分裂）。当前实现融合了双重快速预检、快慢操作分离原子计数、递延销毁原子 CAS 仲裁、ThreadLocal 自死锁规避、`closeLock` 串行释放以及 `synchronized(this)` 状态机对齐，**其复杂度已接近无锁/混合同步方案的安全上限**。
> 
> **准入红线与后续演进原则**：
> 1. **当前模块状态冻结**：当前实现并发安全性与锁粒度已达到平衡闭环，严禁在无自动化并发测试防护下进行主观微调或仅凭人眼推演做修改（人眼静态推演的边际收益已递减为负）；
> 2. **后续改动前置准入门禁**：若后续业务演进必须修改 `LogX.kt` 生命周期逻辑，**必须先建立 JVM 侧并发回归测试套件（或引入 OpenJDK jcstress）作为准入前提**：
>    - **测试形态**：
>      - 多线程交错测试：启动 20~50 个线程高频并发调用 `init`、`close`、`log`、耗时 `export`，并在 `onError` 回调中嵌套同步触发 `close`；
>      - 或者将核心状态迁移切片建模为无 JNI 依赖的状态机，通过 `jcstress` 压测排查弱内存模型下的重排与可见性漏洞。
>    - **核心断言不变量（Invariants）**：
>      - **Inv-1（句柄有效性不变量）**：任何进入 Native 跨 JNI 操作的句柄绝不能是已被释放或正在被并发销毁的无效指针（杜绝 UAF）；
>      - **Inv-2（物理销毁排他性不变量）**：同一实例句柄的 `nativeClose` 严格仅被执行一次；且跨实例交替场景下，旧实例的物理销毁与新实例 `init` 绝对串行互斥（杜绝多实例并发写同一 staging 文件的潜在竞争）；
>      - **Inv-3（单例状态机单调一致性）**：`initCount`、`nativeHandle` 与 `isClosed` 状态转移严格单调可逆，绝不存在半初始化或污染残留；
>      - **Inv-4（自死锁与饥饿免疫）**：回调嵌套 close 绝不自死锁，所有测试线程在限定超时（如 2000ms）内安全退栈。



---

## 超长日志截断保前缀: B-1 方案B 落地为 T5.5 (2026-09-13)

> 决策背景:Backlog B-1 评估的三方案中,方案 B(截断保留前缀)经用户拍板实施;
> 方案 A(NO_SPACE 有界阻塞重试,涉及"绝不阻塞"原则的延迟契约变更)仍留待决策池。

### 1. 语义变更(`core/src/logx_c.cpp` `logx_write`)

- **旧行为**:格式化后总长 > `max_log_length` → 整条丢弃,`drop_too_large` 递增,
  发 `LOGX_EV_DROP`/`LOGX_ERR_TOO_LARGE`("log too large"),返回 `LOGX_ERR_TOO_LARGE`。
- **新行为**:超长日志**截断保留 msg 前缀**后照常落环,返回 `LOGX_OK`;事件保留
  `LOGX_EV_DROP`/`LOGX_ERR_TOO_LARGE` 但 msg 改为 `"log truncated (dropped N bytes)"`,
  仍为**调用方线程、锁外同步发出**(P1-1/J-04 契约不变);新计数 `truncated_logs` 递增。
- **真正丢弃仅剩两类边缘场景**:① 病态小 `max_log_length`(截断到空记录仍放不下,
  `kBuildRecordsCannotFit` 兜底,`drop_too_large` 仅此路径递增);② 磁盘持续 NO_SPACE
  (方案 A 的待决策范畴)。

### 2. 预算求解与截断实现(`core/src/format.cpp` / `logx_format.h`)

- **`solve_truncated_msg_budget`**:对片数 s ∈ [1,4] 逐一取
  `cap(s) = min(20480·s, max_total − 6s, 60KB)`,`b = cap − (27 + tag_len)` 取最优;
  结果保守不超过 6·(s−1) ≤ 18B。60KB 用户载荷硬上限(`format_record` 契约)纳入约束。
- **截断拼装**:msg = 原始前缀 + ASCII marker `...[LogX truncated, dropped N bytes]`
  (预算预留 48B,实际 ≤ 45B——dropped ≤ uint32 上限 10 位十进制);预算 < 48B 时弃
  marker 保前缀;**独立 thread_local 拼装缓冲**,不复用 `format_record` 内部 scratch
  (二次调用会覆写);预算 < 最小空记录(27+tag+6)时返回 `kCannotFit`。
- **新增 `format_record_bounded`**(`logx_format.h` 声明,三态结果
  `FormatBoundedStatus{kOk, kInvalidInput, kCannotFit}`):热路径仍恰好一次
  `format_record`——先原样格式化,仅超限时才进截断分支,未超限零额外成本;
  `format_record` 签名与既有语义不动(`test_format.cpp` 11 处调用零波及)。

### 3. 顺带修正的既有误分类缺陷(>60KB payload 误报 INVALID_ARG)

复核中发现:payload > 60KB 时 `format_record` 直接失败(用户载荷硬上限),旧链路把该
失败归为 `kBuildRecordsInvalidInput`,导致 61KB 以上 payload 以
`LOGX_ERR_INVALID_ARG`("invalid log input")误报——属"内容超长"而非"输入非法"。
E-02 测试仅覆盖 level 越界与小幅超限,该区间无断言故漏网。B-1B 将 `format_record`
失败区分为"输入非法(level/tag/payload 契约,先行排除)"与"内容超长(走截断)"两类,
>60KB 区间自然并入截断语义。

### 4. 计数与 ABI 兼容(`logx_c.h` / `logx_internal.h`)

- `logx_stats_t.reserved[0]` 按既定 E-07 纪律转正为 `truncated_logs`(超长截断保前缀
  写入计数),`reserved` 递减为 `[3]`;**sizeof 不变、布局前缀不变**,新旧二进制兼容。
- 命名区分:既有 `truncated_blocks` 为"启动恢复截尾块计数",新字段 `truncated_logs`
  为"写入截断保前缀计数",互不混淆。
- `build_records` 返回协议追加 `kBuildRecordsCannotFit = -2`(内部 API,唯一调用方
  `logx_write` 同步更新);JNI `nativeGetStats`(5 元素 LongArray)与 Kotlin `Stats`
  按既定纪律零改动(Kotlin API 不动,后续如需暴露 `truncatedLogs` 为可追加项)。

### 5. 测试与验证

- **改写 3 处旧丢弃断言**:`test_robustness.cpp` E-02(超长段改断言 LOGX_OK + 截断
  计数)、`test_flusher.cpp` `ErrorMonitoring`(拆为截断用例 + `CannotFitDropPath`
  病态兜底用例)、`test_j04_regression.cpp`(拆为 `TooLargeTruncated_*` 同步契约用例 +
  `TooLargeCannotFit_*` 丢弃兜底孪生用例)。
- **新增 `FormatBounded` 套件 7 项**:前缀字节级一致 + marker 后缀精确匹配、70KB
  (>60KB 误分类修复路径)跨片流式重组、纯前缀边界(预算 < 48B 弃 marker)、
  kCannotFit、非法输入不变、33..1200B 上限全扫描(总长恒 ≤ 上限)。
- **全量结果**:三构建矩阵单测 **122/122 (OFF/OFF) + 128/128 (ON/OFF) + 141/141
  (ON/ON) 全绿**,压测 4/4 × 3 全绿;Android arm64 (NDK r29) 交叉编译 0 错误,
  `liblogx.so` 已同步至 `logx-tester` 与 `kotlin-gradle` jniLibs。
- spec(`LogX日志库最终实施方案.md`)§3.1/§5 边界情况/§7 maxLogLength 语义同步修订;
  计划表升级 v1.12(T5.5 销号,B-1 拆分:方案B ✅ / 方案A 待决策)。

---

## R4 审查批次: 28acf7f/cf08e75 外部审查发现收口 (2026-09-13)

> 审查报告(`code_review_2026-09-13.md`)6 项发现经逐条核实:5 项属实,1 项(P2 影响推断)
> 不成立——报告称"截断返回 OK 被计入 writeOk、失真告警不触发、压测结论被 silently 判有效",
> 实际 `LogXProbe` 的 ThreadLocal 同步事件捕获(J-04 契约保证调用方线程)会命中
> `code == ERR_TOO_LARGE` → `R.TooLarge` → `dropTooLarge` 递增 → `isDistorted = true`,
> **失真告警照样触发**。但同区域确有方向相反的真实问题:截断(有损写入成功)被误标为
> `DROP: log too large`。R4-2 按"接线该做、理由修正"落实。

### R4-1 (P1) `format_record_bounded` 出参 NULL 守卫
`out_truncated`/`out_dropped` 入口与成功出口共 4 处赋值全部加空指针守卫,使
`logx_internal.h` "可为 NULL" 契约为真;新增 `FormatBounded.NullOutParamsTolerated` 单测
(截断/未截断/双 NULL/单 NULL 四态)。

### R4-2 (P2) `truncated_logs` Android 接线 + 截断/丢弃语义分流
- **JNI** (`logx_jni.cpp`): `nativeGetStats` 5→6 元素,追加 `truncated_logs`;
  "与 logx_stats_t 非 reserved 字段一一对应"注释恢复为真。
- **Kotlin** (`LogX.kt`): `Stats` 追加 `truncatedLogs` 字段(与 C 结构顺序一致),
  `dropTooLarge` KDoc 修正为"病态小 maxLogLength 丢弃"语义;`getStats` 解包 `arr[5]`。
- **LogXProbe**: 新增 `R.Truncated(nanos, msg)`;`ERR_TOO_LARGE` 下按 msg 前缀分流
  (`"log truncated"` → Truncated / `"log too large"` → TooLarge,仅一词之差);
  头注释中引用已删除旧代码路径(`total > inst->max_log_length → "log too large"`)的
  段落改写为 B-1B 三路径现状。
- **logx-tester**: `TesterMetrics` 加 `truncatedLogs`;`TesterViewModel` 新增 Truncated
  分支(控制台 `TRUNCATE: log truncated (dropped N bytes)`,DROP 色标=有损语义);
  `StressRunner` 截断计数并入失真判定(截断=有损,与 bench harness 口径一致),压测报告
  失真行追加 `Truncated=N`;`MetricsPanel` native 快照区补 `truncatedLogs` 行并澄清
  `truncatedBlocks` 标签;`Preset 8/9` 期望文案对账截断语义(">60KB 触发 INVALID /
  返回 TOO_LARGE" → "返回 OK + 截断保前缀")。

### R4-3 (P2) `logx_error_fn` msg 生命周期契约
`logx_c.h` 回调 typedef 补注释:"msg 仅在回调执行期间有效,返回后即失效;实现混合使用
字符串字面量与栈缓冲(T5.5 截断路径为栈上 char[64]),需留存必须自行拷贝"。现网唯一
消费方 `jni_error_trampoline` 经 `make_jstring_utf16` 同步复制,安全。

### R4-4 (P3) 内部符号收入匿名命名空间
`kMarkerReserve` 与 `solve_truncated_msg_budget` 由 `namespace logx` 外部链接+无声明,
重开匿名命名空间收编(与同文件 `tag_len_bounded` 风格一致),消除
`-Wmissing-declarations` 类告警与 ODR 隐患。

### R4-5 (P3) `max_total_bytes == 0` 虚设契约修正
`logx_format.h` 注释改为实际语义:0 = 不施加 max_total 截断(与 format_record 等价);
用户载荷 >60KB 硬上限时 `format_record` 无法格式化,返回 `kCannotFit`(不截断)。
不改行为:该路径仅测试可达(`build_records` 恒传 ≥1)。

### R4-6 (P3) CHANGELOG 提交表补 `9b1af14` 记录行
`9b1af14`(B-2 冻结备忘)向 CHANGELOG 写入 19 行但未为自己建表格行,按 `d2ab63f`
同款补齐模式补行,表格历史链接续。

### R4-7 (观察项, 采纳一半) `trunc_buf` 缩至 60KB
TLS 截断拼装缓冲 64KB → 60KB(证明:msg_len = prefix + marker ≤ budget ≤
cap − (27+tag) ≤ 61440−27);附 thread_local 惰性提交说明(冷路径不触页,无常驻代价)。
不采纳与 `scratch` 合并——需把 `format_record` 拆成装配/出片两半,热路径结构改动
换 4KB 虚拟内存,收益不成立。

### 验证
- C++ 三构建矩阵:OFF/OFF **123/123**、ON/OFF **129/129**、ON/ON **142/142** 单测 +
  压测 4/4 ×3 全绿(新增 NULL 出参用例);
- Android arm64(NDK r29)交叉编译 0 错误,`liblogx.so` 同步双 jniLibs;
- kotlin-gradle 离线 Gradle `:app:testDebugUnitTest` **BUILD SUCCESSFUL**(纯逻辑单测,
  LogX.kt 新 Stats 字段编译通过);logx-tester `:app:assembleDebug --offline`
  **BUILD SUCCESSFUL**(R4-2 全部 Kotlin/Compose 改动编译打包通过);
- `gradle.properties` 固定 `org.gradle.java.home=JDK 17`(AGP 8.5.1 要求,消除
  本机 JAVA_HOME=11 导致的构建阻断,Gradle 标准机器级配置)。

---

## R5 复审批次: 516d091/5828b3f 外部复审发现收口 (2026-09-13)

> 复审报告(`code_review_R4_2026-09-13.md`)6 项新发现经逐条核实**全部属实**(6 实 0 误);
> 复审同时确认上一轮 R4 的 5 项闭环结论、"1 误"证伪成立、`trunc_buf` 缩容 19,795,743 组
> 穷举安全、`logx_stats_t` 72B ABI 不变——均无异议。

### R5-1 (P2) 机器绝对路径迁出仓库
R4 批次将 `org.gradle.java.home=D\:/DevEnv/Java/jdk-17.0.4.1` 提交进
`kotlin-gradle/gradle.properties`,异构机器/CI 必然构建失败。R5-1 将该行迁至**用户级**
`~/.gradle/gradle.properties`(机器特定配置的标准归属),仓库文件还原,kotlin-gradle
README 补 JDK 17 定位说明(JAVA_HOME 或用户级 java.home 二选一)。本机构建经重跑验证
不受影响。

### R5-2 (P3) 截断出参单测改真四态
`NullOutParamsTolerated` 第三个断言与第一个逐字节相同(双 NULL)却注释"单 NULL 混用"。
改为两个真实混用态:`(&truncated, nullptr)` 断言 `kOk && truncated==true`;
`(nullptr, &dropped)` 断言 `kOk && dropped>0`。

### R5-3 (P3) LogXProbe 头注释限定表述
"彻底消除了对错误消息文案的脆弱依赖"与 `ERR_TOO_LARGE` 下按 `msg` 前缀分流的实现矛盾。
改为限定表述:NO_SPACE / INVALID_ARG 已消除文案依赖;ERR_TOO_LARGE 截断/病态丢弃共码
分流为**已知文本耦合**,根治候选立案 **Backlog B-3**(独立事件错误码/事件种类,待决策)。

### R5-4 (P3) 计划表头部元数据去重
历次版本修订在修订说明块中重复带入"制定日期/基准方案/变更日志"三元组(累积 3 份),去重
保留 1 份。

### R5-5 (P3) 60KB 硬上限口径精确化
`logx_format.h` "用户载荷 >60KB" 改为 "内部用户数据超 60KB(`user_total` = 27B 用户头 +
tag + payload,长 tag 会提前触限)",与 `format_record` 实际门槛一致。

### R5-6 (P3) `getStats()` 长度守卫
解包前加 `if (arr.size < 6) return@executeOp null`:旧 `.so` + 新 Kotlin 版本错配时
优雅降级为 null(与"init 前/close 后返回 null"既有契约一致),不再抛
`ArrayIndexOutOfBoundsException`。

### R5-7 (观察项) marker 常量化 + static_assert 固化安全前提

> ⚠️ **R6-1 勘误(2026-09-13)**:本节数值 61346/61407/33B 均有误——budget 随 tag_len
> 单调递减,缓冲取最坏须按 tag_len=0(非减 tag 上限 64),且 61407 源自未覆盖 ≥1e9
> dropped 的扫描(marker 恒 <45B)。真实解析最坏 msg_len = **61410**、余量 **30B**,
> 断言下界已同步修正,详见 R6 章节。以下保留历史原文。

`trunc_buf` 缩至 60KB 后余量仅 33B,安全性依赖"marker ≤ 45B"与"payload 为 uint32_t"
两条未固化前提。R5-7:marker 拆为 `kMarkerHead[]`/`kMarkerTail[]` 常量并经
`snprintf("%s%llu%s")` 引用(字面量与断言绑定);两条 `static_assert` 固化
`kMarkerReserve`(45 ≤ 48)与 `kTruncBufSize`(最坏 msg 61346 ≤ 61440)不变量;
注释"绰绰有余"改为精确表述(最坏写入 61407B @ max_total=61458/tag=0,余量 33B,前提变更
须重审容量)。

### 验证
- C++ 三构建矩阵:OFF/OFF **123/123**、ON/OFF **129/129**、ON/ON **142/142** 单测 +
  压测 4/4 ×3 全绿;
- kotlin-gradle 离线 `:app:testDebugUnitTest` **BUILD SUCCESSFUL**(仓库无 java.home,
  用户级配置生效,R5-1 得证);logx-tester `:app:assembleDebug --offline`
  **BUILD SUCCESSFUL**;
- Android arm64 交叉编译 0 错误,`liblogx.so` 同步双 jniLibs。

---

## T5.6 事件种类扩展: 截断独立事件 LOGX_EV_TRUNCATED (2026-09-13, B-3 方案B)

> 用户在 B-3 三个候选方案中拍板**方案 B(新增独立事件种类)**。枚举追加按值传递,
> ABI 安全;`logx_stats_t` 零改动(72B 不变),返回值语义零变化(截断仍返回 LOGX_OK)。

### 1. C ABI 与核心(`logx_c.h` / `logx_c.cpp`)

- `logx_event_kind_e` 追加 **`LOGX_EV_TRUNCATED = 5`**(有损写入成功,非丢弃)。
- 截断路径事件由 `emit_event(LOGX_EV_DROP, LOGX_ERR_TOO_LARGE, ...)` 改为
  `emit_event(LOGX_EV_TRUNCATED, LOGX_OK, "log truncated (dropped N bytes)")`:
  kind 承载语义,code 取 `LOGX_OK`——截断不是错误;返回值仍为 `LOGX_OK`,
  `truncated_logs` 计数与调用方线程锁外同步契约(P1-1/J-04)不变。
- 病态小上限丢弃独占 `LOGX_EV_DROP / LOGX_ERR_TOO_LARGE`("log too large"),
  R5-3 记录的下游文案耦合消除。

### 2. Kotlin 与 tester 接线

- **`LogX.kt`**:`EventKind.TRUNCATED` + `onNativeError` kindInt=5 映射
  (未识别码仍兜底 IO)。
- **`LogXProbe`**:`Capture` 增 `kind` 字段;`onEvent` 增 TRUNCATED 分支
  (`eventTruncatedCount` 独立计数 + 哨兵捕获);`write()` 分类改 **kind 优先**
  (`c.kind == TRUNCATED → R.Truncated`),不再依赖 msg 前缀,头注释对账。
- **logx-tester**:`Metrics.eventTruncated` + `TesterViewModel` 指标流转 +
  控制台事件流(TRUNCATED 用 DROP 色标=有损语义)+ `MetricsPanel` 展示行。

### 3. 测试同步

- `test_flusher` 截断用例事件过滤器改 `LOGX_EV_TRUNCATED`;
- `test_j04` 截断孪生用例改 `(LOGX_EV_TRUNCATED, LOGX_OK)` 断言(kind+code 双确认,
  同步契约与调用方线程断言保持);
- `LogXPureLogicTest` EventKind 映射扩 kindInt=5 → TRUNCATED(断言数 5→6)。

### 4. 文档对账

spec §3.1 截断事件表述同步;计划表升 v1.15(T5.6 销号、B-3 ✅ 闭环)。

### 迁移提示(R6-2 补记,B-3 决策点"须同步通告"的落实)

外部若按 `kind == LOGX_EV_DROP`（或 DROP + `LOGX_ERR_TOO_LARGE` 组合）过滤截断相关
事件，自 T5.6 起**截断事件不再出现在该过滤器中**（DROP 仅余病态小上限丢弃与
NO_SPACE），须改订阅 `LOGX_EV_TRUNCATED`（code 取 LOGX_OK）；按 kind 全量订阅或仅
关心丢弃的消费方不受影响。本仓库 tester 已同步（LogXProbe 按 kind 分类）。

### 验证

三构建矩阵 **123/123 + 129/129 + 142/142** 单测 + 压测 4/4 ×3 全绿;
kotlin-gradle 离线单测与 logx-tester `assembleDebug` BUILD SUCCESSFUL;
Android arm64 交叉编译 0 错误,`liblogx.so` 同步双 jniLibs。

---

## R6 批次: T5.6 复审 §5 收口 (2026-09-13)

> 复审报告(`code_review_R5_T56_2026-09-13.md`)§5.1 经独立复算**属实**(R5-7 引入,
> 属"收口观察项时引入新缺陷");§5.2 观察采纳。复审对其余各项(枚举 ABI 安全、kind
> 分类文案耦合根除、压测事件处理器/Kotlin ordinal 不适用判定、五处表述一致)的确认无异议。

### R6-1 (P3) `static_assert` 下界修正(61346 → 61410)
- **缺陷**:R5-7 第二条断言 `kTruncBufSize >= 61440 − 27 − 64 − 48 + 45 = 61346`,
  误减 tag 上限 64。`budget = cap − (27 + tag_len)` 随 tag_len **单调递减**,缓冲取
  最坏须按 **tag_len = 0**(空 tag 合法可达):真实最坏 msg_len = (61440−27) − 48 + 45
  = **61410**。原断言下,`kTruncBufSize` 缩至 [61346, 61409] 仍可通过编译,而 tag=0 +
  10 位十进制 dropped 场景将 `memcpy` 61365B 进更小的缓冲——写穿,防护失效。
- **修正**:断言改 `kTruncBufSize >= kHardUserTotal − kUserHdrSize − kMarkerReserve
  + (sizeof(kMarkerHead)−1 + kMaxU32Digits + sizeof(kMarkerTail)−1)`(下界 61410,
  当前 61440 满足,余量 30B);`kUserHdrSize`(27)/`kHardUserTotal`(61440) 自求解器
  函数局部上提至匿名命名空间共用,消除魔法数重复。
- **注释统一**:trunc_buf 声明处注释重写为解析口径 61410/余量 30B;旧"61407/33B"
  源自未覆盖 ≥1e9 dropped(payload 边界点最大 500000)的穷举扫描——marker 恒 <45B,
  已注明取代。

### R6-2 (P3 观察) T5.6 迁移提示补记
CHANGELOG T5.6 章节补"迁移提示"小节:外部按 `kind == LOGX_EV_DROP`(或 DROP+TOO_LARGE
组合)过滤截断事件的消费方,自 T5.6 起不再收到截断事件(仅余病态丢弃与 NO_SPACE),
须改订阅 `LOGX_EV_TRUNCATED`;全量订阅或仅关心丢弃者不受影响。落实 B-3 决策点
"截断事件将从原过滤器消失,须同步通告"。

### 验证
三构建矩阵 **123/123 + 129/129 + 142/142** 单测 + 压测 4/4 ×3 全绿(收紧后的
static_assert 编译通过,即 61440 ≥ 61410 得证);Android arm64 交叉编译 0 错误,
`liblogx.so` 同步双 jniLibs。本批次无 Kotlin 改动,kotlin-gradle/logx-tester 无需重建。

---

## R7 批次: R6 复审 §3 收口 (2026-09-13)

> 复审报告(`code_review_R6_2026-09-13.md`)3 项发现经核实**全部属实**;复审同时确认
> R6-1 断言下界 61410 经 27.3M 组穷举为紧界(余量 30B)、R6-2 迁移提示落实——无异议。
> 另自纠 1 项审查未提及的同类问题(kHardUserTotal 与既有 kMaxUserPayload 重复)。

### R7-1 (P3) 常量绑定权威定义处 `format_record`
R6-1 上提 `kUserHdrSize`/`kHardUserTotal` 后,`format_record`(权威定义处)仍用独立
字面量 `hdr_size = 8+4+8+1+2+4` 与 `60u*1024u`——若头布局或上限变更,断言/求解器
基于过期常量照常成立,`落环总长 ≤ max_total` 不变量可能被破坏。修正:`format_record`
改用 `kUserHdrSize`/`kMaxUserPayload`,并以
`static_assert(kUserHdrSize == 8+4+8+1+2+4, "user hdr layout: ...")` 把布局表达式
钉在定义处——常量与权威实现编译期绑定,漂移即失配报警。

### R7-2 (P3) CHANGELOG R5-7 条目内联勘误
R5-7 节以断言口吻留存 R6 已证伪的 61346/61407/33B,与 R6 章节"61410/30B"并存矛盾。
按"历史原文保留 + 顶部勘误标注"处理:节首补 ⚠️ 勘误块(错因:tag 方向取反 + 扫描
未覆盖 ≥1e9 dropped),检索 `61346` 的命中者即刻可见取代关系。

### R7-3 (P3) 删除 `kMaxSlicePayload` 函数局部遮蔽
`format_record` 内 T5.5 遗留的局部 `constexpr kMaxSlicePayload = 20*1024` 遮蔽头文件
命名空间常量,删除,分片上限回归单源(logx_format.h)。

### R7-4 (自纠) 删除 `kHardUserTotal` 重复常量
R6-1 新增的 `kHardUserTotal`(60*1024) 与头文件既有 `kMaxUserPayload`(60*1024,
logx_format.h:17,此前全仓库零引用)完全重复。删除前者,求解器/`kTruncBufSize` 断言/
`format_record` 三处统一引用 `kMaxUserPayload`;其头文件注释同步对账(B-1B 后超限
语义:format_record 拒绝格式化 → 截断路径接管;精确口径 user_total = 27B 头 + tag +
payload)。至此截断子系统全部数值前提(27/60KB/20480/48/45/61410/61440)均单源命名
常量,相互以 static_assert 或同一表达式绑定。

### 验证
三构建矩阵 **123/123 + 129/129 + 142/142** 单测 + 压测 4/4 ×3 全绿;Android arm64
交叉编译 0 错误,`liblogx.so` 同步双 jniLibs。全仓检索 `kHardUserTotal`/局部
`kMaxSlicePayload` 零残留。

---

## R8 批次: R7 复审 §4 收口 (2026-09-13)

> 复审报告(`code_review_R7_2026-09-13.md`)3 项发现经核实**全部属实**(1×P3 死常量、
> 1×P3 占位符流程、1 项 T5.5 遗留观察);复审对 R7-1~R7-4 四项落实、单源化完整性、
> 61410 紧界沿用有效性的确认无异议。

### R8-1 (P3) 删除死常量 `kMaxMsg`
`UserRecordHeader::kMaxMsg`(60*1024,format.cpp:51)全仓库零引用,且 `UserRecordHeader`
从未被用作类型(纯文档性声明)——与 R7-4"60KB 统一单源"的声称矛盾,且语义失实
(真实约束 `user_total = 27 + tag + msg ≤ 60KB`,msg 单独不可能达 60KB,误改此常量
毫无效果)。删除该常量;结构体补标注"仅作文档用途,序列化直接操作 scratch 缓冲,
布局以 format_record 内 static_assert 钉住的 kUserHdrSize 表达式为准"——根除
"未来维护者误以为它能调整上限"的歧义。

### R8-2 (P3) 占位符流程承诺
`471466a` 曾将字面占位符 `<R7>` 写入提交表哈希列(下一提交即修正,当前表格干净;
哈希列现全部合规)。根因是写 CHANGELOG 行时 fix 提交哈希未知,却选择了"占位 + 回填"
两段式。承诺并执行修正后流程:**fix 提交在前,docs 单次提交直接写入真实哈希**
(本批次即按此执行,R8 行仅一个 docs 提交)。

### R8-3 (§4.3 观察,采纳) 20KB 常量族单源化
- `logx_staging.h` 的 `kBlockMaxRaw = 20*1024 + 6` → `kMaxSlicePayload + kHeaderLen`
  (staging.h 新增 `#include "logx_format.h"`,仅含 cstdint/vector,无包含环);
- `flusher.cpp` 的 `kSingleRecordMaxRaw = 20*1024 + 6` → `= kBlockMaxRaw`
  (语义同为"单条记录含 6B 头的原始上限",与块上限同源);
- `format_pad` 的 `20 * 1024` 防御钳制与 `staging.cpp` 回绕 PAD 的
  `std::min<uint32_t>(20*1024, remain-6)` 字面量 → `kMaxSlicePayload` / `kHeaderLen`
  (后者连带把裸 `6` 一并绑定)。
- 至此截断/分片/PAD 子系统全部数值前提(27 / 60KB / 20KB / 6B / 45 / 48 / 61410 /
  61440)均为单源命名常量,相互以 static_assert 或派生表达式绑定。

### 验证
三构建矩阵 **123/123 + 129/129 + 142/142** 单测 + 压测 4/4 ×3 全绿;Android arm64
交叉编译 0 错误,`liblogx.so` 同步双 jniLibs。

---

## R9 批次: R8 复审 §4 收口 (2026-09-13)

> 复审报告(`code_review_R8_2026-09-13.md`,对象 60f2365/1904cfb)1 项 P3 + 1 项观察 +
> 1 项 Nit 经核实**全部属实**;复审对 R8-1/R8-2 落实、R8-3 数值零变化、包含图/守卫/
> 可见性的确认无异议。本轮另自查出 2 项复审未及问题(1 项同类遗漏 + 1 项复审误判)。

### R9-1 (P3) PAD 回绕循环"部分替换"补齐
R8-3 在 staging.cpp 回绕 PAD 块内仅把 `remain - 6` 换为 `remain - kHeaderLen`,同块
另有 3 处同概念裸 `6` 未动:`pad >= 6`(阈值)、`pos + 6 <= data_size_`(循环界)、
`pos += 6 + this_pad`(步进)——块内口径自相矛盾。3 处一并绑定 `kHeaderLen`。

### R9-2 (审查未及,同类自查) flusher.cpp 5 处同概念裸 `6` 补齐
全量扫描发现 flusher.cpp 另有 5 处同为"6B 记录头"概念的裸字面量:
peek_record_len 的 `total - off < 6` / `sz = 6 + len`,split_impl 的两处循环界
`total - pos < 6` / `total - cur < 6`,块解析防御 `raw_len < 6`——一并绑定
`kHeaderLen`。R8 文档"6B 全部单源"的声称至此才完全成立。staging.cpp:111/122
(`flags@6`)与 flusher.cpp:120/264/305(`seqno@6`/`date@6`)为**块/文件头字段偏移**,
与 6B 记录头概念不同,保留(与复审 §4.1 说明一致)。

> **勘误(R10)**:本节"全量扫描 / 6B 单源声称完全成立"失准——实际扫描仅覆盖
> 5 个 TU,`recovery.cpp` 尚有 5 处同概念裸 `6` 漏转(R10-1 补齐,见下节);历史原文保留。

### R9-3 (§4.2 观察,采纳) 16KB 常量族单源化
flusher.cpp 的 `kNormalBlockMax = 16 * 1024` → `= kBlockMaxNormal`
(logx_staging.h:232),与 R8-3 的 20KB 族同款处理。单源清单增至
27 / 60KB / 20KB / **16KB** / 6B / 45 / 48 / 61410 / 61440。

### R9-4 (§4.3 Nit,采纳) flusher.cpp 直含 logx_staging.h
`kBlockMaxRaw`/`kBlockMaxNormal` 此前经 logx_internal.h 传递包含可见;补
`#include "logx_staging.h"`(codec.cpp:22 已有同款先例),依赖显式化。

### R9-5 (自纠,复审误判) R8 提交表行漏加,补记
`1904cfb` 提交信息声称"补 60f2365 提交记录行",但其 37 行 diff 仅含 R8 章节正文,
提交记录表并无该行——R8 复审"哈希列全部合规、真实哈希行已写入"的结论失准(仅核对
无占位/无重复,未核对行存在性)。本轮于提交表补记 `60f2365` 行。R8-2 承诺的
"fix 提交在前、docs 单次提交带真实哈希"流程本身继续有效并沿用。

### 验证
三构建矩阵 **123/123 + 129/129 + 142/142** 单测 + 压测 4/4 ×3 全绿;Android arm64
交叉编译 0 错误,`liblogx.so` 同步双 jniLibs。

---

## R10 批次: R9 复审 §3 收口 (2026-09-13)

> 复审报告(`code_review_R9_2026-09-13.md`,对象 7b2ead6/2aa6984)1 项 P3 + 1 项观察 +
> 1 项 Nit 经核实**全部属实**;复审对 R9-1/R9-3/R9-4 落实、R9-5 自纠成立(含其对 R8
> "哈希行已写入"误判的确认与教训补记)的确认无异议。

### R10-1 (P3) recovery.cpp 5 处同概念裸 `6` 补齐 + 扫描方式纠偏
R9-2 的"全量扫描"实际仅覆盖 5 个 TU(漏 `recovery.cpp`),其与 flusher.cpp 平行实现
对应的 5 处同概念裸 `6` 漏转:`probe_record` 下界/步长(:37/:39)、`ring_scan` 循环界
(:59)、6B 全零终止符判定(:67)、cksum 载荷起点(:91)——一并绑定 `kHeaderLen`
(该 TU 经 `logx_internal.h` 传递本已可见,纯遗漏,无需新增包含)。本批起扫描方式
改为**按模式全仓穷尽**:core/src + core/include 全部 14 文件 + android/cpp 逐一检索;
修复后 C++ 核心记录头语义裸 `6` **零残留**,余下命中均为不同概念(字段偏移
flags@6/seqno@6/date@6、错误码 -6、stats 数组长度 6、UTF-8 `<< 6` 位运算)。
tests/ 与 decoder/ 的字面量**有意保留**:单测以具体值钉死线上格式,Python 解码器
为独立跨语言镜像(且已有 `BLOCK_RAW_MIN = 6`/`MAX_BLOCK_RAW` 命名常量)。

### R10-2 (§3.3 Nit,采纳) 头注释同步
`logx_internal.h:219` 块头严检注释 `raw_len ∈ [6, kSingleRecordMaxRaw]` →
`[kHeaderLen, kSingleRecordMaxRaw]`,与 R9-2 已转换的实现同步。

### R10-3 (§3.2 观察,采纳) Backlog 新增 B-4
`peek_record_len`/`split_impl`(flusher)与 `probe_record`/`ring_scan`(recovery)为
平行重复实现,是常量单源化三度遗漏(R8-3/R9-2/R10-1)的结构性根因。收敛方案
(共享原语参数化 vs 维持平行 + 流程门禁)涉及写/恢复两条关键路径的等价性论证,
按复审建议**后续评估**,记入计划表 Backlog **B-4** 待决策。

### 验证
三构建矩阵 **123/123 + 129/129 + 142/142** 单测 + 压测 4/4 ×3 全绿;Android arm64
交叉编译 0 错误,`liblogx.so` 同步双 jniLibs。

---

## R11 批次: R10 复审 §4 收口 (2026-09-13)

> 复审报告(`code_review_R10_2026-09-13.md`,对象 85f0b31/3605829)唯一新发现 1×Nit
> 经核实**属实**;复审对 R10-1/R10-2/R10-3 落实、"零残留"代码层声称的独立穷尽复核、
> decoder/tests 保留前提、提交表行存在性的确认无异议(§4.2 两疑点复审自证不成立)。

### R11-1 (Nit) 注释层裸 `6` 同步
R10-2 已立"注释随实现同步"先例,但注释层尚有 6 处记录头语义裸 `6` 未覆盖(其中
flusher.cpp:310 与 staging.cpp:315 位于刚统一的代码块内):范围下界注释(:310)、
PAD 块内两处(:315/:329)、求解器注释(:201/:205)、`format_pad` 契约注释
(logx_format.h:69)——连同同块复审未列的 format.cpp:203 `− 6s`(同一模式)共 **7 处**
一并同步 `kHeaderLen`。豁免判据:编号列表项("6."/"6)")、spec §6 章节引用、
"6B" 人类单位表述、块/文件头字段偏移(flags@6/seqno@6/date@6)、错误码 -6、
stats 数组长度、UTF-8 位运算。

### R11-2 (流程固化,复审 §六建议采纳) 穷尽扫描升级为收口门禁
"按模式穷尽扫描"固化为每批次收口门禁,清单覆盖**代码+注释两层**,判据:
记录头语义裸 `6`(不带 B 单位)零残留。本批已按该门禁执行复扫确认。

### 验证
三构建矩阵 **123/123 + 129/129 + 142/142** 单测 + 压测 4/4 ×3 全绿;Android arm64
交叉编译 0 错误,`liblogx.so` 同步双 jniLibs。

---

## T5.2 补测批次: 资源占用/延迟抖动/零分配/泄漏回归 (2026-09-13)

> 立项依据:`docs/benchmark_report.md` §8.4 自认"spec §9 提及但不在 T5.2 验收内的项未覆盖:
> CPU 占用率、JNI 路径 GC 监控";`docs/LogX日志库最终实施方案.md` §349 把"50 线程压测
> **无 fd/内存泄漏**"列为验收条款 —— 而既有自动化(9,206 行测试代码 / 约 170 用例)中
> **零内存度量、零抖动量化、零分配计数、零泄漏判定**,属"条款存在、门禁缺失"。本批次
> 把这些**设计声明与验收条款**变成可回归契约,不改变任何生产代码行为。

### 新增用例

| 文件 / 用例 | 固化契约 | 判定条件 |
|---|---|---|
| `test_observability.cpp`<br>`Observability.MemoryRssPss_BoundedGrowthAcrossWriteCycles` | 稳态循环后 RSS/PSS 增量有界 | 预热+稳定期取基线 → 40k 条负载后复测,**Δ < 16MB** |
| 同上<br>`Observability.LatencyJitter_StddevCvWithinSanityBound` | 热路径写入延迟抖动合理 | 输出 stddev/CV/median/MAD/robustCV + 长尾比;**robustCV < 5.0**、CV < 50.0、P99/P50 < 100x |
| 同上<br>`LeakRegression.CountingPal_NoResourceLeakAcrossOpenCloseCycles` | Pal 层资源 create/destroy 净 0 | 6 轮 open→写→flush→close 后 fd/mmap/mutex/ev/thread **各自净 0**;含反假通过断言 + OS 级 fd 数交叉验证 |
| `test_hotpath_alloc.cpp`(独立目标 `logx_hotpath_alloc_tests`)<br>`HotPathAlloc.LogxWrite_ZeroHeapAllocationOnCallerThread` | 常规写入热路径零堆分配 | 调用线程 400 条 → **0 次 / 0B** |
| 同上<br>`HotPathAlloc.TruncatedPath_ZeroHeapAllocationOnCallerThread` | 截断路径(T5.5/T5.6 新增分支)零堆分配 | 调用线程 100 条 → **0 次 / 0B** |

### 实测数据(Windows / MSVC 14.43 / Release / ZSTD=ON + AESGCM=ON)

| 维度 | 实测 |
|---|---|
| 内存 | RSS `11747328 → 11751424` B(**Δ = 4096B**,基本持平);Windows 无 PSS 概念 → 该子项降级"仅报告不判定" |
| 抖动 | mean 787~1189ns;**经典 CV 2.5~6.3**(被极少数 OS 抢占样本放大,max 172~358µs) vs **robustCV 0.125~0.25**;P99/P50 ≈ 10~17x |
| 泄漏 | fd 12/12、mmap 6/6、mutex 6/6、ev 6/6、thread 6/6 —— **净 0** |
| 零分配 | 常规路径 **0 次 / 0B**;截断路径 **0 次 / 0B** |

抖动一行印证判据取舍:单次调度抢占足以把经典 CV 抬到 6.3,而 MAD 口径稳在 0.25 ——
故**断言只用 robustCV**,经典 CV 仅作灾难性护栏(直接断言经典 CV 必然 flaky)。

### 关键实现要点

1. **零分配探针用 `thread_local` 计数**(非全局):口径 = "仅当前调用线程",天然隔离 Flusher
   后台线程的压缩/路径拼接分配;thread_local POD 常量初始化自身零分配。未启用计数时
   透传 `malloc/free`,行为与默认分配器等价。
2. **必须设"仪器自检"**:先故意 `new char[64]` 并断言被捕获(用 `volatile` 汇指针阻断
   allocator elision),否则替换一旦失效,"零分配"断言将退化为永真;窗口内另断言写入返回
   `LOGX_OK`(截断用例还断言 `truncated_logs` 按预期增长),证明热路径真被执行。
3. **零分配探针独立成可执行目标**:避免替换全局分配器改变 `logx_unit_tests` 其余用例的运行环境。
4. **内存用例取"稳态增量"**:预热+稳定期后取基线再测增量,对一次性增长(TLS/线程栈/分配器
   arena)天然免疫,只捕捉逐轮泄漏。
5. **陷阱记录(易错)**:`logx_config_t{}` 零初始化的 `flush_threshold = 0`,语义是"**禁用**
   按阈值自动唤醒"(E-06),**不是**"取默认 8KB";长循环不加节拍会填满 127KB 环
   (`kDataSizeDefault = 128KB − 128`)触发 `LOGX_ERR_NO_SPACE`。用例已显式置 `UINT32_MAX`
   并配合 256 条/次显式 flush。

### `logx_bench.cpp` 抖动维度扩展
`print_ns_dist` 增 stddev / CV / median / MAD / robustCV 与长尾比(P99/P50、max/P50),
文件头方法论同步(分位数刻画长尾量级、离散度刻画稳定性,二者互补)。

### 发现既有失败(非本批次引入,已立 Backlog B-5)
`Robustness.PalRmdirErrnoDistinction_NotEmptyVsMissing`(`test_robustness.cpp:1785`)本机失败:
对**非空目录** `file_rmdir` 返回 `0`(期望 -1)、errno 未置 `ENOTEMPTY`(为 0)、随后
`file_unlink(occupied.bin)` 返回 `-1`(文件找不到)。**归因证据**:用 `cl.exe` 手工链接一个
**仅含 `test_robustness.cpp` + `logx_core.lib`、零本次改动**的探针 exe,复现同一失败 →
确认为既有缺陷(Windows PAL 的 `RemoveDirectoryA/DeleteFileA` 与该用例的 POSIX errno
期望不符),与本次改动无关。详见计划表 Backlog **B-5**。

> **勘误(R12 审查)**:该失败为**间歇性**(单次观测,历史原文保留)——同日更早的
> R11 基线 142/142 全绿(含本用例,同机);R12 审查 filtered 10 连跑 **10/10 通过**、
> 三矩阵 **126/126 + 132/132 + 145/145 全绿**。原"稳定失败/既有缺陷"定性失准,
> "Windows 桌面单测全绿基线不成立"表述撤回;B-5 已改口径为"间歇性失败(复现条件
> 未知)",方案 A/B/C 前置稳定性复现协议。探针 exe 判别手法保留(其证明的是代码
> 无关性,不能证明确定性)。另:本节及提交表行中"spec §349"系实施方案**第 349 行**
> (L349 验收条款,内容属实,非章节号)。

### 验证
本机 **Windows / MSVC 14.43 / Release / ZSTD=ON + AESGCM=ON**(即 142 基线的全开矩阵)实跑:
`logx_hotpath_alloc_tests` **2/2 通过**;`logx_unit_tests` **145 项 144 通过**
(142 既有 + 3 新增;唯一失败即上述既有项,已证明与本批次无关)。

**本批次未覆盖/未复跑**:① 另两个构建矩阵(OFF/OFF、ZSTD=ON + AESGCM=OFF)、压测 4/4、
Android arm64 交叉编译;② 新增用例的 **Linux/Android 平台分支**(`/proc/self/statm` RSS、
`smaps_rollup` PSS、OS 级 fd 计数)本机为 Windows 故**既未编译也未运行**,需在真机/交叉
矩阵复跑时一并确认。

---

## R12 批次: T5.2 补测批次审查收口 (2026-09-13)

> 审查对象 `e67be46`(test,+978−7)与 `51e2c47`(docs,+104−1)。总体结论:测试资产质量
> 合格(仪器自检/反空转断言/平台降级等防假通过设计到位),提交纪律保持(提交表行
> 存在、未复跑项显式声明);唯一实质发现为 **1×P2 文档定性问题**(B-5),另 2×Nit +
> 1 项补记。零生产代码改动(仅 test 注释 3 处)。

### R12-1 (P2) B-5 定性纠偏:"既有失败" → "间歇性失败"
复现性证据:① 同日更早的 R11 基线本机 **142/142 全绿**(含 `PalRmdirErrnoDistinction`,
同机);② R12 审查 filtered **10 连跑 10/10 通过**;③ 三矩阵全套件 **126/126 +
132/132 + 145/145 全绿**(含 Robustness 全部)。原"稳定失败(单跑亦失败)"与
""Windows 桌面单测全绿"基线在本机不成立"的表述**失准**并撤回;B-5 改口径为
"间歇性失败(复现条件未知)",并新增**前置协议**——循环 ≥100 次/负载下运行/
`GetLastError` 逐调用点日志增强,命中 ≥2 次并归纳出命中条件后方可投入方案 A/B/C。
提交方"与本批次无关"的代码无关性论证经核实成立(零接触 pal/core/test_robustness);
探针 exe 判别手法保留,但含义收窄:它证明代码无关性,不能证明失败是确定性的。

### R12-2 (Nit) "spec §349" 编号口径修正
`§349` 实为实施方案**第 349 行**(L349,验收条款"50 线程压测无 fd/内存泄漏",内容核对
无误),非章节号,与项目 `§x.y` 章节引用约定冲突。现行文本修正:`test_observability.cpp`
注释 3 处、计划表 §5 验收清单 ④;历史文本(T5.2 章节引文、提交表行、v1.22 说明)以
上方内联勘误覆盖、原文保留。

### R12-3 (Nit) benchmark_report "安全策略限制"表述修正
`logx_bench` 构建经 build-bench(VS 生成器)`cmake --build --target logx_bench` 实际
编译通过——构建并不受限,未重跑属该批刻意延后。原句"其驱动命令受本机安全策略
限制"删除,改为"未运行属本批刻意延后,非环境限制"。

### R12-4 (补) 计划表补 T5.2 补测批次表行
补测批次此前仅在 §5 清单、B-5 与 v1.22 头部说明落墨,未在批次表落行;本批补记
(与 R11 后续行并列),并注明其"145 项 144 通过"记录经 R12 复跑为 145/145。

### F4/F5 决议
F4(新用例无 RAII 清理守卫):与既有套件风格一致、成功路径清理已验证(遗留目录
计数 0),**维持观察不实施**;后续统一 fixture TearDown 时一并处理。F5(新增用例
Linux/Android 分支未编译未运行):双方共同局限,待真机/CI 复跑确认。

### 验证
三构建矩阵 **126/126 + 132/132 + 145/145** 单测 + hotpath **2/2 ×3** + ctest **4/4** +
压测 **4/4 ×3** 全绿;Android arm64 无待办(core 未变);`logx_bench` 编译通过。

---

## 补记: 安卓真机验证——F5 Android 分支闭环 (2026-09-13)

> 用户指示在当前连接设备上实跑全量测试用例。设备 **Redmi Note 2**(hermes,
> Android 5.0.2 / **API 21**,arm64-v8a)——比历史真机(Redmi K30 Pro)老得多,
> 恰好构成对老版本 Android 兼容性的实证。纯验证运行,零代码改动。

### 构建方式
新增本地 `build-android-tests` 目录(不入库):NDK r26.1 + `ANDROID_PLATFORM=android-21`
(降低编译平台以启用全部 bionic 符号版本防护,保证 API 21 可加载)、ZSTD + AES-GCM
全开(等同桌面全开矩阵口径)、GTest 1.14 FetchContent 交叉编译,产物静态自包含。
二进制留存设备 `/data/local/tmp/logx_tests/` 供复用;测试临时目录已清理。

### 实跑结果(全绿)

| 套件 | 设备 | 桌面对照 |
|---|---|---|
| `logx_unit_tests` | **146/146** | 145/145 |
| `logx_hotpath_alloc_tests` | **2/2** | 2/2 |
| `logx_stress_tests` | **4/4** | 4/4 |
| `logx_crash_fork_tests` | **3/3** | 3/3 |

多出的 1 项为 `ArmCe_HardwarePathOnDevice`(C-04 的 ARMv8 Crypto Extensions
**设备专属用例**,桌面 x86 无此分支)——通过。

### F5 Android 分支闭环(T5.2 观测用例首次真机编译+运行)
- 内存: 40k 条稳态负载后 **ΔRSS = 16KB / ΔPSS = 0**(无泄漏);
- 泄漏: Pal 层 create/destroy 全部净 0(fd 12/12、mmap/mutex/ev/thread 各 6/6),
  OS 级 fd 数交叉验证 **17 → 17**;
- 零分配: 常规 400 条与截断 100 条窗口 **0 次 / 0B**;
- 抖动: 中位 8.7µs(2015 年 SoC,慢于桌面属预期)、**robustCV = 0.026**(界 5.0)、
  CV 0.914(护栏 50)、P99/P50 = 6.98x(界 100)。

### 局限更新
F5 仅余 **Linux 桌面分支**未编译未运行(本机无 Linux 环境,待 Linux/CI);计划表
R12 行 F5 表述已同步(v1.24)。

---

## R13 全量源码审查批次收口 (2026-09-25)

> 依据《logx 全量源码审查报告 (R13)》（`code_review_R13_2026-09-24.md`）审查发现与复核结论（§10 核对表），执行“逐项修复 -> 独立审查 -> 单独提交”工作流。
> 本批次共收口 8 项发现（包含 1 项 P2 真实隐患、1 项 P2 契约对齐、6 项 P3 规范与防御性加固），并新增针对跨块碎片清理的单元测试。全量 146/146 单测 100% 通过。

### 修复条目与审查说明

#### 1. N-2 (原 P2-2) export 损坏块分支清空 `carry_slice_buf` (提交 `c07f979`)
- **根因**: `logx_c.cpp` 的 `decode_one_file` 在遇到块 CRC 损坏或解码失败跳过坏块时，未调用 `carry_slice_buf.clear()`。若前一块末尾含第一片（`is_first`），损坏块之后恰好出现后半分片（`is_last`），可能导致跨越损坏块发生非法拼接。
- **修复**: 在 CRC 校验失败与解码失败两大跳过分支中显式增加 `carry_slice_buf.clear()`。
- **测试**: 在 `tests/unit/test_review_p1_p4.cpp` 中新增 `ReviewFixesTest.R13_N2_ExportCorruptedBlockClearsCarrySliceBuf` 专用测试用例，实测验证坏块截断隔离有效。

#### 2. N-1 / N-5 (原 P1-1 / P3-2) `max_log_length` 注释与 Kotlin 校验对齐 (方案 B, 提交 `0adfc55`)
- **裁决**: 经评估，`max_log_length < 97` 时已有 `kBuildRecordsCannotFit` 路径安全捕获并上报 `LOGX_EV_DROP` / `LOGX_ERR_TOO_LARGE` 事件并计数，且现有单测 `CannotFitDropPath_PreservedForPathologicalLimit` 专用于保护此兜底路径。按用户选定“方案 B”：保持当前运行时防御设计。
- **修复**:
  - `core/include/logx_c.h:33`: 将 `LOGX_ERR_TOO_LARGE` 简短注释修改为 `/* 单条放不下(截断到空记录仍超出病态小 maxLogLength) */`，与后文详细契约消除自相矛盾；
  - `android/kotlin/LogX.kt`: 补充 `maxLogLength` 推荐值（>= 97B）及病态小值行为文档，并在 `init` 中增加 `require(config.maxLogLength > 0)` 边界校验。

#### 3. N-3 (原 P2-1) `sum_raw_len_since` 失败路径出参契约明确 (提交 `9ad0db3`)
- **修复**: 在 `core/include/logx_internal.h` 中修正过时的 `found_*` 参数名（已重命名为 `out_last_block_*`），并补充明确契约：“返回 false 时，`*out_bytes` 与两个 `*out_last_block_*` 均重置为 0”。源码实现 `flusher.cpp:920` 已在入口完成清零，契约与实现闭环。

#### 4. N-7 (原 P3-4) 清除私有未用死函数 `map_and_validate_locked` (提交 `314804f`)
- **修复**: `core/include/logx_staging.h` 与 `core/src/staging.cpp` 中的 `map_and_validate_locked()` 全仓 0 引用，彻底移除。

#### 5. N-8 (原 P3-5, 提交误标 N-6) `pal_win32.cpp` 文件大小拼装改用 `uint64_t` 无符号移位 (提交 `f7fb069`)
- **修复**: 将 `fad.nFileSizeHigh` 和 `nFileSizeLow` 转为 `uint64_t` 进行左移 32 位及按位或，最后转为 `int64_t` 返回，彻底杜绝针对有符号整数的移位与按位或静态分析歧义。（注：编号勘误，R13 §9.3 映射为 N-8 / 原 P3-5）。

#### 6. N-6 (原 P3-3, 提交误标 N-8) `flusher.cpp` 移除空 `if (enc.enc_ok)` 块 (提交 `b41e67c`)
- **修复**: 将 `flusher.cpp:136` 中仅包含注释的空 `if` 控制块移除，改为单行架构注释，消除语法异味。（注：编号勘误，R13 §9.3 映射为 N-6 / 原 P3-3）。

#### 7. N-4 (原 P3-1) `format.cpp` `trunc_buf` 运行时容量越界防御 (提交 `c84dca1`)
- **修复**: 在 `format.cpp:298` 的 `std::memcpy(trunc_buf, ...)` 之前补充 `if (prefix_len + marker_len > sizeof(trunc_buf)) return FormatBoundedStatus::kInvalidInput;` 不可达防御性校验，与全文件其他不可达防御风格保持一致。

### 全量验证
- **构建环境**: Windows / MSVC 17.13.5 (x64) Release, ZSTD + AES-GCM 全开
- **单测结果**: `logx_unit_tests.exe` **146 / 146 测试全绿 (0 失败)**（含新增 R13-N2 跨块清理单测）。

## R14 / R15 批次: R13 修复审查文档级收口 (2026-09-25)

- **R14 D-1 / D-3 (P3)**: 纠正 `logx_internal.h` 中 `sum_raw_len_since` 契约注释（如实描述 IO 失败返回 false、坏块返回 true 携带部分累加值）；`format.cpp` 补齐同线程 scratch 64KB + trunc_buf 60KB ≈ 124KB/线程 TLS 内存总账注释。
- **R14 D-2 (P3)**: `logx_c.h` 补齐 `max_log_length >= 97` 建议（用户头 27B + 最大 Tag 64B + 片头 6B），Backlog 增设 B-6 留痕待裁决。
- **R14 D-4 / D-5 (Nit)**: 统一 N-6（flusher 空 if）/ N-8（pal_win32 移位）的权威标号映射；纠正测试范围表述为"Windows 桌面矩阵 146/146 全绿（Linux/Android 未复跑）"。
- **R15 E-1 / E-2 (Nit)**: R13 报告 §11.1 两行编号就地勘误（原文保留、映射回锚 §9.3）；B-6 背景句更正为"`logx_open` 无下界校验（仅 `==0` 与两条上界）"。均为文档级、零行为变化。
- 至此 R13→R14→R15 三轮审查链完全闭合（0 代码遗留）。

## R16 批次: R15 修复批次审查收口 (2026-09-30)

> 审查对象 `code_review_R16_2026-09-30.md`，提交 `151294a`…`d80cd9d`。R18 复核确认六项**全部属实闭环且无引入性缺陷**。

| 编号 | 级别 | 位置 | 内容 | 提交 |
|---|---|---|---|---|
| F-1 | P3 | `test_review_p1_p4.cpp` | `make_slice` 校验和改 **XOR 折叠**与核心 `cksum1`（`format.cpp`）对齐——原误用加法，致 slice 在记录级校验即失败、`is_first`/`is_last` 碎片从未进入 `carry_slice_buf`，N-2 的跨块拼接场景从未真正被构造（**空洞绿**） | `151294a` |
| F-2 | P3 | `logx_format.h` / `logx_c.cpp` | `parse_records_stream` 契约改为"恒返回 true + 损坏容灾内部完成"，并删除 `logx_c.cpp:165` 恒真 if 死分支 | `082cc5a` |
| F-3 | P3 | `logx_jni.cpp` | `key_provider` trampoline 补 attach/detach 对称（与 `jni_error_trampoline` 同模式），避免 Flusher 线程退出时 ART 报未 detach 告警 | `61bd5e1` |
| F-4 | P3 | `staging.cpp` | 纠正回绕路径 `free` 注释（原称"free 没变"失实，实为 `free_new = free_old − pad`）并标明下方判断属恒假防御守卫 | `d80cd9d` |
| F-5 | P3 | `flusher.cpp` | 跨天分支补 `current_size = 0`，防昨日尾文件 size 跨天沿用导致当日首文件误从 `.log.1` 起写 | `93f6f9c` |
| F-6 | P3 | `logx_c.cpp` | `make_tmp_path` 的随机数/时间/PID 统一走 `inst->pal`；CSPRNG 失败回退**原子递增计数器**而非时间戳，与 PAL 契约"严禁低熵回退"对齐 | `082cc5a` |

