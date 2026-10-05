# logx 代码审查报告 (R22) —— R21 补强批次复核

> **复核对象**: 工作树未提交改动（7 个文件：`core/src/flusher.cpp`、`core/include/logx_c.h`、`core/include/logx_internal.h`、`tests/unit/test_r19_regression.cpp`、`CHANGELOG.md`、`docs/LogX后续实施计划表.md`、`docs/LogX日志库最终实施方案.md`），即 R21 报告（`code_review_R21_2026-10-05.md`）的补强批次，基线 `69f274d`。复核开始时工作树即含上述未提交改动。
> **复核日期**: 2026-10-05
> **复核范围**: ① 全量 diff 逐行独立审读（flusher 双处产品修复 / 两处头文件 / 测试 599 行改写 / 三份文档）；② 修复点上下游契约核对（`make_flag`/`flag_encrypted`/`encode_block` 的 flag 位语义、`open_or_create_append_target` ↔ `FlusherCtx` 能力传递、`decode_one_file`/`scan_one`/Python decoder 的读侧逐块判定、`split_impl`/`probe_wrap_seam`/`copy_pending` 的缝语义）；③ 全量测试复跑；④ **撤销对照实验复做**（R18-2/R21-1 门禁强制步骤，本轮为该实验的第三次独立执行）。

---

## 一、总体结论

**本批 6 项补强全部成立。** 撤销对照实验复做结果与 CHANGELOG 声称**逐字吻合**：一次构建内同时撤销 4 处修复，恰好 `F1c`/`F3`/`F6b`/`F14` 四项复红、其余 9 个 R19Regression 用例（含孪生用例 F14b）保持绿，失败断言细节（`seam_blocks_skipped 增量 0 vs 1`、`raw_len actual: 3 vs 6`、`resume_seqno 6 ≠ 7`）与 CHANGELOG 记录一字不差。**R20「空洞绿」问题在本批已实质修复，R21 建立的 4 项新回归屏障真实有效。**

| 维度 | 结果 |
|---|---|
| 全量测试复跑（实验前基线） | ✅ 单测 **161/161**、热路径 **2/2**、压力 **4/4**、崩溃 fork **3/3**，与 CHANGELOG 记录一致 |
| 产品代码（能力锁定 F-5 / 降级泄漏 / 自愈事件 F-6 / 缝全零校验 F-7） | ✅ 逐路径推演成立，未发现新缺陷（见 §二） |
| 回归屏障（撤销对照复做） | ✅ 恰好 4 项复红，与声称完全一致（见 §三） |
| 实验后还原与基线恢复 | ✅ 源码字节级还原、`git grep "R22 EXPERIMENT"` 归零、重建后 161+2+4+3 全绿 |
| 文档收口 | ✅ 计划表 v1.29 补记 R19/R20/R21 三行 + B-7/B-8 + R21-1/R21-2 纪律；spec §3 补缝处置与能力锁定语义；CHANGELOG 含 R20 勘误框 |
| 构建环境 | ⚠️ 本会话 ninja 全形态挂死（含 VS 官方版），R21 的挂死归因被反例证伪（见 §四·F-1）；以 `ninja -t commands` 抽取命令行的绕过方案完成定向重建 |

### 发现汇总

| 编号 | 级别 | 一句话 |
|---|---|---|
| **F-1** | **P3** | 「ninja 挂死 = Git Bash 路径转换」归因被反例证伪：本会话从纯 PowerShell 直跑、最小单规则工程、零工作量空跑、console pool、`-j 2`、cmd 内部重定向**全形态挂死**，且 VS 自带官方版 ninja 1.12.1 同样挂死——挂死是会话/环境级机制，R19/R20 的原始记录应恢复采信，R21-2 纪律的「附带更正」段需勘误 |
| **F-2** | P3 | ~~`logx_c.h` 新契约注释称 export 期 `key_provider`「每文件一次」，与实现不符……实际可达 **2 次/文件**~~ **R22 收口勘误（同日）**：该推断有误——`decode_one_file:93` 的每文件首次调用同步置位 `key_lookup_attempted`，循环内 :120 补偿分支被 `!key_lookup_attempted` 守卫**恒不可达**（仅当 `key_provider==NULL` 才可能为假，此时两处均不调用）。实际频度为**每文件恰好 1 次**，`logx_c.h` 契约注释准确、无需修改；:120-123 属不可达防御死分支（按 R18「恒冗余守卫记录不改」惯例保留原状）。详见 §五 勘误框 |
| N-1 | Nit | `F1c` 依赖 `copy_pending` 把回绕窗口线性化进 scratch（`raw_offset` 直接索引）——经代码核对成立，非缺陷，留痕备查 |
| N-2 | Nit | 缝非全零的 RECOVERY 分支（「宁丢不脏」丢弃，flusher.cpp:809-813）零测试覆盖：现有用例只能构造全零常态分支（F1c 断言其不出现）。构造非全零需写侧 bug 或 staging 位翻转，难度高、可接受，但属防御性裸分支，建议留待变异测试补 |
| N-3 | Nit | CHANGELOG「未复跑」清单（Android arm64、构建矩阵另两组、Python decoder）如实；本轮同样未复跑（同局限）+ 本会话追加一条：ninja 全量重建因环境挂死不可执行，以定向重建等价替代（见 §四） |

---

## 二、产品代码逐项核验

| 项 | 核验结论 |
|---|---|
| **F-5 能力锁定** | `open_or_create_append_target` 三条路径全覆盖：新建 = 建文件时刻探测；已存在 = 回读头 flag（续写不改口）；撕裂头自愈分支走「新建」语义重写头。出参经 `ensure_append_target:725` 存入 `FlusherCtx::current_enc_capable`，仅随 fd 重开刷新。`drain_once:855-856` 逐块以 `encrypt_enable && current_enc_capable` 组装 flag。与 `make_flag`/`encode_block`（codec.cpp:205-245，严格按 flag 位而非 key 指针定加密）、三个读端（按块头 flag 逐块判定）核对一致，不变量「文件头声明 == 本文件全部块形态」成立。`ensure_append_target` 失败返回 -1 时 drain 走降级分支不触碰 flag，无陈旧能力值被消费的路径 |
| **降级泄漏修复** | 旧缺陷确认：flag 循环外算一次 + 缺钥就地清位 ⇒ 同轮后续块静默明文且不再发 DEGRADED。新实现逐块组装、降级只影响当块；key 按 `(drain, fd)` 缓存，换文件即失效重取——JNI attach/detach 频次不放大，跨文件不泄漏。缓存生命周期 = 单轮 drain 局部量，无跨轮状态 |
| **F-6 自愈事件** | `open_or_create_append_target:523-526` 补 `LOGX_EV_RECOVERY`，文案带站点名（`at append-target open`），与 `scan_one:404-405`（`truncated to empty for rewrite`）文案可区分——F3 改按文案断言后正好依赖这种区分度 |
| **F-7 全零校验** | 就地校验 + 非全零发 RECOVERY 事件 + `seam_blocks_skipped` 内部计数器（atomic，不进 `logx_stats_t`，零 ABI 影响）。跳过分支仍计入 `flushed_bytes_total`（`flush_blocking` 进度代理语义保持）。「宁丢不脏」处置与 R20 F-1 立论一致 |
| **契约注释** | `logx_c.h` 三类调用点描述中①②③均准确（open 期 fail-fast `logx_c.cpp:375`；建文件探测 `flusher.cpp:543/1035`，Flusher 线程，每文件一次；export 期 `logx_c.cpp:92` 每文件一次——R22 收口勘误后确认③准确，原「有偏差」判定系未追踪 `key_lookup_attempted` 置位所致，见 §五勘误框）。`logx_internal.h` 读侧接受域注释（`kBlockMaxRawPlausible`=16MB、与写侧 `kBlockMaxRaw`=20486 刻意不同源）与 `flusher.cpp:326/359` 实现一致；`bytes_since_hint` 死字段删除正确（全仓确无生产者/消费者，Σraw_len 回推由 `sum_raw_len_since` 独立完成） |
| **测试助手** | `make_entry` cksum 移至 offset 5，与生产布局 `[len:2][magic:2][flags:1][cksum:1][payload]` 对齐（format 头 + XOR 折叠核对无误）；`scan_disk_blocks` 刻意不复用 `block_header_plausible` 避免循环论证，链尾残字节判定（`off == sz`）正确；`make_block_header` 的 34B 布局与写侧 `append_block_raw` 逐字段一致 |

## 三、撤销对照实验复做（R18-2/R21-1 门禁）

**方法**: 备份两文件（SHA-256 双侧比对确认）→ 一次构建内同时撤销 4 处修复（① 删除 `drain_once` 微块跳过整段分支；② flag 改回 `make_flag(algo, encrypt_enable != 0)`；③ `scan_one` 撕裂头事件改 `if (false)` 永假；④ `recovery.cpp:227` `sr.seqno + 1` → `sr.seqno`）→ 定向重编（`ninja -t commands` 原样抽取 7 条 cl/lib/link 命令落 .bat 执行，exit 0 / MSVC 诊断 0）→ 复跑 `--gtest_filter=R19Regression.*`。

**结果**（13 用例：9 绿 / 4 红，与 CHANGELOG 声称完全一致）：

| 撤销项 | 复红用例 | 失败断言（实测输出） |
|---|---|---|
| ① 缝跳过分支 | `F1c` | `seam_blocks_skipped 增量: 0 vs 1`（:374）；`raw_len actual: 3 vs 6`（:386）——3B 微块确实进了文件 |
| ② 能力锁定 | `F6b` | 明文声明文件内出现密文块（块头 bit0=1） |
| ③ scan_one 事件 | `F3` | `saw_msg_substring("truncated to empty for rewrite")` 为假 |
| ④ F-14 跳隙 | `F14` | `resume_seqno: 6 ≠ 7`（:806）；`F14b` 常规路径孪生保持绿（撤销只影响 rebuilt 分支，路径隔离有效） |

**正向对照**: R21 已实测撤销 R19 F-10 → `F10` 红、撤销 R19 F-4 → `F4` 红（本轮基线复跑中二者均绿，排除「harness 本身失灵」）。`F2`（回调内 close）撤销表现为崩溃而非断言失败，维持 R21 的静态判定口径。

**还原**: 备份字节级还原 → `git grep "R22 EXPERIMENT"` = 0 → `git status` 仅本批 7 个预期文件 → 重建后 161/161 + 2/2 + 4/4 + 3/3 全绿。这是该实验继 R21 复核轮、R21 收口轮之后的**第三次独立执行，结果一致**。

## 四、F-1：ninja 挂死根因调查（本会话实证）

R21 勘误声称「挂死疑为 Git Bash 把 `/c` 转成 `C:\` 所致，R21 未复现」，并将其写入 R21-2 纪律「附带更正」。本会话证据链推翻该归因：

1. 纯 PowerShell 直跑 `build_tmp.bat`（无 Git Bash、无路径转换环节）→ 挂死；cmd 内部重定向 `> log 2>&1` → 仍挂死（排除输出管道死锁）。
2. `ninja -n` 空跑 0.0s 完成、7 个待建目标识别正确（排除建图/依赖解析问题）。
3. 最小单规则探针工程（一条 `cmd /c echo` 规则）真实执行 → 挂死；`-j 2` → 挂死；`pool = console`（stdio 不经管道）→ 挂死；**零工作量**（输出已存在）→ 仍挂死（挂点在 ninja 启动/收尾机制，非子进程 spawn）。
4. pip 装的 `1.13.0.git.kitware.jobserver-pipe-1` 与 VS 自带官方版 `1.12.1` **都**挂死（排除单个发行版缺陷）。

**结论**: 挂死是**会话/环境级机制**（疑似与本运行环境的进程/管道限制相关，ninja jobserver-pipe 变体启动即建命名管道），在受限会话中确定性复现、在 R21 所用会话中未复现——两类记录都是真实的，分歧在环境而非操作方式。R19/R20 的原始挂死记录应恢复采信；R21-2 纪律的「附带更正」段（Git Bash 路径转换论）需勘误；R21-2 的主体规则（头文件/结构体布局改动必须 clean 全量重建）本身依然成立且被 R21 正确执行。
**已实证的绕过方案**: `ninja -t commands <target>` 抽取与 ninja 完全一致的命令行落 .bat 执行（本轮 2 编译 + lib + 4 链接，exit 0 / 诊断 0，测试可执行文件时间戳与链接产物核验无误）——建议作为门禁 fallback 写入计划表，供受限会话使用；该方案不做任何手工参数拼接，无 R20 式命令失真风险。

## 五、F-2：export 期 key_provider 频次 —— R22 收口勘误（原推断证伪）

> **⚠️ R22 收口勘误（2026-10-05，同日；原文保留于下方存档块）**：收口修复时对 F-2 做了逐行复查，**原推断「可达 2 次/文件」不成立**，`logx_c.h` 的「③ 每文件一次」契约注释本就准确，**无需修改**：
> - `decode_one_file` 的控制流是：`if (inst->key_provider)` 成立时 `key32 = key_provider(...)`（:92）**并同步置位 `key_lookup_attempted = true`（:93）**。因此循环内 ：118-123 的补偿分支条件 `!key32 && !key_lookup_attempted && inst->key_provider` 中，`!key_lookup_attempted` 恒为假——**补偿分支不可达**（唯一反例是 `key_provider == NULL`，此时外层 :91 不成立，两处调用都不发生，`attempted` 保持 false 但无人调用）。
> - 实际频度：`key_provider` 非 NULL ⇒ **每文件恰好 1 次**（首块之前）；为 NULL ⇒ 每文件 0 次。与契约注释「每文件一次」一致。
> - 原推断的根因：只看了 :120-123 的调用点形态，未追踪 `key_lookup_attempted` 在 :93 的置位——与 R21-1 纪律「判定不得凭印象/注释声称，须以判别性证据证实」同型失误，报告自身也应受该纪律约束。
> - **收口处置**：不改 `logx_c.h`（注释准确）；:120-123 按 R18「恒冗余守卫记录不改」惯例保留原状（属不可达防御死分支，若未来引入「每块取钥/密钥中途热插拔」语义时可复活，届时须先做撤销实验）；本报告 §一 发现汇总与 §二 对应行就地勘误。
> - **预防启示**：审查报告中「与实现不符」级发现（尤其频次/可达性断言）落笔前应附控制流证据链；本轮 F-2 最初仅凭调用点枚举推断，未做不可达性验证。

<details>
<summary><b>原推断存档（已证伪，原文保留）</b></summary>

新契约注释写「③ export: 按文件头 KID 逐块解密（每文件一次）」。实现上 `decode_one_file` 每文件在 :92 先取一次，但**循环内** :120-123 存在补偿调用点（首次遇到加密块且此前未取到钥时再取一次）。因此「每文件一次」仅在「密钥全程可得」或「全程缺失」时成立；密钥在文件中途出现或消失时实际调用 **2 次**。

- 影响面：极小——频次仍按文件数线性，无正确性问题；仅契约注释失真（实现方若按「每文件恰好一次」做严格断言/计数会误报）。
- ~~修复建议~~（勘误：基于错误前提，放弃）：注释改为「每文件 1~2 次（密钥中途出现/消失时存在循环内补偿调用）」即可。**不要**采用「按文件头加密位预判、单次取钥」方案——历史混合文件（头声明加密、含明文降级块）仍须逐块判定，预判会破坏 R19 F-6 修复② 的导出兼容性。（注：「不要按文件头预判」这一提醒本身仍然有效，独立于频次结论。）

</details>

## 六、验证记录

| # | 项目 | 结果 |
|---|---|---|
| 1 | 实验前基线全量复跑 | 161/161、2/2、4/4、3/3 ✅ |
| 2 | 撤销对照实验（4 处同时撤销） | 恰好 F1c/F3/F6b/F14 红，其余 9 绿 ✅ |
| 3 | 失败断言细节比对 | 与 CHANGELOG 记录逐字一致 ✅ |
| 4 | 还原与残留检查 | SHA-256 字节级还原、residue 0、`git status` 仅 7 预期文件 ✅ |
| 5 | 还原后重建 + 全量复跑 | 161/161、2/2、4/4、3/3 ✅（exit 0 / MSVC 诊断 0） |
| 6 | 未复跑 | Android arm64 交叉编译与真机、构建矩阵另两组（ZSTD/AESGCM 关闭组合）、Python decoder 运行期；ninja 全量 56 步重建因本会话环境挂死以定向重建等价替代（产物核验：本轮撤销仅涉 2 个 .cpp，测试文件未改动，链接产物时间戳与 .ninja_log 一致） |

> **收口状态**: 本报告可作为 R22 批次复核结论。F-1（P3）随本批收口：计划表 R21-2「附带更正」段就地勘误 + CHANGELOG R20 勘误框/R21 章节两处反证更正 + `ninja -t commands` fallback 固化为计划表纪律条目（零代码改动）。F-2（P3）经收口复查**证伪为本报告误报**（`logx_c.h` 注释本就准确，`logx_c.cpp:120-123` 为不可达防御死分支），就地勘误见 §五，`logx_c.h` 与核心代码零改动。
