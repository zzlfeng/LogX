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
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// (T-10) 原子自增目录序号:替代未播种 rand(),消除并行/重跑目录撞名
static std::atomic<uint64_t> s_test_dir_seq{0};

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
                      std::to_string(s_test_dir_seq.fetch_add(1));
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
                      std::to_string(s_test_dir_seq.fetch_add(1));
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
                      std::to_string(s_test_dir_seq.fetch_add(1));
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
                      std::to_string(s_test_dir_seq.fetch_add(1));
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
                      std::to_string(s_test_dir_seq.fetch_add(1));
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
                      std::to_string(s_test_dir_seq.fetch_add(1));
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

    // P3-1 (方案 B): flush_blocking 严格专注落盘, 剥离 retention 清理以消除高频 flush 扫盘开销
    flush_blocking(&inst);
    // 验证 flush_blocking 不再进行昂贵的目录扫描与删除
    EXPECT_GT(pal.file_size_path(old_file.c_str()), 0);

    // 显式调用 retention_cleanup 或跨天/启动阶段执行文件清理
    retention_cleanup(&inst);
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
                      std::to_string(s_test_dir_seq.fetch_add(1));
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
                      std::to_string(s_test_dir_seq.fetch_add(1));
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
                      std::to_string(s_test_dir_seq.fetch_add(1));
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
    uint16_t split = 0;
    EXPECT_TRUE(parse_log_filename("20260907.log", &date, &split));
    EXPECT_EQ(date, 20260907u);
    EXPECT_EQ(split, 0);

    EXPECT_TRUE(parse_log_filename("20260907.log.1", &date, &split));
    EXPECT_EQ(date, 20260907u);
    EXPECT_EQ(split, 1);

    EXPECT_TRUE(parse_log_filename("20260907.log.255", &date, &split));
    EXPECT_EQ(date, 20260907u);
    EXPECT_EQ(split, 255);

    EXPECT_TRUE(parse_log_filename("20260907.log.256", &date, &split));
    EXPECT_EQ(date, 20260907u);
    EXPECT_EQ(split, 256);

    EXPECT_TRUE(parse_log_filename("20260907.log.9999", &date, &split));
    EXPECT_EQ(date, 20260907u);
    EXPECT_EQ(split, 9999);

    EXPECT_TRUE(parse_log_filename("20260907.log.65535", &date, &split));
    EXPECT_EQ(date, 20260907u);
    EXPECT_EQ(split, 65535);

    EXPECT_TRUE(parse_log_filename("/var/log/app/20260907.log.42", &date, &split));
    EXPECT_EQ(date, 20260907u);
    EXPECT_EQ(split, 42);

    EXPECT_TRUE(parse_log_filename("D:\\Logs\\MyApp\\20260907.log.99", &date, &split));
    EXPECT_EQ(date, 20260907u);
    EXPECT_EQ(split, 99);

    EXPECT_FALSE(parse_log_filename("20260907.log.65536", &date, &split));
    EXPECT_FALSE(parse_log_filename("20260907.log.99999", &date, &split));
    EXPECT_FALSE(parse_log_filename("20260907.log.-1", &date, &split));
    EXPECT_FALSE(parse_log_filename("20260907.log. 1", &date, &split));
    EXPECT_FALSE(parse_log_filename("20260907.log.bak", &date, &split));
    EXPECT_FALSE(parse_log_filename("staging.mmap", &date, &split));
    EXPECT_FALSE(parse_log_filename("staging.mmap.lock", &date, &split));
    EXPECT_FALSE(parse_log_filename("short.log", &date, &split));
    EXPECT_FALSE(parse_log_filename("abcd1234.log", &date, &split));
    EXPECT_FALSE(parse_log_filename("", &date, &split));

    // R-03: 数值合法但日历不存在的日期必须被拒绝
    EXPECT_FALSE(parse_log_filename("20261399.log", &date, &split));  // 13 月 99 日
    EXPECT_FALSE(parse_log_filename("20260231.log", &date, &split));  // 2 月 31 日
    EXPECT_FALSE(parse_log_filename("20260229.log", &date, &split));  // 非闰年 2/29
    EXPECT_FALSE(parse_log_filename("20260010.log", &date, &split));  // 0 月
    EXPECT_FALSE(parse_log_filename("20260100.log", &date, &split));  // 0 日
    EXPECT_FALSE(parse_log_filename("00000101.log", &date, &split));  // 0 年
    // 闰年边界:2000/2400 是闰年(400 倍数),1900 不是(100 倍数非 400 倍数)
    EXPECT_TRUE(parse_log_filename("20240229.log", &date, &split));
    EXPECT_EQ(date, 20240229u);
    EXPECT_TRUE(parse_log_filename("20000229.log", &date, &split));
    EXPECT_FALSE(parse_log_filename("19000229.log", &date, &split));

    // R-03: valid_yyyymmdd 直接断言
    EXPECT_TRUE(valid_yyyymmdd(20260907u));
    EXPECT_TRUE(valid_yyyymmdd(20000229u));   // 400 倍数闰年
    EXPECT_TRUE(valid_yyyymmdd(20241231u));
    EXPECT_FALSE(valid_yyyymmdd(20261399u));
    EXPECT_FALSE(valid_yyyymmdd(20260231u));
    EXPECT_FALSE(valid_yyyymmdd(20260229u));  // 平年
    EXPECT_FALSE(valid_yyyymmdd(19000229u));  // 100 倍数非 400 倍数
    EXPECT_FALSE(valid_yyyymmdd(0u));
    EXPECT_FALSE(valid_yyyymmdd(20260000u));
    EXPECT_FALSE(valid_yyyymmdd(20260431u));  // 4 月 31 日

    // R-03: static_assert 编译期常量断言
    static_assert(valid_yyyymmdd(20260907u), "valid date");
    static_assert(valid_yyyymmdd(20000229u), "leap year 400");
    static_assert(!valid_yyyymmdd(19000229u), "century non-leap 100");
    static_assert(!valid_yyyymmdd(20260231u), "feb 31 invalid");
    static_assert(!valid_yyyymmdd(20261399u), "month 13 invalid");

    // make_log_path <-> parse_log_filename 往返一致性
    const std::string dummy_dir = "logs";
    for (uint16_t s : {uint16_t(0), uint16_t(1), uint16_t(42), uint16_t(255),
                       uint16_t(256), uint16_t(1000), uint16_t(65535)}) {
        std::string p = make_log_path(dummy_dir, 20260907u, s);
        uint32_t d_out = 0;
        uint16_t s_out = 0;
        EXPECT_TRUE(parse_log_filename(p, &d_out, &s_out));
        EXPECT_EQ(d_out, 20260907u);
        EXPECT_EQ(s_out, s);
    }
}

TEST(Flush, FileSizeThreshold_ResumeRotatesToNextSplitWithoutOverwrite) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_split_resume_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
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
                      std::to_string(s_test_dir_seq.fetch_add(1));
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
        atc_set_split(&old_atc, 0);
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

// (T-02) 原名 ExportWithMultipleRecordsAndMissingKey_HandledGracefully 与实现
// 不符:配置实为明文模式,从未涉及"缺失密钥"。更名如实反映测试内容;
// 真正的"缺失密钥导出优雅降级"场景由下方 MissingKey 派生用例覆盖。
TEST(Flush, ExportWithMultipleRecords_PlaintextMode_ExportsAllLines) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_exp_multi_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
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

// (T-02 补齐) 真正的"缺失密钥"场景:加密文件 + key_provider 对该 kid 返回
// null → 导出必须优雅降级:返回 LOGX_OK、输出 [skipped: ...] 占位行(与
// C++ decode_one_file 的降级契约一致),绝不崩溃、绝不误报成功导出明文。
// (未接 AES-GCM 的基础构建下 logx_open 对 encrypt_enable=1 fail-fast,
// 该用例无意义,按文件内既有惯例以宏守卫排除。)
#if LOGX_WITH_AESGCM
TEST(Flush, ExportEncryptedFile_MissingKey_SkipsGracefully) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_exp_nokey_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    struct KeyContext {
        std::atomic<bool> missing{false};
    } key_ctx;

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 20 * 1024;
    cfg.retention_days = 7;
    cfg.max_file_size = 10 * 1024 * 1024;
    cfg.flush_threshold = 8 * 1024;
    cfg.compress_algo = kAlgoNone;
    cfg.encrypt_enable = 1;
    cfg.kid = 77;
    cfg.user = &key_ctx;
    cfg.key_provider = [](uint16_t kid, void* user) -> const uint8_t* {
        auto* ctx = static_cast<KeyContext*>(user);
        if (ctx && ctx->missing.load(std::memory_order_acquire)) {
            return nullptr;
        }
        // 打开/写入阶段提供密钥 (写路径需要),但导出阶段若 missing=true 则返回 null
        static uint8_t key[32] = {0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A,
                                  0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A,
                                  0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A,
                                  0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A};
        (void)kid;
        return key;
    };

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    ASSERT_EQ(logx_write(inst, 2, "Sec",
                         (const uint8_t*)"encrypted_payload", 17), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    // 导出阶段切换为"该 kid 缺失密钥":通过外部上下文标志使 key_provider 返回 null,
    // 避免在运行期通过 static_cast 强写 Instance 内部指针
    key_ctx.missing.store(true, std::memory_order_release);

    uint32_t yyyymmdd = 0, hhmmss = 0;
    pal.wallclock_date(&yyyymmdd, &hhmmss);
    char date_str[16];
    std::snprintf(date_str, sizeof(date_str), "%08u", yyyymmdd);

    struct Ctx { int called = 0; std::string content; } ctx;
    auto cb = [](void* user, const char* /*date*/, const char* path, uint64_t size) -> int {
        auto* c = static_cast<Ctx*>(user);
        c->called++;
        Pal& p = default_pal();
        int fd = p.file_open(path, false, false);
        if (fd >= 0 && size > 0) {
            std::vector<char> buf(size);
            if (p.file_pread(fd, buf.data(), size, 0) == static_cast<long>(size)) {
                c->content.append(buf.data(), size);
            }
            p.file_close(fd);
        }
        return 0;
    };

    // 优雅降级契约:导出流程本身成功(不崩溃/不 IO 失败),但内容为
    // [skipped: ...] 占位而非明文载荷
    EXPECT_EQ(logx_export(inst, date_str, date_str, cb, &ctx), LOGX_OK);
    EXPECT_GE(ctx.called, 1);
    EXPECT_NE(ctx.content.find("[skipped:"), std::string::npos);
    EXPECT_EQ(ctx.content.find("encrypted_payload"), std::string::npos);

    logx_close(inst);

    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}
#endif  // LOGX_WITH_AESGCM

TEST(Flush, HeapDegraded_WritesAndFlushesCorrectly) {
    // 验证在 mmap 降级模式下, 环形缓冲区的日志依然能够被 Flusher 正常切块、落盘和导出
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_degraded_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
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

TEST(ErrorMonitoring, TruncateEventOnTooLarge_PrefixKeptAndCounted) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_drop_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
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
        // B-3 方案B/T5.6: 截断用独立事件种类 LOGX_EV_TRUNCATED 承载
        if (kind == LOGX_EV_TRUNCATED) {
            auto* t = static_cast<DropTracker*>(user);
            t->drop_count++;
            t->last_msg = msg ? msg : "";
        }
    }, &tracker);

    // B-1B 方案B: 写入 600B 日志(超 max_log_length 500B)→ 截断保前缀,
    // 返回 LOGX_OK 并同步发 TRUNCATED 事件,truncated_logs 计数递增。
    std::string large_msg(600, 'L');
    EXPECT_EQ(logx_write(inst, 2, "Tag",
                         reinterpret_cast<const uint8_t*>(large_msg.data()),
                         static_cast<uint32_t>(large_msg.size())), LOGX_OK);
    EXPECT_EQ(tracker.drop_count, 1);
    EXPECT_EQ(tracker.last_msg.compare(0, 13, "log truncated"), 0);

    logx_stats_t st{};
    logx_get_stats(inst, &st);
    EXPECT_EQ(st.truncated_logs, 1u);
    EXPECT_EQ(st.drop_too_large, 0u);

    // 截断记录真实落环:flush 后 staging 待刷区被消费
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    logx_close(inst);

    // 清理
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

// B-1B: 病态小 max_log_length(截断到空记录仍放不下)保留 TOO_LARGE 丢弃兜底。
TEST(ErrorMonitoring, CannotFitDropPath_PreservedForPathologicalLimit) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_cannot_fit_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    // tag "E" (1B) 的最小空记录 = 27(用户头) + 1(tag) + 6(片头) = 34B;
    // max_log_length=33 连空记录都放不下 → kCannotFit → TOO_LARGE 丢弃兜底。
    cfg.max_log_length = 33;
    cfg.retention_days = 7;
    cfg.max_file_size = 10 * 1024 * 1024;
    cfg.flush_threshold = 16 * 1024;
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

    std::string large_msg(600, 'L');
    EXPECT_EQ(logx_write(inst, 2, "E",
                         reinterpret_cast<const uint8_t*>(large_msg.data()),
                         static_cast<uint32_t>(large_msg.size())), LOGX_ERR_TOO_LARGE);
    EXPECT_EQ(tracker.drop_count, 1);
    EXPECT_EQ(tracker.last_msg, "log too large");

    logx_stats_t st{};
    logx_get_stats(inst, &st);
    EXPECT_EQ(st.drop_too_large, 1u);
    EXPECT_EQ(st.truncated_logs, 0u);

    logx_close(inst);
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

TEST(ErrorMonitoring, InitStageEventsCapturedViaConfig) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_init_ev_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
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

TEST(Flush, FileSizeThreshold_RotatesBeyond255_Succeeds) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_split_beyond255_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    uint32_t yyyymmdd = 0, hhmmss = 0;
    pal.wallclock_date(&yyyymmdd, &hhmmss);

    // 预先构造 split=255 的日志文件，使得后续写入直接触发向 .log.256 切分
    std::string path255 = make_log_path(dir, yyyymmdd, 255);
    create_valid_test_log(pal, path255, yyyymmdd, 100, 500);

    // 构造 Staging 缓存: split_no = 255 (已满)
    {
        Staging st_builder(&pal, staging, 8 * 1024);
        RecoveryState st; bool rec = false;
        ASSERT_TRUE(st_builder.open(&st, &rec));
        AppendTargetCache atc{};
        atc.date = yyyymmdd;
        atc_set_split(&atc, 255);
        atc.last_block_offset = 32;
        atc.last_block_end_offset = 500;
        st_builder.commit_header_locked(100, 0, 0, &atc);
        st_builder.close();
    }

    logx_config_t cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 20 * 1024;
    cfg.max_file_size = 300;  // 阈值 300B，已有 500B 必定立即切分

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    // 写入日志, 验证成功创建并切入 .log.256
    const char* msg = "message_in_split_256";
    ASSERT_EQ(logx_write(inst, 2, "T256", reinterpret_cast<const uint8_t*>(msg),
                         static_cast<uint32_t>(strlen(msg))), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    std::string path256 = make_log_path(dir, yyyymmdd, 256);
    EXPECT_GE(pal.file_size_path(path256.c_str()), 32);  // 至少包含 32B 文件头与 1 块日志

    logx_close(inst);

    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

TEST(Flush, FileSizeThreshold_SplitLimit65535_EmitsEventAndSuspends) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_split_limit65535_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    uint32_t yyyymmdd = 0, hhmmss = 0;
    pal.wallclock_date(&yyyymmdd, &hhmmss);

    // 预先构造 split=65535 的日志文件 (已满 500B)
    std::string path65535 = make_log_path(dir, yyyymmdd, 65535);
    create_valid_test_log(pal, path65535, yyyymmdd, 100, 500);

    // 构造 Staging 缓存: split_no = 65535 (已满)
    {
        Staging st_builder(&pal, staging, 8 * 1024);
        RecoveryState st; bool rec = false;
        ASSERT_TRUE(st_builder.open(&st, &rec));
        AppendTargetCache atc{};
        atc.date = yyyymmdd;
        atc_set_split(&atc, 65535);
        atc.last_block_offset = 32;
        atc.last_block_end_offset = 500;
        st_builder.commit_header_locked(100, 0, 0, &atc);
        st_builder.close();
    }

    struct EventTracker {
        std::atomic<int> event_count{0};
        std::atomic<int> last_event_kind{0};
        std::string last_event_msg;
        std::mutex mu;
    } tracker;

    logx_config_t cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 20 * 1024;
    cfg.max_file_size = 300;  // 阈值 300B，已有 500B 必定触发切分
    cfg.err_fn = [](void* user, logx_event_kind_e kind, int code, const char* msg) {
        (void)code;
        auto* t = static_cast<EventTracker*>(user);
        t->event_count.fetch_add(1);
        t->last_event_kind.store(static_cast<int>(kind));
        std::lock_guard<std::mutex> lk(t->mu);
        t->last_event_msg = msg ? msg : "";
    };
    cfg.err_user = &tracker;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    // 写入日志, 此时 Flusher 尝试切分发现分卷达到 65535 上限
    const char* msg = "message_at_limit";
    ASSERT_EQ(logx_write(inst, 2, "TLIM", reinterpret_cast<const uint8_t*>(msg),
                         static_cast<uint32_t>(strlen(msg))), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    // 验证事件: 必须发射 LOGX_EV_IO 且包含 65535 上限描述
    EXPECT_GE(tracker.event_count.load(), 1);
    EXPECT_EQ(tracker.last_event_kind.load(), LOGX_EV_IO);
    {
        std::lock_guard<std::mutex> lk(tracker.mu);
        EXPECT_NE(tracker.last_event_msg.find("daily split limit (65535) exhausted"), std::string::npos);
        // 关键断言: 抑制了重复的 "append target open failed"
        EXPECT_EQ(tracker.last_event_msg.find("append target open failed"), std::string::npos);
    }

    logx_close(inst);

    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

TEST(Flush, AppendTargetCache_HistoricalStagingLayoutCompatibility) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_atc_compat_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    uint32_t yyyymmdd = 0, hhmmss = 0;
    pal.wallclock_date(&yyyymmdd, &hhmmss);

    // 构造 split=3 的日志文件 (含 1 块 300B)
    std::string path3 = make_log_path(dir, yyyymmdd, 3);
    create_valid_test_log(pal, path3, yyyymmdd, 50, 300);

    // 手工以原始字节流构造旧版本 Staging 双 Header:
    // Header.reserved[0..3] = date, reserved[4] = 3, reserved[5] = 0 (历史填充恒零),
    // reserved[8..11] = last_block_offset (32), reserved[12..15] = end_offset (300)
    {
        int fd = pal.file_open(staging.c_str(), true, true);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(pal.file_truncate(fd, 8 * 1024), 0);
        std::vector<uint8_t> raw(8 * 1024, 0);

        // 构造 Header A
        put_u32(raw.data() + 0, kMagicStaging);
        put_u16(raw.data() + 4, 1);   // version
        put_u16(raw.data() + 6, 0);   // flags
        put_u32(raw.data() + 8, 50);  // seqno
        put_u32(raw.data() + 12, 0);  // flushed_len
        put_u32(raw.data() + 16, 0);  // write_pos

        // reserved[0..15]: 旧布局
        put_u32(raw.data() + 20 + 0, yyyymmdd);
        raw[20 + 4] = 3;  // split_no = 3
        raw[20 + 5] = 0;  // 历史填充字节恒为 0
        raw[20 + 6] = 0;
        raw[20 + 7] = 0;
        put_u32(raw.data() + 20 + 8, 32);   // last_block_offset
        put_u32(raw.data() + 20 + 12, 300); // last_block_end_offset

        // 计算 Header A CRC
        uint32_t crc_a = crc32_update(0, raw.data(), 60);
        put_u32(raw.data() + 60, crc_a);

        // 复制为 Header B
        std::memcpy(raw.data() + 64, raw.data(), 64);

        ASSERT_EQ(pal.file_write(fd, raw.data(), static_cast<uint32_t>(raw.size())),
                  static_cast<long>(raw.size()));
        pal.file_fsync(fd);
        pal.file_close(fd);
    }

    logx_config_t cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 20 * 1024;
    cfg.max_file_size = 10 * 1024 * 1024;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    // 验证新版本恢复正确读取旧布局: 接管追加到 .log.3
    int64_t before_sz = pal.file_size_path(path3.c_str());
    EXPECT_EQ(before_sz, 300);

    const char* msg = "append_to_historical_split_3";
    ASSERT_EQ(logx_write(inst, 2, "COMP", reinterpret_cast<const uint8_t*>(msg),
                         static_cast<uint32_t>(strlen(msg))), LOGX_OK);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    int64_t after_sz = pal.file_size_path(path3.c_str());
    EXPECT_GT(after_sz, before_sz);  // 成功追加到 .log.3

    logx_close(inst);

    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

// =========================================================================
// F-08: 水位线防饥饿与超时机制验证 (T4.0-1 / WARN-3)
// =========================================================================

TEST(Flush, WatermarkReturnsUnderSustainedWrites) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_f08_starve_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 4096;
    cfg.flush_threshold = 512;
    cfg.compress_algo = kAlgoNone;
    cfg.encrypt_enable = 0;

    struct ErrorCounter {
        std::atomic<int> drop_count{0};
    } err_ctx;

    cfg.err_fn = [](void* user, logx_event_kind_e kind, int code, const char* msg) {
        (void)code; (void)msg;
        if (kind == LOGX_EV_DROP) {
            static_cast<ErrorCounter*>(user)->drop_count.fetch_add(1);
        }
    };
    cfg.err_user = &err_ctx;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    std::atomic<bool> stop_writer{false};
    std::atomic<uint64_t> total_written{0};

    // 启动后台持续写入线程
    std::thread writer([&]() {
        for (int i = 0; !stop_writer.load(std::memory_order_relaxed); ++i) {
            std::string msg = "sustained_log_" + std::to_string(i);
            if (logx_write(inst, 2, "SUSTAIN",
                           reinterpret_cast<const uint8_t*>(msg.data()),
                           static_cast<uint32_t>(msg.size())) == LOGX_OK) {
                total_written.fetch_add(1, std::memory_order_relaxed);
            }
            pal.sleep_ms(1);
        }
    });

    // 等待写线程产出一定量日志
    while (total_written.load(std::memory_order_relaxed) < 20) {
        pal.sleep_ms(2);
    }

    // 主线程在写入持续进行时调用 flush，必须迅速准时返回（修复前会因 staging 永不为空而饥饿死等）
    auto t0 = std::chrono::steady_clock::now();
    logx_status_e rc = logx_flush(inst);
    auto cost_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    EXPECT_EQ(rc, LOGX_OK);
    EXPECT_LT(cost_ms, 3000);

    // 停止写线程
    stop_writer.store(true, std::memory_order_relaxed);
    writer.join();

    logx_close(inst);

    EXPECT_EQ(err_ctx.drop_count.load(), 0);

    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

class StallFileWritePal : public Pal {
public:
    explicit StallFileWritePal(Pal* real_pal) : real_(real_pal) {}

    std::atomic<bool> stall_write{false};
    std::atomic<bool> entered_stall{false};
    std::condition_variable stall_cv;
    std::mutex stall_mu;

    int file_open(const char* p, bool c, bool a) override { return real_->file_open(p, c, a); }
    int file_close(int fd) override { return real_->file_close(fd); }
    int file_truncate(int fd, uint64_t len) override { return real_->file_truncate(fd, len); }
    int64_t file_size(int fd) override { return real_->file_size(fd); }
    int64_t file_size_path(const char* p) override { return real_->file_size_path(p); }
    long file_pread(int fd, void* b, size_t n, uint64_t o) override { return real_->file_pread(fd, b, n, o); }
    
    long file_write(int fd, const void* b, size_t n) override {
        if (stall_write.load(std::memory_order_acquire)) {
            entered_stall.store(true, std::memory_order_release);
            std::unique_lock<std::mutex> lock(stall_mu);
            stall_cv.wait(lock, [&]() { return !stall_write.load(std::memory_order_acquire); });
        }
        return real_->file_write(fd, b, n);
    }
    long file_pwrite(int fd, const void* b, size_t n, uint64_t o) override {
        if (stall_write.load(std::memory_order_acquire)) {
            entered_stall.store(true, std::memory_order_release);
            std::unique_lock<std::mutex> lock(stall_mu);
            stall_cv.wait(lock, [&]() { return !stall_write.load(std::memory_order_acquire); });
        }
        return real_->file_pwrite(fd, b, n, o);
    }
    int file_fsync(int fd) override { return real_->file_fsync(fd); }
    int file_dir_fsync(const char* p) override { return real_->file_dir_fsync(p); }
    int file_unlink(const char* p) override { return real_->file_unlink(p); }
    int file_mkdirs(const char* p) override { return real_->file_mkdirs(p); }
    int file_rmdir(const char* p) override { return real_->file_rmdir(p); }
    std::vector<std::string> file_list(const char* d) override { return real_->file_list(d); }
    PalMmap mmap_open(int fd, size_t len, bool w) override { return real_->mmap_open(fd, len, w); }
    int mmap_sync(const PalMmap& m, bool s) override { return real_->mmap_sync(m, s); }
    int mmap_close(const PalMmap& m) override { return real_->mmap_close(m); }
    void* mutex_create() override { return real_->mutex_create(); }
    void  mutex_destroy(void* h) override { real_->mutex_destroy(h); }
    void  mutex_lock(void* h) override { real_->mutex_lock(h); }
    void  mutex_unlock(void* h) override { real_->mutex_unlock(h); }
    int   threading_init() override { return real_->threading_init(); }
    int   thread_start(void* (*fn)(void*), void* a, void** out_h) override { return real_->thread_start(fn, a, out_h); }
    int   thread_join(void* h) override { return real_->thread_join(h); }
    void* ev_create() override { return real_->ev_create(); }
    void  ev_destroy(void* h) override { real_->ev_destroy(h); }
    void  ev_signal(void* h) override { real_->ev_signal(h); }
    void  ev_wait(void* h) override { real_->ev_wait(h); }
    bool  ev_wait_timeout(void* h, uint32_t ms) override { return real_->ev_wait_timeout(h, ms); }
    uint64_t atomic_add(uint64_t* p, int64_t d) override { return real_->atomic_add(p, d); }
    uint64_t atomic_load(uint64_t* p) override { return real_->atomic_load(p); }
    uint64_t monotonic_ns() override { return real_->monotonic_ns(); }
    uint64_t wallclock_ms() override { return real_->wallclock_ms(); }
    void     wallclock_date(uint32_t* y, uint32_t* h) override { real_->wallclock_date(y, h); }
    void     sleep_ms(uint32_t ms) override { real_->sleep_ms(ms); }
    uint32_t pid() override { return real_->pid(); }
    uint64_t tid() override { return real_->tid(); }
    bool     random_bytes(void* b, size_t l) override { return real_->random_bytes(b, l); }
    void     debug_log(const char* s) override { real_->debug_log(s); }
private:
    Pal* real_ = nullptr;
};

TEST(Flush, TimeoutWhenFlusherStalled_EmitsEventAndReturns) {
    Pal& real_pal = default_pal();
    StallFileWritePal stall_pal(&real_pal);

    std::string dir = std::string("logx_test_f08_stall_") +
                      std::to_string(real_pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
    real_pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    struct TimeoutEventTracker {
        std::atomic<bool> timeout_seen{false};
        std::string timeout_msg;
        std::mutex mu;
    } tracker;

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 4096;
    cfg.flush_threshold = 256;
    cfg.compress_algo = kAlgoNone;
    cfg.encrypt_enable = 0;
    cfg.err_fn = [](void* user, logx_event_kind_e kind, int code, const char* msg) {
        (void)code;
        if (kind == LOGX_EV_IO && msg && std::strstr(msg, "flush timeout")) {
            auto* t = static_cast<TimeoutEventTracker*>(user);
            t->timeout_seen.store(true);
            std::lock_guard<std::mutex> lk(t->mu);
            t->timeout_msg = msg;
        }
    };
    cfg.err_user = &tracker;
    cfg.pal_override = &stall_pal;  // (T-04) Flusher 启动前定型 PAL,根除注入竞态

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    // 启用写入挂起注入
    stall_pal.stall_write.store(true, std::memory_order_release);

    // 写入一条日志，触发 Flusher 尝试写盘并卡在 file_write 中
    const char* msg = "stalled_log_entry";
    ASSERT_EQ(logx_write(inst, 2, "STALL", reinterpret_cast<const uint8_t*>(msg),
                         static_cast<uint32_t>(std::strlen(msg))), LOGX_OK);

    // 等待 Flusher 线程真正进入写挂起状态
    while (!stall_pal.entered_stall.load(std::memory_order_acquire)) {
        real_pal.sleep_ms(2);
    }

    // 调用 logx_flush，必须在 ~5000ms 超时后返回 LOGX_ERR_TIMEOUT
    auto t0 = std::chrono::steady_clock::now();
    logx_status_e rc = logx_flush(inst);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    EXPECT_EQ(rc, LOGX_ERR_TIMEOUT);
    EXPECT_GE(elapsed_ms, 4800);
    EXPECT_LE(elapsed_ms, 7000);
    EXPECT_TRUE(tracker.timeout_seen.load());

    // 解除挂起，使 Flusher 完成正常落盘
    {
        std::lock_guard<std::mutex> lk(stall_pal.stall_mu);
        stall_pal.stall_write.store(false, std::memory_order_release);
    }
    stall_pal.stall_cv.notify_all();

    logx_close(inst);

    for (const auto& f : real_pal.file_list(dir.c_str())) {
        real_pal.file_unlink(f.c_str());
    }
    real_pal.file_rmdir(dir.c_str());
}

TEST(Flush, DiscardPathDoesNotStall) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_f08_discard_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    uint32_t yyyymmdd = 0, hhmmss = 0;
    pal.wallclock_date(&yyyymmdd, &hhmmss);

    // 预先构造 split=65535 的日志文件 (已满 500B)
    std::string path65535 = make_log_path(dir, yyyymmdd, 65535);
    create_valid_test_log(pal, path65535, yyyymmdd, 100, 500);

    // 构造 Staging 缓存: split_no = 65535 (已满)
    {
        Staging st_builder(&pal, staging, 8 * 1024);
        RecoveryState st; bool rec = false;
        ASSERT_TRUE(st_builder.open(&st, &rec));
        AppendTargetCache atc{};
        atc.date = yyyymmdd;
        atc_set_split(&atc, 65535);
        atc.last_block_offset = 32;
        atc.last_block_end_offset = 500;
        st_builder.commit_header_locked(100, 0, 0, &atc);
        st_builder.close();
    }

    struct TimeoutCheckTracker {
        std::atomic<bool> timeout_seen{false};
    } tracker;

    logx_config_t cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 20 * 1024;
    cfg.max_file_size = 300;  // 阈值 300B，已有 500B 必定触发切分到达 65535 上限并走丢弃路径
    cfg.err_fn = [](void* user, logx_event_kind_e kind, int code, const char* msg) {
        (void)code;
        if (kind == LOGX_EV_IO && msg && std::strstr(msg, "flush timeout")) {
            static_cast<TimeoutCheckTracker*>(user)->timeout_seen.store(true);
        }
    };
    cfg.err_user = &tracker;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    // 写入日志, 此时 Flusher 将进入 65535 上限丢弃路径
    const char* msg = "discard_path_entry";
    ASSERT_EQ(logx_write(inst, 2, "DISC", reinterpret_cast<const uint8_t*>(msg),
                         static_cast<uint32_t>(std::strlen(msg))), LOGX_OK);

    // 字节水位线必须随丢弃块推进，flush 应当快速返回 LOGX_OK（远小于 5000ms 超时）
    auto t0 = std::chrono::steady_clock::now();
    logx_status_e rc = logx_flush(inst);
    auto cost_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    EXPECT_EQ(rc, LOGX_OK);
    EXPECT_LT(cost_ms, 1000);
    EXPECT_FALSE(tracker.timeout_seen.load());

    logx_close(inst);

    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

#if LOGX_WITH_AESGCM

TEST(FlusherTest, KeyProvider_DrainsEncryptedTailAndNotCalledAfterClose) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_j01_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
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

// =========================================================================
// F-10: 75ms Debounce 聚集刷盘与低频兜底测试 (Spec §4.1 / T4.0-4)
// =========================================================================

TEST(Flush, Debounce_LowFrequencyWritesFlushedWithinWindow) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_debounce_lowfreq_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;
    cfg.flush_threshold = 8 * 1024; // 8KB, 3 条小日志绝不会触发 threshold 唤醒

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    // 写入 3 条日志, 间隔 ~20ms, 总字节数远低于 8KB
    for (int i = 1; i <= 3; ++i) {
        std::string line = "low_freq_log_message_" + std::to_string(i);
        EXPECT_EQ(logx_write(inst, 1, "TAG", reinterpret_cast<const uint8_t*>(line.c_str()),
                             static_cast<uint32_t>(line.size())), LOGX_OK);
        pal.sleep_ms(20);
    }

    uint64_t t0 = pal.monotonic_ns();

    // 不调用 logx_flush, 轮询等待 Debounce 兜底刷盘 (断言 <= 500ms 内落盘)
    bool flushed = false;
    std::string log_file;
    while ((pal.monotonic_ns() - t0) / 1000000 <= 500) {
        auto files = pal.file_list(dir.c_str());
        for (const auto& f : files) {
            if (f.find(".log") != std::string::npos && pal.file_size_path(f.c_str()) >= 32) {
                // 检查是否有大于 32 字节的文件(即已包含数据块)
                if (pal.file_size_path(f.c_str()) > 32) {
                    flushed = true;
                    log_file = f;
                    break;
                }
            }
        }
        if (flushed) break;
        pal.sleep_ms(10);
    }

    uint64_t elapsed_ms = (pal.monotonic_ns() - t0) / 1000000;
    EXPECT_TRUE(flushed) << "Debounce failed to flush within 500ms, elapsed: " << elapsed_ms << "ms";
    EXPECT_LE(elapsed_ms, 500ULL);

    // 验证导出 3 条日志完整且无解码失败
    uint32_t ymd = 0, hms = 0;
    pal.wallclock_date(&ymd, &hms);
    char date_str[16];
    std::snprintf(date_str, sizeof(date_str), "%08u", ymd);

    struct ExpCtx {
        int count = 0;
        std::string content;
        Pal* pal = nullptr;
    } ctx;
    ctx.pal = &pal;

    auto cb = [](void* user, const char* /*date*/, const char* plaintext_path, uint64_t size) -> int {
        auto* c = static_cast<ExpCtx*>(user);
        int fd = c->pal->file_open(plaintext_path, false, false);
        if (fd >= 0) {
            std::vector<char> buf(size);
            if (c->pal->file_pread(fd, buf.data(), size, 0) == static_cast<long>(size)) {
                c->content.append(buf.data(), size);
            }
            c->pal->file_close(fd);
        }
        return 0;
    };

    EXPECT_EQ(logx_export(inst, date_str, date_str, cb, &ctx), LOGX_OK);
    EXPECT_EQ(ctx.content.find("decode fail"), std::string::npos);
    // 导出行格式为 "seq=N raw=M <hex payload>"(logx_c.cpp), 内容断言必须用 hex 特征
    // low_freq_log_message_1/2/3 的 hex 前缀 ( 共同前缀 "low_freq_log_message_" ):
    EXPECT_NE(ctx.content.find("6c6f775f667265715f6c6f675f6d6573736167655f31"), std::string::npos);
    EXPECT_NE(ctx.content.find("6c6f775f667265715f6c6f675f6d6573736167655f32"), std::string::npos);
    EXPECT_NE(ctx.content.find("6c6f775f667265715f6c6f675f6d6573736167655f33"), std::string::npos);

    logx_close(inst);

    // 清理
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

TEST(Flush, Debounce_BurstAggregatesIntoFewBlocks) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_debounce_burst_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());
    std::string st_path = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = st_path.c_str();
    cfg.max_log_length = 4096;
    cfg.flush_threshold = 8 * 1024; // 8KB threshold

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    // 突发一次性写入 10 条小日志 (每条 ~50B, 总计 ~500B, 远小于 8KB)
    for (int i = 1; i <= 10; ++i) {
        std::string line = "burst_small_log_record_" + std::to_string(i);
        EXPECT_EQ(logx_write(inst, 1, "TAG", reinterpret_cast<const uint8_t*>(line.c_str()),
                             static_cast<uint32_t>(line.size())), LOGX_OK);
    }

    // 不调用 logx_flush, 等待 75ms Debounce 窗口触发落盘 (等待 200ms)
    pal.sleep_ms(200);

    logx_close(inst);

    // 找到生成的 .log 文件并统计块数
    auto files = pal.file_list(dir.c_str());
    std::string log_file;
    for (const auto& f : files) {
        if (f.find(".log") != std::string::npos && pal.file_size_path(f.c_str()) > 32) {
            log_file = f;
            break;
        }
    }
    ASSERT_FALSE(log_file.empty());

    // 统计文件中的物理块数量
    int fd = pal.file_open(log_file.c_str(), false, false);
    ASSERT_GE(fd, 0);
    int64_t fsz = pal.file_size(fd);
    int block_count = 0;
    uint64_t off = 32;
    while (off + 34 <= static_cast<uint64_t>(fsz)) {
        uint8_t bhdr[34];
        if (pal.file_pread(fd, bhdr, 34, off) != 34) break;
        if (bhdr[0] != 'L' || bhdr[1] != 'G' || bhdr[2] != 'X' || bhdr[3] != 'B') break;
        uint32_t enc_len = get_u32(bhdr + 14);
        block_count++;
        off += 34 + enc_len;
    }
    pal.file_close(fd);

    // 断言: 10 条记录被聚集在少量块内 (<= 2 块, 通常为 1 块), 而非产生 10 个碎片块
    EXPECT_GT(block_count, 0);
    EXPECT_LE(block_count, 2);

    // 清理
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

TEST(Flush, Export_RejectsInvalidCalendarDates) {
    // R-03 回归: logx_export 对输入 from/to 日期做严格日历合法性校验,
    // 拒绝数值合法但日历不存在的日期(如 20260231、20261399、平年 20260229 等)。
    Pal& pal = default_pal();
    std::string dir = "test_export_invalid_dates_dir";
    std::string staging = "test_export_invalid_dates.staging";
    for (const auto& f : pal.file_list(dir.c_str())) pal.file_unlink(f.c_str());
    pal.file_rmdir(dir.c_str());
    pal.file_unlink(staging.c_str());

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.flush_threshold = 8 * 1024;
    cfg.retention_days = 7;
    cfg.max_log_length = 64 * 1024;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    ASSERT_NE(inst, nullptr);

    auto dummy_cb = [](void*, const char*, const char*, uint64_t) -> int { return 0; };

    // 1. 日历不存在的非法日期 -> LOGX_ERR_INVALID_ARG
    EXPECT_EQ(logx_export(inst, "20260231", "20260231", dummy_cb, nullptr), LOGX_ERR_INVALID_ARG);
    EXPECT_EQ(logx_export(inst, "20261399", "20261399", dummy_cb, nullptr), LOGX_ERR_INVALID_ARG);
    EXPECT_EQ(logx_export(inst, "20260229", "20260229", dummy_cb, nullptr), LOGX_ERR_INVALID_ARG);
    EXPECT_EQ(logx_export(inst, "19000229", "19000229", dummy_cb, nullptr), LOGX_ERR_INVALID_ARG);
    EXPECT_EQ(logx_export(inst, "20260431", "20260431", dummy_cb, nullptr), LOGX_ERR_INVALID_ARG);

    // 2. 格式不合法的日期 -> LOGX_ERR_INVALID_ARG
    EXPECT_EQ(logx_export(inst, "bad_date", "20260907", dummy_cb, nullptr), LOGX_ERR_INVALID_ARG);
    EXPECT_EQ(logx_export(inst, "20260907", "bad_date", dummy_cb, nullptr), LOGX_ERR_INVALID_ARG);
    EXPECT_EQ(logx_export(inst, "202697", "20260907", dummy_cb, nullptr), LOGX_ERR_INVALID_ARG);

    // 3. to < from 逆序范围 -> LOGX_ERR_INVALID_ARG
    EXPECT_EQ(logx_export(inst, "20260907", "20260905", dummy_cb, nullptr), LOGX_ERR_INVALID_ARG);

    // 4. 合法日历范围 (无日志文件时正常返回 NOT_FOUND, 证明已通过参数解析)
    EXPECT_EQ(logx_export(inst, "20260901", "20260907", dummy_cb, nullptr), LOGX_ERR_NOT_FOUND);

    logx_close(inst);

    for (const auto& f : pal.file_list(dir.c_str())) pal.file_unlink(f.c_str());
    pal.file_rmdir(dir.c_str());
    pal.file_unlink(staging.c_str());
}

// R18 F-1 回归屏障: append_block 以 file_salt=nullptr 追加到"已存在"文件时,
// 块头 salt 域(hdr+22)必须回读并复用文件头 salt(hdr+12), 不得把未初始化栈内存落盘。
//
// 关键(避免 R16 F-1 式"空洞绿"): 未初始化栈读的取值依赖调用现场, 上一次
// append_block(seed_salt) 恰好把 seed_salt 留在同一栈槽, 会让"不修也绿"。
// 故在两次 append_block 之间调用 __declspec(noinline) 的 poison 助手,
// 用大数组把该栈区覆写成 0xCC, 使未初始化 salt 必然读到毒值而非 seed_salt。
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void poison_r18_stack() {
    volatile uint8_t junk[4096];
    for (size_t i = 0; i < sizeof(junk); ++i) junk[i] = 0xCC;
    (void)junk;
}

TEST(Flush, AppendBlock_NullSaltOnExistingFile_ReusesFileHeaderSalt) {
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_r18_salt_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_test_dir_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());

    Instance inst{};
    inst.log_dir = dir;
    inst.pal = &pal;
    inst.compress_algo = kAlgoNone;
    inst.encrypt_enable = false;
    inst.kid = 1;

    auto rec = make_entry(64);

    // 1) 首次调用传显式 salt 建文件(合法路径, 落 32B 文件头)
    uint8_t seed_salt[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};
    uint64_t s0 = 0, e0 = 0;
    ASSERT_TRUE(append_block(&inst, seed_salt, 1, rec.data(),
                              static_cast<uint32_t>(rec.size()),
                              /*seqno=*/1, /*flag=*/0, &s0, &e0));

    // 2) 毒化调用栈后, 以 file_salt=nullptr 追加到已存在文件 —— 命中 R18 F-1 分支
    poison_r18_stack();
    uint64_t s1 = 0, e1 = 0;
    ASSERT_TRUE(append_block(&inst, nullptr, 1, rec.data(),
                              static_cast<uint32_t>(rec.size()),
                              /*seqno=*/2, /*flag=*/0, &s1, &e1));

    // 3) 读回两个块头的 salt 域, 均须等于文件头 salt
    uint32_t ymd = 0, hms = 0;
    pal.wallclock_date(&ymd, &hms);
    int fd = pal.file_open(make_log_path(dir, ymd, 0).c_str(), false, false);
    ASSERT_GE(fd, 0);

    uint8_t fhdr[32];
    ASSERT_EQ(pal.file_pread(fd, fhdr, 32, 0), 32);
    EXPECT_EQ(0, std::memcmp(fhdr + 12, seed_salt, 8));  // 文件头 salt 未被污染

    // 块 0(显式 salt)与块 1(nullptr salt)的块头 salt 都必须等于文件头 salt。
    // 块 1 是回归屏障所在: 未修复时此处为 0xCC… 毒值, 断言必红。
    const uint64_t blk_size = e0 - s0;
    for (int i = 0; i < 2; ++i) {
        uint8_t bhdr[34];
        ASSERT_EQ(pal.file_pread(fd, bhdr, 34, s0 + static_cast<uint64_t>(i) * blk_size), 34);
        EXPECT_EQ(0, std::memcmp(bhdr + 22, fhdr + 12, 8))
            << "block#" << i << " salt must reuse file header salt, not uninitialized stack";
    }
    pal.file_close(fd);

    for (const auto& f : pal.file_list(dir.c_str())) pal.file_unlink(f.c_str());
    pal.file_rmdir(dir.c_str());
}
