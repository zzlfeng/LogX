# logx 代码审查报告 (R20) —— R19 修复批次复核

> **复核对象**: logx 仓库 `5d7ac34` 之上的**未提交修复批次**(工作树 11 个文件,+401/−105,含新增 0 字节 `tests/unit/test_r19_regression.cpp`);即 `code_review_R19_2026-10-04.md` 全部 16 项发现(N-1)的修复实现
> **复核日期**: 2026-10-04
> **复核范围**: ① R19 修复 diff 逐行独立审读(staging / flusher / recovery / logx_c / codec / format / JNI / Kotlin / decoder / PAL);② 跨模块契约核对(写侧产出 vs 读侧接受域);③ 全量重建 + 全量测试;④ 对 P0 修复自建探针**端到端验证**(不复用仓库测试桩,对齐 R19 §七·8 门禁纪律)
> **方法**: `vcvars64` 导入环境 → `ninja -t commands` 取证 → 逐条直连 `cl.exe`/`lib.exe`/`link.exe` 复跑 54 步(沙箱内 ninja jobserver-pipe 挂死复现,与 R19 §六 记录一致)→ 全量测试 → 自建 `split_impl` 切分探针 + 回调内 close 生命周期探针。实验产物已全部清理,`git status` 回到修复批次原状(11 项)。

---

## 一、总体结论

**修复方向正确、大部分成立;但存在 1 个实测新缺陷(修复引入)+ 1 项残留未修完 + 回归测试完全缺失。**

| 维度 | 结果 |
|---|---|
| 构建 | ✅ 54 步全量重建 **BUILD_ALL_OK**,0 error / 0 warning(ninja 挂死按 R19 同款方法绕过) |
| 单测 | ✅ `logx_unit_tests.exe` **148/148 PASSED**;热路径 2/2;压力 4/4 |
| 修复完成度 | 16 项中 **11 项成立**、**1 项实测引入新缺陷(F-1)**、**1 项残留(F-15 PAL)**、3 项有小缺口 |
| 回归屏障 | ❌ **`test_r19_regression.cpp` 为 0 字节空文件**;CHANGELOG 声称"156/156(148+8 项 R19 新增)"**与实测 148/148 不符,验证记录失实** |
| CHANGELOG 其他 | "55/55 步"实测 54 步(ninja -t commands 汇总后可执行步数);"压力 397/397"沿用 R18 旧数字,本轮未复跑注入对账明细(4/4 通过为实测) |

### 复核裁定总表

| R19 编号 | 级别 | 修复裁定 | 关键证据 / 缺口 |
|---|---|---|---|
| F-1 | **P0** | ⚠️ **主场景已修,边界场景实测引入新缺陷** | 探针 P1:原始场景(100+3B缝+292 记录)→ 2 块合规;探针 P2:**满长记录+缝 → 3B 微块,读侧必拒**(见 §二·1) |
| F-2 | **P0** | ✅ 成立(探针实证) | 回调内 close 全程存活、`MAIN EXIT CLEAN`(对照 R19 探针 F 的 0xC0000005);3 个次要缺口见 §二·2 |
| F-3 | P1 | ✅ 成立 | `scan_one` repair 截断 + `open_or_create` 重写头 + `block_write_errors` 补计数,三处齐全 |
| F-4 | P1 | ✅ 成立(一处巧合性正确) | `wrap_boundary` 传递正确;`valid_end = wrap_boundary` 的收尾清零副作用靠巧合无害,需注释锁定 |
| F-5 | P1 | ✅ 成立 | 每次 reconcile 均查目录最新文件,跨天回退正确;同日多分卷场景未覆盖 |
| F-6 | P1 | ⚠️ **2/3 修复,1 处残留** | open 期 fail-fast ✅ + 导出逐块判定 ✅;`write_file_header` 加密位仍无条件声明(见 §二·6) |
| F-7/F-8 | P1/P2 | ✅ 成立 | `nativeWrite`/`nativeFlush` 返回 `jint`,`flush(): Int` 可区分 `-10`;KDoc 修正 |
| F-8/F-9/F-15(decoder) | P1/P2 | ✅ 成立 | 数值排序 / try-except / nonce 派生 / raw_len 交叉校验 / seqno 单调 / 撕裂尾告警 / zlib eof 强校验全部落实;`py_compile` 通过 |
| F-10 | P2 | ✅ 成立 | `*out_inst = nullptr` 统一(一处置空覆盖全部失败分支) |
| F-11 | P2 | ✅ 成立 | `std::string` 拼接,截断消除 |
| F-12 | P2 | ✅ 成立 | 先 `store(-1, release)` 再 fsync/close |
| F-13 | P2 | ✅ 成立(`is_new_target` 边界缺口) | 空新文件不再虚报;**`is_new_target` 判定条件 `sr.seqno == 0` 排除了"新文件已有孤儿块"场景** |
| F-14 | P2 | ✅ 成立 | rebuilt 分支 `hdr.seqno = sr.seqno + 1` + 计数 + 事件 |
| F-15(PAL 子项) | P2 | ❌ **未修,CHANGELOG 声称已修** | `logx_pal.h:51` 使用 `std::vector` 仍无 `#include <vector>`(仅 cstddef/cstdint/functional/string) |
| N-1 | Nit | ✅ 成立 | CHANGELOG 已补 `5d7ac34` 行 |

---

## 二、逐项复核详述

### 1️⃣ F-1(P0)回绕缝超限块 —— 原缺陷已修,边界场景实测产出"微毒块"(新缺陷)

**已修对的部分**(探针 P1 实证):新增 `probe_wrap_seam`(flusher.cpp:47,识别"1~5B 全零 + 其后合法记录"),在 `split_impl` 两处 `peek_record_len==0` 分支按缝封块(:74 / :105,缝归入当前块、缝后另起新块),并给撕裂尾路径加 `kBlockMaxRaw` 硬上限兜底。R19 原始场景复测:

```
[P1 seam-mid] total=19603 blocks=2
  block[0] off=0 len=5003
  block[1] off=5003 len=14600
  [OK] >=2 blocks; every block <= kBlockMaxRaw(20486); blocks cover whole buffer;
       every block raw_len >= kHeaderLen(读侧可收)
```

**实测新缺陷**(探针 P2):当缝恰好紧跟一条 `kBlockMaxRaw` 满长记录时,:104-107 的 `if (cur + seam - blk_begin <= kBlockMaxRaw) cur += seam;` 判**伪**(20486+3 > 20486),随后 break 把 3B 缝**单独封成一块**:

```
[P2 full-rec+seam] total=22989 blocks=3
  block[0] off=0 len=20486
  block[1] off=20486 len=3   <<< 1~5B 微块
  block[2] off=20489 len=2500
  [FAIL] no 1~5B micro-block
```

该块落盘后 `raw_len=3 < kHeaderLen=6`,读侧 `block_header_plausible`(flusher.cpp:353 `raw_len < kHeaderLen` 判 false)**必然拒绝**。后果分两条路径:
- **导出**(`decode_one_file`):plausible 失败即 break —— 块边界不可信处终止,**整文件后半段丢失**,比 F-1 原缺陷的单块丢失更严重;
- **repair 扫描**(`scan_one`):失败即 `file_truncate(off)` —— **物理截断**掉全部后续数据。

这是 F-1 同款"写侧产出读侧必拒之物"的复发,只是约束从上限换成了下限。触发条件与原 F-1 相同(回绕缝 + 满长记录),非罕见构造。

**修法建议(二选一,均需回归用例)**:
- **正解**:`:104-107` 分支中 `cur += seam` 后**继续沿记录边界累加后续记录直至 16KB/缓冲尾**,而不是立即 break——缝只是填充,后续记录完全可以在同一块继续;
- **最简**:`drain_once` 逐块循环跳过 `blk.raw_len < kHeaderLen` 的块(不落盘,直接并入 `cur_f` 推进清零)——缝本来就是无数据填充,吞掉无损。

### 2️⃣ F-2(P0)回调内 close use-after-free —— 修复成立(探针实证),3 个次要缺口

自建探针复刻 R19 报告探针 F 场景(`flush_threshold=0` 保证 TRUNCATED 事件在调用方线程 write 栈内同步发生,回调内调 `logx_close`):

```
===== MODE: closecb =====
[cb] -> calling logx_close() from inside error callback
[cb] -> logx_close returned 0 (expect 0 = deferred OK)
[probe] write[0] after in-callback close returned 0 (survived)
[probe] SURVIVED: 1 events, close_in_cb=1
[probe] MAIN EXIT CLEAN            ← 对照 R19 报告: 0xC0000005
```

递延机制设计合理:`emit_event` 递增 `in_callback`(logx_internal.h:97)→ `logx_close` 检测重入(`in_callback > 0 || active_ops > 0`)只置 `close_pending` 并返回 OK → 最外层 `OpGuard` 析构时执行 `destroy_instance`(logx_c.cpp:294-295);Flusher 线程路径(`flusher_tid` 比对)置 `close_on_flusher`,由 `flusher_main` 退出后自销毁,绝不 join 自身。`write` 各风险点(closing 检查)布局正确。

**次要缺口(均不阻塞,建议跟进)**:
1. **并发写者时序**:线程 A 回调内递延 close 后,线程 B 若已越过 `closing` 检查,会继续写入并发事件——安全性保住(实例等 B 的 OpGuard 析构才销毁),但"close 后数据不再落盘"的时序不确定。已在 R20 修复批次将契约写入 `logx_c.h`。
2. ~~**Flusher 自毁时 `current_fd` 不关闭**~~(2026-10-05 勘误:此条系复核误判):`FlusherCtx` 是 `flusher_loop` 的栈对象,`close_on_flusher` 自毁发生在 `flusher_loop` 返回之后,而循环尾部必经 `close_current(&c, ...)`——`current_fd` 恒已关闭,无泄漏。
3. **`destroy_instance` 的 `flush_blocking` 依赖 `flusher_running`**:递延销毁发生在最后一个 OpGuard 析构,此时 Flusher 通常仍在跑,行为正确;但若 close 递延后 Flusher 已因异常退出,`flush_blocking` 走"flusher not running"分支返回 false,数据不丢(staging 仍在)但无落盘尝试——边界行为建议补注释。

### 3️⃣ F-3(P1)1~31B 撕裂文件卡死追加 —— ✅ 修复正确

三处全部落实,与 R19 报告修法建议一致:`scan_one` 对 `0<sz<32` 在 repair 模式截断(flusher.cpp:392-395);`open_or_create_append_target` 对同区间截断为空并重写 32B 头(:500-504);`drain_once` 追加目标打开失败分支补 `block_write_errors.fetch_add(1)`(:757,消除"6 计数器全 0"的静默面)。

### 4️⃣ F-4(P1)ring_scan 回绕缝抹除 —— ✅ 修复成立,一处"巧合性正确"

`ring_scan` 增加 `wrap_boundary` 参数(默认 0,旧调用点不破坏),两处识别逻辑(入口前 <6B 跳段;`probe_record==0` 时验证缝全零后跳段)与 spec"跳过 <6B 到段尾、回绕后继续"一致。调用侧 `wrap_boundary = (w < f) ? (size - f) : 0` 计算正确(recovery.cpp:338)。

**注**:`pos = wrap_boundary; valid_end = wrap_boundary;` 中 `valid_end` 的赋值在第一段场景下会让 §6 收尾清零(`truncated_tail` 段)跳过第一部分——因该部分本就是待刷数据且后续记录从缝后重新起算,结果恰好无害。**属巧合正确**:建议在注释里写明该副作用与依赖条件,否则未来重构极易踩坏。本轮未新增任何 F-4 回归用例。

### 5️⃣ F-5(P1)跨天 ATC 过期重复 seqno —— ✅ 修复成立

`reconcile` 现在每次先 `scan_dir_for_target` 取目录最新文件,`best_date > hdr.target.date` 时直接扫描最新文件(跳过 ATC 提示),正确切断"昨日 ATC 接管今日文件"→ 盲开 → 同文件重复 seqno 的路径。

**小缺口**:仅按 `date` 比较。同一天内 ATC 落后于最新分卷(如 ATC 指 `.log.2` 而已存在 `.log.5`)时,孤儿块漏检场景仍在。低概率(需同日多次切分 + 崩溃窗口叠加),建议纳入 backlog。

### 6️⃣ F-6(P1)密钥恒缺明文落盘 —— ⚠️ 2/3 修复,1 处残留

✅ **open 期 fail-fast**(logx_c.cpp:374-378):`encrypt_enable=1` 时对 `cfg->kid` 探测调用 `key_provider`,返回 NULL 即拒绝启动;
✅ **导出逐块判定**:`decode_one_file` 删除"文件头 encrypted 位 → 整文件 skip"逻辑,改为块头逐块判定——明文块无条件解码输出;加密块缺 key 时输出 `[skipped: kid=%u missing key (seq=%u)]` 单行并继续。
❌ **残留**:`write_file_header`(flusher.cpp:298)仍**无条件**按 `inst->encrypt_enable` 写文件头加密位。R19 建议③("加密位与实际首块一致,或取不到密钥时拒绝建文件")未实现。open 期 fail-fast 掐掉了"密钥恒缺"实例,但**运行期密钥丢失**(`key_provider` 运行中才开始返回 NULL)依旧走 `append_block_raw` 降级分支 → 文件头声明加密、块明文落盘。修复后的导出侧可正常解出这些明文块(可读性已保住),残留是**文件头元数据失真**——建议至少在降级事件消息中写明"file header declares encryption",并在 spec 层面决定加密位的语义归属。

### 7️⃣ F-7 / F-8(P1/P2)JNI / Kotlin 状态码与文档 —— ✅ 修复成立

`nativeWrite`/`nativeFlush` 改返回 `jint`(各失败分支补 `LOGX_ERR_INVALID_ARG`/`LOGX_ERR_NOMEM`/`LOGX_ERR_STATE`);Kotlin `log()`/`flush()` 返回 `Int`(`executeOp{...} ?: ERR_STATE`),`-10 LOGX_ERR_TIMEOUT` 自此可达;KDoc `:28`"超过被丢弃"改为"截断保前缀落盘 + TRUNCATED 事件",与行为一致。与 R19 §四 复核批注口径一致(补的是返回码,事件通道本就可见)。

注意:`log()` 从 `Unit` 改 `Int` 是源码级破坏性演进,既有 Kotlin 调用方(忽略返回值)不受影响,显式依赖 `Unit` 返回的代码需适配——CHANGELOG 已提,可接受。

### 8️⃣ F-8 / F-9 / F-15 Python 解码器 —— ✅ 修复成立

分卷按 `(date, split)` 数值排序(`log_file_sort_key`);`os.listdir`/`open`/单文件解码三层 try-except(单文件失败 rc=2 继续,不再 traceback 终止整轮);`log_dir` 用 `os.path.isdir` 校验(区分普通文件与不存在);nonce 由文件头 `salt[8] + seqno_be[4]` 派生(对齐 C++ `make_nonce`,不再取块头回显域);`raw_len` 与解压输出长度交叉校验;`block_header_plausible` 增加 seqno 严格递增;块尾 <34B 输出撕裂告警;zlib 分支 `d.eof` 强校验(`flush()` 后仍非 eof 即拒绝)。`python -m py_compile` 通过。

### 9️⃣ F-10 ~ F-14(P2)工程加固批量 —— ✅ 全部成立(F-15 PAL 子项除外)

- **F-10** `logx_open` 入口即 `*out_inst = nullptr`,全部失败返回统一遵守契约;
- **F-11** `make_log_path` 改 `dir + buf` 拼接(片段仍栈缓冲但不含 dir,不再截断路径);
- **F-12** `close_current` 先 `active_log_fd.store(-1, release)` 再 fsync/close,`durability_sync` 不再可能 fsync 已关闭 fd;
- **F-13** `ReconcileResult.is_new_target` + `truncated_blocks` 虚报豁免 + `resume_seqno = S_hdr` 继承(不跳空隙,符合"无块被丢弃"语义)。**边界缺口**:`is_new_target` 判定要求 `sr.seqno == 0 && !sr.truncated`——若新文件已有孤儿块(seqno > 0),走常规路径,孤儿块对账由 `seqno_file > S_hdr` 分支兜住,行为正确;但"新文件 + 崩溃留下部分块且 Header 未提交"组合下 `S_hdr > seqno_file` 仍会虚报。窗口极窄,建议 backlog;
- **F-14** rebuilt 分支 `sr.truncated` 时 `hdr.seqno = sr.seqno + 1` + 计数 + 事件,被截块 seqno/nonce 不再复用;
- **F-15 PAL 子项** ❌ **未修**:`logx_pal.h` 使用 `std::vector`(line 51)仍无 `#include <vector>`,靠传递包含侥幸。CHANGELOG 第 9 条"显式补齐 `#include <vector>`"**与事实不符**。一行修复。

### 🔟 N-1(Nit)CHANGELOG 行数 —— ✅ 已补 `5d7ac34` 行

---

## 三、必须处理的两个问题(按优先级)

1. **回归测试缺失 + 验证记录失实(提交阻塞项)**:`tests/unit/test_r19_regression.cpp` 为 **0 字节空文件**,却已加入 `tests/unit/CMakeLists.txt` 编译列表。R19 报告 §七·1 明确要求该用例"先撤销修复跑一遍必须见红"——目前**整个 R19 批次零回归屏障**,下一轮重构即可无声回退 F-1/F-2。同时 CHANGELOG"156/156 全绿(148 既有 + R19 新增 8 项)"与实测 148/148 不符。**二选一**:补齐 8 个针对性用例(至少覆盖:①缝在缓冲中部且缝后积压 > 20550B → 块数 ≥2 且每块 `raw_len ∈ [6, 20486]`;②满长记录+缝 → 无 1~5B 微块;③回调内 close 后 write 安全返回;④0<sz<32 文件可自愈;⑤zstd 导出可完整取回缝场景数据),或先修正 CHANGELOG 记录并从 CMake 移除空文件、声明测试为后续工作。
2. **F-1 微块缺陷**(§二·1):`drain_once` 跳过 `raw_len < kHeaderLen` 块,或 `split_impl` 缝后继续累加;并把"`kHeaderLen ≤ 每块 raw_len ≤ kBlockMaxRaw`"写成写读两侧共用的单测断言(跨模块常量一致性门禁,R19 §七·8 已建议)。

## 四、次要建议(不阻塞)

- `logx_pal.h` 补 `#include <vector>`(一行,CHANGELOG 已许诺,落实它)。
- F-6 残留:定夺"文件头加密位"语义——与实际首块一致,或运行期降级时改写文件头(需评估 CRC/回写纪律),至少在 DEGRADED 事件中写明失真。
- F-4 `valid_end = wrap_boundary` 的收尾清零副作用写进注释(锁定"巧合正确"的依赖条件)。
- F-5 同日多分卷孤儿块场景、F-13 新文件+孤儿块组合场景纳入 backlog。
- CHANGELOG"压力 397/397"改为"4/4 通过(对账明细沿用 R18 记录)"或本轮复跑对账。

## 五、验证记录

| 项 | 命令/环境 | 结果 |
|---|---|---|
| 构建 | MSVC 14.43 (/std:c++17 /utf-8 /O2 /MD,`LOGX_WITH_ZSTD=ON`、`LOGX_WITH_AESGCM=ON`);`vcvars64` 环境导入 + `ninja -t commands` 取证 + 逐条直连 cl/lib/link **54 步** | **BUILD_ALL_OK**,0 error / 0 warning |
| 单测 | `build-full\tests\unit\logx_unit_tests.exe` | **148/148 PASSED**(非 CHANGELOG 声称的 156) |
| 热路径 | `logx_hotpath_alloc_tests.exe` | 2/2 PASSED |
| 压力 | `build-full\tests\stress\logx_stress_tests.exe` | 4/4 PASSED(对账明细未复跑) |
| Python | `python -m py_compile decoder\logx_decode.py` | 通过 |
| 探针 1 | 自建 `r19_verify_probe.cpp`(直连 logx_core.lib,不经仓库测试桩):P1 原始 F-1 场景 / P2 满长记录+缝 / P3 撕裂尾兜底 / P4 变异语义 | P1 ✅ 2 块合规;**P2 ❌ 3B 微块**;P3 ✅ |
| 探针 2 | 自建 `r19f2_probe.cpp`(closecb vs nonecb 双模式) | closecb 全程存活、**MAIN EXIT CLEAN**(对照 R19 探针 F 的 0xC0000005);nonecb 行为不变 |
| 收尾 | 探针源码/obj/exe/日志/临时目录全部删除;`git status --porcelain` | 回到修复批次原状(10 M + 1 ??) |

> **环境备注**:ninja(1.13,jobserver-pipe)在本沙箱下派发后挂死(与 R19 §六、R18 记录一致),`-j1` 同样挂死;沿用"vcvars64 → `ninja -t commands` 原始命令行逐条调用"的等价构建。未复跑:另两个构建矩阵、Android arm64 交叉编译与真机(沿用既有共同局限)。

## 六、下一批次(提交前)顺序建议

1. **补 R19 回归测试**(§三·1 方案 A),特别是 F-1 微块边界——先撤销修复跑一遍必须见红,防止"空洞绿"。
2. **修 F-1 微块**(§二·1 两种修法择一)+ `raw_len ∈ [kHeaderLen, kBlockMaxRaw]` 双侧断言。
3. `logx_pal.h` 补 `<vector>`;CHANGELOG 验证数字改为实测值。
4. F-6 残留决策 + F-4 注释锁定(可并入提交或转 backlog,提交信息中如实声明)。

---

> **复核声明**:本报告结论基于读码 + 全量重建 + 全量测试 + 自建探针实验;探针程序未复用仓库测试辅助代码;实验产物已全部清理,工作树与复核开始时一致。

---

## 七、R21 批次收口批注（2026-10-05 追加，原文保留）

> 本报告 §三/§四 全部条目已由 **R21 批次**处理完毕（提交见 `CHANGELOG.md` R21 章节）。逐条对照：

| 本报告条目 | R21 处置 |
|---|---|
| §三·1 回归测试缺失 + 验证记录失实 | ✅ 已补齐并**经撤销对照实验验证有效**（R21 复核先证明这 8 项中 4 项为空洞绿：撤销 R20 三项产品修复后 156+4+2 全绿）。新增 `F1c`（落盘层微块）、`F6b`（能力锁定）、`F14b`（常规路径孪生）、读侧接受域 2 项；`F1b` 恒真断言修正并改名；`F3` 改按事件文案断言；`F14` 改为真毒化双 Header 走 rebuilt 分支。单测 156 → **161**。CHANGELOG R20 段的失实记录已就地勘误。 |
| §三·2 / §二·1 F-1 微块缺陷 | ✅ 修复本体（R20 的 `drain_once` 跳过分支）经 R21 逐路径推演确认**成立**（块覆盖连续、缝必为纯零、seqno 不消耗、计入 `flushed_bytes_total` 是 `flush_blocking` 进度代理的必要项）。R21 另加**全零前提校验** + `seam_blocks_skipped` 计数器，并补上本报告要求的"写读两侧共用断言"的读侧半边。 |
| §四 `logx_pal.h` 补 `<vector>` | ✅ R20 已落实（R21 复核确认）。 |
| §四 F-6 残留：定夺文件头加密位语义 | ✅ 用户拍板**方案 A：文件级能力锁定**——`out_enc_capable` 出参 + `FlusherCtx::current_enc_capable`，已存在文件回读头 flag，本文件生命周期内不改口；语义写入 spec §3.2。**顺带修掉 R21 新发现的两处**：反向竞态（头声明明文、块实为密文）与缺钥降级状态跨块/跨文件泄漏（`flag` 原在循环外算一次，一次瞬时缺钥会把本进程后续全部块永久静默降为明文）。 |
| §四 F-4 `valid_end = wrap_boundary` 注释锁定 | ✅ R20 已落实（R21 复核确认表述准确）。 |
| §四 F-5 同日多分卷 / F-13 组合场景纳入 backlog | ✅ R21 落为 **B-7 / B-8**（计划表 §6，含影响面核实与候选方案）。 |
| §四 CHANGELOG"压力 397/397"改口径 | ✅ R20 已落实。 |
| §六 提交前顺序建议 1~4 | ✅ 全部完成，顺序与建议一致。 |

> **⚠️ 本报告 §五「环境备注」更正（R21 实测）**：「ninja(1.13, jobserver-pipe) 在本沙箱下派发后挂死、`-j1` 同样挂死」**不成立**。R21 以 `MSYS_NO_PATHCONV=1 cmd.exe /c build_tmp.bat`（bat 内 `call vcvars64.bat && ninja`）正常跑完 **56 步全量重建，0 warning / 0 error**，其后多轮增量重建亦正常。挂死记录的疑因：Git Bash 未加 `MSYS_NO_PATHCONV=1` 时 `/c` 被路径转换成 `C:\`，`cmd.exe` 只打印横幅后以 **rc=0 空转退出**，极易被误读为"派发后无输出/挂死"。
>
> **⚠️ 连带风险（R21 发现，已固化为计划表 §4 纪律 R21-2）**：因 R20 复核以手工 `cl.exe` 绕过 ninja 编译，`build-full` 的 ninja 依赖日志与磁盘 `.obj` 失配——R21 实测修改 `logx_internal.h`（含 `Instance` / `FileScanResult` **结构体布局变更**）后，ninja 只重编了 `.cpp` 改动方，`logx_c.cpp.obj` 停留在旧布局，产生**混 ABI 二进制**。凡涉及头文件改动一律 `ninja -t clean` 全量重建（`build-full/clean_build.bat`）。本报告 §五 记录的"54 步"与 R20 提交信息记录的"增量 49 步"均属该手工口径，与 ninja 步数不可比。
