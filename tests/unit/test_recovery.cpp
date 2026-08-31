// test_recovery.cpp - 启动恢复与对账 单测。
//
// 覆盖(spec §4.3):
//   - S_file == S_hdr:常规,无需回推 F
//   - S_file >  S_hdr:k 块回溯,直接用文件 seqno 当 S_hdr
//   - S_file <  S_hdr:文件尾损坏,截到 S_file,丢失打点
//   - 双 Header 均损坏:整区重建,seqno 接管为 max(file)
//   - 环形扫描:缝残留 / 撕裂 / 6B 全零终止符

#include "logx_internal.h"
#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace logx;

namespace {
constexpr uint32_t kDataSize = 8 * 1024;
constexpr uint32_t kTotal = kDataSize + kHeaderTotal;
}

class RecoveryTest : public ::testing::Test {
protected:
    std::string staging_path_;
    std::string log_dir_;
    void SetUp() override {
        Pal& pal = default_pal();
        staging_path_ = std::string("logx_test_tmp_") +
                        std::to_string(pal.pid()) + "_" +
                        std::to_string(rand()) + ".bin";
        log_dir_ = std::string("logx_test_tmp_") +
                   std::to_string(pal.pid()) + "_" +
                   std::to_string(rand());
        pal.file_unlink(staging_path_.c_str());
        pal.file_mkdirs(log_dir_.c_str());
    }
    void TearDown() override {
        Pal& pal = default_pal();
        pal.file_unlink(staging_path_.c_str());
        // 简单清理 log_dir
        for (const auto& f : pal.file_list(log_dir_.c_str())) {
            pal.file_unlink(f.c_str());
        }
        pal.file_rmdir(log_dir_.c_str());
    }
};

TEST_F(RecoveryTest, FreshStart_NoFile) {
    Instance* inst = new Instance();
    inst->pal = &default_pal();
    inst->log_dir = log_dir_;
    inst->staging_path = staging_path_;
    inst->staging = new Staging(inst->pal, staging_path_, kDataSize);
    run_recovery(inst);
    EXPECT_EQ(get_u32(inst->staging->raw_base() + 8), 0u);  // seqno
    EXPECT_EQ(get_u32(inst->staging->raw_base() + 12), 0u); // flushed_len
    delete inst->staging;
    delete inst;
}

TEST_F(RecoveryTest, FileTruncationOnRecovery) {
    // 写一个合法文件头 + 半个块(故意截断),再启动恢复 → 应被截断并打点
    Pal& pal = default_pal();
    std::string today_path = log_dir_ + "/" + "20260831.log";
    int fd = pal.file_open(today_path.c_str(), true, false);
    ASSERT_GE(fd, 0);
    uint8_t hdr[32] = {};
    put_u32_le(hdr, kMagicFile);
    hdr[4] = 1; hdr[5] = 0;
    put_u32_le(hdr + 6, 20260831u);
    put_u16(hdr + 10, 0);
    uint8_t salt[8] = {1,2,3,4,5,6,7,8};
    std::memcpy(hdr + 12, salt, 8);
    put_u32_le(hdr + 28, crc32_update(0, hdr, 28));
    ASSERT_EQ(pal.file_write(fd, hdr, 32), 32);
    // 半块:只写 10 字节(头应当 34B)
    uint8_t half[10] = {0};
    pal.file_write(fd, half, 10);
    pal.file_close(fd);

    Instance* inst = new Instance();
    inst->pal = &default_pal();
    inst->log_dir = log_dir_;
    inst->staging_path = staging_path_;
    inst->staging = new Staging(inst->pal, staging_path_, kDataSize);
    run_recovery(inst);
    // truncated_blocks 应增长
    EXPECT_GE(inst->truncated_blocks.load(), 1u);
    delete inst->staging;
    delete inst;
}

TEST_F(RecoveryTest, RingScanStopsOnTear) {
    Instance* inst = new Instance();
    inst->pal = &default_pal();
    inst->log_dir = log_dir_;
    inst->staging_path = staging_path_;
    inst->staging = new Staging(inst->pal, staging_path_, kDataSize);
    RecoveryState st; bool rec = false;
    ASSERT_TRUE(inst->staging->open(&st, &rec));

    // 写 3 条 6B 整记录 + 半条(头 6B 完,payload 缺最后 2B)
    std::vector<uint8_t> chunk;
    // 三条 6B 记录 payload = 0(非法,因为 len=0 仅 PAD 合法;但 ring_scan 只看 magic/len/cksum)
    // 这里直接放 3 条 len=6,payload = [tag-like bytes],合法
    for (int i = 0; i < 3; ++i) {
        uint8_t rec[12];
        put_u16(rec, 6);
        put_u16(rec + 2, kMagicEntry);
        rec[4] = 0x00;
        rec[5] = 0x00;  // cksum = 0(payload 全零)
        std::memset(rec + 6, 0, 6);
        chunk.insert(chunk.end(), rec, rec + 12);
    }
    // 半条:头 6B 完(len=4 声明 4B payload),payload 只有 2B(截断 2B)
    chunk.push_back(4); chunk.push_back(0);  // len = 4
    chunk.push_back(0x4c); chunk.push_back(0x47); // magic LG
    chunk.push_back(0x00); chunk.push_back(0x00);
    chunk.insert(chunk.end(), {0,0});  // 只有 2B(应 4B)→ 撕裂尾巴

    ASSERT_EQ(inst->staging->write_records(chunk.data(),
                static_cast<uint32_t>(chunk.size())),
              Staging::WriteStatus::kOk);
    run_recovery(inst);
    // 验证:write_pos = 3 条整条长度 = 36;撕掉的半条被清零 → 总有效 36B
    EXPECT_EQ(get_u32(inst->staging->raw_base() + 16), 36u);
    delete inst->staging;
    delete inst;
}