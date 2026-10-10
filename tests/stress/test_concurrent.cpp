// test_concurrent.cpp - 50 线程高并发压力测试与死锁/数据一致性对账 (T4.4)
//
// 压测场景:
//   - 50 个 Worker 线程通过原子发令枪同时并发起跑;
//   - 写入变长日志 (80B ~ 2000B 混合分布);
//   - 1 个专属竞争线程在高并发写入期间以 10ms~20ms 间隔高频主动触发 logx_flush(),
//     与内核后台 Flusher 线程及 50 个写线程的互斥锁制造极限锁争用与切块打断;
//   - 线程遇空间满时进行合理 yield 重试, 充分压测环形吞吐、动态切块与多文件切分轮转;
//   - 压测后通过文件扫描验证 seqno 严格单调递增,
//     并通过 export 端到端对账验证无死锁、零损坏、零重复。

#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "logx_c.h"
#include "logx_codec.h"
#include "logx_format.h"
#include "logx_pal.h"
#include "logx_staging.h"
#include "logx_internal.h"  // (T-11) Instance 完整定义(staged_bytes_total)

using namespace logx;

// 文件作用域常量 (R37 N-2): constexpr 局部量在 MSVC 下无法被显式捕获列表 lambda
// 引用 (C3493)、Clang 下捕获又触发 -Wunused-lambda-capture——上提至文件作用域后
// 两侧均无需捕获。
constexpr int kStressWorkerCount = 50;
constexpr int kStressLogsPerWorker = 500;  // 50 * 500 = 25,000 条混合变长并发写入

TEST(StressTest, FiftyThreads_HeavyConcurrentWritesWithFlushCompetition) {
    Pal& pal = default_pal();
    // (T-10) 原子自增替代未播种 rand():消除 CI 重跑/上轮残留目录撞名风险
    static std::atomic<uint64_t> s_dir_seq{0};
    std::string dir = std::string("logx_stress_dir_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_dir_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());
    std::string staging_file = dir + "/staging.mmap";

    const uint64_t kTotalExpected = static_cast<uint64_t>(kStressWorkerCount) * kStressLogsPerWorker;

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging_file.c_str();
    cfg.max_log_length = 20 * 1024;
    cfg.retention_days = 7;
    cfg.max_file_size = 512 * 1024;       // 512KB 强制触发多次文件切分轮转 (.log, .log.1, .log.2 ...)
    cfg.flush_threshold = 16 * 1024;      // 16KB 自动刷盘阈值
    cfg.compress_algo = kAlgoNone;
    cfg.encrypt_enable = 0;

    std::atomic<uint64_t> dropped_count{0};
    cfg.err_fn = [](void* user, logx_event_kind_e kind, int, const char*) {
        if (kind == LOGX_EV_DROP) {
            static_cast<std::atomic<uint64_t>*>(user)->fetch_add(1, std::memory_order_relaxed);
        }
    };
    cfg.err_user = &dropped_count;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    std::atomic<bool> pre_gun{false};    // (T-11) 第一阶段:允许 Worker 预热写
    std::atomic<bool> start_gun{false};  // 第二阶段:预热达标后激活竞争 flush
    std::atomic<bool> stop_competitor{false};
    std::atomic<uint64_t> total_written{0};
    std::vector<std::thread> workers;
    workers.reserve(kStressWorkerCount);

    std::vector<std::vector<bool>> written_table(kStressWorkerCount, std::vector<bool>(kStressLogsPerWorker, false));

    // 1. 启动 50 个 Worker 写线程
    for (int tid = 0; tid < kStressWorkerCount; ++tid) {
        workers.emplace_back([&inst, &pre_gun, &start_gun, &total_written, &written_table, tid]() {
            char tag_buf[32];
            std::snprintf(tag_buf, sizeof(tag_buf), "Wk_%02d", tid);

            // 单条写入(带 NO_SPACE 重试);warmup=true 时成功也计入对账表
            auto write_one = [&](int i) {
                int mode = (i + tid) % 20;
                uint32_t payload_len = 80 + static_cast<uint32_t>((i * 17 + tid * 13) % 200);
                if (mode == 18) payload_len = 800;
                else if (mode == 19) payload_len = 1500;

                std::string msg;
                msg.reserve(payload_len);
                msg.append("tid=").append(std::to_string(tid))
                   .append(" seq=").append(std::to_string(i))
                   .append(" ");
                if (msg.size() < payload_len) {
                    msg.append(payload_len - msg.size(), 'x');
                }
                logx_status_e rc = LOGX_ERR_NO_SPACE;
                for (int retry = 0; retry < 50; ++retry) {
                    rc = logx_write(inst, 2, tag_buf,
                                    reinterpret_cast<const uint8_t*>(msg.data()),
                                    static_cast<uint32_t>(msg.size()));
                    if (rc == LOGX_OK) {
                        written_table[tid][i] = true;
                        total_written.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                    std::this_thread::yield();
                }
            };

            // (T-11) 两阶段起跑:先等 pre_gun 预热写满 staging,
            // 竞争线程只被 start_gun 放行 —— 竞争窗口无空转
            while (!pre_gun.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            // 预热阶段:每线程先写 kWarmupPerWorker 条,50×8 条 ≈ 90KB > 16KB 阈值
            constexpr int kWarmupPerWorker = 8;
            for (int i = 0; i < kWarmupPerWorker; ++i) {
                write_one(i);
            }
            // 等待竞争发令枪:主线程确认 staging 已有数据后鸣枪
            while (!start_gun.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = kWarmupPerWorker; i < kStressLogsPerWorker; ++i) {
                write_one(i);
            }
        });
    }

    // 2. 启动 1 个竞争 flush 线程, 制造极限锁冲突与打断
    std::thread competitor([&inst, &start_gun, &stop_competitor, &pal]() {
        while (!start_gun.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        while (!stop_competitor.load(std::memory_order_relaxed)) {
            pal.sleep_ms(15);
            logx_flush(inst);
        }
    });

    // 2.5 (T-11) 放行预热写,主线程等待 staging 积累真实数据后再激活竞争,
    // 消除"竞争线程起跑后前 15ms staging 尚无数据"的空跑窗口
    pre_gun.store(true, std::memory_order_release);
    while (inst->staged_bytes_total.load(std::memory_order_acquire) < 16 * 1024) {
        std::this_thread::yield();
    }

    // 3. 鸣枪起跑!
    auto t_start = std::chrono::steady_clock::now();
    start_gun.store(true, std::memory_order_release);

    // 4. 等待全部 50 个 Worker 写入结束
    for (auto& w : workers) {
        w.join();
    }
    stop_competitor.store(true, std::memory_order_release);
    competitor.join();

    auto t_write_done = std::chrono::steady_clock::now();

    // 5. 执行最后一次刷盘并关闭实例
    ASSERT_EQ(logx_flush(inst), LOGX_OK);
    logx_close(inst);

    auto t_all_done = std::chrono::steady_clock::now();

    double write_sec = std::chrono::duration<double>(t_write_done - t_start).count();
    double total_sec = std::chrono::duration<double>(t_all_done - t_start).count();

    uint64_t written = total_written.load();
    uint64_t dropped = dropped_count.load();

    std::cout << "\n[Stress Metrics] 50 Threads Concurrency Summary:" << std::endl;
    std::cout << "  - Expected logs : " << kTotalExpected << std::endl;
    std::cout << "  - Written logs  : " << written << " (" << (100.0 * written / kTotalExpected) << "%)" << std::endl;
    std::cout << "  - Dropped events: " << dropped << std::endl;
    std::cout << "  - Pure write sec: " << write_sec << " s (" << (written / write_sec) << " logs/s)" << std::endl;
    std::cout << "  - Total time sec: " << total_sec << " s" << std::endl;

    EXPECT_GT(written, 0u);

    // 6. 严苛对账 1: 扫描所有生成的日志文件, 校验 CRC32 与 seqno 严格全局单调自增
    auto files = pal.file_list(dir.c_str());
    std::vector<std::string> log_files;
    for (const auto& f : files) {
        if (f.find(".log") != std::string::npos) {
            log_files.push_back(f);
        }
    }
    auto parse_key = [](const std::string& f) -> std::pair<uint32_t, int> {
        size_t base = f.find_last_of("/\\");
        std::string name = (base == std::string::npos) ? f : f.substr(base + 1);
        uint32_t d = static_cast<uint32_t>(std::strtoul(name.c_str(), nullptr, 10));
        auto p = name.rfind(".log.");
        int s = (p == std::string::npos) ? 0 : std::atoi(name.c_str() + p + 5);
        return {d, s};
    };
    std::sort(log_files.begin(), log_files.end(), [&](const std::string& a, const std::string& b) {
        return parse_key(a) < parse_key(b);
    });
    ASSERT_FALSE(log_files.empty());
    std::cout << "  - Total log files generated: " << log_files.size() << " (Files rotated successfully!)" << std::endl;

    uint32_t expected_next_seqno = 1;
    uint64_t total_blocks_verified = 0;
    uint64_t disk_records_count = 0;

    std::vector<std::vector<bool>> exported_table(kStressWorkerCount, std::vector<bool>(kStressLogsPerWorker, false));

    for (const auto& fpath : log_files) {
        int fd = pal.file_open(fpath.c_str(), false, false);
        ASSERT_GE(fd, 0);
        int64_t fsize = pal.file_size(fd);
        ASSERT_GE(fsize, 32);  // 至少包含 32B 文件头

        uint8_t fhdr[32];
        ASSERT_EQ(pal.file_pread(fd, fhdr, 32, 0), 32);
        EXPECT_EQ(get_u32(fhdr), kMagicFile);

        std::vector<uint8_t> carry_slice_buf;
        uint64_t off = 32;
        while (off + 34 <= static_cast<uint64_t>(fsize)) {
            uint8_t bhdr[34];
            if (pal.file_pread(fd, bhdr, 34, off) != 34) break;
            if (get_u32(bhdr) != kMagicBlock) break;

            uint32_t seqno = get_u32(bhdr + 6);
            uint32_t raw_len = get_u32(bhdr + 10);
            uint32_t payload_len = get_u32(bhdr + 14);
            uint32_t crc = get_u32(bhdr + 18);

            // 本测试 kAlgoNone + 明文: 存储载荷必等于原始长度, 块头双长度字段交叉互证
            EXPECT_EQ(raw_len, payload_len)
                << "raw_len mismatch at file " << fpath << " offset " << off;

            // 严格验证 seqno 单调自增, 绝无乱序、跳号或重复!
            EXPECT_EQ(seqno, expected_next_seqno)
                << "Seqno break at file " << fpath << " offset " << off;
            expected_next_seqno++;
            total_blocks_verified++;

            std::vector<uint8_t> payload(payload_len);
            ASSERT_EQ(pal.file_pread(fd, payload.data(), payload_len, off + 34),
                      static_cast<long>(payload_len));

            // 严格验证块级 CRC32 强校验
            EXPECT_EQ(crc32_update(0, payload.data(), payload_len), crc);

            std::vector<std::vector<uint8_t>> recs;
            parse_records_stream(payload.data(), payload_len, &recs, &carry_slice_buf);
            for (const auto& r : recs) {
                std::string s(r.begin(), r.end());
                auto pos_tid = s.find("tid=");
                auto pos_seq = s.find("seq=");
                if (pos_tid != std::string::npos && pos_seq != std::string::npos) {
                    int tid = std::atoi(s.c_str() + pos_tid + 4);
                    int seq = std::atoi(s.c_str() + pos_seq + 4);
                    if (tid >= 0 && tid < kStressWorkerCount && seq >= 0 && seq < kStressLogsPerWorker) {
                        exported_table[tid][seq] = true;
                    }
                }
            }
            disk_records_count += recs.size();

            off += 34 + payload_len;
        }
        pal.file_close(fd);
    }

    int missing_count = 0;
    for (int tid = 0; tid < kStressWorkerCount; ++tid) {
        for (int i = 0; i < kStressLogsPerWorker; ++i) {
            if (written_table[tid][i] && !exported_table[tid][i]) {
                missing_count++;
            }
        }
    }
    std::cout << "  - Missing records found: " << missing_count << std::endl;
    // (T-06) 显式断言:逐条对账不允许任何"已写入但导出缺失"的记录
    // (纯总数对账无法捕捉"等量丢失+等量重复"逃逸,此处为逐条精确对账)
    EXPECT_EQ(missing_count, 0);

    std::cout << "  - Verified Blocks: " << total_blocks_verified
              << ", Monotonic Seqno reached: " << (expected_next_seqno - 1)
              << ", Raw Disk Records: " << disk_records_count << std::endl;
    EXPECT_GT(total_blocks_verified, 0u);

    // 7. 严苛对账 2: 通过公开 C API logx_export 导出, 对账导出的总有效日志行数
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    uint32_t yyyymmdd = 0, hhmmss = 0;
    pal.wallclock_date(&yyyymmdd, &hhmmss);
    char date_str[16];
    std::snprintf(date_str, sizeof(date_str), "%08u", yyyymmdd);

    std::atomic<uint64_t> exported_lines{0};
    auto exp_cb = [](void* user, const char* /*date*/, const char* plaintext_path, uint64_t size) -> int {
        if (size == 0) return 0;
        auto* counter = static_cast<std::atomic<uint64_t>*>(user);
        Pal& p = default_pal();
        int fd = p.file_open(plaintext_path, false, false);
        if (fd >= 0) {
            std::vector<char> buf(size + 1, 0);
            p.file_pread(fd, buf.data(), size, 0);
            p.file_close(fd);
            int cur_lines = 0;
            for (uint64_t i = 0; i < size; ++i) {
                if (buf[i] == '\n') cur_lines++;
            }
            counter->fetch_add(cur_lines, std::memory_order_relaxed);
        }
        return 0;
    };

    EXPECT_EQ(logx_export(inst, date_str, date_str, exp_cb, &exported_lines), LOGX_OK);
    logx_close(inst);

    std::cout << "  - Exported records: " << exported_lines.load() << std::endl;

    // 核心断言: 导出的记录数必须精确等于 50 线程成功落盘的记录数!
    EXPECT_EQ(exported_lines.load(), written);

    // 清理测试目录
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}
