# LogX 代码审查报告 R36 (2026-10-09)

> **审查对象**: 提交 `9c7b1b5`（R35 遗留建议 1/2/4 收口——Wire 编译期 Roundtrip 断言、Clang 警告清理与 NO_SPACE 性能基准对比，计划表 v1.51）及关联纯登记提交 `92e7747`。
> **上一轮**: R35（`2a9565f`，2026-10-08，报告 [code_review_R35_2026-10-08.md](code_review_R35_2026-10-08.md)）。
> **审查方式**: 全量 diff 逐行审读 + MSVC clean 全量重建独立复现 + 四套件实测 + 变异实验（钉住力正向对照）+ Stage D 基准独立快跑。

---

## 一、总体结论

**产品代码零缺陷。** N-1/N-2/建议 4（Stage D）三项收口全部成立，且核心主张（新断言具备钉住力）经变异实验实证。发现 1×P3 + 1×Nit，**均为文档收口层问题，且已在本轮审查中当场修正**（改动在工作区待提交）：

| 编号 | 级别 | 位置 | 摘要 | 状态 |
|---|---|---|---|---|
| D-1 | P3 | 计划表 v1.51 / CHANGELOG 条目 14 | R35 建议②（Android 定期构建冒烟入纪律）未收口且未声明留置；提交信息「建议 1/2/4」编号与 R35 报告 §七原始编号不对应 | ✅ 已当场修正（B-11 补记 + 编号口径注记） |
| D-2 | Nit | `tests/unit/test_format.cpp` 文件尾 | 新增用例后引入 2 个多余空行 | ✅ 已当场修正 |

**修正批次（未提交）**: `CHANGELOG.md`（+1 行口径注记）、`docs/LogX后续实施计划表.md`（v1.51 修订说明追加注记 + Backlog 新增 B-11）、`tests/unit/test_format.cpp`（尾部空行）。提交时建议与 R36 报告同批入库。

---

## 二、逐项发现

### D-1 (P3) R35 建议②未收口、未声明留置，提交信息编号与报告原文不对应

- **位置**: 提交 `9c7b1b5` 标题「R35 遗留建议 1/2/4 收口」；计划表 v1.51 修订说明；CHANGELOG 条目 14；计划表 §4 纪律表。
- **现象**: R35 报告 §七「后续建议」共 3 条：① N-1（wire 断言换 roundtrip）与 N-2（`cleanup_file` 清理）；② 将「Android 侧定期构建冒烟」记入流程纪律；③ bench 补 NO_SPACE 延迟对照。收口批次落地了 N-1、N-2 与建议③（Stage D），但建议②在 §4 纪律表中无对应条目（grep「冒烟」仅命中 v1.50 修订说明与历史条目），v1.51 修订说明亦未声明其留置状态。
- **触发链**: 提交信息按 N-1/N-2/Stage D 自行编号为「1/2/4」，读者以 R35 报告 §七编号对照时无法推断哪条被跳过、为何跳过——正是 R10/R13 以来「逐条对照上轮清单」纪律要防的断链形态（同型缺陷：R20 报告提出 B-7/B-8 纳入 backlog 当时未落，R21 才补记）。
- **后果**: 跨工具链断裂防线（R35 F-2 的根因教训）停留在"纪律候选"口头状态，无跟踪载体；后续批次可能再次在不知情下跳过 NDK 冒烟。
- **修法（已实施）**: ① 计划表 Backlog 补记 **B-11**（记录日期、背景、方案 A 入纪律表 / 方案 B 维持隐性覆盖、决策点，状态待拍板）；② v1.51 修订说明与 CHANGELOG 各补一条「建议编号口径注记」，显式声明编号映射与建议②留置 B-11。
- **需补测试**: 无（文档收口层）。**待用户拍板**: B-11 选方案 A 或 B。

### D-2 (Nit) test_format.cpp 文件尾多余空行

- **位置**: `tests/unit/test_format.cpp` 文件尾（`9c7b1b5` 新增用例后 EOF 为 `}\n\n\n`）。
- **修法（已实施）**: 收敛为 `}\n` 单换行。纯空白改动，无需重建。

---

## 三、已排除疑点（排除项与发现项同等记录）

1. **新 Wire 断言是否真有"钉住力"（R35 N-1 的核心主张）——变异实验实证**：临时将 `make_staging_record_header` 的 payload_len/magic 字节序写反（小端→大端），重建即编译期报红 `logx_format.h(117): error C2338: static_assert failed: 'Staging record header wire-format roundtrip and literal layout check failed'`；还原后重建复绿，目标用例 `Format.StagingRecordHeader_WireFormatMakeParseRoundtrip` 通过，`git status` 确认头文件字节级还原。与旧版自指断言（`kWireOffMagic == 2` 这类常量自比，任何一致的错误布局都能通过）形成有效对照。
2. **`cleanup_file` 删除是否有残留引用**——穷尽扫描：全仓 `grep -r "cleanup_file" --include="*.{cpp,h}"` 零命中（按 R10 门禁标准独立复核，非沿用提交声称）。
3. **`put_u16` 剩余使用是否违背"编码原语统一"主张**——全仓 grep `put_u16`：`format.cpp:133`（文件格式 tag_len 头，非 staging 记录头）、`logx_staging.h`/`flusher.cpp`/`staging.cpp`（staging 文件头/加密块头，另一套 wire 格式）及各测试的头部构造。`format_record`/`format_pad` 的 staging **记录头**路径已全部改走 `make_staging_record_header`（`format.cpp` 中 `put_u16` 仅剩 tag_len 一处），主张成立，CHANGELOG 表述未夸大。
4. **Stage D 两模式 `skip_drop_no_space` 非对称是否正确**——核对 `logx_c.cpp:626-629`：`drop_no_space` 仅在最终 `kFull`（内部重试耗尽或 `staging_wait_ms==0` 立即失败）时计数。模式 1 外部重试期间每次 NO_SPACE 返回都计一次 `drop_no_space`（消息随后重试成功，非真实丢失），跳过正确；模式 2 内部 1000ms 超时才 kFull，计数即真实丢失，不跳过正确。且与阶段 B 先例（`logx_bench.cpp:440`，同样 `skip=true`）一致。
5. **Stage D 两模式重试预算是否对称**——模式 1 至多 500×2ms=1000ms，模式 2 库内 `staging_wait_ms=1000ms`，公平。
6. **`constexpr` roundtrip 在 MSVC 下是否成立**——C++14 起允许 constexpr 函数内写局部数组，本仓标准下合法；clean 全量 58/58 编译通过本身即 `static_assert(StagingHeaderWireRoundtrip::verify())` 在全部包含 TU 求值通过的证明。
7. **「58/58 目标 0 警告」声称**——独立复现：clean 全量重建 `[58/58]`，按精确诊断格式（`warning C[0-9]|error C[0-9]|error LNK|fatal error`，`grep -a` 防 GBK 误判）匹配零命中。目标数 56→58 与提交声称一致。
8. **单测 178/178 口径**——独立复现 178/178 + hotpath 2/2 + stress 4/4 + crash_fork 3/3（crash_fork 为本轮补跑，R35 后首次在本会话验证）。178 = 桌面 177 + 本批新增 1，与真机 178/178 为不同口径（真机恒多 1 个设备专属用例，R33 建立口径），CHANGELOG 表述无误。
9. **Stage D 方法论**——`start_gate` 保证 4 线程同时起跑；`thread_ns[]` 每线程独立 vector 无锁竞争；`actual_total = logs_per_thread * kThreads` 对不整除的 `k_d` 自动收缩并以实际值打印，无虚报。

---

## 四、测试覆盖观察

- 新增运行时用例 `Format.StagingRecordHeader_WireFormatMakeParseRoundtrip` 与编译期断言**采样值不同**（运行时 500/0x7E/120 vs 编译期 0x1234/0x5A/0x0020），互不顶包，构成"编译期钉布局 + 运行时文档化"双保险——符合 R21-1"判别性断言"纪律。
- Stage D 为 bench harness（独立 main，不注册 ctest），不进入单测门禁；其方法论护栏（round_valid + 丢失计数）经独立快跑确认生效（两模式 0 丢失、分布正常打印）。
- 本轮未发现新的覆盖缺口（上述判断均经实际 grep/运行核验，非延续前轮结论）。

---

## 五、验证局限（必须说明）

1. **Android NDK Clang 构建 62/62 与真机复跑未独立复现**——本机无设备接入确认，NDK 侧仅采信提交声称；「0 警告」口径在 NDK 侧的区分（R33 N-1：历史 NDK 有 1 条警告，本批称已清零）未独立验证。
2. **Stage D 性能数字未全量复现**——审查独立快跑（A/B 缩模为 2000/2000）结果：模式 1 16,278 logs/s / mean 210.8µs，模式 2 530,631 logs/s / mean 7.0µs，双模式 0 丢失。方向与量级一致（本机差距约 32x，比声称的 16.5x 更大，机器/负载相关，属正常离散）；16.5x/18.4x/270x 等精确数字仍以提交时环境为准。
3. **ZSTD/加密矩阵未跑**——Stage D 两种模式均为 `kAlgoNone` 且关加密（by design，避免干扰对比）；ZSTD 开启下的 NO_SPACE 退避行为未测。
4. **Python 解码器运行未覆盖**（与本批改动无交集，例行声明）。

---

## 六、收口建议与修复顺序

1. **随批提交**（已就绪）: D-1/D-2 修正改动 + 本报告 + README 索引行，建议单提交入库。
2. **待拍板**: Backlog **B-11**（Android 定期构建冒烟：方案 A 入 §4 纪律表 / 方案 B 维持隐性覆盖）。若采纳方案 A，建议纪律行同时固化「0 警告声称必须区分工具链」口径（R33 N-1 先例）。
3. **流程注记（候选）**: 收口批次提交信息的"建议编号"今后建议直接沿用来源报告的 §编号（或显式给映射），避免本轮 D-1 形态的对照断链复发。
