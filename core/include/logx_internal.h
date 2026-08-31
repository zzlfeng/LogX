// logx_internal.h - core 内部组件协作的"小调度中心"。
//
// 该头仅在 core 内部使用:flusher / recovery / export 都要触达 staging 与 codec;
// C ABI 层 logx_c.cpp 持有唯一实例并对外暴露句柄。

#ifndef LOGX_INTERNAL_H
#define LOGX_INTERNAL_H

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "logx_c.h"
#include "logx_codec.h"
#include "logx_format.h"
#include "logx_pal.h"
#include "logx_staging.h"

namespace logx {

class Staging;
class Flusher;

// 单实例引擎;logx_instance_t 在 C ABI 里就是这个指针。
struct Instance {
    // ---- 配置 ----
    std::string log_dir;
    std::string staging_path;
    uint32_t    max_log_length = 0;     // 64KB 默认
    uint32_t    retention_days = 7;
    uint64_t    max_file_size  = 10 * 1024 * 1024;
    uint32_t    flush_threshold = 8 * 1024;
    uint8_t     compress_algo = kAlgoNone;
    bool        encrypt_enable = false;
    uint16_t    kid = 0;
    const uint8_t* (*key_provider)(uint16_t, void*) = nullptr;
    void*       key_user = nullptr;

    // ---- 组件 ----
    Pal*     pal = nullptr;
    void*     mu = nullptr;            // 全局 mutex
    void*     flush_event = nullptr;   // Flusher 唤醒
    void*     flusher_thread = nullptr;
    std::atomic<bool> flusher_exit{false};
    std::atomic<bool> flusher_running{false};

    Staging*  staging = nullptr;
    Flusher*  flusher = nullptr;

    // ---- 错误回调 ----
    logx_error_fn err_fn = nullptr;
    void* err_user = nullptr;

    // ---- 统计 ----
    logx_stats_t stats{};
    std::atomic<uint64_t> drop_no_space{0};
    std::atomic<uint64_t> drop_too_large{0};
    std::atomic<uint64_t> pad_records{0};
    std::atomic<uint64_t> block_write_errors{0};
    std::atomic<uint64_t> truncated_blocks{0};
};

// 触发回调的便捷函数;忽略 null 回调。
inline void emit_event(Instance* inst, logx_event_kind_e k, int code, const char* msg) {
    if (inst->err_fn) {
        inst->err_fn(inst->err_user, k, code, msg ? msg : "");
    }
}

// 格式化一个外部 level(0..5)/tag/payload 为 staging 记录字节序列;
// 失败原因:level 非法 / 超 max_log_length。
// out_buf 由调用方提供(kMaxRecordTotal)。
bool build_records(Instance* inst, int level, const char* tag,
                   const uint8_t* payload, uint32_t payload_len,
                   uint8_t* out_buf, uint32_t* out_total_bytes);

// 把一段连续的"拷出流"按记录边界切成多个块(每块 raw ≤ kBlockMaxNormal,
// 单条 16KB..20KB+6B 头独占一块);返回块数。
//
// out_blocks:每元素 = {raw_offset, raw_len, boundary_record_count}。
// raw 区域下标 [0..total);消费方顺序遍历。
struct PendingBlock {
    uint32_t raw_offset;
    uint32_t raw_len;          // 含 6B 记录头与 LGXP PAD;含块尾 < 6B 缝残留
};
uint32_t split_into_blocks(const uint8_t* raw, uint32_t total,
                           std::vector<PendingBlock>* out);

// 一次"块落地"完整流程的结果(测试可见)。
struct BlockWriteResult {
    bool ok = false;
    uint64_t start_offset = 0;
    uint64_t end_offset = 0;
};
BlockWriteResult write_and_record(Instance* inst, int fd,
                                  const uint8_t* raw_block, uint32_t raw_len,
                                  uint32_t seqno, uint8_t flag,
                                  const uint8_t salt[8],
                                  const uint8_t* key32_or_null);

// 把字节以小端写 4B(测试可见)。
inline void put_u32_le(uint8_t* p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

// 写一个完整块到日志文件;返回成功与否。
bool write_block_to_file(Instance* inst,
                         const uint8_t* raw, const PendingBlock& blk,
                         uint32_t seqno,
                         const uint8_t salt[8], uint8_t flag);

// 一次性 helper:打开(或创建)追加目标文件并追加一个块。
// 返回是否成功,*out_start/*out_end 写块在文件中的字节偏移。
// 此接口是 write_and_record 的"自带 fd 生命周期"封装,便于单元测试。
bool append_block(Instance* inst,
                  const uint8_t* file_salt, uint16_t kid,
                  const uint8_t* raw_block, uint32_t raw_len,
                  uint32_t seqno, uint8_t flag,
                  uint64_t* out_block_start_offset,
                  uint64_t* out_block_end_offset);

// 校验 / 推进 / 截断追加目标文件;返回 S_file(= 最后一个有效块的 seqno)。
// 在 recovery 启动或每天首次 flush 时调用。
struct FileScanResult {
    uint32_t seqno = 0;
    bool     truncated = false;     // 是否发生过截断(为真时续写 seqno 跳 1)
    uint64_t last_block_offset = 0;
    uint64_t last_block_end_offset = 0;
    bool     ok = false;
    std::string path;              // 命中文件路径
};
FileScanResult scan_append_target(Instance* inst, const std::string& hint_path);

// 当前活跃日志文件的路径;按当天日期 + split_no 计算。
std::string current_log_path(Instance* inst, uint32_t yyyymmdd, uint8_t split_no);

// 启动/退出时按保留期清理过期文件。
void retention_cleanup(Instance* inst);

// flush 同步:阻塞至当前 staging 全部落块提交。
void flush_blocking(Instance* inst);

// Flusher 线程入口。
void* flusher_main(void* arg);

// recovery 模块入口:启动时跑一次。
void run_recovery(Instance* inst);

// export:按日期范围拉取并解密写临时文件;明文文件在 callback 后删除。
// 期间 Flusher 继续写新块,但本次 export 的"快照止点" = 调用时刻各文件长度。
struct ExportRequest {
    std::string from;  // yyyyMMdd
    std::string to;
    logx_export_cb cb;
    void* cb_user;
};
logx_status_e export_range(Instance* inst, const ExportRequest& req);

}  // namespace logx
#endif /* LOGX_INTERNAL_H */