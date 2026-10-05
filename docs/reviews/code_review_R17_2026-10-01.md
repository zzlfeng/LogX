# logx 代码审查报告 (R17)

> **审查对象**: logx 仓库 `d80cd9d`（2026-10-01，R16 修复批次 5 提交后的 HEAD；工作树除本轮测试修复外干净）
> **审查日期**: 2026-10-01
> **审查范围**: ① R16 修复批次（`151294a`…`d80cd9d`）逐提交独立复核（不沿用 R16 结论）；② 当前 HEAD 抽样重审（format/flusher/logx_c/staging/JNI）；③ 全量构建 + 单测/热路径/压力测试实测回归
> **方法**: 沿用项目纪律——修复本体逐行核对 + 实测验证（构建 → 测试 → 必要时构造对照实验）

---

## 一、总体结论

**R16 修复批次（F-1~F-6）全部核实在位且实现正确**，无引入性缺陷。本轮实测发现 **1×P3（测试时序炸弹，本轮已就地修复并验证）**，core 产品代码持续保持零 P2、零行为缺陷。

| 编号 | 级别 | 位置 | 一句话 | 状态 |
|---|---|---|---|---|
| F-1 | **P3** | `tests/unit/test_review_p1_p4.cpp` R13_N2 | 测试时序炸弹：历史日期 `20260920.log` 超默认 7 天保留期，export 前 retention 清理删之 → `LOGX_ERR_NOT_FOUND`；2026-09-27 后爆发 | **本轮已修复实测绿** |
| N-1 | Nit | — | R16 修复批一处注释先行于行为、一处符号细节（见 §二·5/6），不构成行为缺陷 | 记录备查 |

---

## 二、R16 修复批次独立复核（逐条重验，不沿用前轮结论）

### 1. R16 F-1（`make_slice` 校验和 XOR 化）——**属实闭环**
- `tests/unit/test_review_p1_p4.cpp:190` 现为 `cksum ^= b`，与核心 `cksum1`（`format.cpp:23-27` XOR 折叠）同算法；注释完整交代了事故链。
- 实测：修掉本轮新发现的 F-1（时序炸弹）后，该测试**以有效绿通过**（不再是 R16 所述"空洞绿"）。

### 2. R16 F-2（`parse_records_stream` 契约对齐）——**属实闭环**
- `logx_format.h:80-82` 契约改为"恒返回 true + 损坏容灾内部完成 + carry 清空说明"，逐句与 `format.cpp:388-475` 实现核对吻合（重同步/滑 6B/跳整条三路径均内部处理，全函数确无 `return false`）。
- `logx_c.cpp:165` 死分支条件包裹已去除，注释（`:163`）指明恒真语义。全仓其余 4 个调用点（tests 3 处）不受影响。

### 3. R16 F-3（key_provider trampoline attach/detach 对称）——**属实闭环**
- `logx_jni.cpp:139-188` 现与 `jni_error_trampoline`（`:95-131`）模式完全对称：EDETACHED → attach → `need_detach=true` → 返回前 detach；`GetEnv==JNI_OK` 的已 attach 线程不误 detach。
- 密钥经 `thread_local g_tls_key_buf` 拷贝后返回，detach 不影响生存期（注释 `:171-172` 已论证）；异常清理与 `DeleteLocalRef` 顺序正确。

### 4. R16 F-5（跨天分支补 `current_size = 0`）——**属实闭环**
- `flusher.cpp:597-600` 跨天分支现清 path/date/split/size 四项齐全；补测注释准确。对照 `ensure_append_target` 切分分支（`:630`）本就清 size，语义一致。

### 5. R16 F-6（`make_tmp_path` 用 inst->pal + 原子计数回退）——**属实闭环，两处细节备查**
- `logx_c.cpp:64-74`：随机数/时间/PID 统一走 `inst->pal`；CSPRNG 失败回退 `s_tmp_name_seq` 原子递增，与 PAL 契约"严禁低熵回退"对齐；注释明确"仅保证文件名唯一性，非加密用途"。
- 细节 a（Nit，不列发现）：`static std::atomic` 为进程级共享，与"每实例独立"的其余状态不对称——但文件名另有 date+pid 维度，无撞名风险，仅注释层面可提。
- 细节 b（Nit）：回退分支注释声称"严格单调不撞名"，严格说 rnd 仅进程内单调、跨进程靠 pid 区分，注释略过 pid 维度（代码行为正确）。

### 6. R16 F-4（回绕注释纠正 + 恒假守卫定位）——**属实闭环**
- `staging.cpp:334-338` 注释现准确描述 `free_new = free_old − pad` 且声明下方检查为防御守卫；与 `:310` 前置 `free < pad + total → kFull` 的数学关系自洽。

---

## 三、新发现详述

### F-1（P3，本轮已修复）R13_N2 测试时序炸弹——历史日期文件被 retention 清理删掉，export 返回 NOT_FOUND

- **位置**: `tests/unit/test_review_p1_p4.cpp`（`R13_N2_ExportCorruptedBlockClearsCarrySliceBuf`，cfg 未设 `retention_days`）
- **现象（实测复现，HEAD 与 a0d0d9d 均红）**: 该测试手工构造历史日期文件 `dir/20260920.log`（相对路径、`logx_open` 之后写入），随后 `logx_export("20260920","20260920",...)` 返回 `-8 (LOGX_ERR_NOT_FOUND)`，快照为空、回调 0 次。
- **根因链**:
  1. 测试未设 `cfg.retention_days` → 默认 7 天（`logx_c.cpp:338`）。
  2. export 先 `flush_blocking` → Flusher 线程 `drain_once` → `ensure_append_target` 跨天分支（`flusher.cpp:600`，启动时 `date_changed=true` 必走）调 `retention_cleanup_impl`。
  3. 清理阈值 = 当日(2026-10-01) − 7 天 = `20260924`，`20260920 < 20260924` → `file_unlink`（`flusher.cpp:514-517`）。
  4. `take_snapshot` 列不到文件 → `LOGX_ERR_NOT_FOUND`。
- **为何此前未被发现**: 测试保质期仅到 2026-09-27（9-20+7），恰在 R16 审查日期（09-30）之前已失效——R16 当时未跑该测试或未察觉。**与本轮 R16 F-1 修复无关**（用临时 worktree 在 a0d0d9d 上复现同样失败，排除新引入）。
- **修法（本轮已落盘）**: cfg 补 `cfg.retention_days = 36500;`（注释交代根因），同类先例 `test_flusher.cpp:414`（"长期保留, 杜绝测试时效隐患"）。
- **实测**: 修复后 `ReviewFixesTest.*` 4/4 绿；全量 `logx_unit_tests` 146/146 绿；`logx_hotpath_alloc_tests` 2/2 绿；`logx_stress_tests` 4/4 绿。
- **教训（建议纳入项目门禁）**: 单测中凡手工构造"历史日期"日志文件/依赖非当日日期的用例，必须显式设 `retention_days = 36500`（或用未来日期）；否则是潜伏的时序炸弹。可 grep 模式：测试文件中 `yyyymmdd = 20` 开头的字面量，逐一核对对应 cfg 是否设了超长保留期。

---

## 四、已排除疑点

| # | 疑点 | 判定与依据 |
|---|---|---|
| X-1 | F-6 修复的 `s_tmp_name_seq` 多实例共享是否撞名 | **排除**。文件名 = date + pid + rnd 三维，rnd 仅需进程内区分；跨实例/跨进程由 date+pid 隔离 |
| X-2 | F-3 detach 后 `g_tls_key_buf` 仍被调用方读取是否悬垂 | **排除**。thread_local 是纯 C++ 存储（非 JNI 引用），detach 不回收；Flusher 同线程读，生存期至下次覆盖 |
| X-3 | F-5 修复后 hint 命中昨日文件但 `date_changed` 未置位的时序 | **排除**。`drain_once` 先比对 `yyyymmdd != current_date` 置位（`flusher.cpp:639`），`ensure_append_target` 内跨天分支读同一 `today`，链路自洽；size 清零只影响切分判定，不影响 hint 命中 |
| X-4 | `decode_one_file` 对明文文件引用 `salt`（hdr+12）传给 `decode_block` 但未用 | **排除**。非加密路径 `tag=nullptr`，salt 仅为加密分支准备；`block_header_plausible` 已做长度交叉校验 |

---

## 五、验证记录

| 项 | 命令/环境 | 结果 |
|---|---|---|
| 构建 | `build-full`（vcvars64 + ninja，MSVC 14.43 /std:c++17 /utf-8） | 55/55 全链接成功，零告警 |
| 单测 | `tests\unit\logx_unit_tests.exe` | **146/146 PASSED** |
| 热路径 | `tests\unit\logx_hotpath_alloc_tests.exe` | 2/2 PASSED |
| 压力 | `tests\stress\logx_stress_tests.exe` | 4/4 PASSED |
| 对照实验 | 临时 worktree @ a0d0d9d 复跑 R13_N2（已清理） | 同样失败 → 证实时序炸弹为既有问题，非 R16 批引入 |

## 六、修复顺序建议

本轮唯一发现 F-1 已就地修复并全量验证（工作树 1 文件改动：`tests/unit/test_review_p1_p4.cpp` 补 `retention_days = 36500`）。建议：

1. 提交本轮测试修复（建议 `fix(test): R13_N2 用例补超长保留期, 消除历史日期文件被 retention 清理的时序炸弹 (R17 F-1)`）。
2. R16 F-6 的两处注释细节（§二·5）可随下批文档化提交顺手处理，非必须。
3. §三"教训"项建议写入项目审查门禁清单。
