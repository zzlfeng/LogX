# LogX 离线解码与线上排障手册

> 对应计划表任务 T5.3；接入配置见 [`docs/integration_guide.md`](./integration_guide.md)。
> 读者：接入手完成后的值班/排障同学。文中工具行为与 `decoder/logx_decode.py`（2026-10-11 基线）逐项核对过。

## 1. 取证流程

### 1.1 取什么

问题发生后**尽早**取走整个 `logDir` 目录（默认 `files/logs/`），包含：

| 文件 | 内容 | 说明 |
|---|---|---|
| `YYYYMMDD.log` | 当日密文/压缩分卷 | 解码主对象 |
| `YYYYMMDD.log.1` `.2`… | 同日切分分卷 | 单文件超 `maxFileSize` 产生 |
| `.tmp_export_*` | export 遗留明文临时文件 | 正常应为 0 个；发现即说明有 export 异常中断 |

staging 文件（`files/logx_staging[/]/logx_staging[_后缀].bin`）**离线解码用不到**（解码只吃正式分卷），但怀疑崩溃丢失窗口时一并取走，供库作者分析恢复对账。

### 1.2 怎么取

```bash
# debuggable 构建：run-as 直接打包拉取
adb shell "run-as <pkg> tar -cf - files/logs" | tar -xf -   # Git Bash
# 或单文件
adb shell "run-as <pkg> cat files/logs/20261011.log" > 20261011.log

# release 构建（不可 run-as）：走应用内导出（LogX.export 上传通道）或系统 bugreport
```

配合应用内埋点时，同时记录 `LogX.getStats()` 六项计数与最近一次 `ErrorHandler` 事件流——这是判断「没打出来」还是「打出来没解出来」的第一手证据。

## 2. 离线解码工具 `decoder/logx_decode.py`

### 2.1 用法

```bash
python decoder/logx_decode.py <log_dir> [--from YYYYMMDD] [--to YYYYMMDD] \
    [--kid-map kid=hexkey,kid=hexkey,...] [--out out_dir]
```

| 参数 | 说明 |
|---|---|
| `log_dir` | 正式日志目录（只认 `YYYYMMDD.log[.N]` 命名，按（日期, 切分号）数值排序） |
| `--from` / `--to` | 日期闭区间过滤（按文件名前缀比较） |
| `--kid-map` | KID→32B 十六进制密钥映射，如 `--kid-map 1=aabb…(64 hex chars),2=ccdd…`。密钥不足 32B 直接报错 |
| `--out` | 输出目录（每天一个 `plain_YYYYMMDD.log`）；省略则写到 stdout |

**可选依赖**（按日志配置装）：

| 日志配置 | 需要安装 |
|---|---|
| 明文 / zlib | 无（纯标准库） |
| zstd 压缩 | `pip install zstandard` |
| 加密 | `pip install cryptography` + `--kid-map`（GCM nonce 由文件头 salt + 块 seqno 派生，无需额外输入） |

### 2.2 输出与退出码

明文行格式（与 C++ 导出路径一致）：

```
20261011 1728617600123 pid=12345 tid=12345 [I] Pay: order created
└日期    └epoch 毫秒                └级别    └tag:  消息(UTF-8)
```

- 块级解码失败处输出占位行 `[seq=N crc fail]` / `[seq=N decode fail]` 标记数据空洞（不丢块间健康记录）。
- 诊断信息走 stderr，格式 `[日期] 错误详情`。
- **退出码：0** = 全部干净；**1** = 用法/目录错误/无匹配文件；**2** = 存在解码错误（stdout 输出仍有效，按空洞理解）。

### 2.3 错误分两层（与 C++ 导出语义对齐）

| 层级 | 触发 | 行为 |
|---|---|---|
| 块头**结构**损坏（magic/版本/保留位/seqno 回退/长度越界） | 块边界已不可信 | `bad block header; stop scanning file`——**终止该文件**（宁丢不脏，盲扫会整文件错位） |
| **块级**解码失败（payload CRC / GCM / 解压 / 缺钥） | 单块内容问题 | 占位行 + stderr，**跳过该块继续**，文件后半照常解码 |
| 记录级损坏（cksum 失败/长度越界/magic 失效） | 块内单条 | 跳过或滑动重同步，绝不因单条丢整块 |

文件尾不足 34B 的撕裂残片输出 `trailing torn tail` 警告（多为进程崩溃时未刷完，属崩溃安全设计的正常痕迹）。

### 2.4 常见报错速查

| stderr 关键字 | 根因 | 处置 |
|---|---|---|
| `log_dir not a valid directory` / `[no matching files]` | 目录错/取回不完整 | 核对 §1 取证范围与文件名 |
| `missing key for kid=N` | `--kid-map` 未含该 KID | 补齐历史代际密钥（KID 轮换后旧分卷按文件头 KID 解密） |
| `gcm fail` | 密钥与 KID 不匹配（错代/错环境） | 核对 `--kid-map` 与加密侧 provider 的 KID→密钥映射 |
| `crc32 mismatch` → `[seq=N crc fail]` | 该块落盘内容损坏 | 结合 RECOVERY/truncatedBlocks 判断是否伴随崩溃 |
| `bad file header` / `file too short` | 文件头损坏或截断 0 字节 | 检查取证/传输环节 |
| `decompress fail` | 压缩流损坏或 zstd 依赖缺失 | `pip install zstandard` 后重试 |

## 3. 事件码分级处置表

`ErrorHandler.onError(kind, code, message)` 的五类事件（C ABI `LOGX_EV_*` 1~5）：

| Kind | 触发场景 | 建议动作 |
|---|---|---|
| `DEGRADED` | mmap staging 失败（目录不可写/磁盘异常）；加密分卷缺钥降级（新建分卷**整卷明文**锁定，或续写分卷当期明文待密钥恢复） | 查文件系统；密钥降级要区分「已锁定明文卷」（需换新分卷才恢复密文）与「待恢复」（自动重试） |
| `DROP` | 环满 NO_SPACE（`stagingWaitMs=0` 默认不阻塞）；病态小 maxLogLength TOO_LARGE | NO_SPACE：调小 `flushThreshold`、给 `stagingWaitMs`（如 50~200ms）摊平突发、或减小日志量；TOO_LARGE：修 `maxLogLength` 下界（≥97，init 已 fail-fast，正常不会出现） |
| `IO` | 分卷写短写/失败、文件系统错误 | 查磁盘配额、存储空间、SELinux/权限 |
| `RECOVERY` | 启动恢复对账：staging 撕裂尾丢弃、截尾块计数、异常重建 | 单次崩溃后的少量计数属正常善后；持续大量出现才需要深查 |
| `TRUNCATED` | 单条超 `maxLogLength` 截断保前缀（有损**写入成功**，code=0） | 检查业务侧大 payload（如整包 JSON 打日志），或调大 `maxLogLength`（上界 130942） |

## 4. 症状 → 根因速查

| 症状 | 优先检查 | 机制说明 |
|---|---|---|
| 日志完全缺失 | ① 是否 pre-init 写入（init 前静默丢弃）② init 是否抛异常回滚 ③ 目录是否 `logs_<后缀>/`（多进程隔离漂移，`:push` 进程日志在 `logs_push/`） | init 前调用返回 `ERR_STATE` 不落盘；隔离目录规则见接入指南 §8.2 |
| 重启后最后几秒日志没了 | RECOVERY 事件与 `truncatedBlocks` | 未 flush 的 staging 残留由下次启动恢复对账捞回；若对账也没捞到，staging 文件本身损坏（取原件分析） |
| 部分日志缺 | `getStats().dropNoSpace` 是否增长 | 环满丢弃；突发量 > 吞吐时发生，处置见 §3 DROP 行 |
| 日志持续迟到 | debounce/阈值未触达 | 75ms debounce 聚合窗口 + 8KB 阈值；关键节点手动 `flush()` |
| 单条尾部缺失 | `getStats().truncatedLogs` / TRUNCATED 事件 | 截断保前缀；或仅 logcat 观察到——consoleEcho 单条 >2KB native 截断、logcat 单行 ~4KB 系统截断，都是镜像端假象，落盘数据完整 |
| 分卷数量暴涨 | `retentionDays` / `maxFileSize` / level | 过期清理发生在启动对账与 Flusher 轮次；10MB 默认切分 |
| export 返回 -8 | 日期区间写法 | `yyyyMMdd` 闭区间；时段内确实无分卷 |
| 解码出现 `[seq=N decode fail]` | §2.4 missing key / gcm fail | 多为 KID 轮换后 `--kid-map` 不全 |
| 「开了加密但有的日期分卷解出来是明文」 | DEGRADED 事件时间线 | 新建分卷瞬间缺钥 → 该分卷整卷明文锁定，即便密钥稍后恢复也不再改口（设计如此）；换新分卷自动恢复密文 |
| 启动变慢 | 是否 close→init 热切换 | init 前奏等旧实例 ≤1s + 遗留销毁最坏 ~5s；建议放后台线程 |

## 5. stats 指标精确口径

`LogX.getStats()` / C ABI `logx_stats_t` 六项（累计值，自 init 起）：

| 字段 | 精确口径 |
|---|---|
| `dropNoSpace` | staging 空闲不足以容纳单条记录而丢弃的次数（B-1：`stagingWaitMs>0` 时为有界退避超时后的丢弃） |
| `dropTooLarge` | B-1B 后仅剩病态小上限路径（截断到空记录仍放不下）；正常配置恒为 0 |
| `truncatedBlocks` | 启动恢复发现的截尾块计数。**口径注记**：语义为「staging Header 已提交序号与文件尾块序号的缺口」，Header 撕裂场景下不必然等于真实丢块数——仅作诊断参考，非精确 SLA 指标 |
| `padRecords` | 环形缓冲回绕缝垫 PAD 数；常态噪声（每次 pad<6B 回绕一次），仅用于结构分析 |
| `blockWriteErrors` | 分卷 write 短写/IO 错误计数；增长即有真实磁盘问题 |
| `truncatedLogs` | 超长截断保前缀写入次数（有损写入成功，非丢弃） |

丢失窗口模型：崩溃瞬间最多丢「staging 中尚未 drain 的最后一批」吗？不——staging 是 mmap，崩溃不丢；真正可能丢的只有 ① 环满丢弃（计入 `dropNoSpace`）② 撕裂尾（计入 `truncatedBlocks`，见口径注记）。

## 6. C ABI 直连附录（原生集成者）

完整契约见 `core/include/logx_c.h`（权威注释），此处仅索引：

```c
logx_status_e logx_open (const logx_config_t* cfg, logx_instance_t** out_inst);
logx_status_e logx_close(logx_instance_t* inst);
logx_status_e logx_write(logx_instance_t* inst, int level, const char* tag,
                         const uint8_t* payload, uint32_t payload_len);
logx_status_e logx_flush(logx_instance_t* inst);
logx_status_e logx_export(logx_instance_t* inst, const char* from_yyyymmdd,
                          const char* to_yyyymmdd, logx_export_cb cb, void* cb_user);
logx_status_e logx_set_error_handler(logx_instance_t* inst, logx_error_fn fn, void* user);
void          logx_get_stats(logx_instance_t* inst, logx_stats_t* out);
```

三条最容易踩的契约：

1. **线程模型**：open/close/write/flush/export 任意线程可调；错误回调运行在触发线程，回调内不得调用除 `logx_close` 外的任何 logx 接口。
2. **close 递延语义**：close 在回调或任一活跃调用栈内被调时立即返回 OK，真实销毁递延；close 返回后句柄立即失效不得再用，但在途并发写可能继续完成——需要强顺序时先停写再 close。
3. **key_provider 三类调用点**：open fail-fast 校验 / 建文件·续写时锁定加密能力（**运行在 Flusher 后台线程**）/ export 逐块解密；返回指针由实现方持有，须在同一文件追加生命周期内保持有效、内容稳定。

状态码 `-1 INVALID_ARG / -2 STATE / -3 IO / -4 NO_SPACE / -5 TOO_LARGE / -6 CODEC / -7 NOMEM / -8 NOT_FOUND / -9 INTERNAL / -10 TIMEOUT` 与 Kotlin 层 `LogX.ERR_*` 常量一一对应。

## 7. 与 logx-tester 联动

`android/logx-tester` 是手工验收应用：配置面板可实时调整 `maxLogLength`（含 96/97 边界预警）、压缩/加密开关、flush 阈值等（README 场景表给出逐档验收步骤）。排障用途：

- 复现配置类问题：在 tester 上按可疑配置跑一遍，与业务包行为对比，快速区分「库行为」与「集成差异」。
- 验证修复：调参建议（如 `stagingWaitMs`）先用 tester 真机验证再改业务包。
