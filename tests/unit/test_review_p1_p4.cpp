// test_review_p1_p4.cpp - Regression tests for P1-P4 code review issues
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "logx_c.h"
#include "logx_internal.h"
#include "logx_pal.h"
#include "logx_staging.h"

namespace logx {
namespace {

std::string make_temp_dir() {
    std::string p = "test_review_tmp_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    default_pal().file_mkdirs(p.c_str());
    return p;
}

void clean_dir(const std::string& d) {
    auto files = default_pal().file_list(d.c_str());
    for (const auto& f : files) {
        default_pal().file_unlink(f.c_str());
    }
    default_pal().file_unlink(d.c_str());
}

}  // namespace

// 1. P1-1: 满缓冲触发错误回调，回调内调用 logx_close，验证绝不死锁 (Lock reentrancy & write stall fix)
TEST(ReviewFixesTest, P1_1_NoDeadlockWhenCallbackClosesInstance) {
    std::string dir = make_temp_dir();
    std::string staging_file = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging_file.c_str();
    cfg.max_log_length = 64 * 1024;
    cfg.flush_threshold = 64 * 1024;  // 不自动唤醒，便于灌满 staging

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    ASSERT_NE(inst, nullptr);

    struct CbContext {
        logx_instance_t* target_inst = nullptr;
        std::atomic<bool> callback_hit{false};
        std::atomic<bool> close_completed{false};
    } ctx;
    ctx.target_inst = inst;

    logx_set_error_handler(inst, [](void* user, logx_event_kind_e kind, int code, const char* msg) {
        (void)msg;
        auto* c = static_cast<CbContext*>(user);
        if (kind == LOGX_EV_DROP && code == LOGX_ERR_NO_SPACE) {
            c->callback_hit.store(true);
            // 契约允许在回调内执行 close / flush 操作。
            // 若持有 inst->mu 时调用此回调，flush_blocking 重新获取 inst->mu 必发自死锁！
            if (c->target_inst) {
                logx_close(c->target_inst);
                c->target_inst = nullptr;
                c->close_completed.store(true);
            }
        }
    }, &ctx);

    // 构造一条 8KB 的数据反复写入，直到 staging 满
    std::vector<uint8_t> payload(8192, 'A');
    bool full_detected = false;
    for (int i = 0; i < 20; ++i) {
        if (!ctx.target_inst) break;
        logx_status_e st = logx_write(ctx.target_inst, 2, "FILL", payload.data(), static_cast<uint32_t>(payload.size()));
        if (st == LOGX_ERR_NO_SPACE) {
            full_detected = true;
            break;
        }
    }

    EXPECT_TRUE(full_detected || ctx.callback_hit.load());
    if (ctx.callback_hit.load()) {
        EXPECT_TRUE(ctx.close_completed.load());
    } else if (ctx.target_inst) {
        logx_close(ctx.target_inst);
    }

    clean_dir(dir);
}

// 2. P2-3: pad_records 计数器接线验证
TEST(ReviewFixesTest, P2_3_PadRecordsIncrementedOnWrap) {
    std::string dir = make_temp_dir();
    std::string staging_file = dir + "/staging_pad.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging_file.c_str();
    cfg.max_log_length = 64 * 1024;
    cfg.flush_threshold = 0;  // 禁用自动 flush，观察 staging 自身垫 PAD

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    ASSERT_NE(inst, nullptr);

    // 持续写入直到发生回绕并垫 PAD
    // staging 数据区约 128KB - 128B = ~130944B
    std::vector<uint8_t> chunk(16 * 1024, 'P');
    for (int i = 0; i < 7; ++i) {
        logx_write(inst, 2, "PADT", chunk.data(), static_cast<uint32_t>(chunk.size()));
    }

    // 刷盘以推进 flushed_len，使得后续写能回绕到环头
    logx_flush(inst);

    // 再写一条触发回绕
    std::vector<uint8_t> wrap_chunk(20 * 1024, 'W');
    logx_write(inst, 2, "WRAP", wrap_chunk.data(), static_cast<uint32_t>(wrap_chunk.size()));

    logx_stats_t stats{};
    logx_get_stats(inst, &stats);

    // 回绕必然写入 PAD 记录，pad_records 绝不能恒为 0！
    EXPECT_GT(stats.pad_records, 0ULL);

    logx_close(inst);
    clean_dir(dir);
}

// 3. P3-6: staging open 失败时 logx_open 必须安全中止返回错误码，杜绝空指针运行
TEST(ReviewFixesTest, P3_6_RecoveryFailureAbortsOpenSafely) {
    logx_config_t cfg{};
    cfg.log_dir = "/invalid_dir_path_xyz_123";
    // 空路径必然导致 staging open 失败
    cfg.staging_path = "";

    logx_instance_t* inst = nullptr;
    logx_status_e st = logx_open(&cfg, &inst);
    // 必须返回失败，且句柄必须为 null
    EXPECT_NE(st, LOGX_OK);
    EXPECT_EQ(inst, nullptr);
}

}  // namespace logx

