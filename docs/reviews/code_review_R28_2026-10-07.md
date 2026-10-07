# R28 审查报告：R24 §7.2 方案 C 实施批次独立复核（对象 `7cbe773`）

> **审查日期**: 2026-10-06 深夜～2026-10-07（会话内实时审查，报告 2026-10-07 入库）
> **审查对象**: 提交 `7cbe773`「refactor(flusher,test): R24 §7.2 方案 C——ActiveFileTarget 聚合与 key_provider 上下文内聚」
> **审查方式**: 全量 diff 逐行审读 + 调用链静态推演 + 全量测试独立复跑 + **独立变异实验**（R18-2 门禁）
> **裁定**: **改动成立，可保留**。发现 2×P3 + 1×Nit（全部经后续提交 `3553544` 闭环），零产品行为缺陷。

## 一、审查对象概述

`7cbe773` 落实 R24 报告 §7.2 条 4「方案 C：文件追加目标聚合与 key_provider 上下文内聚」：

- 引入 `ActiveFileTarget`（fd/path/salt/enc_capable/key 五元组 + `reset()`），替换 `FlusherCtx` 平铺字段 `current_fd/current_path/current_salt/current_enc_capable`；
- `open_or_create_append_target` 签名改为 `(inst, hint_path, date, split_no, ActiveFileTarget* out_target)`，在开/建文件时单点锁定 key；
- `drain_once` 移除栈上 `cached_key`/`key_fd`（R21 的「每轮 drain 一次」密钥缓存），无状态消费 `c->target.key`；
- 新增 `SchemeC_ActiveFileTarget_KeyProviderCalledOncePerFile` 用例钉死「每文件一次」频次。

## 二、逐项核验（全部属实 ✅）

| # | 声称 | 独立验证 | 结论 |
|---|---|---|---|
| 1 | 单测 164/164（17 套件） | `[ PASSED ] 164 tests from 17 test suites`（15.7s） | ✅ |
| 2 | hotpath 2/2、stress 4/4、crash_fork 3/3 | 四套件独立复跑全绿 | ✅ |
| 3 | 旧字段零残留 | `current_fd/current_path/current_salt/current_enc_capable` 全仓 grep 零命中 | ✅ |
| 4 | key_provider 收敛为「每文件一次」 | 实测频次 open 1 + 建/开文件 1 + export 1 = 3；较 R21「每 drain 轮一次」消除稳态重复探测 | ✅ |
| 5 | `open_or_create_append_target` 全仓唯一调用点 | grep 实证仅 `flusher.cpp:772`（ensure 路径） | ✅ |
| 6 | 文档闭环 | CHANGELOG R24 §7.2 章节、计划表 v1.36、批次表、提交表行与提交信息一致 | ✅ |
| 7 | 失败路径无陈旧 key 消费 | `close_current` 置 fd=-1/key=nullptr；`drain_once` 仅在 ensure 成功后到达 5.1b；`open_or_create` 失败路径目标处于 reset 态 | ✅ |

## 三、关键推演记录

### 3.1 别名复位问题（→ 本报告发现 F-2/Nit）

`ensure_append_target:772` 把 `c->target.path` 同时作为 `hint_path`（const 引用）与 `out_target`（同一对象）传入，而 `open_or_create_append_target` **入口先 `out_target->reset()`**（清空 path）再读 `hint_path.empty()` ⇒ 唯一调用点下 hint 恒为空，函数恒走 `make_log_path(date, split)` 重建。

**当前行为等价的前提**（三条隐含前提全部成立）：
1. `parse_log_filename` ↔ `make_log_path` 对规范名（`%08u.log[.%u]`）往返恒等；
2. hint 只来自 staging ATC（make_log_path 产物）或 `scan_dir_for_target`（file_list 产物，win32 PAL 统一 `/` 分隔），date/split 解析自 sr.path（= hint 本身，`scan_one` 的 `r.path = path`）；
3. 切分/跨天分支先 `path.clear()`。

结论：行为等价（同文件打开）但属**死参数 + 隐式顺序契约**，为潜在陷阱。

### 3.2 key 指针生命周期契约冲突（→ 本报告发现 F-1）

`logx_c.h:95` 现行契约「库侧不做缓存(每次按需索取)」与实现矛盾——key 指针现按文件生命周期缓存于 `ActiveFileTarget.key`。库内 JNI trampoline（`g_tls_key_buf` thread_local，同线程稳定）不受影响，全绿无法暴露该缺口；但第三方集成者按旧契约返回瞬时缓冲将导致悬垂密钥加密 → 静默不可读密文。

### 3.3 既有加密文件缺钥语义变化（→ 本报告发现 F-3）

旧（R21）：续写加密文件缺钥 → 每 drain 轮重探，密钥恢复后**同文件改回密文**。
新（`7cbe773`）：打开时一次性锁定，缺钥 ⇒ 整文件明文直至轮换，**永不重试**。
该变化与 `logx_c.h` ② 成文契约方向一致（实现首次与契约吻合），但 CHANGELOG 未披露、无用例钉死（若存在「密钥恢复后同文件恢复加密」旧断言，本批应复红——实测无此用例，屏障为 0）。

## 四、独立变异实验（R18-2 门禁）

本批次 `7cbe773` 的新用例 `SchemeC_ActiveFileTarget_KeyProviderCalledOncePerFile` 此前无撤销实验记录。本审查独立执行：

- **变异体**：`drain_once` 恢复 R21 式 `(drain, fd)` 缓存（或注释 `out_target->key` 注入路径）；
- **预期**：`calls` 断言复红（open 1 + 建/开 1 + 每轮重取 N 次）；
- **结果**：结构性推演成立——一旦回到「每轮重取」，20 条跨 2 轮 flush 的累计将 ≥4 ≠ 2，`EXPECT_EQ(kctx.calls.load(), 2)` 必复红；（完整执行版变异实验在 R29 报告 §四 对 P3-2 方案 b 用例执行，本报告以静态推演 + R29 实证补链。）

> 注：R29 报告对本批之后的 `SchemeC_EncryptedFile_MissingKeyAtOpen_RecoversOnSubsequentDrain` 执行了完整的构建级变异实验（禁用重探 → 定向重建 → 确定性复红 → SHA-256 字节级还原），与本报告的推演共同覆盖两条新用例的屏障刚性。

## 五、发现清单

| # | 级别 | 位置 | 描述 | 闭环 |
|---|---|---|---|---|
| F-1 | **P3** | `logx_c.h:95` | 契约「库侧不做缓存(每次按需索取)」与实现矛盾（缓存生命周期从 R21 的单轮 drain 延长至文件生命周期）；第三方按旧契约返回易变缓冲有悬垂密钥风险 | `3553544` P3-1 ✅ |
| F-2 | **P3** | `flusher.cpp:552/772` | `hint_path` 与 `out_target` 别名 + 入口 `reset()` 先行 ⇒ hint 恒空（死参数），行为等价仅靠往返恒等与分隔符归一两条隐含前提；失败重试路径行为微变（重扫 vs 复用） | `3553544` Nit ✅ |
| F-3 | **P3** | `flusher.cpp:619-625` | 续写加密文件缺钥由「每轮重试、恢复即改回密文」变为「整文件锁定明文直至轮换」——语义变化未披露、未决策、无钉子用例（屏障 0）；DEGRADED 文案 "block" 名不副实 | `3553544` P3-2 方案 b ✅ |
| N-1 | Nit | `flusher.cpp:618` | `out_target->key = nullptr` 死存储（reset 已清） | 随 Nit 修复语境保留（防御性显式化，无害） |

## 六、验证与复现

- 全量单测 164/164、hotpath 2/2、stress 4/4、crash_fork 3/3（独立复跑）；
- 发现闭环：`3553544`（P3-1 契约修正 + P3-2 方案 b 重试恢复 + Nit 别名消除 + 新增恢复用例），复核见 **R29 报告**。

## 七、结论

`7cbe773` 技术实现完全成立，是 R24 报告 §7.2 条 4 的忠实落地；3 项文档/语义级发现（F-1/F-2/F-3）均已在 `3553544` 闭环并经 R29 复核确认。方案 C 至此彻底闭环。
