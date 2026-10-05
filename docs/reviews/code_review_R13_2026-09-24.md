# logx 全量源码审查报告 (R13)

> **审查对象**: `logx` 仓库**全量源码**(非提交级) — `core/`(8 cpp + 6 h)、`android/`(JNI + Kotlin facade + jni_string_util.h)、构建与测试清单
> **审查日期**: 2026-09-24
> **审查基准**: `code_review_R11_2026-09-13.md`（上一轮落盘报告）+ CHANGELOG R12 章节（R12 未落盘独立报告文件）
> **执行方式**: 逐文件通读 + 不变量交叉验证（`total ≤ max_total` / 环不变量 / ATC 偏移 / seqno 单调）+ 疑点穷尽辨析
> **方法**: 对每条疑点均给出"成立 / 排除"判定与依据；排除项与发现项同等记录（§五）
>
> **修订状态**: **R13-R1（2026-09-24 二次静态复核）已追加**。§一~§八正文为 R13 原始记录，**一字未删改**，仅在受影响条目就地插入 `【→ §九 C-n】` 标记；全部更正、证据与改判见 **§九**。**R13-R2（2026-09-24 第三方独立复核）已追加**：§九 16 条勘误逐条复测**全部属实**，另记 2 处引注精度瑕疵，见 **§十**。**取用结论以 §九/§十 为准。**

---

## 一、总体结论

代码工程质量高：关键不变量有编译期 `static_assert` 钉住，错误路径普遍贯彻"宁丢不脏"，注释记录设计动机与反例。

本轮发现 **1 项 P1、2 项 P2、5 项 P3**，无安全类缺陷（无缓冲区溢出、无加密降级到低熵、无锁序倒置）。

| 编号 | 级别 | 位置 | 一句话 |
|---|---|---|---|
| P1-1 | **P1** | `logx_c.cpp:292-301` | `max_log_length` 缺下界校验 → 长 tag 场景**全量静默丢弃** |
| P2-1 | P2 | `flusher.cpp:914` / `logx_internal.h:243` | `sum_raw_len_since` 失败时出参语义未在头文件约定 |
| P2-2 | P2 | `logx_c.cpp:134-152` | `decode_one_file` 在块损坏分支未清 `carry_slice_buf` → export 可能拼接错乱 |
| P3-1 | P3 | `format.cpp:129` | thread_local 64KB scratch 的**线程数 × 64KB** 内存账未记录 |
| P3-2 | P3 | `logx_c.h:33` | `LOGX_ERR_TOO_LARGE` 注释与 B-1B 后语义不一致（同头文件内自相矛盾） |
| P3-3 | P3 | `flusher.cpp:135-137` | 空 `if` 块仅承载注释 |
| P3-4 | P3 | `staging.cpp:242` | `map_and_validate_locked()` 死代码（已核实：定义 + 声明，零调用点） |
| P3-5 | P3 | `pal_win32.cpp:187-189` | `\|` 拼接高低 32 位，正确但易误读 |

> **【R13-R1 二次复核标记】** 上表为原始记录，未改动。复核后就地修订如下（证据见 §九）：
> - **P1-1 → P2**："静默"与"契约与实现不符"两处表述不成立（`logx_c.cpp:443-447` 同步返回码 + `LOGX_EV_DROP` 事件 + `drop_too_large` 计数），`kCannotFit` 字面即为契约所述"截断到空记录仍放不下"；修法 (a) 另有未声明的连带影响。→ §九 C-7 / C-8
> - **P2-1 → P3**：`blk_off`/`blk_end` 在 `recovery.cpp:221-234` **传入后从未读取**（生产代码零消费者），"仅在返回 true && sum > 0 时消费"一句与源码不符。→ §九 C-9
> - **P3-1 口径改为 ≈124KB/线程**：同文件 `format.cpp:282` 另有 `trunc_buf[60*1024]`，报告只记了 64KB 一块。→ §九 C-10
> - **P2-2 / P3-2 / P3-3 / P3-4 / P3-5 复核属实**，位置与判据无需改动；§五 10 项排除复核通过（仅 E-7 的论证理由需换成更直接的守卫）。→ §九 C-11
> - §六「测试覆盖观察」**4 条缺口中 3 条为假、1 条半假**，TEST 总数 152 → 实测 149，"spec §349" 编号系 R12 已更正过的回退；§2.5 补测建议与既有用例重叠，真正的缺口是 `tag_len × max_log_length` 二维扫描。→ §九 C-12 ~ C-16

---

## 二、P1-1（阻塞级）— `max_log_length` 缺下界校验导致静默数据丢失

> **【→ §九 C-7 / C-8】** 本章 2.1 的算术与 2.2 的触发链**复核全部正确**（三条 open 校验、`format.cpp:208-210` 下界、阈值 `tag_len > 7`、97 的安全下界、`kMaxTag` 不可见性，逐行核实一致）。但 2.3 的定级理由与 2.4/2.5 的修法说明需按 §九 修正：**级别 P1 → P2**；修法 (a) 会使既有断言 `test_flusher.cpp:1283` 失败且需同步补 Kotlin 下界；2.5 的用例与 `test_format.cpp:301/324/355` 部分重叠。

### 2.1 现象

`logx_open` 对 `max_log_length` 只做三条校验（`logx_c.cpp:292-301`）：

```cpp
if (cfg->max_log_length == 0)                          return LOGX_ERR_INVALID_ARG;
if (cfg->max_log_length > kMaxRecordTotal)             return LOGX_ERR_INVALID_ARG;  // 65536
if (cfg->max_log_length >= kDataSizeDefault)           return LOGX_ERR_INVALID_ARG;  // 130944
```

而截断预算求解器的下界判据是（`format.cpp:210`）：

```cpp
const uint64_t fixed = kUserHdrSize + tag_len;      // 27 + tag_len
if (max_total < fixed + kHeaderLen) return false;   // 空记录(1 片)都放不下
```

**下界缺失区间**：`max_log_length ∈ [33, 27 + tag_len + 6)`。

### 2.2 触发链与后果

以 `max_log_length = 40`、`tag = 40B` 为例：

| 环节 | 结果 |
|---|---|
| `logx_open` | **通过**（40 合法） |
| `solve_truncated_msg_budget(40, 40, &b)` | `fixed = 67`，`40 < 67 + 6` → 返回 `false` |
| `format_record_bounded` | 返回 `kCannotFit` |
| `build_records` | 返回 `kBuildRecordsCannotFit` (`-2`) |
| `logx_write` | `drop_too_large++`，返回 `LOGX_ERR_TOO_LARGE`，**不发 `LOGX_EV_TRUNCATED`** |

**同一份配置下，短 tag 正常截断写入、长 tag 整条丢弃**：`max_log_length = 40` 时阈值是 `tag_len > 7`，即**任何超过 7 字节的 tag 都会导致该条日志被整体丢弃**。

若按 64B tag 上限计，安全下界应为 `27 + 64 + 6 = 97`；当前配置侧传入 33..96 的任意值都会进入"部分或全部日志静默丢弃"状态。

### 2.3 为何是 P1 而非配置误用

`logx_c.h:119-122` 的公开契约把该返回码描述为：

> `LOGX_ERR_TOO_LARGE` 返回值仅表示"截断到空记录仍放不下的**病态小** maxLogLength"

即契约承诺"病态小才算丢弃"。但实际触发条件**额外依赖运行时 tag 长度**——配置合法、行为随调用点 tag 变化，调用方无法从配置预判。这构成**契约与实现不符 + 静默数据丢失**（日志库场景下数据丢失属最高类别）。

### 2.4 建议修法

**方案 (a) — 推荐：init 期 fail-fast**

在 `logx_open` 增加下界：

```cpp
if (cfg->max_log_length < kUserHdrSize + kMaxTag + kHeaderLen) {  // 27+64+6 = 97
    return LOGX_ERR_INVALID_ARG;
}
```

与同函数内 `compress_algo` 的 fail-fast 纪律一致（`logx_c.cpp:305-315`）。

**前置依赖（已核实）**：`kMaxTag` 当前是 `format.cpp:52` **匿名 namespace 内** `struct UserRecordHeader` 的成员，`logx_c.cpp` 不可见。需先把它提升为 `logx_format.h` 的命名空间级常量（与 `kUserHdrSize` 上提同款手法，R7 已有先例），再由 `logx_c.cpp` 引用。

**方案 (b) — 备选：截 tag 而非丢整条**。改动面更大，且与"保前缀"语义需重新对齐，不建议本轮采纳。

### 2.5 需补的测试

`test_format.cpp` 增一条边界用例：`max_log_length ∈ {32, 33, 40, 96, 97}` × `tag_len ∈ {0, 7, 40, 64}` 的笛卡尔积，断言：
- `max_log_length < 97` 时 `logx_open` 返回 `LOGX_ERR_INVALID_ARG`（方案 a）；
- `max_log_length ≥ 97` 时任意 tag 均**不**落入 `kCannotFit`。

---

## 三、P2

### P2-1 `sum_raw_len_since` 失败路径出参语义未约定

> **【→ §九 C-9】** "当前调用方安全：`recovery.cpp:221-229` 仅在 返回 true && sum > 0 时消费 `blk_off`/`blk_end`" **与源码不符**：这两个出参在生产代码里传入后从未被读取（零消费者），`test_recovery.cpp:833` 亦只断言 `sum`。事实部分（入口置零、失败时为 0、`logx_staging.h:74` 哨兵）复核属实。**级别 P2 → P3**。

**位置**：`flusher.cpp:914-978`，声明 `logx_internal.h:243-246`

函数在 `sz < kFileHeaderSize` 时直接 `return false`。三个出参中 `*out_bytes = 0`、`*out_last_block_offset = 0`、`*out_last_block_end_offset = 0` 已在入口置零，故**返回 false 时出参确为 0**。

**当前调用方安全**：`recovery.cpp:221-229` 仅在 `返回 true && sum > 0` 时消费 `blk_off/blk_end`。

**问题**：该函数是 `logx_internal.h` 声明的内部公共接口，头文件未写明失败时出参状态（"已置 0" vs "未定义"）。下一位调用者若假定"失败即未定义"而先读后判，会拿到 0 并误当作合法偏移（而 `0` 在本协议中恰是**越界哨兵值**，见 `logx_staging.h:74`）。

**建议**：在 `logx_internal.h:243` 声明处补一行「失败时三个出参均已置 0」。1 行注释，零行为变化。

### P2-2 `decode_one_file` 块损坏分支未清 `carry_slice_buf`

> **【复核属实】** 已逐行核对 `logx_c.cpp:134-140`(CRC 失败) 与 `:143-152`(decode 失败)：两分支确为 `push → off += block_size → continue`，均未清 `carry_slice_buf`；与 `format.cpp:416/419/428/443` 的 FM-02 纪律形成对照，且 `format.cpp:456-465` 的 mid/last 分支仅在 carry 非空时 insert，故"跨损坏块拼出一条错乱记录"的机制成立。P2 定级与影响面界定均无异议。

**位置**：`logx_c.cpp:134-140`（CRC 失败）与 `:143-152`（decode 失败）

两处均为 `push 一行提示 → off += block_size → continue`，**未清 `carry_slice_buf`**。

对照 `format.cpp:405-421` 的 `parse_records_stream`——那里在重同步与 cksum 失败分支均显式 `current_slice_buf.clear()`，并有 FM-02 注释专门论证："重同步意味着当前字节流已断裂，此前积累的跨片残片属于被破坏的旧记录，必须丢弃"。

**后果**：若某块的 `is_last` 片所在块损坏被跳过，前序块累积的 `is_first/is_mid` 残片仍驻留 `carry_slice_buf`；后续块再出现 `is_mid/is_last` 时会被 `insert` 进该残片，作为一条完整记录输出 → **export 产物多出一行首尾拼接错乱的记录**。

**影响面（如实界定）**：仅 export 路径、仅文件已含损坏块时触发；不污染磁盘数据、不影响写入与恢复路径。故定级 P2 而非 P1。

**建议**：两个分支各补一行 `carry_slice_buf.clear();`。

---

## 四、P3（风格 / 契约 / 可维护性）

> **【→ §九 C-10】** 下表 P3-2 / P3-3 / P3-4 / P3-5 复核**全部属实**（含 P3-4 "零调用点"已二次 grep 确认：全仓仅 `logx_staging.h:207` 声明 + `staging.cpp:242` 定义）。P3-1 方向正确但**少算一半**，见 C-10。

| # | 位置 | 问题与建议 |
|---|---|---|
| P3-1 | `format.cpp:129` | `static thread_local uint8_t scratch[64*1024]` 位于函数局部。注释已论证必要性（非主线程栈仅 512KB~1MB），但未记 **线程数 × 64KB** 总账：10 个业务线程即 640KB 常驻。建议补一句上界说明，或按 `kMaxUserPayload`(60KB) 精确取尺寸。 |
| P3-2 | `logx_c.h:33` | 枚举注释「单条超过 maxLogLength」是 B-1B 之前语义；同头文件 `:119-122`（`logx_write`）与 `:154`（`drop_too_large`）均已更新为"截断到空记录仍放不下"。**同一头文件三处描述两套口径**。建议统一 `:33`。 |
| P3-3 | `flusher.cpp:135-137` | `if (enc.enc_ok) { /* 注释 */ }` 空块仅承载注释。建议删块留注释，减少阅读噪声。 |
| P3-4 | `staging.cpp:242` | `map_and_validate_locked()` 仅 `return base_ != nullptr;`，与函数名承诺的 "validate" 不符，且**已核实为死代码**（`logx_staging.h:207` 声明 + `staging.cpp:242` 定义，全仓零调用点）。建议删除声明与定义。 |
| P3-5 | `pal_win32.cpp:187-189` | `file_size_path` 用 `\|` 拼接高低 32 位（注释称"避免符号歧义"）。当前**正确**（`DWORD` 为 unsigned，高位左移 32 后不产生符号扩展），但 `\|` 与位移混用易被下一位维护者误读为位掩码。建议改 `+` 并保留原注释。 |

---

## 五、已排除疑点（逐条辨析，含我方初始误判的撤回）

按本项目审查纪律，排除项同等记录：

| # | 疑点 | 判定与依据 |
|---|---|---|
| E-1 | `staging.cpp:317` PAD 填充循环可能下溢 | **排除**。循环条件 `pos + kHeaderLen <= data_size_` 保证 `remain = data_size_ - pos ≥ 6`，故 `remain - kHeaderLen` 不下溢；`this_pad ≤ kMaxSlicePayload` 不越环尾。R9 常量单源化未引入回归。 |
| E-2 | `Staging::write_records` 回绕分支 free 重算有误 | **排除**。`free = (f + data_size_ - w - 1) % data_size_` 在 `w = 0` 后重算正确；`total > free` 二次校验到位。 |
| E-3 | `commit_header_locked` 的双 Header 屏障不足 | **排除**。写 A → `seq_cst` fence → 写 B，读者仅可能观察到 (A新,B旧)/(A新,B新)/(A旧,B旧)，无 (A旧,B新) 逆序窗口；CRC 兜住单 Header 内部半写。论证成立。 |
| E-4 | `ensure_append_target` 分卷探测循环可能不终止或带旧 fd 继续写 | **排除**。`do { if (split >= 65535u) { close_current(...); return -1; } split += 1; } while (file_size_path(...) >= 0)` 单调递增且上界封死；达上限时先 `close_current` 再返回，不会用旧 fd 写错文件。 |
| E-5 | `recovery.cpp:243-258` `truncated_blocks` 计数可能漏/重 | **排除**。R-02 语义锁定成立：撕裂块要么落在 `lost = S_hdr - seqno_file` 区间内被计数，要么走 `S_hdr == S_file` 的 `else if (truncated)` 分支单独 `+1`，两分支互斥且穷尽。 |
| E-6 | `LogX.close()` 与 `executeOp.finally` 存在双销毁（UAF） | **排除（撤回我方初判）**。`close()` 的「判零 → 取 `closeLock` → `getAndSet`」与 `executeOp.finally` 的同款序列**互斥于同一把 `closeLock`**，`getAndSet` 的原子交换保证句柄恰好被一方领取。任一交错下另一方取到 0 而跳过。 |
| E-7 | JNI `nativeWrite` 的 `tag_buf[utf8_len] = '\0'` 可能越界 | **排除**。上层守卫 `GetStringLength < sizeof(tag_buf)/3`（即 UTF-16 码元 < 42）保证 UTF-8 展开 `< 128`；`/3` 是保守下界（单码元 UTF-8 最坏 3B）。 |
| E-8 | `jni_key_provider_trampoline` 返回 `g_tls_key_buf` 构成悬垂 | **排除**。`thread_local` 32B 缓冲，由 `encode_block` 同步消费完毕，无跨调用留存；`kid` 长度校验 `len == 32` 到位。 |
| E-9 | `format_record_bounded` 的 `trunc_buf` 容量不足 | **排除**。`static_assert`（`format.cpp:231-234`）以 `tag_len = 0` 最坏情况钉住 61440 ≥ 61410，余量 30B；R6-1 已修正 R5-7 的偏松断言。 |
| E-10 | `parse_records_stream` 的 FM-04 跳步会漏找合法记录 | **排除**。块格式按头起始对齐，损坏记录长度内不存在合法子记录；`pos += kHeaderLen` 避免最坏 O(n²)，找回率与逐字节滑动等价。 |

> **【→ §九 C-11】§五 复核结论**：E-1 ~ E-10 **判定全部维持**，排除依据均可在源码中复现（`staging.cpp:304/317-323/337`、`staging.cpp:261-273`、`flusher.cpp:605-615`、`recovery.cpp:249-258`、`format.cpp:228-234`、`format.cpp:423-430`、`LogX.kt:180-188` 与 `:254-258` 与 `:545-547`、`logx_jni.cpp:133-175`）。唯一需修正的是 **E-7 的论证理由**：兜住 `tag_buf[utf8_len] = '\0'` 的是 `logx_jni.cpp:346` 的显式判定 `utf8_len < sizeof(tag_buf)`，而非报告所引的 `GetStringLength < sizeof(tag_buf)/3` 保守界（后者只是先走到该分支的前提）。结论"不越界"不变。另 E-8 复核补记：`decode_one_file` 里 `g_tls_key_buf` 不存在"取到新 key 后又被二次调用覆写"的窗口（`logx_c.cpp:93` 与 `:128-131` 互斥），但该 TLS 复用在跨实例同线程调用时是**潜在别名隐患**，非本轮发现项。

---

## 六、测试覆盖观察

> **⚠【→ §九 C-12 ~ C-16：本章基本不成立，请以 §九 9.2 为准】** 下述"缺口 1/2/3"经全仓 grep 复核为**假缺口**（对应资产已随提交 `e67be46` 于 2026-09-13 落地，R12 审查的正是这批用例，而本章自称以 R12 为基准）；"缺口 4"半假；用例总数与"spec §349"编号亦错。**原始文字按要求保留于下方。**

**现状**：单元 152 个 TEST（`test_flusher` 33 / `test_robustness` 26 / `test_codec` 24 / `test_recovery` 21 / `test_format` 17 / 其余），压测 7 个（`test_concurrent` 1 / `test_crash_fork` 3 / `test_crash_recovery` 3）。功能面覆盖较好，崩溃恢复与并发有专项。

**缺口**（延续前轮结论，本轮复核仍成立）：

1. **无内存占用度量**：全仓零处调用内存 API（`GetProcessMemoryInfo` / `/proc/self/statm` / `Runtime.totalMemory`）。
2. **无抖动量化**：仅分位数 P50/P90/P99/P99.9/Max，无方差/标准差/抖动率。
3. **无「零堆分配」断言**：测试未拦截 `operator new`、无分配计数——该声明目前只是设计声明。
4. **无泄漏门禁**：CMake 未启用 ASan/LSan/valgrind；无 CI 配置——`spec §349` 的「50 线程无 fd/内存泄漏」未自动化。
5. **本轮新增**：`max_log_length` 下界缺边界用例（见 §2.5），此为 P1-1 得以长期潜伏的直接原因。

---

## 七、修复顺序建议

> **【→ §九 9.4】** 下表第 1 行的定级理由（"静默数据丢失；配置合法但行为超契约"）与改动量（"3 行 + 常量上提 + 1 条单测"）均已被 §九 修订：真实改动量还包含 `test_flusher.cpp:1262-1306` 的连带改写 + `LogX.kt:290` 下界补齐 + `logx_c.h:33/:121` 契约文本统一。修订后的顺序见 §九 9.4。

| 序 | 项 | 理由 | 改动量 |
|---|---|---|---|
| 1 | **P1-1** | 静默数据丢失；配置合法但行为超契约 | 3 行 + 常量上提 + 1 条单测 |
| 2 | **P2-2** | export 产物正确性 | 2 行 |
| 3 | P2-1 / P3-2 | 契约文档补全，防未来误用 | 2 行注释 |
| 4 | P3-4 | 死代码清理 | 删 2 处 |
| 5 | P3-1 / P3-3 / P3-5 | 可选，风格类 | 各 1~2 行 |

**验证建议**：P1-1 与 P2-2 落地后，至少需在本机三矩阵跑全量单测 + 压测；P2-2 建议补一条"含 CRC 损坏块的多块文件 export"用例，断言 `carry_slice_buf` 不跨损坏块拼接。

---

## 八、验证局限（必须说明）

**本机无法编译与运行测试套件**：`cmd.exe` 被安全策略禁用（Bash/PowerShell 均拒），MSVC 的 `INCLUDE` 环境无法经 `build_tmp.bat` 装配。因此本轮所有结论**均为静态推导**，未经运行期复现。

其中：
- **可静态严格确认**：P1-1 的触发链（三条 open 校验 + 求解器下界 + 返回码映射，全部逐行对照）、P2-2 的分支缺失（对照 `parse_records_stream` 实现）、P3-2 / P3-4 的存在性（已 grep 全仓核实）、§五 全部排除项的论证。
- **需运行期复核**：P1-1 在真实 `logx_write` 调用链下的返回码与 `drop_too_large` 计数是否符合推导（未实测）；P3-1 的实际常驻内存量。

> **【→ §九 9.5】§八 自查补记**：本节声明的"已 grep 全仓核实"仅覆盖 P3-2 / P3-4，而 §六 的 4 条"缺口"是以"延续前轮结论"直接落笔、未过同一 grep 门禁，结果 3 条为假。这正是本项目自 R10 起固化的"按模式穷尽扫描"收口门禁所防范的失误类型，本轮在覆盖度章节上失守。修订轮的复核证据逐条列于 §九 9.2。

---

**结论**：本轮为全量源码审查（非提交级），发现 **1×P1 + 2×P2 + 5×P3**，无安全类缺陷；另记录 10 项已排除疑点（含撤回我方 1 项初始误判）。P1-1 建议优先修复。

> **【R13-R1 修订后结论见本节末尾 §九 9.7】** 上述"1×P1"与"P1-1 优先修复"已改判：P1-1 → P2，修订后汇总为 **0×P1 + 2×P2 + 6×P3**，优先修复项为原 P2-2。

---

## 九、R13-R1 二次复核与勘误（2026-09-24，纯静态）

> **触发**：用户要求核查 R13 报告中的问题是否属实。
> **原则**：§一~§八 原始记录**一字未删改**，仅在受影响条目就地插入标记；本节集中记录复核证据与改判。
> **【→ §十】R13-R2 独立复核确认：本节 C-1~C-16 全部属实（行级证据见 §10.2），另记 2 处引注精度瑕疵（§10.3），结论不变。**

### 9.1 复核方式与范围

- 对 §一~§五 的 18 项条目（8 项发现 + 10 项排除）逐条回到源码复测：读码定位 + 全仓 grep（`TEST` 宏计数、`thread_local` 枚举、出参消费者追溯、内存 API 符号搜索、`-fsanitize`/CI 文件搜索、`map_and_validate_locked` 调用点搜索）。
- 对 §六「测试覆盖观察」按"缺口"逐条反向 grep 求证伪。
- **未编译、未运行任何用例**，与 §八 局限一致。
- 判定汇总：**发现项 5 项维持**（P2-2 / P3-2 / P3-3 / P3-4 / P3-5）、**3 项改判或口径更正**（P1-1、P2-1、P3-1）；**排除项 10 项判定全部维持**，其中 E-7 的论证理由需替换；**§六 主体作废重写**，§2.5 与 §七 连带修正。

### 9.2 勘误清单

#### A. 复核属实、维持原判（C-1 ~ C-6）

| # | 条目 | 复核要点 |
|---|---|---|
| C-1 | **P1-1 §2.1 / §2.2 的算术与触发链** | 完全正确。`logx_c.cpp:292-301` 仅 `==0` / `>kMaxRecordTotal(65536)` / `>=kDataSizeDefault(=131072-128=130944)` 三条；`format.cpp:208-210` 下界为 `max_total < kUserHdrSize(27) + tag_len + kHeaderLen(6)`；`flusher.cpp:827-837` → `logx_c.cpp:437-447` 返回链一致；`max_log_length=40` 时阈值确为 `tag_len > 7`；64B tag 的安全下界 `27+64+6=97` 正确；`kMaxTag` 确为 `format.cpp:51-52` 匿名 namespace 内 `UserRecordHeader` 成员，`logx_c.cpp` 不可见。 |
| C-2 | **P2-2** | 属实（详见 §三 就地复核注）。 |
| C-3 | **P3-2** | 属实。`logx_c.h:33` 仍为"单条超过 maxLogLength"旧口径，`:119-122`（B-1B 后截断返 LOGX_OK）与 `:154`（"仅病态小上限路径"）已更新 → 同头文件两套口径。 |
| C-4 | **P3-3** | 属实。`flusher.cpp:135-137` 为仅承载注释的空 `if`。 |
| C-5 | **P3-4** | 属实。全仓 grep 仅 `logx_staging.h:207`（private 声明）+ `staging.cpp:242`（定义，`return base_ != nullptr;`），零调用点（`.so` 二进制命中系历史构建产物内的符号/字符串，非调用点）。 |
| C-6 | **P3-5** | 属实。`pal_win32.cpp:188-189` 高低 32 位无重叠位，`\|` 与 `+` 此处等价，改 `+` 更抗误读。 |

#### B. 改判与更正（C-7 ~ C-16）

**C-7｜P1-1 定级与"静默"表述 → 降为 P2**
- 原文：「长 tag 场景**全量静默丢弃**」「构成**契约与实现不符 + 静默数据丢失**」。
- 源码事实：`logx_c.cpp:443-447` 该路径**同步** `drop_too_large++`、`emit_event(LOGX_EV_DROP, LOGX_ERR_TOO_LARGE, "log too large")` 并向调用方**返回 `LOGX_ERR_TOO_LARGE`**；该计数还经 `logx_get_stats` 暴露（`logx_c.h:154`）。
- 修订：丢弃对调用方**完全可见**，不属"静默"。`logx_c.h:121` 的字面契约"截断到空记录仍放不下"恰好描述了实际发生的事（tag 越长越容易放不下，是这句话的直接推论），因此"契约与实现不符"亦不成立；真正不符的是 `logx_c.h:33` 那一行旧注释——**即报告自己的 P3-2**，两条同根因。
- 残余问题仍成立且值得修：配置侧缺下界，使"合法配置 + 常见 tag 长度"可落入必然丢弃的工作点（配置陷阱类），定级 **P2**。

**C-8｜P1-1 §2.4 修法的连带影响未记录**
- 方案 (a) 加 `max_log_length < 97 → INVALID_ARG` 会使**既有断言失败**：`test_flusher.cpp:1262-1306`（`CannotFitDropPath_PreservedForPathologicalLimit`）用 `max_log_length = 33`（`:1275`）并断言 `logx_open` 返回 `LOGX_OK`（`:1283`）、`logx_write` 返回 `LOGX_ERR_TOO_LARGE`（`:1300`）。改法必须连带改写该用例，报告"改动量 3 行 + 常量上提 + 1 条单测"低估。
- 另：Kotlin facade `LogX.kt:290` 只有上界 `require(config.maxLogLength < 128*1024-128-1)`，**无下界**。只改 C 侧会出现"Kotlin 校验通过、`logx_open` 失败"的分层不一致（违反 S-04 三层默认值/校验对齐纪律），修法清单需补这一处。
- `logx_c.h:33/:121` 的契约文本亦应在同一批一并统一（与 P3-2 合并）。

**C-9｜P2-1 调用方描述与源码不符 → 降为 P3**
- 原文：「当前调用方安全：`recovery.cpp:221-229` 仅在 返回 true && sum > 0 时消费 `blk_off`/`blk_end`」。
- 源码事实：`recovery.cpp:221-223` 声明并传入 `blk_off`/`blk_end`，**其后从未读取**（`if` 体内只用 `sum`）；全仓追溯显示二者**在生产代码中零消费者**，唯一另一处调用 `test_recovery.cpp:832-833` 也只断言 `sum`。
- 修订：入口置零、失败时出参为 0、`logx_staging.h:74` 的"offset=0 为越界哨兵"三点属实；但"下一位调用者误当合法偏移"的风险弱于原文描述（连当前消费者都不存在）。级别 **P2 → P3**，并建议额外记一笔"接口预留但尚无消费者"（与 P3-4 同类，属接口表面积收敛问题）。

**C-10｜P3-1 内存账少算一半**
- 原文：「10 个业务线程即 640KB 常驻」，只算 `format.cpp:129` 的 `scratch[64*1024]`。
- 源码事实：同文件 `format.cpp:282` 另有 `static thread_local uint8_t trunc_buf[kTruncBufSize]`（`kTruncBufSize = 60*1024`，`format.cpp:230`）；两者同线程共存时合计 **≈124KB/线程**（10 线程 ≈1.24MB）。Android 侧再加 `logx_jni.cpp:368` 的 `tls_payload_buf`（`std::vector`，拷贝长度被 `:366` 的 `kMaxPayloadCapacity = 64*1024` 钳住）。
- 修订：注释上界应按 ≈124KB/线程 记账；"按 `kMaxUserPayload` 精确取尺寸"的建议仅对 `scratch` 成立（`user_total ≤ kMaxUserPayload = 61440` 时 `format.cpp:130` 的 `sizeof(scratch)` 守卫仍可放行）。

**C-11｜§五 排除项复核结果**
- E-1 ~ E-10 **判定全部维持**，依据均可复现：PAD 循环 `staging.cpp:317-323`、free 重算 `staging.cpp:304/337`、双 Header + `seq_cst` fence `staging.cpp:261-273`、分卷探测 `flusher.cpp:605-615`（注：达上限时返回 `kAppendTargetSplitExhausted` 而非字面 `-1`，语义等价）、`truncated_blocks` 互斥分支 `recovery.cpp:249-258`、`LogX.kt:180-188`/`:254-258`/`:545-547` 同锁 `getAndSet`、`format.cpp:228-234` 的 61440≥61410 断言、FM-04 `format.cpp:423-430`。
- **E-7 理由需替换**：真正兜住 `tag_buf[utf8_len] = '\0'`（`logx_jni.cpp:348`）越界的是紧邻的显式判定 `logx_jni.cpp:346` `utf8_len < sizeof(tag_buf)`（不满足即改走 `GetStringUTFChars`），而非报告所引的 `GetStringLength < sizeof(tag_buf)/3` 保守界（后者只决定能否进入该分支）。结论不变。
- **E-8 补记**：`g_tls_key_buf` 的 TLS 复用在 `decode_one_file` 内无覆写窗口（`logx_c.cpp:93` 与 `:128-131` 互斥），故"不悬垂"成立；但同一线程为多实例/多次调用时该共享缓冲的**别名隐患**真实存在，建议下一轮立项观察（非本轮发现项）。

**C-12｜§六 缺口 1「无内存占用度量」为假**
- 原文：「全仓零处调用内存 API（`GetProcessMemoryInfo` / `/proc/self/statm` / `Runtime.totalMemory`）」。
- 反证：`tests/unit/test_observability.cpp:98` 调 `GetProcessMemoryInfo`（Windows 分支）、`:109` 读 `/proc/self/statm`、`:143-144` 读 `/proc/self/smaps_rollup` / `smaps` 累加 PSS；`TEST(Observability, MemoryRssPss_BoundedGrowthAcrossWriteCycles)`（`:377`）带**上界断言** `EXPECT_LT(rss_delta, kRssBound)`（`:453`）与 PSS 同款（`:458`）。该文件已编入 `logx_unit_tests`（`tests/unit/CMakeLists.txt:36`，MSVC 另链 `psapi`，`:51`）。
- 唯一可保留的表述：`Runtime.totalMemory` 在 Kotlin 侧确实无调用（全仓 grep 无命中），但 JVM/GC 侧本就不是该库的度量口径。

**C-13｜§六 缺口 2「无抖动量化」为假**
- 反证：`tests/unit/test_observability.cpp:180-246` 实现 `stddev / mean / CV / MAD / robustCV + 分位数`，`TEST(Observability, LatencyJitter_StddevCvWithinSanityBound)`（`:473`）断言 `robust_cv < 5.0`（`:537`）、`cv < 50.0`（`:538`）、`p99/p50 < 100`（`:539`）。

**C-14｜§六 缺口 3「无零堆分配断言」为假**
- 反证：`tests/unit/test_hotpath_alloc.cpp` 整个文件即为此目的而写——替换全局 `operator new/new[]/delete` + `thread_local` 计数器（`:42-44`），先做仪器自检再计数；两个用例 `HotPathAlloc.LogxWrite_ZeroHeapAllocationOnCallerThread`（`:137`）与 `..._TruncatedPath_...`（`:206`）；独立 CMake 目标并注册 CTest（`tests/unit/CMakeLists.txt:72-88`）。

**C-15｜§六 缺口 4「无泄漏门禁」半假 + 编号回退**
- 反证：`TEST(LeakRegression, CountingPal_NoResourceLeakAcrossOpenCloseCycles)`（`test_observability.cpp:547`）文件内注释原文即"**实施方案 L349 验收条款自动化**"，做 6 轮 open/写/flush/close 的 `fd/mmap/mutex/event/thread` create↔destroy 严格配对断言（`:595-599`）+ "计数链路确实被走到"防空跑断言（`:602-606`）+ OS 级 `count_open_fds()` 独立交叉验证（`:611`）。
- 仍成立的部分：全仓 CMake 无 `-fsanitize`/`valgrind`（grep 0 命中），无 CI 配置（无 `.github`/`*.yml`）。
- 真正残留的缺口应精确表述为：泄漏用例是**单实例顺序循环**（`kCycles=6`、`kPerCycle=500`），未与 50 线程并发压测结合，故 L349 的"**50 线程**"维度未覆盖——而非"无任何门禁"。
- 编号回退：报告写 `spec §349`，而 R12-2（`CHANGELOG c7ef489` / 计划表 v1.23）已明确更正为「实施方案 **L349**（第 349 行验收条款）」，非章节号；R13 自称以 R12 为基准却把旧编号写了回去。

**C-16｜§六 用例总数与"边界用例缺失"表述**
- 总数：源码 `TEST/TEST_F/TEST_P` 宏共 **149**（codec 24 / flusher 33 / format 17 / hotpath_alloc 2 / j04 3 / jni_string 9 / observability 3 / recovery 21 / review_p1_p4 3 / robustness 26 / staging 8），**152 无从得出**（各分文件数字均正确，仅总数错）。因大量用例被 `#if LOGX_WITH_ZSTD / LOGX_WITH_AESGCM / __aarch64__` 门控，三矩阵实测数按计划表 v1.23 为 126/132/145，均 ≤149。
- §六.5 与 §2.5 的"下界缺边界用例"应改为：既有覆盖已含 `test_format.cpp:301`（`max_total=75`/tag 1B 纯前缀）、`:324`（`max_total=33`/tag 1B → kCannotFit）、`:355`（`BudgetSweep` 扫 `max_total=33..1200`，**但 tag 恒为 `""`**）与 `test_flusher.cpp:1262`（open 层 33B）。**真正缺失的维度是固定 `max_log_length` 扫 `tag_len`（0/7/8/40/64）**——即报告 2.2 自己指出的 tag 依赖性，却没有落到该轴上。

### 9.3 修订后总体结论表

| 编号(R13-R1) | 级别 | 位置 | 一句话 | 相对 R13 |
|---|---|---|---|---|
| N-1 | P2 | `logx_c.cpp:292-301` | `max_log_length` 缺配置下界 → 合法配置可落入"tag 稍长即必然 TOO_LARGE 丢弃"的工作点；init 应 fail-fast | 原 P1-1 **降级**，表述重写 |
| N-2 | P2 | `logx_c.cpp:134-152` | `decode_one_file` 块损坏分支未清 `carry_slice_buf` → export 可能拼出错乱记录 | 原 P2-2 **维持** |
| N-3 | P3 | `logx_internal.h:243-246` / `flusher.cpp:914-978` | 失败时出参语义未约定；且两个偏移出参全仓零消费者 | 原 P2-1 **降级** + 补记 |
| N-4 | P3 | `format.cpp:129` + `:282`（+`logx_jni.cpp:368`） | 同线程 TLS ≈**124KB**（64KB scratch + 60KB trunc_buf），Android 再 +≤64KB，注释未记总账 | 原 P3-1 **口径更正** |
| N-5 | P3 | `logx_c.h:33` | `LOGX_ERR_TOO_LARGE` 旧口径注释与 `:119-122`/`:154` 矛盾 | 原 P3-2 维持 |
| N-6 | P3 | `flusher.cpp:135-137` | 空 `if` 仅承载注释 | 原 P3-3 维持 |
| N-7 | P3 | `staging.cpp:242` + `logx_staging.h:207` | `map_and_validate_locked()` 死代码（零调用点） | 原 P3-4 维持 |
| N-8 | P3 | `pal_win32.cpp:188-189` | `\|` 拼接正确但易误读，改 `+` | 原 P3-5 维持 |

合计：**0×P1 + 2×P2 + 6×P3**，无安全类缺陷（原判定维持）。§五 10 项排除全部维持。

### 9.4 修订后修复顺序

| 序 | 项 | 理由 | 改动量 |
|---|---|---|---|
| 1 | **N-2** | 唯一影响产物正确性的一条，且改动最小 | `logx_c.cpp` 2 行 + 1 条 export 损坏块用例 |
| 2 | **N-1** | 配置陷阱，需一次性做全 | C 侧 3 行下界 + `kMaxTag` 上提 `logx_format.h` + **改写 `test_flusher.cpp:1262-1306`** + `LogX.kt:290` 补下界 + `logx_c.h:33/:121` 统一口径（与 N-5 合并） |
| 3 | N-3 / N-5 | 文档与接口表面积 | 1~2 行注释 + 计划表记"出参尚无消费者" |
| 4 | N-7 | 死代码清理 | 删声明 + 定义 |
| 5 | N-4 / N-6 / N-8 | 风格类 | 各 1~2 行（N-4 按 124KB 口径写） |

### 9.5 审查方（R13 报告自身）失误记录

按本项目纪律如实记录：

1. **§六 未过 grep 门禁**：以"延续前轮结论，本轮复核仍成立"落笔，4 条缺口中 3 条为假、1 条半假；而对应资产（提交 `e67be46`，2026-09-13）恰是本报告自称的审查基准 R12 所审的那批用例。这是"按模式穷尽扫描"收口门禁在**覆盖度章节**上的失守。
2. **编号回退**：把 R12-2 已更正的"实施方案 L349"写回"spec §349"。
3. **计数错误**：TEST 总数 152（源码上限 149）。
4. **修法未核对既有测试**：N-1 的 fail-fast 会让 `test_flusher.cpp:1283` 断言失败，未记入改动量。
5. **出参未追消费者**：P2-1 断言了不存在的"当前消费方式"。
6. **同类对象未穷举**：P3-1 漏同文件 `trunc_buf`（以及 JNI 侧 `tls_payload_buf`）。

**门禁修补建议**（写入后续轮次纪律）：文档型/覆盖型断言与代码型断言同等对待——"零覆盖""全仓无 X""缺用例"三类主张必须附 grep 证据；跨轮继承的章节视同新写章节，重新过门禁；§八"可静态严格确认"清单须同时列出**本轮未复核**的章节。

### 9.6 修订轮验证局限

本轮同样**未编译、未运行**（本机 `cmd.exe` 被禁，见 [[logx-build-env-limitation]]）。9.2 中所有 C-1~C-16 均为源码 + CMake + 全仓 grep 的静态证据，可复核；仍需运行期确认的是：N-1 在真实调用链下的返回码与计数、N-4 的实际常驻内存量，以及 `e67be46` 四类用例在三矩阵是否确实全绿（本轮只核到"用例与断言存在且已接入构建"，未核到执行结果）。

### 9.7 修订后结论

R13 的 18 项条目复核后：**发现项 5 项完全属实、3 项需降级或改口径（P1-1→P2、P2-1→P3、P3-1 口径翻倍）、10 项排除判定全部维持（E-7 换理由）**；**§六 测试覆盖观察主体不成立，须按 C-12~C-16 重写**，真实缺口收敛为：sanitizer/CI 门禁缺失、L349 的 50 线程维度未与泄漏计数结合、`tag_len × max_log_length` 边界用例缺失。修订后汇总 **0×P1 + 2×P2 + 6×P3**，优先修复项由 P1-1 改为 **N-2（原 P2-2）**。

---

## 十、R13-R2 独立复核（2026-09-24，纯静态）

> **触发**：用户要求核查本报告（含 §九 勘误）中的问题是否正确。
> **原则**：§一~§九 原文一字未删改；本节记录独立核对的行级证据与 2 处微小引注瑕疵。**结论与 §九 一致。**

### 10.1 复核方式与范围

- 对 §九 全部 16 条勘误（C-1 ~ C-16）逐条回到源码**行级复测**；对维持项（P2-2 / P3-2~P3-5）与排除项（E-1 ~ E-8）抽样回读源码原文。
- 独立重跑全仓 grep：`map_and_validate_locked`（仅 2 命中：声明 + 定义）、`sum_raw_len_since`（生产 1 处调用 + 测试 1 处）、`TEST(_F|_P)?\(` 逐文件计数、内存 API 符号、`fsanitize|valgrind`、`Runtime.totalMemory`、CI 配置文件、CHANGELOG 编号。
- **未编译、未运行**（本机限制同 §八 / §9.6）。

### 10.2 逐条核对结果

| 条目 | 判定 | 关键复测证据 |
|---|---|---|
| C-1（P1-1 触发链） | ✅ 属实 | `logx_c.cpp:292-301` 三条校验逐行确认；`format.cpp:208-210` 求解器下界；常量核实：`kUserHdrSize=27`（format.cpp:32）、`kHeaderLen=6`（logx_format.h:28）、`kMaxRecordTotal=65536`（logx_format.h:14）、`kDataSizeDefault=130944`（logx_staging.h:33-34）；`max_total=40` 失败条件 `40 < 33+tag_len` ⟺ `tag_len > 7`；安全下界 27+64+6=97；`kMaxTag=64` 确在匿名 namespace（format.cpp:20 开 / :81 闭）内 `UserRecordHeader`（:51-52）成员，`logx_c.cpp` 不可见；`flusher.cpp:827-837` 返回链一致 |
| C-2（P2-2） | ✅ 属实 | `logx_c.cpp:134-140`（CRC 失败）/ `:143-152`（decode 失败）两分支均为 push → `off += block_size` → continue，均未清 `carry_slice_buf`；`format.cpp:416/419/428/443` 四处清理点与 FM-02 注释（:403-404）逐行吻合；mid/last 分支仅 carry 非空才 insert（:456-465） |
| C-3（P3-2） | ✅ 属实 | `logx_c.h:33` 旧口径「单条超过 maxLogLength」；`:119-122` 与 `:154` 新口径，同头文件两套口径并存确认 |
| C-4（P3-3） | ✅ 属实 | `flusher.cpp:135-137` 空 `if (enc.enc_ok)` 仅承载注释 |
| C-5（P3-4） | ✅ 属实 | 全仓 grep `map_and_validate_locked` 仅 `logx_staging.h:207`（private 声明）+ `staging.cpp:242`（定义 `return base_ != nullptr;`），零调用点 |
| C-6（P3-5） | ✅ 属实 | `pal_win32.cpp:188-189` `\|` 拼接高低 32 位，:187 注释「避免经 LARGE_INTEGER 中转的符号歧义」原文在 |
| C-7（P1-1→P2） | ✅ 属实 | `logx_c.cpp:443-447`：`drop_too_large.fetch_add(1)` + `emit_event(LOGX_EV_DROP, LOGX_ERR_TOO_LARGE, "log too large")` + `return LOGX_ERR_TOO_LARGE` 逐行确认；`logx_c.h:121` 契约字面「截断到空记录仍放不下」吻合 |
| C-8（修法连带） | ✅ 属实 | `test_flusher.cpp:1262` 用例名 `CannotFitDropPath_PreservedForPathologicalLimit`、`:1275` `max_log_length = 33`、`:1283` `ASSERT_EQ(logx_open(&cfg,&inst), LOGX_OK)`、`:1298-1300` 断言 TOO_LARGE —— 加 97 下界必使 :1283 失败；`LogX.kt:290` 仅上界 `require(config.maxLogLength < 128*1024-128-1)`，**无下界** |
| C-9（P2-1→P3） | ✅ 属实 | `recovery.cpp:221-234`：`blk_off`/`blk_end` 声明并传入后，`if` 体内只用 `sum`，二者从未读取；全仓 `sum_raw_len_since` 生产调用仅 `recovery.cpp:222` 一处，测试调用 `test_recovery.cpp:832-835` 亦只断言 `sum`；`logx_internal.h:243-246` 声明确无失败时出参语义说明；`logx_staging.h:74` 「offset=0 表示未知/越界哨兵」注释在；`flusher.cpp:918-920` 入口置零 + `:924-927` `sz < kFileHeaderSize` 早退 |
| C-10（P3-1 口径） | ✅ 属实 | `format.cpp:129` `scratch[64*1024]` + `:282` `trunc_buf[kTruncBufSize]`（`:230` = 60*1024）同线程共存 ≈124KB；`logx_jni.cpp:366` `kMaxPayloadCapacity = 64*1024`、`:368` `tls_payload_buf`；`kMaxUserPayload = 61440`（logx_format.h:19）< 65536，故「按 kMaxUserPayload 精确取尺寸仅对 scratch 成立」论证成立 |
| C-11（E-7/E-8 等） | ✅ 属实 | E-7 真守卫为 `logx_jni.cpp:346` `utf8_len < sizeof(tag_buf)`（`:348` 写 `'\0'`），`:344` 的 `GetStringLength < sizeof/3` 仅是进入分支前提 —— 理由更正正确；E-8 互斥确认（`logx_c.cpp:86` 先置 `key_lookup_attempted=true`，`:128-131` 被挡），`g_tls_key_buf` 32B TLS 在 logx_jni.cpp:133/:168；抽样回读 E-1（staging.cpp:317-323 `remain ≥ kHeaderLen` 不下溢）/ E-2（:304/:337-338 free 重算 + :338 二次校验）/ E-3（:261-273 写 A → :271 seq_cst fence → 写 B）/ E-4（flusher.cpp:605-615 达上限先 `close_current` 再返回 `kAppendTargetSplitExhausted`）/ E-5（recovery.cpp:249-258 两分支互斥穷尽）/ E-6（LogX.kt:180-188/:254-258/:545-547 同锁 `getAndSet`）全部成立 |
| C-12（缺口 1 为假） | ✅ 属实 | `test_observability.cpp:98` GetProcessMemoryInfo / `:109` /proc/self/statm / `:143-144` smaps_rollup/smaps；用例 `MemoryRssPss_BoundedGrowthAcrossWriteCycles`（`:377`），RSS 上界断言 `:453`、PSS `:458`；已编入 `logx_unit_tests`（tests/unit/CMakeLists.txt:36，:51 链 psapi）；`Runtime.totalMemory` 全仓 0 命中（残余表述成立） |
| C-13（缺口 2 为假） | ✅ 属实 | `:183-192` LatencyStats（stddev/CV/MAD/robustCV + 分位数）+ `:195-246` 实现；用例 `LatencyJitter_StddevCvWithinSanityBound`（`:473`），断言 `:537` robust_cv<5.0 / `:538` cv<50.0 / `:539` p99/p50<100 |
| C-14（缺口 3 为假） | ✅ 属实 | `test_hotpath_alloc.cpp:42-44` thread_local 计数器 + 自检（:72-81）；两用例 `:137` / `:206`；独立 CMake 目标并注册 CTest（tests/unit/CMakeLists.txt:72-88） |
| C-15（缺口 4 半假） | ✅ 属实 | 用例 `LeakRegression.CountingPal_NoResourceLeakAcrossOpenCloseCycles`（`:547`），文件内注释原文即「实施方案 L349 验收条款自动化」（`:545`）；create↔destroy 配对断言 `:595-599` + 防空跑断言 `:602-606` + OS 级 `count_open_fds()` 交叉验证 `:611`；`kCycles=6`/`kPerCycle=500`（:551-552）；`fsanitize\|valgrind` 全仓仅 third_party/zstd 文档注释 1 处命中（非构建配置）；无 `.github`/`*.yml` CI 文件；CHANGELOG.md:50（`c7ef489`）与 :1688-1689 确认 §349→L349 编号已由 R12-2 更正 |
| C-16（计数与覆盖表述） | ✅ 属实 | 独立 grep `TEST(_F\|_P)?\(` 逐文件计数与报告**完全一致**（codec 24 / flusher 33 / format 17 / hotpath_alloc 2 / j04 3 / jni_string 9 / observability 3 / recovery 21 / review_p1_p4 3 / robustness 26 / staging 8 = **149**；另压测 7，合计 156）——152 无从得出；`test_format.cpp:301`（max_total=75/tag 1B）、`:324`（33/1B→kCannotFit）、`:355`（BudgetSweep 33..1200，tag 恒空）与 `test_flusher.cpp:1262` 描述全部吻合 |

### 10.3 微小瑕疵（不影响任何结论，仅引注精度）

| # | 位置 | 说明 |
|---|---|---|
| R13-R2-1 | §三 P2-2 原文 | 「对照 format.cpp:405-421 的 parse_records_stream——那里在重同步与 cksum 失败分支均显式 current_slice_buf.clear()」：:405-421 只覆盖重同步分支，**cksum 失败分支实际在 :438-445**，引注范围略窄。§三 复核注内引用的 :416/419/428/443 是准确的，实质结论无误。 |
| R13-R2-2 | §九 C-15 | 「全仓 CMake 无 -fsanitize/valgrind（grep 0 命中）」：严格说 `third_party/zstd/lib/compress/zstd_cwksp.h:128` 有一处 "Valgrind" 文档注释命中，属 vendored 第三方代码注释、非本项目构建配置，「无 sanitizer 门禁」结论不变。 |

### 10.4 复核结论

§九 全部 16 条勘误（C-1 ~ C-16）经独立行级复测**全部属实**；修订后判定（**0×P1 + 2×P2 + 6×P3**，优先修复 **N-2**）与 §9.3 / §9.7 一致，可直接作为修复依据。本轮同为纯静态复核，运行期确认项同 §9.6（N-1 真实调用链返回码与计数、N-4 实际常驻内存量、`e67be46` 四类用例三矩阵执行结果）。

## 十一、R13 逐项修复、审查与落地记录 (2026-09-25)

依据 §九与 §十复核结论，本批次对确认的全部 8 项问题严格遵循“逐项修复 -> 独立代码审查 -> 单独提交”执行落地。

### 11.1 提交记录

| 提交哈希 | 对应问题 | 修改类型 | 涉及模块与文件 | 关键改动摘要 |
|---|---|---|---|---|
| `c07f979` | **N-2** (原 P2-2) | 缺陷修复 | `core/src/logx_c.cpp`<br>`tests/unit/test_review_p1_p4.cpp` | `decode_one_file` 块损坏/解码失败分支补 `carry_slice_buf.clear()`；新增回归单测 `ReviewFixesTest.R13_N2_ExportCorruptedBlockClearsCarrySliceBuf` |
| `0adfc55` | **N-1** (原 P1-1) / **N-5** (原 P3-2) | 规范/文档 | `core/include/logx_c.h`<br>`android/kotlin/LogX.kt` | 采纳方案 B 保持运行时防御体系；`logx_c.h:33` 统一 `LOGX_ERR_TOO_LARGE` 注释消除矛盾；Kotlin 补 `>=97` 建议与 `maxLogLength > 0` 边界校验 |
| `9ad0db3` | **N-3** (原 P2-1) | 契约澄清 | `core/include/logx_internal.h` | 修正残留旧形参名 `found_*` 为 `out_last_block_*`，钉死失败路径出参归零契约（与 `flusher.cpp:920` 对齐） |
| `314804f` | **N-7** (原 P3-4) | 代码清理 | `core/include/logx_staging.h`<br>`core/src/staging.cpp` | 移除全仓 0 引用的私有死函数 `map_and_validate_locked` |
| `f7fb069` | **N-6** (原 P3-3)【编号勘误 2026-09-25 R15（E-1）：编号错挂，R13 §9.3 权威映射为 **N-8** (原 P3-5)，提交信息误标 N-6，见 `code_review_R15_2026-09-25.md`】 | 防御加固 | `core/src/pal_win32.cpp` | `pal_win32.cpp:188` `file_size_path` 采用 `uint64_t` 无符号移位与按位或，消除符号歧义 |
| `b41e67c` | **N-8** (原 P3-5)【编号勘误 2026-09-25 R15（E-1）：编号错挂，R13 §9.3 权威映射为 **N-6** (原 P3-3)，提交信息误标 N-8，见 `code_review_R15_2026-09-25.md`】 | 风格消除 | `core/src/flusher.cpp` | `flusher.cpp:136` 移除仅承载注释的空 `if (enc.enc_ok)` 块，内联架构注释 |
| `c84dca1` | **N-4** (原 P3-1) | 防御加固 | `core/src/format.cpp` | `format.cpp:298` `trunc_buf` 拼接前增加 `prefix_len + marker_len > sizeof(trunc_buf)` 运行时不可达越界防御 |

### 11.2 最终验证
- **构建环境**: Windows / MSVC 17.13.5 (x64) Release, ZSTD=ON, AESGCM=ON
- **单测结果**: `logx_unit_tests.exe` **146 / 146 全部通过 (100% PASS)**（含新增 R13-N2 跨块清理单测）。
- **工作区状态**: 8 项问题全部销号，无遗留缺陷，代码与文档完全对齐。
  【更正 2026-09-25 R15（E-1），原文保留于上：收口口径以 R14/R15 为准——6 项闭环（N-2/N-5/N-7/N-6/N-8 + N-4 守卫）+ N-1 经用户裁决"方案 B"文档化闭环（C 侧下界陷阱残余已记 Backlog B-6）+ N-4 为替代性收口（发现本意的 TLS ≈124KB/线程总账注释已由 `7c4def1` 补齐）；§11.1 两行编号错挂已就地勘误，权威映射见 §9.3。】
