# logx 代码审查报告 (R18)

> **审查对象**: logx 仓库 `795ec73`（2026-10-02，R17 修复批次提交后的 HEAD；审查前工作树干净）
> **审查日期**: 2026-10-04
> **审查范围**: ① R17 修复（`795ec73`）逐行独立复核 + 反向对照实验；② R17 §六"教训"门禁项全量执行（历史日期测试时序炸弹全仓审计）；③ 当前 HEAD 抽样重审（format/staging/flusher/recovery/logx_c/codec/pal/jni/kotlin）
> **方法**: 沿用项目纪律——修复本体逐行核对 + 实测验证（构建 → 全量测试 → 构造对照实验）
> **本轮落盘**: `core/src/flusher.cpp`（F-1 修复 + N-1 注释）、`tests/unit/test_flusher.cpp`（F-1 回归屏障），二者均经"撤销即红、修复即绿"双向实验验证

---

## 一、总体结论

**R17 F-1 修复属实闭环且为"承载性"修复**（反向撤销后精确复现原故障，见 §二·1）。本轮在其建议的门禁项上执行全仓审计，**未发现第二例同类时序炸弹**；核心产品代码仍保持零 P2 缺陷。

抽样重审新发现 **1×P2（`append_block` 未初始化栈内存落盘）**，属测试专用接口、当前无生产调用方，实证可 100% 复现。

| 编号 | 级别 | 位置 | 一句话 | 状态 |
|---|---|---|---|---|
| F-1 | **P2** | `core/src/flusher.cpp:861-891` `append_block` | `file_salt==nullptr` 且文件已存在时 `salt[8]` 从未初始化，却仍写入块头 `hdr+22` 并参与 nonce 派生 | **本轮已修复 + 回归屏障经双向实验验证** |
| N-1 | Nit | `flusher.cpp:850-851` | 函数注释称"测试 / 上层(导出离线回放)用此接口"，与实际仅测试可见不符 | **本轮已顺手修正** |

---

## 二、R17 修复独立复核（含反向对照实验）

### 1. R17 F-1（`R13_N2` 用例补 `retention_days = 36500`）——**属实闭环，且为承载性修复**

- **逐行核对**：`tests/unit/test_review_p1_p4.cpp:157-165` 补 `cfg.retention_days = 36500;`，注释完整交代了根因链与先例出处（`test_flusher.cpp:414`），断言未放宽。
- **反向对照实验（本轮新增）**：把该行改回 `retention_days = 7` 并重编译运行，精确复现 R17 报告的原始故障：
  ```
  test_review_p1_p4.cpp(270): logx_export(...) Which is: -8    (LOGX_ERR_NOT_FOUND)
  test_review_p1_p4.cpp(271): result.cb_calls Which is: 0
  [  FAILED  ] 1 test
  ```
  改回 `36500` 后同用例通过。**证明该修复是承载性的（非"改了就绿"的掩盖）**，且此故障在今天（2026-10-04，距 20260920 已 14 天）依然会 100% 触发——修复是必要的，不是可选的。
- 实验产物已全部清理，工作树经 `git status --porcelain` 确认 0 行变更。

### 2. R17 §六"教训"门禁项——**全仓审计执行完毕，未发现第二例**

R17 建议："单测中凡手工构造历史日期日志文件/依赖非当日日期的用例，必须显式设 `retention_days = 36500`"。本轮以 `20[0-9]{6}\.log` 全仓检索（45 处命中）逐一核对所属用例的配置与触发路径：

| 文件 | 历史日期用例 | 判定 |
|---|---|---|
| `test_review_p1_p4.cpp:173` | `20260920.log` + `logx_export` | ✅ 已修（R17 F-1） |
| `test_recovery.cpp:70,266,945,949` | `20260831/0901/0902.log` | ✅ 安全。直接构造 `Instance` 调 `run_recovery`，**不启动 Flusher 线程**，无 retention 清理触发路径 |
| `test_codec.cpp:613,718,986` | `20260903/0904.log` | ✅ 安全。同样绕过 `logx_open`，直接用栈上 `Instance` + `write_and_record`，无 Flusher |
| `test_flusher.cpp:124,177` | `20260831/0902.log` | ✅ 安全。同上，`Instance inst{}` 栈对象 + 直调 `write_and_record` |
| `test_robustness.cpp:200,210,252,262` | `20260902~0904.log` | ✅ 安全。走 `MockClockPal` 注入**模拟时钟**，真实 `wallclock_date` 返回 2026-10-04，retention 阈值算的是模拟日期，与 mock 文件日期同源，不构成"过期" |
| `test_flusher.cpp:478-605` | `20250101/20240101/20230501/20200101.log` | ✅ **有意构造过期文件**测 retention 清理本身，日期越老越好，非缺陷 |

**关键区分**（R17 未细化，此处补全）：真正危险的组合是 **① 构造历史日期文件 + ② 经 `logx_open` 启动真实 Flusher + ③ 断言依赖该文件内容**。三者同时成立的用例全仓仅 R13_N2 一例，已修。其余历史日期用例要么绕开 Flusher（直接调 core 函数），要么用 MockClockPal 保持时钟自洽，要么本就在测清理逻辑。

**建议纳入门禁的精确判据**（比 R17 表述更可执行）：
> 当且仅当用例同时满足：构造/依赖 `YYYYMMDD` 为**真实过去日期**、经 `logx_open` 启动真实 Flusher（未经 `MockClockPal` 注入时钟）、且断言依赖该文件内容 → 必须显式设 `retention_days = 36500`。
> 补测建议（防再犯）：加一条"时钟注入"式用例，用 `MockClockPal` 把日期推到 20 年后，验证既有历史文件不因保留期被误删——这比逐条 grep 字面量更能防住变体。

---

## 三、新发现详述

### F-1（P2，本轮已修复）`append_block` 在 `file_salt==nullptr` + 文件已存在时，未初始化栈内存被写入磁盘

- **位置**：`core/src/flusher.cpp:843-891`（`append_block`），缺陷分支在 `:861-871` 与 `:890-891`。
- **代码事实**：
  ```cpp
  uint8_t salt[8];                                    // :861  未初始化
  if (file_salt) {
      std::memcpy(salt, file_salt, 8);
  } else if (sz == 0) {                              // :864  仅"新建文件"才填
      if (!inst->pal->random_bytes(salt, 8)) { ... return false; }
  }
  // sz > 0 且 file_salt == nullptr → salt 全程未赋值
  ...
  BlockWriteResult wr = write_and_record(inst, fd, raw_block, raw_len,
                                         seqno, flag, salt, nullptr);  // :890 传入垃圾
  ```
  `write_and_record → append_block_raw` 在 `flusher.cpp:125` 执行 `std::memcpy(hdr + 22, salt, 8)`，把这份未初始化栈内容写进块头；加密路径下 `make_nonce(seqno, salt, nonce)`（`codec.cpp:56-63`）还会用它派生 GCM nonce。
- **实测复现（构造对照实验，5/5 次稳定复现）**：
  ```
  file hdr salt (truth) : AA BB CC DD EE FF 11 22
  block#0 (seqno=1)  block hdr salt : AA BB CC DD EE FF 11 22   ← 调用方传入，正确
  block#1 (seqno=2)  block hdr salt : AA BB CC DD EE FF 11 22   ← 栈巧合，非保证
  block#2 (seqno=3)  block hdr salt : 98 8C 79 4A F9 7F 00 00   ← 未初始化栈残留
  ```
  同一次运行内 block#1"看起来正确"纯属栈布局巧合，block#2 立即暴露——这正是未初始化读取的典型特征（**不可用"这次看起来对"论证其安全**）。
- **定级 P2 而非 P1 的理由**：`append_block` 的注释与调用面显示它是**测试/离线回放专用**接口——全仓 7 个调用点（`test_recovery.cpp` 5 处 + `logx_internal.h` 声明）**全部传入非空 salt**，生产 Flusher 主循环走的是 `open_or_create_append_target`（`flusher.cpp:442-481`，其 `out_salt` 在两条分支上均被无条件赋值，无此缺陷）。故当前**无生产影响路径**。但缺陷本身客观存在且写盘可观测，一旦将来有"以 nullptr + 已有文件"方式复用该接口（如按文件头回读 salt 的自然需求），将直接落盘栈垃圾。
- **加密场景的次生影响（推断，未单独实证）**：若该路径在加密开启时被复用，写侧 nonce 用"垃圾 salt + seqno"，而读侧 `decode_one_file`（`logx_c.cpp:88`）固定用**文件头 salt**（`hdr+12`）派生 nonce，两者失配 → GCM 认证失败 → 该块永久不可解，构成静默数据丢失。
- **修法（本轮已落盘）**：把"已存在文件"也纳入取值分支，从文件头回读 salt 并与调用方交叉校验，与 `open_or_create_append_target`（`:473-478`）保持同一纪律：
  ```cpp
  } else {
      // R18 F-1: 已存在文件且调用方未给 salt 时, 必须回读文件头 salt 复用。
      if (inst->pal->file_pread(fd, salt, 8, 12) != 8) { inst->pal->file_close(fd); return false; }
  }
  ```
  该改动同时消除了原先"仅 `file_salt` 非空时才校验 salt 一致性"的分支不对称。
- **实测（修复后）**：全量 `logx_unit_tests` **147/147 绿**（146 + 本轮新增 1 条回归屏障）、`logx_hotpath_alloc_tests` **2/2 绿**、`logx_stress_tests` **4/4 绿**（7 处既有 `append_block` 调用点全部走"新建 + 显式 salt"路径，行为不变）。

#### 回归屏障的"空洞绿"陷阱（本轮实际踩中并修正，方法论价值高于缺陷本身）

首版回归测试 `Flush.AppendBlock_NullSaltOnExistingFile_ReusesFileHeaderSalt` **在修复被撤销的情况下依然全绿**——与 R16 F-1（`make_slice` 校验和用加法而非 XOR，导致"测的东西根本没被测到"）同型。根因：

> 上一次 `append_block(&inst, seed_salt, ...)` 的 `memcpy(salt, file_salt, 8)` 恰好把 `seed_salt` 留在**同一栈槽**；下一次调用未初始化的 `salt` 读到的正是这份残留，于是"块头 salt == 文件头 salt"成立——但成立的原因是**栈巧合**，不是修复。

修正：两次 `append_block` 之间插入 `__declspec(noinline) void poison_r18_stack()`，用 4096B `volatile` 数组把该栈区覆写为 `0xCC`，使未初始化 `salt` 必然读到毒值。

**双向验证结果**：

| 状态 | 期望 | 实测 |
|---|---|---|
| 撤销修复 + 毒化测试 | 红 | ✅ `FAILED`（`memcmp` 返回 1，block#1 salt 为 `0xCC…`） |
| 恢复修复 + 毒化测试 | 绿 | ✅ `PASSED` (32 ms) |

> **教训（建议纳入门禁）**：任何针对"未初始化内存"或"值巧合正确"的回归测试，**必须做撤销对照实验**。这类测试的默认状态极易是绿的，绿得毫无意义。判据很硬：写完测试先撤销修复跑一遍，见红才算建立了屏障。

### N-1（Nit，本轮已顺手修正）`append_block` 注释与实际调用面不符

- `flusher.cpp:850-851` 原称"测试 / 上层（**导出离线回放**）用此接口绕过 Flusher 主循环"，但全仓无任何"上层/导出"调用点（见 §三·F-1 调用面统计）。**本轮已随 F-1 一并改为"仅单元测试"**，避免后续维护者误以为存在生产调用方而降低审慎。

---

## 四、已排除疑点

| # | 疑点 | 判定与依据 |
|---|---|---|
| X-1 | R17 F-1 是否"改断言掩盖" | **排除**。反向撤销即复现 `-8`，改回即绿；且本轮未改任何断言 |
| X-2 | 其余历史日期测试是否为潜伏炸弹 | **排除**。见 §二·2 全量 45 处命中逐一核对：或绕开 Flusher，或 MockClockPal 时钟自洽，或本就在测清理 |
| X-3 | `drain_once` 逐块取 key，key 缺失时 `flag` 降级是否跨块泄漏 | **排除**。`flag` 是 drain_once 局部变量，降级后 `key` 保持 `nullptr`，`append_block_raw`（`:96-99`）对每块独立再校验，不存在"一块降级、后续块误加密" |
| X-4 | `ensure_append_target` 切分循环在 `file_size_path` 恒 ≥0 时是否死循环 | **排除**。`current_split` 每轮 +1，抵达 65535 由 `:607-613` 显式返回哨兵并告警；`test_flusher.cpp:1451` 覆盖该边界 |
| X-5 | `decode_one_file` 用文件头 salt 解密，但块头 `hdr+22` 另存一份 salt，二者是否可能失配 | **排除（就现有写入路径）**。`flusher.cpp:724` 与 `open_or_create_append_target` 传的是同一 `c->current_salt`，块头 salt 与文件头 salt 同源；F-1 修复后该不变式在 `append_block` 上同样成立 |
| X-6 | JNI `tag_buf` 的 `GetStringUTFRegion` + `utf8_len` 落 NUL 是否越界 | **排除**。`docs/code_review_supplement.md:60-63` 已纠偏：`utf8_len` 恰为写入字节数，`tag_buf[utf8_len]` 即结束位；且 `logx_jni.cpp:354-356` 双层长度门（`sizeof/3` 与 `sizeof`）确保不越界 |
| X-7 | `commit_header_locked` 双 Header `seq_cst` 屏障是否被 `release` 尾屏障削弱 | **排除**。`staging.cpp:269-271` 前 `seq_cst` 保证 A 先于 B 全局可见，尾 `release` 仅约束后续操作的排序，不改变已建立的 A→B 可见性 |

---

## 五、验证记录

| 项 | 命令/环境 | 结果 |
|---|---|---|
| 构建 | MSVC 14.43 /std:c++17 /utf-8 /O2，`LOGX_WITH_ZSTD=ON`、`LOGX_WITH_AESGCM=ON` | 增量 2 TU（`flusher.cpp` + `test_flusher.cpp`），编译 + 链接零告警 |
| 单测（修复前基线） | `tests\unit\logx_unit_tests.exe` @ `795ec73` | 146/146 PASSED |
| 单测（含 R18 修复与新回归） | 同上 | **147/147 PASSED**（146 + 新增 1 条回归屏障） |
| 热路径 | `tests\unit\logx_hotpath_alloc_tests.exe` | **2/2 PASSED**（常规与截断路径均 0 次堆分配） |
| 压力 | `tests\stress\logx_stress_tests.exe` | **4/4 PASSED**（含 50 轮崩溃注入对账 397/397） |
| 对照实验 A | 撤销 R17 F-1（`retention_days` 改回 7） | 精确复现 `LOGX_ERR_NOT_FOUND(-8)`，1 FAILED → 改回后绿 |
| 对照实验 B | 构造 `append_block(nullptr salt, 已有文件)` | 5/5 次复现块头 salt 为栈垃圾（`98 8C 79 4A F9 7F 00 00`）→ 修复后消除 |
| 对照实验 C | **撤销 R18 F-1 + 毒化栈** | 回归测试 ✅ FAILED（`memcmp`=1，块头 salt 为 `0xCC…`）→ 恢复修复后 ✅ PASSED |
| 门禁审计 | `20[0-9]{6}\.log` 全仓 45 处命中逐一核对 | 无第二例同类时序炸弹 |
| 收尾 | 实验产物（含临时探针二进制/源码）全部清理 | 已清理；`git status` 仅剩本轮 2 个源文件改动 |

> **环境备注**：本轮 `ninja`（jobserver-pipe 版）在当前沙箱下会挂起且零输出（`ninja -n` 干跑与 `ninja -t commands` 均正常，故非构建定义问题）。已改用"导出 vcvars64 环境 → 按 `ninja -t commands` 取证的原始命令行直接调用 `cl.exe` / `lib.exe` / `link.exe`"完成**完全等价**的构建（含 36 个 core 目标文件的归档，库体积与原始一致），不影响验证结论。

---

## 六、修复顺序建议

1. **提交本轮落盘改动**（工作树 2 文件）：
   - `core/src/flusher.cpp`：F-1 修复（已存在文件分支回读文件头 salt）+ N-1 注释更正，建议
     `fix(flusher): append_block 已存在文件分支回读文件头 salt, 杜绝未初始化栈内存落盘 (R18 F-1)`
   - `tests/unit/test_flusher.cpp`：新增回归屏障 `Flush.AppendBlock_NullSaltOnExistingFile_ReusesFileHeaderSalt`，建议
     `test(flusher): 补 append_block nullptr salt 复用文件头 salt 回归屏障 (R18 F-1, 含栈毒化防空洞绿)`
2. 把 §二·2 的**精确判据**（历史日期 + 真实 Flusher + 断言依赖该文件，三者同时成立才需 `retention_days = 36500`）写入项目审查门禁清单，替代 R17 的字面量 grep 判据。
3. 把 §三的**"未初始化内存类测试必须做撤销对照实验"**写入门禁清单（见该节的方法论教训）——这条比任何具体判据都值钱，本轮亲历踩中。
4. 可选补测：按 §二·2 建议加一条"用 `MockClockPal` 把日期推到 20 年后，验证既有历史文件不因保留期被误删"的不误删用例，把时序类风险从"靠 grep 防守"转为"靠用例防守"。
