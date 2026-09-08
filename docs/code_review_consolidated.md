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

### 🟡 [S-03] `kBlockHeaderSize` 注释与实际求值不符（32B vs 34B）[✅ 已修复]
- **来源：** 报告一 BUG-12（经交叉复核校准为 P2）
- **位置：** `core/include/logx_staging.h:226`
- **描述：** 常量实际求值 `4+1+1+4+4+4+4+12 = 34`，末尾注释误写为 `= 32B`。nonce 布局本身（`hdr[22..29]` salt + `hdr[30..33]` seqno 大端）实现正确。
- **修复建议：** 修正注释为 34B。
- **状态：** ✅ 已修复——注释更正为 `= 34B (S-03)`，与实际求值及读盘/测试代码中的 34B 块头处理一致。

### 🟡 [S-04] `max_log_length` 注释与默认值不一致 [✅ 已修复]
- **来源：** 报告一 BUG-15（经交叉复核校准）
- **位置：** `core/include/logx_internal.h:33`
- **描述：** `max_log_length = 0` 时 `logx_open`（`logx_c.cpp:244-246`）会主动拦截返回 `LOGX_ERR_INVALID_ARG`，正常使用下不会发生静默丢弃；核心问题是配置结构体默认值未与注释"64KB 默认"对齐。
- **修复建议：** 在配置默认值中显式赋 64KB。
- **状态：** ✅ 已修复——`Instance::max_log_length` 默认值改为 `64 * 1024`，与注释、Kotlin `LogX.Config.maxLogLength = 64*1024` 及 JNI 层兜底值三层对齐；C ABI 层显式传 0 的拦截逻辑保持不变（防御不受影响）。

### 🟡 [S-05] `logx_open` 失败路径 staging 已 delete 但指针未置 nullptr [✅ 已修复]
- **来源：** 报告一 WARN-10
- **位置：** `core/src/logx_c.cpp:331-337`
- **描述：** 防御性编程缺失，若未来析构函数引用 `staging` 指针会引发 double-free。
- **修复建议：** delete 后置 nullptr。
- **状态：** ✅ 已修复——`thread_start` 失败路径 `delete inst->staging` 后追加 `inst->staging = nullptr`，与 `logx_close` 成功路径（delete 后置空）纪律拉齐。

### 🟡 [S-06] 双 Header 提交内存序轻微偏离 Spec（T4.0-3 偏差）[✅ 已修复]
- **来源：** 报告三 §四 T4.0-3（Spec 偏差）
- **位置：** `core/src/staging.cpp:244-272`
- **描述：** spec §4.2/§10 规定双 Header 写入应为"写 Header A → seq_cst 内存屏障 → 写 Header B"；原实现为 A(中间态 CRC) → B → release fence，在极端掉电窗口下内存序存在轻微偏离。
- **修复建议：** 严格按照 Spec 要求在 Header A 与 B 的提交之间插入完整顺序一致性屏障（或显式同步语义）。
- **状态：** ✅ 已修复——`commit_header_locked` 改为"写 A 完整（含最终 CRC）→ `atomic_thread_fence(seq_cst)` → 写 B 完整 → release fence"纪律：seq_cst 屏障保证 A 的全部字节先于 B 的任意字节全局可见，读者只会观察到 (A新,B旧)/(A新,B新)/(A旧,B旧) 三种组合，永不出现 (A旧,B新) 逆序窗口；掉电场景下 pick_header 取 CRC 完整且 seqno 较新者，"半新半旧"窗口被压缩到单个 Header 内部（由 CRC 把关）。T4.0-3 规格偏差随之闭环。

---

## 四、格式化模块

### 🟠 [FM-01] `tag[tag_len]` 在 64 字节非 NUL 结尾 buffer 时越界读（UB）[✅ 已修复]
- **来源：** 报告一 BUG-7 ＝ 报告二 R2-中#11
- **位置：** `core/src/format.cpp:89-97`、`core/include/logx_c.h:102-107`
- **描述：** `tag` 恰为 64 字节无 NUL 终止的 buffer 时，循环停止于 `tag_len == 64`，随后访问 `tag[64]` 超出边界，属未定义行为。
- **修复建议：** 改用 `strnlen(tag, 65)` + 范围检查；明确 C API 的 tag 终止符契约。
- **状态：** ✅ 已修复——(1) `format_record` 改用 `strnlen(tag, 65)`：仅当 `tag[0..63]` 全非 NUL 时才检视 `tag[64]`（此时 NUL 契约保证其必在界内），`n == 65` 直接判非法，原实现在 64B 无 NUL 输入下无条件读 `tag[64]` 的越界路径消除；(2) 发现并修正了根因级契约矛盾——`logx_c.h` 原注释声称 "tag 不需要 NUL 结尾" 但接口无长度参数、实现必须扫描 NUL，两者自相矛盾；头文件契约更正为 "tag 必须是 NUL 终止的 C 字符串且有效长度 ≤ 64B"（与 JNI 侧 `GetStringUTFRegion` 后显式写 `'\0'` 的实际行为一致），payload 保持 "不需要 NUL 结尾、长度由 payload_len 显式给出" 不变。

### 🟠 [FM-02] `parse_records_stream` 重同步后未清除 `carry_slice_buf` 残片 [✅ 已修复]
- **来源：** 报告一 BUG-16
- **位置：** `core/src/format.cpp:241-268`
- **描述：** 检测到无效 magic 并重同步后，已积累的跨片数据未被清除，后续数据会与前一条记录的残片错误拼接，产生错乱输出。
- **修复建议：** 重同步成功后立即清空 `current_slice_buf`。
- **状态：** ✅ 已修复——三条错误路径（magic 无效重同步成功、重同步失败退出、长度越界、cksum 失败）均追加 `current_slice_buf.clear()`：重同步意味着字节流已断裂，此前积累的跨片残片属于被破坏的旧记录，必须丢弃；export 侧 `carry_slice_buf` 跨块生命周期不受影响（仅坏块路径清空，正常末片提交后本就清空）。

### 🟡 [FM-03] 栈上 64KB scratch 数组存在线程栈溢出风险 [✅ 已修复]
- **来源：** 报告一 WARN-1
- **位置：** `core/src/format.cpp:105-113`
- **描述：** `format_record` 为热路径，每次调用在栈上分配 64KB；非主线程（Flusher、JNI 调用链）栈通常只有 512KB～1MB。
- **修复建议：** 改为堆分配（`std::vector<uint8_t>`）或 `static thread_local`。
- **状态：** ✅ 已修复——采用修复建议第二方案 `static thread_local uint8_t scratch[64*1024]`：热路径零分配零初始化开销、每线程仅一份且跨调用复用、堆段存放不受线程栈深约束。中途曾尝试"直接在 out_buf 组装 + 头区集中回填"的零拷贝方案，但该布局改变了磁盘交错格式 `[h0][user0][h1][user1]...`（decoder/恢复按 `pos += 6+len` 顺序遍历的格式契约），被既有 `Format.LongRecord_Slices` 字节级断言当场拦截，随即回退为 thread_local 方案——既有格式契约优先。

### 🟡 [FM-04] cksum 失败时 `pos += 1` 逐字节重扫，O(n²) 复杂度 [✅ 已修复]
- **来源：** 报告一 WARN-9
- **位置：** `core/src/format.cpp:258-268`
- **描述：** 校验失败后逐字节滑动重同步，最坏情况复杂度过高。
- **修复建议：** 至少跳过 `kHeaderLen` 或整条记录长度。
- **状态：** ✅ 已修复——两条逐字节路径均改为跳过语义完整单元：cksum 失败跳过 `kHeaderLen + len`（原记录已损坏，其长度内不存在任何合法子记录——记录格式按头起始对齐，逐字节重扫既不可能找回数据又造成最坏 O(n²)）；长度越界（头声明长度不可信）跳过 `kHeaderLen`。最坏复杂度从 O(n²) 降为 O(n)。

---

## 五、恢复模块

### ✅ [R-01] hint 文件不可读时 `was_truncated` 状态被错误传播给 best 文件 [已修复并闭环]
- **来源：** 报告一 BUG-10（报告三 §1.2 将其列为"确认准确"）
- **位置：** `core/src/recovery.cpp`（`reconcile`）
- **状态：** ✅ **已修复并闭环**（truncated 严格跟随实际命中的扫描结果 + 回归测试锁定）
- **描述：** `reconcile` 中 `was_truncated` 从 hint 扫描取值后跨文件残留，回退扫描 best 文件后以 `sr.truncated || was_truncated` 合并，hint 文件的截断状态会被误传给另一个文件，导致续写 seqno 凭空跳 1 空隙。（修复前核实：属"休眠但脆弱"隐患——当前 `scan_one` 的 `ok=false` 路径恰好不置 `truncated`，报告的字面场景暂不可触发，但跨文件变量残留模式一旦 `scan_one` 失败路径语义微调即会真实触发。）
- **修复方案与落地：**
  1. 重构 `reconcile`：删除跨文件残留变量 `was_truncated`，`r.file_truncated` 只取最终命中那次扫描的 `sr.truncated`；hint 不可读回退 best 时，hint 的整个扫描结果（含 truncated）整体作废；
  2. 新增回归测试 `RecoveryTest.HintUnreadable_FallsBackToBest_TruncatedNotPropagated`：构造 hint（带撕裂尾块后删除，触发回退）与 best（干净完整文件），断言恢复后 seqno 接管为 best 的 S_file（不凭空跳空隙）且追加目标缓存回退指向 best；
  3. 双构建矩阵全绿通过。

### ✅ [R-02] `S_hdr > S_file && truncated` 时丢块统计少计一次 [已复核校准并闭环]
- **来源：** 报告一 BUG-11
- **位置：** `core/src/recovery.cpp`（丢块打点分支）
- **状态：** ✅ **已复核校准：报告断言不成立，现行实现正确；已通过语义注释 + 回归测试固化，防止按报告建议误改引入重复计数**
- **描述（原报告）：** 报告认为文件中被截断的撕裂块本身也是丢失块，统计少计 1，应为 `S_hdr - seqno_file + 1`。
- **复核结论（校准）：** 块提交纪律是"整块写入成功后才 `commit_header_locked`"。撕裂尾块（文件中第 S_file+1 号块）若其 Header 提交已持久化，则 S_hdr ≥ S_file + 1，该块天然落在差值区间 (S_file, S_hdr] 内，`lost = S_hdr - S_file` 已覆盖；若其提交未持久化，则 S_hdr == S_file，走独立 `else if (truncated)` 分支单独 +1。两种情况均恰好计数一次，报告建议的再 +1 反而会对撕裂块重复计数。
- **固化措施：**
  1. 在打点分支补齐语义注释，锁定"只记差值、不 +1"的完整推导依据；
  2. 新增回归测试 `RecoveryTest.TornTailWithHeaderAhead_LostCountEqualsSeqnoDiffExactly`：构造块 1..3 + 撕裂第 4 块 + Header 提交至 seqno=4（S_hdr=4 > S_file=3 且 truncated），断言 `truncated_blocks == 1`（恰好一次，若按报告建议 +1 则为 2）且续写 seqno 按 max+1 跳空隙、文件截断修复至块 3 末尾；
  3. 双构建矩阵全绿通过。

### ✅ [R-03] 日期解析不验证真实日历日期，多套解析逻辑不一致 [已修复并闭环]
- **来源：** 报告二 R2-中#14 ＋ 报告一 WARN-7
- **位置：** `core/src/logx_c.cpp`（`parse_yyyymmdd`）、`core/src/flusher.cpp`（`parse_log_filename` / `retention_cleanup_impl` / `scan_dir_for_target`）、`core/src/logx_c.cpp`（`take_snapshot` / export 临时文件名）、`core/include/logx_internal.h`
- **状态：** ✅ **已修复并闭环**（统一日历校验入口 `valid_yyyymmdd` + 5 处内联解析全部收敛）
- **描述：** `parse_yyyymmdd()` 只检查 8 位数字和数值范围，会接受 `20261399`、`20260231` 等数值合法但日历不存在的日期；除两套命名解析外，retention 清理、目录扫描、导出快照排序（含 `atoi` 解析分卷号）与导出临时文件命名共 5 处各自内联 `strtoul`/`atoi` 解析，安全级别不一致。
- **修复方案与落地：**
  1. `logx_internal.h` 新增 `inline bool valid_yyyymmdd(uint32_t)` 真实日历校验：年 ≥ 1、月 1..12、按月天数与闰年规则校验日（含 2000/1900 类世纪闰年边界），作为全库唯一日期合法性入口；
  2. `parse_log_filename`（文件名日期）与 `parse_yyyymmdd`（export 范围）均接入 `valid_yyyymmdd`，`20261399`/`20260231`/平年 `20260229` 等一律拒绝；
  3. 内联解析全量收敛：`retention_cleanup_impl`、`scan_dir_for_target`、`take_snapshot`（含排序 key）统一改走 `parse_log_filename`；export 临时文件名与回调日期字串改由已校验的日期统一格式化，消除最后一处裸 `strtoul`；recovery 侧经 F-02 修复后已复用 `parse_log_filename`，自此全库仅剩单一解析实现；
  4. 单测闭环：`ParseLogFilename_Comprehensive` 扩充非法日历日期拒绝断言（`20261399`/`20260231`/`20260229` 平年/0 月/0 日/0 年）与闰年边界放行（`20240229`/`20000229`，拒绝 `19000229`），并追加 `valid_yyyymmdd` 直接断言；
  5. 双构建矩阵全绿通过（全开 ON/ON: 105/105 单测 + 4/4 压测 + 3/3 强杀恢复；基础 OFF/OFF: 87/87 单测 + 4/4 压测 + 3/3 强杀恢复）。

---

## 六、C ABI 与导出

### ✅ [E-01] 错误回调注册无并发保护（`err_fn`/`err_user` 数据竞争）[已修复并闭环]
- **来源：** 报告一 BUG-4 ＝ 报告二 R2-高#5（均定为 P0/高优先级）
- **位置：** `core/src/logx_c.cpp`（`logx_set_error_handler`）；并发读方 `logx_internal.h` 的 `emit_event`
- **状态：** ✅ **已修复并闭环**（实例级 `err_mu` 互斥量 + 锁外回调执行 + 并发回归测试）
- **描述：** `logx_set_error_handler` 无锁写 `err_fn`/`err_user`，后台 Flusher 在 `emit_event` 中并发读取，构成数据竞争；头文件注释声明"可被任意线程调用"，风险更高。
- **修复方案与落地：**
  1. `Instance` 新增独立 `std::mutex err_mu`（不复用 `inst->mu`：`logx_write` 持 `inst->mu` 期间也会 `emit_event("staging full")`，非递归锁会死锁；锁序恒为 `inst->mu → err_mu`，无反转）；
  2. `logx_set_error_handler` 写入与 `emit_event` 快照读取均在 `err_mu` 保护下完成；回调本身在锁外执行，回调内再注册 handler 或再打事件不会自锁；
  3. 新增回归测试 `Robustness.ErrorHandler_ConcurrentReregistrationNoRace`：后台线程高频注册/注销回调 + 主线程持续写入驱动 Flusher 产生并发事件读，全程无崩溃无脏指针回调；
  4. 双构建矩阵 + Android 交叉编译全绿通过。

### ✅ [E-02] `logx_write` 中 `build_records` 失败路径 `drop_too_large` 统计与错误分类不准确 [已修复并闭环]
- **来源：** 报告一 BUG-9 ＋ 报告二 R2-中#16（合并）
- **位置：** `core/src/logx_c.cpp`（`logx_write`）、`core/src/flusher.cpp`（`build_records`）
- **状态：** ✅ **已修复并闭环**（三态返回码 + 递增点统一至超长分支 + INVALID_ARG 分类）
- **描述：** `build_records` 格式化失败（非法 level/空 tag 等）与内容超长混在一起，`logx_write` 统一报 `LOGX_ERR_TOO_LARGE`/"log too large"；`drop_too_large` 递增点分散且格式化失败分支不递增，统计与错误回调均不准确。
- **修复方案与落地：**
  1. `build_records` 返回码三态化：`< 0`（`kBuildRecordsInvalidInput`）= 输入非法；`> 0` = 格式化总字节数；超长判定职责上移至调用方；
  2. `logx_write` 区分两类失败：非法输入返回 `LOGX_ERR_INVALID_ARG` 并发 `"invalid log input"` 事件；超长才返回 `LOGX_ERR_TOO_LARGE` 且在唯一超长分支递增 `drop_too_large`；
  3. 核实澄清：空 tag 是 `format_record` 的合法输入（`tag_len=0` 允许），并非报告所述非法分类场景；非法分类的真实输入为 level 越界（< 0 / > 5）与超 64B tag；
  4. 新增回归测试 `Robustness.WriteInvalidLevel_ClassifiedAsInvalidArg`：level 越界 → `INVALID_ARG` 且不影响超长计数；空 tag → 正常写入；超长 → `TOO_LARGE` 且 `drop_too_large` 恰好 +1；
  5. 双构建矩阵 + Android 交叉编译全绿通过。

### ✅ [E-03] 导出错误被吞掉 [已修复并闭环]
- **来源：** 报告二 R2-中#13
- **位置：** `core/src/logx_c.cpp`（`write_all_lines`、`export_range`）
- **状态：** ✅ **已修复并闭环**（写错误传播 + 回调返回值传播 + 失败文件清理）
- **描述：** 导出过程忽略临时文件写入错误（`write_all_lines` 中 `(void)n`），也忽略回调返回值，最后仍返回 `LOGX_OK`。上层误以为导出成功，但文件可能没写完整，或业务回调已报告失败。
- **修复方案与落地：**
  1. `write_all_lines` 改为 `bool`：任一写入返回值 ≠ 行长度即整体失败，杜绝短写静默；
  2. `export_range` 引入 `final_status`：解码失败 / 临时文件创建或截断或写出失败 → `LOGX_ERR_IO`；业务回调返回非 0 → `LOGX_ERR_CODEC` 并发事件打点；任一失败均如实向上传播，不再恒返 `LOGX_OK`；
  3. 失败路径完整性：写失败 / 截断失败时删除残留临时文件，回调失败后临时文件仍按契约删除；
  4. 新增回归测试 `Robustness.ExportErrorPropagation_WriteFailAndCallbackFail`：装饰 PAL 对非 append 打开的 fd（导出临时文件）注入写失败 → `LOGX_ERR_IO` 且回调零调用；回调返回 7 → `LOGX_ERR_CODEC`；
  5. 双构建矩阵 + Android 交叉编译全绿通过。

### ✅ [E-04] `logx_export` 临时文件在同一天多分卷场景下固定命名覆盖冲突 [已修复并闭环]
- **来源：** 报告一 WARN-8 ＝ 报告三 BUG-E1
- **位置：** `core/src/logx_c.cpp`（`make_tmp_path`）
- **状态：** ✅ **已修复并闭环**（日期 + PID + CSPRNG 随机数命名）
- **描述：** 临时明文文件按日期命名（`.tmp_export_%u`）。同一天多个分卷文件或多次调用 export 共用同一路径；前次回调处理缓慢或并发导出时，临时文件会被后一次处理强行覆盖或截断。
- **修复方案与落地：**
  1. `make_tmp_path` 改为 `.tmp_export_<日期>_<PID>_<8hex 随机数>`：随机数经 `Pal::random_bytes`（CSPRNG，F-09 契约）获取，失败退化用毫秒时钟；
  2. 同一天多分卷 / 多次导出 / 并发导出各自持有独立临时文件，`O_TRUNC` 互踩根除；
  3. 新增回归测试 `Robustness.ExportTmpPath_UniquePerCallAndCleaned`：同日连续 5 次导出，5 个回调路径两两互异、含 `.tmp_export_` 前缀，导出完成后全部清理；
  4. 双构建矩阵 + Android 交叉编译全绿通过。

### ✅ [E-05] Windows 路径分隔符仅查找 `'/'`，不兼容 `'\\'` [已修复并闭环]
- **来源：** 报告一 BUG-17
- **位置：** `core/src/logx_c.cpp`（`logx_open` staging 目录解析）
- **状态：** ✅ **已修复并闭环**
- **描述：** `logx_open` 中 staging 目录解析只 `find_last_of('/')`，Windows 下 `\` 路径找不到分隔符，导致 `dir = "."`，staging 目录创建失败。
- **修复方案与落地：** 改为 `find_last_of("/\\")`，与 F-02 修复后 `parse_log_filename` 的跨平台分隔符语义对齐；双构建矩阵 + Android 交叉编译全绿通过。

### ✅ [E-06] `flush_threshold == 0` 语义模糊 [已修复并闭环（含复审修正）]
- **来源：** 报告三 BUG-A1；2026-09-07 复审追加发现核心唤醒判定语义反转 P0 缺陷
- **位置：** `core/include/logx_c.h`（`logx_config_t.flush_threshold`）、`core/src/logx_c.cpp`（`logx_open` / `logx_write` 唤醒判定）、`android/cpp/logx_jni.cpp`、`android/kotlin/LogX.kt`
- **状态：** ✅ **已修复并闭环**（0 = 显式禁用 / UINT32_MAX = 显式默认 + 复审发现的唤醒判定反转 P0 修正 + ev_signal 计数确定性测试）
- **描述：** 配置传入 0 被静默替换为默认 8KB。若业务意图是"彻底关闭自动刷盘、完全由手动 flush 控制"，该意图被吞。
- **复审追加发现（🔴 P0，首版修复引入）：** 首版修复只改了 `logx_open` 的配置接收逻辑，遗漏了 `logx_write` 中的核心唤醒判定 `need_wake = pending >= inst->flush_threshold`——阈值 0 时因 `pending >= 0` 恒真，每条写入均立即 `ev_signal` 唤醒 Flusher，"禁用自动唤醒"被精确反转为"每写必醒"。且首版测试断言 `flushed_bytes_total == 0` 依赖与 Flusher 75ms debounce 的竞态抢跑，属偶发假绿。
- **修复方案与落地：**
  1. C ABI 语义显式化：`flush_threshold = 0` → 禁用按阈值自动唤醒（纯手动 `logx_flush` 控制）；`UINT32_MAX` → 取默认 8KB；其它值 → 按字节阈值；头文件契约注释同步；
  2. **唤醒判定守卫（复审修正）**：`logx_write` 改为 `need_wake = flush_threshold > 0 && pending >= flush_threshold`。守卫必须放在阈值上而非 pending 上——写入成功后 pending 恒 > 0，对 pending 加前置条件是死代码（首版 `pending > 0 &&` 即犯此错，被复审测试当场拦截）；阈值 0 时落盘节奏由 Flusher 自身 `kFlushDebounceMs` 轮询兜底，数据不丢；
  3. 全栈对齐：JNI 层约定 `flushThreshold == -1` 映射 `UINT32_MAX` 哨兵（Kotlin Int 无法表达），0 直传 core 表示禁用；Kotlin `Config.flushThreshold` 注释同步更新；
  4. **确定性测试（根除假绿）**：`Robustness.FlushThreshold_ZeroDisablesAutoWake_MaxMeansDefault` 重写为 `CountingEvSignalPal` 装饰器直接计数 `ev_signal`：阈值 0 时写入 10 条零 signal（反转回归锚点）→ 手动 flush 后 signal 增加（`flush_blocking` 轮询语义为 ≥ +1）且数据落盘；默认阈值下小写入零 signal；阈值 1 时单条写入恰好 +1（自动唤醒正路径）；三段覆盖正/反/边界，无任何时序依赖；
  5. 双构建矩阵 + Android 交叉编译全绿通过。

### ✅ [E-07] `logx_stats_t` 结构体按值返回缺乏 ABI 向前兼容能力 [已修复并闭环]
- **来源：** 报告三 BUG-A2
- **位置：** `core/include/logx_c.h`（`logx_stats_t`）、`core/src/logx_c.cpp`（`logx_get_stats`）
- **状态：** ✅ **已修复并闭环**（reserved[4] 版本化预留 + 零值契约）
- **描述：** 结构体定义暴露在 C 头中按值拷贝，未来新增统计指标会破坏既有二进制符号兼容性。
- **修复方案与落地：**
  1. `logx_stats_t` 末尾追加 `uint64_t reserved[4]`：未来新增统计指标一律占用 reserved 位并同步递减数量，现有字段永不重排/删除；头文件注释固化追加纪律（老调用方按旧 sizeof 分配的缓冲不会被新实现越界写）；
  2. `logx_get_stats` 对 reserved 区保持全零输出契约；
  3. 新增回归测试 `Robustness.StatsReservedZeroContract`：脏值预填结构体后调用 `logx_get_stats`，断言 reserved 区全零；
  4. 双构建矩阵 + Android 交叉编译全绿通过。

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

### ✅ [P-02] Windows `file_size_path` 频繁打开关闭句柄开销过高 [已修复并闭环]
- **来源：** 报告三 BUG-P2
- **位置：** `core/src/pal_win32.cpp`（`file_size_path`）
- **状态：** ✅ **已修复并闭环**
- **描述：** 每次查询文件大小均执行 `CreateFileA + CloseHandle`，扫盘调用频繁时 IO 开销偏大。
- **修复方案与落地：** 改用 `GetFileAttributesExA` 直接读 `WIN32_FILE_ATTRIBUTE_DATA`，零内核句柄往返；64 位大小由 `nFileSizeHigh/Low` 显式拼接。复核确认两处调用方语义兼容：flusher 分卷探测以 `>= 0` 判"路径已占用"、快照统计以 `< 0` 剔除——`GetFileAttributesExA` 对存在路径（含目录）返回成功，错误态契约（不存在 → -1）不变；新增回归测试 `Robustness.PalFileSizePath_AttributesBased_SemanticsPreserved`（精确大小、与 fd 版 `file_size` 一致、不存在 → -1、目录可查询）。

### ✅ [P-03] `file_rmdir`/`file_unlink` 失败无明确错误转译 [已修复并闭环]
- **来源：** 报告三 BUG-P3
- **位置：** `core/src/pal_win32.cpp`（`file_unlink`/`file_rmdir` + `win32_error_to_errno`/`translate_win32`）；`pal_posix.cpp` 的 rmdir/unlink 原生透传 errno，无需改动
- **状态：** ✅ **已修复并闭环**
- **描述：** Windows 侧 rmdir/unlink 失败直接返回 -1 且不设置 errno，调用方无法区分"目录非空"与"目录不存在"等语义；POSIX 侧原生透传，两侧行为不对称。
- **修复方案与落地：** 新增 `win32_error_to_errno` 转译表（ERROR_FILE/PATH_NOT_FOUND → ENOENT、ERROR_DIR_NOT_EMPTY → ENOTEMPTY、SHARING/LOCK_VIOLATION → EBUSY、ACCESS_DENIED → EACCES、WRITE_PROTECT → EROFS、其余保守归 EIO 不丢错误态），`file_unlink`/`file_rmdir` 统一经 `translate_win32` 置 errno 后返回 -1，与 POSIX 语义对齐；新增回归测试 `Robustness.PalRmdirErrnoDistinction_NotEmptyVsMissing`（重复 rmdir → ENOENT、目录非空 → ENOTEMPTY、重复 unlink → ENOENT，POSIX/Win32 双侧断言一致）。

### ✅ [P-04] `monotonic_ns` 每次调用都查询性能计数器频率 [已修复并闭环（含溢出缺陷追加修复）]
- **来源：** 报告一 WARN-2；2026-09-07 修复复核追加发现乘法溢出回绕缺陷
- **位置：** `core/src/pal_win32.cpp`（`monotonic_ns`）
- **状态：** ✅ **已修复并闭环**
- **描述：** `QueryPerformanceFrequency` 结果在系统运行期间固定不变，却每次调用都查询，纯属高频内核开销。
- **复核追加发现（原实现叠加缺陷）：** 旧代码 `now.QuadPart * 1000000000ULL / freq` 先乘后除，QPC 典型频率 10MHz 下开机约 31 分钟 `now*1e9` 即溢出 uint64 回绕，单调时钟发生跳变——flush 同步自旋、测试耗时轮询等依赖单调性的逻辑会失效。
- **修复方案与落地：** ① QPF 用 C++11 magic static 进程内仅查询一次并缓存（线程安全）；② 换算改为先除后乘两段式 `(ticks/freq)*1e9 + (ticks%freq)*1e9/freq`，余数 < freq 时余数项距 2^64 有数倍裕度，彻底消除溢出回绕；新增回归测试 `Robustness.PalMonotonic_NonDecreasingSaneUnits`（单调不回退、sleep 15ms 后推进量落在 [15ms, 5s] 的量纲与跳变双断言）。

### ✅ [P-05] `file_open` 中 `SetFilePointer` 设置末尾是冗余的 [已修复并闭环]
- **来源：** 报告一 WARN-6
- **位置：** `core/src/pal_win32.cpp`（`file_open`）
- **状态：** ✅ **已修复并闭环**
- **描述：** `O_APPEND` 已通过 `_open_osfhandle` 保证原子 append，open 时的指针移动冗余无害，易误导维护者以为 append 语义由该调用提供。
- **修复方案与落地：** 删除 `SetFilePointer(h, 0, nullptr, FILE_END)` 冗余调用，原位注释固化"append 语义由 `_O_APPEND` 每次写原子保证"契约；全量单测（含 append 写、切分、恢复各路径）通过验证语义无损。

### ✅ [P-06] Windows `atomic_add`/`atomic_load` 缺少 64 位对齐前置检查 [已修复并闭环]
- **来源：** 报告三 BUG-P4
- **位置：** `core/src/pal_win32.cpp`（`atomic_add`/`atomic_load`）
- **状态：** ✅ **已修复并闭环**
- **描述：** `InterlockedExchangeAdd64`/`InterlockedCompareExchange64` 要求目标 8 字节对齐，错位访问触发硬件异常且无提示。
- **修复方案与落地：** 两函数入口均增加 `assert((reinterpret_cast<uintptr_t>(p) & 7) == 0)` Debug 断言并注释说明 Release 依赖（Instance 内计数器均为独立对齐成员）；新增回归测试 `Robustness.PalAtomicAdd_AlignedBehaviorContract`（旧值返回、负增量、4 线程 × 10000 次并发累加零丢失）。

---

## 八、JNI 层

### 🔴 [J-01] `nativeClose` 提前释放 KeyProvider 上下文，与 Flusher 线程竞态导致潜在 UAF [✅ 已修复]
- **来源：** 报告二 R2-高#6 ＝ 报告三 BUG-J5（报告二列高优先级，报告三列 P1）
- **位置：** `android/cpp/logx_jni.cpp:213-238`
- **描述：** 原实现 `nativeClose` 先 `delete kp_ctx` 再调 `logx_close(inst)`；而 `logx_close` 首先执行 `flush_blocking()`，期间后台 Flusher 仍在运行，可能继续调用 `jni_key_provider_trampoline` 访问已释放的 JNI 上下文（UAF 崩溃）。此外若 `key_user` 被置空或释放，Flusher 取 key 失败将静默把尾块降级为明文，且提前清空 `g_err_ctx` 会导致关闭阶段产生的错误事件丢失。
- **修复方案：** 调整 `nativeClose` 析构顺序：先调用 `logx_close(inst)` 确保 `flush_blocking` 完成（密钥全程可用）并 `thread_join` Flusher 退出；随后再安全释放 `kp_ctx` 全局引用与对象，最后清理 `g_err_ctx`。不给 `inst->key_user` 赋值避免对已释放 `inst` 的写 UAF。核心契约已在 `tests/unit/test_flusher.cpp:KeyProvider_DrainsEncryptedTailAndNotCalledAfterClose` 严密断言。

### ✅ [J-02] `g_err_ctx` 全局静态变量多实例无保护 [已修复并闭环]
- **来源：** 报告三 BUG-J4
- **位置：** `android/cpp/logx_jni.cpp`（`jni_error_trampoline` / `nativeInit` / `nativeClose`）
- **状态：** ✅ **已修复并闭环**
- **描述：** 多实例环境共享同一全局 `g_err_ctx`，一个实例关闭时清空全局结构（`g_err_ctx = JniErrorContext{}`），导致其余实例的错误回调静默失效。
- **修复方案与落地：**
  1. 错误上下文按实例持有：`nativeInit` 为每个实例分配独立 `JniErrorContext`（jvm + 类级 jclass/jmethodID 引用），经 `cfg.err_user` 传入 core；`jni_error_trampoline` 从 `user` 参数取上下文，文件级全局 `g_err_ctx` 整体移除；
  2. 类级常量（`g_logx_class` 全局引用、`g_mid_on_error`）保留文件级静态并注明复用安全性——二者为只读类元数据，进程内稳定，不构成实例间可变共享状态；
  3. 生命周期与实例严格同步：`nativeClose` 在 `logx_close`（含 flush_blocking 最后一次 drain）完成、Flusher join 后释放本实例上下文，关闭阶段事件不丢；`logx_open` 失败路径同样回收，杜绝泄漏；
  4. 核心契约回归测试 `Robustness.ErrUserPerInstance_IsolationAcrossClose`：双实例各绑独立 probe，事件精确路由到各自 user；实例 A close 后实例 B 的回调路径照常工作（全局态若残留则必然失败），同时验证 E-01 锁保护下的逐实例重注册路由。
- **构建验证：** 三重矩阵全绿（build-full 126 测试 / build 108 单测 + 4 压测 / Android arm64 `liblogx.so` 编出含新 JNI 代码）。

### ✅ [J-03] `NewStringUTF(msg)` 在含非 ASCII 时的 Modified UTF-8 格式风险 [已修复并闭环]
- **来源：** 报告三 BUG-J6
- **位置：** `android/cpp/logx_jni.cpp`（`make_jstring_utf16` + 新增 `android/cpp/jni_string_util.h`）
- **状态：** ✅ **已修复并闭环**
- **描述：** `NewStringUTF` 期望 Modified UTF-8（CESU-8 变体），C++ 端错误消息透传标准 UTF-8（尤其 4 字节补充平面字符、嵌入 NUL 等）时在个别 JVM 实现上可能触发编码异常。
- **修复方案与落地：**
  1. 统一构造路径：新增 `make_jstring_utf16` —— 先将 UTF-8 严格解码为 UTF-16 码元序列，再经 `env->NewString`（UTF-16 原生语义）构造 jstring；错误回调与导出回调（date/path 含用户自定义 logDir 时同样可能非 ASCII）两条链路全部切换；
  2. 解码器纯逻辑化（`jni_string_util.h`，无 JNI 依赖）：按 RFC 3629 拒绝过长编码/代理区/越界，非法字节产出 U+FFFD 并滑动 1 字节重同步（与核心 `parse_records_stream` 容灾策略一致），绝不崩溃；兼容 Modified UTF-8 的 NUL 编码（C0 80 → U+0000）；
  3. 构造失败（OOM）降级空串占位，回调链路不中断；
  4. 回归测试：新增 `tests/unit/test_jni_string.cpp` 共 9 个用例（ASCII 恒等、Modified NUL、2/3 字节 BMP、4 字节 emoji 代理对、过长编码/代理区/U+10FFFF 以上拒绝、截断与乱码重同步不丢后续字符、混合串往返），桌面与 Android 单测同源覆盖；CMakeLists 增补 `android/cpp` include 路径。
- **构建验证：** 三重矩阵全绿（同 J-02）。

---

## 九、Kotlin 封装层

### ✅ [K-01] 协程 `export` 丢弃 `nativeExport` 返回值，底层失败静默返回成功 [已修复并闭环]
- **来源：** 报告三 BUG-K5
- **位置：** `android/kotlin/LogX.kt`（协程 `export`）
- **状态：** ✅ **已修复并闭环**
- **描述：** `nativeExport` 返回 Int 状态码，但协程函数未接收该返回值，恒返回初始化的 `var result = 0`。底层彻底失败且从未触发回调时，业务侧仍收到 0（成功），导致导出状态误判。
- **修复方案与落地：**
  1. 协程 `export` 直接接收并返回 `nativeExport` 的状态码，删除无用 `var result = 0` 中转变量；
  2. 函数 KDoc 固化返回值与异常契约：0 = LOGX_OK；-1 = 实例未初始化；-6 LOGX_ERR_CODEC = 业务回调返回非 0（经 E-03 错误传播链路上报）；-3 LOGX_ERR_IO = 临时文件写出失败；-8 LOGX_ERR_NOT_FOUND = 时段内无日志文件；
  3. 异常如实传播：若业务 block 内部抛出真实异常，优先向外抛出真实业务异常，避免掩盖错误根因；临时文件在 finally 块中安全兜底删除；
  4. 多分卷容错：前一分卷处理抛出异常后快速失败，后续分卷直接返回 -1 抑制冗余调用。
- **备注：** 本条与 K-02 在同一函数内协同重构，验证方式一致（见 K-02）。

### ✅ [K-02] `export` 协程中嵌套 `runBlocking` 阻塞 IO 调度器 [已修复并闭环（含死锁与编译缺陷校正）]
- **来源：** 报告三 BUG-K3；2026-09-08 复审修正初版 `CountDownLatch` 改动缺陷
- **位置：** `android/kotlin/LogX.kt`（协程 `export`）
- **状态：** ✅ **已修复并闭环**
- **描述：** 在 `withContext(Dispatchers.IO)` 内直接启动无上下文的 `runBlocking`，外部协程上下文与结构化并发特征丢失。
- **复审追溯（初版修复缺陷校正）：**
  初版尝试采用 `CountDownLatch` + `launch(ATOMIC)`，复审发现三大严重缺陷：
  1. 编译错误：标准 JDK / Android SDK 的 `CountDownLatch` 根本没有 `awaitUninterruptibly()` 方法（属于 Guava / Netty 第三方库），在纯 Kotlin 编译链下直接无法编译；
  2. 调度器饥饿与死锁：C++ `logx_export` 为同步 Push 契约（回调返回即执行 `file_unlink` 删除临时文件），回调线程必须同步等待。若回调线程阻塞在 Latch 上，同时通过 `launch` 将 block 派发到同一个 `Dispatchers.IO`，单次导出将占用 2 个 IO 线程；若调用方在受限并发度调度器（如 `limitedParallelism(1)`）下运行或线程池耗尽，将发生**确定性永久死锁**；
  3. 语法兼容性与异常吞噬：`catch (_: Throwable)` 在低于 Kotlin 2.1 时报错，且将业务真实异常吞噬转化为 -6 状态码，无法排查定位。
- **最终修复方案与落地：**
  1. 上下文绑定：在协程入口捕获外层挂起点上下文 `val parentContext = coroutineContext`，通过 `runBlocking(parentContext)` 执行业务 block；
  2. 结构化并发保真：`parentContext` 包含父协程的 Job 与取消状态，父协程被取消时（如 Activity/ViewModel 销毁），取消信号能无缝传导至 `block` 挂起点中断执行；
  3. 防死锁与零额外线程占用：`runBlocking` 直接在当前已被 native 调用的线程上运行事件循环，不向 `Dispatchers.IO` 重复派发任务，在 `limitedParallelism(1)` 或单线程调度器下依然 100% 安全运行，杜绝互锁；
  4. 异常真实传递：`userException?.let { throw it }`，业务发生网络/解析等异常时真实抛出，临时文件仍在 finally 中安全删除。
- **验证方式：** 全文静态复核、全编译兼容性断言、调度器并发无死锁推演。

### ✅ [K-03] `init()` 失败时 `errorHandler` 静态引用未清理导致 GC 延迟 [已修复并闭环]
- **来源：** 报告三 BUG-K1
- **位置：** `android/kotlin/LogX.kt`（`init`）
- **状态：** ✅ **已修复并闭环（顺带修复 initCount 卡死隐患）**
- **描述：** `nativeInit` 返回 0 失败时，抛异常前未将 `errorHandler` 重置为 null，单例持有的引用导致外部回调对象无法及时释放。
- **修复方案与落地：**
  1. `init` 全体语句纳入 `try/catch(Throwable)`：任何失败路径（参数校验 `require` 失败 / 目录创建异常 / `nativeInit` 返回 0）统一执行 `errorHandler = null` + `initCount.set(0)` 后原样重抛；
  2. 顺带修复原实现隐患：失败路径原仅 `nativeInit == 0` 分支会归零 `initCount`，`require` 校验失败路径会导致 `initCount` 永久卡在非零、后续合法重试 init 被"重复 init"误拒——现统一回滚，允许业务修正配置后重试；
  3. `close()` 同步补齐同一纪律：关闭成功后 `errorHandler = null`，回调对象可及时释放。
- **验证方式：** 同 K-02（全文静态复核，回滚路径覆盖全部失败分支）。

### ✅ [K-04] `setErrorHandler` 缺乏并发与时序校验 [已修复并闭环（含 init 前配置拦截校正）]
- **来源：** 报告三 BUG-K4；2026-09-08 复审修正初版时序逻辑缺陷
- **位置：** `android/kotlin/LogX.kt`（`setErrorHandler` / `close` / `init`）
- **状态：** ✅ **已修复并闭环**
- **描述：** 允许在任意时机修改错误处理器，缺乏状态检查与并发保护。
- **复审追溯（初版修复缺陷校正）：**
  初版拦截条件为 `if (initCount.get() == 0L && nativeHandle == 0L) return`，意图是防止 close 后的悬挂注册，却混淆了“未初始化状态”与“已关闭状态”（两者均为 0 与 0L），导致在 `LogX.init()` 前注册全局错误回调被静默丢弃（No-op）。
- **最终修复方案与落地：**
  1. 显式终结状态隔离：引入 `@Volatile private var isClosed: Boolean = false`，精确区分未初始化与已关闭；
  2. `setErrorHandler` 标注 `@Synchronized`：仅当 `isClosed` 为真时拒绝注册（保持 null 语义，防止悬挂引用）；未初始化时完全允许注册，支持在 `Application.onCreate` 提前配置；
  3. 生命周期联动：`init` 成功重置 `isClosed = false`；`close()` 终结时设置 `isClosed = true` 并清理 `errorHandler = null`；
  4. 读方 `onNativeError` 维持 `@Volatile errorHandler` 高性能无锁读取，可见性由 `@Volatile` 保证，锁仅保护写方与时序切换。
- **验证方式：** 同 K-02（全文静态复核，状态机时序推演覆盖全生命周期）。

---

## 十、Python 离线解码器

> **校准总注（2026-09-08 修复与加固闭环）：** D 系列各报告的建议基于 FM-02/FM-04/F-05 修复**之前**的旧 C++ 行为写作，本次修复逐条对照现行 C++ 实现校准后落地，并按 C++ 写盘格式构造合成日志文件完成 18 项端到端验证（正常/zstd/AES-GCM 加密/跨块分片/cksum 损坏/坏块头/坏 magic 重同步/解压上限/同日多分卷合并/块尾断裂 carry 丢弃/CLI 冒烟），全部通过。复审中额外发现并修复两个解码器真实缺陷：
> 1. `decrypt_gcm` 调用 `AESGCM.decrypt()` 缺失必需的 `associated_data` 参数（C++ 侧无 AAD，须显式传 `None`），即修复前任何加密文件的解密路径都会直接 TypeError 失败；
> 2. `main()` 中在 `--out` 模式下同日多分卷文件（如 `20260908.log` 与 `20260908.log.1`）因重复以 `'w'` 模式打开同一个输出文件导致前序分卷被完全覆写抹除，现已改为 `current_date` 状态跟踪并在同日分卷下保持句柄顺序连续写入，确保多分卷日志无损合并。

### ✅ [D-01] `parse_lgxe` cksum 失败直接 `break`，块内后续记录丢失 [已修复并闭环]
- **来源：** 报告三 BUG-D2；报告二 R2-测#7 关联
- **位置：** `decoder/logx_decode.py`（`parse_lgxe_segments`）
- **状态：** ✅ **已修复并闭环（校准方案落地，未采纳报告建议原文）**
- **描述：** C++ 端 `parse_records_stream` 校验失败时滑动 1 字节重同步；Python 端直接 `break`，导致块内后续所有健康日志丢失。
- **建议校准：** 报告建议 `pos += 1; continue`（对齐旧 C++ 逐字节滑动）。FM-04 修复后现行 C++ 对 cksum 失败改为**跳过整条损坏记录**（6B 头 + len：记录格式按头起始对齐，损坏记录内不存在合法子记录，逐字节重扫既找不回数据又造成最坏 O(n²)）；对 magic 失效才做字节滑动重同步。逐字节 `pos += 1` 会落回 O(n²) 且与现行 C++ 语义脱节，故不采纳原文。
- **修复方案与落地：**
  1. `parse_lgxe` 重构为 `parse_lgxe_segments`：cksum 失败跳过整条损坏记录（`6 + payload_len`）继续，单条损坏不再丢弃块内后续健康记录（对齐现行 C++ FM-04）；
  2. magic 失效（含零段）按现行 C++ 字节滑动重同步到下一个合法 magic（要求 6B 头完整），重同步失败才停止本块扫描；
  3. 记录头长度越界（头长不可信）滑动 6B 重扫（对齐 FM-04），不再整块放弃；
  4. 每次流断裂事件（cksum 失败 / 长度越界 / 重同步）按 FM-02 语义丢弃跨片残片：解析器输出分段序列，调用方在段边界调用重组器新增的 `drop_carry()`，重同步点后的首/中片绝不与旧残片错误拼接；
  5. 块末状态精准追踪：任何流断裂事件（坏 cksum / 长度越界 / 坏 magic）置 `carry_ok = False`，仅当合法记录成功解析后置 `carry_ok = True`，确保块尾遭遇损坏截断时外层 `drop_carry()` 准时触发，彻底杜绝孤儿跨块残片误保留（实测用例覆盖）；块末干净收束时合法保留，跨块分片照常衔接。

### ✅ [D-02] `decode_file_records` 块解码失败直接 `break`，整文件后半部分截断 [已修复并闭环]
- **来源：** 报告三 BUG-D3；报告二 R2-测#7 关联
- **位置：** `decoder/logx_decode.py`（`block_header_plausible`、`decode_file_records`、`main`）
- **状态：** ✅ **已修复并闭环（校准方案落地，未采纳报告建议原文）**
- **描述：** C++ 端遇坏块时记录标记并 `continue` 到下一块；Python 端 `break` 退出整文件扫描，单块损坏导致整文件后半部分丢失。
- **建议校准：** 报告建议一律 `off += block_size; continue` 不安全且不可行——块边界自描述于块头，**块头结构损坏**时 `block_size` 本身不可信，盲目 continue 会以错误跨度滑动导致整文件错位；现行 C++（F-05 `block_header_plausible`）对结构损坏正是终止扫描（宁丢不脏）。`continue` 仅适用于**块头合法但块级解码失败**（payload CRC / GCM / 解压 / 缺 key）的场景。
- **修复方案与落地：**
  1. 新增 `block_header_plausible` 块头严检，逐项对齐 C++ F-05：magic / 版本 / flag 保留位 / 算法编号 / `seqno > 0` / `raw_len ∈ [6, 16MB]` / `payload_len ∈ (0, 16MB]` / 未压缩时 `payload_len == raw_len` / 块大小不越文件尾；
  2. 结构损坏 → errors 记录后终止本文件扫描（与 C++ 一致）；块级解码失败 → errors 记录 + 生成与 C++ 导出一致的占位行（`[seq=N crc fail]` / `[seq=N decode fail]`）后跳过该块继续，文件后半部分照常解码；
  3. 占位行在 `main()` 输出层直通（精准匹配 `crc fail]` / `decode fail]` 后缀，不进入用户载荷解析），与 C++ 导出"占位行出现在导出文本流中标记数据空洞"的语义一致；
  4. 同日多分卷无损合并：修复 `main()` 中 `--out` 模式下遍历同一天的分卷文件（`.log`、`.log.1`...）时重复以 `'w'` 打开导致前序分卷被覆写抹除的历史 Bug，改为按日期归并句柄顺序连续写入；
  5. 端到端验证：坏 CRC 块跳过后续块解码成功、坏块头干净终止不错位、同日多分卷合并无损完整、CLI 退出码 2 + stderr 错误报告 + stdout 含占位行。

### ✅ [D-03] `decode_block_raw` 的 GCM 校验失败处理策略与 C++ 端不一致 [已修复并闭环]
- **来源：** 报告三 BUG-D4
- **位置：** `decoder/logx_decode.py`（模块 docstring、`decode_block_raw`、`decrypt_gcm`）
- **状态：** ✅ **已修复并闭环（文档化策略差异 + 顺带修复 GCM 缺参缺陷）**
- **描述：** C++ 导出路径 GCM 校验失败时静默丢块并触发 Event；Python 端输出到 stderr，策略差异未在文档中明确。
- **修复方案与落地：**
  1. 核实确认：现行 C++ 导出路径对块级解码失败（含 GCM）的行为 = 占位行 `[seq=N decode fail]` + `emit_event` 进程内监控 + 继续下一块——**并不"静默丢块"**，报告对 C++ 行为的描述已过时；Python 端 = errors 收集 + stderr + 退出码 2 + 继续下一块。差异仅在事件通道（C++ 走 emit_event、本工具走 stderr），扫描连续性两侧一致；
  2. 模块 docstring 新增"错误处理策略"章节，按块头结构损坏 / 块级解码失败 / 块内记录损坏三层显式归档两侧语义与差异来源；`decode_block_raw` docstring 固化错误前缀分类（`crc32 mismatch` / `gcm fail` / `decompress fail` / `missing key`）与占位行映射；
  3. **顺带修复（报告未提及的真实缺陷）**：`decrypt_gcm` 的 `AESGCM.decrypt(nonce, ct)` 缺失必需的 `associated_data` 位置参数——C++ `encrypt_gcm` 无 AAD，须显式传 `None`；修复前加密文件解密路径 100% TypeError 失败（端到端实测暴露并验证修复）。

### ✅ [D-04] 死代码 `reassemble()` 残留 [已修复并闭环]
- **来源：** 报告三 BUG-D1
- **位置：** `decoder/logx_decode.py`
- **状态：** ✅ **已修复并闭环**
- **描述：** 旧版切片重组函数已无任何调用者，容易误导维护者。
- **修复方案与落地：** 删除 `reassemble()`（全库引用核实：无任何调用者，CHANGELOG 中仅历史记录）；`RecordReassembler.feed` 流式重组器为唯一重组路径。端到端验证 `not hasattr(L, 'reassemble')`。

### ✅ [D-05] `decompress()` 内存输出上限 64MB 过宽 [已修复并闭环]
- **来源：** 报告三 BUG-D5
- **位置：** `decoder/logx_decode.py`（`decompress`、常量 `MAX_BLOCK_RAW`）
- **状态：** ✅ **已修复并闭环（精确值校准：20KB+6 而非报告近似值 20KB+128）**
- **描述：** C++ 内部块最大约 20KB，Python 解码器允许 64MB 输出，遭遇恶意畸形帧时存在内存过度分配风险。
- **建议校准：** 报告建议收紧为约 `20 * 1024 + 128` 是近似估计；精确契约是 C++ `logx_staging.h` 的 `kBlockMaxRaw = 20 * 1024 + 6`（单条独占一块上限，含 6B 记录头），直接对齐该常量。
- **修复方案与落地：**
  1. 新增 `MAX_BLOCK_RAW = 20 * 1024 + 6` 常量并注明与 `kBlockMaxRaw` 的对应关系；zstd 路径 `max_output_size` 由 64MB 收紧至该值；
  2. zlib 路径补齐同等防护（原 `zlib.decompress` 无上限参数）：改用 `decompressobj` 限长解压，输出超限抛 `RuntimeError`（防 zip bomb 过度分配）；
  3. 端到端验证：超限输入被拒绝、合法小载荷解压不受影响。

---

## 十一、测试用例问题

> **修复批次注（2026-09-08）：** T-01 复核确认已闭环（`logx_crash_fork_tests` 已注册进 CMake 并三用例全绿）；T-02~T-11 本批次全部修复闭环。全套测试已在 Windows 宿主机与 Android arm64-v8a 真实物理设备双端全矩阵通过（Android 真机：128/128 单测全通过 + 4/4 高并发压测全通过 + 3/3 强杀恢复全通过）。T-04 修复引入 `logx_config_t.pal_override` 测试 seam（生产传 NULL 零行为变化），并在 POSIX PAL 补充 `thread_join` 空指针防护。

### 🔴 [T-01] "强杀恢复"测试没有真正模拟强杀 [✅ 已修复（2026-09-08 复核确认闭环）]
- **来源：** 报告二 R2-测#1
- **位置：** `tests/stress/test_crash_recovery.cpp:57`，新建 `tests/stress/test_crash_fork.cpp`
- **描述：** 测试设置 `flusher_exit = true` 后调用 `thread_join()`。但 Flusher 退出循环后还会执行最终 `drain_once()`（`core/src/flusher.cpp:613`），staging 数据已被正常刷盘，**没有验证"未刷盘 staging 在进程突然死亡后的恢复"**。
- **修复方案：** 
  1. 新增独立可执行文件 `logx_crash_fork_tests`（`tests/stress/test_crash_fork.cpp`），利用独立子进程执行写入，并在指定窗口调用系统底层立即终止接口（Windows `ExitProcess` / POSIX `::_exit`）彻底绕过 C++ 析构函数、atexit 钩子与 CRT 缓冲区，真实模拟硬崩溃。父进程重启实例验证数据 100% 零丢失复原与单调块序号衔接，包含 50 轮混合硬杀压力循环。
  2. 原 `test_crash_recovery.cpp` 语义重命名为 `GracefulTeardown_*`，准确反映优雅拆毁下的数据恢复与坏块自愈，并消除 `rand()` 路径竞争（T-10）。

### ✅ [T-02] 测试名称 `ExportWithMultipleRecordsAndMissingKey_HandledGracefully` 与实现不符 [已修复并闭环]
- **来源：** 报告一 TEST-BUG-1 ＝ 报告二 R2-测#2
- **位置：** `tests/unit/test_flusher.cpp`
- **状态：** ✅ **已修复并闭环（改名 + 补真实缺失密钥场景，双管齐下）**
- **描述：** 测试名暗示"缺失密钥优雅降级"，实际配置 `encrypt_enable = 0`（明文模式），根本没有涉及缺失 key 场景，误导维护人员。
- **修复方案与落地：**
  1. 原用例更名 `ExportWithMultipleRecords_PlaintextMode_ExportsAllLines`，如实反映"明文模式多记录完整导出"语义；
  2. 新增派生用例 `ExportEncryptedFile_MissingKey_SkipsGracefully` 补齐真正的缺失密钥场景：加密实例导出阶段将 `key_provider` 切换为返回 null，断言导出返回 `LOGX_OK`（优雅降级不崩溃）、输出含 `[skipped: ...]` 占位行、不含明文载荷——与 C++ `decode_one_file` 的降级契约逐点对齐；
  3. 派生用例按文件内既有惯例以 `#if LOGX_WITH_AESGCM` 守卫（基础构建未接 GCM 时 `logx_open` 对 `encrypt_enable=1` fail-fast，用例无意义；首轮验证该失败已被守卫根除）。

### ✅ [T-03] 加密块测试绕过生产文件头逻辑且未验证加密生效 [已随 F-01 闭环]
- **来源：** 报告二 R2-测#3 ＋ 报告一 TEST-BUG-8
- **位置：** `tests/unit/test_flusher.cpp`
- **状态：** ✅ **已随 F-01 修复一并闭环（2026-09-08 复核确认），无需额外改动**
- **描述：** 测试手工写入带加密标志的文件头，因此无法发现生产 `write_file_header()` 固定写入明文标志的问题（见 F-01）。报告一另在 TEST-BUG-8 中指出 `EncryptedBlockFileWriteAndDecode_Roundtrip` 只验证解密后大小正确，未验证"密文 ≠ 明文"（即加密确实发生）。
- **复核结论：** 报告建议的两项要求均已在 F-01 修复中落地并验证：
  1. 生产路径端到端：`EndToEndFormattedLog_WriteAndExport` 经生产配置（`encrypt_enable=1` + key_provider）写入→flush→断言落盘文件头 `flag_encrypted(fhdr[5])` 与 CRC→`logx_export` 导出明文并断言含载荷 Hex 特征、无 `decode fail`/`[skipped:` 占位——正是"生产文件创建→导出"闭环；
  2. 密文/明文差异断言：`EncryptedBlockFileWriteAndDecode_Roundtrip` 已含 `EXPECT_NE(payload_buf, rec)`（TEST-BUG-8 要求）。
  另有 `ExportHistoricalEncryptedFileWithUnsetFileHeaderFlag_RecoversSuccessfully` 覆盖历史存量文件的向前兼容。

### ✅ [T-04] PAL 注入时机晚于 Flusher 线程启动，产生数据竞争 [已修复并闭环]
- **来源：** 报告一 TEST-BUG-3 ＝ 报告二 R2-测#4
- **位置：** `core/include/logx_c.h`、`core/src/logx_c.cpp`、`tests/unit/test_robustness.cpp`、`tests/unit/test_flusher.cpp`
- **状态：** ✅ **已修复并闭环（新增 C ABI 注入 seam，报告两建议合并落地，优于任一单独方案）**
- **描述：** `logx_open` 内部已启动 Flusher 线程，测试随后才将 `inst->pal` 改为 Mock PAL；Flusher 读 `inst->pal` 与测试写 `inst->pal` 并发，数据竞争，测试结果不可信。
- **修复方案与落地：**
  1. `logx_config_t` 末尾追加 `void* pal_override` 测试专用注入字段（注释固化"生产必须传 NULL"纪律；`{}`/memset 零初始化天然兼容，无 ABI 消费方按 sizeof 拷贝该结构）；
  2. `logx_open` 在启动 Flusher 线程**之前**以 `cfg->pal_override` 定型 `inst->pal`——从根源消除竞态，比"创建实例前注入"（仍需后写指针）与"逐用例后注入"（仍留竞态窗口）都更彻底；
  3. 全测试面清理：`test_robustness.cpp` 17 处与 `test_flusher.cpp:TimeoutWhenFlusherStalled` 注入点全部改为 `cfg.pal_override = &mock;` 前置注入，并删除不再需要的"恢复真实 PAL 再关闭"补救代码（实例生命周期全程即注入 PAL，语义更真实且零竞态）；
  4. 双构建矩阵（全开 ON/ON 127/127、基础 OFF/OFF 108/108）重新编译与全量回归通过。

### ✅ [T-05] `FlushAndZeroMaintainsInvariant` 断言永远为真，无检测意义 [已修复并闭环]
- **来源：** 报告一 TEST-BUG-2 ＝ 报告二 R2-测#5
- **位置：** `tests/unit/test_staging.cpp`
- **状态：** ✅ **已修复并闭环（报告两建议合并落地：反证前置 + 空闲区语义双重校验）**
- **描述：** 先清零 `[0, 256)` 再调用 `debug_check_invariant(0, 256)`，被检查区域已全为零，断言必然为 true，无法验证清零与坐标推进逻辑。
- **修复方案与落地：**
  1. 反证前置：写入数据后、清零前先断言 `debug_check_invariant(0, 256)` 通过（检查器真实读取数据区内容）；
  2. 反证排除恒真：数据仍在 `[0,256)` 时按"空闲区"语义 `debug_check_invariant(256, 256)` 校验**必须失败**——证明检查器与数据布局真实耦合，排除"区域本就全零导致恒真"的假绿；
  3. 清零后再以 `(f=0,w=256)`（已提交语义）与 `(f=w=256)`（空闲区必须为零）双重校验，证明 `zero_range_locked` 真实执行、未漏清。

### ✅ [T-06] 压力测试没有断言 missing_count [已修复并闭环]
- **来源：** 报告二 R2-测#6
- **位置：** `tests/stress/test_concurrent.cpp`
- **状态：** ✅ **已修复并闭环（建议正确，直接采纳）**
- **描述：** 测试计算了缺失日志数量但只输出结果，没有 `EXPECT_EQ(missing_count, 0)`，可能存在丢日志却仍通过测试的情况（注：测试末尾 `EXPECT_EQ(exported_lines, written)` 的总数对账可捕获净丢失，仅"等量丢失+等量重复"可逃逸）。
- **修复方案与落地：** 输出语句后立即补 `EXPECT_EQ(missing_count, 0)` 显式断言，逐条精确对账封死"等量丢失+等量重复"逃逸路径；50 线程压测实测 missing=0 断言通过。

### ✅ [T-07] `ClockRollback` 测试未验证 seqno 单调性和文件切换正确性 [已修复并闭环]
- **来源：** 报告一 TEST-BUG-6
- **位置：** `tests/unit/test_robustness.cpp`
- **状态：** ✅ **已修复并闭环（建议正确，直接采纳；复用同文件 ClockJump 用例的 scan 断言模式）**
- **描述：** 时钟回拨场景两个核心保证缺少断言：回拨后写入日志的 seqno 是否大于未来日期文件的 seqno；Flusher 是否正确切换了日志文件。
- **修复方案与落地：** 新增 `scan_append_target` 三重断言：未来文件 `seqno == 1`（恰一块）、回拨文件 `seqno == 2`（严格大于，全局单调接管）、`truncated == false`（干净追加无撕裂）——同时锁死"文件切换正确"（回拨写入确实落在 20260902.log）与"seqno 单调"两项核心保证。

### ✅ [T-08] 硬编码 magic 字节（0x4c, 0x47） [已修复并闭环]
- **来源：** 报告一 TEST-BUG-4（`test_recovery.cpp` RingScanStopsOnTear）＋ TEST-BUG-5（`test_format.cpp` ParseSkipsPadAndReassembles）
- **位置：** `tests/unit/test_recovery.cpp`、`tests/unit/test_format.cpp`
- **状态：** ✅ **已修复并闭环（建议正确，直接采纳）**
- **描述：** 使用硬编码魔数而非常量，未来常量修改时测试容易失效且可读性差。（注：当前硬编码值与 `kMagicEntry = 0x474c` 小端字节序一致，属可维护性问题。）
- **修复方案与落地：** 两处全部改为 `put_u16` 写入 `kMagicEntry` 常量：test_recovery 撕裂半条改经 6B 头缓冲常量构造；test_format 首片/末片头经 `make_head` lambda 统一构造（`test_format.cpp` 补 `logx_staging.h` include 引入 `put_u16`，`kMagicEntry` 本定义于已包含的 `logx_format.h`）。

### ✅ [T-09] GCM 测试 key 仅初始化前 3 字节，不具代表性 [已修复并闭环]
- **来源：** 报告一 TEST-BUG-7
- **位置：** `tests/unit/test_codec.cpp`
- **状态：** ✅ **已修复并闭环（建议采纳；范围扩大到同族第二处稀疏密钥）**
- **描述：** `uint8_t key1[32] = {1, 2, 3}` 剩余 29B 全为 0，建议使用差异更大的完整 32B 密钥，与标准测试向量（NIST CAVS）对齐。
- **修复方案与落地：** `GCMRejectsWrongKey` 两把密钥与 `GCMRejectsCorruptTagAndCiphertext` 的 `{0xAA}` 稀疏密钥（同族遗漏，一并修正）均改用 `make_randomish(32)` 确定性伪随机完整 32B 密钥——不引 `rand()`（规避 T-10 同族不确定性）、可复现、全字节分布均匀，风格与 NIST CAVS 随机密钥一致。

### ✅ [T-10] `rand()` 未播种导致测试目录重名风险 [已修复并闭环]
- **来源：** 报告三 BUG-T1
- **位置：** `tests/stress/test_concurrent.cpp`、`tests/unit/test_recovery.cpp`、`tests/unit/test_flusher.cpp`、`tests/unit/test_robustness.cpp`
- **状态：** ✅ **已修复并闭环（建议采纳；范围扩大至全部同族用法）**
- **描述：** 未 `srand()` 产生确定伪随机序列，CI 重跑或上轮异常退出遗留目录时可能引发目录冲突。
- **复核校准：** 报告点位之一的 `test_crash_recovery.cpp` 已在 T-01 闭环时改为 `s_recovery_dir_seq` 原子计数器；实际残余在 `test_concurrent.cpp` 与单测三文件（报告未列出的同族）。
- **修复方案与落地：** 全部替换为文件级 `static std::atomic<uint64_t>` 自增计数器（配合既有 PID 前缀，进程内/跨进程均无撞名）；测试目录名生成不再依赖任何随机源，确定性可复现。

### ✅ [T-11] 竞争线程过早启动导致前 15ms 空跑 [已修复并闭环]
- **来源：** 报告三 BUG-T2
- **位置：** `tests/stress/test_concurrent.cpp`
- **状态：** ✅ **已修复并闭环（建议采纳，落地为两阶段发令枪 + 数据量门控）**
- **描述：** 竞争线程发令枪响后立即每 15ms 调用 `logx_flush`，此时工作线程刚启动、staging 尚无数据，前 15ms 未构成实质竞争窗口。
- **修复方案与落地：**
  1. 发令枪拆为两阶段：`pre_gun` 放行 Worker 预热写（每线程 8 条，50×8 条 ≈ 90KB），`start_gun` 才放行竞争 flush 线程——竞争窗口从第一笔 flush 起就有真实数据可刷；
  2. 主线程数据量门控：`pre_gun` 后自旋等待 `staged_bytes_total >= 16KB`（恰为分块阈值）才鸣 `start_gun`，门控条件与被测行为强耦合；
  3. Worker 循环体重构为 `write_one` lambda（预热与主循环共用同一写入+重试+对账路径，消除重复），对账语义不变；
  4. 50 线程压测含新断言全绿，`Missing records found: 0`。
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
| **T4.0-3** | §4.2/§10 双 Header "写 A→seq_cst 屏障→写 B" | ✅ 已通过 S-06 闭环（写 A 完整 → seq_cst fence → 写 B 完整 → release fence，杜绝 (A旧,B新) 逆序窗口） | — | S-06 |
| **T4.0-4** | §4.1 Flusher 50~100ms Debounce 聚集刷盘 | ✅ 已通过 F-10 闭环（定型 75ms 聚集窗口 + 常态化防抖轮询，零散小写入自然聚集落盘） | — | F-10 |
| **T4.0-5** | §10 ARMv8 Crypto Extensions 加速 | ✅ 已落地并闭环：`aesce.c` 移植完成，AESE/AESMC/EOR 标准指令直通；后续独立复审完成官方向量锚定（McGrew-Viega TC13/14/15 + 零密钥基线，OpenSSL 交叉验证 72/72）、热路径逐块探针消除与防竞态定型，x86 + 真机（骁龙865）双路径全量回归通过 | — | C-04 |
| **T4.0-6** | §11 运行期每 30s 定时 msync/fdatasync | ✅ 已通过 F-04 补齐：`flusher_loop` 接入 30s 超时唤醒与 `durability_sync`，落盘 + staging 强刷盘 | — | — |
| **T4.0-7** | §9 未对齐访问 / 23:59:59 时钟跳变用例 | ✅ 已通过 T4.5 落地 | — | — |
| **T4.0-8** | §2/§10 离线解码工具 `logx_decode.py` | ✅ 已通过 D-01~D-05 闭环（三层错误策略对齐现行 C++；同日多分卷无损合并；块尾断裂 carry 丢弃；死代码清理；解压上限收紧；顺带修复 GCM 缺参） | — | D-01~D-05 |
| **T4.0-9** | §7/§10 Kotlin Facade `LogX.kt` | ✅ 已通过 K-01/K-02 闭环（export 状态码如实透传 + 结构化上下文 runBlocking(parentContext) 防死锁 + 异常如实传播） | — | K-01/K-02 |
| **T4.0-10** | §7/§10 JNI Config 传递与 ErrorHandler | ✅ 全部闭环：Compress 映射语义混淆随 C-02（歧义常量删除 + helper 全栈命名拉齐）；全局错误上下文单例化随 J-02（错误上下文按实例持有，`g_err_ctx` 移除） | — | C-02/J-02 |
| **T4.0-11** | §4.3/§4.4 恢复后切分序号强制归 0 风险 | ✅ 已通过 F-02 落地（严格解析文件名 + 探测安全网） | — | — |
| **T4.0-12** | §10 新建文件父目录 fsync | ✅ 已通过 F-04 补齐：新建日志文件头落盘后执行文件与父目录 `file_dir_fsync` 双同步 | — | — |

> 注：T4.0 十二项偏差已全部闭环销号（T4.0-10 的两项分属 C-02 与 J-02，分别于编解码批次与 JNI 批次闭环）。

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
| E-01 | 错误回调注册数据竞争（UB）[✅ 已修复] | 报告一 BUG-4 / 报告二#5 |
| P-01 | Win64 HANDLE → int 截断（报告一列 P0） [✅ 已修复] | 报告一 BUG-5 / 报告二#10（报告二列中等） |
| T-01 | "强杀恢复"测试未真正模拟强杀（恢复路径零覆盖） [✅ 已修复] | 报告二测#1 |

### 🟠 P1 —— 功能 / 并发 / 协议 / 容错缺陷

| 编号 | 问题 | 编号 | 问题 |
|:---|:---|:---|:---|
| F-03 | 短写未检测致日志丢失 [✅ 已修复] | F-04 | fsync/同步持久化零接入 [✅ 已修复] |
| F-05 | 恢复块头校验不足 [✅ 已修复] | F-06 | atc offset uint32 截断 [✅ 已修复] |
| F-07 | split_no uint8 / 255 停写（T4.0-2） [✅ 已修复] | C-02 | 压缩/加密 flag 命名歧义 [✅ 已修复] |
| C-03 | decode_block 忽略 raw/raw_len [✅ 已修复] | S-02 | AppendTargetCache 非 packed [✅ 已解决] |
| FM-01 | tag[64] 越界读 UB [✅ 已修复] | FM-02 | 重同步未清 carry_slice_buf [✅ 已修复] |
| R-01 | was_truncated 误传播 [✅ 已修复] | R-02 | 丢块统计少计 1 [✅ 已复核校准：报告不成立，实现正确，已固化] |
| R-03 | 日期校验不完整 / 多套解析不一致 [✅ 已修复] | E-02 | drop_too_large 统计与错误分类 [✅ 已修复] |
| E-03 | 导出错误被吞 [✅ 已修复] | E-04 | export 临时文件命名冲突 [✅ 已修复] |
| E-05 | Windows 路径分隔符 [✅ 已修复] | E-06 | flush_threshold==0 语义 [✅ 已修复] |
| E-07 | logx_stats_t ABI 兼容 [✅ 已修复] | P-02 | file_size_path 开销 [✅ 已修复] |
| P-03 | file_rmdir 错误转译 [✅ 已修复] | J-02 | g_err_ctx 多实例互踩 [✅ 已修复] |
| K-01 | Kotlin export 丢返回值 [✅ 已修复] | K-02 | runBlocking 嵌套 [✅ 已修复] |
| D-01 | Python cksum break 丢记录 [✅ 已修复] | D-02 | Python 坏块 break 丢文件 [✅ 已修复] |
| D-03 | GCM 失败策略不一致 [✅ 已修复] | F-10 | 缺少 50~100ms Debounce 聚集刷盘 [✅ 已修复] |
| T-02~T-11 | 测试用例缺陷（详见第十一节）[✅ 已全部修复] | — | — |

### 🟡 P2 —— 质量 / 性能 / 可维护性

| 编号 | 问题 | 编号 | 问题 |
|:---|:---|:---|:---|
| S-03 | kBlockHeaderSize 注释 32B→34B [✅ 已修复] | S-04 | max_log_length 默认值注释 [✅ 已修复] |
| S-05 | staging 指针未置 null [✅ 已修复] | FM-03 | 64KB 栈数组溢出风险 [✅ 已修复] |
| FM-04 | cksum O(n²) 重扫 [✅ 已修复] | F-08 | flush_blocking 无超时 [✅ 已修复] |
| F-09 | random_bytes 退化低熵 [✅ 已修复] | P-04 | QPF 重复查询 [✅ 已修复] |
| P-05 | SetFilePointer 冗余 [✅ 已修复] | P-06 | atomic_add 对齐断言 [✅ 已修复] |
| J-03 | NewStringUTF 编码风险 [✅ 已修复] | K-03 | errorHandler GC 延迟 [✅ 已修复] |
| K-04 | setErrorHandler 并发校验 [✅ 已修复] | D-04 | Python 死代码 [✅ 已修复] |
| D-05 | Python 64MB 解压上限 [✅ 已修复] | C-04 | 缺少 ARMv8 硬件加速（T4.0-5）[✅ 已修复] |
| S-06 | 双 Header 提交内存序偏离（T4.0-3） | — | — |

### 修复顺序建议（综合三份报告）

1. **P0 核心数据正确性与崩溃恢复**：[✅ 文件头加密标志（F-01）] → [✅ 压缩降级 flag（C-01）] → [✅ 分卷号恢复（F-02）] → [✅ 真实崩溃恢复测试（T-01）]
2. **P0 并发安全与内存生命周期**：[✅ CRC 表 constexpr 编译期无竞争（S-01）] → [✅ 错误回调保护（E-01）] → [✅ JNI 关闭顺序与 UAF 防护（J-01）] → [✅ Win64 句柄截断修复（P-01）]
3. **P1 协议健壮性与持久化**：[✅ 短写检测与撕裂回滚（F-03）] → [✅ 持久化同步接入（F-04）] → [✅ 恢复校验加强（F-05）] → [✅ 分卷序号 uint16_t 与上限告警（F-07）] → [✅ 50~100ms Debounce 聚集刷盘（F-10）]
4. **P2 API、测试质量与工具链**：tag 边界、~~日期校验~~ [✅ R-03 已闭环]、统计分类、导出错误传播、Python 解码器容错、测试命名与恒真断言清理、故障注入路径断言补全
5. **覆盖率补齐**：优先补"压缩降级往返"与"多分卷恢复后再轮转"两个负向用例（正是它们缺席掩盖了 C-01 与 F-02）
