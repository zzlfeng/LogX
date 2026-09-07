// test_crash_fork.cpp - 真正进程级突然强杀 (Abrupt Process Termination / kill -9) 恢复测试套件
//
// 解决报告二 T-01:
// 之前测试在同进程内通过 thread_join + delete 模拟崩溃，会触发 flusher exit-drain
// 和 Staging::close() 刷盘，无法验证未刷盘 staging 在真实 kill -9 下的持久化与恢复。
//
// 本套件通过独立子进程运行 logx_write，随后调用系统级立即退出函数
// (Windows: ExitProcess, POSIX: ::_exit) 绕过 C++ 析构函数、atexit 钩子与 CRT 缓冲区，
// 模拟真实硬崩溃。父进程随后重新打开实例并校验 run_recovery 是否完整恢复。

#include "logx_c.h"
#include "logx_format.h"
#include "logx_internal.h"
#include "logx_pal.h"
#include "logx_staging.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace logx;

namespace {

[[noreturn]] void exit_immediately(int code) {
#if defined(_WIN32)
    ExitProcess(static_cast<UINT>(code));
#else
    ::_exit(code);
#endif
}

std::atomic<uint64_t> s_fork_dir_seq{0};

std::string make_fork_test_dir(Pal& pal, const char* prefix) {
    uint64_t seq = s_fork_dir_seq.fetch_add(1, std::memory_order_relaxed);
    return std::string(prefix) + "_" + std::to_string(pal.pid()) + "_" +
           std::to_string(pal.monotonic_ns()) + "_" + std::to_string(seq);
}

void clean_dir(Pal& pal, const std::string& dir) {
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

uint64_t count_exported_records(Pal& pal, logx_instance_t* inst) {
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
    return exported_count.load();
}

} // namespace

// 子进程工作函数 (声明为全局以供 main 派发)
int run_child_worker(const char* dir, const char* mode, int count) {
    Pal& pal = default_pal();
    pal.file_mkdirs(dir);
    std::string st_path = std::string(dir) + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir;
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    if (logx_open(&cfg, &inst) != LOGX_OK) {
        exit_immediately(101);
    }

    if (std::strcmp(mode, "staging_kill") == 0) {
        // 模式: 写入 staging 环形缓冲区后直接硬杀 (绝不调用 logx_flush 或 logx_close)
        for (int i = 0; i < count; ++i) {
            std::string msg = "fork_stg_" + std::to_string(i);
            if (logx_write(inst, 2, "STG",
                           reinterpret_cast<const uint8_t*>(msg.data()),
                           static_cast<uint32_t>(msg.size())) != LOGX_OK) {
                exit_immediately(102);
            }
        }
        // 操作系统级立即终止，完全绕过析构函数、atexit 钩子与 CRT flush
        exit_immediately(count % 250);
    } else if (std::strcmp(mode, "midblock_kill") == 0) {
        // 模式: 写入基准条目并刷盘，然后继续写入 staging 后硬杀
        int initial = (std::min)(5, count);
        for (int i = 0; i < initial; ++i) {
            std::string msg = "fork_base_" + std::to_string(i);
            if (logx_write(inst, 2, "BASE",
                           reinterpret_cast<const uint8_t*>(msg.data()),
                           static_cast<uint32_t>(msg.size())) != LOGX_OK) {
                exit_immediately(102);
            }
        }
        if (logx_flush(inst) != LOGX_OK) {
            exit_immediately(103);
        }
        for (int i = initial; i < count; ++i) {
            std::string msg = "fork_unflushed_" + std::to_string(i);
            if (logx_write(inst, 2, "UNFL",
                           reinterpret_cast<const uint8_t*>(msg.data()),
                           static_cast<uint32_t>(msg.size())) != LOGX_OK) {
                exit_immediately(102);
            }
        }
        exit_immediately(count % 250);
    } else if (std::strcmp(mode, "graceful") == 0) {
        // 模式: 优雅刷盘退出
        for (int i = 0; i < count; ++i) {
            std::string msg = "fork_graceful_" + std::to_string(i);
            if (logx_write(inst, 2, "GRACE",
                           reinterpret_cast<const uint8_t*>(msg.data()),
                           static_cast<uint32_t>(msg.size())) != LOGX_OK) {
                exit_immediately(102);
            }
        }
        logx_flush(inst);
        logx_close(inst);
        return (count % 250);
    }

    return 0;
}

namespace {

bool spawn_child_worker(const std::string& dir, const std::string& mode, int count, int* out_exit_code) {
#if defined(_WIN32)
    char buf[32768];
    DWORD len = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    if (len == 0 || len >= sizeof(buf)) {
        return false;
    }
    std::string exe_path(buf, len);
    std::string cmd = "\"" + exe_path + "\" --child \"" + dir + "\" " + mode + " " + std::to_string(count);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');

    BOOL ok = CreateProcessA(
        nullptr,
        cmd_buf.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        nullptr,
        &si,
        &pi
    );
    if (!ok) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (out_exit_code) {
        *out_exit_code = static_cast<int>(exit_code);
    }
    return true;
#else
    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        int ret = run_child_worker(dir.c_str(), mode.c_str(), count);
        ::_exit(ret);
    }
    int status = 0;
    pid_t w = waitpid(pid, &status, 0);
    if (w < 0) {
        return false;
    }
    if (WIFEXITED(status)) {
        if (out_exit_code) {
            *out_exit_code = WEXITSTATUS(status);
        }
        return true;
    }
    return false;
#endif
}

// =========================================================================
// 1. 真实进程强杀: Staging 仅写入未刷盘强杀 → 启动恢复自动补刷落盘 (零丢失)
// =========================================================================

TEST(RealProcessKill, StagingDataRecoveredZeroLoss) {
    if (const char* skip = std::getenv("LOGX_SKIP_FORK_TESTS")) {
        if (std::strcmp(skip, "1") == 0) {
            GTEST_SKIP() << "Skipping fork test as LOGX_SKIP_FORK_TESTS=1";
        }
    }

    Pal& pal = default_pal();
    std::string dir = make_fork_test_dir(pal, "logx_kill_stg");
    std::string st_path = dir + "/staging.mmap";

    int exit_code = -1;
    ASSERT_TRUE(spawn_child_worker(dir, "staging_kill", 30, &exit_code));
    EXPECT_EQ(exit_code, 30);

    // 父进程重启 LogX 引擎: 触发 run_recovery 对账并补刷
    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    uint64_t count = count_exported_records(pal, inst);
    EXPECT_EQ(count, 30u);

    logx_close(inst);
    clean_dir(pal, dir);
}

// =========================================================================
// 2. 真实进程强杀: 磁盘块已落盘但后续写未提交强杀 → 精确对账与单调 seqno 衔接
// =========================================================================

TEST(RealProcessKill, MidBlockWrite_ExactReconciliation) {
    if (const char* skip = std::getenv("LOGX_SKIP_FORK_TESTS")) {
        if (std::strcmp(skip, "1") == 0) {
            GTEST_SKIP() << "Skipping fork test as LOGX_SKIP_FORK_TESTS=1";
        }
    }

    Pal& pal = default_pal();
    std::string dir = make_fork_test_dir(pal, "logx_kill_mid");
    std::string st_path = dir + "/staging.mmap";

    int exit_code = -1;
    // 子进程先 flush 5 条，再写 25 条未刷盘记录并被硬杀
    ASSERT_TRUE(spawn_child_worker(dir, "midblock_kill", 30, &exit_code));
    EXPECT_EQ(exit_code, 30);

    // 父进程重启 LogX 引擎
    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    // 验证 30 条全部恢复
    uint64_t count = count_exported_records(pal, inst);
    EXPECT_EQ(count, 30u);

    // 验证磁盘日志文件各块 seqno 严格单调无断层
    for (const auto& f : pal.file_list(dir.c_str())) {
        if (f.find(".log") != std::string::npos && f.find("staging") == std::string::npos) {
            int fd = pal.file_open(f.c_str(), false, false);
            if (fd >= 0) {
                int64_t sz = pal.file_size(fd);
                int64_t offset = 0;
                uint32_t expected_seq = 1;
                while (offset + 34 <= sz) {
                    uint8_t hdr[34];
                    if (pal.file_pread(fd, hdr, 34, offset) != 34) break;
                    if (get_u32(hdr + 0) != kMagicBlock) break;
                    uint32_t seq = get_u32(hdr + 6);
                    EXPECT_EQ(seq, expected_seq++);
                    uint32_t plen = get_u32(hdr + 14);
                    offset += 34 + plen;
                }
                pal.file_close(fd);
            }
        }
    }

    logx_close(inst);
    clean_dir(pal, dir);
}

// =========================================================================
// 3. 真实进程强杀: 50 轮混合硬杀/优雅退出循环压力测试
// =========================================================================

TEST(RealProcessKill, FiftyCyclesForkLoop) {
    if (const char* skip = std::getenv("LOGX_SKIP_FORK_TESTS")) {
        if (std::strcmp(skip, "1") == 0) {
            GTEST_SKIP() << "Skipping fork test as LOGX_SKIP_FORK_TESTS=1";
        }
    }

    Pal& pal = default_pal();
    std::string base_dir = make_fork_test_dir(pal, "logx_kill_loop");
    pal.file_mkdirs(base_dir.c_str());

    const int kCycles = 50;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        std::string cycle_dir = base_dir + "/cycle_" + std::to_string(cycle);
        std::string st_path = cycle_dir + "/staging.mmap";

        int count = 10 + (cycle % 15);
        const char* mode = "staging_kill";
        if (cycle % 3 == 1) {
            mode = "midblock_kill";
        } else if (cycle % 3 == 2) {
            mode = "graceful";
        }

        int exit_code = -1;
        ASSERT_TRUE(spawn_child_worker(cycle_dir, mode, count, &exit_code));
        EXPECT_EQ(exit_code, count % 250);

        // 父进程打开并恢复
        logx_config_t cfg{};
        cfg.log_dir = cycle_dir.c_str();
        cfg.staging_path = st_path.c_str();
        cfg.max_log_length = 4096;

        logx_instance_t* inst = nullptr;
        ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
        ASSERT_EQ(logx_flush(inst), LOGX_OK);

        uint64_t recovered = count_exported_records(pal, inst);
        EXPECT_EQ(recovered, static_cast<uint64_t>(count))
            << "Mismatch in cycle " << cycle << " mode " << mode;

        logx_close(inst);
        clean_dir(pal, cycle_dir);
    }

    clean_dir(pal, base_dir);
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 5 && std::strcmp(argv[1], "--child") == 0) {
        const char* dir = argv[2];
        const char* mode = argv[3];
        int count = std::atoi(argv[4]);
        return run_child_worker(dir, mode, count);
    }

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
