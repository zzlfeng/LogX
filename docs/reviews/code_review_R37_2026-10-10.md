# LogX 代码审查报告 R37

- **审查日期**: 2026-10-10
- **审查对象**: `4f4461c`（feat: B-6 方案 A 落地——max_log_length 下界 fail-fast 校验与常量单源化，计划表 v1.52）+ `64d77dc`（docs: 登记提交记录表行）
- **基线**: `2947a31`（R36 审查报告入库后的仓库状态）
- **审查方式**: 逐 hunk 通读 + 求解器/预算语义推演 + 穷尽扫描 + MSVC clean 全量重建 + 四套件实测 + 编译期变异实验 + 撤销对照独立复做 + Android NDK clean-first 交叉编译

---

## 一、总体结论

**产品代码（core/ 与 android/kotlin/LogX.kt）零缺陷。** B-6 方案 A 的核心改动——`logx_open` 下界 fail-fast 门（`logx_c.cpp:342`）、常量单源化（`logx_format.h:24-45`）、编译期守卫、Kotlin require 对齐——经逐路径推演与运行期/编译期实验全部成立，撤销对照独立复做两轮确定性复红，与提交声称一致。

发现集中于**外围层收口与口径**：2×P3（tester 应用未随收口；NDK clean 构建实测 3 条存量警告，历史「NDK 警告清零」全量口径被证伪）+ 3×Nit。

| 级别 | 数量 | 编号 |
|---|---|---|
| P2 | 0 | — |
| P3 | 2 | N-1（tester 分层口径）、N-2（NDK 存量警告） |
| Nit | 3 | N-3（NDK 步数口径）、N-4（Kotlin require 无单测）、N-5（自指 static_assert） |

---

## 二、发现明细

### N-1 (P3) Android tester 应用未随 B-6 收口——面板校验与 README 仍为旧下界口径

- **位置**: `android/logx-tester/app/src/main/java/com/logx/tester/ui/ConfigPanel.kt:123-124`；`android/logx-tester/README.md:59、:83`；`android/logx-tester/app/src/main/java/com/logx/tester/model/Preset.kt:105`
- **现象**: 本批将 C 侧与 Kotlin 层（`LogX.kt:301` `require(config.maxLogLength >= 97)`）下界收口为 97，但 tester 应用三层未同步：
  - `ConfigPanel.kt:123` 预警条件仍为 `config.maxLogLength > 65536 || config.maxLogLength == 0`，`:124` 文案仍写「超出合法区间 (1..65536)」；
  - `README.md:59` 参数表仍写「合法范围 1..65536；>65536 或 ==0 在 C 侧校验失败」；
  - `README.md:83` 场景表第 7 行与 `Preset.kt:105` 预设 7 的引导文案（「分别尝试调整 maxLogLength 为 131072、100000 和 0」）均无 96/97 新边界档。
- **触发链**: 滑块 `valueRange = 0f..131072f`（ConfigPanel.kt:130）→ 用户选择 1..96 → 面板判断「合法」不预警 → `LogX.init()` 走到 `LogX.kt:301` require → 抛 `IllegalArgumentException`（与面板红色警告承诺的行为相矛盾）。
- **后果**: tester 的引导文案对 1..96 区间给出错误的合法性承诺；README 验收场景缺 96/97 边界档，无法用真机按表验收本批新契约。属分层契约一致性遗漏（与 R36 D-1 同类：产品层收口后外围未跟上）。
- **修法**: `ConfigPanel.kt:123` 条件改为 `config.maxLogLength < 97 || config.maxLogLength > 65536`、文案改「合法区间 (97..65536)」；README 参数表/场景表同步；预设 7 建议档位补 96/97（变四档或替换 0 档——0 仍被拦，可保留）。
- **需补测试**: tester 为手工验收应用、无 UI 自动化；README 场景表补记 96/97 档作为真机手工验收项即可（可与真机复跑收口同批）。

### N-2 (P3) NDK clean 全量构建实测 3 条存量 Clang 警告——历史「NDK 警告清零」全量口径证伪

- **位置**: `tests/unit/test_staging.cpp:22`（`unused variable 'kTotal'` [-Wunused-const-variable]）；`tests/stress/test_concurrent.cpp:77`（`unused lambda capture 'kLogsPerWorker'` [-Wunused-lambda-capture]）；`tests/stress/test_concurrent.cpp:229`（`unused variable 'raw_len'` [-Wunused-variable]）
- **现象**: 本审查对 `build-android-tests` 执行 clean-first 全量交叉编译（62/62 边，RC=0），实测恰好 3 条警告（`sort -u` 去重后）。两文件最后改动均为 `0976cbc`（T-01..T-11 批次），**非本批引入**。
- **成因**: 此前各轮 NDK 构建多为增量——警告仅在对应 TU 重编时出现，故历史各轮「NDK 警告清零/消除」声称实际口径是「当轮改动文件无警告」；R36 记忆已将「NDK 警告清零」标注为未独立复现，本轮 clean 口径实测将其证伪为「全仓零警告」。
- **后果**: 无功能影响；但「0 警告」类声称必须区分 MSVC/Clang × 增量/clean 四象限，否则后续轮次会延续错误基线（R33 N-1 已立过一次「口径仅限 MSVC」的先例，本轮需再把 clean/增量维度补齐）。
- **修法**: 删除或 `(void)` 化三处未用变量/捕获（注意 `kTotal` 为 constexpr 常量，确认无文档引用后删）。可并入 B-11（Android 冒烟入纪律）批次一并收口。
- **需补测试**: 无运行时测试——与 B-11 联动建立「NDK clean 构建零警告」门禁即可。

### N-3 (Nit) 提交信息/CHANGELOG 的 NDK 步数口径「27/27 + 10/10」与实测不符

- **位置**: `CHANGELOG.md:604`（「build-android-tests (27/27) 与 build-android (10/10) clean 编译链接 100% 成功」）；提交信息同口径
- **现象**: 本审查实测：`build-android-tests` clean-first 全量为 **62/62**（25 编译 + 8 链接 + 其余边），本批改动后增量重建为 23/23，全量后增量 0——「27/27」无法对应任何实测切面；`build-android` 增量 9/9（clean 约 10，与声称基本吻合）。R36 时代的 `9c7b1b5` 批次口径为「clean 62/62」，与本次实测一致。
- **后果**: 实质声称（NDK Clang 交叉编译链接成功）成立、无功能争议；但步数口径与两轮提交间互相矛盾，削弱 CHANGELOG 的可复核性。
- **修法**: 按真机复跑收口惯例补记 CHANGELOG：统一为「build-android-tests clean 62/62 + build-android 10/10」，并注明 R36 遗留的 62/62 核对项已由本轮实测闭合。

### N-4 (Nit) Kotlin `require(maxLogLength >= 97)` 无单测钉住（结构性原因：位于 loadLibrary 之后）

- **位置**: `android/kotlin/LogX.kt:276-278`（`System.loadLibrary`）、`:301-303`（require）；`android/kotlin-gradle/app/src/test/kotlin/com/zzlfeng/logx/LogXPureLogicTest.kt`（仅有默认值 64KB 断言，无 96/97 边界断言）
- **现象**: C 侧下界已有 6 值穷举 + 撤销屏障 + 矩阵三层防护，Kotlin require 层则无任何测试钉住。结构性原因：`init()` 中 `System.loadLibrary("logx")` 在 require **之前**执行，JVM 纯逻辑单测一旦调 `init()` 即触发 `UnsatisfiedLinkError`，永远到不了 require——即现有测试形态下该断言不可测。
- **后果**: 若未来 Kotlin require 被误删/改回 `> 0`，现有单测全绿不报警（C 侧门仍在，风险为分层冗余退化）。
- **修法（可选）**: 将纯配置类 require（130943 上界、97 下界等不依赖 context/native 的校验）前置到 `loadLibrary` 之前，即可在 `LogXPureLogicTest` 中以 `assertFailsWith<IllegalArgumentException>` 钉住 96/97；或接受现状，在测试覆盖观察中登记该结构约束。

### N-5 (Nit) `test_format.cpp:509` 自指 static_assert 无钉住力（:508 有效，实测双复红）

- **位置**: `tests/unit/test_format.cpp:508-510`
- **现象**: `static_assert(kMinLogLengthValid == kUserHdrSize + kMaxTag + kHeaderLen, ...)`（:509）与头文件 `logx_format.h:43` 的定义式**完全同构**，恒真、无钉住力——R35 对 wire 断言已有同类先例（Nit）。而 :508 的 `kMinLogLengthValid == 97` 有独立价值：把字面 97 也钉死。编译期变异实验实测两处**均**报 C2338（头文件 :44 与测试 :508），守卫真实有效。
- **修法**: 删除 :509（或改注为文档性说明）；保留 :508。

---

## 三、已排除疑点（与发现项同等记录）

1. **97 下界推导与求解器语义严格一致**。`kCannotFit` ⇔ `max_total < kUserHdrSize + tag_len + kHeaderLen`（`solve_truncated_msg_budget`，format.cpp:199-215，返回 false 于 :202；触发点 format.cpp:262-263）。tag_len 经 `tag_len_bounded` 限 0..64（format.cpp:66-69，且本批已单源绑定 `logx::kMaxTag`）。故 `max_log_length ≥ 97` ⇒ 对任意合法 tag 预算恒可解 ⇒ 运行期 `kCannotFit` 对合法 open 配置**不可达**。97 为闭可达下界：97 + 64B tag 时 budget=0，空记录总长恰 97 ≤ 97（test_format 矩阵含此边界组合，实测 kOk）。
2. **加密路径与记录预算正交**。`max_log_length` 在 core 内唯一消费点为 `format_record_bounded` 的预算形参（flusher.cpp:1071）；AES-GCM 在块级施加（flusher.cpp:168 `encode_block`；块尾 +16B 计入块尺寸，flusher.cpp:393），不参与单条记录预算。故 97 下界在加密开启时同样充分，不存在「加密后 97 不够」的隐藏陷阱。
3. **test_codec 第 7 节断言非「错因通过」**。进入第 7 节时 `compress_algo = kAlgoNone`（第 4 节设置后 5/6 节只动 encrypt 侧），下界循环的 INVALID_ARG 只能来自新增长度门；六值 {0,1,32,33,40,96} 中 1..96 五值为新门独有判别（0 与旧门重叠，无害）。
4. **旧 `== 0` 校验无 active 代码残留**。全仓扫描 `max_log_length == 0`：仅命中 docs 历史报告/CHANGELOG 档案（不属 active 代码）与 JNI 三元兜底 `maxLogLength > 0 ? … : 64KB`（`logx_jni.cpp:282`，0→64KB 合法值，语义自洽且经新门放行）。tester 的 `== 0` 属 N-1 已报，不重复计。
5. **运行时兜底路径测试有效承载**。test_flusher / test_j04 的注入式改写（合法 open(97) 后直写 `inst->max_log_length = 33`）依赖的注入点即运行时唯一直读处（flusher.cpp:1071），注入必然生效；且撤销对照复做实证两用例承载新门（见下）。
6. **既有用例/压力/bench 无误拒**。全仓 `max_log_length` 配置值最低 500（test_robustness.cpp:1362），stress 20KB/4096、bench 常量、其余 4096/64KB——clean 重建后 179+2+4+3 全绿实证无回归。
7. **build-full 增量构建对头文件变更完全不感知（R21-2 必要性的直接实证）**。变异实验中 `build_tmp.bat` 两次输出 "no work to do"，`test_format.cpp.obj` 时间戳（00:02:34）早于头文件变更（00:06:15）——头文件改动在增量路径下被忽略而非报错。本审查所有头文件相关验证均走 clean 路径（`clean_build.bat` 全量 ×2 + 基于已知良好 CRLF 模板的手动单 TU 编译），结论不受影响；**任何「改了头文件后增量构建通过」的证据在本仓库不可采信**。
8. **编译期守卫钉住力与撤销屏障均实证成立**（实验详见 §六）：①变异 `kUserHdrSize` 27→28 → `logx_format.h(44)` 与 `test_format.cpp(508)` 双双 `error C2338` 编译期复红，还原后字节级一致（`git status`/`git diff` 空）且四套件复绿；②`logx_open` 校验回退 `== 0` → `CodecTest.LogxOpen_CompressAlgo_FailFast` 与 `ErrorMonitoring.CannotFitDropPath_PreservedForPathologicalLimit` 两轮独立复跑均确定性 FAILED（运行时同步校验路径，无时序依赖），还原复绿 179/179。与 R35 教训相关的确定性要求本轮天然满足。

---

## 四、测试覆盖观察

以下各条均经实际 grep/读码核实，非延续前轮结论：

- **新增矩阵用例（`MaxLogLength_BoundsFailFastAndBudgetMatrix`）覆盖良好**：max_total ∈ {97,100,128,500,4096,65536} × tag ∈ {0,1,7,32,40,64} 共 36 组合断言 kOk 且 total ≤ max_total；其中 100/128 × tag 32/40/64 组合的预算落在 3..31 < 48，顺带覆盖了「预算不足 48B 保前缀弃 marker」冷路径在长 tag 下的行为。
- **96 边界的单点判定是完备的**：`max_total=96` 时仅 tag=64 落 kCannotFit（tag ≤ 63 时 96 ≥ 27+tag+6 本就可解），故 :529-541 的单点断言对 96 无遗漏。
- **缺口一（N-1）**：tester 层 96/97 边界无任何验收项（README 手工场景表未更新）。
- **缺口二（N-4）**：Kotlin require 层 96/97 无单测，且受 `loadLibrary` 顺序约束，现有测试形态不可测。
- **无其他缺口**：撤销屏障（test_codec/test_flusher）、运行时兜底注入（test_flusher/test_j04）、编译期守卫（头 + 测试双 static_assert）四层齐备。

---

## 五、验证局限（必须说明）

1. **Android 真机未复跑**（本批亦未声称真机）：无设备接入。本批主机新增 1 用例，真机套件数应同步 +1（主机 179 → 真机预期 180/180，含设备专属用例），待设备接入后按惯例复跑并在 CHANGELOG 补记。
2. **Kotlin Gradle `testDebugUnitTest`（23 任务）未独立复跑**：本审查仅静态核对该层 diff（KDoc 与 require）及 require 在 `init()` 中的位置。
3. **Stage A~D 基准未复跑**：本批未触碰 bench 代码；计划表 v1.52 修订说明中「Stage A~D 基准稳定通过」未独立复核。
4. **`LOGX_WITH_AESGCM=0` / `LOGX_WITH_ZSTD=0` 关闭矩阵未复测**：本批改动位于与编译开关正交的校验/常量层（fail-fast 门不依赖加密符号），风险低，按惯例声明。

---

## 六、实验记录（本审查运行的实测）

| 实验 | 操作 | 结果 |
|---|---|---|
| clean 全量重建 ×2 | `build-full` + `clean_build.bat` | 58/58 步、RC=0，诊断精确匹配 `warning C[0-9]\|error C[0-9]\|error LNK\|fatal error` 计 **0** |
| 四套件 | unit / hotpath / stress / crash_fork | **179/179 + 2/2 + 4/4 + 3/3 全绿**（与提交声称一致） |
| 编译期变异 | `kUserHdrSize` 27→28，单 TU 手动编译 | `logx_format.h(44)` + `test_format.cpp(508)` 双 **C2338** 复红；还原后 `git status` 空、四套件复绿 |
| 撤销对照 ×2 轮 | `logx_c.cpp:342` 回退 `== 0`，增量重建（7 边）后跑两目标用例 | 两轮均确定性 FAILED（test_codec、test_flusher）；还原重建复绿 179/179 |
| NDK 交叉编译 | `build-android-tests --clean-first`（62/62）、`build-android`（9/9 增量） | RC=0；**3 条存量 Clang 警告**（N-2）；「27/27」口径无法复现（N-3） |
| 穷尽扫描 | `max_log_length == 0` / `[^0-9]97[^0-9]` / `kUserHdrSize|kMaxTag` / 建议口径 / stress·bench 配置值 | 结果见 §三 排除项 4/6 与 N-1 |

---

## 七、上轮（R36）遗留对照

| R36 遗留 | 本轮状态 |
|---|---|
| B-6（已给方案：推荐翻案实施 fail-fast `<97→INVALID_ARG`） | **本批即方案 A 落地**，本轮复核通过，Backlog 可标闭环 |
| B-11（Android 冒烟入纪律，方案 A/B 待拍板） | 未动；N-2/N-3 为其新增佐证（clean 口径警告与步数门禁的价值） |
| B-5（R=前置协议自动化先行，A/B 待 R 出结果） | 未动 |
| 「Android NDK 62/62 与 NDK 警告清零声称未独立复现」 | 本轮实测闭合：62/62 成立；「全量零警告」证伪（3 条存量，见 N-2） |

---

## 八、修复顺序建议

1. **N-1**（tester 三层口径收口）——独立小批或随下次 tester 改动；README 场景表 96/97 档可与真机复跑同批验收。
2. **N-2**（3 处存量警告清理）——建议并入 B-11 批次，以「NDK clean 全量零警告」作为纪律门禁一并固化。
3. **N-3**（CHANGELOG 步数口径补记）——随真机复跑收口补记，同时闭合 R36 遗留核对项。
4. **N-4 / N-5**（可选微收口）——N-4 若采纳「require 前置」须同步审视 `init()` 异常回滚路径（K-03）不受影响；N-5 一行删除。

---

---

## 九、收口记录 (2026-10-10 修复批次)

N-1~N-5 已全部收口，验证实测如下：

| 项 | 收口内容 | 验证 |
|---|---|---|
| N-1 | `ConfigPanel.kt` 预警条件 `< 97 \|\| > 65536` + 文案 97..65536 + 滑块提示补 <97 档；Preset 7 升级四档越界（131072/100000/96/0）；README 参数表/场景表补 96/97 | `logx-tester :app:compileDebugKotlin` BUILD SUCCESSFUL（Gradle 9.4.1 + JDK17） |
| N-2 | 删 `test_staging.cpp` 未用 `kTotal`；`test_concurrent.cpp` 常量上提文件作用域 `kStressWorkerCount`/`kStressLogsPerWorker`（化解 Clang「无需捕获」警告 vs MSVC C3493 必须捕获的跨编译器冲突）+ 删多余捕获；`raw_len` 死读取转 kAlgoNone+明文下的 `EXPECT_EQ(raw_len, payload_len)` 交叉断言 | **NDK clean-first 62/62 实测 0 警告**；stress 4/4 全绿（含新断言） |
| N-3 | CHANGELOG 条目 15 NDK 步数 27/27 → clean 实测 62/62 勘误（附注记） | 文档核对 |
| N-4 | `LogX.kt` 抽取 `internal fun validateInitConfig(config)`（97/130943 双界，消息原文不变），`init()` 前置调用（病态配置不再触发 native 加载、不消耗 initCount）；`LogXPureLogicTest` 新增 3 用例 | Gradle 8.9+JDK17 `testDebugUnitTest` 23 任务 BUILD SUCCESSFUL，13 用例 0 失败（含 validateInitConfig_rejectsMaxLogLengthBelow97 / AtDataSizeBound / acceptsBoundaryValues） |
| N-5 | 删 `test_format.cpp` 自指 static_assert，保留 `== 97` 钉住 | MSVC 四套件 179/179+2/2+4/4+3/3 全绿 |

其余验证：MSVC 增量重建（纯 .cpp 改动，无头文件变更）0 错误；本批源码改动均为测试/外围层，未触碰 core 产品代码。收口批次对应 CHANGELOG 条目 16、计划表 v1.53。

**审查方法论补记（本批实测新知，供后续轮次引用）**：① build-full 增量构建对头文件变更完全不感知（§三 排除项 7）；② constexpr 局部量在 lambda 显式捕获列表下存在跨编译器语义冲突——Clang C++17 允许无捕获读取（捕获反触发 -Wunused-lambda-capture），MSVC 则要求显式捕获（C3493）——跨平台测试代码中的此类常量应上提文件作用域；③ 本机 Gradle 入口：PATH 中的 gradle-4.10.2 无法用于本项目（AGP 8.5.1 需 ≥8.7），实际入口为 `~/.gradle/wrapper/dists` 下 Gradle 8.9（kotlin-gradle）与 9.4.1（logx-tester）+ JDK17，且须经干净环境（`env -i`）启动，Git Bash 直跑报「Unable to get mutable Windows environment variable map」。

**真机复跑补记（2026-10-10，收口批次提交后）**：Redmi K30 Pro (Android 12, 无线 adb 39389) 四套件 **180/180 + 2/2 + 4/4 + 3/3 全绿**（退出码全 0；主套件含设备专属 1 例，与主机 179 口径一致）。产物 mtime：R37 改动三 TU 重编重链（20:15），liblogx.so 与其余产物经 ninja 全图核验为当前；时钟 1:1 且 auto_time=0 阶跃防护在位。

---

*审查人：Qoder（R37）；所有实验命令与输出见 §六，收口批次见 §九。*
