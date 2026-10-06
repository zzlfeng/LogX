# logx 代码审查报告 (R27) —— R26 批次 (B-9 根除与方案 A+B 落地) 独立复核

> **复核对象**: R26 批次提交（`48875dd` 核心修复 + `c3d0feb` 批次收口文档 + `5df4f9b` 真机随行记录），审查基线 `11fef10`。
> **复核日期**: 2026-10-06
> **复核方法**:
> ① 源码与契约逐行审读：`core/include/logx_staging.h`、`core/src/flusher.cpp`、`decoder/logx_decode.py`、`docs/LogX日志库最终实施方案.md`、`tests/unit/test_r19_regression.cpp` 全量 diff；
> ② 读端四端兼容性独立核对：`block_header_plausible`、`zstd` 解压上限、Python 解码器 `MAX_BLOCK_RAW`、`parse_records_stream` 块尾消费；
> ③ 本地全量构建与实测验证：MSVC Release 全开矩阵（ZSTD=ON + AESGCM=ON）全量重建 56/56 步（ninja 退出码 0，诊断 0）、单测 **163/163 全绿**、压测 **4/4**、真实进程强杀恢复 **3/3**、热路径零堆分配 **2/2**（0 次/0B）、Python 解码器编译与命令行冒烟；
> ④ **独立撤销对照实验（R18-2 反空洞绿门禁）**：单撤销内层探测（F9 报红）、单撤销方案 A（F1b 报红）、双撤销方案 B 前置消费+下游拦截（F1c 与 F7 双红）；实验后源码 SHA-256 字节级还原（`0F715FACE6BC6E8EA4068CC084AD3AAF4F3606D93F66448679EA34273C619BED`）；
> ⑤ 文档交叉引用与一致性核对：`CHANGELOG.md`、`docs/LogX后续实施计划表.md`、`docs/reviews/README.md`。

---

## 一、总裁定

**R26 批次技术实现完全成立，B-9 核心缺陷与历史微块问题得到实质性根除。**
方案 A+B 架构组合在切分层和落盘层构建了严密的协同与纵深，反空洞绿撤销实验实证了所有新增与改写用例的防护有效性。本轮审查发现 1 项文档不同步缺陷（P3）及 3 项观察/建议（Nit）。

| 审查维度 | 声明内容 | 独立核验方式 | 裁定 |
|---|---|---|---|
| **方案 A** | 上限放宽至 20491，缝并入块尾，F1b 型微块在切分层根除 | 源码审读 + 撤销实验 + 读端四端推演 | ✅ **完全成立** |
| **方案 B** | 窗口头全零缝在 copy_pending 前置消费，F1c 型微块根除 | 锁内控制流推演 + 双撤销实验 | ✅ **完全成立** |
| **B-9 修正** | 内层对称脏缝探测，杜绝脏字节与后续记录融合落盘 | 逻辑推演 + 撤销实验（blocks.size 1 vs 3） | ✅ **完全成立** |
| **纵深防御** | 保留 drain_once 微块拦截作为脏缝残片处置路径（修正 R24） | 架构边界推演 + 双撤销对照实测 | ✅ **完全成立，修正必要且精准** |
| **进度环会计** | 前置消费计入 `flushed_bytes_total`，杜绝 flush_blocking 超时 | 跨函数计数器追踪 + 阻塞等待推演 | ✅ **完全成立，缺陷发现真实** |
| **读端四端同步** | plausible / zstd / python decoder / format 块尾 break | 跨模块源码对照 + python 编译冒烟 | ✅ **完全成立** |
| **全量验证** | 单测 163/163 + 压测 4/4 + crash 3/3 + hotpath 2/2 全绿 | 本机工具链实际运行（17 套件，退出码 0） | ✅ **逐字验证，实测吻合** |
| **文档同步** | 计划表 v1.34 与 CHANGELOG 记录一致 | 跨文档逐行比对 | ⚠️ **存在不同步（F-1）** |

---

## 二、发现汇总

| 编号 | 级别 | 类型 | 一句话描述 |
|---|---|---|---|
| **F-1** | **P3** | 文档不同步 / 占位符残留 | `docs/LogX后续实施计划表.md` 滞后于 `CHANGELOG.md`：提交 `5df4f9b` 已完成 Redmi K30 Pro 真机全量复跑（单测 164/164 全绿），但计划表 v1.34 仍标注「*Android 真机复跑待下批随行*」；批次表第 160 行状态列残留「`⚠️ 收口行由 docs 提交补记哈希`」占位符，未填入真实提交哈希 `48875dd`。 |
| **N-1** | Nit | 测试覆盖精细度 | `tests/unit/test_r19_regression.cpp` 的 `ReadSideAcceptanceDomain_RawLenBounds` 钉死上限仍使用旧值 `kBlockMaxRaw`（20486），建议追加写侧新上限 `kBlockMaxRawWithSeam`（20491）的读侧收容断言，使契约边界更加显式。 |
| **N-2** | Nit | 叙述口径澄清 | 提交信息与 CHANGELOG 称「`split_impl` 三处吸收点统一放宽」，代码物理上为 2 处（L109 窗口尾 + L121 缝封块）；「内层纯零缝」与「16KB 边界缝」在控制流上合并为 L121 同一判定点。概念清晰，仅行文粒度有轻微合并。 |
| **N-3** | Nit | 架构演进跟踪 | R24 §7.2 条 4 方案 C（`ActiveFileTarget` 聚合，收编 `key_provider` 5 处调用点）保持待决策 Backlog，未引入代码耦合，符合小步演进策略。 |

---

## 三、核心机制独立推演与审查

### 3.1 方案 A：缝并入块尾（F1b 型微块根除）

1. **单源派生**：
   [`core/include/logx_staging.h:236`](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_staging.h#L236) 定义：
   ```cpp
   constexpr uint32_t kBlockMaxRawWithSeam = kBlockMaxRaw + (kHeaderLen - 1);
   ```
   `kBlockMaxRaw` 为 20486B（20KB 载荷 + 6B 记录头），留零缝最大为 5B（`pad < kHeaderLen`），故上限恰为 **20491B**。通过派生表达式避免了魔法数字硬编码，与写侧留零几何定义严格自洽。
2. **切块层放宽**：
   [`core/src/flusher.cpp:109`](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L109) 与 [`L121`](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L121) 将判伪上界从 `kBlockMaxRaw` 放宽至 `kBlockMaxRawWithSeam`。满长记录紧随 1~5B 缝时，`cur + seam - blk_begin <= 20491` 成立，缝被无条件吸纳至当块末尾。
3. **读端四端兼容性核验**：
   - **端 1 (`block_header_plausible`)**：[`flusher.cpp:358`](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L358) 上界为 `kBlockMaxRawPlausible = 16MB`，20491B 远在其安全接受域内。
   - **端 2 (`codec.cpp`)**：[`codec.cpp:43`](file:///d:/Project/Android_Project/LogX/logx/core/src/codec.cpp#L43) 的 `kDecompressOutputMax = kBlockMaxRaw + 64 = 20550 >= 20491`，解压预分配缓冲足以承载，不会触发坏块拒绝。
   - **端 3 (`decoder/logx_decode.py`)**：[`logx_decode.py:66`](file:///d:/Project/Android_Project/LogX/logx/decoder/logx_decode.py#L66) 同步更新为 `MAX_BLOCK_RAW = 20 * 1024 + 6 + 5 = 20491`。R24 曾遗漏此端，R26 补齐核验消除了 Python 解码端 F-05 严检拒绝合法 20491B 块的严重隐患。
   - **端 4 (`format.cpp`)**：[`format.cpp:396-399`](file:///d:/Project/Android_Project/LogX/logx/core/src/format.cpp#L396-L399) 在 `recs_len - pos < kHeaderLen` 时执行原生 break，缝零字节被静默忽略，且**不触发 FM-02 重同步、不清空 `carry_slice_buf`**，彻底消除了伪微块对跨块分片记录的连带误杀。

### 3.2 方案 B：窗口头全零缝前置消费（F1c 型微块根除）

1. **几何条件与触发窗口**：
   [`core/src/flusher.cpp:781-802`](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L781-L802) 在 `copy_pending` 前检测：
   ```cpp
   bytes = inst->staging->snapshot_pending(&f, &snap);
   if (bytes > 0 && inst->staging->data_size() - f < kHeaderLen) { ... }
   ```
   缝几何上仅存在于环尾 `[data_size - k, data_size)`，因此 `data_size - f < 6` **当且仅当 `f` 恰好停在回绕留零缝起点**。
2. **锁内原子操作与双 Header 提交**：
   在持有 `inst->mu` 锁内，校验区间全零后执行 `zero_range_locked(f, seam_len)` 并通过 `commit_header_locked(..., 0, ...)` 直接将 `f` 置 0。由于在锁内完成，并发 `logx_write` 无法插入；二次执行 `snapshot_pending(&f, &snap)` 时，`f` 已经对齐环首 `0`，待刷数据直接从真实记录头开始，缝字节永远不会拷入 `scratch`。
3. **进度环会计闭环（R26 关键修复）**：
   `logx_write` 回绕写入时，`staged_bytes_total` 累加的 `consumed = pad + total` 已经包含缝长度 `pad`（[`logx_c.cpp:540`](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp#L540) 与 [`staging.cpp:308`](file:///d:/Project/Android_Project/LogX/logx/core/src/staging.cpp#L308)）。
   若前置消费不递增 `flushed_bytes_total`，`flush_blocking` 会因 `flushed < target` 空转轮询至 5 秒超时并报错 `LOGX_ERR_TIMEOUT`。R26 在前置消费处补上：
   ```cpp
   inst->flushed_bytes_total.fetch_add(seam_len, std::memory_order_relaxed);
   ```
   使写端与刷端进度代理完全闭合，逻辑严密。

### 3.3 B-9 修复：流中部脏缝对称探测

1. **缺陷根因复盘**：
   R25 仅在窗口头路径（`first == 0`）探测脏缝，而在内层累计循环中，当 `rec == 0` 且 `probe_wrap_seam == 0` 时，直接走撕裂尾兜底 `cur += min(remain, allowed)`，将 `[100记录][3B脏缝][50记录]` 融合成单块 8103B/10903B 落盘。
2. **R26 修复机制**：
   [`core/src/flusher.cpp:131-140`](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L131-L140)：
   ```cpp
   uint32_t corrupt_seam = 0;
   for (uint32_t s = 1; s < kHeaderLen && cur + s < total; ++s) {
       if (peek_record_len(raw, cur + s, total) > 0) {
           corrupt_seam = s;
           break;
       }
   }
   if (corrupt_seam > 0) {
       break;  // 封 [blk_begin, cur); 外层窗口头路径处置缝残片
   }
   ```
   - 控制流追踪：内层发现 `cur + s` 紧随合法记录头，立即 break；
   - 此时 `cur > blk_begin`，外层将 `[blk_begin, cur)` 正常封块（前 100 条记录独立成块）；
   - 下一轮外层循环从 `pos = cur` 开始，此时正好落在脏缝起始处，自然进入窗口头路径（`first == 0`）；
   - 窗口头路径将 `s` 字节脏缝封为独立微块 `{pos, s}`，并推进 `pos += s`；
   - 下一轮循环继续处理缝后的 50 条记录；
   - 在 `drain_once` 循环中，`raw_len = s < 6` 的微块进入拦截分支，校验发现非全零，触发 `LOGX_EV_RECOVERY` 事件丢弃，不予落盘。
3. **架构评价**：内层并不直接制造微块，而是通过 break 将脏缝推演到外层窗口头路径，复用了 R25 F-2 的成熟拦截管道，避免了在内层重复编写微块处理逻辑。

### 3.4 纵深防御：对 R24 §7.2 字面清单的纠偏

R24 报告曾建议「方案 A+B 落地后整体拆除 `drain_once` 微块拦截/全零校验/计数/RECOVERY 分支」。
R26 在落地时识别出该建议的盲区：**脏缝（位翻转残片）仍需通过微块形式交由下游丢弃**。若彻底拆除下游拦截，脏缝微块将直接写盘，引发读侧 `block_header_plausible` 截断。
R26 选择保留下游拦截分支，并将其职责明确收敛为「脏缝残片兜底处置」，常规全零缝则由 A+B 从源头消除。这一纠偏体现了高度的工程审慎性。

---

## 四、撤销对照实验独立复测实录（R18-2 门禁）

为防止「空洞绿」并验证测试屏障的刚性，本审查在本地 MSVC Release 环境下独立执行了三组代码撤销实验：

### 4.1 实验 1：单撤销内层脏缝探测（验证 F9 刚性）

- **操作**：将 [`flusher.cpp:137`](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L137) 改为 `if (false && corrupt_seam > 0)`。
- **实测结果**：15 项用例恰好 **14 绿 / 1 红**，唯一失败即 `F9`：
  ```text
  D:\Project\Android_Project\LogX\logx\tests\unit\test_r19_regression.cpp(326): error: Expected equality of these values:
    blocks.size()
      Which is: 1
    3u
      Which is: 3
  [  FAILED  ] R19Regression.F9_MidStreamCorruptSeam_NeverFusedIntoBlock (0 ms)
  ```
  *证明：撤销探测后流中部脏缝确实发生恶性融合，`F9` 精准捕获该退化，屏障有效。*

### 4.2 实验 2：单撤销方案 A 上限放宽（验证 F1b 刚性）

- **操作**：恢复探测，将 [`flusher.cpp:121`](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L121) 放宽条件改回 `kBlockMaxRaw`（20486）。
- **实测结果**：15 项用例恰好 **14 绿 / 1 红**，唯一失败即 `F1b`：
  ```text
  D:\Project\Android_Project\LogX\logx\tests\unit\test_r19_regression.cpp(283): error: Value of: bb.any_micro
    Actual: true
  Expected: false
  R26 B-9: seam must be absorbed into block tail, no 1~5B seam-only block
  D:\Project\Android_Project\LogX\logx\tests\unit\test_r19_regression.cpp(286): error: Expected equality of these values:
    blocks.size()
      Which is: 3
    2u
      Which is: 2
  [  FAILED  ] R19Regression.F1b_SeamAfterFullLengthRecord_SeamAbsorbedIntoBlockTail (0 ms)
  ```
  *证明：撤销放宽后切分层重新暴露出微块，`F1b` 的反转断言精准拦截。*

### 4.3 实验 3：双撤销方案 B 前置消费 + 下游微块拦截（验证纵深防御必要性）

- **操作**：将 [`flusher.cpp:782`](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L782)（前置消费）与 [`L864`](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp#L864)（下游拦截）同时置为 `if (false && ...)`。
- **实测结果**：15 项用例恰好 **13 绿 / 2 红**，`F1c` 与 `F7` 同时报红：
  ```text
  test_r19_regression.cpp(418): error: Expected equality of these values:
    ii->seam_blocks_skipped.load() - skipped_before
      Which is: 0
    1u
  test_r19_regression.cpp(430): error: Expected: (b.raw_len) >= (kHeaderLen), actual: 3 vs 6
  micro-block reached disk: read side rejects raw_len<kHeaderLen
  [  FAILED  ] R19Regression.F1c_WrapSeamAtPendingHead_MicroBlockNeverReachesDisk

  test_r19_regression.cpp(1117): error: Value of: probe.saw_msg_substring("sub-header block is not seam padding; dropped")
    Actual: false
  Expected: true
  [  FAILED  ] R19Regression.F7_MicroBlockNotSeamPadding_DropsAndEmitsRecoveryEvent
  ```
  *证明：若照单全收 R24 建议拆除下游拦截，微块确定性落盘（`raw_len: 3 vs 6`），且 RECOVERY 报警丢失。*

### 4.4 还原校验

实验结束后恢复 [`core/src/flusher.cpp`](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp)，执行哈希校验：
- 恢复后 SHA-256：`0F715FACE6BC6E8EA4068CC084AD3AAF4F3606D93F66448679EA34273C619BED`（与提交 `48875dd` 逐字节一致）；
- `git status` 干净；
- 重建复跑 `logx_unit_tests.exe`：**163/163 全绿**。

---

## 五、全量实测验证记录

| 目标二进制 | 测试集 | 用例数 / 状态 | 关键指标 / 观测门禁 |
|---|---|---|---|
| `logx_unit_tests.exe` | 全量单元测试套件（17 套件） | **163 / 163 全绿** | 耗时 13.1s；F1b/F1c/F7/F9 缝四件套全部通过；RSS Δ=4096B，6 轮 open/close 资源净 0 |
| `logx_hotpath_alloc_tests.exe` | 热路径堆分配守护 | **2 / 2 全绿** | 常规写入 400 条 + 截断写入 100 条，调用线程堆分配 **0 次 / 0 B** |
| `logx_crash_fork_tests.exe` | 进程强杀恢复自愈 | **3 / 3 全绿** | 真实进程 fork + 50 轮循环自愈零丢失 |
| `logx_stress_tests.exe` | 50 线程并发压测 | **4 / 4 全绿** | 50 Worker 并发 20675 条日志，Missing=0，440 块 seqno 严格单调；崩溃循环 50 轮 397/397 挽回 |
| `logx_decode.py` | Python 离线解码器 | **编译与 CLI 通过** | `py_compile` 零错误，CLI 帮助与参数解析正常，常量 20491 对齐 |
| *Redmi K30 Pro (真机)* | *随行真机验证 (5df4f9b)* | *164/164 全绿* | *含硬件 ArmCe 用例；缝四件套真机全绿；压测/强杀/零分配通过（引用 CHANGELOG 记录）* |

---

## 六、整改与收口建议

针对本次审查发现的文档与测试精细度问题，提出以下收口动作：

1. **修正计划表与 CHANGELOG 状态不同步（针对 F-1，P3）**：
   - 更新 [`docs/LogX后续实施计划表.md`](file:///d:/Project/Android_Project/LogX/logx/docs/LogX后续实施计划表.md)：
     - 将头部 v1.34 修订说明末尾的「Android 真机复跑待下批随行」修正为已随 `5df4f9b` 闭环；
     - 将批次表第 160 行 R26 状态栏的占位符「`⚠️ 收口行由 docs 提交补记哈希`」更新为真实哈希 `48875dd`，并删除「Android 真机复跑待下批」；
     - 将 Backlog B-9 节末尾的「Android 真机复跑待下批随行」同步闭环。
2. **测试用例边界对齐加固（针对 N-1，建议后续批次合入）**：
   - 在 [`tests/unit/test_r19_regression.cpp`](file:///d:/Project/Android_Project/LogX/logx/tests/unit/test_r19_regression.cpp) 的 `ReadSideAcceptanceDomain_RawLenBounds` 用例中，追加对 `kBlockMaxRawWithSeam`（20491）的 `block_header_plausible` 显式判定。
3. **审查报告索引登记**：
   - 按纪律在 [`docs/reviews/README.md`](file:///d:/Project/Android_Project/LogX/logx/docs/reviews/README.md) 中登记本报告 `code_review_R27_2026-10-06.md`。
