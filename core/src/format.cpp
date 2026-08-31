// format.cpp - 日志条目序列化与分片。
//
// 设计要点(参见 spec §3.1、§4.1):
//   - 单条日志由格式化元数据 + 用户 payload 组成,格式化在调用方线程锁外完成。
//   - 若 (metadata+payload) 超过 20KB,按 LGXE 记录拆为多片;每片头部一致,
//     flags 区分首/中/末;总片长 ≤ 64KB(Config.maxLogLength)。
//   - payload 8 位校验和(XOR 折叠):锁外计算;写入路径只 memcpy。
//   - 6B 头 = [len:2][magic:2][flags:1][cksum:1];len 是 payload 字节数,不含头。
//
// 此模块不知道 staging/Flusher 的存在,只产出"写入环形缓冲区的字节流"。

#include "logx_format.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace logx {

namespace {

// 1B 校验和:payload XOR 折叠(8 位);PAD 头 cksum 恒为 0。
inline uint8_t cksum1(const uint8_t* p, uint32_t n) {
    uint8_t c = 0;
    for (uint32_t i = 0; i < n; ++i) c ^= p[i];
    return c;
}

// 把整数按小端写入(便于 raw 字节比较与 gtest 复读)。
inline void put_u16(uint8_t* p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
inline void put_u32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
inline void put_u64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (i * 8));
}

// 单条 payload 内的字段布局(用户实际看到的明文行)。
//
// [ts_ms:8][pid:4][tid:8][level:1][taglen:2][tag:N][msglen:4][msg:M]
//
// 这是加密后磁盘上 / 导出明文文件里的实际格式,不是 staging 6B 头。
struct UserRecordHeader {
    static constexpr uint32_t kMaxTag = 64;
    static constexpr uint32_t kMaxMsg = 60 * 1024;  // 配合 64KB 单条上限

    uint64_t ts_ms;
    uint32_t pid;
    uint64_t tid;
    uint8_t  level;
    uint16_t tag_len;
    uint8_t  tag[kMaxTag];
    uint32_t msg_len;
    // msg 直接跟在结构体后面;此处仅声明以方便序列化器组织内存。
};

// 估算 msg 长度上限:tag 字节数 + msg 字节数(分片前)。
// 超过 20KB 的日志需要按 LGXE 分片;每片都是完整 6B 头 + 用户数据切片。

}  // namespace

struct FormatResult {
    uint32_t total_bytes;        // 待写入 staging 的总字节数
    uint8_t  buf[kMaxRecordTotal];
};

// 把 level(0..5)/tag/payload 格式化成"环形缓冲区可写入"的多分片 6B 记录序列。
// 单条日志分片规则:
//   - 用户数据 = [header|tag|msg],header 固定 8+4+8+1+2+4 = 27B
//   - 用户数据总长 ≤ 20KB:不切片,1 条 LGXE 记录(头 + 用户数据)
//   - 用户数据总长 20KB..60KB:多片连续 LGXE,首片 flags=01,中间=10,末片=11
//   - 用户数据总长 > 60KB:返回 false(单条上限 64KB,扣 6B 头 ≈ 60KB 用户数据)
//
// 注意:tag 长度硬上限 64B,msg 字节流限制 60KB;两段总长 ≤ 60KB 即合规。
//
// 输出缓冲 out_buf;out_buf_size ≥ kMaxRecordTotal(64KB + slack)。
// 失败原因:超长 / 输出缓冲不足(只在内部校验失败时,正常路径 out_buf_size 必够)。

bool format_record(uint8_t level, const char* tag, const uint8_t* payload, uint32_t payload_len,
                   uint64_t ts_ms, uint32_t pid, uint64_t tid,
                   uint8_t* out_buf, uint32_t out_buf_capacity,
                   uint32_t* out_total_bytes) {
    if (level > 5) return false;
    if (!tag) return false;
    if (!payload && payload_len > 0) return false;
    if (out_buf_capacity < kMaxRecordTotal) return false;

    // ---- 计算 tag 长度 ----
    uint32_t tag_len = 0;
    for (const char* p = tag; *p && tag_len < 64; ++p) ++tag_len;
    if (tag[tag_len] != '\0') return false;  // tag 超过 64B 视为非法

    // ---- 内部用户数据(待分片的载荷)----
    // 内部格式:[ts_ms:8][pid:4][tid:8][level:1][taglen:2][tag:N][msglen:4][msg:M]
    const uint32_t hdr_size = 8 + 4 + 8 + 1 + 2 + 4;  // 27B
    const uint64_t user_total = static_cast<uint64_t>(hdr_size) + tag_len + 4 + payload_len;
    if (user_total > 60u * 1024u) return false;

    // 把内部数据先组装到 scratch(最多 60KB,远超 20KB 单片上限)。
    uint8_t scratch[64 * 1024];
    if (user_total > sizeof(scratch)) return false;
    uint8_t* w = scratch;
    put_u64(w, ts_ms); w += 8;
    put_u32(w, pid);   w += 4;
    put_u64(w, tid);   w += 8;
    *w++ = level;
    put_u16(w, static_cast<uint16_t>(tag_len)); w += 2;
    std::memcpy(w, tag, tag_len); w += tag_len;
    put_u32(w, payload_len); w += 4;
    if (payload_len > 0) std::memcpy(w, payload, payload_len); w += payload_len;
    // w - scratch == user_total

    // ---- 分片写入 out_buf ----
    uint8_t* out = out_buf;
    constexpr uint32_t kMaxSlicePayload = 20 * 1024;  // spec: 单片 payload ≤ 20KB

    if (user_total <= kMaxSlicePayload) {
        // 整条
        put_u16(out, static_cast<uint16_t>(user_total)); out += 2;
        put_u16(out, kMagicEntry); out += 2;
        *out++ = 0x00;  // flags = 00 (整条)
        *out++ = cksum1(scratch, static_cast<uint32_t>(user_total));
        std::memcpy(out, scratch, user_total); out += user_total;
    } else {
        // 多片;首/中/末
        uint32_t offset = 0;
        bool first = true;
        while (offset < user_total) {
            uint32_t slice = std::min(kMaxSlicePayload, user_total - offset);
            bool is_last = (offset + slice == user_total);
            uint8_t flags = 0;
            if (first)        flags = 0x01;        // 01: 首片
            else if (!is_last) flags = 0x02;       // 10: 中片(bit0=0, bit1=1)
            else               flags = 0x03;       // 11: 末片(bit0=1, bit1=1)
            put_u16(out, static_cast<uint16_t>(slice)); out += 2;
            put_u16(out, kMagicEntry); out += 2;
            *out++ = flags;
            *out++ = cksum1(scratch + offset, slice);
            std::memcpy(out, scratch + offset, slice); out += slice;
            offset += slice;
            first = false;
        }
    }

    *out_total_bytes = static_cast<uint32_t>(out - out_buf);
    return true;
}

// 格式化一个 LGXP PAD 记录:payload 全零,cksum 恒为 0。
// 单一 PAD 长度上限 20KB;若 total_pad > 20KB,需要循环调用。
uint32_t format_pad(uint8_t* out_buf, uint32_t pad_len) {
    if (pad_len > 20 * 1024) pad_len = 20 * 1024;  // 防御性
    put_u16(out_buf, static_cast<uint16_t>(pad_len));
    put_u16(out_buf + 2, kMagicPad);
    out_buf[4] = 0x00;  // flags
    out_buf[5] = 0x00;  // cksum (PAD payload 全零,自然为 0)
    std::memset(out_buf + 6, 0, pad_len);
    return 6 + pad_len;
}

// 复读用户 payload(测试与 decoder 共用):仅对整条 / 末片有效。
// 把 LGXE 序列重组还原为 [ts_ms|...] + payload bytes。返回 false = 分片不完整。
//
// 本函数是 decoder 的核心子例程;不持有任何状态。
bool parse_record(const uint8_t* recs, uint32_t recs_len,
                  std::vector<uint8_t>* out_user_payload) {
    out_user_payload->clear();
    std::vector<uint8_t> user;
    uint32_t pos = 0;
    bool got_last = false;
    while (pos < recs_len) {
        if (recs_len - pos < kHeaderLen) return false;
        uint16_t len = recs[pos] | (uint16_t(recs[pos + 1]) << 8);
        uint16_t magic = recs[pos + 2] | (uint16_t(recs[pos + 3]) << 8);
        uint8_t flags = recs[pos + 4];
        uint8_t cksum = recs[pos + 5];
        if (magic != kMagicEntry) return false;
        if (recs_len - pos < uint32_t(kHeaderLen + len)) return false;
        const uint8_t* payload = recs + pos + kHeaderLen;
        if (cksum1(payload, len) != cksum) return false;
        bool is_first = (flags & 0x01) != 0 && (flags & 0x02) == 0;   // 01
        bool is_mid   = (flags & 0x01) == 0 && (flags & 0x02) != 0;   // 10
        bool is_last  = (flags & 0x01) != 0 && (flags & 0x02) != 0;   // 11
        bool is_whole = (flags & 0x03) == 0;                            // 00
        if (is_whole) {
            if (!user.empty()) return false;  // 单条不允许多记录
            user.insert(user.end(), payload, payload + len);
            got_last = true;
        } else {
            if (is_first) {
                if (!user.empty()) return false;  // 第二个首片
                user.insert(user.end(), payload, payload + len);
            } else if (is_mid || is_last) {
                if (user.empty()) return false;   // 没有首片
                user.insert(user.end(), payload, payload + len);
                if (is_last) got_last = true;
            }
        }
        pos += kHeaderLen + len;
    }
    if (!got_last) return false;
    *out_user_payload = std::move(user);
    return true;
}

}  // namespace logx