# logx 全量源码审查报告 (R16)

> **审查对象**: logx 仓库 `a0d0d9d`（2026-09-25 17:13，R15 收口末提交；此后零提交、工作树干净）
> **审查日期**: 2026-09-30
> **审查范围**: `core/include`(6 头文件) + `core/src`(8 实现文件) + `android/cpp`(JNI 2 文件) + `tests/`(16 文件抽查+门禁扫描)，共约 5.5k 行核心 + 9.9k 行测试；third_party 与 build 目录排除
> **执行方式**: 全量逐文件通读 + 既往修复收口独立复核 + 按模式穷尽扫描（项目固化门禁）
> **方法**: 沿用项目纪律——既往收口不沿用前轮结论、逐条重验；排除项与发现项同等记录（§四）；验证局限显式声明（§六）

---

## 一、总体结论

自 R15（2026-09-25）以来仓库零代码改动，本轮为对当前 HEAD 的全量复审。**core 产品代码（含 win32 PAL）持续保持零 P2、零行为缺陷**；本轮发现集中在**测试有效性**与**契约/注释对齐层**，其中 F-1 是项目首例"回归屏障失效"类发现——R13 N-2（P2 修复）的唯一回归测试因校验和算法写错而断言空洞。

本轮发现 **0×P2 + 3×P3 + 3×Nit**，无安全类问题。

| 编号 | 级别 | 位置 | 一句话 |
|---|---|---|---|
| F-1 | **P3** | `tests/unit/test_review_p1_p4.cpp:189` | R13_N2 回归测试 `make_slice` 用加法 checksum（核心为 XOR 折叠），first 碎片从未进入 carry，目标场景未构造——**删除 N-2 修复该测试依然全绿** |
| F-2 | **P3** | `core/include/logx_format.h:80-81` ↔ `core/src/format.cpp:388-473` | `parse_records_stream` 契约注释承诺"严重损坏返回 false"，实现恒返回 true（损坏全内联容灾）；`logx_c.cpp:157` 存在恒真死分支 |
| F-5 | **P3** | `core/src/flusher.cpp:593-617` | 跨天分支清 path/split/date 但不清 `current_size`，昨日尾文件达切分阈值时当日首文件从 `.log.1` 起写（`.log` 缺失） |
| F-3 | Nit | `android/cpp/logx_jni.cpp:135-175` | `jni_key_provider_trampoline` AttachCurrentThread 后从不 Detach（同文件错误 trampoline 有完整 need_detach 处理，模式不对称） |
| F-4 | Nit | `core/src/staging.cpp:334-336` | 回绕路径注释"free 没变"失实（实际 `free_new = free_old − pad`），且 336 行 `total > free` 为数学上恒假的防御检查 |
| F-6 | Nit | `core/src/logx_c.cpp:59-68` | `make_tmp_path` 用 `default_pal()` 而非 `inst->pal`，且 random_bytes 失败回退低熵 `wallclock_ms`——与 PAL 契约"严禁低熵回退"字面冲突（仅临时文件名，非加密用途） |

---

## 二、发现项详述

### F-1（P3）R13_N2 回归测试 checksum 算法失配，断言空洞——N-2 修复失去唯一回归屏障

- **位置**: `tests/unit/test_review_p1_p4.cpp:189`（`make_slice` 内 `cksum = static_cast<uint8_t>(cksum + b);`）对照 `core/src/format.cpp:23-27`（`cksum1` 为 XOR 折叠 `c ^= p[i]`）
- **现象**: 该测试（`ReviewFixesTest.R13_N2_ExportCorruptedBlockClearsCarrySliceBuf`）自造 LGXE slice 时用**加法**校验和，而核心解析用 **XOR** 折叠。以块 1 数据 `"PARTIAL_HEAD_AAA"`（16B）手算验证：XOR = 0x1A，加法和 = 1184 mod 256 = 0xA0，**不等**（块 3 `"PARTIAL_TAIL_BBB"` 同理：XOR = 0x01 vs 加法 0xBB）。
- **触发链**: 块 1（好块，块级 CRC 正确）解码成功 → `parse_records_stream` 在其 raw 上运行 → slice1 记录级 cksum 校验失败（0x1A ≠ 0xA0）→ 走 `format.cpp:441-448` 损坏分支：跳过整条 + `current_slice_buf.clear()` → **is_first 碎片从未进入 carry_slice_buf**。块 3 的 is_last 同理因 cksum 失败被跳过。
- **后果**: N-2 的目标场景——"块 1 first 碎片残留 carry → 块 2 块级 CRC 失败清空 carry → 块 3 last 不得与之拼接"——**从未被构造**。断言 `EXPECT_EQ(result.content.find("seq=3"), npos)`（`:270`）恒真通过：carry 本来就是空的。**即使删除 `logx_c.cpp:137-140/148-153` 的两处 `carry_slice_buf.clear()`（N-2 修复本体），该测试依然全绿**——R13 唯一 P2 修复的回归屏障实际为零。（注：断言 `:266`"[seq=2 crc fail] 行存在"仍有效，块级路径不依赖记录级 cksum；失效的仅是跨块拼接场景。）
- **修法**: `make_slice` 内 `cksum + b` 改为 `cksum ^= b`（1 行）。修复后 slice1 会在块 1 解析时进入 carry，块 2 块级 CRC 失败触发 N-2 修复清空，块 3 last 因 carry 空不拼接——`:270` 断言开始真正生效（若回退 N-2 修复则输出 `seq=3` 拼接行、测试变红）。
- **需补测试**: 修 checksum 后原测试即恢复有效性；可选加自证——断言块 1 单独可解析（防再犯"构造即失效"）。

### F-2（P3）`parse_records_stream` 契约注释与实现不符，调用方存在死分支

- **位置**: `core/include/logx_format.h:80-81`（"返回 false = 格式严重损坏(magic 错 / 校验和失败 / 结构截断)"）；`core/src/format.cpp:388-473`（实现无任何 `return false` 路径，恒 `return true`）；`core/src/logx_c.cpp:157`（`if (parse_records_stream(...))` 恒真）
- **现象**: 所有损坏场景（magic 失配重同步 FM-02、长度越界滑 6B FM-04、cksum 失败跳整条）均已在函数内部按"宁丢不脏"处理完毕，从不以返回值向调用方报损坏。
- **后果**: 契约注释误导后续调用方编写永不生效的防御分支；`logx_c.cpp:157` 的条件包裹即为已存在的死分支（编译器不警告，读者误以为存在失败路径）。与 R14 D-1（`sum_raw_len_since` 契约注释失实）同类别。
- **修法**: 推荐方案 (a)：头文件契约改为"恒返回 true；损坏容灾（重同步/跳坏记录/清 carry）在函数内部完成，宁丢不脏"，并删去 `logx_c.cpp:157` 的条件包裹（内容缩进一级）。方案 (b)（让实现真正按严重损坏返回 false）不推荐——现行为是 FM-02/FM-04 设计意图，改动会波及 4 处调用方。
- **需补测试**: 无。

### F-5（P3）跨天切分沿用昨日 `current_size`，当日首文件可能从 `.log.1` 起写

- **位置**: `core/src/flusher.cpp:593-598`（跨天分支清 `current_path`/`current_split=0`/`current_date`，**未清 `current_size`**）+ `:602`（`should_rotate_file_size(c->current_size, ...)` 复用该值）
- **触发链**: 昨日尾文件已达 `max_file_size`（或 4GiB 防线）→ 次日启动（或运行中跨天）→ `ensure_append_target`：`current_path` 为空走 hint 探测，staging ATC 命中昨日文件 → `current_size = sr.last_block_end_offset`（昨日满尺寸）→ 跨天分支重置 date/split 但 size 保留旧值 → `should_rotate(旧 size)` 为真 → split 从 **1** 起步 → 当日第一个文件为 `YYYYMMDD.log.1`，`.log` 缺失。
- **后果**: 数据零丢失、导出/保留期/解析/ATC 全部不受影响（任意合法 split 均被 `parse_log_filename` 接受），但文件命名序列异常起始会给使用/运维侧带来困惑，且与"每日首卷 split=0"的隐含约定不符。
- **修法**: 跨天分支（`:594-597`）补 `c->current_size = 0;`（1 行）。
- **需补测试**: 可补——测试 PAL 注入 `wallclock_date` seam 构造"昨日满文件 + 跨天"，断言当日首块落在 split=0 文件。

### F-3（Nit）`jni_key_provider_trampoline` attach 后不 detach

- **位置**: `android/cpp/logx_jni.cpp:141-155`（EDETACHED 分支 attach 后直接继续，无 detach）；对照同文件 `jni_error_trampoline:98-131`（完整 need_detach 模式）
- **现象**: key_provider 由 Flusher 线程（native pthread，未 attach）在每块加密前调用；首次调用 attach 后**进程内永不 detach**。对照同文件错误回调 trampoline 有正确的 attach/detach 对称处理，说明正确模式已知，此处为遗漏。
- **后果**: Flusher 线程在 `logx_close` join 后退出时，ART 检测到 attached-native-thread 未 detach，打 logcat 告警（W 级）并自动回收——不崩溃、不影响数据，属资源管理不对称 + 退出期噪音。调用方线程路径（export）GetEnv 返回 OK 不受影响。
- **修法**: 镜像 `jni_error_trampoline` 的 need_detach 模式：EDETACHED 时 attach、记录、返回前 detach。注意不能无条件 detach（GetEnv == JNI_OK 的已 attach 线程不得 detach）；Flusher 每块 attach/detach 的开销（微秒级、仅加密开启时）相对块编码成本可忽略；或整线程保持 attach 并接受退出告警（现状），二选一，但应与错误 trampoline 模式统一。
- **需补测试**: 无（JNI 层桌面矩阵不编译，见 §六）。

### F-4（Nit）`write_records` 回绕路径注释失实 + 恒假防御检查

- **位置**: `core/src/staging.cpp:334-336`
- **现象**: 注释"重新计算 free(没变,但 w 改了)"——实际 `free_new = free_old − pad`（w 归零使 free 缩小 pad 字节）。且 `:336` 的 `if (total > free) return kFull` 在前置检查 `free_old < pad + total → kFull`（`:310`）成立的前提下**数学上恒假**（`free_old ≥ pad + total ⟹ free_new = free_old − pad ≥ total`）。
- **后果**: 无行为影响（恒假守卫是安全的防御代码）；注释"没变"会误导维护者推导环会计。
- **修法**: 注释改为"回绕后 free 缩小 pad 字节；下方检查为恒假防御守卫（由 :310 前置检查保证）"。
- **需补测试**: 无。

### F-6（Nit）`make_tmp_path` 用 default_pal 且低熵回退

- **位置**: `core/src/logx_c.cpp:59-68`
- **现象**: (a) 取随机数/时间走 `default_pal()` 而非 `inst->pal`——PAL 注入测试下行为源不一致；(b) `random_bytes` 失败时回退 `wallclock_ms`，与 `logx_pal.h:96-98` 契约"严禁在失败时退化为低熵伪随机数"字面冲突。
- **后果**: 该随机数仅用于导出临时文件名（非加密用途，加密侧 salt 的 random_bytes 失败是 fail-fast 拒绝的，`flusher.cpp:456-461` 已验证在位）。残余风险为 CSPRNG 失效 + 同进程并发导出 + 同毫秒三重条件叠加下临时文件撞名（E-04 的防互踩意图失效），概率极低。
- **修法**: 改用 `inst->pal`；失败回退改为"pid + 原子递增计数器"等仍具唯一性的来源，而非时间戳。
- **需补测试**: 无。

---

## 三、既往收口独立复核（不沿用前轮结论）

| 项 | 判定 | 依据（本轮实际核对） |
|---|---|---|
| R15 E-1（R13 报告 §11 就地勘误） | **属实闭环** | `git show 0185aa6` diff 确认计划表 v1.27 增补；R13 报告 `:409-410` 两行已带"编号勘误 2026-09-25 R15（E-1）"标注、`:416` 已补更正行（本报告 §11 现文本逐行读核对） |
| R15 E-2（B-6 背景句） | **属实闭环** | `0185aa6` diff 中 B-6 背景句已改为"无下界校验（仅 `==0` 与 `> kMaxRecordTotal`、`>= kDataSizeDefault` 两条上界，`logx_c.cpp:292-301`）"，与 `logx_c.cpp:294-302` 三条校验逐条吻合 |
| R13 N-2 修复本体（carry 清空） | **在位** | `logx_c.cpp:137-140`（块 CRC fail 分支）与 `:148-153`（decode fail 分支）两处 `carry_slice_buf.clear()` 均在——F-1 是**测试**空洞，非修复缺失 |
| R13 N-4（trunc_buf 越界防御） | **在位** | `format.cpp:302`（`prefix_len + marker_len > sizeof(trunc_buf)` 守卫） |
| R13 N-8（file_size_path 无符号拼接） | **在位** | `pal_win32.cpp:188-189`（`uint64_t` 移位与按位或） |
| R13 N-6（空 if 移除） | **在位** | `flusher.cpp` `append_block_raw` 无空 if 残留（全函数通读） |
| R13 N-3（失败路径出参归零契约） | **在位** | `logx_internal.h:240-242` 两段式契约注释与 `flusher.cpp:916-918`（入口三出参清零）/`:920`/`:922-924`（两条 false 路径）逐项吻合 |
| R13 N-7（死函数清理） | **在位** | 全仓 grep `map_and_validate_locked` 零命中 |
| 裸 6 穷尽扫描（项目门禁，模式 `[^0-9]6[^0-9]`） | **零残留** | core+android 全部命中逐一归类：块/Header 字段偏移（`hdr+6`、`p+6`，布局注释钉于 `logx_staging.h:38-47`/`:228`）、API 常量（`JNI_VERSION_1_6`）、UTF-8 移位算法（`cp << 6`）、权威定义处（`kHeaderLen = 6`）——无存储格式语义的裸 6 |
| R12 四类用例（RSS/PSS、抖动、分配计数、泄漏配对） | **在位** | `test_observability.cpp`（3 TEST 宏含资源/抖动断言组）、`test_hotpath_alloc_tests` 二进制（09-27 实测 2/2 过）链路完整 |

---

## 四、已排除疑点（与发现项同等记录）

| # | 疑点 | 判定与依据 |
|---|---|---|
| X-1 | `drain_once` 失败路径（fd 打开失败/写失败）仍递增 `flushed_bytes_total`，是否虚报"已落盘" | **排除（刻意设计）**。`flushed_bytes_total` 实为"已消费字节"水位：失败块同样清零 staging 区间并 commit Header（`flusher.cpp:702-711`/`:725-733`，宁丢不脏），若不计入则 `flush_blocking`（`:993`）将恒等至超时卡死；失败经 `LOGX_EV_IO` 事件上报，调用方可感知。语义自洽 |
| X-2 | `logx_close` 双重调用为 UAF，`LOGX_ERR_STATE` 注释"已 close"场景是否承诺防护 | **排除（契约边界）**。头文件线程模型未承诺 double-close 安全；句柄生命周期归调用方是 C ABI 惯例，历届（R13-R15）同口径未列为发现 |
| X-3 | split 耗尽（65535）后每轮 `drain_once` 重复 emit 事件 | **排除（设计意图）**。`flusher.cpp:604-609` 注释明示"停写并明确报警 (WARN-4)"；跨天分支显式 `current_split = 0` 恢复，已验证恢复链路（F-5 仅涉 size 未复位） |
| X-4 | JNI `nativeGetStats` 数组长度 6 三处硬编码（`:462/:465/:473`） | **排除（装配常量）**。与 `logx_stats_t` 非 reserved 字段数耦合，注释（`:449-451`）已声明一一对应关系；属跨语言装配而非存储格式常量族，不在裸 6 门禁射程内 |
| X-5 | `tag_len_bounded` 读 `tag[64]` 是否越界 | **排除**。`strnlen(tag, 65)` 语义 + 头文件 NUL 终止契约（`logx_c.h:117-119`），FM-01 注释（`format.cpp:67-72`）已完整论证 |
| X-6 | `solve_truncated_msg_budget` 片数扫描上限 4 是否够 | **排除**。64KB 总长 ÷ 20480B/片 ≤ 3.2 片，上扫 4 片覆盖；`static_assert`（`format.cpp:233-236`）钉死 trunc_buf 容量前提 |
| X-7 | posix `Event::wait_timeout` 用 CLOCK_REALTIME，NTP 跳变影响超时精度 | **排除（常见取舍）**。`pthread_cond_timedwait` 默认 realtime 时钟；头文件契约"不限制虚假唤醒"（`logx_pal.h:77`）+ 循环重查条件已兜底 |
| X-8 | `pal_posix` `atomic_add/load` 经 `reinterpret_cast` 转 `std::atomic` | **排除（经典写法）**。`Instance` 统计字段本身即 `std::atomic<uint64_t>`，对齐成立；全仓无经裸 `uint64_t*` 的调用方 |
| X-9 | recovery R-02 `truncated_blocks` 计数差一 | **排除**。`recovery.cpp:243-258` 语义锁定注释已论证两种情况恰好计数一次；本轮重读确认分支覆盖完备（`S_hdr > seqno_file` 记差值 / `truncated` 单独 +1） |

---

## 五、修复顺序建议

| 序 | 项 | 改动量 | 优先理由 |
|---|---|---|---|
| 1 | **F-1** `make_slice` checksum 改 XOR | 1 行 | 恢复 R13 唯一 P2 修复的回归屏障；改后建议真机复跑 unit 套件确认该测试仍绿（此时为有效绿） |
| 2 | **F-5** 跨天分支补 `current_size = 0` | 1 行 | 触发条件常见（昨日写满即触发），与 F-1 同批可测 |
| 3 | **F-3** key_provider trampoline 补 detach 对称 | 3–5 行 | 与错误 trampoline 模式统一；随 Android 侧改动批走 |
| 4 | **F-2** `parse_records_stream` 契约注释修正 + `logx_c.cpp:157` 死分支清理 | 注释 2 行 + 缩进 | 与 R14 D-1 同类文档收口 |
| 5 | **F-4** staging 回绕注释修正 | 1 行 | 纯注释 |
| 6 | **F-6** `make_tmp_path` 改 `inst->pal` + 回退源加固 | 3–5 行 | 三重罕见条件，最后处理 |

全部落地后 R13→R16 四轮链闭环：产品代码层自 R13 修复批次起持续零缺陷；F-1 修复使 N-2 屏障真实生效。

---

## 六、验证局限（必须说明）

1. **真机未复跑**：设备 d16ced26 今日未连接（`adb devices` 空）。但经三重证据链——① 4 个测试二进制 mtime 均为 2026-09-27 21:07，晚于最后提交 09-25 17:13；② `ninja -C build-android-tests` 报 "no work to do"（二进制与当前源码严格一致）；③ git 零提交零工作区改动——**2026-09-27 实测批次的 156/156 全绿结论对当前 HEAD 逐字有效**，本轮所有结论不依赖运行期行为。
2. **F-1 的运行期复现**：checksum 失配为手算十六进制验证（`"PARTIAL_HEAD_AAA"` XOR=0x1A vs SUM=0xA0；`"PARTIAL_TAIL_BBB"` XOR=0x01 vs SUM=0xBB）。"删除 N-2 修复测试仍全绿"的断言为静态推导（carry 为空 ⟹ 无拼接输出 ⟹ 断言通过），运行期复现需修改测试后真机实跑，属修复批次工作。
3. **JNI 层（F-3）**：`logx_jni.cpp` 仅 Android 构建编译，桌面矩阵与 `test_jni_string.cpp`（纯逻辑解码器，桌面可测）均不覆盖 attach 行为；需真机带 keyProvider 的 init + close 后观察 logcat 告警验证。
4. **win32 PAL**：本机 cmd.exe/MSVC 构建仍被禁（沿既有限制），pal_win32.cpp 结论维持静态推导；安卓真机链路（NDK/pal_posix）不受此限。
5. 测试文件为抽查 + 门禁扫描（非逐行通读）；重点抽查覆盖 R13/R14/R15 全部关联用例与 checksum 构造模式全仓 grep（加法仅 F-1 一处）。

---

## 七、修复批次落实记录（2026-10-01 追加）

§五 全部 6 项已于 2026-10-01 按建议顺序落地并提交（5 个提交，F-2/F-6 同文件按先例合并）：

| 项 | 落实 | 提交 | 改动 |
|---|---|---|---|
| F-1 | ✅ | `151294a` | `test_review_p1_p4.cpp` `make_slice` checksum 改 XOR 折叠，附成因注释（防"构造即失效"再犯） |
| F-5 | ✅ | `93f6f9c` | `flusher.cpp` 跨天分支补 `c->current_size = 0;`（含 R16 F-5 注释） |
| F-3 | ✅ | `61bd5e1` | `logx_jni.cpp` key_provider trampoline 镜像 `jni_error_trampoline` 的 need_detach 模式：EDETACHED 时 attach 记录、返回前 detach；GetEnv==JNI_OK 的调用方线程不 detach。密钥先拷入 thread_local（detach 不影响其生存期） |
| F-2/F-6 | ✅ | `082cc5a` | `logx_format.h` 契约注释改"恒返回 true（容灾内部完成）"；`logx_c.cpp` 恒真 `if` 包裹删除、内容缩进一级；`make_tmp_path` 签名改 `(Instance*, uint32_t)` 统一走 `inst->pal`，CSPRNG 失败回退进程内原子递增计数器，补 `#include <atomic>` |
| F-4 | ✅ | `d80cd9d` | `staging.cpp` 回绕注释改如实描述（free 缩小 pad 字节 + 恒假防御守卫定位） |

**编译验证**：`ninja -C build-android-tests` 全量重编 26/26 成功（含 logx_jni.cpp，确认 F-3 在 Android 工具链下编译通过）；本次改动文件零新增警告（构建输出仅 3 条既有告警：`test_staging.cpp:22` 未用常量、`test_concurrent.cpp:77/229` 未用捕获/变量，均未触碰文件，非本批引入）。

**运行期验证悬置**：设备 d16ced26 全天未连接（`adb devices` 空）。修复后二进制已重建（2026-10-01 13:49，含 F-1/F-5 的单测改动与 core 行为改动），**待设备可用后按 09-27 链路复跑 156 用例**——重点确认：①`R13_N2` 用例修复后仍绿（有效绿：slice1 碎片真正进入 carry）；②全量无回退。win32 PAL 侧（本批未触碰）维持静态推导。CHANGELOG/计划表的 R16 收口条目按惯例待真机复跑出结果后一并补记（历轮收口行均含测试矩阵结论）。

---

**结论**：R16 全量复审——产品代码（core 8 文件 + JNI 2 文件 + win32/posix PAL）零行为缺陷，既往 R13-R15 全部修复与勘误经独立复核确认在位。发现 0×P2 + 3×P3 + 3×Nit：F-1（回归屏障失效）为本轮最重要发现，F-2/F-5 为契约与边界收口，三项 Nit 为一致性打磨。修复批次已于 2026-10-01 全部落地并编译验证（见 §七），待真机复跑收口。
