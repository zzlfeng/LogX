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
#include "logx_staging.h"
#include "logx_codec.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
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

void cleanup_file(const std::string& p) { default_pal().file_unlink(p.c_str()); }

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
    Instance* ii = static_cast<Instance*>(inst);
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
    Instance* ii = static_cast<Instance*>(inst);
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
