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
    EXPECT_EQ(user.size(), 27 + 3 + 4 + 5);
    EXPECT_EQ(user[19], 3);   // taglen = 3
    EXPECT_EQ(0, std::memcmp(user.data() + 21, "Net", 3));
    EXPECT_EQ(0, std::memcmp(user.data() + 27 + 3 + 4 - 5, "hello", 5));
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
    EXPECT_EQ(user.size(), 27 + 1 + 4 + 50 * 1024);
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
    // 两条 LGXE 之间夹一条 LGXP PAD
    uint8_t rec1[64], rec2[64], pad[106];
    uint32_t r1 = 0, r2 = 0, rp = format_pad(pad, 100);
    ASSERT_TRUE(format_record(1, "A", (const uint8_t*)"hi", 2, 0, 1, 1,
                              rec1, sizeof(rec1), &r1));
    ASSERT_TRUE(format_record(1, "A", (const uint8_t*)"yo", 2, 0, 1, 1,
                              rec2, sizeof(rec2), &r2));
    std::vector<uint8_t> concat;
    concat.insert(concat.end(), rec1, rec1 + r1);
    concat.insert(concat.end(), pad, pad + rp);
    concat.insert(concat.end(), rec2, rec2 + r2);

    std::vector<uint8_t> user;
    ASSERT_TRUE(parse_record(concat.data(),
                             static_cast<uint32_t>(concat.size()), &user));
    // 应该解析到 rec1 + rec2 的拼接(因为 parse_record 视分片连续,PAD 跳过)
    EXPECT_EQ(user.size(), (27 + 1 + 4 + 2) * 2);
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