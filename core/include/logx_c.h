/* logx_c.h - C ABI for logx core.
 *
 * 阶段约束:本 ABI 只覆盖本期 Android 必需的子集。iOS/Flutter/Windows 走同套
 * C 接口即可,平台壳只做转发,不持逻辑。
 *
 * 线程模型:
 *   - logx_open / logx_close / logx_write / logx_flush / logx_export 可被任意
 *     线程调用;写路径内部互斥,flush 走单一后台线程。
 *   - logx_set_error_handler 与回调运行在调用方线程;错误回调内不得调用除
 *     logx_close 之外的任何 logx 接口。
 *   - logx_close 生命周期契约 (R19 F-2 / R20 复核):close 在错误回调内部或
 *     任一活跃 logx_* 调用栈内被调用时立即返回 LOGX_OK,真实销毁递延到最外层
 *     操作退出(或 Flusher 线程退出)时执行;因此 close 返回后:
 *       ① 句柄立即视为失效,不得再用于任何 logx_* 调用;
 *       ② 已越过入参检查的在途并发写调用可能继续完成写入并发事件——close 的
 *          "此后不再落盘新数据"是递延生效,非调用返回时刻的强同步保证;
 *       ③ 需要"close 返回即无写入"的强顺序时,由调用方自行先停写再 close。
 *
 * 内存契约:
 *   - 所有 out 参数指向调用方提供的缓冲区;失败时 *out = 0/NULL。
 *   - logx_export_callback 内的临时明文文件路径仅在该回调返回前有效。
 */
#ifndef LOGX_C_H
#define LOGX_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 错误码 ===== */
typedef enum {
    LOGX_OK                  =  0,
    LOGX_ERR_INVALID_ARG     = -1,
    LOGX_ERR_STATE           = -2,   /* init 前调用 / 重复 init / 已 close */
    LOGX_ERR_IO              = -3,   /* 文件 / mmap / 刷盘失败 */
    LOGX_ERR_NO_SPACE        = -4,   /* staging 满 */
    LOGX_ERR_TOO_LARGE       = -5,   /* 单条放不下(截断到空记录仍超出病态小 maxLogLength) */
    LOGX_ERR_CODEC           = -6,   /* 编解码失败 */
    LOGX_ERR_NOMEM           = -7,
    LOGX_ERR_NOT_FOUND       = -8,   /* export 时段无文件 */
    LOGX_ERR_INTERNAL        = -9,
    LOGX_ERR_TIMEOUT         = -10,  /* F-08: flush 等待超时 */
} logx_status_e;

/* ===== 错误类型(供回调分类) ===== */
typedef enum {
    LOGX_EV_DEGRADED = 1,   /* 降级态,如 mmap 失败 / 加密失败 / 密钥缺失 */
    LOGX_EV_DROP     = 2,   /* 单条 / 块被丢弃 */
    LOGX_EV_IO       = 3,   /* 磁盘/文件系统错误 */
    LOGX_EV_RECOVERY = 4,   /* 启动恢复告警(截尾/作废重建/丢失计数) */
    LOGX_EV_TRUNCATED = 5,  /* B-3 方案B/T5.6: 超长截断保前缀写入(有损写入成功,
                             * 非丢弃;code 取 LOGX_OK,语义由 kind 承载) */
} logx_event_kind_e;

/* ===== 错误回调 ===== */
/* msg 仅在回调执行期间有效,回调返回后立即失效;当前实现混合使用字符串字面量
 * 与栈缓冲,需留存必须自行拷贝,严禁保存该指针。 */
typedef void (*logx_error_fn)(void* user,
                             logx_event_kind_e kind,
                             int code,
                             const char* msg);

/* ===== 配置 ===== */
typedef struct logx_config {
    const char* log_dir;          /* 正式日志目录,应用私有;NULL = 必填 */
    const char* staging_path;     /* mmap staging 文件绝对路径;NULL = 必填 */

    uint32_t max_log_length;     /* 单条日志上限(格式化后);init 校验 >= 97 且 < 数据区 - 1。
                                  * 必须 >= 97(用户头 27B + 最大 Tag 64B + 片头 6B);
                                  * 低于 97 属于病态小配置(连空记录都无法容纳),init 期直接返回 LOGX_ERR_INVALID_ARG。 */
    uint32_t retention_days;     /* 保留期,过期文件在启动/退后台时清理 */
    uint64_t max_file_size;      /* 单文件上限,默认 10MB,切分决策在 flush 轮次边界 */
    /* E-06: staging 待刷阈值。0 = 禁用按阈值自动唤醒(纯手动 logx_flush 控制);
     * UINT32_MAX = 取默认 8KB;其它值 = 配置字节阈值。 */
    uint32_t flush_threshold;
    /* B-1 方案 A: 环形暂存区满 (NO_SPACE) 时的有界阻塞等待上限 (毫秒)。
     * 0 = 保持默认绝不阻塞 (立即丢弃并返回 LOGX_ERR_NO_SPACE);
     * > 0 = 业务线程在锁外有界退避重试，直至 Flusher 腾出空间或超时。 */
    uint32_t staging_wait_ms;

    /* 编解码:0=none / 1=zstd / 2=zlib */
    uint8_t compress_algo;
    /* 加密总开关(0/1);关闭时新块以明文落盘 */
    uint8_t encrypt_enable;
    /* 当前实例写入使用的默认 KID(单实例生命周期内不可变)。新建文件以此 KID 写入文件头;
     * 续写既有文件时,以既有文件头声明的 KID 为准(F-12: 保证单文件内 KID 100% 统一,
     * 若文件头 KID 与当前实例配置 KID 不一致,以文件头 KID 派生密钥并发出 DEGRADED 告警)。
     * 运行期动态多 KID 轮换契约与管理规划于 M6 里程碑专项落地。 */
    uint16_t kid;

    /* (kid)->32B 裸密钥;返回 NULL 表示该 KID 不可解。实现必须可应答当前及历史 KID。
     * 三类调用点(R21 N-1 补全,此前仅记了前两类):
     *   ① open: 校验当前 KID 可用性,返回 NULL 即 fail-fast 拒绝启动;
     *   ② 建文件/打开追加目标: 探测本文件的加密能力并**锁定**能力位——
     *      新建文件: 探测为 NULL 时文件头不置加密位,该文件此后所有块一律明文落盘
     *      (即便密钥稍后恢复也不再改口),直至按天/按大小轮换到新文件;
     *      续写已存在加密文件: 打开瞬间缺钥仅当期降级明文,此后每轮 drain 至多重试
     *      一次,密钥恢复后立即锁定并恢复密文落盘;
     *      两种情形均发 LOGX_EV_DEGRADED 事件。
     *      注意此调用点运行在 **Flusher 后台线程**(JNI 实现须自行 attach/detach);
     *   ③ export: 按文件头 KID 逐块解密。
     * J-6 规约: 返回的指针由实现方持有,调用方不释放、不改动其内容;库侧在单文件追加生命周期内缓存该指针
     * (②与③的频率均为"每文件一次",仅在缺钥降级态下每轮重试),实现方须保证返回指针持续有效、
     * 内容稳定,且不同 kid 必须拥有互不覆盖的独立存储区域(如 JNI thread_local 多槽缓存,
     * 当前实现支持单线程并发活跃不同 kid 数量 ≤ 4, 更大规模动态多 kid 轮换规划于 M6 专项落地),
     * 直至下一次同 kid 刷新或实例关闭。 */
    const uint8_t* (*key_provider)(uint16_t kid, void* user);

    void* user;                  /* 透传给 key_provider */

    /* 错误回调(可选);若提供,启动恢复阶段事件亦会通知 */
    logx_error_fn err_fn;
    void* err_user;              /* 透传给 err_fn */

    /* (T-04) 测试专用 PAL 注入 seam;生产代码必须传 NULL。
     * logx_open 在启动 Flusher 线程之前把它赋给 inst->pal,
     * 从根源消除"先 open 后改 inst->pal"造成的 Flusher 读 / 测试写数据竞争。
     * 非 NULL 时要求 Pal 对象存活期覆盖整个实例生命周期。 */
    void* pal_override;          /* 实际类型 logx::Pal*;仅测试构建使用 */

    /* J-1/F-2 / R45-12 / R46 P3-03: 实例完全销毁时的清理回调(可选,由 destroy_instance 最终退出前调用)。
     * 无论同步还是递延销毁,该回调保证在 Flusher 主循环已退出(flusher_loop 已完成尾轮刷盘与同步并退出)、
     * staging 已关闭、库内所有对 user/err_user 的访问全部终结后触发。
     * 线程模型约束: 执行线程为最后完成销毁认领的线程(可能是调用 logx_close 的线程、
     * 最后退出的并发操作线程或正在退出途中的 Flusher 后台线程),实现方不得依赖回调线程身份,
     * 亦不得仅依赖在 on_destroy 内清理跨线程 thread_local 状态(R46 P3-08: 跨线程 TLS 缓存如 JNI 密钥槽
     * 采用调用方线程主动清扫 + 全局已销毁代数水位惰性清扫策略,销毁后不再触发回调的完全空闲线程其 TLS
     * 槽位将保留至下次探测或线程终结)。
     * 若未提供,则由调用方自行根据 logx_close_ex 返回值或生命周期纪律管理。 */
    void (*on_destroy)(void* user, void* err_user);
} logx_config_t;

#ifdef __cplusplus
}  /* extern "C" */

/* C++ 侧: 严格不透明句柄类型声明，编译期阻断裸指针强转 (R44-06) */
struct logx_instance;
typedef struct logx_instance logx_instance_t;

extern "C" {
#else
typedef struct logx_instance logx_instance_t;
#endif

/* ===== 生命周期 ===== */
/* 空句柄/陈旧句柄返回码契约 (R45-04 / R46 P2-02):
 *   - logx_open: out_inst == NULL 或 cfg 必填项非法返回 LOGX_ERR_INVALID_ARG;
 *   - logx_close / logx_close_ex: inst == NULL 返回 LOGX_ERR_INVALID_ARG;已关闭的陈旧墓碑句柄幂等返回 LOGX_OK;
 *   - logx_write / logx_flush / logx_export / logx_set_error_handler: inst == NULL 或已关闭墓碑句柄统一返回 LOGX_ERR_STATE;
 *   - logx_get_stats: inst == NULL 或已关闭墓碑句柄时将 *out 清零返回。 */
logx_status_e logx_open(const logx_config_t* cfg,
                        logx_instance_t** out_inst);

/* J-1/F-2: 带同步/递延状态反馈的关闭接口。
 * 若提供 out_closed_immediately,当且仅当实例在当前调用中已完成同步销毁(Flusher 已 join,
 * 内存已释放)时置 true;若因处于 Flusher 线程或活跃回调/调用栈而进入递延销毁,则置 false。
 * 调用方(如 JNI)若收到 false,不得立即释放由 key_provider/err_fn 引用的底层上下文。 */
logx_status_e logx_close_ex(logx_instance_t* inst, bool* out_closed_immediately);

logx_status_e logx_close(logx_instance_t* inst);

/* ===== 写入 ===== */
/* 单条日志写入;level ∈ {0..5};tag/payload UTF-8。
 * tag 必须是 NUL 终止的 C 字符串且有效长度 ≤ 64B(无长度参数,按 C 字符串契约
 * 扫描;恰好 64B 非常终止的 buffer 属未定义输入,将被拒绝);payload 不需要
 * NUL 结尾,长度由 payload_len 显式给出。
 * 返回 LOGX_ERR_NO_SPACE 表示 staging 满(LOGX_EV_DROP)。
 * B-1B 方案B: 超 maxLogLength 的日志截断保留前缀后照常写入并返回 LOGX_OK,
 * 以 LOGX_EV_TRUNCATED 事件(code 取 LOGX_OK,kind 承载语义)与 truncated_logs
 * 计数上报;LOGX_ERR_TOO_LARGE 返回值仅表示"截断到空记录仍放不下"的病态小
 * maxLogLength(事件仍为 LOGX_EV_DROP / LOGX_ERR_TOO_LARGE)。 */
logx_status_e logx_write(logx_instance_t* inst,
                         int level,
                         const char* tag,
                         const uint8_t* payload, uint32_t payload_len);

/* 阻塞至 staging 当前内容全部落块提交。退后台 / onStop 调用。 */
logx_status_e logx_flush(logx_instance_t* inst);

/* ===== 导出 ===== */
/* from/to = yyyyMMdd(本地时区,闭区间);内部先 flush,以调用时刻各文件长度为
 * 快照止点(期间 Flusher 增量不计入本次导出);临时明文文件在 callback 返回后删除。
 * callback 返回 0 表示已成功接收并取走,非 0 表示业务侧处理失败(临时文件仍会删除)。 */
typedef int (*logx_export_cb)(void* user,
                              const char* date,           /* yyyyMMdd */
                              const char* plaintext_path,
                              uint64_t size);
logx_status_e logx_export(logx_instance_t* inst,
                          const char* from_yyyymmdd,
                          const char* to_yyyymmdd,
                          logx_export_cb cb, void* cb_user);

/* ===== 错误回调注册 ===== */
logx_status_e logx_set_error_handler(logx_instance_t* inst,
                                     logx_error_fn fn, void* user);

/* ===== 诊断 ===== */
typedef struct logx_stats {
    /* E-07: ABI 向前兼容。结构体按值经出参拷贝,新增统计指标只能追加在
     * reserved 区,替换保留位会破坏既有二进制(老调用方按旧 sizeof 分配的
     * 缓冲被新实现越界写)。新增字段一律占用 reserved 并同步递减其数量。 */
    uint64_t drop_no_space;      /* staging 空闲不足丢弃 */
    uint64_t drop_too_large;     /* 单条超 maxLogLength 丢弃(B-1B 后仅病态小上限路径) */
    uint64_t truncated_blocks;    /* 启动恢复截尾块计数。口径注记(B-8 方案C): 计数
                                     语义为 "staging Header 已提交序号与目标文件尾块
                                     序号的缺口", 在 Header 撕裂等场景下该缺口不必然
                                     等于真实丢块数, 仅作诊断参考, 非精确 SLA 指标 */
    uint64_t pad_records;        /* 回绕垫 PAD 数 */
    uint64_t block_write_errors; /* write 短写/IO 错误计数 */
    uint64_t truncated_logs;     /* B-1B: 超长截断保前缀写入计数(占原 reserved[0]) */
    uint64_t reserved[3];        /* 预留给未来统计指标,必须保持全 0 */
} logx_stats_t;

void logx_get_stats(logx_instance_t* inst, logx_stats_t* out);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* LOGX_C_H */