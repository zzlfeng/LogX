# 审查报告索引(docs/reviews/)

> 本目录收存 LogX 全部**代码审查批次报告本体**(R 系列 + 早期专项),2026-10-06 自仓库外统一迁入并纳入版本控制。
> 批次结论、修复清单与验证摘要在 [`../../CHANGELOG.md`](../../CHANGELOG.md)(活跃批次)与 [`../changelog/archive-2026-09.md`](../changelog/archive-2026-09.md)(2026-09 历史批次);本目录只放报告原文。

## 报告清单

| 报告 | 批次/主题 | 对应 CHANGELOG 记录 |
|---|---|---|
| [code_review_2026-09-13.md](code_review_2026-09-13.md) | 2026-09-13 综合审查 | 归档 2026-09 相应批次 |
| [code_review_R4_2026-09-13.md](code_review_R4_2026-09-13.md) | R4 审查批次 | 归档 2026-09 R4 |
| [code_review_R5_T56_2026-09-13.md](code_review_R5_T56_2026-09-13.md) | R5 + T5.6 事件扩展 | 归档 2026-09 R5 |
| [code_review_R6_2026-09-13.md](code_review_R6_2026-09-13.md) | R6 复审批次 | 归档 2026-09 R6 |
| [code_review_R7_2026-09-13.md](code_review_R7_2026-09-13.md) | R7 收口复审 | 归档 2026-09 R7 |
| [code_review_R8_2026-09-13.md](code_review_R8_2026-09-13.md) | R8 收口复审 | 归档 2026-09 R8 |
| [code_review_R9_2026-09-13.md](code_review_R9_2026-09-13.md) | R9 收口复审 | 归档 2026-09 R9 |
| [code_review_R10_2026-09-13.md](code_review_R10_2026-09-13.md) | R10 收口复审 | 归档 2026-09 R10 |
| [code_review_R11_2026-09-13.md](code_review_R11_2026-09-13.md) | R11 收口复审 | 归档 2026-09 R11 |
| [code_review_R13_2026-09-24.md](code_review_R13_2026-09-24.md) | R13 审查批次 | 归档 2026-09 R13 |
| [code_review_R14_2026-09-25.md](code_review_R14_2026-09-25.md) | R14 审查批次 | 归档 2026-09 R14 |
| [code_review_R15_2026-09-25.md](code_review_R15_2026-09-25.md) | R15 收口(R13→R14→R15 链闭合) | 归档 2026-09 R15 |
| [code_review_R16_2026-09-30.md](code_review_R16_2026-09-30.md) | R16 修复批次审查 | 归档 2026-09 R16 |
| [code_review_R17_2026-10-01.md](code_review_R17_2026-10-01.md) | R17 审查(R16 修复批次审查) | 主文件 R17 批次 |
| [code_review_R18_2026-10-04.md](code_review_R18_2026-10-04.md) | R18 审查(门禁固化) | 主文件 R18 批次 |
| [code_review_R19_2026-10-04.md](code_review_R19_2026-10-04.md) | R19 审查(16 项修复) | 主文件 R19 批次 |
| [code_review_R20_2026-10-04.md](code_review_R20_2026-10-04.md) | R20 复核补强 | 主文件 R20 批次 |
| [code_review_R21_2026-10-05.md](code_review_R21_2026-10-05.md) | R21 补强批次 | 主文件 R21 批次 |
| [code_review_R22_2026-10-05.md](code_review_R22_2026-10-05.md) | R22 审查(R21 补强复核) | 主文件 R22 批次 |
| [code_review_R23_2026-10-05.md](code_review_R23_2026-10-05.md) | R23 审查(R22 代码优化补丁裁定) | 主文件 R25 批次 |
| [code_review_R24_2026-10-05.md](code_review_R24_2026-10-05.md) | R24 独立验证与 §7.1 处方 | 主文件 R25 批次 |
| [code_review_R27_2026-10-06.md](code_review_R27_2026-10-06.md) | R27 审查(R26 批次独立复核) | 主文件 R26 批次 |
| [code_review_R28_2026-10-07.md](code_review_R28_2026-10-07.md) | R28 审查(方案 C 实施批次独立复核) | 主文件 R24 §7.2 方案 C 章节 |
| [code_review_R29_2026-10-07.md](code_review_R29_2026-10-07.md) | R29 审查(方案 C 复核收口批次复核) | 主文件 R24 §7.2 方案 C 章节第 8 项 |
| [code_review_R32_2026-10-07.md](code_review_R32_2026-10-07.md) | R32 审查(近期提交与 B-10/B-8 落地综合独立审查) | 主文件近期批次章节 |

## 说明

1. **位置沿革**:以上 21 份报告原落在 git 仓库外(工作区根目录),2026-10-06 迁入本目录入库;迁移系整体 `Move`,报告原文未改写。
2. **更早的 4 份综合报告**(`code_review_consolidated.md`、`code_review_report.md`、`code_review_report_2026-09-04.md`、`code_review_supplement.md`)仍在 [`../`](../) ——它们已入库且内部使用 `../core/...` 等相对链接,迁移动需重写链接,故保持原位。
3. **行号锚点告警**:R24 报告引用的 `CHANGELOG.md#L2092-L2105`/`#L2105` 等行号系 **2026-10-06 CHANGELOG 拆分前坐标**;对应内容现位于主 CHANGELOG 的「R22 代码优化补丁」与「R25 批次」章节。各报告内 `file:///d:/...` 绝对链接指向的源码路径不受迁移影响。
4. **命名纪律**:新报告命名 `code_review_R{n}_YYYY-MM-DD.md`,收口后在 CHANGELOG 对应批次章节与本表各登记一行。
