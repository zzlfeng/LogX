# LogX 日志库最终实施方案

> 基于 mmap 的 Android 高性能崩溃安全日志库设计方案
> 技术路线对标:xlog(微信 mars)/ MMKV / Logan(美团)的核心机制,并做了架构简化
> 决策日期:2026-09-02(v5 评审优化定稿,修订记录见 §13)

> ## ⚠️ 实现状态标注(2026-09-03 深度代码级复核与 M4 阶段演进)
>
> 本文档是**设计规范**。复核与实测结论:
> 1. M1~M3 C++ 核心主干与加解密已全面闭环；
> 2. **M4 生产加固已全量落地**: 包含 T4.0B 8 处覆盖缺口补齐与块级 CRC32 读路径校验、T4.1 堆内存降级、T4.2 错误监控打点、T4.3 保留期全时机自动清理、T4.4 50 线程极限并发压测与零死锁对账、T4.5 硬件未对齐访问校验/时钟跳变仿真/50 轮崩溃注入自愈循环(零丢失)；单测套件扩充至 **59/59 100% PASS**，压测套件 4/4 100% PASS；**Windows (MSVC) 与 Android 真机 (Redmi K30 Pro, arm64-v8a, Android 12) 100% 双端全绿跑通**；
> 3. 下列项目主要涉及跨端 JNI/Kotlin 配置补齐与配套周边工具重构，归口任务 **T4.0**：
>
> | spec 条目 | 代码现状(2026-09-03) | 状态 |
> |---|---|---|
> | §7 `flush()` 水位线 `target_seqno` 防饥饿 | 自旋等待 staging 全空 + 5ms sleep | T4.0-1 |
> | §3.1/§4.4 切分序号 2B (uint16_t) | 仍为 uint8_t, 255 上限风险在 | T4.0-2 |
> | §4.2/§10 双 Header "写 A→seq_cst 屏障→写 B" | A→B→release fence, 顺序与内存序不一致 | T4.0-3 |
> | §4.1 Flusher 50~100ms Debounce | 无超时阻塞 ev_wait | T4.0-4 |
> | §10 ARMv8 Crypto Extensions 加速 | 未实现(纯 mbedtls 软算) | T4.0-5 |
> | §11 运行期每 30s 定时 msync/fdatasync | 30s 定时未实现; 且全库 fsync/msync 调用为 0 | T4.0-6 |
> | §9 未对齐访问 / 23:59:59 时钟跳变用例 | **✅ 100% 已实现并测试通过** (`test_robustness.cpp`, 单测增至 59/59) | T4.5 (已销号) |
> | §2/§10 离线解码工具 `logx_decode.py` | 状态机缺陷(is_whole 恒返回 None)+误判单块单日志 | T4.0-8 |
> | §7/§10 Kotlin Facade `LogX.kt` | 语法截断错误(漏 if 致必抛异常)+缺失 Gradle 工程 | T4.0-9 |
> | §7/§10 JNI Config 传递与 ErrorHandler | JNI 硬编码写死配置; Kotlin ErrorHandler 虚置未接线 | T4.0-10 |
> | §4.3/§4.4 恢复后切分序号倒退风险 | 恢复时 split 强制重置为 0, 再切分倒退覆写历史 .1 文件 | T4.0-11 |
> | §10 新建文件父目录 fsync | PAL 接口已实现但全库业务调用为 0 | T4.0-12 |
>
> 其余核心章节(staging 环形设计、对账回推、AES-GCM、JNI 零堆写入路径等)
> 与代码一致, 无需折扣阅读。

## 1. 决策摘要(已锁定)

| 决策项 | 结论 |
|---|---|
| 架构 | 环形 mmap staging(128KB,清零回收)+ 分块压缩加密 + 直接 `write()` 落盘 + seqno 对账恢复(k 块回溯) |
| 并发 | 全局轻量 mutex(临界区=一次 memcpy)+ 单 Flusher 线程 |
| 平台 | 第一期以 Android 为主，最低支持 Android 5.0 (API Level 21)；核心全在 C++（经 PAL 跨平台抽象隔离），C ABI 边界为后续 iOS/Flutter 预留 |
| 压缩 | zstd level 3(块格式 flag 预留 zlib/none) |
| 加密 | AES-256-GCM(ARM64 硬件指令加速 / mbedtls 兜底),init 时由调用方注入密钥,KID 支持换钥,可整体开关 |
| 上报 | 库只提供 `export()` 解密导出,上报由业务侧接入 |
| 原则 | 宁丢不脏;任何校验失败丢弃并打点,绝不解析脏数据 |

### 设计目标(量化验收标准)

| 目标 | 量化标准 |
|---|---|
| 崩溃不丢日志 | kill -9 下丢失为 0(仅最后正在写的 1 条可能截断,可接受) |
| 写入开销 | 单条日志路径只有格式化(含 1B 校验和)+ 一次 memcpy,JNI 零堆内存分配,无 syscall,主线程无感 |
| CPU 平滑 | 单次压缩/加密的数据量有上限(常规块 raw ≤16KB;单条超长记录独占一块,≤20KB+6B 记录头) |
| 安全 | 正式日志文件落盘为密文;staging 文件为应用私有目录(filesDir)内明文(与 xlog 同),日志目录与 staging 一并排除出自动备份(§7/§10) |
| 任何时点可恢复 | 断电/崩溃/被杀,重启后恢复到最后一个完整块,绝不读出脏数据;kill -9 类进程死亡零丢失、零重复;断电丢失窗口 ≤30s,断电窗口内极端情况可能出现重复块(见 §11) |
| 容量可控 | 按天分文件、单文件上限、保留 N 天自动清理 |
| 可运维 | 提供导出/上报接口和离线解码工具,关键异常有监控打点,降级态显式暴露 |

### 从三个参考库提炼的核心取舍

| 决策点 | xlog | MMKV | Logan | 本方案采用 |
|---|---|---|---|---|
| mmap 里放什么 | 原始明文,攒满整块压缩 | protobuf 记录流 | 每条流式压缩加密后的密文 | 原始 staging 区 + 独立压缩加密的块 |
| 撕裂写防护 | 块级 magic+seqno+CRC 断点续写 | 头部 CRC+actualSize 提交边界 | mmap 残留整体转存 | 块级 CRC + seqno 对账(k 块回溯) |
| 落盘方式 | block 先存 mmap 文件,攒满再转正式文件 | —(单文件即终点) | mmap 攒够写正式文件 | block 生成后直接 write() 落正式文件 |
| CPU 峰值控制 | 128KB 批量压 | 不压 | 逐条流式 | 按 8~16KB 分块批压 |
| 容量策略 | 环形覆盖 | 排重重整+倍增扩容 | 按天+10MB 上限 | 按天轮换+上限+保留期清理 |
| 多进程 | 要求每进程独立目录 | 原生 IPC 锁 | 不支持 | 框架层自动按进程后缀隔离目录 (Safe-by-Default) |
| 失败降级 | — | CRC 失败默认丢弃重建 | mmap 失败退堆内存 | 堆缓冲降级 + 宁丢不脏原则 |

**关键架构简化的理由**:`write()` 写入的数据进 page cache,同样由内核持有、进程死了照常回写。mmap 的真正价值不是"唯一安全的通道",而是**把 N 条日志聚合成 1 次 syscall**。所以本方案让 mmap 只承担一个角色:装还没凑满一块的原始日志(staging);一旦凑满一块,压缩加密后直接 `write()` 进正式日志文件。相比 xlog 的双文件设计,mmap 文件固定小、格式简单,省掉了"mmap 文件里的 block 转存"逻辑。

**为什么 staging 用环形缓冲而不是"线性 + memmove 压缩"**:memmove 方案存在恢复算法无法建模的崩溃窗口——数据已搬移、Header 未更新时,Header 坐标与物理布局脱节,恢复扫描会读到错位副本,结果在"重复补写"与"整段待刷数据丢失"之间摇摆,与"零丢失、零重复"的验收标准冲突。环形缓冲的回收只做**清零、不搬移**,坐标永不失配;清零后的区域天然是扫描终止符,"除 [flushed_len, write_pos) 外全环为零"成为可依赖的全局不变量。这也是 xlog mmap buffer 的同款选择。

## 2. 工程结构

```
logx/
├── core/                        # 纯 C++,零平台依赖,静态库
│   ├── include/logx_c.h         # C ABI:logx_open / write / flush / export / close
│   ├── src/
│   │   ├── staging.cpp          # mmap 双 Header + 环形缓冲(append-only,清零回收,相对坐标系)
│   │   ├── codec.cpp            # zstd + AES-GCM(ARMv8 Crypto 加速 / mbedtls 兜底)
│   │   ├── flusher.cpp          # 消费线程、延时合并(Debounce)、块写入、按天轮换、保留期清理
│   │   ├── recovery.cpp         # seqno 对账四分支 + k 块回溯、环形扫描
│   │   ├── format.cpp           # 时间戳/pid/tid/级别,TLS scratch 原地拼装,>20KB 分片(flags 标记)
│   │   └── pal_posix.cpp        # mmap/msync/fdatasync/mutex/thread
│   └── CMakeLists.txt
├── android/
│   ├── cpp/logx_jni.cpp         # 薄胶水,GetStringUTFRegion 零堆分配
│   └── .../LogX.kt              # Kotlin Facade,协程 export,自动进程名隔离
├── decoder/logx_decode.py       # 离线解码工具(必备,接受 KID→key 映射;解码时跳过块内 LGXP 记录与块尾 <6B 零残留)
└── tests/
    ├── unit/                    # gtest:格式编解码、分片重组(跨块)、对账四分支、环形扫描、空环/满环边界、回绕缝残留(pad<6)、撕裂校验和、未对齐访问
    └── stress/                  # kill -9 注入、多线程压测、时钟回拨与跨天跳变
```

依赖:mbedtls(~300KB 可裁剪,优先使用 ARMv8 硬件指令)、zstd 单文件版(~100KB)。若想先轻后重,M1/M2 可用"flag=none + crc32"跑通,M3 再引入两者——文件格式不变。

## 3. 数据格式(字节级)

### 3.1 环形 mmap staging 文件(128KB,filesDir 下;禁用 cacheDir,理由见 §10)

```
[0, 64)  Header A   [64, 128)  Header B   [128, 128KB)  环形 staging 区 (大小 kDataSize = 130944B)
```

**坐标系规范**:Header 与内存中的 `flushed_len` / `write_pos` 统一为**环内相对偏移**（取值范围 `[0, kDataSize)`，其中 `kDataSize = 128 * 1024 - 128 = 130944` 字节）。物理内存指针统一通过 `pos_ptr(pos) = mmap_base + 128 + pos` 换算。

Header(两份互备,写时先 A 后 B 并插入内存屏障,读时取"CRC 对且 seqno 新"者;定长 64B):

| 字段 | 大小 | 说明 |
|---|---|---|
| magic | 4B | `LGXS` |
| version | 2B | 格式版本 |
| flags | 2B | 加密开关等 |
| seqno | 4B | **已落盘块数(提交水位)** |
| flushed_len | 4B | 已提交消费的环形相对偏移 `[0, kDataSize)` (下一待刷字节) |
| write_pos | 4B | 诊断用(最近一次提交时刻的写指针,相对偏移 `[0, kDataSize)`) |
| reserved | 40B | 其中 14B 为追加目标缓存(文件日期 4B + 切分序号 2B + 最近提交块起/止偏移各 4B,见 §4.3),其余 26B 预留 |
| header_crc | 4B | 覆盖前 60B |

(4+2+2+4+4+4+40+4 = 64 ✓)

环形区纪律:append-only,写满回绕;**回收 = 清零,永不搬移**。数据区**在会计上保留 1 字节空隙(不存在固定的物理保留字节)**:空环(flushed==write)时 free = 大小−1 而非 0(否则一次全量 flush 后所有写入将被永久丢弃,见 §5),活区占用恒 ≤ 大小−1,空/满不歧义,环形扫描整圈必遇零。回绕处垫 PAD 占满至环尾;**尾部剩余 pad < 6B(最小记录头)时无记录可写,不垫 PAD、留零回绕**——该 1~5B 残留由不变量保证为零,切块/恢复扫描/解码按"距段尾不足 6B 即无条件跳过"处理,并计入所在块 raw_len(§3.2/§4.2/§4.3)。由此得到全局不变量:任意时刻全环除 `[flushed_len, write_pos)` 外全为零——恢复扫描从 flushed_len 出发、遇 PAD 跳过、遇 **6B 头全零**(终止符;len=0 的 PAD 头含 magic,不是终止符)或校验失败即停,永远不会读到旧圈残影。

staging 内记录统一格式(条目与 PAD),头部 6B(所有多字节读写均使用安全未对齐加载，防 SIGBUS):

```
[len:2B][magic:2B][flags:1B][cksum:1B][payload]
```

- len:payload 字节数,≤ 20KB(超长日志先分片);len 是撕裂扫描的步进依据;最小记录 6B(len=0),**len=0 仅为 PAD 合法**——日志条目 payload 恒非空(时间戳/pid/tid 保底,格式化器拒绝产出空记录),len=0 的 LGXE 视为损坏即停
- magic:`LGXE` 日志条目 / `LGXP` PAD(回绕填充,可为**连续多条**、每条 payload ≤ 20KB,合计占满至环尾——pad < 6B 时留零回绕、不垫 PAD;拷出与解码时按记录跳过)
- flags:bit0–1 = 分片类型(00 整条 / 01 首片 / 10 中片 / 11 末片),其余预留
- cksum:payload 的 8 位校验和(XOR 折叠或 CRC-8),**锁外随格式化在 TLS Scratch Buffer 一并计算**,热路径只多一次顺序读;PAD payload 全零,cksum 恒为 0。它是撕裂扫描的守门——仅靠 magic/len 无法识别"头已写完、payload 截断"的记录,cksum 失败即停;漏检率约 1/256,只作撕裂守门,不承担传输完整性
- 同一条日志的全部分片在**单次临界区内连续写入**,逻辑上必然相邻,分片运行中不会出现其他条目;解码按"连续分片段"重组,**允许跨块**、跨 PAD 仍视为连续;首片到末片不完整即整条丢弃 + 打点
- 单条日志(格式化后含全部分片与记录头)上限 64KB(Config.maxLogLength),超出**截断保留前缀**后照常落环:msg 压缩为"原始前缀 + ASCII marker(...[LogX truncated, dropped N bytes])"并经预算求解保证落环总长 ≤ 上限(预算不足 48B 时弃 marker 保前缀;连空记录都放不下的病态小上限仍整条丢弃 + 打点兜底);截断经 errorHandler(独立事件种类 **LOGX_EV_TRUNCATED**,code 取 LOGX_OK——有损写入成功非丢弃,"log truncated (dropped N bytes)")同步打点并计入 truncated_logs 计数(B-1B 方案B + B-3 方案B,2026-09-13);病态小上限丢弃仍走 LOGX_EV_DROP / LOGX_ERR_TOO_LARGE;上限必须小于数据区大小 − 1,否则永远放不进环

### 3.2 正式日志文件 `prefix_YYYYMMDD.log`(单文件超 10MB 切 `.1`, `.2` 等)

文件头 32B:`[magic LGXF:4B][ver:1B][flag:1B][date:4B][KID:2B][盐:8B][reserved:8B][头CRC:4B]`,CRC 覆盖前 28B。**盐在每次创建文件时随机生成**(非派生、非固定),与 seqno 共同保证 nonce 唯一。

**文件头 flag 的加密位语义 = 本文件的加密能力，创建时一次性锁定（R21 F-5）**:
- 新建文件时探测一次 `keyProvider(kid)`:取到密钥 → 置位;取不到 → **不置位**并发 `LOGX_EV_DEGRADED`("key unavailable at file creation; this file stays plaintext until rollover")。续写已存在文件时**回读该文件头的加密位**作为能力，不得改口。
- 锁定后，本文件生命周期内（直至按天/按大小轮换）**所有块的实际形态必须与该位一致**:头未置位 ⇒ 即便密钥稍后恢复可用，本文件后续块也一律明文落盘（`drain_once` 逐块以 `encrypt_enable && 本文件能力 && 有效密钥` 组装 flag）。续写已存在加密文件的临时缺钥降级不改变能力位：缺钥当块明文落盘，此后每轮 drain 至多重试取钥一次（不逐块重试），恢复后立即锁定并恢复密文落盘——降级状态不跨块静默泄漏，也不泄漏到后续文件。
- 由此建立不变量:**`encrypt_enable` 开启期间，"文件头声明 == 本文件全部块形态"**。读端（C++ 导出 / `scan_one` / Python decoder）仍一律按**块头** flag 逐块判定，文件头该位属元数据（供外部工具快速分类），不参与解密决策——这是历史文件（头声明加密但含明文降级块）仍可逐块导出的原因（R19 F-6 修复②）。
- 加密能力探测是 `keyProvider` 的第三类调用点（另两类为 open 期 fail-fast 与 export 逐块解密），运行在 **Flusher 后台线程**，频率为"每文件一次"（续写缺钥降级态下每轮至多重试一次直至恢复）；JNI 实现须自行 attach/detach。契约详见 `logx_c.h`。

Block(自包含,流式排列):

```
[magic:4B LGXB][ver:1B][flag:1B]      flag: bit0 加密, bit1-2 压缩算法
[seqno:4B]        全局递增,跨天连续;同 (key,盐) 下永不复用、永不回退
[raw_len:4B]      块覆盖的 staging 字节跨度,即压缩加密前原始字节数(**含记录头、LGXP PAD 与回绕缝 1~5B 零残留**)—— 恢复对账的关键字段
[payload_len:4B]
[crc32:4B]        覆盖 payload(密文块由 GCM tag 兜底,此 CRC 仍保留,统一格式)
[nonce:12B]       = 8B 文件盐(存文件头)+ 4B seqno,保证 (key,nonce) 唯一
[payload]         压缩+加密后的数据
[GCM tag:16B]     仅加密块存在
```

**块由完整记录组成**(条目与 PAD 均不得被块边界切开,但同一条日志的分片可以分布在连续多个块中——>20KB 的日志必然如此):从拷出流头部沿记录边界累积,加入下一条记录会超限则封块;**回绕缝处距段尾 <6B 的零残留无条件跳过、归入当前块 raw_len 并在该处封块**(§3.1);常规块 raw_len ≤ 16KB,单条 16~20KB 的记录(含 PAD)独占一块。**块 raw_len 上限(R26 B-9 修订)**:`kBlockMaxRawWithSeam = kBlockMaxRaw + (kHeaderLen-1) = 20491B`——缝(≤5B 留零)并入块尾后块可达 20491B;单条记录本身的 raw 上限仍为 `kBlockMaxRaw = 20KB+6B = 20486B`(缝不是记录,只可能出现在块尾)。读侧接受域均已覆盖:块头 plausible 上界 16MB、zstd 解压上限 `kBlockMaxRaw+64=20550B`、Python 解码器 `MAX_BLOCK_RAW=20491`(R26 同步);块尾 <6B 留零由 `parse_records_stream` 原生 break 消费。因此 **raw_len 仍记录边界对齐**(缝残留随块封边)——恢复对账据此推进 flushed_len 后,扫描必然从记录边界开始,Σraw_len 与 flushed_len 的推进严格一致。解码按"连续分片段"跨块重组(块按序消费,段内允许出现 PAD 被跳过;块尾 <6B 零残留直接忽略)。

**缝处置的最终形态(R26 B-9 落地,R24 §7.2 采纳 A+B 组合)**:缝的产生点唯一——`write_records` 的 `pad<6` 留零回绕(几何上仅存在于环尾 `[data_size-k, data_size)`,`k∈[1,5]`)。处置分三条路径,共同保证 **1~5B 缝残片绝不作为独立块落盘**(读侧 `block_header_plausible` 要求 `raw_len ≥ kHeaderLen=6B`,微块一旦写入,导出会在块边界不可信处 break、repair 会物理截断其后全部数据)。**① 窗口头缝前置消费(方案B)**:`drain_once` 在 `copy_pending` 前于锁内检测 `data_size - f < kHeaderLen`——全零缝则清零区间、`f=0` 并提交 Header,缝自此不进入切块层(F1c 型微块根除);非全零(位翻转打脏)不吞,交路径②处置以保留腐坏可见性。**② 脏缝探测封微块**:切块层对 `rec==0 && probe_wrap_seam==0` 的位置(窗口头 R25 F-2 / 流中部 R26 B-9 内层对称探测)扫描 `[pos, pos+kHeaderLen)` 内是否存在合法记录头,命中则封 s 字节独立微块交 `drain_once` 拦截分支——全零属常态(只递增 `seam_blocks_skipped`,不发事件以免刷屏),非全零按「宁丢不脏」丢弃该残片并**发 `LOGX_EV_RECOVERY`** 明示。**③ 缝并入块尾(方案A)**:窗口尾与累计循环内的纯零缝直接并入当前块尾(`kBlockMaxRawWithSeam` 放宽),由读侧块尾 <6B break 语义消费。两条防线为纵深结构——单独撤销任一条仍不落盘,双撤销即复现微块落盘(R26 双撤销对照实验实证)。跨模块契约的门禁在两侧都有用例:写侧 `F1b`(缝并入块尾形态钉死)/`F1c`(前置消费,落盘层独立遍历块链)/`F9`(流中部脏缝不融合),读侧 `ReadSideAcceptanceDomain_RawLenBounds`(1~5B 逐值必拒、`== kHeaderLen` 必收)。

`raw_len` 必须保留——它让恢复逻辑在"块已写入但 header 未提交"的窗口里,能精确算出 staging 中哪段已落盘。

seqno 生命周期:全局递增,跨天连续;staging 作废重建时,新 seqno 基准 = 现有日志文件中的 max(seqno),**禁止归零**(归零会在同 (key,盐) 下造成 GCM nonce 重用)。**凡实际发生截断(恢复截尾、坏块截断),续写首块 seqno = max(S_hdr, S_file) + 2**——被弃块(含至多一个未提交在飞块)曾占用的序号一律跳过留空隙,堵住"截断后同 (key,盐) 复用 nonce"的漏洞;对账、定位与解码只依赖 seqno 单调与大小比较,空隙无副作用(§4.3)。未发生截断时续写 = max(S_hdr, S_file) + 1。

## 4. 核心流程

### 4.1 写入(业务线程)

```
JNI 边界(GetStringUTFRegion 到 TLS Buffer,零堆分配)
→ 格式化 + 记录校验和(锁外,TLS scratch)
→ 锁内写入环形 staging → 解锁
锁内:尾部放不下 → 垫 1..n 条 LGXP PAD 回绕到相对偏移 0(pad < 6B 时留零回绕、不垫 PAD,§3.1);
     空闲不足或单条超上限 → 丢弃 + 原子计数打点,绝不阻塞
staging 待刷 ≥ 8KB / 退后台 / 手动 flush → 唤醒 Flusher (Flusher 配合 50~100ms Debounce 超时聚合)
```

### 4.2 Flush(单一线程)

```
1. 锁内取 F = flushed_len、snap = write_pos(未满阈值时对整个 staging 做全量 drain);解锁
2. 锁外把 [F, snap) 的原始字节流拷入 Flusher 私有 scratch(≤ 数据区大小)
                                         # 跨回绕则两段;**保留 PAD 原样,不做剔除**;缝处携带 1~5B 零残留(§3.1);此区间不可变
3. 沿记录边界把 scratch 切成块(§3.2 规则;缝残留 <6B 无条件跳过、计入当前块 raw_len 并封块),对每块依次执行 4→6:
4.   压缩(zstd level 3) → AES-GCM(ARMv8 硬件加速) → 组块(seqno 递增)→ write() 进当天文件(循环处理短写) # page cache
5.   锁外清零该块覆盖的 staging 物理区间  # 生产者此间仍视其为占用,无竞争
6.   锁内写双 Header:
       - 写入 Header A (含 header_crc 计算)
       - 执行 std::atomic_thread_fence(std::memory_order_seq_cst)  # 内存屏障,保证 A 完全落盘可见
       - 写入 Header B (含 header_crc 计算)
       - Header 字段: seqno = 该块 seqno、flushed_len = 该块末尾、write_pos = 当前写指针,
         并更新追加目标缓存字段(§3.1)                            ← 提交边界(**逐块提交**)
       - 广播 committed_seqno 条件变量,唤醒等待特定水位的 flush() 调用方
```

顺序纪律:**先落数据(4),再清零(5),最后提交 Header(6)**,对每个块依次重复。清零必须先于提交——提交后生产者立即可以回绕进入该区间,顺序颠倒会与清零竞争。提交即承诺"该区间已清零",Header 的 seqno 就是唯一提交边界。逐块提交(每次 64B mmap 写,开销可忽略)把未提交窗口缩到单块,生产者也能更早回收空间;拷出流保留 PAD 与缝残留后,块 raw_len 恒等于其 staging 跨度,Σraw_len 与 flushed_len 的推进严格一致——这是 §4.3 回溯正确性的前提。

### 4.3 恢复(启动时,recovery.cpp)

```
读双 Header(取"CRC 对且 seqno 新"者)→ S_hdr、F、追加目标缓存(date/split/块起止偏移,§3.1)
定位追加目标文件:优先用缓存打开并校验所指块(seqno == S_hdr 即命中,O(1));
  缓存失效 → 先只扫文件名日期最新的文件(常态即命中),仅在时钟回拨造成多候选冲突时
  才按"文件名日期 + 尾块 seqno 最大"扩展到全部保留文件
  # 勿单靠 mtime:时钟回拨会选错文件,文件名日期回退还会产生同名冲突
确定 S_file:缓存命中 → 自缓存块止偏移向文件尾正向扫描剩余块(通常 0~1 个);
  缓存失效 → 自文件头正向扫描校验到最后有效块(慢路径,仅故障场景)
分支对账(k > 1 的来源:断电时 fdatasync 领先 msync、双 Header 回退;kill -9 未提交为 k = 1):
  S_file == S_hdr      → [F, 撕裂点) 未落盘,重新压块补写              # 常规情况
  S_file >  S_hdr      → 块已写入但 header 未提交:取 seqno ∈ (S_hdr, S_file] 的块累加 raw_len,
                         F = (F + Σraw_len) mod 数据区大小,seqno = S_file,剩余部分补写
                         # raw_len 已含 PAD 与缝残留(§3.2),Σraw_len 恒等于 staging 跨度,回溯精确
                         # 本分支跨过的区间可能尚未清零(崩溃落在 §4.2 第 4→5 步之间),由文末不变量收尾补清
                         # 双 Header A 撕裂、回退到落后的 B,同样落入本分支
  S_file <  S_hdr      → 追加目标尾部损坏(断电时为常态分支),截到最后有效块;
                         seqno 以 S_hdr 为下限续增(永不回退);丢失的已提交块数量打点
  文件缺失/长度异常     → 信任 header,重刷 staging
追加前:仅把追加目标文件截到最后一个有效块(历史文件轮换后封闭,不再打开校验);
  凡实际截断,续写首块 seqno = max(S_hdr, S_file) + 2(跳 1 空隙防 nonce 重用,§3.2;
  逐块提交保证未提交在飞块至多一个,+1 空隙即够);未截断则为 max + 1
环形扫描 staging:从 F 出发逐记录校验(len/magic/cksum),PAD 跳过,
  距环尾 <6B 的零残留跳过后回绕(§3.1),
  遇 6B 全零记录头、记录不完整或校验失败即停 → 有效末尾 W',并置 write_pos = W'
  撕裂尾巴丢弃 + 打点;任何失败 → 丢弃对应部分,宁丢不脏
不变量收尾(提交新 Header 前必须完成):分支对账跨过的区间与撕裂尾巴 [W', W_crash) 一律清零,
  使恢复出口处"除 [新 flushed, write_pos) 外全环为零"即刻成立——否则环将满且 free 区无零时,
  下次扫描会穿过校验和完好的陈旧记录重新压块,产生重复块(§5 不变量 5、§10 断言的前提)。
  重刷补写经 §4.2 流程会顺带清零 [F, W'),实际只需补清上述两段
双 Header 均损坏 / staging 文件缺失 → 整个 staging 作废重建(全环清零,天然满足不变量),
  新 seqno 基准 = 现有文件 max(seqno)(防 nonce 重用;仅此场景才全量扫描所有文件尾块)
```

追加目标缓存于每次提交时更新(§4.2 第 6 步);缓存校验失败一律回退全量扫描慢路径,只在故障场景出现。

### 4.4 容量与清理

按天文件 + 10MB 上限(切分决策只在 flush 轮次边界做出:一轮 drain 开始时选定目标文件,轮内所有块必属同一文件;切分序号 `.1`/`.2`… 为 **2B (uint16_t)**, 上限达 65535, 支持单日 655GB 超大容量, 彻底消除写满停写边界风险);启动及退后台时清理 `retentionDays`(默认 7 天)前文件;staging 文件固定 128KB 不参与清理。

## 5. 并发模型

**不变量(六条)**:

1. 条目原子性:一条日志(含全部分片)在缓冲区里连续,不交错,PAD 不打断分片的逻辑连续性
2. 写指针并发安全
3. 生产者与 Flusher 互不阻塞(回收清零在锁外)
4. 文件单写者(轮换/切分/清理全在 Flusher)
5. 环形不变量:任意时刻除 [flushed_len, write_pos) 外全环为零——回收只清零不搬移,坐标永不失配;数据区在会计上保留 1 字节空隙(无固定物理保留字节),活区占用恒 ≤ 大小−1(空/满不歧义,扫描整圈必遇零);恢复流程负责在出口处重建本不变量(§4.3 不变量收尾)
6. 崩溃撕裂窗口最小化:任意时刻被 kill,撕裂条目最多一条,且必然是最后一条

**生产者路径**(临界区 ≈ 一次几百字节 memcpy,约 50~200ns; 相对坐标系 `[0, kDataSize)`):

```cpp
// 内部统一使用环内相对偏移 [0, kDataSize)
// kDataSize = 128 * 1024 - 128 = 130944; 
// 物理地址辅助: char* pos_ptr(uint32_t rel_pos) { return mmap_base + 128 + rel_pos; }

void write_records(const char* recs, uint32_t total) {  // 一条日志的全部记录(含分片)一次写入
    std::lock_guard<std::mutex> lk(mu);
    // 会计上保留 1 字节空隙(无固定物理保留字节):空环(flushed==write)时 free = size−1 而非 0。
    // 若按 (flushed−write+size)%size 计算,全量 flush 后 free 恒为 0,所有写入被永久丢弃。
    uint32_t free = (flushed_pos - write_pos - 1 + kDataSize) % kDataSize;
    if (write_pos + total > kDataSize) {                // 尾部不足: 垫 PAD 后回绕
        uint32_t pad = kDataSize - write_pos;
        if (free < pad + total) { atomic_fetch_add(&drop_count, 1); return; }
        if (pad >= kMinRecordLen) {                     // 最小记录 6B(§3.1); pad < 6 时留零回绕
            write_pad(write_pos, pad);                  // 垫 1..n 条 [len][LGXP][cksum=0], 占满至环尾
        }   // pad ∈ [1,5]: 留零回绕; 消费侧按"距环尾 <6B 无条件跳过"处理(§3.1)
        write_pos = 0;                                  // 回绕到相对坐标 0
    } else if (total > free) {                          // 空闲不足: 丢弃 + 计数
        atomic_fetch_add(&drop_count, 1);
        return;
    }
    memcpy(pos_ptr(write_pos), recs, total);
    write_pos = (write_pos + total) % kDataSize;
}
```

**为什么不用无锁**:

- 原子预留 + 锁外拷贝:会产生"空洞"(线程 A 半条 + 线程 B 完整条),恢复扫描碰到半条就得停,B 的完整日志也被丢——全局锁下不存在空洞,这是决定性理由。
- mmap 无锁 MPSC 环 / TLS 每线程缓冲:工程量是 MMKV 级别,TLS 还有交接窗口丢数据问题;真实日志速率(每秒几万条)下收益体现不出来。
- 重新评估时机:simpleperf 实测锁争用成为热点,或单进程日志量到每秒几十万条以上。

**为什么不用 memmove 压缩**:见 §1。回收只有清零一条路,且在锁外完成(生产者此间视该区间为占用),不存在长临界区。

**边界情况**:staging 空闲不足则丢弃+打点;单条超上限(64KB,§3.1)截断保留前缀后照常落环(打点 truncated_logs,病态小上限仍丢弃+打点兜底,B-1B 方案B);格式化与 Flusher 路径禁止调用业务回调(防重入死锁);signal handler 内禁用日志接口(非异步信号安全);锁粒度=单实例,多进程按进程隔离目录,进程间共享文件永远禁止。

## 6. 降级矩阵

| 故障 | 策略 |
|---|---|
| mmap 失败 | 堆内存 staging + 每 5s/64KB 定时 write(),打点(实测概率约万分之一量级);**经 errorHandler(type=DEGRADED)显式暴露——此期间"崩溃零丢失"承诺失效** |
| zstd/mbedtls 初始化失败 | flag 降级为 none/明文继续写,同样经 errorHandler 暴露 |
| keyProvider 取不到当前 KID 密钥 | 新块降级明文(flag bit0=0)继续写,经 errorHandler(DEGRADED) 暴露;续写已存在加密文件时此后每轮至多重试取钥一次,恢复即恢复密文落盘;历史文件不受影响 |
| 磁盘满 | 先清过期日志;仍不足停写+打点,不向业务抛异常 |
| 块 CRC/GCM 失败 | 从损坏点截断续写(截到最后有效块);截断后续写 seqno 跳 1 空隙(§3.2) |
| 断电丢尾部(S_file < S_hdr) | 截到最后有效块续写;丢失的已提交块数量打点,窗口 ≤30s(§11) |
| 双 Header 均损坏 | 整个 staging 作废重建,已落盘文件不受影响;seqno 从文件 max(seqno) 续接 |
| 检测到多进程共用目录 | 框架层自动按进程名后缀分流(Safe-by-Default);若手动强配相同目录则文件锁探测拦截并打点告警 |

## 7. Kotlin API(最终)

```kotlin
LogX.init(context, LogX.Config(
    logDir = File(context.filesDir, "logs"),              // 框架内部自动根据进程名加后缀 (主进程除外, 如 :push -> logs_push)
    stagingDir = File(context.filesDir, "logx_staging"), // 必须 filesDir 系; 内部自动隔离 (如 staging_push.mmap); 禁用 cacheDir
    level = LogLevel.INFO,
    retentionDays = 7,
    maxFileSize = 10 * 1024 * 1024,
    maxLogLength = 64 * 1024,             // 单条上限(格式化后); init 校验: 必须 < staging 数据区 − 1, 否则 init 抛错; 推荐 ≤ 数据区/2; 运行期超出截断保留前缀+打点(病态小上限仍丢弃,§3.1 B-1B)
    flushThreshold = 8 * 1024,
    compress = Compress.ZSTD,             // ZSTD / ZLIB / NONE
    keyProvider = { kid -> keyStore.getKey(kid) },  // (kid)->32B 裸密钥; 须能应答历史 KID; null=该 KID 不可解
    kid = 1,                              // 当前写入使用的 KID
))
LogX.i("Net", "cost=${ms}ms")
LogX.flush()                              // 同步: 抓取当前写入水位 (target_seqno), 阻塞等待至该水位落盘提交即返回 (防高并发饥饿)
suspend fun LogX.export(from: String, to: String, block: suspend (File) -> Unit) 
// from/to = 日期 (yyyyMMdd, 闭区间); 内部先 flush, 协程执行完毕或异常退出后自动清理明文临时文件
LogX.registerErrorHandler { type, msg -> ... }   // DEGRADED / DROP / IO 等
```

**多进程默认隔离 (Safe-by-Default)**:
业务接入在 `Application.onCreate` 时常忘记区分子进程。LogX 在 `LogX.init()` 内部自动调用系统接口获取当前进程名：若为主进程，使用指定目录；若为 `:push`、`:channel` 等子进程，自动将目录变更为 `logs_push`、`staging_push.mmap`，彻底杜绝多进程 mmap 冲突。

**密钥管理约定**:LogX 需要 32B 裸密钥做 AES-GCM,而 AndroidKeyStore 密钥**不可导出**。标准做法:业务侧生成随机 32B 软件密钥,用 Keystore 包裹(Keystore AES 加密后落盘),init 时解出裸密钥经 keyProvider 交给 LogX;LogX 本身不管理密钥存储,保持无状态。

**KID 轮换**:换钥 = kid+1 继续写,新块用新 KID;读取旧文件按文件头 KID 向 keyProvider 索取历史密钥。**保留期内历史密钥必须可得**,否则该期间日志不可解;decoder 同样接受 KID→key 映射。

**调用时序约定**:init 之前调用任何写接口一律丢弃 + 原子计数(不做 pre-init 缓冲重放;Android 进程在 Application.onCreate 即 init,窗口极小)。keyProvider 会在 init(恢复解密)与 export(历史 KID 解密)路径被调用,允许阻塞(Keystore 解包)但不得重入 LogX。

**flush 水位控制**:调用 `flush()` 时，锁内记录当前目标提交水位 `target_seqno = current_alloc_seqno`，随后在条件变量上等待直到 `committed_seqno >= target_seqno`。即使业务线程在此期间持续写入新日志，本次 `flush()` 也仅保证进入时的日志落盘即准时返回，避免无限期挂起。

**export 语义**:先 flush,再以调用时刻各日志文件的长度为快照止点导出(期间 Flusher 继续写入的增量不包含在本次导出);明文临时文件写在 filesDir 下独立临时目录(禁外部存储、禁 cacheDir),采用 Kotlin `suspend` 协程作用域管理生命周期，待上传回调返回或抛出后立即删除。

**其余 API 语义**:`keyProvider` 返回 null(该 KID 不可解)只影响 export——对应文件跳过、计数打点并经 errorHandler 暴露,不中断其余文件(恢复对账只读块头明文字段与 CRC32,不需要密钥);重复调用 `init` 视为编程错误,第二次调用直接抛 `IllegalStateException`,不支持热替换。

## 8. 里程碑与验收(生产级标准)

**M1 · 核心写入链路(第 1–2 周)**
环形双 Header staging、格式化与分片、TLS 零分配、锁、Flusher (Debounce 聚合)、明文块、按天文件、轮换。
验收:单条写入 < 2µs(native 路径,不含 JNI/Kotlin 边界);kill -9 注入 1000 次零丢失(除撕裂尾条)、零重复。

**M2 · 恢复与对账(第 2 周)**
对账四分支(k 块回溯)、双 Header 容错与内存屏障、启动恢复、环形扫描(PAD/撕裂/零界)、打点埋点。
验收:对"块写入前/后、Header 提交前/后、清零中"等崩溃窗口逐一注入通过;对账逻辑 gtest 全覆盖。

**M3 · 压缩与加密(第 3 周)**
zstd、ARMv8 硬件加速 AES-GCM (mbedtls 兜底)、nonce 派生(8B 每文件随机盐 + seqno)、KID 与历史密钥、明文/密文块共存、`export()` (协程生命周期)、python 解码工具。
验收:开关切换兼容旧文件;nonce 唯一性单测(含 staging 重建、文件重建场景);解码工具往返一致。

**M4 · 生产加固(第 4 周)**
降级矩阵全部实现、多进程自动隔离、保留期清理、大日志分片与重组、错误回调、未对齐内存访问测试。
验收:故障注入矩阵全过;50 线程压测无 fd/内存泄漏;72h monkey 稳定。

**M5 · 文档收尾**
接入文档、API 参考、密钥管理指引、解码工具说明、性能基线报告(延迟分布/吞吐/CPU/so 体积)。

## 9. 测试矩阵(必做项)

| 类别 | 方法 | 通过标准 |
|---|---|---|
| 崩溃注入 | 压测中每写 N 条 kill -9,重启校验 | 零丢失、零重复、零脏数据,循环 1000 次 |
| 撕裂注入 | hook write/memcpy 中途 SIGKILL | 恢复算法正确,损失 ≤1 条 |
| 窗口注入 | 定点 hook:清零中/提交中/回绕中崩溃 | 对账四分支全部按预期收敛,零重复 |
| 并发压测 | 50 线程随机级别打点 + 随机 kill + 同步 flush 竞争 | 无交错乱码,无死锁,flush 无饥饿挂死 |
| 超长分片 | 多线程交错写 >20KB 日志 + 中途崩溃 | 分片重组正确,残片整条丢弃并打点 |
| 时钟跳变与跨天 | 写入中途回拨/前跳系统时间后重启(含 23:59:59 回拨至 00:00:01、回拨致文件名同名冲突) | 按文件名日期+尾块 seqno 定位,不丢不脏,不产生坏文件 |
| 未对齐访问 | 32 位 ARM / ASan 开启下对非 4 字节对齐数据进行封块与解包 | 零 SIGBUS 崩溃,正确读取 |
| 断电注入 | QEMU/模拟器快照掉电或真机电池拔除,压测中随机时点断电后重启 | 不读出脏数据;丢失窗口 ≤30s;重复块仅允许出现在断电窗口内且打点可见(kill -9 的零重复标准不适用于本行,见 §11) |
| 降级注入 | 调试开关/hook 强制 mmap 失败、zstd/mbedtls 初始化失败 | errorHandler(DEGRADED) 触发,堆缓冲路径继续写;kill -9 允许丢失但无脏数据、无崩溃 |
| 外部破坏 | staging 随机字节破坏、日志文件被外部 truncate、staging 文件删除 | 对应恢复分支收敛:作废重建/信任 header 重刷,不崩溃不挂死 |
| 兼容性 | API 21–35,arm64/armv7/x86_64 | 全通过,32 位地址空间无异常 |
| 性能基线 | 延迟分布、开关压缩吞吐、CPU 占用、JNI 堆分配监控 | 建立回归基线,JNI 路径 0 GC 发生 |

## 10. 实现纪律清单(代码评审对照用)

- 必须 `MAP_SHARED`,`MAP_PRIVATE` 是 COW 匿名页,进程死数据丢
- mmap 前 `ftruncate` 预分配完整大小(全零初始化,是环形扫描"零=终止符"的前提),否则越界写 SIGBUS;文件不可被其他进程 truncate
- fd 加 `O_CLOEXEC`;日志文件追加用 `O_APPEND`;`write()` 必须循环处理短写;close 前 `munmap`(先 msync)
- 恢复后、追加前,把**追加目标文件**截到最后一个有效块(仅此一个;历史文件轮换后封闭,不再打开校验)
- 32 位设备注意虚拟地址空间(128KB 固定映射无风险,勿盲目加大)
- 结构体多字节字段读写一律使用 `memcpy` 或安全未对齐加载宏，防 32 位 ARM SIGBUS
- 信号处理器里禁止调用日志接口(非 async-signal-safe),崩溃堆栈走独立 crash SDK
- 多进程 app 默认由框架按进程名后缀自动隔离目录,永远不要两个进程映射同一日志文件
- JNI 字符串传递使用 `GetStringUTFRegion` 配合 TLS Scratch Buffer, 避免堆内存分配与 GC 压力
- ARM64 构建开启 ARMv8 Crypto Extensions 硬件加速指令集支持
- 双 Header 提交在写完 A 后必须插入 `std::atomic_thread_fence(std::memory_order_seq_cst)` 内存屏障再写 B
- 不用 Java `FileChannel.map`(unmap 不可控),核心全在 native
- mmap 文件放内部存储,别放外部存储(xlog 官方注释里的 SIGBUS 坑);staging 放 filesDir,**禁用 cacheDir**——低存储时系统可能清空 cache 目录,进程存活期间文件被删会让 mmap 变成孤儿 inode、写入静默丢失(iOS 同理,见 §12)
- 刷盘:staging mmap 用 `msync(MS_SYNC)`,`write()` 的日志文件用 `fdatasync(fd)`——msync 对非映射文件无效;均只在退后台/定时 30s 做;`MS_ASYNC` 对断电无保证,不采用
- 新建日志文件(跨天/切分)后对父目录 fsync 一次:fdatasync 不保证目录项持久,否则断电可能丢掉整个新文件
- debug 构建在 Header 提交点、恢复扫描点与恢复出口(不变量收尾后)断言环形不变量(除 [flushed_len, write_pos) 外全环为零),纳入 CI
- 回绕缝残留(<6B)必须四处同规则:生产留零(§4.1)、切块跳过并计入 raw_len(§4.2)、恢复扫描跳过后回绕(§4.3)、解码容忍块尾零残留(decoder),配固定 gtest 用例
- Android `allowBackup`/`fullBackupContent`(API 31+ 为 `dataExtractionRules`)必须排除日志目录与 staging 文件,防止云备份带走明文 staging 与密文日志
- JNI 只做转发不持逻辑;平台壳零逻辑(C ABI 纪律,为 iOS 预留)
- **平台最低兼容基线 (Android API 21 / 5.0)**: 全库（Native C++ 内核、PAL 抽象层、JNI 桥接层、Kotlin Facade）必须严格保证向后兼容至 Android API 21 (Android 5.0 Lollipop)。后续所有代码编写与系统 API 引入均以此为硬性标准；禁止直接使用未做 `Build.VERSION.SDK_INT` 降级保护的高版本特性（如 API 28+ 的 `Application.getProcessName`、API 31+ 的 `dataExtractionRules` 均已做多版本分流适配），严禁破坏 API 21 运行兼容性。

## 11. 耐久性分层说明

- **进程崩溃 / 被杀 / LMK / SIGKILL**:什么都不用做。staging 在 mmap(page cache),已写的 Block 也在 page cache,都归内核管,进程死亡照常回写。
- **断电 / 强制重启**:唯一需要持久化检查点的场景。策略:退后台时、以及运行中每 30s,staging mmap 文件做一次 `msync(MS_SYNC)`,日志文件对 fd 做一次 `fdatasync()`(它不是映射,msync 对它无效)。平时不刷;进程崩溃本身不需要任何刷盘。由此**断电丢失窗口 = 最近 ≤30s 的已提交块(fdatasync 滞后)+ 未提交的 staging 内容(msync 滞后)**;§1 验收表的"零丢失""零重复"仅指 kill -9 类进程死亡(页缓存一致,承诺成立),断电按此窗口计。另:断电时 staging 按页回写,某页可能停留在上一轮使用纪元的快照(旧记录、校验和完好),而 Header 页停在另一时刻,恢复扫描可能把旧纪元记录重新压块——**断电窗口内可能出现重复块**;这是 mmap 页缓存写回模型的固有属性,经打点观测即可,不为此复杂化恢复逻辑。

## 12. 后续扩展预留(本期不做)

- **iOS 适配**:核心零改动,需处理四个坑——文件保护属性(`CompleteUntilFirstUserAuthentication`)、文件不放 Caches 目录(防系统清理导致 SIGBUS)、后台 flush 挂 `didEnterBackground` + `beginBackgroundTask`、ObjC 桥 + CocoaPods 打包。iOS 无多进程问题,比 Android 简单。
- **Flutter/RN**:C ABI 就位后为纯胶水工作(dart:ffi / Turbo Module)。
- **Windows**:PAL 预留接口,`CreateFileMapping + MapViewOfFile(FILE_MAP_WRITE)`,`FlushViewOfFile` 替代 msync。
- **上报/回捞闭环**:如需 Logan 式主动上报 + Push 回捞 + 流量控制,需后端配合,建议独立立项。

## 13. 修订记录

- **v5.2(2026-09-03,深度系统复核新增标注)**
  - 对周边配套链路(Python 解码器、Kotlin Facade、JNI 参数桥接、切分恢复状态、持久化调用)开展深度审计:
    标明 5 项新发现的缺陷与偏差(decoder 状态机 Bug、LogX.kt 语法截断与缺失 Gradle、JNI 参数硬编码/ErrorHandler 虚置、重启切分序号重置为 0 覆写风险、持久化调用为 0),
    文首标注扩展为 12 项并统一归口计划表任务 T4.0 回补修复。

- **v5.1(2026-09-03,实现状态标注,不改设计)**
  - 依代码级独立复核(Windows/MSVC Release,zstd+AES-GCM 全开,39/39 单测实测通过)在文首增加「实现状态标注」块:标明 7 项 v5 优化(§7 flush 水位线、§3.1 切分序号 2B、§4.2 双 Header 屏障纪律、§4.1 Debounce、§10 ARMv8 加速、§11 30s 定时持久化、§9 两类测试用例)仅 spec 定稿、代码未落地或偏离,归口计划表 T4.0 回补;spec 正文本身不做任何设计改动。

- **v5(2026-09-02,四次评审优化定稿)**
  - [P0] **环形坐标系规范化**: 修复 §5 伪代码中绝对指针与相对偏移混淆的 Bug; 统一规范 Header 与运行时指针为环内相对偏移 `[0, kDataSize)`, 物理指针由 `pos_ptr(pos)` 换算(§3.1/§5)
  - [P0] **双 Header 写入增加内存屏障**: 在写完 Header A 后插入 `std::atomic_thread_fence(std::memory_order_seq_cst)` 再写 Header B, 消除 ARM 弱内存模型下的指令乱序可见性问题(§4.2/§10)
  - [P0] **`LogX.flush()` 水位线与防饥饿控制**: `flush()` 记录目标提交水位 `target_seqno`, 阻塞等待至该水位即返回, 防止高频写入下的无限期饥饿等待(§4.2/§7)
  - [P1] **多进程 Safe-by-Default 自动隔离**: 框架层在 `init()` 自动根据进程名后缀隔离 `logDir` 与 `stagingDir` (如 `logs_push`), 避免业务漏配导致的跨进程 mmap 覆写(§6/§7/§10)
  - [P1] **`LogX.export()` 协程化管理**: 导出接口设计为 Kotlin `suspend` 函数, 待异步上传操作完成后再清理明文临时文件, 彻底消除异步场景下的 `FileNotFoundException`(§7)
  - [P1] **切分序号升级为 2B (uint16_t)**: Header reserved 划出的追加目标缓存中切分序号改为 2B (上限 65535, 支持 655GB/天), 彻底消除 1B 达 255 停写的边界风险(§3.1/§4.4)
  - [P1] **JNI 零堆内存分配**: 采用 `GetStringUTFRegion` 直接写入 TLS Scratch Buffer 原地格式化, 消除 JNI 边界 GC 开销(§4.1/§7/§10)
  - [P2] **硬件加速与 Flusher 聚合优化**: 引入 ARMv8 Crypto Extensions 硬件加速 AES-GCM; Flusher 增加 50~100ms Debounce 超时聚合, 提高 zstd 压缩率并减少 syscall(§2/§4.1/§10)
  - [P2] **测试矩阵补全**: 新增未对齐内存访问单测 (32 位 ARM)、极端时钟跳变跨天边界测试 (23:59:59 回拨至 00:00:01)(§9)

- **v4(2026-08-31,三次评审修订)**
  - [P0] 回绕缝 **1~5B 残留**定稿:pad < 6B 时留零回绕、不垫 PAD;切块/恢复扫描/解码按"距段尾 <6B 无条件跳过"处理,残留计入所在块 raw_len——原 spec 的"垫 PAD 占满至环尾"在 pad ∈ [1,5] 时无解,会卡死 Flusher 切块并把回绕后全部未提交记录当撕裂丢弃,违反 kill -9 零丢失(§3.1/§3.2/§4.1/§4.2/§4.3/§5/§10)
  - [P0] 恢复流程新增**不变量收尾**:分支对账跨过的区间(可能未清零)与撕裂尾巴在提交新 Header 前清零,显式置 write_pos = W'——否则 debug 断言必失败,且环将满、free 区无零时,下次扫描会穿过校验和完好的陈旧记录重新压块,产生重复块(§4.3/§5/§10)
  - [P1] **截断后 seqno 跳 1 空隙**(续写首块 = max(S_hdr, S_file) + 2):修复"截断丢弃的块与新块同 (key,盐) 复用 GCM nonce"违反 §3.2 不变量的问题;对账/定位/解码只依赖比较,空隙无副作用(§3.2/§4.3/§6)
  - [P1] 安全口径修正:staging 文件为应用私有目录内明文(30s msync 落盘),仅正式日志文件为密文;新增自动备份排除纪律(allowBackup/dataExtractionRules)(§1/§10)
  - [P1] 断电语义显式化:"零重复"限定 kill -9;§11 补页回退复活旧纪元记录的说明;§9 新增断电注入、降级注入、外部破坏三行;§6 新增 keyProvider 取不到当前密钥的降级行(§1/§6/§9/§11)
  - [P2] "全零记录头"定义为 6B 头全零,len=0 仅 PAD 合法、格式化器拒绝空 payload;"保留 1 字节"改为会计空隙表述;maxLogLength init 校验(必须 < 数据区−1,推荐 ≤ 数据区/2);flush 同步语义、export from/to 日期闭区间、keyProvider null 行为、重复 init 抛错;缓存失效慢路径先扫最新日期文件;"轮次边界"明确为 flush 轮次、切分序号达 255 停写;M1 <2µs 注明 native 路径;gtest 与 decoder 补回绕缝残留用例(§2/§3.1/§4.3/§4.4/§7/§8)

- **v3(2026-08-31,二次评审修订)**
  - [P0] 生产者 free 计算改为**保留 1 字节**(`(flushed−write−1+size)%size`):原式在空环(flushed==write,全量 flush 后必然出现)时恒为 0,写入被永久丢弃;顺带消除空/满歧义、保证环形扫描整圈必遇零(§3.1/§5)
  - [P0] raw_len 重定义为**块覆盖的 staging 字节跨度(含记录头与 PAD)**,拷出流保留 PAD、解码端按记录跳过:原"跳过 PAD + 按条目累加"在跨回绕轮次会把 F 推短一个 PAD 长度,恢复时整段未刷数据被误当撕裂丢弃或产生重复块(§3.2/§4.2/§4.3)
  - Header 提交粒度由每轮改为**逐块**(64B mmap 写,开销可忽略),未提交窗口缩到单块;k 块回溯保留(断电时 fdatasync 领先 msync、双 Header 回退仍可 k>1)(§4.2/§4.3)
  - 分片改为**允许跨块**:删除"全部分片必须同块"(与块 ≤20KB 上限矛盾),改为记录原子不跨块 + 连续分片段跨块重组;"条目对齐"改为"记录边界对齐"(§3.1/§3.2)
  - staging 记录头 5B→6B,新增 **1B payload 校验和**(锁外随格式化计算):可识别"头完好、payload 截断"的撕裂记录,落实宁丢不脏;漏检约 1/256(§3.1)
  - PAD 改为**可连续多条**(每条 payload ≤ 20KB):修复大日志回绕时单条 PAD 超过记录上限的问题(§3.1/§4.1/§5)
  - 追加目标定位由 mtime 改为**文件名日期 + 尾块 seqno**(时钟回拨下 mtime 不可靠,文件名回退还有同名冲突);Header reserved 划出 13B 缓存追加目标位置,恢复 O(1) 定位,失配回退全量扫描慢路径(§3.1/§4.3)
  - 启动恢复只校验/截断**追加目标**文件,不再全量扫描历史文件,冷启动恢复降为 O(缓存 + 文件尾)(§4.3/§10)
  - 新建日志文件后对父目录 fsync;断电丢失窗口(≤30s)显式写入 §1/§11;S_file < S_hdr 分支增加丢失块数打点(§6/§10/§11)
  - 新增约定:单条日志上限 64KB(Config.maxLogLength)、init 前调用丢弃 + 计数、export 快照止点与临时文件位置、keyProvider 调用线程、debug 构建断言环形不变量(§5/§7/§10)

- **v2(2026-08-31,方案评审修订)**
  - staging 由"线性 + memmove 压缩"改为**环形缓冲 + 清零回收**,消除"已搬移/未提交"崩溃窗口;回收清零在锁外、先清零后提交(§1/§3.1/§4.2/§5)
  - 恢复对账由"S_file == S_hdr + 1"泛化为 **k 块回溯、累加 raw_len**,覆盖单轮多块与双 Header 回退场景;追加前无条件截到最后有效块(§4.3)
  - 块内只装完整条目,raw_len 恒条目对齐;单条 16~20KB 独占一块(§3.2)
  - nonce 修正为 **8B 每文件随机盐 + 4B seqno**;seqno 永不回退,staging 重建时从文件 max(seqno) 续接,防 GCM nonce 重用(§3.2/§4.3)
  - 日志文件刷盘由 msync 修正为 **fdatasync**(msync 对非映射文件无效);staging 用 msync(MS_SYNC),放弃 MS_ASYNC(§10/§11)
  - Header 字段修正为定长 64B(reserved 40B);文件头补 reserved 至 32B(§3)
  - staging 目录改 filesDir 并禁用 cacheDir;新增密钥管理约定(Keystore 包裹软件密钥)与 KID 轮换约定;`cipherProvider` 改为 `keyProvider(kid)`;export 先 flush(§7)
  - 分片格式定稿:条目头加 flags,同条分片单临界区连续写入,按连续分片段重组(§3.1)
  - 降级态经 errorHandler 显式暴露;测试矩阵新增窗口注入、超长分片、时钟跳变(§6/§9)
