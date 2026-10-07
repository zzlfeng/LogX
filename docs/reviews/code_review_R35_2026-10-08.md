# R35 复核报告：R34 批次（方案 2 撤销复红屏障与 N-2/N-3 收口）独立复核 + Android 链路修复 + 真机复跑

> **审查日期**: 2026-10-08
> **审查对象**:
> - 提交 `c1860a4`「feat(format,recovery,flusher): Backlog B-4 落地——收敛平行记录扫描实现与 cksum1 单源化 (计划表 v1.47)」
> - 提交 `b3a758d`「feat(core,flusher,test): R32 方案 2 落地——NO_SPACE 退避事件驱动化与级联唤醒令牌 (计划表 v1.48)」
> - 提交 `6217326`「fix(test,format,core): 收口 N-1~N-3——建立 R18-2 撤销复红屏障、防下溢守卫与 Wire 偏移编译期断言 (计划表 v1.49)」（= R34 批次）
> - 提交 `3640d8a`「fix(test): 级联唤醒屏障停驻门固化 + 补 #include <thread> 修复 Android 构建断裂」（本批复核产出的收口）
> **审查方式**: 全量 diff 逐行审读 + PAL 事件语义实查（auto-reset/sticky 消费）+ 独立撤销对照实验 3 组（R18-2 门禁）+ 探针级信号路径取证 + MSVC clean 全量重建 + Android NDK 构建修复 + Redmi K30 Pro 真机四套件复跑
> **裁定**: **R34 批次产品代码改动成立；N-1 屏障经停驻门固化后由「时序依赖」转为「刚性」**；发现并修复 1 项 Android 构建断裂（F-2）；真机四套件全绿。

---

## 一、 复核结论摘要

| 对象 | 结论 |
|---|---|
| `c1860a4`（B-4 收敛） | ✅ 成立。`parse_staging_record_header`/`peek_record_len`/`cksum1` 单源化与两侧原语义逐行等价（探测不含 magic、后置校验顺序保持、uint16 取值域相同），收敛深度恰当（方案 C 仍无必要） |
| `b3a758d`（方案 2） | ✅ 成立。auto-reset + sticky 消费语义下无丢唤醒、无忙旋；全 signal 均在 `mutex_unlock` 后；close/destroy 经 `active_ops`/`close_pending` 递延衔接安全；5ms 切片提供与 signal 正确性解耦的活性保险 |
| `6217326`（R34 批次） | ⚠️ **N-2/N-3 成立；N-1 屏障为「时序依赖型」，经本批停驻门（`3640d8a`）固化后方成刚性**（见 F-1） |
| `3640d8a`（停驻门收口） | ✅ 撤销实验确定性复红（1654ms），绿路径 3 连跑通过 |

---

## 二、 测试验证与构建复跑核验

| 环境 | 结果 |
|---|---|
| MSVC clean 全量重建（R21-2） | 56/56，**0 警告 0 错误** |
| 桌面四套件 | **177/177** + 2/2 + 4/4 + 3/3（独立复跑） |
| Android NDK 构建 | **修复后成功**（修复前自 `b3a758d` 起断裂，见 F-2）；clang 余 1 条存量 `-Wunused-function` 警告（`cleanup_file`，test_r19_regression.cpp:123，非本批引入） |
| 真机（Redmi K30 Pro / Android 12，无线 adb） | 二进制 mtime（10-08 00:54）晚于全部审查对象提交；时钟实测 1:1；**178/178**（含停驻门版级联用例）+ 2/2 + 4/4 + 3/3，退出码全 0 |

---

## 三、 独立撤销对照实验（3 组）

| 实验 | 变异 | 结果 |
|---|---|---|
| A | 禁用 flusher.cpp 全部 5 处 `space_event` signal | `FastWakeupOnSpaceFreed` 确定性复红（1553ms 超时红，兜底无挂死）；还原回绿。**屏障成立**。注：单点删除会被 `bytes==0` 早退分支等冗余信号源兜住（属合理冗余，屏障在机制层成立） |
| B（停驻门前） | 仅禁用 `logx_write` 级联令牌 | `CascadingWakeupMultiThread` **5+ 次连续假绿**（15-89ms），见 F-1 |
| B'（停驻门后） | 同 B | `CascadingWakeupMultiThread` **确定性复红（1654ms）**：探针取证 Flusher 侧信号 #2-#9 全部被限流丢弃、仅 1 个等待者被放行的首脉冲唤醒；还原回绿 ×3 |

---

## 四、 发现清单（Findings）

### F-1（P3，已由 `3640d8a` 收口）R34 批次的级联撤销复红为时序依赖型，非确定性

R34 批次记录声称「注释级联令牌传递时确定性复红（Actual 1 vs Expected 3）」。本复核以相同变异独立复做，**5+ 次连续假绿**（15-89ms 全部通过），探针取证根因：

- `limit_flusher_space_signals` 仅能拦截**布防之后**的 Flusher 信号，而 Flusher 的去抖定时器（`kFlushDebounceMs = 75ms`）会在布防前自主 drain 填充记录；
- pre-limit 信号不受限流约束，实测即可唤醒全部等待者（探针显示 3 次 `strict WOKE` 均发生在首个 `DROPPED` 之前），级联令牌未被依赖，用例无论删不删级联都绿。

**定性**：该复红实验能否变红取决于 Flusher 去抖定时器与测试主线布防之间的调度竞态——R34 记录的红是真实出现过的一次运行，但不具确定性（同代码多次运行可绿），按 R18-2 门禁不构成刚性屏障。

**收口（`3640d8a`）**：`StrictEventWaitPal` 增加 Flusher 停驻门——`hold_flusher` 先于 `logx_open` 布防（stub 捕获首个 `ev_create` 句柄识别 flush_event），等待首轮 `drain_once` 完成且停驻确认（`flusher_parked_count >= 1`）后再填充，保证首轮 drain 面对空环且填充阶段零自主 drain；放行后恰好一次 drain 在限流下进行，级联令牌成为后继写线程唯一唤醒源。撤销实验由「时序依赖」转为确定性复红。

### F-2（P3，已由 `3640d8a` 收口）b3a758d 起 Android 构建断裂

`b3a758d` 在测试中引入 `std::thread`（写线程）未包含 `<thread>`——MSVC 经透传包含编译通过掩盖问题，**Android/clang（libc++）自该提交起编译失败**（`variable has incomplete type 'std::thread'`），真机验证链路中断约 1.5 天（期间计划表 v1.48/v1.49 的「Android」相关验证实为不可执行状态）。本批补 `#include <thread>` 修复，NDK 构建恢复。**教训**：头文件/include 变更与 MSVC「0 警告」声称均不等价于跨工具链可编译，Android 侧至少应保留定期构建冒烟。

### N-1（Nit，观察）wire-format `static_assert` 为同义反复

`logx_format.h` 中 5 条断言均为自指式（如 `static_assert(kWireOffPayloadLen == 0)` 断言常量等于自身，`kWireOffCksum + 1 == kHeaderLen` 同样自指），对线格式漂移无钉住力；偏移量具名化并统一 `parse_staging_record_header` 引用是有效成分。可选处置：换为「`make_block_header` 写 → `parse_staging_record_header` 读」的字节级 roundtrip 断言，或删除装饰性断言。

### N-2（Nit，观察）clang 构建余 1 条存量警告

`test_r19_regression.cpp:123` `cleanup_file` 未被引用（`-Wunused-function`）。历史「0 警告」声称口径仍仅限 MSVC（R33 已建立该口径注记）；建议随下一测试批次顺带清理。

---

## 五、 已排除疑点（逐条核验依据）

1. **撤销实验假绿非二进制陈旧**：实验 B 每次均确认 `logx_c.cpp` 重编 + 全链接（构建日志 `[1/6] Building ... logx_c.cpp.obj` + `[6/6] Linking`），且探针计数器证明信号路径真实执行。
2. **停驻门 BYPASS 探针非缺陷**：`park BYPASS h=...CAF0` 为写线程对 `space_event` 的等待命中旁路（停驻门仅拦截 `flush_event`），属预期路径；flusher 的 `PARKED`/`RELEASED` 按设计出现。
3. **真机 178 vs 主机 177 用例差**：+1 延续历史「真机恒多 1 个设备专属用例」（R33 已核，本次数量变化与新增测试数吻合）。
4. **真机时钟**：`auto_time 0` 维持，3s 步进实测 3.05s ≈ 1:1，无 NTP 阶跃复发。
5. **停驻门死锁风险排除**：Flusher 停驻于门闩时不持有任何实例锁；`release_flusher()` 由主线在 `logx_flush` 前调用；红路径兜底（closing + 人工 signal）在实验 A/B' 中均于 ~1.5s 内干净解堵。

---

## 六、 验证局限（必须说明）

1. 平台覆盖：桌面 win32/MSVC + 真机 arm64/bionic；Linux 无环境未验证。
2. 警告口径：「0 警告」仅 MSVC；clang 余 1 条存量 unused-function 警告（N-2）。
3. 撤销对照实验均为工作区临时变异 + 增量重建，实验后已还原（工作区干净、目标用例回绿），未产生提交。
4. 真机单测含停驻门版级联用例（Green 路径）；红路径为 MSVC 侧实验取证，未在真机复做（逻辑与平台无关，依赖的 PAL 事件语义两平台一致）。

---

## 七、 审查裁定与后续建议

1. **当前实现评定**：R34 批次产品代码（`logx_format.h` 守卫与 wire 常量、`logx_c.cpp` 警告清理）成立；N-1 屏障经停驻门固化后刚性成立；B-4 与方案 2 的实现经深查无新缺陷；
2. **入库与合并建议**：`3640d8a` 可保留合入（已入库）；
3. **后续建议**：① N-1（wire 断言换 roundtrip）与 N-2（`cleanup_file` 清理）可随下批顺带收口；② 建议将「Android 侧定期构建冒烟」记入流程纪律（防 MSVC 透传包含再次掩盖跨工具链断裂）；③ R32 报告遗留的方案 2 已落地，其「高并发收益」尚无量化数据，可选在 bench 中补 NO_SPACE 等待延迟对照。
