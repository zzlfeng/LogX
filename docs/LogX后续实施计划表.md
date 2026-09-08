# LogX 日志库后续实施路线与任务计划表

> **文档版本**: v1.9 (2026-09-08 T5.2 性能基线测试与报告输出完成)  
> **制定日期**: 2026-09-02  
> **基准方案**: [LogX 日志库最终实施方案.md](./LogX日志库最终实施方案.md) (v5.2 定稿)  
> **变更日志**: [CHANGELOG.md](./logx/CHANGELOG.md)  
>
> **v1.9 修订说明**: 2026-09-08 **T5.2 性能基线测试与报告输出落地**: 新增独立基准 harness `tests/bench/logx_bench.cpp`(`LOGX_BUILD_BENCH=ON`,非 ctest)与 [docs/benchmark_report.md](./benchmark_report.md)。Windows 桌面 + Redmi K30 Pro 真机双端实测(全部轮次 0 丢弃 0 丢失):写入延迟桌面 P50 0.2µs/P99 3.1µs(纯路径),真机 P50 3.2µs;压缩开关对写路径零影响(实证热路径无 codec 成本);zstd 落盘体积 -38%(1.52x)、真机吞吐反超 +48%;so 体积(arm64 strip)MinSizeRel 全开 548.9KB(裸核心 121KB,超 500KB 目标归因 zstd ~426KB)。2µs/500KB 两目标判定与 v1.0 回归基线声明见报告。  
> **v1.8 修订说明**: 2026-09-08 **T5.1 Android 备份安全策略配置工程化落地**: 交付 `android/res/xml/data_extraction_rules.xml` (API 31+,cloud-backup/device-transfer 双通道)、`android/res/xml/backup_rules.xml` (API 23~30 fullBackupContent) 与重写后的 `android/AndroidManifest-snippet.xml` 完整可集成片段(修正原草稿资源根元素命名错误 `<dataExtractionRules>` → `<data-extraction-rules>`)。`filesDir/logs/` 与 `filesDir/logx_staging/` 在云端备份与设备迁移双通道均被排除,防明文 staging/密文日志进云;附自定义目录/多进程隔离目录 (`logs_<后缀>/`) 边界说明与 `allowBackup="false"` 最严备选方案。三个 XML 经 xml.etree 逐项断言校验通过。  
>
> **v1.1 修订说明**: 2026-09-03 对代码库做了独立复核(MSVC Release, zstd + AES-GCM 全开, 单测 39/39 实测通过)。统一归口为新增任务 **T4.0**。  
> **v1.2 修订说明**: 同日完成**测试逻辑复核**: 发现 5 个失真用例与 8 处覆盖缺口, 归口为新增任务 **T4.0B**。  
> **v1.3 修订说明**: 同日完成**系统级深度复核**: 归口为 T4.0 扩充任务。  
> **v1.4 修订说明**: 2026-09-03 **M4 生产加固阶段核心任务全面落地**: 完成 T4.0B 测试修正与 CRC32 读路径闭环、T4.1 堆内存降级、T4.2 错误打点对齐、T4.3 保留期清理全时机触发与闰年防御、T4.4 50 线程并发极限压测(30w+ logs/s)与零死锁对账、T4.5 硬件未对齐访问/跨日时钟跳变仿真/50 轮崩溃注入循环自愈(零丢失)。单元测试增至 **59/59 100% PASS**，压力测试套件 4/4 全部通过。  
> **v1.7 修订说明**: 2026-09-08 **M5 两个验证闭环落地**: ① `android/kotlin-gradle/` Kotlin 编译工程建成,`LogX.kt` 首次通过真实 Kotlin 编译链并附 8 项纯逻辑单测; ② Python 解码器真机最终联调通过(新增 `tests/unit/logx_decode_demo.cpp` 引擎驱动器,104/104 精确对账,中文/40KB 长记录字节级还原); ③ keyProvider 真机泄漏专项通过(10 轮重复 + logcat 无 JNI 引用告警)。真机三套件: 128/128 单测 + 4/4 压测 + 3/3 强杀恢复全绿。M5 剩余: T5.1 备份配置 / T5.2 性能基线 / T5.3 交付文档。  
> **v1.6 修订说明**: 2026-09-08 **三份代码审查报告全部条目闭环销号**: K 系列 (Kotlin Facade export 状态码透传/结构化并发/生命周期回滚/并发保护)、D 系列 (Python 解码器三层错误策略对齐 C++/同日多分卷合并/死代码/解压上限 + 顺带修复 GCM AAD 缺参)、T 系列 (T-01 复核确认 + T-02~T-11 测试用例修正,引入 `logx_config_t.pal_override` 测试 seam 根除 PAL 注入竞态)。全开矩阵 127/127 单测 + 4/4 压测 + 3/3 强杀恢复全绿;基础矩阵 108/108 + 4/4 全绿。T4.0 十二项 spec 偏差全部闭环。下一阶段: **M5 工程集成与基线交付**。  
> **v1.5 修订说明**: 2026-09-03 **Android 真机 (Redmi K30 Pro / arm64-v8a / Android 12) 端到端全量验证跑通**: 交叉编译产出 `liblogx.so`、`logx_unit_tests`、`logx_stress_tests`；修复 JNI 扩展参数签名与 `logx_internal.h` 依赖，修复堆降级测试中 NTFS 特有非法路径在 POSIX 下失效的问题；真机实测单元测试 **59/59 100% PASS** (123ms)，压力测试 **4/4 100% PASS** (820ms，50 线程 85,796 logs/s 零丢失零损坏，50 轮崩溃注入自愈 100% 挽回)。已实现 Windows + Android 双端 100% 全绿。

---

## 1. 总体推进状态全景

```mermaid
gantt
    title LogX 演进阶段与里程碑进度
    dateFormat  YYYY-MM-DD
    section 核心已落地 (M1~M4)
    M1 · 核心写入链路与基础能力        :done, des1, 2026-08-20, 2026-08-28
    M2 · 崩溃恢复与 Σraw_len 对账      :done, des2, 2026-08-28, 2026-09-01
    M3 · AES-GCM 加密与 KeyProvider    :done, m3, 2026-09-02, 2026-09-03
    T4.0B · 测试修正与覆盖缺口补齐      :done, t40b, 2026-09-03, 2026-09-03
    M4 · 生产加固与稳定性压测          :done, m4, 2026-09-03, 2026-09-03
    section 后续实施 (M5)
    T4.0 · 跨端与周边工具缺陷回补      :done, t40, 2026-09-03, 2026-09-08
    M5 · 工程集成与基线交付            :active, m5, 2026-09-08, 2026-09-19
```

| 里程碑 | 目标与范围 | 状态 | 核心产出 / 验收标准 |
|---|---|---|---|
| **M1 核心写入链路** | 环形 mmap staging、TLS Scratch 格式化、单 Flusher 线程、按天切分、基础 JNI / Kotlin 接入 | **✅ 100% 已完成** | 跨平台 PAL (Win/POSIX) 跑通；JNI 热路径零堆内存分配；多进程自动目录隔离。 |
| **M2 恢复与对账** | 对账四分支、k 块回溯、Σraw_len 精确回推、双 Header 容错与不变量收尾、启动 O(1) 缓存定位 | **✅ 100% 已完成** | 截断续写自动跳 1 空隙；单测全绿；崩溃恢复自愈实测通过。 |
| **M3 压缩与加密** | zstd 单文件版、AES-256-GCM、mbedtls 裁剪、KeyProvider JNI 桥、协程 export、Python 解码工具 | **✅ 核心闭环 / 工具待联调** | `zstd` 与 `AES-256-GCM` 闭环；12B Nonce 安全构造；JNI KeyProvider 全局引用桥接。 |
| **M4 生产加固与压测** | 降级矩阵（堆缓冲）、保留期清理、50 线程高并发压测、断电/撕裂/时钟跳变故障注入与恢复自愈 | **✅ 100% 已完成** | 堆内存降级自愈；保留期全时机自动清理；50 线程高频竞争压测零死锁/零重复(30w+ logs/s)；50 轮崩溃注入 397/397 日志零丢失；未对齐读写零 SIGBUS；单测 59/59 全绿（后续批次扩充至 127,见 v1.6）。 |
| **M5 交付与基线** | Android 备份安全规则配置、最低支持 API 21 基线与兼容性测试、性能与包体积基线、接入与密钥管理文档 | **🟡 进行中** (T5.1 ✅ / T5.2 ✅ 已完成;剩余 T5.3 交付文档) | `dataExtractionRules` 配置 (✅ 2026-09-08,见 T5.1);性能/压缩比/so 体积基线报告 (✅ 2026-09-08,见 T5.2);minSdkVersion 21 基准测试；接入与密钥管理文档；发布 Release AAR。 |

---

## 2. 详细任务分解表 (WBS)

### 阶段一：M3 加密链路闭环 (核心完成, 工具待修)

| 任务编号 | 任务名称 | 责任模块 / 文件 | 优先级 | 状态 | 实施要点与验收结果 |
|---|---|---|---|---|---|
| **T3.1** | mbedtls AES-GCM 源码引入与 CMake 接线 | `third_party/mbedtls`, `core/CMakeLists.txt` | **P0** | **✅ 完成** | 引入裁剪版 mbedtls 核心源文件；配置 `mbedtls_config.h`；CMake 打通 `LOGX_WITH_AESGCM=ON`。 |
| **T3.2** | AES-256-GCM 块加密与解密实现 | `core/src/codec.cpp` | **P0** | **✅ 完成** | 落地真加解密；12B Nonce (`8B salt + 4B seqno_be`)；`[ciphertext] + [16B GCM tag]` 格式；密钥缺失降级明文。 |
| **T3.3** | JNI `keyProvider` 完整回调桥 (Trampoline) | `android/cpp/logx_jni.cpp`, `android/kotlin/LogX.kt` | **P0** | **✅ 完成** | `nativeInit` 持有全局引用；后台线程通过 `AttachCurrentThread` 安全回调 Kotlin `KeyProvider.getKey`；`nativeClose` 自动析构释放。 |
| **T3.4** | 离线 Python 解码工具端到端联调 | `decoder/logx_decode.py` | **P0** | **✅ 100% 闭环 (含真机联调)** | 支持 `--kid-map <kid>=<hex32B>` 解密。<br>2026-09-08 **真机最终联调通过**: 新增 `logx_decode_demo` 引擎驱动器 (Redmi K30 Pro),生产配置 (zstd + AES-256-GCM + KeyProvider/kid 1) 写入 20 条中文消息 + 1 条 40KB 长记录(跨分片) + 120 条 1KB 记录,`logx_export` 导出后 `adb pull`,`logx_decode.py` 离线解码 **104/104 行与 C++ 导出精确对账**,中文 UTF-8 与 40KB 长记录字节级还原。<br>此前合成数据 18 项验证 + D-01~D-05 修复记录不变。 |
| **T3.5** | M3 加解密单元测试集补全 | `tests/unit/test_codec.cpp`, `tests/unit/test_flusher.cpp` | **P1** | **✅ 完成** | 增加错密钥拒绝、篡改 Tag 拒绝、zstd+AES-GCM 联合测试、端到端密文落盘与 export 导出验证（测试集扩充至 39/39 100% PASS）。 |

---

### 阶段二：M4 生产加固与稳定性压测

| 任务编号 | 任务名称 | 责任模块 / 文件 | 优先级 | 预计工时 | 详细实施要点与验收标准 |
|---|---|---|---|---|---|
| **T4.0** | **v5 spec 未落地项与系统缺陷回补**(2026-09-03 复核扩充) | `core/src/logx_c.cpp`, `core/src/flusher.cpp`, `core/src/staging.cpp`, `core/include/logx_staging.h`, `decoder/logx_decode.py`, `android/kotlin/LogX.kt`, `android/cpp/logx_jni.cpp`, `tests/unit/` | **P0** | 2.5d | 依 [CHANGELOG.md](./logx/CHANGELOG.md) 偏差表逐项回补 (共 12 项):<br>1. `flush()` 水位线 `target_seqno` 防饥饿(现为自旋等空,spec §7);<br>2. 切分序号 uint8_t → uint16_t(spec §3.1/§4.4);<br>3. 双 Header 提交改为"写 A → seq_cst fence → 写 B"(现为 A→B→release fence,spec §4.2/§10);<br>4. Flusher 50~100ms Debounce 超时聚合(现为无超时阻塞 ev_wait,spec §4.1);<br>5. ARMv8 Crypto Extensions 硬件加速(spec §10);<br>6. 运行期每 30s 定时 msync/fdatasync 持久化(spec §11);<br>7. 补未对齐访问、23:59:59 时钟跳变 gtest 用例(spec §9);<br>8. **修复 Python 解码器 `logx_decode.py`**: 重构重组状态机(修复 is_whole 逻辑)、支持单块内多条连续日志解析、支持跨块分片重组并实测通过;<br>9. **修复 `LogX.kt` 语法截断**: 补齐 `if (nativeHandle == 0L)` 条件，避免无条件 throw；搭建基础 Gradle 编译验证;<br>10. **JNI Config 参数传递与 ErrorHandler 桥接**: 将 retentionDays、maxFileSize 等参数由 Kotlin 传至 C++，打通 C++ `err_fn` 到 Kotlin `ErrorHandler`;<br>11. **修复重启后切分序号重置为 0 风险**: `ensure_append_target` 从现有文件名正确提取 split 序号，避免覆写历史切分文件;<br>12. **持久化与目录项 fsync 业务接线**: 新建日志文件后对目录执行 fsync，退后台与 close 执行数据持久化。<br>**验收**:每项附测试或脚本验证;12 项偏差逐项销号。 |
| **T4.0B** | **测试用例修正与覆盖缺口补齐**(2026-09-03 测试逻辑复核) | `tests/unit/test_staging.cpp`, `test_flusher.cpp`, `test_recovery.cpp`, `test_codec.cpp`, `core/src/flusher.cpp`, `core/src/codec.cpp` | **P0** | **✅ 100% 已完成** | 修正 5 个失真用例；补齐块级 CRC32 读路径强校验、双 Header 重建、ATC 命中、保留期清理、按天切分、kTooLarge、format 入参防御等 8 处覆盖缺口；单测增至 55+ 且全部通过。 |
| **T4.1** | mmap 失败堆内存 staging 降级 | `core/src/staging.cpp`, `core/src/flusher.cpp` | **P1** | **✅ 100% 已完成** | 1. `mmap` 创建失败时降级分配 128KB 堆内存；<br>2. Flusher 启用定时 5s / 64KB 兜底 write 落盘；<br>3. 显式标记并通知 `ErrorHandler(DEGRADED)`；单测验证闭环。 |
| **T4.2** | 关键异常监控打点与 ErrorHandler 对齐 | `core/src/logx_c.cpp`, `android/kotlin/LogX.kt` | **P1** | **✅ 100% 已完成** | 1. 打通 `DROP` (超长/环满)、`IO` (写盘失败)、`RECOVERY` (截断丢块)、`DEGRADED` (mmap/密钥降级) 等事件；<br>2. 单测 `ErrorMonitoring` 全量覆盖。 |
| **T4.3** | 日志保留期自动清理机制 | `core/src/flusher.cpp` | **P1** | **✅ 100% 已完成** | 1. 触发时机全时机闭环: Flusher 启动时、跨天切换文件时、退后台/flush_blocking 时全链路触发；<br>2. 精确计算平年 28 天/闰年 29 天及大月小月边界防御，无条件豁免 Staging 映射文件；Commit `15fa92c`。 |
| **T4.4** | 50 线程高并发压测与数据一致性对账 | `tests/stress/test_concurrent.cpp` | **P0** | **✅ 100% 已完成** | 1. 50 个 Worker 线程并发起跑 + 1 个 flush 竞争线程高频 15ms 强切竞争；<br>2. 达到 30w+ logs/s 吞吐，多文件切分无死锁；<br>3. 严格验证落盘 CRC32 与单调自增 seqno，导出对账零丢失、零损坏；Commit `2118592`。 |
| **T4.5** | 故障注入与鲁棒性验证矩阵 | `tests/stress/test_crash_recovery.cpp`, `tests/unit/test_robustness.cpp` | **P1** | **✅ 100% 已完成** | 1. **未对齐访问**: 奇数地址读写整型与编解码零 SIGBUS；<br>2. **时钟跳变**: 23:59:59 跨日归档创建新文件 + 回拨接管无覆写；<br>3. **崩溃注入**: Staging 未落盘强杀自愈零丢失，50 轮多阶段随机崩溃循环 397/397 日志 100% 复原；<br>4. 根治环尾不足 6B 留零脏内存与滑动窗口重同步提前截断缺陷；Commit `78a2e3c`。 |

---

### 阶段三：M5 工程集成、基线报告与交付收尾

| 任务编号 | 任务名称 | 责任模块 / 文件 | 优先级 | 预计工时 | 详细实施要点与验收标准 |
|---|---|---|---|---|---|
| **T5.1** | Android 备份安全策略配置工程化 | `android/AndroidManifest-snippet.xml`, `android/res/xml/` | **P2** | **✅ 100% 已完成 (2026-09-08)** | 1. 交付完整 `res/xml/data_extraction_rules.xml` (API 31+,`<cloud-backup>` 与 `<device-transfer>` 双通道各排除 `logs/` 与 `logx_staging/`) 与 `res/xml/backup_rules.xml` (API 23~30 `<full-backup-content>`,同两条 exclude;API 21/22 无 Auto Backup 机制,由 `allowBackup="false"` 兜底);<br>2. `logs/` 与 `logx_staging/` 彻底排除出云端备份与设备迁移,防明文 staging 泄露;export 明文临时文件 (`.tmp_export_*`) 位于 log_dir 内被一并覆盖;<br>3. `AndroidManifest-snippet.xml` 重写为完整可集成片段: 修正原草稿资源根元素命名错误 (`<dataExtractionRules>` → `<data-extraction-rules>`),三步集成流程 + `tools:targetApi="31"` lint 抑制 (minSdk 21 基线) + `allowBackup="false"` 最严备选方案;<br>4. 边界注释内置: 自定义 logDir/stagingDir 须同步改 path;多进程隔离兄弟目录 (`logs_<后缀>/`) 不支持通配符,须逐目录追加 exclude 或改用 allowBackup="false";<br>5. 验证: 三个 XML 经 xml.etree 解析良构校验,根元素名/双通道 exclude 清单/application 四属性逐项断言通过。 |
| **T5.2** | 性能基线测试与报告输出 | `docs/benchmark_report.md`, `tests/bench/logx_bench.cpp` | **P2** | **✅ 100% 已完成 (2026-09-08)** | 1. 新增独立基准 harness `tests/bench/logx_bench.cpp`(`LOGX_BUILD_BENCH=ON` 开关,非 ctest),三阶段:单条延迟分布(生产口径+纯路径口径,含失真护栏)/ zstd 开关吞吐与落盘体积(混合负载,NO_SPACE 有界重试可持续口径)/ flush 阻塞耗时;**Windows 桌面 + Redmi K30 Pro 真机双端实测,全部轮次 0 丢弃 0 丢失**;<br>2. 实测:桌面写入 P50 0.2µs(纯路径)/ P99 3.1µs,真机 P50 3.2µs;压缩开关对写路径零影响(实证热路径无 codec 成本);zstd 落盘体积 -38%(1.52x),真机吞吐反超 +48%,桌面 -27%;flush P99 桌面 17ms / 真机 9.4ms;<br>3. so 体积(arm64 strip 后):MinSizeRel 全开 **548.9KB**(超 500KB 目标 10%,归因 zstd 贡献 ~426KB 占 77%,裸核心仅 121KB);判定与回归基线声明见 [benchmark_report.md](./benchmark_report.md)(2µs 目标建议改口径为确定性热路径段,已达成;500KB 目标建议接受 548.9KB 为 v1.0 基线,zstd 源码裁剪列为可选优化)。 |
| **T5.3** | 接入指南与运维解码文档 | `docs/integration_guide.md`, `README.md` | **P2** | 0.5d | 1. 编写接入流程文档（Application 初始化、KeyStore 密钥包裹示例、协程 export 示例）；<br>2. 编写离线解码与线上排障手册。 |

---

## 3. 任务执行甘特排期与里程碑节点

```
2026-09-03 ~ 2026-09-08 (M3 阶段):
  [T3.1] mbedtls/ARMv8 引入 ───► [T3.2] AES-GCM 实现 ───► [T3.3] JNI KeyProvider ───► [T3.4/T3.5] 测试与解码
  【里程碑 1 交付】: 支持全密文落盘、历史 KID 轮换、跨平台解密一致。

2026-09-03 ~ 2026-09-06 (M4 阶段前置):
  [T4.0] v5 spec 未落地项与系统缺陷回补 (12项: 水位线/切分2B/屏障/Debounce/ARMv8/30s持久化/测试用例/Python解码器重构/LogX.kt语法修复/JNI配置与错误桥接/切分序号覆写风险/目录fsync)
  [T4.0B] 测试用例修正 (5 个名不符实用例) + 覆盖缺口补齐 (8 处,含 CRC32 读路径)
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
| **平台安全** | 1. Staging 文件必须置于 `context.filesDir`，严禁放 `cacheDir` 或外部存储；<br>2. Android 云备份必须显式排除 staging 与 logs 目录；<br>3. 最低支持 Android 5.0 (API Level 21)，全库遵循 API 21 兼容基线编写，高版本特性必须带 `SDK_INT` 降级保护。 | `android/kotlin/LogX.kt`, `AndroidManifest.xml` |

---

## 5. 验收标准与检查清单 (Checklist)

> 状态标注(2026-09-03 复核):✅ = 已有实测依据;⬜ = 未完成/无实证/实测未通过。

- [x] ✅ `LOGX_WITH_ZSTD=ON` + `LOGX_WITH_AESGCM=ON` 下，C++ 单元测试增至 **127/127 100% 全绿**（2026-09-08 现状:经 T4.0B 及 C/F/S/R/E/P/J/K/D/T 多轮缺陷修复批次持续扩充;基础 OFF/OFF 矩阵 108/108 同步全绿;压测 4/4、真实进程强杀恢复 3/3）。
- [x] ✅ Python 解码工具 `logx_decode.py` 用**实际引擎真机产出**的加密日志最终联调通过（2026-09-08,Redmi K30 Pro,zstd+AES-GCM 生产配置,104/104 精确对账;D-01~D-05 修复 + 合成数据 18 项验证为前置）。
- [x] ✅ Kotlin Facade `LogX.kt` 语法缺陷已修复且 K-01~K-04 全部闭环（2026-09-08）。
- [x] ✅ Kotlin Facade `LogX.kt` Gradle 自动化编译与单测工程建立（2026-09-08,`android/kotlin-gradle/`:AGP 8.5.1 + KGP 1.9.23 + Gradle 8.9/JDK 17,单源引用交付源文件,compileDebugKotlin 通过 + 纯逻辑单测 8/8 全绿,离线构建）。
- [x] ✅ JNI 边界通过真实 Android 设备验证 keyProvider 无内存泄漏（2026-09-08,Redmi K30 Pro:加密/KeyProvider 用例连续 10 轮运行 MemAvailable 波动 < 0.1%,logcat 无 ReferenceTable/global reference 告警,KeyProvider 生命周期用例随 128 项单测全绿）。
- [x] ✅ **50 线程高并发读写与落盘对账压测**: 50 Worker + 15ms 强切竞争，30w+ logs/s，seqno 严格单调递增，落盘块 CRC32 强校验通过，端到端导出对账零死锁、零重复、零丢失（T4.4 实测通过）。
- [x] ✅ **崩溃注入与 50 轮随机阶段崩溃循环自愈压测**: Staging 未落盘强杀 100% 挽回，磁盘撕裂尾块安全截断，50 轮多阶段随机崩溃循环 397/397 日志 100% 复原（T4.5 实测通过）。
- [x] ✅ **未对齐访问与时钟跳变仿真**: 奇数内存地址读写整型与编解码零 SIGBUS；23:59:59 跨日平滑归档轮转与时钟回拨接管验证通过（T4.5 实测通过）。
- [x] ✅ **性能基线报告已输出** (2026-09-08,T5.2):单条日志写入实测桌面 P50 0.2µs(纯路径口径,确定性热路径段达成 2µs 量级目标;整调用口径 P99 3.1µs 未达,归因计时器开销与调度尾部,见 [benchmark_report.md](./benchmark_report.md) §4);zstd 体积 -38% / so 体积 548.9KB(strip,MinSizeRel)等全部实测数据与回归基线声明已归档。

---

## 6. 待决策任务池 (Backlog)

> 本章节记录已评估但**尚未拍板**的候选设计变更。条目仅作决策备查,未经确认不排期、不实施。

### B-1 两条丢弃路径改造评估:NO_SPACE / TOO_LARGE 能否不丢日志

- **记录日期**: 2026-09-08
- **状态**: ⏸ 待决策(决策点见文末)
- **背景**: 现行设计遵循 spec §5/§8 "宁丢不脏、绝不阻塞"原则:
  - **NO_SPACE** — staging 空闲不足时整条丢弃:`drop_no_space` 计数 + `LOGX_EV_DROP` 事件 + 返回 `LOGX_ERR_NO_SPACE`(`core/src/logx_c.cpp:435-457`,`Staging::write_records` 返回 `kFull`);
  - **TOO_LARGE** — 单条格式化后超 `maxLogLength` 时整条丢弃:`drop_too_large` 计数 + `LOGX_EV_DROP` 事件 + 返回 `LOGX_ERR_TOO_LARGE`(`core/src/logx_c.cpp:428-432`;init 强制 `max_log_length < 数据区−1`,超长记录物理上放不进环,无"等待腾空间"选项)。

#### 方案评估结论

| 方案 | 适用路径 | 改动量 | 思路 | 波及面 |
|---|---|---|---|---|
| **A. 有界阻塞重试** | NO_SPACE | **小**(~0.5d,`logx_write` 单函数 ~40-60 行) | `logx_write` 进锁前已完成 `build_records` 格式化(栈上 `buf`),重试零重新格式化成本:锁内 `write_records` 失败即解锁,轮询短暂等待(复用 `flush_blocking` 的 `sleep_ms(5)` 风格,或在 `drain_once` 每块提交后补一行 `ev_signal`)后重试;超过总上限(如 5s)或 Flusher 已死才走现行丢弃路径保底 | `logx_c.cpp`、`logx_internal.h` 加等待上限常量/配置;`flusher.cpp` 可选加 1-2 行 signal;补单测(满时阻塞至 flush 腾出、Flusher 死亡仍返回 NO_SPACE);现有 stress 测试自带 NO_SPACE 重试,兼容 |
| **B. 截断保留前缀** | TOO_LARGE | **小**(~20-40 行) | 超长时用缩小后的 payload 经 `format_record` 重新格式化一次,保证 ≤ `max_log_length` 落环;新增截断计数(占用 `logx_stats_t.reserved` 预留位,ABI 兼容),事件改为 "log truncated" | `format.cpp`/`logx_c.cpp`;Kotlin 层 API 与磁盘格式均不动 |
| **C. 溢出文件保全文** | 两者 | **大**(不推荐) | 超限记录落独立溢出文件 | 打破"文件单写者"不变量(spec 不变量 4,仅 Flusher 写日志文件),需新增文件类型并贯通恢复/导出/保留期/切分全链路,引入新对账复杂度 |

- **可行性依据(方案 A 活性保证)**: `drain_once` 的块写失败分支(`flusher.cpp:696-705`、`717-727`)同样清零区间并推进 `f`,即使磁盘持续 IO 失败空间也必然释放,不会死等;Flusher debounce 仅 75ms,需 5s 内灌满 128KB 且磁盘彻底卡死才真正丢,反压传导到业务线程是最坏 5s 的延迟代价。
- **效果预估**: A+B 落地后,真正丢日志仅剩"磁盘彻底坏死且持续 5s+"与"超长且截断不可接受"两类边缘场景。

#### 决策点(拍板前需确认)

1. **延迟契约变更**: 方案 A 与 spec "空闲不足 → 丢弃 + 打点,绝不阻塞"原则冲突——磁盘卡住时反压会阻塞业务调用线程(最坏等满等待上限)。建议加配置开关(如 `staging_wait_ms`:0 = 保持现行丢弃语义,>0 = 有界等待),默认值待定。
2. **TOO_LARGE 语义选择**: 截断保前缀(方案 B)是否可接受;若业务要求超长记录一字节不能少,才考虑方案 C(代价大)。
3. spec 文档(`LogX日志库最终实施方案.md` §3.1/§5/§8)需随决策同步修订对应条款与计数/事件说明。

