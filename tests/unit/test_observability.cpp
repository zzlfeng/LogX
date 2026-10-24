// test_observability.cpp - 资源占用与延迟抖动的可回归度量。
//
// 立项依据:
//   1) docs/benchmark_report.md §8.4「已知局限」自认未覆盖 CPU 占用率与 JNI 路径
//      GC 监控;docs/LogX日志库最终实施方案.md L349 把「50 线程压测无 fd/内存
//      泄漏」列为验收条款,但既有自动化只有数据完整性断言,无任何 fd/内存计数;
//   2) 延迟指标只有 P50/P90/P99/P99.9/max 分位数,缺方差/标准差/CV 等离散度
//      (jitter)维度 —— 分位数反映长尾量级,不等于抖动。
//
// 本文件补三类可回归契约(与 tests/bench/logx_bench.cpp 的度量维度互补):
//   - Observability.MemoryRssPss_*   稳态多轮写入后 RSS/PSS 增量有界(无逐轮泄漏)
//   - Observability.LatencyJitter_*  热路径写入延迟 stddev/CV/MAD 在宽松界内
//   - LeakRegression.CountingPal_*   Pal 层 create/destroy 严格配对(净 0),
//                                    并以 OS 级 fd 计数做独立交叉验证
//
// 平台策略:内存指标依赖 OS 接口(Linux/Android 走 /proc,Windows 走 psapi);
// 缺失平台该项降级为"仅报告不判定",不阻塞其它断言。
//
// 阈值策略:全部为**宽松合理性界**(非性能门禁)。目的是捕捉病态回归 ——
// 逐轮资源泄漏、锁 convoy、偶发堆分配 —— 而非卡住正常环境噪声。

#include "logx_c.h"
#include "test_accessor.h"
#include "logx_internal.h"   // 传递包含 logx_pal.h / logx_staging.h / logx_format.h
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
// NOMINMAX: 阻止 windows.h 定义 min/max 宏,避免与 std::max/std::min 冲突
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>      // GetProcessMemoryInfo(WorkingSetSize = RSS 等价量)
#elif defined(__linux__) || defined(__ANDROID__)
#include <dirent.h>
#include <unistd.h>
#endif

using namespace logx;

namespace {

// (同 test_robustness.cpp 惯例) 原子自增目录序号,消除并行/重跑目录撞名
std::atomic<uint64_t> s_obs_seq{0};

std::string make_dir(Pal& pal, const char* tag) {
    std::string dir = std::string("logx_test_") + tag + "_" +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_obs_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());
    return dir;
}

// 清理目录内全部文件后删目录(必须在 logx_close 之后调用,否则映射/句柄未释放)
void cleanup_dir(Pal& pal, const std::string& dir) {
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

using Clock = std::chrono::steady_clock;

uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count());
}

// =========================================================================
// 1. OS 级内存指标采集: RSS(常驻集) / PSS(比例分摊集)
// =========================================================================

struct MemSample {
    uint64_t rss = 0;      // Resident Set Size,字节
    uint64_t pss = 0;      // Proportional Set Size,字节;非 Linux 平台恒为 0
    bool     rss_ok = false;
    bool     pss_ok = false;
};

#if defined(_WIN32)

bool sample_rss(uint64_t* out) {
    PROCESS_MEMORY_COUNTERS pmc{};
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc))) return false;
    // Windows 无 PSS 概念,以 WorkingSetSize 作为 RSS 等价量
    *out = static_cast<uint64_t>(pmc.WorkingSetSize);
    return true;
}

bool sample_pss(uint64_t* /*out*/) { return false; }

#elif defined(__linux__) || defined(__ANDROID__)

bool sample_rss(uint64_t* out) {
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return false;
    unsigned long total_pages = 0, resident_pages = 0;
    const int n = std::fscanf(f, "%lu %lu", &total_pages, &resident_pages);
    std::fclose(f);
    if (n != 2) return false;
    long ps = ::sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    *out = static_cast<uint64_t>(resident_pages) * static_cast<uint64_t>(ps);
    return true;
}

// 累加给定 /proc 自描述文件中的 "Pss:" 行(kB);未找到任何 Pss 行视为失败
bool scan_pss_file(const char* path, uint64_t* out) {
    std::FILE* f = std::fopen(path, "r");
    if (!f) return false;
    char line[256];
    uint64_t kb = 0;
    bool found = false;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "Pss:", 4) == 0) {
            kb += std::strtoull(line + 4, nullptr, 10);
            found = true;
        }
    }
    std::fclose(f);
    if (!found) return false;
    *out = kb * 1024ull;
    return true;
}

bool sample_pss(uint64_t* out) {
    // Linux 4.14+ / Android 10+ 提供 smaps_rollup(单次读,开销低);
    // 缺失时退回逐映射累加 smaps(慢但兼容老内核)。
    if (scan_pss_file("/proc/self/smaps_rollup", out)) return true;
    return scan_pss_file("/proc/self/smaps", out);
}

#else

bool sample_rss(uint64_t* /*out*/) { return false; }
bool sample_pss(uint64_t* /*out*/) { return false; }

#endif

MemSample sample_mem() {
    MemSample s;
    s.rss_ok = sample_rss(&s.rss);
    s.pss_ok = sample_pss(&s.pss);
    return s;
}

// OS 级打开 fd 计数(独立于 Pal 计数的交叉验证)。不可用平台返回 -1。
// 注意: opendir 自身占用一个 fd,但前后两次调用口径一致,故**增量**仍有效。
int count_open_fds() {
#if defined(__linux__) || defined(__ANDROID__)
    DIR* d = ::opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0;
    while (struct dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;   // 跳过 . 与 ..
        ++n;
    }
    ::closedir(d);
    return n;
#else
    return -1;
#endif
}

// =========================================================================
// 2. 延迟离散度统计(stddev / CV / MAD / robustCV + 分位数)
// =========================================================================

struct LatencyStats {
    size_t n = 0;
    double mean_ns = 0.0;
    double stddev_ns = 0.0;    // 总体标准差(全样本)
    double cv = 0.0;           // 经典变异系数 = stddev / mean
    double p50 = 0.0, p90 = 0.0, p99 = 0.0, p999 = 0.0, max = 0.0;
    double median = 0.0;       // == p50
    double mad = 0.0;          // 中位绝对偏差:P50(|x - median|)
    double robust_cv = 0.0;    // MAD / median —— 对个别长尾离群点稳健
};

// 最近秩(nearest-rank)百分位: v 必须已升序
double percentile_sorted(const std::vector<uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    size_t idx = static_cast<size_t>(
        std::ceil(p / 100.0 * static_cast<double>(sorted.size())));
    if (idx == 0) idx = 1;
    if (idx > sorted.size()) idx = sorted.size();
    return static_cast<double>(sorted[idx - 1]);
}

LatencyStats compute_latency_stats(std::vector<uint64_t>& v) {
    LatencyStats s;
    s.n = v.size();
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());

    double sum = 0.0;
    for (uint64_t x : v) sum += static_cast<double>(x);
    s.mean_ns = sum / static_cast<double>(v.size());

    double var = 0.0;
    for (uint64_t x : v) {
        const double d = static_cast<double>(x) - s.mean_ns;
        var += d * d;
    }
    var /= static_cast<double>(v.size());   // 总体方差(描述本批样本,非推断总体)
    s.stddev_ns = std::sqrt(var);
    s.cv = s.mean_ns > 0.0 ? s.stddev_ns / s.mean_ns : 0.0;

    s.p50  = percentile_sorted(v, 50);
    s.p90  = percentile_sorted(v, 90);
    s.p99  = percentile_sorted(v, 99);
    s.p999 = percentile_sorted(v, 99.9);
    s.max  = static_cast<double>(v.back());
    s.median = s.p50;

    // MAD: 对排序后的样本取 |x - median| 的中位数
    std::vector<uint64_t> dev;
    dev.reserve(v.size());
    const long long med = static_cast<long long>(s.median);
    for (uint64_t x : v) {
        dev.push_back(static_cast<uint64_t>(
            std::llabs(static_cast<long long>(x) - med)));
    }
    std::sort(dev.begin(), dev.end());
    s.mad = percentile_sorted(dev, 50);
    s.robust_cv = s.median > 0.0 ? s.mad / s.median : 0.0;
    return s;
}

void print_latency_stats(const char* label, const LatencyStats& s) {
    printf("[obs] %s (n=%zu)\n", label, s.n);
    printf("      mean   = %9.1f ns   stddev = %9.1f ns   CV = %.3f\n",
           s.mean_ns, s.stddev_ns, s.cv);
    printf("      median = %9.1f ns   MAD    = %9.1f ns   robustCV = %.3f\n",
           s.median, s.mad, s.robust_cv);
    printf("      P50=%.0f P90=%.0f P99=%.0f P99.9=%.0f max=%.0f (ns)\n",
           s.p50, s.p90, s.p99, s.p999, s.max);
    printf("      长尾比: P99/P50 = %.2fx   max/P50 = %.2fx\n",
           s.p50 > 0 ? s.p99 / s.p50 : 0.0,
           s.p50 > 0 ? s.max / s.p50 : 0.0);
}

// =========================================================================
// 3. CountingPal: Pal 层资源 create/destroy 计数包装
// =========================================================================
//
// 用途:把实施方案 L349「无 fd/内存泄漏」验收条款变成可回归断言。
// 纪律:仅在真实成功时计数(create 失败/close 失败不计),保证等式
//       create == destroy 严格等价于"net 0 泄漏"。
class CountingPal : public Pal {
public:
    explicit CountingPal(Pal* real) : real_(real) {}

    struct Counters {
        std::atomic<int64_t> fd_create{0},     fd_destroy{0};
        std::atomic<int64_t> mmap_create{0},   mmap_destroy{0};
        std::atomic<int64_t> mutex_create{0},  mutex_destroy{0};
        std::atomic<int64_t> ev_create{0},     ev_destroy{0};
        std::atomic<int64_t> thread_create{0}, thread_destroy{0};
    };
    Counters c;

    int file_open(const char* p, bool cr, bool ap) override {
        const int fd = real_->file_open(p, cr, ap);
        if (fd >= 0) c.fd_create.fetch_add(1, std::memory_order_relaxed);
        return fd;
    }
    int file_close(int fd) override {
        const int rc = real_->file_close(fd);
        if (rc == 0) c.fd_destroy.fetch_add(1, std::memory_order_relaxed);
        return rc;
    }

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

    PalMmap mmap_open(int fd, size_t l, bool w) override {
        const PalMmap m = real_->mmap_open(fd, l, w);
        if (m.base != nullptr) c.mmap_create.fetch_add(1, std::memory_order_relaxed);
        return m;
    }
    int mmap_sync(const PalMmap& m, bool a) override { return real_->mmap_sync(m, a); }
    int mmap_close(const PalMmap& m) override {
        if (m.base == nullptr) return real_->mmap_close(m);   // 失败映射不计入
        const int rc = real_->mmap_close(m);
        if (rc == 0) c.mmap_destroy.fetch_add(1, std::memory_order_relaxed);
        return rc;
    }

    void* mutex_create() override {
        void* h = real_->mutex_create();
        if (h) c.mutex_create.fetch_add(1, std::memory_order_relaxed);
        return h;
    }
    void mutex_destroy(void* h) override {
        if (h) c.mutex_destroy.fetch_add(1, std::memory_order_relaxed);
        real_->mutex_destroy(h);
    }
    void mutex_lock(void* h) override { real_->mutex_lock(h); }
    void mutex_unlock(void* h) override { real_->mutex_unlock(h); }

    int threading_init() override { return real_->threading_init(); }
    int thread_start(void* (*fn)(void*), void* arg, void** out) override {
        const int rc = real_->thread_start(fn, arg, out);
        if (rc == 0) c.thread_create.fetch_add(1, std::memory_order_relaxed);
        return rc;
    }
    int thread_join(void* h) override {
        const int rc = real_->thread_join(h);
        if (rc == 0) c.thread_destroy.fetch_add(1, std::memory_order_relaxed);
        return rc;
    }
    int thread_detach(void* h) override {
        const int rc = real_->thread_detach(h);
        if (rc == 0) c.thread_destroy.fetch_add(1, std::memory_order_relaxed);
        return rc;
    }

    void* ev_create() override {
        void* h = real_->ev_create();
        if (h) c.ev_create.fetch_add(1, std::memory_order_relaxed);
        return h;
    }
    void ev_destroy(void* h) override {
        if (h) c.ev_destroy.fetch_add(1, std::memory_order_relaxed);
        real_->ev_destroy(h);
    }
    void ev_signal(void* h) override { real_->ev_signal(h); }
    void ev_wait(void* h) override { real_->ev_wait(h); }
    bool ev_wait_timeout(void* h, uint32_t ms) override { return real_->ev_wait_timeout(h, ms); }

    uint64_t atomic_add(uint64_t* p, int64_t d) override { return real_->atomic_add(p, d); }
    uint64_t atomic_load(uint64_t* p) override { return real_->atomic_load(p); }

    uint64_t monotonic_ns() override { return real_->monotonic_ns(); }
    uint64_t wallclock_ms() override { return real_->wallclock_ms(); }
    void     wallclock_date(uint32_t* y, uint32_t* h) override { real_->wallclock_date(y, h); }
    void     sleep_ms(uint32_t ms) override { real_->sleep_ms(ms); }

    uint32_t pid() override { return real_->pid(); }
    uint64_t tid() override { return real_->tid(); }

    bool random_bytes(void* buf, size_t len) override { return real_->random_bytes(buf, len); }

    void debug_log(const char* s) override { real_->debug_log(s); }

private:
    Pal* real_;
};

// =========================================================================
// 用例 1: 稳态多轮写入后 RSS/PSS 增量有界
// =========================================================================
//
// 设计:先预热(吸收一次性分配:线程栈、TLS、内部缓冲、分配器 arena),
// 再取基线,然后跑多轮稳态负载并复测。判定的是"稳态增量"而非"绝对占用",
// 因而对一次性增长天然免疫,只捕捉**随写入量线性增长**的泄漏。
TEST(Observability, MemoryRssPss_BoundedGrowthAcrossWriteCycles) {
    Pal& pal = default_pal();
    const std::string dir = make_dir(pal, "mem");
    const std::string staging = dir + "/logx_staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 4096;
    cfg.encrypt_enable = 0;
    // UINT32_MAX = 取默认 8KB 阈值自动唤醒。注意 logx_config_t 零初始化时该字段为 0,
    // 其语义是"禁用按阈值自动唤醒"(见 logx_open 的 E-06 注释),此处显式取生产默认值。
    cfg.flush_threshold = UINT32_MAX;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    ASSERT_NE(inst, nullptr);

    const std::string payload(200, 'm');
    const uint8_t* pp = reinterpret_cast<const uint8_t*>(payload.data());
    const uint32_t plen = static_cast<uint32_t>(payload.size());

    // 预热 + 稳定期:覆盖懒初始化与首次增长,使基线落在稳态平台。
    // 每 kFlushEvery 条显式 flush:256×~230B ≈ 59KB < 128KB 环容量,
    // 保证热路径永不命中 kFull 丢弃分支(即便自动唤醒时序异常)。
    constexpr int kWarm   = 2000;
    constexpr int kSettle = 4000;
    constexpr int kFlushEvery = 256;
    for (int i = 0; i < kWarm + kSettle; ++i) {
        ASSERT_EQ(logx_write(inst, 3, "obs", pp, plen), LOGX_OK) << "warmup i=" << i;
        if ((i + 1) % kFlushEvery == 0) (void)logx_flush(inst);
    }
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    const MemSample before = sample_mem();

    constexpr int kRounds   = 10;
    constexpr int kPerRound = 4000;
    for (int r = 0; r < kRounds; ++r) {
        for (int i = 0; i < kPerRound; ++i) {
            ASSERT_EQ(logx_write(inst, 3, "obs", pp, plen), LOGX_OK)
                << "round " << r << " i=" << i;
            if ((i + 1) % kFlushEvery == 0) (void)logx_flush(inst);
        }
        ASSERT_EQ(logx_flush(inst), LOGX_OK) << "round " << r;
    }

    const MemSample after = sample_mem();

    // 负载有效性:全程无丢弃/截断,否则"内存不增长"可能是因为压根没写入
    logx_stats_t st{};
    logx_get_stats(inst, &st);
    EXPECT_EQ(st.drop_no_space, 0u);
    EXPECT_EQ(st.truncated_logs, 0u);
    EXPECT_EQ(st.block_write_errors, 0u);

    ASSERT_EQ(logx_close(inst), LOGX_OK);

    const uint64_t rss_delta =
        after.rss > before.rss ? after.rss - before.rss : 0;
    const uint64_t pss_delta =
        after.pss > before.pss ? after.pss - before.pss : 0;

    printf("[obs] 内存: RSS %llu -> %llu B (Δ=%llu)   PSS %llu -> %llu B (Δ=%llu)  "
           "[负载 %d 条]\n",
           (unsigned long long)before.rss, (unsigned long long)after.rss,
           (unsigned long long)rss_delta,
           (unsigned long long)before.pss, (unsigned long long)after.pss,
           (unsigned long long)pss_delta,
           kRounds * kPerRound);

    // 阈值:16MB。稳态下库使用固定 127KB 环 + staging mmap + 线程栈,内存不应随
    // 写入量增长;该宽松界吸收分配器碎片、页表与文件页缓存波动,只捕捉逐轮泄漏
    // (每轮泄漏 >1.6MB 即触发)。PSS 不可用的平台仅报告不判定。
    constexpr uint64_t kRssBound = 16ull * 1024 * 1024;
    if (before.rss_ok && after.rss_ok) {
        EXPECT_LT(rss_delta, kRssBound) << "RSS 稳态增量超界(疑似逐轮内存泄漏)";
    } else {
        printf("[obs] 本平台不支持 RSS 采集,该项仅报告不判定\n");
    }
    if (before.pss_ok && after.pss_ok) {
        EXPECT_LT(pss_delta, kRssBound) << "PSS 稳态增量超界(疑似逐轮内存泄漏)";
    } else {
        printf("[obs] 本平台不支持 PSS 采集,该项仅报告不判定\n");
    }

    cleanup_dir(pal, dir);
}

// =========================================================================
// 用例 2: 热路径写入延迟的抖动度量(stddev / CV / MAD / robustCV)
// =========================================================================
//
// 方法论与 bench 阶段 A 同型(突发-清空节拍,保持热路径不命中 kFull),
// 但断言的是**离散度合理**而非"延迟达标";只捕捉病态抖动(锁 convoy /
// 偶发堆分配 / 系统调用风暴),不作性能门禁。
TEST(Observability, LatencyJitter_StddevCvWithinSanityBound) {
    Pal& pal = default_pal();
    const std::string dir = make_dir(pal, "jit");
    const std::string staging = dir + "/logx_staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 4096;
    cfg.encrypt_enable = 0;
    cfg.flush_threshold = UINT32_MAX;   // 生产默认 8KB 自动唤醒(见上一用例说明)

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    ASSERT_NE(inst, nullptr);

    const std::string payload(200, 'j');
    const uint8_t* pp = reinterpret_cast<const uint8_t*>(payload.data());
    const uint32_t plen = static_cast<uint32_t>(payload.size());

    // 256 × ~230B ≈ 59KB < 128KB 环容量 → 环永不满,消除 kFull 丢弃噪声
    constexpr int kWarm = 5000;
    constexpr int kN = 20000;
    constexpr int kFlushEvery = 256;

    for (int i = 0; i < kWarm; ++i) {
        (void)logx_write(inst, 3, "jit", pp, plen);
        if ((i + 1) % kFlushEvery == 0) (void)logx_flush(inst);
    }
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    std::vector<uint64_t> ns;
    ns.reserve(kN);
    int bad = 0;
    for (int i = 0; i < kN; ++i) {
        const uint64_t t0 = now_ns();
        const logx_status_e rc = logx_write(inst, 3, "jit", pp, plen);
        ns.push_back(now_ns() - t0);
        if (rc != LOGX_OK) ++bad;
        if ((i + 1) % kFlushEvery == 0) (void)logx_flush(inst);
    }
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    logx_stats_t st{};
    logx_get_stats(inst, &st);
    ASSERT_EQ(logx_close(inst), LOGX_OK);

    // 样本有效性:全 OK 且无丢弃/截断
    ASSERT_EQ(bad, 0) << "窗口内出现非 OK 返回,延迟样本无效";
    EXPECT_EQ(st.drop_no_space, 0u);
    EXPECT_EQ(st.truncated_logs, 0u);

    const LatencyStats s = compute_latency_stats(ns);
    print_latency_stats("logx_write 延迟分布", s);

    ASSERT_EQ(s.n, static_cast<size_t>(kN));
    EXPECT_GT(s.mean_ns, 0.0);
    EXPECT_TRUE(std::isfinite(s.stddev_ns));
    EXPECT_TRUE(std::isfinite(s.cv));

    // 宽松合理性界(非性能门禁):
    //   robustCV = MAD/median 对个别长尾离群点不敏感,故可用较严的界(5.0);
    //   经典 CV 易被单次调度抢占放大,故只作灾难性护栏(50.0);
    //   P99/P50 比值护栏(100x)捕捉"偶发阻塞"型抖动。
    EXPECT_LT(s.robust_cv, 5.0) << "稳健离散度 MAD/median 超界,热路径抖动异常";
    EXPECT_LT(s.cv, 50.0) << "CV 超界,热路径抖动异常";
    EXPECT_LT(s.p99 / std::max(s.p50, 1.0), 100.0) << "长尾 P99/P50 比异常";

    cleanup_dir(pal, dir);
}

// =========================================================================
// 用例 3: CountingPal 资源泄漏回归(实施方案 L349 验收条款自动化)
// =========================================================================
TEST(LeakRegression, CountingPal_NoResourceLeakAcrossOpenCloseCycles) {
    Pal& real = default_pal();
    CountingPal pal(&real);   // 必须存活覆盖整个实例生命周期(见 pal_override 契约)

    constexpr int kCycles = 6;
    constexpr int kPerCycle = 500;
    const std::string payload(160, 'l');
    const uint8_t* pp = reinterpret_cast<const uint8_t*>(payload.data());
    const uint32_t plen = static_cast<uint32_t>(payload.size());

    int fd_before = -1;   // 首轮后取基线,吸收一次性 fd 分配

    for (int cyc = 0; cyc < kCycles; ++cyc) {
        const std::string dir = make_dir(real, "leak");
        const std::string staging = dir + "/logx_staging.mmap";

        logx_config_t cfg{};
        cfg.log_dir = dir.c_str();
        cfg.staging_path = staging.c_str();
        cfg.max_log_length = 4096;
        cfg.encrypt_enable = 0;
        cfg.flush_threshold = UINT32_MAX;   // 生产默认 8KB 自动唤醒
        cfg.pal_override = &pal;   // T-04 测试专用注入 seam

        logx_instance_t* inst = nullptr;
        ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK) << "cycle " << cyc;
        ASSERT_NE(inst, nullptr);

        for (int i = 0; i < kPerCycle; ++i) {
            ASSERT_EQ(logx_write(inst, 3, "leak", pp, plen), LOGX_OK)
                << "cycle " << cyc << " write " << i;
        }
        ASSERT_EQ(logx_flush(inst), LOGX_OK) << "cycle " << cyc;

        logx_stats_t st{};
        logx_get_stats(inst, &st);
        EXPECT_EQ(st.drop_no_space, 0u) << "cycle " << cyc;
        EXPECT_EQ(st.truncated_logs, 0u) << "cycle " << cyc;

        ASSERT_EQ(logx_close(inst), LOGX_OK) << "cycle " << cyc;
        cleanup_dir(real, dir);

        if (cyc == 0) fd_before = count_open_fds();
    }

    const int fd_after = count_open_fds();

    // ---- 判定 A:create/destroy 严格配对(net 0 泄漏) ----
    EXPECT_EQ(pal.c.fd_create.load(),     pal.c.fd_destroy.load())     << "fd 泄漏";
    EXPECT_EQ(pal.c.mmap_create.load(),   pal.c.mmap_destroy.load())   << "mmap 映射泄漏";
    EXPECT_EQ(pal.c.mutex_create.load(),  pal.c.mutex_destroy.load())  << "mutex 泄漏";
    EXPECT_EQ(pal.c.ev_create.load(),     pal.c.ev_destroy.load())     << "event 泄漏";
    EXPECT_EQ(pal.c.thread_create.load(), pal.c.thread_destroy.load()) << "线程句柄泄漏";

    // ---- 判定 B:计数链路确实被走到(防"空测试"假通过) ----
    EXPECT_GT(pal.c.fd_create.load(), 0)     << "fd 计数未生效";
    EXPECT_GT(pal.c.mmap_create.load(), 0)   << "mmap 计数未生效";
    EXPECT_GT(pal.c.mutex_create.load(), 0)  << "mutex 计数未生效";
    EXPECT_GT(pal.c.ev_create.load(), 0)     << "event 计数未生效";
    EXPECT_GT(pal.c.thread_create.load(), 0) << "线程计数未生效";

    // ---- 判定 C:OS 级 fd 计数独立交叉验证(Linux/Android) ----
    if (fd_before >= 0 && fd_after >= 0) {
        printf("[leak] 进程 fd 数: 基线=%d 终值=%d\n", fd_before, fd_after);
        EXPECT_LE(fd_after, fd_before) << "进程 fd 数净增(独立于 Pal 计数的交叉验证)";
    } else {
        printf("[leak] 本平台不支持 OS 级 fd 计数,该项仅报告不判定\n");
    }

    printf("[leak] %d 轮 open/写/flush/close: fd %lld/%lld  mmap %lld/%lld  "
           "mutex %lld/%lld  ev %lld/%lld  thread %lld/%lld  (create/destroy)\n",
           kCycles,
           (long long)pal.c.fd_create.load(),     (long long)pal.c.fd_destroy.load(),
           (long long)pal.c.mmap_create.load(),   (long long)pal.c.mmap_destroy.load(),
           (long long)pal.c.mutex_create.load(),  (long long)pal.c.mutex_destroy.load(),
           (long long)pal.c.ev_create.load(),     (long long)pal.c.ev_destroy.load(),
           (long long)pal.c.thread_create.load(), (long long)pal.c.thread_destroy.load());
}


// T5.8 / R44-05: Flusher 回调内自销毁 thread_detach 句柄回收测试
TEST(LeakRegression, FlusherSelfDestroyThreadDetachNoLeak) {
    Pal& real_pal = default_pal();
    CountingPal counting_pal(&real_pal);
    std::string dir = make_dir(real_pal, "detach_leak");
    counting_pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    std::atomic<bool> in_flusher_cb{false};
    std::atomic<bool> destroyed{false};

    struct Context {
        logx_instance_t* inst = nullptr;
        std::atomic<bool>* in_flusher_cb = nullptr;
        std::atomic<bool>* destroyed = nullptr;
    } ctx;

    ctx.in_flusher_cb = &in_flusher_cb;
    ctx.destroyed = &destroyed;

    auto err_cb = [](void* user, logx_event_kind_e, int, const char*) {
        auto* c = static_cast<Context*>(user);
        c->in_flusher_cb->store(true, std::memory_order_release);
        // 在回调内部调用 logx_close
        logx_close(c->inst);
    };

    auto on_destroy = [](void* user, void*) {
        auto* c = static_cast<Context*>(user);
        c->destroyed->store(true, std::memory_order_release);
    };

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 64 * 1024;
    cfg.flush_threshold = 1024;
    cfg.pal_override = &counting_pal;
    cfg.err_fn = err_cb;
    cfg.err_user = &ctx;
    cfg.on_destroy = on_destroy;
    cfg.user = &ctx;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    ctx.inst = inst;

    // 手动在内部触发一个事件，模拟回调
    emit_event(logx::test::test_get_instance(inst), LOGX_EV_DEGRADED, -1, "test callback self-close");

    // 等待销毁完成
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!destroyed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(destroyed.load());

    // 核心断言 (R44-05): thread_create 与 thread_destroy 必须严格配对 (detach 回收了句柄)
    EXPECT_EQ(counting_pal.c.thread_create.load(), counting_pal.c.thread_destroy.load())
        << "Flusher 自销毁未泄漏线程句柄";

    cleanup_dir(real_pal, dir);
}

}  // namespace
