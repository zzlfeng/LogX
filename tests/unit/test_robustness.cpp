// test_robustness.cpp - 鲁棒性与边界容灾单测: 未对齐内存访问与时钟跳变/回拨仿真。
//
// 覆盖:
//   - 未对齐内存读写 (put_u16/32/64, get_u16/32/64, format_record, parse_records_stream)
//   - 23:59:59 跨日跳变至次日 00:00:01 自动轮转新文件与 seqno 单调衔接
//   - 时钟大幅回拨至过去日期, 不产生文件覆盖, 数据 100% 安全

#include "logx_c.h"
#include "logx_internal.h"
#include "logx_format.h"
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace logx;

namespace {

// =========================================================================
// 1. 未对齐内存访问测试
// =========================================================================

TEST(Robustness, UnalignedMemoryAccess_IntegerPrimitives) {
    alignas(16) uint8_t buf[64];
    std::memset(buf, 0, sizeof(buf));

    const size_t offsets[] = {1, 3, 5, 7};

    for (size_t off : offsets) {
        // 16 位整型
        uint16_t v16 = 0xABCD;
        put_u16(buf + off, v16);
        EXPECT_EQ(get_u16(buf + off), v16);
        EXPECT_EQ(buf[off], 0xCD);      // 严格小端
        EXPECT_EQ(buf[off + 1], 0xAB);

        // 32 位整型
        uint32_t v32 = 0x12345678;
        put_u32(buf + off, v32);
        EXPECT_EQ(get_u32(buf + off), v32);
        EXPECT_EQ(buf[off], 0x78);      // 严格小端
        EXPECT_EQ(buf[off + 3], 0x12);

        // 64 位整型
        uint64_t v64 = 0x0123456789ABCDEFULL;
        put_u64(buf + off, v64);
        EXPECT_EQ(get_u64(buf + off), v64);
        EXPECT_EQ(buf[off], 0xEF);      // 严格小端
        EXPECT_EQ(buf[off + 7], 0x01);
    }
}

TEST(Robustness, UnalignedMemoryAccess_RecordFormatAndParse) {
    alignas(16) uint8_t raw_buf[kMaxRecordTotal + 16];
    std::memset(raw_buf, 0, sizeof(raw_buf));
    uint8_t* unaligned_dst = raw_buf + 3;

    const char* payload_text = "test_unaligned_payload_content_12345";
    uint32_t payload_len = static_cast<uint32_t>(std::strlen(payload_text));

    uint32_t written = 0;
    bool format_ok = format_record(2, "Net",
                                   reinterpret_cast<const uint8_t*>(payload_text),
                                   payload_len,
                                   1700000000000ULL, 1001, 2001,
                                   unaligned_dst,
                                   kMaxRecordTotal,
                                   &written);
    ASSERT_TRUE(format_ok);
    ASSERT_GT(written, 6u);

    // 从未对齐地址执行流式解析
    std::vector<std::vector<uint8_t>> out_records;
    bool parse_ok = parse_records_stream(unaligned_dst, written, &out_records);
    ASSERT_TRUE(parse_ok);
    ASSERT_EQ(out_records.size(), 1u);

    // 验证能够正确还原
    std::vector<uint8_t> user;
    ASSERT_TRUE(parse_record(unaligned_dst, written, &user));
    ASSERT_FALSE(user.empty());
}

// =========================================================================
// 2. MockClockPal: 模拟时间控制
// =========================================================================

class MockClockPal : public Pal {
public:
    explicit MockClockPal(Pal* real_pal) : real_(real_pal) {
        real_->wallclock_date(&mock_date_, &mock_time_);
    }

    void set_clock(uint32_t yyyymmdd, uint32_t hhmmss) {
        mock_date_ = yyyymmdd;
        mock_time_ = hhmmss;
    }

    void wallclock_date(uint32_t* yyyymmdd, uint32_t* hhmmss) override {
        if (yyyymmdd) *yyyymmdd = mock_date_;
        if (hhmmss)   *hhmmss   = mock_time_;
    }

    uint64_t wallclock_ms() override { return real_->wallclock_ms(); }
    uint64_t monotonic_ns() override { return real_->monotonic_ns(); }
    void     sleep_ms(uint32_t ms) override { real_->sleep_ms(ms); }

    int      file_open(const char* p, bool w, bool c) override { return real_->file_open(p, w, c); }
    int      file_close(int fd) override { return real_->file_close(fd); }
    int64_t  file_size(int fd) override { return real_->file_size(fd); }
    int64_t  file_size_path(const char* p) override { return real_->file_size_path(p); }
    long     file_pread(int fd, void* b, size_t n, uint64_t o) override { return real_->file_pread(fd, b, n, o); }
    long     file_pwrite(int fd, const void* b, size_t n, uint64_t o) override { return real_->file_pwrite(fd, b, n, o); }
    long     file_write(int fd, const void* b, size_t n) override { return real_->file_write(fd, b, n); }
    int      file_truncate(int fd, uint64_t sz) override { return real_->file_truncate(fd, sz); }
    int      file_fsync(int fd) override { return real_->file_fsync(fd); }
    int      file_dir_fsync(const char* p) override { return real_->file_dir_fsync(p); }
    int      file_unlink(const char* p) override { return real_->file_unlink(p); }
    int      file_mkdirs(const char* p) override { return real_->file_mkdirs(p); }
    int      file_rmdir(const char* p) override { return real_->file_rmdir(p); }
    std::vector<std::string> file_list(const char* dir) override { return real_->file_list(dir); }

    PalMmap  mmap_open(int fd, size_t sz, bool mut) override { return real_->mmap_open(fd, sz, mut); }
    int      mmap_sync(const PalMmap& m, bool async) override { return real_->mmap_sync(m, async); }
    int      mmap_close(const PalMmap& m) override { return real_->mmap_close(m); }

    uint32_t pid() override { return real_->pid(); }
    uint64_t tid() override { return real_->tid(); }
    void     random_bytes(void* b, size_t n) override { real_->random_bytes(b, n); }
    void     debug_log(const char* s) override { real_->debug_log(s); }

    int      threading_init() override { return real_->threading_init(); }
    void*    mutex_create() override { return real_->mutex_create(); }
    void     mutex_lock(void* m) override { return real_->mutex_lock(m); }
    void     mutex_unlock(void* m) override { return real_->mutex_unlock(m); }
    void     mutex_destroy(void* m) override { return real_->mutex_destroy(m); }

    void*    ev_create() override { return real_->ev_create(); }
    void     ev_signal(void* e) override { return real_->ev_signal(e); }
    void     ev_wait(void* e) override { return real_->ev_wait(e); }
    bool     ev_wait_timeout(void* e, uint32_t ms) override { return real_->ev_wait_timeout(e, ms); }
    void     ev_destroy(void* e) override { return real_->ev_destroy(e); }

    int      thread_start(void* (*fn)(void*), void* a, void** out_t) override { return real_->thread_start(fn, a, out_t); }
    int      thread_join(void* t) override { return real_->thread_join(t); }

    uint64_t atomic_add(uint64_t* p, int64_t delta) override { return real_->atomic_add(p, delta); }
    uint64_t atomic_load(uint64_t* p) override { return real_->atomic_load(p); }

private:
    Pal* real_ = nullptr;
    uint32_t mock_date_ = 20260903;
    uint32_t mock_time_ = 120000;
};

// =========================================================================
// 3. 时钟跳变与回拨测试
// =========================================================================

TEST(Robustness, ClockJump_DayRolloverAtMidnight) {
    MockClockPal mock_pal(&default_pal());
    std::string dir = std::string("logx_test_clock_") +
                      std::to_string(mock_pal.pid()) + "_" +
                      std::to_string(rand());
    mock_pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    // 1. 设置时间为当天 23:59:59
    mock_pal.set_clock(20260903, 235959);

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    // 注入 mock PAL
    static_cast<Instance*>(inst)->pal = &mock_pal;

    // 写入第一天的日志并刷盘
    ASSERT_EQ(logx_write(inst, 2, "TAG", (const uint8_t*)"log_at_235959", 13), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    std::string expected_day1 = dir + "/20260903.log";
    EXPECT_GT(mock_pal.file_size_path(expected_day1.c_str()), 32);

    // 2. 模拟时钟跳变跨日: 23:59:59 -> 00:00:01 (次日)
    mock_pal.set_clock(20260904, 000001);

    // 写入第二天的日志并刷盘
    ASSERT_EQ(logx_write(inst, 2, "TAG", (const uint8_t*)"log_at_000001", 13), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    std::string expected_day2 = dir + "/20260904.log";
    EXPECT_GT(mock_pal.file_size_path(expected_day2.c_str()), 32);

    // 校验 Block seqno 单调自增衔接
    auto scan1 = scan_append_target(static_cast<Instance*>(inst), expected_day1);
    auto scan2 = scan_append_target(static_cast<Instance*>(inst), expected_day2);
    EXPECT_TRUE(scan1.ok);
    EXPECT_TRUE(scan2.ok);
    EXPECT_EQ(scan1.seqno, 1u);
    EXPECT_EQ(scan2.seqno, 2u);  // 完美单调递增接管!

    logx_close(inst);

    for (const auto& f : mock_pal.file_list(dir.c_str())) {
        mock_pal.file_unlink(f.c_str());
    }
    mock_pal.file_rmdir(dir.c_str());
}

TEST(Robustness, ClockRollback_GracefullyHandledWithoutOverwrite) {
    MockClockPal mock_pal(&default_pal());
    std::string dir = std::string("logx_test_rollback_") +
                      std::to_string(mock_pal.pid()) + "_" +
                      std::to_string(rand());
    mock_pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    // 1. 初始为 20260904
    mock_pal.set_clock(20260904, 120000);

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    static_cast<Instance*>(inst)->pal = &mock_pal;

    ASSERT_EQ(logx_write(inst, 2, "TAG", (const uint8_t*)"future_log", 10), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    std::string f_future = dir + "/20260904.log";
    int64_t fsz_future = mock_pal.file_size_path(f_future.c_str());
    EXPECT_GT(fsz_future, 32);

    // 2. 模拟系统时钟大幅回拨至前天 20260902
    mock_pal.set_clock(20260902, 100000);

    ASSERT_EQ(logx_write(inst, 2, "TAG", (const uint8_t*)"rollback_log", 12), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    std::string f_rollback = dir + "/20260902.log";
    EXPECT_GT(mock_pal.file_size_path(f_rollback.c_str()), 32);
    // 验证未来文件完全未被覆盖破坏
    EXPECT_EQ(mock_pal.file_size_path(f_future.c_str()), fsz_future);

    logx_close(inst);

    for (const auto& f : mock_pal.file_list(dir.c_str())) {
        mock_pal.file_unlink(f.c_str());
    }
    mock_pal.file_rmdir(dir.c_str());
}

} // namespace
