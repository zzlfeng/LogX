# logx 代码审查报告 (R19)

> **审查对象**: logx 仓库 `5d7ac34`（2026-10-04，R18 修复 + 门禁批次后的 HEAD；审查前工作树干净，审查后仍干净）
> **审查日期**: 2026-10-04
> **审查范围**: ① R18 两笔提交（`a507030` 修复 + `5d7ac34` 门禁）逐行独立复核 + 反向/变异对照实验；② 全模块深度审查（flusher / logx_c / staging+recovery / JNI+Kotlin / decoder+PAL 五路并行）；③ 对高危发现自建探针**端到端复现**（不复用仓库测试桩，避免"测试自证"）
> **方法**: 独立全量重建（绕开沙箱内挂死的 ninja，直连 `cl.exe`/`lib.exe`/`link.exe` 复跑 55 步）→ 全量测试 → 自建 `r19_probe.exe` 对照实验 → 变异测试。实验产物全部清理。

<!-- ============================================================
【独立复核批注 · 2026-10-04 · 逐项读码核验（与报告作者独立）】
复核方式：不沿用本报告结论，直接对 `5d7ac34` 源码逐条重验全部
15 项发现 + 7 项排除疑点（未复跑构建/探针，实验性声明除外）。

总体裁定：**报告高度准确，核心发现全部成立。**
- F-1/F-2 两个 P0 事实链逐环核对无误（split_impl 中部缝短路吞块、
  回调内 close 的 use-after-free，均与源码一致）；
- F-3~F-6、F-10~F-15 全部属实，行号/机制/影响面均核对无误；
- X-1~X-5 排除理由与源码一致；N-1 经 git show 实测确为 7 行。

两处修正（详见文中各处 HTML 批注）：
① F-7"无任何 Kotlin 可见信号"表述过强——tag 非法路径仍会发
   LOGX_EV_DROP 事件，真实缺口是"无返回码 + 无计数器"，
   应与 §四 对子代理的复核口径统一；
② F-8 需单日 ≥10 分卷（默认 10MB/卷即 >100MB/日）才触发，
   建议由 P1 降为 P2，并入 F-15 decoder 批次。

另注：F-2 的崩溃面限定于直接使用 C ABI 的调用方；经 Kotlin facade
（LogX.kt 的 pendingCloseHandle 递延销毁）不会触发，报告 §三·2
"Flusher 线程次生面"的读码推断成立。
============================================================ -->


---

## 一、总体结论

**R18 的两项核心声明全部属实**：F-1 修复是承载性修复，其"栈毒化"回归屏障确有必要（撤销修复后不毒化依然全绿，见 §二·1）；`5d7ac34` 新增的 retention 用例是**真实双向屏障**（三个变异方向中两个报红，见 §二·2）。

但本轮在**核心生产路径**上发现了 R1~R18 均未触及的缺陷，并全部端到端实证：

| 编号 | 级别 | 位置 | 一句话 | 验证状态 |
|---|---|---|---|---|
| F-1 | **P0** | `core/src/flusher.cpp:47-83`（`split_impl`） | 回绕缝落在拷出缓冲**中部**时，缝与其后全部记录被封成一块（实测 `raw_len=32931`，契约上限 20486），超出读侧上限 → zstd 下整块永久不可解，导出仍返回 `LOGX_OK` | **端到端实证 + 对照实验** |
| F-2 | **P0** | `core/include/logx_c.h:9-10` + `core/src/logx_c.cpp:457-473,410-427` | 头文件明确许可的"错误回调内调用 `logx_close`"会让调用栈在实例析构后继续使用它 → 实测 `0xC0000005` | **端到端实证 + 对照实验** |
| F-3 | **P1** | `core/src/flusher.cpp:348-355,442-481,580` | 1~31B 的同名 `.log` 文件永久卡死追加目标：`logx_write`/`logx_flush` 返回 `LOGX_OK`、6 个计数器全 0，数据全部丢弃 | **端到端实证** |
| F-4 | **P1** | `core/src/recovery.cpp:54-64,291,303-306` | `ring_scan` 无法跨越 pad<6B 回绕缝，把缝后**全部未提交记录**当撕裂清零（与 F-1 同源，仅发生侧不同） | 读码（未独立复跑） |
| F-5 | **P1** | `core/src/recovery.cpp:124-156` + `flusher.cpp:593-627` | 跨天滚动后盲开目标文件：崩溃窗口内的孤儿块造成**同文件 seqno 重复** → 该日后续块全部不可读，下次启动物理截断 | 读码（未独立复跑） |
| F-6 | **P1** | `core/src/logx_c.cpp:91-108,325-333` | 文件头声明加密 + `key_provider` 恒无密钥：块被降级为**明文**落盘，但导出整文件跳过，只回一行 `[skipped: kid=N missing key]` 且返回 `LOGX_OK` | **端到端实证** |
| F-7 | **P1** | `android/.../LogX.kt:401,391` + `logx_jni.cpp:424-429,350-365` | `flush` 超时码 `-10` 在 JNI/Kotlin 两层被丢弃；>64B 的 tag 被原生拒绝却无任何 Kotlin 可见信号、无计数器 | 读码 + 对抗复核 |
| F-8 | **P1** | `decoder/logx_decode.py:471-474` | 分卷按**字典序**合并（`.log.10` 排在 `.log.2` 前），与 C++ `(date, split_no)` 数值序不一致 | 子代理实测 |
<!-- 【复核批注 2026-10-04 · 独立读码核验】
  F-7：机制与后果成立，但"无任何 Kotlin 可见信号"表述过强——format.cpp 判非法后 logx_write 走
  `emit_event(inst, LOGX_EV_DROP, LOGX_ERR_INVALID_ARG, "invalid log input")`（logx_c.cpp），
  注册了 ErrorHandler 的调用方仍能看到 DROP 事件；真实缺口是①无状态码可查（nativeWrite 返回值
  被 Kotlin 丢弃）②无计数器递增。与 §四 对子代理 F1/F3 的复核口径（"事件可见、缺码缺计数"）
  对齐后，建议正文统一为"事件可见但无返回码、无计数器"。
  F-8：事实正确，但触发条件苛刻——需同一天产生 ≥10 个分卷（默认 10MB/卷即单日 >100MB 日志量）
  才会发生字典序错位，建议降级为 P2，与 F-15 的 decoder 批次合并修复。 -->
| F-9 | **P1** | `decoder/logx_decode.py:371-372,471-472,503` | 文件系统异常直接 traceback 终止整轮解码（C++ 同场景是"跳过该文件 + 继续"） | 子代理实测 |
| F-10 | **P2** | `core/src/logx_c.cpp:297-403` | `logx_open` 全部失败返回都不写 `*out_inst`，违反 `logx_c.h:13` 明文契约 | **端到端实证** |
| F-11 | **P2** | `core/src/flusher.cpp:208-216` | `make_log_path` 128B 栈缓冲长路径静默截断：retention/export 失效，且各分卷同路径 → 达到 65535 停写 | 读码（算术确定） |
| F-12 | **P2** | `core/src/flusher.cpp:539-550` vs `:194-200` | `active_log_fd` 在 `file_close` **之后**才置 -1，并发 flush 可能 fsync 已关闭/被复用的 fd | 读码 |
| F-13 | **P2** | `core/src/recovery.cpp:249-251` | 空/仅头文件被当作"损坏尾部" → `truncated_blocks += 全局 seqno`（虚报丢失 N 块） | 读码 |
| F-14 | **P2** | `core/src/recovery.cpp:174-190` | rebuilt 路径忽略 `sr.truncated` → 复用被丢弃块的 seqno（违反 spec `max+2`） | 读码 |
| F-15 | **P2** | `decoder/logx_decode.py:302` 等 6 项 + `logx_pal.h:9-12` | 见 §三·5：nonce 取回显域、无 `raw_len` 交叉校验、缺 seqno 单调检查、尾部 <34B 静默、整文件读入内存、zlib 分支 | 子代理实测/读码 |
| N-1 | Nit | `CHANGELOG.md` 提交表 | `5d7ac34` 提交信息称"补 R16/R17/R18 共 8 行"，实际新增 **7 行** | 实证 |

**与既往轮次的关系**：F-1 与 F-4 是同一根因（`pad<6B` 回绕缝）在**写侧**与**恢复侧**的两种表现形式，此前 18 轮从未触及；F-2 属"契约许可但实现不安全"的 API 级缺陷；F-3/F-6 属"返回成功但数据没落盘/取不回"的静默失败族——正是本项目一直在猎杀的类型。

---

## 二、R18 修复与门禁的独立复核

### 1. R18 F-1（`append_block` 未初始化栈内存落盘）——**修复承载，屏障有效**

- **撤销对照（本轮实做）**：`git checkout` 回退 `core/src/flusher.cpp` 的 `else` 分支（即恢复未初始化 `salt`），重建后运行 `Flush.AppendBlock_NullSaltOnExistingFile_ReusesFileHeaderSalt`：
  ```
  test_flusher.cpp(2333): error: Expected equality of these values:
    0 vs std::memcmp(bhdr + 22, fhdr + 12, 8)  Which is: 1
  block#1 salt must reuse file header salt, not uninitialized stack
  [  FAILED  ] 1 test
  ```
- **"空洞绿"现象复现（本轮实做）**：在上述**缺陷仍在**的前提下，仅注释掉 `poison_r18_stack();` 调用并重建，同一用例变成 `[ OK ]`——**证明该毒化助手是屏障的承重件，不是装饰**。R18 §三所述"首版不修也绿"的经验事实成立。
- **恢复后**：全量 148/148 绿。结论：R18 F-1 定性准确（P2、无生产调用面）、修复正确、屏障有效，**无引入性缺陷**。

### 2. R18-1 门禁（retention 时钟推后 20 年用例）——**真实双向屏障**

对 `Robustness.RetentionCleanup_ClockJumpForward20Years_*` 做三个方向的变异（每次变异后重建并单跑该用例）：

| 变异 | 语义 | 期望 | 实测 |
|---|---|---|---|
| M2a 删除阈值循环的 `--days;` | 阈值恒为 0，清理过弱 | 红 | ✅ **RED**（`A ancient must be deleted`） |
| M2b 删除 `d < yyyymmdd_now` 守卫 | 检验"该守卫恒冗余"的声明 | 绿 | ✅ **GREEN**（声明属实） |
| M2c 改为 `if (d < yyyymmdd_now)` | 清理过激（错删期内/当天） | 红 | ✅ **RED**（`B within retention must NOT be deleted`） |

M2c 是本轮**自选**变异（非 R18 自证方向），证明该用例确实守住了"R17 事故面"（不得误删期内/当天/未来日期文件），而非只守一个方向。

### 3. N-1（Nit）CHANGELOG 行数

`5d7ac34` 提交信息称 "CHANGELOG 提交记录表补 R16/R17/R18 共 8 行真实哈希"，实测新增 7 行（R16 五项 + R17 一项 + R18 一项）。因提交无法记录自身哈希，7 行即上限，正文表述应改为 7 行。

---

## 三、新发现详述

### 1️⃣ F-1（P0）回绕缝位于缓冲中部时 `split_impl` 产出超契约块，zstd 读路径据此丢弃整块

**事实链（每一步都已核对/实测）**

1. 环形缓冲写满尾部时若剩余不足 6B，`write_records` 走"留零回绕"分支（`staging.cpp:326-333`），在环尾留下 **1~5B 全零缝**并 `w=0` 继续写。
2. `copy_pending` 把 `[f,size) ∪ [0,w)` **连续拼接**成拷出缓冲，缝因此位于缓冲区**中部**。
3. `split_impl`（`flusher.cpp:47-83`）判定"撕尾"的唯一手段是 `peek_record_len(...) == 0`（`:58-63`）；缝处 magic 为 0 必然命中该分支，于是**从块首一直封到 `total`**——把缝**及其后的全部记录**吞进同一块。实测：
   ```
   [probe] raw = 32931 bytes (100 recs | 3B seam | 292 recs)
   [probe] split_into_blocks -> 1 block(s); contract: raw_len <= kBlockMaxRaw=20486
   [probe]   block[0] raw_offset=0 raw_len=32931 <<< VIOLATES kBlockMaxRaw
   ```
   契约出处：`logx_internal.h:112-122`（"每块 raw ≤ kBlockMaxNormal"，`logx_staging.h:230-231` 定 `kBlockMaxRaw=20486`）；spec 要求"缝归入当前块 raw_len **并在该处封块**"。
4. 该超限块**无任何写入侧拦截**（`append_block_raw`/`encode_block` 不校验 `raw_len` 上限），带合法 CRC 落盘；读侧 `block_header_plausible` 允许 raw_len ≤ 16MB 故放行，随后 `decompress_block` 以 `kDecompressOutputMax = kBlockMaxRaw + 64 = 20550` 拒绝（`codec.cpp:43-44,106-108`）。
5. **端到端实测**（`append_block` 以 zstd flag 落盘该块 → `logx_export`）：
   ```
   [export_cb] plaintext: 20 bytes, 2 lines, 1 line(s) contain "decode fail"
   [export_cb] head(200): [seq=1 decode fail]
   [probe] logx_export(20261004)=0  (expect 0 = LOGX_OK)
   ```
   **对照实验（同字节、改 kAlgoNone 落盘）**：
   ```
   [export_cb] plaintext: 67816 bytes, 393 lines, 0 line(s) contain "decode fail"
   [probe] logx_export(20261004)=0
   ```
   同字节非压缩路径可完整解出 393 行 → 丢失**唯一**归因于压缩读路径上限，而非数据本身损坏。

**影响面与定级理由**：zstd 是 Android 出货配置（`CMakeLists.txt` 对 `ANDROID` 默认 ON），此刻起**任何一次"缝 + 其后积压 > 20550B"的 drain 都会把该 drain 中缝之后的全部记录永久写死**：块已落盘且 CRC 合法，却永远解不开，`logx_export` 仍报 `LOGX_OK`。命中条件 = ① 回绕时尾部剩 1~5B（概率低但随写入量单调累积）；② 该 drain 的"缝前累计 + 缝后积压"超过 20550B（默认 8KB 唤醒阈值 + 75ms 防抖下，活跃应用极易满足）。定 **P0**：不可恢复的数据破坏 + 静默成功。

**修法建议**
- 正解：把缝的位置/长度从产生处传给切分器——`copy_pending` 本就知道回绕点，应输出"缝偏移/缝长"，`split_impl` 在该处封块（缝计入前一块 `raw_len`，缝后记录另起新块），与 spec 措辞一致。
- 兜底：`split_impl` 增加硬约束——块 `raw_len` 不得超过 `kBlockMaxNormal`/`kBlockMaxRaw`（超过即在该边界前封块），并把该不变式写成单测断言，防未来再次静默破约。

### 2️⃣ F-2（P0）契约许可的"错误回调内 `logx_close`"导致 use-after-free 崩溃

**契约事实**（`logx_c.h:9-10`）：
```c
 *   - logx_set_error_handler 与回调运行在调用方线程;错误回调内不得调用除
 *     logx_close 之外的任何 logx 接口。
```
即 **`logx_close` 是文件明确许可的唯一可调用接口**；Android 侧 `LogX.kt:515-517` 也写明支持"ErrorHandler 内部同步调用 close()"。

**实现事实**：`logx_write` 在 `:466` 于**调用方线程、锁外**发 `LOGX_EV_TRUNCATED`，随后继续执行 `:470 inst->pal->mutex_lock(inst->mu)`、`:473 inst->staging->write_records(...)`；而 `logx_close`（`:410-427`）会 `delete inst->staging` 并把 `staging` 置 nullptr、`mutex_destroy`、`delete inst`。回调返回后调用栈即在这块已释放内存上继续。

**实测（自建探针，两条仅差回调动作的路径）**：
```
=== MODE: nonecb（回调空实现：对照）===
[cb] kind=5 code=0 msg=log truncated (dropped 3922 bytes)
[probe] SURVIVED: logx_write returned 0            ExitCode=0

=== MODE: closecb（回调内 logx_close）===
[cb] kind=5 code=0 msg=log truncated (dropped 3922 bytes)
[cb] -> logx_close() from inside the error callback (logx_c.h:9-10 permits exactly this)
[cb] -> logx_close returned 0; control returns into logx_write, which continues on freed memory
（进程终止）ExitCode=-1073741819 = 0xC0000005 (访问违例)
```
**每一次**（非偶发）触发；`logx_close` 自身返回 `LOGX_OK`，崩溃发生在**它返回之后**的 `logx_write` 栈帧。

**Flusher 线程上的次生面**（读码推断，未实测）：`drain_once` 十余处 `emit_event` 都在 Flusher 线程；回调内 `logx_close` 会先 `flush_blocking`（同一线程等待自己提交 → 5s 超时 → 再发事件 → 递归 close），或在事件位于 commit 之后时 `thread_join(自己)`（Windows `WaitForSingleObject` 永久死锁）。Kotlin 侧的递延销毁依赖 `threadOpDepth>0`，而原生 Flusher 线程 depth 恒为 0，兜不住这条路径。

**修法建议**（择一，不可只改注释了事）
- **A（推荐）**：实现"回调内 close 递延销毁"——`emit_event` 期间置 `in_callback` 标志；`logx_close` 检测到自己处于回调/派生调用栈内时，仅置 `close_pending` 并立即返回；由**最外层** API 帧（或 Flusher 循环退出处，视调用线程而定）完成真正的 join/析构；Flusher 线程内绝不 `join` 自身。
- **B（次选）**：收缩契约——改为"错误回调内不得调用任何 logx 接口（含 `logx_close`）"，同步修改 `LogX.kt` 的支持性表述，并把该约束写入门禁；此方案代价是削弱现有能力，且需确保 Android 侧不再宣传该用法。

### 3️⃣ F-3（P1）1~31B 的同名日志文件永久卡死追加目标，且 `flush` 报成功

`scan_one` 对 `sz < 32` 走"文件过短，视为空文件（将由 `write_file_header` 覆盖）"（`flusher.cpp:348-355`，**repair=true 时也不截断**）；但 `write_file_header` 只在 `sz == 0` 分支可达（`:877`），且 `open_or_create_append_target` 的"已存在"分支要求 `file_pread(fd, hdr, 32, 0) == 32`（`:442-481`），`sz ∈ [1,31]` 时恒 `!= 32` → 永久返回 -1。跨天重置已把 `current_split/current_size` 归零，切分分支（`:605`）永不进入，因此也不会失败切换到 `.log.1`。

**实测**（预置 5B 的 `<today>.log`，随后 100 条写入 + flush）：
```
[probe] created ...\20261004.log with 5 bytes (torn file header)
[cb] kind=3 code=-1 msg=append target open failed
[probe] after 100 writes + flush: logx_flush=0 file_size=5
[probe] stats: drop_no_space=0 block_write_errors=0 drop_too_large=0 truncated_logs=0
[probe] logx_export=-3
```
数据全部丢弃、**flush 返回 `LOGX_OK`**、6 个统计计数器**全 0**，唯一信号是每条块一次 `LOGX_EV_IO`。产生源真实存在：断电撕裂的 32B 文件头，或短写后补偿 `file_truncate(fd,0)`（`:269-273`）同样失败。

**修法建议**：`scan_one` 对 `0 < sz < 32` 视为"坏头/可修复"，`repair=true` 时 `file_truncate(fd, 0)`（与 `get_u32(hdr)!=kMagicFile` 分支同款纪律）；`open_or_create_append_target` 一并按"小于 32B 即视为空文件并重写头"处理；同时在 `drain_once` 的丢弃分支递增 `block_write_errors`（当前该分支不计数）。

### 4️⃣ F-6（P1）加密开启 + 密钥恒缺：明文落盘却"导出取不回、状态码成功"

**实测（`encrypt_enable=1` + `key_provider` 恒返回 NULL）**：
```
[probe] logx_open(encrypt=1, provider->NULL) = 0        ← open 期无任何密钥可用性校验
[cb] kind=1 code=-1 msg=missing key; downgrading block to plaintext
[probe] logx_write=0 logx_flush=0 ; stats all-zero
[probe]   FILE  header flag=0x01  encrypted_bit=1  kid=7    ← 文件头声明加密
[probe]   BLOCK header flag=0x00  encrypted_bit=0          ← 块实为明文
[cb] kind=1 code=-1 msg=export: missing key for kid
[export_cb] content: [skipped: kid=7 missing key]
[probe] logx_export(20261004) = 0                        ← 仍报 LOGX_OK
```
三个独立问题叠加：
1. **契约失实**：`logx_c.h:81` 称 `key_provider` "init(恢复解密)与 export 路径均会调用"，实际全仓仅 3 个调用点（`flusher.cpp:687`、`logx_c.cpp:100`、`:137`），**init/recovery 从不调用** → `logx_open` 无法对该确定性降级 fail-fast，与 `logx_c.cpp:311-313` 自述的 fail-fast 目的（"仅用于运行时偶发失败"）矛盾。
2. **静默明文**：`flusher.cpp:686-694` 把块降级为明文（仅一条 DEGRADED 事件），`logx_write`/`logx_flush` 均返回 OK、计数器全 0。
3. **导出侧断裂**（F-01 的镜像未修）：导出见文件头 `encrypted_bit=1` 即整文件放弃（`logx_c.cpp:91-108`），连**根本不需要密钥**的明文块也永不输出，最终返回 `LOGX_OK`，调用方无法区分"当天无日志"与"文件被跳过"。

**修法建议**：① 修正 `logx_c.h` 的 `key_provider` 契约表述，或在 `logx_open`（`encrypt_enable=1`）时对 `inst->kid` 做一次探测调用并 fail-fast；② 导出侧改为**逐块判定**：块头 `encrypted_bit=0` 的块无条件解码，仅对加密块走取 key 逻辑；③ `write_file_header` 的加密位应与"本文件实际首块是否加密"一致，或在取不到密钥时拒绝建文件（与 F-09 的"拒绝创建低熵文件"同款纪律）。

### 5️⃣ 其余 P1/P2 提要（含子代理证据）

- **F-4（P1，读码）** `ring_scan`（`recovery.cpp:54-64`）的"距段尾 <6B"判定作用于**扁平视图长度**，而环形段界未传入（`:291` 调 `ring_scan(view,0,raw_bytes,...)`）；缝在视图中部时，扫描读到"缝零字节 + 回绕后首记录"的 6B 窗口 → magic 失配 → `valid_end` 停在缝处，`:303-306` 把其后的**全部未提交记录 `memset` 清零**并提交为新 `write_pos`。与 F-1 同一根因（缝），但发生在恢复侧且**更严重**（静默抹除、无事件无计数）。建议与 F-1 合并修复：`ring_scan` 需接收环段边界，实现 spec 的"跳过 <6B 到段尾、回绕后继续"。
- **F-5（P1，读码）** `reconcile` 只扫 ATC 提示文件（`recovery.cpp:124-156`），跨天后 ATC 仍指向昨日文件 → 今日文件中的孤儿块不被发现，Flusher 盲开该文件（`:622-627` 无 scan）→ 同一文件出现重复 seqno → 该日文件自重复点起全部不可读（`block_header_plausible` 强制严格递增），且下次启动 repair 会**物理截断**掉后续数据。修法：reconcile 在"提示文件日期 < 最新文件日期"时回退 `scan_dir_for_target`（spec 本就把 ATC 定义为 O(1) 快路径）。
- **F-7（P1，读码+对抗复核）** `LogX.flush()`（`LogX.kt:401`）+ `nativeFlush`（`logx_jni.cpp:424-429`，返回 void）丢掉了 `-10`（`LOGX_ERR_TIMEOUT` 在 Kotlin 全仓不可达；错误通道仍会发 `LOGX_EV_IO`，故**不是**"应用完全无感"，属诊断能力缺口）；>64B 的 tag 由 JNI 接收（超长走 `GetStringUTFChars`，无上限）后被 `format.cpp:73-79` 判非法 → 记录静默丢弃、无计数器、Kotlin 无返回码可查（`logx_jni.cpp:350-365`）。
  <!-- 【复核批注】tag 非法路径 `logx_write` 返回 `LOGX_ERR_INVALID_ARG` 前会同步发
  `LOGX_EV_DROP` 事件（logx_c.cpp，调用方线程、锁外），注册了 ErrorHandler 的调用方可见。
  故本条与上面 `-10` 子项应统一表述为："事件通道仍可见，缺的是①JNI/Kotlin 返回码
  ②丢弃计数器"。"记录静默丢弃"不宜与"-10 被丢弃"并列暗示两层都无信号。 -->
- **F-8/F-9（P1）** Python 离线解码器：① 分卷按字典序合并（`.log.10` 早于 `.log.2`）；② `os.listdir`/`open` 无任何异常保护，目录名撞 `^\d{8}\.log$`、`log_dir` 传普通文件、或与运行中的 retention 删文件竞争时，**整轮解码 traceback 终止**（C++ 同场景契约是跳过并继续）。
  <!-- 【复核批注】F-9 事实成立；F-8 触发条件需单日 ≥10 分卷（默认 10MB/卷 → 单日 >100MB），
  建议降为 P2 并与 F-15 decoder 批次合并修复。 -->
- **F-10（P2，实测）** `logx_open` 的 6 条失败返回均不写 `*out_inst`；实测复用句柄变量后 `out_inst` 仍指向**上一个存活实例**（`logx_c.h:13` 要求失败时置 NULL），按契约书写的 `else { logx_close(h); }` 会误关旧实例。
- **F-11（P2）** `make_log_path` 的 `char buf[128]`（`flusher.cpp:208-216`）在 `len(log_dir) ≥ 115` 时截断 `.log` 后缀：retention/export 因 `parse_log_filename` 失败而永不处理该文件，且**各分卷返回同一路径** → 存在性探测循环推到 65535 → `kAppendTargetSplitExhausted` 停写全部丢弃。
- **F-12（P2）** `close_current` 先 `file_close` 后 `store(-1)`（`flusher.cpp:544-547`），而 `durability_sync` 从任意线程读 `active_log_fd`（`:194-200`，经 `flush_blocking` 可达）→ 可能对已关闭/被复用的 fd 做 fsync（虚假 IO 事件或同步到无关文件）。一行修复：`exchange(-1)` 或先 store 后 close。
- **F-13/F-14（P2）** 空/仅头文件被误判为损坏尾部（`truncated_blocks += S_hdr`，虚报 N 块丢失且不发事件）；rebuilt 路径忽略 `sr.truncated` 而复用被丢弃块的 seqno（违反 spec"实际截断则首块 seqno = max+2"，加密下即 nonce 复用面）。
- **F-15（P2，decoder/PAL 其余 6 项）**：nonce 取块头回显域 `bhdr[22:34]` 而非由文件头 salt 派生（损坏/历史文件上与 C++ 分歧）；无 `raw_len` 与解压输出长度交叉校验（C++ 会拒绝整块）；`block_header_plausible` 缺 seqno 严格递增检查；块尾 <34B 撕裂完全静默（rc=0）；整文件 `f.read()`（内存 = 文件大小，4GiB 分卷即 MemoryError）；algo=2 分支接受 C++ 拒绝的块且不校验 `d.eof`。另有 `logx_pal.h` 使用 `std::vector` 却未 `#include <vector>`（靠传递包含侥幸通过，换标准库即编译失败）。

---

## 四、Android 8 项发现的对抗复核裁定（子代理提出 → 独立证伪复核）

| 原编号 | 原级别 | 复核裁定 | 修正要点 |
|---|---|---|---|
| F1 flush 丢 `-10` | P1 | PARTIALLY（**P2**） | "应用完全无感"不成立：`emit_event(LOGX_EV_IO)` 仍会到 ErrorHandler；真实缺口是错误码不可区分、且需注册回调才可见 |
| F2 `nativeInit` 缺 `ExceptionCheck` | P1 | PARTIALLY（**P2**） | "事件永久静默丢弃"不成立：`LogX.kt:316-330` 的 fail-fast 会抛 `IllegalStateException` 并回滚单例，可重试自愈；真实问题是 pending 异常污染同一次 init 的后续 JNI 调用 |
| F3 >64B tag 静默丢弃 | P1 | **CONFIRMED（P1）** | 但"JNI 窗口 ≤127B"表述有误：超长走 `GetStringUTFChars` 无上限；机制与后果（无计数器、无返回码）属实 |
| F4 `tag_buf[utf8_len]` 缺下界 | P2 | PARTIALLY（nit，不可达） | `GetStringUTFLength` 按规范/实现返回非负，构造不出负值 |
| F5 `nativeExport` 缺 `ExceptionCheck` | P2 | CONFIRMED（低影响） | Kotlin 侧 `ExportCallback` 类型保证 `GetMethodID` 不会失败；且 `-1` 契约仅承诺"未初始化/已关闭"，原文表述不准 |
| F6 `GetByteArrayRegion` 未检查 | P2 | **REFUTED（后果）** | 区间恒在界内、空载荷另有分支，无法产生"未初始化字节落盘"；可留作加固 |
| F7 `kid`/`flushThreshold` 静默收窄 | P2 | PARTIALLY（**P2**） | "静默明文降级"不成立：降级发 DEGRADED 事件、无 provider 时 open 期即拒绝；真实问题是两侧都缺范围校验（仓库自己的 tester UI 反而 `coerceIn(0,65535)`） |
| F8 Kotlin KDoc 自相矛盾 | P2 | CONFIRMED（文档） | `LogX.kt:28`"超过被丢弃"与 `:103-107`"截断保前缀落盘"冲突，行为是后者；一行文档修复 |

> 复核结论：**8 项中 0 项 P0**；2 项完全成立、5 项部分成立（后果普遍被高估）、1 项后果被证伪。这正是本项目"发现即对抗复核"纪律的价值所在。

---

## 五、已排除疑点（本轮核对，不构成缺陷）

| # | 疑点 | 判定 |
|---|---|---|
| X-1 | `split_impl` 的常规累加是否会把块做到 16KB 上限之外？ | **正常路径不会**：`:76` 的 `cur + rec - blk_begin > kNormalBlockMax && cur > blk_begin` 按记录边界封块；F-1 的超限**只**来自 `:75` 的 `rec == 0 → cur = total` 短路 |
| X-2 | 回绕缝是否会被 `write_records` 的 `pad ≥ 6B` 分支消化掉？ | **不会全消**：`pad < kHeaderLen` 时按设计留 1~5B 零缝（`staging.cpp:326-333`），该缝必然存在（F-1/F-4 的触发前提） |
| X-3 | `append_block` 的 R18 修复是否引入"文件头 salt 与调用方 salt 不一致"的新风险？ | **无**：`:888-893` 保留了对非空 `file_salt` 的一致性校验，两条路径现在都从文件头取值/校验 |
| X-4 | `retention_cleanup_impl` 的阈值循环是否会因为 `days` 极大而死循环？ | **不会**：`threshold > 0` 为循环条件，最多退到公元 1 年（约 74 万次迭代）后 `break`，有界 |
| X-5 | `logx_write` 的丢弃/截断路径是否会在持锁状态下回调（自死锁）？ | **否**：`:449/:454` 在加锁前、`:499` 在解锁后发事件，`emit_event` 亦在锁外执行回调（E-01 在位）；F-2 的问题是**生命周期**而非锁 |
| X-6 | 全量重建是否存在告警/错误被忽略？ | **否**：55 步全量重建（含 36 个 core 目标文件归档 + 4 个可执行文件链接）`BUILD_ALL_OK`，`warning` 计数 **0** |
| X-7 | 上一轮结论"核心产品代码零 P2 缺陷"是否仍成立？ | **不成立**。本轮 F-1/F-3/F-6 均在核心生产路径且已实证，F-1 为 P0 |

---

## 六、验证记录

| 项 | 命令/环境 | 结果 |
|---|---|---|
| 构建 | MSVC 14.43 /std:c++17 /utf-8 /O2 /MD，`LOGX_WITH_ZSTD=ON`、`LOGX_WITH_AESGCM=ON`；沙箱内 ninja 1.13(jobserver-pipe) 挂起，改用 `ninja -t commands` 取证的原始命令行直连 `cl/lib/link` 复跑全部 55 步 | **BUILD_ALL_OK**，0 warning / 0 error |
| 单测 | `build-full\tests\unit\logx_unit_tests.exe` | **148/148 PASSED** |
| 热路径 | `logx_hotpath_alloc_tests.exe` | **2/2 PASSED** |
| 压力 | `logx_stress_tests.exe` | **4/4 PASSED**（50 轮崩溃注入对账 **397/397**） |
| 对照实验 A | 撤销 R18 F-1 修复 → 跑其回归屏障 | ✅ RED（`memcmp`=1，block#1 salt 被污染） |
| 对照实验 B | A + 移除 `poison_r18_stack()`（缺陷仍在） | ✅ **GREEN**（复现"空洞绿"，证明毒化是承重件） |
| 变异 M2a/M2b/M2c | retention 用例三方向变异 | RED / GREEN(冗余守卫属实) / RED |
| 探针 C | `split_into_blocks`（100 记录+3B 缝+292 记录） | 1 块，`raw_len=32931` > 契约 20486 |
| 探针 D | 该块以 zstd 落盘后 `logx_export` | 仅 `[seq=1 decode fail]`，返回 `LOGX_OK` |
| 探针 E | 同字节以 kAlgoNone 落盘后导出（对照） | 67816B / 393 行 / 0 处 decode fail |
| 探针 F | 回调内 `logx_close`（vs 空回调对照） | **0xC0000005**（对照 ExitCode=0） |
| 探针 G | 预置 5B `<today>.log` + 100 次写入 + flush | `flush=LOGX_OK`，文件仍 5B，计数器全 0 |
| 探针 H | `encrypt=1` + provider 恒 NULL | 文件头 encrypted=1 / 块 encrypted=0 / 导出 `[skipped]` + `LOGX_OK` |
| 探针 I | 失败 `logx_open` 后 `*out_inst` | 仍指向旧存活实例（契约要求 NULL） |
| 收尾 | 实验产物（`build-probe*/`、`r19_*.bat/log`）全部删除；`git status --porcelain` | **0 行变更**（工作树与 `5d7ac34` 一致） |

> **环境备注**：`ninja`（1.13.0.git.kitware.jobserver-pipe-1）在本沙箱下每次派发 8 个 job 后即挂起零输出（与 R18 记录一致），故本轮沿用"导出 vcvars64 → 按 `ninja -t commands` 的原始命令行逐条调用编译器/归档器/链接器"的等价构建。未复跑：另两个构建矩阵（OFF/OFF、矩阵 zstd）、Android arm64 交叉编译与真机（沿用既有共同局限）。

---

## 七、修复顺序建议

1. **F-1（P0，最先修）**：`split_impl` 按缝封块 + 块 `raw_len` 硬上限兜底；补回归用例——构造"缝在缓冲中部且缝后积压 > 20550B"的 raw 流，断言**块数 ≥ 2 且每块 `raw_len ≤ kBlockMaxRaw`**，并断言 zstd 解码可完整取回全部记录。（该用例天然是"反空洞绿"型：先撤销修复跑一遍，必须见红。）
2. **F-2（P0）**：实现"回调内 close 递延销毁"（Flusher 线程绝不 join 自身），或收缩契约并同步 Kotlin 文档；补回归用例——回调内 close 后 `logx_write` 必须安全返回而非崩溃（ASan 下无 use-after-free）。
3. **F-4（P1）**：与 F-1 合并修——`ring_scan` 接收环段边界，实现"跳过 <6B 到段尾、回绕后继续"；`staging.cpp:326-333` 的缝语义需在写/读/恢复三侧统一成一条不变式并写入契约。
4. **F-3 / F-6（P1）**：`scan_one`/`open_or_create_append_target` 把 `0 < sz < 32` 当可修复坏头处理；导出侧改为**逐块**判定加密位；`logx_open` 增加密钥可用性 fail-fast（或修正 `logx_c.h` 契约）。
5. **F-5（P1）**：reconcile 在提示文件非最新时回退目录扫描，杜绝"同文件重复 seqno"。
6. **F-7~F-9（P1）**：JNI 返回 `logx_flush` 状态码（或至少区分超时事件码）；tag 上限在 Kotlin/JNI 侧显式校验 + 计数；Python 解码器修分卷排序并逐文件 try/except。
   <!-- 【复核批注】F-8 降为 P2 后，本条建议中"修分卷排序"可与第 7 条 decoder/PAL 批量合并；
   其余两项（flush 状态码、tag 显式校验）维持 P1 不变。 -->
7. **P2 批量**：`*out_inst` 归零（F-10，一行×6 处）、`active_log_fd` 先置 -1 再 close（F-12，一行）、`make_log_path` 扩容/校验 `log_dir` 长度（F-11）、`truncated_blocks` 空文件语义（F-13）、rebuilt 路径 `+2`（F-14）、decoder/PAL 六项（F-15）、三个文档修正（`logx_c.h:81` key_provider 契约、`LogX.kt:28` 截断表述、CHANGELOG"8 行"→7 行）。
8. **门禁补充建议**：
   - **不变式门禁**：把"块 `raw_len ≤ kBlockMaxRaw`"`写侧产出 == 读侧上限`"这类**跨模块常量一致性**做成 `static_assert` 或单测（F-1 本质是写侧能产出读侧必然拒绝的东西，两侧各自自洽却互不兼容）。
   - **契约门禁**：凡头文件写明"许可某调用"（如回调内 close），必须有对应单测覆盖该组合——本轮 F-2 正是"文档许可、测试缺失、实现不安全"的典型。
   - 沿用 R18-1/R18-2 两条门禁；本轮 R19 的探针式验证（自建探针而非复用仓库桩）建议固化：**高危缺陷的复现程序不得依赖被测仓库自身的测试辅助代码**，否则共享的错误假设会让两侧同时"看不见"缺陷。
