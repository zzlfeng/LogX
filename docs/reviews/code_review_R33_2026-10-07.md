# R33 审查报告：B-10 方案 1 升级与 rebuilt 收口批次的独立复核

> **审查日期**: 2026-10-07
> **审查对象**:
> - 提交 `ec4c0c5`「feat(recovery,test,docs): B-10 升级全局目录最大序号防御门(方案 1)——收口 F-1/F-2 盲区与 Nonce 密码学定性勘误 (计划表 v1.44)」
> - 提交 `1a82f1d`「feat(recovery,test,docs): 补全 hdr.rebuilt 整区重建防御门与逻辑精简 (F-1/N-1 收口, 计划表 v1.45)」
> - 关联文档提交 `9ca6d53` / `0cd1e60` / `1507765`（R32 修订勘误版报告入库与登记行）
> **审查方式**: 全量 diff 逐行审读 + 恢复路径全分支推演 + 独立撤销对照实验两次（R18-2 门禁）+ MSVC clean 全量重建（R21-2 头文件门禁）+ 四套件测试独立复跑 + Android NDK 重建与 Redmi K30 Pro 真机四套件复跑
> **裁定**: **两批次改动成立，B-10 防御在 reconcile 与 rebuilt 双路径闭环，可保留合入**。本审查独立复做的撤销对照实验与提交记录逐字吻合；另识别 1×P3 + 1×Nit（均为存量问题，非本批次引入）。

---

## 一、 审查对象概述

1. **`ec4c0c5` (B-10 升级方案 1)**：
   - `scan_dir_for_target` 扩展可选出参 `out_max_seqno`（flusher.cpp），在既有的 `scan_one(..., repair=false)` 目录遍历中顺带收集全目录最大尾块序号，**零额外磁盘 IO**；
   - `reconcile` 防御门由「best vs 单个 hint」升级为「best vs 目录全局最大序号」，覆盖 F-1（空残留高分卷且 Header 撕裂）与 F-2（多级历史分卷/无 hint 漏防）；
   - 密码学定性勘误：纠正沿袭自 d636559/计划表 v1.43 的「B-10 防 GCM Nonce 复用」错误假设（nonce = 每文件独立随机 salt(8B) ‖ seqno(4B)，跨分卷撞号不构成 nonce 复用；B-10 真实价值是磁盘 seqno 全局单调结构不变量）；
   - 新增 F-1 回归用例 `B10_EmptyResidualSplitWithTornHeader_ClampedToHistoryMax`，补齐 F-3 注释。

2. **`1a82f1d` (rebuilt 收口)**：
   - `run_recovery` 的 `hdr.rebuilt == true` 整区重建分支（绕过 reconcile）同样提取 `max_dir_seqno`，`hdr.seqno = std::max(sr.seqno, max_dir_seqno)`（截断时 `+1`），倒退时发 rebuilt 专属告警；
   - 合并 reconcile 防御门两个同构分支条件（N-1），移除多余空行；
   - 新增 `B10_RebuiltStaging_ResidualHigherSplit_ClampedAndWarned` 回归用例。

---

## 二、 测试验证与构建复跑核验

**桌面（Windows / MSVC 14.43 x64 Release，`LOGX_WITH_ZSTD=ON` / `LOGX_WITH_AESGCM=ON`）**：

| 验证项 | 结果 |
|---|---|
| clean 全量重建（R21-2，因改 `logx_internal.h`） | **56/56，0 警告 0 错误**（独立复跑，`ninja -t clean` 后全量） |
| `logx_unit_tests` | **174 / 174 PASSED**（15.4s） |
| `logx_hotpath_alloc_tests` / `logx_stress_tests` / `logx_crash_fork_tests` | **2/2 + 4/4 + 3/3 PASSED** |

**真机（Redmi K30 Pro / Android 12，NDK 26.1 arm64-v8a Release，无线 adb）**：

| 验证项 | 结果 |
|---|---|
| 二进制新鲜度 | 重建后 mtime（22:04-22:05）晚于 `1a82f1d`（21:44），非陈旧产物 |
| 设备时钟 | `auto_time 0` 维持；复测 3s 步进 3.047/3.049s ≈ 1:1（首轮单次 4.85s 经复测归因无线 adb 连接建立抖动，见 §六·4） |
| `logx_unit_tests` | **175 / 175 PASSED**（11.2s；与历史「真机比主机多 1 用例」延续一致，见 §六·3） |
| hotpath / stress / crash_fork | **2/2 + 4/4 + 3/3 PASSED**，退出码全 0 |

**结论**：两批次提交声称的全部测试数据（MSVC 174/174、0 警告）独立复跑逐字吻合；真机复跑为本轮新增验证，四套件 100% 全绿。

---

## 三、 逐项改动关键推演

### 3.1 `ec4c0c5`：全局目录最大序号防御门（reconcile 路径）

```cpp
// recovery.cpp:212-219 (N-1 合并后)
if ((sr.seqno > 0 && max_dir_seqno > sr.seqno) ||
    (sr.seqno == 0 && max_dir_seqno > hdr.seqno)) {
    r.seqno_floor = max_dir_seqno;
    emit_event(inst, LOGX_EV_RECOVERY, -1,
               "target seqno regression vs directory max; resume clamped");
}
```

- **门位置正确**：位于 `!sr.ok` 早退与 `is_new_target` 判定之后，覆盖 reconcile 全部三条对账路径（is_target_newer / hint 命中 / best 兜底），F-2 的「`hdr.target.date == 0` 时门完全不进入」确实收口；
- **分支 1 无假阳性**：`max_dir_seqno` 含 best 自身尾块，仅当**其他**分卷更高才触发；best 即目录最大时条件恒假；
- **分支 2（空目标）语义推演**：合法单写者轮转（A 尾块 25 已提交、空 B、S_hdr=25）时 `25 > 25` 为假，不触发，R19 F-13 的 is_new_target 豁免语义完整保留；而 S_hdr 撕裂落后（A 尾块 25、S_hdr=20、空 B 更新）时钳制至 25——该场景旧 hint 门因 `sr.seqno > 0` 前置条件无法覆盖，属本升级**额外修复的单写者崩溃时序洞**；
- **is_new_target + floor 组合**：`resume = max(S_hdr, seqno_floor)`，新块自 floor+1 起，与「已刷区间不参与环形扫描」的既有会计不冲突。

### 3.2 `1a82f1d`：rebuilt 路径收口

```cpp
// recovery.cpp:240-266
uint32_t max_dir_seqno = 0;
std::string best = scan_dir_for_target(inst, &max_dir_seqno);
if (!best.empty()) {
    FileScanResult sr = scan_append_target(inst, best);
    if (sr.ok) {
        hdr.seqno = std::max(sr.seqno, max_dir_seqno);
        if (sr.truncated) { hdr.seqno += 1; ... }
        if (max_dir_seqno > sr.seqno) { emit_event(..., "rebuilt: target seqno regression ..."); }
        ...
    } else if (max_dir_seqno > 0) { hdr.seqno = max_dir_seqno; }
} else if (max_dir_seqno > 0) { hdr.seqno = max_dir_seqno; }
```

- 截断 `+1` 语义由「sr.seqno + 1」改为「max(sr.seqno, max_dir_seqno) + 1」：无残留时与 R19 F-14 旧值逐一相同（5+1=6）；有残留时产生保守跳隙，无重复风险；
- 告警事件串 `"rebuilt: target seqno regression ..."` 为 rebuilt 分支独有，回归用例断言该串即可**自证用例真实走进 rebuilt 路径**（R21 F14「注释声称 rebuilt 实走别路」类空洞在本用例中结构性不可能）；
- 兜底分支观察见 §五 N-2。

### 3.3 `1a82f1d`：N-1 分支合并等价性

合并式 `(A && B) || (C && D)` 与原 `if(A&&B) else if(C&&D)` 逐 case 等价：`sr.seqno > 0` 时 C 恒假，`sr.seqno == 0` 时 A 恒假——两支互斥，合并无行为变化。已排除疑点级确认。

---

## 四、 独立撤销对照实验（R18-2 门禁，两次）

| 实验 | 变异 | 结果 | 恢复后 |
|---|---|---|---|
| 实验 1（对 `ec4c0c5`） | reconcile 防御门两分支条件前置 `false &&` | `B10_ResidualHigherSplit...` 复红（**recovered 11 vs 20**）、`B10_EmptyResidualSplit...` 复红（**recovered 10 vs 20**），与提交信息逐字吻合 | 3 个 B10 用例全绿 |
| 实验 2（对 `1a82f1d`） | rebuilt 钳制退回 `hdr.seqno = sr.seqno` | `B10_RebuiltStaging...` 复红（**recovered 5 vs 20**） | 全绿 |

两次实验的「红」本身即双向自证：若变异未生效（二进制陈旧/编译旁路），用例将保持绿——绿红差异同时证明**变异生效**与**用例真实承载修复**，无需另行配置正向对照。实验后源码恢复，`git status` 干净。

---

## 五、 审查发现清单（Findings）

| 编号 | 级别 | 位置 | 现象与成因 | 建议 |
|---|---|---|---|---|
| **N-1** | **P3** | `core/src/logx_c.cpp:90` | **`key_lookup_attempted` 触发 clang `-Wunused-but-set-variable` 警告**。该变量仅置位不再被读取（R22 F-2 勘误所述「:120 补偿分支恒不可达」的残留），MSVC 对此静默导致历轮「0 警告」声称均基于 MSVC；NDK clang 构建实测 **1 warning**（本复跑 capture 实证）。存量问题，非本批次引入。 | 删除该变量与置位语句（或如需保留可观测语义，改接计数器）；属 R22 F-2 死分支清理的自然延伸 |
| **N-2** | **Nit** | `core/src/recovery.cpp:247-248, 262-263` | rebuilt 分支两处 `else if (max_dir_seqno > 0) hdr.seqno = max_dir_seqno;` 实际不可达：`max_dir_seqno > 0` 蕴含目录存在 `scan_one` ok 的文件，而 ok 文件必然参与 best 决选（日期日历校验保证 `date > 0`），故 best 非空、且其 `scan_append_target` 仅在瞬态 IO 错误下失败。防御性代码，无行为危害。 | 可保留作纵深防御（recovery 路径保守为佳），或删除并在注释中记录「max>0 ⟹ best 非空」不变量 |

---

## 六、 已排除疑点（逐条辨析与核验依据）

1. **合法轮转/跨天场景误报排除**：S_hdr 与目录 max 相等的全部单写者时序（轮转已提交/未提交、跨天空文件、B 非空但 max 即其尾块）逐一推演均不触发防御门；触发前提必为「文件尾块序号 > Header 已提交序号」的真实撕裂或外部残留。
2. **F-13 豁免语义保持**：空新目标 `resume = S_hdr` 的既有豁免仅在 `max_dir_seqno > S_hdr`（即 Header 落后于磁盘）时被钳制覆盖，正常豁免路径不受影响。
3. **真机 175 vs 主机 174 用例数差异**：与 2026-09-27 时代「真机 171/主机 170」的 +1 延续一致，为设备专属用例（设备相关断言），非用例丢失或二进制错配（二进制 mtime 已核对晚于 `1a82f1d`）。
4. **首轮时钟步进 4.85s/3s 归因**：`auto_time 0` 维持不变，紧接复测两轮 3s 间隔实测 3.047/3.049s（1:1），判定首轮为无线 adb 连接建立抖动（连接窗口内 host 侧计时早于 shell 侧），非 NTP 阶跃复发；时序敏感用例全绿佐证。
5. **全零 salt 误用排除**：`flusher.cpp` 中 `memset(salt, 0)` 仅存在于 `ActiveFileTarget::reset()`（缓存复位），加密文件 salt 唯一来源为创建期 `random_bytes`（fail-fast 拒绝低熵）或既有文件头回读，无全零 salt 落盘路径。
6. **N-1 合并等价性**：见 §3.3，逐 case 推演无行为差异。

---

## 七、 验证局限（必须说明）

1. **平台覆盖**：桌面验证为 win32 PAL + MSVC；真机验证为 arm64 + Android (bionic) PAL。Linux 桌面分支无环境，未验证（延续 R12 补测以来的一贯局限）。
2. **工具链警告口径**：「0 警告」仅在 MSVC 工具链成立；NDK clang 工具链存在 1 条警告（§五 N-1），历史各轮「0 警告」声称应按此口径理解。
3. **撤销对照为临时变异**：实验通过工作区源码临时修改 + 增量重建完成，实验后已字节级还原（`git status` 干净、B10 用例回绿），未产生任何提交。
4. **真机时钟**：残余固定时钟偏移不影响测试（monotonic 时钟路径 + 时序用例容忍域内）；`auto_time 0` 建议保持，防止 NTP 阶跃复发假红（R32 时代环境归因记录）。

---

## 八、 审查裁定与后续建议

1. **当前实现评定**：`ec4c0c5` 与 `1a82f1d` 逻辑严密，两次独立撤销对照复红、MSVC clean 重建与真机四套件全绿；B-10 防御在 **reconcile 与 rebuilt 双路径完整闭环**，全局 seqno 单调不变量在全部恢复路径受保护；
2. **入库与合并建议**：可直接保留合入（已入库）；
3. **修复顺序建议**：N-1（P3，clang 警告清理）可随下一代码批次顺带收口（一处变量删除 + 相关注释）；N-2 二选一处置均可，建议保留并在注释钉死「max>0 ⟹ best 非空」不变量；
4. **Backlog 建议**：R32 报告方案 2（NO_SPACE 事件驱动退避）是否收录 Backlog，待用户拍板。
