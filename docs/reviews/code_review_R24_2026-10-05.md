# logx 代码审查报告 (R24) —— R23 报告结论的独立验证与修正

> **复核对象**: `code_review_R23_2026-10-05.md` 全部裁定（F-1/F-2 缺陷、四轮提交有效性、方案 A/B/C 架构建议）
> **复核日期**: 2026-10-05
> **复核方法**:
> ① git 考古：对 `66c5345`/`8c71afb`/`69f274d`/`3b90173` 逐提交提取历史版本源码与测试文件比对；
> ② 等价构建纪律实测：按 R22-1 纪律从 `r19f_commands.txt` 提取 MSVC 编译/链接命令，对 `66c5345` 原版测试代码独立重编重链并运行；
> ③ 逐行推演：对 `probe_wrap_seam` / `split_impl` / `drain_once` / `parse_records_stream` / `write_records` 全链路手工执行字节级推演。
> **复核结论**: **R23 核心结论全部成立**——F-1（P1）与 F-2（P2）均经本报告独立实测复现，工程纪律指控证据确凿；但 R23 第四节"方案 A 可彻底删除下游防御"存在**覆盖面过度声明**，详见本报告第四节。

---

## 一、总裁定

| R23 声明 | 验证方式 | 裁定 |
|---|---|---|
| F-1（P1）：`66c5345` 新增单测确定性失败，`logx_write` 返回 `-4` | 等价构建纪律重编原版测试并运行 | ✅ **属实，逐字复现** |
| F-2（P2）：脏缝防御分支不可达，脏数据融合落盘 | 修复版测试实测 + 字节级推演 | ✅ **属实，双重验证** |
| CHANGELOG "实测验证通过"系虚假声明 | `git show 66c5345 -- CHANGELOG.md` | ✅ **属实** |
| R19 提交时测试文件 0 字节 | `git show 3b90173` 实测 | ✅ 属实（0 字符） |
| R20 补齐 8 项单测、当时无全零校验 | `git show 69f274d` 实测 | ✅ 属实 |
| R21 F-5 加密能力锁定彻底有效 | 逐行审读 `open_or_create_append_target` | ✅ 属实 |
| R22 死代码清理成立 | diff 审读删除分支的可达性 | ✅ 属实 |
| F-4：`key_provider` 重复调用未收敛 | 全仓调用点扫描（5 处） | ✅ 属实 |
| 方案 A 可"100% 根除微块"、删除全部下游防御 | 切块路径逐支分析 | ⚠️ **过度声明**，见第四节 |

---

## 二、F-1（P1）独立实测复现

### 2.1 复现环境与命令

按 R22-1 等价构建纪律，绕过 ninja，直接执行 MSVC 工具链：

1. 从 `logx/build-full/r19f_commands.txt` 提取 `test_r19_regression.cpp` 的 `cl.exe` 编译命令；
2. 先 `git stash` 暂存工作区对该文件的未提交修改，使源码树回到 HEAD（`66c5345`）原版测试；
3. 重编 `.obj` → 按 ninja `-t commands logx_unit_tests` 提取的 `link.exe` 命令重链 `logx_unit_tests.exe`；
4. 运行 `--gtest_filter='R19Regression.F7_*'`。

### 2.2 实测输出（与 R23 报告逐字一致）

```text
[ RUN      ] R19Regression.F7_MicroBlockNotSeamPadding_DropsAndEmitsRecoveryEvent
D:\Project\Android_Project\LogX\logx\tests\unit\test_r19_regression.cpp(1061): error: Expected equality of these values:
  logx_write(inst, 2, kTag, rmsg.data(), static_cast<uint32_t>(rmsg.size()))
    Which is: -4
  LOGX_OK
    Which is: 0
[  FAILED  ] R19Regression.F7_MicroBlockNotSeamPadding_DropsAndEmitsRecoveryEvent (1 ms)
```

行号（1061）、错误码（`-4 = LOGX_ERR_NO_SPACE`）、耗时（1 ms）均与 R23 报告第三节现场复现完全吻合。

### 2.3 根因复核

[`write_records`](file:///d:/Project/Android_Project/LogX/logx/core/src/staging.cpp#L290-L358) 空闲计算 `free = (f + data_size - w - 1) % data_size`（[staging.cpp:302](file:///d:/Project/Android_Project/LogX/logx/core/src/staging.cpp#L302)）。原版测试在 `w = data_size - 3`、`f = 0` 的状态下直接写入 88B 记录（6B 记录头 + 27B 用户头 + 6B tag + 50B msg）：`free = data_size - (data_size-3) - 1 = 2 < 3 + 88` → [`kFull`](file:///d:/Project/Android_Project/LogX/logx/core/src/staging.cpp#L310) → [logx_c.cpp:561](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L561) 返回 `LOGX_ERR_NO_SPACE`。R23 的推演链**完全正确**。

### 2.4 虚假声明证据定位

`66c5345` 对 [`CHANGELOG.md`](file:///d:/Project/Android_Project/LogX/logx/CHANGELOG.md#L2092-L2105) 新增的第 2105 行：

> `- **实测验证**：`ninja` 或对应构建工具全量重建通过，新增单测顺利执行并成功断言。`

而该测试自提交起从未通过。另从构建产物时间线看：`build-full` 内各 `.obj` 多数为 10-05 02:08，`test_r19_regression.cpp.obj` 与 `logx_unit_tests.exe` 时间戳为 **19:52**（晚于 `66c5345` 的提交时刻 19:43:54 仅 8 分钟）——即提交时刻并不存在包含该测试的新二进制，"实测验证通过"无产物支撑。

---

## 三、F-2（P2）独立实测复现

### 3.1 动态实测（含 flush 修复版）

工作区现存一份未提交的测试修改（在写入 `rmsg` 前补 `logx_flush(inst)`，即 R23 建议的"止血"）。对该版本重编运行：

```text
[ RUN      ] R19Regression.F7_MicroBlockNotSeamPadding_DropsAndEmitsRecoveryEvent
test_r19_regression.cpp(1073): error: Value of: probe.saw_msg_substring("sub-header block is not seam padding; dropped")
  Actual: false
Expected: true
test_r19_regression.cpp(1074): error: Expected equality of these values:
  ii->seam_blocks_skipped.load()
    Which is: 0
  1
[  FAILED  ] R19Regression.F7_MicroBlockNotSeamPadding_DropsAndEmitsRecoveryEvent (76 ms)
```

**结论**：补上 flush 后测试依然失败——`LOGX_EV_RECOVERY` 事件未触发、`seam_blocks_skipped` 计数为 0。R23"即便修复 F-1 该测试仍无法通过，并暴露产品代码设计漏洞"的判断**实测成立**。这同时证明：当前工作区的止血修复（仅测试侧）不足以让套件回绿，产品侧必须同步修正。

### 3.2 字节级推演复核

以 `data_size - 3` 处 3B 缝 `[0x00, 0x00, 0xAA]`（脏字节注入在环尾最后一字节）、回绕后 89B 记录（payload 83B → 记录头 `[0x53, 0x00, 'L', 'G', ...]`）为例，拷出的 92B scratch 流：

1. [`split_impl`](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L58-L126) 在 `pos=0` 处 `peek_record_len` 返回 0（`len=0x0000`，`magic=0x4c53` 失配）；
2. [`probe_wrap_seam`](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L47-L55)：`s=1`、`s=2` 时 `raw[pos+s-1]==0` 但其后均非合法记录头；`s=3` 时 `raw[2]=0xAA≠0` **break**，返回 0；
3. 走[撕裂尾兜底](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L78-L82)：`take = min(92, 20486) = 92` → 单块 `{0, 92}`——**3B 脏缝与 89B 合法记录融合**；
4. 回到 [`drain_once`](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L803-L825)：`blk.raw_len=92 ≥ kHeaderLen=6`，微块拦截分支**不可达**，块被正常编码追加写盘。

R23 报告该机制追踪（含其"raw_len = 3 + overhead + 50 ≥ 80"的尺寸估算，实测 92B）**逐环节正确**。读端后果亦核验属实：[parse_records_stream](file:///d:/Project/Android_Project/LogX/logx/core/src/format.cpp#L400-L423) 在块首遭遇 `0x00 0x00 0xAA 0x53...` 时 magic 失配 → FM-02 向前重同步 → [carry_slice_buf 清空](file:///d:/Project/Android_Project/LogX/logx/core/src/format.cpp#L418-L423)，若前序块尾恰有未完成的跨块分片，该分片被连带丢弃。

### 3.3 全量套件交叉验证

以 `66c5345` 原版测试 + 当前产品码运行全量单测：**162 个用例，161 通过，唯一失败即 F7**。与 R23 第五节"R19~R21 基线 161/161、R22 增量 13 通过/1 失败"的数字体系自洽（R19Regression 套件现 14 例，13 绿 1 红；其余 148 例全绿）。

---

## 四、R23 报告自身的瑕疵：方案 A 覆盖面过度声明

这是本次复核发现的 **R23 唯一实质性偏差**。R23 称方案 A（放宽 [`flusher.cpp:104`](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L104) 封块上限 5B）可"从源头上 100% 根除 1~5B 微块的生成"，并据此主张"彻底删除 `drain_once` 的 `raw_len < kHeaderLen` 检查、全零检查、`seam_blocks_skipped` 计数及 RECOVERY 分支"。

### 4.1 微块生成路径有两支，方案 A 只覆盖一支

`split_impl` 产出 1~5B 微块的路径：

| 路径 | 代码位置 | 触发场景 | 方案 A 是否消除 |
|---|---|---|---|
| **P1：缝随记录后**（F1b 型） | [flusher.cpp:100-108](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L100-L108)：缝并入当前块将超 `kBlockMaxRaw`，只能独立封块 | 满长记录停在环尾前 1~5B，缝后有记录 | ✅ 消除（放宽上限后缝并入块尾） |
| **P2：缝在待刷窗口头**（F1c 型） | [flusher.cpp:64-68](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L64-L68)（窗口整体 <6B）与 [flusher.cpp:71-77](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L71-L77)（缝独立封块） | `f` 停在缝前，回绕后窗口以缝开头 | ❌ **不受 L104 修改影响** |

P2 正是 [F1c_WrapSeamAtPendingHead_MicroBlockNeverReachesDisk](file:///d:/Project/Android_Project/LogX/logx/tests/unit/test_r19_regression.cpp#L304-L374) 实测守护的场景（场景构造见其注释：填到 `w == data_size-3` → flush → 再写一条 → 下一次 drain 窗口以缝开头）。方案 A 落地后 F1b 型微块消失，但 **F1c 型微块依旧产生**。若按 R23 建议同时删除 `drain_once` 的微块拦截分支，F1c 型微块将直接落盘——`raw_len=3 < 6` 被 [block_header_plausible](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L359) 拒收 → 导出在该块 break → repair 物理截断，**恢复出比现状更糟的数据丢失路径**。

### 4.2 为什么不能用"块头吸收缝"来补 P2

若将窗口头缝并入后续块（块首前置 1~5B 非记录字节）：

1. 破坏"块内字节流 = 记录序列"的既有不变量（[flusher.cpp:4](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L4) 自述契约）；
2. 读端 `parse_records_stream` 从块首扫描：缝字节拼出的伪 `len/magic` 必失配 → FM-02 重同步滑动至 `+s` 处 → **`carry_slice_buf` 被清空**。当跨块分片记录（16KB 切块边界落在同条日志分片序列中间）恰逢块头带缝时，前序块积累的合法分片被误弃。即块头缝不是无害填充，而是**主动触发跨片丢弃的毒药前缀**。

因此 P2 的正确处置是 R23 自己给出的**方案 B**（Staging 层前置跳过：[`copy_pending`](file:///d:/Project/Android_Project/LogX/logx/core/src/staging.cpp#L369-L380) 前检测 `data_size - f < kHeaderLen`，锁内推进 `f = 0`）。

### 4.3 修正后的正确结论

**方案 A 与方案 B 是互补关系而非二选一**：A 消灭 F1b 型（缝随记录后），B 消灭 F1c 型（缝在窗口头）。两者齐备后微块才真正绝迹，`drain_once` 的拦截/全零校验/计数/RECOVERY 分支才可整体拆除。R23 将 A 单独标为"强烈推荐，彻底砍掉整套防御"属于**以偏概全的过度声明**——单独落地 A 并照单全收其"删除下游防御"清单，会直接引入 F1c 型微块落盘的回归缺陷。

顺带核验方案 A 的读端兼容性论据本身：块尾 <6B 留零由 [format.cpp:396-399](file:///d:/Project/Android_Project/LogX/logx/core/src/format.cpp#L396-L399) 原生 break 支持 ✅；块上限 16MB（[flusher.cpp:326](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L326)）容许 20491B ✅；zstd 解压上限 `kBlockMaxRaw + 64 = 20550 ≥ 20491`（[codec.cpp:43-44](file:///d:/Project/Android_Project/LogX/logx/core/src/codec.cpp#L43-L44)）✅。**论据成立，仅结论外延过度。**

---

## 五、其余各项核验明细

### 5.1 git 考古结果

| 提交 | R23 声明 | 实测 |
|---|---|---|
| `3b90173` (R19) | 测试文件 0 字节 | `git show 3b90173:tests/unit/test_r19_regression.cpp` → **0 字符** ✅ |
| `69f274d` (R20) | 补齐 8 项单测 | 该版本恰 **8 个 TEST** ✅；且当时 `raw_len < kHeaderLen` 分支确无 `seam_all_zero` 校验（R21 才引入）✅ |
| `8c71afb` (R21) | F-7 全零校验引入 | 该版本 diff 确认 `seam_all_zero` 于本提交加入 ✅ |
| `66c5345` (R22) | 删除恒不可达死分支 | 删除的 `if (!key32 && !key_lookup_attempted && ...)` 分支中，`key_lookup_attempted` 在外层（现 [logx_c.cpp:90-94](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L90-L94)）循环前已置 true，**恒不可达判定成立** ✅ |

### 5.2 F-4（key_provider 重复调用）核验

全仓 `key_provider(` 调用点共 5 处：[logx_c.cpp:92](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L92)（导出）、[logx_c.cpp:371](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L371)（open 探测）、[flusher.cpp:543](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L543)（建文件探测）、[flusher.cpp:860](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L860)（逐块缓存重取）、[flusher.cpp:1035](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L1035)（append_block 探测）。同一轮建文件流程中 543（探测）与 860（实取）确构成重复 JNI 往返；`FlusherCtx` 的 `current_fd/current_enc_capable` 与 `drain_once` 栈上 `cached_key/key_fd` 的双轨管理也如 R23 方案 C 所述。**P3 定级与改进方向合理**。

### 5.3 行号引用精度

R23 报告的行号引用整体准确，个别存在 ≤30 行漂移（如 `drain_once` 实为 [flusher.cpp:734-910](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L734-L910)，报告写 764-868；`ensure_append_target` 实为 L649 起，报告写 715-735），均不影响指向正确代码，属可忽略瑕疵。

---

## 六、当前工作区状态与风险提示

- **HEAD** = `66c5345`（R22），`git status` 显示 [test_r19_regression.cpp](file:///d:/Project/Android_Project/LogX/logx/tests/unit/test_r19_regression.cpp) 存在**未提交修改**：F7 测试在写 `rmsg` 前补 `ASSERT_EQ(logx_flush(inst), LOGX_OK)`（现 [L1059-1061](file:///d:/Project/Android_Project/LogX/logx/tests/unit/test_r19_regression.cpp#L1059-L1061)）。该修改正确解决了 F-1 的测试侧根因（`f` 追平 `w` 后回绕写入有空间），**但按本报告 3.1 实测，单靠它套件仍红**。
- **风险**：若将该测试修复单独提交并宣称"修复 F-1"，CI/人工运行仍会失败于 1073-1074 行断言，且容易被误判为"测试又写错了"。**测试修复必须与产品侧 `probe_wrap_seam`/`split_impl` 修正同批提交**。
- 附带说明：该 flush 修复与 F1c 测试（[L362-364](file:///d:/Project/Android_Project/LogX/logx/tests/unit/test_r19_regression.cpp#L362-L364) 同款手法）一致性良好，方向正确。

## 七、后续修复建议（R25 批次清单）

### 7.1 P1/P2 止血（必须同批）

1. **测试侧**：提交工作区现存的 F7 flush 修复；
2. **产品侧**：修正[窗口头路径](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L69-L83)的脏缝识别——当 `first == 0` 且 `probe_wrap_seam == 0` 时，先探测 `[pos, pos+kHeaderLen)` 内是否存在偏移 `s ∈ [1,5]` 使 `pos+s` 处为合法记录头；存在则**封一个 s 字节的独立块**（无论其字节是否全零），交给 `drain_once` 现有的 `< kHeaderLen` 拦截分支完成全零校验、RECOVERY 事件与水位推进。这样最小改动即可打通 F-2 防御分支，且不动 `probe_wrap_seam` 的既有语义；
3. **文档侧**：修正 [`CHANGELOG.md:2105`](file:///d:/Project/Android_Project/LogX/logx/CHANGELOG.md#L2105) 的失真记录（注明该行验证声明不实，以实测输出为准），杜绝"文档绿、控制台红"的信任裂缝。

### 7.2 长期演进（采纳 A+B 组合，而非 R23 的单独方案 A）

1. **方案 A**：放宽 [flusher.cpp:104](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L104) 上限至 `kBlockMaxRaw + (kHeaderLen - 1)`，消灭 F1b 型微块（读端三重兼容性论据已核验成立）；
2. **方案 B**：`copy_pending` 前在 Staging 锁内检测并跳过窗口头缝（`data_size - f < kHeaderLen` → `f = 0`），消灭 F1c 型微块；
3. 两案落地后回归验证 F1b/F1c/F7 三用例行为（F1b 断言块序列形态需同步更新为"缝并入块尾"、F1c/F7 场景下缝在进入切块层前已被消费，用例需改写或退役），方可整体拆除 `drain_once` 微块拦截、全零校验、`seam_blocks_skipped` 计数与 RECOVERY 分支；
4. 方案 C（`ActiveFileTarget` 聚合）独立于 A/B，可随时并行推进，收编 `key_provider` 五处调用点。

### 7.3 验证纪律（沿用 R22-1/R23 建议）

任何触及 `core/` 与 `tests/` 的提交，必须附 `0 FAILED` 的真实控制台日志；构建产物时间戳与提交时间戳的交叉核对（如本报告 2.4 节手法）可作为"是否真的跑过"的旁证。

---

## 八、验证记录汇总

| 验证项 | 方式 | 结果 |
|---|---|---|
| F-1 复现（`66c5345` 原版测试） | MSVC 等价构建重编重链 + 运行 | ❌ `-4` 确定性失败，与 R23 逐字一致 |
| F-2 复现（含 flush 修复版） | 同上 | ❌ RECOVERY 未触发、计数 0，拦截分支不可达 |
| 全量单测 | `logx_unit_tests.exe`（162 例） | 161 绿 / 1 红（仅 F7） |
| R19 测试文件空置 | `git show` | 0 字节 ✅ |
| R20 八项测试/无全零校验 | `git show` | ✅ |
| R22 死代码恒不可达 | diff + 外层变量初始化审读 | ✅ |
| 方案 A 读端兼容性三论据 | format.cpp / flusher.cpp / codec.cpp 逐条核对 | ✅ 成立 |
| 方案 A"根除全部微块" | split_impl 路径逐支分析 | ⚠️ 不成立（F1c 型窗口头缝不受影响） |

**总评**：R23 是一份高质量审查报告——两大缺陷发现均真实且已实证，历史考据与纪律复盘可信，其"短期止血"建议可直接采纳。唯一需要修正的是第四节架构方案的覆盖面表述：**方案 A 必须与方案 B 组合才能兑现"删除全部下游防御"的承诺，单独落地反而会引入 F1c 型微块落盘的新回归**。
