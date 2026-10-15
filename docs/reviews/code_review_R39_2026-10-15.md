# LogX 代码审查报告 R39（全量审查）

- **审查日期**: 2026-10-15
- **审查对象**: 全仓库代码（非增量批次审查）——core 全部 8 个源文件与头文件契约层、PAL 双平台实现（`pal_posix.cpp` / `pal_win32.cpp`）、JNI 桥接层与 Kotlin Facade（`logx_jni.cpp` / `jni_string_util.h` / `LogX.kt`）、Python 离线解码器（`decoder/logx_decode.py`）、tester 应用（`android/logx-tester/` 全部 Kotlin）、顶层 `CMakeLists.txt`
- **审查方式**: core 层逐文件全文通读 + 关键不变量语义推演（环形会计、双 Header 提交纪律、恢复对账四分支、nonce 唯一性、切块预算数学）；外围四模块三路并行子代理只读审查；发现与代理结果交叉核对、去重、核销待查项
- **基线**: 工作区当前未提交状态（R38 之后）

---

## 一、总体结论

**核心链路经推导验证闭环成立**：环形会计 `free=(f+size-w-1)%size` 空/满不歧义；双 Header "A 先(中间态 CRC=0 不可读)→seq_cst 屏障→B" 提交纪律与 `pick_header` 读侧规则自洽；启动恢复四分支对账（S_file vs S_hdr、Σraw_len 回推、truncated_blocks 口径、seqno_floor 钳制、跳空隙防 nonce 重用）完备；`split_impl` 切块预算数学安全（单块 ≤ kBlockMaxRawWithSeam=20491）；nonce=salt8B+seqno 大端 4B 全局唯一无重用；解码器与 C++ 写端核心字节契约（块头 34B/记录头 6B/CRC32 约定/GCM 布局/20491 上限）逐项核验一致。

工程亮点：wire 布局编译期断言钉死、错误注入 seam（compress fail / clock）、缺陷编号可追溯注释、EINTR/CLOEXEC/JNI 局部引用/ExceptionCheck 等常规项扎实，PAL 与 JNI 层均未发现 fd/HANDLE/heap 泄漏路径。

**风险集中在三类**：

1. **生命周期递延的跨层假设不一致**——`logx_close` 递延销毁在 core 与 JNI 两层理解不同（J-1/F-2 同族），可达路径真实；
2. **Windows 桌面侧实现欠打磨**——ANSI 路径、mmap 尺寸冻结、Event 创建失败吞噬（P-1/P-5/P-6）；
3. **"声称对齐、实际分叉"的次级不一致**——解码器 carry 语义（D-1）、回调重入契约无硬防护（J-2）、tester 忽略返回码（T-1）。

### 发现统计

| 级别 | 数量 | 编号 |
|---|---|---|
| 高 | 5 | J-1, J-2, P-1, D-1, T-1 |
| 中 | 28 | F-1, F-2, F-9, F-10, F-11, F-12, F-20, F-28, P-2~P-6, J-3~J-7, D-2~D-6, T-2~T-6 |
| 低 | 47 | F-3, F-4, F-13, F-14, F-15, F-25, F-31, F-32, P-7~P-18, J-8~J-19, D-7~D-13, T-7~T-14 |
| 提示 | 30 | F-5~F-8, F-17~F-19, F-23, F-26, F-27, F-29, F-30, F-33, F-34, P-19~P-23, J-20~J-24, D-14~D-16, T-15~T-17 |

合计 110 条。编号前缀：F=core，P=PAL，J=JNI/Kotlin，D=解码器，T=tester。

---

## 二、高危发现（5 条，建议立即修复）

### J-1 (高) nativeClose 在递延 close 路径提前释放 ctx → UAF

- **位置**: `android/cpp/logx_jni.cpp:330-342` + `android/kotlin/LogX.kt:538-575`；关联 `core/src/logx_c.cpp:472-488`
- **现象**: `nativeClose` 假设 `logx_close` 返回即 Flusher 已 join，随即 `DeleteGlobalRef(kp_ctx->g_provider)`、`delete kp_ctx`、`delete err_ctx`。但 `logx_close` 有两条**递延返回**路径：① close 在 Flusher 线程被调用（`on_flusher`）→ 仅置 `close_on_flusher` 返回 OK，实例销毁递延到 flusher_main 退出；② `in_callback>0 || active_ops>0` → 置 `close_pending`，由最外层 OpGuard 的 `flush_blocking` 完成销毁。递延期间 Flusher 仍会经 `inst->key_provider(kid, inst->key_user)`（`flusher.cpp:580/613/935/1121`、`logx_c.cpp:91`）取密钥、经 `emit_event` 使用 `err_user`。
- **触发链**: 错误回调在 Flusher 线程触发（drain 期 DEGRADED/IO 事件）→ 业务在 `onError` 内按 ABI 允许调用 `close()` → Kotlin `threadOpDepth` 对 Flusher 线程返回 0 → 走同步 nativeClose → `logx_close` 递延返回 → JNI 释放 ctx → Flusher 续跑再次调用 trampoline → **悬垂指针**。
- **影响**: 进程崩溃/内存破坏。
- **修复方向**: core 需提供"销毁是否真正完成"的信号（`logx_close` 出参或析构回调），JNI 仅在确认销毁完成后释放 ctx；否则递延登记 ctx 待销毁通知时释放。

### J-2 (高) 错误回调内重入写接口未被阻断 → 违反 ABI 且可死锁

- **位置**: `android/kotlin/LogX.kt:383-397` + `170-198`；契约见 `core/include/logx_c.h:9-10`
- **现象**: ABI 规定错误回调内除 close 外不得调用任何 logx 接口，但 `onNativeError` 直接执行业务 `onError`，业务若在其中调 `LogX.e()/flush()/getStats()`，`executeOp` 不检测"当前处于回调栈"（`threadOpDepth` 只用于 close 自等待规避）。而 `logx_internal.h:62-63` 明确 `logx_write` 持 `inst->mu` 时也会 `emit_event`——非递归锁重入 `logx_write` → 同线程二次取 `inst->mu` **死锁**。
- **修复方向**: `onNativeError` 期间设 ThreadLocal 回调标志，`executeOp` 对非 close 操作直接拒绝返回 ERR_STATE。

### P-1 (高) pal_win32 全部文件 API 使用 ANSI(A) 版本，UTF-8 中文路径必坏

- **位置**: `core/src/pal_win32.cpp:105, 186, 199, 209, 213, 230, 247`（`file_open/file_size_path/file_dir_fsync/file_unlink/file_rmdir/file_mkdirs/file_list` 均走 `CreateFileA/GetFileAttributesExA/DeleteFileA/FindFirstFileA`）
- **现象**: 上层（C API / Kotlin）传入的 `log_dir` 是 UTF-8 字节串；中文 Windows ACP=GBK，UTF-8 字节按 GBK 解码——路径要么打不开（乱码目录名），要么创建出乱码文件。且 `file_list` 返回路径与 `file_open` 解码不一致会导致自产文件"列出来却删不掉"（retention 失效）。文件头虽声明为"已知限制"，但对中文用户是功能性缺陷。
- **修复方向**: `MultiByteToWideChar(CP_UTF8,...)` + W 系 API；或入口处显式校验并拒绝非 ASCII 路径。

### D-1 (高) 解码器块级解码失败后未丢弃跨块分片 carry

- **位置**: `decoder/logx_decode.py:424-433`；对照 `core/src/logx_c.cpp:123/133/146`
- **现象**: C++ `decode_one_file` 在 missing key / CRC fail / decode fail 三条路径均 `carry_slice_buf.clear()`；Python 仅追加占位行后 `continue`，重组器 carry 原样保留。若块 N 末尾含首/中片、块 N+1 解码失败，块 N+2 的中/末片会与块 N 残片**错误拼接**，违背"宁丢不脏"，产生错乱记录。
- **修复方向**: `if blk_res.error:` 分支内调用 `reassembler.drop_carry()`。

### T-1 (高) tester 完全忽略 LogX.log 返回码，指标可信度失真

- **位置**: `android/logx-tester/app/src/main/java/com/logx/tester/engine/LogXProbe.kt:111-118`
- **现象**: 仅靠 ThreadLocal 哨兵事件判定写入结果。实例被并发 stop/restart 时 `executeOp` 返回 null → `log()` 返回 ERR_STATE（`LogX.kt:412`）且不发事件；Kotlin 层等级过滤（`LogX.kt:408`）也直接返回 ERR_OK 无事件。影响：压测/单条写入在实例失效期间被计为**海量假 OK**，吞吐/延迟/丢弃指标全部失真——正是测试器最不该失真的路径。
- **修复方向**: `write()` 检查 `LogX.log` 返回值，非 0（或 ERR_STATE）归入失败分支。

---

## 三、中危发现（28 条）

### 3.1 core（8 条）

#### F-1 (中) logx_write TOCTOU：closing 检查先于 active_ops 增计

- **位置**: `core/src/logx_c.cpp:496`（查 closing）→ `:500`（`active_ops.fetch_add`）
- **现象**: 若 close 恰在两步之间完成（`active_ops==0` 走立即 `destroy_instance`），写线程对已 delete 的 inst 增计数 → use-after-free。契约③虽要求调用方先停写，但 ABI 头宣称 close 并发安全，两者矛盾。
- **修复方向**: 先增计数再查 closing，closing 则减计数返回。

#### F-2 (中) 递延销毁死角：close_pending 无人消费

- **位置**: `core/src/logx_c.cpp:480-486`
- **现象**: `logx_close` 在 `in_callback>0` 且 `active_ops==0` 时置 `close_pending`，但销毁只由 OpGuard（活跃调用方线程）或 `close_on_flusher` 触发。若回调正运行在 Flusher 线程（drain_once emit_event）而用户线程无在途调用 → `close_pending` 无人消费 → 实例泄漏、staging 不关、线程不 join。与 J-1 同族，建议一并解决（core 提供销毁完成信号）。

#### F-9 (中) logx_write 调用线程栈 64KB 缓冲

- **位置**: `core/src/logx_c.cpp:509`（`uint8_t buf[kMaxRecordTotal]`）
- **现象**: format 内部 scratch 已 TLS 化（FM-03），但调用方 buf 仍是普通局部数组；每写线程触碰 64KB 栈，叠加 format TLS 合计 ~128KB，小栈线程（≤256KB）有风险。头注释称"TLS Scratch"与实现不符。
- **修复方向**: 同样 TLS 化或堆缓冲复用。

#### F-10 (中) ensure_append_target 裸读 Header A 取 ATC，无 CRC 校验

- **位置**: `core/src/flusher.cpp:710-714`
- **现象**: 直接 memcpy 读 Header A reserved(+20) 取 ATC，未做 CRC/合法性校验。Header 撕裂时 hint 路径可为垃圾（后续 scan_one 会验证文件本身，风险受控），但 date/split 可能指向不存在文件 → 走 open_or_create 新建，产生**孤儿分卷**。

#### F-20 (中) run_recovery 裸读 Header A(+20) 而非已验证的 hdr.target

- **位置**: `core/src/recovery.cpp:377`（`std::memcpy(&atc, inst->staging->raw_base() + 20, sizeof(atc));`）
- **现象**: 未用 `pick_header` 已验证的 `hdr.target`；Header A 撕裂而 B 被选中时，把垃圾 ATC 写回 `commit_header` → 未来可能生成畸形日期孤儿文件（retention 不删、export 混入）。
- **修复方向**: 改用 `hdr.target` 作为基底。

#### F-11 (中) 追加已存在文件时不校验 32B 文件头即取 salt/加密位

- **位置**: `core/src/flusher.cpp:607-610`
- **现象**: 已存在文件分支读 32B 头取 salt/加密位，但未校验 magic/CRC（`scan_one` 有校验；此路径在 recovery 之后运行）。若外部破坏文件头，salt 取错、后续块 nonce 派生错——读侧 CRC 仍过（同 salt 加解密自洽），但离线解码时 salt 与文件头不符 → **整文件不可解**。
- **修复方向**: 补 magic+CRC 校验，失败走重建分支。

#### F-12 (中) kid 轮换设计隐患：续写密钥与文件头 kid 声明不一致

- **位置**: `core/src/flusher.cpp:613`（续写用 `inst->kid` 取密钥）vs `:589`（文件头 kid 建文件时写入）
- **现象**: 若运行期 kid 变更：旧文件新块用新 key 加密、文件头却声明旧 kid → 导出按头 kid 找密钥解不开。当前 ABI 无 kid 热更接口（cfg 固定），风险仅在未来支持 kid 轮换时爆发。叠加 **J-6**（见 3.3）后即使不轮换也有脆弱点。记为设计隐患，需在支持轮换前先立契约。

#### F-28 (中) 加密失败 → 块被静默丢弃且无 DEGRADED 事件

- **位置**: `core/src/codec.cpp:228-231`（`encrypt_gcm` 失败 → `encode_block` 返回 false）→ drain 丢弃该块（zero+commit 跳过）
- **现象**: 明文降级有安全问题可以理解，但"丢失且几乎不可观测"（仅 `block_write_errors` 计数）不合理。
- **修复方向**: 至少发 DEGRADED/IO 事件。

### 3.2 PAL（5 条）

#### P-2 (中) mutex 可重入性两平台不一致

- **位置**: `core/src/pal_posix.cpp:42`（`std::mutex` 非递归，同线程重复 lock=死锁/UB）vs `core/src/pal_win32.cpp:75-83`（`CRITICAL_SECTION` 可重入）
- **现象**: core 层任何"持锁路径上再次取锁"的演化在 Windows 测试全绿、Android 上线死锁。
- **修复方向**: 契约明确非递归（Win32 改 SRWLOCK）或明确可重入（POSIX 改 `PTHREAD_MUTEX_RECURSIVE`），二者取一并写入 `logx_pal.h` 注释。

#### P-3 (中) macOS F_FULLFSYNC 无 fsync 回退且重于 data-only 契约

- **位置**: `core/src/pal_posix.cpp:157-163`
- **现象**: `F_FULLFSYNC` 在 NFS、部分外置/旧文件系统返回 `EINVAL/ENOTSUP` → 每次 durability 检查点失败（flusher 持续发 IO 事件），Mac 上"永远同步失败"。且它同时刷元数据、耗时可达数十 ms，违反头文件"data only"契约（虽只重不轻）。
- **修复方向**: 失败于 `EINVAL/ENOTSUP/EOPNOTSUPP` 时回退 `fsync(fd)`；可先试 `F_BARRIERFSYNC`。

#### P-4 (中) ev_wait_timeout 基于 CLOCK_REALTIME，时钟跳变使 Flusher 等待失控

- **位置**: `core/src/pal_posix.cpp:67-77`；消费方 `flusher.cpp:1012` / `logx_c.cpp:613`
- **现象**: 默认 cond 属性绑定 CLOCK_REALTIME，代码与之一致（本身没错），但 Android 开机后 NITZ/NTP 校时常大幅跳变：回跳让带超时等待挂起远超 ms，前跳则提前超时。
- **修复方向**: `pthread_condattr_setclock(CLOCK_MONOTONIC)`（bionic API 21 起支持）后再 init cond。

#### P-5 (中) Win32 mmap_open 不校验文件大小与 len 一致；映射尺寸冻结

- **位置**: `core/src/pal_win32.cpp:263-278`（连带 118-121）
- **现象**: `CreateFileMapping(0,0)+MapViewOfFile(0)` 映射"文件当前全尺寸"，与传入 `len` 无关：文件 < len 时视图小于 `m.len`，core 按 len 写视图越界触发 `EXCEPTION_IN_PAGE_ERROR`（POSIX 等价 SIGBUS）；映射后再 `_chsize_s` 会使既有视图失效/悬空，POSIX 无此问题。当前 staging 调用顺序（先 truncate 后 map、映射后不再 truncate）恰好安全，属"靠调用方纪律而非 PAL 守卫"。
- **修复方向**: mmap_open 内 `GetFileSizeEx`/`fstat` 校验 size==len，不等返回失败；头注释写明"映射建立后不得再 truncate 该 fd"。

#### P-6 (中) Win32 Event 创建失败被静默吞掉 → 忙轮询

- **位置**: `core/src/pal_win32.cpp:88-95`
- **现象**: `CreateEventA` 返回 NULL 时对象仍构造成功：`wait()` 立即返回、`wait_timeout()` 恒 false → Flusher 等待循环退化为满 CPU 自旋，且无任何错误信号。
- **修复方向**: `ev_create()` 返回 nullptr（或失败置位），调用方判空。

### 3.3 JNI/Kotlin（5 条）

#### J-3 (中) tag/路径以 Modified UTF-8（CESU-8）传给按 UTF-8 契约的 core

- **位置**: `android/cpp/logx_jni.cpp:359/362、239-240`
- **现象**: `GetStringUTFRegion/GetStringUTFChars` 产出 Modified UTF-8：非 BMP 字符按代理对拆成 6 字节 CESU-8。core 按 UTF-8 存储后，导出回读走 `utf8_to_utf16`（`jni_string_util.h:51` 拒绝 ED A0..BF）→ emoji tag 变两个 U+FFFD，往返不一致；log_dir/staging 路径含非 BMP 时与文件系统真实路径（标准 UTF-8）不匹配。payload 走 byte[] 无此问题。
- **修复方向**: 输入方向补 CESU-8→UTF-8 转码，或 facade 限制 tag/路径为 BMP。

#### J-4 (中) 进程名获取失败返回空串 → 多进程隔离静默失效

- **位置**: `android/kotlin/LogX.kt:349-363`
- **现象**: API<28 分支 `runningAppProcesses` 可为 null/不含自身 pid → `processName=""` → suffix="" → 子进程与主进程共用同一 staging mmap 文件，并发写互相踩踏，违背 Safe-by-Default 设计意图。
- **修复方向**: 失败时回退 `"pid_$pid"` 后缀或直接拒绝 init。

#### J-5 (中) suspend export runBlocking(parentContext) 继承调用方调度器 → 主线程 IO

- **位置**: `android/kotlin/LogX.kt:472-504`
- **现象**: 注释宣称"直接在当前 native 调用栈线程执行、不重复调度"，但 `parentContext` 含调用方 dispatcher：从 Main 协程调用 suspend export 时，`block(tempFile)`（读明文文件）被派发到主线程执行。
- **修复方向**: 仅继承 Job 保取消传导，调度器固定 `withContext(Dispatchers.IO + parentContext.job)`。

#### J-6 (中) key_provider 返回共享 thread_local 缓冲，core 跨调用缓存指针

- **位置**: `android/cpp/logx_jni.cpp:133、173-174`；消费方 `core/src/flusher.cpp:613`（`out_target->key` 缓存）；契约 `core/include/logx_c.h:102-104`
- **现象**: 当前 `inst->kid` 实例内固定、同线程重复调用返回同内容，暂不出错；但一旦同线程先后应答不同 kid（未来 kid 轮换、export 与追加混跑），前一缓存指针即被覆写 → 用错密钥。与 F-12 叠加。
- **修复方向**: 按 kid 维护小 map 或每 kid 独立缓冲。

#### J-7 (中) FindClass/GetMethodID 失败遗留 pending 异常且 mid 永久 null

- **位置**: `android/cpp/logx_jni.cpp:255-266`
- **现象**: 失败时不清 Exception，nativeInit 返回后 JVM 向 Kotlin 抛异常（与"返回 0L 抛 IllegalStateException"路径叠加）；且 `g_mid_on_error` 为空后，后续所有实例（含新 init）err_ctx 均不创建，错误事件**永久静默丢失**、无重试。
- **修复方向**: 失败清异常并记录 logcat；mid 获取失败时下次 init 重试。

### 3.4 解码器（5 条）

#### D-2 (中) Python 放行 ALGO_ZLIB，C++ 拒绝

- **位置**: `decoder/logx_decode.py:355` vs `core/src/flusher.cpp:370-374`（`block_header_plausible` 仅放行 None/Zstd）
- **现象**: zlib 块在 C++ 导出路径终止扫描、Python 照常解码，两端行为分叉（当前写端不产 zlib，属潜在不一致）。
- **修复方向**: 与 C++ 对齐拒绝 algo=2，或 docstring 显式归档差异。

#### D-3 (中) is_whole 记录 Python 清空 carry，C++ 不清

- **位置**: `decoder/logx_decode.py:162-166` vs `core/src/format.cpp:442-443`
- **现象**: C++ 中 whole 记录直接输出、`current_slice_buf` 不受影响；Python `feed()` 遇 whole 时若 `in_split` 则清空。对抗性/损坏流（first→whole→mid→last）下两端输出不同，Python 更保守但与声称的"对齐 C++ 现行语义"不符。

#### D-4 (中) missing key 占位文案与 C++ 不一致

- **位置**: `decoder/logx_decode.py:307/428-431` vs `core/src/logx_c.cpp:121`
- **现象**: C++ 输出 `[skipped: kid=%u missing key (seq=%u)]`，Python 归入 `decode fail` 分支输出 `[seq=N decode fail]`，且 main 的后缀过滤（:563）不识别 kid 变体。docstring 声明"与 C++ 导出一致"，实际缺 key 场景输出文本不同。

#### D-5 (中) 文件头 magic 用 assert 校验，python -O 下被跳过

- **位置**: `decoder/logx_decode.py:264`
- **现象**: `-O` 运行时坏头文件继续以随机 salt/kid 解码整文件，静默产出垃圾而非报错。
- **修复方向**: 改为显式 `raise RuntimeError`。

#### D-6 (中) parse_user_payload 无 tag_len/msg_len 边界校验

- **位置**: `decoder/logx_decode.py:455-467`
- **现象**: C++ 写端保证 tag≤64、总长≤60KB，但 cksum 是弱 XOR，畸形载荷可声明越界长度：`msg_len` 超出剩余字节时 msg 被静默截断输出、不报错；损坏数据以"看似正常"的截断行呈现，违背宁丢不脏。
- **修复方向**: 显式校验 `23+tag_len+4+msg_len <= len(b)` 且 `tag_len<=64`，越界 raise。

### 3.5 tester（5 条）

#### T-2 (中) maxConsoleBuffer 普通 var 跨线程读写

- **位置**: `TesterViewModel.kt:84` + `ui/TesterScreen.kt:223-224`
- **现象**: UI 线程写（onCapacityChange）、Dispatchers.Default 批处理协程读（:133），无 volatile/同步，跨线程可见性无保证；且非 StateFlow，修改容量不触发重组，控制台"当前缓存 N/M"显示滞后。改 `MutableStateFlow<Int>`。

#### T-3 (中) applyAndRestart 在主线程执行 LogX.init

- **位置**: `TesterViewModel.kt:157-176`
- **现象**: `viewModelScope.launch` 默认 Main.immediate，init 内含 mmap、staging 双 Header 恢复、日志文件全量扫描（scan_one 逐块 CRC）——历史文件大时主线程阻塞数秒，ANR 风险。`withContext(Dispatchers.IO)` 包裹。

#### T-4 (中) onCleared 主线程 runBlocking(LogX.close())

- **位置**: `TesterViewModel.kt:415-420` + `engine/EngineController.kt:46-50`
- **现象**: close 内部有等待活跃操作语义（最坏 5s），退出时有导出/压测活跃操作则主线程阻塞至超时 → ANR。close 移到 IO 线程（独立短生命周期 scope + NonCancellable）。

#### T-5 (中) verticalScroll 内嵌 LazyColumn

- **位置**: `ui/TesterScreen.kt:103-108` + `ui/LogConsole.kt:108`
- **现象**: 嵌套滚动容器 → 控制台手势冲突（滚动被外层截获/双重消费）、LazyColumn 在无限高父容器中测量行为异常、整页随 10Hz metrics/progress 更新反复测量。外层改 LazyColumn 或 console 固定区域 + nestedScroll。

#### T-6 (中) SmokeActivity JNI 回调线程直写非线程安全 ArrayList

- **位置**: `kotlin-gradle/.../SmokeActivity.kt:46,18-21`
- **现象**: errorHandler 的 `report()` 会被 RECOVERY/IO/DEGRADED 事件的 Flusher 线程同步回调（`LogX.kt:383` onNativeError 在 native 调用线程执行），与主线程 `lines.add/joinToString` 竞争；且 `tv.text` 仅 onCreate finally 设置一次，后续事件 UI 不可见。`runOnUiThread`/Handler.post 或线程安全队列。

---

## 四、低危发现（47 条）

### 4.1 core（8 条）

| 编号 | 位置 | 摘要 |
|---|---|---|
| F-3 | `logx_c.cpp:629-639` | kTooLarge 归入 drop_no_space 返回 NO_SPACE，错误码语义不符（open 已校验，实际不可触发） |
| F-4 | `logx_c.cpp:348` | `max_log_length >= kDataSizeDefault` 校验为死代码（64KB < 130944 恒成立），无害冗余 |
| F-13 | `flusher.cpp:949` | drain_once 锁外读 cur_seqno；单写者前提下安全，建议注释钉死前提 |
| F-14 | `flusher.cpp:685` | close_current 先置 fd=-1 再 fsync/close；并发 durability_sync 窗口内丢一次同步机会或 EBADF（无害） |
| F-15 | `flusher.cpp:420/723` | 仅头文件(sz==32)时 last_block_end_offset=32 且 seqno=0，target 复用正确，无碍（记录口径） |
| F-25 | `staging.cpp open()` | 降级堆模式跳过 recovery 对账（数据全丢），可接受但应文档化 |
| F-31 | `recovery.cpp` | S_file>S_hdr 且 Σraw_len≥size 分支不推进 F → [F,w) 已落块数据重刷（重复块）；仅磁盘损坏可达，建议 F 置 w 或发 DROP 事件 |
| F-32 | `staging.cpp pick_header` | 不校验 Header.version（写恒 1 读不查），未来格式演进缺先验拒绝点 |

### 4.2 PAL（12 条）

| 编号 | 位置 | 摘要 |
|---|---|---|
| P-7 | `pal_win32.cpp:197-206` vs `pal_posix.cpp:164-170` | 目录 fsync 失败模式不对称（Win32 需 FILE_ADD_FILE 写权限，只读目录返 -1；POSIX O_RDONLY 即可），注释应写明差异 |
| P-8 | `pal_win32.cpp:192-196` | FlushFileBuffers 被当 fdatasync 但无注释（语义更重：数据+元数据） |
| P-9 | `pal_win32.cpp:320-326` | thread_join 恒返回 0（WAIT_FAILED/ABANDONED 也报成功），失败应返 -1 |
| P-10 | `pal_win32.cpp:109-113` | P-05 注释错误声称 CRT `_O_APPEND` "原子性"从文件尾写（MSVC CRT 是用户态 seek+write）；单写者不变量兜住正确性，注释误导 |
| P-11 | `pal_posix.cpp:117,131,145` / `pal_win32.cpp:133,152,172` | 返回类型 `long` 在 LLP64/32 位截断风险（len>2^31-1 静默变负），建议 int64_t 或文档限定 |
| P-12 | `pal_win32.cpp:356-367` | QueryPerformanceFrequency 失败未检查 → freq=0 除零 |
| P-13 | `pal_win32.cpp:369-377` / `pal_posix.cpp:306-310` | 系统时间早于 1970 时 wallclock_ms 无符号下溢回绕，建议 clamp 0 |
| P-14 | `pal_posix.cpp:191` / `pal_win32.cpp:230-233` | file_mkdirs 把"同名普通文件已存在"当成功（EEXIST 不区分），失败推迟到后续 open |
| P-15 | `pal_win32.cpp:221-238` | file_mkdirs 对 UNC(`\\server\share`) 与盘符相对路径(`C:foo`)处理错误 |
| P-16 | `pal_win32.cpp:69-73` | errno 契约跨平台不一致（Win32 仅 unlink/rmdir 转译），上层按 errno 诊断在 Windows 拿陈旧值 |
| P-17 | `pal_posix.cpp:331-337` | tid() 非 Linux 回退 std::hash<thread::id>，非内核 tid；macOS 应用 pthread_mach_thread_np（Android API21 路径正确） |
| P-18 | `pal_posix.cpp:161` | Linux tmpfs 上 fdatasync 返回 EINVAL → 测试环境检查点恒失败，可 EINVAL 时回退 fsync |

### 4.3 JNI/Kotlin（12 条）

| 编号 | 位置 | 摘要 |
|---|---|---|
| J-8 | `logx_jni.cpp:296` | kid jint→uint16_t 静默截断，负值/超 65535 变另一合法 kid，密钥错配无告警 |
| J-9 | `logx_jni.cpp:287-289` | flushThreshold ≤-2 的负值 cast 成巨大 uint32（阈值永不触发），未定义未拒绝 |
| J-10 | `logx_jni.cpp:367` | GetStringUTFChars OOM 返 LOGX_ERR_NOMEM 时 pending OutOfMemoryError 未清，Kotlin 实际抛异常而非收 -7 |
| J-11 | `logx_jni.cpp:439-441` | GetMethodID("onExport") 失败遗留 NoSuchMethodError 且 cbClass 局部引用未删 |
| J-12 | `logx_jni.cpp:200-203` | export_trampoline make_jstring_utf16 失败无 null 兜底 → Kotlin 非空参数 NPE 被 ExceptionClear 吞，该分卷静默失败 |
| J-13 | `LogX.kt:450-454` | 非挂起 export 未初始化返回 -1，与 log()/flush() 的 ERR_STATE(-2) 口径不一致 |
| J-14 | `LogX.kt:298` | contains("/cache/") 子串检查误伤 `/cache_x/` 类合法路径，应规范化后比较路径段 |
| J-15 | `LogX.kt:284-286、322-329` | init 失败回滚无条件置 null errorHandler，清掉用户预设的启动期监听（K-03 副作用） |
| J-16 | `LogX.kt:250-262` | 重复 init 先执行最长 1s 退栈轮询+遗留销毁才在锁内抛"called more than once"，慢前奏误导排障 |
| J-17 | `logx_jni.cpp:100-130/143-184` | Flusher 线程每事件 Attach/Detach 一轮，事件风暴时开销显著（detach 前 DeleteLocalRef 顺序正确） |
| J-18 | `logx_jni.cpp:32` | g_console_echo 进程级全局，与 J-02"实例间零共享可变状态"声明矛盾（Kotlin 单例契约下无实害） |
| J-19 | `LogX.kt:538-575` | close() 持 this 锁内执行 nativeClose（最坏 ~5s），期间 setErrorHandler/init 全部挂起（N6 只移了 init 侧） |

### 4.4 解码器（7 条）

| 编号 | 位置 | 摘要 |
|---|---|---|
| D-7 | `logx_decode.py:514` | 文件名正则不校验日历合法性、split 无 uint16 上限（C++ parse_log_filename 有 R-03 校验） |
| D-8 | `logx_decode.py:519-520` | --from/--to 字符串比较无格式校验，`2026-10-15` 静默错滤 |
| D-9 | `logx_decode.py:69-80` | 纯 Python 逐字节 CRC32，16MB 块需数秒；应直接 zlib.crc32（语义等价） |
| D-10 | `logx_decode.py:578` | Windows GBK 控制台/重定向下 msg 含 U+FFFD 可能 UnicodeEncodeError 崩整个工具；应 sys.stdout.reconfigure(errors='replace') |
| D-11 | `logx_decode.py:475` | parse_kid_map 缺 `=` 时 ValueError 未捕获，main 直接 traceback |
| D-12 | `logx_decode.py:448-451` | 结构损坏 break 后重复输出 trailing 警告（双错误噪音） |
| D-13 | `logx_decode.py:309` | 未交叉校验块头 [22..34) 存储的 nonce（写端落盘 salt+BE seqno；C++ 读端同样不校验，属两端共同盲区） |

### 4.5 tester（8 条）

| 编号 | 位置 | 摘要 |
|---|---|---|
| T-7 | `engine/StressRunner.kt:93` | 失败写入（NoSpace/TooLarge/Invalid）延迟也写入 latencies → P50/P99 混入丢弃路径耗时 |
| T-8 | `engine/StressRunner.kt:136-139` | catch Exception 吞全部异常标记 wasCancelled，真异常（OOM 等）被误报"手动中止" |
| T-9 | `TesterViewModel.kt:297` | 落盘文件过滤 name.contains(".log.") 误匹配（如 catalog.log.bak），应正则 `^\d{8}\.log(\.\d+)?$` |
| T-10 | `ui/ConfigPanel.kt:239` | UI 文案"AES-128-GCM"与实现不符（AES-256-GCM/32B key），误导密钥长度预期 |
| T-11 | `engine/LogXProbe.kt:113` | slot.set(null) 未用 remove()，池线程 ThreadLocalMap entry 常驻 |
| T-12 | `TesterViewModel.kt:109-122` | 引擎停止后批处理协程仍 10Hz 调 getStats()（返回 null 仍触发 metrics 更新与重组） |
| T-13 | `model/TesterConfig.kt:41` | CUSTOM_HEX 模式 hex 非法时静默退化为"密钥缺失"，UI 无提示 |
| T-14 | `TesterViewModel.kt:266-272` | export 回调内 fileCount++/totalBytes+= 捕获可变局部变量，依赖 nativeExport 同步单线程回调前提（当前满足，脆弱） |

---

## 五、提示级（30 条）

### 5.1 core（14 条）

| 编号 | 位置 | 摘要 |
|---|---|---|
| F-5 | `logx_c.cpp:154-167` | export"明文文件"实为 hex dump，非可读日志文本；与 C 头契约有出入，建议文档标注 |
| F-6 | `logx_c.cpp make_tmp_path` | snprintf buf 160B，log_dir 过长静默截断可能产生错误路径 |
| F-7 | `logx_internal.h` | Instance::flusher 字段疑似从未使用（死字段） |
| F-8 | `logx_c.cpp:456` | open 侧 store(true) flusher_running 与 flusher_loop 内重复；线程瞬间退出时窗口极小可能覆写 |
| F-17 | `flusher.cpp block_header_plausible` | seqno>prev 依赖文件内单调；export 跨文件重置 prev_seqno，正确（记录口径） |
| F-18 | `flusher.cpp:996` | flusher_loop 启动即执行 retention 清理，与 open 时 run_recovery 重复，无害 |
| F-19 | `logx_format.h cksum1` | 1B XOR 弱校验仅防撕裂，块级 CRC32 兜底，可接受（spec 如此） |
| F-23 | `staging.cpp commit_header_locked` | seq_cst 屏障保证可见序不保证掉电持久序；风险被恢复对账吸收，建议注释明示"依赖对账而非持久序" |
| F-26 | `format.cpp:65-71` | tag_len_bounded 依赖 tag NUL 终止契约，恰 64B 无 NUL 属 UB 输入（注释已声明）；strnlen API21 可用 |
| F-27 | `format.cpp parse_record` | is_whole 后继续循环，后续记录因 user 非空返 false——契约"单条记录"，调用方需按边界传入 |
| F-29 | `codec.cpp` | GCM 无 AAD：块头不被认证，CRC32 非 MAC；篡改块头可致语义漂移（raw_len 交叉校验兜底）。与 xlog 同级设计，建议文档标注威胁模型 |
| F-30 | `codec.cpp:81` | zstd level 3 硬编码无配置面（设计如此）；zlib 配置 open fail-fast 拒绝正确 |
| F-33 | `recovery.cpp reconcile` | 目录每候选文件 scan_one 全量读 payload 验 CRC，启动成本 O(总日志量)；文件量大时冷启动拖慢（Android open 在业务线程，注意 ANR） |
| F-34 | `recovery.cpp` | 文件存在但打不开时 file_truncated=true → 首块 seqno 跳 2；设计如此（防 nonce 重用），无碍 |

### 5.2 PAL（5 条）

| 编号 | 位置 | 摘要 |
|---|---|---|
| P-19 | `pal_posix.cpp:293-298` | reinterpret_cast<std::atomic<uint64_t>*> 形式 UB（lock-free 布局依赖，实际可行）；C++20 可 atomic_ref |
| P-20 | `pal_win32.cpp:337-350` | Interlocked64 对齐仅 Debug 断言；Release 下错 8B 对齐指针硬件异常 |
| P-21 | `pal_win32.cpp:279-284` | mmap_sync(synchronous=false) 静默返回 0 什么都不做，易误读为"已异步提交"（当前 core 只传 true） |
| P-22 | `pal_win32.cpp:282` | FlushViewOfFile 只到文件缓存不到物理盘，弱于 msync(MS_SYNC)；调用点已配对 file_fsync 补偿，头契约应写明 |
| P-23 | `logx_pal.h:21` | out_handle 所有权两平台不同且注释含糊（POSIX 借用 fd 不关 / Win32 mapping HANDLE 归 mmap_close 关）；按字面理解去 close POSIX out_handle 会双重关闭 |

### 5.3 JNI/Kotlin（5 条）

| 编号 | 位置 | 摘要 |
|---|---|---|
| J-20 | `LogX.kt:392-396` | onNativeError 吞业务全部 Throwable 无留痕，事件丢失难排查；debug 构建建议 Log.w 兜底 |
| J-21 | `logx_jni.cpp:377-399` vs `LogX.kt:28-32` | JNI 层 64KB 拷贝上限与文档"60KB 用户载荷硬上限"口径不一致，60-64KB 区间依赖 core 截断事件 |
| J-22 | `LogX.kt:492-497` | suspend export tempFile.delete() 与 core file_unlink（logx_c.cpp:276）重复删除，冗余无害 |
| J-23 | `SmokeActivity.kt:41-57` | 示例只 flush 从不 close（mmap 保证进程终止安全，可接受）；缺"正常销毁→close"示范 |
| J-24 | `jni_string_util.h` | 解码器对过长编码/代理区/越界拒绝 + U+FFFD 重同步正确，未发现越界读（正面核验记录） |

### 5.4 解码器（3 条）

| 编号 | 位置 | 摘要 |
|---|---|---|
| D-14 | `logx_decode.py:167-178` | payload_len=0 首片：Python 继续拼接，C++ 以 !empty() 判据丢弃其后中/末片（format.cpp:447）；仅对抗性流可触发 |
| D-15 | `logx_decode.py:211` | magic 重同步逐字节扫描，全零大块最坏 O(n²)；与 C++ 同型，性能提示 |
| D-16 | `logx_decode.py:263-268` | Python 校验文件头 CRC，C++ decode_one_file:85 仅校验 magic——Python 更严，个别 C++ 可导出文件 Python 会拒（差异方向与 D-2 相反，建议文档归档） |

### 5.5 tester（3 条）

| 编号 | 位置 | 摘要 |
|---|---|---|
| T-15 | `engine/StressRunner.kt:49` | LongArray(total) 无上限防御（当前 UI 最大 500k=4MB 安全），公共 API 应 coerce |
| T-16 | `engine/StressRunner.kt:82-133` | 多线程分槽写 latencies 无重叠、awaitAll happens-before，竞态安全；正确性依赖 slot 分配公式，改动需保持 |
| T-17 | 全局 | 正面确认：无泄漏 scope、StateFlow 全走 .update{} CAS、SharedFlow(4096,DROP_OLDEST) 有界、内存有界、Manifest 仅私有目录 |

---

## 六、已核销与解除项

| 项 | 结论 |
|---|---|
| F-16（enc payload≥16） | **解除**：`encrypt_gcm` 输出恒为 in_len+16 ≥16，`enc.payload.size()-16` 安全 |
| F-24（staging close 二次 close fd） | **解除**：PAL 代理证实 POSIX `mmap_close` 不关 fd（归 `file_close`），staging close 的 mmap_sync→mmap_close→file_fsync→file_close 序列正确；但所有权注释含糊记为 P-23 |
| 环形会计/双 Header 提交纪律/恢复四分支对账 | **闭环成立**（推导见 §一） |
| split_impl 切块预算数学 | **安全**：FM-03 推导 user_total_new ≤ cap ≤ max_total−6s，总长恒 ≤ max_total；kTruncBufSize 下界与 static_assert 绑定 |
| nonce 唯一性 | **无重用**：seqno 全局单调 + 每文件新 salt；kDecompressOutputMax=20550 ≥ 20491 |
| 解码器核心字节契约 | **一致**：块头 34B 各偏移/双份 seqno 端序、LGXF/LGXB/LG/GP、6B 记录头、slice flags、CRC32 反射表约定、GCM 布局、MAX_BLOCK_RAW=20491 逐项对齐 |
| PAL 常规项 | O_CLOEXEC、pread/pwrite EINTR 循环、msync 对齐、虚假唤醒 deadline 重算、getrandom→/dev/urandom 双路 CSPRNG、Android API 21 基线（SYS_gettid/fdatasync/getrandom 守卫）均验证无误 |
| 导出/回放路径 | 未发现 fd/HANDLE/heap 泄漏 |

---

## 七、修复优先级建议

| 优先级 | 项目 | 说明 |
|---|---|---|
| **P0（崩溃/数据破坏级）** | J-1、F-1、F-2、J-2、D-1 | J-1/F-2 建议一并解决：core 提供"销毁完成"信号（close 出参或析构回调），JNI 据此释放 ctx；F-1 改"先增计数再查 closing"；J-2 回调栈标志 + executeOp 拒绝；D-1 块失败清 carry |
| **P1（功能正确性）** | P-1、F-10/F-20、F-11、J-4、P-2、T-1 | Win32 W 系 API；ATC 裸读改 hdr.target/补校验；追加前验文件头 magic+CRC；进程名回退 pid 后缀；mutex 契约统一；tester 查返回码 |
| **P2（健壮性/可观测）** | P-3~P-6、J-3、J-5、J-6、J-7、F-9、F-12/F-28、D-2~D-6、T-2~T-6 | F-9 栈缓冲 TLS 化；F-28 补事件；kid 轮换契约先行（F-12+J-6 合并处理）；解码器对齐 C++ 语义；tester 主线程 IO 三处 |
| **P3（随迭代消化）** | 全部低/提示项 | 建议补一条"Flusher 线程 onError 内调 close"专项压测用例覆盖 J-1/F-2 路径；§5.1 中 F-5/F-23/F-29/F-33 为文档/注释类，可批量处理 |

---

> 审查执行记录：core 层（logx_internal.h / logx_format.h / logx_staging.h / logx_c.h / logx_pal.h / logx_codec.h / flusher.cpp / logx_c.cpp / staging.cpp / format.cpp / codec.cpp / recovery.cpp / CMakeLists.txt）由主线程逐行深审；PAL、JNI+Kotlin、解码器+tester 由三个独立只读子代理并行审查后交叉核对汇总。本报告未修改任何产品代码。
