# R46 代码审查报告 — 提交 0ef345f（R45 缺陷全量收口）

> **审查对象**: `0ef345f` fix(core,jni,test,docs): R45 审查 12 项缺陷全量收口（ACT-1~8）
> **基线**: 父提交 `e834e60`（R45 复核报告）
> **审查方法**: 逐 hunk 通读 + 全仓引用检索 + 实际编译 + 实跑单测/压力测试 + 探针实证
> **审查日期**: 2026-10-24
> **审查结论**: **可以入库**（无 P0/P1 阻断项）；但"12 项全量收口"的表述偏乐观，仍需 4 项 P2 + 8 项 P3 收口。

---

## 一、总体结论

1. **R45 报告中 4 项 P0（R45-01/02/03/04）的核心修复在实现层面成立**：OpGuard 回滚认领、Dekker 全序屏障、递延路径禁止裸读 `inst`、`set_error_handler`/`get_stats` 接入 OpGuard，均已按报告要求的"最小正确形态"落地，且经实证编译通过、203 项单测与 4 项压力测试全绿。
2. **两项声明与实装不一致**（不构成阻塞，但必须更正声称）：
   - ACT-6 "将 `key_provider` 纳入 `in_callback` 门禁" 只覆盖了 3 处，`flusher.cpp:1155` 仍为裸调用；
   - ACT-7/R45-11 "文档与实装对齐" 未完成——计划文档仍写着本提交刚删除的 `cb->flusher_thread` 字段。
3. **两项测试质量问题**：`OpenCloseMemoryBounded` 的"内存上界断言"是恒真式（无法失败）；新增的 key_provider 并发用例对正在运行的 Flusher 线程写入非原子字段，构成形式上的数据竞争（UB）。
4. 未发现新的 UAF、死锁或漏认领路径。同步/递延/自毁/递延-自毁交叉四类状态迁移逐条推演均闭合。

### 提交清单

| 项 | 值 |
|---|---|
| SHA | `0ef345f` |
| 父提交 | `e834e60` |
| 改动规模 | 14 文件 / +709 / −273 |
| 触及层 | core（`logx_c.cpp`、`flusher.cpp`、`logx_internal.h`、`logx_c.h`）、JNI（`jni_key_cache.h`、`logx_jni.cpp`）、tests（4 个文件 + 8 个 CMake 行）、docs（plan/decisions） |
| 工作区状态 | 审查前后 `git status --short` 均为空（未污染） |

---

## 二、R45 十二项缺陷闭环逐项核实

| 编号 | 上轮定级 | 声称处置 | 独立核实结论 |
|---|---|---|---|
| R45-01 | P0 | ACT-1：OpGuard 两处早退回滚在归零时触发 `check_deferred_dispose_on_exit` | ✅ **成立**。`logx_c.cpp:296-299`、`305-309` 两分支均已对齐；首处 `is_closing` 早退发生在 `fetch_add` **之前**（`:288`），不存在"未递增却递减"。报告要求的 `incremented` 标志因采用分支内回滚而无需引入 |
| R45-02 | P0 | ACT-2：恢复 6 处 seq_cst 全序屏障 | ⚠️ **基本成立**。`OpGuard`（add/load/sub）、`emit_event`、`invoke_key_provider`、`check_deferred`、`close_ex`、`flusher_main` 均为 seq_cst；但 `cb->pal`（非原子）仍存在跨线程读写，见 P3-02 |
| R45-03 | P0 | ACT-3：递延路径禁止 `close_requested.store` 后裸读 `inst`；非 Flusher 线程不提前置 `flusher_exit` | ✅ **成立**。`close_requested.store` 已移到 `inst`/`pal` 使用之后（`:576`），其后仅剩原子操作 + 自有 `exchange` 认领；非 Flusher 递延路径完全不再置 `flusher_exit`，由 `destroy_instance` 在 `flush_blocking` 之后置位 |
| R45-04 | P0 | ACT-4：两接口接入 OpGuard | ✅ **成立**（附 P2-02 的语义副作用：空句柄返回码由 INVALID_ARG 改为 STATE） |
| R45-05 | P1 | ACT-5：用例改由 Flusher 线程真实触发 + 拆分明细计数 | ✅ **成立且非空洞**。`cb_tid == flusher_tid`、`thread_detach_count == 1`、`thread_join_count == 0`、`create == destroy` 四重断言；注释掉 `pal->thread_detach` 必红（断言与行为直接绑定） |
| R45-06 | P1 | ACT-6：`invoke_key_provider` 纳入门禁 + 死锁用例 | ⚠️ **部分成立**。3 处生产路径已改（`flusher.cpp:584/639/965`、`logx_c.cpp:90`），但 `flusher.cpp:1155` 漏改，见 P2-01 |
| R45-07 | P1 | ACT-4：`err_mu` 锁内同步 `on_destroy_err_user` | ✅ **成立**。写入在 `err_mu` 内（`:780-785`）；读取侧由 `active_ops==0` 的 seq_cst 递减建立 happens-before，无需持锁 |
| R45-08 | P2 | ACT-7：删死声明 / 补唤醒 / 迁 test_accessor | ✅ **成立**。二参 `destroy_instance` 声明已删；`test_simulate_crash_destroy` 已补 `flush_event`+`space_event` 唤醒；`test_accessor.h` 迁至 `tests/`，3 个 target 的 include 路径同步补齐（已核对无遗漏 target） |
| R45-09 | P2 | ACT-7：`current_tid != 0` + `flusher_tid` 清零 | ✅ **成立**。`check_deferred:129`、`close_ex:543` 均已加固；`destroy_instance:371` 清零 |
| R45-10 | P2 | ACT-7：`OpenCloseMemoryBounded` → 10k 轮 + 内存断言 | ⚠️ **形式成立、实质未达**。轮数已 10k，但"内存上界断言"恒真，见 P2-03 |
| R45-11 | P2 | ACT-7：文档与实装对齐至 v1.2 | ❌ **未完成**。计划文档仍保留已删除字段，见 P3-04 |
| R45-12 | P2 | ACT-8：补线程契约 + TLS 残留惰性清扫 | ⚠️ **部分成立**。契约已补但文案自相矛盾（P3-03）；TLS 仅"下次 lookup 时"清零，空闲线程仍长期驻留明文（P3-08） |

**统计**：完全成立 7 项（01/03/04/05/07/08/09），部分成立 4 项（02/06/10/12），未完成 1 项（11）。

---

## 三、独立验证（实证）

### 3.1 编译

使用 MSVC 14.43.34808 + SDK 10.0.22621.0 直接驱动 Ninja（`build-full`）：

| 目标 | 结果 |
|---|---|
| `logx_unit_tests` | ✅ 编译链接成功 |
| `logx_stress_tests` | ✅ 编译链接成功 |
| `logx_crash_fork_tests` | ✅ 编译链接成功 |

`static_assert(sizeof(InstanceControlBlock) <= 64)` 编译期通过。

### 3.2 单测与压力测试

| 套件 | 结果 |
|---|---|
| `logx_unit_tests`（全量） | **203/203 通过**（51.9s） |
| `ControlBlockLifetime.* + LeakRegression.*`（重复 20 轮） | **20×4 全部通过**，无抖动 |
| `logx_stress_tests` | **4/4 通过**（50 线程重压 + 3 项崩溃恢复） |

### 3.3 关键常量/布局实测

| 项 | 实测值 | 结论 |
|---|---|---|
| `sizeof(InstanceControlBlock)` | **64** | 与断言上限恰好相等，无余量 |
| `alignof(InstanceControlBlock)` | **8** | 断言未约束对齐（见 P3-06） |
| `offsetof(inst)` / `offsetof(pal)` | 24 / 32 | 与手工推算一致 |
| `cb->flusher_thread` 残留引用 | 代码层 0 处，文档层 1 处 | 见 P3-04 |

### 3.4 全仓检索

- `key_provider` 调用点：生产路径 5 处（4 处已封装 + **1 处裸调用** `flusher.cpp:1155`）、open 期探测 1 处（`cfg->key_provider`，实例尚未创建，无需门禁，合规）。
- `destroy_instance` 调用点：3 处（`internal.h:135`、`logx_c.cpp:555/585`、`flusher.cpp:1283`），全部为三参版本，无旧签名残留。
- `append_block`：全仓**仅测试**调用（`test_flusher.cpp`、`test_recovery.cpp`），无生产调用点——据此确定 P2-01 的实际风险等级。

### 3.5 跨层核查（JNI）

| 检查项 | 结论 |
|---|---|
| `on_destroy` 落在 Flusher 线程时 JNI 的线程附着 | ✅ `jni_destroy_trampoline` 已处理 `JNI_EDETACHED → AttachCurrentThread → DetachCurrentThread` |
| `on_destroy_err_user` 是否可能被替换成非 `err_ctx` 指针 | ✅ 当前安全：Kotlin `setErrorHandler` 只改 Kotlin 侧 `@Volatile` 字段，**不调用** `logx_set_error_handler`；JNI 无该 API 导出。属潜在契约风险（见 P3-05 备注） |
| `TlsKeyCache::lookup` 由 `const` 改非 `const` 的调用方影响 | ✅ 全仓唯一调用点 `jni_key_provider_trampoline`，`g_tls_key_cache` 为非 const 静态对象，无需改动 |
| `logx_close_ex` 出参消费者 | `nativeClose` 调用的是 `logx_close`（忽略出参），出参失真无实际影响（见 P3-07） |

### 3.6 环境注记（避免误判）

本机存在**基于镜像路径的写权限限制**：当可执行文件位于工作区内（`build-full/tests/unit/*.exe`）时，`CreateDirectoryW` 一律返回 `ERROR_ACCESS_DENIED(5)`（实测连已存在的 `D:/Project` 也返回 5，属拦截而非真实权限）。后果是 Staging 退化为堆模式、`.log` 无法落盘，直接从该目录运行会出现 **2 个"假红"**（`FlusherBlockedInKeyProvider...`、`DeferredCloseFlushes...`）。

已用两种方式排除：① 把同一个 exe 复制到工作区外再运行 → 全绿；② 插桩 `GetLastError` 取到 5 并回滚插桩（`git status` 已确认干净）。**该现象与本提交代码无关，不应作为缺陷计入。**

---

## 四、新发现问题

### P2-01（覆盖不全 / 潜在 P1）ACT-6 漏改一处 `key_provider` 裸调用

- **位置**: `core/src/flusher.cpp:1153-1159`（`append_block` 建文件探测分支）
- **问题**: `!inst->key_provider || !inst->key_provider(inst->kid, inst->key_user)` 仍直接调用，未经 `invoke_key_provider`，`in_callback` 未计入。
- **影响范围**: 当前 `append_block` 全仓仅测试可达，故实际影响为"ACT-6 的'全量'声明不成立"；**一旦该函数被生产路径复用**，将原样复现 R45-06 的死锁形态（Flusher 阻塞于用户密钥回调期间并发 close 误入同步路径 → `thread_join` 死锁），且该函数无 `OpGuard`，并发 close 下直接访问 `inst` 还有 UAF 窗口。
- **修改建议**: 改为 `key = invoke_key_provider(inst, inst->kid); if (!key) enc_capable = false;`，与 `open_or_create_append_target:583-591` 保持同构。

### P2-02（公开契约注释待补齐）`logx_set_error_handler` 空句柄返回码恢复为 `LOGX_ERR_STATE`，头文件未显式写明各接口空句柄契约

- **位置**: `core/src/logx_c.cpp:775`（`return LOGX_ERR_STATE`），公开头 `core/include/logx_c.h:190`
- **问题**: 历史基线（`431d5ee~1` 及此前版本）中 `logx_set_error_handler(nullptr, ...)` 原本即返回 `LOGX_ERR_STATE`；未发布的中间重构提交 `431d5ee` 误将其改为了 `LOGX_ERR_INVALID_ARG`（见 R45-04），`0ef345f` 将其**恢复为原有的 `LOGX_ERR_STATE`**。但公开头 `logx_c.h` 对各接口的空句柄返回码（`logx_close(nullptr)→INVALID_ARG` vs `logx_write/flush/export/set_error_handler(nullptr)→STATE`）缺乏统一显式说明。
- **影响范围**: 属于未发布中间提交的回归修复（非对外破坏性变更）；但在公开头文件中显式写明各接口对空句柄与已关闭墓碑句柄的返回码契约，可避免宿主误判。
- **修改建议**: ① 在 `logx_c.h` 生命周期契约段写明各接口对空句柄及陈旧墓碑句柄的返回码；② 在 CHANGELOG 同步记录。

### P2-03（空洞断言，R45-10 未实质闭环）10k 轮"内存上界"断言恒真

- **位置**: `tests/unit/test_r19_regression.cpp:2964`
- **问题**: `EXPECT_LE(sizeof(InstanceControlBlock) * kRounds, 640u * 1024u)` 只依赖编译期常量（64×10000 = 640000 ≤ 655360），与运行期是否泄漏**完全无关**；把 `delete inst` 删掉、把 `Staging` 泄漏掉，该断言依然通过。
- **影响范围**: 测试门禁虚化。CB 墓碑模型本身每实例永久驻留 64B(实测)，10k 轮 = 640KB 常驻；该断言恰好把这个泄漏"正常化"，却无法发现 `Instance`/`Staging`/event/mutex 的泄漏（这些才是有界性真正要守的东西）。
- **修改建议**: 复用仓库已有的进程内存采集（`test_observability.cpp` 的 `GetProcessMemoryInfo` / `MemoryRssPss_BoundedGrowthAcrossWriteCycles`），断言"10k 轮后 PrivateUsage 增长 ≤ 阈值（如 2MB）"；并保留现有 `destroy_cnt == kRounds`（该项是有效的）。

### P2-04（测试代码数据竞争）新用例对运行中的 Flusher 写入非原子字段

- **位置**: `tests/unit/test_r19_regression.cpp:2997-3000`
- **问题**: `logx_open` 返回时 Flusher 线程已启动，用例随后从测试线程直接写 `raw_inst->encrypt_enable / kid / key_user / key_provider`（均为普通成员），Flusher 在 `drain_once`/`open_or_create_append_target` 中并发读这些字段 → 形式上的 C++ 数据竞争（UB）。
- **影响范围**: 测试本身（可能在高优化/其他编译器下间歇性失败或读到撕裂值）；不污染生产代码。20 轮重复未复现，但属形式缺陷。
- **修改建议**: 在 `logx_open` **之前**通过 `cfg.encrypt_enable=1` + 可控 `cfg.key_provider` 注入（open 期探测调用一次，可设计为首次直通、阻塞仅发生在写后 drain 轮次）；或为 `test_accessor` 增加"open 前注入"钩子，避免运行期改字段。

### P3-01（隐式不变量）`check_deferred` Flusher 分支以 `load` 重新取 `inst`

- **位置**: `core/include/logx_internal.h:142-146`
- **问题**: 该分支不做认领（只 `load`），随后 `inst->flusher_exit.store(...)`。其安全性依赖"任何非 Flusher 销毁者必须先 `thread_join(本线程)` 才能释放 `inst`"这一**未写明的**不变量。
- **影响范围**: 无当前缺陷；但一旦未来有人把 `destroy_instance` 的 join 改为 detach/异步释放，即退化为 UAF。
- **修改建议**: 在分支处补注释固化该推理（"此处 `inst` 无需认领：唯一可能释放 inst 的线程必须 join 本线程，故 inst 在本线程退出前恒有效"）；或改为由 `flusher_main` 持有的局部 `Instance*` 传递，避免二次 `load`。

### P3-02（内存序残留）`cb->pal` 为非原子指针，存在跨线程读写竞争

- **位置**: 写 `core/src/logx_c.cpp:372`；读 `core/include/logx_internal.h:127`、`logx_c.cpp:537`
- **问题**: `destroy_instance` 的 `cb->pal = nullptr` 与 `check_deferred`/`close_ex` 的读取是普通指针的并发读写 → 形式上 UB。ACT-2 只把"状态量"升级为 seq_cst，未覆盖此字段。
- **影响范围**: 实际影响受限（生产环境 `default_pal()` 为静态对象永不析构；测试 Pal 生命周期由用例保证），故仅列为打磨项。
- **修改建议**: 改为 `std::atomic<Pal*>`（仍可保持 64B 布局），或在 `destroy_instance` 里把 `pal` 置空**放到 `delete inst` 之前**并注释说明"置空仅用于陈旧句柄的 null 检查，不保证可见性"。

### P3-03（文档自相矛盾）`on_destroy` 契约新增文案与实际线程模型冲突

- **位置**: `core/include/logx_c.h:124-127`
- **问题**: 新文案称"该回调保证在 **Flusher 线程已退出**、staging 已关闭……后触发"，但紧接着的线程模型又说执行线程**可能是 Flusher 后台线程**。若回调正在 Flusher 线程上执行，该线程显然未退出（只是 `flusher_loop` 已返回）。
- **影响范围**: 文档契约层。实现方可能据此假设"回调时 Flusher 线程已不存在"，进而在回调里做依赖线程计数的清理。
- **修改建议**: 改为"**Flusher 循环已退出**（Flusher 线程可能正是当前执行回调的线程）"。

### P3-04（文档与实装脱节）计划文档仍引用已删除字段

- **位置**: `docs/lifecycle_control_block_plan.md:131`（`cb->flusher_thread = inst->flusher_thread;`）
- **问题**: 本提交删除了 `InstanceControlBlock::flusher_thread`，文档 v1.2 的结构体定义已同步，但创建流程第 6 步仍保留该赋值语句。
- **影响范围**: R45-11 的"文档与实装对齐"未达成；后人按文档实现会编译失败。
- **修改建议**: 删除该行，并补一句"Flusher 线程句柄仅存于 `Instance::flusher_thread`，CB 不再冗余保存"。

### P3-05（流程/契约风险）R45 报告 ACT 未勾选、CHANGELOG 未登记

- **位置**: `docs/reviews/code_review_R45_2026-10-24.md:257-264`、`CHANGELOG.md`
- **问题**: 8 个 ACT 复选框全为 `- [ ]`，报告未回写收口结论；`CHANGELOG.md` 未收录本批（含公开返回码变更与 CB 布局变更）。与仓库既有惯例（`21cb3bb` 勾选 R43 ACT-3、`3cb83cc` 补登 R39~R43）不一致。
- **修改建议**: 在本批勾选 ACT-1..8 并注记对应的新发现编号；CHANGELOG 增加 `Fixed`（生命周期/并发）与 `Changed`（返回码）两类条目。

### P3-06（断言目标未达成）64B 断言只约束 size，不约束对齐

- **位置**: `core/include/logx_internal.h:45-46`
- **问题**: 实测 `sizeof == 64` 且 `alignof == 8`。断言文案称"fit within a single 64-byte cache line"，但无 `alignas(64)` 时 CB 与相邻堆数据可能共享缓存行（伪共享），且跨行放置。
- **影响范围**: 仅性能语义（CB 是每实例一次分配、热路径只读 `is_closing`/`active_ops`，伪共享影响有限）。
- **修改建议**: 要么把文案改为"sizeof ≤ 64B"（如实描述），要么真正加 `alignas(64)` 并把断言改为 `sizeof == 64 && alignof == 64`（注意与 `new` 的对齐支持）。

### P3-07（防御性分支对称性打磨，当前不可达且无消费方）同步快路径置 `*out_closed_immediately = true` 未放入 `if (target != nullptr)` 块内

- **位置**: `core/src/logx_c.cpp:553-557`
- **问题**: `if (target != nullptr)` 块外无条件置 `*out_closed_immediately = true`。注：由于到达 `:553` 的线程是 `is_closing.exchange(true)` 的唯一胜出者，且此时 `:576` 的 `close_requested.store(true)` 尚未执行，全仓其余 `inst.exchange(nullptr)` 均受 `close_requested.exchange(false)` 保护，故正常生命周期下此处的 `target` 恒非空（`target == nullptr` 为不可达防御分支）；但将其移入 `if (target != nullptr)` 内可与 `:584-587` 兜底双检分支保持严格对称。
- **影响范围**: 无实际运行时后果；属防御性代码对称性与语义严谨性打磨。
- **修改建议**: 仅在 `target != nullptr` 时置 `*out_closed_immediately = true`。

### P3-08（安全目标部分达成）TLS 明文密钥仅"下次 lookup 时"清零

- **位置**: `android/cpp/jni_key_cache.h:46-53`、`logx_jni.cpp:257-259`
- **问题**: 已销毁代数水位只驱动**惰性清扫**；某宿主线程若在实例销毁后不再调用任何 `lookup/allocate_slot`，其 TLS 中的明文密钥（32B×4）将驻留至线程结束。R45-12 描述的"即时清零"仅在活跃线程上成立。
- **影响范围**: 内存取证场景下的密钥暴露窗口（Android 进程内、同进程任意代码可读 TLS）。属已知取舍，但应在契约中写明而非隐含。
- **修改建议**: ① 在 `logx_c.h`/JNI 注释中明确"惰性回收，空闲线程可能长期驻留"；② 若要收紧，可在 `LogX.close()` 的 JNI 侧对"调用线程"显式调用一次清扫（`invalidate_owner`）并对其他线程登记待清扫标记，配合线程退出钩子清理。

---

## 五、验证局限（必读）

1. **未做 Android 交叉编译**：`jni_key_cache.h` / `logx_jni.cpp` 的改动仅在桌面 TU（`test_jni_string.cpp`）验证；NDK/arm64 未复跑，`std::atomic<uint64_t>` 在 32 位 armv7 下的 lock-free 性与 CB 尺寸断言未验证（当前目标为 arm64，风险低）。
2. **未跑动态检测**：未执行 TSAN/ASan。并发正确性依据为"静态全序推演 + 20 轮重复 + 50 线程压力测试"，不等于证明。
3. **未复跑性能门禁**：`logx_get_stats`/`set_error_handler` 新增一次 `active_ops` 原子加减，热路径 bench（P50 ≤ 4.2µs）与 AAR 体积门禁本次**未**重测。
4. **`append_block` 风险等级依赖"当前无生产调用点"这一检索结论**（grep 覆盖 `core/` 与 `tests/`）；若有外部宿主直接链接该符号，P2-01 应升为 P1。
5. **未做撤销对照实验**：本次未逐一"撤销修复确认复红"（P2-03 的恒真断言除外，该项由式面即可判定）。

---

## 六、结论与待收口项

**结论**：`0ef345f` 在生命周期与内存序的核心修复上**方向正确、实现成立、测试绿灯真实**（203 单测 + 4 压力测试，20 轮无抖动），可入库继续迭代。但"12 项全量收口"的表述需修正为 **"7 项成立 / 4 项部分 / 1 项未完成"**。

**建议的收口顺序**：

| 顺序 | 项 | 理由 | 状态 |
|---|---|---|:---:|
| 1 | P2-01 补 `append_block` 门禁 | 与 ACT-6 同类缺陷，改动 3 行，消除潜在 P1 | ✅ 已闭环 |
| 2 | P2-04 消除测试数据竞争 | 影响 CI 稳定性，改动局限在用例 | ✅ 已闭环 |
| 3 | P2-03 换成真实内存断言 | 否则 R45-10 的门禁形同虚设 | ✅ 已闭环 |
| 4 | P2-02 契约与 CHANGELOG 登记 | 公开头显式写明空句柄/陈旧句柄返回码契约 | ✅ 已闭环 |
| 5 | P3-01/02/03/04 | 注释与文档修订、`cb->pal` 原子化，零风险 | ✅ 已闭环 |
| 6 | P3-05/06/07/08 | 流程与语义打磨、`nativeClose` 主动清扫宿主 TLS | ✅ 已闭环 |
