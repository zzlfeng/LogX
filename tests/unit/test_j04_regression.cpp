// test_j04_regression.cpp - J-04 回归测试:错误回调同步直调契约(C ABI 层)。
//
// J-04 崩溃根因:修复前的 nativeWrite 用 GetPrimitiveArrayCritical 包裹 logx_write。
// 当 staging 环形缓冲写满(kFull → LOGX_ERR_NO_SPACE)或单条超长
// (LOGX_ERR_TOO_LARGE)时,logx_write 会在调用方线程同步触发 emit_event →
// (JNI 交付层)jni_error_trampoline → NewString / CallStaticVoidMethod。在 JNI
// 临界区内执行任何 JNI 调用都会命中 ART CheckJNI("using JNI after critical
// get in call to NewString")而致命崩溃。
//
// 本测试无法在桌面端加载 JVM 去复现 JNI 临界区本身,但精确守护 JNI 崩溃所依赖
// 的 C ABI 核心契约(emit_event 直调语义):
//   a) 超长截断(B-1B 方案B)/ 超长病态丢弃 / 环满三条路径必须
//      "同步 + 在调用方线程"触发 err_fn
//      —— 这正是 jni_error_trampoline 能拿到合法 JNI env 的前提;
//   b) 回调必须拿到正确的 kind/code("log truncated..." / "log too large" /
//      "staging full");
//   c) 错误回调执行期间不得死锁,尤其环满路径是在持 inst->mu 前提下触发的
//      (emit_event 用独立 err_mu,锁序 inst->mu → err_mu,见 logx_internal.h);
//   d) 单次事件后实例生命周期不变,后续日志仍可正常写入。
// 若将来有人重新引入 Critical 包裹、把错误事件延迟到后台线程、或改锁序,
// 本测试即回归转红。

#include "logx_c.h"
#include "logx_internal.h"
#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

using namespace logx;

namespace {

static std::atomic<uint64_t> s_j04_dir_seq{0};

// 记录每次错误回调的现场。用锁保护:实例启动后 Flusher 线程可能并发触发
// LOGX_EV_IO/DEGRADED 事件,与写线程的 DROP 事件并存,须按 (kind,code) 精确过滤。
struct ErrorProbe {
    mutable std::mutex mu;
    std::vector<int> kinds;
    std::vector<int> codes;
    std::vector<std::string> msgs;
    std::vector<uint64_t> tids;

    void on_event(logx_event_kind_e kind, int code, const char* msg) {
        std::lock_guard<std::mutex> lk(mu);
        kinds.push_back(static_cast<int>(kind));
        codes.push_back(code);
        msgs.push_back(msg ? msg : "");
        tids.push_back(default_pal().tid());
    }

    bool saw(logx_event_kind_e kind, int code) const {
        std::lock_guard<std::mutex> lk(mu);
        for (size_t i = 0; i < kinds.size(); ++i) {
            if (kinds[i] == static_cast<int>(kind) && codes[i] == code) return true;
        }
        return false;
    }

    // 校验 target (kind,code) 事件可达且发生在指定调用线程(同步契约)。
    bool saw_on_tid(logx_event_kind_e kind, int code, uint64_t tid) const {
        std::lock_guard<std::mutex> lk(mu);
        for (size_t i = 0; i < kinds.size(); ++i) {
            if (kinds[i] == static_cast<int>(kind) && codes[i] == code && tids[i] == tid) {
                return true;
            }
        }
        return false;
    }
};

// 构造最小验证实例:关闭自动刷盘(threshold=0),测试线程自己掌控泄空时机。
logx_config_t make_cfg(const char* dir, const char* staging, uint32_t max_log_len,
                       uint32_t flush_threshold, ErrorProbe* probe,
                       logx_error_fn fn) {
    logx_config_t cfg{};
    cfg.log_dir = dir;
    cfg.staging_path = staging;
    cfg.max_log_length = max_log_len;
    cfg.retention_days = 7;
    cfg.max_file_size = 10 * 1024 * 1024;
    cfg.flush_threshold = flush_threshold;
    cfg.compress_algo = 0;   // kAlgoNone
    cfg.encrypt_enable = 0;
    cfg.err_fn = fn;
    cfg.err_user = probe;
    return cfg;
}

void cleanup_dir(const std::string& dir) {
    Pal& pal = default_pal();
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

}  // namespace

// J-04 a/b/c/d: 超长截断路径(B-1B 方案B)。同步、调用方线程、正确 kind/code、
// 事件后实例可继续写入。截断事件与旧丢日志路径同在调用方线程锁外同步发出,
// JNI 临界区安全性契约不变。
TEST(J04Regression, TooLargeTruncated_ErrHandlerSynchronousOnCallerThread) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_j04_too_large_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_j04_dir_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    ErrorProbe probe;
    constexpr logx_error_fn fn = [](void* user, logx_event_kind_e k, int code, const char* msg) {
        static_cast<ErrorProbe*>(user)->on_event(k, code, msg);
    };

    // max_log_length=256:写一条明显超过的日志确定触发截断事件
    logx_config_t cfg = make_cfg(dir.c_str(), staging.c_str(),
                                 /*max_log_len=*/256, /*flush_threshold=*/0,
                                 &probe, fn);

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    uint64_t caller_tid = pal.tid();
    std::vector<uint8_t> big(4096, 'A');

    // a) 截断写返回 LOGX_OK(记录已落环,非错误)
    EXPECT_EQ(logx_write(inst, 2, "J04", big.data(),
                         static_cast<uint32_t>(big.size())),
              LOGX_OK);

    // b) kind/code 正确,msg 为截断语义
    ASSERT_TRUE(probe.saw(LOGX_EV_DROP, LOGX_ERR_TOO_LARGE));
    // a) 回调在调用方线程触发(同步契约 —— JNI trampoline 靠它拿合法 env)
    EXPECT_TRUE(probe.saw_on_tid(LOGX_EV_DROP, LOGX_ERR_TOO_LARGE, caller_tid));
    {
        std::lock_guard<std::mutex> lk(probe.mu);
        for (size_t i = 0; i < probe.codes.size(); ++i) {
            if (probe.codes[i] == LOGX_ERR_TOO_LARGE) {
                EXPECT_EQ(probe.msgs[i].compare(0, 13, "log truncated"), 0);
            }
        }
    }

    // d) 截断事件后实例仍可写入;truncated_logs 计数佐证
    const uint8_t ok[] = "still alive";
    EXPECT_EQ(logx_write(inst, 2, "J04", ok, sizeof(ok) - 1), LOGX_OK);
    logx_stats_t st{};
    logx_get_stats(inst, &st);
    EXPECT_EQ(st.truncated_logs, 1u);
    EXPECT_EQ(st.drop_too_large, 0u);

    ASSERT_EQ(logx_close(inst), LOGX_OK);
    cleanup_dir(dir);
}

// J-04 a/b/c/d: 病态小 max_log_length(截断到空记录仍放不下)保留的
// TOO_LARGE 丢弃兜底路径。同步、调用方线程、正确 kind/code、事件后实例可继续写入。
TEST(J04Regression, TooLargeCannotFit_ErrHandlerSynchronousOnCallerThread) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_j04_cannot_fit_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_j04_dir_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    ErrorProbe probe;
    constexpr logx_error_fn fn = [](void* user, logx_event_kind_e k, int code, const char* msg) {
        static_cast<ErrorProbe*>(user)->on_event(k, code, msg);
    };

    // tag "J04"(3B) 的最小空记录 = 27+3+6 = 36B;max_log_length=33 连空记录
    // 都放不下 → kCannotFit → TOO_LARGE 丢弃兜底。
    logx_config_t cfg = make_cfg(dir.c_str(), staging.c_str(),
                                 /*max_log_len=*/33, /*flush_threshold=*/0,
                                 &probe, fn);

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    uint64_t caller_tid = pal.tid();
    std::vector<uint8_t> big(4096, 'A');

    // a) 同步 + 返回正确错误码
    EXPECT_EQ(logx_write(inst, 2, "J04", big.data(),
                         static_cast<uint32_t>(big.size())),
              LOGX_ERR_TOO_LARGE);

    // b) kind/code 与丢弃语义 msg 正确
    ASSERT_TRUE(probe.saw(LOGX_EV_DROP, LOGX_ERR_TOO_LARGE));
    // a) 回调在调用方线程触发(同步契约)
    EXPECT_TRUE(probe.saw_on_tid(LOGX_EV_DROP, LOGX_ERR_TOO_LARGE, caller_tid));
    {
        std::lock_guard<std::mutex> lk(probe.mu);
        for (size_t i = 0; i < probe.codes.size(); ++i) {
            if (probe.codes[i] == LOGX_ERR_TOO_LARGE) {
                EXPECT_EQ(probe.msgs[i], "log too large");
            }
        }
    }

    // d) 病态上限下任何记录都放不下:第二条小日志同样 TOO_LARGE(实例存活、
    //    行为一致),计数累计 drop_too_large=2,truncated_logs 恒 0
    const uint8_t ok[] = "still alive";
    EXPECT_EQ(logx_write(inst, 2, "J04", ok, sizeof(ok) - 1), LOGX_ERR_TOO_LARGE);
    logx_stats_t st{};
    logx_get_stats(inst, &st);
    EXPECT_EQ(st.drop_too_large, 2u);
    EXPECT_EQ(st.truncated_logs, 0u);

    ASSERT_EQ(logx_close(inst), LOGX_OK);
    cleanup_dir(dir);
}

// J-04 a/b/c/d: staging 环满(kFull)丢日志路径。此路径 emit_event 在持 inst->mu
// 前提下触发(锁序 inst->mu → err_mu),回访死锁与"环满回调能正常发出"的双重契约。
TEST(J04Regression, StagingFull_ErrHandlerFiresWithoutDeadlockInstanceSurvives) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_j04_nospace_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_j04_dir_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    ErrorProbe probe;
    constexpr logx_error_fn fn = [](void* user, logx_event_kind_e k, int code, const char* msg) {
        static_cast<ErrorProbe*>(user)->on_event(k, code, msg);
    };

    // max_log_length=48KB 足以容纳单条 32KB 记录;flush_threshold=0 关闭自动刷盘,
    // 使测试可在 75ms 轮询窗口内一次性塞满 ~128KB 数据区,确定性命中 kFull。
    logx_config_t cfg = make_cfg(dir.c_str(), staging.c_str(),
                                 /*max_log_len=*/48 * 1024, /*flush_threshold=*/0,
                                 &probe, fn);

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    uint64_t caller_tid = pal.tid();
    std::vector<uint8_t> rec(32 * 1024, 'B');

    // 持续写入至环满:每个记录 ~32KB,~5 条即超出 128KB 数据区;
    // 上限 16 次放量,确保除非实现退化否则必然出现 NO_SPACE。
    bool saw_nospace = false;
    for (int i = 0; i < 16; ++i) {
        if (logx_write(inst, 2, "J04", rec.data(),
                       static_cast<uint32_t>(rec.size())) == LOGX_ERR_NO_SPACE) {
            saw_nospace = true;
            break;
        }
    }
    ASSERT_TRUE(saw_nospace) << "staging 未能被塞满,用例前提不成立";

    // b) kind/code 正确;c) 环满回调在持 inst->mu 前提下未死锁、正常发出
    EXPECT_TRUE(probe.saw(LOGX_EV_DROP, LOGX_ERR_NO_SPACE));
    // a) 回调在调用方线程触发(同步契约)
    EXPECT_TRUE(probe.saw_on_tid(LOGX_EV_DROP, LOGX_ERR_NO_SPACE, caller_tid));

    // d) 显式排空再写入:实例生命周期不受丢弃事件影响
    ASSERT_EQ(logx_flush(inst), LOGX_OK);
    const uint8_t ok[] = "recovered";
    EXPECT_EQ(logx_write(inst, 2, "J04", ok, sizeof(ok) - 1), LOGX_OK);

    // 统计佐证:drop_no_space 计数必须非零
    logx_stats_t st{};
    logx_get_stats(inst, &st);
    EXPECT_GT(st.drop_no_space, 0u);

    ASSERT_EQ(logx_close(inst), LOGX_OK);
    cleanup_dir(dir);
}