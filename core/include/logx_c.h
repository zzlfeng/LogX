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

    uint32_t max_log_length;     /* 单条日志上限(格式化后);init 校验 < 数据区 - 1。
                                  * 建议 >= 97(用户头 27B + 最大 Tag 64B + 片头 6B);
                                  * 若配置病态小值(< 97),单条写入连空记录都放不下时将触发 DROP / LOGX_ERR_TOO_LARGE */
    uint32_t retention_days;     /* 保留期,过期文件在启动/退后台时清理 */
    uint64_t max_file_size;      /* 单文件上限,默认 10MB,切分决策在 flush 轮次边界 */
    /* E-06: staging 待刷阈值。0 = 禁用按阈值自动唤醒(纯手动 logx_flush 控制);
     * UINT32_MAX = 取默认 8KB;其它值 = 配置字节阈值。 */
    uint32_t flush_threshold;

    /* 编解码:0=none / 1=zstd / 2=zlib */
    uint8_t compress_algo;
    /* 加密总开关(0/1);关闭时新块以明文落盘 */
    uint8_t encrypt_enable;
    /* 当前写入使用的 KID;导出时按文件头 KID 经 key_provider 取对应密钥 */
    uint16_t kid;

    /* (kid)->32B 裸密钥;返回 NULL 表示该 KID 不可解。
     * 实现必须可应答当前及历史 KID; open(校验当前 KID 可用性)与 export(逐块解密)路径均会调用。 */
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
} logx_config_t;

#ifdef __cplusplus
}  /* extern "C" */

/* C++ 侧:不透明句柄即 logx::Instance(定义见 logx_internal.h),实现方可直接转换。 */
namespace logx { struct Instance; }
typedef logx::Instance logx_instance_t;

extern "C" {
#else
typedef struct logx_instance logx_instance_t;
#endif

/* ===== 生命周期 ===== */
logx_status_e logx_open(const logx_config_t* cfg,
                        logx_instance_t** out_inst);

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
    uint64_t truncated_blocks;    /* 启动恢复截尾块计数 */
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