// recovery.cpp - 启动恢复与对账。
//
// spec §4.3 决策树:
//   读双 Header -> 追加目标文件 -> k 块回溯对账四分支 -> 环形扫描 staging
//   -> 不变量收尾(清零跨过区间 + 撕裂尾巴)
//
// 四分支:
//   S_file == S_hdr      -> 常规;从 F 起补刷
//   S_file >  S_hdr      -> 块已写但 Header 未提交;Σraw_len 回推 F,剩余补刷
//                           跨过的区间可能尚未清零,由不变量收尾补清
//   S_file <  S_hdr      -> 文件尾损坏;截到 S_file,丢失打点,续写 seqno 跳 1
//   文件缺失/长度异常    -> 信任 header,重刷 staging
// 双 Header 均损坏       -> 整区重建,新 seqno = 文件 max(seqno)
//
// seqno 生命周期注记(CHANGELOG 遗留项 5):resume_seqno 统一取"下一块序号"。
// 写入路径(Flusher)以 cur_seqno + 1 作为首块 seqno,故此处:
//   无任何已落盘块        -> resume = 0,首块从 1 开始
//   常规续写              -> resume = max(S_hdr, S_file),首块 = max + 1
//   截断后续写(跳 1 空隙)-> resume = max + 1,首块 = max + 2(spec §3.2 防 nonce 重用)

#include "logx_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace logx {

namespace {

// 读 raw 区里一条记录,返回该记录的字节数(头+payload);0 表示到达尾部/失败。
// 跨回绕处理由 caller 保证 raw 是连续 buffer。
uint32_t probe_record(const uint8_t* raw, uint32_t off, uint32_t total) {
    if (total - off < 6) return 0;
    uint32_t len = uint32_t(raw[off]) | (uint32_t(raw[off + 1]) << 8);
    uint32_t sz = 6 + len;
    if (off + sz > total) return 0;
    return sz;
}

// 计算一条 staging 记录的 payload 校验和(8 位折叠)。
uint8_t cksum1(const uint8_t* p, uint32_t n) {
    uint8_t c = 0;
    for (uint32_t i = 0; i < n; ++i) c ^= p[i];
    return c;
}

// 环形扫描 [F, total),遇 6B 全零、撕裂、校验失败即停。
// *out_valid_end 为有效末尾(不含任何不可识别数据);*out_truncated_tail 为
// 撕裂尾巴字节数(≤ 1 条记录;置 0 表示无)。
uint32_t ring_scan(const uint8_t* raw, uint32_t f, uint32_t total,
                   uint32_t* out_valid_end, uint32_t* out_truncated_tail) {
    uint32_t pos = f;
    uint32_t valid_end = f;
    while (pos < total) {
        if (total - pos < 6) {
            // 缝残留 < 6B:不识别为记录,跳过(并归零收尾);保留为撕裂尾巴。
            *out_valid_end = valid_end;
            *out_truncated_tail = total - pos;
            return pos;
        }
        // 6B 全零终止符?
        bool all_zero = true;
        for (uint32_t i = 0; i < 6; ++i) if (raw[pos + i] != 0) { all_zero = false; break; }
        if (all_zero) {
            *out_valid_end = valid_end;
            *out_truncated_tail = 0;
            return pos;
        }
        uint32_t rec = probe_record(raw, pos, total);
        if (rec == 0) {
            *out_valid_end = valid_end;
            *out_truncated_tail = total - pos;
            return pos;
        }
        // 校验 cksum
        uint16_t magic = uint16_t(raw[pos + 2]) | (uint16_t(raw[pos + 3]) << 8);
        uint8_t cksum_byte = raw[pos + 5];
        uint32_t payload_len = uint32_t(raw[pos]) | (uint32_t(raw[pos + 1]) << 8);
        // magic 必须合法(LGXGXX / GP)
        bool valid_magic = (magic == kMagicEntry || magic == kMagicPad);
        if (!valid_magic) {
            *out_valid_end = valid_end;
            *out_truncated_tail = total - pos;
            return pos;
        }
        if (magic == kMagicEntry) {
            uint8_t actual = cksum1(raw + pos + 6, payload_len);
            if (actual != cksum_byte) {
                *out_valid_end = valid_end;
                *out_truncated_tail = total - pos;
                return pos;
            }
        } else {
            // PAD:c payload 全部为 0;cksum_byte 必须为 0。
            if (cksum_byte != 0) {
                *out_valid_end = valid_end;
                *out_truncated_tail = total - pos;
                return pos;
            }
        }
        valid_end = pos + rec;
        pos += rec;
    }
    *out_valid_end = valid_end;
    *out_truncated_tail = 0;
    return pos;
}

// 启动 k 块回溯对账(在 run_recovery 中调用)。
// 返回:
//   S_file          : 文件最后有效块 seqno
//   file_truncated  : 文件发生过截断(影响续写 seqno)
//   target_path     : 命中的文件路径(用于 k 块回溯与回写 atc)
struct ReconcileResult {
    uint32_t S_file = 0;
    bool     file_truncated = false;
    std::string target_path;
};

ReconcileResult reconcile(Instance* inst, const RecoveryState& hdr) {
    ReconcileResult r;
    // 优先用 hint(追加目标缓存)
    std::string hint;
    if (hdr.target.date != 0) {
        hint = make_log_path(inst->log_dir, hdr.target.date, hdr.target.split_no);
    }
    FileScanResult sr;
    bool was_truncated = false;
    if (!hint.empty()) {
        sr = scan_append_target(inst, hint);
        if (sr.truncated) was_truncated = true;
        if (!sr.ok) {
            // 缓存指向文件不存在或不可读;扩展到全保留文件扫描
            std::string best = scan_dir_for_target(inst);
            if (!best.empty()) sr = scan_append_target(inst, best);
            else return r;  // 目录里也没有任何文件:全新启动(非截断)
        }
    } else {
        std::string best = scan_dir_for_target(inst);
        if (!best.empty()) sr = scan_append_target(inst, best);
        else return r;  // 全新启动:无任何日志文件
    }
    if (!sr.ok) {
        r.file_truncated = true;  // 文件存在但打不开:IO 错误,信任 header 重刷
        return r;
    }
    r.S_file = sr.seqno;
    r.file_truncated = sr.truncated || was_truncated;
    r.target_path = sr.path;
    return r;
}

}  // namespace

void run_recovery(Instance* inst) {
    // 1) 读 staging 双 Header
    RecoveryState hdr;
    bool recovered = false;
    if (!inst->staging->open(&hdr, &recovered)) {
        emit_event(inst, LOGX_EV_RECOVERY, -1, "staging open failed");
        return;
    }

    // 2) 对账
    ReconcileResult rec;
    if (!hdr.rebuilt) {
        rec = reconcile(inst, hdr);
    } else {
        // 整区重建:seqno 接管为文件 max(seqno), 环坐标归零并提交初始 Header
        std::string best = scan_dir_for_target(inst);
        AppendTargetCache atc{};
        if (!best.empty()) {
            FileScanResult sr = scan_append_target(inst, best);
            if (sr.ok) {
                hdr.seqno = sr.seqno;
                parse_log_filename(sr.path, &atc.date, &atc.split_no);
                atc.last_block_offset = static_cast<uint32_t>(sr.last_block_offset);
                atc.last_block_end_offset = static_cast<uint32_t>(sr.last_block_end_offset);
            }
        }
        inst->staging->commit_header_locked(hdr.seqno, 0, 0, &atc);
        return;
    }

    // 3) 决定 S_hdr / S_file / Σraw_len 回推 / 是否截断
    uint32_t S_hdr = hdr.seqno;
    uint32_t F = hdr.flushed_len;
    uint32_t seqno_file = rec.S_file;
    bool truncated = rec.file_truncated;

    uint32_t size = inst->staging->data_size();

    // 防御:撕裂写可能让 Header 的裸 write_pos / flushed_len 越界(mmap 页内
    // 部分写入),越界坐标一律当 0 处理(整区零态),宁丢不脏。
    if (F >= size) F = 0;
    uint32_t F_orig = F;  // 回推前的 flushed_len(不变量收尾清零 [F_orig, F) 用)
    uint32_t w_raw = get_u32(inst->staging->raw_base() + 16);
    uint32_t w = (w_raw < size) ? w_raw : 0;

    // 跨过区间(S_file > S_hdr 分支)由 F_orig → F 的推进体现,不变量收尾
    // 按坐标差清零。

    if (seqno_file == S_hdr) {
        // 常规:无需回推 F
    } else if (seqno_file > S_hdr) {
        // 块已写入但 Header 未提交(spec §4.3):
        //   取 seqno ∈ (S_hdr, S_file] 的块累加 raw_len,F = (F + Σraw_len) mod size,
        //   seqno = S_file;剩余部分由环形扫描 + 补刷处理。
        // raw_len 是块头自描述字段(§3.2),这些块由 Flusher 从 staging
        // [旧 F, …) 顺序写出,Σraw_len 即精确跨过跨度,无需依赖 atc 缓存
        // (缓存为空的"首块未提交"场景同样成立,不能退化为重复补写)。
        uint64_t sum = 0, blk_off = 0, blk_end = 0;
        if (sum_raw_len_since(inst, rec.target_path, S_hdr,
                              &sum, &blk_off, &blk_end) && sum > 0) {
            if (sum < size) {
                F = static_cast<uint32_t>((static_cast<uint64_t>(F) + sum) % size);
            } else {
                emit_event(inst, LOGX_EV_RECOVERY, -1,
                           "backfill sum exceeds staging capacity; trusting file seqno");
            }
        } else {
            // 回溯失败(IO 错误 / 文件内无更后继的完整块):退化为信任文件 seqno
            emit_event(inst, LOGX_EV_RECOVERY, -1,
                       "blocks written but header not committed; trusting file seqno");
        }
        // 本分支跨过的区间可能尚未清零(崩溃落在 §4.2 第 4→5 步之间),
        // 由下方不变量收尾统一补清。
    }

    // 文件尾被截断(S_file < S_hdr):无论文件尾是否"干净"(断电丢块时
    // fdatasync 缺席,文件以完整块结尾、无撕裂痕迹),丢失都必须打点;
    // 只有 truncated(撕裂半块被截)时才额外 +1 兜底计数。
    if (S_hdr > seqno_file) {
        uint32_t lost = S_hdr - seqno_file;
        inst->truncated_blocks.fetch_add(lost);
        if (truncated) {
            emit_event(inst, LOGX_EV_RECOVERY, -1, "appended file tail truncated");
        }
    } else if (truncated) {
        emit_event(inst, LOGX_EV_RECOVERY, -1, "appended file tail truncated");
        inst->truncated_blocks.fetch_add(1);
    }

    // 4) 续写首块 seqno(语义对齐见文件头注记):
    //    resume 是"下一块序号",Flusher 首块取 resume + 1。
    uint32_t resume_seqno;
    if (seqno_file == 0 && S_hdr == 0 && !truncated) {
        resume_seqno = 0;                       // 全新启动:首块 = 1
    } else if (truncated || seqno_file < S_hdr) {
        resume_seqno = std::max(S_hdr, seqno_file) + 1;  // 首块 = max + 2(跳 1 空隙)
    } else {
        resume_seqno = std::max(S_hdr, seqno_file);      // 首块 = max + 1
    }

    // 5) 环形扫描 staging:上界 = 裸 write_pos(崩溃前沿;Header 只在逐块提交时
    //    更新,最后一批未提交记录坐标仅存在于裸 write_pos 字段)。
    //    注意跨过区间 [旧 F, 新 F) 在 S_file > S_hdr 时属于"文件已持有"的已刷
    //    数据,不参与扫描——扫描必须从新 F 出发。
    uint32_t f = F;
    uint32_t raw_bytes = (w >= f) ? (w - f) : (size - f + w);

    std::vector<uint8_t> view(raw_bytes);
    if (raw_bytes > 0) {
        if (w >= f) {
            std::memcpy(view.data(), inst->staging->data_base() + f, raw_bytes);
        } else {
            uint32_t first = size - f;
            std::memcpy(view.data(), inst->staging->data_base() + f, first);
            std::memcpy(view.data() + first, inst->staging->data_base(),
                        raw_bytes - first);
        }
    }

    uint32_t valid_end = 0, truncated_tail = 0;
    ring_scan(view.data(), 0, raw_bytes, &valid_end, &truncated_tail);

    // 6) 不变量收尾(spec §4.3):提交新 Header 前必须让"除 [新 f, 新 w) 外
    //    全环为零"成立。需要补清的两段:
    //    a) S_file > S_hdr 分支跨过的区间 [F_orig, F) —— 可能未清零
    //    b) 环形扫描发现的撕裂尾巴 [valid_end, valid_end + truncated_tail)
    //    重刷补写经 Flusher 流程会顺带清零 [F, W'),实际只需补清上述两段。
    if (F != F_orig) {
        uint32_t span = (F - F_orig + size) % size;
        if (span == 0) span = size;  // 整圈(理论不可达:Σraw_len < 数据区)
        inst->staging->zero_range_locked(F_orig, span);
    }
    if (truncated_tail > 0) {
        uint32_t zero_off = (f + valid_end) % size;
        inst->staging->zero_range_locked(zero_off, truncated_tail);
    }

    // 7) 写新 Header(seqno = resume_seqno,flushed_len = f,write_pos = 环扫描末尾)
    AppendTargetCache atc{};
    std::memcpy(&atc, inst->staging->raw_base() + 20, sizeof(atc));
    // 更新追加目标缓存:指向 resume 目标(暂用 hint 或原 cache)
    if (!rec.target_path.empty()) {
        parse_log_filename(rec.target_path, &atc.date, &atc.split_no);
        // last_block_offset 与 last_block_end_offset 由下次 commit 更新
    }
    // write_pos(环坐标) = (F + valid_end) % size;若 valid_end == raw_bytes 则 = w(原值)
    uint32_t new_w = raw_bytes == valid_end
        ? w
        : (f + valid_end) % size;
    inst->staging->commit_header_locked(resume_seqno, f, new_w, &atc);
}

}  // namespace logx
