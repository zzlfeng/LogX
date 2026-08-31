// logx_staging.h - 环形 mmap staging 的内部头。
//
// 内存布局(总 128KB,前 128B 是双 Header,后 127.875KB 是环形数据区):
//   [0, 64)   Header A
//   [64, 128) Header B
//   [128, 128KB) Data ring(下标 0..kDataSize-1)
//
// 设计要点(对应 spec §3.1、§5):
//   - 环形坐标:f=flushed_len(下一待刷字节),w=write_pos(下一写入字节)
//   - 会计上保留 1 字节空隙:(f - w - 1 + size) % size = free;
//     当 f == w 时 free = size-1 ≠ 0,空环不歧义。
//   - 任何时刻除 [f, w) 外全环为零(不变量 5)。回收只清零不搬移。
//   - 回绕缝 pad < 6B 留零回绕,不垫 PAD;切块/扫描/解码按"距段尾 < 6B 跳过"。

#ifndef LOGX_STAGING_H
#define LOGX_STAGING_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "logx_pal.h"

namespace logx {

constexpr uint32_t kHeaderASize = 64;
constexpr uint32_t kHeaderTotal = 128;
// 数据区大小 = staging 文件总大小 - kHeaderTotal;启动时按实际 mmap 长度设置。
// 默认约定:总文件大小 128KB → 数据区 128KB - 128 = 130944B。
constexpr uint32_t kStagingFileSize = 128 * 1024;
constexpr uint32_t kDataSizeDefault = kStagingFileSize - kHeaderTotal;

constexpr uint32_t kMagicStaging = 0x5358474c;  // 'LGXS' 小端

// 64B Header 字段(前 60B + 末尾 4B CRC):
//   [0, 4)   magic 'LGXS'
//   [4, 6)   version
//   [6, 8)   flags
//   [8, 12)  seqno(已落盘块数)
//   [12, 16) flushed_len
//   [16, 20) write_pos(诊断用)
//   [20, 60) reserved(40B,其中 13B 是追加目标缓存:日期 4B + 切分序号 1B +
//                    最近提交块起/止偏移各 4B)
//   [60, 64) header_crc(覆盖前 60B)
struct alignas(8) Header {
    uint32_t magic;       // 'LGXS'
    uint16_t version;
    uint16_t flags;
    uint32_t seqno;
    uint32_t flushed_len;
    uint32_t write_pos;
    uint8_t  reserved[40];
    uint32_t header_crc;
};
static_assert(sizeof(Header) == 64, "Header must be 64B");

// 追加目标缓存(reserved[0..12],共 13B):
//   reserved[0..3]   date(本机 yyyyMMdd)
//   reserved[4]      split_no(切分序号 .1/.2 ...)
//   reserved[5..8]   last_block_offset(最近已提交块在文件中的字节偏移)
//   reserved[9..12]  last_block_end_offset
struct AppendTargetCache {
    uint32_t date;
    uint8_t  split_no;
    uint32_t last_block_offset;
    uint32_t last_block_end_offset;
};

// 启动时读到的"恢复上下文":recovery.cpp 用其驱动对账。
struct RecoveryState {
    // 来自双 Header(已选较新且 CRC 通过者;双坏 = 整区重建)
    uint32_t seqno;            // 已提交 seqno(S_hdr)
    uint32_t flushed_len;      // 下一次刷盘的起点 f(F)
    uint32_t write_pos;        // 诊断
    AppendTargetCache target;  // 追加目标缓存

    // 来自文件扫描的最终结论
    uint32_t seqno_file;       // 文件最后一个有效块的 seqno(S_file)
    uint32_t bytes_since_hdr;  // k 块回溯累加的 raw_len,回写 flushed_len 用
    bool     truncated;        // 文件被截断过
    bool     rebuilt;          // staging 整区重建(双 Header 均坏)
};

// 通用读写 helper,用于把字段写进 raw buffer;调试/恢复代码都用同一套。
inline void put_u32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
inline uint32_t get_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline void put_u16(uint8_t* p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
inline uint16_t get_u16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }

// CRC32(IEEE 802.3);codec 也会用到,放这里共享。
uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t len);
uint32_t crc32_finalize(uint32_t crc);

class Staging {
public:
    Staging(Pal* pal, const std::string& path, uint32_t data_size);
    ~Staging();

    // 打开并按 spec 做"启动恢复";失败时返回 false(由调用方决定降级策略)。
    // state_out 始终填充;recovered = true 表示发生过恢复决策(否则全新启动)。
    bool open(RecoveryState* state_out, bool* recovered);

    // 关闭并 msync;幂等。
    void close();

    // ---- 生产者路径(锁内调用)----
    // 一次写入"一条日志的全部记录"(包含全部分片),临界区约一次 memcpy。
    // 失败原因:空闲不足(staging 满)/单条超上限 / write 失败。
    enum class WriteStatus {
        kOk = 0,
        kFull,
        kTooLarge,
    };
    WriteStatus write_records(const uint8_t* recs, uint32_t total);

    // ---- Flusher 路径 ----
    // 锁内取快照:(f, snap = write_pos),返回期间需要拷出的字节数。
    uint32_t snapshot_pending(uint32_t* out_f, uint32_t* out_snap);

    // 把 [f, snap) 的原始字节拷出到 dst(跨回绕则两段),dst_capacity ≥ 数据区大小。
    // 返回实际拷出的字节数(包含跨回绕全部内容,保留 PAD 原样)。
    // 不修改 staging。
    uint32_t copy_pending(uint32_t f, uint32_t snap, uint8_t* dst, uint32_t dst_capacity);

    // 清零 [f, f+bytes) 区间(锁外),按 wrap 推进。bytes = 单块 raw_len。
    void zero_range_locked(uint32_t f, uint32_t bytes);

    // 提交双 Header(seqno/flushed_len/write_pos/追加目标缓存)。原子写 A 再 B。
    // 写入采用"先 A 后 B,读时取 CRC 对且 seqno 新者"的策略。
    void commit_header_locked(uint32_t seqno, uint32_t f, uint32_t write_pos,
                              const AppendTargetCache* target);

    // 把 staging 整区清零(recovery 双 Header 损坏场景使用);f=w=0。
    void wipe_all_locked();

    // ---- 读端 ----
    uint8_t* data_base() { return data_base_; }
    uint32_t data_size() const { return data_size_; }

    // 不变量校验(debug 构建调用):除 [f, w) 外全环为零。返回 true = 满足。
    bool debug_check_invariant(uint32_t f, uint32_t w) const;

    // 仅供测试 / 调试:把 staging 当 byte slice 暴露(不可在生产逻辑里用)。
    uint8_t* raw_base() { return base_; }
    uint32_t raw_size() const { return total_size_; }

    Pal* pal() { return pal_; }

private:
    bool map_and_validate_locked();
    void read_header(const uint8_t* p, Header* out) const;
    uint32_t compute_header_crc(const uint8_t* p) const;
    bool pick_header(Header* out, AppendTargetCache* out_target) const;
    void write_header_raw(uint8_t* p, const Header& h) const;

    Pal* pal_;
    std::string path_;
    uint32_t data_size_;
    uint32_t total_size_;

    int fd_;
    PalMmap mm_;

    uint8_t* base_;          // mmap 起始 = Header A
    uint8_t* data_base_;     // 数据区起始(== base + kHeaderTotal)
};

// 块格式(spec §3.2)常量。块写入与解码共用。
constexpr uint32_t kBlockHeaderSize = 4 + 1 + 1 + 4 + 4 + 4 + 4 + 12;  // magic+ver+flag+seqno+raw+payload+crc+nonce = 32B
constexpr uint32_t kBlockGcmTagSize = 16;
constexpr uint32_t kBlockMaxRaw = 20 * 1024 + 6;            // 单条独占一块上限(含 6B 记录头)
constexpr uint32_t kBlockMaxNormal = 16 * 1024;             // 常规块 ≤ 16KB raw
constexpr uint32_t kFileHeaderSize = 32;
constexpr uint32_t kMagicFile = 0x4658474c;                  // 'LGXF'
constexpr uint32_t kMagicBlock = 0x4258474c;                 // 'LGXB'

// 单块元数据(读盘 / 写盘共用)。
struct BlockView {
    uint8_t  ver;
    uint8_t  flag;        // bit0=enc, bit1-2=压缩算法
    uint32_t seqno;
    uint32_t raw_len;
    uint32_t payload_len;
    uint32_t crc32;
    uint8_t  nonce[12];
    const uint8_t* payload;
    const uint8_t* tag;   // 仅加密块存在
};

// 文件头 32B。
struct FileHeader {
    uint32_t magic;       // 'LGXF'
    uint8_t  ver;
    uint8_t  flag;
    uint32_t date;        // yyyyMMdd
    uint16_t kid;
    uint8_t  salt[8];
    uint8_t  reserved[8];
    uint32_t crc;         // 覆盖前 28B
};
static_assert(sizeof(FileHeader) == 32, "FileHeader must be 32B");

}  // namespace logx
#endif /* LOGX_STAGING_H */