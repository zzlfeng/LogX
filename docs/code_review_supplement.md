# LogX 代码审查遗漏问题补充与核验报告（校准修订版）

> 审查范围：原 `code_review_report.md` 核心结论复核、周边交付件（JNI、Kotlin Facade、Python 离线解码器、压力测试、PAL 跨平台抽象层、C ABI）以及跨模块 Spec 偏差追踪  
> 审查时间：2026-09-04（经二次交叉核验校准）  
> 关联文档：[code_review_report.md](./code_review_report.md)、[LogX日志库最终实施方案.md](./LogX日志库最终实施方案.md)、[LogX后续实施计划表.md](./LogX后续实施计划表.md)、[CHANGELOG.md](../CHANGELOG.md)

---

## 一、原报告（code_review_report.md）核验结论

对 `code_review_report.md` 列出的 17 个 BUG 进行严格交叉验证，区分“确认属实”、“确认误报”与“描述偏差”。

### 1.1 确认需修正或撤销的条目（5 处）

| 编号 | 原报告描述 | 核验结论 | 校准处理建议 |
|:---|:---|:---|:---|
| **BUG-6** | `debug_check_invariant` 不跨回绕场景漏检 `[0, f)` 区间 | **完全误报**。[`staging.cpp:354-375`](../core/src/staging.cpp#L354-L375) 第一个循环 `for (i = len2; i < off1; ++i)` 在 `w >= f` 时 `len2 = 0, off1 = f`，正好遍历 `[0, f)`；第二个循环 `[off1+len1, data_size_)` 遍历 `[w, data_size_)`，完整覆盖了全部非活跃区。原审查推论有误。 | **已在原报告标注撤销** |
| **BUG-14** | `decode_one_file` 中 CRC 校验对象（密文 vs 明文）需对齐 | **完全误报**。写入端 [`flusher.cpp:108`](../core/src/flusher.cpp#L108)（对 `enc.payload.data(), payload_len` 计算 CRC，`payload_len = enc.payload.size() - 16`，即不含 16B GCM tag 的密文）与读取端 [`logx_c.cpp:110`](../core/src/logx_c.cpp#L110)（对 `payload.data(), payload.size()` 计算 CRC，读取长度同样为密文且不含 tag）**两侧完全一致**。 | **已在原报告标注撤销** |
| **BUG-1** | `AppendTargetCache` 无 packed 属性，实际大小 16B 而非 13B，**数据错位静默损坏** | **单一运行时内不触发错位，但破坏二进制协议契约**。结构体确实缺 packed（`sizeof=16`）。在当前单一 C++ 编译单元内，写和读均用 `sizeof(AppendTargetCache)` 做 `memcpy`，闭环内字段能一一对应；但对跨语言/离线解码器规范而言，`split_no` 后的 3B 填充导致 `last_block_offset` 实际上写在了第 8~11 字节而非 Spec 约定的第 5~8 字节。 | 严重性从 🔴 P0 降为 🟠 P1；问题描述校准为“结构体对齐破坏跨语言二进制协议规范” |
| **BUG-12** | nonce 布局与 `kBlockHeaderSize` 注释不符 | 实际是 [`logx_staging.h:194`](../core/include/logx_staging.h#L194) 计算常量末尾注释误写为 `= 32B`（实际常量求值为 `4+1+1+4+4+4+4+12 = 34`），nonce 布局本身（`hdr[22..29]` salt + `hdr[30..33]` seqno 大端）实现正确。 | 严重性从 P1 降为 🟡 P2；修改头文件注释 |
| **BUG-15** | `max_log_length == 0` 时丢弃所有日志 | 注释与默认值不一致真实存在，但 [`logx_c.cpp:244-246`](../core/src/logx_c.cpp#L244-L246) 在 `logx_open` 中会主动拦截拒绝 `max_log_length == 0` 并直接返回 `LOGX_ERR_INVALID_ARG`，正常调用下**不会触发**“丢弃所有日志”。仅绕过 open 路径直接裸构 Instance 才会发生。 | 修复建议校准为“在配置默认值中显式赋 64KB” |

### 1.2 确认准确的条目（12 处）

经逐行比对源码，以下条目均真实存在，严重性合理，描述准确：
- **✅ BUG-2**：`kFlagCompressZstd` 与 `kFlagEncrypted` 均为 0x1，命名风格极易引起调用混淆。
- **✅ BUG-3**：`init_crc32_table()` 全局变量无锁 double-check 初始化，存在多线程数据竞争。
- **✅ BUG-4**：`logx_set_error_handler` 读写无互斥锁或原子保护，存在数据竞争。
- **✅ BUG-5**：`pal_win32.cpp` 中 64 位 Windows 下 `HANDLE` 强转为 `int` 截断高 32 位。
- **✅ BUG-7**：`format.cpp` 中 `tag[tag_len]` 在无 NUL 结尾的 64 字节缓冲区下越界访问（UB）。
- **✅ BUG-8**：`decode_block` 忽略 `raw`/`raw_len`，内部未执行 CRC 校验。
- **✅ BUG-9**：`logx_write` 中 `build_records` 超限或失败时，`drop_too_large` 统计未递增。
- **✅ BUG-10**：`recovery.cpp` 中 hint 文件不可读时，`was_truncated` 状态被残留传给 best 文件。
- **✅ BUG-11**：`recovery.cpp` 中 `S_hdr > S_file && truncated` 时丢块统计少计一次半块。
- **✅ BUG-13**：`flusher.cpp` 中 `atc.last_block_offset` 将 `uint64_t` 强转为 `uint32_t`。
- **✅ BUG-16**：`parse_records_stream` 重同步成功后未清除 `carry_slice_buf` 残留分片。
- **✅ BUG-17**：`logx_c.cpp` Windows 路径分隔符仅查找 `/`，不兼容 `\`。

---

## 二、补充审查报告自身误报与技术硬伤澄清（复核纠偏）

此前版本审查中存在以下 5 处技术误读与误报，本次复核予以彻底澄清与撤销：

### 2.1 ⚪ [已撤销] 原 BUG-J2：Kotlin Facade `export` trailing lambda 语法
- **原错误断言**：声称 Kotlin 中 `external fun` 不支持 trailing lambda，Kotlin 编译器会报 `"trailing lambda not supported for external functions"`，断言测试从未编译。
- **复核事实**：
  [`LogX.kt:42-45`](../android/kotlin/LogX.kt#L42-L45) 中 `ExportCallback` 声明为 **`fun interface`（SAM 接口）**。在 Kotlin 语言规范中，`external` 仅修饰方法实现由 native 提供，**Trailing Lambda 与 SAM 转换对所有函数（包括 `external fun`）完全通用合法**。Kotlin 编译器根本不存在所谓不支持的报错。该项为**完全误报，已彻底撤销**。

### 2.2 ⚪ [已撤销] 原 BUG-T4：`test_concurrent.cpp` 对账逻辑 `\n` 干扰
- **原错误断言**：声称用户日志消息内部可能含有 `\n`，导致对账计数 `cur_lines` 偏多。
- **复核事实**：
  [`logx_c.cpp:132-144`](../core/src/logx_c.cpp#L132-L144) 明确将用户载荷转换为十六进制 ASCII 字符串（`0-9a-f`）。十六进制字符集不可能包含 ASCII 0x0A (`\n`)，消息原有的 `\n` 会被转为字符 `'0'` 和 `'a'`。导出文件中**唯一**的 `\n` 就是每条记录末尾追加的行换行符。因此通过 `\n` 计数严格等于日志条数，完全精确。该项为**完全误报，已彻底撤销**。

### 2.3 ⚪ [已撤销] 原 BUG-T3：`test_crash_recovery.cpp` 模拟 kill -9 存在 UAF
- **原错误断言**：声称释放 staging 时未确认 Flusher 退出，可能触发 drain_once 访问已释放的 staging。
- **复核事实**：
  [`test_crash_recovery.cpp:57-64`](../tests/stress/test_crash_recovery.cpp#L57-L64) 在 `delete raw_inst->staging` 之前，**已经显式调用了 `pal->thread_join(raw_inst->flusher_thread)`**。当 `thread_join` 返回时，Flusher 线程已经彻底终止退出，不可能再访问 staging。该项为**完全误报，已彻底撤销**。

### 2.4 ℹ️ [描述纠偏] 原 BUG-J3：JNI `nativeWrite` 的 `tag_buf` 长度处理
- **原错误断言**：声称使用 `tag_len` 会导致中文字符下 `tag_buf[utf8_len] = '\0'` 越界或落在字符中间。
- **复核事实**：
  `tag_len = env->GetStringLength(jTag)` 为整串 Unicode 字符数。`GetStringUTFRegion(jTag, 0, tag_len, tag_buf)` 负责将这批字符全部转换为 Modified UTF-8。写入的字节数恰好等于 `utf8_len = env->GetStringUTFLength(jTag)`。在 `tag_buf[utf8_len]` 放 `\0` 恰好是字符串结束位置。仅有的边界隐患是当 `sizeof(tag_buf)` 边界溢出时的降级逻辑，不存在“截断落在字符中间”的逻辑错误。

### 2.5 ℹ️ [描述纠偏] 原 BUG-K2：`Application.getProcessName()` API 性质
- **原错误断言**：声称 `Application.getProcessName()` 是未公开私有 API，未来可能被移除。
- **复核事实**：
  `android.app.Application.getProcessName()` 是 Android 9.0 (API 28) 引入的**官方公开标准 API**，Google 推荐使用它替代以往读取 `/proc/self/cmdline` 的方式，不存在兼容性政策风险。

---

## 三、周边未审代码路径新发现（更新校准版）

### A. 核心 C++ 引擎与编解码缺陷（新增重磅项）

#### 🔴 [BUG-C1] Codec 压缩降级未同步更新 Block Header Flag（导致离线解码 100% 失败、整块报废）— P0

**位置：** [`codec.cpp:185-202`](../core/src/codec.cpp#L185-L202) 与 [`flusher.cpp:100-115`](../core/src/flusher.cpp#L100-L115)

```cpp
// codec.cpp:196-199
if (comp != kAlgoNone) {
    if (!compress_block(comp, raw, raw_len, &compd)) {
        comp = kAlgoNone;                // 试图降级为明文
        compd.assign(raw, raw + raw_len);
    }
}
```

```cpp
// flusher.cpp:100-115
if (!encode_block(flag, key32_or_null, seqno, salt, raw_block, raw_len, &enc)) {
    return -1;
}
// 3) 构造 34B 头
uint8_t hdr[34];
hdr[5] = flag;                           // ← 致命：依然写入未经降级修改的原始 flag！
```

**问题分析：**
`encode_block` 函数按值传递 `uint8_t flag`，返回的 `EncodedBlock` 结构体仅包含 `payload` 和 `enc_ok`，**没有将压缩降级后的实际 flag 返回给调用方**。
当 ZSTD 压缩失败（或编译期未开启 `LOGX_WITH_ZSTD` 时）：`encode_block` 内部将未压缩的明文放入了 `payload`，但调用方 `flusher.cpp` 构造 34B 块头时，写下的 `hdr[5]` **依然携带着原始的 ZSTD 压缩标志位**！
离线解码器或 C++ 导出路径读取到该块时，根据 `hdr[5]` 的压缩位调用 ZSTD 解压，由于数据实为未压缩明文，**ZSTD 解压必然失败，导致该块日志直接被判定为坏块丢弃**！Spec §6 承诺的“压缩失败平滑降级”在实际落地中变成了“静默产出毒丸块”。

**修复：**
在 `EncodedBlock` 结构体中增加 `uint8_t final_flag`，或将 `encode_block` 的第一个参数改为 `uint8_t* inout_flag`，在降级时清除对应的压缩位，确保写入 `hdr[5]` 的 flag 与 Payload 真实编码状态一致。

---

#### 🔴 [BUG-X2] 恢复后 `current_split` 强制归零导致历史日志文件被覆写 — P0

**位置：** [`flusher.cpp:447, 456`](../core/src/flusher.cpp#L447)

```cpp
c->current_date = std::strlen(fn) >= 8
    ? static_cast<uint32_t>(std::strtoul(fn, nullptr, 10))
    : c->current_date;
c->current_split = 0;                    // ← 强制归零！未从文件名解析真实 split_no
```

**问题分析：**
当实例崩溃恢复或重启、通过追加目标缓存命中当天的历史日志文件时（例如命中了 `20260904.log.3`），代码只解析了日期 `current_date`，而把 `current_split` 硬编码重置为 0。
当该文件后续写满 10MB 触发下一次滚动切分时，Flusher 会生成序号 `current_split + 1 = 1` 的文件名（即 `20260904.log.1`）！这会**直接覆盖或篡改当天之前已经落盘的历史分卷文件**，造成严重的持久化数据破坏。

**修复：**
从命中的文件名后缀中严格解析当前分卷号：
```cpp
auto p = std::string(fn).rfind(".log.");
c->current_split = (p == std::string::npos) ? 0 : std::atoi(fn + p + 5);
```

---

#### 🟠 [BUG-E1] `logx_export` 临时文件在同一天多分卷场景下固定命名覆盖冲突 — P1

**位置：** [`logx_c.cpp:198-220`](../core/src/logx_c.cpp#L198-L220)

```cpp
snprintf(buf, sizeof(buf), "%s/.tmp_export_%u", log_dir.c_str(), date);
```

**问题分析：**
导出明文临时文件是以日期 `date` 命名的。若同一天产生了多个分卷文件（`.log`、`.log.1`、`.log.2`），或者多次调用 export，所有分卷在导出时使用同一个固定临时文件路径。一旦前次回调处理缓慢或存在并发导出，临时文件会被后一次处理强行覆盖或截断，破坏数据隔离性。

**修复：**
在临时文件名中引入唯一的进程 PID + 随机数或分卷序号：
```cpp
snprintf(buf, sizeof(buf), "%s/.tmp_export_%u_%u_%lld", log_dir.c_str(), date, split, now_ns);
```

---

#### 🟠 [BUG-X1] `AppendTargetCache.split_no` 仍为 `uint8_t`（T4.0-2 偏差未落地）— P1

**位置：** [`logx_staging.h:65`](../core/include/logx_staging.h#L65)、[`flusher.cpp:312`](../core/src/flusher.cpp#L312)

切分序号仍为 `uint8_t split_no`。在单日高吞吐场景（如极端压力或调试日志）下，单日文件切分超过 255 个分卷后，Flusher 将停写并丢弃后续日志。Spec §3.1 要求升级为 `uint16_t`，该偏差尚未落地。

---

### B. JNI 层 (`android/cpp/logx_jni.cpp`)

#### 🟠 [BUG-J5] `nativeClose` 提前释放 `kp_ctx` 与 Flusher 线程竞态导致潜在 UAF — P1

**位置：** [`logx_jni.cpp:210-230`](../android/cpp/logx_jni.cpp#L210-L230)

```cpp
if (kp_ctx) {
    if (kp_ctx->g_provider) {
        env->DeleteGlobalRef(kp_ctx->g_provider);
    }
    delete kp_ctx;                       // ← 提前释放
    inst->key_user = nullptr;
}
logx_close(inst);                       // ← 内部 flush_blocking 期间 Flusher 仍在运行
```

**问题分析：**
`nativeClose` 在调用线程上先 `delete kp_ctx`，然后再调 `logx_close(inst)`。
然而查看 [`logx_c.cpp:313`](../core/src/logx_c.cpp#L313)，`logx_close` 首先调用的是 `flush_blocking(inst)` 轮询清空环形缓冲区，**在此期间后台 Flusher 线程依然在全速运转**！如果此时触发了一次加密块落盘，Flusher 线程会调用 `inst->key_provider(inst->kid, inst->key_user)`（即 `jni_key_provider_trampoline`）。此时 `kp_ctx` 正在被析构或已为悬垂指针，引发非法内存访问（UAF）。

**修复：**
调整调用顺序，确保在 `logx_close` 内部 join 且 Flusher 彻底退出后，再释放 JNI 上下文：
```cpp
logx_close(inst);
if (kp_ctx) {
    if (kp_ctx->g_provider) env->DeleteGlobalRef(kp_ctx->g_provider);
    delete kp_ctx;
}
```

---

#### 🟠 [BUG-J1] `Compress.ZSTD(1)` 映射与 C++ flag 字节布局语义不统一 — P1

**位置：** [`logx_jni.cpp:187`](../android/cpp/logx_jni.cpp#L187) + [`LogX.kt:30`](../android/kotlin/LogX.kt#L30) + [`flusher.cpp:515`](../core/src/flusher.cpp#L515)

JNI 端传 `Compress.NONE/ZSTD/ZLIB`（算法号 0/1/2），C++ 端需要 `(algo << 1)` 才能写入 flag 字节的 bit1~2 位置。这一转换在 `flusher.cpp` 内部完成是合理的，但常量命名与入参风格不统一（`kFlagCompressZstd=0x1` 暗示是 flag 位，实际是算法编号），极易误导后续接入层开发。

---

#### 🟠 [BUG-J4] `g_err_ctx` 全局静态变量多实例无保护 — P1

**位置：** [`logx_jni.cpp:40`](../android/cpp/logx_jni.cpp#L40)

`g_err_ctx` 是裸全局变量。多实例环境下共享同一 `g_err_ctx`，一个实例关闭时将全局结构清空，会导致其余实例的错误回调静默失效。

**修复：** 将错误回调上下文绑定到 `inst->err_user`，由每个 LogX 实例独立管理生命周期。

---

#### 🟡 [BUG-J6] `NewStringUTF(msg)` 在含非 ASCII 时的 Modified UTF-8 格式风险 — P2

**位置：** [`logx_jni.cpp:64`](../android/cpp/logx_jni.cpp#L64)

JNI `NewStringUTF` 要求 Modified UTF-8 规范。C++ 端错误消息若直接透传了外部非 ASCII 或含特殊字节的字符串，在个别 JVM 实现上可能触发编码异常。建议统一使用 UTF-16 构造。

---

### C. Kotlin 封装层 (`android/kotlin/LogX.kt`)

#### 🟠 [BUG-K5] 协程 `export` 丢弃 `nativeExport` 返回值，底层失败静默返回成功 — P1（新增）

**位置：** [`LogX.kt:177-196`](../android/kotlin/LogX.kt#L177-L196)

```kotlin
suspend fun export(from: String, to: String, block: suspend (File) -> Unit): Int = withContext(Dispatchers.IO) {
    val h = nativeHandle
    if (h == 0L) return@withContext -1
    var result = 0
    nativeExport(h, from, to) { _, plaintextPath, _ ->
        ...
    }
    result // ← 致命：永远返回 0！
}
```

**问题分析：**
`nativeExport` 本身返回一个 `Int` 状态码（成功返回 0，底层若遇到参数非法、目录不可读等错误返回负数错误码）。
但 `export` 协程函数完全没有接收 `nativeExport` 的返回值，在方法最后一行直接返回了初始化的 `var result = 0`。
若底层 `nativeExport` 彻底失败且从未触发回调，该函数依然会向业务侧返回 `0`（表示成功），导致业务误判导出状态。

**修复：**
接收 `nativeExport` 的返回值，并在出错时优先返回底层错误码：
```kotlin
val nativeRes = nativeExport(h, from, to) { ... }
if (nativeRes != 0) nativeRes else result
```

---

#### 🟠 [BUG-K3] `export` 协程中嵌套 `runBlocking` 阻塞 IO 调度器 — P1

**位置：** [`LogX.kt:184-186`](../android/kotlin/LogX.kt#L184-L186)

```kotlin
nativeExport(h, from, to) { _, plaintextPath, _ ->
    val tempFile = File(plaintextPath)
    try {
        kotlinx.coroutines.runBlocking {      // ← 阻塞 IO 调度器线程
            block(tempFile)
        }
        0
    }
```

在 `withContext(Dispatchers.IO)` 内再启动 `runBlocking` 阻塞线程，不仅违背协程规范，且会导致外部协程上下文与结构化并发特征丢失。应采用 `CountDownLatch` 或异步续体桥接。

---

#### 🟡 [BUG-K1] `init()` 失败时 `errorHandler` 静态引用未清理导致 GC 延迟 — P2

**位置：** [`LogX.kt:106-110`](../android/kotlin/LogX.kt#L106-L110)

若 `nativeInit` 返回 0 失败，抛出异常前未将 `errorHandler` 重置为 null。单例持有的引用会导致外部传入的回调对象无法及时释放。

---

#### 🟡 [BUG-K4] `setErrorHandler` 缺乏并发与时序校验 — P2

**位置：** [`LogX.kt:128`](../android/kotlin/LogX.kt#L128)

`setErrorHandler` 缺乏状态检查，允许在任意时机修改。建议增加并发保护或限定在初始化配置时传入。

---

### D. Python 离线解码器 (`decoder/logx_decode.py`)

#### 🟠 [BUG-D2] `parse_lgxe` cksum 失败直接 `break` 导致块内后续记录丢失 — P1

**位置：** [`logx_decode.py:158-163`](../decoder/logx_decode.py#L158-L163)

```python
actual = cksum1(raw[pos+6:pos+6+payload_len])
if actual != cksum:
    break                                  # ← C++ 端是滑动窗口 resync
```

C++ 端 `parse_records_stream` 在某条记录校验失败时，仅递增 1 字节尝试重新对齐 Magic。Python 端直接 `break`，导致该块内后续所有健康日志全部丢失。应改为 `pos += 1; continue`。

---

#### 🟠 [BUG-D3] `decode_file_records` Block CRC 失败直接 `break` 丢弃整文件后续日志 — P1

**位置：** [`logx_decode.py:259-261`](../decoder/logx_decode.py#L259-L261)

```python
blk_res = decode_block_raw(...)
if blk_res.error:
    errors.append(...)
    break                                  # ← C++ 端是 continue 到下一 Block
```

C++ 端遇到坏块时会记录标记并 `continue` 到下一块；Python 端直接 `break` 退出整文件扫描，单块损坏导致整文件后半部分截断。应改为 `off += block_size; continue`。

---

#### 🟠 [BUG-D4] `decode_block_raw` 的 GCM 校验失败处理策略与 C++ 端不一致 — P1

**位置：** [`logx_decode.py:198-205`](../decoder/logx_decode.py#L198-L205)

C++ 导出路径在 GCM 校验失败时静默丢块并触发 Event；Python 端输出到 stderr。需在解码器文档中明确错误策略差异。

---

#### 🟡 [BUG-D1] 死代码 `reassemble()` 残留 — P2

**位置：** [`logx_decode.py:134-140`](../decoder/logx_decode.py#L134-L140)

旧版切片重组函数已无任何调用者，容易误导维护者，应彻底清理。

---

#### 🟡 [BUG-D5] `decompress()` 内存输出上限 64MB 过宽 — P2

**位置：** [`logx_decode.py:73`](../decoder/logx_decode.py#L73)

C++ 内部块最大仅约为 20KB，Python 解码器允许 64MB 输出，存在遭遇恶意畸形帧时内存过度分配风险，建议设为 `20 * 1024 + 128`。

---

### E. 压力测试 (`tests/stress/*.cpp`)

#### 🟠 [BUG-T1] `rand()` 未播种导致目录重名风险 — P1

**位置：** [`test_concurrent.cpp:33-34`](../tests/stress/test_concurrent.cpp#L33-L34)、[`test_crash_recovery.cpp:33-34`](../tests/stress/test_crash_recovery.cpp#L33-L34)

```cpp
std::string dir = std::string("logx_stress_dir_") +
                  std::to_string(pal.pid()) + "_" +
                  std::to_string(rand());          // ← rand() 默认种子为 1
```

在多用例连续执行或 CI 重跑场景下，未 `srand()` 会产生确定的伪随机序列，若上轮异常退出遗留目录，可能引发目录冲突。建议改用原子自增计数器。

---

#### 🟠 [BUG-T2] `test_concurrent.cpp` 竞争线程过早启动导致前 15ms 空跑 — P1

**位置：** [`test_concurrent.cpp:117-126`](../tests/stress/test_concurrent.cpp#L117-L126)

竞争线程在发令枪响后立即每 15ms 调用 `logx_flush`，此时工作线程刚启动、staging 尚无数据，前 15ms 未构成实质竞争窗口。建议在 Worker 写入一定数据后再激活竞争。

---

### F. PAL 跨平台抽象层与 C ABI

#### 🟠 [BUG-P2] Windows `file_size_path` 频繁打开关闭句柄开销过高 — P1

**位置：** [`pal_win32.cpp:154-163`](../core/src/pal_win32.cpp#L154-L163)

Windows 版每次查询文件大小均执行 `CreateFileA + CloseHandle`。Recovery 阶段扫盘调用频繁，IO 开销偏大。建议改用 `GetFileAttributesExA`。

---

#### 🟠 [BUG-P3] `file_rmdir` 无明确错误转译 — P1

**位置：** [`pal_posix.cpp:162`](../core/src/pal_posix.cpp#L162)、[`pal_win32.cpp:182`](../core/src/pal_win32.cpp#L182)

`rmdir` 失败直接返回 -1，缺乏对 EEXIST/ENOTEMPTY 等具体错误码的区分与透传。

---

#### 🟠 [BUG-A1] `flush_threshold == 0` 语义模糊 — P1

**位置：** [`logx_c.h:62`](../core/include/logx_c.h#L62) + [`logx_c.cpp:260`](../core/src/logx_c.cpp#L260)

配置传入 0 会被静默替换为默认值 8KB。若业务意图是“彻底关闭自动刷盘，完全由手动 flush 控制”，该意图被吞。建议定义显式宏或常量（如 `UINT32_MAX`）表示禁用。

---

#### 🟠 [BUG-A2] `logx_stats_t` 结构体按值返回缺乏 ABI 向前兼容能力 — P1

**位置：** [`logx_c.h:130-136`](../core/include/logx_c.h#L130-L136)

结构体定义暴露在 C 头中按值拷贝。未来新增统计指标时会破坏既有二进制符号兼容性。

---

#### 🟡 [BUG-P4] Windows `atomic_add` 对齐前置检查 — P2

**位置：** [`pal_win32.cpp:307-310`](../core/src/pal_win32.cpp#L307-L310)

`InterlockedExchangeAdd64` 要求 64 位对齐，建议在 Debug 模式下对输入指针施加 `assert((reinterpret_cast<uintptr_t>(p) & 7) == 0)` 保护。

---

## 四、跨模块 T4.0 偏差落地状态（12 项跟踪）

| T4.0 编号 | Spec 规定 | 当前代码状态（2026-09-04 复核） | 影响与风险 |
|:---|:---|:---|:---|
| **T4.0-1** | §7 `flush()` 水位线 `target_seqno` 防饥饿 | `flush_blocking` 仍为轮询 `staging 空` + 5ms sleep | 高并发写入下 flush 容易饥饿阻塞 |
| **T4.0-2** | §3.1/§4.4 切分序号 2B (uint16_t) | 仍为 `uint8_t split_no` | 单日超过 255 个分卷停写丢日志 |
| **T4.0-3** | §4.2/§10 双 Header "写 A→seq_cst 屏障→写 B" | 实现为 A(中间态 CRC)→ B → `release fence` | 掉电极端窗口内存序存在轻微偏离 |
| **T4.0-4** | §4.1 Flusher 50~100ms Debounce 聚集刷盘 | `flusher_loop` 仍为无超时阻塞 `ev_wait` | 缺乏低频写定时兜底刷盘机制 |
| **T4.0-5** | §10 ARMv8 Crypto Extensions 加速 | 全库无 `__ARM_FEATURE_CRYPTO` 引用 | 缺少专用硬件加密指令加速 |
| **T4.0-6** | §11 运行期每 30s 定时 msync/fdatasync | 30s 定时器未实现；PAL 接口已实现但全库 0 调用 | 极端掉电下内核脏页刷盘时机不可控 |
| **T4.0-7** | §9 未对齐访问 / 23:59:59 时钟跳变用例 | ✅ 已通过 T4.5 落地 | `test_robustness.cpp` 已覆盖 |
| **T4.0-8** | §2/§10 离线解码工具 `logx_decode.py` | 坏块 break、死代码残留、容错性待对齐 | 影响离线日志捞取成功率 |
| **T4.0-9** | §7/§10 Kotlin Facade `LogX.kt` | export 丢弃 native 返回值；runBlocking 嵌套 | 导出状态误判与调度器线程阻塞 |
| **T4.0-10** | §7/§10 JNI Config 传递与 ErrorHandler | Compress 映射语义混淆；全局错误上下文单例化 | 多实例接入与算法配置异味 |
| **T4.0-11** | §4.3/§4.4 恢复后切分序号强制归 0 风险 | `flusher.cpp:447,456` 强制 `current_split = 0` | 致命覆写历史 `.log.1` 分卷（参 BUG-X2） |
| **T4.0-12** | §10 新建文件父目录 fsync | PAL 接口已实现但全库 0 调用 | 极端崩溃下可能出现目录项丢失 |

---

## 五、严重性汇总与优先修复建议

### 5.1 🔴 P0 立即修复（2 项核心数据破坏隐患）

1. **BUG-C1**：Codec 压缩降级未更新 Block Header Flag（导致解码必崩、整块数据报废）。
2. **BUG-X2**：崩溃恢复后 `current_split` 强制归零，写满切分直接覆写历史分卷日志。

### 5.2 🟠 P1 优先修复（11 项）

- **JNI 与并发安全**：BUG-J5（`nativeClose` 析构顺序竞态致 UAF）、BUG-J4（`g_err_ctx` 多实例隔离）。
- **解码容错与数据协议**：BUG-D2（Python 块内校验滑动重同步）、BUG-D3（Python 坏块 continue 容错）、BUG-1（`AppendTargetCache` packed 属性校准）、BUG-X1（分卷序号升级为 uint16_t）。
- **Kotlin 与导出稳定性**：BUG-K5（协程 export 接收 native 返回值）、BUG-K3（协程消除 runBlocking）、BUG-E1（export 临时文件名增加唯一标识）。
- **性能与工程规范**：BUG-P2（Windows 文件大小查询轻量化）、BUG-T1（测试目录随机数播种）。

### 5.3 🟡 P2 中低优先级（7 项）

- BUG-D1（清理 Python 死代码 `reassemble()`）
- BUG-D5（限制 Python zstd 内存上限）
- BUG-K1（init 失败清理 errorHandler 静态引用）
- BUG-J1（统一压缩算法枚举与 flag 命名）
- BUG-12（修正 `kBlockHeaderSize` 注释为 34B）
- BUG-A1（规范 flush_threshold 禁用语义）
- BUG-P4（Windows atomic_add 对齐断言）

---

## 六、测试覆盖率盲区真实评估

为什么上述最严重的两个 P0 隐患（BUG-C1 与 BUG-X2）在当前单元测试与压力测试中能够全绿通过？

1. **压缩失败降级路径测试盲区（掩盖 BUG-C1）**：
   当前 `test_codec.cpp` 仅测试了“压缩成功后解密”与“纯明文写入后解密”，**从未构造“显式开启压缩、但在运行时模拟压缩库失败触发降级，再将落盘块交由解码器还原”的负向回归用例**。正因缺少该闭环测试，导致 `hdr[5] = flag` 带着错误压缩位的毒丸逻辑一直未被触发。

2. **多历史分卷滚动追加测试盲区（掩盖 BUG-X2）**：
   当前 `test_crash_recovery.cpp` 仅测试了写入未切分的单个文件（`20260904.log`）后模拟崩溃并恢复，**从未测试“当天已切分出 `.log.1`、`.log.2`，崩溃后重新打开 `.log.2` 追加并再次写满 10MB 触发下一次滚动”的场景**。因此 `current_split = 0` 覆写历史分卷的隐患未被暴露。

> 结论：建议在下阶段重点补充上述两项负向与边界集成测试用例，并依序闭环修复 BUG-C1 与 BUG-X2。