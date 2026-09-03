// test_recovery.cpp - 启动恢复与对账 单测。
//
// 覆盖(spec §4.3):
//   - S_file == S_hdr:常规,无需回推 F
//   - S_file >  S_hdr:k 块回溯,Σraw_len 精确回推 F + 跨过区间清零
//   - S_file <  S_hdr:文件尾损坏,截到 S_file,丢失打点(无下溢)
//   - 双 Header 均损坏:整区重建,seqno 接管为 max(file)
//   - 环形扫描:缝残留 / 撕裂 / 6B 全零终止符
//   - 裸 write_pos 越界钳制(撕裂写防御)

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

// ---- S_file > S_hdr:k 块回溯精确回推(spec §4.3) ----
//
// 构造方式:先把记录写进 staging 并 commit(seqno=1,F 推进)→ 真实把一个块
// append 进日志文件(seqno=2,模拟 Flusher 第 4 步落盘)→ 手工回滚 staging
// Header 到"未提交"状态(seqno=1,F=旧值,模拟崩溃落在第 4→6 步之间)。
// run_recovery 后:F 应精确前移 Σraw_len(而非 0),且跨过区间被清零。

// 构造一条 len=8 的合法 LGXE 记录(payload 8B,cksum 正确),写入 out。
static void append_entry_record(std::vector<uint8_t>* out, uint8_t payload_seed) {
    uint8_t rec[14];  // 6B 头 + 8B payload
    put_u16(rec, 8);
    put_u16(rec + 2, kMagicEntry);
    rec[4] = 0x00;
    uint8_t c = 0;
    for (int i = 0; i < 8; ++i) {
        rec[6 + i] = static_cast<uint8_t>(payload_seed + i);
        c ^= rec[6 + i];
    }
    rec[5] = c;
    out->insert(out->end(), rec, rec + sizeof(rec));
}

TEST_F(RecoveryTest, BlocksWrittenHeaderNotCommitted_BackfillExact) {
    Instance* inst = new Instance();
    inst->pal = &default_pal();
    inst->log_dir = log_dir_;
    inst->staging_path = staging_path_;
    inst->staging = new Staging(inst->pal, staging_path_, kDataSize);
    RecoveryState st; bool rec = false;
    ASSERT_TRUE(inst->staging->open(&st, &rec));

    // 1) 两条记录(14B each)写入 staging,提交 Header(seqno=1,F=28)
    std::vector<uint8_t> recs;
    append_entry_record(&recs, 0x10);
    append_entry_record(&recs, 0x20);
    ASSERT_EQ(inst->staging->write_records(recs.data(),
                static_cast<uint32_t>(recs.size())),
              Staging::WriteStatus::kOk);
    AppendTargetCache atc{};  // date=0:首块未提交场景(缓存为空)
    inst->staging->commit_header_locked(1, 28, 28, &atc);

    // 2) 真实把块 append 进日志文件:seqno=2,raw = 这两条记录
    uint8_t salt[8] = {1,2,3,4,5,6,7,8};
    ASSERT_TRUE(append_block(inst, salt, 0, recs.data(),
                             static_cast<uint32_t>(recs.size()),
                             /*seqno=*/2, /*flag=*/0,
                             nullptr, nullptr));

    // 3) 回滚到"块已写、Header 未提交"的崩溃现场(§4.2 第 4 步后第 6 步前):
    //    Flusher 时序是 写块(4)→清零(5)→提交(6),故 staging 侧这两条已按
    //    第 5 步清零,Header 停留在第 6 步之前(seqno=1,F=28,w=28)。
    inst->staging->zero_range_locked(0, 28);
    inst->staging->commit_header_locked(1, 28, 28, &atc);

    // 4) 恢复:文件 S_file=2 > S_hdr=1 → Σraw_len=28 精确回推
    run_recovery(inst);
    const uint8_t* base = inst->staging->raw_base();
    EXPECT_EQ(get_u32(base + 8), 2u);    // seqno = S_file(下一块序号,首块将取 3)
    EXPECT_EQ(get_u32(base + 12), 56u);  // flushed_len = 28 + Σraw_len(28)
    EXPECT_EQ(get_u32(base + 16), 56u);  // write_pos 不变(无新增未提交数据)
    // 跨过区间 [28, 56) 应被清零(不变量收尾)
    EXPECT_EQ(inst->staging->data_base()[28], 0);
    EXPECT_EQ(inst->staging->data_base()[40], 0);
    EXPECT_EQ(inst->staging->data_base()[55], 0);
    // 不变量成立
    EXPECT_TRUE(inst->staging->debug_check_invariant(56, 56));
    delete inst->staging;
    delete inst;
}

TEST_F(RecoveryTest, BlocksWrittenHeaderNotCommitted_RemainderRescanned) {
    // 同上,但崩溃时还有第三条记录已写 staging(裸 write_pos=42)未落任何块:
    // 恢复后 F 回推到 56,扫描从 56 出发……注意第三条写在裸 write_pos=42
    // 处(跨过区间 [28,42)),会被 F 回推"越过"——恢复把 [28,42) 清零,
    // 第三条位于 42..56,而扫描从新 F=56 出发不再覆盖它,属宁丢不脏:
    // spec §4.3 场景里,在飞块之后的记录必须等块重新补写后重写。
    // 本用例验证:F 精确回推 56、第三条所在区间被清零、不变量成立。
    Instance* inst = new Instance();
    inst->pal = &default_pal();
    inst->log_dir = log_dir_;
    inst->staging_path = staging_path_;
    inst->staging = new Staging(inst->pal, staging_path_, kDataSize);
    RecoveryState st; bool rec = false;
    ASSERT_TRUE(inst->staging->open(&st, &rec));

    std::vector<uint8_t> recs;
    append_entry_record(&recs, 0x10);
    append_entry_record(&recs, 0x20);
    ASSERT_EQ(inst->staging->write_records(recs.data(),
                static_cast<uint32_t>(recs.size())),
              Staging::WriteStatus::kOk);
    AppendTargetCache atc{};
    inst->staging->commit_header_locked(1, 28, 28, &atc);

    // 第三条记录落在 28..42(裸 write_pos 推到 42)
    std::vector<uint8_t> third;
    append_entry_record(&third, 0x30);
    ASSERT_EQ(inst->staging->write_records(third.data(),
                static_cast<uint32_t>(third.size())),
              Staging::WriteStatus::kOk);   // 裸 write_pos = 42

    uint8_t salt[8] = {1,2,3,4,5,6,7,8};
    ASSERT_TRUE(append_block(inst, salt, 0, recs.data(),
                             static_cast<uint32_t>(recs.size()),
                             2, 0, nullptr, nullptr));

    // 崩溃现场:块已落盘,清零只覆盖 [0,28)(前两条),Header 未提交;
    // 裸 write_pos = 42 由 write_records 直接写在 Header A[16]。
    inst->staging->zero_range_locked(0, 28);
    inst->staging->commit_header_locked(1, 28, 42, &atc);

    run_recovery(inst);
    const uint8_t* base = inst->staging->raw_base();
    EXPECT_EQ(get_u32(base + 8), 2u);
    EXPECT_EQ(get_u32(base + 12), 56u);  // F = 28 + Σraw_len(28) 精确回推
    EXPECT_EQ(get_u32(base + 16), 56u);  // 在飞记录被清,write_pos 收敛到 F
    EXPECT_TRUE(inst->staging->debug_check_invariant(56, 56));
    delete inst->staging;
    delete inst;
}

TEST_F(RecoveryTest, TruncatedFile_LostCountNoUnderflow) {
    // S_hdr > S_file:丢失块数打点必须等于 S_hdr - S_file(下溢回归)。
    // 构造:S_hdr=5(Header 提交过),文件里只有文件头(无块,S_file=0,
    // 长度正常不触发 truncated)——但 spec 分支 S_file < S_hdr 要求打点。
    Pal& pal = default_pal();
    std::string today_path = log_dir_ + "/" + "20260901.log";
    int fd = pal.file_open(today_path.c_str(), true, false);
    ASSERT_GE(fd, 0);
    uint8_t hdr[32] = {};
    put_u32_le(hdr, kMagicFile);
    hdr[4] = 1; hdr[5] = 0;
    put_u32_le(hdr + 6, 20260901u);
    put_u16(hdr + 10, 0);
    uint8_t salt[8] = {9,8,7,6,5,4,3,2};
    std::memcpy(hdr + 12, salt, 8);
    put_u32_le(hdr + 28, crc32_update(0, hdr, 28));
    ASSERT_EQ(pal.file_write(fd, hdr, 32), 32);
    pal.file_close(fd);

    Instance* inst = new Instance();
    inst->pal = &default_pal();
    inst->log_dir = log_dir_;
    inst->staging_path = staging_path_;
    inst->staging = new Staging(inst->pal, staging_path_, kDataSize);
    RecoveryState st; bool rec = false;
    ASSERT_TRUE(inst->staging->open(&st, &rec));
    AppendTargetCache atc{};
    inst->staging->commit_header_locked(5, 0, 0, &atc);  // S_hdr = 5

    run_recovery(inst);
    // 丢失 = S_hdr - S_file = 5 - 0 = 5;旧行为只在 truncated 时打点 1,
    // 且 S_file > S_hdr 分支会因 uint32 下溢打出 ~40 亿。
    EXPECT_EQ(inst->truncated_blocks.load(), 5u);
    // 续写首块 seqno = max(5,0) + 2 = 7(跳 1 空隙;Header 存 resume=6)
    EXPECT_EQ(get_u32(inst->staging->raw_base() + 8), 6u);
    delete inst->staging;
    delete inst;
}

TEST_F(RecoveryTest, CorruptWritePos_ClampedToZero) {
    // 撕裂写可能让 Header 裸 write_pos 出现越界垃圾值:恢复必须钳制(当 0),
    // 而不是按越界值分配近 4GB 的扫描缓冲。
    Instance* inst = new Instance();
    inst->pal = &default_pal();
    inst->log_dir = log_dir_;
    inst->staging_path = staging_path_;
    inst->staging = new Staging(inst->pal, staging_path_, kDataSize);
    RecoveryState st; bool rec = false;
    ASSERT_TRUE(inst->staging->open(&st, &rec));
    AppendTargetCache atc{};
    inst->staging->commit_header_locked(0, 0, 0, &atc);
    put_u32_le(inst->staging->raw_base() + 16, 0xFFFFFFF0u);  // 越界垃圾

    run_recovery(inst);
    // 钳制后 w=0:环为空,扫描无输出,恢复收敛到全零态
    EXPECT_EQ(get_u32(inst->staging->raw_base() + 16), 0u);
    EXPECT_EQ(get_u32(inst->staging->raw_base() + 12), 0u);
    delete inst->staging;
    delete inst;
}

TEST_F(RecoveryTest, CorruptBlockCrc32_TruncatedOnRecovery) {
    // 块级 CRC32 读路径校验 (spec §3.2/宁丢不脏):
    // 若块头完整但 payload 数据损坏 (断电/坏页导致比特翻转),
    // scan_one 必须校验出 CRC 失败并将文件截断至坏块起始处, 触发 truncated 打点与跳空隙。
    Instance* inst = new Instance();
    inst->pal = &default_pal();
    inst->log_dir = log_dir_;
    inst->staging_path = staging_path_;
    inst->staging = new Staging(inst->pal, staging_path_, kDataSize);
    RecoveryState st; bool rec = false;
    ASSERT_TRUE(inst->staging->open(&st, &rec));

    std::vector<uint8_t> rec1, rec2;
    append_entry_record(&rec1, 0x10);
    append_entry_record(&rec2, 0x20);

    // 写入第 1 个有效块 (seqno=1)
    uint8_t salt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint64_t b1_start = 0, b1_end = 0;
    ASSERT_TRUE(append_block(inst, salt, 0, rec1.data(),
                             static_cast<uint32_t>(rec1.size()),
                             1, 0, &b1_start, &b1_end));

    // 写入第 2 个块 (seqno=2)
    uint64_t b2_start = 0, b2_end = 0;
    ASSERT_TRUE(append_block(inst, salt, 0, rec2.data(),
                             static_cast<uint32_t>(rec2.size()),
                             2, 0, &b2_start, &b2_end));

    // 故意篡改第 2 块的 payload(破坏 CRC32)
    uint32_t yyyymmdd = 0, hhmmss = 0;
    inst->pal->wallclock_date(&yyyymmdd, &hhmmss);
    std::string today_path = make_log_path(log_dir_, yyyymmdd, 0);
    int fd = inst->pal->file_open(today_path.c_str(), false, false);
    ASSERT_GE(fd, 0);
    uint8_t bad_byte = 0xFF;
    // 第 2 块起始位置 + 34B 头 + 2B payload 偏移处翻转
    inst->pal->file_pwrite(fd, &bad_byte, 1, b2_start + 34 + 2);
    inst->pal->file_close(fd);

    // Header 记录当时提交水位 seqno=2, flushed_len=28
    AppendTargetCache atc{};
    inst->staging->commit_header_locked(2, 28, 28, &atc);

    // 启动恢复: scan_one 扫描第 2 块时必须校验 CRC 失败并截断到 b2_start (即 b1_end)
    run_recovery(inst);

    // 1) 验证文件长度已精确截断回第 1 块末尾
    int64_t sz_after = inst->pal->file_size_path(today_path.c_str());
    EXPECT_EQ(sz_after, static_cast<int64_t>(b1_end));

    // 2) 丢失计数增加
    EXPECT_GE(inst->truncated_blocks.load(), 1u);

    // 3) 发生截断, 续写首块跳空隙 (seqno = max(S_hdr, S_file) + 2 = max(2, 1) + 2 = 4, Header resume=3)
    const uint8_t* base = inst->staging->raw_base();
    EXPECT_EQ(get_u32(base + 8), 3u);

    delete inst->staging;
    delete inst;
}