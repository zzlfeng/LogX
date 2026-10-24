# LogX 1.x 生命周期堆控制块（Control Block）重构实施计划 (T5.8 / R41-05 根治)

> **版本**: v1.2 (2026-10-24)  
> **任务编号**: T5.8 (登记于 `docs/LogX后续实施计划表.md` §2 阶段三任务表)  
> **当前状态**: ✅ 已按 R45 审查结论完成修复并对齐实装代码  
> **关联裁决**: [`lifecycle_control_block_decisions.md`](file:///d:/Project/Android_Project/LogX/logx/docs/lifecycle_control_block_decisions.md) v1.2 (6 大决策项正式拍板)  
> **关联审查**: [`reviews/code_review_R44_2026-10-18.md`](file:///d:/Project/Android_Project/LogX/logx/docs/reviews/code_review_R44_2026-10-18.md)、[`reviews/code_review_R45_2026-10-24.md`](file:///d:/Project/Android_Project/LogX/logx/docs/reviews/code_review_R45_2026-10-24.md)  
>
> **v1.1 关键修订摘要（相比 v1.0）**：
> 1. **根治 R44-01（尾读 UAF）**：采纳 **CB 墓碑模型 (Tombstone)**，实例销毁仅物理释放 `Instance` 数据主体，控制块 CB 永久保留。彻底消除裸句柄在入口首行解引用已释放内存的 UAF 窗口；顺带消除 `weak_count` 及其配套复杂拓扑。
> 2. **根治 R44-02（关停丢数据）**：采纳**强引用 + `active_ops` 静止判定（含回调在飞门禁）**，同步关停判定严格为 `active_ops == 0 && in_callback == 0`，一次判定非零即递延，绝不自旋等待。同步路径先 `flush_blocking` 排空再停转 Flusher；递延路径由收尾者协同 Flusher 完成最终落盘与认领，杜绝在途日志静默丢失。
> 3. **闭合 R44-03 / R44-07（协议与启动引用）**：`cb->inst` 采用 `std::atomic<Instance*>` 并在读写侧建立 acquire-release 配对；销毁认领采用 `inst.exchange(nullptr)` 唯一胜出语义；open 初始引用清晰归属，启动失败路径完整回收。
> 4. **解决 R44-04（关闭期事件）**：在墓碑模型下，只要 `inst != nullptr` 允许关闭期事件正常分发；关停协议感知 `in_callback > 0` 递延，规避 R43 死锁形态。
> 5. **解决 R44-05（Flusher 自毁泄漏）**：扩展 PAL 接口增加 `thread_detach`，Flusher 回调内自销毁时 detach 自身，根治原生线程句柄泄漏。
> 6. **解决 R44-06（句柄错转型与迁移漏项）**：C++ 内部声明不透明句柄类型，引入 `test_accessor.h` 穿透测试 seam，批量迁移 28 处强转与 7 处状态访问；`logx_set_error_handler` 与 `logx_get_stats` 补齐生命周期守卫。
> 7. **对齐 R44-12（基线与门禁）**：单测基线勘误为实测 **196 项**；锁定真机热路径 P50 **门禁线 ≤ 4.2µs**（成功线 4.0µs，对齐 B2 回归基线 3.958µs）。

---

## 一、 重构背景与核心目标

### 1.1 现状分析与痛点 (R41-05 的本质)
在当前 LogX 实现中，引擎生命周期与关闭时序由内嵌在 `Instance` 结构体中的 4 个原子状态量协同控制：
1. `active_ops`: 记录当前正在执行的外部 API 操作（`logx_write` / `logx_flush` / `logx_export`）；
2. `in_callback`: 记录当前正在执行的错误回调调用深度（`emit_event`）；
3. `close_pending`: 记录外部线程触发但因存在活跃操作而转入递延关闭的标志；
4. `close_on_flusher`: 记录 Flusher 线程内部触发或转置自毁的标志。

**存在的核心痛点**：
- **极高的并发心智负担**：为协调这 4 个变量，退出逻辑分散在 5 个关键路径（`logx_close_ex` 外部主干、`logx_close_ex` 内部双检兜底、`OpGuard::~OpGuard` 退出段、`emit_event` 退出段、`flusher_main` 循环退出段）。在 R41~R43 期间连续修复了 Dekker store-load 重排、Flusher 回调盲区、递延漏认领等隐蔽并发缺陷。这套机制维护极为脆弱；
- **理论“尾读 UAF”隐患 (R41-05)**：所有状态标志均位于 `Instance` **内部**。当胜出线程判定条件满足并调用 `destroy_instance(inst)` 执行 `delete inst` 后，败方线程在此微秒级间隙仍在执行 `fetch_sub` 后的指令（如寄存器加载与栈清理）。虽然通过严格的 Dekker 屏障可以防止其触碰业务数据，但在严格的 C++ 内存模型下，对已释放 `inst` 指针的任何窥探都属于未定义行为（UB）。
- **v1.0 初稿的局限**：初稿试图通过 weak 计数管辖 CB，但外部传入的裸句柄在入口首行解引用 CB 时尚未持有任何引用，UAF 只是从 `Instance` 搬到了 CB（R44-01）。

### 1.2 重构目标
1. **解耦生命周期元数据与实例主体数据**：引入独立的堆控制块（`InstanceControlBlock`），其生命周期超越 `Instance` 主体，且采用**墓碑模型（Tombstone）**，彻底根治 R41-05 尾读 UAF；
2. **状态机大幅收敛**：废弃 `close_pending`、`close_on_flusher` 两态纠缠与双检 Dekker 屏障，统一由 CB 的原子 `is_closing` 拦截、`active_ops` 与 `in_callback` 精确静止判定、以及 `inst.exchange(nullptr)` 唯一胜出认领收敛；
3. **零破损 C ABI / JNI 契约**：`logx_instance_t*` 依然维持不透明指针形态；JNI 层与 Android Kotlin Facade 零改动；C++ 源码级改用严格不透明类型阻断危险强转；
4. **性能极致保全**：热路径（`logx_write`）零多余堆分配，**仅维护单次原子在途计数加减**（绝不增加双重原子引用计数），确保真机 P50 满足 ≤ 4.2µs 严苛门禁。

---

## 二、 核心架构设计方案

### 2.1 控制块墓碑模型与内存拓扑

```mermaid
graph TD
    subgraph "External Handles & Operations"
        H["logx_instance_t* (Caller Raw Pointer)"] -->|Opaque Ptr| CB
        OG["OpGuard (Write/Flush/Export)"] -->|fetch_add active_ops| CB
        EG["emit_event (Callback)"] -->|fetch_add in_callback| CB
    end

    subgraph "Control Block (Heap - Tombstone)"
        CB["InstanceControlBlock (Permanent Tombstone)<br/>- is_closing: atomic<bool><br/>- active_ops: atomic<int><br/>- in_callback: atomic<int><br/>- flusher_tid: atomic<uint64_t><br/>- inst: atomic<Instance*>"]
    end

    subgraph "Engine Data Subject (Disposable Heap)"
        INST["Instance Engine Data<br/>(staging, pal, mu, flusher)"]
    end

    CB -->|owns atomic ptr| INST

    style CB fill:#e1f5fe,stroke:#0288d1,stroke-width:2px
    style INST fill:#fff3e0,stroke:#f57c00,stroke-width:2px
```

- **墓碑模型（Tombstone）核心语义**：
  - `InstanceControlBlock` 在 `logx_open` 成功时分配一次；
  - 当实例被 `logx_close` 关闭并最终销毁时，**仅物理释放 `Instance` 数据主体**，并将 `cb->inst` 原子置空（`exchange(nullptr)`）；
  - **CB 结构体自身永久保留在堆中**，永不 `delete cb`；
  - 外部无论何时传入陈旧裸句柄，首行读取 `cb->is_closing` 或 `cb->inst` 永远是合法内存访问，绝对消灭 heap-use-after-free 崩溃窗口；
  - 顺带彻底消除内存地址快速复用导致的 ABA 假句柄隐患；
  - 单个 CB 结构体约 56B，计入分配器元数据约 80~100B，在移动端单实例/低频实例形态下开销可完全忽略。

### 2.2 数据结构定义

在 [`core/include/logx_internal.h`](file:///d:/Project/Android_Project/LogX/logx/core/include/logx_internal.h) 中定义：

```cpp
namespace logx {

struct Instance;

// 生命周期堆控制块（墓碑模型，sizeof <= 64B）
struct InstanceControlBlock {
    std::atomic<bool>      is_closing{false};      // 关闭中标志 (阻断新进入者)
    std::atomic<bool>      close_requested{false}; // 外部已发起 close (递延销毁触发源)
    std::atomic<int>       active_ops{0};          // 在途外部操作计数 (write/flush/export/stats/set_err)
    std::atomic<int>       in_callback{0};         // 在途回调计数 (emit_event / key_provider)
    std::atomic<uint64_t>  flusher_tid{0};         // Flusher 线程 TID (用于自销毁检测)

    std::atomic<Instance*> inst{nullptr};          // 实例主体指针 (原子指针，负责认领销毁)
    Pal*                   pal{nullptr};           // 平台抽象层指针

    // 销毁回调与上下文 (J-1/F-2 / R45-12 契约)
    void (*on_destroy)(void* user, void* err_user){nullptr};
    void*                  on_destroy_key_user{nullptr};
    void*                  on_destroy_err_user{nullptr};
};

static_assert(sizeof(InstanceControlBlock) <= 64,
              "InstanceControlBlock must fit within a single 64-byte cache line");

// 统一释放 Instance 资源主体 (仅由 inst.exchange(nullptr) 胜出者调用一次)
void destroy_instance(InstanceControlBlock* cb, Instance* inst, bool is_flusher_thread);

} // namespace logx
```

同时，从 `Instance` 结构体中**彻底移除**以下废弃字段：
- ~~`std::atomic<int> active_ops;`~~（改由 `cb->active_ops` 承载）
- ~~`std::atomic<int> in_callback;`~~（改由 `cb->in_callback` 承载）
- ~~`std::atomic<bool> closing;`~~（改由 `cb->is_closing` 承载）
- ~~`std::atomic<bool> close_pending;`~~（彻底删除）
- ~~`std::atomic<bool> close_on_flusher;`~~（彻底删除）

`Instance` 内部保留反向引用：`InstanceControlBlock* cb{nullptr};`。

---

## 三、 关键路径重构逻辑

### 3.1 实例创建 (`logx_open`) 失败路径与清理清单
1. 分配 `InstanceControlBlock* cb = new (std::nothrow) InstanceControlBlock();`；
2. 分配 `Instance* inst = new (std::nothrow) Instance();`，若失败则 `delete cb; return LOGX_ERR_NO_MEM;`；
3. 将 `inst->cb = cb; cb->inst.store(inst, std::memory_order_relaxed);`；
4. 初始化 `cb->pal = pal; cb->on_destroy = cfg->on_destroy;` 等元数据；
5. 初始化 mutex、events、staging mmap 与 recovery；
   - **失败清理清单**：若在 `thread_start` 之前的任一步骤失败（如 staging 创建失败、event 创建失败），必须按照依赖逆序释放 `staging`/`events`/`mutex`/`inst`，最后 `delete cb`（此时尚未向外部暴露句柄，无并发，可安全释放 CB），并返回对应错误码；
6. 启动 Flusher 线程：`cb->flusher_thread = inst->flusher_thread;`；
   - 若 `thread_start` 失败，执行完整资源清理，`delete inst; delete cb; return LOGX_ERR_OS;`；
7. 成功后将 `*out_inst = reinterpret_cast<logx_instance_t*>(cb);` 返回给外部。

### 3.2 写入/刷盘/导出/统计/设置错误回调热路径 (`OpGuard`)
为保证真机 P50 写入延迟 ≤ 4.2µs 门禁，**热路径仅维护一次 `active_ops` 原子加减**，并在跨变量 Store-Load 对偶点使用 `std::memory_order_seq_cst` 全序屏障（R45-01 / R45-02）：
```cpp
struct OpGuard {
    InstanceControlBlock* cb{nullptr};
    Instance*             inst{nullptr};

    explicit OpGuard(logx_instance_t* h) {
        if (!h) return;
        auto* c = reinterpret_cast<InstanceControlBlock*>(h);
        if (c->is_closing.load(std::memory_order_acquire)) return;

        c->active_ops.fetch_add(1, std::memory_order_seq_cst);
        if (c->is_closing.load(std::memory_order_seq_cst)) {
            int rem = c->active_ops.fetch_sub(1, std::memory_order_seq_cst) - 1;
            if (rem == 0) {
                check_deferred_dispose_on_exit(c);
            }
            return;
        }

        Instance* i = c->inst.load(std::memory_order_acquire);
        if (!i) {
            int rem = c->active_ops.fetch_sub(1, std::memory_order_seq_cst) - 1;
            if (rem == 0) {
                check_deferred_dispose_on_exit(c);
            }
            return;
        }

        cb = c;
        inst = i;
    }

    ~OpGuard() {
        if (cb) {
            int rem = cb->active_ops.fetch_sub(1, std::memory_order_seq_cst) - 1;
            if (rem == 0 && cb->is_closing.load(std::memory_order_seq_cst)) {
                check_deferred_dispose_on_exit(cb);
            }
        }
    }

    bool is_valid() const noexcept { return inst != nullptr; }
};
```
在 `logx_write` / `logx_flush` / `logx_export` / `logx_set_error_handler` / `logx_get_stats` 中统一使用 `OpGuard` 保护（R44-06 / R45-04）。

### 3.3 错误事件发射 (`emit_event`) 与密钥回调门禁 (`invoke_key_provider`)
允许关闭期事件正常穿透分发，并对 `emit_event` 与 `key_provider` 对称维护 `in_callback`（R44-04 / R45-02 / R45-06）：
```cpp
inline void emit_event(Instance* inst, logx_event_kind_e k, int code, const char* msg) {
    if (!inst) return;
    InstanceControlBlock* cb = inst->cb;
    if (cb) {
        cb->in_callback.fetch_add(1, std::memory_order_seq_cst);
    }
    inst->err_mu.lock();
    logx_error_fn fn = inst->err_fn;
    void* user = inst->err_user;
    inst->err_mu.unlock();
    if (fn) {
        fn(user, k, code, msg ? msg : "");
    }
    if (cb) {
        int rem = cb->in_callback.fetch_sub(1, std::memory_order_seq_cst) - 1;
        if (rem == 0 && cb->is_closing.load(std::memory_order_seq_cst)) {
            check_deferred_dispose_on_exit(cb);
        }
    }
}

inline const uint8_t* invoke_key_provider(Instance* inst, uint16_t kid) {
    if (!inst || !inst->key_provider) return nullptr;
    InstanceControlBlock* cb = inst->cb;
    if (cb) {
        cb->in_callback.fetch_add(1, std::memory_order_seq_cst);
    }
    const uint8_t* key = inst->key_provider(kid, inst->key_user);
    if (cb) {
        int rem = cb->in_callback.fetch_sub(1, std::memory_order_seq_cst) - 1;
        if (rem == 0 && cb->is_closing.load(std::memory_order_seq_cst)) {
            check_deferred_dispose_on_exit(cb);
        }
    }
    return key;
}
```

### 3.4 实例关闭 (`logx_close_ex` / `logx_close`)
严格遵循 **决策二方案 B（`active_ops == 0 && in_callback == 0` 静止判定 + R45-02/R45-03 修复）**：
```cpp
logx_status_e logx_close_ex(logx_instance_t* h, bool* out_closed_immediately) {
    if (out_closed_immediately) *out_closed_immediately = false;
    if (!h) return LOGX_ERR_INVALID_ARG;
    auto* cb = reinterpret_cast<InstanceControlBlock*>(h);

    if (cb->is_closing.exchange(true, std::memory_order_seq_cst)) {
        return LOGX_OK;
    }

    Instance* inst = cb->inst.load(std::memory_order_acquire);
    Pal* pal = cb->pal;
    if (inst && inst->space_event && pal) {
        pal->ev_signal(inst->space_event);
    }

    uint64_t current_tid = pal ? pal->tid() : 0;
    bool on_flusher = (current_tid != 0 &&
                       cb->flusher_tid.load(std::memory_order_acquire) == current_tid);

    if (!on_flusher &&
        cb->active_ops.load(std::memory_order_seq_cst) == 0 &&
        cb->in_callback.load(std::memory_order_seq_cst) == 0) {
        Instance* target = cb->inst.exchange(nullptr, std::memory_order_acq_rel);
        if (target != nullptr) {
            destroy_instance(cb, target, /*is_flusher_thread=*/false);
        }
        if (out_closed_immediately) *out_closed_immediately = true;
        return LOGX_OK;
    }

    if (on_flusher && inst &&
        cb->active_ops.load(std::memory_order_seq_cst) == 0 &&
        cb->in_callback.load(std::memory_order_seq_cst) == 0) {
        inst->flusher_exit.store(true, std::memory_order_release);
        if (inst->flush_event && pal) pal->ev_signal(inst->flush_event);
    }

    cb->close_requested.store(true, std::memory_order_seq_cst);

    if (!on_flusher &&
        cb->active_ops.load(std::memory_order_seq_cst) == 0 &&
        cb->in_callback.load(std::memory_order_seq_cst) == 0) {
        if (cb->close_requested.exchange(false, std::memory_order_seq_cst)) {
            Instance* target = cb->inst.exchange(nullptr, std::memory_order_acq_rel);
            if (target != nullptr) {
                destroy_instance(cb, target, /*is_flusher_thread=*/false);
                if (out_closed_immediately) *out_closed_immediately = true;
            }
        }
    }

    return LOGX_OK;
}
```

### 3.5 递延排空与销毁认领 (`check_deferred_dispose_on_exit`)
当在途操作或回调归零时协同认领（R45-02 / R45-03 / R45-09）：
```cpp
inline void check_deferred_dispose_on_exit(InstanceControlBlock* cb) {
    if (!cb) return;
    if (!cb->close_requested.load(std::memory_order_seq_cst)) return;
    if (cb->active_ops.load(std::memory_order_seq_cst) == 0 &&
        cb->in_callback.load(std::memory_order_seq_cst) == 0) {
        Pal* pal = cb->pal;
        uint64_t current_tid = pal ? pal->tid() : 0;
        bool on_flusher = (current_tid != 0 &&
                           cb->flusher_tid.load(std::memory_order_acquire) == current_tid);
        if (!on_flusher) {
            if (cb->close_requested.exchange(false, std::memory_order_seq_cst)) {
                Instance* target = cb->inst.exchange(nullptr, std::memory_order_acq_rel);
                if (target != nullptr) {
                    destroy_instance(cb, target, /*is_flusher_thread=*/false);
                }
            }
        } else {
            Instance* inst = cb->inst.load(std::memory_order_acquire);
            if (inst) {
                inst->flusher_exit.store(true, std::memory_order_release);
                if (inst->flush_event && pal) pal->ev_signal(inst->flush_event);
            }
        }
    }
}
```

### 3.6 统一物理清理 (`destroy_instance`)
由 `inst.exchange(nullptr)` 唯一胜出者调用：
```cpp
void destroy_instance(InstanceControlBlock* cb, Instance* inst, bool is_flusher_thread) {
    if (!inst) return;
    Pal* pal = cb ? cb->pal : inst->pal;
    if (!is_flusher_thread) {
        flush_blocking(inst);
    }
    inst->flusher_exit.store(true, std::memory_order_release);
    if (inst->flush_event && pal) pal->ev_signal(inst->flush_event);
    if (inst->space_event && pal) pal->ev_signal(inst->space_event);
    if (inst->flusher_thread) {
        if (!is_flusher_thread) {
            if (pal) pal->thread_join(inst->flusher_thread);
        } else {
            if (pal) pal->thread_detach(inst->flusher_thread);
        }
        inst->flusher_thread = nullptr;
    }
    delete inst->staging;
    inst->staging = nullptr;
    if (inst->space_event && pal) { pal->ev_destroy(inst->space_event); inst->space_event = nullptr; }
    if (inst->flush_event && pal) { pal->ev_destroy(inst->flush_event); inst->flush_event = nullptr; }
    if (inst->mu && pal) { pal->mutex_destroy(inst->mu); inst->mu = nullptr; }
    auto on_destroy = cb ? cb->on_destroy : nullptr;
    void* key_user = cb ? cb->on_destroy_key_user : inst->key_user;
    void* err_user = cb ? cb->on_destroy_err_user : inst->err_user;
    delete inst;
    if (cb) {
        cb->flusher_tid.store(0, std::memory_order_release);
        cb->pal = nullptr;
    }
    if (on_destroy) {
        on_destroy(key_user, err_user);
    }
}
```

---

## 四、 新旧状态机对比与收益确认

| 维度 | 旧架构 (Dekker 多状态原子仲裁) | 新架构 (CB 墓碑模型 + 精确静止) |
|---|---|---|
| **状态量个数** | 4 个 (`active_ops`, `in_callback`, `close_pending`, `close_on_flusher`) | **CB 永久留存 + `is_closing` + `active_ops` + `in_callback`** |
| **内存屏障复杂度** | 全链路强制要求 `seq_cst` Store-Load 屏障与双重校验 | 标准的 `acquire` / `release` 内存序 |
| **认领点位置** | 分散在 5 处退出段，逻辑各异容易漏补 | **收敛至 `try_deferred_dispose` 配合 `inst.exchange(nullptr)`** |
| **尾读 UAF (R41-05)** | 理论存在（败方在胜方 `delete inst` 后访问其字段） | **彻底根除**（控制块墓碑永久存活，指针解引用合法） |
| **Flusher 自毁泄漏** | 无法 join 导致 OS 原生线程句柄泄漏 | **彻底根除**（PAL `thread_detach` 原生解耦回收） |
| **热路径性能** | 存在 check-then-increment 窗口，Dekker 屏障开销 | **单次原子操作提升**，零额外堆开销，延迟 ≤ 4.2µs |

---

## 五、 测试与回归验证计划

1. **测试基线核准**：
   - 实测单元测试基线确认为 **196 项**（clean 构建产物 `--gtest_list_tests` 实数）；
2. **桌面端 MSVC x64 全量验证**：
   - 必须遵循 **R21-2** 纪律：头文件变更强制 clean 全量编译，达到 **0 警告 0 错误**；
   - CTest 四套件 **100% 通过 (4/4)**：
     - `logx_unit_tests` (196 项)
     - `logx_hotpath_alloc_tests` (2 项零堆分配守护断言)
     - `logx_stress_tests` (4 项 50 线程极限并发压测)
     - `logx_crash_fork_tests` (3 项真实进程崩溃恢复对账)
3. **Android NDK 交叉编译验证**：
   - 必须遵循 **B-11-1** 纪律：NDK 26.1 / 29.0 Clang clean 编译，达到 **61/61 TU 0 警告**；
4. **Android 真机 (Redmi K30 Pro) 全量复跑**：
   - 通过无线 adb 部署四套件至设备，断言全绿；
   - 热路径写入性能门禁：P50 **门禁线 ≤ 4.2µs**（成功线 4.0µs）；
5. **专项测试补齐**：
   - 陈旧句柄访问测试（返回 `LOGX_ERR_STATE` 且不崩溃）；
   - 并发双 close 压力测试；
   - 10k 轮 open/close 墓碑内存上界断言（验证内存平稳有界）；
   - `thread_detach` 句柄配对断言。

---

## 六、 分阶段实施计划 (WBS)

```mermaid
sequenceDiagram
    autonumber
    participant C1 as Phase 1 (机械迁移)
    participant C2 as Phase 2 (协议替换)
    participant V as Phase 3 (全量回归)

    Note over C1: 编译期强类型阻断 + 测试 Accessor 迁移 + 守卫补齐
    C1->>C1: 落地 InstanceControlBlock 基础定义与不透明句柄
    C1->>C1: tests/ 引入 test_accessor.h, 机械替换 28 处强转与 7 处状态访问
    C1->>C1: 补齐 logx_set_error_handler / logx_get_stats 生命周期守卫
    C1->>C1: 保持旧并发状态机, 验证 MSVC clean 196/196 全绿

    Note over C2: 协议替换 + 拆除 Dekker 屏障 + PAL detach
    C2->>C2: PAL 扩展 thread_detach (Win32/POSIX/CountingPal)
    C2->>C2: 引入 CB 墓碑与 inst.exchange(nullptr) 销毁认领
    C2->>C2: 拆除 close_pending / close_on_flusher 与 Dekker 屏障
    C2->>C2: 补齐并发 close、陈旧句柄、10k 轮墓碑内存上界单测

    Note over V: 双端回归 + 性能门禁 + 收口
    V->>V: MSVC clean 0 警告 + CTest 4/4
    V->>V: NDK Clang clean 61/61 0 警告
    V->>V: Android 真机四套件部署复跑全绿 + P50 ≤ 4.2µs
    V->>V: 更新 CHANGELOG.md 与实施计划表
```
