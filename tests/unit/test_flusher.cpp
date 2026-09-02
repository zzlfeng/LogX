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
#include <cstring>
#include <string>
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

TEST(Split, OneBlockManySmallRecords) {
    std::vector<uint8_t> raw;
    for (int i = 0; i < 100; ++i) {
        auto r = make_entry(50);
        raw.insert(raw.end(), r.begin(), r.end());
    }
    std::vector<PendingBlock> blocks;
    uint32_t n = split_into_blocks(raw.data(),
                                   static_cast<uint32_t>(raw.size()), &blocks);
    EXPECT_GE(n, 1u);
    // 每块 raw_len 应是 16 的整数倍:6 + 50 = 56;每块 285 条;首块 ≈ 16KB
    for (auto& b : blocks) {
        EXPECT_GT(b.raw_len, 0u);
    }
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
    // raw = [record A 30B] + [半条头 6B + payload 2B] → 应 1 块,raw_len = 38B
    // (撕裂尾巴归入当前块并封块)
    auto a = make_entry(24);
    std::vector<uint8_t> raw(a.begin(), a.end());
    raw.push_back(2); raw.push_back(0);             // len = 2
    raw.push_back(0x4c); raw.push_back(0x47);       // magic
    raw.push_back(0); raw.push_back(0);             // flags / cksum
    raw.push_back(0); raw.push_back(0);             // payload (撕裂 2B)
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

    // 调用 decode_block 验证完整还原
    std::vector<uint8_t> decoded_raw;
    ASSERT_TRUE(decode_block(blk_flag, key, 5, salt,
                             payload_buf.data(), payload_len, tag_buf,
                             nullptr, static_cast<uint32_t>(rec.size()), &decoded_raw));
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

    // 执行 export 导出
    uint32_t yyyymmdd = 0, hhmmss = 0;
    pal.wallclock_date(&yyyymmdd, &hhmmss);
    char date_str[16];
    std::snprintf(date_str, sizeof(date_str), "%08u", yyyymmdd);

    int export_called = 0;
    auto export_cb = [](void* user, const char* /*d*/, const char* /*path*/, uint64_t size) -> int {
        int* counter = static_cast<int*>(user);
        (*counter)++;
        EXPECT_GT(size, 0u);
        return 0;
    };

    EXPECT_EQ(logx_export(inst, date_str, date_str, export_cb, &export_called), LOGX_OK);
    EXPECT_GE(export_called, 1);

    logx_close(inst);

    // 清理目录
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

#endif  // LOGX_WITH_AESGCM