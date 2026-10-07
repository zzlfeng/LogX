// test_format.cpp - 格式化/分片/校验和 单测。
//
// 覆盖:
//   - 整条(短) 单条 < 20KB → 不切片
//   - 跨片 20KB..60KB → 多片连续,flags 正确,parse_record 还原一致
//   - 单条上限拒绝(> 60KB 用户数据)
//   - tag 过长拒绝
//   - PAD 头构造正确,parse_record 跳过 PAD
//   - 撕裂记录 cksum 失败

#include "logx_format.h"
#include "logx_staging.h"  // (T-08) put_u16 / kMagicEntry
#include <gtest/gtest.h>
#include <cstring>
#include <string>

using namespace logx;

TEST(Format, ShortRecord_OnePiece) {
    uint8_t buf[kMaxRecordTotal];
    uint32_t total = 0;
    bool ok = format_record(2, "Net", (const uint8_t*)"hello", 5,
                            1700000000000ULL, 1001, 2001,
                            buf, sizeof(buf), &total);
    ASSERT_TRUE(ok);
    ASSERT_GT(total, 6u);
    // 头解析
    uint16_t len = buf[0] | (uint16_t(buf[1]) << 8);
    uint16_t magic = buf[2] | (uint16_t(buf[3]) << 8);
    uint8_t flags = buf[4];
    uint8_t cksum = buf[5];
    EXPECT_EQ(magic, kMagicEntry);
    EXPECT_EQ(flags, 0x00);  // 整条
    EXPECT_EQ(len, total - 6);
    uint8_t expected = 0;
    for (uint32_t i = 0; i < len; ++i) expected ^= buf[6 + i];
    EXPECT_EQ(cksum, expected);

    // parse_record 还原
    std::vector<uint8_t> user;
    ASSERT_TRUE(parse_record(buf, total, &user));
    // 内部格式:[ts_ms:8][pid:4][tid:8][level:1][taglen:2]["Net"][msglen:4]["hello"]
    // 偏移:ts@0 pid@8 tid@12 level@20 taglen@21 tag@23 msglen@26 msg@30
    // 总长: 8 + 4 + 8 + 1 + 2 + 3 + 4 + 5 = 35 字节 (27 + 3 + 5)
    EXPECT_EQ(user.size(), 27u + 3u + 5u);
    EXPECT_EQ(user[21], 3);   // taglen = 3
    EXPECT_EQ(0, std::memcmp(user.data() + 23, "Net", 3));
    EXPECT_EQ(0, std::memcmp(user.data() + 30, "hello", 5));
}

TEST(Format, LongRecord_Slices) {
    // 用户数据 = 50KB → 触发 3 片(20KB + 20KB + 10KB)
    std::string big(50 * 1024, 'A');
    uint8_t buf[kMaxRecordTotal];
    uint32_t total = 0;
    bool ok = format_record(3, "X", (const uint8_t*)big.data(),
                            static_cast<uint32_t>(big.size()),
                            1700000000000ULL, 7, 13,
                            buf, sizeof(buf), &total);
    ASSERT_TRUE(ok);

    // 切片识别
    uint32_t pos = 0;
    int slices = 0;
    bool saw_first = false, saw_last = false;
    while (pos < total) {
        uint16_t len = buf[pos] | (uint16_t(buf[pos + 1]) << 8);
        uint16_t magic = buf[pos + 2] | (uint16_t(buf[pos + 3]) << 8);
        uint8_t flags = buf[pos + 4];
        uint8_t cksum = buf[pos + 5];
        ASSERT_EQ(magic, kMagicEntry);
        if ((flags & 0x03) == 0x01) saw_first = true;
        if ((flags & 0x03) == 0x03) saw_last = true;
        // 校验和
        uint8_t e = 0;
        for (uint32_t i = 0; i < len; ++i) e ^= buf[pos + 6 + i];
        EXPECT_EQ(cksum, e);
        pos += 6 + len;
        ++slices;
    }
    EXPECT_EQ(slices, 3);
    EXPECT_TRUE(saw_first);
    EXPECT_TRUE(saw_last);

    // parse_record 还原 = 50KB
    std::vector<uint8_t> user;
    ASSERT_TRUE(parse_record(buf, total, &user));
    EXPECT_EQ(user.size(), 27u + 1u + 50u * 1024u);
}

TEST(Format, RejectOverMax) {
    std::string big(60 * 1024 + 1, 'B');
    uint8_t buf[kMaxRecordTotal];
    uint32_t total = 0;
    bool ok = format_record(0, "T", (const uint8_t*)big.data(),
                            static_cast<uint32_t>(big.size()),
                            0, 0, 0, buf, sizeof(buf), &total);
    EXPECT_FALSE(ok);
}

TEST(Format, RejectLongTag) {
    std::string tag(65, 'c');
    uint8_t buf[kMaxRecordTotal];
    uint32_t total = 0;
    bool ok = format_record(0, tag.c_str(), (const uint8_t*)"x", 1,
                            0, 0, 0, buf, sizeof(buf), &total);
    EXPECT_FALSE(ok);
}

TEST(Format, PadRecord) {
    uint8_t buf[256];
    uint32_t wrote = format_pad(buf, 100);
    EXPECT_EQ(wrote, 106u);
    uint16_t len = buf[0] | (uint16_t(buf[1]) << 8);
    uint16_t magic = buf[2] | (uint16_t(buf[3]) << 8);
    EXPECT_EQ(len, 100u);
    EXPECT_EQ(magic, kMagicPad);
    // payload 全零
    for (int i = 6; i < 106; ++i) EXPECT_EQ(buf[i], 0);
}

TEST(Format, ParseSkipsPadAndReassembles) {
    // 一条日志分两片,中间夹一条 LGXP PAD:重组应跨 PAD 连续。
    // 用户载荷 = "ABCDEFGH"(8B),首片 4B(flags=01) + 末片 4B(flags=11)。
    uint8_t pad[106];
    uint32_t rp = format_pad(pad, 100);
    std::vector<uint8_t> concat;
    // (T-08) 记录头经 put_u16 + kMagicEntry 常量构造,杜绝硬编码魔数
    auto make_head = [](uint8_t* h, uint16_t len, uint8_t flags, uint8_t ck) {
        put_u16(h, len);
        put_u16(h + 2, kMagicEntry);
        h[4] = flags; h[5] = ck;
    };
    // 首片:len=4, flags=01, cksum("ABCD")=0x04
    uint8_t first[10];
    make_head(first, 4, 0x01, 0x04);
    first[6] = 'A'; first[7] = 'B'; first[8] = 'C'; first[9] = 'D';
    concat.insert(concat.end(), first, first + sizeof(first));
    concat.insert(concat.end(), pad, pad + rp);
    // 末片:len=4, flags=11, cksum("EFGH")=0x0c
    uint8_t last[10];
    make_head(last, 4, 0x03, 0x0c);
    last[6] = 'E'; last[7] = 'F'; last[8] = 'G'; last[9] = 'H';
    concat.insert(concat.end(), last, last + sizeof(last));

    std::vector<uint8_t> user;
    ASSERT_TRUE(parse_record(concat.data(),
                             static_cast<uint32_t>(concat.size()), &user));
    // 首片 + 末片拼接(PAD 被跳过),还原 8B 用户数据
    EXPECT_EQ(user.size(), 8u);
    EXPECT_EQ(0, std::memcmp(user.data(), "ABCDEFGH", 8));
}

TEST(Format, TornRecordCksumFails) {
    // 制造一条"头完好 payload 截断":把 buf 末字节改成 0xFF
    uint8_t buf[kMaxRecordTotal];
    uint32_t total = 0;
    ASSERT_TRUE(format_record(0, "T", (const uint8_t*)"hello", 5, 0, 0, 0,
                              buf, sizeof(buf), &total));
    buf[total - 1] ^= 0xFF;
    std::vector<uint8_t> user;
    EXPECT_FALSE(parse_record(buf, total, &user));
}

TEST(Format, EdgeDefenses_RejectsInvalidInputs) {
    uint8_t buf[kMaxRecordTotal];
    uint32_t total = 0;

    // 1) level > 5
    EXPECT_FALSE(format_record(6, "tag", (const uint8_t*)"msg", 3, 0, 0, 0,
                               buf, sizeof(buf), &total));

    // 2) tag == nullptr
    EXPECT_FALSE(format_record(2, nullptr, (const uint8_t*)"msg", 3, 0, 0, 0,
                               buf, sizeof(buf), &total));

    // 3) payload == nullptr && payload_len > 0
    EXPECT_FALSE(format_record(2, "tag", nullptr, 10, 0, 0, 0,
                               buf, sizeof(buf), &total));

    // 4) 输出缓冲区不足
    EXPECT_FALSE(format_record(2, "tag", (const uint8_t*)"msg", 3, 0, 0, 0,
                               buf, 1024, &total));
}

TEST(Format, MultipleWholeRecordsInOneBlock_StreamParsed) {
    // 模拟 1 个 16KB 块内包含 3 条连续的整记录 (is_whole)
    std::vector<uint8_t> raw_block;
    for (int i = 0; i < 3; ++i) {
        uint8_t buf[kMaxRecordTotal];
        uint32_t total = 0;
        std::string msg = "multi_record_" + std::to_string(i);
        ASSERT_TRUE(format_record(2, "TestTag", (const uint8_t*)msg.data(),
                                  static_cast<uint32_t>(msg.size()),
                                  1000 + i, 101, 202, buf, sizeof(buf), &total));
        raw_block.insert(raw_block.end(), buf, buf + total);
    }

    // 单条解析器面对多条记录应按 spec 纪律拒绝 (单条不允许多记录)
    std::vector<uint8_t> single_out;
    EXPECT_FALSE(parse_record(raw_block.data(), static_cast<uint32_t>(raw_block.size()), &single_out));

    // 流式解析器应成功解析并拆解出全部 3 条独立记录
    std::vector<std::vector<uint8_t>> stream_outs;
    ASSERT_TRUE(parse_records_stream(raw_block.data(), static_cast<uint32_t>(raw_block.size()), &stream_outs));
    ASSERT_EQ(stream_outs.size(), 3u);

    for (int i = 0; i < 3; ++i) {
        std::string expected_msg = "multi_record_" + std::to_string(i);
        EXPECT_EQ(stream_outs[i].size(), 27u + 7u + expected_msg.size());
    }
}
// ---------------------------------------------------------------------------
// B-1B 方案B: format_record_bounded 截断保前缀
// ---------------------------------------------------------------------------

namespace {

// 从内部用户载荷切出 msg 段([27B 头][tag][msg] 的 msg 部分)。
std::string msg_of(const std::vector<uint8_t>& user, uint32_t tag_len) {
    return std::string(reinterpret_cast<const char*>(user.data()) + 27 + tag_len,
                       user.size() - 27 - tag_len);
}

}  // namespace

TEST(FormatBounded, UnderLimit_NoTruncation) {
    uint8_t buf[kMaxRecordTotal];
    uint32_t total = 0;
    bool truncated = true;
    uint64_t dropped = 99;
    // tag "T"(1B): total = 27+1+66+6 = 100 == max_total → 恰好不截断
    logx::FormatBoundedStatus st = format_record_bounded(
        2, "T", (const uint8_t*)"hello", 66, 0, 0, 0,
        buf, sizeof(buf), &total, 100, &truncated, &dropped);
    ASSERT_EQ(st, FormatBoundedStatus::kOk);
    EXPECT_FALSE(truncated);
    EXPECT_EQ(dropped, 0u);
    EXPECT_EQ(total, 100u);
}

TEST(FormatBounded, TruncateSingleSlice_PrefixPreservedWithMarker) {
    std::string big(600, 'L');
    uint8_t buf[kMaxRecordTotal];
    uint32_t total = 0;
    bool truncated = false;
    uint64_t dropped = 0;
    logx::FormatBoundedStatus st = format_record_bounded(
        2, "Tag", (const uint8_t*)big.data(), static_cast<uint32_t>(big.size()),
        0, 0, 0, buf, sizeof(buf), &total, 500, &truncated, &dropped);
    ASSERT_EQ(st, FormatBoundedStatus::kOk);
    ASSERT_TRUE(truncated);
    // prefix = 600 - dropped;msg = prefix + marker,落环总长必须 ≤ 上限
    ASSERT_GT(dropped, 0u);
    EXPECT_LE(total, 500u);
    const uint32_t prefix_len = 600u - static_cast<uint32_t>(dropped);

    std::vector<uint8_t> user;
    ASSERT_TRUE(parse_record(buf, total, &user));  // 校验和逐片验证
    const std::string msg = msg_of(user, 3);
    // 前缀字节级一致
    ASSERT_GE(msg.size(), prefix_len);
    EXPECT_EQ(0, std::memcmp(msg.data(), big.data(), prefix_len));
    // marker 后缀正确且记录丢弃字节数
    const std::string suffix = msg.substr(prefix_len);
    EXPECT_EQ(suffix,
              "...[LogX truncated, dropped " + std::to_string(dropped) + " bytes]");
    EXPECT_LE(suffix.size(), 48u);  // marker 定长预算契约
}

TEST(FormatBounded, TruncateOversixtyKB_MultiSliceRoundTrip) {
    // 70KB payload:旧实现误报 INVALID_ARG(format_record 直接失败),
    // B-1B 后并入截断;64KB 上限下为多片记录。
    std::string big(70 * 1024, 'K');
    for (uint32_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>('A' + (i % 26));
    uint8_t buf[kMaxRecordTotal];
    uint32_t total = 0;
    bool truncated = false;
    uint64_t dropped = 0;
    logx::FormatBoundedStatus st = format_record_bounded(
        2, "", (const uint8_t*)big.data(), static_cast<uint32_t>(big.size()),
        0, 0, 0, buf, sizeof(buf), &total, 64 * 1024, &truncated, &dropped);
    ASSERT_EQ(st, FormatBoundedStatus::kOk);
    ASSERT_TRUE(truncated);
    EXPECT_GT(dropped, 0u);
    EXPECT_LE(total, 64u * 1024u);

    // 多片流式重组:前缀逐字节一致 + marker 后缀
    std::vector<std::vector<uint8_t>> users;
    std::vector<uint8_t> carry;
    ASSERT_TRUE(parse_records_stream(buf, total, &users, &carry));
    ASSERT_EQ(users.size(), 1u);
    const uint32_t prefix_len = big.size() - static_cast<uint32_t>(dropped);
    const std::string msg = msg_of(users[0], 0);
    ASSERT_GE(msg.size(), prefix_len);
    EXPECT_EQ(0, std::memcmp(msg.data(), big.data(), prefix_len));
    EXPECT_EQ(msg.substr(prefix_len),
              "...[LogX truncated, dropped " + std::to_string(dropped) + " bytes]");
}

TEST(FormatBounded, NoRoomForMarker_PurePrefix) {
    // max_total=75,tag "T"(1B): 预算 b = 75-6-27-1 = 41 < 48 → 弃 marker 保前缀
    std::string big(600, 'M');
    uint8_t buf[kMaxRecordTotal];
    uint32_t total = 0;
    bool truncated = false;
    uint64_t dropped = 0;
    logx::FormatBoundedStatus st = format_record_bounded(
        2, "T", (const uint8_t*)big.data(), static_cast<uint32_t>(big.size()),
        0, 0, 0, buf, sizeof(buf), &total, 75, &truncated, &dropped);
    ASSERT_EQ(st, FormatBoundedStatus::kOk);
    ASSERT_TRUE(truncated);
    EXPECT_LE(total, 75u);
    EXPECT_EQ(dropped, 600u - 41u);

    std::vector<uint8_t> user;
    ASSERT_TRUE(parse_record(buf, total, &user));
    const std::string msg = msg_of(user, 1);
    EXPECT_EQ(msg.size(), 41u);
    EXPECT_EQ(0, std::memcmp(msg.data(), big.data(), 41u));
    EXPECT_EQ(msg.find("LogX truncated"), std::string::npos);  // 无 marker
}

TEST(FormatBounded, CannotFit_PathologicalTinyLimit) {
    // tag "E"(1B): 最小空记录 = 27+1+6 = 34B;33 连空记录都放不下
    uint8_t buf[kMaxRecordTotal];
    uint32_t total = 0;
    bool truncated = false;
    uint64_t dropped = 0;
    logx::FormatBoundedStatus st = format_record_bounded(
        2, "E", (const uint8_t*)"payload", 7, 0, 0, 0,
        buf, sizeof(buf), &total, 33, &truncated, &dropped);
    EXPECT_EQ(st, FormatBoundedStatus::kCannotFit);
    EXPECT_FALSE(truncated);
    EXPECT_EQ(dropped, 0u);
}

TEST(FormatBounded, InvalidInputStaysInvalid) {
    uint8_t buf[kMaxRecordTotal];
    uint32_t total = 0;
    bool truncated = false;
    uint64_t dropped = 0;
    // level 越界 / tag 非法:与超长无关,仍按非法输入上报
    EXPECT_EQ(format_record_bounded(6, "T", (const uint8_t*)"m", 1, 0, 0, 0,
                                    buf, sizeof(buf), &total, 500,
                                    &truncated, &dropped),
              FormatBoundedStatus::kInvalidInput);
    EXPECT_EQ(format_record_bounded(2, nullptr, (const uint8_t*)"m", 1, 0, 0, 0,
                                    buf, sizeof(buf), &total, 500,
                                    &truncated, &dropped),
              FormatBoundedStatus::kInvalidInput);
    EXPECT_FALSE(truncated);
}

TEST(FormatBounded, BudgetSweep_TotalAlwaysFitsLimit) {
    // 一组上限 × 空 tag 扫描:凡可行(≥ 最小空记录 33B)必成功且总长 ≤ 上限;
    // marker 恒在预算内(实际 marker ≤ 45 ≤ 48)由总长断言兜底验证。
    std::string big(64 * 1024, 'Z');
    for (uint32_t max_total = 33u; max_total <= 1200u; max_total += 37u) {
        uint8_t buf[kMaxRecordTotal];
        uint32_t total = 0;
        bool truncated = false;
        uint64_t dropped = 0;
        logx::FormatBoundedStatus st = format_record_bounded(
            2, "", (const uint8_t*)big.data(), static_cast<uint32_t>(big.size()),
            0, 0, 0, buf, sizeof(buf), &total, max_total, &truncated, &dropped);
        ASSERT_EQ(st, FormatBoundedStatus::kOk) << "max_total=" << max_total;
        ASSERT_TRUE(truncated) << "max_total=" << max_total;
        ASSERT_LE(total, max_total) << "max_total=" << max_total;
        ASSERT_GT(dropped, 0u) << "max_total=" << max_total;
    }
}

// R4-1: out_truncated / out_dropped 按契约可为 NULL,函数内守卫赋值不崩溃。
TEST(FormatBounded, NullOutParamsTolerated) {
    uint8_t buf[kMaxRecordTotal];
    uint32_t total = 0;
    // 截断路径 + 双 NULL
    std::string big(600, 'L');
    EXPECT_EQ(format_record_bounded(2, "Tag", (const uint8_t*)big.data(),
                                    static_cast<uint32_t>(big.size()),
                                    0, 0, 0, buf, sizeof(buf), &total, 500,
                                    nullptr, nullptr),
              FormatBoundedStatus::kOk);
    EXPECT_LE(total, 500u);
    // 未截断路径 + 双 NULL
    EXPECT_EQ(format_record_bounded(2, "Tag", (const uint8_t*)"ok", 2,
                                    0, 0, 0, buf, sizeof(buf), &total, 500,
                                    nullptr, nullptr),
              FormatBoundedStatus::kOk);
    // 混用态 1:只要 out_truncated(out_dropped 为 NULL),截断标记可观测
    bool truncated_only = false;
    EXPECT_EQ(format_record_bounded(2, "Tag", (const uint8_t*)big.data(),
                                    static_cast<uint32_t>(big.size()),
                                    0, 0, 0, buf, sizeof(buf), &total, 500,
                                    &truncated_only, nullptr),
              FormatBoundedStatus::kOk);
    EXPECT_TRUE(truncated_only);
    // 混用态 2:只要 out_dropped(out_truncated 为 NULL),丢弃字节数可观测
    uint64_t dropped_only = 0;
    EXPECT_EQ(format_record_bounded(2, "Tag", (const uint8_t*)big.data(),
                                    static_cast<uint32_t>(big.size()),
                                    0, 0, 0, buf, sizeof(buf), &total, 500,
                                    nullptr, &dropped_only),
              FormatBoundedStatus::kOk);
    EXPECT_GT(dropped_only, 0u);
}

// B-4: peek_record_len 与 parse_staging_record_header 单源原语与等价性验证
TEST(Format, PeekAndParseStagingRecordHeader_EquivalenceAndBounds) {
    uint8_t buf[64] = {};

    // 1. 合法 Entry (len = 10, total = 16)
    buf[0] = 10; buf[1] = 0;
    buf[2] = 0x4c; buf[3] = 0x47;  // 'L', 'G' (kMagicEntry)
    buf[4] = 0x01; buf[5] = 0xAA;
    for (int i = 0; i < 10; ++i) buf[6 + i] = static_cast<uint8_t>(i);

    EXPECT_EQ(peek_record_len(buf, 0, 16), 16u);
    StagingRecordHeaderView rh{};
    EXPECT_TRUE(parse_staging_record_header(buf, 0, 16, &rh));
    EXPECT_EQ(rh.total_size, 16u);
    EXPECT_EQ(rh.payload_len, 10u);
    EXPECT_EQ(rh.magic, kMagicEntry);
    EXPECT_TRUE(rh.valid_magic);
    EXPECT_EQ(rh.flags, 0x01);
    EXPECT_EQ(rh.cksum, 0xAA);
    EXPECT_EQ(cksum1(buf + 6, 10), 0 ^ 1 ^ 2 ^ 3 ^ 4 ^ 5 ^ 6 ^ 7 ^ 8 ^ 9);

    // 2. 合法 Pad (len = 4, total = 10)
    buf[0] = 4; buf[1] = 0;
    buf[2] = 0x47; buf[3] = 0x50;  // 'G', 'P' (kMagicPad)
    buf[4] = 0x00; buf[5] = 0x00;
    EXPECT_EQ(peek_record_len(buf, 0, 10), 10u);
    EXPECT_TRUE(parse_staging_record_header(buf, 0, 10, &rh));
    EXPECT_EQ(rh.total_size, 10u);
    EXPECT_EQ(rh.payload_len, 4u);
    EXPECT_EQ(rh.magic, kMagicPad);
    EXPECT_TRUE(rh.valid_magic);

    // 3. 边界防御: 缓冲区短于 6B 头
    EXPECT_EQ(peek_record_len(buf, 0, 5), 0u);
    EXPECT_FALSE(parse_staging_record_header(buf, 0, 5, &rh));

    // 4. 边界防御: 声明长度超出 total 范围
    EXPECT_EQ(peek_record_len(buf, 0, 9), 0u);
    EXPECT_FALSE(parse_staging_record_header(buf, 0, 9, &rh));

    // 5. 非法 Magic
    buf[2] = 0xFF; buf[3] = 0xFF;
    EXPECT_EQ(peek_record_len(buf, 0, 10), 0u);
    EXPECT_TRUE(parse_staging_record_header(buf, 0, 10, &rh));
    EXPECT_FALSE(rh.valid_magic);

    // 6. 空指针防御
    EXPECT_FALSE(parse_staging_record_header(buf, 0, 10, nullptr));

    // 7. N-2 边界防御: off > total 防无符号整数下溢
    EXPECT_EQ(peek_record_len(buf, 20, 10), 0u);
    EXPECT_FALSE(parse_staging_record_header(buf, 20, 10, &rh));
    EXPECT_EQ(peek_record_len(buf, 1, 0), 0u);
    EXPECT_FALSE(parse_staging_record_header(buf, 1, 0, &rh));
}

