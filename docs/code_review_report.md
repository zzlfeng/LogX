# LogX 综合代码审查报告

> 审查范围：`core/src/*.cpp`、`core/include/*.h`、`tests/unit/*.cpp`  
> 审查时间：2026-09-03（2026-09-04 交叉复核校准）  
> 关联补充：[code_review_supplement.md](./code_review_supplement.md)（周边交付件审查与误报校准）  
> 按模块分组，问题按严重性降序排列。

---

## 一、严重问题（🔴 P0/P1）

### 🟠 [BUG-1] `AppendTargetCache` 结构体缺少 packed 属性，实际大小因编译器对齐可能变为 16B 而非 13B

> [!NOTE] 交叉复核说明（校准为 P1）
> 结构体确实缺少 packed 属性（`sizeof=16`）。在当前单一 C++ 运行时内部，因写入与读取均通过 `sizeof(AppendTargetCache)` 做 `memcpy`，闭环内暂未发生数据错位。但在跨语言/离线解码器规范中破坏了 `reserved[0..12]` 的 13 字节二进制布局（padding 导致字段后移 3 字节），属于二进制协议破坏隐患。

**位置：** [`logx_staging.h`](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_staging.h)

```cpp
struct AppendTargetCache {
    uint32_t date;           // 4B
    uint8_t  split_no;       // 1B → 后面会被 padding 3B
    uint32_t last_block_offset;      // 4B
    uint32_t last_block_end_offset;  // 4B
};
// 注释说共 13B，但在 MSVC/ARM GCC 默认对齐下实际为 16B！
```

注释声明结构体共 13B，而实际上 `split_no`（1B）后面编译器会插入 3B 填充以对齐 `uint32_t`，导致结构体实际大小为 **16B**，而非 13B。当外部或跨平台协议读取 `reserved` 字节区域时会发生**数据错位**。

**修复：** 添加 `LOGX_PACKED_ATTR`（与 `FileHeader` 相同方式），或用 `put_u32`/`get_u32` 手动序列化。

---

### 🔴 [BUG-2] `kFlagCompressZstd` 与 `kFlagEncrypted` 数值相同（均为 0x1），极易误用

**位置：** [`logx_codec.h`](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_codec.h)

```cpp
constexpr uint8_t kFlagCompressZstd  = 0x1;   // ← 0x1
constexpr uint8_t kFlagEncrypted     = 0x1;   // ← 0x1 !!
```

设计意图是：压缩算法编号需左移 1 位写入 bit1~2（`kFlagCompressZstd << 1`），而加密标志占 bit0（`kFlagEncrypted`）。但两个常量数值相同且命名前缀都是 `kFlag`，极易混淆。`test_codec.cpp` L95 中确实存在手动 `<< 1` 的用法，说明此命名已造成实际困惑。

**修复：** 将算法编号常量重命名为 `kAlgoZstd = 1`（已有定义），完全废弃 `kFlagCompressZstd/kFlagCompressZlib`，或将其改为已预移位的值（`0x2`/`0x4`）。

---

### 🔴 [BUG-3] `init_crc32_table()` 全局状态无锁初始化（多线程数据竞争）

**位置：** [`staging.cpp`](file:///d:/Project/Android_Project/LogX/logx/core/src/staging.cpp)

```cpp
bool crc32_table_init = false;    // 非 atomic！
void init_crc32_table() {
    if (crc32_table_init) return; // ← 无锁 double-check
    ...
    crc32_table_init = true;
}
```

`crc32_table_init` 是普通 `bool`（非 `std::atomic`），Flusher 线程和用户线程并发首次触发时产生**数据竞争（Data Race）**，属于 C++ 未定义行为，TSan 会报竞争。

**修复：** 改用 `std::once_flag` + `std::call_once`，或改为函数内 `static` 局部变量初始化。

---

### 🔴 [BUG-4] `logx_set_error_handler` 无并发保护（`err_fn`/`err_user` 数据竞争）

**位置：** [`logx_c.cpp`](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp)

```cpp
logx_status_e logx_set_error_handler(...) {
    inst->err_fn   = fn;        // ← 无锁写
    inst->err_user = user;
    return LOGX_OK;
}
```

`err_fn` / `err_user` 字段没有任何互斥锁保护，而 Flusher 线程在 `emit_event` 中并发读取，构成**数据竞争**。头文件注释声明"可被任意线程调用"，风险更高。

**修复：** 对读写操作均加 `inst->mu` 保护，或改用 `std::atomic<logx_error_fn>` + `std::atomic<void*>`。

---

### 🔴 [BUG-5] `pal_win32.cpp`：HANDLE → int 转换在 64 位 Windows 上截断高 32 位

**位置：** [`pal_win32.cpp`](file:///d:/Project/Android_Project/LogX/logx/core/src/pal_win32.cpp)

```cpp
inline int handle_to_int(HANDLE h) {
    return static_cast<int>(reinterpret_cast<intptr_t>(h)); // 8B → 4B 截断！
}
```

64 位 Windows 进程中 `HANDLE` 是 8 字节，强转为 `int`（4 字节）截断高 32 位。内核对象 HANDLE 的高 32 位并不保证为 0，在极端情况下通过 `int_to_handle` 还原的 HANDLE 将错误，导致关闭错误句柄或资源泄漏。同根问题同时存在于 `thread_join` 的 `thread_start` 返回的线程 HANDLE 处理（风险-5）。

**修复：** 将 `PalMmap.out_handle` 及相关存储类型改为 `intptr_t` 或直接用 `HANDLE`。

---

### ⚪ [BUG-6] [已核验证实为误报] `debug_check_invariant()` 在不跨回绕场景漏检 `[0, f)` 区间

> [!NOTE] 核验结论：完全误报，已撤销
> 经复核 `staging.cpp:354-375`，在 `w >= f` 时 `len2 = 0, off1 = f`，循环 `for (i = len2; i < off1; ++i)` 正好检查 `[0, f)`，第二个循环检查 `[w, data_size_)`，完整覆盖了非活跃区。原审查推论有误。

**位置：** [`staging.cpp`](file:///d:/Project/Android_Project/LogX/logx/core/src/staging.cpp)

当 `w >= f`（不跨回绕）时：
- `len1 = w - f`, `off1 = f`
- `len2 = 0`, `off2 = 0`

（原推论认为漏检 `[0, f)`，实测代码中第一个循环已完整检查 `[0, f)`）。

---

### 🔴 [BUG-7] `format.cpp`：`tag[tag_len]` 在 tag 为非 NUL 结尾的 64 字节 buffer 时是越界读（UB）

**位置：** [`format.cpp`](file:///d:/Project/Android_Project/LogX/logx/core/src/format.cpp)

```cpp
for (const char* p = tag; *p && tag_len < 64; ++p) ++tag_len;
if (tag[tag_len] != '\0') return false;  // ← tag[64] 可能越界！
```

当 `tag` 恰好是一个 64 字节无 NUL 终止的 buffer 时，循环停止于 `tag_len == 64`，随后访问 `tag[64]` 超出 buffer 边界，属于**未定义行为（UB）**。

**修复：** 改用 `strnlen(tag, 65)` + 范围检查。

---

### 🔴 [BUG-8] `decode_block`：`raw`/`raw_len` 参数被完全忽略，CRC 校验从未执行

**位置：** [`codec.cpp`](file:///d:/Project/Android_Project/LogX/logx/core/src/codec.cpp)

```cpp
(void)raw;      // 注释说"CRC 由调用方算"，但若调用方忘记则无任何防护
(void)raw_len;
```

函数签名承诺接收 `raw`/`raw_len` 用于 CRC32 校验，但实现中用 `(void)` 直接忽略，若调用方未做 CRC 校验则数据完整性完全没有保证。`test_codec.cpp` 多处以 `nullptr, 0` 传入，测试完全绕过了这一路径。

**修复：** 在 `decode_block` 内部实现 CRC 校验，或从函数签名中删除这两个参数并在 API 文档中明确调用方责任。

---

## 二、中等问题（🟠 P1）

### 🟠 [BUG-9] `logx_write` 中 `build_records` 失败时 `drop_too_large` 统计未更新

**位置：** [`logx_c.cpp`](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp)

`drop_no_space` 在另一分支正确调用了 `fetch_add`，但 `build_records` 失败或 `total > max_log_length` 时返回 `LOGX_ERR_TOO_LARGE`，**`drop_too_large` 计数没有递增**，导致统计数据不准确。

---

### 🟠 [BUG-10] `recovery.cpp`：hint 文件不可读时，`was_truncated` 状态被错误传播给 best 文件

**位置：** [`recovery.cpp`](file:///d:/Project/Android_Project/LogX/logx/core/src/recovery.cpp)

```cpp
if (sr.truncated) was_truncated = true;   // hint 的 truncated 状态
if (!sr.ok) {
    sr = scan_append_target(inst, best);  // 覆盖 sr，但 was_truncated 未重置
}
```

若 hint 文件有截断且不可读（`ok=false`），`was_truncated=true` 会被误传给重新扫描的 best 文件，导致 truncated 状态误判。

---

### 🟠 [BUG-11] `recovery.cpp`：`S_hdr > S_file && truncated` 时丢块统计少计一次

**位置：** [`recovery.cpp`](file:///d:/Project/Android_Project/LogX/logx/core/src/recovery.cpp)

```cpp
if (S_hdr > seqno_file) {
    uint32_t lost = S_hdr - seqno_file;
    inst->truncated_blocks.fetch_add(lost);   // ← 未计入被截断的那个撕裂半块
    if (truncated) { emit_event(...); }
}
```

当文件中有被截断的撕裂块（`truncated=true`）且 `S_hdr > S_file` 同时成立时，被截断的那个块本身也是丢失块，但统计中**少计了 1**，应为 `S_hdr - seqno_file + 1`。

---

###  [BUG-12] `logx_staging.h`：`kBlockHeaderSize` 注释与实际求值不符（校准为 P2）

> [!NOTE] 交叉复核说明（校准为 P2）
> Nonce 布局实现正确（salt[8B] + seqno[4B] 大端）。实为 `logx_staging.h:194` 中 `kBlockHeaderSize` 计算常量末尾注释将 34B 笔误写为 32B，严重性校准为 P2。

**位置：** [`flusher.cpp`](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp)、[`logx_staging.h`](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_staging.h)

```cpp
std::memcpy(hdr + 22, salt, 8);          // nonce 前 8B = salt
hdr[30] = (seqno >> 24) & 0xff;          // nonce 后 4B = seqno 大端
hdr[31] = (seqno >> 16) & 0xff;
hdr[32] = (seqno >> 8) & 0xff;
hdr[33] = (seqno) & 0xff;
```

nonce 共 12B（`hdr[22..33]`），由文件盐值和 per-block seqno 拼接而成。`kBlockHeaderSize = 4 + 1 + 1 + 4 + 4 + 4 + 4 + 12 = 34`，但头文件末尾注释写为 `= 32B`，需修正该注释。

---

### 🟠 [BUG-13] `flusher.cpp`：`atc.last_block_offset` 将 `uint64_t` 截断为 `uint32_t`

**位置：** [`flusher.cpp`](file:///d:/Project/Android_Project/LogX/logx/core/src/flusher.cpp)

```cpp
atc.last_block_offset     = static_cast<uint32_t>(wr.start_offset);  // uint64_t → uint32_t
atc.last_block_end_offset = static_cast<uint32_t>(wr.end_offset);
```

虽然当前 `max_file_size` 默认 10MB，截断不会触发，但 `FileScanResult` 中同名字段为 `uint64_t`，两者类型不一致是设计上的隐患，应统一。

---

### ⚪ [BUG-14] [已核验证实为误报] `logx_c.cpp`：`decode_one_file` 中 CRC 校验对象需与写入端对齐

> [!NOTE] 核验结论：完全误报，已撤销
> 经交叉核验 `flusher.cpp:108` 与 `logx_c.cpp:110`，写入端与读取端计算 CRC32 时均使用的是不含 16B GCM tag 的密文 Payload（`payload_len` 长度一致），两侧完全对称，原审查疑虑不存在。

**位置：** [`logx_c.cpp`](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp)

```cpp
if (crc32_update(0, payload.data(), payload.size()) != crc) { ...continue; }
// 解码(raw bytes)
if (!decode_block(...)) { ...continue; }
```

---

### 🟠 [BUG-15] `max_log_length` 注释与默认值不一致（实际由 `logx_open` 拦截校验）

> [!NOTE] 交叉复核说明
> `max_log_length == 0` 时，`logx_c.cpp:244-246` 在 `logx_open` 中会主动拦截并返回 `LOGX_ERR_INVALID_ARG`，正常使用下不会发生静默丢弃所有日志的运行期行为。核心问题在于配置结构体中默认值未与注释 64KB 对齐。

**位置：** [`logx_internal.h`](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_internal.h)

```cpp
uint32_t max_log_length = 0;   // 注释写 "// 64KB 默认" ← 错误！
```

`max_log_length` 默认值为 0，注释和实际值不符，应将默认值显式改为 64KB。

---

### 🟠 [BUG-16] `parse_records_stream`：重同步后未清除 `carry_slice_buf` 残片数据

**位置：** [`format.cpp`](file:///d:/Project/Android_Project/LogX/logx/core/src/format.cpp)

当检测到无效 magic 并重同步后，`carry_slice_buf` 中已积累的跨片数据**没有被清除**，导致后续数据会与前一条的残片错误拼接，产生错乱输出。

**修复：** 重同步成功后立即调用 `current_slice_buf.clear()`。

---

### 🟠 [BUG-17] `logx_c.cpp`：Windows 路径分隔符仅查找 `'/'`，不兼容 `'\\'`

**位置：** [`logx_c.cpp`](file:///d:/Project/Android_Project/LogX/logx/core/src/logx_c.cpp)

```cpp
size_t pos = inst->staging_path.find_last_of('/');   // Windows 下 '\' 路径会失效
```

在 Windows 上路径可能使用 `\`（如 `C:\logx\staging.mmap`），此处只查找 `/`，会找不到分隔符，导致 `dir = "."` 而非正确的父目录，staging 目录创建失败。

**修复：** 改为 `find_last_of("/\\")`。

---

## 三、轻微问题（🟡 P2/P3）

### 🟡 [WARN-1] `format.cpp`：栈上 64KB scratch 数组，存在线程栈溢出风险

```cpp
uint8_t scratch[64 * 1024];   // 热路径！非主线程栈通常 1MB 或更小
```

`format_record` 是热路径，每次调用在栈上分配 64KB，非主线程（Flusher、JNI 调用链）的栈通常只有 512KB～1MB，存在栈溢出风险。**建议改为堆分配（`std::vector<uint8_t>`）或 `static thread_local`**。

---

### 🟡 [WARN-2] `pal_win32.cpp`：`monotonic_ns` 每次调用都查询性能计数器频率

```cpp
QueryPerformanceFrequency(&freq);  // ← 每次调用都查，应缓存为成员变量
QueryPerformanceCounter(&now);
```

频率在系统运行期间固定不变，应在初始化时缓存。

---

### 🟡 [WARN-3] `flusher.cpp`：`flush_blocking` 自旋等待无超时上界

```cpp
while (true) {
    if (bytes == 0) break;
    inst->pal->sleep_ms(5);   // 若 Flusher 挂死则永久阻塞
}
```

若 Flusher 线程挂起或死锁，`flush_blocking` 将**永久阻塞**。

---

### 🟡 [WARN-4] `flusher.cpp`：`split_no` 达到 255 后静默丢弃所有日志，无报警

```cpp
if (c->current_split >= 255) {
    close_current(c, inst->pal);
    return -1;   // ← 无 emit_event 报警
}
```

停写后没有触发 `emit_event` 通知用户，日志被静默丢弃。

---

### 🟡 [WARN-5] `pal_posix.cpp` / `pal_win32.cpp`：`random_bytes` 退化路径熵极低

```cpp
// 每 8 字节周期性重复，熵非常低
static_cast<uint8_t*>(buf)[i] = static_cast<uint8_t>((t >> (i % 8 * 8)) ^ (addr >> (i % 8 * 8)));
```

退化路径（`/dev/urandom` 或 `BCryptGenRandom` 失败时）生成的随机数质量极差，若在生产路径触发则构成严重安全风险（影响加密 nonce 和盐值）。

---

### 🟡 [WARN-6] `pal_win32.cpp`：`file_open` 中 `SetFilePointer` 设置末尾是冗余的

`O_APPEND` flag 已通过 `_open_osfhandle` 保证原子 append，`SetFilePointer` 只是在 open 时多移动一次指针，属于冗余但无害代码，容易误导维护者。

---

### 🟡 [WARN-7] `recovery.cpp`：`strtoul` 解析文件名日期未做范围校验，未复用 `parse_yyyymmdd`

`logx_c.cpp` 中 `parse_yyyymmdd` 做了完整的月/日范围校验，但 `recovery.cpp` 直接用 `strtoul` 解析，安全级别不一致。

---

### 🟡 [WARN-8] `logx_c.cpp`：同一天多个 split 文件导出时，临时文件路径相同可能互相覆盖

```cpp
// date 相同时，.tmp_export_YYYYMMDD 路径唯一，顺序处理不冲突，
// 但若回调失败则下一次导出会覆盖上次临时文件
snprintf(buf, ..., "%s/.tmp_export_%u", log_dir.c_str(), date);
```

---

### 🟡 [WARN-9] `format.cpp`：cksum 失败时 `pos += 1` 逐字节重扫，O(n²) 复杂度

```cpp
if (cksum1(payload, len) != cksum) {
    pos += 1;   // ← 应至少跳 kHeaderLen 或整条记录长度
    continue;
}
```

---

### 🟡 [WARN-10] `logx_c.cpp`：`logx_open` 失败路径 `staging` 已 delete 但指针未置 nullptr

属于防御性编程缺失，若析构函数未来引用 `staging` 指针会引发 double-free。

---

## 四、测试用例错误

### 🔴 [TEST-BUG-1] `test_flusher.cpp`：测试名称 `ExportWithMultipleRecordsAndMissingKey_HandledGracefully` 与实现内容不符

测试名暗示"缺失密钥时优雅降级"，但实际配置 `encrypt_enable = 0`（明文模式），根本没有涉及缺失 key 的场景，**测试名称与内容严重不符**，会误导维护人员。

---

### 🔴 [TEST-BUG-2] `test_staging.cpp`：`FlushAndZeroMaintainsInvariant` 断言永远为 true，无检测意义

```cpp
s.zero_range_locked(0, 256);   // 清零了 [0, 256)
EXPECT_TRUE(s.debug_check_invariant(0, 256));  // ← 全为零，断言必然 true
```

正确做法应在清零前验证该区间有数据，或在清零后用不同的 `(f, w)` 参数验证推进状态。

---

### 🔴 [TEST-BUG-3] `test_robustness.cpp`：PAL 注入时机晚于 Flusher 线程启动，产生数据竞争

```cpp
ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
static_cast<Instance*>(inst)->pal = &mock_pal;   // ← Flusher 已在使用 default pal
```

`logx_open` 内部已启动 Flusher 线程，PAL 注入在之后发生，Flusher 读 `inst->pal` 与测试写 `inst->pal` 并发，属于**数据竞争**，测试结果不可信。

---

### 🟠 [TEST-BUG-4] `test_recovery.cpp`：`RingScanStopsOnTear` 中 magic 字节构造可能与实际 `kMagicEntry` 不符

```cpp
chunk.push_back(0x4c); chunk.push_back(0x47); // magic LG（硬编码）
```

若 `kMagicEntry` 的实际值与此不符，`ring_scan` 会在 magic 校验处提前停止（而非在"payload 不足"处触发预期逻辑），测试可能意外通过但出于错误原因。

---

### 🟠 [TEST-BUG-5] `test_format.cpp`：`ParseSkipsPadAndReassembles` 硬编码 magic 字节（0x4c, 0x47）

使用硬编码魔数而非常量，未来常量修改时测试容易失效且可读性差。建议改用 `put_u16` 写入 `kMagicEntry`。

---

### 🟠 [TEST-BUG-6] `test_robustness.cpp`：`ClockRollback` 测试未验证 seqno 单调性和文件切换正确性

时钟回拨场景的两个核心保证均缺少断言：
1. 回拨后写入日志的 seqno 是否大于未来日期文件的 seqno；
2. Flusher 是否正确切换了日志文件。

---

### 🟠 [TEST-BUG-7] `test_codec.cpp`：GCM 测试 key 仅初始化前 3 字节，不具代表性

```cpp
uint8_t key1[32] = {1, 2, 3};   // 剩余 29B 全为 0
```

建议使用差异更大的完整 32B 密钥，与标准测试向量（NIST CAVS）对齐。

---

### 🟠 [TEST-BUG-8] `test_flusher.cpp`：`EncryptedBlockFileWriteAndDecode_Roundtrip` 未验证加密确实发生

测试只验证了解密后大小正确，没有验证"密文 != 明文"（即加密确实进行了），属于测试覆盖不足。

---

## 五、测试覆盖率分析

### ✅ 已充分覆盖

| 功能 | 测试文件 |
|------|------|
| 空环/满环 free 计算 | `test_staging.cpp` |
| 回绕写入 + PAD 不变量 | `test_staging.cpp` |
| 双 Header 容错（A 损坏 B 接管） | `test_staging.cpp` |
| mmap 失败降级堆内存 | `test_staging.cpp` |
| 多条记录切块（16KB 阈值） | `test_flusher.cpp` |
| 单条大记录独占块 | `test_flusher.cpp` |
| 保留期清理删除过期文件 | `test_flusher.cpp` |
| 文件大小阈值切分 | `test_flusher.cpp` |
| 压缩/解密往返（Zstd、AES-GCM） | `test_codec.cpp` |
| GCM 篡改拒绝 | `test_codec.cpp` |
| 整条/多片格式化 + parse | `test_format.cpp` |
| cksum 失败拒绝 | `test_format.cpp` |
| ring_scan 撕裂/全零终止 | `test_recovery.cpp` |
| 双 Header 各损坏场景（部分） | `test_recovery.cpp` |

### ❌ 重要缺口

| 缺失测试 | 风险 |
|------|------|
| `parse_records_stream` 跨块 carry（多块衔接场景） | 🔴 高 |
| `parse_records_stream` 重同步路径（坏 magic/cksum 后继续解析） | 🔴 高 |
| `encode_block` 压缩降级链（zstd 失败 → 明文降级） | 🔴 高 |
| `logx_c.cpp` export 路径（`export_range`、`decode_one_file`） | 🔴 高 |
| 双 Header 双坏（整区重建）路径 | 🟠 中 |
| 双 Header 容错（B 损坏，A 接管）反向场景 | 🟠 中 |
| `copy_pending` 跨回绕拷贝 | 🟠 中 |
| `zero_range_locked` 跨回绕清零 | 🟠 中 |
| `split_no` 达到 255 的停写行为 | 🟠 中 |
| `sum_raw_len_since` 正确性 | 🟠 中 |
| seqno 回绕（UINT32_MAX → 0） | 🟠 中 |
| `max_log_length == 0` 时的丢弃行为 | 🟠 中 |
| 并发 write + flush 竞争 | 🟠 中 |
| 加密块 nonce 不同 seqno 的唯一性（防重放） | 🟡 低 |
| `flush_blocking` 等待真实 Flusher 追上 | 🟡 低 |

---

## 六、优先级汇总表

| 优先级 | 编号 | 问题 |
|--------|------|------|
| **P0 🔴** | BUG-1 | `AppendTargetCache` 无 packed，实际大小 16B 而非 13B，数据错位 |
| **P0 🔴** | BUG-3 | `init_crc32_table` 无锁初始化，多线程数据竞争 |
| **P0 🔴** | BUG-4 | `err_fn`/`err_user` 数据竞争 |
| **P0 🔴** | BUG-5 | Win64 HANDLE → int 截断 |
| **P0 🔴** | BUG-2 | `kFlagCompressZstd == kFlagEncrypted == 0x1` 命名歧义 |
| **P1 🟠** | BUG-6 | `debug_check_invariant` 非回绕场景漏检 |
| **P1 🟠** | BUG-7 | `tag[tag_len]` UB 越界读 |
| **P1 🟠** | BUG-8 | `decode_block` CRC 参数被忽略 |
| **P1 🟠** | BUG-9 | `drop_too_large` 统计未更新 |
| **P1 🟠** | BUG-12 | 块头 nonce 布局无文档 |
| **P1 🟠** | BUG-14 | CRC 校验对象（密文 vs 明文）需核实 |
| **P1 🟠** | BUG-15 | `max_log_length == 0` 丢弃所有日志 |
| **P1 🟠** | BUG-16 | 重同步未清除 `carry_slice_buf` |
| **P1 🟠** | BUG-17 | Windows 路径 `'\\'` 分隔符不支持 |
| **P2 🟡** | WARN 系列 | 栈溢出风险、性能问题、资源限制无报警等 |
| **P2 🟡** | TEST-BUG 系列 | 多个测试用例错误或无效断言 |
| **P3 🟢** | 覆盖率缺口 | parse 重同步、export、跨回绕、并发等路径 |
