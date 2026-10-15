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

// probe_record 与 cksum1 移入 logx_format.h 单源共享 (B-4)

// 环形扫描 [F, total),遇 6B 全零、撕裂、校验失败即停。
// *out_valid_end 为有效末尾(不含任何不可识别数据);*out_truncated_tail 为
// 撕裂尾巴字节数(≤ 1 条记录;置 0 表示无)。
uint32_t ring_scan(const uint8_t* raw, uint32_t f, uint32_t total,
                   uint32_t* out_valid_end, uint32_t* out_truncated_tail,
                   uint32_t wrap_boundary = 0) {
    uint32_t pos = f;
    uint32_t valid_end = f;
    while (pos < total) {
        if (wrap_boundary > 0 && pos < wrap_boundary && (wrap_boundary - pos < kHeaderLen)) {
            // 缝残留 < 6B: 位于回绕点, 跳过缝进入第二段继续扫描 (R19 F-4)。
            // 注记 (R20 复核): 此处 valid_end = wrap_boundary 有一个被依赖的
            // 副作用——扫描从 f 起步且首记录未及缝时, valid_end 直接跳到缝
            // 之后, §6 收尾清零会因此跳过 [valid_end, wrap_boundary) 段。该
            // 跳过恰好无害的前提: 这段本就是"未提交、待补刷"的数据
            // (run_recovery 的 flushed_len 提交窗口), 补刷由 Flusher 按记录
            // 边界重新扫描处理。若改动扫描起点语义或收尾清零策略, 必须重验
            // 该前提(见 run_recovery §5/§6 注记)。
            pos = wrap_boundary;
            valid_end = wrap_boundary;
            continue;
        }
        if (total - pos < kHeaderLen) {
            // 缝残留 < 6B:不识别为记录,跳过(并归零收尾);保留为撕裂尾巴。
            *out_valid_end = valid_end;
            *out_truncated_tail = total - pos;
            return pos;
        }
        // 6B 全零终止符?
        bool all_zero = true;
        for (uint32_t i = 0; i < kHeaderLen; ++i) if (raw[pos + i] != 0) { all_zero = false; break; }
        if (all_zero) {
            *out_valid_end = valid_end;
            *out_truncated_tail = 0;
            return pos;
        }
        StagingRecordHeaderView rh{};
        if (!parse_staging_record_header(raw, pos, total, &rh)) {
            // 检查是否恰好为回绕缝 (<6B 且全为 0 且到达 wrap_boundary)
            if (wrap_boundary > 0 && pos < wrap_boundary && (wrap_boundary - pos < kHeaderLen)) {
                bool seam_zero = true;
                for (uint32_t i = pos; i < wrap_boundary; ++i) {
                    if (raw[i] != 0) { seam_zero = false; break; }
                }
                if (seam_zero) {
                    pos = wrap_boundary;
                    valid_end = wrap_boundary;
                    continue;
                }
            }
            *out_valid_end = valid_end;
            *out_truncated_tail = total - pos;
            return pos;
        }
        if (!rh.valid_magic) {
            *out_valid_end = valid_end;
            *out_truncated_tail = total - pos;
            return pos;
        }
        // 校验 cksum
        if (rh.magic == kMagicEntry) {
            uint8_t actual = cksum1(raw + pos + kHeaderLen, rh.payload_len);
            if (actual != rh.cksum) {
                *out_valid_end = valid_end;
                *out_truncated_tail = total - pos;
                return pos;
            }
        } else {
            // PAD: payload 全部为 0; cksum_byte 必须为 0。
            if (rh.cksum != 0) {
                *out_valid_end = valid_end;
                *out_truncated_tail = total - pos;
                return pos;
            }
        }
        valid_end = pos + rh.total_size;
        pos += rh.total_size;
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
//   is_new_target   : 跨天或新分卷新文件(仅头、无块),非截断损坏 (R19 F-13)
struct ReconcileResult {
    uint32_t S_file = 0;
    bool     file_truncated = false;
    std::string target_path;
    bool     is_new_target = false;
    // B-10 升级 (方案 1): 异常残留分卷防御——选中目标尾块 seqno 低于目录历史分卷
    // 最大尾块时, resume_seqno 的钳制下限, 维持磁盘全局 seqno 单调递增结构不变量
    // (防恢复对账失真与离线解码器排序紊乱)。0 = 未触发。
    uint32_t seqno_floor = 0;
};

ReconcileResult reconcile(Instance* inst, const RecoveryState& hdr) {
    ReconcileResult r;
    // 优先用 hint(追加目标缓存)
    std::string hint;
    if (hdr.target.date != 0) {
        hint = make_log_path(inst->log_dir, hdr.target.date, atc_split(hdr.target));
    }
    FileScanResult sr;
    // R19 F-5 / B-7: 检查目录最新目标。若目录存在 (date, split) 大于 ATC 提示的目标，
    // 说明跨天或同日切分已创建新文件，ATC 提示属于旧文件，必须回退最新文件扫描。
    uint32_t max_dir_seqno = 0;
    std::string best = scan_dir_for_target(inst, &max_dir_seqno);
    uint32_t best_date = 0;
    uint16_t best_split = 0;
    if (!best.empty()) {
        parse_log_filename(best, &best_date, &best_split);
    }

    bool is_target_newer = false;
    if (!best.empty() && hdr.target.date != 0) {
        uint16_t hint_split = atc_split(hdr.target);
        if (best_date > hdr.target.date ||
            (best_date == hdr.target.date && best_split > hint_split)) {
            is_target_newer = true;
        }
    }

    if (is_target_newer) {
        // 跨天或同日切分后 ATC 提示文件已过时，直接使用最新的目标文件 (R19 F-5 / B-7)
        sr = scan_append_target(inst, best);
    } else if (!hint.empty()) {
        sr = scan_append_target(inst, hint);
        if (!sr.ok) {
            // 缓存指向文件不存在或不可读;扩展到全保留文件扫描
            if (!best.empty()) sr = scan_append_target(inst, best);
            else return r;  // 目录里也没有任何文件:全新启动(非截断)
        }
    } else {
        if (!best.empty()) sr = scan_append_target(inst, best);
        else return r;  // 全新启动:无任何日志文件
    }
    if (!sr.ok) {
        r.file_truncated = true;  // 文件存在但打不开:IO 错误,信任 header 重刷
        return r;
    }
    r.S_file = sr.seqno;
    r.file_truncated = sr.truncated;
    r.target_path = sr.path;
    if (is_target_newer && sr.seqno == 0 && !sr.truncated) {
        r.is_new_target = true;
    }

    // B-10 升级 (方案 1): 目录全局最大序号防御门 (Global Max Seqno Floor)。
    // 只要选中追加目标尾块低于目录历史最大尾块 (或空目标但 Header 撕裂落后于历史最大)，
    // 发 LOGX_EV_RECOVERY 告警并以 seqno_floor 钳制 resume_seqno 下限，维护全局单调递增。
    // 收口 F-1 (空残留分卷且 Header 撕裂) 与 F-2 (多级分卷/无 hint 漏防)。
    if ((sr.seqno > 0 && max_dir_seqno > sr.seqno) ||
        (sr.seqno == 0 && max_dir_seqno > hdr.seqno)) {
        r.seqno_floor = max_dir_seqno;
        emit_event(inst, LOGX_EV_RECOVERY, -1,
                   "target seqno regression vs directory max; resume clamped");
    }
    return r;
}

}  // namespace

bool run_recovery(Instance* inst) {
    // 1) 读 staging 双 Header
    RecoveryState hdr;
    bool recovered = false;
    if (!inst->staging->open(&hdr, &recovered)) {
        emit_event(inst, LOGX_EV_RECOVERY, -1, "staging open failed");
        return false;
    }

    // 2) 对账
    ReconcileResult rec;
    if (!hdr.rebuilt) {
        rec = reconcile(inst, hdr);
    } else {
        // 整区重建:seqno 接管为文件 max(seqno), 环坐标归零并提交初始 Header
        // B-10: 提取目录中历史分卷的最大 seqno，防止残留下级分卷导致序号倒退 (F-1 收口)
        uint32_t max_dir_seqno = 0;
        std::string best = scan_dir_for_target(inst, &max_dir_seqno);
        AppendTargetCache atc{};
        if (!best.empty()) {
            FileScanResult sr = scan_append_target(inst, best);
            if (sr.ok) {
                hdr.seqno = std::max(sr.seqno, max_dir_seqno);
                if (sr.truncated) {
                    // R19 F-14: 若发生截断, 为防止同文件被截断块的 seqno 与加密 nonce 被复用,
                    // 首块 seqno 须跳空隙 (hdr.seqno = max(sr.seqno, max_dir_seqno) + 1, Flusher 首块为 hdr.seqno + 1 即 max+2)
                    hdr.seqno += 1;
                    inst->truncated_blocks.fetch_add(1);
                    emit_event(inst, LOGX_EV_RECOVERY, -1, "rebuilt: appended file tail truncated");
                }
                if (max_dir_seqno > sr.seqno) {
                    emit_event(inst, LOGX_EV_RECOVERY, -1,
                               "rebuilt: target seqno regression vs directory max; resume clamped");
                }
                uint16_t split = 0;
                parse_log_filename(sr.path, &atc.date, &split);
                atc_set_split(&atc, split);
                if (!safe_assign_atc_offsets(sr.last_block_offset, sr.last_block_end_offset,
                                             &atc.last_block_offset, &atc.last_block_end_offset)) {
                    emit_event(inst, LOGX_EV_IO, -1, "scan offset exceeds atc capacity");
                }
            } else if (max_dir_seqno > 0) {
                hdr.seqno = max_dir_seqno;
            }
        } else if (max_dir_seqno > 0) {
            hdr.seqno = max_dir_seqno;
        }
        inst->staging->commit_header_locked(hdr.seqno, 0, 0, &atc);
        return true;
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
                // F-31: Σraw_len ≥ 数据区——文件端块长超出 staging 容量, 磁盘已损坏,
                // 无法对账。不推进 F 会让 [F, w) 已落块数据被重刷(重复块, 读侧
                // block_header_plausible 判伪)。宁丢不脏: F 置 w 整批丢弃并清环,
                // DROP 事件保证上层可观测(推进区间由下方不变量收尾补清零)。
                emit_event(inst, LOGX_EV_RECOVERY, -1,
                           "backfill sum exceeds staging capacity; trusting file seqno");
                emit_event(inst, LOGX_EV_DROP, -1,
                           "staging ring dropped: backfill sum exceeds capacity");
                F = w;
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
    // 只有 truncated(撕裂半块被截)时才额外发事件。
    //
    // R-02 语义锁定:truncated_blocks 只记 lost = S_hdr - seqno_file,
    // 不再 +1。理由:块提交纪律是"整块写入成功后才 commit_header_locked",
    // 撕裂尾块(文件中第 S_file+1 块)若其 Header 提交已持久化,则
    // S_hdr ≥ S_file + 1,该块天然落在差值区间 (S_file, S_hdr] 内;
    // 若其提交未持久化,则 S_hdr == S_file,走下方 else 分支单独 +1。
    // 两种情况均恰好计数一次,按报告原建议再 +1 反而会对撕裂块重复计数。
    // R19 F-13: 新分卷/新日期空文件 (rec.is_new_target 为 true) 无任何块被丢弃,
    // 不得误判定为损坏尾部而将历史 S_hdr 计入 truncated_blocks。
    if (S_hdr > seqno_file && !rec.is_new_target) {
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
    } else if (rec.is_new_target) {
        resume_seqno = S_hdr;                   // 空/仅头新分卷/新日期文件: 继承 S_hdr, 无跳空隙 (R19 F-13)
    } else if (truncated || seqno_file < S_hdr) {
        resume_seqno = std::max(S_hdr, seqno_file) + 1;  // 首块 = max + 2(跳 1 空隙)
    } else {
        resume_seqno = std::max(S_hdr, seqno_file);      // 首块 = max + 1
    }
    // B-10 方案 1 升级: 目录全局最大序号回退下限钳制 (防跨卷倒退)
    resume_seqno = std::max(resume_seqno, rec.seqno_floor);

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
    uint32_t wrap_boundary = (w < f) ? (size - f) : 0;
    ring_scan(view.data(), 0, raw_bytes, &valid_end, &truncated_tail, wrap_boundary);

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
    // F-20: 基底用 pick_header 已验证过的 hdr.target, 不再裸读 Header A(+20):
    // Header A 撕裂而 B 被选中时, 裸读会把垃圾 ATC 写回 commit_header,
    // 未来可能生成畸形日期孤儿文件(retention 不删、export 混入)。
    AppendTargetCache atc = hdr.target;
    // 更新追加目标缓存:指向 resume 目标(暂用 hint 或原 cache)
    if (!rec.target_path.empty()) {
        uint16_t split = 0;
        parse_log_filename(rec.target_path, &atc.date, &split);
        atc_set_split(&atc, split);
        // last_block_offset 与 last_block_end_offset 由下次 commit 更新
    }
    // write_pos(环坐标) = (F + valid_end) % size;若 valid_end == raw_bytes 则 = w(原值)
    uint32_t new_w = raw_bytes == valid_end
        ? w
        : (f + valid_end) % size;
    inst->staging->commit_header_locked(resume_seqno, f, new_w, &atc);

    // 恢复完成后若 staging 仍有遗留未落盘日志，将 staged_bytes_total 对齐待刷字节数 (F-08 水位线闭环)
    uint32_t rem_f, rem_snap;
    uint32_t pending = inst->staging->snapshot_pending(&rem_f, &rem_snap);
    if (pending > 0) {
        inst->staged_bytes_total.store(pending, std::memory_order_relaxed);
    }
    return true;
}

}  // namespace logx
