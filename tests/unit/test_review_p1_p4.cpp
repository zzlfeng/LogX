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
#include "logx_format.h"

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


// 4. R13 N-2: decode_one_file 在块损坏(CRC失败/decode失败)时必须清空 carry_slice_buf，
//    杜绝跨越损坏块将此前残留的 is_first/is_mid 碎片与后续独立记录的 is_last 碎片拼接成错乱记录。
TEST(ReviewFixesTest, R13_N2_ExportCorruptedBlockClearsCarrySliceBuf) {
    Pal& pal = default_pal();
    std::string dir = make_temp_dir();
    std::string staging_file = dir + "/staging.mmap";

    // 1) 先打开实例 (避免启动扫描与修复影响我们要构造的特定测试文件)
    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging_file.c_str();
    cfg.max_log_length = 64 * 1024;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    ASSERT_NE(inst, nullptr);

    // 2) 构造历史日期日志文件 (20260920.log)
    uint32_t yyyymmdd = 20260920;
    std::string log_file = dir + "/20260920.log";
    int fd = pal.file_open(log_file.c_str(), true, false);
    ASSERT_GE(fd, 0);

    // 写入 32B 文件头
    uint8_t fhdr[32];
    std::memset(fhdr, 0, sizeof(fhdr));
    put_u32_le(fhdr + 0, kMagicFile);
    fhdr[4] = 1;  // version
    fhdr[5] = 0;  // flags (plaintext)
    put_u32_le(fhdr + 6, yyyymmdd);
    put_u16(fhdr + 10, 0);  // kid
    uint8_t salt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    std::memcpy(fhdr + 12, salt, 8);
    put_u32_le(fhdr + 28, crc32_update(0, fhdr, 28));
    pal.file_pwrite(fd, fhdr, 32, 0);

    // 辅助函数: 构造单个 LGXE slice
    auto make_slice = [](uint8_t flags, const std::string& data) -> std::vector<uint8_t> {
        uint16_t len = static_cast<uint16_t>(data.size());
        uint8_t cksum = 0;
        for (uint8_t b : data) cksum = static_cast<uint8_t>(cksum + b);
        std::vector<uint8_t> buf(kHeaderLen + len);
        put_u16(buf.data() + 0, len);
        put_u16(buf.data() + 2, kMagicEntry);
        buf[4] = flags;
        buf[5] = cksum;
        if (len > 0) std::memcpy(buf.data() + kHeaderLen, data.data(), len);
        return buf;
    };

    // 辅助函数: 写入单个块
    auto write_block = [&](uint64_t offset, uint32_t seqno, const std::vector<uint8_t>& raw_payload, bool corrupt_crc) -> uint64_t {
        uint32_t payload_len = static_cast<uint32_t>(raw_payload.size());
        uint32_t crc = crc32_update(0, raw_payload.data(), payload_len);
        if (corrupt_crc) {
            crc ^= 0xdeadbeef;
        }
        uint8_t bhdr[34];
        std::memset(bhdr, 0, sizeof(bhdr));
        put_u32_le(bhdr + 0, kMagicBlock);
        bhdr[4] = 1;  // version
        bhdr[5] = 0;  // flags (plaintext, uncompressed)
        put_u32_le(bhdr + 6, seqno);
        put_u32_le(bhdr + 10, payload_len);  // raw_len
        put_u32_le(bhdr + 14, payload_len);  // payload_len
        put_u32_le(bhdr + 18, crc);
        std::memcpy(bhdr + 22, salt, 8);
        bhdr[30] = (seqno >> 24) & 0xff;
        bhdr[31] = (seqno >> 16) & 0xff;
        bhdr[32] = (seqno >> 8) & 0xff;
        bhdr[33] = seqno & 0xff;

        pal.file_pwrite(fd, bhdr, 34, offset);
        pal.file_pwrite(fd, raw_payload.data(), payload_len, offset + 34);
        return 34 + payload_len;
    };

    // 块 1: 包含 is_first 碎片 (flags = 0x01)
    std::vector<uint8_t> slice1 = make_slice(0x01, "PARTIAL_HEAD_AAA");
    uint64_t off = 32;
    off += write_block(off, 1, slice1, false);

    // 块 2: CRC 校验损坏块
    std::vector<uint8_t> slice2 = make_slice(0x00, "CORRUPTED_BLOCK_BODY");
    off += write_block(off, 2, slice2, true);

    // 块 3: 包含 is_last 碎片 (flags = 0x03)
    std::vector<uint8_t> slice3 = make_slice(0x03, "PARTIAL_TAIL_BBB");
    off += write_block(off, 3, slice3, false);

    pal.file_close(fd);

    struct ExportResult {
        int cb_calls = 0;
        std::string content;
    } result;

    auto cb = [](void* user, const char* /*date*/, const char* plaintext_path, uint64_t size) -> int {
        auto* r = static_cast<ExportResult*>(user);
        r->cb_calls++;
        int pfd = default_pal().file_open(plaintext_path, false, false);
        if (pfd >= 0) {
            std::vector<char> buf(size);
            if (default_pal().file_pread(pfd, buf.data(), size, 0) == static_cast<long>(size)) {
                r->content.assign(buf.data(), size);
            }
            default_pal().file_close(pfd);
        }
        return 0;
    };

    char date_str[16];
    std::snprintf(date_str, sizeof(date_str), "%u", yyyymmdd);
    EXPECT_EQ(logx_export(inst, date_str, date_str, cb, &result), LOGX_OK);
    EXPECT_EQ(result.cb_calls, 1);
    
    // 必须记录了块 2 的 crc fail 提示
    EXPECT_NE(result.content.find("[seq=2 crc fail]"), std::string::npos);

    // 修复后: 块 1 的 is_first 残片必须在块 2 失败时被 clear，
    // 绝不能与块 3 的 is_last 残片错误拼接成一条 seq=3 的新记录!
    EXPECT_EQ(result.content.find("seq=3"), std::string::npos);

    logx_close(inst);
    clean_dir(dir);
}

}  // namespace logx

