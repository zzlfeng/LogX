// flusher.cpp - 单 Flusher 线程 + 块切分 + 文件 IO。
//
// 节:
//   A. 块切分(按记录边界,raw_len 恒记录对齐)
//   B. 块写出 / 文件打开 + 截断 + 校验
//   C. 文件路径与按天轮换 / 切分 / 保留期清理
//   D. Flusher 主循环
//   E. flush_blocking / export 调用入口
//
// 所有按块/按文件 IO 的 syscall 都收口在这里;其它模块通过 logx_internal.h
// 调用本文件实现的函数。

#include "logx_internal.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "logx_format.h"

namespace logx {

namespace {

constexpr uint32_t kNormalBlockMax = 16 * 1024;
constexpr uint32_t kSingleRecordMaxRaw = 20 * 1024 + 6;

// put_u32_le 见 logx_internal.h(测试可见)

// 在 raw 流里看下一条记录的字节数(头+payload);不足返回 0。
uint32_t peek_record_len(const uint8_t* p, uint32_t off, uint32_t total) {
    if (total - off < 6) return 0;
    uint16_t magic = uint16_t(p[off + 2]) | (uint16_t(p[off + 3]) << 8);
    if (magic != kMagicEntry && magic != kMagicPad) return 0;
    uint32_t len = uint32_t(p[off]) | (uint32_t(p[off + 1]) << 8);
    uint32_t sz = 6 + len;
    if (off + sz > total) return 0;
    return sz;
}

// ---- §A 块切分 ----
uint32_t split_impl(const uint8_t* raw, uint32_t total,
                    std::vector<PendingBlock>* out) {
    out->clear();
    uint32_t pos = 0;
    while (pos < total) {
        uint32_t blk_begin = pos;
        if (total - pos < 6) {
            // 缝残留 < 6B:归入本块并封块;消费侧按"距段尾 <6B 跳过"处理
            out->push_back({blk_begin, total - blk_begin});
            break;
        }
        uint32_t first = peek_record_len(raw, pos, total);
        if (first == 0) {
            // 撕裂尾部:封块到 total(其中可能有半条记录被丢)
            out->push_back({blk_begin, total - blk_begin});
            break;
        }
        if (first > kSingleRecordMaxRaw) {
            // 单条独占一块
            out->push_back({blk_begin, first});
            pos += first;
            continue;
        }
        // 普通累计:沿记录边界累加,加下一条超过 16KB 则封块
        uint32_t cur = pos;
        while (cur < total) {
            if (total - cur < 6) { cur = total; break; }
            uint32_t rec = peek_record_len(raw, cur, total);
            if (rec == 0) { cur = total; break; }
            if (cur + rec - blk_begin > kNormalBlockMax && cur > blk_begin) break;
            cur += rec;
        }
        out->push_back({blk_begin, cur - blk_begin});
        pos = cur;
    }
    return static_cast<uint32_t>(out->size());
}

// ---- §B 块写出 ----
// 把 raw_block + seqno + flag 编码后追加到 fd;返回写入字节数;失败 -1。
// flag 同时表达压缩算法与加密开关。
long append_block_raw(int fd,
                      const uint8_t* raw_block, uint32_t raw_len,
                      uint32_t seqno, uint8_t flag,
                      const uint8_t salt[8],
                      const uint8_t* key32_or_null,
                      Instance* inst) {
    if (!inst) return -1;
    // 1) 压缩 + 加密
    if ((flag & kFlagEncrypted) && key32_or_null == nullptr) {
        emit_event(inst, LOGX_EV_DEGRADED, -1, "missing key; downgrade to plaintext");
        flag &= ~kFlagEncrypted;
    }
    EncodedBlock enc;
    if (!encode_block(flag, key32_or_null, seqno, salt, raw_block, raw_len, &enc)) {
        inst->block_write_errors.fetch_add(1);
        return -1;
    }
    // 若发生压缩降级,通知监控层
    uint8_t orig_comp = flag_algo(flag);
    uint8_t final_comp = flag_algo(enc.final_flag);
    if (orig_comp != kAlgoNone && final_comp == kAlgoNone) {
        emit_event(inst, LOGX_EV_DEGRADED, -1, "compression failed; downgrade to none");
    }
    // 2) 确定 payload_len 与 CRC32(payload)
    uint32_t payload_len = enc.enc_ok
        ? static_cast<uint32_t>(enc.payload.size() - 16)
        : static_cast<uint32_t>(enc.payload.size());
    uint32_t crc = crc32_update(0, enc.payload.data(), payload_len);
    // 3) 构造 34B 头
    uint8_t hdr[34];
    put_u32_le(hdr + 0, kMagicBlock);
    hdr[4] = 1;
    hdr[5] = enc.final_flag;
    put_u32_le(hdr + 6,  seqno);
    put_u32_le(hdr + 10, raw_len);
    put_u32_le(hdr + 14, payload_len);
    put_u32_le(hdr + 18, crc);
    std::memcpy(hdr + 22, salt, 8);
    hdr[30] = (seqno >> 24) & 0xff;
    hdr[31] = (seqno >> 16) & 0xff;
    hdr[32] = (seqno >> 8) & 0xff;
    hdr[33] = (seqno) & 0xff;
    // 4) 拼装 frame
    std::vector<uint8_t> frame;
    frame.reserve(34 + enc.payload.size());
    frame.insert(frame.end(), hdr, hdr + 34);
    frame.insert(frame.end(), enc.payload.begin(), enc.payload.end());
    if (enc.enc_ok) {
        // encode_block 已把 [cipher | tag] 合并到 enc.payload 尾部 16B,这里不再追加
    }
    // 5) 写盘
    long n = inst->pal->file_write(fd, frame.data(), frame.size());
    if (n < 0) {
        inst->block_write_errors.fetch_add(1);
        return -1;
    }
    return n;
}

// 一次"块落地"完整流程:定义在文件末尾 anonymous namespace 之外
// (MSVC 对 anonymous 内定义与 header 声明不 merge,会产生重载歧义)。

}  // namespace

BlockWriteResult write_and_record(Instance* inst, int fd,
                                  const uint8_t* raw_block, uint32_t raw_len,
                                  uint32_t seqno, uint8_t flag,
                                  const uint8_t salt[8],
                                  const uint8_t* key32_or_null) {
    BlockWriteResult r;
    int64_t sz = inst->pal->file_size(fd);
    if (sz < 0) {
        inst->block_write_errors.fetch_add(1);
        return r;
    }
    r.start_offset = static_cast<uint64_t>(sz);
    long n = append_block_raw(fd, raw_block, raw_len, seqno, flag, salt,
                              key32_or_null, inst);
    if (n < 0) {
        return r;
    }
    r.end_offset = r.start_offset + static_cast<uint64_t>(n);
    r.ok = true;
    return r;
}

// ---- §C 文件路径 / 清理 ----
std::string make_log_path(const std::string& dir, uint32_t yyyymmdd, uint8_t split) {
    char buf[128];
    if (split == 0) {
        std::snprintf(buf, sizeof(buf), "%s/%08u.log", dir.c_str(), yyyymmdd);
    } else {
        std::snprintf(buf, sizeof(buf), "%s/%08u.log.%u", dir.c_str(), yyyymmdd, split);
    }
    return std::string(buf);
}

// 写 32B 文件头(magic + ver + flag + date + kid + salt + reserved + crc)。
// 仅在"新建文件"时调用。
bool write_file_header(Instance* inst, int fd,
                        uint32_t date, uint16_t kid, const uint8_t salt[8]) {
    uint8_t hdr[32];
    std::memset(hdr, 0, sizeof(hdr));
    put_u32_le(hdr + 0, kMagicFile);
    hdr[4] = 1;     // ver
    hdr[5] = make_flag(inst->compress_algo, inst->encrypt_enable != 0);     // flag
    put_u32_le(hdr + 6, date);
    put_u16(hdr + 10, kid);
    std::memcpy(hdr + 12, salt, 8);
    // reserved[8] = 0
    uint32_t crc = crc32_update(0, hdr, 28);
    put_u32_le(hdr + 28, crc);
    return inst->pal->file_write(fd, hdr, 32) == 32;
}

// 扫描 hint_path;若文件不存在则返回 ok=false 让上层决定创建。
// repair = true 时对损坏尾部做 ftruncate 修复(确定追加目标时用);
// false 时只评估不修改(scan_dir_for_target 遍历候选文件时用,避免副作用)。
FileScanResult scan_one(Instance* inst, const std::string& path, bool repair) {
    FileScanResult r;
    r.path = path;
    int fd = inst->pal->file_open(path.c_str(), false, false);
    if (fd < 0) {
        r.ok = false;
        return r;
    }
    int64_t sz = inst->pal->file_size(fd);
    if (sz < 0) {
        inst->pal->file_close(fd);
        r.ok = false;
        return r;
    }
    r.last_block_end_offset = static_cast<uint64_t>(sz);
    if (sz < 32) {
        // 文件过短,视为空文件(允许;将由 write_file_header 覆盖)
        inst->pal->file_close(fd);
        r.ok = true;
        r.seqno = 0;
        r.last_block_end_offset = 0;
        return r;
    }
    // 读 32B 头
    uint8_t hdr[32];
    long nr = inst->pal->file_pread(fd, hdr, 32, 0);
    if (nr != 32) {
        inst->pal->file_close(fd);
        r.ok = false;
        return r;
    }
    if (get_u32(hdr) != kMagicFile || get_u32(hdr + 28) != crc32_update(0, hdr, 28)) {
        // 头坏:截到 0
        if (repair) inst->pal->file_truncate(fd, 0);
        r.ok = true;
        r.truncated = true;
        r.last_block_end_offset = 0;
        inst->pal->file_close(fd);
        return r;
    }
    // 顺序扫剩余块
    uint64_t off = 32;
    while (off < static_cast<uint64_t>(sz)) {
        uint8_t bhdr[34];
        if (inst->pal->file_pread(fd, bhdr, 34, off) != 34) {
            if (repair) inst->pal->file_truncate(fd, off);
            r.truncated = true;
            r.last_block_end_offset = off;
            inst->pal->file_close(fd);
            r.ok = true;
            return r;
        }
        if (get_u32(bhdr) != kMagicBlock) {
            if (repair) inst->pal->file_truncate(fd, off);
            r.truncated = true;
            r.last_block_end_offset = off;
            inst->pal->file_close(fd);
            r.ok = true;
            return r;
        }
        uint32_t payload_len = get_u32(bhdr + 14);
        uint8_t flag = bhdr[5];
        uint64_t block_size = 34 + payload_len + (flag_encrypted(flag) ? 16 : 0);
        if (off + block_size > static_cast<uint64_t>(sz)) {
            if (repair) inst->pal->file_truncate(fd, off);
            r.truncated = true;
            r.last_block_end_offset = off;
            inst->pal->file_close(fd);
            r.ok = true;
            return r;
        }
        // 校验块 CRC32(payload) 与 Tag 可读性 (宁丢不脏)
        uint32_t expected_crc = get_u32(bhdr + 18);
        bool block_valid = true;
        if (payload_len > 0) {
            std::vector<uint8_t> pbuf(payload_len);
            if (inst->pal->file_pread(fd, pbuf.data(), payload_len, off + 34) != static_cast<long>(payload_len)) {
                block_valid = false;
            } else if (crc32_update(0, pbuf.data(), payload_len) != expected_crc) {
                block_valid = false;
            }
        } else if (expected_crc != 0) {
            block_valid = false;
        }
        if (block_valid && (flag & kFlagEncrypted)) {
            uint8_t tag[16];
            if (inst->pal->file_pread(fd, tag, 16, off + 34 + payload_len) != 16) {
                block_valid = false;
            }
        }
        if (!block_valid) {
            if (repair) inst->pal->file_truncate(fd, off);
            r.truncated = true;
            r.last_block_end_offset = off;
            inst->pal->file_close(fd);
            r.ok = true;
            return r;
        }
        r.seqno = get_u32(bhdr + 6);
        r.last_block_offset = off;
        r.last_block_end_offset = off + block_size;
        off += block_size;
    }
    r.ok = true;
    inst->pal->file_close(fd);
    return r;
}

// 在 hint_path 命中失败时,从 log_dir 扫所有文件;选"文件名日期最大且尾块
// seqno 最大"者作为追加目标。
// 打开/创建追加目标文件;返回 fd;失败 -1。
// *out_path / *out_salt 由调用方在 retry 时复用。
int open_or_create_append_target(Instance* inst, const std::string& hint_path,
                                 uint32_t date, uint8_t split_no,
                                 std::string* out_path,
                                 uint8_t out_salt[8]) {
    std::string path = hint_path.empty()
        ? make_log_path(inst->log_dir, date, split_no)
        : hint_path;
    int fd = inst->pal->file_open(path.c_str(), true, true);
    if (fd < 0) return -1;
    int64_t sz = inst->pal->file_size(fd);
    if (sz < 0) { inst->pal->file_close(fd); return -1; }
    if (sz == 0) {
        // 新建:写 32B 头
        uint8_t salt[8];
        inst->pal->random_bytes(salt, 8);
        if (!write_file_header(inst, fd, date, inst->kid, salt)) {
            inst->pal->file_close(fd); return -1;
        }
        std::memcpy(out_salt, salt, 8);
    } else {
        // 已存在:读头取 salt
        uint8_t hdr[32];
        if (inst->pal->file_pread(fd, hdr, 32, 0) != 32) { inst->pal->file_close(fd); return -1; }
        std::memcpy(out_salt, hdr + 12, 8);
    }
    if (out_path) *out_path = path;
    return fd;
}

void retention_cleanup_impl(Instance* inst) {
    auto files = inst->pal->file_list(inst->log_dir.c_str());
    std::sort(files.begin(), files.end());
    auto parse_date = [](const std::string& path, uint32_t* out_date) -> bool {
        size_t pos = path.find_last_of("/\\");
        std::string name = (pos == std::string::npos) ? path : path.substr(pos + 1);
        if (name.find("staging") != std::string::npos) return false;
        if (name.size() < 12) return false;
        if (name.compare(name.size() - 4, 4, ".log") != 0 &&
            name.find(".log.") == std::string::npos) return false;
        char* endp = nullptr;
        unsigned long d = std::strtoul(name.c_str(), &endp, 10);
        if (endp == name.c_str() || d == 0 || d > 99991231u) return false;
        *out_date = static_cast<uint32_t>(d);
        return true;
    };
    uint32_t yyyymmdd_now = 0, hhmmss_now = 0;
    inst->pal->wallclock_date(&yyyymmdd_now, &hhmmss_now);
    uint32_t threshold = yyyymmdd_now;
    uint32_t days = inst->retention_days;
    auto is_leap_year = [](uint32_t y) -> bool {
        return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    };
    while (days > 0 && threshold > 0) {
        uint32_t y = threshold / 10000;
        uint32_t m = (threshold / 100) % 100;
        uint32_t d = threshold % 100;
        if (d > 1) {
            --d;
        } else {
            if (m > 1) {
                --m;
                static const uint8_t md[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
                d = (m == 2 && is_leap_year(y)) ? 29 : md[m - 1];
            } else {
                if (y > 1) { --y; m = 12; d = 31; }
                else { threshold = 0; break; }
            }
        }
        threshold = y * 10000 + m * 100 + d;
        --days;
    }
    for (const auto& f : files) {
        uint32_t d;
        if (!parse_date(f, &d)) continue;
        // 严格保护当天与未来时钟漂移文件, 只删除严格小于 threshold 的超期文件
        if (d < threshold && d < yyyymmdd_now) {
            inst->pal->file_unlink(f.c_str());
        }
    }
}

// ---- §D Flusher 主循环 ----
struct FlusherCtx {
    Instance* inst = nullptr;
    std::string current_path;     // 当前活跃文件路径
    uint32_t current_date = 0;
    uint8_t  current_split = 0;
    uint8_t  current_salt[8] = {0};
    int      current_fd = -1;
    uint64_t current_size = 0;
    bool     date_changed = true;   // 启动时强制为 true,触发首次 target 选择
};

void close_current(FlusherCtx* c, Pal* pal) {
    if (c->current_fd >= 0) {
        pal->file_close(c->current_fd);
        c->current_fd = -1;
    }
}

// 决定"这一轮的追加目标文件";失败/跨天时切换。
// 返回 fd;失败 -1。*out_salt 写到调用方缓冲。
int ensure_append_target(FlusherCtx* c, uint32_t today, Instance* inst) {
    if (!c->date_changed && c->current_fd >= 0) {
        if (c->current_size < inst->max_file_size) {
            return c->current_fd;
        }
    }

    close_current(c, inst->pal);
    if (c->current_path.empty()) {
        // 优先用 hint(追加目标缓存)
        std::string hint;
        if (inst->staging) {
            // 从 staging Header reserved[0..12] 读 cache
            const uint8_t* base = inst->staging->raw_base();
            AppendTargetCache tc;
            std::memcpy(&tc, base + 20, sizeof(tc));
            if (tc.date != 0) {
                hint = make_log_path(inst->log_dir, tc.date, tc.split_no);
            }
        }
        if (hint.empty()) {
            // 慢路径:扫所有文件
            hint = scan_dir_for_target(inst);
        }
        if (!hint.empty()) {
            FileScanResult sr = scan_append_target(inst, hint);  // 命中即修复截断
            if (sr.ok && !sr.truncated && sr.last_block_end_offset > 0) {
                // 命中并校验过
                c->current_path = hint;
                size_t base_idx = sr.path.find_last_of("/\\");
                const char* fn = (base_idx == std::string::npos) ? sr.path.c_str() : (sr.path.c_str() + base_idx + 1);
                c->current_date = std::strlen(fn) >= 8
                    ? static_cast<uint32_t>(std::strtoul(fn, nullptr, 10))
                    : today;
                c->current_split = 0;
            }
        }
    }

    // 跨天:轮换到新文件并清理超期日志
    if (c->current_date != today) {
        c->current_path.clear();
        c->current_date = today;
        c->current_split = 0;
        retention_cleanup_impl(inst);
    }

    // 切分序号决策:如果当前 size ≥ max_file_size → split+1 切到新文件。
    if (c->current_size >= inst->max_file_size) {
        if (c->current_split >= 255) {
            // 达上限,停写
            close_current(c, inst->pal);
            return -1;
        }
        c->current_split += 1;
        c->current_path.clear();
        c->current_size = 0;
        close_current(c, inst->pal);
    }

    if (c->current_fd < 0) {
        c->current_fd = open_or_create_append_target(inst, c->current_path,
                                                     c->current_date, c->current_split,
                                                     &c->current_path,
                                                     c->current_salt);
        if (c->current_fd < 0) return -1;
        int64_t sz = inst->pal->file_size(c->current_fd);
        c->current_size = sz < 0 ? 0 : static_cast<uint64_t>(sz);
    }
    c->date_changed = false;
    return c->current_fd;
}

void drain_once(FlusherCtx* c, Instance* inst) {
    uint32_t yyyymmdd = 0, hhmmss = 0;
    inst->pal->wallclock_date(&yyyymmdd, &hhmmss);
    if (yyyymmdd != c->current_date) c->date_changed = true;

    // 1) 锁内取快照并把待刷字节拷贝到 scratch (锁内时间仅微秒级内存拷贝)
    std::vector<uint8_t> scratch(inst->staging->data_size());
    uint32_t f = 0, snap = 0, bytes = 0;
    inst->pal->mutex_lock(inst->mu);
    bytes = inst->staging->snapshot_pending(&f, &snap);
    if (bytes > 0) {
        uint32_t copied = inst->staging->copy_pending(f, snap, scratch.data(),
                                                      static_cast<uint32_t>(scratch.size()));
        if (copied != bytes) {
            inst->pal->mutex_unlock(inst->mu);
            emit_event(inst, LOGX_EV_IO, -1, "copy_pending mismatch");
            return;
        }
    }
    inst->pal->mutex_unlock(inst->mu);

    if (bytes == 0) return;

    // 3) 沿记录边界切块
    std::vector<PendingBlock> blocks;
    split_impl(scratch.data(), bytes, &blocks);

    // 4) 决定 flag:bit0 = encrypt,bit1-2 = compress(经 make_flag 组装)
    uint8_t flag = make_flag(inst->compress_algo, inst->encrypt_enable != 0);

    // 5) 逐块:压缩/加密 → write() → 清零该区间 → 提交 Header
    // 关键顺序(spec §4.2 第 4→5→6 步):先落数据,再清零,最后提交。
    AppendTargetCache atc{};
    atc.date = c->current_date;
    atc.split_no = c->current_split;

    // 当前写指针;清零 [f, f+raw_len) 后需要推进;按块依次 commit。
    uint32_t cur_f = f;

    for (const auto& blk : blocks) {
        if (blk.raw_len == 0) continue;

        // 取 key(可空)
        const uint8_t* key = nullptr;
        if ((flag & kFlagEncrypted) && inst->key_provider) {
            const uint8_t* k = inst->key_provider(inst->kid, inst->key_user);
            if (!k) {
                emit_event(inst, LOGX_EV_DEGRADED, -1, "missing key; downgrading block to plaintext");
                flag &= ~kFlagEncrypted;
            } else {
                key = k;
            }
        }

        // 5.1 决定追加目标文件(跨天/切分)
        int fd = ensure_append_target(c, yyyymmdd, inst);
        atc.date = c->current_date;
        atc.split_no = c->current_split;
        if (fd < 0) {
            emit_event(inst, LOGX_EV_IO, -1, "append target open failed");
            // 不再尝试,清零 cur_f..(cur_f+blk.raw_len) 以维持不变量
            inst->pal->mutex_lock(inst->mu);
            inst->staging->zero_range_locked(cur_f, blk.raw_len);
            uint32_t new_f = (cur_f + blk.raw_len) % inst->staging->data_size();
            uint32_t latest_w = get_u32(inst->staging->raw_base() + 16);
            inst->staging->commit_header_locked(get_u32(inst->staging->raw_base() + 8),
                                                new_f, latest_w, &atc);
            inst->pal->mutex_unlock(inst->mu);
            cur_f = new_f;
            continue;
        }

        // 5.2 编码 + 写盘;seqno 取自当前 staging Header
        uint32_t cur_seqno = get_u32(inst->staging->raw_base() + 8);
        BlockWriteResult wr = write_and_record(inst, fd,
                                               scratch.data() + blk.raw_offset,
                                               blk.raw_len,
                                               cur_seqno + 1,
                                               flag,
                                               c->current_salt,
                                               key);
        if (!wr.ok) {
            emit_event(inst, LOGX_EV_IO, -1, "write block failed");
            inst->pal->mutex_lock(inst->mu);
            inst->staging->zero_range_locked(cur_f, blk.raw_len);
            uint32_t new_f = (cur_f + blk.raw_len) % inst->staging->data_size();
            uint32_t latest_w = get_u32(inst->staging->raw_base() + 16);
            inst->staging->commit_header_locked(cur_seqno, new_f, latest_w, &atc);
            inst->pal->mutex_unlock(inst->mu);
            cur_f = new_f;
            continue;
        }

        // 5.3 清零 [cur_f, cur_f+raw_len) 与 5.4 提交 Header (保持锁内最新 write_pos)
        inst->pal->mutex_lock(inst->mu);
        inst->staging->zero_range_locked(cur_f, blk.raw_len);

        uint32_t new_seqno = cur_seqno + 1;
        atc.last_block_offset = static_cast<uint32_t>(wr.start_offset);
        atc.last_block_end_offset = static_cast<uint32_t>(wr.end_offset);
        uint32_t new_f = (cur_f + blk.raw_len) % inst->staging->data_size();
        uint32_t latest_w = get_u32(inst->staging->raw_base() + 16);
        inst->staging->commit_header_locked(new_seqno, new_f, latest_w, &atc);
        inst->pal->mutex_unlock(inst->mu);

        c->current_size = wr.end_offset;
        cur_f = new_f;
    }
}

void flusher_loop(Instance* inst) {
    FlusherCtx c{};
    inst->flusher_running.store(true);
    retention_cleanup_impl(inst);
    while (!inst->flusher_exit.load()) {
        drain_once(&c, inst);
        if (inst->flush_event) {
            if (inst->staging && inst->staging->is_degraded()) {
                // 堆内存降级模式 (spec §2 / §10): 启用 5s (5000ms) 定时兜底落盘
                inst->pal->ev_wait_timeout(inst->flush_event, 5000);
            } else {
                inst->pal->ev_wait(inst->flush_event);
            }
        }
    }
    drain_once(&c, inst);
    close_current(&c, inst->pal);
}

// 在 log_dir 下扫描所有日志文件,选"日期最大且尾块 seqno 最大"者作为追加目标。
// 定义在 anonymous 外(header 声明 + MSVC merge 限制);recovery 对账也用。
std::string scan_dir_for_target(Instance* inst) {
    auto files = inst->pal->file_list(inst->log_dir.c_str());
    std::sort(files.begin(), files.end());
    std::string best;
    uint32_t best_date = 0;
    uint32_t best_seqno = 0;
    for (const auto& f : files) {
        size_t pos = f.find_last_of("/\\");
        std::string name = (pos == std::string::npos) ? f : f.substr(pos + 1);
        if (name.size() < 12) continue;
        if (name.compare(name.size() - 4, 4, ".log") != 0 &&
            name.find(".log.") == std::string::npos) continue;
        char* endp = nullptr;
        unsigned long d = std::strtoul(name.c_str(), &endp, 10);
        if (endp == name.c_str() || d == 0) continue;
        uint32_t date = static_cast<uint32_t>(d);
        FileScanResult r = scan_one(inst, f, /*repair=*/false);  // 只评估,不修复
        if (!r.ok) continue;
        if (date > best_date || (date == best_date && r.seqno > best_seqno)) {
            best = f; best_date = date; best_seqno = r.seqno;
        }
    }
    return best;
}

// ---- 对外实现 ----

uint32_t split_into_blocks(const uint8_t* raw, uint32_t total,
                           std::vector<PendingBlock>* out) {
    return split_impl(raw, total, out);
}

bool build_records(Instance* inst, int level, const char* tag,
                   const uint8_t* payload, uint32_t payload_len,
                   uint8_t* out_buf, uint32_t* out_total_bytes) {
    if (!inst) return false;
    if (level < 0 || level > 5) return false;
    uint64_t ts_ms = inst->pal->wallclock_ms();
    uint32_t pid_v = inst->pal->pid();
    uint64_t tid_v = inst->pal->tid();
    if (!format_record(static_cast<uint8_t>(level), tag, payload, payload_len,
                       ts_ms, pid_v, tid_v,
                       out_buf, kMaxRecordTotal, out_total_bytes)) {
        return false;
    }
    if (*out_total_bytes > inst->max_log_length) {
        inst->drop_too_large.fetch_add(1);
        return false;
    }
    return true;
}

bool write_block_to_file(Instance*, const uint8_t*, const PendingBlock&,
                         uint32_t, const uint8_t[8], uint8_t) {
    // 已统一到 write_and_record(internal);保留接口占位。
    return false;
}

bool append_block(Instance* inst,
                  const uint8_t* file_salt, uint16_t kid,
                  const uint8_t* raw_block, uint32_t raw_len,
                  uint32_t seqno, uint8_t flag,
                  uint64_t* out_block_start_offset,
                  uint64_t* out_block_end_offset) {
    if (!inst || !inst->log_dir.size()) return false;
    // 单文件模式:按 hint 路径写;若文件不存在则创建并写 32B 文件头。
    // 测试 / 上层 (导出离线回放) 用此接口绕过 Flusher 主循环。
    (void)kid;
    uint32_t yyyymmdd = 0, hhmmss = 0;
    inst->pal->wallclock_date(&yyyymmdd, &hhmmss);
    std::string path = make_log_path(inst->log_dir, yyyymmdd, 0);
    int fd = inst->pal->file_open(path.c_str(), true, true);
    if (fd < 0) return false;
    // 若是新建文件,先写 32B 头
    int64_t sz = inst->pal->file_size(fd);
    if (sz < 0) { inst->pal->file_close(fd); return false; }
    uint8_t salt[8];
    if (file_salt) std::memcpy(salt, file_salt, 8);
    if (sz == 0) {
        if (!write_file_header(inst, fd, yyyymmdd, inst->kid, salt)) {
            inst->pal->file_close(fd); return false;
        }
    } else {
        if (file_salt) {
            // 用 caller 提供的 salt 校验;不一致则报错(防止调用方用错)
            uint8_t on_disk[8];
            if (inst->pal->file_pread(fd, on_disk, 8, 12) != 8) { inst->pal->file_close(fd); return false; }
            if (std::memcmp(on_disk, salt, 8) != 0) { inst->pal->file_close(fd); return false; }
        }
    }
    BlockWriteResult wr = write_and_record(inst, fd, raw_block, raw_len,
                                           seqno, flag, salt, nullptr);
    inst->pal->file_close(fd);
    if (!wr.ok) return false;
    if (out_block_start_offset) *out_block_start_offset = wr.start_offset;
    if (out_block_end_offset)   *out_block_end_offset   = wr.end_offset;
    return true;
}

std::string current_log_path(Instance* inst, uint32_t yyyymmdd, uint8_t split_no) {
    return make_log_path(inst->log_dir, yyyymmdd, split_no);
}

void retention_cleanup(Instance* inst) { retention_cleanup_impl(inst); }

FileScanResult scan_append_target(Instance* inst, const std::string& hint_path) {
    return scan_one(inst, hint_path, /*repair=*/true);
}

// k 块回溯:从 seqno ∈ (upto_seqno, ...] 的第一块起累加 raw_len 到文件尾。
// 与 scan_one 相同的遍历纪律(块头 magic / 长度自洽),但只读不修——
// 跨过区间是否清零由 recovery 决定,这里不产生截断副作用。
bool sum_raw_len_since(Instance* inst, const std::string& target_path,
                       uint32_t upto_seqno, uint64_t* out_bytes,
                       uint64_t* out_last_block_offset,
                       uint64_t* out_last_block_end_offset) {
    *out_bytes = 0;
    *out_last_block_offset = 0;
    *out_last_block_end_offset = 0;
    int fd = inst->pal->file_open(target_path.c_str(), false, false);
    if (fd < 0) return false;
    int64_t sz = inst->pal->file_size(fd);
    if (sz < kFileHeaderSize) {
        inst->pal->file_close(fd);
        return false;
    }
    uint64_t off = kFileHeaderSize;
    while (off + 34 <= static_cast<uint64_t>(sz)) {
        uint8_t bhdr[34];
        if (inst->pal->file_pread(fd, bhdr, 34, off) != 34) break;
        if (get_u32(bhdr) != kMagicBlock) break;   // 撕裂半头:计入后续对账失败,宁丢不脏
        uint32_t seqno = get_u32(bhdr + 6);
        uint32_t raw_len = get_u32(bhdr + 10);
        uint32_t payload_len = get_u32(bhdr + 14);
        uint8_t flag = bhdr[5];
        // raw_len 范围防御:必须在 (0, kSingleRecordMaxRaw] 范围内,超限视作块头损坏,终止对账防错误推进水位
        if (raw_len == 0 || raw_len > kSingleRecordMaxRaw) break;
        uint64_t block_size = 34 + payload_len + (flag_encrypted(flag) ? 16 : 0);
        if (off + block_size > static_cast<uint64_t>(sz)) break;  // 半块尾巴
        if (seqno > upto_seqno) {
            *out_bytes += raw_len;      // raw_len(spec §3.2 对账关键字段)
        }
        *out_last_block_offset = off;
        *out_last_block_end_offset = off + block_size;
        off += block_size;
    }
    inst->pal->file_close(fd);
    return true;
}

void* flusher_main(void* arg) {
    Instance* inst = static_cast<Instance*>(arg);
    flusher_loop(inst);
    return nullptr;
}

void flush_blocking(Instance* inst) {
    if (!inst) return;
    // 唤醒 + 等 Flusher 追上:写一个 dummy(其实不需要)— 只要把 event 触发并等
    // 到 staging 空。
    if (inst->flush_event) inst->pal->ev_signal(inst->flush_event);
    // 简易同步:自旋到 staging 空。最坏几十毫秒。
    uint32_t f, snap;
    while (true) {
        uint32_t bytes = inst->staging->snapshot_pending(&f, &snap);
        if (bytes == 0) break;
        // 主动再 wake 一次
        inst->pal->ev_signal(inst->flush_event);
        inst->pal->sleep_ms(5);
    }
    // 退后台落盘完成时, 执行保留期超期清理
    retention_cleanup_impl(inst);
}

}  // namespace logx