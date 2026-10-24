// test_r19_regression.cpp - R19 批次回归测试 + R20/R21 复核回归。
//
// 覆盖(括号内为"撤销该修复 → 本用例复红"的对应关系, R21 已逐条做撤销对照实验):
//   [R19 F-1]  F1_SeamMidStream_SplitsWithinContract
//              回绕缝在拷出缓冲中部 + 缝后积压 > kBlockMaxRaw → 块数 ≥ 2,
//              每块 raw_len ∈ [kHeaderLen, kBlockMaxRaw](写侧产出 == 读侧可收)
//   [R20 F-1]  F1b_SeamAfterFullLengthRecord_SeamAbsorbedIntoBlockTail(切分层:
//              R26 B-9 方案A 后缝并入块尾, 微块在切分层根除; 撤销放宽即复红)
//              + F1c_WrapSeamAtPendingHead_MicroBlockNeverReachesDisk
//              (落盘层: R26 方案B 后缝在 copy_pending 前置消费, 绝不进切块层)
//   [R26 B-9]  F9_MidStreamCorruptSeam_NeverFusedIntoBlock(流中部脏缝: 先封当前
//              块 + 缝残片独立微块, 绝不融合落盘; R26 新增, 撤销内层探测即复红)
//   [R19 F-2]  F2_CloseInsideErrorCallback_SurvivesAndDefers
//              错误回调内 logx_close → 安全返回, 后续 write 不 UAF(旧实现 0xC0000005;
//              撤销表现为崩溃而非断言失败, 故未纳入撤销实验)
//   [R19 F-3 + R20 事件] F3_ShortTornLogFile_SelfHealsAndCounts
//              预置 5B 撕裂 <today>.log → 自愈 + 文件增长 + **按事件文案**断言
//              (只比 kind 会被 recovery.cpp 的同类 RECOVERY 事件顶包, R21 F-4)
//   [R19 F-6]  F6_EncryptWithoutKey_FailFastAndPerBlockExport(open 期 fail-fast /
//              头声明加密但块明文仍可逐块导出)
//   [R21 F-5]  F6b_EncCapabilityLockedAtFileCreation_BlocksStayPlaintext
//              建文件时无钥 → 文件头不置加密位, 且**密钥随后恢复**也不得写出密文块
//   [R19 F-10] F10_FailedOpen_WritesNullOutParam
//   [R19 F-14] F14_RebuiltTruncatedFile_SeqnoSkipsGap(毒化双 Header → 真走 rebuilt
//              分支)+ F14b_NormalPathTruncatedFile_ResumeSeqnoMaxPlusOne(常规路径孪生)
//   [R19 F-4]  F4_WrapSeam_PostSeamRecordsSurviveRecovery
//   [R21 N-2]  ReadSideAcceptanceDomain_*: 读侧 block_header_plausible 接受域直测
//              ("raw_len < kHeaderLen 必拒"是上面全部 F-1 处置的立论依据, 此前零覆盖)
//
// 反"空洞绿"纪律(计划表 §4 R18-2): 本文件每项用例均在 R21 批次做过撤销对照实验,
// 并配正向对照(撤销 R19 F-10 / F-4 确认本 harness 能复红)。R21 之前的版本声称
// "撤销对应修复即可复红"但从未实验, 实测 F1b/F3(事件)/F6(能力锁定)/F14 四项
// 撤销后全绿——详见 code_review_R21_2026-10-05.md §二。

#include "logx_internal.h"
#include "logx_c.h"
#include "test_accessor.h"
#include "logx_staging.h"
#include "logx_codec.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace logx;

namespace {

static std::atomic<uint64_t> s_r19_dir_seq{0};

// 构造一条 LGXE 记录(whole 分片), 布局与生产 format_record 严格一致:
//   [len:2][magic:2][flags:1][cksum:1][payload:len]  → 总长 = kHeaderLen + pl
// cksum 为 payload 的 8 位 XOR 折叠(对齐 format.cpp 的 cksum1;R16 F-1 教训:
// 不能用加法)。R21 N-4 更正: 旧实现把 cksum 追加在 payload **之后**, 与生产布局
// 不符, 合成记录因此不可被 parse_records_stream 解码——凡日后要断言"内容可解"
// 的用例都会失真, 故此处对齐。
std::vector<uint8_t> make_entry(uint32_t pl, uint8_t fill) {
    std::vector<uint8_t> r;
    r.push_back(pl & 0xff);
    r.push_back((pl >> 8) & 0xff);
    r.push_back(static_cast<uint8_t>(kMagicEntry & 0xff));
    r.push_back(static_cast<uint8_t>(kMagicEntry >> 8));
    r.push_back(0x00);              // flags: whole
    uint8_t c = 0;
    for (uint32_t i = 0; i < pl; ++i) c ^= fill;
    r.push_back(c);                 // cksum @ offset 5 (记录头内, 非 payload 尾)
    for (uint32_t i = 0; i < pl; ++i) r.push_back(fill);
    return r;
}

void append_seam(std::vector<uint8_t>& raw, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) raw.push_back(0x00);
}

struct BlockBounds {
    uint32_t min_len = 0xFFFFFFFFu;
    uint32_t max_len = 0;
    uint64_t covered = 0;
    bool any_micro = false;    // 1..kHeaderLen-1 B 块
    bool any_oversize = false;
};
BlockBounds analyze_blocks(const std::vector<PendingBlock>& blocks) {
    BlockBounds b;
    for (const auto& blk : blocks) {
        b.min_len = std::min(b.min_len, blk.raw_len);
        b.max_len = std::max(b.max_len, blk.raw_len);
        b.covered += blk.raw_len;
        if (blk.raw_len > 0 && blk.raw_len < kHeaderLen) b.any_micro = true;
        // R26 B-9: 缝并入块尾后写侧契约上限放宽至 kBlockMaxRawWithSeam(20491),
        // 读侧接受域(block_header_plausible 16MB / zstd 20550 / decoder 20491)均容纳。
        if (blk.raw_len > kBlockMaxRawWithSeam) b.any_oversize = true;
    }
    return b;
}

// 断言"写侧产出全部落在读侧接受域"(跨模块契约,R19 §七·8 门禁)。
void expect_readable_blocks(const std::vector<PendingBlock>& blocks,
                            const std::vector<uint8_t>& raw) {
    BlockBounds bb = analyze_blocks(blocks);
    EXPECT_FALSE(bb.any_micro)
        << "1~5B micro-block: read side rejects raw_len<kHeaderLen (export breaks, repair truncates)";
    EXPECT_FALSE(bb.any_oversize) << "block exceeds kBlockMaxRaw";
    EXPECT_EQ(bb.covered, raw.size()) << "blocks must cover the whole raw stream";
}

std::string make_tmp_dir(const char* tag) {
    Pal& pal = default_pal();
    return std::string(tag) + std::to_string(pal.pid()) + "_" +
           std::to_string(s_r19_dir_seq.fetch_add(1));
}

void cleanup_dir(const std::string& dir) {
    Pal& pal = default_pal();
    for (const auto& f : pal.file_list(dir.c_str())) pal.file_unlink(f.c_str());
    pal.file_rmdir(dir.c_str());
}

// 简单线程安全事件探针(参考 test_j04 ErrorProbe 模式)。
struct EventProbe {
    mutable std::mutex mu;
    std::vector<int> kinds;
    std::vector<std::string> msgs;
    void on_event(logx_event_kind_e k, int /*code*/, const char* msg) {
        std::lock_guard<std::mutex> lk(mu);
        kinds.push_back(static_cast<int>(k));
        msgs.push_back(msg ? msg : "");
    }
    bool saw_kind(int k) const {
        std::lock_guard<std::mutex> lk(mu);
        return std::find(kinds.begin(), kinds.end(), k) != kinds.end();
    }
    // R21 F-4: 只比 kind 的断言会被**同类事件**顶包(F3 的 RECOVERY 断言即被
    // recovery.cpp:308「appended file tail truncated」满足, 撤销被测修复仍绿)。
    // 凡"某个特定站点发了事件"的断言一律走本方法按文案定位。
    bool saw_msg_substring(const char* sub) const {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& m : msgs) if (m.find(sub) != std::string::npos) return true;
        return false;
    }
    size_t count_kind(int k) const {
        std::lock_guard<std::mutex> lk(mu);
        return static_cast<size_t>(std::count(kinds.begin(), kinds.end(), k));
    }
    void clear() {
        std::lock_guard<std::mutex> lk(mu);
        kinds.clear();
        msgs.clear();
    }
};

// 写 32B 合法文件头。
void write_file_header_bytes(int fd, uint32_t date, const uint8_t salt[8]) {
    uint8_t hdr[32] = {};
    put_u32_le(hdr, kMagicFile);
    hdr[4] = 1; hdr[5] = 0;
    put_u32_le(hdr + 6, date);
    put_u16(hdr + 10, 0);
    std::memcpy(hdr + 12, salt, 8);
    put_u32_le(hdr + 28, crc32_update(0, hdr, 28));
    Pal& pal = default_pal();
    ASSERT_EQ(pal.file_write(fd, hdr, 32), 32);
}

// <YYYYMMDD>.log 路径(split 0 无后缀, 与 make_log_path 一致)。
std::string day_log_path(const std::string& dir, uint32_t date) {
    char name[64];
    std::snprintf(name, sizeof(name), "%08u.log", static_cast<unsigned>(date));
    return dir + "/" + name;
}

struct DiskBlock {
    uint64_t off = 0;
    uint32_t seqno = 0;
    uint32_t raw_len = 0;
    uint32_t payload_len = 0;
    uint8_t  flag = 0;
};

// 独立遍历落盘文件的块链: 从 32B 文件头之后逐块取 34B 头, 按 flag 推算块长前进。
// **刻意不复用 block_header_plausible**——用读侧实现去验证读侧结论是循环论证
// (它会在第一个非法块处静默 break, 恰好把缺陷藏起来)。
// 返回 false = 链中途断掉(magic 不符 / 块长越过文件尾 / 尾部有残字节)。
bool scan_disk_blocks(const std::string& path, std::vector<DiskBlock>* out) {
    Pal& pal = default_pal();
    int fd = pal.file_open(path.c_str(), false, false);
    if (fd < 0) return false;
    int64_t sz = pal.file_size(fd);
    if (sz < 32) { pal.file_close(fd); return false; }
    uint64_t off = 32;
    while (off + 34 <= static_cast<uint64_t>(sz)) {
        uint8_t b[34];
        if (pal.file_pread(fd, b, 34, off) != 34) break;
        if (get_u32(b) != kMagicBlock) { pal.file_close(fd); return false; }
        DiskBlock d;
        d.off = off;
        d.flag = b[5];
        d.seqno = get_u32(b + 6);
        d.raw_len = get_u32(b + 10);
        d.payload_len = get_u32(b + 14);
        uint64_t bsize = 34ULL + d.payload_len +
                         ((d.flag & kFlagEncrypted) ? 16ULL : 0ULL);
        if (bsize < 34 || off + bsize > static_cast<uint64_t>(sz)) {
            pal.file_close(fd); return false;
        }
        out->push_back(d);
        off += bsize;
    }
    pal.file_close(fd);
    return off == static_cast<uint64_t>(sz);   // 必须恰好走到文件尾(无残尾)
}

// 构造 34B 块头(读侧接受域直测用; payload/CRC 由调用方按需另行布置)。
void make_block_header(uint8_t out[34], uint32_t seqno, uint32_t raw_len,
                       uint32_t payload_len, uint8_t flag, const uint8_t salt[8]) {
    std::memset(out, 0, 34);
    put_u32_le(out + 0, kMagicBlock);
    out[4] = 1;
    out[5] = flag;
    put_u32_le(out + 6,  seqno);
    put_u32_le(out + 10, raw_len);
    put_u32_le(out + 14, payload_len);
    put_u32_le(out + 18, 0);        // crc: block_header_plausible 不校验 CRC
    std::memcpy(out + 22, salt, 8);
    out[30] = static_cast<uint8_t>((seqno >> 24) & 0xff);
    out[31] = static_cast<uint8_t>((seqno >> 16) & 0xff);
    out[32] = static_cast<uint8_t>((seqno >> 8) & 0xff);
    out[33] = static_cast<uint8_t>(seqno & 0xff);
}

}  // namespace

// ============================================================
// F-1: 回绕缝在缓冲中部(R19 探针 C/D 形态)
// ============================================================
TEST(R19Regression, F1_SeamMidStream_SplitsWithinContract) {
    // 100 条 84B 记录(78B payload) + 3B 缝 + 292 条 → 缝后积压远超
    // kBlockMaxRaw, 旧实现封成单块 32931B(> 20486)(R19 探针 C 原始尺寸),
    // zstd 读侧 kDecompressOutputMax 拒收。
    std::vector<uint8_t> raw;
    for (int i = 0; i < 100; ++i) {
        auto r = make_entry(78, 0xA1);
        raw.insert(raw.end(), r.begin(), r.end());
    }
    append_seam(raw, 3);
    for (int i = 0; i < 292; ++i) {
        auto r = make_entry(78, 0xB2);
        raw.insert(raw.end(), r.begin(), r.end());
    }
    ASSERT_GT(raw.size(), static_cast<size_t>(kBlockMaxRaw));  // 前提: 场景超契约上限
    std::vector<PendingBlock> blocks;
    split_into_blocks(raw.data(), static_cast<uint32_t>(raw.size()), &blocks);
    EXPECT_GE(blocks.size(), 2u) << "seam mid-stream must close a block at the seam";
    expect_readable_blocks(blocks, raw);
}

// ============================================================
// R20 复核: 缝紧随满长记录 → 旧实现切块层产出 1~5B "缝独占块"(R20 探针 P2)。
// R26 B-9(方案A) 落地后缝并入块尾(kBlockMaxRawWithSeam 放宽), 微块在切分层
// 根除——本用例改名并反转断言钉死新契约; 撤销 R26 放宽 → 缝独占微块复现复红。
// (R21 F-2 历史备注: 旧版 NoMicroBlock 的 any_micro 析取恒真断言问题, 已随
//  R21 批次修正; 本轮 R26 契约反转后该备注保留在 CHANGELOG 历史原文中。)
TEST(R19Regression, F1b_SeamAfterFullLengthRecord_SeamAbsorbedIntoBlockTail) {
    std::vector<uint8_t> raw;
    {
        auto r = make_entry(kBlockMaxRaw - kHeaderLen, 0xC3);
        raw.insert(raw.end(), r.begin(), r.end());
    }
    append_seam(raw, 3);
    for (int i = 0; i < 50; ++i) {
        auto r = make_entry(44, 0xD4);
        raw.insert(raw.end(), r.begin(), r.end());
    }
    ASSERT_EQ(raw.size(), static_cast<size_t>(kBlockMaxRaw) + 3 + 50 * 50);
    std::vector<PendingBlock> blocks;
    split_into_blocks(raw.data(), static_cast<uint32_t>(raw.size()), &blocks);

    BlockBounds bb = analyze_blocks(blocks);
    // ① 覆盖完整缓冲、② 无超上限块
    EXPECT_EQ(bb.covered, raw.size());
    EXPECT_FALSE(bb.any_oversize);
    // ③ R26 新契约: 微块在切分层根除——任何块 raw_len 均 ≥ kHeaderLen
    EXPECT_FALSE(bb.any_micro)
        << "R26 B-9: seam must be absorbed into block tail, no 1~5B seam-only block";
    // ④ 块序列形态钉死: [满长记录 20486 + 3B 缝并入][50 条小记录]
    ASSERT_EQ(blocks.size(), 2u);
    EXPECT_EQ(blocks[0].raw_len, static_cast<uint32_t>(kBlockMaxRaw) + 3u);
    EXPECT_EQ(blocks[1].raw_len, 50u * 50u);
    // ⑤ 全部块落在读侧接受域 [kHeaderLen, kBlockMaxRawWithSeam]
    for (const auto& blk : blocks) {
        EXPECT_GE(blk.raw_len, kHeaderLen);
        EXPECT_LE(blk.raw_len, kBlockMaxRawWithSeam);
    }
}

// ============================================================
// R26 B-9(方案A 内层对称探测): 流中部脏缝——缝前仍有未刷记录、待刷窗口
// 跨回绕时, 内层累计循环 rec==0 且 probe_wrap_seam==0(缝含脏字节), 旧实现
// 走撕裂尾兜底把脏字节与后续全部记录融合成单块落盘(R25 独立诊断实证:
// [100条][3B脏缝][50条] → 单块 8103/10903B, 无 RECOVERY、计数 0)。
// R26 修复: 内层命中 corrupt_seam 即 break 封当前块, 缝残片由外层窗口头
// 路径封 s 字节微块交 drain_once 全零校验/RECOVERY 丢弃, 绝不融合。
// 撤销内层探测(恢复 R25 状态) → 融合单块复现 → ④ 形态断言复红。
// ============================================================
TEST(R19Regression, F9_MidStreamCorruptSeam_NeverFusedIntoBlock) {
    std::vector<uint8_t> raw;
    for (int i = 0; i < 100; ++i) {
        auto r = make_entry(78, 0xA1);            // 84B × 100 = 8400B
        raw.insert(raw.end(), r.begin(), r.end());
    }
    append_seam(raw, 3);
    raw[raw.size() - 1] = 0xAA;                   // 位翻转: 末字节打脏, 缝不再全零
    for (int i = 0; i < 50; ++i) {
        auto r = make_entry(44, 0xD4);            // 50B × 50 = 2500B
        raw.insert(raw.end(), r.begin(), r.end());
    }
    ASSERT_EQ(raw.size(), 100u * 84u + 3u + 50u * 50u);
    std::vector<PendingBlock> blocks;
    split_into_blocks(raw.data(), static_cast<uint32_t>(raw.size()), &blocks);

    BlockBounds bb = analyze_blocks(blocks);
    // ① 块链覆盖完整缓冲(融合/丢失都会破坏)
    EXPECT_EQ(bb.covered, raw.size());
    // ② 精确形态钉死: [100条 8400B][3B 脏缝微块][50条 2500B]
    //    旧实现为单块 10903B(融合); 缝并入任一侧亦判伪
    ASSERT_EQ(blocks.size(), 3u);
    EXPECT_EQ(blocks[0].raw_len, 8400u);
    EXPECT_EQ(blocks[1].raw_len, 3u) << "corrupt seam must be pinned as its own micro-block";
    EXPECT_EQ(blocks[2].raw_len, 2500u);
    // ③ 除 3B 微块外全部落在读侧接受域
    for (const auto& blk : blocks) {
        if (blk.raw_len < kHeaderLen) continue;
        EXPECT_GE(blk.raw_len, kHeaderLen);
        EXPECT_LE(blk.raw_len, kBlockMaxRawWithSeam);
    }
}

// ============================================================
// R20 F-1 的**落盘层**防线(R21 新增; R26 B-9 方案B 后防线前移):
// 待刷窗口以 1~5B 留零缝开头时, 缝在 drain_once 的 copy_pending 前置消费
// (锁内清零 + f=0 + 计数), F1c 型窗口头微块不再进入切块层, 缝字节绝不落盘。
// 撤销前置消费分支 → 缝重新进入切块层 → 旧路径产出微块落盘 / R26 放宽后
// 并入块尾(脏缝形态)——两种回归形态均被落盘断言与计数断言双重捕获。
// (块链逐块 raw_len 下界断言 + seam_blocks_skipped 计数断言双重捕获)。
// 场景构造: 填满环到 w == data_size-3 → flush(f 追平 w) → 再写一条 →
// write_records 触发 pad=3<6 留零回绕 → 下一次 drain 的窗口正好以缝开头。
// ============================================================
TEST(R19Regression, F1c_WrapSeamAtPendingHead_MicroBlockNeverReachesDisk) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_r21_f1c_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    uint32_t today = 0, hhmmss = 0;
    pal.wallclock_date(&today, &hhmmss);

    EventProbe probe;
    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 64 * 1024;
    cfg.retention_days = 36500;   // R18-1: 真实 Flusher + 断言依赖文件内容
    cfg.max_file_size = 10 * 1024 * 1024;
    cfg.flush_threshold = 0;
    cfg.compress_algo = 0;
    cfg.encrypt_enable = 0;
    cfg.err_fn = [](void* user, logx_event_kind_e k, int c, const char* m) {
        static_cast<EventProbe*>(user)->on_event(k, c, m);
    };
    cfg.err_user = &probe;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    Instance* ii = logx::test::test_get_instance(inst);
    const uint32_t data_size = ii->staging->data_size();
    auto w_now = [&]() { return get_u32(ii->staging->raw_base() + 16); };
    auto f_now = [&]() { return get_u32(ii->staging->raw_base() + 12); };

    // 1) 探测单条记录的落环长度与固定开销(同 tag/level 下 record = msg + overhead)
    const uint32_t kMsg = 512;
    const char* kTag = "R21F1C";
    std::vector<uint8_t> msg(kMsg, 'M');
    ASSERT_EQ(w_now(), 0u);
    ASSERT_EQ(logx_write(inst, 2, kTag, msg.data(), kMsg), LOGX_OK);
    const uint32_t rec_len = w_now();
    ASSERT_GT(rec_len, kMsg + kHeaderLen);
    const uint32_t overhead = rec_len - kMsg;

    // 2) 精确填到 w == data_size - 3(留 3B 环尾 → 下一次写入必触发 pad=3<6 留零回绕)
    const uint32_t kSeam = 3;
    const uint32_t budget = data_size - kSeam - rec_len;   // 已写一条 rec_len
    ASSERT_GT(budget, overhead + 1);
    const uint32_t n_full = (budget - (overhead + 1)) / rec_len;
    for (uint32_t i = 0; i < n_full; ++i) {
        ASSERT_EQ(logx_write(inst, 2, kTag, msg.data(), kMsg), LOGX_OK);
    }
    ASSERT_EQ(w_now(), rec_len * (n_full + 1));            // 线性假设校验
    const uint32_t tail = budget - n_full * rec_len;       // >= overhead+1
    ASSERT_GE(tail, overhead + 1);
    std::vector<uint8_t> tail_msg(tail - overhead, 'T');
    ASSERT_EQ(logx_write(inst, 2, kTag, tail_msg.data(),
                         static_cast<uint32_t>(tail_msg.size())), LOGX_OK);
    ASSERT_EQ(w_now(), data_size - kSeam)
        << "fill must land exactly 3B short of the ring end to force a pad<6 wrap";

    // 3) flush → f 追平 w, 待刷窗口清空且起点停在缝前
    ASSERT_EQ(logx_flush(inst), LOGX_OK);
    ASSERT_EQ(f_now(), data_size - kSeam);
    ASSERT_EQ(w_now(), data_size - kSeam);

    // 4) 再写一条 → pad=3<6 → 留零缝 + 回绕, 记录落环首
    const uint64_t skipped_before = ii->seam_blocks_skipped.load();
    ASSERT_EQ(logx_write(inst, 2, kTag, msg.data(), kMsg), LOGX_OK);
    ASSERT_EQ(w_now(), rec_len);                           // 已回绕到环首
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    // 5) 跳过分支确实被执行(场景有效性钉死: 撤销修复后此计数为 0 → 复红)
    EXPECT_EQ(ii->seam_blocks_skipped.load() - skipped_before, 1u)
        << "the pad<6 wrap must produce exactly one seam-only block for drain_once to skip";
    // 全零前提成立(R21 F-7): 不得触发"非缝填充"告警
    EXPECT_FALSE(probe.saw_msg_substring("not seam padding"));

    // 6) 落盘文件的每个块都必须落在读侧接受域内(独立遍历, 不经 plausible)
    std::string logfile = day_log_path(dir, today);
    std::vector<DiskBlock> blks;
    ASSERT_TRUE(scan_disk_blocks(logfile, &blks))
        << "block chain must walk cleanly to EOF (no torn/mis-sized block)";
    ASSERT_GE(blks.size(), 2u);
    for (const auto& b : blks) {
        EXPECT_GE(b.raw_len, kHeaderLen)
            << "micro-block reached disk: read side rejects raw_len<kHeaderLen "
               "(export breaks there, repair truncates everything after)";
        EXPECT_LE(b.raw_len, kBlockMaxRaw);
    }
    // 缝后那条记录必须真的落盘(旧实现下它排在 3B 块之后, 读侧到 3B 块即 break)
    EXPECT_EQ(blks.back().raw_len, rec_len);
    // seqno 连续无洞(跳过缝不消耗序号)
    for (size_t i = 1; i < blks.size(); ++i) {
        EXPECT_EQ(blks[i].seqno, blks[i - 1].seqno + 1) << "seqno must stay contiguous";
    }

    // 7) 导出可取回
    std::atomic<int> files_with_content{0};
    ASSERT_EQ(logx_export(inst, "20200101", "20991231",
        [](void* user, const char*, const char*, uint64_t size) -> int {
            if (size > 0) static_cast<std::atomic<int>*>(user)->fetch_add(1);
            return 0;
        }, &files_with_content), LOGX_OK);
    EXPECT_GE(files_with_content.load(), 1);

    logx_close(inst);
    cleanup_dir(dir);
}

// ============================================================
// F-2: 错误回调内 logx_close(契约许可的唯一回调内接口)
// R19 探针 F 实测旧实现 0xC0000005;修复后递延销毁。
// ============================================================
TEST(R19Regression, F2_CloseInsideErrorCallback_SurvivesAndDefers) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_r19_f2_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 4096;
    cfg.retention_days = 7;
    cfg.max_file_size = 10 * 1024 * 1024;
    cfg.flush_threshold = 0;  // 禁自动唤醒: TRUNCATED 事件在调用方线程同步发出
    cfg.compress_algo = 0;
    cfg.encrypt_enable = 0;

    struct Ctx {
        logx_instance_t* inst = nullptr;
        std::atomic<bool> closed_in_cb{false};
        std::atomic<int> truncated_events{0};
    } ctx;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    ctx.inst = inst;

    logx_set_error_handler(inst, [](void* user, logx_event_kind_e kind, int code, const char* msg) {
        (void)code; (void)msg;
        auto* c = static_cast<Ctx*>(user);
        if (kind == LOGX_EV_TRUNCATED && !c->closed_in_cb.exchange(true)) {
            // 契约许可: 错误回调内唯一可调用接口
            EXPECT_EQ(logx_close(c->inst), LOGX_OK);
        }
        if (kind == LOGX_EV_TRUNCATED) c->truncated_events.fetch_add(1);
    }, &ctx);

    // 每条均超 max_log_length → 调用方线程同步发 TRUNCATED → 首条回调内 close。
    // 旧实现在 close 返回后继续使用已释放内存 → 0xC0000005;
    // 修复后 close 递延, write 安全返回。
    std::vector<uint8_t> payload(4096, 'X');
    for (int i = 0; i < 50; ++i) {
        logx_status_e rc = logx_write(inst, 3, "R19F2", payload.data(),
                                      static_cast<uint32_t>(payload.size()));
        EXPECT_TRUE(rc == LOGX_OK || rc == LOGX_ERR_STATE)
            << "write after deferred close must return safely (no UAF), got " << rc;
    }
    EXPECT_GE(ctx.truncated_events.load(), 1);
    if (!ctx.closed_in_cb.load()) {
        logx_close(inst);  // 未触发路径的保守兜底
    }
    cleanup_dir(dir);
}

// ============================================================
// J-1 / F-2: 生命周期闭环与 on_destroy 契约
// 1) 同步关闭: logx_close_ex 返回 true, on_destroy 在 close 返回前触发
// 2) 递延关闭: 回调内 logx_close_ex 返回 false, on_destroy 在退栈后触发
// ============================================================
TEST(R19Regression, J1_F2_OnDestroySynchronous) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_j1_f2_sync_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    struct DestroyCtx {
        std::atomic<bool> destroyed{false};
        void* got_user = nullptr;
        void* got_err_user = nullptr;
    };
    static DestroyCtx dctx;
    dctx.destroyed.store(false);
    dctx.got_user = nullptr;
    dctx.got_err_user = nullptr;

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 4096;
    cfg.retention_days = 7;
    cfg.user = &dctx;
    cfg.err_user = &dctx;
    cfg.on_destroy = [](void* u, void* eu) {
        auto* dc = static_cast<DestroyCtx*>(u);
        if (dc) {
            dc->got_user = u;
            dc->got_err_user = eu;
            dc->destroyed.store(true);
        }
    };

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    bool closed_immediately = false;
    EXPECT_EQ(logx_close_ex(inst, &closed_immediately), LOGX_OK);
    EXPECT_TRUE(closed_immediately);
    EXPECT_TRUE(dctx.destroyed.load());
    EXPECT_EQ(dctx.got_user, &dctx);
    EXPECT_EQ(dctx.got_err_user, &dctx);

    cleanup_dir(dir);
}

TEST(R19Regression, J1_F2_DeferredCloseLifetimeAndOnDestroy) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_j1_f2_defer_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    struct TestState {
        logx_instance_t* inst = nullptr;
        std::atomic<bool> closed_in_cb{false};
        std::atomic<bool> closed_immediately{true};
        std::atomic<bool> on_destroy_called_inside_cb{false};
        std::atomic<bool> on_destroy_called_finally{false};
        std::atomic<bool> in_callback{false};
    } state;

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 4096;
    cfg.flush_threshold = 0;
    cfg.user = &state;
    cfg.on_destroy = [](void* user, void* /*err_user*/) {
        auto* s = static_cast<TestState*>(user);
        if (s->in_callback.load()) {
            s->on_destroy_called_inside_cb.store(true);
        }
        s->on_destroy_called_finally.store(true);
    };

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    state.inst = inst;

    logx_set_error_handler(inst, [](void* user, logx_event_kind_e kind, int code, const char* msg) {
        (void)code; (void)msg;
        auto* s = static_cast<TestState*>(user);
        if (kind == LOGX_EV_TRUNCATED && !s->closed_in_cb.exchange(true)) {
            s->in_callback.store(true);
            bool closed_now = true;
            EXPECT_EQ(logx_close_ex(s->inst, &closed_now), LOGX_OK);
            s->closed_immediately.store(closed_now);
            s->in_callback.store(false);
        }
    }, &state);

    std::vector<uint8_t> payload(4096, 'Y');
    logx_status_e rc = logx_write(inst, 3, "J1F2", payload.data(), static_cast<uint32_t>(payload.size()));
    EXPECT_TRUE(rc == LOGX_OK || rc == LOGX_ERR_STATE);

    EXPECT_TRUE(state.closed_in_cb.load());
    EXPECT_FALSE(state.closed_immediately.load());
    EXPECT_FALSE(state.on_destroy_called_inside_cb.load());
    EXPECT_TRUE(state.on_destroy_called_finally.load());

    cleanup_dir(dir);
}

// ============================================================
// R41-01 / ACT-1: 递延 close 与并发写竞态无 UAF 压力测试
// 验证: 外部写线程并发写入时, 触发递延 close, flusher 绝不提前自毁,
// 递延销毁由写线程全部退栈后安全认领触发 on_destroy, 杜绝 UAF。
// ============================================================
TEST(R19Regression, R41_DeferredClose_ConcurrentWrites_NoUAF) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_r41_act1_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    for (int iter = 0; iter < 10; ++iter) {
        struct TestState {
            std::atomic<bool> destroyed{false};
            std::atomic<bool> stop_writers{false};
        } state;

        logx_config_t cfg{};
        cfg.log_dir = dir.c_str();
        cfg.staging_path = staging.c_str();
        cfg.max_log_length = 4096;
        cfg.flush_threshold = 0;
        cfg.user = &state;
        cfg.on_destroy = [](void* user, void* /*err_user*/) {
            auto* s = static_cast<TestState*>(user);
            if (s) {
                s->destroyed.store(true, std::memory_order_release);
            }
        };

        logx_instance_t* inst = nullptr;
        ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

        constexpr int kNumThreads = 4;
        std::vector<std::thread> writers;
        writers.reserve(kNumThreads);

        std::vector<uint8_t> payload(64, 'X');
        for (int i = 0; i < kNumThreads; ++i) {
            writers.emplace_back([inst, &state, &payload]() {
                while (!state.stop_writers.load(std::memory_order_relaxed)) {
                    logx_status_e rc = logx_write(inst, 3, "ACT1", payload.data(), static_cast<uint32_t>(payload.size()));
                    if (rc != LOGX_OK && rc != LOGX_ERR_STATE) {
                        break;
                    }
                }
            });
        }

        // 让写线程跑片刻产生持续的 active_ops
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

        // 触发 close, 此刻高概率命中 active_ops > 0 走递延关闭
        bool closed_immediately = false;
        EXPECT_EQ(logx_close_ex(inst, &closed_immediately), LOGX_OK);

        // 通知写线程退出循环
        state.stop_writers.store(true, std::memory_order_release);

        for (auto& t : writers) {
            if (t.joinable()) t.join();
        }

        // 验证 on_destroy 最终必须被执行(递延闭环无泄漏)
        EXPECT_TRUE(state.destroyed.load(std::memory_order_acquire));
    }

    cleanup_dir(dir);
}

#if LOGX_WITH_AESGCM
// ============================================================
// R43-01 / ACT-1: close_pending Flusher 盲区泄漏回归探针。
// 构造"最后持有者恰为 Flusher 线程"(active_ops==0 且 in_callback 全由
// Flusher 自身回调贡献): open 后置 armed 使 key_provider 从此返回 NULL;
// 小 max_file_size + 16KB 写 → Flusher drain 时轮转建新文件 → 建文件探测
// 缺钥发 DEGRADED(该探测点运行在 Flusher 线程), err_fn 仅在非主线程事件
// 上阻塞把 Flusher 扣在回调栈内; 主线程(写已退栈)此刻 logx_close_ex
// 走 close_pending 递延。
// 修复前: emit_event 尾段 on_flusher 拒领、flusher_main 退出段只消费
// close_on_flusher → close_pending 永无人认领 → on_destroy 永不执行(泄漏);
// 修复后: emit_event 尾段消费 close_pending 转置 close_on_flusher 移交,
// 退出段自毁, on_destroy 必执行。
// ============================================================
TEST(R19Regression, R43_DeferredClose_FlusherCallbackBlindSpot_OnDestroyRuns) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_r43_blind_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    struct SyncState {
        std::thread::id main_tid{};
        std::atomic<bool> armed{false};          // open 后置位: 从此探测一律缺钥
        std::atomic<bool> flusher_in_cb{false};
        std::atomic<bool> release_cb{false};
        std::atomic<bool> blocked_once{false};
        std::atomic<bool> destroyed{false};
    } state;
    state.main_tid = std::this_thread::get_id();

    struct KeyCtx {
        uint8_t key[32];
        SyncState* st;                           // 非捕获 lambda 经 user 指针取 state
    } kctx;
    for (int i = 0; i < 32; ++i) kctx.key[i] = static_cast<uint8_t>(0x50 + i);
    kctx.st = &state;

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 64 * 1024;
    cfg.retention_days = 36500;
    cfg.max_file_size = 4096;              // 16KB 块必然触发 Flusher drain 中轮转建新文件
    cfg.flush_threshold = 4096;            // 超阈值异步唤醒 Flusher(写退栈后 drain)
    cfg.compress_algo = 0;
    cfg.encrypt_enable = 1;
    cfg.kid = 43;
    cfg.user = &kctx;
    cfg.key_provider = [](uint16_t, void* u) -> const uint8_t* {
        auto* c = static_cast<KeyCtx*>(u);
        // open 期探测给钥(目标文件建在 open 阶段, 主线程); armed 后一律缺钥
        // → Flusher 轮转建文件探测缺钥 → DEGRADED on Flusher
        return c->st->armed.load(std::memory_order_acquire) ? nullptr : c->key;
    };
    cfg.err_user = &state;
    cfg.err_fn = [](void* u, logx_event_kind_e kind, int /*code*/, const char* /*msg*/) {
        auto* s = static_cast<SyncState*>(u);
        if (kind != LOGX_EV_DEGRADED) return;
        // 只扣住 Flusher 线程(非主线程)的首个 DEGRADED; open 期主线程事件放行
        if (s->main_tid == std::this_thread::get_id()) return;
        if (s->blocked_once.exchange(true)) return;
        s->flusher_in_cb.store(true);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!s->release_cb.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    cfg.on_destroy = [](void* /*user*/, void* eu) {
        static_cast<SyncState*>(eu)->destroyed.store(true);
    };

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    // open 完成(目标文件已建、密钥已锁) → 从此缺钥
    state.armed.store(true, std::memory_order_release);

    std::vector<uint8_t> payload(16 * 1024, 'B');
    ASSERT_EQ(logx_write(inst, 3, "R43", payload.data(),
                         static_cast<uint32_t>(payload.size())), LOGX_OK);

    // 写线程已退栈(active_ops==0); 等 Flusher 进入轮转建文件缺钥回调栈
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!state.flusher_in_cb.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(state.flusher_in_cb.load())
        << "Flusher 必须进入轮转建文件 missing-key DEGRADED 回调(用例前提失效)";

    // 命中盲区: in_callback==1(Flusher 贡献) && active_ops==0 → 递延 close
    bool closed_immediately = true;
    EXPECT_EQ(logx_close_ex(inst, &closed_immediately), LOGX_OK);
    EXPECT_FALSE(closed_immediately);

    // 放行 Flusher 回调: emit_event 尾段应消费 close_pending 并转置 close_on_flusher 移交
    state.release_cb.store(true);

    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!state.destroyed.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(state.destroyed.load())
        << "R43-01: close_pending Flusher 盲区——on_destroy 永不执行, 实例与 JNI 上下文泄漏";

    cleanup_dir(dir);
}
#endif

// ============================================================
// F-3: 0<sz<32 撕裂文件头自愈(R19 探针 G 形态: 旧实现 flush=OK、
// 文件永不增长、计数器全 0)
// ============================================================
TEST(R19Regression, F3_ShortTornLogFile_SelfHealsAndCounts) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_r19_f3_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    // 预置 5B 撕裂 <today>.log(模拟断电撕裂的文件头)
    uint32_t today = 0, hhmmss = 0;
    pal.wallclock_date(&today, &hhmmss);
    char name[64];
    std::snprintf(name, sizeof(name), "%08u.log", static_cast<unsigned>(today));
    std::string torn = dir + "/" + name;
    int fd = pal.file_open(torn.c_str(), true, false);
    ASSERT_GE(fd, 0);
    const char five[5] = {'L','G','X','F','x'};
    ASSERT_EQ(pal.file_write(fd, five, 5), 5);
    pal.file_close(fd);

    EventProbe probe;
    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 64 * 1024;
    cfg.retention_days = 7;
    cfg.max_file_size = 10 * 1024 * 1024;
    cfg.flush_threshold = 0;
    cfg.compress_algo = 0;
    cfg.encrypt_enable = 0;
    cfg.err_fn = [](void* user, logx_event_kind_e k, int c, const char* m) {
        static_cast<EventProbe*>(user)->on_event(k, c, m);
    };
    cfg.err_user = &probe;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    // 100 条写入 + flush: 修复前 append target 永久打开失败, flush 报 OK 但文件仍 5B
    std::vector<uint8_t> payload(512, 'B');
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(logx_write(inst, 2, "R19F3", payload.data(),
                             static_cast<uint32_t>(payload.size())), LOGX_OK);
    }
    EXPECT_EQ(logx_flush(inst), LOGX_OK);

    int64_t sz = pal.file_size_path(torn.c_str());
    EXPECT_GT(sz, 32) << "torn 5B header must be truncated and rewritten; file must grow";

    // 自愈信号必须可见(R19 探针 G 的教训: 修复前计数器全 0 + 无任何事件)。
    // R21 F-4 更正: 必须按**文案**断言。只比 kind 时, recovery.cpp:308 的
    // 「appended file tail truncated」同样是 LOGX_EV_RECOVERY, 会把断言顶包——
    // 实测撤销 scan_one 的新事件后旧断言仍然绿。
    EXPECT_TRUE(probe.saw_msg_substring("truncated to empty for rewrite"))
        << "healing a torn <32B header must emit a site-specific RECOVERY event "
           "(no silent heals); kind-only matching is satisfied by unrelated RECOVERY events";

    // 导出可取回数据(修复前 export=-3)
    std::atomic<int> cb_files{0};
    logx_status_e erc = logx_export(inst, "20200101", "20991231",
        [](void* user, const char*, const char*, uint64_t) -> int {
            static_cast<std::atomic<int>*>(user)->fetch_add(1);
            return 0;
        }, &cb_files);
    EXPECT_EQ(erc, LOGX_OK);
    EXPECT_GE(cb_files.load(), 1) << "healed file must be exportable";

    logx_close(inst);
    cleanup_dir(dir);
}

// ============================================================
// F-6: ① encrypt=1 + 密钥恒缺 → open 期 fail-fast + *out_inst=NULL;
//      ② 文件头声明加密但块明文 → 导出逐块判定仍可解(R19 修复②,
//         同时守护 R20 残留修复的"头位与能力一致"决策不回退)
// ============================================================
TEST(R19Regression, F6_EncryptWithoutKey_FailFastAndPerBlockExport) {
    Pal& pal = default_pal();
    // —— ① open 期 fail-fast ——
    {
        std::string dir = make_tmp_dir("logx_test_r19_f6a_");
        pal.file_mkdirs(dir.c_str());
        std::string staging = dir + "/staging.mmap";
        logx_config_t cfg{};
        cfg.log_dir = dir.c_str();
        cfg.staging_path = staging.c_str();
        cfg.max_log_length = 64 * 1024;
        cfg.retention_days = 7;
        cfg.max_file_size = 10 * 1024 * 1024;
        cfg.flush_threshold = 0;
        cfg.compress_algo = 0;
        cfg.encrypt_enable = 1;
        cfg.kid = 7;
        cfg.key_provider = [](uint16_t, void*) -> const uint8_t* { return nullptr; };

        logx_instance_t* inst = nullptr;
        logx_status_e rc = logx_open(&cfg, &inst);
        EXPECT_NE(rc, LOGX_OK) << "encrypt=1 + provider always-NULL must fail fast at open";
        EXPECT_EQ(inst, nullptr) << "failed open must null the out param (F-10)";
        cleanup_dir(dir);
    }

    // —— ② 头声明加密 + 块明文 → 导出逐块解出 ——
    // 构造: 明文实例写入落盘, 手工把文件头加密位置 1(模拟降级/历史文件),
    // 导出必须输出行而非整文件 [skipped](R19 探针 H 形态)。
    {
        std::string dir = make_tmp_dir("logx_test_r19_f6b_");
        pal.file_mkdirs(dir.c_str());
        std::string staging = dir + "/staging.mmap";
        logx_config_t cfg2{};
        cfg2.log_dir = dir.c_str();
        cfg2.staging_path = staging.c_str();
        cfg2.max_log_length = 64 * 1024;
        cfg2.retention_days = 7;
        cfg2.max_file_size = 10 * 1024 * 1024;
        cfg2.flush_threshold = 0;
        cfg2.compress_algo = 0;
        cfg2.encrypt_enable = 0;

        logx_instance_t* inst2 = nullptr;
        ASSERT_EQ(logx_open(&cfg2, &inst2), LOGX_OK);
        std::vector<uint8_t> payload(128, 'P');
        for (int i = 0; i < 10; ++i) {
            ASSERT_EQ(logx_write(inst2, 2, "F6B", payload.data(),
                                 static_cast<uint32_t>(payload.size())), LOGX_OK);
        }
        ASSERT_EQ(logx_flush(inst2), LOGX_OK);

        uint32_t today = 0, hhmmss2 = 0;
        pal.wallclock_date(&today, &hhmmss2);
        char name2[64];
        std::snprintf(name2, sizeof(name2), "%08u.log", static_cast<unsigned>(today));
        std::string logfile = dir + "/" + name2;

        // 读 32B 头, 置加密位, 原位回写(pwrite 语义)
        int fd = pal.file_open(logfile.c_str(), true, false);
        ASSERT_GE(fd, 0);
        uint8_t hdr[32];
        ASSERT_EQ(pal.file_pread(fd, hdr, 32, 0), 32);
        hdr[5] |= 0x01;  // 置加密位(块实为明文)
        put_u32_le(hdr + 28, crc32_update(0, hdr, 28));
        ASSERT_EQ(pal.file_pwrite(fd, hdr, 32, 0), 32);
        pal.file_close(fd);

        std::atomic<int> files_with_content{0};
        logx_status_e erc2 = logx_export(inst2, "20200101", "20991231",
            [](void* user, const char*, const char*, uint64_t size) -> int {
                if (size > 0) static_cast<std::atomic<int>*>(user)->fetch_add(1);
                return 0;
            }, &files_with_content);
        EXPECT_EQ(erc2, LOGX_OK);
        EXPECT_GE(files_with_content.load(), 1)
            << "plaintext blocks in an encrypted-declared file must still be decoded per-block";
        logx_close(inst2);
        cleanup_dir(dir);
    }
}

#if LOGX_WITH_AESGCM
// ============================================================
// R21 F-5: 文件级加密能力锁定(堵 R20 只堵了一半的反向竞态)。
// 有状态 key_provider: 第 1 次调用(open 期探测)给钥 → open 成功;
// 第 2 次(建文件探测)返回 NULL → 文件头不置加密位 + 本文件能力锁定为明文;
// 第 3 次起(drain 逐块取 key / export)恢复给钥。
// 修复前: drain_once 的 flag 只看 inst->encrypt_enable, 于是取到钥就写**密文块**,
// 形成"文件头声明明文、块实为密文"。撤销能力锁定(把 flag 改回
// `make_flag(algo, encrypt_enable != 0)`)→ 块头 bit0=1 → 本用例复红。
// ============================================================
TEST(R19Regression, F6b_EncCapabilityLockedAtFileCreation_BlocksStayPlaintext) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_r21_f6b_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    struct KeyCtx {
        std::atomic<int> calls{0};
        uint8_t key[32];
    } kctx;
    for (int i = 0; i < 32; ++i) kctx.key[i] = static_cast<uint8_t>(0xA0 + i);

    EventProbe probe;
    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 64 * 1024;
    cfg.retention_days = 36500;
    cfg.max_file_size = 10 * 1024 * 1024;
    cfg.flush_threshold = 0;
    cfg.compress_algo = 0;
    cfg.encrypt_enable = 1;
    cfg.kid = 11;
    cfg.user = &kctx;
    cfg.key_provider = [](uint16_t, void* user) -> const uint8_t* {
        auto* c = static_cast<KeyCtx*>(user);
        int n = c->calls.fetch_add(1) + 1;
        return (n == 2) ? nullptr : c->key;   // 仅"建文件那一刻"取不到密钥
    };
    cfg.err_fn = [](void* user, logx_event_kind_e k, int code, const char* m) {
        static_cast<EventProbe*>(user)->on_event(k, code, m);
    };
    cfg.err_user = &probe;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    std::vector<uint8_t> payload(128, 'K');
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(logx_write(inst, 2, "R21F6B", payload.data(),
                             static_cast<uint32_t>(payload.size())), LOGX_OK);
    }
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    // 探测窗口对齐校验: 建文件时刻确实拿到了 NULL(否则本用例测的不是锁定语义)
    EXPECT_TRUE(probe.saw_msg_substring("key unavailable at file creation"))
        << "provider call window drifted; this test no longer exercises capability locking";

    uint32_t today = 0, hhmmss = 0;
    pal.wallclock_date(&today, &hhmmss);
    std::string logfile = day_log_path(dir, today);

    // ① 文件头不声明加密(R20 修复)
    int fd = pal.file_open(logfile.c_str(), false, false);
    ASSERT_GE(fd, 0);
    uint8_t fhdr[32];
    ASSERT_EQ(pal.file_pread(fd, fhdr, 32, 0), 32);
    pal.file_close(fd);
    EXPECT_EQ(get_u32(fhdr), kMagicFile);
    EXPECT_EQ(fhdr[5] & kFlagEncrypted, 0)
        << "file header must not declare encryption when the key was unavailable at creation";

    // ② 本文件**所有块**也必须是明文(R21 能力锁定; 修复前这里会是密文块)
    std::vector<DiskBlock> blks;
    ASSERT_TRUE(scan_disk_blocks(logfile, &blks));
    ASSERT_FALSE(blks.empty());
    for (const auto& b : blks) {
        EXPECT_EQ(b.flag & kFlagEncrypted, 0)
            << "capability is locked per file: a plaintext-declared file must not "
               "contain encrypted blocks even after the key becomes available again";
        EXPECT_GE(b.raw_len, kHeaderLen);
    }

    // ③ 明文块可正常导出
    std::atomic<int> files_with_content{0};
    ASSERT_EQ(logx_export(inst, "20200101", "20991231",
        [](void* user, const char*, const char*, uint64_t size) -> int {
            if (size > 0) static_cast<std::atomic<int>*>(user)->fetch_add(1);
            return 0;
        }, &files_with_content), LOGX_OK);
    EXPECT_GE(files_with_content.load(), 1);

    logx_close(inst);
    cleanup_dir(dir);
}

// ============================================================
// R24 §7.2 方案 C: ActiveFileTarget 聚合与 key_provider 上下文内聚。
// 验证 key_provider 调用次数被严格限制在"每文件恰好一次"契约下:
// - logx_open 期: 启动校验探测 1 次;
// - 写入 20 条记录并触发多轮 flush (落盘到同一文件): 累计恰好 2 次
//   (1 open 探活 + 1 建文件锁定; drain_once 消费 target.key, 绝不重复调用);
// - logx_export 读取该文件: 累计恰好 3 次 (每文件读取 1 次)。
// 同时断言落盘块均为加密块且导出内容完整可读(全部 20 行解出)。
// ============================================================
TEST(R19Regression, SchemeC_ActiveFileTarget_KeyProviderCalledOncePerFile) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_r24_schemec_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    struct KeyCtx {
        std::atomic<int> calls{0};
        uint8_t key[32];
    } kctx;
    for (int i = 0; i < 32; ++i) kctx.key[i] = static_cast<uint8_t>(0xB0 + i);

    EventProbe probe;
    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 64 * 1024;
    cfg.retention_days = 7;
    cfg.max_file_size = 10 * 1024 * 1024;
    cfg.flush_threshold = 0;
    cfg.compress_algo = 0;
    cfg.encrypt_enable = 1;
    cfg.kid = 42;
    cfg.user = &kctx;
    cfg.key_provider = [](uint16_t, void* user) -> const uint8_t* {
        auto* c = static_cast<KeyCtx*>(user);
        c->calls.fetch_add(1);
        return c->key;
    };
    cfg.err_fn = [](void* user, logx_event_kind_e k, int code, const char* m) {
        static_cast<EventProbe*>(user)->on_event(k, code, m);
    };
    cfg.err_user = &probe;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    // 1. open 期探测恰好 1 次
    EXPECT_EQ(kctx.calls.load(), 1)
        << "logx_open boot probe must query key_provider exactly once";

    std::vector<uint8_t> payload(128, 'C');
    // 第一批: 写入 10 条并 flush
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(logx_write(inst, 2, "SCHEMEC", payload.data(),
                             static_cast<uint32_t>(payload.size())), LOGX_OK);
    }
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    // 第二批: 写入 10 条并再次 flush (跨轮 drain)
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(logx_write(inst, 2, "SCHEMEC", payload.data(),
                             static_cast<uint32_t>(payload.size())), LOGX_OK);
    }
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    // 2. 累计调用恰好 2 次 (1 open + 1 建文件锁定; drain_once 消费 target.key, 绝不重复查询)
    EXPECT_EQ(kctx.calls.load(), 2)
        << "ActiveFileTarget must lock key once per file; drain_once must not re-query key_provider";

    uint32_t today = 0, hhmmss = 0;
    pal.wallclock_date(&today, &hhmmss);
    std::string logfile = day_log_path(dir, today);

    // 验证文件头声明加密且落盘块均为加密块
    int fd = pal.file_open(logfile.c_str(), false, false);
    ASSERT_GE(fd, 0);
    uint8_t fhdr[32];
    ASSERT_EQ(pal.file_pread(fd, fhdr, 32, 0), 32);
    pal.file_close(fd);
    EXPECT_EQ(get_u32(fhdr), kMagicFile);
    EXPECT_NE(fhdr[5] & kFlagEncrypted, 0)
        << "file header must declare encryption when key is provided";

    std::vector<DiskBlock> blks;
    ASSERT_TRUE(scan_disk_blocks(logfile, &blks));
    ASSERT_FALSE(blks.empty());
    for (const auto& b : blks) {
        EXPECT_NE(b.flag & kFlagEncrypted, 0)
            << "blocks written with cached key must be encrypted";
    }

    // 3. 导出该文件: key_provider 恰好再被调用 1 次 (累计 3 次)
    struct ExportCtx {
        int files = 0;
        int lines = 0;
    } exp_ctx;

    logx_status_e erc = logx_export(inst, "20200101", "20991231",
        [](void* user, const char*, const char* path, uint64_t size) -> int {
            auto* c = static_cast<ExportCtx*>(user);
            if (size > 0) {
                c->files++;
                Pal& p = default_pal();
                int exp_fd = p.file_open(path, false, false);
                if (exp_fd >= 0) {
                    std::vector<char> buf(size);
                    p.file_pread(exp_fd, buf.data(), size, 0);
                    p.file_close(exp_fd);
                    for (uint64_t i = 0; i < size; ++i) {
                        if (buf[i] == '\n') c->lines++;
                    }
                }
            }
            return 0;
        }, &exp_ctx);
    EXPECT_EQ(erc, LOGX_OK);
    EXPECT_EQ(exp_ctx.files, 1);
    EXPECT_EQ(exp_ctx.lines, 20)
        << "all 20 written records must be decoded and exported successfully";
    EXPECT_EQ(kctx.calls.load(), 3)
        << "export must query key_provider exactly once for this file, totaling 3 calls";

    logx_close(inst);
    cleanup_dir(dir);
}

// ============================================================
// R24 §7.2 方案 C (P3-2 方案 b): 续写加密文件若打开时刻缺钥 (target.key == nullptr),
// 在后续 drain 轮次中每轮尝试重探 key_provider; 一旦恢复则立即锁定, 随后块恢复密文落盘。
// 验证:
// - 既有加密文件在 open 阶段密钥不可用 (调用 2 返回 NULL) -> 当轮 flush 以明文块落盘;
// - 下一轮 drain 密钥恢复 (调用 3 返回有效 key) -> target.key 锁定, 该轮及后续块恢复为密文块;
// - 第三轮 drain target.key 已锁定 -> 不再重复调用 key_provider;
// - 最终导出: 验证所有块 (明文 + 密文) 均可正确完整解码导出。
// ============================================================
TEST(R19Regression, SchemeC_EncryptedFile_MissingKeyAtOpen_RecoversOnSubsequentDrain) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_r24_schemec_p3_2_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    uint8_t static_key[32];
    for (int i = 0; i < 32; ++i) static_key[i] = static_cast<uint8_t>(0xD0 + i);

    // 第一阶段: 用常态密钥实例建立一个声明加密的日志文件, 写入 10 条密文日志
    {
        logx_config_t cfg{};
        cfg.log_dir = dir.c_str();
        cfg.staging_path = staging.c_str();
        cfg.max_log_length = 64 * 1024;
        cfg.retention_days = 7;
        cfg.max_file_size = 10 * 1024 * 1024;
        cfg.flush_threshold = 0;
        cfg.compress_algo = 0;
        cfg.encrypt_enable = 1;
        cfg.kid = 42;
        cfg.key_provider = [](uint16_t, void* user) -> const uint8_t* {
            return static_cast<const uint8_t*>(user);
        };
        cfg.user = static_key;

        logx_instance_t* inst1 = nullptr;
        ASSERT_EQ(logx_open(&cfg, &inst1), LOGX_OK);
        std::vector<uint8_t> payload(128, 'A');
        for (int i = 0; i < 10; ++i) {
            ASSERT_EQ(logx_write(inst1, 2, "STAGE1", payload.data(),
                                 static_cast<uint32_t>(payload.size())), LOGX_OK);
        }
        ASSERT_EQ(logx_flush(inst1), LOGX_OK);
        logx_close(inst1);
    }

    // 第二阶段: 重新打开该目录, 模拟有状态 key_provider
    // 1st 调用: logx_open 探活 -> 返回 key (允许启动)
    // 2nd 调用: open_or_create_append_target 续写打开 -> 返回 NULL (模拟暂时缺钥)
    // 3rd 调用: 下一轮 drain 重试 -> 返回 key (模拟密钥恢复就绪)
    struct StatefulKeyCtx {
        std::atomic<int> calls{0};
        const uint8_t* key = nullptr;
    } kctx;
    kctx.key = static_key;

    EventProbe probe;
    logx_config_t cfg2{};
    cfg2.log_dir = dir.c_str();
    cfg2.staging_path = staging.c_str();
    cfg2.max_log_length = 64 * 1024;
    cfg2.retention_days = 7;
    cfg2.max_file_size = 10 * 1024 * 1024;
    cfg2.flush_threshold = 0;
    cfg2.compress_algo = 0;
    cfg2.encrypt_enable = 1;
    cfg2.kid = 42;
    cfg2.user = &kctx;
    cfg2.key_provider = [](uint16_t, void* user) -> const uint8_t* {
        auto* c = static_cast<StatefulKeyCtx*>(user);
        int n = c->calls.fetch_add(1) + 1;
        if (n == 2) return nullptr;  // 仅在打开追加目标时缺钥
        return c->key;
    };
    cfg2.err_fn = [](void* user, logx_event_kind_e k, int code, const char* m) {
        static_cast<EventProbe*>(user)->on_event(k, code, m);
    };
    cfg2.err_user = &probe;

    logx_instance_t* inst2 = nullptr;
    ASSERT_EQ(logx_open(&cfg2, &inst2), LOGX_OK);
    EXPECT_EQ(kctx.calls.load(), 1);

    // Batch 1 (缺钥轮次): 写入 5 条并 flush -> 降级为明文块落盘
    std::vector<uint8_t> payload(128, 'B');
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(logx_write(inst2, 2, "STAGE2_B1", payload.data(),
                             static_cast<uint32_t>(payload.size())), LOGX_OK);
    }
    ASSERT_EQ(logx_flush(inst2), LOGX_OK);
    EXPECT_EQ(kctx.calls.load(), 2);
    EXPECT_TRUE(probe.saw_msg_substring("missing key; downgrading block to plaintext"));

    // Batch 2 (恢复轮次): 写入 5 条并 flush -> drain_once 重探成功, 恢复密文块落盘
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(logx_write(inst2, 2, "STAGE2_B2", payload.data(),
                             static_cast<uint32_t>(payload.size())), LOGX_OK);
    }
    ASSERT_EQ(logx_flush(inst2), LOGX_OK);
    EXPECT_EQ(kctx.calls.load(), 3);  // 第 3 次调用重探成功

    // Batch 3 (锁定稳态轮次): 写入 5 条并 flush -> target.key 已锁定, 不再重探
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(logx_write(inst2, 2, "STAGE2_B3", payload.data(),
                             static_cast<uint32_t>(payload.size())), LOGX_OK);
    }
    ASSERT_EQ(logx_flush(inst2), LOGX_OK);
    EXPECT_EQ(kctx.calls.load(), 3);  // 保持 3 次, 无冗余调用

    uint32_t today = 0, hhmmss = 0;
    pal.wallclock_date(&today, &hhmmss);
    std::string logfile = day_log_path(dir, today);

    // 检查磁盘块:
    // 应该包含: 阶段 1 密文块 + 阶段 2 Batch 1 明文块 + 阶段 2 Batch 2/3 密文块
    std::vector<DiskBlock> blks;
    ASSERT_TRUE(scan_disk_blocks(logfile, &blks));
    ASSERT_GE(blks.size(), 3u);
    // 第一块是密文
    EXPECT_NE(blks.front().flag & kFlagEncrypted, 0);
    // 至少有 1 块明文 (降级产物)
    bool has_plaintext = false;
    for (const auto& b : blks) {
        if ((b.flag & kFlagEncrypted) == 0) has_plaintext = true;
    }
    EXPECT_TRUE(has_plaintext) << "degraded batch must produce plaintext block";
    // 最后一块是密文 (恢复产物)
    EXPECT_NE(blks.back().flag & kFlagEncrypted, 0)
        << "recovered batch must produce encrypted block";

    // 导出检验: 全部 10 + 5 + 5 + 5 = 25 条日志完整解码导出
    struct ExportCtx {
        int files = 0;
        int lines = 0;
    } exp_ctx;

    logx_status_e erc = logx_export(inst2, "20200101", "20991231",
        [](void* user, const char*, const char* path, uint64_t size) -> int {
            auto* c = static_cast<ExportCtx*>(user);
            if (size > 0) {
                c->files++;
                Pal& p = default_pal();
                int exp_fd = p.file_open(path, false, false);
                if (exp_fd >= 0) {
                    std::vector<char> buf(size);
                    p.file_pread(exp_fd, buf.data(), size, 0);
                    p.file_close(exp_fd);
                    for (uint64_t i = 0; i < size; ++i) {
                        if (buf[i] == '\n') c->lines++;
                    }
                }
            }
            return 0;
        }, &exp_ctx);
    EXPECT_EQ(erc, LOGX_OK);
    EXPECT_EQ(exp_ctx.files, 1);
    EXPECT_EQ(exp_ctx.lines, 25)
        << "all 25 written records (10 initial enc + 5 degraded plain + 10 recovered enc) must be exported";
    EXPECT_EQ(kctx.calls.load(), 4)  // export 阶段该文件调用 1 次 key_provider
        << "export must query key_provider once, totaling 4 calls";

    logx_close(inst2);
    cleanup_dir(dir);
}
#endif  // LOGX_WITH_AESGCM

// ============================================================
// F-10: logx_open 失败 → *out_inst = NULL(R19 探针 I 形态)
// ============================================================
TEST(R19Regression, F10_FailedOpen_WritesNullOutParam) {
    logx_config_t cfg{};
    cfg.log_dir = nullptr;   // 必填缺失 → 失败分支
    cfg.staging_path = nullptr;
    cfg.max_log_length = 4096;

    // 模拟"复用句柄变量"场景: out_inst 预置脏值, 失败后必须为 NULL
    void* dirty = reinterpret_cast<void*>(static_cast<intptr_t>(0xDEADBEEF));
    logx_instance_t* inst = static_cast<logx_instance_t*>(dirty);
    logx_status_e rc = logx_open(&cfg, &inst);
    EXPECT_NE(rc, LOGX_OK);
    EXPECT_EQ(inst, nullptr)
        << "contract logx_c.h: failed open must set *out_inst = NULL";
}

// 造一个"2 个完整块(seqno 5,6;raw_len 各 64)+ 1 个撕裂块(声明 payload 200B,
// 实写 10B)"的日志文件 → 扫描得 S_file=6 且 truncated=true。F14/F14b 共用。
// (日期取真实过去日: 两用例均不经 logx_open、无 Flusher 线程, 按 R18-1 三条件
//  判据条件 (b)(c) 不成立, 不受 retention 清理影响。)
static void craft_truncated_log(const std::string& path, uint32_t date,
                                const uint8_t salt[8]) {
    Pal& p = default_pal();
    int fd = p.file_open(path.c_str(), true, false);
    ASSERT_GE(fd, 0);
    write_file_header_bytes(fd, date, salt);
    auto write_block = [&](uint32_t seqno, uint32_t raw_len,
                           const std::vector<uint8_t>& payload) {
        uint8_t bhdr[34] = {};
        make_block_header(bhdr, seqno, raw_len,
                          static_cast<uint32_t>(payload.size()), 0, salt);
        put_u32_le(bhdr + 18, crc32_update(0, payload.data(),
                                           static_cast<uint32_t>(payload.size())));
        ASSERT_EQ(p.file_write(fd, bhdr, 34), 34);
        ASSERT_EQ(p.file_write(fd, payload.data(), static_cast<long>(payload.size())),
                  static_cast<long>(payload.size()));
    };
    write_block(5, 64, std::vector<uint8_t>(64, 0xAB));
    write_block(6, 64, std::vector<uint8_t>(64, 0xAC));
    write_block(7, 200, std::vector<uint8_t>(10, 0xCD));  // 撕裂: payload_len != raw_len
    p.file_close(fd);
}

// ============================================================
// F-14: rebuilt + 文件尾截断 → 续写 seqno 跳空隙(防 AES-GCM nonce 复用)
// R21 F-3 更正: 旧版只在**全新** staging 上 new Staging——全零 Header 区使
// Staging::open 判 fresh=true ⇒ rebuilt=false(staging.cpp:220-231), 于是
// run_recovery 走常规路径, R19 F-14 所在的 rebuilt 分支(recovery.cpp:216-242)
// **从未执行**; 实测把 `hdr.seqno = sr.seqno + 1` 改回 `sr.seqno` 旧用例照样绿。
// 本版真毒化双 Header, 并用 flushed_len 区分两条路径(rebuilt 归零 / 常规按
// Σraw_len 回推), 避免"同一个数字被两条路径同时满足"而互相掩盖。
// ============================================================
TEST(R19Regression, F14_RebuiltTruncatedFile_SeqnoSkipsGap) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_r19_f14_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";
    uint8_t salt[8] = {1,2,3,4,5,6,7,8};
    craft_truncated_log(dir + "/20260831.log", 20260831u, salt);

    // 毒化双 Header 区([0,128) 全 0xFF): pick_header 必失败, 且 Header 区非全零
    // ⇒ fresh=false ⇒ rebuilt=true。
    {
        int sfd = pal.file_open(staging.c_str(), true, false);
        ASSERT_GE(sfd, 0);
        std::vector<uint8_t> poison(kHeaderTotal, 0xFF);
        ASSERT_EQ(pal.file_pwrite(sfd, poison.data(), static_cast<long>(kHeaderTotal), 0),
                  static_cast<long>(kHeaderTotal));
        pal.file_close(sfd);
    }

    Instance* inst = new Instance();
    inst->pal = &default_pal();
    inst->log_dir = dir;
    inst->staging_path = staging;
    inst->staging = new Staging(inst->pal, staging, 8 * 1024);
    ASSERT_TRUE(run_recovery(inst));

    // rebuilt + truncated: hdr.seqno = sr.seqno + 1 = 7 ⇒ Flusher 首块 = 8 = S_file+2
    EXPECT_EQ(get_u32(inst->staging->raw_base() + 8), 7u)
        << "rebuilt+truncated must resume at S_file+1 so the first block = S_file+2 (nonce gap)";
    // 路径判别: rebuilt 分支 commit(seqno, 0, 0) → 环坐标归零(常规路径会是 128)
    EXPECT_EQ(get_u32(inst->staging->raw_base() + 12), 0u)
        << "flushed_len must be 0: proves the rebuilt branch ran, not the normal path";
    EXPECT_GE(inst->truncated_blocks.load(), 1u) << "truncation must be counted";

    delete inst->staging;
    delete inst;
    cleanup_dir(dir);
}

// ============================================================
// F-14 孪生: 常规路径(S_hdr=0 < S_file=6 且尾块撕裂)→ resume = max+1,
// 且 flushed_len 按 Σraw_len 回推(recovery.cpp:264-287)。与 F14 成对钉死两条
// 路径, 任一条被改坏都会各自复红。
// ============================================================
TEST(R19Regression, F14b_NormalPathTruncatedFile_ResumeSeqnoMaxPlusOne) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_r21_f14b_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";
    uint8_t salt[8] = {9,9,9,9,9,9,9,9};
    craft_truncated_log(dir + "/20260831.log", 20260831u, salt);
    // 不毒化: staging 全新 → Header 区全零 → fresh=true → rebuilt=false

    Instance* inst = new Instance();
    inst->pal = &default_pal();
    inst->log_dir = dir;
    inst->staging_path = staging;
    inst->staging = new Staging(inst->pal, staging, 8 * 1024);
    ASSERT_TRUE(run_recovery(inst));

    EXPECT_EQ(get_u32(inst->staging->raw_base() + 8), 7u)
        << "normal path: resume = max(S_hdr, S_file) + 1";
    // 路径判别: S_file > S_hdr 分支按 Σraw_len(seqno ∈ (0,6] = 64+64) 回推 f
    EXPECT_EQ(get_u32(inst->staging->raw_base() + 12), 128u)
        << "flushed_len must be backfilled by sum_raw_len_since (proves normal path)";
    EXPECT_GE(inst->truncated_blocks.load(), 1u);

    delete inst->staging;
    delete inst;
    cleanup_dir(dir);
}

// ============================================================
// F-4: 回绕缝在环坐标扫描中不被误抹除(ring_scan + wrap_boundary)
// 缝后未提交记录必须保留在待刷窗口内(R19 §三·5 形态: 旧实现 memset 清零)。
// ============================================================
TEST(R19Regression, F4_WrapSeam_PostSeamRecordsSurviveRecovery) {
    Pal& pal = default_pal();
    std::string staging = std::string("logx_test_r19_f4_") +
                          std::to_string(pal.pid()) + "_" +
                          std::to_string(s_r19_dir_seq.fetch_add(1)) + ".bin";
    pal.file_unlink(staging.c_str());

    constexpr uint32_t kDataSize = 8 * 1024;
    Instance* inst = new Instance();
    inst->pal = &default_pal();
    inst->log_dir = "logx_test_r19_f4_dir";  // 空目录: 无日志文件, S_file=0
    pal.file_mkdirs(inst->log_dir.c_str());
    inst->staging_path = staging;
    inst->staging = new Staging(inst->pal, staging, kDataSize);
    RecoveryState st; bool rec = false;
    ASSERT_TRUE(inst->staging->open(&st, &rec));

    // 布局(环坐标):
    //   [8164, 8189) = 一条 25B 记录(payload 19B)     ← 待刷(f 起点)
    //   [8189, 8192) = 3B 留零缝(pad<6B 回绕残留)      ← wrap_boundary 前
    //   [0, 26)      = 回绕后一条 26B 记录              ← 待刷(缝后记录)
    // f = 8164, w = 26 → 待刷窗口横跨回绕点, 缝在窗口中部。
    // 旧缺陷: ring_scan 在缝处 rec==0 → 当撕裂尾 memset 清零 [缝, 窗口末)
    // → 缝后 26B 记录被抹;修复后 wrap_boundary 引导扫描跳缝续扫。
    auto entry = make_entry(20, 0x77);            // 26B
    const uint32_t rec_len = static_cast<uint32_t>(entry.size());
    const uint32_t data_size = inst->staging->data_size();
    const uint32_t seam_len = 3;
    const uint32_t seam_pos = data_size - seam_len;              // 8189
    const uint32_t last_rec_len = 25;                            // 8164..8189
    const uint32_t f_target = seam_pos - last_rec_len;           // 8164(记录边界)

    // 批次 1: 单次写入填到缝位(w = 8189), 不触发回绕(total ≤ free-1)。
    std::vector<uint8_t> recs;
    const uint32_t n_full = f_target / rec_len;                  // 整条填充 [0, 8164)
    for (uint32_t i = 0; i < n_full; ++i) recs.insert(recs.end(), entry.begin(), entry.end());
    {
        auto tail_rec = make_entry(last_rec_len - kHeaderLen, 0x66);  // 25B 记录
        recs.insert(recs.end(), tail_rec.begin(), tail_rec.end());
    }
    ASSERT_EQ(recs.size(), static_cast<size_t>(seam_pos));
    ASSERT_EQ(inst->staging->write_records(recs.data(),
                                           static_cast<uint32_t>(recs.size())),
              Staging::WriteStatus::kOk);
    // 推进 f 到最后一条记录边界(模拟 Flusher 已刷 [0, 8164))。
    AppendTargetCache atc{};
    inst->staging->commit_header_locked(0, f_target, seam_pos, &atc);

    // 批次 2: 写一条完整记录 → pad = 3 < 6 → 留零缝 + 回绕, 记录落环首。
    ASSERT_EQ(inst->staging->write_records(entry.data(), rec_len),
              Staging::WriteStatus::kOk);

    // 跑恢复: 无日志文件 → S_file=0;环形扫描必须识别缝并跳到回绕侧续扫。
    ASSERT_TRUE(run_recovery(inst));

    uint32_t new_f = get_u32(inst->staging->raw_base() + 12);
    uint32_t new_w = get_u32(inst->staging->raw_base() + 16);
    uint32_t pending = (new_w >= new_f) ? (new_w - new_f) : (data_size - new_f + new_w);
    // 待刷 = 25B 记录 + 3B 缝 + 26B 记录 = 54B;旧缺陷只余 25B(缝后被抹)。
    EXPECT_EQ(pending, last_rec_len + seam_len + rec_len)
        << "post-seam record must survive recovery (was memset-wiped before F-4 fix)";

    delete inst->staging;
    delete inst;
    pal.file_unlink(staging.c_str());
    cleanup_dir("logx_test_r19_f4_dir");
}

// ============================================================
// R21 N-2: 读侧接受域直测(跨模块契约门禁的**读侧**半边)。
// "raw_len < kHeaderLen 必拒"是 R20/R21 全部 F-1 处置的立论依据(微块一旦落盘,
// 导出在块边界不可信处 break、repair 物理截断), 但 block_header_plausible 此前
// 全仓零直测, 写侧自检成了单边门禁。此处把接受域两侧与其它判据一并钉死。
// ============================================================
TEST(R19Regression, ReadSideAcceptanceDomain_RawLenBounds) {
    const uint8_t salt[8] = {1,2,3,4,5,6,7,8};
    uint8_t b[34];
    uint32_t seqno = 0, raw_len = 0, payload_len = 0;
    uint64_t bsize = 0;
    const uint64_t kBig = 64ULL * 1024 * 1024;   // 足够大, 避免误触容量判据
    // 读侧上界是 flusher.cpp 匿名空间内的 kBlockMaxRawPlausible(16MB), 测试侧只能
    // 以字面量镜像; 该常数若被改动, 下面两条断言会一红一绿地暴露口径漂移。
    const uint32_t kPlausibleMax = 16u * 1024u * 1024u;

    // 合法基线: 未压缩 + payload_len == raw_len
    make_block_header(b, 7, 64, 64, 0, salt);
    ASSERT_TRUE(block_header_plausible(b, kBig, 0, false,
                                       &seqno, &raw_len, &payload_len, &bsize));
    EXPECT_EQ(seqno, 7u);
    EXPECT_EQ(raw_len, 64u);
    EXPECT_EQ(payload_len, 64u);
    EXPECT_EQ(bsize, 34u + 64u);

    // 下界: 1~5B 缝独占微块必拒(F-1 处置的立论依据)
    for (uint32_t rl = 1; rl < kHeaderLen; ++rl) {
        make_block_header(b, 7, rl, rl, 0, salt);   // payload_len == raw_len, 未压缩
        EXPECT_FALSE(block_header_plausible(b, kBig, 0, false, nullptr, nullptr, nullptr, nullptr))
            << "raw_len=" << rl << " must be rejected: seam-only micro-block is unwritable";
    }
    // raw_len == 0 亦拒(此例由 payload_len==0 判据命中, 结论一致)
    make_block_header(b, 7, 0, 0, 0, salt);
    EXPECT_FALSE(block_header_plausible(b, kBig, 0, false, nullptr, nullptr, nullptr, nullptr));

    // 下界内侧: raw_len == kHeaderLen 必收
    make_block_header(b, 7, kHeaderLen, kHeaderLen, 0, salt);
    EXPECT_TRUE(block_header_plausible(b, kBig, 0, false, nullptr, nullptr, nullptr, nullptr))
        << "raw_len == kHeaderLen is the inclusive lower bound";

    // 写侧上限 kBlockMaxRaw 必收(写读两侧接受域在此重叠)
    make_block_header(b, 7, kBlockMaxRaw, kBlockMaxRaw, 0, salt);
    EXPECT_TRUE(block_header_plausible(b, kBig, 0, false, nullptr, nullptr, nullptr, nullptr))
        << "write side's kBlockMaxRaw must be inside the read side's acceptance domain";

    // 写侧缝并入上限 kBlockMaxRawWithSeam(20491, R26 方案A)必收(R27 N-1):
    // R26 后写侧真实产出上限放宽至此, 读侧显式收容断言使契约边界不依赖 16MB 宽域推演
    make_block_header(b, 7, kBlockMaxRawWithSeam, kBlockMaxRawWithSeam, 0, salt);
    EXPECT_TRUE(block_header_plausible(b, kBig, 0, false, nullptr, nullptr, nullptr, nullptr))
        << "write side's kBlockMaxRawWithSeam must be inside the read side's acceptance domain";

    // 读侧上界: > 16MB 必拒(读侧上界刻意远宽于写侧的 20491, 二者不同源)
    make_block_header(b, 7, kPlausibleMax + 1, kPlausibleMax + 1, 0, salt);
    EXPECT_FALSE(block_header_plausible(b, kBig, 0, false, nullptr, nullptr, nullptr, nullptr));
    make_block_header(b, 7, kPlausibleMax, kPlausibleMax, 0, salt);
    EXPECT_TRUE(block_header_plausible(b, kBig, 0, false, nullptr, nullptr, nullptr, nullptr));
}

TEST(R19Regression, ReadSideAcceptanceDomain_SeqnoFlagAndCapacityGuards) {
    const uint8_t salt[8] = {1,2,3,4,5,6,7,8};
    uint8_t b[34];
    auto ok = [&](uint64_t remaining, uint32_t prev, bool has_prev) {
        return block_header_plausible(b, remaining, prev, has_prev,
                                      nullptr, nullptr, nullptr, nullptr);
    };

    // seqno 严格单调: 0 拒、<= prev 拒、> prev 收
    make_block_header(b, 0, 64, 64, 0, salt);
    EXPECT_FALSE(ok(1 << 20, 0, false)) << "seqno == 0 is reserved";
    make_block_header(b, 5, 64, 64, 0, salt);
    EXPECT_FALSE(ok(1 << 20, 5, true)) << "seqno must strictly increase";
    EXPECT_FALSE(ok(1 << 20, 6, true));
    EXPECT_TRUE(ok(1 << 20, 4, true));

    // flag 保留位(bit3+)必拒; 加密块的 block_size 含 16B tag
    make_block_header(b, 5, 64, 64, 0x08, salt);
    EXPECT_FALSE(ok(1 << 20, 0, false)) << "reserved flag bits must be rejected";
    make_block_header(b, 5, 64, 64, kFlagEncrypted, salt);
    EXPECT_TRUE(ok(34 + 64 + 16, 0, false));
    EXPECT_FALSE(ok(34 + 64 + 15, 0, false)) << "encrypted block needs room for the 16B tag";

    // 容量判据: block_size 越过文件剩余字节必拒
    make_block_header(b, 5, 64, 64, 0, salt);
    EXPECT_TRUE(ok(34 + 64, 0, false));
    EXPECT_FALSE(ok(34 + 63, 0, false));

    // 协议自洽: 未压缩时 payload_len 必须等于 raw_len; payload_len==0 拒
    make_block_header(b, 5, 64, 63, 0, salt);
    EXPECT_FALSE(ok(1 << 20, 0, false)) << "uncompressed payload_len must equal raw_len";
    make_block_header(b, 5, 64, 0, 0, salt);
    EXPECT_FALSE(ok(1 << 20, 0, false));
}

TEST(R19Regression, F7_MicroBlockNotSeamPadding_DropsAndEmitsRecoveryEvent) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_r21_f7_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 64 * 1024;
    cfg.retention_days = 36500;
    cfg.max_file_size = 10 * 1024 * 1024;
    cfg.flush_threshold = 0;
    cfg.compress_algo = 0;
    cfg.encrypt_enable = 0;
    EventProbe probe;
    cfg.err_fn = [](void* user, logx_event_kind_e k, int c, const char* m) {
        static_cast<EventProbe*>(user)->on_event(k, c, m);
    };
    cfg.err_user = &probe;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    Instance* ii = logx::test::test_get_instance(inst);
    const uint32_t data_size = ii->staging->data_size();
    auto w_now = [&]() { return get_u32(ii->staging->raw_base() + 16); };
    
    // First, measure overhead
    const uint32_t kMsg = 512;
    const char* kTag = "F7Test";
    std::vector<uint8_t> msg(kMsg, 'M');
    ASSERT_EQ(logx_write(inst, 2, kTag, msg.data(), kMsg), LOGX_OK);
    const uint32_t rec_len = w_now();
    const uint32_t overhead = rec_len - kMsg;

    // We want to leave a micro block of 3 bytes at the end.
    const uint32_t kSeam = 3;
    const uint32_t budget = data_size - kSeam - rec_len;
    const uint32_t n_full = (budget - (overhead + 1)) / rec_len;
    for (uint32_t i = 0; i < n_full; ++i) {
        ASSERT_EQ(logx_write(inst, 2, kTag, msg.data(), kMsg), LOGX_OK);
    }
    const uint32_t tail = budget - n_full * rec_len;
    std::vector<uint8_t> tmsg(tail - overhead, 'T');
    ASSERT_EQ(logx_write(inst, 2, kTag, tmsg.data(), static_cast<uint32_t>(tmsg.size())), LOGX_OK);
    
    // Check that we have exactly kSeam bytes left
    ASSERT_EQ(w_now(), data_size - kSeam);
    
    // Flush to advance f to w so staging has room to wrap around
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    // Now trigger a wrap-around write so the seam is left behind
    std::vector<uint8_t> rmsg(50, 'R');
    ASSERT_EQ(logx_write(inst, 2, kTag, rmsg.data(), static_cast<uint32_t>(rmsg.size())), LOGX_OK);
    
    // Now inject a dirty byte into the seam padding to simulate bit-flip
    ii->staging->data_base()[data_size - 1] = 0xAA;
    
    // Flush
    logx_flush(inst);
    
    // We expect LOGX_EV_RECOVERY
    EXPECT_TRUE(probe.saw_msg_substring("sub-header block is not seam padding; dropped"));
    EXPECT_EQ(ii->seam_blocks_skipped.load(), 1);

    logx_close(inst);
    cleanup_dir(dir);
}

// ============================================================
// B-7: 同日内多次切分且 ATC 提示陈旧时的跨分卷 seqno 回退与新分卷漏选防御
// 场景:
//   同日内存在分卷 0 (<date>.log, seqno 1..10) 与分卷 1 (<date>.log.1, seqno 11..20)。
//   Staging Header 的 ATC 提示因崩溃未持久化而滞留在分卷 0 (split=0, seqno=10)。
//   在 B-7 修复前:
//     reconcile() 的 is_target_newer 仅比较 best_date > hdr.target.date (同日两值相等为 false),
//     且 scan_dir_for_target 原按 seqno 比较对空分卷失效。恢复错误采信 hint 走向分卷 0,
//     resume_seqno 回退到 10, ATC 滞留在 split 0。
//   在 B-7 修复后:
//     严格按 (date, split) 二元组全序判定, 识别分卷 1 更优, 恢复目标正确切换至分卷 1,
//     resume_seqno 推进至 >= 20, ATC 切换为 split 1。
// ============================================================
TEST(R19Regression, B7_SameDaySplit_StaleAtcHintRecoversToLatestSplit) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_b7_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";
    uint32_t today = 20261007u;
    uint8_t salt0[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t salt1[8] = {8, 7, 6, 5, 4, 3, 2, 1};

    auto write_custom_log = [&](const std::string& path, uint32_t date, const uint8_t salt[8],
                                uint32_t seq_start, uint32_t count) {
        int fd = pal.file_open(path.c_str(), true, false);
        ASSERT_GE(fd, 0);
        write_file_header_bytes(fd, date, salt);
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t bhdr[34] = {};
            std::vector<uint8_t> payload(64, static_cast<uint8_t>(0x30 + i));
            make_block_header(bhdr, seq_start + i, 64,
                              static_cast<uint32_t>(payload.size()), 0, salt);
            put_u32_le(bhdr + 18, crc32_update(0, payload.data(),
                                               static_cast<uint32_t>(payload.size())));
            ASSERT_EQ(pal.file_write(fd, bhdr, 34), 34);
            ASSERT_EQ(pal.file_write(fd, payload.data(), static_cast<long>(payload.size())),
                      static_cast<long>(payload.size()));
        }
        pal.file_close(fd);
    };

    // 分卷 0: seqno 1..10
    write_custom_log(make_log_path(dir, today, 0), today, salt0, 1, 10);
    // 分卷 1: seqno 11..20
    write_custom_log(make_log_path(dir, today, 1), today, salt1, 11, 10);

    // 构造 Staging: 预置 Header 指向陈旧的分卷 0 (seqno=10, flushed_len=0, atc split=0)
    Instance* inst = new Instance();
    inst->pal = &pal;
    inst->log_dir = dir;
    inst->staging_path = staging;
    inst->staging = new Staging(inst->pal, staging, 128 * 1024);
    RecoveryState st; bool rec = false;
    ASSERT_TRUE(inst->staging->open(&st, &rec));

    AppendTargetCache stale_atc{};
    stale_atc.date = today;
    atc_set_split(&stale_atc, 0);
    stale_atc.last_block_offset = 32;
    stale_atc.last_block_end_offset = 32 + 10 * (34 + 64);
    inst->staging->commit_header_locked(10, 0, 0, &stale_atc);

    // 执行恢复
    ASSERT_TRUE(run_recovery(inst));

    // 校验恢复后的 Staging Header:
    const uint8_t* raw = inst->staging->raw_base();
    uint32_t recovered_seqno = get_u32(raw + 8);
    AppendTargetCache recovered_atc{};
    std::memcpy(&recovered_atc, raw + 20, sizeof(recovered_atc));

    // B-7 核心断言:
    // 1. ATC 必须指向最新的分卷 1 (杜绝留在滞后的分卷 0)
    EXPECT_EQ(recovered_atc.date, today);
    EXPECT_EQ(atc_split(recovered_atc), 1u);

    // 2. 恢复后的 seqno 必须 >= 20 (分卷 1 的尾块序号)，严禁回退至分卷 0 的 10
    EXPECT_GE(recovered_seqno, 20u);

    delete inst->staging;
    delete inst;
    cleanup_dir(dir);
}

// ============================================================
// B-8 验证用例 (R20 F-13 原始虚报链): 同日双分卷 + S_hdr 已提交但 ATC 撕裂回退
// 场景:
//   分卷 0 (seqno 1..10, ATC hint 指向) 与分卷 1 (seqno 11..25, 实际尾块持有者)。
//   staging Header 的 seqno 已提交至 25, 但 ATC 撕裂回退指分卷 0。
//   R20 时代 (B-7 前): reconcile 同日采信 hint 扫分卷 0 (尾块 seqno=10),
//     S_hdr(25) > 10 → truncated_blocks 虚报 +15, 实际块 21..25 全在分卷 1。
//   B-7 落地后: reconcile 按 (date, split) 选中分卷 1, seqno_file(25) == S_hdr(25)
//     → 不得计数。本用例钉死 "B-7 已消解 B-8 原始虚报链" 的事实;
//     撤销 B-7 判据 (is_target_newer 回退为仅比 date) 本用例确定性复红 (正向对照耦合)。
// ============================================================
TEST(R19Regression, B8_SameDaySplit_StaleAtcWithCommittedSeqno_NoTruncatedOvercount) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_b8_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";
    uint32_t today = 20261007u;
    uint8_t salt0[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t salt1[8] = {8, 7, 6, 5, 4, 3, 2, 1};

    auto write_custom_log = [&](const std::string& path, uint32_t date, const uint8_t salt[8],
                                uint32_t seq_start, uint32_t count) {
        int fd = pal.file_open(path.c_str(), true, false);
        ASSERT_GE(fd, 0);
        write_file_header_bytes(fd, date, salt);
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t bhdr[34] = {};
            std::vector<uint8_t> payload(64, static_cast<uint8_t>(0x30 + i));
            make_block_header(bhdr, seq_start + i, 64,
                              static_cast<uint32_t>(payload.size()), 0, salt);
            put_u32_le(bhdr + 18, crc32_update(0, payload.data(),
                                               static_cast<uint32_t>(payload.size())));
            ASSERT_EQ(pal.file_write(fd, bhdr, 34), 34);
            ASSERT_EQ(pal.file_write(fd, payload.data(), static_cast<long>(payload.size())),
                      static_cast<long>(payload.size()));
        }
        pal.file_close(fd);
    };

    // 分卷 0: seqno 1..10 (ATC hint 指向); 分卷 1: seqno 11..25 (实际尾块持有者)
    write_custom_log(make_log_path(dir, today, 0), today, salt0, 1, 10);
    write_custom_log(make_log_path(dir, today, 1), today, salt1, 11, 15);

    // 构造 Staging: seqno 已提交至 25, 但 ATC 撕裂回退指分卷 0
    Instance* inst = new Instance();
    inst->pal = &pal;
    inst->log_dir = dir;
    inst->staging_path = staging;
    inst->staging = new Staging(inst->pal, staging, 128 * 1024);
    RecoveryState st; bool rec = false;
    ASSERT_TRUE(inst->staging->open(&st, &rec));

    AppendTargetCache stale_atc{};
    stale_atc.date = today;
    atc_set_split(&stale_atc, 0);
    stale_atc.last_block_offset = 32;
    stale_atc.last_block_end_offset = 32 + 10 * (34 + 64);
    inst->staging->commit_header_locked(25, 0, 0, &stale_atc);

    const uint64_t truncated_before = inst->truncated_blocks.load();

    ASSERT_TRUE(run_recovery(inst));

    // B-8 核心断言: 不得虚报 (R20 时代此处会 +15)
    EXPECT_EQ(inst->truncated_blocks.load(), truncated_before);

    // 恢复目标应正确切换至分卷 1 且 seqno 无回退
    const uint8_t* raw = inst->staging->raw_base();
    uint32_t recovered_seqno = get_u32(raw + 8);
    AppendTargetCache recovered_atc{};
    std::memcpy(&recovered_atc, raw + 20, sizeof(recovered_atc));
    EXPECT_EQ(atc_split(recovered_atc), 1u);
    EXPECT_EQ(recovered_seqno, 25u);

    delete inst->staging;
    delete inst;
    cleanup_dir(dir);
}

// ============================================================
// B-10 方案 A1: 异常残留高分卷 seqno 倒退防御 (告警 + resume 钳制)
// 场景:
//   分卷 0 (seqno 1..20, ATC hint 指向) 与异常残留分卷 1 (外部/多写者注入,
//   seqno 1..5, 低于 hint 尾块)。staging Header seqno 撕裂回退至 10
//   (块 11..20 已落分卷 0 但提交未持久化)。
//   B-7 按 (date, split) 选中分卷 1 (seqno_file=5); 若无防御, resume =
//   max(10,5)+1 = 11, 新块 11.. 将与分卷 0 已有块 11..20 磁盘 seqno 倒退与重复。
//   B-10 升级落地后: 检测 S_target(5) < max_dir_seqno(20) → 发 RECOVERY 告警 +
//   resume 钳制至 20, 新块自 21 起, 磁盘 seqno 全局单调。
//   撤销钳制 (R18-2 门禁): recovered seqno 11 → 本用例确定性复红。
// ============================================================
TEST(R19Regression, B10_ResidualHigherSplitSeqnoRegression_ClampedAndWarned) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_b10_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";
    uint32_t today = 20261007u;
    uint8_t salt0[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t salt1[8] = {8, 7, 6, 5, 4, 3, 2, 1};

    auto write_custom_log = [&](const std::string& path, uint32_t date, const uint8_t salt[8],
                                uint32_t seq_start, uint32_t count) {
        int fd = pal.file_open(path.c_str(), true, false);
        ASSERT_GE(fd, 0);
        write_file_header_bytes(fd, date, salt);
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t bhdr[34] = {};
            std::vector<uint8_t> payload(64, static_cast<uint8_t>(0x30 + i));
            make_block_header(bhdr, seq_start + i, 64,
                              static_cast<uint32_t>(payload.size()), 0, salt);
            put_u32_le(bhdr + 18, crc32_update(0, payload.data(),
                                               static_cast<uint32_t>(payload.size())));
            ASSERT_EQ(pal.file_write(fd, bhdr, 34), 34);
            ASSERT_EQ(pal.file_write(fd, payload.data(), static_cast<long>(payload.size())),
                      static_cast<long>(payload.size()));
        }
        pal.file_close(fd);
    };

    // 分卷 0: seqno 1..20 (hint); 分卷 1: 异常残留 seqno 1..5 (低于 hint 尾块)
    write_custom_log(make_log_path(dir, today, 0), today, salt0, 1, 20);
    write_custom_log(make_log_path(dir, today, 1), today, salt1, 1, 5);

    // 构造 Staging: seqno 撕裂回退至 10, ATC 指分卷 0
    Instance* inst = new Instance();
    inst->pal = &pal;
    inst->log_dir = dir;
    inst->staging_path = staging;
    inst->staging = new Staging(inst->pal, staging, 128 * 1024);
    RecoveryState st; bool rec = false;
    ASSERT_TRUE(inst->staging->open(&st, &rec));

    AppendTargetCache stale_atc{};
    stale_atc.date = today;
    atc_set_split(&stale_atc, 0);
    stale_atc.last_block_offset = 32;
    stale_atc.last_block_end_offset = 32 + 10 * (34 + 64);
    inst->staging->commit_header_locked(10, 0, 0, &stale_atc);

    // 直连错误回调以断言 RECOVERY 告警事件
    EventProbe probe;
    inst->err_fn = [](void* user, logx_event_kind_e k, int c, const char* m) {
        static_cast<EventProbe*>(user)->on_event(k, c, m);
    };
    inst->err_user = &probe;

    ASSERT_TRUE(run_recovery(inst));

    // B-10 核心断言:
    // 1. 告警事件必须发出
    EXPECT_TRUE(probe.saw_kind(LOGX_EV_RECOVERY));
    EXPECT_TRUE(probe.saw_msg_substring("resume clamped"));

    // 2. resume 钳制至 hint 尾块 (20): 新块自 21 起, 磁盘 seqno 无重复
    const uint8_t* raw = inst->staging->raw_base();
    uint32_t recovered_seqno = get_u32(raw + 8);
    AppendTargetCache recovered_atc{};
    std::memcpy(&recovered_atc, raw + 20, sizeof(recovered_atc));
    EXPECT_EQ(recovered_seqno, 20u);
    EXPECT_EQ(atc_split(recovered_atc), 1u);

    delete inst->staging;
    delete inst;
    cleanup_dir(dir);
}

// ============================================================
// B-10 / F-1 验证用例: 空残留高分卷 (sr.seqno == 0) 且 Staging Header 撕裂落后
// 场景:
//   分卷 0 (seqno 1..20, 历史分卷, 尾块 20)。
//   分卷 1 (异常残留空文件, 仅 32B 文件头, seqno == 0)。
//   staging Header 撕裂回退至 seqno = 10, ATC 指向分卷 0。
//   若无 F-1 防御: sr.seqno == 0 走 R19 F-13 is_new_target 分支,
//     resume_seqno 取 S_hdr (10), 新块自 11 起写, 与分卷 0 的 11..20 发生跨卷倒退。
//   方案 1 (全局目录最大序号防御门) 落地后:
//     检测 max_dir_seqno (20) > hdr.seqno (10) → 发 RECOVERY 告警 +
//     resume 钳制至 20, 新块自 21 起写, 全局单调不变量保持。
// ============================================================
TEST(R19Regression, B10_EmptyResidualSplitWithTornHeader_ClampedToHistoryMax) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_b10_f1_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";
    uint32_t today = 20261007u;
    uint8_t salt0[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t salt1[8] = {8, 7, 6, 5, 4, 3, 2, 1};

    auto write_custom_log = [&](const std::string& path, uint32_t date, const uint8_t salt[8],
                                uint32_t seq_start, uint32_t count) {
        int fd = pal.file_open(path.c_str(), true, false);
        ASSERT_GE(fd, 0);
        write_file_header_bytes(fd, date, salt);
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t bhdr[34] = {};
            std::vector<uint8_t> payload(64, static_cast<uint8_t>(0x30 + i));
            make_block_header(bhdr, seq_start + i, 64,
                              static_cast<uint32_t>(payload.size()), 0, salt);
            put_u32_le(bhdr + 18, crc32_update(0, payload.data(),
                                               static_cast<uint32_t>(payload.size())));
            ASSERT_EQ(pal.file_write(fd, bhdr, 34), 34);
            ASSERT_EQ(pal.file_write(fd, payload.data(), static_cast<long>(payload.size())),
                      static_cast<long>(payload.size()));
        }
        pal.file_close(fd);
    };

    // 分卷 0: seqno 1..20; 分卷 1: 异常空残留 (count = 0, 仅 32B 头, seqno == 0)
    write_custom_log(make_log_path(dir, today, 0), today, salt0, 1, 20);
    write_custom_log(make_log_path(dir, today, 1), today, salt1, 1, 0);

    // 构造 Staging: seqno 撕裂回退至 10, ATC 指分卷 0
    Instance* inst = new Instance();
    inst->pal = &pal;
    inst->log_dir = dir;
    inst->staging_path = staging;
    inst->staging = new Staging(inst->pal, staging, 128 * 1024);
    RecoveryState st; bool rec = false;
    ASSERT_TRUE(inst->staging->open(&st, &rec));

    AppendTargetCache stale_atc{};
    stale_atc.date = today;
    atc_set_split(&stale_atc, 0);
    stale_atc.last_block_offset = 32;
    stale_atc.last_block_end_offset = 32 + 10 * (34 + 64);
    inst->staging->commit_header_locked(10, 0, 0, &stale_atc);

    EventProbe probe;
    inst->err_fn = [](void* user, logx_event_kind_e k, int c, const char* m) {
        static_cast<EventProbe*>(user)->on_event(k, c, m);
    };
    inst->err_user = &probe;

    ASSERT_TRUE(run_recovery(inst));

    // F-1 断言:
    // 1. 告警事件必须发出
    EXPECT_TRUE(probe.saw_kind(LOGX_EV_RECOVERY));
    EXPECT_TRUE(probe.saw_msg_substring("resume clamped"));

    // 2. resume 被钳制至历史分卷尾块 (20)，杜绝回退至 S_hdr (10)
    const uint8_t* raw = inst->staging->raw_base();
    uint32_t recovered_seqno = get_u32(raw + 8);
    AppendTargetCache recovered_atc{};
    std::memcpy(&recovered_atc, raw + 20, sizeof(recovered_atc));
    EXPECT_EQ(recovered_seqno, 20u);
    EXPECT_EQ(atc_split(recovered_atc), 1u);

    delete inst->staging;
    delete inst;
    cleanup_dir(dir);
}

// ============================================================
// B-10 (F-1 补全): staging 整区重建 (hdr.rebuilt == true) 路径受防御门保护
// 场景:
//   staging 双 Header 损坏触发整区重建 (rebuilt == true), 环坐标归零。
//   分卷 0 历史写至 seqno = 20; 异常残留分卷 1 仅写至 seqno = 5。
//   scan_dir_for_target 选择分卷 1 为 best append 目标 (sr.seqno = 5)。
//   防御门生效前: rebuilt 分支直接取 sr.seqno (5), 恢复后新写入发生跨卷倒退 (20 -> 6)。
//   防御门修复后: scan_dir_for_target 传出 max_dir_seqno (20),
//     hdr.seqno 钳制至 max(5, 20) = 20, 发出告警事件, 彻底收口 rebuilt 路径。
// ============================================================
TEST(R19Regression, B10_RebuiltStaging_ResidualHigherSplit_ClampedAndWarned) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_b10_rebuilt_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";
    uint32_t today = 20261007u;
    uint8_t salt0[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t salt1[8] = {8, 7, 6, 5, 4, 3, 2, 1};

    auto write_custom_log = [&](const std::string& path, uint32_t date, const uint8_t salt[8],
                                uint32_t seq_start, uint32_t count) {
        int fd = pal.file_open(path.c_str(), true, false);
        ASSERT_GE(fd, 0);
        write_file_header_bytes(fd, date, salt);
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t bhdr[34] = {};
            std::vector<uint8_t> payload(64, static_cast<uint8_t>(0x30 + i));
            make_block_header(bhdr, seq_start + i, 64,
                              static_cast<uint32_t>(payload.size()), 0, salt);
            put_u32_le(bhdr + 18, crc32_update(0, payload.data(),
                                               static_cast<uint32_t>(payload.size())));
            ASSERT_EQ(pal.file_write(fd, bhdr, 34), 34);
            ASSERT_EQ(pal.file_write(fd, payload.data(), static_cast<long>(payload.size())),
                      static_cast<long>(payload.size()));
        }
        pal.file_close(fd);
    };

    // 分卷 0 写至 seqno = 20; 异常残留分卷 1 写至 seqno = 5
    write_custom_log(make_log_path(dir, today, 0), today, salt0, 1, 20);
    write_custom_log(make_log_path(dir, today, 1), today, salt1, 1, 5);

    // 预置损坏的 staging.mmap: 写入非全 0 垃圾数据破坏 Header A/B, 触发 rebuilt == true
    {
        int sfd = pal.file_open(staging.c_str(), true, false);
        ASSERT_GE(sfd, 0);
        std::vector<uint8_t> garbage(128 * 1024, 0xAA);
        ASSERT_EQ(pal.file_write(sfd, garbage.data(), static_cast<long>(garbage.size())),
                  static_cast<long>(garbage.size()));
        pal.file_close(sfd);
    }

    Instance* inst = new Instance();
    inst->pal = &pal;
    inst->log_dir = dir;
    inst->staging_path = staging;
    inst->staging = new Staging(inst->pal, staging, 128 * 1024);

    EventProbe probe;
    inst->err_fn = [](void* user, logx_event_kind_e k, int c, const char* m) {
        static_cast<EventProbe*>(user)->on_event(k, c, m);
    };
    inst->err_user = &probe;

    ASSERT_TRUE(run_recovery(inst));

    // 断言:
    // 1. 发出 rebuilt 专属的钳制告警事件
    EXPECT_TRUE(probe.saw_kind(LOGX_EV_RECOVERY));
    EXPECT_TRUE(probe.saw_msg_substring("rebuilt: target seqno regression vs directory max; resume clamped"));

    // 2. staging 提交的恢复 seqno 被钳制至分卷 0 的最大 seqno (20), 杜绝倒退至分卷 1 的 5
    const uint8_t* raw = inst->staging->raw_base();
    uint32_t recovered_seqno = get_u32(raw + 8);
    AppendTargetCache recovered_atc{};
    std::memcpy(&recovered_atc, raw + 20, sizeof(recovered_atc));
    EXPECT_EQ(recovered_seqno, 20u);
    EXPECT_EQ(atc_split(recovered_atc), 1u);

    delete inst->staging;
    delete inst;
    cleanup_dir(dir);
}


// ============================================================
// B-1 方案 A (基准对照): staging_wait_ms == 0 保持默认绝不阻塞
// 场景:
//   关闭自动刷盘 (flush_threshold = 0)，单条 32KB 记录连续写入。
//   第 5 条必定超出 128KB 数据区触顶 kFull。
//   在 staging_wait_ms == 0 时: 立即返回 LOGX_ERR_NO_SPACE，绝不阻塞，
//   且 drop_no_space 增加，发 LOGX_EV_DROP 事件。
// ============================================================
TEST(R19Regression, B1_NoSpace_DefaultZeroWait_DropsImmediately) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_b1_def_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    EventProbe probe;
    auto on_err = [](void* user, logx_event_kind_e k, int c, const char* m) {
        static_cast<EventProbe*>(user)->on_event(k, c, m);
    };

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 48 * 1024;
    cfg.flush_threshold = 0;  // 禁用按阈值自动刷盘
    cfg.staging_wait_ms = 0;  // 默认: 绝不阻塞
    cfg.retention_days = 36500;
    cfg.err_fn = on_err;
    cfg.err_user = &probe;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    Instance* ii = logx::test::test_get_instance(inst);

    std::vector<uint8_t> rec(32 * 1024, 'A');
    bool saw_nospace = false;
    uint64_t start = pal.wallclock_ms();
    for (int i = 0; i < 8; ++i) {
        int rc = logx_write(inst, 2, "B1Test", rec.data(), static_cast<uint32_t>(rec.size()));
        if (rc == LOGX_ERR_NO_SPACE) {
            saw_nospace = true;
            break;
        }
    }
    uint64_t cost = pal.wallclock_ms() - start;

    EXPECT_TRUE(saw_nospace);
    EXPECT_LT(cost, 100u) << "0ms 等待配置下应即刻返回，不产生退避耗时";
    EXPECT_GE(ii->drop_no_space.load(), 1u);
    EXPECT_TRUE(probe.saw_kind(LOGX_EV_DROP));
    EXPECT_TRUE(probe.saw_msg_substring("staging full"));

    logx_close(inst);
    cleanup_dir(dir);
}

// ============================================================
// B-1 方案 A (核心功能): staging_wait_ms > 0 开启有界重试，突发脉冲零丢日志
// 场景:
//   关闭自动刷盘 (flush_threshold = 0)，配置 staging_wait_ms = 1000。
//   突发写入 8 条 30KB 记录 (累计 240KB >> 128KB 暂存区总容积)。
//   在第 5 条触顶 kFull 时，logx_write 在锁外触发 flush_event 唤醒 Flusher 并退避，
//   Flusher 刷盘腾出空间后 logx_write 重试成功。
//   断言: 全部 8 条全部成功返回 LOGX_OK，drop_no_space 严格为 0，零丢弃事件。
// ============================================================
TEST(R19Regression, B1_NoSpace_BoundedRetry_SucceedsWithoutDrop) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_b1_retry_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    EventProbe probe;
    auto on_err = [](void* user, logx_event_kind_e k, int c, const char* m) {
        static_cast<EventProbe*>(user)->on_event(k, c, m);
    };

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 48 * 1024;
    cfg.flush_threshold = 0;    // 禁用自动阈值，完全依赖 B-1 环满时触发的 flush_event
    cfg.staging_wait_ms = 1000; // 允许至多 1000ms 的有界退避重试
    cfg.retention_days = 36500;
    cfg.err_fn = on_err;
    cfg.err_user = &probe;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    Instance* ii = logx::test::test_get_instance(inst);

    std::vector<uint8_t> rec(30 * 1024, 'R');
    const int kTotalWrites = 8; // 8 * 30KB = 240KB >> 128KB staging
    for (int i = 0; i < kTotalWrites; ++i) {
        ASSERT_EQ(logx_write(inst, 2, "B1Retry", rec.data(), static_cast<uint32_t>(rec.size())), LOGX_OK)
            << "第 " << i << " 条写入应有界重试成功，不应丢弃";
    }

    // 核心断言: 零丢弃、零丢弃事件、统计计数严格为 0
    EXPECT_EQ(ii->drop_no_space.load(), 0u);
    EXPECT_FALSE(probe.saw_kind(LOGX_EV_DROP));
    EXPECT_FALSE(probe.saw_msg_substring("staging full"));

    // 刷盘确认所有日志完好落盘
    ASSERT_EQ(logx_flush(inst), LOGX_OK);
    logx_close(inst);
    cleanup_dir(dir);
}

// ============================================================
// B-1 方案 A (活性防护): Flusher 停止运行时即便配置重试也快速失败
// ============================================================
TEST(R19Regression, B1_NoSpace_BoundedRetry_FailFastWhenFlusherDead) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_b1_dead_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 48 * 1024;
    cfg.flush_threshold = 0;
    cfg.staging_wait_ms = 2000; // 配置了等待 2 秒
    cfg.retention_days = 36500;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    Instance* ii = logx::test::test_get_instance(inst);

    // 填满 staging
    std::vector<uint8_t> rec(32 * 1024, 'D');
    while (true) {
        if (logx_write(inst, 2, "Dead", rec.data(), static_cast<uint32_t>(rec.size())) == LOGX_ERR_NO_SPACE) {
            break;
        }
        // 模拟 Flusher 死亡/未运行
        ii->flusher_running.store(false, std::memory_order_release);
    }

    // 在 flusher_running == false 时写满后再次写入
    uint64_t t0 = pal.wallclock_ms();
    int rc = logx_write(inst, 2, "Dead", rec.data(), static_cast<uint32_t>(rec.size()));
    uint64_t elapsed = pal.wallclock_ms() - t0;

    EXPECT_EQ(rc, LOGX_ERR_NO_SPACE);
    EXPECT_LT(elapsed, 100u) << "Flusher 死亡时应快速失败，不应等待 2000ms 超时";

    logx_close(inst);
    cleanup_dir(dir);
}

// ============================================================
// B-1 P3-2: 正在 closing 时重试循环跳出并统一记账 (drop_no_space + DROP 事件)
// ============================================================
class ClosingOnRetryPal : public Pal {
public:
    explicit ClosingOnRetryPal(Pal* real_pal) : real_(real_pal) {}

    std::atomic<bool> stall_write{false};
    std::atomic<bool> armed_closing{false};
    std::condition_variable stall_cv;
    std::mutex stall_mu;
    Instance* inst = nullptr;

    int file_open(const char* p, bool c, bool a) override { return real_->file_open(p, c, a); }
    int file_close(int fd) override { return real_->file_close(fd); }
    int file_truncate(int fd, uint64_t len) override { return real_->file_truncate(fd, len); }
    int64_t file_size(int fd) override { return real_->file_size(fd); }
    int64_t file_size_path(const char* p) override { return real_->file_size_path(p); }
    long file_pread(int fd, void* b, size_t n, uint64_t o) override { return real_->file_pread(fd, b, n, o); }

    long file_write(int fd, const void* b, size_t n) override {
        if (stall_write.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(stall_mu);
            stall_cv.wait(lock, [&]() { return !stall_write.load(std::memory_order_acquire); });
        }
        return real_->file_write(fd, b, n);
    }
    long file_pwrite(int fd, const void* b, size_t n, uint64_t o) override {
        if (stall_write.load(std::memory_order_acquire)) {
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
    int   thread_detach(void* h) override { return real_->thread_detach(h); }
    void* ev_create() override { return real_->ev_create(); }
    void  ev_destroy(void* h) override { real_->ev_destroy(h); }
    void  ev_signal(void* h) override { real_->ev_signal(h); }
    void  ev_wait(void* h) override { real_->ev_wait(h); }
    bool  ev_wait_timeout(void* h, uint32_t ms) override {
        if (armed_closing.load(std::memory_order_acquire)) {
            if (inst) {
                logx::test::test_set_closing(inst, true);
                if (h) real_->ev_signal(h);
            }
        }
        return real_->ev_wait_timeout(h, ms);
    }
    uint64_t atomic_add(uint64_t* p, int64_t d) override { return real_->atomic_add(p, d); }
    uint64_t atomic_load(uint64_t* p) override { return real_->atomic_load(p); }
    uint64_t monotonic_ns() override { return real_->monotonic_ns(); }
    uint64_t wallclock_ms() override { return real_->wallclock_ms(); }
    void     wallclock_date(uint32_t* y, uint32_t* h) override { real_->wallclock_date(y, h); }
    void     sleep_ms(uint32_t ms) override {
        if (armed_closing.load(std::memory_order_acquire)) {
            if (inst) {
                logx::test::test_set_closing(inst, true);
            }
        }
        real_->sleep_ms(ms);
    }
    uint32_t pid() override { return real_->pid(); }
    uint64_t tid() override { return real_->tid(); }
    bool     random_bytes(void* b, size_t l) override { return real_->random_bytes(b, l); }
    void     debug_log(const char* s) override { real_->debug_log(s); }
private:
    Pal* real_ = nullptr;
};

TEST(R19Regression, B1_NoSpace_ClosingDuringRetry_CountsDropAndReturnsOk) {
    Pal& real_pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_b1_closing_");
    real_pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    EventProbe probe;
    auto on_err = [](void* user, logx_event_kind_e k, int c, const char* m) {
        static_cast<EventProbe*>(user)->on_event(k, c, m);
    };

    ClosingOnRetryPal mock_pal(&real_pal);

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 48 * 1024;
    cfg.flush_threshold = 0;
    cfg.staging_wait_ms = 1000;
    cfg.retention_days = 36500;
    cfg.err_fn = on_err;
    cfg.err_user = &probe;
    cfg.pal_override = &mock_pal;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    Instance* ii = logx::test::test_get_instance(inst);
    mock_pal.inst = ii;

    // 阻塞 Flusher 刷盘，使暂存区得以稳定塞满
    mock_pal.stall_write.store(true, std::memory_order_release);

    std::vector<uint8_t> rec(32 * 1024, 'C');
    // 写满 staging (约 4 条 32KB 记录即可填满 128KB)
    for (int i = 0; i < 4; ++i) {
        logx_write(inst, 2, "Fill", rec.data(), static_cast<uint32_t>(rec.size()));
    }

    probe.clear();
    uint64_t drops_before = ii->drop_no_space.load();

    // 激活 armed_closing: 下一次 logx_write 因暂存区满进入退避 sleep_ms(2) 时，自动将 closing 置为 true
    mock_pal.armed_closing.store(true, std::memory_order_release);

    int rc = logx_write(inst, 2, "CloseRetry", rec.data(), static_cast<uint32_t>(rec.size()));

    // 核心断言 (P3-2):
    // 1. 对外语义返回 LOGX_OK (因为 closing 期间不向外部调用方报错)
    EXPECT_EQ(rc, LOGX_OK);
    // 2. 内部记账绝不静默丢失: drop 计数与事件必须如实产生
    EXPECT_EQ(ii->drop_no_space.load(), drops_before + 1);
    EXPECT_TRUE(probe.saw_kind(LOGX_EV_DROP));
    EXPECT_TRUE(probe.saw_msg_substring("staging full"));

    // 解除 Flusher 阻塞与 closing，以便安全退出
    // 持锁 store+notify: 消除 notify 落在 waiter "pred 判 false 后、阻塞前" 间隙的丢唤醒窗口
    {
        std::lock_guard<std::mutex> lk(mock_pal.stall_mu);
        mock_pal.stall_write.store(false, std::memory_order_release);
        mock_pal.stall_cv.notify_all();
    }
    // logx_close 内部通过 closing.exchange(true) 保证幂等；此处需显式复位 false，以便正常触发 Flusher 退出与资源 join 清理
    logx::test::test_set_closing(inst, false);

    logx_close(inst);
    cleanup_dir(dir);
}

// ============================================================
// N-1 (R18-2 刚性撤销屏障桩): StrictEventWaitPal
// 装饰 PAL：在开启 strict_space_wait 时，space_event 的 ev_wait_timeout
// 强制忽略 5ms 切片超时自旋，转为无界等待真实信号；
// 若删去 Flusher 或级联写线程的 ev_signal，写线程绝无法借 5ms 切片轮询自愈，
// 从而确立确定性的撤销复红屏障。
//
// Flusher 停驻门 (R33 F-1 收口): Flusher 的去抖定时器会自主 drain 填充记录，
// 其信号在 limit 布防前发出即不可控（探针实证会唤醒全部等待者使级联屏障失效）。
// hold_flusher 置位后，Flusher 线程对 flush_event 的等待被拦截到内部门闩，
// 保证填充阶段零自主 drain；release_flusher() 放行后，恰好一次 drain 在
// 限流布防下进行——级联令牌成为后继写线程的唯一唤醒源，屏障转为刚性。
// ============================================================
class StrictEventWaitPal : public Pal {
public:
    explicit StrictEventWaitPal(Pal* real_pal) : real_(real_pal) {}

    Instance* inst = nullptr;
    std::atomic<bool> strict_space_wait{false};
    std::atomic<bool> limit_flusher_space_signals{false};
    std::atomic<int> flusher_space_signals_sent{0};
    std::atomic<int> space_signal_count{0};
    std::atomic<int> space_wait_entered{0};
    std::atomic<int> flusher_parked_count{0};

    void hold_flusher_now() {
        hold_flusher.store(true, std::memory_order_release);
    }
    void release_flusher() {
        {
            std::lock_guard<std::mutex> lk(gate_mu);
            gate_open_ = true;
        }
        gate_cv.notify_all();
        hold_flusher.store(false, std::memory_order_release);
    }

    void* ev_create() override {
        void* h = real_->ev_create();
        // logx_open 首个创建的事件即 flush_event (space_event 第二)——
        // 捕获句柄使停驻门可在 inst 存在前布防 (hold 必须先于 logx_open,
        // 否则 flusher 线程抢先进入真实等待, 去抖定时器照常自主 drain, 探针实证)
        if (!flush_event_handle_) flush_event_handle_ = h;
        return h;
    }

    int file_open(const char* p, bool c, bool a) override { return real_->file_open(p, c, a); }
    int file_close(int fd) override { return real_->file_close(fd); }
    int file_truncate(int fd, uint64_t len) override { return real_->file_truncate(fd, len); }
    int64_t file_size(int fd) override { return real_->file_size(fd); }
    int64_t file_size_path(const char* p) override { return real_->file_size_path(p); }
    long file_pread(int fd, void* b, size_t n, uint64_t o) override { return real_->file_pread(fd, b, n, o); }
    long file_write(int fd, const void* b, size_t n) override { return real_->file_write(fd, b, n); }
    long file_pwrite(int fd, const void* b, size_t n, uint64_t o) override { return real_->file_pwrite(fd, b, n, o); }
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
    int   thread_detach(void* h) override { return real_->thread_detach(h); }
    void  ev_destroy(void* h) override { real_->ev_destroy(h); }
    void  ev_signal(void* h) override {
        if (inst && h == inst->space_event) {
            space_signal_count.fetch_add(1, std::memory_order_relaxed);
            uint64_t tid = real_->tid();
            if (limit_flusher_space_signals.load(std::memory_order_acquire) &&
                tid == logx::test::test_get_flusher_tid(inst)) {
                if (flusher_space_signals_sent.fetch_add(1, std::memory_order_acq_rel) >= 1) {
                    return; // 仅向 space_event 注入 Flusher 的首个唤醒脉冲，后继唤醒必须依赖级联令牌传递
                }
            }
        }
        real_->ev_signal(h);
    }
    void  ev_wait(void* h) override {
        park_flusher_if_hold(h);
        real_->ev_wait(h);
    }
    bool  ev_wait_timeout(void* h, uint32_t ms) override {
        park_flusher_if_hold(h);
        if (strict_space_wait.load(std::memory_order_acquire) && inst && h == inst->space_event) {
            space_wait_entered.fetch_add(1, std::memory_order_release);
            // 忽略传入的 wait_slice_ms (5ms) 超时参数，强制无界等待真实信号；
            // 仅真实收到 ev_signal 时才被唤醒返回 true。
            real_->ev_wait(h);
            return true;
        }
        return real_->ev_wait_timeout(h, ms);
    }
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

    // 停驻门: hold_flusher 期间 flusher 线程在 flush_event 等待点阻塞于门闩
    void park_flusher_if_hold(void* h) {
        if (!hold_flusher.load(std::memory_order_acquire)) return;
        if (!h || h != flush_event_handle_) return;
        flusher_parked_count.fetch_add(1, std::memory_order_release);
        std::unique_lock<std::mutex> lk(gate_mu);
        gate_cv.wait(lk, [this] { return gate_open_; });
    }

    std::atomic<bool> hold_flusher{false};
    void* flush_event_handle_ = nullptr;
    std::mutex gate_mu;
    std::condition_variable gate_cv;
    bool gate_open_ = false;
};

// ============================================================
// R32 方案 2: NO_SPACE 退避事件驱动化 — 空间释放即刻唤醒低延迟测试
// 验证: 写线程因环满阻塞在 space_event 时，后台 Flusher 刷盘释放空间
// 后通过 space_event 即刻唤醒写线程；在 StrictEventWaitPal 下，
// 若删去 Flusher 的 ev_signal 唤醒，写线程绝无法超时自愈，确定性复红 (R18-2)。
// ============================================================
TEST(R19Regression, B1_EventDrivenBackoff_FastWakeupOnSpaceFreed) {
    Pal& real_pal = default_pal();
    StrictEventWaitPal mock_pal(&real_pal);
    std::string dir = make_tmp_dir("logx_test_b1_ev_fast_");
    real_pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 48 * 1024;
    cfg.flush_threshold = 0;    // 禁用自动刷盘阈值，人工控制刷盘时机
    cfg.staging_wait_ms = 3000; // 允许至多 3000ms 等待
    cfg.retention_days = 36500;
    cfg.pal_override = &mock_pal;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    Instance* ii = logx::test::test_get_instance(inst);
    mock_pal.inst = ii;

    // 填满暂存区 (3 条 32KB 记录占用约 98.3KB，环剩余空间约 32.6KB < 32.8KB，第 4 条必满)
    std::vector<uint8_t> rec(32 * 1024, 'F');
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(logx_write(inst, 2, "FastFill", rec.data(), static_cast<uint32_t>(rec.size())), LOGX_OK);
    }

    std::atomic<bool> thread_started{false};
    std::atomic<bool> write_finished{false};
    std::atomic<int> write_result{-999};
    std::atomic<uint64_t> write_duration_ms{0};

    // 激活 strict_space_wait: space_event 等待强制无界阻塞（切断 5ms 轮询自愈后门）
    mock_pal.strict_space_wait.store(true, std::memory_order_release);

    // 启动写线程，第 4 条写入必因环满进入 space_event 等待
    std::thread writer([&]() {
        thread_started.store(true, std::memory_order_release);
        uint64_t t0 = real_pal.wallclock_ms();
        int rc = logx_write(inst, 2, "BlockedWrite", rec.data(), static_cast<uint32_t>(rec.size()));
        write_duration_ms.store(real_pal.wallclock_ms() - t0, std::memory_order_release);
        write_result.store(rc, std::memory_order_release);
        write_finished.store(true, std::memory_order_release);
    });

    while (!thread_started.load(std::memory_order_acquire) ||
           mock_pal.space_wait_entered.load(std::memory_order_acquire) < 1) {
        real_pal.sleep_ms(1);
    }
    // 确保写线程确已稳态处于 space_event 阻塞中
    real_pal.sleep_ms(20);

    // 主线程调用 flush 刷盘腾出空间，Flusher commit 触发 space_event 唤醒
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    // 等待写线程被 signal 唤醒并执行完毕 (至多 1500ms)
    uint64_t wait_start = real_pal.wallclock_ms();
    while (!write_finished.load(std::memory_order_acquire)) {
        if (real_pal.wallclock_ms() - wait_start > 1500) {
            break;
        }
        real_pal.sleep_ms(5);
    }

    // 撤销复红核心断言：若删去 Flusher 的 ev_signal，写线程永不被唤醒，此处必然判定失败 (RED)
    EXPECT_TRUE(write_finished.load()) << "写线程必须由 space_event 真实信号唤醒，若删去 signal 必然超时复红";
    if (!write_finished.load()) {
        // 安全退出兜底：置位 closing 并人工打信号以解除 writer 悬挂
        logx::test::test_set_closing(inst, true);
        real_pal.ev_signal(ii->space_event);
    }
    writer.join();

    EXPECT_EQ(write_result.load(), LOGX_OK);
    EXPECT_EQ(ii->drop_no_space.load(), 0u);
    EXPECT_GE(mock_pal.space_signal_count.load(), 1);
    EXPECT_LT(write_duration_ms.load(), 1500u);

    logx_close(inst);
    cleanup_dir(dir);
}

// ============================================================
// R32 方案 2: NO_SPACE 级联唤醒令牌测试 (Cascading Wake-up Token)
// 验证: 多个并发写线程在环满阻塞排队时，Flusher 刷盘释放空间后，
// 前一写线程写入成功向 space_event 传递令牌，后继写线程依次即刻唤醒并落环；
// 若删去级联令牌传递逻辑，后继写线程在 StrictEventWaitPal 保护下永不被唤醒，
// 确立刚性的撤销复红屏障 (R18-2)。
// 刚性前提 (R33 F-1): hold_flusher 停驻门保证填充阶段零自主 drain——否则
// Flusher 去抖定时器的 pre-limit 信号会唤醒全部等待者、架空级联依赖（实证）。
// ============================================================
TEST(R19Regression, B1_EventDrivenBackoff_CascadingWakeupMultiThread) {
    Pal& real_pal = default_pal();
    StrictEventWaitPal mock_pal(&real_pal);
    std::string dir = make_tmp_dir("logx_test_b1_ev_cascade_");
    real_pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 48 * 1024;
    cfg.flush_threshold = 0;    // 禁用自动刷盘
    cfg.staging_wait_ms = 3000; // 3000ms 有界等待
    cfg.retention_days = 36500;
    cfg.pal_override = &mock_pal;

    // 停驻 Flusher (R33 F-1): 必须先于 logx_open 布防——flusher 线程在 open 内
    // 即启动，若其抢先进入真实等待，去抖定时器的自主 drain 信号在 limit 布防前
    // 发出即不受限流约束（探针实证会唤醒全部等待者、架空级联屏障）。
    // 停驻后填充阶段零自主 drain，放行后的恰好一次 drain 才是唯一受控信号源。
    mock_pal.hold_flusher_now();

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    Instance* ii = logx::test::test_get_instance(inst);
    mock_pal.inst = ii;

    // 等待 Flusher 完成首轮 drain_once 并确认停驻——首轮 drain 必须面对空环
    // (填充尚未开始)，否则线程调度晚于填充时首轮即刷掉填充记录并 pre-limit 发信号
    while (mock_pal.flusher_parked_count.load(std::memory_order_acquire) < 1) {
        real_pal.sleep_ms(1);
    }

    // 填满暂存区至剩余容量不足 4KB (7 条 16KB + 1 条 15KB 占用约 129.7KB，余约 1.2KB)
    std::vector<uint8_t> rec16k(16 * 1024, 'C');
    std::vector<uint8_t> rec15k(15000, 'C');
    for (int i = 0; i < 7; ++i) {
        ASSERT_EQ(logx_write(inst, 2, "CascFill16", rec16k.data(), static_cast<uint32_t>(rec16k.size())), LOGX_OK);
    }
    ASSERT_EQ(logx_write(inst, 2, "CascFill15", rec15k.data(), static_cast<uint32_t>(rec15k.size())), LOGX_OK);

    constexpr int kWaiters = 3;
    std::vector<uint8_t> rec4k(4 * 1024, 'W'); // 3 * 4KB = 12KB，首个 16KB 刷盘释放块即可完全容纳
    std::vector<std::thread> threads;
    std::atomic<int> start_count{0};
    std::atomic<int> success_count{0};
    std::vector<std::atomic<bool>> finished(kWaiters);
    for (int i = 0; i < kWaiters; ++i) finished[i].store(false);

    // 激活 strict_space_wait: 忽略 5ms 切片超时，必须依赖真实信号/级联令牌唤醒
    mock_pal.strict_space_wait.store(true, std::memory_order_release);

    // 3 个排队写线程各自尝试写入 4KB，因环剩余约 1.2KB 均进入 space_event 等待
    for (int t = 0; t < kWaiters; ++t) {
        threads.emplace_back([&, t]() {
            start_count.fetch_add(1, std::memory_order_release);
            char tag[16];
            std::snprintf(tag, sizeof(tag), "Wait_%d", t);
            int rc = logx_write(inst, 2, tag, rec4k.data(), static_cast<uint32_t>(rec4k.size()));
            if (rc == LOGX_OK) {
                success_count.fetch_add(1, std::memory_order_release);
            }
            finished[t].store(true, std::memory_order_release);
        });
    }

    while (start_count.load(std::memory_order_acquire) < kWaiters ||
           mock_pal.space_wait_entered.load(std::memory_order_acquire) < kWaiters) {
        real_pal.sleep_ms(1);
    }
    // 稳态确认: space_wait_entered 含 sticky 消耗型重入（如初始空 drain 的遗留信号），
    // 短暂静置确保 3 线程均已真正阻塞在门闩化的 space_event 等待内
    real_pal.sleep_ms(20);
    // 激活限制 Flusher 仅注入首个唤醒信号，强制后继线程只能依赖级联令牌唤醒 (R18-2 刚性屏障)
    mock_pal.limit_flusher_space_signals.store(true, std::memory_order_release);

    // 放行 Flusher: 恰好一次 drain 在限流布防下进行 (仅首个 space 信号可通过)
    mock_pal.release_flusher();

    // 主线程 flush 腾出空间
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    // 等待所有线程通过级联令牌依次唤醒完毕
    uint64_t wait_start = real_pal.wallclock_ms();
    while (true) {
        int done = 0;
        for (int i = 0; i < kWaiters; ++i) {
            if (finished[i].load(std::memory_order_acquire)) ++done;
        }
        if (done == kWaiters || real_pal.wallclock_ms() - wait_start > 1500) {
            break;
        }
        real_pal.sleep_ms(5);
    }

    // 撤销复红核心断言：若删去 logx_write 的级联令牌 (retry_started && ev_signal)，
    // Flusher 仅唤醒 1 个线程，剩余 2 个线程将永久卡在 ev_wait 中无法唤醒！
    EXPECT_EQ(success_count.load(), kWaiters)
        << "若级联唤醒令牌失效，后继线程将卡死，必然复红";

    // 安全回收兜底
    logx::test::test_set_closing(inst, true);
    for (int i = 0; i < kWaiters; ++i) {
        real_pal.ev_signal(ii->space_event);
    }
    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(ii->drop_no_space.load(), 0u);
    // 信号次数至少为 Flusher 的 1 次 + 写线程级联传递的 2 次
    EXPECT_GE(mock_pal.space_signal_count.load(), kWaiters);

    // 验证追加的数据全部完好落盘
    logx::test::test_set_closing(inst, false);
    ASSERT_EQ(logx_flush(inst), LOGX_OK);
    logx_close(inst);
    cleanup_dir(dir);
}

// ============================================================
// T5.8 生命周期堆控制块 (Control Block 墓碑模型) 专项测试
// ============================================================

// 1. 陈旧句柄 (Stale Handle) 访问安全测试: 彻底根治 R41-05 / R44-01 尾读 UAF
TEST(ControlBlockLifetime, StaleHandleAccessReturnsStateSafe) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_stale_handle_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 64 * 1024;
    cfg.flush_threshold = 0;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    ASSERT_NE(inst, nullptr);

    // 正常写入与关闭
    std::string msg = "live_data";
    ASSERT_EQ(logx_write(inst, 2, "LIVE", reinterpret_cast<const uint8_t*>(msg.data()), static_cast<uint32_t>(msg.size())), LOGX_OK);
    ASSERT_EQ(logx_close(inst), LOGX_OK);

    // 实例主体已释放，此时 inst 为 CB 墓碑 (Tombstone)
    // 验证后续任何陈旧裸句柄访问均为合法内存访问，绝对不崩溃，并返回契约错误码
    EXPECT_EQ(logx_close(inst), LOGX_OK); // 幂等返回
    EXPECT_EQ(logx_write(inst, 2, "STALE", reinterpret_cast<const uint8_t*>(msg.data()), static_cast<uint32_t>(msg.size())), LOGX_ERR_STATE);
    EXPECT_EQ(logx_flush(inst), LOGX_ERR_STATE);
    EXPECT_EQ(logx_export(inst, "20261024", "20261024", nullptr, nullptr), LOGX_ERR_STATE);
    EXPECT_EQ(logx_set_error_handler(inst, nullptr, nullptr), LOGX_ERR_STATE);

    // get_stats 契约: 无效句柄清零输出
    logx_stats_t stats{};
    stats.drop_no_space = 999;
    logx_get_stats(inst, &stats);
    EXPECT_EQ(stats.drop_no_space, 0u);

    cleanup_dir(dir);
}

// 2. 并发双 close 测试: 验证原子 exchange(nullptr) 唯一销毁认领
TEST(ControlBlockLifetime, ConcurrentDoubleCloseIdempotent) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_double_close_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    std::atomic<int> destroy_count{0};
    auto on_destroy = [](void* user, void*) {
        auto* cnt = static_cast<std::atomic<int>*>(user);
        cnt->fetch_add(1, std::memory_order_relaxed);
    };

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 64 * 1024;
    cfg.flush_threshold = 0;
    cfg.on_destroy = on_destroy;
    cfg.user = &destroy_count;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);

    std::atomic<bool> start_gun{false};
    bool res1 = false, res2 = false;

    std::thread t1([&]() {
        while (!start_gun.load(std::memory_order_acquire)) std::this_thread::yield();
        bool closed_imm = false;
        EXPECT_EQ(logx_close_ex(inst, &closed_imm), LOGX_OK);
        res1 = closed_imm;
    });

    std::thread t2([&]() {
        while (!start_gun.load(std::memory_order_acquire)) std::this_thread::yield();
        bool closed_imm = false;
        EXPECT_EQ(logx_close_ex(inst, &closed_imm), LOGX_OK);
        res2 = closed_imm;
    });

    start_gun.store(true, std::memory_order_release);
    t1.join();
    t2.join();

    // 恰好执行一次 on_destroy
    EXPECT_EQ(destroy_count.load(), 1);
    // 至少有一个线程感知到即时销毁
    EXPECT_TRUE(res1 || res2);

    cleanup_dir(dir);
}



// 4. 2000 轮快速 open/close 墓碑内存上界压测 (R44-01 / 决策一)
TEST(ControlBlockLifetime, OpenCloseMemoryBounded) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_cb_mem_bound_");
    pal.file_mkdirs(dir.c_str());

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.max_log_length = 64 * 1024;
    cfg.flush_threshold = 0;

    constexpr int kRounds = 2000;
    for (int i = 0; i < kRounds; ++i) {
        std::string staging = dir + "/staging_" + std::to_string(i % 10) + ".mmap";
        cfg.staging_path = staging.c_str();
        logx_instance_t* inst = nullptr;
        if (logx_open(&cfg, &inst) == LOGX_OK) {
            logx_close(inst);
        }
    }
    // 顺利跑完 2000 轮 open/close 且无崩溃、无死锁即达标
    cleanup_dir(dir);
}

