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
    // 提交非零状态 (seqno=7, F=64, w=64) 到双 Header
    AppendTargetCache atc{};
    s.commit_header_locked(7, 64, 64, &atc);
    // 关闭 staging
    s.close();
    // 损坏 Header A(magic 改成 0)
    Pal& pal = default_pal();
    int fd = pal.file_open(path_.c_str(), false, false);
    ASSERT_GE(fd, 0);
    uint8_t zero[4] = {0, 0, 0, 0};
    pal.file_pwrite(fd, zero, 4, 0);
    pal.file_close(fd);
    // 重新打开: Header A 损坏, 应该由 Header B 接管并成功恢复出已提交的非零状态
    Staging s2(&default_pal(), path_, kDataSize);
    RecoveryState st2; bool rec2 = false;
    ASSERT_TRUE(s2.open(&st2, &rec2));
    EXPECT_EQ(st2.seqno, 7u);
    EXPECT_EQ(st2.flushed_len, 64u);
    EXPECT_EQ(st2.write_pos, 64u);
    EXPECT_FALSE(st2.rebuilt);
}

TEST_F(StagingTest, FlushAndZeroMaintainsInvariant) {
    Staging s(&default_pal(), path_, kDataSize);
    RecoveryState st; bool rec = false;
    ASSERT_TRUE(s.open(&st, &rec));
    std::vector<uint8_t> a(256, 'x');
    ASSERT_EQ(s.write_records(a.data(), 256), Staging::WriteStatus::kOk);
    // (T-05) 反证前置断言:清零前 [0,256) 有数据,检查器以 (f=0,w=256)
    // 认定其为已提交区,校验通过 —— 证明检查器真实读取了该区间内容。
    EXPECT_TRUE(s.debug_check_invariant(0, 256));
    // 反证:数据仍在 [0,256) 时,把该区间当"必须为零"的空闲区 (f=w=256)
    // 校验必须失败 —— 排除"区域本就全零导致断言恒真"的假绿。
    EXPECT_FALSE(s.debug_check_invariant(256, 256));
    // 模拟 Flusher 清零 [0, 256)
    s.zero_range_locked(0, 256);
    // 清零后 f=0,w=256:除 [0,256) 外全零;且刚清零的区域按空闲区语义
    // (f=w=256) 也必须确认为零,证明 zero_range_locked 真实执行(未漏清)
    EXPECT_TRUE(s.debug_check_invariant(0, 256));
    EXPECT_TRUE(s.debug_check_invariant(256, 256));
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

class MockMmapFailPal : public Pal {
public:
    explicit MockMmapFailPal(Pal* real) : real_(real) {}
    int file_open(const char* path, bool create, bool append) override { return real_->file_open(path, create, append); }
    int file_close(int fd) override { return real_->file_close(fd); }
    int file_truncate(int fd, uint64_t len) override { return real_->file_truncate(fd, len); }
    long file_write(int fd, const void* buf, size_t len) override { return real_->file_write(fd, buf, len); }
    long file_pwrite(int fd, const void* buf, size_t len, uint64_t off) override { return real_->file_pwrite(fd, buf, len, off); }
    long file_pread(int fd, void* buf, size_t len, uint64_t off) override { return real_->file_pread(fd, buf, len, off); }
    int64_t file_size(int fd) override { return real_->file_size(fd); }
    int64_t file_size_path(const char* path) override { return real_->file_size_path(path); }
    int file_fsync(int fd) override { return real_->file_fsync(fd); }
    int file_dir_fsync(const char* dir) override { return real_->file_dir_fsync(dir); }
    int file_unlink(const char* path) override { return real_->file_unlink(path); }
    int file_mkdirs(const char* path) override { return real_->file_mkdirs(path); }
    int file_rmdir(const char* path) override { return real_->file_rmdir(path); }
    std::vector<std::string> file_list(const char* dir) override { return real_->file_list(dir); }
    PalMmap mmap_open(int, size_t, bool) override { return PalMmap{nullptr, -1}; } // 故意注入 mmap 失败!
    int mmap_sync(const PalMmap&, bool) override { return -1; }
    int mmap_close(const PalMmap&) override { return -1; }
    void* mutex_create() override { return real_->mutex_create(); }
    void mutex_destroy(void* h) override { real_->mutex_destroy(h); }
    void mutex_lock(void* h) override { real_->mutex_lock(h); }
    void mutex_unlock(void* h) override { real_->mutex_unlock(h); }
    int threading_init() override { return real_->threading_init(); }
    int thread_start(void* (*fn)(void*), void* arg, void** out) override { return real_->thread_start(fn, arg, out); }
    int thread_join(void* h) override { return real_->thread_join(h); }
    void* ev_create() override { return real_->ev_create(); }
    void ev_destroy(void* h) override { real_->ev_destroy(h); }
    void ev_signal(void* h) override { real_->ev_signal(h); }
    void ev_wait(void* h) override { real_->ev_wait(h); }
    bool ev_wait_timeout(void* h, uint32_t ms) override { return real_->ev_wait_timeout(h, ms); }
    uint64_t atomic_add(uint64_t* p, int64_t d) override { return real_->atomic_add(p, d); }
    uint64_t atomic_load(uint64_t* p) override { return real_->atomic_load(p); }
    uint64_t monotonic_ns() override { return real_->monotonic_ns(); }
    uint64_t wallclock_ms() override { return real_->wallclock_ms(); }
    void wallclock_date(uint32_t* d, uint32_t* t) override { real_->wallclock_date(d, t); }
    void sleep_ms(uint32_t ms) override { real_->sleep_ms(ms); }
    uint32_t pid() override { return real_->pid(); }
    uint64_t tid() override { return real_->tid(); }
    bool random_bytes(void* buf, size_t len) override { return real_->random_bytes(buf, len); }
    void debug_log(const char* s) override { real_->debug_log(s); }
private:
    Pal* real_;
};

TEST_F(StagingTest, MmapFailure_DegradesToHeapMemory) {
    MockMmapFailPal mock(&default_pal());
    Staging s(&mock, path_, kDataSize);
    RecoveryState st; bool rec = false;
    ASSERT_TRUE(s.open(&st, &rec));
    EXPECT_TRUE(s.is_degraded());
    EXPECT_FALSE(st.rebuilt);
    EXPECT_EQ(st.seqno, 0u);

    // 写入 100B 记录验证正常运作
    std::vector<uint8_t> recs(100, 0x42);
    EXPECT_EQ(s.write_records(recs.data(), 100), Staging::WriteStatus::kOk);
    EXPECT_EQ(get_u32(s.raw_base() + 16), 100u);
    EXPECT_TRUE(s.debug_check_invariant(0, 100));

    // 快照与清零
    uint32_t f = 0, snap = 0;
    EXPECT_EQ(s.snapshot_pending(&f, &snap), 100u);
    EXPECT_EQ(f, 0u);
    EXPECT_EQ(snap, 100u);

    std::vector<uint8_t> scratch(100);
    EXPECT_EQ(s.copy_pending(f, snap, scratch.data(), 100), 100u);
    EXPECT_EQ(scratch, recs);

    s.zero_range_locked(0, 100);
    AppendTargetCache atc{};
    s.commit_header_locked(1, 100, 100, &atc);
    EXPECT_TRUE(s.debug_check_invariant(100, 100));

    s.close();
    EXPECT_FALSE(s.is_degraded());
}

TEST_F(StagingTest, Crc32_StandardTestVectors) {
    // S-01: 校验 IEEE 802.3 标准向量 (RFC 3720 / POSIX)
    // "123456789" 的 CRC32 严格等于 0xcbf43926
    const uint8_t standard_input[] = "123456789";
    EXPECT_EQ(crc32_update(0, standard_input, 9), 0xcbf43926u);

    // 单字节已知向量覆盖 (对应 table 特征项)
    const uint8_t b1 = 0x01;
    EXPECT_EQ(crc32_update(0, &b1, 1), 0xa505df1bu);

    const uint8_t b_zero = 0x00;
    EXPECT_EQ(crc32_update(0, &b_zero, 1), 0xd202ef8du);
}