# LogX 代码审查问题合并清单

> 本文档将三份审查报告提出的全部问题**合并去重**，按模块归类整理，便于统一跟踪修复。
>
> **来源报告：**
> - 【报告一】[code_review_report.md](./code_review_report.md)（2026-09-03，含 09-04 交叉复核校准标注）
> - 【报告二】[code_review_report_2026-09-04.md](./code_review_report_2026-09-04.md)（2026-09-04）
> - 【报告三】[code_review_supplement.md](./code_review_supplement.md)（2026-09-04，周边交付件与 Spec 偏差）
>
> **编号规则：** 保留各报告原始编号（BUG-x / WARN-x / TEST-BUG-x / T4.0-x）；报告二条目以 R2-#n 标注。
> 同一问题被多份报告提及的，合并为一条并标注全部来源。报告原文中的"已撤销 / 已校准"标注原样保留。
>
> **严重性标记：** 🔴 P0（数据破坏/崩溃）｜🟠 P1（功能/并发/协议缺陷）｜🟡 P2（质量/性能/可维护性）｜⚪ 已撤销

---

## 目录

- [一、编解码模块（codec.cpp / logx_codec.h）](#一编解码模块)
- [二、Flusher 与文件写出（flusher.cpp）](#二flusher-与文件写出)
- [三、Staging 环形缓冲（staging.cpp / logx_staging.h）](#三staging-环形缓冲)
- [四、格式化模块（format.cpp）](#四格式化模块)
- [五、恢复模块（recovery.cpp）](#五恢复模块)
- [六、C ABI 与导出（logx_c.cpp / logx_c.h）](#六c-abi-与导出)
- [七、PAL 跨平台抽象层（pal_posix.cpp / pal_win32.cpp）](#七pal-跨平台抽象层)
- [八、JNI 层（logx_jni.cpp）](#八jni-层)
- [九、Kotlin 封装层（LogX.kt）](#九kotlin-封装层)
- [十、Python 离线解码器（logx_decode.py）](#十python-离线解码器)
- [十一、测试用例问题](#十一测试用例问题)
- [十二、测试覆盖率缺口](#十二测试覆盖率缺口)
- [十三、Spec 偏差跟踪（T4.0 共 12 项）](#十三spec-偏差跟踪)
- [十四、已撤销与校准条目存档](#十四已撤销与校准条目存档)
- [十五、优先级总表](#十五优先级总表)

---

## 一、编解码模块

### ✅ [C-01] 压缩失败降级后未更新块头 flag（毒丸块）[已闭环]
- **来源：** 报告二 R2-高#2 ＝ 报告三 BUG-C1（两份报告独立发现，均为 P0）
- **位置：** `core/src/codec.cpp`、`core/src/flusher.cpp`、`core/src/logx_c.cpp`
- **状态：** ✅ **已修复并闭环验证**（Commit `d0d157e` 及加固提交）
- **描述：** 原 `encode_block()` 内部将压缩算法降级为 none，但该修改只存在于局部变量，返回的 `EncodedBlock` 未携带最终 flag。Flusher 仍将原始 Zstd flag 写入块头 `hdr[5]`，导致：payload 实际为未压缩数据、块头声明为 Zstd、C++ 导出与 Python 解码器均尝试 Zstd 解压必然失败，**整块日志无法导出**。spec §6 承诺的"压缩失败平滑降级"实际落地为"静默产出毒丸块"。
- **测试掩盖原因（报告三）：** `test_codec.cpp` 从未构造"开启压缩→运行时压缩失败→降级→解码还原"的负向闭环用例。
- **修复方案与落地：**
  1. `EncodedBlock` 新增 `uint8_t final_flag`，`encode_block()` 计算实际生效的最终 flag（压缩降级清除 bit1..2，加密位跟随 `enc_ok`）。
  2. `append_block_raw` 严格使用 `enc.final_flag` 写入块头 `hdr[5]`，且发生压缩降级时派发 `LOGX_EV_DEGRADED` 告警事件。
  3. `logx_open` 增加 fail-fast 校验：未知算法、未实现的 zlib 以及未开启 ZSTD 宏却配置 zstd 均一律拒绝启动。
  4. 引入 `ScopedCompressFailInjection` RAII 守卫与测试 Seam，提供多线程无竞争的故障注入与异常安全重置。
  5. 补齐往返解码、反证防退化断言、加密组合及 `logx_open` fail-fast 单元测试，双构建矩阵全绿通过。

### ✅ [C-02] `kFlagCompressZstd` 与 `kFlagEncrypted` 数值相同（均为 0x1），命名歧义 [已闭环]
- **来源：** 报告一 BUG-2（汇总表列 P0）；报告三 BUG-J1 关联（JNI 端 Compress.ZSTD(1) 映射与 flag 字节布局语义不统一）
- **位置：** `core/include/logx_codec.h`
- **状态：** ✅ **已修复并闭环**（删除歧义常量 + helper 收敛魔法数 + 全栈命名对齐）
- **描述：** 设计意图为压缩算法编号需左移 1 位写入 bit1~2（`kFlagCompressZstd << 1`），加密标志占 bit0。两常量数值相同且命名前缀均为 `kFlag`，极易混淆。`test_codec.cpp` 存在手动 `<< 1` 用法，说明命名已造成实际困惑。（修复前核实：生产代码已迁移至 `kAlgo*`，仅 `test_codec.cpp` 残留 3 处 `kFlagCompressZstd << 1`，`kFlagCompressNone/Zlib` 为零引用死常量，但歧义常量未废弃。）
- **修复方案与落地：**
  1. 删除 `kFlagCompressNone/kFlagCompressZstd/kFlagCompressZlib` 三个歧义常量；保留 `kFlagEncrypted` 与 `kAlgo*` 算法编号。
  2. 新增 `kFlagCompressShift/kFlagCompressMask/kAlgoMask` 常量与 `make_flag()/flag_algo()/flag_encrypted()` 三个 constexpr helper，作为 flag 字节组装/解析的唯一入口。
  3. `codec.cpp`（encode/decode 解析、final_flag 重组）、`flusher.cpp`（降级比对、块大小计算、flag 构造）、`logx_c.cpp`（导出判定）中约 15 处手写移位与魔法数（`<< 1`、`(flag >> 1) & 0x3`、`& 0x6`、`~0x07`）全部收敛为 helper 调用。
  4. `test_codec.cpp` 清理全部手写移位改用 helper，并新增 `BlockFlagHelper_BitLayoutAndSemantics` 位布局防退化测试（全枚举锁定线上格式 + 对称反解 + 越界截断防御）。
  5. 全栈命名对齐：JNI 映射处、Kotlin `Compress` 枚举、Python 解码器常量区补充与 `kAlgo*`/位布局的显式对应注释，Python 新增 `FLAG_COMPRESS_SHIFT` 常量替换裸移位。线上 flag 字节格式不变（bit0=加密、bit1..2=算法），纯编译期重构，无 ABI/行为变化。

### ✅ [C-03] `decode_block` 的 `raw`/`raw_len` 参数被完全忽略，CRC 校验从未执行 [已闭环]
- **来源：** 报告一 BUG-8 ＝ 报告二 R2-中#12
- **位置：** `core/src/codec.cpp:232`
- **状态：** ✅ **已修复并闭环**（删除无效 raw 参数 + 实现长度交叉校验 + API 契约文档化）
- **描述：** 函数签名承诺接收 `raw`/`raw_len` 用于校验，实现中以 `(void)` 忽略。若调用方未做 CRC 校验则数据完整性无保证；`test_codec.cpp` 多处以 `nullptr, 0` 传入，测试完全绕过该路径。（修复前核实：生产调用方 `logx_c.cpp:110` 调用前已自行做 payload CRC 校验，实际风险低于报告定级；真实缺口是块头 `raw_len` 与解码输出长度之间无交叉校验——块头本身不在 CRC 覆盖范围内，畸形 raw_len 无人验证；且头文件声称失败原因含 "crc"，与实现自相矛盾。）
- **修复方案与落地：**
  1. 删除从未被有效使用的 `raw` 指针参数；`raw_len` 重定义为 `expected_raw_len`（期望解码输出长度，取自块头 raw_len 字段），传 0 跳过校验。
  2. `decode_block` 内解压完成后追加长度交叉校验：长度不符时清空输出并返回 false（宁丢不脏），畸形块头 raw_len 在解码层被拦截。
  3. 生产调用方 `logx_c.cpp` 传入块头 `raw_len`，导出路径自动获得长度交叉校验；CRC 预检逻辑保留。
  4. 头文件注释明确 API 契约："失败原因：gcm / 压缩失败 / 输出长度不一致；payload 完整性（CRC32）由调用方在调用前校验"，消除声明与实现的矛盾。
  5. 全库 11 处调用点（test_codec 9 处 + test_flusher 1 处 + logx_c 1 处）同步适配；新增 `DecodeBlock_RejectsLengthMismatch` 负向用例（基线通过 / 长度偏大偏小拒绝且输出为空 / 传 0 跳过校验），双构建矩阵全绿通过。
  6. 防御性深度加固：`decode_block` 与 `encode_block` 补齐输出指针与 payload 空指针前置拦截；`flusher.cpp:sum_raw_len_since` 增加 `(0, kSingleRecordMaxRaw]` 边界校验防畸形块头错误推进水位（前置闭环 F-05 关键路径）；单测追加 `UINT32_MAX` 极值与真实 Zstd 压缩块长度不符拒绝断言。

### ✅ [C-04] 缺少 ARMv8 Crypto Extensions 硬件加速（T4.0-5 偏差）[已修复并闭环]
- **来源：** 报告三 §四 T4.0-5（Spec 偏差）
- **位置：** `core/src/codec.cpp`、`core/include/logx_codec.h`、`third_party/mbedtls/`
- **状态：** ✅ **已修复并闭环**（移植 ARMv8 AESCE 硬件路径 + 标准指令序列直通 + 三重运行时防护）
- **描述：** spec §10 规定支持 ARMv8 Crypto Extensions 专用硬件加速指令；当前代码全库无 `__ARM_FEATURE_CRYPTO` 引用，移动端加密吞吐完全依赖通用软实现，未发挥移动端硬件加速性能。（修复前核实：加密栈为 vendored 精简版 mbedtls——`aes.c` 341 行纯查表软实现 + `gcm.c` 305 行，无官方版的 `aesce.c`；Android 构建目标 arm64-v8a + NDK 29 正是 spec 要求加速的平台；GCM 的 3 处 AES 调用全部为 ENCRYPT 方向，只需加速加密原语。）
- **修复方案与落地：**
  1. **新增 `third_party/mbedtls/src/aesce.c`**：以 `vaeseq_u8`/`vaesmcq_u8`/`veorq_u8` intrinsics 标准序列实现硬件单块加密（`AESE` 自动完成 `ShiftRows(SubBytes(state ^ rk))`；中间轮接 `vaesmcq_u8`；最后一轮由 `vaeseq_u8` 处理并由 `veorq_u8` 叠加最终轮密钥）。
  2. **内存字节序直通与零转置**：深入分析确认 vendored 软实现生成的 `ctx->buf` 在 Little-Endian ARM64 下内存分布天然符合 AES 列主序；彻底剔除错误的 4x4 转置逻辑（`kTransposeIdx` / `vqtbl1q_u8`）与多余的 InvMixColumns 变换，加密轮密钥无需任何形态转换即可直接直通硬件指令。
  3. **三重运行时防护**：① auxv 探测（`getauxval(AT_HWCAP) & HWCAP_AES`）——无扩展 CPU 上绝不执行 AESE（否则 SIGILL）；② 启用时自检（Self-check）——硬件路径先与查表软实现比对 2 个测试块，双块一致方置 `ce_mode = 1`，任一不符即安全回退软路径；③ 软实现完整保留为通用兜底。
  4. **构建适配**：`core/CMakeLists.txt` 仅对 `aesce.c` 在 arm64 目标追加 `-march=armv8-a+crypto`；x86 桌面构建下编译为空，软路径 100% 兜底兼容。
  5. **真机全绿验证（2026-09-06，Redmi K30 Pro / 骁龙865 / arm64-v8a）**：
     - `ArmCe_HardwarePathOnDevice` 用例真机运行 100% 通过（`AT_HWCAP` 探测通过，`ce_mode == 1` 硬件加速已激活，AES-256 输出与 NIST SP 800-38A F.1.5 向量严格比对通过，多测试块与软实现比对通过，GCM 全链路往返一致）；
     - 单元测试全部通过（0 失败）；
     - 压力测试全部通过（50 线程高并发压测吞吐达 10.5 万条/秒，50 轮崩溃注入恢复率 100%）。
   6. **热路径优化、官方向量锚定与健壮性加固（2026-09-06）**：
      - **消除热路径探针**：`mbedtls_aes_crypt_ecb`（aes.c）移除逐块 `getauxval` 调用，改以 `#if defined(LOGX_AESCE_HOOK)` 条件编译 + `ctx->ce_mode != 0` 单条件派发硬件 ECB——`ce_mode=1` 只能在 `CE_MODE_ENABLED()` 为真且自检通过时置位（aes.c setkey_enc 路径），故 per-context 标志已隐含运行时支持成立，无需二次探测；桌面 x86 下该段整体编译剔除，软路径 100% 兜底；
      - **探测函数防竞态定型**：`mbedtls_aesce_has_support()` 定型为纯无状态读取（首次实现曾引入函数内 `static int` 惰性缓存，复审判定该缓存的读路径不受 magic static 初始化保护、构成 C++ 数据竞争 UB，与 S-01/E-01 同族，已回退为无状态实现；该函数现仅在 setkey 一次性路径被调用，无缓存必要）；
      - **GCM 权威官方向量锚点**：`test_codec.cpp` 新增 `GCM_OfficialNistVectors_Anchor`——零密钥基线 2 组（K=0^256/IV=0^96 的空明文与 16B 全零明文边界向量）+ McGrew-Viega 附录 B **TC13**（空明文）、**TC14**（64B 明文）、**TC15**（64B 明文 + 20B AAD，覆盖 AAD 认证路径），全部常量经 vendored gcm.c 实测复现 + OpenSSL 后端 `cryptography` 库交叉验证一致，根除"往返测试对加解密对称性错误"的盲区；
      - **防御性缺口补齐**：`gcm.c` 的 `gcm_starts` 与 `mbedtls_gcm_crypt_and_tag` 均补齐 `add_len > 0 && add == NULL` 判空防护，`test_codec.cpp` 增加对应防御性断言（安全返回 `MBEDTLS_ERR_GCM_BAD_INPUT` 而非崩溃）；
      - **密钥长度全覆盖与注释纠偏**：删除 `test_codec.cpp` 中"vendored 软实现 128 位路径与 FIPS-197 C.1 不符"的错误历史注释（经实测 AES-128/192/256 与官方向量逐字节一致，该推断从未成立），CE/软实现交叉比对放开至 128 / 192 / 256 三种密钥长度；
      - **已知取舍归档**：非 Linux/Android 的 aarch64（无 auxv 平台）`mbedtls_aesce_has_support()` 依赖编译特性宏直接返回 1、由运行时自检兜底；当前 CMake 配置下 Windows 构建不编译 CE 路径，符合 Android/x64 平台定位，未来若需支持 Windows on ARM 再补平台探测。
   7. **独立复审实测验证（2026-09-06，x86 双重交叉 + Redmi K30 Pro 真机）**：
      - **独立 harness 全部密码原语从 spec 重新推导**（S-box 由 FIPS-197 定义生成、GF(2^128) 乘法独立实现、AES 参考实现独立编写），消除手抄表与记忆向量的引入误差；
      - **x86 实测**：镜像 `aesce.c` 轨迹（x86 仿真 AESE/AESMC、读同一份 `ctx->buf`、零转置直铺）在 AES-128/192/256 上全部复现官方向量，实证"零转置直通"结论；vendored 软实现 AES-128 与 FIPS-197 C.1 逐字节一致（"AES-128 不符标准"传闻被驳回）；vendored GCM 与 OpenSSL 后端 `cryptography` 库 3 密钥长度 × 12 明文长度 × 有/无 AAD 共 **72/72 全匹配**；
      - **真机全量回归（骁龙865，修改后源码重建）**：真实 AESE/AESMC 指令序列四组官方向量全过、`ce_mode==1` 硬件路径激活、GCM 全长度链路与独立实现一致、8 线程 × 20 万次并发 `has_support()` 压测（160 万次读）全对——**ALL CHECKS PASSED**；
      - **复审驳斥两项外部质疑**（留档防再传播）：①"CE 桥接缺失、需 4x4 转置（含 IMC 密钥形态）"不成立——LE uint32 数组小端铺回内存即 FIPS 列主序，转置/IMC 是上游解密方向设计，与本项目仅加密的用法无关；②"vendored 软实现 AES-128 与 FIPS-197 C.1 不符"不成立——实测一致，该说法源自 test_codec.cpp 已删除的错误注释。

---

## 二、Flusher 与文件写出

### ✅ [F-01] 生产路径加密文件头没有记录加密标志 [已闭环]
- **来源：** 报告二 R2-高#1
- **位置：** `core/src/flusher.cpp:186`（`write_file_header`）、导出判定与校验 `core/src/logx_c.cpp:70, 107, 275`
- **状态：** ✅ **已修复并双矩阵闭环验证**
- **描述：** 原 `write_file_header()` 固定写 `hdr[5] = 0`，即使后续块使用 AES-GCM，文件头仍声明为明文文件。导出路径根据**文件头**判断是否需要密钥，生产路径生成的加密文件会被当作明文处理，导致 `key32 = nullptr`，所有加密块在 `decode_block` 处全部失败并输出 `[seq=%u decode fail]`。现有加密测试（`test_flusher.cpp:192`）手工构造带 `kFlagEncrypted` 的文件头绕过了该问题，而 `EndToEndFormattedLog_WriteAndExport` 仅断言 `EXPECT_GT(size, 0u)`，因失败占位行产生 20B 字节产生假绿。
- **修复方案与落地：**
  1. `write_file_header` 改为调用 `hdr[5] = make_flag(inst->compress_algo, inst->encrypt_enable != 0)`，遵循 C-02 编码契约，如实记录加密状态与初始压缩算法。
  2. `logx_c.cpp:decode_one_file` 增加块级惰性取 Key 兜底：若块头含 `kFlagEncrypted` 而 `key32 == nullptr`，且 `key_provider` 存在，则用 `kid_in_file` 惰性获取密钥并缓存（引入 `key_lookup_attempted` 失败记忆，防重复空耗调用），确保修复前落盘的历史存量加密文件（`hdr[5] == 0`）与混合文件仍能平滑解密。
  3. `logx_c.cpp:logx_open` 增加 fail-fast 启动校验：配置 `encrypt_enable != 0` 但未提供 `key_provider` 或编译期未接入 AES-GCM 时，立即返回 `LOGX_ERR_INVALID_ARG` 拒绝启动，彻底阻断错误配置。
  4. `test_flusher.cpp` 彻底清理假绿：加固 `EndToEndFormattedLog_WriteAndExport`，核验落盘文件头 `hdr[5]` 的 `kFlagEncrypted` 标志与 CRC，并断言导出明文不含 `decode fail` 且包含真实载荷 Hex 特征；新增 `ExportHistoricalEncryptedFileWithUnsetFileHeaderFlag_RecoversSuccessfully`（采用动态墙钟日期 + 超长保留期，根除时效炸弹）验证历史存量 `hdr[5]=0` 文件的向前兼容解密；`EncryptedBlockFileWriteAndDecode_Roundtrip` 补充密文与明文不一致断言（联动 TEST-BUG-8）。
  5. 双构建矩阵（全开 ON/ON: 69/69 单测 + 4/4 压测；基础 OFF/OFF: 52/52 单测 + 4/4 压测）重新编译验证 100% 全绿通过。

### 🔴 [F-02] 恢复后分卷号被强制重置为 0，可能覆写历史分卷文件 [✅ 已修复]
- **来源：** 报告二 R2-中#9（列为中等）；报告三 BUG-X2（校准为 P0）；T4.0-11
- **位置：** `core/src/flusher.cpp:447, 456`（另：`core/src/recovery.cpp:194, 317` 的 `atc.split_no = 0` 为同族问题）
- **描述：** 命中历史文件（如 `20260904.log.3`）时代码只解析日期并执行 `current_split = 0`。该文件后续写满触发滚动切分时，将生成 `20260904.log.1` 并**直接覆盖/篡改当天已落盘的历史分卷**，造成持久化数据破坏。
- **测试掩盖原因（报告三）：** 压测从未测试"当天已切分出 `.log.1`/`.log.2`，崩溃后重新打开 `.log.2` 追加并再次写满触发滚动"的场景。
- **修复措施（已闭环验证）：**
  1. `core/include/logx_internal.h` 声明、`core/src/flusher.cpp` 实现统一路径解析 helper `parse_log_filename`，跨平台兼容 `/` 与 `\\` 分隔符，严格校验 8 位日期与 `.log` / `.log.N`（`N <= 255`）合法性。
  2. 彻底修复三处分卷归零点：
     - `flusher.cpp:ensure_append_target`：命中 hint 后调用 `parse_log_filename` 同步恢复真实 `current_date` 与 `current_split`，并以 `sr.last_block_end_offset` 初始化 `current_size`；
     - `recovery.cpp:run_recovery`（resume 路径）：从 `rec.target_path` 解析真实分卷号赋值 `atc.split_no`，修复原 `find_last_of('/')` 在 Windows 下失效的问题；
     - `recovery.cpp:run_recovery`（rebuilt 路径）：从 `best` 目标解析真实分卷号赋值 `atc.split_no` 并记录块偏移；
     - `recovery.cpp:reconcile`：统一接入 `make_log_path` 生成 hint 路径，消除局部缓冲区溢出隐患。
  3. 滚动切分增加“绝不覆盖已存在分卷”循环探测安全网（`flusher.cpp:467-477`）：切分时 `current_split += 1` 配合 `pal->file_size_path() >= 0` 循环递增至空闲分卷（或达 255 上限），确保即使升级前磁盘残留 `split_no=0` 的旧 staging Header，也绝对不会覆写已存在历史分卷。
  4. 补齐四大严密测试用例（T1~T4）：
     - `test_flusher.cpp`：`FileSizeThreshold_ResumeRotatesToNextSplitWithoutOverwrite`（T1，跨重启追加写满滚动至 `.log.3`，逐字节校验历史 `.log`/`.log.1` 100% 未篡改）；
     - `test_flusher.cpp`：`FileSizeThreshold_DefensiveProbeSkipsExistingSplit`（T4，手工注入旧 staging split=0 缓存，断言跳过已存在 `.log.1` 并安全写入空闲分卷 `.log.2`，`.log.1` 逐字节无损）；
     - `test_flusher.cpp`：`ParseLogFilename_Comprehensive`（全边界路径与异常输入合法性校验）；
     - `test_recovery.cpp`：`RecoveryResume_PreservesSplitNoInAppendTargetCache`（T2）与 `RecoveryRebuilt_PreservesSplitNoInAppendTargetCache`（T3），覆盖两种恢复路径下 `atc.split_no == 2` 的正确接管与持久化保持。
  5. 双构建矩阵（全开 ON/ON: 74/74 单测 + 4/4 压测；基础 OFF/OFF: 57/57 单测 + 4/4 压测）及 Android arm64-v8a 交叉编译全绿通过。

### 🟢 [F-03] 短写会造成日志永久丢失 [✅ 已修复]
- **来源：** 报告二 R2-高#3
- **位置：** `core/src/flusher.cpp:138-144`（`append_block_raw`）、`flusher.cpp:163-171`（`write_and_record`）、`flusher.cpp:255-274`（`write_file_header`）
- **描述：** 只判断 `file_write()` 是否返回负数，未确认返回值是否等于完整 frame 长度。若 PAL 实现或注入测试返回部分写入（短写），代码仍视为块写入成功，随后清零 staging 原始数据、提交新 flushed 水位，丢失尚未完整落盘的日志。且物理文件留下撕裂尾部，污染后续日志块写入。
- **修复方案与落地：**
  1. `append_block_raw` 严格校验返回值 `n != static_cast<long>(frame.size())`：任何短写均视为失败并触发 `block_write_errors.fetch_add(1)` 与 `emit_event(inst, LOGX_EV_IO, -1, "block short write")`；
  2. `write_and_record` 增加物理撕裂尾部回滚截断：在写入失败（短写或返回 -1）后，检测若磁盘文件已增长（`cur_sz > sz`），立即调用 `file_truncate(fd, sz)` 回滚至写前偏移，避免留下撕裂半块污染后续 append；若截断失败尽力而为上报 `LOGX_EV_IO`；
  3. `write_file_header` 保持契约对称性：当写入非 32B 时，调用 `file_truncate(fd, 0)` 抹除残损头部；
  4. 单测闭环：在 `tests/unit/test_robustness.cpp` 中新增 `MockShortWritePal` 及 3 组完整鲁棒性测试用例：
     - `ShortWrite_RollsBackTornBytesAndEmitsEvent` (Case A: 短写被截断回滚至写前偏移、记录错误统计与触发告警事件、`scan_one` 评估健康且 `truncated == false`)；
     - `ShortWrite_TransientFailureAllowsSubsequentBlocksToResumeCleanly` (Case B: 瞬态短写失败回滚后，后续块正常追加落盘，`logx_export` 正常解码且无任何数据损坏)；
     - `ShortWrite_MidStreamEnospcTruncatesTornTail` (Case C: 模拟物理写入中途磁盘满返回 -1，验证残留物理尾部被自动回滚截断)。
  5. 双构建矩阵（全开 ON/ON: 77/77 单测 + 4/4 压测；基础 OFF/OFF: 60/60 单测 + 4/4 压测）及 Android arm64-v8a 交叉编译全绿通过。

### 🟠 [F-04] 关键写入流程没有调用持久化同步 [✅ 已修复]
- **来源：** 报告二 R2-高#7；T4.0-6 / T4.0-12 关联
- **位置：** `core/src/staging.cpp:57`（close 时 `mmap_sync(mm_, false)` 异步）；PAL 已实现 `file_fsync()`、`file_dir_fsync()` 但全库 0 调用
- **描述：** `logx_flush()` 返回后若进程或设备立即断电，文件块、Header 或目录项可能仍未持久化，无法满足强崩溃恢复语义。
- **修复措施与结论（Commit `d6...` 闭环）：**
  1. **耐久性同步核心抽象**：在 `flusher.cpp` 与 `logx_internal.h` 增加 `durability_sync(Instance* inst, uint64_t* out_sync_time_ms)`，协调 staging 强同步与活跃日志文件 `fdatasync`（`out_sync_time_ms` 输出完成同步时的系统时间戳毫秒）；失败发出 `LOGX_EV_IO` 事件，不阻断前台业务写入。
  2. **Staging 强同步能力补齐**：在 `logx_staging.h` 与 `staging.cpp` 实现 `Staging::mmap_sync_now()`（普通模式执行 `MS_SYNC` / `FlushViewOfFile` 强同步并刷新 staging 句柄 `file_fsync(fd_)`；堆降级模式写回脏数据并 fsync）；同时将 `Staging::close()` 升级为同步刷盘。
  3. **新建文件父目录持久化（spec §10 / T4.0-12）**：在 `open_or_create_append_target` 与 `append_block` 中，新创建 `.log` 文件写完 32B 文件头后立即执行 `file_fsync(fd)` 与 `file_dir_fsync(inst->log_dir)`，确保断电下目录项与文件头绝对不丢失。
  4. **运行期 30s 定时检查点（spec §11 / T4.0-6）**：`flusher_loop` 引入 `last_sync_ms` 时间戳与 `ev_wait_timeout(30000)` 唤醒机制，保证运行期最多每 30 秒主动执行一次强刷盘检查点；退出时执行终态同步。
  5. **退后台与退出持久化**：`flush_blocking`（退后台）与 `close_current`/`logx_close`（退出）均接入 `durability_sync`。
  6. **Win32 目录 fsync 权限修复**：修正 `pal_win32.cpp` 中 `file_dir_fsync` 打开目录句柄时遗漏的 `FILE_ADD_FILE` 权限，消除 Windows 下 `FlushFileBuffers` 报 `ERROR_ACCESS_DENIED`（错误码 5）缺陷。
  7. **单测覆盖（`tests/unit/test_robustness.cpp` 新增 5 组用例）**：
     - `Persistence_FlushTriggersFileAndStagingFsync`：验证退后台 `logx_flush` 触发活跃日志文件与 staging 的强同步；
     - `Persistence_DurabilitySyncDirectAndGraceful`：验证直接调用 `durability_sync` 且无 active fd 时优雅返回 0；
     - `Persistence_NewFileTriggersParentDirFsync`：验证新建文件触发父目录同步且匹配目录路径；
     - `Persistence_CloseTriggersFinalSync`：验证 `logx_close` 在关闭句柄前触发终态强刷盘；
     - `Persistence_FsyncFailureEmitsEventWithoutBlocking`：注入 `file_fsync` 失败，验证发出 `LOGX_EV_IO` 且后续写入正常继续。
  8. **双构建矩阵与交叉编译验证**：
     - 全开 ON/ON: 82/82 单测 + 4/4 压测全绿通过；
     - 基础 OFF/OFF: 65/65 单测 + 4/4 压测全绿通过；
     - Android arm64-v8a NDK Clang Ninja 交叉编译零警告零错误通过。

### 🟠 [F-05] 恢复扫描对块头字段校验不足 [✅ 已修复]
- **来源：** 报告二 R2-高#8
- **位置：** `core/src/flusher.cpp`（`block_header_plausible`、`scan_one`、`sum_raw_len_since`）、`core/include/logx_internal.h`、`core/src/recovery.cpp`、`core/src/logx_c.cpp`
- **状态：** ✅ **已修复并全矩阵闭环验证**
- **描述：** 恢复逻辑使用块头中的 `raw_len` 推进 staging 水位，但未完整验证：`raw_len` 合法范围、解码结果长度一致性、块版本支持、压缩算法标志有效、seqno 连续、`payload_len` 与块边界满足协议。畸形或损坏块可能导致恢复错误清零 staging 区间，造成额外数据丢失。（根因之一：34B 块头本身不在任何 CRC 覆盖范围内。）
- **修复方案与落地：**
  1. **块头合理性全字段严检（`block_header_plausible`）**：在 `core/include/logx_internal.h` 声明、`core/src/flusher.cpp` 实现统一块头解析与前置校验 helper，覆盖以下严苛契约：
     - Magic 校验：必须为 `kMagicBlock`（`0x4258474c`，'LGXB'）；
     - 块版本校验：`bhdr[4] == 1`（拒绝非法版本/未来协议/0xFF）；
     - Flag 保留位校验：严格限制 `(flag & ~0x07) == 0`，保留位（bit3~bit7）非零一律拒绝；
     - 压缩算法支持性与宏匹配：`flag_algo(flag) <= kAlgoZstd`，且未定义 `LOGX_WITH_ZSTD` 时仅允许 `kAlgoNone`；
     - seqno 严格单调自增：`seqno > 0`，且块间强制 `seqno > prev_seqno`（拒绝乱序、回滚与重复，同时天生兼容截断修复跳跃合法空洞）；
     - `raw_len` / `payload_len` 合法性与协议自洽：`raw_len ∈ [6, 16MB]`（记录头至少 6B），未压缩时（`algo == kAlgoNone`）强制 `payload_len == raw_len`（无论加密与否，密文长度严格等于明文长度）；
     - 文件物理容量防御：`block_size = 34 + payload_len + (enc ? 16 : 0) <= file_remaining_bytes`。
  2. **扫描与修复路径统一接入**：
     - `scan_one`：维护 `prev_seqno`，凡块头校验失败立即触发 `repair` 截断修复（宁丢不脏）；
     - `sum_raw_len_since`：统一接入 `block_header_plausible`，并补齐与 `scan_one` 对称的 payload CRC32 及 GCM Tag 可读性验证，坏块提前 break；
     - `logx_c.cpp:export_file_range`：导出路径同样接入 `block_header_plausible`，坏块立即终止扫描，根除损坏块头导致内存越界或死循环风险。
  3. **双层水位回推上界与溢出熔断**：
     - 第一层（`sum_raw_len_since`）：累加过程中检测 `*out_bytes + raw_len >= staging_capacity` 时主动终止对账累加；
     - 第二层（`recovery.cpp:run_recovery`）：推进 `F` 前做硬防御 `if (sum < size)`，若累加值异常超限则拒绝回绕、发射 `LOGX_EV_RECOVERY` 告警并安全信任文件 seqno，杜绝 staging 内部数据结构静默被破坏。
  4. **单测全闭环（`tests/unit/test_recovery.cpp` 新增 6 组用例）**：
     - `CorruptBlock_BadVersion_TruncatedOnRecovery`：验证非法块版本（ver=2）触发截断修复；
     - `CorruptBlock_BadFlag_TruncatedOnRecovery`：验证保留位非零（bit7=1）触发截断修复；
     - `CorruptBlock_MismatchedRawLenAndPayloadLen_TruncatedOnRecovery`：验证非压缩块 raw_len 与 payload_len 不一致时被拦截并截断；
     - `CorruptBlock_OutOfRangeRawLen_TruncatedOnRecovery`：验证 `raw_len < 6` 及超限极大值被拦截并截断；
     - `CorruptBlock_SeqnoRollbackStopsBackfill_NormalGapAllowed`：验证截断跳跃合法 seqno 空洞正常放行，seqno 重复与回滚被严厉拦截截断；
     - `Recovery_SumRawLenExceedsCapacity_DoesNotCorruptStaging`：构造超环形容量的多块累加场景，验证双层容量熔断防御生效、staging 环形不变量完好保持。
  5. **三构建矩阵全绿验证**：
     - 基础构建（OFF/OFF）：71/71 单测 + 4/4 压测 100% 通过；
     - 全开构建（ON/ON）：88/88 单测 + 4/4 压测 100% 通过（含 50 线程高并发压测 10 轮连续跑全过）；
     - Android arm64-v8a：交叉编译 0 警告 0 错误通过。

### 🟠 [F-06] `atc.last_block_offset` 将 `uint64_t` 截断为 `uint32_t` [✅ 已修复]
- **来源：** 报告一 BUG-13
- **位置：** `core/include/logx_staging.h:63-68`、`core/include/logx_internal.h:120-136`、`core/src/flusher.cpp:558, 604, 731-735`、`core/src/recovery.cpp:181-185`
- **状态：** ✅ **已修复并全矩阵闭环验证**（方案 D：契约锁定 + 4GiB 强制切分防线 + 越界哨兵守卫）
- **事实核验与风险定级校准：**
  1. 真实消费链核实（`recovery.cpp:128-130`、`flusher.cpp:570-574`）：恢复与追加决策仅从 `AppendTargetCache` 取 `date` 与 `split_no` 组装 `hint` 路径，随后一律调用 `scan_one` 从头扫描文件重算真实偏移；`atc.last_block_offset` 与 `end_offset` 本质上为纯诊断字段，无任何业务逻辑消费，即使截断也不破坏恢复正确性；
  2. 默认 10MB 切分下，单文件累积到 4GiB 在现实路径中完全不可达；若直接盲目将字段升为 `uint64_t` 会破坏 Spec §3.1 原生的 `13B (reserved[0..12])` 二进制契约，并与 S-02（13B packed）产生直接冲突。
- **修复措施（方案 D 落地）：**
  1. **契约锁定与静态校验**（`logx_staging.h`）：保持 13B 二进制布局与 `uint32_t` 类型不变，增补静态断言 `sizeof(atc.last_block_offset) == 4` 与详细契约注释；
  2. **单文件 4GiB 物理容量硬切分防线**（`logx_internal.h` / `flusher.cpp`）：引入 `kSingleFileMaxCapacity = 4GiB - 16MB` 与 `should_rotate_file_size` 纯函数，无论上层配置多大的 `max_file_size`，单文件达到 4GiB 均强制切分滚动，从源头物理杜绝单文件超 4GiB；
  3. **越界哨兵置 0 守卫**（`logx_internal.h` / `flusher.cpp` / `recovery.cpp`）：引入 `safe_assign_atc_offsets` 守卫，偏移 $\ge$ `UINT32_MAX` 时将 `atc` 偏移置 `0` 哨兵值并派发 `LOGX_EV_IO` 报警事件，彻底杜绝静默高位截断环绕；
  4. **单测闭环**：
     - `tests/unit/test_flusher.cpp`：新增 `FileSizeThreshold_ShouldRotateFileSize_Enforces4GiBCapacity`，覆盖正常阈值、100GB/UINT64_MAX 超大配置下 4GiB 强制切分拦截；
     - `tests/unit/test_recovery.cpp`：新增 `AppendTargetCache_ContractAndSafeOffsetAssignment`，覆盖静态断言、边界赋值与 $\ge$ `UINT32_MAX` 越界置 0 哨兵机制。
  5. **全矩阵编译与回归通过**：
     - 基础构建（OFF/OFF）：73/73 单测 + 4/4 压测 100% 通过；
     - 全开构建（ON/ON）：90/90 单测 + 4/4 压测 100% 通过；
     - Android arm64-v8a：交叉编译 0 警告 0 错误通过。

### 🟠 [F-07] 切分序号 `split_no` 为 `uint8_t`，达到 255 后静默停写 [✅ 已修复]
- **来源：** 报告一 WARN-4（无报警部分）＋ 报告三 BUG-X1（T4.0-2 未落地，列为 P1）
- **位置：** `core/include/logx_staging.h:59-95`、`core/include/logx_internal.h:112-120, 206`、`core/src/flusher.cpp:209-251, 440-444, 530-539, 574, 606-618, 668-705, 883`、`core/src/recovery.cpp:129, 178-185, 302-306`
- **状态：** ✅ **已修复并全矩阵闭环验证**（Plan B：ATC 物理对齐保持 + 历史恒零填充位扩宽 + 达上限明确报警与去重）
- **描述：** 单日高吞吐下切分超过 255 个分卷后 Flusher 停写并丢弃后续日志；spec §3.1 要求升级为 `uint16_t`，偏差未落地。报告一同时指出停写时未触发 `emit_event`（注：`drain_once` 失败分支会发 LOGX_EV_IO 事件，但文案未说明停写原因）。
- **修复方案与落地：**
  1. **ATC 物理脚印锁定与零成本向下兼容扩宽**（`logx_staging.h`）：放弃破坏性的前移字段 packed 方案，保留 16B 自然对齐不变（`[8..15]` 块起止偏移物理位置毫厘不差）；利用历史 Staging 全零初始化写入中 `reserved[5]` 填充位恒为 `0` 的天然事实，将 `split_no` 拆为 `split_no_lo`（`[4]`）与 `split_no_hi`（`[5]`），配合 `pad[2]`（`[6..7]`）自然对齐。旧版落盘的 Staging 读入时 `split_no_hi == 0`，数值完全保真，实现 100% 零成本向下兼容与无缝冷启动恢复。
  2. **强类型编译期守护与 Helper 接入**（`logx_staging.h` / `recovery.cpp` / `flusher.cpp`）：提供 `atc_split(c)` 与 `atc_set_split(c, split)` 内联辅助函数；原字段名作废使编译器成为最强重构安全网，强制全工程彻底消除所有裸成员直接读写，杜绝隐式截断。补充 `sizeof(AppendTargetCache) == 16` 与成员偏移 `offsetof == 8 / 12` 静态硬断言。
  3. **全链路类型升级与严格路径解析**（`logx_internal.h` / `flusher.cpp`）：`make_log_path`、`parse_log_filename`、`current_log_path`、`FlusherCtx.current_split` 全量升级为 `uint16_t`；`parse_log_filename` 上限调整至 65535 并增加 `name[13]` 数字合法性严检。
  4. **达上限明确报警与双事件去重**（`flusher.cpp`）：引入内部哨兵常量 `kAppendTargetSplitExhausted = -2`；达 65535 物理上限时主动发射明确事件 `emit_event(inst, LOGX_EV_IO, -1, "daily split limit (65535) exhausted; writes suspended until date rollover")` 并返回 `-2`；`drain_once` 捕获 `-2` 抑制重复冗余的 `"append target open failed"`，确保报警语义纯净精确。
  5. **单测全闭环**：
     - `test_flusher.cpp`：`ParseLogFilename_Comprehensive` 扩充 256/9999/65535 合法测试与 65536/99999 非法测试及往返一致性；
     - `test_flusher.cpp`：新增 `FileSizeThreshold_RotatesBeyond255_Succeeds`，验证分卷成功越过 255 并生成 `.log.256` 正常写入；
     - `test_flusher.cpp`：新增 `FileSizeThreshold_SplitLimit65535_EmitsEventAndSuspends`，验证达 65535 边界时停写并发出专有告警且抑制双事件；
     - `test_flusher.cpp`：新增 `AppendTargetCache_HistoricalStagingLayoutCompatibility`，手工构造旧布局 staging 双 Header（`reserved[4]=3, reserved[5]=0`），验证新版本无缝读取接管并追加至 `.log.3`；
     - `test_recovery.cpp`：更新 `AppendTargetCache_ContractAndSafeOffsetAssignment`，断言 16B 脚印、偏移不变性及 65535 存取独立性。
  6. **三构建矩阵全绿验证**：
     - 基础构建（OFF/OFF）：77/77 单测 + 4/4 压测 100% 通过；
     - 全开构建（ON/ON）：95/95 单测 + 4/4 压测 100% 通过；
     - Android arm64-v8a：交叉编译 0 警告 0 错误通过。

### 🟡 [F-08] `flush_blocking` 自旋等待无超时上界 [✅ 已修复]
- **来源：** 报告一 WARN-3；T4.0-1 关联（Spec §7 水位线 target_seqno 防饥饿未实现，列为 P0 级偏差）
- **位置：** `core/src/flusher.cpp:974-999`、`core/src/logx_c.cpp:380-401`、`core/include/logx_c.h:38`、`core/include/logx_internal.h:56-68, 221`、`core/src/staging.cpp:278-340`、`core/src/recovery.cpp:314-318`
- **状态：** ✅ **已修复并全矩阵闭环验证**（字节水位线 + 环回绕 PAD 跨度补偿 + 5000ms 超时熔断 + Flusher 存活检查 + `LOGX_ERR_TIMEOUT`）
- **核心决策与技术机制：**
  1. **规避 Spec 字面 `target_seqno` 缺陷，改用等价字节水位线**：Spec 字面的 Block `seqno` 水位线在块丢弃路径（目标文件打开失败、短写失败）存在严重语义冲突——Header 语义为“已落盘块数”，丢弃块依法不能递增 seqno，若按块预测 seqno 会导致任何一次丢弃使 committed 永远追不上 target 而死等 5 秒。本实现采用物理等价的单调字节水位线：
     - 进入时锁内记录目标水位 `target = staged_bytes_total`（uint64）；
     - `drain_once` 无论写入成功还是两条丢弃路径，只要推进 `cur_f` 均执行 `flushed_bytes_total.fetch_add(blk.raw_len)`；
     - 等待谓词收敛为 `flushed_bytes_total >= target`，单调 O(1) 判定，进入时的全部日志落盘或明确丢弃即刻返回，彻底根除高并发写入持续饥饿。
  2. **Staging 环形回绕 PAD 跨度补偿**：针对 Staging 环尾垫入 `LGXP PAD` 导致 Flusher 块 `raw_len` 包含 PAD 从而造成出入环计数累积漂移的问题，重构 `Staging::write_records` 输出真实占用的物理跨度（`pad + total`），`logx_write` 严格累加实际占用跨度，保证流入 Staging 与流出 Staging 字节数 100% 绝对 1:1 对齐。
  3. **双级熔断与存活检查（WARN-3）**：循环内检测 `!flusher_running`，若 Flusher 线程未运行则立即派发 `"flush skipped: flusher not running"` 并返回 false；超过 `kFlushBlockingTimeoutMs = 5000` 则派发 `LOGX_EV_IO` 报警 `"flush timeout: flusher stalled; data may be pending"` 并熔断退出，杜绝永久阻塞与 Android 主线程 ANR 隐患。
  4. **ABI 安全错误码**：在 `logx_status_e` 末尾追加 `LOGX_ERR_TIMEOUT = -10`，`logx_flush` 失败时明确返回超时错误码，`logx_close` 与 `export` 路径尽力而为。
  5. **崩溃恢复待刷对齐**：`run_recovery` 恢复出未落盘日志时，自动将 `staged_bytes_total` 初始化为 `pending` 字节数，保证恢复后立即 flush 不会空过。
  6. **单测闭环（`tests/unit/test_flusher.cpp` 新增 3 组针对性用例）**：
     - `Flush.WatermarkReturnsUnderSustainedWrites`：后台线程持续密集写，主线程调用 flush 在 <3000ms（实测 ~350ms）内准时返回 `LOGX_OK`，抗饥饿验证通过；
     - `Flush.TimeoutWhenFlusherStalled_EmitsEventAndReturns`：PAL 装饰器注入 `file_write` 挂起，验证 `logx_flush` 在 ~5000ms（实测 5121ms）精准超时返回 `LOGX_ERR_TIMEOUT`，解除挂起后日志完整导出无污染；
     - `Flush.DiscardPathDoesNotStall`：构造 65535 分卷上限丢弃环境，验证 `logx_flush` 快速返回 `LOGX_OK`（实测 30ms），不误触发超时，直接实证字节水位线优于 seqno 方案。
  7. **三构建矩阵全绿验证**：
     - 基础构建（OFF/OFF）：80/80 单测 + 4/4 压测 + 3/3 强杀恢复 100% 通过；
     - 全开构建（ON/ON）：98/98 单测 + 4/4 压测 + 3/3 强杀恢复 100% 通过；
     - Android arm64-v8a：交叉编译 0 警告 0 错误通过。

### 🟡 [F-09] `random_bytes` 退化路径熵极低 [✅ 已修复]
- **来源：** 报告一 WARN-5
- **位置：** `core/src/pal_posix.cpp`、`core/src/pal_win32.cpp`、`core/include/logx_pal.h`、`core/src/flusher.cpp`
- **描述：** 原实现当 `/dev/urandom` 或 `rand_s` 失败时，退化路径按时间+地址周期性填充伪随机数，熵极低；若在生产路径触发将导致日志分卷文件盐值低熵，进而引发 AES-GCM / Poly1305 Nonce 空间碰撞与复用风险。
- **闭环方案：**
  1. **PAL 契约升级**：`Pal::random_bytes` 返回类型升级为 `bool`（失败明确返回 false，严禁自行伪装退化）；
  2. **POSIX 加固**：新增免文件描述符开销的 `getrandom()` 系统调用优先路径（免疫 `EMFILE`/`ENFILE` 耗尽），回退 `/dev/urandom`，两路均失败清零并返回 false；
  3. **Windows 适配**：保留 CRT `rand_s`，失败清零并返回 false；
  4. **生产端拒绝服务**：`flusher.cpp:open_append_target` 与 `append_block` 检查返回值，获取安全盐失败发射 `LOGX_EV_IO`（"secure random unavailable..."）明确报警，关闭文件并拒绝创建低熵密文文件；
  5. **单测覆盖**：在 `test_robustness.cpp` 新增 `RandomBytesFailure_RefusesNewFileCreation`（验证 CSPRNG 失败拒绝建文件、发射报警且不卡死）与 `RandomBytesSuccess_ProducesNonTrivialSalt`（验证 8B 盐非全零非全同）。

### 🟠 [F-10] 缺少 50~100ms Debounce 聚集刷盘机制（T4.0-4 偏差）[✅ 已修复]
- **来源：** 报告三 §四 T4.0-4（Spec 偏差）
- **位置：** `core/include/logx_internal.h`、`core/src/flusher.cpp:774-778`
- **描述：** spec §4.1 规定 Flusher 具备 50~100ms Debounce 聚集刷盘机制，以避免低频或零散写入触发过度频繁写盘；原 `flusher_loop` 在非降级模式下等待超时为 30000ms（仅为 30s 持久化检查点服务），导致零星低频写入在 staging 滞留长达 30 秒才落盘。
- **闭环方案：**
  1. **超时上界与窗口中值定型**：定义 `kFlushDebounceMs = 75`（50~100ms 黄金中值）；
  2. **消费端常态化防抖轮询**：`flusher_loop` 等待超时直接对接 `kFlushDebounceMs`（堆降级维持 5000ms），彻底消除冷启动或刷空后的长等待滞留死角；
  3. **生产端正交保持**：`logx_write` 保持 `pending >= flush_threshold`（8KB）阈值唤醒不变，小写入不发 signal 避免 sticky 信号破坏防抖，让零散记录在 75ms 内自然聚集落盘；
  4. **单测覆盖**：在 `test_flusher.cpp` 新增 `Debounce_LowFrequencyWritesFlushedWithinWindow`（验证小写入 ≤ 500ms 内必落盘且数据无损）与 `Debounce_BurstAggregatesIntoFewBlocks`（验证 10 条零散写入被聚集进 ≤ 2 个块，避免碎片化小块）。

---

## 三、Staging 环形缓冲

### 🔴 [S-01] `init_crc32_table()` 全局状态无锁初始化（多线程数据竞争）[✅ 已修复]
- **来源：** 报告一 BUG-3 ＝ 报告二 R2-高#4
- **位置：** `core/src/staging.cpp:24-36`
- **描述：** `crc32_table_init` 原为普通 `bool`（非 atomic），多个业务写线程（`logx_write`）或 Flusher 线程并发首次触发 CRC 时产生数据竞争，属 C++ 未定义行为（UB），TSan 会报竞争；且可能导致半初始化脏读。
- **闭环方案：** 基于 C++17 `constexpr` 将 CRC32 表在编译期直接计算完成并生成进只读数据段（`constexpr Crc32Table kCrc32Table{}`），彻底消除运行时初始化，物理实现 Data Race-Free；同时热路径免去初始化检查与分支；配以 `static_assert` 编译期校验特征项及 `test_staging.cpp` 标准测试向量单测。

### 🟠 [S-02] `AppendTargetCache` 缺少 packed 属性，实际大小 16B 而非 13B [✅ 已解决]
- **来源：** 报告一 BUG-1（经交叉复核校准为 P1）＝ 报告二 R2-中#15
- **位置：** `core/include/logx_staging.h:59-86`
- **描述：** 早期结构体缺 packed（实测 `sizeof=16`），`split_no` 后插入 3B 填充。当前单一 C++ 运行时内写入与读取均按 `sizeof(AppendTargetCache)` 做 `memcpy`，闭环内不发生错位；但与早期注释声称的 13 字节布局存在歧义。
- **闭环方案（已随 F-07/F-06 协同落地）：** 放弃破坏性的强制 packed 方案（避免导致历史 staging 文件物理 offset 偏移错位 3B）；转而正式将 `AppendTargetCache` 物理对齐大小锁定为 16B（`static_assert(sizeof == 16)` 且硬断言 `last_block_offset` 在 offset 8），将 `split_no` 扩宽至历史恒为 0 的填充位 `reserved[5]`，有效载荷升至 14B，`reserved[6..7]` 显式填充，实现 100% 物理与逻辑二进制自洽兼容；已通过 `Flush.AppendTargetCache_HistoricalStagingLayoutCompatibility` 与 `RecoveryTest.AppendTargetCache_ContractAndSafeOffsetAssignment` 双重验证。

### 🟡 [S-03] `kBlockHeaderSize` 注释与实际求值不符（32B vs 34B）
- **来源：** 报告一 BUG-12（经交叉复核校准为 P2）
- **位置：** `core/include/logx_staging.h:194`
- **描述：** 常量实际求值 `4+1+1+4+4+4+4+12 = 34`，末尾注释误写为 `= 32B`。nonce 布局本身（`hdr[22..29]` salt + `hdr[30..33]` seqno 大端）实现正确。
- **修复建议：** 修正注释为 34B。

### 🟡 [S-04] `max_log_length` 注释与默认值不一致
- **来源：** 报告一 BUG-15（经交叉复核校准）
- **位置：** `core/include/logx_internal.h:33`
- **描述：** `max_log_length = 0` 时 `logx_open`（`logx_c.cpp:244-246`）会主动拦截返回 `LOGX_ERR_INVALID_ARG`，正常使用下不会发生静默丢弃；核心问题是配置结构体默认值未与注释"64KB 默认"对齐。
- **修复建议：** 在配置默认值中显式赋 64KB。

### 🟡 [S-05] `logx_open` 失败路径 staging 已 delete 但指针未置 nullptr
- **来源：** 报告一 WARN-10
- **位置：** `core/src/logx_c.cpp:297-302`
- **描述：** 防御性编程缺失，若未来析构函数引用 `staging` 指针会引发 double-free。
- **修复建议：** delete 后置 nullptr。

### 🟡 [S-06] 双 Header 提交内存序轻微偏离 Spec（T4.0-3 偏差）
- **来源：** 报告三 §四 T4.0-3（Spec 偏差）
- **位置：** `core/src/staging.cpp:180-205`
- **描述：** spec §4.2/§10 规定双 Header 写入应为“写 Header A → seq_cst 内存屏障 → 写 Header B”；当前实现为 A(中间态 CRC) → B → release fence，在极端掉电窗口下内存序存在轻微偏离。
- **修复建议：** 严格按照 Spec 要求在 Header A 与 B 的提交之间插入完整顺序一致性屏障（或显式同步语义）。

---

## 四、格式化模块

### 🟠 [FM-01] `tag[tag_len]` 在 64 字节非 NUL 结尾 buffer 时越界读（UB）
- **来源：** 报告一 BUG-7 ＝ 报告二 R2-中#11
- **位置：** `core/src/format.cpp:92`
- **描述：** `tag` 恰为 64 字节无 NUL 终止的 buffer 时，循环停止于 `tag_len == 64`，随后访问 `tag[64]` 超出边界，属未定义行为。
- **修复建议：** 改用 `strnlen(tag, 65)` + 范围检查；明确 C API 的 tag 终止符契约。

### 🟠 [FM-02] `parse_records_stream` 重同步后未清除 `carry_slice_buf` 残片
- **来源：** 报告一 BUG-16
- **位置：** `core/src/format.cpp:233-245`
- **描述：** 检测到无效 magic 并重同步后，已积累的跨片数据未被清除，后续数据会与前一条记录的残片错误拼接，产生错乱输出。
- **修复建议：** 重同步成功后立即清空 `current_slice_buf`。

### 🟡 [FM-03] 栈上 64KB scratch 数组存在线程栈溢出风险
- **来源：** 报告一 WARN-1
- **位置：** `core/src/format.cpp:101`
- **描述：** `format_record` 为热路径，每次调用在栈上分配 64KB；非主线程（Flusher、JNI 调用链）栈通常只有 512KB～1MB。
- **修复建议：** 改为堆分配（`std::vector<uint8_t>`）或 `static thread_local`。

### 🟡 [FM-04] cksum 失败时 `pos += 1` 逐字节重扫，O(n²) 复杂度
- **来源：** 报告一 WARN-9
- **位置：** `core/src/format.cpp:259-262`
- **描述：** 校验失败后逐字节滑动重同步，最坏情况复杂度过高。
- **修复建议：** 至少跳过 `kHeaderLen` 或整条记录长度。

---

## 五、恢复模块

### 🟠 [R-01] hint 文件不可读时 `was_truncated` 状态被错误传播给 best 文件
- **来源：** 报告一 BUG-10（报告三 §1.2 将其列为"确认准确"）
- **位置：** `core/src/recovery.cpp:139-160`
- **描述：** 若 hint 文件有截断且不可读（`ok=false`），`was_truncated=true` 会被误传给重新扫描的 best 文件，导致 truncated 状态误判。

### 🟠 [R-02] `S_hdr > S_file && truncated` 时丢块统计少计一次
- **来源：** 报告一 BUG-11
- **位置：** `core/src/recovery.cpp:245-254`
- **描述：** 文件中有被截断的撕裂块且 `S_hdr > S_file` 同时成立时，被截断的那个块本身也是丢失块，统计少计 1，应为 `S_hdr - seqno_file + 1`。
- **修复建议：** 修正计数逻辑。

### 🟠 [R-03] 日期解析不验证真实日历日期，多套解析逻辑不一致
- **来源：** 报告二 R2-中#14 ＋ 报告一 WARN-7
- **位置：** `core/src/logx_c.cpp:33`（`parse_yyyymmdd`）、`core/src/recovery.cpp`、`core/src/flusher.cpp`（`strtoul` 直接解析）
- **描述：** `parse_yyyymmdd()` 只检查 8 位数字和数值范围，会接受 `20261399`、`20260231` 等非法日期；`recovery.cpp` 文件名日期解析未复用 `parse_yyyymmdd` 做范围校验，安全级别不一致。
- **修复建议：** 统一日期解析与校验入口。

---

## 六、C ABI 与导出

###  [E-01] 错误回调注册无并发保护（`err_fn`/`err_user` 数据竞争）
- **来源：** 报告一 BUG-4 ＝ 报告二 R2-高#5（均定为 P0/高优先级）
- **位置：** `core/src/logx_c.cpp:379-385`；并发读方 `logx_internal.h` 的 `emit_event`
- **描述：** `logx_set_error_handler` 无锁写 `err_fn`/`err_user`，后台 Flusher 在 `emit_event` 中并发读取，构成数据竞争；头文件注释声明"可被任意线程调用"，风险更高。
- **修复建议：** 读写均加 `inst->mu` 保护，或改用 `std::atomic<logx_error_fn>` + `std::atomic<void*>`。

### 🟠 [E-02] `logx_write` 中 `build_records` 失败路径 `drop_too_large` 统计与错误分类不准确
- **来源：** 报告一 BUG-9 ＋ 报告二 R2-中#16（合并）
- **位置：** `core/src/logx_c.cpp:328-360`、`core/src/flusher.cpp:651-669`
- **描述：** 报告一：`build_records` 失败或超限时返回 `LOGX_ERR_TOO_LARGE`，`drop_too_large` 计数没有递增（注：超限分支在 `build_records` 内部有 `fetch_add`，格式化失败分支没有）。报告二补充：`format_record` 因非法 level、空 tag 等失败时，`logx_write` 仍报告 "log too large"，错误输入与超长输入被混淆，错误回调和统计信息均不准确。
- **修复建议：** 区分格式化失败与超长两种错误码；统一 `drop_too_large` 递增点。

### 🟠 [E-03] 导出错误被吞掉
- **来源：** 报告二 R2-中#13
- **位置：** `core/src/logx_c.cpp:224, 230`
- **描述：** 导出过程忽略临时文件写入错误（`write_all_lines` 中 `(void)n`），也忽略回调返回值，最后仍可能返回 `LOGX_OK`。上层误以为导出成功，但文件可能没写完整，或业务回调已报告失败。
- **修复建议：** 传播临时文件写入错误与回调返回值。

### 🟠 [E-04] `logx_export` 临时文件在同一天多分卷场景下固定命名覆盖冲突
- **来源：** 报告一 WARN-8 ＝ 报告三 BUG-E1
- **位置：** `core/src/logx_c.cpp:198-220`（`make_tmp_path`）
- **描述：** 临时明文文件按日期命名（`.tmp_export_%u`）。同一天多个分卷文件或多次调用 export 共用同一路径；前次回调处理缓慢或并发导出时，临时文件会被后一次处理强行覆盖或截断。
- **修复建议：** 临时文件名引入进程 PID + 随机数或分卷序号。

### 🟠 [E-05] Windows 路径分隔符仅查找 `'/'`，不兼容 `'\\'`
- **来源：** 报告一 BUG-17
- **位置：** `core/src/logx_c.cpp:280`
- **描述：** `logx_open` 中 staging 目录解析只 `find_last_of('/')`，Windows 下 `\` 路径找不到分隔符，导致 `dir = "."`，staging 目录创建失败。
- **修复建议：** 改为 `find_last_of("/\\")`。

### 🟠 [E-06] `flush_threshold == 0` 语义模糊
- **来源：** 报告三 BUG-A1
- **位置：** `core/include/logx_c.h:62`、`core/src/logx_c.cpp:260`
- **描述：** 配置传入 0 被静默替换为默认 8KB。若业务意图是"彻底关闭自动刷盘、完全由手动 flush 控制"，该意图被吞。
- **修复建议：** 定义显式常量（如 `UINT32_MAX`）表示禁用。

### 🟠 [E-07] `logx_stats_t` 结构体按值返回缺乏 ABI 向前兼容能力
- **来源：** 报告三 BUG-A2
- **位置：** `core/include/logx_c.h:130-136`
- **描述：** 结构体定义暴露在 C 头中按值拷贝，未来新增统计指标会破坏既有二进制符号兼容性。
- **修复建议：** 预留保留字段或改为出参指针 + 版本长度。

---

## 七、PAL 跨平台抽象层

### 🔴 [P-01] Windows `HANDLE` → `int` 转换在 64 位 Windows 上截断高 32 位 [✅ 已修复]
- **来源：** 报告一 BUG-5（列 P0）＝ 报告二 R2-中#10（综合定级 P0）
- **位置：** `core/src/pal_win32.cpp`（原 `handle_to_int`/`int_to_handle`）、`PalMmap.out_handle`、`thread_join`（报告一"风险-5"同根）
- **描述：** 原实现把 64 位 `HANDLE` 强转为 32 位 `int`（`handle_to_int`），存入 `PalMmap.out_handle`；且在 `thread_join` 中对传入的 64 位线程句柄同样执行了强转 `int` 窄化。64 位 Windows 进程中 HANDLE 为 8 字节指针宽度，高 32 位非零是常态，还原出错误句柄会导致 `WaitForSingleObject` 等错对象、`CloseHandle` 句柄泄漏或越权误关他人句柄。原代码注释声称“HANDLE 实际只用低 32 位”系错误断言。
- **修复方案：** 
  1. 将 `core/include/logx_pal.h` 中 `PalMmap.out_handle` 由 `int` 改为 `intptr_t`（POSIX 存 `fd` 无损提升，Win32 存 64 位 `HANDLE` 无损）；
  2. 彻底删除 `pal_win32.cpp` 中 `handle_to_int` 与 `int_to_handle` helper 函数及错误注释，在 `pal_win32.cpp` 顶部增加 `static_assert(sizeof(void*) == sizeof(intptr_t))` 编译期不变量检查；
  3. `mmap_open` 与 `mmap_close` 直接使用 `reinterpret_cast` 往返，并在 `mmap_close` 中增加 `(map != nullptr && map != INVALID_HANDLE_VALUE)` 严格合法性判断；
  4. `thread_join` 增加判空前置条件 `if (!handle || handle == INVALID_HANDLE_VALUE) return -1;` 并直接 `reinterpret_cast<HANDLE>(handle)`；
  5. 在 `tests/unit/test_robustness.cpp` 中增加高 32 位非零虚拟句柄数学往返验证、32 轮并发线程 Spawn & Join 压力循环、及 Staging 多轮生命周期验证。

### 🟠 [P-02] Windows `file_size_path` 频繁打开关闭句柄开销过高
- **来源：** 报告三 BUG-P2
- **位置：** `core/src/pal_win32.cpp:154-163`
- **描述：** 每次查询文件大小均执行 `CreateFileA + CloseHandle`，扫盘调用频繁时 IO 开销偏大。
- **修复建议：** 改用 `GetFileAttributesExA`。

### 🟠 [P-03] `file_rmdir` 无明确错误转译
- **来源：** 报告三 BUG-P3
- **位置：** `core/src/pal_posix.cpp:162`、`core/src/pal_win32.cpp:182`
- **描述：** `rmdir` 失败直接返回 -1，缺乏对 EEXIST/ENOTEMPTY 等具体错误码的区分与透传。
- **修复建议：** 增加错误码转译。

### 🟡 [P-04] `monotonic_ns` 每次调用都查询性能计数器频率
- **来源：** 报告一 WARN-2
- **位置：** `core/src/pal_win32.cpp:317-323`
- **描述：** `QueryPerformanceFrequency` 结果在系统运行期间固定不变，应初始化时缓存。
- **修复建议：** 缓存频率值。

### 🟡 [P-05] `file_open` 中 `SetFilePointer` 设置末尾是冗余的
- **来源：** 报告一 WARN-6
- **位置：** `core/src/pal_win32.cpp:82`
- **描述：** `O_APPEND` 已通过 `_open_osfhandle` 保证原子 append，open 时的指针移动冗余无害，易误导维护者。
- **修复建议：** 删除冗余调用。

### 🟡 [P-06] Windows `atomic_add` 缺少 64 位对齐前置检查
- **来源：** 报告三 BUG-P4
- **位置：** `core/src/pal_win32.cpp:307-310`
- **描述：** `InterlockedExchangeAdd64` 要求 64 位对齐，建议 Debug 模式下对输入指针施加对齐断言。
- **修复建议：** 增加 `assert((uintptr_t(p) & 7) == 0)`。

---

## 八、JNI 层

### 🔴 [J-01] `nativeClose` 提前释放 KeyProvider 上下文，与 Flusher 线程竞态导致潜在 UAF [✅ 已修复]
- **来源：** 报告二 R2-高#6 ＝ 报告三 BUG-J5（报告二列高优先级，报告三列 P1）
- **位置：** `android/cpp/logx_jni.cpp:213-238`
- **描述：** 原实现 `nativeClose` 先 `delete kp_ctx` 再调 `logx_close(inst)`；而 `logx_close` 首先执行 `flush_blocking()`，期间后台 Flusher 仍在运行，可能继续调用 `jni_key_provider_trampoline` 访问已释放的 JNI 上下文（UAF 崩溃）。此外若 `key_user` 被置空或释放，Flusher 取 key 失败将静默把尾块降级为明文，且提前清空 `g_err_ctx` 会导致关闭阶段产生的错误事件丢失。
- **修复方案：** 调整 `nativeClose` 析构顺序：先调用 `logx_close(inst)` 确保 `flush_blocking` 完成（密钥全程可用）并 `thread_join` Flusher 退出；随后再安全释放 `kp_ctx` 全局引用与对象，最后清理 `g_err_ctx`。不给 `inst->key_user` 赋值避免对已释放 `inst` 的写 UAF。核心契约已在 `tests/unit/test_flusher.cpp:KeyProvider_DrainsEncryptedTailAndNotCalledAfterClose` 严密断言。

### 🟠 [J-02] `g_err_ctx` 全局静态变量多实例无保护
- **来源：** 报告三 BUG-J4
- **位置：** `android/cpp/logx_jni.cpp:40`
- **描述：** 多实例环境共享同一 `g_err_ctx`，一个实例关闭时清空全局结构，导致其余实例的错误回调静默失效。
- **修复建议：** 将错误回调上下文绑定到 `inst->err_user`，由每个实例独立管理生命周期。

### 🟡 [J-03] `NewStringUTF(msg)` 在含非 ASCII 时的 Modified UTF-8 格式风险
- **来源：** 报告三 BUG-J6
- **位置：** `android/cpp/logx_jni.cpp:64`
- **描述：** C++ 端错误消息若透传非 ASCII 或特殊字节字符串，个别 JVM 实现上可能触发编码异常。
- **修复建议：** 统一使用 UTF-16 构造。

---

## 九、Kotlin 封装层

### 🟠 [K-01] 协程 `export` 丢弃 `nativeExport` 返回值，底层失败静默返回成功
- **来源：** 报告三 BUG-K5
- **位置：** `android/kotlin/LogX.kt:177-198`
- **描述：** `nativeExport` 返回 Int 状态码，但协程函数未接收该返回值，恒返回初始化的 `var result = 0`。底层彻底失败且从未触发回调时，业务侧仍收到 0（成功），导致导出状态误判。
- **修复建议：** 接收 `nativeExport` 返回值，出错时优先返回底层错误码。

### 🟠 [K-02] `export` 协程中嵌套 `runBlocking` 阻塞 IO 调度器
- **来源：** 报告三 BUG-K3
- **位置：** `android/kotlin/LogX.kt:184-186`
- **描述：** 在 `withContext(Dispatchers.IO)` 内再启动 `runBlocking`，违背协程规范，外部协程上下文与结构化并发特征丢失。
- **修复建议：** 采用 `CountDownLatch` 或异步续体桥接。

### 🟡 [K-03] `init()` 失败时 `errorHandler` 静态引用未清理导致 GC 延迟
- **来源：** 报告三 BUG-K1
- **位置：** `android/kotlin/LogX.kt:106-110`
- **描述：** `nativeInit` 返回 0 失败时，抛异常前未将 `errorHandler` 重置为 null，单例持有的引用导致外部回调对象无法及时释放。
- **修复建议：** 失败路径重置 `errorHandler`。

### 🟡 [K-04] `setErrorHandler` 缺乏并发与时序校验
- **来源：** 报告三 BUG-K4
- **位置：** `android/kotlin/LogX.kt:128`
- **描述：** 允许在任意时机修改错误处理器，缺乏状态检查与并发保护。
- **修复建议：** 增加并发保护或限定在初始化配置时传入。

---

## 十、Python 离线解码器

### 🟠 [D-01] `parse_lgxe` cksum 失败直接 `break`，块内后续记录丢失
- **来源：** 报告三 BUG-D2；报告二 R2-测#7 关联
- **位置：** `decoder/logx_decode.py:158-163`
- **描述：** C++ 端 `parse_records_stream` 校验失败时滑动 1 字节重同步；Python 端直接 `break`，导致块内后续所有健康日志丢失。
- **修复建议：** 改为 `pos += 1; continue`。

### 🟠 [D-02] `decode_file_records` 块解码失败直接 `break`，整文件后半部分截断
- **来源：** 报告三 BUG-D3；报告二 R2-测#7 关联
- **位置：** `decoder/logx_decode.py:259-261`
- **描述：** C++ 端遇坏块时记录标记并 `continue` 到下一块；Python 端 `break` 退出整文件扫描，单块损坏导致整文件后半部分丢失。
- **修复建议：** 改为 `off += block_size; continue`。

### 🟠 [D-03] `decode_block_raw` 的 GCM 校验失败处理策略与 C++ 端不一致
- **来源：** 报告三 BUG-D4
- **位置：** `decoder/logx_decode.py:198-205`
- **描述：** C++ 导出路径 GCM 校验失败时静默丢块并触发 Event；Python 端输出到 stderr，策略差异未在文档中明确。
- **修复建议：** 在解码器文档中明确错误策略差异（或对齐）。

### 🟡 [D-04] 死代码 `reassemble()` 残留
- **来源：** 报告三 BUG-D1
- **位置：** `decoder/logx_decode.py:134-140`
- **描述：** 旧版切片重组函数已无任何调用者，容易误导维护者。
- **修复建议：** 彻底清理。

### 🟡 [D-05] `decompress()` 内存输出上限 64MB 过宽
- **来源：** 报告三 BUG-D5
- **位置：** `decoder/logx_decode.py:73`
- **描述：** C++ 内部块最大约 20KB，Python 解码器允许 64MB 输出，遭遇恶意畸形帧时存在内存过度分配风险。
- **修复建议：** 上限收紧为约 `20 * 1024 + 128`。

---

## 十一、测试用例问题

### 🔴 [T-01] "强杀恢复"测试没有真正模拟强杀 [✅ 已修复]
- **来源：** 报告二 R2-测#1
- **位置：** `tests/stress/test_crash_recovery.cpp:57`，新建 `tests/stress/test_crash_fork.cpp`
- **描述：** 测试设置 `flusher_exit = true` 后调用 `thread_join()`。但 Flusher 退出循环后还会执行最终 `drain_once()`（`core/src/flusher.cpp:613`），staging 数据已被正常刷盘，**没有验证"未刷盘 staging 在进程突然死亡后的恢复"**。
- **修复方案：** 
  1. 新增独立可执行文件 `logx_crash_fork_tests`（`tests/stress/test_crash_fork.cpp`），利用独立子进程执行写入，并在指定窗口调用系统底层立即终止接口（Windows `ExitProcess` / POSIX `::_exit`）彻底绕过 C++ 析构函数、atexit 钩子与 CRT 缓冲区，真实模拟硬崩溃。父进程重启实例验证数据 100% 零丢失复原与单调块序号衔接，包含 50 轮混合硬杀压力循环。
  2. 原 `test_crash_recovery.cpp` 语义重命名为 `GracefulTeardown_*`，准确反映优雅拆毁下的数据恢复与坏块自愈，并消除 `rand()` 路径竞争（T-10）。

### 🟠 [T-02] 测试名称 `ExportWithMultipleRecordsAndMissingKey_HandledGracefully` 与实现不符
- **来源：** 报告一 TEST-BUG-1 ＝ 报告二 R2-测#2
- **位置：** `tests/unit/test_flusher.cpp:514`
- **描述：** 测试名暗示"缺失密钥优雅降级"，实际配置 `encrypt_enable = 0`（明文模式），根本没有涉及缺失 key 场景，误导维护人员。
- **修复建议：** 更正测试名或补真实缺失密钥场景。

###  [T-03] 加密块测试绕过生产文件头逻辑且未验证加密生效
- **来源：** 报告二 R2-测#3 ＋ 报告一 TEST-BUG-8
- **位置：** `tests/unit/test_flusher.cpp:192`
- **描述：** 测试手工写入带加密标志的文件头，因此无法发现生产 `write_file_header()` 固定写入明文标志的问题（见 F-01）。报告一另在 TEST-BUG-8 中指出 `EncryptedBlockFileWriteAndDecode_Roundtrip` 只验证解密后大小正确，未验证"密文 ≠ 明文"（即加密确实发生）。
- **修复建议：** 补生产路径加密文件创建→导出端到端测试；增加密文/明文差异断言。

### 🟠 [T-04] PAL 注入时机晚于 Flusher 线程启动，产生数据竞争
- **来源：** 报告一 TEST-BUG-3 ＝ 报告二 R2-测#4
- **位置：** `tests/unit/test_robustness.cpp:181, 234`
- **描述：** `logx_open` 内部已启动 Flusher 线程，测试随后才将 `inst->pal` 改为 Mock PAL；Flusher 读 `inst->pal` 与测试写 `inst->pal` 并发，数据竞争，测试结果不可信。
- **修复建议：** 在创建实例前注入 PAL，或提供显式 PAL 注入入口。

### 🟠 [T-05] `FlushAndZeroMaintainsInvariant` 断言永远为真，无检测意义
- **来源：** 报告一 TEST-BUG-2 ＝ 报告二 R2-测#5
- **位置：** `tests/unit/test_staging.cpp:127`
- **描述：** 先清零 `[0, 256)` 再调用 `debug_check_invariant(0, 256)`，被检查区域已全为零，断言必然为 true，无法验证清零与坐标推进逻辑。
- **修复建议：** 清零前验证区间有数据，或用不同 `(f, w)` 参数验证推进状态。

### 🟠 [T-06] 压力测试没有断言 missing_count
- **来源：** 报告二 R2-测#6
- **位置：** `tests/stress/test_concurrent.cpp:246`
- **描述：** 测试计算了缺失日志数量但只输出结果，没有 `EXPECT_EQ(missing_count, 0)`，可能存在丢日志却仍通过测试的情况（注：测试末尾 `EXPECT_EQ(exported_lines, written)` 的总数对账可捕获净丢失，仅"等量丢失+等量重复"可逃逸）。
- **修复建议：** 增加显式断言。

### 🟠 [T-07] `ClockRollback` 测试未验证 seqno 单调性和文件切换正确性
- **来源：** 报告一 TEST-BUG-6
- **位置：** `tests/unit/test_robustness.cpp:216`
- **描述：** 时钟回拨场景两个核心保证缺少断言：回拨后写入日志的 seqno 是否大于未来日期文件的 seqno；Flusher 是否正确切换了日志文件。
- **修复建议：** 补充对应断言。

### 🟠 [T-08] 硬编码 magic 字节（0x4c, 0x47）
- **来源：** 报告一 TEST-BUG-4（`test_recovery.cpp` RingScanStopsOnTear）＋ TEST-BUG-5（`test_format.cpp` ParseSkipsPadAndReassembles）
- **位置：** `tests/unit/test_recovery.cpp:120`、`tests/unit/test_format.cpp:128-132`
- **描述：** 使用硬编码魔数而非常量，未来常量修改时测试容易失效且可读性差。（注：当前硬编码值与 `kMagicEntry = 0x474c` 小端字节序一致，属可维护性问题。）
- **修复建议：** 改用 `put_u16` 写入 `kMagicEntry`。

### 🟠 [T-09] GCM 测试 key 仅初始化前 3 字节，不具代表性
- **来源：** 报告一 TEST-BUG-7
- **位置：** `tests/unit/test_codec.cpp:165-166`
- **描述：** `uint8_t key1[32] = {1, 2, 3}` 剩余 29B 全为 0，建议使用差异更大的完整 32B 密钥，与标准测试向量（NIST CAVS）对齐。
- **修复建议：** 使用完整随机密钥。

### 🟠 [T-10] `rand()` 未播种导致测试目录重名风险
- **来源：** 报告三 BUG-T1
- **位置：** `tests/stress/test_concurrent.cpp:33-34`、`tests/stress/test_crash_recovery.cpp:33-34`
- **描述：** 未 `srand()` 产生确定伪随机序列，CI 重跑或上轮异常退出遗留目录时可能引发目录冲突。
- **修复建议：** 改用原子自增计数器。

### 🟠 [T-11] 竞争线程过早启动导致前 15ms 空跑
- **来源：** 报告三 BUG-T2
- **位置：** `tests/stress/test_concurrent.cpp:117-126`
- **描述：** 竞争线程发令枪响后立即每 15ms 调用 `logx_flush`，此时工作线程刚启动、staging 尚无数据，前 15ms 未构成实质竞争窗口。
- **修复建议：** 在 Worker 写入一定数据后再激活竞争。

---

## 十二、测试覆盖率缺口

### 报告一（§五）与报告二（§五）合并后的缺口清单

| 缺失测试 | 风险 | 来源 |
|------|------|------|
| `parse_records_stream` 跨块 carry（多块衔接场景） | 🔴 高 | 报告一 |
| `parse_records_stream` 重同步路径（坏 magic/cksum 后继续解析） | 🔴 高 | 报告一 |
| `encode_block` 压缩降级链（zstd 失败 → 明文降级 → 解码往返） | ✅ 已补齐 | 报告一/三（Commit `d0d157e` 闭环） |
| 生产路径加密文件创建和导出（端到端） | 🔴 高 | 报告二（掩盖 F-01） |
| `logx_c.cpp` export 路径（`export_range`、`decode_one_file`） | 🔴 高 | 报告一 |
| 真实进程级崩溃窗口（独立子进程强杀） | 🔴 高 | 报告二（掩盖 T-01） |
| PAL 短写注入（短写截断回滚与中途 ENOSPC 容灾） | ✅ 已补齐 | 报告二（F-03 闭环，Case A/B/C） |
| PAL fsync 失败和 mmap 同步失败注入 | ✅ 已补齐 | 报告二（F-04 闭环，MockFsyncPal 5 组用例） |
| 畸形 `raw_len`、非法版本和非法 flag | ✅ 已补齐 | 报告二（F-05 闭环，test_recovery 6 组用例） |
| 分卷恢复及 `.log.N` 目标选择（掩盖 F-02） | ✅ 已补齐 | 报告二/三（F-02 闭环，T1~T4 用例） |
| 双 Header 双坏（整区重建）路径 | 🟠 中 | 报告一 |
| 双 Header 容错（B 损坏，A 接管）反向场景 | 🟠 中 | 报告一 |
| `copy_pending` 跨回绕拷贝 | 🟠 中 | 报告一 |
| `zero_range_locked` 跨回绕清零 | 🟠 中 | 报告一 |
| `split_no` 达到 255 的停写行为 | 🟠 中 | 报告一 |
| `sum_raw_len_since` 正确性 | 🟠 中 | 报告一 |
| seqno 回绕（UINT32_MAX → 0） | 🟠 中 | 报告一/二 |
| `max_log_length == 0` 时的丢弃行为 | 🟠 中 | 报告一 |
| 并发 write + flush 竞争 | 🟠 中 | 报告一 |
| 多实例 JNI 错误回调隔离 | 🟠 中 | 报告二 |
| 导出回调失败和临时文件写入失败 | 🟠 中 | 报告二 |
| 加密块 nonce 不同 seqno 的唯一性（防重放） | 🟡 低 | 报告一 |
| `flush_blocking` 等待真实 Flusher 追上 | 🟡 低 | 报告一 |
| Python 解码器与 C++ 产物的端到端一致性 | 🟡 低 | 报告二/三 |

### 已充分覆盖（两份报告一致确认）

- 环形缓冲区空环/满环/回绕 + PAD 不变量（`test_staging.cpp`）
- 双 Header 部分损坏恢复（A 损坏 B 接管）（`test_staging.cpp` / `test_recovery.cpp`）
- 基本恢复对账、损坏块截断、ring_scan 撕裂/全零终止（`test_recovery.cpp`）
- 多条记录切块（16KB 阈值）、单条大记录独占块、文件切分、保留期清理（`test_flusher.cpp`）
- Zstd / AES-GCM 基础编解码、GCM 篡改拒绝（`test_codec.cpp`）
- 整条/多片格式化 + parse、cksum 失败拒绝、未对齐访问、时钟跳变/回拨（`test_format.cpp` / `test_robustness.cpp`）
- 基本并发写入（`test_concurrent.cpp`）

---

## 十三、Spec 偏差跟踪

### T4.0 偏差落地状态（12 项，来源：报告三 §四）

| T4.0 编号 | Spec 规定 | 当前代码状态 | 影响与风险 | 关联问题 |
|:---|:---|:---|:---|:---|
| **T4.0-1** | §7 `flush()` 水位线 `target_seqno` 防饥饿 | ✅ 已通过 F-08 闭环（累计字节水位线 + PAD 跨度补偿 + 5000ms 超时熔断 + LOGX_ERR_TIMEOUT） | — | F-08 |
| **T4.0-2** | §3.1/§4.4 切分序号 2B (uint16_t) | ✅ 已通过 F-07 闭环（16B 自然对齐 + 历史恒零扩宽至 uint16_t + 达上限明确告警与去重） | — | F-07 |
| **T4.0-3** | §4.2/§10 双 Header "写 A→seq_cst 屏障→写 B" | 实现为 A(中间态 CRC)→ B → release fence | 掉电极端窗口内存序轻微偏离 | S-06 |
| **T4.0-4** | §4.1 Flusher 50~100ms Debounce 聚集刷盘 | ✅ 已通过 F-10 闭环（定型 75ms 聚集窗口 + 常态化防抖轮询，零散小写入自然聚集落盘） | — | F-10 |
| **T4.0-5** | §10 ARMv8 Crypto Extensions 加速 | ✅ 已落地并闭环：`aesce.c` 移植完成，AESE/AESMC/EOR 标准指令直通；后续独立复审完成官方向量锚定（McGrew-Viega TC13/14/15 + 零密钥基线，OpenSSL 交叉验证 72/72）、热路径逐块探针消除与防竞态定型，x86 + 真机（骁龙865）双路径全量回归通过 | — | C-04 |
| **T4.0-6** | §11 运行期每 30s 定时 msync/fdatasync | ✅ 已通过 F-04 补齐：`flusher_loop` 接入 30s 超时唤醒与 `durability_sync`，落盘 + staging 强刷盘 | — | — |
| **T4.0-7** | §9 未对齐访问 / 23:59:59 时钟跳变用例 | ✅ 已通过 T4.5 落地 | — | — |
| **T4.0-8** | §2/§10 离线解码工具 `logx_decode.py` | 坏块 break、死代码残留、容错性待对齐 | 影响离线日志捞取成功率 | D-01~D-04 |
| **T4.0-9** | §7/§10 Kotlin Facade `LogX.kt` | export 丢弃 native 返回值；runBlocking 嵌套 | 导出状态误判与调度器线程阻塞 | K-01/K-02 |
| **T4.0-10** | §7/§10 JNI Config 传递与 ErrorHandler | Compress 映射语义混淆 [✅ C-02 部分已闭环]；全局错误上下文单例化 | 多实例接入与算法配置异味 | C-02/J-02 |
| **T4.0-11** | §4.3/§4.4 恢复后切分序号强制归 0 风险 | ✅ 已通过 F-02 落地（严格解析文件名 + 探测安全网） | — | — |
| **T4.0-12** | §10 新建文件父目录 fsync | ✅ 已通过 F-04 补齐：新建日志文件头落盘后执行文件与父目录 `file_dir_fsync` 双同步 | — | — |

> 注：T4.0-10 中"Compress 映射语义混淆"已随 C-02 闭环（删除歧义常量 + `make_flag()/flag_algo()` 等 helper 全栈命名拉齐，JNI/Kotlin/Python 侧均已显式关联 `kAlgo*` 编号）；该行其余偏差（J-02 全局错误上下文）仍待修复。

---

## 十四、已撤销与校准条目存档

### 14.1 报告一内部已撤销的条目（报告一/三交叉复核结论）

| 编号 | 原描述 | 核验结论 |
|:---|:---|:---|
| **BUG-6** | `debug_check_invariant` 非回绕场景漏检 `[0, f)` | **完全误报，已撤销**。`staging.cpp:354-375` 在 `w >= f` 时 `len2=0, off1=f`，第一循环正好检查 `[0, f)`，覆盖完整。 |
| **BUG-14** | `decode_one_file` CRC 校验对象（密文 vs 明文）需对齐 | **完全误报，已撤销**。写入端 `flusher.cpp:108` 与读取端 `logx_c.cpp:110` 均对不含 16B GCM tag 的密文 payload 计算 CRC，两侧完全对称。 |
| **BUG-1** | AppendTargetCache 数据错位静默损坏 | **校准为 P1**。单一运行时内不触发错位，但破坏跨语言二进制协议契约（见 S-02）。 |
| **BUG-12** | nonce 布局与实现不符 | **校准为 P2**。实为 `kBlockHeaderSize` 末尾注释 34B 误写 32B，实现正确（见 S-03）。 |
| **BUG-15** | `max_log_length == 0` 丢弃所有日志 | **校准**。`logx_open` 会主动拦截，正常运行不触发；核心是默认值与注释不一致（见 S-04）。 |

### 14.2 报告三自身复核撤销的 5 处历史误报

| 编号 | 原错误断言 | 复核事实 |
|:---|:---|:---|
| **BUG-J2** | Kotlin `external fun` 不支持 trailing lambda，测试从未编译 | **撤销**。`ExportCallback` 为 `fun interface`（SAM），trailing lambda 与 SAM 转换对 `external fun` 完全合法。 |
| **BUG-T4** | `test_concurrent.cpp` 对账逻辑受消息内 `\n` 干扰 | **撤销**。用户载荷转 hex ASCII 字符串，不含 0x0A；导出文件唯一 `\n` 就是记录行尾。 |
| **BUG-T3** | `test_crash_recovery.cpp` 模拟强杀存在 UAF | **撤销**。测试在 `delete staging` 前已显式 `thread_join`，Flusher 已彻底退出。（注：报告二后续指出该测试的真正问题是"没有真正模拟强杀"，见 T-01。） |
| **BUG-J3** | JNI `nativeWrite` 的 `tag_buf` 中文字符截断越界 | **纠偏**。`GetStringUTFRegion` + `utf8_len` 位置正确，仅边界降级逻辑存在（非字符中间截断）。 |
| **BUG-K2** | `Application.getProcessName()` 是非公开私有 API | **纠偏**。Android 9.0 (API 28) 官方公开标准 API，Google 推荐替代读取 `/proc/self/cmdline`。 |

---

## 十五、优先级总表

> 综合三份报告的定级。同一问题多报告定级不一致时，取较新/较严定级并在括号注明。

### 🔴 P0 —— 数据破坏 / 崩溃 / 未定义行为

| 编号 | 问题 | 来源 |
|:---|:---|:---|
| C-01 | 压缩降级未更新块头 flag（毒丸块，解码必失败）[✅ 已修复] | 报告二#2 / 报告三 BUG-C1 |
| F-01 | 生产路径加密文件头未记录加密标志（加密文件无法导出）[✅ 已修复] | 报告二#1 |
| F-02 | 恢复后 `current_split` 强制归零，覆写历史分卷 [✅ 已修复] | 报告二#9 / 报告三 BUG-X2（报告二列中等） |
| J-01 | `nativeClose` 提前释放 kp_ctx，Flusher UAF [✅ 已修复] | 报告二#6 / 报告三 BUG-J5（报告三列 P1） |
| S-01 | CRC32 表初始化多线程数据竞争（UB） [✅ 已修复] | 报告一 BUG-3 / 报告二#4 |
| E-01 | 错误回调注册数据竞争（UB） | 报告一 BUG-4 / 报告二#5 |
| P-01 | Win64 HANDLE → int 截断（报告一列 P0） [✅ 已修复] | 报告一 BUG-5 / 报告二#10（报告二列中等） |
| T-01 | "强杀恢复"测试未真正模拟强杀（恢复路径零覆盖） [✅ 已修复] | 报告二测#1 |

### 🟠 P1 —— 功能 / 并发 / 协议 / 容错缺陷

| 编号 | 问题 | 编号 | 问题 |
|:---|:---|:---|:---|
| F-03 | 短写未检测致日志丢失 [✅ 已修复] | F-04 | fsync/同步持久化零接入 [✅ 已修复] |
| F-05 | 恢复块头校验不足 [✅ 已修复] | F-06 | atc offset uint32 截断 [✅ 已修复] |
| F-07 | split_no uint8 / 255 停写（T4.0-2） [✅ 已修复] | C-02 | 压缩/加密 flag 命名歧义 [✅ 已修复] |
| C-03 | decode_block 忽略 raw/raw_len [✅ 已修复] | S-02 | AppendTargetCache 非 packed [✅ 已解决] |
| FM-01 | tag[64] 越界读 UB | FM-02 | 重同步未清 carry_slice_buf |
| R-01 | was_truncated 误传播 | R-02 | 丢块统计少计 1 |
| R-03 | 日期校验不完整 / 多套解析不一致 | E-02 | drop_too_large 统计与错误分类 |
| E-03 | 导出错误被吞 | E-04 | export 临时文件命名冲突 |
| E-05 | Windows 路径分隔符 | E-06 | flush_threshold==0 语义 |
| E-07 | logx_stats_t ABI 兼容 | P-02 | file_size_path 开销 |
| P-03 | file_rmdir 错误转译 | J-02 | g_err_ctx 多实例互踩 |
| K-01 | Kotlin export 丢返回值 | K-02 | runBlocking 嵌套 |
| D-01 | Python cksum break 丢记录 | D-02 | Python 坏块 break 丢文件 |
| D-03 | GCM 失败策略不一致 | F-10 | 缺少 50~100ms Debounce 聚集刷盘 [✅ 已修复] |
| T-02~T-11 | 测试用例缺陷（详见第十一节） | — | — |

### 🟡 P2 —— 质量 / 性能 / 可维护性

| 编号 | 问题 | 编号 | 问题 |
|:---|:---|:---|:---|
| S-03 | kBlockHeaderSize 注释 32B→34B | S-04 | max_log_length 默认值注释 |
| S-05 | staging 指针未置 null | FM-03 | 64KB 栈数组溢出风险 |
| FM-04 | cksum O(n²) 重扫 | F-08 | flush_blocking 无超时 [✅ 已修复] |
| F-09 | random_bytes 退化低熵 [✅ 已修复] | P-04 | QPF 重复查询 |
| P-05 | SetFilePointer 冗余 | P-06 | atomic_add 对齐断言 |
| J-03 | NewStringUTF 编码风险 | K-03 | errorHandler GC 延迟 |
| K-04 | setErrorHandler 并发校验 | D-04 | Python 死代码 |
| D-05 | Python 64MB 解压上限 | C-04 | 缺少 ARMv8 硬件加速（T4.0-5）[✅ 已修复] |
| S-06 | 双 Header 提交内存序偏离（T4.0-3） | — | — |

### 修复顺序建议（综合三份报告）

1. **P0 核心数据正确性与崩溃恢复**：[✅ 文件头加密标志（F-01）] → [✅ 压缩降级 flag（C-01）] → [✅ 分卷号恢复（F-02）] → [✅ 真实崩溃恢复测试（T-01）]
2. **P0 并发安全与内存生命周期**：[✅ CRC 表 constexpr 编译期无竞争（S-01）] → 错误回调保护（E-01）→ [✅ JNI 关闭顺序与 UAF 防护（J-01）] → [✅ Win64 句柄截断修复（P-01）]
3. **P1 协议健壮性与持久化**：[✅ 短写检测与撕裂回滚（F-03）] → [✅ 持久化同步接入（F-04）] → [✅ 恢复校验加强（F-05）] → [✅ 分卷序号 uint16_t 与上限告警（F-07）] → [✅ 50~100ms Debounce 聚集刷盘（F-10）]
4. **P2 API、测试质量与工具链**：tag 边界、日期校验、统计分类、导出错误传播、Python 解码器容错、测试命名与恒真断言清理、故障注入路径断言补全
5. **覆盖率补齐**：优先补"压缩降级往返"与"多分卷恢复后再轮转"两个负向用例（正是它们缺席掩盖了 C-01 与 F-02）
