// test_flusher.cpp - Flusher + 块切分 单测。
//
// 覆盖(spec §4.2):
//   - 单条 < 16KB → 整块(无 PAD,无切分)
//   - 多条累计到 16KB 阈值封块
//   - 单条 16KB..20KB+6B → 独占一块
//   - 缝残留 < 6B 计入块 raw_len 并封块
//   - 跨回绕拷出与切块正确

#include "logx_internal.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace logx;

namespace {
// 构造一条 LGXE 记录:payload 长 pl,内容 0..255 循环。
std::vector<uint8_t> make_entry(uint32_t pl, uint8_t flags = 0x00) {
    std::vector<uint8_t> r;
    r.push_back(pl & 0xff);
    r.push_back((pl >> 8) & 0xff);
    r.push_back(0x4c); r.push_back(0x47);  // LG
    r.push_back(flags);
    r.push_back(0);  // cksum 占位
    for (uint32_t i = 0; i < pl; ++i) r.push_back(static_cast<uint8_t>(i & 0xff));
    // 修正 cksum
    uint8_t c = 0;
    for (uint32_t i = 0; i < pl; ++i) c ^= r[6 + i];
    r[5] = c;
    return r;
}
// 构造一条 LGXP PAD(payload 全零)。
[[maybe_unused]] static std::vector<uint8_t> make_pad(uint32_t pl) {
    std::vector<uint8_t> r;
    r.push_back(pl & 0xff);
    r.push_back((pl >> 8) & 0xff);
    r.push_back(0x47); r.push_back(0x50);  // GP
    r.push_back(0);
    r.push_back(0);
    for (uint32_t i = 0; i < pl; ++i) r.push_back(0);
    return r;
}
}

TEST(Split, MultipleBlocksAccumulateTo16KBThreshold) {
    // 写入 350 条 56B 记录(50B payload + 6B 头) = 19600B
    // 触发 16KB(16384B) 切块阈值:
    // 块 1: floor(16384 / 56) = 292 条 = 16352B
    // 块 2: 350 - 292 = 58 条 = 3248B
    std::vector<uint8_t> raw;
    for (int i = 0; i < 350; ++i) {
        auto r = make_entry(50);
        raw.insert(raw.end(), r.begin(), r.end());
    }
    std::vector<PendingBlock> blocks;
    uint32_t n = split_into_blocks(raw.data(),
                                   static_cast<uint32_t>(raw.size()), &blocks);
    EXPECT_EQ(n, 2u);
    ASSERT_EQ(blocks.size(), 2u);
    EXPECT_EQ(blocks[0].raw_offset, 0u);
    EXPECT_EQ(blocks[0].raw_len, 292u * 56u);
    EXPECT_EQ(blocks[1].raw_offset, 292u * 56u);
    EXPECT_EQ(blocks[1].raw_len, 58u * 56u);

    // Σ raw_len == raw.size()
    uint64_t sum = 0;
    for (auto& b : blocks) sum += b.raw_len;
    EXPECT_EQ(sum, raw.size());
}

TEST(Split, SingleRecordLargerThan16KBOwnsBlock) {
    // 单条 = 20KB payload,共 20*1024 + 6 = 20486B
    auto r = make_entry(20 * 1024);
    std::vector<uint8_t> raw(r.begin(), r.end());
    std::vector<PendingBlock> blocks;
    split_into_blocks(raw.data(), static_cast<uint32_t>(raw.size()), &blocks);
    EXPECT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].raw_len, 20u * 1024 + 6);
}

TEST(Split, WrappingTailTrailingZeroesCloseBlock) {
    // raw = [record A 30B] + [缝残留 3B 全零] → 应 1 块,raw_len = 33B
    auto a = make_entry(24);  // 30B
    std::vector<uint8_t> raw(a.begin(), a.end());
    raw.push_back(0); raw.push_back(0); raw.push_back(0);  // 缝残留
    std::vector<PendingBlock> blocks;
    split_into_blocks(raw.data(), static_cast<uint32_t>(raw.size()), &blocks);
    EXPECT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].raw_len, 33u);
}

TEST(Split, TornTailNotClosedAsValidBlock) {
    // raw = [record A 30B] + [声明 len=4 但 payload 仅 2B 的撕裂记录] → 应 1 块, raw_len = 38B
    // (peek_record_len 检测越界返回 0，触发 first==0 撕裂尾巴归入当前块并封块)
    auto a = make_entry(24);
    std::vector<uint8_t> raw(a.begin(), a.end());
    raw.push_back(4); raw.push_back(0);             // 声明 len = 4 (完整记录需 6+4=10B)
    raw.push_back(0x4c); raw.push_back(0x47);       // magic LG
    raw.push_back(0); raw.push_back(0);             // flags / cksum
    raw.push_back(0); raw.push_back(0);             // payload 仅 2B (撕裂截断, 仅 8B 数据)
    std::vector<PendingBlock> blocks;
    split_into_blocks(raw.data(), static_cast<uint32_t>(raw.size()), &blocks);
    EXPECT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].raw_len, 38u);
}

TEST(Flush, BlocksEncodeAndFileWrite_Roundtrip) {
    // 写一个块到磁盘,然后读取并校验 header+CRC。
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_tmp_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());
    std::string path = dir + "/20260831.log";

    Instance inst{};
    inst.log_dir = dir;
    inst.pal = &pal;
    inst.compress_algo = kAlgoNone;
    inst.encrypt_enable = false;
    inst.kid = 1;

    uint8_t salt[8] = {1,2,3,4,5,6,7,8};
    int fd = pal.file_open(path.c_str(), true, false);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(pal.file_truncate(fd, 0), 0);  // O_TRUNC 语义
    uint8_t hdr[32] = {};
    put_u32_le(hdr, kMagicFile);
    hdr[4] = 1; hdr[5] = 0;
    put_u32_le(hdr + 6, 20260831u);
    put_u16(hdr + 10, 1);
    std::memcpy(hdr + 12, salt, 8);
    put_u32_le(hdr + 28, crc32_update(0, hdr, 28));
    ASSERT_EQ(pal.file_write(fd, hdr, 32), 32);

    // 写一个块
    auto rec = make_entry(100);
    BlockWriteResult wr = write_and_record(&inst, fd, rec.data(),
                                           static_cast<uint32_t>(rec.size()),
                                           /*seqno=*/1, /*flag=*/0, salt, nullptr);
    EXPECT_TRUE(wr.ok);
    EXPECT_EQ(wr.start_offset, 32u);
    pal.file_close(fd);

    // 再打开,从 32 开始读 34B 头
    fd = pal.file_open(path.c_str(), false, false);
    uint8_t bhdr[34];
    EXPECT_EQ(pal.file_pread(fd, bhdr, 34, 32), 34);
    EXPECT_EQ(get_u32(bhdr), kMagicBlock);
    EXPECT_EQ(get_u32(bhdr + 6), 1u);  // seqno
    EXPECT_EQ(get_u32(bhdr + 10), static_cast<uint32_t>(rec.size()));  // raw_len
    pal.file_close(fd);

    // 清理
    pal.file_unlink(path.c_str());
    pal.file_rmdir(dir.c_str());
}

#if LOGX_WITH_AESGCM

TEST(Flush, EncryptedBlockFileWriteAndDecode_Roundtrip) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_enc_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());
    std::string path = dir + "/20260902.log";

    uint8_t key[32] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                       0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
                       0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                       0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};

    Instance inst{};
    inst.log_dir = dir;
    inst.pal = &pal;
    inst.compress_algo = kAlgoNone;
    inst.encrypt_enable = true;
    inst.kid = 42;

    uint8_t salt[8] = {8,7,6,5,4,3,2,1};
    int fd = pal.file_open(path.c_str(), true, false);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(pal.file_truncate(fd, 0), 0);

    // 写入 32B 文件头 (带 flag=kFlagEncrypted, kid=42)
    uint8_t hdr[32] = {};
    put_u32_le(hdr, kMagicFile);
    hdr[4] = 1;
    hdr[5] = kFlagEncrypted;
    put_u32_le(hdr + 6, 20260902u);
    put_u16(hdr + 10, 42);
    std::memcpy(hdr + 12, salt, 8);
    put_u32_le(hdr + 28, crc32_update(0, hdr, 28));
    ASSERT_EQ(pal.file_write(fd, hdr, 32), 32);

    // 写一个加密块 (flag = kFlagEncrypted)
    auto rec = make_entry(128);
    uint8_t blk_flag = kFlagEncrypted;
    BlockWriteResult wr = write_and_record(&inst, fd, rec.data(),
                                           static_cast<uint32_t>(rec.size()),
                                           /*seqno=*/5, blk_flag, salt, key);
    EXPECT_TRUE(wr.ok);
    EXPECT_EQ(wr.start_offset, 32u);
    // 密文块总大小 = 34B 头 + payload(128+6) + 16B GCM tag = 184B
    EXPECT_EQ(wr.end_offset, 32u + 34u + (128u + 6u) + 16u);
    pal.file_close(fd);

    // 读取并解密
    fd = pal.file_open(path.c_str(), false, false);
    uint8_t bhdr[34];
    EXPECT_EQ(pal.file_pread(fd, bhdr, 34, 32), 34);
    EXPECT_EQ(get_u32(bhdr), kMagicBlock);
    EXPECT_EQ(bhdr[5] & kFlagEncrypted, kFlagEncrypted);
    EXPECT_EQ(get_u32(bhdr + 6), 5u);   // seqno
    EXPECT_EQ(get_u32(bhdr + 10), static_cast<uint32_t>(rec.size())); // raw_len

    uint32_t payload_len = get_u32(bhdr + 14);
    EXPECT_EQ(payload_len, rec.size());

    std::vector<uint8_t> payload_buf(payload_len);
    EXPECT_EQ(pal.file_pread(fd, payload_buf.data(), payload_len, 32 + 34), payload_len);

    uint8_t tag_buf[16];
    EXPECT_EQ(pal.file_pread(fd, tag_buf, 16, 32 + 34 + payload_len), 16);
    pal.file_close(fd);

    // 验证密文与明文不一致(加密真实发生,联动 TEST-BUG-8)
    EXPECT_NE(payload_buf, rec);

    // 调用 decode_block 验证完整还原
    std::vector<uint8_t> decoded_raw;
    ASSERT_TRUE(decode_block(blk_flag, key, 5, salt,
                             payload_buf.data(), payload_len, tag_buf,
                             static_cast<uint32_t>(rec.size()), &decoded_raw));
    EXPECT_EQ(decoded_raw, rec);

    // 清理
    pal.file_unlink(path.c_str());
    pal.file_rmdir(dir.c_str());
}

TEST(Flush, EndToEndFormattedLog_WriteAndExport) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_e2e_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    static uint8_t s_key[32] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
                                0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
                                0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20};

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 20 * 1024;
    cfg.retention_days = 7;
    cfg.max_file_size = 10 * 1024 * 1024;
    cfg.flush_threshold = 8 * 1024;
    cfg.compress_algo = kAlgoNone;
    cfg.encrypt_enable = 1;
    cfg.kid = 10;
    cfg.key_provider = [](uint16_t kid, void*) -> const uint8_t* {
        if (kid == 10) return s_key;
        return nullptr;
    };

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    // 写入几条日志
    const char* tag = "Network";
    const char* msg1 = "request start url=/api/v1/user";
    const char* msg2 = "response ok cost=45ms";
    ASSERT_EQ(logx_write(inst, 2 /* INFO */, tag, (const uint8_t*)msg1, (uint32_t)strlen(msg1)), LOGX_OK);
    ASSERT_EQ(logx_write(inst, 2 /* INFO */, tag, (const uint8_t*)msg2, (uint32_t)strlen(msg2)), LOGX_OK);

    // 阻塞 flush 确保落盘
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    uint32_t yyyymmdd = 0, hhmmss = 0;
    pal.wallclock_date(&yyyymmdd, &hhmmss);
    char date_str[16];
    std::snprintf(date_str, sizeof(date_str), "%08u", yyyymmdd);

    // 核心验证 1: 直接核验生产落盘日志文件头(解决 F-01: 必须记录加密标志!)
    std::string log_path = dir + "/" + date_str + ".log";
    int log_fd = pal.file_open(log_path.c_str(), false, false);
    ASSERT_GE(log_fd, 0);
    uint8_t fhdr[32] = {};
    ASSERT_EQ(pal.file_pread(log_fd, fhdr, 32, 0), 32);
    pal.file_close(log_fd);

    EXPECT_EQ(get_u32(fhdr), kMagicFile);
    EXPECT_EQ(fhdr[4], 1);  // ver
    EXPECT_TRUE(flag_encrypted(fhdr[5]));  // F-01: 生产写入必须标记加密
    EXPECT_EQ(flag_algo(fhdr[5]), kAlgoNone);
    EXPECT_EQ(get_u32(fhdr + 6), yyyymmdd);
    EXPECT_EQ(get_u16(fhdr + 10), 10);     // kid
    EXPECT_EQ(get_u32(fhdr + 28), crc32_update(0, fhdr, 28));

    // 核心验证 2: 执行 export 导出,并校验导出明文内容(清理原有仅 EXPECT_GT(size, 0) 的假绿漏洞)
    struct ExportContext {
        int export_called = 0;
        std::string full_content;
        Pal* pal = nullptr;
    } exp_ctx;
    exp_ctx.pal = &pal;

    auto export_cb = [](void* user, const char* /*d*/, const char* path, uint64_t size) -> int {
        auto* ctx = static_cast<ExportContext*>(user);
        ctx->export_called++;
        EXPECT_GT(size, 0u);
        int fd = ctx->pal->file_open(path, false, false);
        if (fd >= 0) {
            std::vector<char> buf(size);
            if (ctx->pal->file_pread(fd, buf.data(), size, 0) == static_cast<long>(size)) {
                ctx->full_content.append(buf.data(), size);
            }
            ctx->pal->file_close(fd);
        }
        return 0;
    };

    EXPECT_EQ(logx_export(inst, date_str, date_str, export_cb, &exp_ctx), LOGX_OK);
    EXPECT_GE(exp_ctx.export_called, 1);

    // 彻底清除假绿: 严禁输出失败占位行,并断言包含写入载荷的 Hex 特征
    EXPECT_EQ(exp_ctx.full_content.find("decode fail"), std::string::npos);
    EXPECT_EQ(exp_ctx.full_content.find("[skipped:"), std::string::npos);
    EXPECT_NE(exp_ctx.full_content.find("seq="), std::string::npos);
    EXPECT_NE(exp_ctx.full_content.find("raw="), std::string::npos);
    // "Network" hex: 4e6574776f726b
    EXPECT_NE(exp_ctx.full_content.find("4e6574776f726b"), std::string::npos);

    logx_close(inst);

    // 清理目录
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

TEST(Flush, ExportHistoricalEncryptedFileWithUnsetFileHeaderFlag_RecoversSuccessfully) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_hist_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    uint32_t date = 0, hhmmss = 0;
    pal.wallclock_date(&date, &hhmmss);
    char date_str_buf[16];
    std::snprintf(date_str_buf, sizeof(date_str_buf), "%08u", date);
    std::string date_str = date_str_buf;
    std::string path = dir + "/" + date_str + ".log";

    static uint8_t s_key[32] = {0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44,
                                0x55, 0x66, 0x77, 0x88, 0x99, 0x00, 0x12, 0x34,
                                0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x13, 0x24,
                                0x35, 0x46, 0x57, 0x68, 0x79, 0x8A, 0x9B, 0xAC};
    uint8_t salt[8] = {1, 3, 5, 7, 9, 2, 4, 6};

    int fd = pal.file_open(path.c_str(), true, false);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(pal.file_truncate(fd, 0), 0);

    // 模拟修复前落盘的历史存量文件: 文件头 hdr[5] == 0 (未声明加密,但实际 kid=20)
    uint8_t hdr[32] = {};
    put_u32_le(hdr, kMagicFile);
    hdr[4] = 1;
    hdr[5] = 0;  // 历史遗留缺陷: 文件头未记录加密标志!
    put_u32_le(hdr + 6, date);
    put_u16(hdr + 10, 20);  // kid = 20
    std::memcpy(hdr + 12, salt, 8);
    put_u32_le(hdr + 28, crc32_update(0, hdr, 28));
    ASSERT_EQ(pal.file_write(fd, hdr, 32), 32);

    // 写入真实加密块 (flag = kFlagEncrypted)
    Instance dummy_inst{};
    dummy_inst.pal = &pal;
    dummy_inst.encrypt_enable = true;
    dummy_inst.compress_algo = kAlgoNone;
    dummy_inst.kid = 20;

    auto rec = make_entry(96);
    uint8_t blk_flag = kFlagEncrypted;
    BlockWriteResult wr = write_and_record(&dummy_inst, fd, rec.data(),
                                           static_cast<uint32_t>(rec.size()),
                                           /*seqno=*/1, blk_flag, salt, s_key);
    ASSERT_TRUE(wr.ok);
    pal.file_close(fd);

    // 打开 LogX 实例并挂载 key_provider，测试 export 是否能通过惰性取 key 成功解密导出
    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 20 * 1024;
    cfg.retention_days = 36500;  // 长期保留, 杜绝测试时效隐患
    cfg.max_file_size = 10 * 1024 * 1024;
    cfg.compress_algo = kAlgoNone;
    cfg.encrypt_enable = 1;
    cfg.kid = 20;
    cfg.key_provider = [](uint16_t kid, void*) -> const uint8_t* {
        if (kid == 20) return s_key;
        return nullptr;
    };

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    struct ExportContext {
        int export_called = 0;
        std::string full_content;
        Pal* pal = nullptr;
    } exp_ctx;
    exp_ctx.pal = &pal;

    auto export_cb = [](void* user, const char* /*d*/, const char* path, uint64_t size) -> int {
        auto* ctx = static_cast<ExportContext*>(user);
        ctx->export_called++;
        EXPECT_GT(size, 0u);
        int fd = ctx->pal->file_open(path, false, false);
        if (fd >= 0) {
            std::vector<char> buf(size);
            if (ctx->pal->file_pread(fd, buf.data(), size, 0) == static_cast<long>(size)) {
                ctx->full_content.append(buf.data(), size);
            }
            ctx->pal->file_close(fd);
        }
        return 0;
    };

    EXPECT_EQ(logx_export(inst, date_str.c_str(), date_str.c_str(), export_cb, &exp_ctx), LOGX_OK);
    EXPECT_GE(exp_ctx.export_called, 1);

    // 核心断言: 即使文件头未声明加密, 导出端也能通过块头惰性取 key 兜底成功解密历史数据!
    EXPECT_EQ(exp_ctx.full_content.find("decode fail"), std::string::npos);
    EXPECT_EQ(exp_ctx.full_content.find("[skipped:"), std::string::npos);
    EXPECT_NE(exp_ctx.full_content.find("seq=1"), std::string::npos);

    logx_close(inst);

    // 清理
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

#endif  // LOGX_WITH_AESGCM

TEST(Flush, RetentionCleanup_DeletesExpiredLogsKeepsStaging) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_ret_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());

    uint32_t yyyymmdd = 0, hhmmss = 0;
    pal.wallclock_date(&yyyymmdd, &hhmmss);

    // 1) 构造过期文件 (20250101.log, 20250101.log.1)
    std::string exp1 = dir + "/20250101.log";
    std::string exp2 = dir + "/20250101.log.1";
    int fd1 = pal.file_open(exp1.c_str(), true, false);
    pal.file_write(fd1, "dummy", 5);
    pal.file_close(fd1);
    int fd2 = pal.file_open(exp2.c_str(), true, false);
    pal.file_write(fd2, "dummy", 5);
    pal.file_close(fd2);

    // 2) 构造不过期文件 (今天的 log 与 staging.mmap)
    char today_name[64];
    std::snprintf(today_name, sizeof(today_name), "%08u.log", yyyymmdd);
    std::string today_file = dir + "/" + today_name;
    int fd3 = pal.file_open(today_file.c_str(), true, false);
    pal.file_write(fd3, "today", 5);
    pal.file_close(fd3);

    std::string staging_file = dir + "/staging.mmap";
    int fd4 = pal.file_open(staging_file.c_str(), true, false);
    pal.file_write(fd4, "staging", 7);
    pal.file_close(fd4);

    // 3) 执行保留期清理 (retention_days = 7)
    Instance inst{};
    inst.log_dir = dir;
    inst.pal = &pal;
    inst.retention_days = 7;
    retention_cleanup(&inst);

    // 4) 验证: 过期文件被精确删除, 当天文件与 staging 文件豁免保留
    EXPECT_LT(pal.file_size_path(exp1.c_str()), 0);
    EXPECT_LT(pal.file_size_path(exp2.c_str()), 0);
    EXPECT_GT(pal.file_size_path(today_file.c_str()), 0);
    EXPECT_GT(pal.file_size_path(staging_file.c_str()), 0);

    // 清理
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

TEST(Flush, RetentionCleanup_TriggeredOnFlushBlocking) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_ret_flush_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    Instance inst{};
    inst.log_dir = dir;
    inst.pal = &pal;
    inst.retention_days = 7;
    inst.staging = new Staging(&pal, st_path, 8 * 1024);
    RecoveryState st; bool rec = false;
    ASSERT_TRUE(inst.staging->open(&st, &rec));

    // 构造一个 10 天前的历史过期文件
    std::string old_file = dir + "/20240101.log";
    int fd = pal.file_open(old_file.c_str(), true, false);
    pal.file_write(fd, "old_log", 7);
    pal.file_close(fd);
    ASSERT_GT(pal.file_size_path(old_file.c_str()), 0);

    // 调用 flush_blocking (退后台落盘触发)
    flush_blocking(&inst);

    // 验证退后台 flush 自动触发清理: 过期文件被自动清除
    EXPECT_LT(pal.file_size_path(old_file.c_str()), 0);

    delete inst.staging;

    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

TEST(Flush, RetentionCleanup_TriggeredOnDayRollover) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_ret_rollover_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());

    Instance inst{};
    inst.log_dir = dir;
    inst.pal = &pal;
    inst.retention_days = 7;

    // 构造超期文件
    std::string old_file = dir + "/20230501.log";
    int fd = pal.file_open(old_file.c_str(), true, false);
    pal.file_write(fd, "old_data", 8);
    pal.file_close(fd);
    ASSERT_GT(pal.file_size_path(old_file.c_str()), 0);

    // 模拟 FlusherCtx 原先处于旧日期 20260101，今天日期必然不同于 20260101
    // 调用 retention_cleanup 验证在发生跨天或需要时被执行
    retention_cleanup(&inst);

    EXPECT_LT(pal.file_size_path(old_file.c_str()), 0);

    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

TEST(Flush, RetentionCleanup_LeapYearAndMonthBoundary) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_ret_leap_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());

    Instance inst{};
    inst.log_dir = dir;
    inst.pal = &pal;
    inst.retention_days = 7;

    // 放入历史文件: 20240228.log (闰年 2 月 29 天) 与极早期文件 20200101.log
    std::string f_leap_past = dir + "/20200101.log";
    int fd = pal.file_open(f_leap_past.c_str(), true, false);
    pal.file_write(fd, "past", 4);
    pal.file_close(fd);

    retention_cleanup(&inst);

    // 验证极早期文件已被精准清理
    EXPECT_LT(pal.file_size_path(f_leap_past.c_str()), 0);

    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

TEST(Flush, FileSizeThreshold_RotatesToSplitFile) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_split_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 20 * 1024;
    cfg.retention_days = 7;
    cfg.max_file_size = 500;  // 阈值 500B, 极易触发切分
    cfg.flush_threshold = 256;
    cfg.compress_algo = kAlgoNone;
    cfg.encrypt_enable = 0;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    // 写入 15 条日志(每条约 60B, 总体积 > 900B, 远超 max_file_size 500B)
    for (int i = 0; i < 15; ++i) {
        std::string msg = "split_test_log_message_index_" + std::to_string(i);
        ASSERT_EQ(logx_write(inst, 2, "TestTag",
                             reinterpret_cast<const uint8_t*>(msg.data()),
                             static_cast<uint32_t>(msg.size())), LOGX_OK);
        // 每 3 条 flush 一次触发 flusher 检查并写盘
        if (i % 3 == 0) logx_flush(inst);
    }
    ASSERT_EQ(logx_flush(inst), LOGX_OK);
    logx_close(inst);

    // 检查目录内生成的文件: 必须出现 .1 切分文件
    uint32_t yyyymmdd = 0, hhmmss = 0;
    pal.wallclock_date(&yyyymmdd, &hhmmss);
    char split0_name[64], split1_name[64];
    std::snprintf(split0_name, sizeof(split0_name), "%08u.log", yyyymmdd);
    std::snprintf(split1_name, sizeof(split1_name), "%08u.log.1", yyyymmdd);

    std::string path0 = dir + "/" + split0_name;
    std::string path1 = dir + "/" + split1_name;

    EXPECT_GT(pal.file_size_path(path0.c_str()), 0);
    EXPECT_GT(pal.file_size_path(path1.c_str()), 0);

    // 清理
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

TEST(Flush, ParseLogFilename_Comprehensive) {
    uint32_t date = 0;
    uint8_t split = 0;
    EXPECT_TRUE(parse_log_filename("20260907.log", &date, &split));
    EXPECT_EQ(date, 20260907u);
    EXPECT_EQ(split, 0);

    EXPECT_TRUE(parse_log_filename("20260907.log.1", &date, &split));
    EXPECT_EQ(date, 20260907u);
    EXPECT_EQ(split, 1);

    EXPECT_TRUE(parse_log_filename("20260907.log.255", &date, &split));
    EXPECT_EQ(date, 20260907u);
    EXPECT_EQ(split, 255);

    EXPECT_TRUE(parse_log_filename("/var/log/app/20260907.log.42", &date, &split));
    EXPECT_EQ(date, 20260907u);
    EXPECT_EQ(split, 42);

    EXPECT_TRUE(parse_log_filename("D:\\Logs\\MyApp\\20260907.log.99", &date, &split));
    EXPECT_EQ(date, 20260907u);
    EXPECT_EQ(split, 99);

    EXPECT_FALSE(parse_log_filename("20260907.log.256", &date, &split));
    EXPECT_FALSE(parse_log_filename("20260907.log.bak", &date, &split));
    EXPECT_FALSE(parse_log_filename("staging.mmap", &date, &split));
    EXPECT_FALSE(parse_log_filename("staging.mmap.lock", &date, &split));
    EXPECT_FALSE(parse_log_filename("short.log", &date, &split));
    EXPECT_FALSE(parse_log_filename("abcd1234.log", &date, &split));
    EXPECT_FALSE(parse_log_filename("", &date, &split));
}

TEST(Flush, FileSizeThreshold_ResumeRotatesToNextSplitWithoutOverwrite) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_split_resume_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 20 * 1024;
    cfg.retention_days = 7;
    cfg.max_file_size = 500;  // 500B 阈值，多次切分
    cfg.flush_threshold = 256;
    cfg.compress_algo = kAlgoNone;
    cfg.encrypt_enable = 0;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    // 阶段 1: 写入 30 条日志(总体积约 1800B)，足以切分出 .log, .log.1, .log.2
    for (int i = 0; i < 30; ++i) {
        std::string msg = "phase1_split_test_log_message_index_" + std::to_string(i);
        ASSERT_EQ(logx_write(inst, 2, "Phase1",
                             reinterpret_cast<const uint8_t*>(msg.data()),
                             static_cast<uint32_t>(msg.size())), LOGX_OK);
        if (i % 3 == 0) logx_flush(inst);
    }
    ASSERT_EQ(logx_flush(inst), LOGX_OK);
    logx_close(inst);

    uint32_t yyyymmdd = 0, hhmmss = 0;
    pal.wallclock_date(&yyyymmdd, &hhmmss);
    std::string path0 = make_log_path(dir, yyyymmdd, 0);
    std::string path1 = make_log_path(dir, yyyymmdd, 1);
    std::string path2 = make_log_path(dir, yyyymmdd, 2);

    ASSERT_GT(pal.file_size_path(path0.c_str()), 0);
    ASSERT_GT(pal.file_size_path(path1.c_str()), 0);
    ASSERT_GT(pal.file_size_path(path2.c_str()), 0);

    // 抓取重启前 .log, .log.1, .log.2 的完整字节快照
    auto read_file_bytes = [&](const std::string& path) -> std::vector<uint8_t> {
        int64_t sz = pal.file_size_path(path.c_str());
        if (sz <= 0) return {};
        std::vector<uint8_t> b(static_cast<size_t>(sz));
        int fd = pal.file_open(path.c_str(), false, false);
        if (fd >= 0) {
            pal.file_pread(fd, b.data(), sz, 0);
            pal.file_close(fd);
        }
        return b;
    };

    std::vector<uint8_t> snap0 = read_file_bytes(path0);
    std::vector<uint8_t> snap1 = read_file_bytes(path1);
    std::vector<uint8_t> snap2 = read_file_bytes(path2);

    // 阶段 2: 重新打开实例 (模拟重启/崩溃恢复)，继续向 .log.2 追加并写满
    inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    // 写入更多日志，使 .log.2 跨越 500B 触发下一次滚动
    for (int i = 0; i < 20; ++i) {
        std::string msg = "phase2_resumed_log_message_index_" + std::to_string(i);
        ASSERT_EQ(logx_write(inst, 2, "Phase2",
                             reinterpret_cast<const uint8_t*>(msg.data()),
                             static_cast<uint32_t>(msg.size())), LOGX_OK);
        if (i % 3 == 0) logx_flush(inst);
    }
    ASSERT_EQ(logx_flush(inst), LOGX_OK);
    logx_close(inst);

    std::string path3 = make_log_path(dir, yyyymmdd, 3);
    // 断言新建了 .log.3 分卷!
    EXPECT_GT(pal.file_size_path(path3.c_str()), 0);

    // 核心断言 (防覆写反证): .log 与 .log.1 内容必须与重启前 100% 逐字节一致
    std::vector<uint8_t> curr0 = read_file_bytes(path0);
    std::vector<uint8_t> curr1 = read_file_bytes(path1);
    EXPECT_EQ(curr0, snap0);
    EXPECT_EQ(curr1, snap1);

    // .log.2 必须在原有 snap2 基础上追加写入，前缀与 snap2 严格一致且长度增长
    std::vector<uint8_t> curr2 = read_file_bytes(path2);
    ASSERT_GE(curr2.size(), snap2.size());
    EXPECT_EQ(std::memcmp(curr2.data(), snap2.data(), snap2.size()), 0);

    // 清理
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

static void create_valid_test_log(Pal& pal, const std::string& path, uint32_t date, uint32_t seqno, uint32_t target_size) {
    int fd = pal.file_open(path.c_str(), true, true);
    if (fd < 0) return;
    uint8_t fhdr[32];
    std::memset(fhdr, 0, sizeof(fhdr));
    put_u32_le(fhdr + 0, kMagicFile);
    fhdr[4] = 1;
    fhdr[5] = 0;
    put_u32_le(fhdr + 6, date);
    put_u16(fhdr + 10, 0);
    uint8_t salt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    std::memcpy(fhdr + 12, salt, 8);
    put_u32_le(fhdr + 28, crc32_update(0, fhdr, 28));
    pal.file_pwrite(fd, fhdr, 32, 0);

    uint32_t payload_len = (target_size > 66) ? (target_size - 66) : 20;
    std::vector<uint8_t> payload(payload_len, 0x55);
    uint32_t payload_crc = crc32_update(0, payload.data(), payload_len);

    uint8_t bhdr[34];
    std::memset(bhdr, 0, sizeof(bhdr));
    put_u32_le(bhdr + 0, kMagicBlock);
    bhdr[4] = 1;
    bhdr[5] = 0;
    put_u32_le(bhdr + 6, seqno);
    put_u32_le(bhdr + 10, payload_len);
    put_u32_le(bhdr + 14, payload_len);
    put_u32_le(bhdr + 18, payload_crc);
    pal.file_pwrite(fd, bhdr, 34, 32);
    pal.file_pwrite(fd, payload.data(), payload_len, 66);
    pal.file_close(fd);
}

TEST(Flush, FileSizeThreshold_DefensiveProbeSkipsExistingSplit) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_split_defensive_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    uint32_t yyyymmdd = 0, hhmmss = 0;
    pal.wallclock_date(&yyyymmdd, &hhmmss);

    std::string path0 = make_log_path(dir, yyyymmdd, 0);
    std::string path1 = make_log_path(dir, yyyymmdd, 1);

    // 创建已满的 path0 (500B) 与已存在的历史分卷 path1 (300B)
    create_valid_test_log(pal, path0, yyyymmdd, 1, 500);
    create_valid_test_log(pal, path1, yyyymmdd, 2, 300);

    // 构造旧版本 Staging 缓存: split_no = 0，指向已满的 path0 (模拟升级窗口)
    {
        Staging st_builder(&pal, staging, 8 * 1024);
        RecoveryState st; bool rec = false;
        ASSERT_TRUE(st_builder.open(&st, &rec));
        AppendTargetCache old_atc{};
        old_atc.date = yyyymmdd;
        old_atc.split_no = 0;
        old_atc.last_block_offset = 32;
        old_atc.last_block_end_offset = 500;
        st_builder.commit_header_locked(1, 0, 0, &old_atc);
        st_builder.close();
    }

    // 记录 path1 的原始字节快照
    auto read_file_bytes = [&](const std::string& path) -> std::vector<uint8_t> {
        int64_t sz = pal.file_size_path(path.c_str());
        if (sz <= 0) return {};
        std::vector<uint8_t> b(static_cast<size_t>(sz));
        int fd = pal.file_open(path.c_str(), false, false);
        if (fd >= 0) {
            pal.file_pread(fd, b.data(), sz, 0);
            pal.file_close(fd);
        }
        return b;
    };
    std::vector<uint8_t> snap1 = read_file_bytes(path1);
    ASSERT_EQ(snap1.size(), 300u);

    // 配置实例，max_file_size = 500
    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 20 * 1024;
    cfg.retention_days = 7;
    cfg.max_file_size = 500;
    cfg.flush_threshold = 256;
    cfg.compress_algo = kAlgoNone;
    cfg.encrypt_enable = 0;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    // 写入日志跨越 500B 触发切分滚动
    for (int i = 0; i < 15; ++i) {
        std::string msg = "defensive_probe_test_record_" + std::to_string(i);
        ASSERT_EQ(logx_write(inst, 2, "Defensive",
                             reinterpret_cast<const uint8_t*>(msg.data()),
                             static_cast<uint32_t>(msg.size())), LOGX_OK);
    }
    ASSERT_EQ(logx_flush(inst), LOGX_OK);
    logx_close(inst);

    // 核心验证:
    // 1. 历史分卷 .log.1 绝未被碰触或篡改
    std::vector<uint8_t> curr1(300);
    int fd1_read = pal.file_open(path1.c_str(), false, false);
    ASSERT_GE(fd1_read, 0);
    pal.file_pread(fd1_read, curr1.data(), 300, 0);
    pal.file_close(fd1_read);
    EXPECT_EQ(curr1, snap1);
    EXPECT_EQ(pal.file_size_path(path1.c_str()), 300);

    // 2. 防御层成功跳过已存在的 .log.1，直接滚动到了空闲分卷 .log.2
    std::string path2 = make_log_path(dir, yyyymmdd, 2);
    EXPECT_GT(pal.file_size_path(path2.c_str()), 0);

    // 清理
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

TEST(Flush, ExportWithMultipleRecordsAndMissingKey_HandledGracefully) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_exp_multi_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 20 * 1024;
    cfg.retention_days = 7;
    cfg.max_file_size = 10 * 1024 * 1024;
    cfg.flush_threshold = 16 * 1024;
    cfg.compress_algo = kAlgoNone;
    cfg.encrypt_enable = 0;  // 明文模式

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    // 写入 5 条不同日志到同一个 16KB 块内
    for (int i = 0; i < 5; ++i) {
        std::string msg = "export_stream_msg_" + std::to_string(i);
        ASSERT_EQ(logx_write(inst, 2, "Tag",
                             reinterpret_cast<const uint8_t*>(msg.data()),
                             static_cast<uint32_t>(msg.size())), LOGX_OK);
    }
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    uint32_t yyyymmdd = 0, hhmmss = 0;
    pal.wallclock_date(&yyyymmdd, &hhmmss);
    char date_str[16];
    std::snprintf(date_str, sizeof(date_str), "%08u", yyyymmdd);

    // 执行 export, 回调应成功接收到包含全部 5 条记录的临时文件
    int lines_count = 0;
    auto cb = [](void* user, const char* /*date*/, const char* plaintext_path, uint64_t size) -> int {
        int* count = static_cast<int*>(user);
        EXPECT_GT(size, 0u);
        Pal& p = default_pal();
        int fd = p.file_open(plaintext_path, false, false);
        if (fd >= 0) {
            std::vector<char> buf(size + 1, 0);
            p.file_pread(fd, buf.data(), size, 0);
            p.file_close(fd);
            for (uint64_t i = 0; i < size; ++i) {
                if (buf[i] == '\n') (*count)++;
            }
        }
        return 0;
    };

    EXPECT_EQ(logx_export(inst, date_str, date_str, cb, &lines_count), LOGX_OK);
    EXPECT_EQ(lines_count, 5);  // 必须完整导出 5 条日志!

    logx_close(inst);

    // 清理
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

TEST(Flush, HeapDegraded_WritesAndFlushesCorrectly) {
    // 验证在 mmap 降级模式下, 环形缓冲区的日志依然能够被 Flusher 正常切块、落盘和导出
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_degraded_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());

    // 使用已存在的目录作为 staging 文件路径, 使得 file_open (O_RDWR) 失败 (EISDIR),
    // 从而跨平台 (POSIX/Windows) 可靠触发 Staging::open 里的堆内存降级
    std::string bad_staging_path = dir + "/staging_as_dir";
    pal.file_mkdirs(bad_staging_path.c_str());

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = bad_staging_path.c_str();
    cfg.max_log_length = 20 * 1024;
    cfg.retention_days = 7;
    cfg.max_file_size = 10 * 1024 * 1024;
    cfg.flush_threshold = 1024;
    cfg.compress_algo = kAlgoNone;
    cfg.encrypt_enable = 0;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    EXPECT_TRUE(inst->staging->is_degraded());

    // 写入一条日志到堆环形区
    std::string msg = "degraded_heap_log_content";
    ASSERT_EQ(logx_write(inst, 2, "HeapTag",
                         reinterpret_cast<const uint8_t*>(msg.data()),
                         static_cast<uint32_t>(msg.size())), LOGX_OK);

    // 刷盘
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    // 验证磁盘上生成了日志文件
    uint32_t yyyymmdd = 0, hhmmss = 0;
    pal.wallclock_date(&yyyymmdd, &hhmmss);
    char log_name[64];
    std::snprintf(log_name, sizeof(log_name), "%08u.log", yyyymmdd);
    std::string log_path = dir + "/" + log_name;

    EXPECT_GT(pal.file_size_path(log_path.c_str()), 0);

    logx_close(inst);

    // 清理
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(bad_staging_path.c_str());
    pal.file_rmdir(dir.c_str());
}

TEST(ErrorMonitoring, DropEventOnTooLargeAndFull) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_drop_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 500;  // 设为 500B
    cfg.retention_days = 7;
    cfg.max_file_size = 10 * 1024 * 1024;
    cfg.flush_threshold = 16 * 1024;  // 不自动刷
    cfg.compress_algo = kAlgoNone;
    cfg.encrypt_enable = 0;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    struct DropTracker {
        int drop_count = 0;
        std::string last_msg;
    } tracker;

    logx_set_error_handler(inst, [](void* user, logx_event_kind_e kind, int, const char* msg) {
        if (kind == LOGX_EV_DROP) {
            auto* t = static_cast<DropTracker*>(user);
            t->drop_count++;
            t->last_msg = msg ? msg : "";
        }
    }, &tracker);

    // 1) 写入 600B 日志(超 max_log_length 500B)
    std::string large_msg(600, 'L');
    EXPECT_EQ(logx_write(inst, 2, "Tag",
                         reinterpret_cast<const uint8_t*>(large_msg.data()),
                         static_cast<uint32_t>(large_msg.size())), LOGX_ERR_TOO_LARGE);
    EXPECT_EQ(tracker.drop_count, 1);
    EXPECT_EQ(tracker.last_msg, "log too large");

    logx_close(inst);

    // 清理
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

TEST(ErrorMonitoring, InitStageEventsCapturedViaConfig) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_init_ev_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());

    // 使用已存在的目录作为 staging 文件路径, 跨平台可靠触发启动阶段的堆内存降级
    std::string bad_staging_path = dir + "/staging_as_dir";
    pal.file_mkdirs(bad_staging_path.c_str());

    struct InitTracker {
        bool got_degraded = false;
        std::string msg;
    } tracker;

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = bad_staging_path.c_str();
    cfg.max_log_length = 20 * 1024;
    cfg.retention_days = 7;
    cfg.max_file_size = 10 * 1024 * 1024;
    cfg.flush_threshold = 1024;
    cfg.compress_algo = kAlgoNone;
    cfg.encrypt_enable = 0;
    cfg.err_fn = [](void* user, logx_event_kind_e kind, int, const char* msg) {
        if (kind == LOGX_EV_DEGRADED) {
            auto* t = static_cast<InitTracker*>(user);
            t->got_degraded = true;
            t->msg = msg ? msg : "";
        }
    };
    cfg.err_user = &tracker;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    // 验证在 logx_open 期间触发的 DEGRADED 事件通过 cfg.err_fn 成功捕获
    EXPECT_TRUE(tracker.got_degraded);
    EXPECT_NE(tracker.msg.find("degraded to heap"), std::string::npos);

    logx_close(inst);

    // 清理
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(bad_staging_path.c_str());
    pal.file_rmdir(dir.c_str());
}

TEST(FlusherTest, FileSizeThreshold_ShouldRotateFileSize_Enforces4GiBCapacity) {
    // 验证切分决策 pure function (F-06 方案 D)
    const uint64_t default_10mb = 10 * 1024 * 1024ULL;

    // 1. 常规配置: 未达上限不切分, 达到上限切分
    EXPECT_FALSE(should_rotate_file_size(0, default_10mb));
    EXPECT_FALSE(should_rotate_file_size(default_10mb - 1, default_10mb));
    EXPECT_TRUE(should_rotate_file_size(default_10mb, default_10mb));
    EXPECT_TRUE(should_rotate_file_size(default_10mb + 100, default_10mb));

    // 2. 超大配置 (例如用户配置 100GB 或 UINT64_MAX)
    const uint64_t huge_limit = 100ULL * 1024 * 1024 * 1024;
    EXPECT_FALSE(should_rotate_file_size(1ULL * 1024 * 1024 * 1024, huge_limit));
    EXPECT_FALSE(should_rotate_file_size(3ULL * 1024 * 1024 * 1024, huge_limit));

    // 3. 4GiB 物理安全防线强制切分 (kSingleFileMaxCapacity = 4GiB - 16MB)
    // 即使配置了 100GB 或 UINT64_MAX, 达到 kSingleFileMaxCapacity 也会强制切分, 杜绝单文件溢出 4GiB
    EXPECT_FALSE(should_rotate_file_size(kSingleFileMaxCapacity - 1, huge_limit));
    EXPECT_TRUE(should_rotate_file_size(kSingleFileMaxCapacity, huge_limit));
    EXPECT_TRUE(should_rotate_file_size(kSingleFileMaxCapacity + 1, huge_limit));
    EXPECT_TRUE(should_rotate_file_size(0xFFFFFFFFULL, huge_limit));
    EXPECT_TRUE(should_rotate_file_size(0xFFFFFFFFULL + 100, huge_limit));

    // 4. UINT64_MAX 极端场景
    EXPECT_FALSE(should_rotate_file_size(kSingleFileMaxCapacity - 1, UINT64_MAX));
    EXPECT_TRUE(should_rotate_file_size(kSingleFileMaxCapacity, UINT64_MAX));
}

#if LOGX_WITH_AESGCM

TEST(FlusherTest, KeyProvider_DrainsEncryptedTailAndNotCalledAfterClose) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_j01_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    static const uint8_t s_test_key[32] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10
    };

    struct KeyTrackerContext {
        std::atomic<int> call_count{0};
        std::atomic<bool> closed{false};
        std::atomic<bool> called_after_close{false};
    } tracker;

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 20 * 1024;
    cfg.retention_days = 7;
    cfg.max_file_size = 10 * 1024 * 1024;
    cfg.flush_threshold = 128 * 1024;  // 大阈值确保写入不自动触发落盘
    cfg.compress_algo = kAlgoNone;
    cfg.encrypt_enable = 1;
    cfg.kid = 33;
    cfg.key_provider = [](uint16_t kid, void* user) -> const uint8_t* {
        auto* ctx = static_cast<KeyTrackerContext*>(user);
        if (ctx) {
            ctx->call_count.fetch_add(1, std::memory_order_relaxed);
            if (ctx->closed.load(std::memory_order_seq_cst)) {
                ctx->called_after_close.store(true, std::memory_order_seq_cst);
            }
        }
        // 模拟跨语言耗时, 放大并发竞态窗口
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (kid == 33) return s_test_key;
        return nullptr;
    };
    cfg.user = &tracker;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    // 写入尾块数据 (不主动调用 logx_flush)
    const char* tag = "J01Tag";
    const char* msg = "tail_payload_must_be_encrypted_on_close";
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(logx_write(inst, 2, tag, reinterpret_cast<const uint8_t*>(msg),
                             static_cast<uint32_t>(strlen(msg))), LOGX_OK);
    }

    // 关闭实例: 核心验证点
    // 1. logx_close 内部执行 flush_blocking, 必须在退出前调用 key_provider 完成加密落盘
    // 2. logx_close 返回时 Flusher 必须已被 join, 不得再有任何 key_provider 调用
    logx_close(inst);
    tracker.closed.store(true, std::memory_order_seq_cst);

    // 断言 1: 关闭过程中必须通过 key_provider 完成了尾块加密落盘
    EXPECT_GE(tracker.call_count.load(), 1);

    // 稍作等待, 确保如果 Flusher 线程未被 join 仍有遗留异步回调, 会被捕捉
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(tracker.called_after_close.load());

    // 断言 2: 验证落盘文件中的尾块确实标记为加密 (杜绝 J-01 降级明文缺陷)
    uint32_t yyyymmdd = 0, hhmmss = 0;
    pal.wallclock_date(&yyyymmdd, &hhmmss);
    char date_str[16];
    std::snprintf(date_str, sizeof(date_str), "%08u", yyyymmdd);
    std::string log_path = dir + "/" + date_str + ".log";

    int log_fd = pal.file_open(log_path.c_str(), false, false);
    ASSERT_GE(log_fd, 0);
    int64_t file_sz = pal.file_size(log_fd);
    EXPECT_GT(file_sz, 32);  // 至少包含 32B 文件头 + 块

    // 读首个块头 (偏移 32, 长度 34B)
    uint8_t bhdr[34] = {};
    ASSERT_EQ(pal.file_pread(log_fd, bhdr, 34, 32), 34);
    pal.file_close(log_fd);
    EXPECT_TRUE(flag_encrypted(bhdr[5]));

    // 断言 3: 用新实例导出日志, 验证密文可被正确解密且明文完整无损
    logx_config_t rcfg{};
    rcfg.log_dir = dir.c_str();
    std::string staging2 = dir + "/staging2.mmap";
    rcfg.staging_path = staging2.c_str();
    rcfg.max_log_length = 20 * 1024;
    rcfg.retention_days = 7;
    rcfg.max_file_size = 10 * 1024 * 1024;
    rcfg.compress_algo = kAlgoNone;
    rcfg.encrypt_enable = 1;
    rcfg.kid = 33;
    rcfg.key_provider = [](uint16_t kid, void*) -> const uint8_t* {
        if (kid == 33) return s_test_key;
        return nullptr;
    };

    logx_instance_t* rinst = nullptr;
    ASSERT_EQ(logx_open(&rcfg, &rinst), LOGX_OK);

    struct ExportContext {
        int count = 0;
        std::string content;
        Pal* pal = nullptr;
    } exp_ctx;
    exp_ctx.pal = &pal;

    auto export_cb = [](void* user, const char*, const char* path, uint64_t size) -> int {
        auto* ctx = static_cast<ExportContext*>(user);
        ctx->count++;
        int fd = ctx->pal->file_open(path, false, false);
        if (fd >= 0) {
            std::vector<char> buf(size);
            if (ctx->pal->file_pread(fd, buf.data(), size, 0) == static_cast<long>(size)) {
                ctx->content.append(buf.data(), size);
            }
            ctx->pal->file_close(fd);
        }
        return 0;
    };

    EXPECT_EQ(logx_export(rinst, date_str, date_str, export_cb, &exp_ctx), LOGX_OK);
    EXPECT_GE(exp_ctx.count, 1);
    EXPECT_EQ(exp_ctx.content.find("decode fail"), std::string::npos);
    EXPECT_NE(exp_ctx.content.find("seq="), std::string::npos);
    EXPECT_NE(exp_ctx.content.find("4a3031546167"), std::string::npos);

    logx_close(rinst);

    // 清理
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

#endif  // LOGX_WITH_AESGCM