# logx 代码审查报告 (R21) —— R20 修复批次复核

> **复核对象**: 提交 `69f274d`（2026-10-05 00:40，R20 复核补强批次：F-1 缝独占微块 + F-3 自愈事件 + F-6 头位一致 + 回归测试补齐 8 项），基线 `3b90173`。工作树复核开始时干净。
> **复核日期**: 2026-10-05
> **复核范围**: ① `69f274d` 全量 diff 逐行独立审读（flusher / recovery / logx_c.h / logx_pal.h / CHANGELOG / 新增测试）；② 修复点上下游契约核对（写侧 `split_impl`+`drain_once` ↔ 读侧 `block_header_plausible`/`scan_one`/`decode_one_file`/Python decoder ↔ staging 环语义）；③ 全量重建 + 全量测试复跑；④ **撤销对照实验**（R18-2 门禁强制步骤）。
> **方法**: `MSYS_NO_PATHCONV=1 cmd.exe /c build_tmp.bat`（vcvars64 + ninja）全量重建 → 复跑 156 单测 / 2 热路径 / 4 压力 → 逐项撤销被测修复、重建、复跑，观察是否复红 → `cp` 还原 + `git status --porcelain` 确认干净 + 重建复绿。

---

## 一、总体结论

**产品代码侧：R20 三项修复（F-1 微块跳过 / F-3 自愈事件 / F-6 头位一致）经逐路径推演均成立，未发现新的功能性缺陷。测试与文档侧：本批补齐的 8 项回归用例中，有 4 项经撤销对照实验证明为「空洞绿」——R20 自己的三项产品修复全部撤销后，全仓 156+4+2 用例一个都不红。**

| 维度 | 结果 |
|---|---|
| 构建 | ✅ 全量重建 **56 步** 通过，0 error / 0 warning（**ninja 未挂死**，见 §五） |
| 单测 | ✅ `logx_unit_tests.exe` **156/156**；热路径 **2/2**；压力 **4/4** —— CHANGELOG 记录与实测一致 |
| 产品修复正确性 | ✅ F-1 / F-3 / F-6 / F-15(PAL) 四项成立（推演 + 实测，见 §四） |
| **回归屏障** | ❌ **撤销 F-1+F-3+F-6 三项修复 → 156/156 + 4/4 + 2/2 全绿**；提交信息与测试文件头「撤销对应修复即可复红」的说法对 F1b/F3/F6 三项**不成立** |
| 上轮（R19）修复的屏障 | ⚠️ F-10 / F-4 经撤销实验**确认有效**；**F-14 经撤销实验确认无效**（用例根本没走 rebuilt 路径）；F-2 / F-1 为静态判定有效 |
| 文档收口 | ❌ 计划表仍停在 **v1.28（R18 收口）**，R19/R20 两批次无收口行；R20 §四 要求纳入 Backlog 的两项未落 |

### 发现汇总

| 编号 | 级别 | 一句话 |
|---|---|---|
| **F-1** | **P2** | R20 三项产品修复（`drain_once` 微块跳过、`scan_one` RECOVERY 事件、`enc_capable` 头位）撤销后全仓测试全绿——本批「补齐 8 项回归」对 R20 修复点建立的有效屏障为 **0** |
| **F-2** | P3 | `F1b_SeamAfterFullLengthRecord_NoMicroBlock` 名为「无微块」却从不断言无微块，唯一相关断言 `any_micro \|\| blocks.size()>=2` 恒真；且只调 `split_into_blocks`，未触及真正修复点 `drain_once` |
| **F-3** | P3 | `F14_RebuiltTruncatedFile_SeqnoSkipsGap` 用全新 staging → `rebuilt=false`，**R19 F-14 所在的 rebuilt 分支从未执行**；断言经 `recovery.cpp:319-320` 另一路径满足（撤销实验实证），加密 nonce 跳隙零覆盖 |
| **F-4** | P3 | `F3_ShortTornLogFile_SelfHealsAndCounts` 的 RECOVERY 断言被 `recovery.cpp:308`「appended file tail truncated」同类事件满足——撤销 `scan_one` 新事件仍绿；`EventProbe` 已采集 `msgs` 却从不据此断言 |
| **F-5** | P3 | `F6②` 不覆盖 `enc_capable`（该分支用 `encrypt_enable=0`），文件头「守护 R20 残留修复决策不回退」的声称不成立；反向竞态（建文件时无钥→头声明明文，随后有钥→块加密）无处置、无用例 |
| **F-6** | P3 | 撕裂头自愈有**两个**站点，R20 只给 `scan_one:404` 补了事件；`open_or_create_append_target:510-514` 同款 1~31B 截断仍静默（无事件、无计数器） |
| **F-7** | P3 | `drain_once` 微块跳过路径零观测点（无计数器/无事件），且「`raw_len<6` ⇒ 必为纯零缝」这一**数据销毁前提未被校验**（`split_impl:64-68` 尾部微块形态理论上可含非零字节） |
| N-1 | Nit | `logx_c.h:87-89` `key_provider` 契约注释仍只写「open 与 export 路径均会调用」，R20 新增的第三类调用点（每次建文件探测，运行于 Flusher 线程）未同步 |
| N-2 | Nit | `logx_internal.h:229` 称读侧校验 `raw_len ∈ [kHeaderLen, kSingleRecordMaxRaw]`，实现用的是 `kBlockMaxRawPlausible`(16MB)（`ad04248` 起既有）；`block_header_plausible` 全仓**零直测**，R20 声称的「跨模块契约门禁」实为写侧单边自检 |
| N-3 | Nit | F3 用例中 `EXPECT_TRUE(probe.saw_kind(LOGX_EV_RECOVERY))` 连同同一句失败消息**重复两次**（:303-304 与 :316-317） |
| N-4 | Nit | 测试助手 `make_entry` 把 cksum 放在 payload 之后，生产布局是 `[len:2][magic:2][flags:1][cksum:1][payload]`（见 `format_pad`）；且 `c ^= fill` 重复偶数次恒为 0。合成记录**不可被解码**，故任何依赖其内容可解码的用例都会失真 |
| N-5 | Nit | F14 用硬编码历史日期 `20260831.log`。按 R18-1 三条件判据本轮安全（条件 (b)(c) 不成立：未经 `logx_open`、断言不依赖文件内容），但留痕：若将来 recovery 内引入 retention，此用例会静默变质 |
| N-6 | Nit | 计划表缺 R19/R20 收口行（最新 v1.28=R18）；Backlog 最新仍为 B-6，R20 §四 要求纳入的「F-5 同日多分卷孤儿块」「F-13 新文件+孤儿块」两项未落 |
| N-7 | Nit | CHANGELOG/提交信息「增量重建 49 步」在本会话不可复现（全量 56 步 / 单文件增量 6~7 步）；同时 R19/R20 记录的「ninja jobserver-pipe 挂死」本轮**未复现** |

---

## 二、撤销对照实验（本报告核心证据）

按 R18-2 门禁（计划表 §4：**「写完必须撤销被测修复跑一遍，见红方算建立屏障」**）执行。每次撤销后 `ninja` 重建（确认对应 `.obj` 与 4 个 exe 均重新链接，排除陈旧二进制），再跑 `--gtest_filter=R19Regression.*` 及全量套件。

### 实验一：撤销 R20 三项产品修复（预期：应见红）

| 撤销项 | 改动 | 结果 |
|---|---|---|
| revert A | 删除 `drain_once` 的 `if (blk.raw_len < kHeaderLen)` 跳过分支（微块重新落盘） | — |
| revert B | `write_file_header` 内 `make_flag(algo, enc_capable)` → `make_flag(algo, inst->encrypt_enable != 0)` | — |
| revert C | 删除 `scan_one` 的 `emit_event(LOGX_EV_RECOVERY, ...)` | — |

```
[1/6] Building CXX object core\CMakeFiles\logx_core.dir\src\flusher.cpp.obj
[2/6] Linking CXX static library core\logx_core.lib   ... [6/6] Linking logx_unit_tests.exe
R19Regression.*  →  8 tests PASSED      (F1/F1b/F2/F3/F6/F10/F14/F4 全绿)
全量单测         →  156 tests PASSED
压力             →  4 tests PASSED
热路径           →  2 tests PASSED
```

**结论：R20 修复批次的全部产品代码改动，对测试套件不产生任何可观测差异。** 提交信息「回归测试（补齐 8 项，撤销对应修复即可复红）」与测试文件头 §17-19「撤销对应修复即可复红（实验证据存档于 code_review_R19/R20 §五·验证记录）」两句均**不成立**——R20 报告 §五 存档的是发现缺陷的探针记录，当时这 8 个用例尚不存在，从未对它们做过撤销对照。

### 实验二：正向对照（验证实验链路本身有效）

| 撤销项 | 改动 | 预期 | 实测 |
|---|---|---|---|
| R19 F-10 | 删除 `logx_c.cpp:336` `*out_inst = nullptr;` | F10 红 | ✅ **红**（`test_r19_regression.cpp(424)` 断言失败） |
| R19 F-4 | `recovery.cpp:346` `ring_scan(..., wrap_boundary)` → `ring_scan(..., 0)` | F4 红 | ✅ **红**（`test_r19_regression.cpp(550)` 断言失败） |
| R19 F-14 | `recovery.cpp:227` `hdr.seqno = sr.seqno + 1` → `sr.seqno` | F14 红 | ❌ **仍绿** → 暴露 F-3（见 §三·3） |

正向对照成立，说明实验一的全绿不是「撤销没生效/二进制陈旧」造成的假象，而是这些用例真的不承载被测修复。

### 逐用例裁定

| 用例 | 守护目标 | 裁定 | 依据 |
|---|---|---|---|
| `F1_SeamMidStream_SplitsWithinContract` | R19 F-1 缝封块 | ✅ **有效**（静态判定，未做撤销） | `expect_readable_blocks` 含 `EXPECT_FALSE(any_micro)` + 覆盖完整性 + 上限，断言非恒真；本场景缝被吸收进块（8403B），不产微块 |
| `F1b_SeamAfterFullLengthRecord_NoMicroBlock` | **R20 F-1 微块跳过** | ❌ **空洞**（实验一实证） | 见 §三·1 |
| `F2_CloseInsideErrorCallback_SurvivesAndDefers` | R19 F-2 递延 close | ⚠️ **半有效**（未做撤销：撤销会崩溃而非断言失败） | 旧实现下会 0xC0000005，作为崩溃哨兵有价值；但 `EXPECT_TRUE(rc==OK \|\| rc==ERR_STATE)` 实际恒真（`logx_write` 无其它返回码），且不断言 `close_pending`/递延语义 |
| `F3_ShortTornLogFile_SelfHealsAndCounts` | R19 F-3 截断自愈 + **R20 事件** | ⚠️ **截断部分有效 / 事件部分空洞**（实验一实证） | 见 §三·3 |
| `F6_EncryptWithoutKey_FailFastAndPerBlockExport` | R19 F-6 fail-fast + 逐块导出 | ⚠️ **① 有效 / ② 对 R20 空洞** | 见 §三·4 |
| `F10_FailedOpen_WritesNullOutParam` | R19 F-10 | ✅ **有效**（实验二实证复红） | — |
| `F14_RebuiltTruncatedFile_SeqnoSkipsGap` | R19 F-14 nonce 跳隙 | ❌ **空洞**（实验二实证） | 见 §三·2 |
| `F4_WrapSeam_PostSeamRecordsSurviveRecovery` | R19 F-4 环坐标缝 | ✅ **有效**（实验二实证复红） | — |

**净结果：8 项用例中 3 项确证有效、2 项确证空洞、3 项部分有效；对 R20 本批三项产品修复的有效屏障为 0。**

### 收尾

三次实验后均以备份 `cp` 还原 `flusher.cpp`/`recovery.cpp`/`logx_c.cpp`，`git status --porcelain` 输出为空，重建后 **156/156 复绿**。实验未产生任何提交、未留下任何文件。

---

## 三、逐项详述

### 1️⃣ F-2（P3）F1b 用例：断言恒真 + 测错层次

```cpp
// test_r19_regression.cpp:176-191
BlockBounds bb = analyze_blocks(blocks);
EXPECT_EQ(bb.covered, raw.size());
EXPECT_FALSE(bb.any_oversize);
std::vector<PendingBlock> kept;
for (const auto& blk : blocks) if (blk.raw_len >= kHeaderLen) kept.push_back(blk);  // ← 微块被显式过滤掉
EXPECT_FALSE(kept.empty());
for (const auto& blk : kept) { EXPECT_GE(blk.raw_len, kHeaderLen); EXPECT_LE(blk.raw_len, kBlockMaxRaw); }
EXPECT_TRUE(bb.any_micro || blocks.size() >= 2);   // ← 恒真
```

三层问题：

1. **断言与用例名相反**：用例名 `NoMicroBlock`，但 `any_micro` 只出现在一个**析取式**里，且另一支 `blocks.size() >= 2` 在本场景恒真（满长记录 + 缝 + 50 条记录必然 ≥3 块）。该断言对任何输入都成立。
2. **微块被主动过滤后才断言区间**：`kept` 只保留 `raw_len >= 6` 的块，然后断言它们 `>= 6`——同义反复。
3. **测错层次**：R20 的修复点在 `drain_once`（落盘时跳过微块），而本用例只调 `split_into_blocks`。用例注释自己承认「切分器允许产出缝独占块(它不知道 drain 的兜底策略)」——**即修复点完全不在被测范围内**。

场景本身是有效的（我按常量手工推演确认：`make_entry(kBlockMaxRaw-kHeaderLen)` = 20486B 满长记录 → 累计循环因 `cur==blk_begin` 不 break → 下一轮 `probe_wrap_seam` 得 3、`20486+3 > 20486` 判伪 → break → 下一轮 `split_impl:74` 推出 `{20486, 3}` 微块，正是 R20 探针 P2 形态）。**修法**：
- 立即把恒真断言改成 `EXPECT_TRUE(bb.any_micro)`，钉死场景有效性（改动一行，撤销 R19 缝封块修复时会红）；
- 再补一个**落盘层**用例：仿 `F4_WrapSeam_*` 的环坐标编排（先填到 `data_size - kBlockMaxRaw - 3`，写满长记录，再写一条小记录触发 `pad=3<6` 留零回绕），`logx_open` 真实 Flusher + `logx_flush`，然后扫落盘 `.log` 的每个 34B 块头，断言 `raw_len >= kHeaderLen` 且 `logx_export` 能取回缝后全部记录。这才是 R20 §三·2 要求的「写读两侧共用断言」。

### 2️⃣ F-3（P3）F14 用例没走 rebuilt 路径，加密 nonce 跳隙零覆盖

用例注释（:467）称「双 Header 破坏 → rebuilt 路径接管（全新 staging）」，但**用例从未破坏 staging 双 Header**——它在一个新路径上 `new Staging(...)`。`Staging::open` 对全零新文件走 `state->rebuilt = !fresh`（`staging.cpp:229`），`fresh=true` ⇒ **`rebuilt=false`**。于是 `run_recovery` 走常规路径，`resume_seqno` 由 `recovery.cpp:319-320` 的 `max(S_hdr, seqno_file)+1 = max(0,2)+1 = 3` 得出，**与 R19 F-14 修复所在的 rebuilt 分支（`recovery.cpp:224-230`）无关**。

撤销实验证实：把 `recovery.cpp:227` 的 `sr.seqno + 1` 改回 `sr.seqno`，用例仍绿（`resume_seqno` 依旧 3）。

后果：R19 F-14 是**加密安全**修复（防止被截断块的 seqno 被复用 ⇒ 防 AES-GCM (key,nonce) 重用），目前无任何屏障；下一次重构 rebuilt 分支即可无声回退。

**修法**：在 `run_recovery` 之前真正破坏双 Header——先 `inst->staging->open(...)` 写入合法 Header 与若干字节，再对 staging 文件前 64B 覆写毒值（或直接 `memset` 两个 Header 副本的 CRC 域），使 `pick_header` 双份皆失败 ⇒ `rebuilt=true`；随后断言 `resume_seqno == sr.seqno + 1`（=3）且 `truncated_blocks >= 1`。同时保留一个常规路径的孪生断言，避免两条路径共用一个数字 3 而互相掩盖。

### 3️⃣ F-4（P3）F3 用例的 RECOVERY 断言被同类事件混淆

`scan_one` 的新事件（`flusher.cpp:404`）撤销后，F3 仍绿。原因：该场景下 `reconcile → scan_append_target → scan_one(repair=true)` 置 `truncated=true, seqno=0`，回到 `run_recovery` 后 `S_hdr=0 == seqno_file=0` 走常规分支，再由 `recovery.cpp:307-309` 的 `else if (truncated)` 发出 **`LOGX_EV_RECOVERY`「appended file tail truncated」**。`probe.saw_kind(RECOVERY)` 只比 kind，两条事件不可区分。

`EventProbe` 已经把 `msgs` 存下来了（:110）却从不使用——**修法三行**：加 `bool saw_msg_substring(const char* sub)`，断言改为 `EXPECT_TRUE(probe.saw_msg_substring("torn file header"))`。（不建议靠「RECOVERY 事件计数 ≥2」来区分，那会把无关事件耦合进断言。）

注：用例对 **R19 F-3 截断自愈本体**的守护是有效的（`EXPECT_GT(sz, 32)` + 导出可取回），撤销 R19 修复会红；空洞的只是 R20 新加的事件断言。另：同一断言连同同一句失败消息在 :303-304 与 :316-317 重复出现两次（N-3）。

### 4️⃣ F-5（P3）F6② 不覆盖 `enc_capable`；反向竞态无处置

- **覆盖缺口**：F6② 用 `encrypt_enable = 0` 的实例写盘，再手工把文件头加密位置 1。全程 `enc_capable` 恒为 `true`（`inst->encrypt_enable == 0` ⇒ 探测分支不进入），R20 的新代码路径一行未执行。用例注释（:326）称「同时守护 R20 残留修复的『头位与能力一致』决策不回退」——**该声称不成立**，撤销实验已证（revert B 后 F6 仍绿）。
  **修法**：用一个**有状态** `key_provider`（首次调用即 open 期探测返回有效 32B 密钥，之后返回 `nullptr`），`encrypt_enable=1` 开实例 → 写 → flush，然后读落盘文件头断言 `(hdr[5] & 0x01) == 0`，并断言收到 `LOGX_EV_DEGRADED` 且消息含 `key unavailable at file creation`；再断言 `logx_export` 仍能逐块解出明文。
- **反向竞态（新发现，代码层）**：探测发生在建文件时刻，落块发生在之后的每次 `drain_once`。若建文件时无钥（头声明**明文**）、随后密钥恢复可用，`drain_once:780-787` 会取到 key 并以 `flag` 含 `kFlagEncrypted` 写块 ⇒ **「头声明明文、块实为密文」**，正是 F-6 想消除的失真的镜像形态。功能上不致命（我核对了三个读端：`decode_one_file:112-113`、`scan_one:469`、`logx_decode.py:349/413` 全部按**块头** flag 逐块判定，不看文件头 flag），但文件头元数据仍然失真，且 DEGRADED 文案「file header written without encrypted bit」不含此提示。
  **修法（择一）**：① 把探测结果缓存到 `FlusherCtx`/文件级状态，本文件生命周期内**锁定**加密能力（无钥则本文件所有块强制明文，有钥则头声明加密），使「头 == 全部块形态」成为不变量；② 或明确把文件头 flag 的语义定义为「建文件时刻的能力快照，非块形态承诺」，写进 spec §3 与 `logx_c.h`，并让 decoder 侧注释同步。当前实现处在两种语义之间，无任何文档定义。

### 5️⃣ F-6（P3）撕裂头自愈双站点，只补了一处

```cpp
// flusher.cpp:510-514  open_or_create_append_target —— 无事件、无计数器
if (sz > 0 && sz < 32) {
    // R19 F-3: 1~31B 头部撕裂损坏, 截断为空文件并重写文件头
    inst->pal->file_truncate(fd, 0);
    sz = 0;
}
```

R20 的立论是「自愈必须可见，否则调用方无法区分正常空文件与撕裂头被修复」（`flusher.cpp:402-403`）。同一语义的第二站点在 `open_or_create_append_target`（运行期由 `drain_once → ensure_append_target` 到达，例如切分/跨天新开目标文件时命中一个撕裂头），仍然静默：既不发 `LOGX_EV_RECOVERY`，也不递增 `truncated_blocks`。**修法**：把该分支改为复用 `scan_one(inst, path, /*repair=*/true)`，或就地补同款 `emit_event`（一行），使「1~31B 撕裂头自愈」在全仓只有一个可观测语义。

### 6️⃣ F-7（P3）微块跳过路径：零观测 + 数据销毁前提未校验

`drain_once:765-776` 的跳过分支做了三件有副作用的事——`zero_range_locked`（**清零 staging 数据**）、`commit_header_locked`（推进 `f`，该区间自此可被覆写）、`flushed_bytes_total += raw_len`——但**不发事件、不递增任何计数器**。R19/R20 两轮反复强调的正是「计数器全 0 + 无事件 = 静默面」（R20 补 F-3 事件的动机原话）。缝跳过虽属正常态（每次 `pad<6` 回绕都会发生一次，发事件会是噪音），但**至少应有一个累计计数器**，否则「缝被跳过」与「缝被误吞了真实数据」在运行期不可区分。

更要紧的是前提：注释断言「缝本就是 write_records 留零回绕的填充」，但代码**未校验该区间确为全零**就清零并推进水位。该前提对 `split_impl:74`（`probe_wrap_seam` 逐字节验证过 `raw[pos+s-1]==0`）成立；对 `split_impl:64-68` 的**尾部微块**（`total-pos<6` 即封块）并无任何保证——我推演认为它在生产中不可达（记录写入在 `mu` 内原子完成、`w` 恒落在记录边界，`staging.cpp:305-353` 保证回绕只留纯零缝），但这是**推演结论而非被守护的不变量**。一旦将来 staging 写入语义变动，这里会静默销毁最多 5 字节真实数据。
**修法**：跳过前校验 `[cur_f, cur_f+raw_len)` 在 scratch 中确为全零；非全零则按撕裂尾处理（照常封块落盘）并 `emit_event(LOGX_EV_RECOVERY, ...)` + 计数，把「静默销毁」变成「有声降级」。

---

## 四、已排除疑点（与发现项同等记录）

以下均在复核中作为疑似缺陷追查，最终判定**不成立**，列出依据以免下轮重查：

1. **微块跳过会不会丢数据？** 不会。缝字节由 `write_records` 显式 memset 为 0（`staging.cpp:322-324` pad≥6 分支尾、`:329-331` pad<6 分支），`probe_wrap_seam` 又逐字节复验过零；缝内不含任何记录。跳过只是不把填充零写进文件。
2. **跳过会不会造成 seqno 跳号 / 读侧单调性拒绝？** 不会。跳过分支 `commit_header_locked` 传入的是**未递增**的当前 seqno（`flusher.cpp:770-771`），与两条错误路径（:804、:827）同款；只有成功落盘才 `+1`（:838）。读侧 `block_header_plausible:355` 要求严格递增，序号连续无洞。
3. **块覆盖是否连续（`cur_f` 推进会不会错位）？** 连续。`split_impl` 五条 push 路径每条都以自身长度推进 `pos`（:66/:74-75/:80-81/:86-87/:119-120），块序列无缝无叠地覆盖 `[0,total)`；`cur_f` 按同一长度模 `data_size` 推进，与 scratch 的环坐标映射 `(f+off) % size` 一致。
4. **把未落盘字节计入 `flushed_bytes_total` 是否污染指标？** 是刻意且必要的：该计数是 `flush_blocking:1109` 的**进度代理**（`while flushed < target`），不计入会导致纯缝 drain 空转至 `kFlushBlockingTimeoutMs` 超时误报 IO 事件。两条既有错误路径（:807、:829）早有同款先例。指标语义偏差 ≤5B/次回绕，可忽略。
5. **跳过分支提交的 `atc` 是循环前的旧 date/split（未过 `ensure_append_target`），跨天时会不会写坏追加目标提示？** 不会造成后果。`recovery.cpp` 只消费 `hdr.target.date` 与 `atc_split(hdr.target)`（:156-157、:170）；`AppendTargetCache::last_block_offset/end_offset` **全仓无消费者**（仅结构体定义、static_assert 与写入侧），故 offsets 为 0 无影响；跨天旧提示另由 R19 F-5 的 `best_date > hdr.target.date` 回退兜住。
6. **`scan_one` 的 RECOVERY 事件在 `logx_open` 期间发出，回调是否已装好？** 已装好。`cfg->err_fn/err_user` 在 `logx_c.cpp:398-399` 装入，`run_recovery` 在 `:427`——事件可送达。（`logx_set_error_handler` 只是后续覆盖入口。）
7. **`enc_capable=false` 写出的文件头会不会让读端漏解密？** 不会。三个读端（C++ `decode_one_file:112-113`、C++ `scan_one:469`、Python `logx_decode.py:349/413`）一律按**块头** flag 判定，文件头 flag 仅为元数据。反向竞态见 F-5。
8. **微块跳过是否遗漏其它 `split` 消费者？** 无遗漏。`split_into_blocks` 生产侧唯一调用点是 `drain_once:740`，其余 6 处均在 `tests/`。
9. **`emit_event` 在 `scan_one`/`open_or_create_append_target` 内调用会不会自死锁？** 不会。两处均在 `inst->mu` 之外（`drain_once:736` 已解锁；`run_recovery` 路径不持 `mu`），`emit_event` 用独立 `err_mu`（`logx_internal.h:57-58` 明确记录了不复用 `inst->mu` 的理由）；回调内调 `logx_close` 走递延分支。
10. **`logx_pal.h` 补 `<vector>` / `logx_c.h` close 递延契约 / `ring_scan` 注记三处文档类修复是否落实？** 全部落实且表述准确。`logx_c.h:11-17` 的三条（句柄即失效 / 在途写可能完成 / 需强顺序由调用方先停写）与 `logx_close:455-479` 实现一致；`recovery.cpp:61-68` 的注记准确锁定了「巧合正确」的依赖前提。
11. **`staging.cpp:348-352` 的两段 memcpy（跨回绕写单条记录）是否可达？** 不可达：进入该分支需 `w>0 && w+total>data_size`，而 `:305-333` 的回绕分支已把 `w` 归零。属防御代码，非缺陷（顺带记录，不建议本轮改动）。

---

## 五、验证局限（必须说明）

1. **本轮可实测范围**：Windows / MSVC 14.43 / Release / `LOGX_WITH_ZSTD=ON` + `LOGX_WITH_AESGCM=ON` 全量重建 + 全部测试 + 撤销对照实验。**未复跑**：Android arm64 交叉编译与真机（设备 `d16ced26` 未连接）、另两个构建矩阵（ZSTD/AESGCM 关闭组合）、Python 解码器仅静态核对未运行。
2. **撤销实验覆盖面**：仅对 R20 三项产品修复（A/B/C）与 R19 的 F-10/F-4/F-14 三处做了撤销。**未做**：F-2 递延 close（撤销预期是崩溃而非断言失败，会污染同批其它用例结果，故只做静态判定）、F-1 R19 缝封块本体、Python/Kotlin/JNI 侧全部改动。对这些项的「有效/无效」结论属静态推演，强度低于实验证据。
3. **构建环境更正**（对既有记录）：R19 §六 / R20 §五 / §六 记录的「ninja jobserver-pipe 在沙箱内挂死、`-j1` 同样挂死、须 `ninja -t commands` 逐条直连 cl/lib/link」**本会话未复现**——`MSYS_NO_PATHCONV=1 cmd.exe /c build_tmp.bat` 正常跑完 56 步全量重建。另需注意一个陷阱：Git Bash 下不加 `MSYS_NO_PATHCONV=1` 时 `/c` 会被路径转换成 `C:\`，`cmd.exe` 只打印横幅后以 **rc=0 空转退出**，极易被误读为「构建成功但无输出」或「挂死」。前轮记录的挂死现象有可能源于此。
4. **CHANGELOG 数字口径**：`156/156`、`4/4`、`2/2` 已实测复核为真；「增量重建 49 步」无法在本会话复现（全量 56 步、单文件增量 6~7 步），建议在验证记录里同时写明构建命令与 ninja 版本，否则步数不具备可比性。

---

## 六、修复顺序建议

**第 1 优先（提交阻塞项）——把屏障真正建起来，逐项做撤销对照实验，见红方算完成：**

1. **F-2**：`EXPECT_TRUE(bb.any_micro)` 一行修正（钉死场景）＋ 补落盘层微块用例（§三·1 给了具体编排）。这是 R20 唯一 P0 级修复点，目前零屏障。
2. **F-3**：让 F14 真正进入 rebuilt 路径（破坏双 Header），并断言 `resume_seqno == sr.seqno + 1`；加密 nonce 重用属安全项，不能没有屏障。
3. **F-4**：RECOVERY 断言改为按消息匹配（`EventProbe.msgs` 已在手），顺带删掉重复的第二份断言（N-3）。
4. **F-5**：补有状态 `key_provider` 用例，断言文件头 bit0 为 0 + DEGRADED 消息 + 导出可解。

**第 2 优先（产品代码，均为小改动）：**

5. **F-6**：`open_or_create_append_target:510-514` 补事件（或改走 `scan_one(repair=true)`），消除自愈双站点语义分裂。
6. **F-7**：微块跳过前校验全零；非全零则落盘 + 事件 + 计数。并考虑加一个缝跳过累计计数器。
7. **F-5 反向竞态**：定夺文件头 flag 语义（锁定能力 / 或定义为快照），二选一后同步 spec §3 与 `logx_c.h`。

**第 3 优先（文档收口，本项目惯例）：**

8. **N-6**：计划表补 R19/R20 两个批次的收口行（升版至 v1.30），Backlog 增设 B-7/B-8 收纳 R20 §四 的两项（同日多分卷孤儿块、新文件+孤儿块组合）。
9. **N-1 / N-2**：`key_provider` 契约补第三类调用点；`logx_internal.h:229` 读侧上界注释改 `kBlockMaxRawPlausible`，并为 `block_header_plausible` 补接受域直测（含「`raw_len<6` 必拒」这条 R20 立论所依赖的判据）。
10. **N-4 / N-5 / N-7**：`make_entry` 对齐生产记录布局（cksum 移到 offset 5）并注明合成记录不可解码；F14 历史日期留痕；验证记录补构建命令口径。

---

> **复核声明**：本报告结论基于读码 + 全量重建 + 全量测试复跑 + 6 组撤销对照实验（含 2 组正向对照）。实验改动均以文件备份 `cp` 还原，`git status --porcelain` 复核为空，还原后重建 156/156 复绿；未产生任何提交，未留下任何实验文件。产品代码侧本轮**未发现新的功能性缺陷**，全部发现集中在回归屏障有效性与文档收口。

---

## 七、修复批次收口批注（2026-10-05 同批追加，原文保留）

> 本报告 §六 全部条目已在同一批次内处理完毕（用户拍板：三档全做；F-5 取方案 A「锁定文件级加密能力」；F-6 取「就地补 RECOVERY 事件」；F-7 取「全零校验 + 内部计数器」）。逐条对照：

| 本报告编号 | 处置 | 落点 |
|---|---|---|
| **F-1(P2)** 回归屏障空洞 | ✅ 补齐并**经撤销对照实验证实有效** | 新增 `F1c_WrapSeamAtPendingHead_MicroBlockNeverReachesDisk`（落盘层，独立遍历块链 + `seam_blocks_skipped` 增量判别）；`F1b` 恒真断言改 `EXPECT_TRUE(any_micro)` + 块序列精确形态 `[20486][3][2500]`，改名 `..._SplitPinsSeamOnlyBlock` |
| **F-2(P3)** F1b 测错层次 | ✅ 同上（F1b 重新定位为"切分层场景钉死"，落盘防线交 F1c） | `test_r19_regression.cpp` |
| **F-3(P3)** F14 未走 rebuilt 路径 | ✅ 毒化双 Header（`[0,128)` 写 `0xFF`）真进 rebuilt 分支；块序号改 5/6+撕裂 7；**新增路径判别断言** `flushed_len`（rebuilt=0 / 常规=128）；新增 `F14b` 常规路径孪生 | 同上 |
| **F-4(P3)** F3 事件断言被顶包 | ✅ `EventProbe` 增 `saw_msg_substring`/`count_kind`，改断言站点专属文案 `"truncated to empty for rewrite"` | 同上 |
| **F-5(P3)** enc_capable 无覆盖 + 反向竞态 | ✅ 方案 A 落地：`open_or_create_append_target` 增 `out_enc_capable`（新建=探测结果，已存在=回读头 flag）→ `FlusherCtx::current_enc_capable` → `drain_once` 逐块 `encrypt_enable && 能力`；新增 `F6b`（有状态 provider）；语义写入 spec §3.2 | `flusher.cpp`, `test_r19_regression.cpp`, `docs/LogX日志库最终实施方案.md` |
| **F-6(P3)** 自愈双站点 | ✅ `open_or_create_append_target:510-514` 就地补 `LOGX_EV_RECOVERY`，文案带站点名 `"at append-target open"`。**无专属回归**（该分支需崩溃窗口才可达，公共 API 不可确定性构造，如实记录于 CHANGELOG） | `flusher.cpp` |
| **F-7(P3)** 微块跳过零观测 + 前提未校验 | ✅ 跳过前全零校验（非全零按"宁丢不脏"丢弃 + 发 RECOVERY 事件，绝不落盘）；新增 `Instance::seam_blocks_skipped`（内部 atomic，不进 `logx_stats_t`，零 ABI） | `flusher.cpp`, `logx_internal.h` |
| N-1 key_provider 契约 | ✅ 补第三类调用点（建文件探测，Flusher 线程）+ 指针所有权 + 各调用点频率 | `logx_c.h` |
| N-2 读侧上界注释 / 零直测 | ✅ 注释更正为 `kBlockMaxRawPlausible`(16MB) 并写明"刻意宽于写侧、不同源"；删除失实注释与**死字段 `bytes_since_hint`**（既无生产者也无消费者）；新增读侧接受域 2 项用例 | `logx_internal.h`, `test_r19_regression.cpp` |
| N-3 重复断言 | ✅ 已删 | `test_r19_regression.cpp` |
| N-4 `make_entry` 布局 | ✅ cksum 移到 offset 5，对齐生产 `[len][magic][flags][cksum][payload]` | 同上 |
| N-5 F14 历史日期 | ✅ 就地留痕（R18-1 三条件 (b)(c) 不成立，不受 retention 影响） | 同上 |
| N-6 计划表/Backlog 收口 | ✅ 计划表升 **v1.29**，补记 R19/R20/R21 三批收口行；§4 新增纪律 **R21-1**（撤销对照须配正向对照；"用例是否真走到目标分支"不得凭注释声称）与 **R21-2**（头文件改动必须 clean 全量重建）；Backlog 增设 **B-7/B-8** | `docs/LogX后续实施计划表.md` |
| N-7 验证记录口径 | ✅ CHANGELOG R20 段就地勘误（含 49 步/挂死记录），R21 段写明构建命令与步数；本报告 §五·3 的更正同步追加到 `code_review_R20_2026-10-04.md` §七 | `CHANGELOG.md`, R20 报告 |
| §四 已排除疑点 11 条 | ✅ 全部保留结论；其中第 5 条（ATC offsets 无消费者）在修 N-2 时顺带清理了失实注释与死字段 | `logx_internal.h` |

**附带发现并修复（不在本报告 §一 汇总内）**：`drain_once` 原在循环外算一次 `flag`、缺钥时就地 `flag &= ~kFlagEncrypted`，该变量被**同一轮 drain** 的后续块沿用（含中途轮换到别的文件之后的块），且因加密位已清、守卫条件不再成立，后续块静默降级连 DEGRADED 事件都不发。随 F-5 改为逐块重算，降级只影响当块。

> **本段自我更正**：初稿写的是「一次瞬时缺钥把本进程剩余生命周期内的全部块永久静默降级」——**说过头了**。`flag` 是 `drain_once` 的局部量，泄漏范围止于本轮 drain（约一个 75ms 周期），下一轮会重新探测；不是进程级永久降级。已按实际作用域更正表述（CHANGELOG 同步）。

**逐块重算的副作用与处置**：`key_provider` 调用频次会从「每轮 drain 一次」升到「每块一次」，而 Android 侧它是 JNI trampoline、Flusher 线程每次调用都要 `AttachCurrentThread`/`DetachCurrentThread`（重量级）。已改为按 **(本轮 drain, fd)** 缓存密钥、换文件即失效重取——既保住「降级不跨块泄漏」，又不放大 JNI 调用次数。

**撤销对照实验（R18-2 + 新增 R21-1 正向对照纪律）**：一次构建内同时撤销 4 处修复（`drain_once` 微块跳过 / 能力锁定 / `scan_one` RECOVERY 事件 / rebuilt 分支 `sr.seqno + 1`），结果**恰好** `F1c`、`F6b`、`F3`、`F14` 复红（`F1c` 报 `raw_len actual: 3 vs 6`——3B 微块确实进了文件），其余 9 项 R19Regression 用例与全量 157 项保持绿 ⇒ 这 4 项是各自修复的唯一屏障。正向对照：本报告 §二 实验二已实测撤销 R19 F-10/F-4 分别使 `F10`/`F4` 复红。实验改动以 `cp` 备份还原，`grep -c "R21 EXPERIMENT"` 归零确认无残留。**二次复验**：`key_provider` 缓存改动落地后重跑同组 4 项撤销，结果一致（同 4 项复红、其余 9 项绿）；还原后 `git status` 仅含本批 7 个预期文件，`core/src/recovery.cpp` 无 diff。最终态：clean 全量重建 56 步 + `touch` 全部源与头重建 24 步，MSVC 诊断（`warning C`/`error C`/`error LNK`/`fatal error`）命中 **0**；单测 **161/161**、热路径 2/2、压力 4/4、崩溃 fork 3/3。
