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
    uint32_t    max_log_length = 64 * 1024;  // 64KB 默认 (S-04: 与 Kotlin/JNI 默认值及注释对齐)
    uint32_t    retention_days = 7;
    uint64_t    max_file_size  = 10 * 1024 * 1024;
    uint32_t    flush_threshold = 8 * 1024;
    uint32_t    staging_wait_ms = 0;  // B-1 方案 A: 环满阻塞等待上限 (毫秒), 0=默认不阻塞
    uint8_t     compress_algo = kAlgoNone;
    bool        encrypt_enable = false;
    uint16_t    kid = 0;
    const uint8_t* (*key_provider)(uint16_t, void*) = nullptr;
    void*       key_user = nullptr;

    // ---- 组件 ----
    Pal*     pal = nullptr;
    void*     mu = nullptr;            // 全局 mutex
    void*     flush_event = nullptr;   // Flusher 唤醒
    void*     space_event = nullptr;   // 暂存区有可用空间唤醒 (R32 方案 2 / 事件驱动退避)
    void*     flusher_thread = nullptr;
    std::atomic<bool> flusher_exit{false};
    std::atomic<bool> flusher_running{false};

    Staging*  staging = nullptr;
    Flusher*  flusher = nullptr;
    std::atomic<int> active_log_fd{-1};

    // ---- 错误回调 ----
    // E-01: err_fn/err_user 由 err_mu 串行化——logx_set_error_handler(写)与后台
    // Flusher/写线程 emit_event(读)并发访问,裸指针构成数据竞争(UB)。
    // 不复用 inst->mu(PAL 互斥量):logx_write 在持有 inst->mu 时也会 emit_event
    // ("staging full"),非递归锁会死锁;err_mu 为独立小锁,锁序恒为 inst->mu → err_mu。
    logx_error_fn err_fn = nullptr;
    void* err_user = nullptr;
    ::std::mutex err_mu;

    // ---- 水位线与超时 (F-08 / T4.0-1) ----
    std::atomic<uint64_t> staged_bytes_total{0};
    std::atomic<uint64_t> flushed_bytes_total{0};

    // ---- 统计 ----
    std::atomic<uint64_t> drop_no_space{0};
    std::atomic<uint64_t> drop_too_large{0};
    std::atomic<uint64_t> truncated_logs{0};   // B-1B: 超长截断保前缀写入计数
    std::atomic<uint64_t> pad_records{0};
    std::atomic<uint64_t> block_write_errors{0};
    std::atomic<uint64_t> truncated_blocks{0};
    // R21 F-7: 回绕缝独占微块(raw_len < kHeaderLen, 落盘必被读侧拒收)被 drain_once
    // 跳过的次数。常态事件(每次 pad<6B 回绕一次), 故只计数不发事件; 用途是让
    // "缝被正常跳过"与"缝被误吞了数据"在运行期/测试里可区分——回归用例据此钉死
    // 场景真的走到了跳过分支。内部字段, 不进 logx_stats_t(零 ABI 影响)。
    std::atomic<uint64_t> seam_blocks_skipped{0};

    // ---- 实例生命周期与错误回调重入防护 (R19 F-2) ----
    std::atomic<int> active_ops{0};
    std::atomic<int> in_callback{0};
    std::atomic<bool> closing{false};
    std::atomic<bool> close_pending{false};
    std::atomic<bool> close_on_flusher{false};
    std::atomic<uint64_t> flusher_tid{0};
};

constexpr uint32_t kFlushBlockingTimeoutMs = 5000;

// Spec §4.1 / T4.0-4 / F-10: Flusher 50~100ms Debounce 超时聚合窗口 (取中值 75ms)
constexpr uint32_t kFlushDebounceMs = 75;

// 触发回调的便捷函数;忽略 null 回调。
// E-01: 快照读取 err_fn/err_user 在 err_mu 保护下完成,消除与
// logx_set_error_handler 并发注册的数据竞争;回调本身在锁外执行,
// 避免回调内再注册 handler 或再打事件造成死锁。
// R19 F-2: in_callback 跟踪回调调用深度, 供 logx_close 检测重入并实施递延销毁。
inline void emit_event(Instance* inst, logx_event_kind_e k, int code, const char* msg) {
    if (!inst) return;
    inst->in_callback.fetch_add(1, std::memory_order_acq_rel);
    inst->err_mu.lock();
    logx_error_fn fn = inst->err_fn;
    void* user = inst->err_user;
    inst->err_mu.unlock();
    if (fn) {
        fn(user, k, code, msg ? msg : "");
    }
    inst->in_callback.fetch_sub(1, std::memory_order_acq_rel);
}

// 格式化一个外部 level(0..5)/tag/payload 为 staging 记录字节序列;
// 失败原因:level 非法 / tag 非法 / (kBuildRecordsCannotFit)病态小 max_log_length。
// out_buf 由调用方提供(kMaxRecordTotal)。
// E-02: 返回码区分失败类别,便于上层区分"输入非法"与"内容超长":
//   > 0 = 格式化后总字节数(超长时已由 B-1B 方案B 截断保前缀,
//         out_truncated 标记是否截断、out_dropped 为丢弃字节数,可为 NULL);
//   < 0 = 格式化失败:kBuildRecordsInvalidInput = 输入非法(level 越界 / tag 为空或超长);
//         kBuildRecordsCannotFit = 截断到空记录仍超 max_log_length(病态小上限),
//         由调用方走 TOO_LARGE 丢弃兜底(drop_too_large 仅此路径递增)。
constexpr int kBuildRecordsInvalidInput = -1;
constexpr int kBuildRecordsCannotFit    = -2;
int build_records(Instance* inst, int level, const char* tag,
                  const uint8_t* payload, uint32_t payload_len,
                  uint8_t* out_buf, uint32_t* out_total_bytes,
                  bool* out_truncated, uint64_t* out_dropped);

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

// 追加目标已达单日最大分卷上限 (65535) 内部返回哨兵码 (F-07 / WARN-4)
constexpr int kAppendTargetSplitExhausted = -2;

// R-03: 校验 yyyymmdd 数值是否为真实存在的日历日期 (含闰年规则)。
// 拒绝 20261399 / 20260231 / 00000000 等非法日期;是所有日期解析入口
// (parse_yyyymmdd / parse_log_filename) 的统一合法性校验。
constexpr inline bool valid_yyyymmdd(uint32_t v) {
    uint32_t y = v / 10000;
    uint32_t m = (v / 100) % 100;
    uint32_t d = v % 100;
    if (y == 0 || m < 1 || m > 12 || d < 1) return false;
    constexpr uint8_t md[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    uint32_t dim = (m == 2 && leap) ? 29u : md[m - 1];
    return d <= dim;
}

// 生成日志文件路径: split=0 为 dir/YYYYMMDD.log, split>0 为 dir/YYYYMMDD.log.N (N <= 65535)
std::string make_log_path(const std::string& dir, uint32_t yyyymmdd, uint16_t split);

// 解析日志文件路径或文件名: 提取 8 位日期与切分序号 (0..65535)。
// 支持 "YYYYMMDD.log" (split=0) 与 "YYYYMMDD.log.N" (split=N)。
// 日期必须通过 valid_yyyymmdd 日历合法性校验 (R-03)。
// 成功返回 true; 其它文件返回 false。
bool parse_log_filename(const std::string& path_or_name, uint32_t* out_date, uint16_t* out_split);

// 单日志文件物理容量硬上限 (4GiB - 16MB 安全余量, 保证即使加上最后一块也不会溢出 uint32_t) (F-06 方案 D)
constexpr uint64_t kSingleFileMaxCapacity = 0xFFFFFFFFULL - (16 * 1024 * 1024ULL);

// 判定是否应触发日志文件切分滚动 (同时满足配置阈值与 4GiB 物理安全防线)
inline bool should_rotate_file_size(uint64_t current_size, uint64_t max_file_size) {
    return current_size >= max_file_size || current_size >= kSingleFileMaxCapacity;
}

// 写入 AppendTargetCache 时的 32 位安全防线 (F-06 方案 D):
// 若偏移溢出 uint32_t (>= UINT32_MAX), 置 0 哨兵值并返回 false (指示已溢出);
// 否则安全收窄至 uint32_t 并返回 true.
inline bool safe_assign_atc_offsets(uint64_t start_offset, uint64_t end_offset,
                                    uint32_t* out_start, uint32_t* out_end) {
    if (start_offset >= UINT32_MAX || end_offset >= UINT32_MAX) {
        *out_start = 0;
        *out_end = 0;
        return false;
    }
    *out_start = static_cast<uint32_t>(start_offset);
    *out_end = static_cast<uint32_t>(end_offset);
    return true;
}


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
    // 末块起/止偏移: 由 scan_one 产出, 仅 recovery.cpp 的 rebuilt 分支消费
    // (经 safe_assign_atc_offsets 写回 AppendTargetCache)。
    // R21 N-2 更正: 原注释称"recovery 用 last_block_offset + atc.last_block_end_offset
    // 区间信息补齐 bytes_since_hint"——不成立。AppendTargetCache 的两个 offset 被
    // commit_header_locked 持久化进 staging Header 后**全仓无读取方**(recovery 只消费
    // target.date 与 atc_split(target)); Σraw_len 的回推实际由 sum_raw_len_since 独立
    // 重扫文件完成, 不依赖任何缓存偏移。同名的 bytes_since_hint 字段则既无生产者
    // 也无消费者, 已随本次更正删除。
    uint64_t last_block_offset = 0;
    uint64_t last_block_end_offset = 0;
    bool     ok = false;
    std::string path;              // 命中文件路径
};
// 校验 34B 块头字段合理性 (F-05)。
// 校验项: magic、ver=1、flag 保留位全 0、压缩算法有效性(且与编译宏匹配)、
// seqno 严格单调递增 (seqno > prev_seqno)、raw_len ∈ [kHeaderLen, kBlockMaxRawPlausible]
// (下界 kHeaderLen=6 正是 R20/R21 F-1 "缝独占微块不得落盘"的读侧依据; 上界是 16MB
// 的防御性常数, **不是**写侧的 kBlockMaxRaw=20486——写侧更严, 二者刻意不同源)、
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

// 在 log_dir 下扫描所有日志文件,选"(日期, 分卷序号) 最新"者作为追加目标;
// 返回文件路径(空 = 无)。可选输出目录下所有分卷尾块的最大有效序号 out_max_seqno。recovery 对账用。
std::string scan_dir_for_target(Instance* inst, uint32_t* out_max_seqno = nullptr);

// k 块回溯:从 target_path 中 seqno = upto_seqno(不含)之后的第一块开始,
// 逐块累加 raw_len 直到文件尾(最后一个有效块),返回 Σraw_len。
// out_last_block_* 输出最后一个有效块的起止偏移;仅文件打开失败或文件小于 32B 文件头时
// 返回 false(此时三个出参均为 0);循环内遇块损坏或短读则提前终止并返回 true,
// 出参携带终止前的部分累加结果与最后有效块起止(宁丢不脏,与 scan_one 纪律一致)。
// spec §4.3 S_file > S_hdr 分支:"取 seqno ∈ (S_hdr, S_file]
// 的块累加 raw_len,F = (F + Σraw_len) mod 数据区大小"。
bool sum_raw_len_since(Instance* inst, const std::string& target_path,
                       uint32_t upto_seqno, uint64_t* out_bytes,
                       uint64_t* out_last_block_offset,
                       uint64_t* out_last_block_end_offset);

// 当前活跃日志文件的路径;按当天日期 + split_no 计算。
std::string current_log_path(Instance* inst, uint32_t yyyymmdd, uint16_t split_no);

// 启动/退出时按保留期清理过期文件。
void retention_cleanup(Instance* inst);

// flush 同步:阻塞至当前 staging 全部落块提交。
bool flush_blocking(Instance* inst);

// 实例销毁实现 (R19 F-2): is_flusher_thread 为 true 时绝不 join 自身
void destroy_instance(Instance* inst, bool is_flusher_thread);

// Flusher 线程入口。
void* flusher_main(void* arg);

// recovery 模块入口:启动时跑一次;返回是否成功(staging open 失败等返回 false)。
bool run_recovery(Instance* inst);

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