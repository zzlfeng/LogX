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
#include <cerrno>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

// (T-10) 原子自增目录序号:替代未播种 rand(),消除并行/重跑目录撞名
static std::atomic<uint64_t> s_test_dir_seq{0};

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
                      std::to_string(s_test_dir_seq.fetch_add(1));
    mock_pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    // 1. 设置时间为当天 23:59:59
    mock_pal.set_clock(20260903, 235959);

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    cfg.pal_override = &mock_pal;  // (T-04) Flusher 启动前定型 PAL,根除注入竞态
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

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
                      std::to_string(s_test_dir_seq.fetch_add(1));
    mock_pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    // 1. 初始为 20260904
    mock_pal.set_clock(20260904, 120000);

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    cfg.pal_override = &mock_pal;  // (T-04) Flusher 启动前定型 PAL,根除注入竞态
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

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

    // (T-07) 补齐两项核心保证断言:
    // 1. 回拨写入发生在回拨日期文件 (文件切换正确,而非误追加到未来文件)
    // 2. 回拨后新文件的块 seqno 严格大于未来文件块 seqno (全局单调,不回退)
    auto scan_future = scan_append_target(static_cast<Instance*>(inst), f_future);
    auto scan_rollback = scan_append_target(static_cast<Instance*>(inst), f_rollback);
    EXPECT_TRUE(scan_future.ok);
    EXPECT_TRUE(scan_rollback.ok);
    EXPECT_EQ(scan_future.seqno, 1u);       // 未来文件恰 1 块 (seqno=1)
    EXPECT_EQ(scan_rollback.seqno, 2u);     // 回拨文件块 seqno=2 > 1,单调接管
    EXPECT_FALSE(scan_rollback.truncated);  // 干净追加,无撕裂截断

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
                      std::to_string(s_test_dir_seq.fetch_add(1));
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
    cfg.pal_override = &mock_pal;  // (T-04) Flusher 启动前定型 PAL,根除注入竞态
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
auto* internal_inst = static_cast<Instance*>(inst);


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
                      std::to_string(s_test_dir_seq.fetch_add(1));
    mock_pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    cfg.pal_override = &mock_pal;  // (T-04) Flusher 启动前定型 PAL,根除注入竞态
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
auto* internal_inst = static_cast<Instance*>(inst);


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
                      std::to_string(s_test_dir_seq.fetch_add(1));
    mock_pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    cfg.pal_override = &mock_pal;  // (T-04) Flusher 启动前定型 PAL,根除注入竞态
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
auto* internal_inst = static_cast<Instance*>(inst);


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
                      std::to_string(s_test_dir_seq.fetch_add(1));
    mock_pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    cfg.pal_override = &mock_pal;  // (T-04) Flusher 启动前定型 PAL,根除注入竞态
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
auto* internal_inst = static_cast<Instance*>(inst);

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
                      std::to_string(s_test_dir_seq.fetch_add(1));
    mock_pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    cfg.pal_override = &mock_pal;  // (T-04) Flusher 启动前定型 PAL,根除注入竞态
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
auto* internal_inst = static_cast<Instance*>(inst);

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
                      std::to_string(s_test_dir_seq.fetch_add(1));
    mock_pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    cfg.pal_override = &mock_pal;  // (T-04) Flusher 启动前定型 PAL,根除注入竞态
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
auto* internal_inst = static_cast<Instance*>(inst);

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
                      std::to_string(s_test_dir_seq.fetch_add(1));
    mock_pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    cfg.pal_override = &mock_pal;  // (T-04) Flusher 启动前定型 PAL,根除注入竞态
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
auto* internal_inst = static_cast<Instance*>(inst);

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
                      std::to_string(s_test_dir_seq.fetch_add(1));
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
    cfg.pal_override = &mock_pal;  // (T-04) Flusher 启动前定型 PAL,根除注入竞态
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
auto* internal_inst = static_cast<Instance*>(inst);

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
                      std::to_string(s_test_dir_seq.fetch_add(1));
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
                      std::to_string(s_test_dir_seq.fetch_add(1));
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
    cfg.pal_override = &failing_pal;  // (T-04) Flusher 启动前定型 PAL,根除注入竞态
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

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
                      std::to_string(s_test_dir_seq.fetch_add(1));
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

// =========================================================================
// 9. E 系列:C ABI 与导出模块回归
// =========================================================================

// E-01: logx_set_error_handler 与后台 Flusher emit_event 并发——注册/注销
// 高频切换期间持续产生事件,验证无崩溃、无脏指针回调(数据竞争回归)。
TEST(Robustness, ErrorHandler_ConcurrentReregistrationNoRace) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_e01_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    std::atomic<bool> stop{false};
    std::atomic<int> callback_calls{0};
    // 后台线程高频注册/注销错误回调(模拟任意线程注册契约)
    std::thread churn([&inst, &stop, &callback_calls]() {
        int i = 0;
        while (!stop.load()) {
            if (i % 2 == 0) {
                logx_set_error_handler(inst, [](void* user, logx_event_kind_e,
                                                int, const char*) {
                    static_cast<std::atomic<int>*>(user)->fetch_add(1);
                }, &callback_calls);
            } else {
                logx_set_error_handler(inst, nullptr, nullptr);
            }
            ++i;
        }
    });

    // 主线程持续写入小日志,驱动 Flusher 产生 emit_event 并发读
    const uint8_t payload[] = "e01_race_probe";
    for (int i = 0; i < 2000; ++i) {
        ASSERT_EQ(logx_write(inst, 2, "E01", payload, sizeof(payload) - 1), LOGX_OK);
        if (i % 200 == 0) logx_flush(inst);
    }
    logx_flush(inst);

    stop.store(true);
    churn.join();
    EXPECT_GE(callback_calls.load(), 0);  // 存活即通过;计数仅防优化

    logx_close(inst);

    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

// E-02: 非法输入(level 越界)不再被误分类为 TOO_LARGE,
// 且 drop_too_large 不递增;真正超长的输入走 B-1B 方案B 截断保前缀路径
// (LOGX_OK + truncated_logs 递增),不再整条丢弃。
TEST(Robustness, WriteInvalidLevel_ClassifiedAsInvalidArg) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_e02_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 500;
    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    auto* ii = static_cast<Instance*>(inst);
    uint64_t too_large_before = ii->drop_too_large.load();
    uint64_t truncated_before = ii->truncated_logs.load();

    const uint8_t payload[] = "payload";

    // level 越界(负值与 > 5):INVALID_ARG,且不影响超长/截断计数
    EXPECT_EQ(logx_write(inst, -1, "E02", payload, sizeof(payload) - 1),
              LOGX_ERR_INVALID_ARG);
    EXPECT_EQ(logx_write(inst, 6, "E02", payload, sizeof(payload) - 1),
              LOGX_ERR_INVALID_ARG);
    EXPECT_EQ(ii->drop_too_large.load(), too_large_before);
    EXPECT_EQ(ii->truncated_logs.load(), truncated_before);

    // 空 tag 是合法输入(format_record 允许 tag_len=0),正常写入
    EXPECT_EQ(logx_write(inst, 2, "", payload, sizeof(payload) - 1), LOGX_OK);
    EXPECT_EQ(ii->drop_too_large.load(), too_large_before);
    EXPECT_EQ(ii->truncated_logs.load(), truncated_before);

    // 真正超长(600B vs 上限 500B):B-1B 方案B 截断保前缀,返回 LOGX_OK,
    // truncated_logs 递增,drop_too_large 不动
    std::string large(600, 'L');
    EXPECT_EQ(logx_write(inst, 2, "E02",
                         reinterpret_cast<const uint8_t*>(large.data()),
                         static_cast<uint32_t>(large.size())),
              LOGX_OK);
    EXPECT_EQ(ii->drop_too_large.load(), too_large_before);
    EXPECT_EQ(ii->truncated_logs.load(), truncated_before + 1);

    logx_close(inst);
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

// E-03: 导出临时文件写失败 → LOGX_ERR_IO 且不调用回调;
// 业务回调返回非 0 → 状态如实传播为失败。
TEST(Robustness, ExportErrorPropagation_WriteFailAndCallbackFail) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_e03_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    // Case A: 临时文件写失败(装饰器对"非 append 模式打开的文件"即导出临时文件
    // 返回写失败;日志文件以 append 模式打开,不受影响)
    class MockExportWritePal : public Pal {
    public:
        explicit MockExportWritePal(Pal* real) : real_(real) {}
        int  file_open(const char* p, bool c, bool a) override {
            int fd = real_->file_open(p, c, a);
            if (fd >= 0 && !a) {
                // 导出临时文件: O_CREAT 但非 O_APPEND
                std::lock_guard<std::mutex> lock(mu_);
                no_append_fds_.insert(fd);
            }
            return fd;
        }
        int  file_close(int fd) override {
            std::lock_guard<std::mutex> lock(mu_);
            no_append_fds_.erase(fd);
            return real_->file_close(fd);
        }
        long file_write(int fd, const void* b, size_t n) override {
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (no_append_fds_.count(fd)) return -1;  // E-03: 导出临时文件写失败
            }
            return real_->file_write(fd, b, n);
        }
        // 其余接口全部直通真实 PAL
        int  file_truncate(int fd, uint64_t len) override { return real_->file_truncate(fd, len); }
        long file_pwrite(int fd, const void* b, size_t n, uint64_t o) override { return real_->file_pwrite(fd, b, n, o); }
        long file_pread(int fd, void* b, size_t n, uint64_t o) override { return real_->file_pread(fd, b, n, o); }
        int64_t file_size(int fd) override { return real_->file_size(fd); }
        int64_t file_size_path(const char* p) override { return real_->file_size_path(p); }
        int  file_fsync(int fd) override { return real_->file_fsync(fd); }
        int  file_dir_fsync(const char* p) override { return real_->file_dir_fsync(p); }
        int  file_unlink(const char* p) override { return real_->file_unlink(p); }
        int  file_mkdirs(const char* p) override { return real_->file_mkdirs(p); }
        int  file_rmdir(const char* p) override { return real_->file_rmdir(p); }
        std::vector<std::string> file_list(const char* p) override { return real_->file_list(p); }
        PalMmap mmap_open(int fd, size_t len, bool w) override { return real_->mmap_open(fd, len, w); }
        int  mmap_sync(const PalMmap& m, bool s) override { return real_->mmap_sync(m, s); }
        int  mmap_close(const PalMmap& m) override { return real_->mmap_close(m); }
        void* mutex_create() override { return real_->mutex_create(); }
        void  mutex_destroy(void* h) override { return real_->mutex_destroy(h); }
        void  mutex_lock(void* h) override { return real_->mutex_lock(h); }
        void  mutex_unlock(void* h) override { return real_->mutex_unlock(h); }
        int  threading_init() override { return real_->threading_init(); }
        int  thread_start(void* (*fn)(void*), void* arg, void** out_handle) override { return real_->thread_start(fn, arg, out_handle); }
        int  thread_join(void* handle) override { return real_->thread_join(handle); }
        void* ev_create() override { return real_->ev_create(); }
        void  ev_destroy(void* h) override { return real_->ev_destroy(h); }
        void  ev_signal(void* h) override { return real_->ev_signal(h); }
        void  ev_wait(void* h) override { return real_->ev_wait(h); }
        bool  ev_wait_timeout(void* h, uint32_t ms) override { return real_->ev_wait_timeout(h, ms); }
        uint64_t atomic_add(uint64_t* p, int64_t delta) override { return real_->atomic_add(p, delta); }
        uint64_t atomic_load(uint64_t* p) override { return real_->atomic_load(p); }
        uint64_t monotonic_ns() override { return real_->monotonic_ns(); }
        void  sleep_ms(uint32_t ms) override { return real_->sleep_ms(ms); }
        void  debug_log(const char* s) override { return real_->debug_log(s); }
        uint64_t wallclock_ms() override { return real_->wallclock_ms(); }
        void  wallclock_date(uint32_t* d, uint32_t* t) override { return real_->wallclock_date(d, t); }
        uint32_t pid() override { return real_->pid(); }
        uint64_t tid() override { return real_->tid(); }
        bool  random_bytes(void* b, size_t n) override { return real_->random_bytes(b, n); }
        Pal* real_;
        std::mutex mu_;
        std::set<int> no_append_fds_;
    };
    MockExportWritePal mock(&pal);
    {
        logx_config_t cfg{};
        cfg.log_dir = dir.c_str();
        cfg.staging_path = st_path.c_str();
        cfg.max_log_length = 4096;
        logx_instance_t* inst = nullptr;
        cfg.pal_override = &mock;  // (T-04) Flusher 启动前定型 PAL,根除注入竞态
        ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

        ASSERT_EQ(logx_write(inst, 2, "E03", (const uint8_t*)"export_err", 10), LOGX_OK);
        ASSERT_EQ(logx_flush(inst), LOGX_OK);

        uint32_t ymd = 0, hms = 0;
        pal.wallclock_date(&ymd, &hms);
        char date_str[16];
        std::snprintf(date_str, sizeof(date_str), "%08u", ymd);

        int cb_called = 0;
        auto cb = [](void* user, const char*, const char*, uint64_t) -> int {
            (*static_cast<int*>(user))++;
            return 0;
        };
        logx_status_e st = logx_export(inst, date_str, date_str, cb, &cb_called);
        EXPECT_EQ(st, LOGX_ERR_IO);   // E-03: 写失败不再吞成 OK
        EXPECT_EQ(cb_called, 0);      // 文件没写完整,不回调
        logx_close(inst);
    }

    // Case B: 业务回调返回非 0 → LOGX_ERR_CODEC
    {
        logx_config_t cfg{};
        cfg.log_dir = dir.c_str();
        cfg.staging_path = st_path.c_str();
        cfg.max_log_length = 4096;
        logx_instance_t* inst = nullptr;
        ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

        ASSERT_EQ(logx_write(inst, 2, "E03", (const uint8_t*)"cb_fail", 7), LOGX_OK);
        ASSERT_EQ(logx_flush(inst), LOGX_OK);

        uint32_t ymd = 0, hms = 0;
        pal.wallclock_date(&ymd, &hms);
        char date_str[16];
        std::snprintf(date_str, sizeof(date_str), "%08u", ymd);

        auto cb_fail = [](void*, const char*, const char*, uint64_t) -> int { return 7; };
        EXPECT_EQ(logx_export(inst, date_str, date_str, cb_fail, nullptr),
                  LOGX_ERR_CODEC);      // E-03: 回调失败如实上报
        logx_close(inst);
    }

    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

// E-04: 同一天多次导出,临时文件名各自独立,不互相覆盖截断;
// 导出完成后临时文件被清理。
TEST(Robustness, ExportTmpPath_UniquePerCallAndCleaned) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_e04_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;
    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    ASSERT_EQ(logx_write(inst, 2, "E04", (const uint8_t*)"tmp_unique", 10), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    uint32_t ymd = 0, hms = 0;
    pal.wallclock_date(&ymd, &hms);
    char date_str[16];
    std::snprintf(date_str, sizeof(date_str), "%08u", ymd);

    std::mutex paths_mu;
    std::vector<std::string> seen_paths;
    struct Ctx {
        std::mutex* mu;
        std::vector<std::string>* paths;
    } ctx{&paths_mu, &seen_paths};
    auto cb = [](void* user, const char*, const char* plaintext_path, uint64_t) -> int {
        auto* c = static_cast<Ctx*>(user);
        std::lock_guard<std::mutex> lock(*c->mu);
        c->paths->push_back(plaintext_path);
        return 0;
    };
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(logx_export(inst, date_str, date_str, cb, &ctx), LOGX_OK);
    }

    // 5 次导出回调各拿到独立临时路径(E-04: 不互踩)
    std::lock_guard<std::mutex> lock(paths_mu);
    ASSERT_EQ(seen_paths.size(), 5u);
    for (size_t i = 0; i < seen_paths.size(); ++i) {
        for (size_t j = i + 1; j < seen_paths.size(); ++j) {
            EXPECT_NE(seen_paths[i], seen_paths[j]);
        }
        // 命名仍以 .tmp_export_ 前缀并含 PID 与日期
        EXPECT_NE(seen_paths[i].find(".tmp_export_"), std::string::npos);
    }

    logx_close(inst);
    // 导出完成后临时文件全部清理(回调后被 unlink)
    for (const auto& p : seen_paths) {
        EXPECT_LT(pal.file_size_path(p.c_str()), 0);
    }
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

// E-06: flush_threshold 显式语义——0 = 禁用阈值自动唤醒(写入不主动 signal,
// 仅手动 flush 落盘);UINT32_MAX = 显式取默认 8KB。
// 复审加固:旧断言 flushed_bytes_total == 0 依赖与 Flusher 75ms debounce
// 抢跑,属偶发假绿。现以 CountingEvSignalPal 直接计数 ev_signal 调用:
// 阈值 0 时写入路径零 signal(语义反转回归),阈值 8KB 时首条写入即 signal。
TEST(Robustness, FlushThreshold_ZeroDisablesAutoWake_MaxMeansDefault) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_e06_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    // 装饰 PAL:统计 ev_signal 次数,其余全部直通
    class CountingEvSignalPal : public Pal {
    public:
        explicit CountingEvSignalPal(Pal* real) : real_(real) {}
        void ev_signal(void* h) override {
            ev_signal_count_.fetch_add(1);
            return real_->ev_signal(h);
        }
        int  file_open(const char* p, bool c, bool a) override { return real_->file_open(p, c, a); }
        int  file_close(int fd) override { return real_->file_close(fd); }
        int  file_truncate(int fd, uint64_t len) override { return real_->file_truncate(fd, len); }
        long file_write(int fd, const void* b, size_t n) override { return real_->file_write(fd, b, n); }
        long file_pwrite(int fd, const void* b, size_t n, uint64_t o) override { return real_->file_pwrite(fd, b, n, o); }
        long file_pread(int fd, void* b, size_t n, uint64_t o) override { return real_->file_pread(fd, b, n, o); }
        int64_t file_size(int fd) override { return real_->file_size(fd); }
        int64_t file_size_path(const char* p) override { return real_->file_size_path(p); }
        int  file_fsync(int fd) override { return real_->file_fsync(fd); }
        int  file_dir_fsync(const char* p) override { return real_->file_dir_fsync(p); }
        int  file_unlink(const char* p) override { return real_->file_unlink(p); }
        int  file_mkdirs(const char* p) override { return real_->file_mkdirs(p); }
        int  file_rmdir(const char* p) override { return real_->file_rmdir(p); }
        std::vector<std::string> file_list(const char* p) override { return real_->file_list(p); }
        PalMmap mmap_open(int fd, size_t len, bool w) override { return real_->mmap_open(fd, len, w); }
        int  mmap_sync(const PalMmap& m, bool s) override { return real_->mmap_sync(m, s); }
        int  mmap_close(const PalMmap& m) override { return real_->mmap_close(m); }
        void* mutex_create() override { return real_->mutex_create(); }
        void  mutex_destroy(void* h) override { return real_->mutex_destroy(h); }
        void  mutex_lock(void* h) override { return real_->mutex_lock(h); }
        void  mutex_unlock(void* h) override { return real_->mutex_unlock(h); }
        int  threading_init() override { return real_->threading_init(); }
        int  thread_start(void* (*fn)(void*), void* arg, void** out_handle) override { return real_->thread_start(fn, arg, out_handle); }
        int  thread_join(void* handle) override { return real_->thread_join(handle); }
        void* ev_create() override { return real_->ev_create(); }
        void  ev_destroy(void* h) override { return real_->ev_destroy(h); }
        void  ev_wait(void* h) override { return real_->ev_wait(h); }
        bool  ev_wait_timeout(void* h, uint32_t ms) override { return real_->ev_wait_timeout(h, ms); }
        uint64_t atomic_add(uint64_t* p, int64_t delta) override { return real_->atomic_add(p, delta); }
        uint64_t atomic_load(uint64_t* p) override { return real_->atomic_load(p); }
        uint64_t monotonic_ns() override { return real_->monotonic_ns(); }
        void  sleep_ms(uint32_t ms) override { return real_->sleep_ms(ms); }
        void  debug_log(const char* s) override { return real_->debug_log(s); }
        uint64_t wallclock_ms() override { return real_->wallclock_ms(); }
        void  wallclock_date(uint32_t* d, uint32_t* t) override { return real_->wallclock_date(d, t); }
        uint32_t pid() override { return real_->pid(); }
        uint64_t tid() override { return real_->tid(); }
        bool  random_bytes(void* b, size_t n) override { return real_->random_bytes(b, n); }
        Pal* real_;
        std::atomic<int> ev_signal_count_{0};
    };
    CountingEvSignalPal mock(&pal);

    // Case A: 0 = 禁用自动唤醒
    {
        logx_config_t cfg{};
        cfg.log_dir = dir.c_str();
        cfg.staging_path = st_path.c_str();
        cfg.max_log_length = 4096;
        cfg.flush_threshold = 0;   // E-06: 显式禁用,不再被静默替换为 8KB
        logx_instance_t* inst = nullptr;
        cfg.pal_override = &mock;  // (T-04) Flusher 启动前定型 PAL,根除注入竞态
        ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
        auto* ii = static_cast<Instance*>(inst);
        (void)ii;
        EXPECT_EQ(ii->flush_threshold, 0u);

        // 确定性断言:阈值 0 下写入路径不触发任何 ev_signal(语义反转回归:
        // 旧实现 pending >= 0 恒真,每条写入均唤醒 Flusher)
        int before = mock.ev_signal_count_.load();
        const uint8_t payload[] = "held";
        for (int i = 0; i < 10; ++i) {
            ASSERT_EQ(logx_write(inst, 2, "E06", payload, sizeof(payload) - 1), LOGX_OK);
        }
        EXPECT_EQ(mock.ev_signal_count_.load(), before);   // 零 signal
        EXPECT_GT(ii->staged_bytes_total.load(), 0u);      // 数据已入 staging

        // 手动 flush:flush_blocking 发出信号并轮询等待水位推进(设计内每 5ms
        // 重复 signal 直至落盘,故断言 ≥ +1 而非恰 +1),数据必须落盘
        ASSERT_EQ(logx_flush(inst), LOGX_OK);
        EXPECT_GE(mock.ev_signal_count_.load(), before + 1);
        EXPECT_GT(ii->flushed_bytes_total.load(), 0u);

        logx_close(inst);
    }

    mock.ev_signal_count_.store(0);

    // Case B: UINT32_MAX = 显式默认 8KB
    {
        logx_config_t cfg{};
        cfg.log_dir = dir.c_str();
        cfg.staging_path = st_path.c_str();
        cfg.max_log_length = 4096;
        cfg.flush_threshold = UINT32_MAX;   // E-06: 显式默认哨兵
        logx_instance_t* inst = nullptr;
        cfg.pal_override = &mock;  // (T-04) Flusher 启动前定型 PAL,根除注入竞态
        ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
        auto* ii = static_cast<Instance*>(inst);
        (void)ii;
        EXPECT_EQ(ii->flush_threshold, 8u * 1024);

        // 默认阈值 8KB:小写入 pending < 8KB 不应唤醒 → 零 signal
        int before = mock.ev_signal_count_.load();
        const uint8_t payload[] = "small";
        ASSERT_EQ(logx_write(inst, 2, "E06", payload, sizeof(payload) - 1), LOGX_OK);
        EXPECT_EQ(mock.ev_signal_count_.load(), before);   // 小写入未达阈值,零 signal

        logx_close(inst);
    }

    // Case C: 阈值极小(1B)时,单条写入立即越过阈值 → 写入路径 signal 恰好 +1
    {
        logx_config_t cfg{};
        cfg.log_dir = dir.c_str();
        cfg.staging_path = st_path.c_str();
        cfg.max_log_length = 4096;
        cfg.flush_threshold = 1;   // 任意写入都会 pending >= 1
        logx_instance_t* inst = nullptr;
        cfg.pal_override = &mock;  // (T-04) Flusher 启动前定型 PAL,根除注入竞态
        ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
        auto* ii = static_cast<Instance*>(inst);
        (void)ii;

        int before = mock.ev_signal_count_.load();
        ASSERT_EQ(logx_write(inst, 2, "E06", (const uint8_t*)"x", 1), LOGX_OK);
        EXPECT_EQ(mock.ev_signal_count_.load(), before + 1);  // 自动唤醒生效

        logx_close(inst);
    }

    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

// E-07: logx_stats_t 预留区零值契约(未来字段向前兼容)
TEST(Robustness, StatsReservedZeroContract) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_e07_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;
    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    ASSERT_EQ(logx_write(inst, 2, "E07", (const uint8_t*)"stats", 5), LOGX_OK);

    logx_stats_t st;
    std::memset(&st, 0xAB, sizeof(st));  // 预填脏值,验证 get_stats 全量覆写
    logx_get_stats(inst, &st);
    for (size_t i = 0; i < sizeof(st.reserved) / sizeof(st.reserved[0]); ++i) {
        EXPECT_EQ(st.reserved[i], 0u);
    }

    logx_close(inst);
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

// =========================================================================
// 9. P 系列: PAL 实现质量回归 (P-02 size_path / P-03 rmdir errno / P-04 时钟 / P-06 原子对齐)
// =========================================================================

// P-03: rmdir 错误码必须区分"目录非空"与"目录不存在",不得一律 -1 丢语义。
// Windows 侧经 win32_error_to_errno 转译,POSIX 侧 rmdir 原生 errno;两侧语义对齐。
TEST(Robustness, PalRmdirErrnoDistinction_NotEmptyVsMissing) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_p03_rmdir_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(pal.monotonic_ns() % 1000000);
    ASSERT_EQ(pal.file_mkdirs(dir.c_str()), 0);

    errno = 0;
    EXPECT_EQ(pal.file_rmdir(dir.c_str()), 0);  // 空目录删除成功

    errno = 0;
    EXPECT_EQ(pal.file_rmdir(dir.c_str()), -1);  // 再删:目录已不存在
    EXPECT_EQ(errno, ENOENT);

    // 非空目录:放入一个文件后 rmdir 必须失败且 errno 为 ENOTEMPTY(POSIX)
    // 或等效错误(Windows 转译 ERROR_DIR_NOT_EMPTY → ENOTEMPTY)
    std::string dir2 = dir + "_ne";
    ASSERT_EQ(pal.file_mkdirs(dir2.c_str()), 0);
    std::string fpath = dir2 + "/occupied.bin";
    int fd = pal.file_open(fpath.c_str(), true, false);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(pal.file_write(fd, "x", 1), 1);
    ASSERT_EQ(pal.file_close(fd), 0);

    errno = 0;
    EXPECT_EQ(pal.file_rmdir(dir2.c_str()), -1);
    EXPECT_EQ(errno, ENOTEMPTY);

    // 目录内文件 unlink 后 errno 语义:再 unlink 同路径 → ENOENT
    errno = 0;
    EXPECT_EQ(pal.file_unlink(fpath.c_str()), 0);
    errno = 0;
    EXPECT_EQ(pal.file_unlink(fpath.c_str()), -1);
    EXPECT_EQ(errno, ENOENT);

    EXPECT_EQ(pal.file_rmdir(dir2.c_str()), 0);
}

// P-02: file_size_path 语义回归——普通文件精确大小、与 fd 版 file_size 一致、
// 不存在路径返回 -1、目录返回非负(GetFileAttributesExA 对目录返回属性数据,
// 大小字段为目录元数据大小;消费方只对 >= 0 做"路径存在"判断,不读目录大小)。
TEST(Robustness, PalFileSizePath_AttributesBased_SemanticsPreserved) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_p02_size_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(pal.monotonic_ns() % 1000000);
    ASSERT_EQ(pal.file_mkdirs(dir.c_str()), 0);
    std::string fpath = dir + "/sized.bin";

    // 精确大小:写入 3 块共 3000 字节(跨 1KB 页边界,避免整页巧合)
    int fd = pal.file_open(fpath.c_str(), true, false);
    ASSERT_GE(fd, 0);
    uint8_t buf[1000];
    std::memset(buf, 0xA5, sizeof(buf));
    for (int i = 0; i < 3; ++i) ASSERT_EQ(pal.file_write(fd, buf, sizeof(buf)), 1000);
    ASSERT_EQ(pal.file_close(fd), 0);

    int64_t by_path = pal.file_size_path(fpath.c_str());
    EXPECT_EQ(by_path, 3000);

    // 与 fd 版一致性(fd open 时不加 append,避免 O_APPEND 改变语义)
    int fd2 = pal.file_open(fpath.c_str(), false, false);
    ASSERT_GE(fd2, 0);
    EXPECT_EQ(pal.file_size(fd2), by_path);
    pal.file_close(fd2);

    // 不存在路径必须返回 -1(错误态契约不变)
    EXPECT_LT(pal.file_size_path((dir + "/no_such_file.bin").c_str()), 0);

    // 目录路径:存在即可查询成功(非负),大小值不参与任何决策
    EXPECT_GE(pal.file_size_path(dir.c_str()), 0);

    pal.file_unlink(fpath.c_str());
    pal.file_rmdir(dir.c_str());
}

// P-06 并发参数:命名空间级常量,lambda 内直接可见无需捕获
// (MSVC 要求显式捕获而 clang 会报 unused-capture,放作用域常量两端皆净)
constexpr int kP06AtomicThreads = 4;
constexpr int kP06AtomicIters = 10000;

// P-04 + 基本时钟契约:单调性(不回退)、量纲合理(ns 级、1ms 睡眠 ≥ 1ms)、
// 连续两次查询间隔为小正值(防回绕/防跳变回归锚点)。
TEST(Robustness, PalMonotonic_NonDecreasingSaneUnits) {
    Pal& pal = default_pal();
    uint64_t t0 = pal.monotonic_ns();
    uint64_t t1 = pal.monotonic_ns();
    EXPECT_GE(t1, t0);

    pal.sleep_ms(15);
    uint64_t t2 = pal.monotonic_ns();
    // 睡 15ms 后时钟推进量必须 ≥ 15ms(量纲错误如把 ms 当 ns 会在此暴露),
    // 且 ≤ 5s(排除病态跳变)。
    uint64_t delta = t2 - t0;
    EXPECT_GE(delta, 15ULL * 1000000ULL);
    EXPECT_LE(delta, 5000ULL * 1000000ULL);
}

// P-06: 8 字节对齐指针上的 atomic_add/load 语义正确性(对齐断言在 Debug 构建
// 生效;本测试同时验证对齐路径的行为契约——累加、负增量、并发无丢失)。
TEST(Robustness, PalAtomicAdd_AlignedBehaviorContract) {
    Pal& pal = default_pal();
    alignas(8) uint64_t v = 100;
    EXPECT_EQ(pal.atomic_add(&v, 5), 100u);   // 返回旧值
    EXPECT_EQ(v, 105u);
    EXPECT_EQ(pal.atomic_add(&v, -30), 105u); // 负增量
    EXPECT_EQ(v, 75u);
    EXPECT_EQ(pal.atomic_load(&v), 75u);

    alignas(8) uint64_t shared = 0;
    std::vector<std::thread> ths;
    for (int i = 0; i < kP06AtomicThreads; ++i) {
        ths.emplace_back([&pal, &shared] {
            for (int j = 0; j < kP06AtomicIters; ++j) pal.atomic_add(&shared, 1);
        });
    }
    for (auto& t : ths) t.join();
    EXPECT_EQ(pal.atomic_load(&shared),
              static_cast<uint64_t>(kP06AtomicThreads) * kP06AtomicIters);  // 并发累加零丢失
}

// =========================================================================
// 10. J 系列回归:JNI 层支持性契约 (J-02 逐实例错误上下文)
// =========================================================================

// J-02: err_fn/err_user 必须逐实例绑定。JNI 侧已把错误上下文从全局静态
// g_err_ctx 下放到 inst->err_user;本测试在 C ABI 层验证核心契约:
//   a) logx_open 拷贝 cfg.err_user 且 emit_event 把它原样回传回调;
//   b) 两个实例各自注册不同的 user,事件互不串扰 —— 单实例 close 后
//      其它实例的回调路径不受任何影响(全局态若仍存在则必然失败)。
TEST(Robustness, ErrUserPerInstance_IsolationAcrossClose) {
    Pal& pal = default_pal();

    struct Probe { std::atomic<int> hits{0}; int id{0}; };
    Probe pa; pa.id = 1;
    Probe pb; pb.id = 2;

    auto make_cfg = [&](const char* dir, const char* staging) {
        logx_config_t cfg{};
        cfg.log_dir = dir;
        cfg.staging_path = staging;
        cfg.max_log_length = 4096;
        return cfg;
    };

    std::string dir_a = "logx_test_j02_a_" + std::to_string(pal.pid());
    std::string dir_b = "logx_test_j02_b_" + std::to_string(pal.pid());
    pal.file_mkdirs(dir_a.c_str());
    pal.file_mkdirs(dir_b.c_str());
    // staging 路径必须与 cfg 同生命周期(悬垂临时 string 会让 logx_open 读野指针)
    std::string staging_a = dir_a + "/staging.mmap";
    std::string staging_b = dir_b + "/staging.mmap";

    logx_config_t cfg_a = make_cfg(dir_a.c_str(), staging_a.c_str());
    logx_config_t cfg_b = make_cfg(dir_b.c_str(), staging_b.c_str());

    // 与 JNI trampoline 同形:user 承载全部上下文,实例间无共享
    static constexpr logx_error_fn probe_fn = [](void* user, logx_event_kind_e, int, const char*) {
        static_cast<Probe*>(user)->hits.fetch_add(1);
    };
    cfg_a.err_fn = probe_fn; cfg_a.err_user = &pa;
    cfg_b.err_fn = probe_fn; cfg_b.err_user = &pb;

    logx_instance_t* ia = nullptr;
    logx_instance_t* ib = nullptr;
    ASSERT_EQ(logx_open(&cfg_a, &ia), LOGX_OK);
    ASSERT_EQ(logx_open(&cfg_b, &ib), LOGX_OK);

    // 两实例各产生一条 DROP 事件(非法 level),事件必须路由到各自的 probe
    ASSERT_EQ(logx_write(ia, -1, "J02A", nullptr, 0), LOGX_ERR_INVALID_ARG);
    ASSERT_EQ(logx_write(ib, 7, "J02B", nullptr, 0), LOGX_ERR_INVALID_ARG);
    EXPECT_EQ(pa.hits.load(), 1);   // a 的事件只进 a 的 user
    EXPECT_EQ(pb.hits.load(), 1);   // b 的事件只进 b 的 user

    // 关闭 a:J-02 场景核心 —— 若错误上下文仍存全局,这里清空后 b 将静默失效
    ASSERT_EQ(logx_close(ia), LOGX_OK);
    ASSERT_EQ(logx_write(ib, -1, "J02B2", nullptr, 0), LOGX_ERR_INVALID_ARG);
    EXPECT_EQ(pb.hits.load(), 2);   // b 的回调路径不受 a 关闭影响
    EXPECT_EQ(pa.hits.load(), 1);   // a 的 probe 不再增长(实例已销毁)

    ASSERT_EQ(logx_close(ib), LOGX_OK);

    // 运行中逐实例重注册也路由正确(E-01 锁保护路径)
    // (cleanup)
    for (const auto& f : pal.file_list(dir_a.c_str())) pal.file_unlink(f.c_str());
    pal.file_rmdir(dir_a.c_str());
    for (const auto& f : pal.file_list(dir_b.c_str())) pal.file_unlink(f.c_str());
    pal.file_rmdir(dir_b.c_str());
}

// R18 §六门禁项 2: 时钟推后 20 年的 retention 双向纪律回归。
//
// R17 曾发现单测时序炸弹(历史日期文件被 retention 清理误删), 当时的门禁建议是
// "逐条 grep yyyymmdd 字面量核对 cfg"——但字面量 grep 只能防住"已知形状", 防不住
// 变体(换个日期、换个注入方式就漏)。本用例把防线从"grep 防守"转为"用例防守":
//
// 场景: 用 MockClockPal 把时钟从 2026-09-03 直接推后 20 年到 2046-09-03,
// 触发 retention_cleanup_impl。此时构造四类文件, 按"相对推后后的 now"各应
// 受到不同处置, 本用例逐类锁定:
//
//   A 远古 20200101(now-26 年) —— 严格早于 now-retention → **必删**
//   B 期内 20460830(now-4 天)  —— 未超期                 → **必留**
//   C 当天 20460903(d == now)   —— 严格保护(flusher.cpp:520 的 d < now 守卫) → **必留**
//   D 未来 20460910(d >  now)   —— 严格保护(时钟漂移)   → **必留**
//
// ②(C/D 保护面)正是 R17 事故的防御面: 若清理只做"老就删"而不保护当天与未来,
// 一次时钟回拨/漂移就会把用户当天刚写的日志整批误删——R17 事故的形态正是
// "该留的被删了"。①(A 必删)则防止"保护过头演化成永不清理"的反向退化,
// 二者同时断言以杜绝空洞绿。
TEST(Robustness, RetentionCleanup_ClockJumpForward20Years_DeletesExpiredProtectsTodayAndFuture) {
    MockClockPal mock_pal(&default_pal());
    std::string dir = std::string("logx_test_r18_retention_") +
                      std::to_string(mock_pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
    mock_pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    // 固定起始日期后推后 20 年, 避免用例随真实日历漂移
    mock_pal.set_clock(20260903, 120000);
    const uint32_t kFuture = 20460903;

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;
    // B-12 (方案一): open 时 retention_days = 36500 (100 年, R18-1 判据),
    // 确保创建循环与时钟推后 20 年期间, 后台 Flusher 跨天 drain 绝不提前回收远古文件 A
    cfg.retention_days = 36500;

    logx_instance_t* inst = nullptr;
    cfg.pal_override = &mock_pal;  // 必须在 logx_open 前定型(消除注入竞态)
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    // B-12 (方案三): RAII 自动关闭守卫, 杜绝用例任何断言失败跳出导致活跃实例与 Flusher 线程泄漏
    struct InstGuard {
        logx_instance_t*& target;
        ~InstGuard() {
            if (target) {
                logx_close(target);
                target = nullptr;
            }
        }
    } inst_guard{inst};

    // 推后时钟到 2046-09-03, 再按"相对新 now"构造四类文件
    mock_pal.set_clock(kFuture, 120000);

    struct Case { const char* name; const char* fname; bool should_survive; };
    const Case cases[] = {
        {"A ancient (expired)",  "20200101.log", false},
        {"B within retention",  "20460830.log", true},
        {"C today (protected)", "20460903.log", true},
        {"D future (protected)", "20460910.log", true},
    };
    for (const auto& cs : cases) {
        std::string p = dir + "/" + cs.fname;
        int fd = mock_pal.file_open(p.c_str(), true, false);
        ASSERT_GE(fd, 0) << cs.name;
        ASSERT_EQ(mock_pal.file_write(fd, "payload", 7), 7) << cs.name;
        mock_pal.file_close(fd);
        int64_t sz0 = mock_pal.file_size_path(p.c_str());
#if defined(_WIN32)
        // B-12: file_size_path 失败路径不消费 LastError,可采获 GetFileAttributesExA 原始
        // 错误码区分「真消失」(2=FILE_NOT_FOUND/3=PATH_NOT_FOUND)与「共享冲突」
        // (32=SHARING_VIOLATION,AV/索引器瞬时独占);POSIX 分支无此歧义,零改动
        DWORD b12_le = (sz0 < 0) ? GetLastError() : 0;
        ASSERT_GT(sz0, 0) << cs.name << " [B12] raw_lasterror=" << b12_le;
#else
        ASSERT_GT(sz0, 0) << cs.name;
#endif
    }

    // B-12 (方案一): 四类文件建档与断言完毕后, 将保留期恢复为 7 天 (与 flusher.cpp:635
    // retention_cleanup_impl 调用点现读一致), 再显式触发 retention 清理, 测试意图完整保留且
    // 无文件删除时序竞态。R38 F-1 指出的形式数据竞争已由 B-13 方案 1 atomic 化收口
    // (logx_internal.h retention_days 为 std::atomic<uint32_t>, 本写即为 atomic store)。
    static_cast<Instance*>(inst)->retention_days = 7;

    // 触发 retention 清理(与 Flusher 启动/跨天同一条路径)
    retention_cleanup(static_cast<Instance*>(inst));

    for (const auto& cs : cases) {
        int64_t sz = mock_pal.file_size_path((dir + "/" + cs.fname).c_str());
        if (cs.should_survive) {
#if defined(_WIN32)
            DWORD b12_le = (sz < 0) ? GetLastError() : 0;
            EXPECT_GT(sz, 0) << cs.name << " must NOT be deleted by retention"
                             << " [B12] raw_lasterror=" << b12_le;
#else
            EXPECT_GT(sz, 0) << cs.name << " must NOT be deleted by retention";
#endif
        } else {
            EXPECT_LT(sz, 0) << cs.name << " must be deleted after 20-year clock jump";
        }
    }

    logx_close(inst);
    inst = nullptr;

    for (const auto& f : mock_pal.file_list(dir.c_str())) {
        mock_pal.file_unlink(f.c_str());
    }
    mock_pal.file_rmdir(dir.c_str());
}

} // namespace
