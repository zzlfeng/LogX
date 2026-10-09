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

// cksum1 移入 logx_format.h 单源共享 (B-4)

// 内部用户数据头尺寸: [ts_ms:8][pid:4][tid:8][level:1][taglen:2][msglen:4] = 27B
// (R7 上提至命名空间级,供 format_record / 截断求解器 / kTruncBufSize 断言共用;
// 布局表达式由 format_record 内的 static_assert 钉住,常量与权威定义处绑定)。
constexpr uint32_t kUserHdrSize = 27;

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
// 仅作文档用途,序列化直接操作 scratch 缓冲;布局以 format_record 内
// static_assert 钉住的 kUserHdrSize 表达式为准,60KB 上限单源 kMaxUserPayload。
struct UserRecordHeader {
    static constexpr uint32_t kMaxTag = 64;

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

// (FM-01) tag 长度有界扫描,format_record / format_record_bounded 共用。
// 契约:tag 必须是 NUL 终止的 C 字符串(logx_c.h 显式约定;JNI 侧 GetStringUTFRegion
// 后显式写 '\0')。仅当 tag[0..63] 全非 NUL 时才会读取 tag[64],此时契约保证
// NUL 位于下标 >= 64 处,tag[64] 必在调用方缓冲区内 —— 扫描不越界。
// 恰好 64B 无 NUL 的 buffer 属调用方违反 C 字符串契约(与任何 C 字符串 API 同)。
// 返回 false = tag 非法(NULL 或 >64B)。
inline bool tag_len_bounded(const char* tag, uint32_t* out_len) {
    if (!tag) return false;
    const size_t n = strnlen(tag, 65);
    if (n == 65) return false;  // >64B 视为非法
    *out_len = static_cast<uint32_t>(n);
    return true;
}

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
    if (!tag_len_bounded(tag, &tag_len)) return false;

    // ---- 内部用户数据(待分片的载荷)----
    // 内部格式:[ts_ms:8][pid:4][tid:8][level:1][taglen:2][tag:N][msglen:4][msg:M]
    // R7: 头尺寸与 60KB 上限改用命名空间/头文件常量,使 static_assert 的前提与
    // 权威定义处编译期绑定(布局变更时此处失配报警,求解器/断言不再可能脱钩)。
    static_assert(kUserHdrSize == 8 + 4 + 8 + 1 + 2 + 4,
                  "user hdr layout: ts8+pid4+tid8+level1+taglen2+msglen4");
    const uint32_t hdr_size = kUserHdrSize;
    const uint64_t user_total = static_cast<uint64_t>(hdr_size) + tag_len + payload_len;
    if (user_total > kMaxUserPayload) return false;

    // (FM-03) scratch 从栈数组(64KB)改为 static thread_local:
    //   - format_record 是热路径且全程无阻塞/无递归, 线程独占安全;
    //   - 非主线程(Flusher/JNI 调用链)栈仅 512KB~1MB, 每次调用压栈 64KB 有溢出
    //     风险并反复触碰新内存页;thread_local 存放于堆段, 每线程仅一份且复用。
    //   - 多线程写并发(业务线程)正是本库常态, thread_local 天然无竞争。
    // 注意:此缓冲仅在本函数内使用, 不返回指针, 出栈后内容由下次调用覆盖。
    // 内存总账: 本函数 TLS scratch 64KB + format_record_bounded 的 trunc_buf 60KB，
    // 同线程常驻 ≈124KB（N 线程即 ×N）；Android/JNI 侧另有 tls_payload_buf ≤64KB（logx_jni.cpp:366-368）。
    static thread_local uint8_t scratch[64 * 1024];
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

    // ---- 分片写入 out_buf(交错布局: [h0][user0][h1][user1]..., 磁盘格式契约) ----
    // kMaxSlicePayload 用头文件命名空间常量(R7 删除 T5.5 遗留的函数局部遮蔽)
    uint8_t* out = out_buf;

    if (user_total <= kMaxSlicePayload) {
        // 整条
        make_staging_record_header(out, static_cast<uint16_t>(user_total), kMagicEntry, 0x00,
                                   cksum1(scratch, static_cast<uint32_t>(user_total)));
        out += kHeaderLen;
        std::memcpy(out, scratch, user_total); out += user_total;
    } else {
        // 多片;首/中/末
        uint32_t offset = 0;
        bool first = true;
        while (offset < user_total) {
            uint32_t slice = static_cast<uint32_t>(
                std::min<uint64_t>(kMaxSlicePayload, user_total - offset));
            bool is_last = (offset + slice == user_total);
            uint8_t flags = 0;
            if (first)        flags = 0x01;        // 01: 首片
            else if (!is_last) flags = 0x02;       // 10: 中片(bit0=0, bit1=1)
            else               flags = 0x03;       // 11: 末片(bit0=1, bit1=1)
            make_staging_record_header(out, static_cast<uint16_t>(slice), kMagicEntry, flags,
                                       cksum1(scratch + offset, slice));
            out += kHeaderLen;
            std::memcpy(out, scratch + offset, slice); out += slice;
            offset += slice;
            first = false;
        }
    }

    *out_total_bytes = static_cast<uint32_t>(out - out_buf);
    return true;
}

// ---------------------------------------------------------------------------
// B-1B 方案B: 超长截断保前缀
// ---------------------------------------------------------------------------

// 截断 marker(ASCII,追加在保留前缀之后,便于解码侧肉眼识别截断)。
namespace {

// 截断 marker 拆为头/尾常量并与 kMarkerReserve 用 static_assert 绑定(R5-7):
// 最坏长度 = 头 28 + 10(payload 为 uint32_t,丢弃字节数 ≤ 10 位十进制) + 尾 7 = 45 ≤ 48;
// marker[64] snprintf 钳制兜底。
constexpr char kMarkerHead[] = "...[LogX truncated, dropped ";
constexpr char kMarkerTail[] = " bytes]";
constexpr uint32_t kMarkerReserve = 48;
constexpr uint32_t kMaxU32Digits = 10;
static_assert(kMarkerReserve >= sizeof(kMarkerHead) - 1 + kMaxU32Digits + sizeof(kMarkerTail) - 1,
              "kMarkerReserve must cover head + max uint32 digits + tail");

// (R7:kUserHdrSize 上移至文件头匿名命名空间——format_record 亦需引用;
//  kHardUserTotal 删除——60KB 上限统一用头文件既有的 kMaxUserPayload,
//  消除 R6-1 引入的重复常量。)

// 求解截断后 msg(含 marker 占用)的最大字节数,使落环总长 ≤ max_total:
//   total(user_total) = user_total + kHeaderLen·slices,slices = max(1, ⌈user_total/20480⌉)
//   user_total 同时受 60KB 用户载荷硬上限(format_record 契约)约束。
// 做法:对片数 s 逐个取 cap(s) = min(20480·s, max_total − kHeaderLen·s, 60KB),b = cap − (27+tag),
// 取最大者。cap(s) 随 s 先增后减,峰值即最优;最多扫 4 片(64KB 总长 ≤ ~4 片)。
// 结果偏保守不超过 kHeaderLen·(s−1) ≤ 18B(实际片数可能少于预算片数),不追求逐字节精确。
// 返回 false = 连"空 msg 记录"(27B 用户头 + tag + 6B 片头)都放不下(病态 max_total)。
bool solve_truncated_msg_budget(uint32_t max_total, uint32_t tag_len, uint32_t* out_budget) {
    const uint64_t fixed = kUserHdrSize + tag_len;
    *out_budget = 0;
    if (max_total < fixed + kHeaderLen) return false;  // 空记录(1 片)都放不下
    uint64_t best = 0;
    const uint64_t s_scan = (max_total - fixed) / kHeaderLen;
    for (uint64_t s = 1; s <= s_scan && s <= 4; ++s) {
        uint64_t cap = static_cast<uint64_t>(max_total) - kHeaderLen * s;  // 落环总长约束
        if (cap > kMaxSlicePayload * s) cap = kMaxSlicePayload * s;        // 片容量约束
        if (cap > kMaxUserPayload) cap = kMaxUserPayload;                  // 60KB 硬上限
        if (cap <= fixed) continue;
        const uint64_t b = cap - fixed;
        if (b > best) best = b;
    }
    *out_budget = static_cast<uint32_t>(best);
    return true;
}

// 截断拼装缓冲容量(R6-1 修正下界):budget = cap − (kUserHdrSize + tag_len) 随
// tag_len 单调递减,缓冲取最坏须按 tag_len = 0(空 tag 合法可达),不得减 tag 上限
// —— R5-7 首版误减 64 使断言偏松(61346 < 真实最坏 61410),失去防护意义。
// 最坏 msg_len = (kMaxUserPayload − kUserHdrSize) − kMarkerReserve + 45 = 61410,
// 对 kTruncBufSize(61440) 余量 30B。
constexpr uint32_t kTruncBufSize = 60 * 1024;
static_assert(kTruncBufSize >=
                  kMaxUserPayload - kUserHdrSize - kMarkerReserve +
                  (sizeof(kMarkerHead) - 1 + kMaxU32Digits + sizeof(kMarkerTail) - 1),
              "trunc_buf must hold worst-case msg (prefix + max marker, tag_len=0)");

}  // namespace

FormatBoundedStatus format_record_bounded(
    uint8_t level, const char* tag,
    const uint8_t* payload, uint32_t payload_len,
    uint64_t ts_ms, uint32_t pid, uint64_t tid,
    uint8_t* out_buf, uint32_t out_buf_capacity,
    uint32_t* out_total_bytes,
    uint32_t max_total_bytes,
    bool* out_truncated, uint64_t* out_dropped) {
    // 出参按契约可为 NULL(R4-1):全部赋值走守卫。
    if (out_truncated) *out_truncated = false;
    if (out_dropped) *out_dropped = 0;

    // 输入契约校验与 format_record 一致(非法输入不属于截断范畴,由调用方按
    // INVALID_ARG 上报;此处先行排除后,format_record 的失败只剩"内容超长"一类)。
    if (level > 5) return FormatBoundedStatus::kInvalidInput;
    if (!tag) return FormatBoundedStatus::kInvalidInput;
    if (!payload && payload_len > 0) return FormatBoundedStatus::kInvalidInput;
    if (out_buf_capacity < kMaxRecordTotal) return FormatBoundedStatus::kInvalidInput;
    uint32_t tag_len = 0;
    if (!tag_len_bounded(tag, &tag_len)) return FormatBoundedStatus::kInvalidInput;

    // 快路径:原样格式化,热路径恰好一次 format_record,零额外成本。
    if (format_record(level, tag, payload, payload_len, ts_ms, pid, tid,
                      out_buf, out_buf_capacity, out_total_bytes)) {
        if (max_total_bytes == 0 || *out_total_bytes <= max_total_bytes) {
            return FormatBoundedStatus::kOk;  // 未截断
        }
    }
    // 走到此处 = 格式化成功但超 max_total,或 user_total > 60KB 格式化失败
    // (输入非法已在上方排除)—— 两条路同归截断。

    uint32_t budget = 0;
    if (!solve_truncated_msg_budget(max_total_bytes, tag_len, &budget)) {
        return FormatBoundedStatus::kCannotFit;
    }

    // 预算内拼装"前缀 + marker"。独立 thread_local 缓冲,不复用 format_record
    // 内部的 scratch(二次调用会覆写);仅截断冷路径触碰,页成本可忽略。
    // 容量论证(R6-1 修正):最坏 msg_len = prefix(≤ budget−48) + marker(≤45)
    // ≤ budget − 3 ≤ (61440 − 27 − 0tag) − 3 = 61410(tag 取最坏 0,非上限 64),
    // 对 61440 余量 30B —— 安全依赖 static_assert 固化的"marker ≤ 45"与
    // "payload 为 uint32_t"两条前提,任一变更须重审本容量。
    // (R5-7 旧注释的 61407/33B 源自未覆盖 ≥1e9 dropped 的穷举扫描——marker
    // 未达 45B,已由解析上界 61410/30B 取代。)
    static thread_local uint8_t trunc_buf[kTruncBufSize];
    const uint8_t* src = payload;
    uint32_t msg_len = budget;
    uint32_t prefix_len = budget;
    if (budget >= kMarkerReserve) {
        prefix_len = budget - kMarkerReserve;
        // 防御:prefix 理论上恒 < payload_len(截断触发意味着原记录更长),
        // 万一求解给出更大预算则钳到 payload,退化为"全量 + marker"。
        if (prefix_len > payload_len) prefix_len = payload_len;
        char marker[64];
        const int m = std::snprintf(marker, sizeof(marker), "%s%llu%s",
                                    kMarkerHead,
                                    static_cast<unsigned long long>(payload_len - prefix_len),
                                    kMarkerTail);
        if (m <= 0) return FormatBoundedStatus::kInvalidInput;  // 防御:不可达
        const uint32_t marker_len = (static_cast<uint32_t>(m) > sizeof(marker) - 1)
                                        ? static_cast<uint32_t>(sizeof(marker) - 1)
                                        : static_cast<uint32_t>(m);
        if (prefix_len + marker_len > sizeof(trunc_buf)) return FormatBoundedStatus::kInvalidInput;  // 防御:不可达
        std::memcpy(trunc_buf, payload, prefix_len);
        std::memcpy(trunc_buf + prefix_len, marker, marker_len);
        src = trunc_buf;
        msg_len = prefix_len + marker_len;  // ≤ budget,总长仍 ≤ max_total
    } else {
        // 预算不足 48B:保前缀弃 marker(前缀优先),零拷贝直接引用原 payload。
        prefix_len = budget;
        src = payload;
        msg_len = budget;
    }

    if (!format_record(level, tag, src, msg_len, ts_ms, pid, tid,
                       out_buf, out_buf_capacity, out_total_bytes)) {
        // 防御:预算求解已保证 user_total ≤ 60KB 且容量充足,不可达。
        return FormatBoundedStatus::kInvalidInput;
    }
    if (out_truncated) *out_truncated = true;
    if (out_dropped) *out_dropped = static_cast<uint64_t>(payload_len) - prefix_len;
    return FormatBoundedStatus::kOk;
}

// 格式化一个 LGXP PAD 记录:payload 全零,cksum 恒为 0。
// 单一 PAD 长度上限 20KB;若 total_pad > 20KB,需要循环调用。
uint32_t format_pad(uint8_t* out_buf, uint32_t pad_len) {
    if (pad_len > kMaxSlicePayload) pad_len = kMaxSlicePayload;  // 防御性
    make_staging_record_header(out_buf, static_cast<uint16_t>(pad_len), kMagicPad, 0, 0);
    if (pad_len > 0) {
        std::memset(out_buf + kHeaderLen, 0, pad_len);
    }
    return kHeaderLen + pad_len;
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
        if (recs_len - pos < uint32_t(kHeaderLen + len)) return false;
        const uint8_t* payload = recs + pos + kHeaderLen;
        if (magic == kMagicPad) {
            // PAD:不产出内容,直接跳过(分片跨 PAD 仍视为连续)
            pos += kHeaderLen + len;
            continue;
        }
        if (magic != kMagicEntry) return false;
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

bool parse_records_stream(const uint8_t* recs, uint32_t recs_len,
                          std::vector<std::vector<uint8_t>>* out_user_payloads,
                          std::vector<uint8_t>* carry_slice_buf) {
    out_user_payloads->clear();
    std::vector<uint8_t> local_buf;
    std::vector<uint8_t>& current_slice_buf = carry_slice_buf ? *carry_slice_buf : local_buf;
    uint32_t pos = 0;
    while (pos < recs_len) {
        if (recs_len - pos < kHeaderLen) {
            // 距段尾不足 6B (spec §3.3): 缝残留/留零回绕, 正常结束解析
            break;
        }
        uint16_t len = recs[pos] | (uint16_t(recs[pos + 1]) << 8);
        uint16_t magic = recs[pos + 2] | (uint16_t(recs[pos + 3]) << 8);
        uint8_t flags = recs[pos + 4];
        uint8_t cksum = recs[pos + 5];
        // 如果 magic 既不是 Entry 也不是 Pad (包括段尾 1..5 字节留零缝隙与填充):
        // 必须向前滑动重同步, 绝不因缝残留提前截断退出!
        // (FM-02) 重同步意味着当前字节流已断裂, 此前积累的跨片残片属于被破坏的
        // 旧记录, 必须丢弃 —— 否则后续找到的首/中片会与残片错误拼接产生错乱输出。
        if (magic != kMagicEntry && magic != kMagicPad) {
            bool resynced = false;
            for (uint32_t s = 1; pos + s + kHeaderLen <= recs_len; ++s) {
                uint16_t next_magic = recs[pos + s + 2] | (uint16_t(recs[pos + s + 3]) << 8);
                if (next_magic == kMagicEntry || next_magic == kMagicPad) {
                    pos += s;
                    resynced = true;
                    break;
                }
            }
            if (resynced) {
                current_slice_buf.clear();
                continue;
            }
            current_slice_buf.clear();
            break;
        }

        if (recs_len - pos < uint32_t(kHeaderLen + len)) {
            // 长度越界, 记录头声明的长度不可信:
            // (FM-04) 至少滑动一个完整记录头(6B)而非逐字节, 避免最坏 O(n²) 重扫;
            // 6B 内不含任何可判定的记录边界, 逐字节滑动只增加复杂度不增加找回率。
            pos += kHeaderLen;
            current_slice_buf.clear();
            continue;
        }

        const uint8_t* payload = recs + pos + kHeaderLen;
        if (magic == kMagicPad) {
            pos += kHeaderLen + len;
            continue;
        }

        if (cksum1(payload, len) != cksum) {
            // (FM-04) cksum 失败 = 本记录已损坏, 原记录长度 (6B 头 + len) 内不存在
            // 任何合法子记录(记录格式按头起始对齐), 逐字节重扫既不可能找回数据
            // 又造成最坏 O(n²);跳过整个损坏记录长度重新对齐。
            pos += kHeaderLen + len;
            current_slice_buf.clear();
            continue;
        }

        bool is_first = (flags & 0x01) != 0 && (flags & 0x02) == 0;   // 01
        bool is_mid   = (flags & 0x01) == 0 && (flags & 0x02) != 0;   // 10
        bool is_last  = (flags & 0x01) != 0 && (flags & 0x02) != 0;   // 11
        bool is_whole = (flags & 0x03) == 0;                            // 00

        if (is_whole) {
            out_user_payloads->emplace_back(payload, payload + len);
        } else if (is_first) {
            current_slice_buf.assign(payload, payload + len);
        } else if (is_mid) {
            if (!current_slice_buf.empty()) {
                current_slice_buf.insert(current_slice_buf.end(), payload, payload + len);
            }
        } else if (is_last) {
            if (!current_slice_buf.empty()) {
                current_slice_buf.insert(current_slice_buf.end(), payload, payload + len);
                out_user_payloads->push_back(std::move(current_slice_buf));
                current_slice_buf.clear();
            }
        }
        pos += kHeaderLen + len;
    }
    return true;
}

}  // namespace logx