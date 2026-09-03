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
    // 首片:len=4, LG, flags=01, cksum("ABCD")=0x04
    uint8_t first[10] = {4, 0, 0x4c, 0x47, 0x01, 0x04, 'A', 'B', 'C', 'D'};
    concat.insert(concat.end(), first, first + sizeof(first));
    concat.insert(concat.end(), pad, pad + rp);
    // 末片:len=4, LG, flags=11, cksum("EFGH")=0x0c
    uint8_t last[10] = {4, 0, 0x4c, 0x47, 0x03, 0x0c, 'E', 'F', 'G', 'H'};
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