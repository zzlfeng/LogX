# B-5 / B-12 负载实验证据资产

> 转正日期: 2026-10-11（用户拍板「scratch 证据转正」；计划表 v1.58）
> 来源: 仓库上级 scratch `test_review_tmp_b5/`、`test_review_tmp_b12/`（原样保留未删除）
> 剔除: 5 个二进制（`b5_probe.exe`/`b5_probe2.exe` 及 3 个 `.obj`）——可经 `b5/build_probe*.bat` 重建

## 1. 目录与出处

| 目录 | 内容 | 关联提交 |
|---|---|---|
| `b5/` | B-5 前置协议全套：探针源码与自检日志（`b5_probe*.cpp/.log`、`build_probe*.bat`）、R1 零改动循环（`r1/`、`r1_loop.sh`）、R2 负载 rig（`r2_loop.sh`/`r2_repeat.sh`/`r2_batch.sh`、`r2/`）、probe2 六万轮对照（`r2_probe2/`）、12 负载轮档案（`r2_run2..12/`，**含 B-12 原始命中证据 `r2_run5/`**） | 协议收口 `8ea0820`（登记 `5aa9e3b`）；B-12 定性 `26d3752` |
| `b12/` | B-12 闭环 R2c 复跑全套：rig（`r2_loop.sh`/`r2_batch.sh`，与 b5 R2 形态 1:1）、共享工作区（`r2/`）、18 负载轮档案（`r2c_run1..18/`） | R38 收口批次 `f5db3d7`；R38 报告 `ab37c9e` §九.3 |

## 2. 判读结论（详见计划表 B-5 / B-12 条目与 R38 报告）

- **B-5**（`PalRmdirErrnoDistinction` 间歇性失败）: R1 200 + R2 十二负载轮×300 = 3800 轮 0 命中 → 判定门走向方案 C（维持「已知间歇性红」定性）。
- **B-12**（`RetentionCleanup_ClockJumpForward20Years` 负载下文件可见性异常）: 原 R2 撞出 2/12 轮命中（`r2_run5/full_suite_1.log` 含 `RetentionCleanup...FAILED` 与 `[B12] raw_lasterror=2` 取证）；定性为用例创建循环与后台 Flusher 跨天 retention 巡扫的时序竞态，产品零缺陷；`b1559e7` 修复（36500 天建档 + InstGuard）后 R2c 复跑 18 负载轮三判读面全零（RetentionCleanup 负载下 18/18 完整执行 0 命中，与原归档面 18 次完整执行同量级；前台 PalRmdirErrno 5400 轮 0 命中）。

## 3. 复现与注意

- rig 语义: 背景全量单测+hotpath+stress 死循环陪跑，前台 filtered 300 轮；前台先结束即 kill 背景（kill 竞速下**每负载轮 ≈1 次完整套件执行**，判读按「每轮 1 次观测」估）。
- 复跑必须建独立 scratch/archive 目录——**不得覆盖本目录与原 scratch 命中证据**（B-12 教训，R38 报告 §九.3）。
- 负载实验 fatal-ASSERT 失败会泄漏实例污染后续观测；判读须区分「根因异常」与「泄漏级联」（InstGuard 后级联已切断）。
- 负载轮会在构建/运行目录留下 `logx_test_*` 残留测试目录（session.log 前后清点有记录），属预期现场，非资产的一部分。
