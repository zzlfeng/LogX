// test_crash_recovery.cpp - 优雅拆毁与磁盘坏块自愈恢复测试套件。
//
// 注: 真正操作系统级突然强杀 (Abrupt Process Termination / kill -9) 由
//     test_crash_fork.cpp (logx_crash_fork_tests) 通过跨进程测试覆盖 (解决 T-01)。
//
// 覆盖:
//   - GracefulTeardown 模式下未显式 flush 的 staging 数据恢复
//   - 磁盘尾部写出撕裂坏块 → 启动恢复自动截断自愈并打点 LOGX_EV_RECOVERY
//   - 50 轮多模式拆毁与重启恢复压力循环 → 零死锁、零状态错乱、数据 100% 一致

#include "logx_c.h"
#include "test_accessor.h"
#include "logx_internal.h"
#include "logx_format.h"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace logx;

namespace {

static std::atomic<uint64_t> s_recovery_dir_seq{0};

static std::string make_recovery_test_dir(Pal& pal, const char* prefix) {
    uint64_t seq = s_recovery_dir_seq.fetch_add(1, std::memory_order_relaxed);
    return std::string(prefix) + "_" + std::to_string(pal.pid()) + "_" +
           std::to_string(pal.monotonic_ns()) + "_" + std::to_string(seq);
}

// =========================================================================
// 1. Staging 仅写入未落盘优雅拆毁 → 重启恢复自动补刷落盘
// =========================================================================

TEST(CrashRecovery, GracefulTeardown_StagingDataRecovered) {
    Pal& pal = default_pal();
    std::string dir = make_recovery_test_dir(pal, "logx_teardown_dir");
    pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    // 写入 20 条日志 (仅放入 staging 环形缓冲区, 绝不调用 logx_flush)
    for (int i = 0; i < 20; ++i) {
        std::string msg = "crash_test_record_" + std::to_string(i);
        ASSERT_EQ(logx_write(inst, 2, "CRASH",
                             reinterpret_cast<const uint8_t*>(msg.data()),
                             static_cast<uint32_t>(msg.size())), LOGX_OK);
    }

    // 模拟进程突然崩溃或 kill -9 (R44-06: 经 test_accessor 模拟强杀)
    logx::test::test_simulate_crash_destroy(inst);

    // 重启引擎: 模拟崩溃后应用重新启动, 触发 run_recovery
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    // 调用 logx_flush 触发对账恢复后的全量落盘
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    // 通过 logx_export 导出验证: 20 条日志全部完整复原!
    uint32_t yyyymmdd = 0, hhmmss = 0;
    pal.wallclock_date(&yyyymmdd, &hhmmss);
    char date_str[16];
    std::snprintf(date_str, sizeof(date_str), "%08u", yyyymmdd);

    std::atomic<uint64_t> exported_count{0};
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

    EXPECT_EQ(logx_export(inst, date_str, date_str, exp_cb, &exported_count), LOGX_OK);
    EXPECT_EQ(exported_count.load(), 20u);

    logx_close(inst);

    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

// =========================================================================
// 2. 磁盘尾部写出撕裂坏块 → 启动恢复自动截断自愈并打点 LOGX_EV_RECOVERY
// =========================================================================

TEST(CrashRecovery, TornBlockAtDiskTail_TruncatedSafelyAndCapturesEvent) {
    Pal& pal = default_pal();
    std::string dir = make_recovery_test_dir(pal, "logx_torn_dir");
    pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    std::atomic<int> recovery_event_count{0};
    cfg.err_user = &recovery_event_count;
    auto on_recovery = [](void* user, logx_event_kind_e kind, int /*code*/, const char* /*msg*/) {
        if (kind == LOGX_EV_RECOVERY) {
            auto* cnt = static_cast<std::atomic<int>*>(user);
            cnt->fetch_add(1, std::memory_order_relaxed);
        }
    };
    cfg.err_fn = on_recovery;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    // 正常写入 5 条日志并落盘
    for (int i = 0; i < 5; ++i) {
        std::string msg = "normal_record_" + std::to_string(i);
        ASSERT_EQ(logx_write(inst, 2, "NORM",
                             reinterpret_cast<const uint8_t*>(msg.data()),
                             static_cast<uint32_t>(msg.size())), LOGX_OK);
    }
    ASSERT_EQ(logx_flush(inst), LOGX_OK);
    logx_close(inst);

    // 找到落盘日志文件, 在尾部注入伪造的坏块 (破坏 CRC32)
    auto files = pal.file_list(dir.c_str());
    std::string log_file;
    for (const auto& f : files) {
        if (f.find(".log") != std::string::npos && f.find("staging") == std::string::npos) {
            log_file = f;
            break;
        }
    }
    ASSERT_FALSE(log_file.empty());
    int64_t valid_size = pal.file_size_path(log_file.c_str());
    ASSERT_GT(valid_size, 32);

    // 在文件末尾追加一个损坏的 Block Header (crc 错误)
    int fd = pal.file_open(log_file.c_str(), true, false);
    ASSERT_GE(fd, 0);
    uint8_t bad_block[34 + 100];
    std::memset(bad_block, 0x55, sizeof(bad_block));
    put_u32(bad_block + 0, kMagicBlock);
    put_u16(bad_block + 4, 1);
    bad_block[5] = 0;
    put_u32(bad_block + 6, 999);  // seqno
    put_u32(bad_block + 10, 100); // raw_len
    put_u32(bad_block + 14, 100); // payload_len
    put_u32(bad_block + 18, 0xDEADBEEF); // 故意错误的 CRC32!
    pal.file_pwrite(fd, bad_block, sizeof(bad_block), valid_size);
    pal.file_close(fd);

    // 重新打开实例: 校验 recovery 精准截断坏块并打点 LOGX_EV_RECOVERY
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    EXPECT_GT(recovery_event_count.load(), 0);
    // 验证文件大小被精确截断回合法尺寸
    int64_t repaired_size = pal.file_size_path(log_file.c_str());
    EXPECT_EQ(repaired_size, valid_size);

    // 恢复后可继续正常写入并落盘
    std::string new_msg = "after_recovery_log";
    ASSERT_EQ(logx_write(inst, 2, "NEW",
                         reinterpret_cast<const uint8_t*>(new_msg.data()),
                         static_cast<uint32_t>(new_msg.size())), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);
    logx_close(inst);

    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

// =========================================================================
// 3. 50 轮随机阶段拆毁与恢复自愈压力循环
// =========================================================================

TEST(CrashRecovery, FiftyCycles_RandomTeardownAndRecoveryLoop) {
    Pal& pal = default_pal();
    std::string dir = make_recovery_test_dir(pal, "logx_loop_dir");
    pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    const int kCycles = 50;
    uint64_t total_expected_logs = 0;

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        logx_instance_t* inst = nullptr;
        ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

        int batch_logs = 5 + (cycle % 7);
        for (int i = 0; i < batch_logs; ++i) {
            std::string msg = "cycle=" + std::to_string(cycle) + " item=" + std::to_string(i);
            ASSERT_EQ(logx_write(inst, 2, "LOOP",
                                 reinterpret_cast<const uint8_t*>(msg.data()),
                                 static_cast<uint32_t>(msg.size())), LOGX_OK);
        }
        total_expected_logs += batch_logs;

        int mode = cycle % 3;
        if (mode == 0) {
            // 模式 0: 执行标准 flush 后关闭
            ASSERT_EQ(logx_flush(inst), LOGX_OK);
            logx_close(inst);
        } else if (mode == 1) {
            // 模式 1: 不显式 flush, 直接关闭 (依靠 close 内置兜底 flush)
            logx_close(inst);
        } else {
            // 模式 2: 模拟突然强杀崩溃 (绕过 close flush, 直接拆毁实例)
            logx::test::test_simulate_crash_destroy(inst);
        }
    }

    // 最终轮: 打开实例, 执行最后一次 flush 并全量导出对账
    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    uint32_t yyyymmdd = 0, hhmmss = 0;
    pal.wallclock_date(&yyyymmdd, &hhmmss);
    char date_str[16];
    std::snprintf(date_str, sizeof(date_str), "%08u", yyyymmdd);

    std::atomic<uint64_t> exported_count{0};
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

    EXPECT_EQ(logx_export(inst, date_str, date_str, exp_cb, &exported_count), LOGX_OK);
    logx_close(inst);

    std::cout << "\n[Crash Injection Summary] 50 Cycles Multi-Stage Crash Loop:" << std::endl;
    std::cout << "  - Expected logs : " << total_expected_logs << std::endl;
    std::cout << "  - Exported logs : " << exported_count.load() << std::endl;

    // 核心断言: 经过 50 轮连续崩溃恢复循环, 日志 100% 完整复原, 零丢失!
    EXPECT_EQ(exported_count.load(), total_expected_logs);

    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

} // namespace
