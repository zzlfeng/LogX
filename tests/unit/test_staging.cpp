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
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
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
        path_ = std::string("/tmp/logx_staging_") +
                std::to_string(::getpid()) + "_" +
                std::to_string(rand()) + ".bin";
        ::unlink(path_.c_str());
    }
    void TearDown() override { ::unlink(path_.c_str()); }
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
    // 1 字节空隙保护:即使把数据区全填到 size-1 也不能继续
    uint8_t big[1024];
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
    // 再写 80 字节:触发回绕;近环尾的剩余空间 < 6B 时留零
    std::vector<uint8_t> b(80, 'b');
    auto r2 = s.write_records(b.data(), 80);
    EXPECT_EQ(r2, Staging::WriteStatus::kOk);
    uint32_t w = get_u32(s.raw_base() + 16);
    EXPECT_EQ(w, 80u);
    // 不变量:[0,80) 是活跃区;其余全零
    EXPECT_TRUE(s.debug_check_invariant(0, 80));
}

TEST_F(StagingTest, DualHeaderCorruptionBHandWins) {
    Staging s(&default_pal(), path_, kDataSize);
    RecoveryState st; bool rec = false;
    ASSERT_TRUE(s.open(&st, &rec));
    // 关闭 staging
    s.close();
    // 损坏 Header A(magic 改成 0)
    int fd = ::open(path_.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    uint8_t zero[4] = {0, 0, 0, 0};
    ::pwrite(fd, zero, 4, 0);
    ::close(fd);
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
    // 把 f 推到 data_size - 2(剩余 2B,小于 6B,触发留零回绕)
    uint32_t pos = kDataSize - 2;
    std::vector<uint8_t> a(pos, 'y');
    ASSERT_EQ(s.write_records(a.data(), pos), Staging::WriteStatus::kOk);
    EXPECT_EQ(get_u32(s.raw_base() + 16), pos);
    // 写一条 6B 记录(等于最小记录长),需要回绕;剩 2B 不足 6B → 留零
    std::vector<uint8_t> small(6, 'z');
    auto r = s.write_records(small.data(), 6);
    EXPECT_EQ(r, Staging::WriteStatus::kOk);
    EXPECT_EQ(get_u32(s.raw_base() + 16), 6u);
    // 验证 [pos, data_size) 是零;这是我们的"距段尾 < 6B 留零"不变量
    for (uint32_t i = pos; i < kDataSize; ++i) {
        EXPECT_EQ(s.data_base()[i], 0u) << "i=" << i;
    }
}