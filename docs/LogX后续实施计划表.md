# LogX 日志库后续实施路线与任务计划表

> **文档版本**: v1.3 (2026-09-03 依据系统级深度复核修订,并入配套工具与周边链路缺陷)  
> **制定日期**: 2026-09-02  
> **基准方案**: [LogX 日志库最终实施方案.md](./LogX日志库最终实施方案.md) (v5.2 定稿)  
> **变更日志**: [CHANGELOG.md](./logx/CHANGELOG.md)  
>
> **v1.1 修订说明**:2026-09-03 对代码库做了独立复核(MSVC Release,
> zstd + AES-GCM 全开,单测 39/39 实测通过)。M1~M3 主体落地属实,但 v5 spec
> 中若干优化项**仅停留在设计层面、未落地代码**(flush 水位线、切分序号 2B、
> 双 Header 屏障纪律、Debounce、ARMv8 加速、30s 定时持久化、2 类测试用例),
> 统一归口为新增任务 **T4.0**;M1/M2 验收栏中无实测依据的表述已同步修正。
> 偏差明细见 [CHANGELOG.md](./logx/CHANGELOG.md) 「独立复核记录」。  
>
> **v1.2 修订说明**:同日完成**测试逻辑复核**(39 个用例与实现源码逐行对照):
> 断言数值/偏移/语义全部正确,但发现 **4 个"名不符实"用例**(断言通过却未覆盖
> 声称的分支)与 **8 处已实现功能的覆盖缺口**(最重要的是块级 CRC32 读路径
> 验证无实现消费、无测试)。归口为新增任务 **T4.0B**,明细见
> [CHANGELOG.md](./logx/CHANGELOG.md) 「测试逻辑复核记录」。  
>
> **v1.3 修订说明**:同日完成**系统级深度复核**(周边工具、JNI/Kotlin 衔接、恢复边缘场景):
> 发现 **Python 解码器致命缺陷**(is_whole 恒定返回 None + 误判单块单日志导致实际解码崩溃)、
> **Kotlin Facade 语法截断**(漏 if 导致 init 必崩 + 缺失 Gradle 工程)、
> **JNI 参数硬编码与 ErrorHandler 虚置**、**重启后切分序号强制重置为 0 导致历史切分文件覆写风险**、
> 以及 **PAL 持久化 API 全库调用为 0**。T3.4 状态调整为待修复，T4.0 扩充为 12 项回补任务。
> 明细见 [CHANGELOG.md](./logx/CHANGELOG.md) 「深度系统复核记录」。

---

## 1. 总体推进状态全景

```mermaid
gantt
    title LogX 演进阶段与里程碑进度
    dateFormat  YYYY-MM-DD
    section 核心已落地 (M1~M3)
    M1 · 核心写入链路与基础能力        :done, des1, 2026-08-20, 2026-08-28
    M2 · 崩溃恢复与 Σraw_len 对账      :done, des2, 2026-08-28, 2026-09-01
    方案 v5 升级 & JNI 零堆优化       :done, des3, 2026-09-01, 2026-09-02
    M3 · AES-GCM 加密与 KeyProvider    :done, m3, 2026-09-02, 2026-09-03
    section 后续实施 (M4~M5)
    T4.0 · v5 spec 未落地与工具缺陷回补 :active, t40, 2026-09-03, 2026-09-06
    T4.0B · 测试修正与覆盖缺口补齐      :active, t40b, 2026-09-03, 2026-09-04
    M4 · 生产加固与稳定性压测          :m4, 2026-09-07, 2026-09-12
    M5 · 工程集成与基线交付            :m5, 2026-09-13, 2026-09-17
```

| 里程碑 | 目标与范围 | 状态 | 核心产出 / 验收标准 |
|---|---|---|---|
| **M1 核心写入链路** | 环形 mmap staging、TLS Scratch 格式化、单 Flusher 线程、按天切分、基础 JNI / Kotlin 接入 | **✅ 100% 已完成** | 跨平台 PAL (Win/POSIX) 跑通；JNI 热路径零堆内存分配（代码已落实）；多进程自动目录隔离。⚠️ "单条写入 <2µs" 无实测依据，待 T5.2 基准报告佐证。 |
| **M2 恢复与对账** | 对账四分支、k 块回溯、Σraw_len 精确回推、双 Header 容错与不变量收尾、启动 O(1) 缓存定位 | **✅ 100% 已完成（代码）/ 🟡 验收待补** | 截断续写自动跳 1 空隙；单测全绿。⚠️ "kill -9 注入 1000 次零丢失、零重复" 属验收目标而非已完成事实——仓库尚无崩溃注入 harness，实测归 T4.5。 |
| **M3 压缩与加密** | zstd 单文件版、AES-256-GCM、mbedtls 裁剪、KeyProvider JNI 桥、协程 export、Python 解码工具 | **🟡 90% 完成 / 配套工具待修** | `zstd` 与 `AES-256-GCM` 闭环；12B Nonce 安全构造；JNI KeyProvider 全局引用桥接；39/39 单测通过。⚠️ ARMv8 硬件加速未落地（归 T4.0）；⚠️ Python 解码工具存在重组状态机 Bug 与单块单日志假设缺陷（归 T4.0-8）。 |
| **M4 生产加固** | 降级矩阵（堆缓冲）、保留期清理、50 线程并发压测、断电/撕裂/时钟跳变故障注入 | **🟡 下一步实施** | 降级经 ErrorHandler 暴露；50 线程压测无死锁/泄漏；72h Monkey 稳定。 |
| **M5 交付与基线** | Android 备份安全规则配置、性能与包体积基线、接入与密钥管理文档 | **⚪ 待启动** | `dataExtractionRules` 配置；输出延迟/吞吐/CPU/so 报告；发布 Release AAR。 |

---

## 2. 详细任务分解表 (WBS)

### 阶段一：M3 加密链路闭环 (核心完成, 工具待修)

| 任务编号 | 任务名称 | 责任模块 / 文件 | 优先级 | 状态 | 实施要点与验收结果 |
|---|---|---|---|---|---|
| **T3.1** | mbedtls AES-GCM 源码引入与 CMake 接线 | `third_party/mbedtls`, `core/CMakeLists.txt` | **P0** | **✅ 完成** | 引入裁剪版 mbedtls 核心源文件；配置 `mbedtls_config.h`；CMake 打通 `LOGX_WITH_AESGCM=ON`。 |
| **T3.2** | AES-256-GCM 块加密与解密实现 | `core/src/codec.cpp` | **P0** | **✅ 完成** | 落地真加解密；12B Nonce (`8B salt + 4B seqno_be`)；`[ciphertext] + [16B GCM tag]` 格式；密钥缺失降级明文。 |
| **T3.3** | JNI `keyProvider` 完整回调桥 (Trampoline) | `android/cpp/logx_jni.cpp`, `android/kotlin/LogX.kt` | **P0** | **✅ 完成** | `nativeInit` 持有全局引用；后台线程通过 `AttachCurrentThread` 安全回调 Kotlin `KeyProvider.getKey`；`nativeClose` 自动析构释放。 |
| **T3.4** | 离线 Python 解码工具端到端联调 | `decoder/logx_decode.py` | **P0** | **🟡 待修复** | 支持 `--kid-map <kid>=<hex32B>` 解密。<br>⚠️ 2026-09-03 发现严重缺陷: 1. `reassemble` 将 `is_whole` 状态置 1 导致普通日志恒返回 None; 2. 误判每块仅一条日志; 3. 跨块分片未重组。实测无法解码实际日志。归口 T4.0-8 修复。 |
| **T3.5** | M3 加解密单元测试集补全 | `tests/unit/test_codec.cpp`, `tests/unit/test_flusher.cpp` | **P1** | **✅ 完成** | 增加错密钥拒绝、篡改 Tag 拒绝、zstd+AES-GCM 联合测试、端到端密文落盘与 export 导出验证（测试集扩充至 39/39 100% PASS）。 |

---

### 阶段二：M4 生产加固与稳定性压测

| 任务编号 | 任务名称 | 责任模块 / 文件 | 优先级 | 预计工时 | 详细实施要点与验收标准 |
|---|---|---|---|---|---|
| **T4.0** | **v5 spec 未落地项与系统缺陷回补**(2026-09-03 复核扩充) | `core/src/logx_c.cpp`, `core/src/flusher.cpp`, `core/src/staging.cpp`, `core/include/logx_staging.h`, `decoder/logx_decode.py`, `android/kotlin/LogX.kt`, `android/cpp/logx_jni.cpp`, `tests/unit/` | **P0** | 2.5d | 依 [CHANGELOG.md](./logx/CHANGELOG.md) 偏差表逐项回补 (共 12 项):<br>1. `flush()` 水位线 `target_seqno` 防饥饿(现为自旋等空,spec §7);<br>2. 切分序号 uint8_t → uint16_t(spec §3.1/§4.4);<br>3. 双 Header 提交改为"写 A → seq_cst fence → 写 B"(现为 A→B→release fence,spec §4.2/§10);<br>4. Flusher 50~100ms Debounce 超时聚合(现为无超时阻塞 ev_wait,spec §4.1);<br>5. ARMv8 Crypto Extensions 硬件加速(spec §10);<br>6. 运行期每 30s 定时 msync/fdatasync 持久化(spec §11);<br>7. 补未对齐访问、23:59:59 时钟跳变 gtest 用例(spec §9);<br>8. **修复 Python 解码器 `logx_decode.py`**: 重构重组状态机(修复 is_whole 逻辑)、支持单块内多条连续日志解析、支持跨块分片重组并实测通过;<br>9. **修复 `LogX.kt` 语法截断**: 补齐 `if (nativeHandle == 0L)` 条件，避免无条件 throw；搭建基础 Gradle 编译验证;<br>10. **JNI Config 参数传递与 ErrorHandler 桥接**: 将 retentionDays、maxFileSize 等参数由 Kotlin 传至 C++，打通 C++ `err_fn` 到 Kotlin `ErrorHandler`;<br>11. **修复重启后切分序号重置为 0 风险**: `ensure_append_target` 从现有文件名正确提取 split 序号，避免覆写历史切分文件;<br>12. **持久化与目录项 fsync 业务接线**: 新建日志文件后对目录执行 fsync，退后台与 close 执行数据持久化。<br>**验收**:每项附测试或脚本验证;12 项偏差逐项销号。 |
| **T4.0B** | **测试用例修正与覆盖缺口补齐**(2026-09-03 测试逻辑复核新增) | `tests/unit/test_staging.cpp`, `test_flusher.cpp`, `test_recovery.cpp`, `test_codec.cpp`, `core/src/flusher.cpp`, `core/src/codec.cpp` | **P0** | 1.0d | 依 [CHANGELOG.md](./logx/CHANGELOG.md) 「测试逻辑复核记录」逐项处置(预计新增/修正约 10 个用例):<br>**A. 修正 4 个"名不符实"用例**:<br>1. `DualHeaderCorruptionBHandWins`:先 commit 非零值(如 seqno=7)再只破坏 A,断言 B 胜出(现两 Header 全零相同,断言恒真);<br>2. `TornTailNotClosedAsValidBlock`:构造改为声明 len=4 只给 2B payload,真实验证 `first==0` 撕裂封块分支(现为完整合法记录);<br>3. `OneBlockManySmallRecords`:扩到 ≥300 条记录,断言 16KB 封块分支与每块 raw_len ≤ 16KB(现 5600B 单块,封块分支零覆盖;注释"每块 285 条"算错,应为 ≈292);<br>4. `BlocksWrittenHeaderNotCommitted_RemainderRescanned`:改名或重构为把在飞记录放在 snap 之后的真实位置(现构造 raw_len=28 > pending 14B,物理不可达)。<br>**B. 补齐 8 处覆盖缺口**(按优先级):<br>1. **块级 CRC32 读路径**(P0,最重要):写路径已产 crc32(payload) 但 `scan_one`/`decode_block` 均不校验——先补实现消费,再补坏 CRC 拒绝用例(spec §3.2/宁丢不脏);<br>2. 双 Header 均损坏 → 整区重建 + seqno 接管文件 max(spec §4.3);<br>3. 追加目标缓存 O(1) 命中快路径(现所有 recovery 用例 atc.date=0,只走慢路径);<br>4. 保留期清理 `retention_cleanup_impl`(过期文件删除/staging 豁免);<br>5. 按天轮换与 10MB 切分 `.1/.2`(make_log_path + 轮次边界);<br>6. `write_records` kTooLarge 分支(kFull 已测);<br>7. keyProvider 返回 null 时 export 跳过 + 打点语义(spec §7);<br>8. format 入参防御(level>5 / payload=null / 中片 flags=10 断言)。<br>**验收**:gcheck 全绿;`grep` 确认 4 个用例断言已指向真实分支;CRC32 读路径有实现 + 用例。 |
| **T4.1** | mmap 失败堆内存 staging 降级 | `core/src/staging.cpp`, `core/src/flusher.cpp` | **P1** | 1.0d | 1. `mmap` 创建失败时降级分配 128KB 堆内存；<br>2. Flusher 启用定时 5s / 64KB 兜底 write 落盘；<br>3. 显式标记并通知 `ErrorHandler(DEGRADED)`。 |
| **T4.2** | 关键异常监控打点与 ErrorHandler 对齐 | `core/src/logx_c.cpp`, `android/kotlin/LogX.kt` | **P1** | 0.5d | 1. 打通 `DROP` (超长/环满)、`IO` (写盘失败)、`RECOVERY` (截断丢块)、`DEGRADED` (mmap/密钥降级) 等事件；<br>2. 确保在 JNI 层正确回调 Kotlin 注册的 `ErrorHandler`。 |
| **T4.3** | 日志保留期自动清理机制 | `core/src/flusher.cpp` | **P1** | 0.5d | 1. 在 Flusher 启动时及退后台/跨天时扫描日志目录；<br>2. 自动删除 `retentionDays` 之前的超期日志文件，staging 文件豁免。 |
| **T4.4** | 50 线程高并发压测与内存/fd 泄漏检验 | `tests/stress/` | **P0** | 1.5d | 1. 模拟 50 个线程高频随机写入 + 随机 `flush()` 竞争；<br>2. 连续运行 1 小时，监测虚拟内存、RSS、fd 数量与 GC 次数，零泄漏、零死锁。 |
| **T4.5** | 故障注入与鲁棒性验证矩阵 | `tests/stress/`, `tests/unit/` | **P1** | 1.5d | 1. **崩溃注入**: 写入中随机 `kill -9` 循环 1000 次，校验零丢失、零重复；<br>2. **未对齐访问**: 32 位 ARM / ASan 下校验多字节字段读取零 SIGBUS；<br>3. **时钟跳变**: 模拟 23:59:59 回拨至 00:00:01，校验文件无同名冲突与追加正确性。 |

---

### 阶段三：M5 工程集成、基线报告与交付收尾

| 任务编号 | 任务名称 | 责任模块 / 文件 | 优先级 | 预计工时 | 详细实施要点与验收标准 |
|---|---|---|---|---|---|
| **T5.1** | Android 备份安全策略配置工程化 | `android/AndroidManifest-snippet.xml` | **P2** | 0.5d | 1. 提供完整的 `dataExtractionRules.xml` (API 31+) 与 `backup_rules.xml` (API < 31) 示例；<br>2. 确保将 `logs/` 与 `logx_staging/` 彻底排除出云端备份，防止明文泄露。 |
| **T5.2** | 性能基线测试与报告输出 | `docs/benchmark_report.md` | **P2** | 1.0d | 1. 测试并输出单条写入延迟分布 (P50/P90/P99 < 2µs)；<br>2. 对比开启/关闭 zstd 压缩的落盘体积与吞吐量；<br>3. 统计 Release 版 `liblogx.so` 的二进制体积（目标 < 500KB）。 |
| **T5.3** | 接入指南与运维解码文档 | `docs/integration_guide.md`, `README.md` | **P2** | 0.5d | 1. 编写接入流程文档（Application 初始化、KeyStore 密钥包裹示例、协程 export 示例）；<br>2. 编写离线解码与线上排障手册。 |

---

## 3. 任务执行甘特排期与里程碑节点

```
2026-09-03 ~ 2026-09-08 (M3 阶段):
  [T3.1] mbedtls/ARMv8 引入 ───► [T3.2] AES-GCM 实现 ───► [T3.3] JNI KeyProvider ───► [T3.4/T3.5] 测试与解码
  【里程碑 1 交付】: 支持全密文落盘、历史 KID 轮换、跨平台解密一致。

2026-09-03 ~ 2026-09-06 (M4 阶段前置):
  [T4.0] v5 spec 未落地项与系统缺陷回补 (12项: 水位线/切分2B/屏障/Debounce/ARMv8/30s持久化/测试用例/Python解码器重构/LogX.kt语法修复/JNI配置与错误桥接/切分序号覆写风险/目录fsync)
  [T4.0B] 测试用例修正 (4 个名不符实用例) + 覆盖缺口补齐 (8 处,含 CRC32 读路径)
  【交付】: 复核偏差表 12 项逐项销号,spec 与代码重新对齐;解码工具与 Kotlin Facade 实测可用;测试断言指向真实分支。

2026-09-07 ~ 2026-09-12 (M4 阶段):
  [T4.1] 堆内存降级 ───► [T4.2] 错误监控 ───► [T4.3] 保留期清理 ───► [T4.4/T4.5] 并发与故障注入压测
  【里程碑 2 交付】: 具备生产级容灾能力,通过 1000 次 kill -9 崩溃注入(T4.5 实测为准)。

2026-09-16 ~ 2026-09-19 (M5 阶段):
  [T5.1] 备份安全配置 ───► [T5.2] 性能基线测试 ───► [T5.3] 交付文档与 Release 打包
  【最终交付】: LogX v1.0 正式发布。
```

---

## 4. 关键设计不变量与技术纪律对照表

在执行上述任务时，必须严格遵守方案中锁定的技术纪律：

| 关注维度 | 核心规则与技术约束 | 对应代码责任点 |
|---|---|---|
| **加密安全** | 1. 同一个 `(Key, Salt)` 下 Nonce 永不复用、永不回退；<br>2. 发生实际截断时，续写首块必须执行 `seqno = max + 2`（跳 1 空隙）。 | `core/src/codec.cpp`, `core/src/recovery.cpp` |
| **内存与并发** | 1. 双 Header 写入时写完 Header A 必须执行 `std::atomic_thread_fence(seq_cst)` 再写 B；<br>2. staging 坐标系统一为环内相对偏移 `[0, 130944)`；<br>3. JNI 路径严禁分配堆内存，采用 `GetStringUTFRegion` 栈缓冲。 | `core/src/staging.cpp`, `android/cpp/logx_jni.cpp` |
| **崩溃恢复** | 1. 恢复扫描出口处必须执行“不变量收尾”，清零对账跨过区间与撕裂尾巴，重设 `write_pos`；<br>2. 严禁解析校验和失败的脏数据（宁丢不脏原则）。 | `core/src/recovery.cpp` |
| **平台安全** | 1. Staging 文件必须置于 `context.filesDir`，严禁放 `cacheDir` 或外部存储；<br>2. Android 云备份必须显式排除 staging 与 logs 目录。 | `android/kotlin/LogX.kt`, `AndroidManifest.xml` |

---

## 5. 验收标准与检查清单 (Checklist)

> 状态标注(2026-09-03 复核):✅ = 已有实测依据;⬜ = 未完成/无实证/实测未通过。

- [x] ✅ `LOGX_WITH_ZSTD=ON` + `LOGX_WITH_AESGCM=ON` 下，C++ 单元测试全绿 39/39（含明/密文共存、错误密钥拒绝、篡改 Tag 拒绝；2026-09-03 MSVC Release 实测复现）。⚠️ 测试逻辑复核后,T4.0B 将修正 4 个名不符实用例并补 8 处覆盖缺口。
- [ ] ⬜ Python 解码工具 `logx_decode.py` 成功解析由实际引擎产出的加密日志（实测发现整条日志 reassemble 返回 None 及单块假设缺陷，待 T4.0-8 修复后联调）。
- [ ] ⬜ Kotlin Facade `LogX.kt` 建立自动化编译与单测工程（实测发现语法截断错误，待 T4.0-9 修复并接入 Gradle）。
- [ ] ⬜ JNI 边界通过真实 Android 设备验证 keyProvider 无内存泄漏（历史记录为单测通过;泄漏专项监测归 T4.4）。
- [ ] ⬜ 50 线程高并发读写 + 随机 `kill -9` 注入 1000 次,校验无死锁、零重复、零脏数据（T4.4/T4.5;此前无 harness,未实测）。
- [ ] ⬜ 在 32 位设备与开启 ASan 模式下运行单元测试,零 SIGBUS（用例本身也待 T4.0-7 补齐）。
- [ ] ⬜ 导出性能基线报告:单条日志写入耗时 P99 < 2µs(Native 热路径,T5.2;此前无任何实测数据)。

