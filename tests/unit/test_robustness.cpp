// test_robustness.cpp - 鲁棒性与边界容灾单测: 未对齐内存访问、时钟跳变/回拨仿真、写盘短写容灾与持久化同步。
//
// 覆盖:
//   - 未对齐内存读写 (put_u16/32/64, get_u16/32/64, format_record, parse_records_stream)
//   - 23:59:59 跨日跳变至次日 00:00:01 自动轮转新文件与 seqno 单调衔接
//   - 时钟大幅回拨至过去日期, 不产生文件覆盖, 数据 100% 安全
//   - 写盘短写 (0 <= n < size) 与中途 ENOSPC (n < 0 after partial write) 撕裂尾部回滚截断与恢复 (F-03)
//   - 持久化同步与断电检查点 (file_fsync, file_dir_fsync, mmap_sync, durability_sync) (F-04)

#include "logx_c.h"
#include "logx_internal.h"
#include "logx_format.h"
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

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
    bool     random_bytes(void* b, size_t n) override { return real_->random_bytes(b, n); }
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

// =========================================================================
// 4. MockShortWritePal: 模拟写盘短写与物理写入截断容灾 (F-03)
// =========================================================================

class MockShortWritePal : public Pal {
public:
    explicit MockShortWritePal(Pal* real_pal) : real_(real_pal) {}

    enum InjectMode {
        MODE_NORMAL = 0,
        MODE_SHORT_WRITE,             // 物理写入 len / 2 字节并返回 len / 2
        MODE_SHORT_WRITE_ONCE,        // 首次写入 len / 2 字节并返回 len / 2，之后自动恢复 NORMAL
        MODE_MIDSTREAM_ENOSPC         // 物理写入 len / 2 字节但返回 -1 (模拟中途磁盘写满)
    };

    void set_mode(InjectMode m) { mode_.store(m); }
    void set_skip_header(bool skip) { skip_header_.store(skip); }
    void reset() {
        mode_.store(MODE_NORMAL);
        injected_count_.store(0);
        skip_header_.store(true);
    }

    long file_write(int fd, const void* b, size_t n) override {
        if (skip_header_.load() && n == 32) {
            return real_->file_write(fd, b, n);
        }
        int cur_mode = mode_.load();
        if (cur_mode == MODE_SHORT_WRITE) {
            size_t half = n / 2;
            if (half == 0) half = 1;
            return real_->file_write(fd, b, half);
        }
        if (cur_mode == MODE_SHORT_WRITE_ONCE) {
            if (injected_count_.fetch_add(1) == 0) {
                size_t half = n / 2;
                if (half == 0) half = 1;
                return real_->file_write(fd, b, half);
            }
            return real_->file_write(fd, b, n);
        }
        if (cur_mode == MODE_MIDSTREAM_ENOSPC) {
            size_t half = n / 2;
            if (half == 0) half = 1;
            real_->file_write(fd, b, half);
            return -1;
        }
        return real_->file_write(fd, b, n);
    }

    void wallclock_date(uint32_t* yyyymmdd, uint32_t* hhmmss) override { real_->wallclock_date(yyyymmdd, hhmmss); }
    uint64_t wallclock_ms() override { return real_->wallclock_ms(); }
    uint64_t monotonic_ns() override { return real_->monotonic_ns(); }
    void     sleep_ms(uint32_t ms) override { real_->sleep_ms(ms); }

    int      file_open(const char* p, bool w, bool c) override { return real_->file_open(p, w, c); }
    int      file_close(int fd) override { return real_->file_close(fd); }
    int64_t  file_size(int fd) override { return real_->file_size(fd); }
    int64_t  file_size_path(const char* p) override { return real_->file_size_path(p); }
    long     file_pread(int fd, void* b, size_t n, uint64_t o) override { return real_->file_pread(fd, b, n, o); }
    long     file_pwrite(int fd, const void* b, size_t n, uint64_t o) override { return real_->file_pwrite(fd, b, n, o); }
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
    bool     random_bytes(void* b, size_t n) override { return real_->random_bytes(b, n); }
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
    std::atomic<int> mode_{MODE_NORMAL};
    std::atomic<bool> skip_header_{true};
    std::atomic<int> injected_count_{0};
};

struct EventRecord {
    logx_event_kind_e kind;
    int code;
    std::string msg;
};

struct EventCtx {
    std::mutex mu;
    std::vector<EventRecord> events;
};

// Case A: 短写导致物理撕裂块被回滚截断，并记录错误统计与发出告警事件
TEST(Robustness, ShortWrite_RollsBackTornBytesAndEmitsEvent) {
    MockShortWritePal mock_pal(&default_pal());
    std::string dir = std::string("logx_test_short_write_a_") +
                      std::to_string(mock_pal.pid()) + "_" +
                      std::to_string(rand());
    mock_pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    EventCtx ev_ctx;
    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;
    cfg.err_user = &ev_ctx;
    cfg.err_fn = [](void* user, logx_event_kind_e kind, int code, const char* msg) {
        auto* ctx = static_cast<EventCtx*>(user);
        std::lock_guard<std::mutex> lock(ctx->mu);
        ctx->events.push_back({kind, code, msg ? msg : ""});
    };

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    auto* internal_inst = static_cast<Instance*>(inst);
    internal_inst->pal = &mock_pal;

    // 1. 正常写入一条基准日志并落盘，生成有效文件头与第一个数据块
    ASSERT_EQ(logx_write(inst, 2, "INIT", (const uint8_t*)"initial_payload", 15), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    uint32_t ymd = 0, hms = 0;
    mock_pal.wallclock_date(&ymd, &hms);
    std::string log_file = make_log_path(dir, ymd, 0);
    int64_t initial_size = mock_pal.file_size_path(log_file.c_str());
    EXPECT_GT(initial_size, 32);

    // 2. 开启短写注入模式 (物理写入 len / 2 字节并返回 len / 2)
    mock_pal.set_mode(MockShortWritePal::MODE_SHORT_WRITE);

    // 写入第二条日志并触发 flush
    ASSERT_EQ(logx_write(inst, 2, "FAIL", (const uint8_t*)"short_write_payload_test", 24), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    // 3. 验证断言:
    // a) block_write_errors 计数增加
    EXPECT_GE(internal_inst->block_write_errors.load(), 1u);

    // b) 物理文件大小被截断回滚至写前偏移 (无残留撕裂尾部)
    int64_t cur_size = mock_pal.file_size_path(log_file.c_str());
    EXPECT_EQ(cur_size, initial_size);

    // c) scan_one 扫描评估文件健康度，truncated 必须为 false
    FileScanResult sr = scan_one(internal_inst, log_file, /*repair=*/false);
    EXPECT_TRUE(sr.ok);
    EXPECT_FALSE(sr.truncated);
    EXPECT_EQ(sr.last_block_end_offset, static_cast<uint64_t>(initial_size));

    // d) 收到 LOGX_EV_IO 告警且包含 "block short write"
    bool found_short_write_ev = false;
    {
        std::lock_guard<std::mutex> lock(ev_ctx.mu);
        for (const auto& ev : ev_ctx.events) {
            if (ev.kind == LOGX_EV_IO && ev.msg.find("block short write") != std::string::npos) {
                found_short_write_ev = true;
                break;
            }
        }
    }
    EXPECT_TRUE(found_short_write_ev);

    mock_pal.set_mode(MockShortWritePal::MODE_NORMAL);
    logx_close(inst);

    for (const auto& f : mock_pal.file_list(dir.c_str())) {
        mock_pal.file_unlink(f.c_str());
    }
    mock_pal.file_rmdir(dir.c_str());
}

// Case B: 瞬态短写失败被回滚后，后续块能够正常单调追加落盘，文件结构健康可被正常导出
TEST(Robustness, ShortWrite_TransientFailureAllowsSubsequentBlocksToResumeCleanly) {
    MockShortWritePal mock_pal(&default_pal());
    std::string dir = std::string("logx_test_short_write_b_") +
                      std::to_string(mock_pal.pid()) + "_" +
                      std::to_string(rand());
    mock_pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    auto* internal_inst = static_cast<Instance*>(inst);
    internal_inst->pal = &mock_pal;

    // 1. 写入一条基准日志并落盘
    ASSERT_EQ(logx_write(inst, 2, "BASE", (const uint8_t*)"base_block_data", 15), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    uint32_t ymd = 0, hms = 0;
    mock_pal.wallclock_date(&ymd, &hms);
    std::string log_file = make_log_path(dir, ymd, 0);
    int64_t base_size = mock_pal.file_size_path(log_file.c_str());
    EXPECT_GT(base_size, 32);

    // 2. 模拟瞬态短写: 仅下一次写盘注入短写，之后自动恢复正常
    mock_pal.set_mode(MockShortWritePal::MODE_SHORT_WRITE_ONCE);

    // 写入将触发短写的块
    ASSERT_EQ(logx_write(inst, 2, "ERR", (const uint8_t*)"this_block_will_fail", 20), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    // 短写被回滚，文件大小仍为 base_size
    EXPECT_EQ(mock_pal.file_size_path(log_file.c_str()), base_size);

    // 3. 写入后续正常块 (此时 mock_pal 已自动恢复 MODE_NORMAL)
    ASSERT_EQ(logx_write(inst, 2, "NEXT", (const uint8_t*)"resumed_block_payload", 21), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    // 文件成功增长
    int64_t resumed_size = mock_pal.file_size_path(log_file.c_str());
    EXPECT_GT(resumed_size, base_size);

    // 4. 验证后续块紧随基准块写入，中间无撕裂损坏，可被 scan_one 评估
    FileScanResult sr = scan_one(internal_inst, log_file, /*repair=*/false);
    EXPECT_TRUE(sr.ok);
    EXPECT_FALSE(sr.truncated);
    EXPECT_EQ(sr.last_block_end_offset, static_cast<uint64_t>(resumed_size));

    // 显式确保处于 NORMAL 模式以进行 export
    mock_pal.set_mode(MockShortWritePal::MODE_NORMAL);

    // 5. 使用 logx_export 验证日志解码导出
    char date_str[16];
    std::snprintf(date_str, sizeof(date_str), "%08u", ymd);
    struct ExportCtx {
        Pal* pal = nullptr;
        int count = 0;
        std::string content;
    } exp_ctx;
    exp_ctx.pal = &mock_pal;
    auto exp_cb = [](void* user, const char* /*date*/, const char* plaintext_path, uint64_t size) -> int {
        auto* ctx = static_cast<ExportCtx*>(user);
        ctx->count++;
        int fd = ctx->pal->file_open(plaintext_path, false, false);
        if (fd >= 0) {
            std::vector<char> buf(size);
            if (ctx->pal->file_pread(fd, buf.data(), size, 0) == static_cast<long>(size)) {
                ctx->content.append(buf.data(), size);
            }
            ctx->pal->file_close(fd);
        }
        return 0;
    };
    logx_status_e exp_st = logx_export(inst, date_str, date_str, exp_cb, &exp_ctx);
    EXPECT_EQ(exp_st, LOGX_OK);
    EXPECT_GE(exp_ctx.count, 1);
    EXPECT_EQ(exp_ctx.content.find("decode fail"), std::string::npos);
    EXPECT_NE(exp_ctx.content.find("seq="), std::string::npos);

    logx_close(inst);

    for (const auto& f : mock_pal.file_list(dir.c_str())) {
        mock_pal.file_unlink(f.c_str());
    }
    mock_pal.file_rmdir(dir.c_str());
}

// Case C: 模拟中途磁盘满 (写入部分数据后返回 -1)，验证物理残留被回滚截断
TEST(Robustness, ShortWrite_MidStreamEnospcTruncatesTornTail) {
    MockShortWritePal mock_pal(&default_pal());
    std::string dir = std::string("logx_test_short_write_c_") +
                      std::to_string(mock_pal.pid()) + "_" +
                      std::to_string(rand());
    mock_pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    auto* internal_inst = static_cast<Instance*>(inst);
    internal_inst->pal = &mock_pal;

    // 1. 基准写入一条日志并落盘
    ASSERT_EQ(logx_write(inst, 2, "BASE", (const uint8_t*)"base_data_for_c", 15), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    uint32_t ymd = 0, hms = 0;
    mock_pal.wallclock_date(&ymd, &hms);
    std::string log_file = make_log_path(dir, ymd, 0);
    int64_t base_size = mock_pal.file_size_path(log_file.c_str());
    EXPECT_GT(base_size, 32);

    // 2. 模拟 mid-stream ENOSPC: 物理写入部分数据后返回 -1
    mock_pal.set_mode(MockShortWritePal::MODE_MIDSTREAM_ENOSPC);

    ASSERT_EQ(logx_write(inst, 2, "ENOSPC", (const uint8_t*)"enospc_failure_payload", 22), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    // 3. 验证撕裂的物理尾部被 write_and_record 回滚截断至 base_size
    int64_t cur_size = mock_pal.file_size_path(log_file.c_str());
    EXPECT_EQ(cur_size, base_size);

    // 4. scan_one 必须评估为健康，truncated = false
    FileScanResult sr = scan_one(internal_inst, log_file, /*repair=*/false);
    EXPECT_TRUE(sr.ok);
    EXPECT_FALSE(sr.truncated);
    EXPECT_EQ(sr.last_block_end_offset, static_cast<uint64_t>(base_size));

    mock_pal.set_mode(MockShortWritePal::MODE_NORMAL);
    logx_close(inst);

    for (const auto& f : mock_pal.file_list(dir.c_str())) {
        mock_pal.file_unlink(f.c_str());
    }
    mock_pal.file_rmdir(dir.c_str());
}

// =========================================================================
// 5. MockFsyncPal: 持久化与断电检查点同步模拟 (F-04)
// =========================================================================

class MockFsyncPal : public Pal {
public:
    explicit MockFsyncPal(Pal* real_pal) : real_(real_pal) {}

    void reset_counts() {
        file_fsync_count.store(0);
        file_fsync_success_count.store(0);
        file_dir_fsync_count.store(0);
        file_dir_fsync_success_count.store(0);
        mmap_sync_count.store(0);
        mmap_sync_success_count.store(0);
        mmap_sync_sync_count.store(0);
        mmap_sync_async_count.store(0);
        {
            std::lock_guard<std::mutex> lock(dir_mu);
            synced_dirs.clear();
        }
        fail_file_fsync.store(false);
        fail_dir_fsync.store(false);
        fail_mmap_sync.store(false);
    }

    int file_fsync(int fd) override {
        file_fsync_count.fetch_add(1);
        if (fail_file_fsync.load()) {
            return -1;
        }
        int rc = real_->file_fsync(fd);
        if (rc == 0) {
            file_fsync_success_count.fetch_add(1);
        }
        return rc;
    }

    int file_dir_fsync(const char* p) override {
        file_dir_fsync_count.fetch_add(1);
        if (p) {
            std::lock_guard<std::mutex> lock(dir_mu);
            synced_dirs.push_back(p);
        }
        if (fail_dir_fsync.load()) {
            return -1;
        }
        int rc = real_->file_dir_fsync(p);
        if (rc == 0) {
            file_dir_fsync_success_count.fetch_add(1);
        }
        return rc;
    }

    int mmap_sync(const PalMmap& m, bool synchronous) override {
        mmap_sync_count.fetch_add(1);
        if (synchronous) {
            mmap_sync_sync_count.fetch_add(1);
        } else {
            mmap_sync_async_count.fetch_add(1);
        }
        if (fail_mmap_sync.load()) {
            return -1;
        }
        int rc = real_->mmap_sync(m, synchronous);
        if (rc == 0) {
            mmap_sync_success_count.fetch_add(1);
        }
        return rc;
    }

    // 基础代理接口
    void wallclock_date(uint32_t* yyyymmdd, uint32_t* hhmmss) override { real_->wallclock_date(yyyymmdd, hhmmss); }
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
    int      file_unlink(const char* p) override { return real_->file_unlink(p); }
    int      file_mkdirs(const char* p) override { return real_->file_mkdirs(p); }
    int      file_rmdir(const char* p) override { return real_->file_rmdir(p); }
    std::vector<std::string> file_list(const char* dir) override { return real_->file_list(dir); }

    PalMmap  mmap_open(int fd, size_t sz, bool mut) override { return real_->mmap_open(fd, sz, mut); }
    int      mmap_close(const PalMmap& m) override { return real_->mmap_close(m); }

    uint32_t pid() override { return real_->pid(); }
    uint64_t tid() override { return real_->tid(); }
    bool     random_bytes(void* b, size_t n) override { return real_->random_bytes(b, n); }
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

    std::atomic<int> file_fsync_count{0};
    std::atomic<int> file_fsync_success_count{0};
    std::atomic<int> file_dir_fsync_count{0};
    std::atomic<int> file_dir_fsync_success_count{0};
    std::atomic<int> mmap_sync_count{0};
    std::atomic<int> mmap_sync_success_count{0};
    std::atomic<int> mmap_sync_sync_count{0};
    std::atomic<int> mmap_sync_async_count{0};

    std::mutex dir_mu;
    std::vector<std::string> synced_dirs;

    std::atomic<bool> fail_file_fsync{false};
    std::atomic<bool> fail_dir_fsync{false};
    std::atomic<bool> fail_mmap_sync{false};

private:
    Pal* real_ = nullptr;
};

// 1. 退后台 logx_flush 触发日志文件与 staging 的强同步
TEST(Robustness, Persistence_FlushTriggersFileAndStagingFsync) {
    MockFsyncPal mock_pal(&default_pal());
    std::string dir = std::string("logx_test_fsync_flush_") +
                      std::to_string(mock_pal.pid()) + "_" +
                      std::to_string(rand());
    mock_pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    auto* internal_inst = static_cast<Instance*>(inst);
    internal_inst->pal = &mock_pal;
    if (internal_inst->staging) {
        internal_inst->staging->set_pal(&mock_pal);
    }

    mock_pal.reset_counts();

    // 写入日志并触发退后台 flush
    ASSERT_EQ(logx_write(inst, 2, "FSYNC", (const uint8_t*)"test_flush_fsync_payload", 24), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    // 验证:
    // 1) 日志文件被 fsync (file_fsync_count >= 1 且底层系统调用成功)
    EXPECT_GE(mock_pal.file_fsync_count.load(), 1);
    EXPECT_GE(mock_pal.file_fsync_success_count.load(), 1);
    // 2) staging 环形缓冲区被显式强同步 (mmap_sync_sync_count >= 1 且底层系统调用成功)
    EXPECT_GE(mock_pal.mmap_sync_sync_count.load(), 1);
    EXPECT_GE(mock_pal.mmap_sync_success_count.load(), 1);

    logx_close(inst);

    for (const auto& f : mock_pal.file_list(dir.c_str())) {
        mock_pal.file_unlink(f.c_str());
    }
    mock_pal.file_rmdir(dir.c_str());
}

// 2. durability_sync 直接调用与无 active fd 时的优雅处理
TEST(Robustness, Persistence_DurabilitySyncDirectAndGraceful) {
    MockFsyncPal mock_pal(&default_pal());
    std::string dir = std::string("logx_test_durability_direct_") +
                      std::to_string(mock_pal.pid()) + "_" +
                      std::to_string(rand());
    mock_pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    auto* internal_inst = static_cast<Instance*>(inst);
    internal_inst->pal = &mock_pal;
    if (internal_inst->staging) {
        internal_inst->staging->set_pal(&mock_pal);
    }

    mock_pal.reset_counts();

    // 1. 无日志写入时直接调用 durability_sync: 必须优雅返回 0 (无 active fd 也不应报错)
    uint64_t sync_ms = 0;
    int rc = durability_sync(internal_inst, &sync_ms);
    EXPECT_EQ(rc, 0);
    EXPECT_GT(sync_ms, 0u);
    EXPECT_GE(mock_pal.mmap_sync_sync_count.load(), 1);
    EXPECT_GE(mock_pal.mmap_sync_success_count.load(), 1);

    // 2. 写入日志并 flush，使 active_log_fd 成功建立
    ASSERT_EQ(logx_write(inst, 2, "DIRECT", (const uint8_t*)"durability_direct_payload", 25), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    mock_pal.reset_counts();
    sync_ms = 0;
    rc = durability_sync(internal_inst, &sync_ms);
    EXPECT_EQ(rc, 0);
    EXPECT_GT(sync_ms, 0u);
    EXPECT_GE(mock_pal.file_fsync_count.load(), 1);
    EXPECT_GE(mock_pal.file_fsync_success_count.load(), 1);
    EXPECT_GE(mock_pal.mmap_sync_sync_count.load(), 1);
    EXPECT_GE(mock_pal.mmap_sync_success_count.load(), 1);

    logx_close(inst);

    for (const auto& f : mock_pal.file_list(dir.c_str())) {
        mock_pal.file_unlink(f.c_str());
    }
    mock_pal.file_rmdir(dir.c_str());
}

// 3. 新建日志文件必须同步父目录 (spec §10 / T4.0-12)
TEST(Robustness, Persistence_NewFileTriggersParentDirFsync) {
    MockFsyncPal mock_pal(&default_pal());
    std::string dir = std::string("logx_test_fsync_dir_") +
                      std::to_string(mock_pal.pid()) + "_" +
                      std::to_string(rand());
    mock_pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    auto* internal_inst = static_cast<Instance*>(inst);
    internal_inst->pal = &mock_pal;
    if (internal_inst->staging) {
        internal_inst->staging->set_pal(&mock_pal);
    }

    mock_pal.reset_counts();

    // 写入第一条日志，触发 open_or_create_append_target (新建 32B 文件头) 并落盘
    ASSERT_EQ(logx_write(inst, 2, "DIR_SYNC", (const uint8_t*)"test_dir_sync_content", 21), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    // 验证:
    // 1) 目录同步函数必须至少被调用 1 次 (spec §10 / T4.0-12)
    EXPECT_GE(mock_pal.file_dir_fsync_count.load(), 1);
    // 2) 目录同步必须真正执行成功 (验证底层 FlushFileBuffers / fsync 成功且权限无 ACCESS_DENIED)
    EXPECT_GE(mock_pal.file_dir_fsync_success_count.load(), 1);

    // 3) 同步的目录路径必须与 log_dir 一致
    bool found_dir = false;
    {
        std::lock_guard<std::mutex> lock(mock_pal.dir_mu);
        for (const auto& d : mock_pal.synced_dirs) {
            if (d.find(dir) != std::string::npos) {
                found_dir = true;
                break;
            }
        }
    }
    EXPECT_TRUE(found_dir);

    logx_close(inst);

    for (const auto& f : mock_pal.file_list(dir.c_str())) {
        mock_pal.file_unlink(f.c_str());
    }
    mock_pal.file_rmdir(dir.c_str());
}

// 4. logx_close 触发终态持久化同步 (spec §11)
TEST(Robustness, Persistence_CloseTriggersFinalSync) {
    MockFsyncPal mock_pal(&default_pal());
    std::string dir = std::string("logx_test_fsync_close_") +
                      std::to_string(mock_pal.pid()) + "_" +
                      std::to_string(rand());
    mock_pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    auto* internal_inst = static_cast<Instance*>(inst);
    internal_inst->pal = &mock_pal;
    if (internal_inst->staging) {
        internal_inst->staging->set_pal(&mock_pal);
    }

    // 写入日志并刷新，使活跃句柄建立
    ASSERT_EQ(logx_write(inst, 2, "CLOSE", (const uint8_t*)"test_close_sync_payload", 23), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    mock_pal.reset_counts();

    // 调用 logx_close，必须在关闭 fd 与 staging 前触发终态持久化同步 (spec §11)
    logx_close(inst);

    EXPECT_GE(mock_pal.file_fsync_count.load(), 1);
    EXPECT_GE(mock_pal.file_fsync_success_count.load(), 1);
    EXPECT_GE(mock_pal.mmap_sync_sync_count.load(), 1);
    EXPECT_GE(mock_pal.mmap_sync_success_count.load(), 1);

    for (const auto& f : mock_pal.file_list(dir.c_str())) {
        mock_pal.file_unlink(f.c_str());
    }
    mock_pal.file_rmdir(dir.c_str());
}

// 5. fsync 失败时发出 LOGX_EV_IO 告警且不阻断后续正常写入 (非致命容灾)
TEST(Robustness, Persistence_FsyncFailureEmitsEventWithoutBlocking) {
    MockFsyncPal mock_pal(&default_pal());
    std::string dir = std::string("logx_test_fsync_fail_") +
                      std::to_string(mock_pal.pid()) + "_" +
                      std::to_string(rand());
    mock_pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    EventCtx ev_ctx;
    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;
    cfg.err_user = &ev_ctx;
    cfg.err_fn = [](void* user, logx_event_kind_e kind, int code, const char* msg) {
        auto* ctx = static_cast<EventCtx*>(user);
        std::lock_guard<std::mutex> lock(ctx->mu);
        ctx->events.push_back({kind, code, msg ? msg : ""});
    };

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    auto* internal_inst = static_cast<Instance*>(inst);
    internal_inst->pal = &mock_pal;
    if (internal_inst->staging) {
        internal_inst->staging->set_pal(&mock_pal);
    }

    // 建立 active_log_fd
    ASSERT_EQ(logx_write(inst, 2, "BASE", (const uint8_t*)"base_fsync_data", 15), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    // 开启 fsync 失败注入
    mock_pal.fail_file_fsync.store(true);

    // 执行 durability_sync
    uint64_t sync_ms = 0;
    int rc = durability_sync(internal_inst, &sync_ms);
    EXPECT_EQ(rc, -1);

    // 验证发出 LOGX_EV_IO 告警且包含 "durability sync" 或 "fsync"
    bool found_fsync_err_ev = false;
    {
        std::lock_guard<std::mutex> lock(ev_ctx.mu);
        for (const auto& ev : ev_ctx.events) {
            if (ev.kind == LOGX_EV_IO && (ev.msg.find("durability sync") != std::string::npos ||
                                          ev.msg.find("fsync") != std::string::npos)) {
                found_fsync_err_ev = true;
                break;
            }
        }
    }
    EXPECT_TRUE(found_fsync_err_ev);

    // 验证随后的正常写入不受阻断 (非致命容灾)
    EXPECT_EQ(logx_write(inst, 2, "NEXT", (const uint8_t*)"after_fsync_failure", 19), LOGX_OK);

    // 恢复正常以安全关闭
    mock_pal.fail_file_fsync.store(false);
    logx_close(inst);

    for (const auto& f : mock_pal.file_list(dir.c_str())) {
        mock_pal.file_unlink(f.c_str());
    }
    mock_pal.file_rmdir(dir.c_str());
}

TEST(Robustness, Win64HandleTruncation_PalMmapAndThreadHandle64BitSafe) {
    // 1. 结构宽度断言: PalMmap.out_handle 必须严格具备指针宽度 (P-01 核心契约)
    EXPECT_EQ(sizeof(PalMmap::out_handle), sizeof(void*));

    Pal& pal = default_pal();

#if defined(_WIN32)
    // 2. 纯值往返数学验证: 构造高 32 位非零的 64 位 HANDLE (验证无 32 位截断)
    const uintptr_t kHighVal = 0xFFFFFFFF87654321ULL;
    void* raw_ptr = reinterpret_cast<void*>(kHighVal);
    HANDLE h_test = reinterpret_cast<HANDLE>(raw_ptr);
    intptr_t stored = reinterpret_cast<intptr_t>(h_test);
    HANDLE h_restored = reinterpret_cast<HANDLE>(stored);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(h_restored), kHighVal);

    // 异常参数边界校验
    EXPECT_EQ(pal.thread_join(nullptr), -1);
    EXPECT_EQ(pal.thread_join(INVALID_HANDLE_VALUE), -1);
#else
    EXPECT_EQ(pal.thread_join(nullptr), -1);
#endif

    // 3. 32 轮并发线程 Spawn & Join 压力循环 (验证句柄表分配增长与释放无误)
    for (int i = 0; i < 32; ++i) {
        struct ThreadCtx {
            std::atomic<bool> done{false};
        } tc;
        void* th = nullptr;
        ASSERT_EQ(pal.thread_start([](void* p) -> void* {
            auto* c = static_cast<ThreadCtx*>(p);
            c->done.store(true);
            return nullptr;
        }, &tc, &th), 0);
        ASSERT_NE(th, nullptr);
        EXPECT_EQ(pal.thread_join(th), 0);
        EXPECT_TRUE(tc.done.load());
    }

    // 4. Staging 多轮 Open -> Sync -> Close -> Reopen 生命周期循环 (验证 mapping HANDLE 正确关闭)
    std::string dir = std::string("logx_test_p01_mmap_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());
    std::string staging_path = dir + "/staging.mmap";

    for (int iter = 0; iter < 5; ++iter) {
        Staging st(&pal, staging_path, 64 * 1024);
        RecoveryState rstate{};
        bool recovered = false;
        ASSERT_TRUE(st.open(&rstate, &recovered));
        EXPECT_EQ(st.mmap_sync_now(), 0);
        st.close();
    }

    // 清理
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

// =========================================================================
// 8. F-09 random_bytes 故障注入与熵质量验证
// =========================================================================

class FailingRandomPal : public Pal {
public:
    explicit FailingRandomPal(Pal* real) : real_(real) {}

    int  file_open(const char* p, bool c, bool a) override { return real_->file_open(p, c, a); }
    int  file_close(int fd) override { return real_->file_close(fd); }
    int  file_truncate(int fd, uint64_t l) override { return real_->file_truncate(fd, l); }
    long file_write(int fd, const void* b, size_t l) override { return real_->file_write(fd, b, l); }
    long file_pwrite(int fd, const void* b, size_t l, uint64_t o) override { return real_->file_pwrite(fd, b, l, o); }
    long file_pread(int fd, void* b, size_t l, uint64_t o) override { return real_->file_pread(fd, b, l, o); }
    int64_t file_size(int fd) override { return real_->file_size(fd); }
    int64_t file_size_path(const char* p) override { return real_->file_size_path(p); }
    int  file_fsync(int fd) override { return real_->file_fsync(fd); }
    int  file_dir_fsync(const char* d) override { return real_->file_dir_fsync(d); }
    int  file_unlink(const char* p) override { return real_->file_unlink(p); }
    int  file_mkdirs(const char* d) override { return real_->file_mkdirs(d); }
    int  file_rmdir(const char* d) override { return real_->file_rmdir(d); }
    std::vector<std::string> file_list(const char* d) override { return real_->file_list(d); }

    PalMmap mmap_open(int fd, size_t l, bool w) override { return real_->mmap_open(fd, l, w); }
    int     mmap_sync(const PalMmap& m, bool a) override { return real_->mmap_sync(m, a); }
    int     mmap_close(const PalMmap& m) override { return real_->mmap_close(m); }

    void* mutex_create() override { return real_->mutex_create(); }
    void  mutex_destroy(void* h) override { real_->mutex_destroy(h); }
    void  mutex_lock(void* h) override { return real_->mutex_lock(h); }
    void  mutex_unlock(void* h) override { return real_->mutex_unlock(h); }

    int  threading_init() override { return real_->threading_init(); }
    int  thread_start(void* (*fn)(void*), void* arg, void** out) override { return real_->thread_start(fn, arg, out); }
    int  thread_join(void* h) override { return real_->thread_join(h); }

    void* ev_create() override { return real_->ev_create(); }
    void  ev_destroy(void* h) override { real_->ev_destroy(h); }
    void  ev_signal(void* h) override { return real_->ev_signal(h); }
    void  ev_wait(void* h) override { return real_->ev_wait(h); }
    bool  ev_wait_timeout(void* h, uint32_t ms) override { return real_->ev_wait_timeout(h, ms); }

    uint64_t atomic_add(uint64_t* p, int64_t d) override { return real_->atomic_add(p, d); }
    uint64_t atomic_load(uint64_t* p) override { return real_->atomic_load(p); }

    uint64_t monotonic_ns() override { return real_->monotonic_ns(); }
    uint64_t wallclock_ms() override { return real_->wallclock_ms(); }
    void     wallclock_date(uint32_t* y, uint32_t* h) override { real_->wallclock_date(y, h); }
    void     sleep_ms(uint32_t ms) override { real_->sleep_ms(ms); }

    uint32_t pid() override { return real_->pid(); }
    uint64_t tid() override { return real_->tid(); }

    bool random_bytes(void* buf, size_t len) override {
        if (fail_random_) {
            std::memset(buf, 0, len);
            return false;
        }
        return real_->random_bytes(buf, len);
    }

    void debug_log(const char* s) override { real_->debug_log(s); }

    void set_fail_random(bool f) { fail_random_ = f; }

private:
    Pal* real_;
    bool fail_random_ = true;
};

// 验证 CSPRNG 失败时拒绝创建新文件、发射 LOGX_EV_IO 明确报警、不卡死且拒绝低熵服务
TEST(PalRobustness, RandomBytesFailure_RefusesNewFileCreation) {
    Pal& real_pal = default_pal();
    FailingRandomPal failing_pal(&real_pal);
    failing_pal.set_fail_random(true);

    std::string dir = std::string("logx_test_rand_fail_") +
                      std::to_string(real_pal.pid()) + "_" +
                      std::to_string(rand());
    real_pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    std::atomic<bool> saw_event{false};
    std::string event_msg;

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;
    cfg.err_fn = [](void* user, logx_event_kind_e kind, int code, const char* msg) {
        (void)code;
        if (kind == LOGX_EV_IO && msg && std::strstr(msg, "secure random unavailable")) {
            auto* p = static_cast<std::pair<std::atomic<bool>*, std::string*>*>(user);
            p->first->store(true);
            *p->second = msg;
        }
    };
    std::pair<std::atomic<bool>*, std::string*> arg_pair{&saw_event, &event_msg};
    cfg.err_user = &arg_pair;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    // 注入 failing_pal
    static_cast<Instance*>(inst)->pal = &failing_pal;

    // 写入日志: 正常入 staging
    const char* line = "test log while secure random unavailable";
    EXPECT_EQ(logx_write(inst, 1, "TAG", reinterpret_cast<const uint8_t*>(line),
                         static_cast<uint32_t>(std::strlen(line))), LOGX_OK);

    // flush: 触发 Flusher 尝试建文件写盘, 因无法获取安全盐拒绝建文件, 走块丢弃路径推进水位快速返回
    EXPECT_EQ(logx_flush(inst), LOGX_OK);

    // 验证捕获到明确的告警事件
    EXPECT_TRUE(saw_event.load());
    EXPECT_FALSE(event_msg.empty());

    // 验证目录中没有生成非零字节的 .log 文件 (拒绝创建低熵密文文件)
    auto files = real_pal.file_list(dir.c_str());
    for (const auto& f : files) {
        if (f.find(".log") != std::string::npos) {
            EXPECT_EQ(real_pal.file_size_path(f.c_str()), 0);
        }
    }

    logx_close(inst);

    // 清理
    for (const auto& f : real_pal.file_list(dir.c_str())) {
        real_pal.file_unlink(f.c_str());
    }
    real_pal.file_rmdir(dir.c_str());
}

// 验证正常路径下成功生成的 8B 文件盐非全零且非全同 (防退化填充漏网)
TEST(PalRobustness, RandomBytesSuccess_ProducesNonTrivialSalt) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_rand_succ_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    const char* line = "hello secure salt";
    EXPECT_EQ(logx_write(inst, 1, "TAG", reinterpret_cast<const uint8_t*>(line),
                         static_cast<uint32_t>(std::strlen(line))), LOGX_OK);
    EXPECT_EQ(logx_flush(inst), LOGX_OK);
    logx_close(inst);

    // 找到生成的 .log 文件
    auto files = pal.file_list(dir.c_str());
    std::string log_file;
    for (const auto& f : files) {
        if (f.find(".log") != std::string::npos && pal.file_size_path(f.c_str()) >= 32) {
            log_file = f;
            break;
        }
    }
    ASSERT_FALSE(log_file.empty());

    // 读 32B 文件头
    int fd = pal.file_open(log_file.c_str(), false, false);
    ASSERT_GE(fd, 0);
    uint8_t hdr[32];
    ASSERT_EQ(pal.file_pread(fd, hdr, 32, 0), 32);
    pal.file_close(fd);

    // 提取 salt (hdr[12..19])
    uint8_t salt[8];
    std::memcpy(salt, hdr + 12, 8);

    // 验证盐非全零且非全同字节
    bool all_zero = true;
    bool all_same = true;
    for (int i = 0; i < 8; ++i) {
        if (salt[i] != 0) all_zero = false;
        if (salt[i] != salt[0]) all_same = false;
    }
    EXPECT_FALSE(all_zero);
    EXPECT_FALSE(all_same);

    // 清理
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

} // namespace
