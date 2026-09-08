// logx_bench.cpp - T5.2 性能基线基准 harness。
//
// 三阶段基准(验收项见 docs/LogX后续实施计划表.md T5.2 与 docs/benchmark_report.md):
//   A. 单条写入延迟分布:单线程 Native 热路径 logx_write,P50/P90/P99/P99.9/max;
//      分别以 compress_algo = none / zstd 各跑一轮(压缩在 Flusher 线程执行,
//      预期写入热路径延迟与压缩开关无关,此处一并实证)。
//   B. 开/关 zstd 压缩的纯写吞吐与落盘体积对比(混合变长 payload,80% 文本 +
//      20% 伪随机,代表可压与不可压负载)。
//   C. logx_flush 阻塞耗时分布(每轮写 100 条后 flush,模拟退后台 flush 场景)。
//
// 独立 main(),非 gtest,不注册 ctest;LOGX_BUILD_BENCH=ON 时才参与构建。
// 用法: logx_bench [root_dir] [stage_a_n] [stage_b_m]
//   root_dir  输出根目录,默认 ./logx_bench_out(Android 建议 /data/local/tmp/logx_bench)
//   stage_a_n 阶段 A 每轮计样条数,默认 100000
//   stage_b_m 阶段 B 每轮写入条数,默认 100000
//
// 方法论护栏:
//   - 全程核对 drop_no_space / drop_too_large / block_write_errors,非零即宣告该轮
//     数据失真(结果无效),防止环满丢弃污染延迟/吞吐/体积结论;
//   - 计时 std::chrono::steady_clock(与既有测试一致),开头附带计时器开销实测;
//   - 输出目录按 run_N 递增唯一化,避免历史文件污染落盘体积统计;每轮测完即清理
//     日志文件压缩磁盘占用(真机 /data 空间紧张)。

#include <logx_c.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace {

constexpr uint32_t kWarmupA = 10000;     // 阶段 A 预热条数(不计入样本)
constexpr uint32_t kFlushEveryA = 256;   // 阶段 A 突发间隔: 256×~230B ≈ 59KB < 128KB 环容量,环永不满
constexpr uint32_t kWarmupB = 2000;      // 阶段 B 预热条数
constexpr uint32_t kRetryB = 100000;     // 阶段 B NO_SPACE 单条重试上限(自旋为主)
constexpr uint32_t kSpinB = 50000;       // 阶段 B 自旋让位次数(Windows sleep 粒度 ~15ms 会
                                         // 扼杀持续吞吐,优先用 yield 让位 Flusher;仅极端
                                         // 情况才退避到 1ms 休眠,此时吞吐由 drain 决定)
constexpr uint32_t kBenchRoundsC = 300;  // 阶段 C 计时轮数
constexpr uint32_t kWarmupC = 50;        // 阶段 C 预热轮数
constexpr uint32_t kMaxLogLength = 32768; // 显式单条上限(< 数据区-1,远大于最大 payload)

constexpr uint8_t kAlgoNone = 0;
constexpr uint8_t kAlgoZstd = 1;

using Clock = std::chrono::steady_clock;

uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count());
}

// ---- 目录工具(免 std::filesystem,NDK 低 API 级别兼容) ----

bool try_create_dir(const std::string& p) {  // true = 新建成功
#ifdef _WIN32
    return _mkdir(p.c_str()) == 0;
#else
    return ::mkdir(p.c_str(), 0755) == 0;
#endif
}

bool ensure_dir(const std::string& p) {
    return try_create_dir(p) || errno == EEXIST;
}

// 累计目录内普通文件字节总数(不含子目录)
uint64_t dir_file_bytes(const std::string& dir) {
    uint64_t total = 0;
#ifdef _WIN32
    _finddata_t fd;
    intptr_t h = _findfirst((dir + "/*").c_str(), &fd);
    if (h == -1) return 0;
    do {
        if (!(fd.attrib & _A_SUBDIR)) total += static_cast<uint64_t>(fd.size);
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR* d = ::opendir(dir.c_str());
    if (!d) return 0;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;  // . 与 ..
        std::string fp = dir + "/" + e->d_name;
        struct stat st{};
        if (::stat(fp.c_str(), &st) == 0 && S_ISREG(st.st_mode))
            total += static_cast<uint64_t>(st.st_size);
    }
    ::closedir(d);
#endif
    return total;
}

// 删除目录内全部普通文件(测完即清,压缩磁盘占用;staging 在独立目录不受影响)
void clear_dir_files(const std::string& dir) {
#ifdef _WIN32
    _finddata_t fd;
    intptr_t h = _findfirst((dir + "/*").c_str(), &fd);
    if (h == -1) return;
    do {
        if (!(fd.attrib & _A_SUBDIR)) std::remove((dir + "/" + fd.name).c_str());
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        std::string fp = dir + "/" + e->d_name;
        struct stat st{};
        if (::stat(fp.c_str(), &st) == 0 && S_ISREG(st.st_mode)) std::remove(fp.c_str());
    }
    ::closedir(d);
#endif
}

// ---- 统计工具 ----

// 最近秩(nearest-rank)百分位:v 必须已升序排序
uint64_t percentile(const std::vector<uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    size_t idx = static_cast<size_t>(std::ceil(p / 100.0 * static_cast<double>(sorted.size())));
    if (idx == 0) idx = 1;
    if (idx > sorted.size()) idx = sorted.size();
    return sorted[idx - 1];
}

void print_ns_dist(const char* label, std::vector<uint64_t>& v) {
    std::sort(v.begin(), v.end());
    double mean = 0;
    for (uint64_t x : v) mean += static_cast<double>(x);
    mean /= static_cast<double>(v.size());
    uint64_t p50 = percentile(v, 50);
    uint64_t p90 = percentile(v, 90);
    uint64_t p99 = percentile(v, 99);
    uint64_t p999 = percentile(v, 99.9);
    uint64_t mx = v.back();
    printf("  %s (n=%llu):\n", label, static_cast<unsigned long long>(v.size()));
    printf("    mean  = %9llu ns (%8.3f us)\n", (unsigned long long)mean, mean / 1000.0);
    printf("    P50   = %9llu ns (%8.3f us)\n", (unsigned long long)p50, p50 / 1000.0);
    printf("    P90   = %9llu ns (%8.3f us)\n", (unsigned long long)p90, p90 / 1000.0);
    printf("    P99   = %9llu ns (%8.3f us)\n", (unsigned long long)p99, p99 / 1000.0);
    printf("    P99.9 = %9llu ns (%8.3f us)\n", (unsigned long long)p999, p999 / 1000.0);
    printf("    max   = %9llu ns (%8.3f us)\n", (unsigned long long)mx, mx / 1000.0);
}

// ---- 实例与失真护栏 ----

// flush_threshold: UINT32_MAX = 生产默认 8KB 自动唤醒; 0 = 禁用(纯路径, 仅手动 flush)
logx_instance_t* open_instance(const std::string& root, const char* name, uint8_t algo,
                               uint32_t flush_threshold = UINT32_MAX) {
    std::string log_dir = root + "/" + name + "_logs";
    std::string staging_dir = root + "/" + name + "_staging";
    ensure_dir(log_dir);
    ensure_dir(staging_dir);
    // .mmap 后缀:保留期清理对 staging 映射文件无条件豁免(T4.3)
    std::string staging = staging_dir + "/logx_staging.mmap";
    logx_config_t cfg{};
    cfg.log_dir = log_dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = kMaxLogLength;
    cfg.flush_threshold = flush_threshold;
    cfg.compress_algo = algo;
    cfg.encrypt_enable = 0;  // 压缩对比轮关闭加密,避免密文干扰体积对比
    logx_instance_t* inst = nullptr;
    logx_status_e rc = logx_open(&cfg, &inst);
    if (rc != LOGX_OK) {
        fprintf(stderr, "logx_open(%s) failed: rc=%d\n", name, (int)rc);
        return nullptr;
    }
    return inst;
}

struct RoundStats {
    uint64_t drop_no_space = 0;
    uint64_t drop_too_large = 0;
    uint64_t block_write_errors = 0;
};

RoundStats collect_stats(logx_instance_t* inst) {
    logx_stats_t st{};
    logx_get_stats(inst, &st);
    RoundStats rs;
    rs.drop_no_space = st.drop_no_space;
    rs.drop_too_large = st.drop_too_large;
    rs.block_write_errors = st.block_write_errors;
    return rs;
}

// 丢弃计数口径说明: logx_write 每次返回 NO_SPACE 都会累加 drop_no_space
// (含阶段 B 的重试尝试),故阶段 B 不能以该计数判失真;真正的数据丢失判定是
// "重试耗尽仍失败"(返回值口径),由调用方单独统计。only_if_retried=true 时
// 跳过 drop_no_space 检查。
bool round_valid(const char* tag, const RoundStats& rs, bool skip_drop_no_space = false) {
    if ((!skip_drop_no_space && rs.drop_no_space) || rs.drop_too_large || rs.block_write_errors) {
        printf("  !! [INVALID] %s: drop_no_space=%llu drop_too_large=%llu "
               "block_write_errors=%llu — 本轮数据失真,不纳入结论\n",
               tag, (unsigned long long)rs.drop_no_space,
               (unsigned long long)rs.drop_too_large,
               (unsigned long long)rs.block_write_errors);
        return false;
    }
    return true;
}

// ---- payload 构造(确定性,可复现) ----

// 阶段 A/C 用:~200B 中文 + ASCII 混合
std::string make_latency_payload() {
    static const char* zh =
        "基准测试-混合内容日志消息,包含中文与 ASCII 混排,模拟业务典型写入负载,trace_id=";
    std::string s = zh;
    char tail[64];
    snprintf(tail, sizeof(tail), "%09u user_id=10086 cost_ms=12.5", 42u);
    s += tail;
    while (s.size() < 200) s += 'x';
    return s;
}

// 阶段 B 用:1024 条混合变长 payload(80~1536B),80% 结构化文本(可压)+ 20% 伪随机(不可压)
std::vector<std::string> build_mixed_payloads() {
    std::vector<std::string> v;
    v.reserve(1024);
    uint32_t rng = 0x12345678u;
    auto next = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return rng;
    };
    static const char* zh =
        "用户点击了商品详情页并浏览了推荐列表,页面渲染耗时约 35ms,网络类型为 WiFi,"
        "设备电量充足,会话保持正常,推荐位曝光 12 个,点击率环比上升。";
    for (uint32_t i = 0; i < 1024; ++i) {
        uint32_t len = 80 + (i * 37u) % 1456u;
        std::string s;
        if (i % 5 != 4) {
            char head[128];
            snprintf(head, sizeof(head),
                     "order_id=%u event=click page=detail session=%08x ", i, next());
            s += head;
            s += zh;
            while (s.size() < len) s += char('a' + (next() % 26));
        } else {
            s.resize(len);
            for (auto& b : s) b = static_cast<char>(next() & 0xFF);
        }
        s.resize(len);
        v.push_back(std::move(s));
    }
    return v;
}

// ---- 阶段 A:单条写入延迟分布 ----
// 方法论:突发-清空节拍 —— 每 kFlushEveryA 条(≈59KB,远小于 128KB 环容量)
// 阻塞 flush 清空 staging,保证热路径永不命中 kFull 丢弃分支。两种口径:
//   - 默认 8KB 阈值轮(生产口径): 含 pending≥8KB 唤醒 Flusher 与 mutex 交接成本;
//   - flush_threshold=0 纯路径轮: 禁用自动唤醒,度量 format+CRC+memcpy 裸成本,
//     对照 spec M1 "单条写入 <2µs (native 路径)" 目标。

void bench_stage_a(const std::string& run_dir, uint32_t n) {
    const std::string payload = make_latency_payload();
    const char* kTag = "bench";
    printf("\n==== Stage A: 单条写入延迟分布 (单线程热路径, payload=%zuB, N=%u, 突发%u条后flush清空) ====\n",
           payload.size(), (unsigned)n, (unsigned)kFlushEveryA);
    struct RoundDef { const char* name; const char* dirname; uint8_t algo; uint32_t thr; };
    const RoundDef rounds[4] = {
        {"compress=none  thr=8KB(生产默认)", "lat_none", kAlgoNone, UINT32_MAX},
        {"compress=zstd  thr=8KB(生产默认)", "lat_zstd", kAlgoZstd, UINT32_MAX},
        {"compress=none  thr=0(纯路径)",     "lat_none_pure", kAlgoNone, 0},
        {"compress=zstd  thr=0(纯路径)",     "lat_zstd_pure", kAlgoZstd, 0},
    };
    for (const auto& rd : rounds) {
        printf("\n-- %s --\n", rd.name);
        logx_instance_t* inst = open_instance(run_dir, rd.dirname, rd.algo, rd.thr);
        if (!inst) return;
        for (uint32_t w = 0; w < kWarmupA; ++w) {
            logx_write(inst, 3, kTag,
                       reinterpret_cast<const uint8_t*>(payload.data()),
                       static_cast<uint32_t>(payload.size()));
            if ((w + 1) % kFlushEveryA == 0) logx_flush(inst);
        }
        logx_flush(inst);
        std::vector<uint64_t> ns;
        ns.reserve(n);
        uint64_t t_begin = now_ns();
        for (uint32_t j = 0; j < n; ++j) {
            uint64_t t0 = now_ns();
            logx_write(inst, 3, kTag,
                       reinterpret_cast<const uint8_t*>(payload.data()),
                       static_cast<uint32_t>(payload.size()));
            ns.push_back(now_ns() - t0);
            if ((j + 1) % kFlushEveryA == 0) logx_flush(inst);
        }
        uint64_t t_end = now_ns();
        logx_flush(inst);
        RoundStats rs = collect_stats(inst);
        logx_close(inst);
        double sec = static_cast<double>(t_end - t_begin) / 1e9;
        printf("  聚合吞吐: %.0f logs/s (含每%u条阻塞 flush 与其计时,仅供量级参考)\n",
               static_cast<double>(n) / sec, (unsigned)kFlushEveryA);
        if (round_valid(rd.name, rs)) print_ns_dist("logx_write 延迟", ns);
        clear_dir_files(run_dir + "/" + rd.dirname + "_logs");
    }
}

// ---- 阶段 B:纯写吞吐与落盘体积(开/关 zstd 对比) ----
// 方法论:持续压满节拍 —— 写入速率远超 Flusher 消化速率时,NO_SPACE 走有界
// 重试(yield 让位),度量的是"可持续不丢吞吐";全量校验 drop 计数为零,
// 确保落盘体积对比基于无丢失数据集(与压测重试模式同型)。

void bench_stage_b(const std::string& run_dir, uint32_t m) {
    printf("\n==== Stage B: 纯写吞吐与落盘体积 (混合变长 80~1536B, 80%%文本+20%%伪随机, M=%u, NO_SPACE 有界重试) ====\n",
           (unsigned)m);
    static const std::vector<std::string> payloads = build_mixed_payloads();
    const char* kTag = "bench";
    const uint8_t algos[2] = {kAlgoNone, kAlgoZstd};
    const char* names[2] = {"compress=none", "compress=zstd"};
    const char* dirnames[2] = {"thr_none", "thr_zstd"};
    for (int i = 0; i < 2; ++i) {
        printf("\n-- %s --\n", names[i]);
        logx_instance_t* inst = open_instance(run_dir, dirnames[i], algos[i]);
        if (!inst) return;
        for (uint32_t w = 0; w < kWarmupB; ++w) {
            const std::string& p = payloads[w % payloads.size()];
            logx_write(inst, 3, kTag,
                       reinterpret_cast<const uint8_t*>(p.data()),
                       static_cast<uint32_t>(p.size()));
        }
        logx_flush(inst);
        uint64_t bytes = 0;
        uint64_t retry_total = 0;
        uint64_t lost = 0;  // 重试耗尽仍失败 = 真实数据丢失
        uint64_t t0 = now_ns();
        for (uint32_t j = 0; j < m; ++j) {
            const std::string& p = payloads[j % payloads.size()];
            uint32_t plen = static_cast<uint32_t>(p.size());
            bool ok = false;
            for (uint32_t retry = 0; retry < kRetryB; ++retry) {
                if (logx_write(inst, 3, kTag,
                               reinterpret_cast<const uint8_t*>(p.data()),
                               plen) == LOGX_OK) {
                    ok = true;
                    if (retry) ++retry_total;
                    break;
                }
                if (retry < kSpinB) {
                    std::this_thread::yield();
                } else {
                    // 混合负载写入速率 > Flusher 消化速率时,yield 自旋会饿死
                    // Flusher;带退避休眠给其释放窗口(风格同 flush_blocking 的 sleep_ms)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            if (ok) {
                bytes += plen;
            } else {
                ++lost;
            }
        }
        uint64_t t1 = now_ns();  // 纯写止点(最终 flush/close 不计入吞吐)
        logx_flush(inst);
        RoundStats rs = collect_stats(inst);
        logx_close(inst);
        double sec = static_cast<double>(t1 - t0) / 1e9;
        printf("  纯写吞吐: %.0f logs/s, %.1f MB/s (payload %llu B / %.3f s, NO_SPACE 重试 %llu 次)\n",
               static_cast<double>(m) / sec,
               static_cast<double>(bytes) / 1048576.0 / sec,
               (unsigned long long)bytes, sec, (unsigned long long)retry_total);
        if (i == 1 && rs.block_write_errors > 0)
            printf("  (提示: zstd 块编码失败 — 确认是否以 -DLOGX_WITH_ZSTD=ON 构建)\n");
        // 阶段 B 口径: drop_no_space 含重试尝试属预期,只校验真实丢失/损坏
        if (lost == 0 && round_valid(names[i], rs, /*skip_drop_no_space=*/true)) {
            std::string log_dir = run_dir + "/" + dirnames[i] + "_logs";
            uint64_t disk = dir_file_bytes(log_dir);
            printf("  落盘体积: %llu B (含块头/CRC), payload/落盘压缩比 = %.2fx\n",
                   (unsigned long long)disk,
                   disk ? static_cast<double>(bytes) / static_cast<double>(disk) : 0.0);
        } else if (lost) {
            printf("  !! [INVALID] %s: %llu 条重试耗尽仍失败(数据丢失),本轮不纳入结论\n",
                   names[i], (unsigned long long)lost);
        }
        clear_dir_files(run_dir + "/" + dirnames[i] + "_logs");
    }
}

// ---- 阶段 C:logx_flush 阻塞耗时 ----

void bench_stage_c(const std::string& run_dir) {
    printf("\n==== Stage C: logx_flush 阻塞耗时 (每轮写 100 条 ~200B 后 flush, 模拟退后台) ====\n");
    const std::string payload = make_latency_payload();
    const char* kTag = "bench";
    logx_instance_t* inst = open_instance(run_dir, "flush", kAlgoNone);
    if (!inst) return;
    std::vector<uint64_t> ns;
    ns.reserve(kBenchRoundsC);
    uint64_t flush_err = 0;
    for (uint32_t r = 0; r < kWarmupC + kBenchRoundsC; ++r) {
        for (int j = 0; j < 100; ++j)
            logx_write(inst, 3, kTag,
                       reinterpret_cast<const uint8_t*>(payload.data()),
                       static_cast<uint32_t>(payload.size()));
        uint64_t t0 = now_ns();
        logx_status_e rc = logx_flush(inst);
        uint64_t dt = now_ns() - t0;
        if (r >= kWarmupC) {
            ns.push_back(dt);
            if (rc != LOGX_OK) ++flush_err;
        }
    }
    RoundStats rs = collect_stats(inst);
    logx_close(inst);
    if (flush_err)
        printf("  !! flush 非 OK 返回 %llu 次 (LOGX_ERR_TIMEOUT 等)\n",
               (unsigned long long)flush_err);
    if (round_valid("flush", rs)) print_ns_dist("logx_flush 耗时", ns);
    clear_dir_files(run_dir + "/flush_logs");
}

void print_clock_overhead() {
    const int kN = 200000;
    uint64_t sink = 0;
    uint64_t t0 = now_ns();
    for (int i = 0; i < kN; ++i)
        sink += static_cast<uint64_t>(Clock::now().time_since_epoch().count());
    uint64_t t1 = now_ns();
    printf("计时器开销: 单次 steady_clock::now() 均值 ~= %.0f ns (sink=%llu 防优化消除)\n",
           static_cast<double>(t1 - t0) / kN, (unsigned long long)sink);
}

}  // namespace

int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : "logx_bench_out";
    uint32_t n_a = argc > 2 ? static_cast<uint32_t>(strtoul(argv[2], nullptr, 10)) : 100000;
    uint32_t m_b = argc > 3 ? static_cast<uint32_t>(strtoul(argv[3], nullptr, 10)) : 100000;

    printf("LogX 性能基线基准 (T5.2)  stage_a_n=%u stage_b_m=%u\n", (unsigned)n_a, (unsigned)m_b);
    print_clock_overhead();

    if (!ensure_dir(root)) {
        fprintf(stderr, "cannot create root dir: %s\n", root.c_str());
        return 2;
    }
    std::string run_dir;
    for (uint32_t i = 0;; ++i) {
        char buf[32];
        snprintf(buf, sizeof(buf), "/run_%u", (unsigned)i);
        std::string cand = root + buf;
        if (try_create_dir(cand)) {
            run_dir = cand;
            break;
        }
    }
    printf("输出目录: %s\n", run_dir.c_str());

    bench_stage_a(run_dir, n_a);
    bench_stage_b(run_dir, m_b);
    bench_stage_c(run_dir);

    printf("\n==== 基准完成 ====\n");
    return 0;
}
