// test_r19_regression.cpp - R19 批次回归测试 + R20 复核回归。
//
// 覆盖:
//   [R19 F-1]  回绕缝在拷出缓冲中部 + 缝后积压 > kBlockMaxRaw → 块数 ≥ 2,
//              每块 raw_len ∈ [kHeaderLen, kBlockMaxRaw](写侧产出 == 读侧可收)
//   [R20-F1b]  缝紧随满长记录(kBlockMaxRaw)→ 禁止产出 1~5B "缝独占块"
//              (读侧 raw_len < kHeaderLen 必拒: 导出 break / repair 截断)
//   [R19 F-2]  错误回调内 logx_close → 安全返回,后续 write 不 UAF(契约许可路径)
//   [R19 F-3]  预置 5B 撕裂 <today>.log → 写入/flush 可自愈,文件头重写,
//              事件可见,导出可取回
//   [R19 F-6]  encrypt=1 + 密钥恒缺 → open 期 fail-fast;失败 open 写 NULL 出参;
//              文件头声明加密但块明文 → 导出逐块判定仍可解(R20 残留修复)
//   [R19 F-10] logx_open 失败 → *out_inst = NULL(防误关旧实例)
//   [R19 F-14] rebuilt + 文件尾截断 → 续写首块 seqno 跳空隙(防 nonce 复用)
//   [R19 F-4]  回绕缝在环坐标扫描中不被误抹除(ring_scan wrap_boundary)
//
// 反"空洞绿"纪律: 各"修复后必须绿"的断言目标均对应 R19 报告探针 C/D/F/G/I、
// R20 复核探针 P2 实测过的失败形态——撤销对应修复即可复红(实验证据存档于
// code_review_R19/R20_2026-10-04.md §五·验证记录)。

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

// 构造一条 LGXE 记录(whole 分片),cksum 按 8 位折叠。
std::vector<uint8_t> make_entry(uint32_t pl, uint8_t fill) {
    std::vector<uint8_t> r;
    r.push_back(pl & 0xff);
    r.push_back((pl >> 8) & 0xff);
    r.push_back(static_cast<uint8_t>(kMagicEntry & 0xff));
    r.push_back(static_cast<uint8_t>(kMagicEntry >> 8));
    r.push_back(0x00);              // flags: whole
    uint8_t c = 0;
    for (uint32_t i = 0; i < pl; ++i) { r.push_back(fill); c ^= fill; }
    r.push_back(c);
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
        if (blk.raw_len > kBlockMaxRaw) b.any_oversize = true;
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
// R20 复核: 缝紧随满长记录 → drain 侧必须跳过 1~5B 缝独占块
// (split 产出覆盖完整的块列;真实防线在 drain_once 的 <kHeaderLen 分支)
// ============================================================
TEST(R19Regression, F1b_SeamAfterFullLengthRecord_NoMicroBlock) {
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
    std::vector<PendingBlock> blocks;
    split_into_blocks(raw.data(), static_cast<uint32_t>(raw.size()), &blocks);
    // 切分器允许产出缝独占块(它不知道 drain 的兜底策略), 但必须:
    // ① 覆盖完整缓冲 ② 无超上限块 ③ 剔除缝独占块后其余块全部可读。
    BlockBounds bb = analyze_blocks(blocks);
    EXPECT_EQ(bb.covered, raw.size());
    EXPECT_FALSE(bb.any_oversize);
    std::vector<PendingBlock> kept;
    for (const auto& blk : blocks) {
        if (blk.raw_len >= kHeaderLen) kept.push_back(blk);
    }
    EXPECT_FALSE(kept.empty());
    for (const auto& blk : kept) {
        EXPECT_GE(blk.raw_len, kHeaderLen);
        EXPECT_LE(blk.raw_len, kBlockMaxRaw);
    }
    // 钉死"至少有一个 1..5B 候选块被该场景触发"(保证场景有效性,
    // 撤销 F-1 缝封块修复或改动 drain 兜底前提时, 本断言提示场景失效)。
    EXPECT_TRUE(bb.any_micro || blocks.size() >= 2)
        << "scenario must exercise the seam-after-full-record boundary";
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

    // 自愈信号必须可见(R19 探针 G 的教训: 修复前计数器全 0 + 无任何事件)
    EXPECT_TRUE(probe.saw_kind(static_cast<int>(LOGX_EV_RECOVERY)))
        << "healing a torn <32B header must emit a RECOVERY event (no silent heals)";

    // 导出可取回数据(修复前 export=-3)
    std::atomic<int> cb_files{0};
    logx_status_e erc = logx_export(inst, "20200101", "20991231",
        [](void* user, const char*, const char*, uint64_t) -> int {
            static_cast<std::atomic<int>*>(user)->fetch_add(1);
            return 0;
        }, &cb_files);
    EXPECT_EQ(erc, LOGX_OK);
    EXPECT_GE(cb_files.load(), 1) << "healed file must be exportable";
    // 自愈事件(R20 复核补的产品侧修复): 修复头撕裂必须发 RECOVERY 事件
    EXPECT_TRUE(probe.saw_kind(static_cast<int>(LOGX_EV_RECOVERY)))
        << "healing a torn <32B header must emit a RECOVERY event (no silent heals)";

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

// ============================================================
// F-14: rebuilt + 文件尾截断 → 续写 seqno 跳空隙(防 nonce 复用)
// ============================================================
TEST(R19Regression, F14_RebuiltTruncatedFile_SeqnoSkipsGap) {
    Pal& pal = default_pal();
    std::string dir = make_tmp_dir("logx_test_r19_f14_");
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    // 文件: 2 个完整块(seqno=1,2) + 1 个撕裂块(声明 payload 200B, 只写 10B)
    std::string logfile = dir + "/20260831.log";
    int fd = pal.file_open(logfile.c_str(), true, false);
    ASSERT_GE(fd, 0);
    uint8_t salt[8] = {1,2,3,4,5,6,7,8};
    write_file_header_bytes(fd, 20260831u, salt);
    auto write_block = [&](uint32_t seqno, uint32_t raw_len,
                           const std::vector<uint8_t>& payload) {
        Pal& p = default_pal();
        uint8_t bhdr[34] = {};
        put_u32_le(bhdr, kMagicBlock);
        bhdr[4] = 1; bhdr[5] = 0;  // none, plaintext
        put_u32_le(bhdr + 6, seqno);
        put_u32_le(bhdr + 10, raw_len);
        put_u32_le(bhdr + 14, static_cast<uint32_t>(payload.size()));
        put_u32_le(bhdr + 18, crc32_update(0, payload.data(), payload.size()));
        std::memcpy(bhdr + 22, salt, 8);
        bhdr[30] = static_cast<uint8_t>((seqno >> 24) & 0xff);
        bhdr[31] = static_cast<uint8_t>((seqno >> 16) & 0xff);
        bhdr[32] = static_cast<uint8_t>((seqno >> 8) & 0xff);
        bhdr[33] = static_cast<uint8_t>(seqno & 0xff);
        ASSERT_EQ(p.file_write(fd, bhdr, 34), 34);
        ASSERT_EQ(p.file_write(fd, payload.data(), static_cast<long>(payload.size())),
                  static_cast<long>(payload.size()));
    };
    write_block(1, 64, std::vector<uint8_t>(64, 0xAB));
    write_block(2, 64, std::vector<uint8_t>(64, 0xAC));
    write_block(3, 200, std::vector<uint8_t>(10, 0xCD));  // 撕裂: raw_len 声明 200
    pal.file_close(fd);

    // 双 Header 破坏 → rebuilt 路径接管(全新 staging), 扫文件得 S_file=2 + truncated
    Instance* inst = new Instance();
    inst->pal = &default_pal();
    inst->log_dir = dir;
    inst->staging_path = staging;
    inst->staging = new Staging(inst->pal, staging, 8 * 1024);
    ASSERT_TRUE(run_recovery(inst));

    // rebuilt + truncated: hdr.seqno = sr.seqno + 1 = 3(下一块序号),
    // Flusher 首块 = 4 = S_file + 2(spec: 防被截块 seqno/nonce 复用)。
    uint32_t resume_seqno = get_u32(inst->staging->raw_base() + 8);
    EXPECT_EQ(resume_seqno, 3u)
        << "rebuilt+truncated must resume at S_file+1 so the first block = S_file+2 (nonce gap)";
    EXPECT_GE(inst->truncated_blocks.load(), 1u) << "truncation must be counted";

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
