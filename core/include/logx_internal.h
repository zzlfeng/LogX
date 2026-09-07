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
    std::atomic<int> active_log_fd{-1};

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

// 生成日志文件路径: split=0 为 dir/YYYYMMDD.log, split>0 为 dir/YYYYMMDD.log.N
std::string make_log_path(const std::string& dir, uint32_t yyyymmdd, uint8_t split);

// 解析日志文件路径或文件名: 提取 8 位日期与切分序号 (0..255)。
// 支持 "YYYYMMDD.log" (split=0) 与 "YYYYMMDD.log.N" (split=N)。
// 成功返回 true; 其它文件返回 false。
bool parse_log_filename(const std::string& path_or_name, uint32_t* out_date, uint8_t* out_split);

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
    // 对账用:k 块回溯过程中逐块累加的 raw_len 总和
    // (= seqno ∈ (S_hdr, S_file] 各块 raw_len 之和;仅 S_file > S_hdr 分支消费)。
    // scan 从缓存命中点续扫时只累加新读到的块,缓存命中块本身不在此列,
    // 因此 recovery 用 last_block_offset + atc.last_block_end_offset 区间信息补齐。
    uint64_t bytes_since_hint = 0;
    bool     ok = false;
    std::string path;              // 命中文件路径
};
// 校验 34B 块头字段合理性 (F-05)。
// 校验项: magic、ver=1、flag 保留位全 0、压缩算法有效性(且与编译宏匹配)、
// seqno 严格单调递增 (seqno > prev_seqno)、raw_len ∈ [6, kSingleRecordMaxRaw]、
// payload_len 合理范围及无压缩时 payload_len == raw_len、block_size <= file_remaining_bytes。
// 校验通过返回 true, 并在 out 参数中返回解析后的字段; 校验失败返回 false。
bool block_header_plausible(const uint8_t bhdr[34], uint64_t file_remaining_bytes,
                            uint32_t prev_seqno, bool has_prev_seqno,
                            uint32_t* out_seqno, uint32_t* out_raw_len,
                            uint32_t* out_payload_len, uint64_t* out_block_size);

FileScanResult scan_one(Instance* inst, const std::string& path, bool repair);
FileScanResult scan_append_target(Instance* inst, const std::string& hint_path);
// 显式断电持久化检查点: 同步 staging 脏数据 (MS_SYNC) 与活跃日志文件 (fdatasync) (spec §11)。
// 成功返回 0, 失败返回 -1 (非致命, 仅打点发事件)。
// out_sync_time_ms: 可选输出完成同步时的系统时间戳 (毫秒, wallclock ms)。
int durability_sync(Instance* inst, uint64_t* out_sync_time_ms = nullptr);

// 在 log_dir 下扫描所有日志文件,选"日期最大且尾块 seqno 最大"者作为追加目标;
// 返回文件路径(空 = 无)。recovery 对账用。
std::string scan_dir_for_target(Instance* inst);

// k 块回溯:从 target_path 中 seqno = upto_seqno(不含)之后的第一块开始,
// 逐块累加 raw_len 直到文件尾(最后一个有效块),返回 Σraw_len。
// found_* 输出最后一个有效块的起止偏移;文件 IO 失败返回 false(调用方按
// 长度异常处理)。spec §4.3 S_file > S_hdr 分支:"取 seqno ∈ (S_hdr, S_file]
// 的块累加 raw_len,F = (F + Σraw_len) mod 数据区大小"。
bool sum_raw_len_since(Instance* inst, const std::string& target_path,
                       uint32_t upto_seqno, uint64_t* out_bytes,
                       uint64_t* out_last_block_offset,
                       uint64_t* out_last_block_end_offset);

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