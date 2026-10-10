# LogX 代码审查报告 R38

- **审查日期**: 2026-10-10
- **审查对象**: B-12 闭环链四提交——`2d7a15f`（方案 B：断言失败分支 GetLastError 归因）→ `26d3752`（定性收口：根因锁定时序竞态，产品零缺陷）→ `9ab11b6`（tester 三层 maxLogLength 越界文案勘误）→ `b1559e7`（方案一+方案三落地：36500 天时序解耦 + InstGuard RAII，计划表 v1.55）+ `71380b2`（登记提交记录表行）
- **基线**: R37 真机复跑收口后状态（计划表 v1.54）
- **审查方式**: 逐 hunk 通读 + 逐路径语义推演（竞态交错结局枚举）+ 穷尽扫描三路 + MSVC 增量重建与改动 TU 强制重编 + 四套件实测 + NDK clean 全量重建 + 变异实验（R37 预留核验点）+ Kotlin 双 Gradle 任务验证

---

## 一、总体结论

**产品代码零改动、零缺陷，B-12 闭环成立。** 定性结论（用例创建循环与后台 Flusher 跨天 retention 巡扫的时序竞态）经本轮独立推演复核成立；方案一（36500 天建档 + 显式清理前直改 7）在机制上封闭了该竞态窗口（详见 §四.2/§四.3 的交错结局枚举：所有可能交错均与断言期望兼容）；方案三（InstGuard）正确覆盖「断言失败跳出 → 实例泄漏」的级联链。`9ab11b6` 的三层文案与 Kotlin require 实际边界（`LogX.kt:341` `< 130943`、`:344` `>= 97`）逐字核对成立。

发现 1×P3 + 1×Nit，均测试/文档层、不影响闭环结论：

| 级别 | 数量 | 编号 |
|---|---|---|
| P2 | 0 | — |
| P3 | 1 | F-1（retention_days 非原子写与 Flusher 读的形式数据竞争） |
| Nit | 1 | N-1（计划表 B-12 行涉及文件口径） |

---

## 二、批次内容核验与实测口径

### 2.1 逐提交核验

| 提交 | 内容 | 核验结果 |
|---|---|---|
| `2d7a15f` | 方案 B：`file_size_path` 失败分支采获 GetLastError 注入断言消息 | ✅ 成立。`#if defined(_WIN32)` 包裹正确（POSIX 零改动）；「失败路径不消费 LastError」经 `pal_win32.cpp` `file_size_path` 实读证实（`GetFileAttributesExA` 失败直 `return -1`，无 GetLastError 消费）；且 `[B12] raw_lasterror=2` 已在实际命中轮取证（26d3752 定性记录①），机制经实战验证 |
| `26d3752` | 定性收口（纯文档） | ✅ 三线证据链（GetLastError=2 真删除 / probe2 六万轮 0 命中 / 残留目录仅剩 staging.mmap）自洽，与产品代码行为（flusher.cpp:743 跨天分支 → :635 现读 retention_days）吻合 |
| `9ab11b6` | tester 三层文案勘误 | ✅ 与 Kotlin 实际拦截边界逐字一致：131072 > 130942 被 `LogX.kt:341` require 拦截；96、0 被 `:344` require 拦截；100000 双 require 均通过、穿透至 C 侧被 `INVALID_ARG` 拦截。三层（README 场景表 / Preset 7 / ConfigPanel 滑块提示）口径统一 |
| `b1559e7` | 方案一+方案三落地 | ✅ 语义成立（推演见 §四.2/§四.3/§四.4），测试意图 100% 保留（显式 `retention_cleanup` 在恢复 7 后调用，A 必删 / B/C/D 必留断言链原样执行）。产品代码零改动（diff 仅 tests + docs） |
| `71380b2` | 登记行 | ✅ CHANGELOG §提交记录表补一行，符合「只补一行批次行」纪律 |

### 2.2 实测口径表

| 切面 | 方式 | 实测结果 |
|---|---|---|
| MSVC 构建 | `build_tmp.bat`（增量） | 58/58 通过 |
| MSVC 改动 TU 警告 | touch `test_robustness.cpp` 强制重编，`grep -a "warning C[0-9]\|error C[0-9]\|error LNK\|fatal error"` 精确匹配（陷阱 3 口径） | **0 条** |
| unit | `logx_unit_tests.exe` | **179/179** |
| stress | `logx_stress_tests.exe` | **4/4**（2 suites） |
| hotpath | `logx_hotpath_alloc_tests.exe` | **2/2** |
| crash_fork | `logx_crash_fork_tests.exe` | **3/3**（2.1s） |
| NDK clean | `build-android-tests` `ninja -t clean` + 全量 | **62/62、0 警告**（`grep -a "warning:\|error:"` 精确匹配） |
| Kotlin (tester) | `:app:compileDebugKotlin`（gradle-9.4.1，干净环境） | BUILD SUCCESSFUL（UP-TO-DATE，git 干净 ⇒ 当前源码已编译通过） |
| Kotlin (JVM) | `:app:testDebugUnitTest`（gradle-8.9，干净环境） | BUILD SUCCESSFUL（UP-TO-DATE，同上） |

> **口径注记**: 批次声称「179/179 + 2/2 + 4/4 + 3/3」未标注切面归属。实测映射为 stress=4、hotpath=2、crash_fork=3，与声称多集吻合；但 R37 N-3（「27/27 无法对应任何实测切面」）先例表明无标签数字易在轮次间漂移。建议后续声称按「unit 179 / stress 4 / hotpath 2 / crash_fork 3」切面标注。

---

## 三、发现明细

### F-1 (P3) 主线程对 `Instance::retention_days` 的非原子写与 Flusher 线程读构成形式数据竞争——「全流程零竞态」声称字面不成立

- **位置**: `tests/unit/test_robustness.cpp:2059`（写方）；`core/include/logx_internal.h:34`（`uint32_t retention_days = 7;` 普通非原子字段）；`core/src/flusher.cpp:635`（读方，到达路径：`:743` 跨天分支、`:996` flusher 启动清理）
- **现象**: 本批次在 Flusher 线程存活期间由用例主线程裸写 `static_cast<Instance*>(inst)->retention_days = 7`。**修复前该字段运行期无写者**（仅 `logx_c.cpp:385` open 时初始化，此后只读），本批引入了仓库内首个并发写者。C++ 抽象机层面构成数据竞争（UB）。
- **触发链**: `set_clock(20460903)`（:2030）后 Flusher 下一次 `drain_once` 必命中跨天分支 → `retention_cleanup_impl` 读 `retention_days`；drain 由 75ms 去抖定时器驱动、窗口内必发生，若恰与主线程执行到 :2059 并发，即成竞争读。独跑时创建循环（4×open/write/close，µs 级）几乎必然先于 drain 完成，故实际窗口极窄；负载下（B-12 的教训：2/12 命中即来自负载时序）交错概率上升。
- **后果**: **实测为零**——①x64/ARM64 对齐 32 位存储在硬件层原子，不撕裂；②交错结局枚举两支均与断言期望兼容：读到旧值 36500 → 该轮巡扫不删任何 fixture（无害）；读到新值 7 → 仅提前删除 A，而 A 本就是期望被删项，B/C/D 受 `d < threshold && d < yyyymmdd_now` 双守卫（flusher.cpp:662）保护不被误删。**但计划表 v1.55 修订说明①「全流程零竞态」的声称在字面上不成立**——消除的是「文件删除时序竞态」，形式数据竞争仍在。以本仓库「0 警告声称须区分四象限」的同等口径纪律，竞态类声称也不应超出其实际消除面。
- **修法**: 二选一。**最小收口**：:2059 注释补一句「与 Flusher 线程 flusher.cpp:635 的读为形式数据竞争；目标平台对齐 32 位存储原子、新旧两支结局均与断言期望兼容，属已文档化的良性竞争」，并将计划表 v1.55「全流程零竞态」措辞收敛为「无文件删除时序竞态（形式数据竞争良性，见 R38 F-1）」。**彻底化**：`retention_days` 升级 `std::atomic<uint32_t>`（3 处：logx_internal.h:34、logx_c.cpp:385、flusher.cpp:635；字段尺寸/对齐不变，C ABI 无虞）——注意属头文件改动，须走 R21-2 clean 全量重建门禁，建议列 Backlog 与同类内部字段一并处理。
- **需补测试**: 无（无行为面可断言；原子化后亦无新增可观测行为）。

### N-1 (Nit) 计划表 B-12 收口行「涉及文件」列含 CHANGELOG.md，而该文件实际由独立登记行提交改动

- **位置**: `docs/LogX后续实施计划表.md` 收口总览表 B-12 行（v1.55）
- **现象**: 行内涉及文件为 `tests/unit/test_robustness.cpp`、计划表、`CHANGELOG.md`，但 `b1559e7` 实际仅改前两者；CHANGELOG 的改动在独立的 `71380b2`（登记行提交）。R35~R37 收口链惯例正是「批次提交后另起登记行提交」，B-6 等历史行把 CHANGELOG 列入是因为批次提交本身触碰了它（条目 16），B-12 无此情形。
- **后果**: 无实质影响（登记实质存在且合规）；仅行内文件清单与代码批次提交的 diff 不严格对应，削弱「行内清单 = 该提交 diff」的可复核口径。
- **修法**: 下次计划表修订顺手将该行涉及文件收敛为实际两文件，或加注「CHANGELOG 经 71380b2 登记行改动」。

---

## 四、已排除疑点（与发现项同等记录）

1. **36500 会被 open 校验拒绝或钳制的可能性不存在**。`logx_c.cpp:385` 为 `inst->retention_days = cfg->retention_days ? cfg->retention_days : 7;`——无上限校验/钳制，36500 原样存储；`retention_cleanup_impl` 的日期回退循环 O(36500) 次（flusher.cpp:639-657），微秒级，无性能面。
2. **恢复 7 后背景 drain 提前删 A 不会破坏断言**。期望本就是「A 删、B/C/D 留」：提前删 A 与期望兼容；B（20460830）≥ threshold（20460903−7d=20460827）不受影响；C/D 受 `d < yyyymmdd_now` 严格保护（flusher.cpp:662）。**所有交错结局与断言期望兼容**（详见 F-1 后果②），这是方案一「测试语义完整保留」声称的推演基础。
3. **显式 `retention_cleanup` 与背景跨天清理并发无双重误删风险**。`retention_cleanup_impl`（flusher.cpp:625-666）无共享可变状态（file_list → 排序 → 逐个 unlink），`file_unlink` 返回值不被检查、对已删文件幂等失败；B/C/D 双守卫在任何并发下成立。该并发模式系既有设计（显式调用点 :2062 修复前已存在），非本批引入。
4. **InstGuard 与尾部手动 close 无双重关闭**。手动路径 `logx_close(inst); inst = nullptr;`（:2079-2080）先于守卫析构（函数作用域末），守卫判空跳过；早退路径（fatal ASSERT return）守卫单次 close。`logx_open` 失败时 `inst` 保持 nullptr（:2014 预初始化），ASSERT_EQ 早退时守卫同为 no-op。守卫不清理临时目录（:2082-2085 在早退时被跳过）——与修复前行为一致，且 B-12 取证恰依赖残留目录，属可接受甚至有利的行为。
5. **`file_size_path` 失败路径不消费 LastError 的取证前提成立**。`pal_win32.cpp` `file_size_path`：`GetFileAttributesExA` 失败直 `return -1`，无 GetLastError/中间 WinAPI 调用消费错误码；捕获点 :2050 与断言点 :2051 之间无其他 WinAPI 调用，采获值即原码。
6. **变异实验还原完整**。`git status --porcelain` 验空后重建复绿（stress 4/4），实验无残留（详见 §六.3）。
7. **`test_flusher.cpp` 直构 Instance 处的 `inst.retention_days = 7`（:505-506 等 4 处）不构成同类竞争**。该处 Instance 为栈上直构、无 Flusher 线程，写无并发读者；与本批「Flusher 存活期写」形态不同类。

---

## 五、测试覆盖观察（穷尽扫描）

按「写缺口前逐条 grep 验证」门禁，本轮三路扫描（模式与结果如实记录）：

1. **mock 时钟跳变类**：全仓 `set_clock` 仅 `test_robustness.cpp` 两用例。`ClockJump_DayRolloverAtMidnight`（:175）无预建老日期 fixture（API 写入，文件均为当日/次日日期，落在 7 天保留期内，跨天巡扫不触及断言对象）——不构成 B-12 同类竞态。**无缺口**。
2. **「老日期 fixture + Flusher 存活」类**（首次 drain / 跨天 drain 均可触发巡扫，触发不依赖时钟跳变）：`test_review_p1_p4.cpp:165` 与 `test_r19_regression.cpp` 9 处（:366/:703/:1364/:1870/:1926/:1967/:2098/:2294/:2385）已按 R18-1 判据使用 36500；`test_recovery.cpp` 直调 `run_recovery`（无 `logx_open`、无 Flusher 线程）；`test_flusher.cpp` 的 retention 直写用例直构 Instance（无 Flusher 线程，见排除疑点 7）；其余 `retention_days = 7` 用例（test_codec/test_j04/test_concurrent/logx_decode_demo 等）均为 API 写入当日文件。**无缺口**。
3. **直改 `Instance` 成员类**（`static_cast<Instance*>(x)->field =` 模式）：全仓测试**仅 1 处**（本批 :2059）。无同类并发写者残留。

**覆盖结论**: B-12 同类竞态模式全仓唯一、已修复；无「延续前轮结论」成分，以上三条均本轮实扫。InstGuard 属用例内故障路径脚手架，不承载可被他用例测试的语义；`[B12]` GetLastError 归因（2d7a15f）为断言消息增强、非修复承载断言（断言本体 `sz0 > 0` 系既有），不适用撤销对照。

---

## 六、R37 报告为 R38 预留的三个核验点——全部落实

1. **Kotlin `validateInitConfig` 抽取（R37 收口 N-4）**: `:app:testDebugUnitTest`（gradle-8.9 干净环境）BUILD SUCCESSFUL（UP-TO-DATE）。git 工作区干净 ⇒ 输入哈希与上次成功运行一致，即 `LogXPureLogicTest` 3 用例（96/97/130942/130943）对当前源码为绿。**通过**。
2. **NDK clean 0 警告可复现性（R37 收口 N-2 声称）**: `build-android-tests` `ninja -t clean`（62 files）+ 全量重建 **62/62、0 警告**（Clang 诊断格式精确匹配），独立复现 R37 收口声称，「NDK 警告清零」在全量口径下经第二轮独立验证成立。**通过**。
3. **stress raw_len 交叉断言是否真承载修复（R21-1 撤销对照）**: 变异实验——`test_concurrent.cpp:238` 断言改 `EXPECT_EQ(raw_len + 1, payload_len)` → 重建 → `StressTest.FiftyThreads_HeavyConcurrentWritesWithFlushCompetition` **确定性 FAILED**，真实块偏移（offset 32/16407/19397/…）逐块报 `raw_len mismatch`；还原（`git status --porcelain` 验空）→ 重建 → 复绿 4/4。断言钉住真实数据、非恒真。**通过**。

---

## 七、验证局限（必须说明）

1. **B-12 修复无运行时撤销对照，且此类对照在本缺陷上无判别力**。撤销方案一（open 恢复 `retention_days = 7`）后，独跑必然复绿——「独跑恒绿」本就是 B-12 缺陷定性的一部分（巡扫落在断言后）；确定性复红须复现 R2 负载形态（全量套件陪跑 + 进程 churn，原判定门 2/12）。故本批修复有效性的证据 = 机制推演封闭（§四.2 交错结局枚举 + 36500 期间巡扫不删任何 fixture）+ 全套件绿，**未做 R2 12 轮负载复跑作经验确认**（可选后续项，非本批声称缺口）。
2. **NDK 侧仅编译+链接验证**（本机无设备接入），测试运行以真机复跑为准；本轮 NDK clean 重建同时补证了 B-12 改动 TU（`test_robustness.cpp` 在 NDK 测试构建清单内，`build.ninja` 3 处引用）的 Clang 可编译性——批次提交信息未声称 NDK，该空白由本轮审查补上（B-11「Android 冒烟入纪律」仍待拍板，本批是其适用性的又一实例）。
3. **Kotlin 两项为 UP-TO-DATE 证据**：干净 git 状态下 Gradle 输入哈希一致 ⇒ 当前源码已有成功编译/测试记录，逻辑上等价于对 HEAD 源码的验证；如需强制重编证据可 `--rerun-tasks`（本轮未做）。
4. **MSVC 为增量构建**：本批无头文件改动（R21-2 不触发），纯 .cpp 增量可信（R37 ①门禁）；另做了改动 TU 强制重编抓警告（§2.2），0 条。
5. 真机基线（180）与本批的关联：本批仅测试文件改动且不含设备专属用例，真机复跑预期仍 180；未在本轮验证。

---

## 八、修复顺序建议

1. **F-1 最小收口**（:2059 注释 + v1.55 措辞收敛）——一句话改动，可随下一批次顺手带走；**彻底化（atomic 升级）列 Backlog**，与 R21-2 clean 门禁一并规划。
2. **N-1**——下次计划表修订时顺手更正，不独立开批。
3. **口径纪律**（无编号）：四套件测试声称按切面标注（unit/stress/hotpath/crash_fork 各自数字），防 R37 N-3 型口径漂移再现。

---

## 九、收口记录（2026-10-10/11，用户拍板「先修 F-1/N-1，再 R2 负载复跑」）

### 9.1 F-1 最小收口（已落地）

- `tests/unit/test_robustness.cpp` 直改 `retention_days` 处注释扩写：良性竞争四要素文档化（运行期唯一写者即此处 / 对齐 32 位存储平台原子 / 读旧值不删任何 fixture、读新值仅提前删期望被删的 A——两支结局均与断言期望兼容 / 彻底化 `std::atomic` 列 Backlog）。
- 计划表措辞 in-place 勘误：v1.55 三处「(全流程)零竞态」→「无文件删除时序竞态（R38 F-1 勘误）」，并加 v1.56 头部与修订说明。**未做** atomic 化（Backlog 遗留，涉及头文件须走 R21-2 clean 门禁）。

### 9.2 N-1 收口（已落地）

- 计划表 B-12 收口行涉及文件更正：删 `CHANGELOG.md`，加注「CHANGELOG 经 `71380b2` 登记行另改，R38 N-1 更正」。

### 9.3 R2 负载复跑（B-12 闭环经验确认——三判读面全零）

- **rig**: 独立 scratch `D:\Project\Android_Project\LogX\test_review_tmp_b12\r2c_run1..18`，脚本 1:1 复刻 B-5 R2 rig（`r2_loop.sh`：背景 full-suite/hotpath/stress 死循环陪跑 + 前台 300 轮 `PalRmdirErrnoDistinction` filtered；`r2_batch.sh`：三面判读计数）。**原 `test_review_tmp_b5/r2_run1..12` 命中证据未触碰**。
- **结果（18 负载轮，batch 1..12 + 补 13..18）**: 三面全零——
  - 面 1（B-12 原命中面，背景 full_suite `[  FAILED  ]` 行）: **0**；
  - 面 2（RetentionCleanup 定向）: **18/18 次负载下完整执行全部 PASS、0 命中**（每轮恰 1 次完整执行，无部分截断）——与原 rig 归档面精确同量级（原 17 次通过 + 1 次归档命中 = 18 次完整执行，2/12 轮命中）；
  - 面 3（前台 PalRmdirErrno）: 18×300 = 5400 轮 **0 命中**（B-5 自 3800 轮后连续确认至 9200+ 轮）；
  - 无 segfault、无残留进程、批后 tasklist 清点零残留。
- **判读**: 原判定门（负载 2/12 轮命中）在修复后同形态 18 轮 0 命中；经验结果与机制推演（36500 期间跨天巡扫不删任何 fixture，§四.2）合拢——**B-12 闭环由「机制封闭」升级为「机制封闭 + 同面经验确认」**。统计注记：0/18 对 11% 原命中率的后验意义有限（单独不能证伪），经验确认的效力来自「同 rig 同面」的对照性质 + 机制论证在先，与 R21-1「正向对照」纪律同源。
- **证据**: `test_review_tmp_b12/`（r2c_run1..18 各含 stdout/stderr/session/full_suite 日志）；scratch 资产转正与否随本收口提交拍板（原 B-12 决策点沿用）。

### 9.4 批次验证

- MSVC 重建 0 警告（精确匹配口径）；179/179 + stress 4/4 + hotpath 2/2 + crash_fork 3/3 全绿（修复后实测）。
- 真机无需复跑：本批产品零改动、测试仅注释、docs 勘误，行为不变。
- 提交链经用户拍板按三段式执行（本报告入库 → 收口批次 → 登记行），与 R35~R37 惯例一致。
