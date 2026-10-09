// logx_format.h - 序列化/分片/校验和公共声明。
//
// 该头仅对 core 内部使用;不在 C ABI 中暴露。

#ifndef LOGX_FORMAT_H
#define LOGX_FORMAT_H

#include <cstdint>
#include <vector>

namespace logx {

// 单条日志 staging 总上限(格式化后,含记录头与全部分片);由 Config.maxLogLength 注入。
constexpr uint32_t kMaxRecordTotal = 64 * 1024;

// 单条日志格式化"内部用户数据"上限:user_total = 用户头 27B + tag + payload ≤ 60KB,
// 超过此值 format_record 拒绝格式化(B-1B 后该"内容超长"类由截断路径接管,
// 见 format_record_bounded;kUserHdrSize 为其 27B 头的命名空间常量)。
constexpr uint32_t kMaxUserPayload = 60 * 1024;

// 单片 LGXE payload 上限(20KB);超过即分片。
constexpr uint32_t kMaxSlicePayload = 20 * 1024;

// staging 记录头 6B;len = payload 字节数。
// magic 存为小端 2B;磁盘字节顺序为字面 'L','G'(条目) / 'G','P'(PAD)。
//   kMagicEntry 低字节 'L'=0x4c, 高字节 'G'=0x47 -> 写入 [0x4c,0x47] -> 'L','G'。
//   kMagicPad   低字节 'G'=0x47, 高字节 'P'=0x50 -> 写入 [0x47,0x50] -> 'G','P'。
constexpr uint32_t kHeaderLen = 6;
constexpr uint16_t kMagicEntry = 0x474c;   // 高字节 0x47='G', 低字节 0x4c='L'
constexpr uint16_t kMagicPad   = 0x5047;   // 高字节 0x50='P', 低字节 0x47='G'

// Wire-format 字节偏移定义 (N-3)
constexpr uint32_t kWireOffPayloadLen = 0;
constexpr uint32_t kWireOffMagic      = 2;
constexpr uint32_t kWireOffFlags      = 4;
constexpr uint32_t kWireOffCksum      = 5;

// 1B 校验和: payload XOR 折叠(8 位); PAD 头 cksum 恒为 0。
inline uint8_t cksum1(const uint8_t* p, uint32_t n) {
    uint8_t c = 0;
    for (uint32_t i = 0; i < n; ++i) c ^= p[i];
    return c;
}

// B-4: staging 记录头解析视图 (零堆分配 POD)
struct StagingRecordHeaderView {
    uint32_t total_size = 0;   // kHeaderLen + payload_len
    uint16_t payload_len = 0;  // 载荷声明长度
    uint16_t magic = 0;        // kMagicEntry 或 kMagicPad
    uint8_t  flags = 0;
    uint8_t  cksum = 0;
    bool     valid_magic = false;
};

// 编码 staging 记录头 6B (供 format_record / format_pad / 测试单源共用)
constexpr inline void make_staging_record_header(uint8_t* out, uint16_t payload_len,
                                                 uint16_t magic, uint8_t flags, uint8_t cksum) {
    out[kWireOffPayloadLen]     = static_cast<uint8_t>(payload_len & 0xFF);
    out[kWireOffPayloadLen + 1] = static_cast<uint8_t>((payload_len >> 8) & 0xFF);
    out[kWireOffMagic]          = static_cast<uint8_t>(magic & 0xFF);
    out[kWireOffMagic + 1]      = static_cast<uint8_t>((magic >> 8) & 0xFF);
    out[kWireOffFlags]          = flags;
    out[kWireOffCksum]          = cksum;
}

// 完整解包记录头元数据并校验边界 (供 recovery / flusher / format / 测试共用)
// N-2: 严密防御 off > total 下溢防线 (防 total - off 无符号回绕)
constexpr inline bool parse_staging_record_header(const uint8_t* raw, uint32_t off, uint32_t total,
                                                 StagingRecordHeaderView* out) {
    if (!out || off > total || total - off < kHeaderLen) return false;
    out->payload_len = uint16_t(raw[off + kWireOffPayloadLen]) |
                       (uint16_t(raw[off + kWireOffPayloadLen + 1]) << 8);
    out->magic = uint16_t(raw[off + kWireOffMagic]) |
                 (uint16_t(raw[off + kWireOffMagic + 1]) << 8);
    out->flags = raw[off + kWireOffFlags];
    out->cksum = raw[off + kWireOffCksum];
    out->valid_magic = (out->magic == kMagicEntry || out->magic == kMagicPad);
    out->total_size = kHeaderLen + out->payload_len;
    if (off + out->total_size > total) return false;
    return true;
}

// Wire-format 编译期行为级 Roundtrip 与物理布局校验 (替代原自指断言, 收口 R35 N-1)
struct StagingHeaderWireRoundtrip {
    static constexpr bool verify() {
        // 1. 测试 make -> parse 闭环与物理字节布局 (小端 2B payload_len + 2B magic + 1B flags + 1B cksum)
        uint8_t buf[kHeaderLen] = {};
        make_staging_record_header(buf, 0x1234, kMagicEntry, 0x03, 0x5A);

        // 验证物理内存字节严格对齐 spec wire format:
        // [0..1] payload_len (小端 0x1234 -> 0x34, 0x12)
        // [2..3] magic (小端 0x474c -> 'L', 'G' -> 0x4c, 0x47)
        // [4]    flags (0x03)
        // [5]    cksum (0x5A)
        if (buf[0] != 0x34 || buf[1] != 0x12) return false;
        if (buf[2] != 0x4c || buf[3] != 0x47) return false;
        if (buf[4] != 0x03) return false;
        if (buf[5] != 0x5A) return false;

        // 验证 parse_staging_record_header 精确读出对应字段
        StagingRecordHeaderView h{};
        if (!parse_staging_record_header(buf, 0, kHeaderLen + 0x1234, &h)) return false;
        if (h.payload_len != 0x1234 || h.magic != kMagicEntry || !h.valid_magic) return false;
        if (h.flags != 0x03 || h.cksum != 0x5A || h.total_size != kHeaderLen + 0x1234) return false;

        // 2. 测试 PAD magic 闭环 ('G','P' -> 0x47, 0x50)
        make_staging_record_header(buf, 0x0020, kMagicPad, 0x00, 0x00);
        if (buf[2] != 0x47 || buf[3] != 0x50) return false;
        StagingRecordHeaderView pad_h{};
        if (!parse_staging_record_header(buf, 0, kHeaderLen + 0x0020, &pad_h)) return false;
        if (pad_h.payload_len != 0x0020 || pad_h.magic != kMagicPad || !pad_h.valid_magic) return false;

        return true;
    }
};

static_assert(StagingHeaderWireRoundtrip::verify(),
              "Staging record header wire-format roundtrip and literal layout check failed");

// 快速探测下一条记录总尺寸 (头 6B + 载荷); 不足 6B、magic 非法或长度越界返回 0
inline uint32_t peek_record_len(const uint8_t* raw, uint32_t off, uint32_t total) {
    StagingRecordHeaderView h;
    if (!parse_staging_record_header(raw, off, total, &h) || !h.valid_magic) {
        return 0;
    }
    return h.total_size;
}

// 写入环形缓冲区的多片 LGXE 记录(同条日志的所有分片,连续)。
// 失败原因:level 非法 / tag 过长 / payload 总长 > 60KB / 输出缓冲不足(仅内部防御)。
bool format_record(uint8_t level, const char* tag,
                   const uint8_t* payload, uint32_t payload_len,
                   uint64_t ts_ms, uint32_t pid, uint64_t tid,
                   uint8_t* out_buf, uint32_t out_buf_capacity,
                   uint32_t* out_total_bytes);

// B-1B 方案B: format_record_bounded 的三态结果。
enum class FormatBoundedStatus {
    kOk,             // 成功(可能已截断,见 out_truncated / out_dropped)
    kInvalidInput,   // 输入非法(level 越界 / tag 非法 / payload 契约),与 format_record 同款
    kCannotFit,      // 截断到空记录仍超 max_total_bytes(病态小上限,调用方走丢弃兜底)
};

// 带截断预算的格式化:格式化后总长超 max_total_bytes 时,把 msg 截断为
// "原始前缀 + ASCII marker(...[LogX truncated, dropped N bytes])"后重新落片,
// 保证落环总长 ≤ max_total_bytes(含逐片 6B 头与片数重算)。
//   - max_total_bytes = 0 表示不施加 max_total 截断(与 format_record 等价);
//     但内部用户数据超 60KB 硬上限时(user_total = 27B 用户头 + tag + payload,
//     长 tag 会提前触限,见 format_record 契约)无法格式化,此时返回 kCannotFit
//     (不截断)——调用方按丢弃兜底处理;
//   - 预算不足以容纳 marker 时退化为纯前缀截断(不加 marker);
//   - 预算连"空 msg 记录"都放不下时返回 kCannotFit;
//   - 热路径零额外成本:先原样 format_record,仅超限时才进入截断分支。
// out_truncated = 是否发生截断;out_dropped = 截断丢弃的 msg 字节数(不含 marker 占用);
// 两者均可为 NULL(忽略对应输出)。
FormatBoundedStatus format_record_bounded(
    uint8_t level, const char* tag,
    const uint8_t* payload, uint32_t payload_len,
    uint64_t ts_ms, uint32_t pid, uint64_t tid,
    uint8_t* out_buf, uint32_t out_buf_capacity,
    uint32_t* out_total_bytes,
    uint32_t max_total_bytes,
    bool* out_truncated, uint64_t* out_dropped);

// 写一条 LGXP PAD(单条,≤ 20KB);调用方负责循环调用以填满回绕缝。
// 返回写入 out_buf 的字节数(== kHeaderLen + pad_len)。
uint32_t format_pad(uint8_t* out_buf, uint32_t pad_len);

// 解析一段 LGXE 序列(单条记录的所有分片,跨 PAD 仍连续)。
// out_user_payload = 拼接后的"内部用户载荷"(hdr_size+tag+msg)。
// 返回 false = 分片不完整、magic 错、cksum 失败。
bool parse_record(const uint8_t* recs, uint32_t recs_len,
                  std::vector<uint8_t>* out_user_payload);

// 流式解析一段 LGXE 序列(单块内可能包含多条独立整记录与/或连续分片)。
// out_user_payloads = 成功还原的完整"内部用户载荷"列表。
// 恒返回 true:损坏容灾(magic 失配重同步 FM-02 / 长度越界滑 6B FM-04 /
// cksum 失败跳整条)均在函数内部按"宁丢不脏"完成,不以返回值上报损坏;
// 跨块 carry_slice_buf 在任何损坏分支被清空,杜绝残片错拼(R13 N-2)。
bool parse_records_stream(const uint8_t* recs, uint32_t recs_len,
                          std::vector<std::vector<uint8_t>>* out_user_payloads,
                          std::vector<uint8_t>* carry_slice_buf = nullptr);

}  // namespace logx
#endif /* LOGX_FORMAT_H */