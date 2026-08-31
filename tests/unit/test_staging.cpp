// test_staging.cpp - Staging 单测:环形缓冲、双 Header、清零回收、缝残留、PAD。
//
// 覆盖(spec §3.1/§5):
//   - 空环不歧义:f==w 时 free = size-1 ≠ 0
//   - 满环:写入后不能继续
//   - 回绕后 PAD 留零 / 1~5B 缝残留
//   - 双 Header 容错:仅 A 损坏时 B 接管
//   - 不变量(除 [f,w) 外全环为零)在写入路径上始终成立
//   - debug_check_invariant 工具可调用

#include "logx_staging.h"
#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace logx;

namespace {
constexpr uint32_t kDataSize = 4 * 1024;  // 测试用小尺寸,加速用例
constexpr uint32_t kTotal    = kDataSize + kHeaderTotal;
}

class StagingTest : public ::testing::Test {
protected:
    std::string path_;
    void SetUp() override {
        Pal& pal = default_pal();
        path_ = std::string("logx_test_tmp_") +
                std::to_string(pal.pid()) + "_" +
                std::to_string(rand()) + ".bin";
        pal.file_unlink(path_.c_str());
    }
    void TearDown() override { default_pal().file_unlink(path_.c_str()); }
};

TEST_F(StagingTest, EmptyRingFreeIsSizeMinusOne) {
    Staging s(&default_pal(), path_, kDataSize);
    RecoveryState st; bool rec = false;
    ASSERT_TRUE(s.open(&st, &rec));
    // f == w == 0;free 应为 kDataSize - 1
    auto r = s.write_records((const uint8_t*)"X", 1);
    EXPECT_EQ(r, Staging::WriteStatus::kOk);
    EXPECT_EQ(get_u32(s.raw_base() + 16), 1u);
    // 不变量:除 [0,1) 外全零
    EXPECT_TRUE(s.debug_check_invariant(0, 1));
}

TEST_F(StagingTest, FullRingDrops) {
    Staging s(&default_pal(), path_, kDataSize);
    RecoveryState st; bool rec = false;
    ASSERT_TRUE(s.open(&st, &rec));
    // 1 字节空隙保护:把数据区全填到 size-1(会计上 free = 0),再写必 kFull
    uint8_t big[kDataSize - 1];
    std::memset(big, 'A', sizeof(big));
    auto r = s.write_records(big, sizeof(big));
    EXPECT_EQ(r, Staging::WriteStatus::kOk);
    // 再写任意字节:应返回 kFull(因为 free = 0)
    auto r2 = s.write_records((const uint8_t*)"X", 1);
    EXPECT_EQ(r2, Staging::WriteStatus::kFull);
}

TEST_F(StagingTest, WrapAroundPadsAndKeepsInvariant) {
    Staging s(&default_pal(), path_, kDataSize);
    RecoveryState st; bool rec = false;
    ASSERT_TRUE(s.open(&st, &rec));
    // 先写到接近环尾
    uint32_t near_end = kDataSize - 30;
    std::vector<uint8_t> a(near_end);
    std::memset(a.data(), 'a', near_end);
    auto r = s.write_records(a.data(), near_end);
    EXPECT_EQ(r, Staging::WriteStatus::kOk);
    // 模拟 Flusher 消费:[0, near_end) 清零 + 提交 Header 推进 f
    // (否则环头还是活跃区,跨回绕无处可写)
    AppendTargetCache atc{};
    s.zero_range_locked(0, near_end);
    s.commit_header_locked(/*seqno=*/1, /*f=*/near_end, /*w=*/near_end, &atc);
    // 再写 80 字节:触发回绕;近环尾剩余 30B ≥ 6B → 垫 PAD 后回绕
    std::vector<uint8_t> b(80, 'b');
    auto r2 = s.write_records(b.data(), 80);
    EXPECT_EQ(r2, Staging::WriteStatus::kOk);
    uint32_t w = get_u32(s.raw_base() + 16);
    EXPECT_EQ(w, 80u);
    // 不变量:活跃区 [f,w) = [near_end, size) ∪ [0, 80);其余全零
    EXPECT_TRUE(s.debug_check_invariant(near_end, 80));
    // 回绕处垫了 PAD:数据区 [near_end, near_end+6) 是 LGXP 头(小端 'G','P')
    EXPECT_EQ(s.data_base()[near_end + 2], 0x47);  // 'G'
    EXPECT_EQ(s.data_base()[near_end + 3], 0x50);  // 'P'
}

TEST_F(StagingTest, DualHeaderCorruptionBHandWins) {
    Staging s(&default_pal(), path_, kDataSize);
    RecoveryState st; bool rec = false;
    ASSERT_TRUE(s.open(&st, &rec));
    // 关闭 staging
    s.close();
    // 损坏 Header A(magic 改成 0)
    Pal& pal = default_pal();
    int fd = pal.file_open(path_.c_str(), false, false);
    ASSERT_GE(fd, 0);
    uint8_t zero[4] = {0, 0, 0, 0};
    pal.file_pwrite(fd, zero, 4, 0);
    pal.file_close(fd);
    // 重新打开:应该回退到 B
    Staging s2(&default_pal(), path_, kDataSize);
    RecoveryState st2; bool rec2 = false;
    ASSERT_TRUE(s2.open(&st2, &rec2));
    // B 是空白,seqno/flushed/write_pos 应为 0
    EXPECT_EQ(st2.seqno, 0u);
}

TEST_F(StagingTest, FlushAndZeroMaintainsInvariant) {
    Staging s(&default_pal(), path_, kDataSize);
    RecoveryState st; bool rec = false;
    ASSERT_TRUE(s.open(&st, &rec));
    std::vector<uint8_t> a(256, 'x');
    ASSERT_EQ(s.write_records(a.data(), 256), Staging::WriteStatus::kOk);
    // 模拟 Flusher 清零 [0, 256)
    s.zero_range_locked(0, 256);
    // 此时 f=0,w=256;除 [0,256) 外全零(数据区本来全零)
    EXPECT_TRUE(s.debug_check_invariant(0, 256));
    // 提交 Header(f=256)
    AppendTargetCache atc{};
    s.commit_header_locked(/*seqno=*/1, /*f=*/256, /*w=*/256, &atc);
    EXPECT_EQ(get_u32(s.raw_base() + 8), 1u);
    EXPECT_EQ(get_u32(s.raw_base() + 12), 256u);
}

TEST_F(StagingTest, SmallRecordPreservesAllInvariantsAcrossWrap) {
    Staging s(&default_pal(), path_, kDataSize);
    RecoveryState st; bool rec = false;
    ASSERT_TRUE(s.open(&st, &rec));
    // 把数据区写到距环尾 2B(pos = data_size - 2)
    uint32_t pos = kDataSize - 2;
    std::vector<uint8_t> a(pos, 'y');
    ASSERT_EQ(s.write_records(a.data(), pos), Staging::WriteStatus::kOk);
    EXPECT_EQ(get_u32(s.raw_base() + 16), pos);
    // 模拟 Flusher 消费:清零 + 提交 Header 推进 f = pos
    AppendTargetCache atc{};
    s.zero_range_locked(0, pos);
    s.commit_header_locked(/*seqno=*/1, /*f=*/pos, /*w=*/pos, &atc);
    // 写一条 6B 记录(等于最小记录长):尾部剩 2B 不足 6B → 留零回绕
    std::vector<uint8_t> small(6, 'z');
    auto r = s.write_records(small.data(), 6);
    EXPECT_EQ(r, Staging::WriteStatus::kOk);
    EXPECT_EQ(get_u32(s.raw_base() + 16), 6u);
    // 验证 [pos, data_size) 是零;这是我们的"距段尾 < 6B 留零"不变量
    for (uint32_t i = pos; i < kDataSize; ++i) {
        EXPECT_EQ(s.data_base()[i], 0u) << "i=" << i;
    }
    // 不变量:活跃区 [f,w) = [pos, size) ∪ [0, 6)
    EXPECT_TRUE(s.debug_check_invariant(pos, 6));
}