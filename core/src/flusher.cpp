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
#include "logx_staging.h"  // kBlockMaxRaw / kBlockMaxNormal(块上限单源,R9 直含;先例 codec.cpp)

namespace logx {

namespace {

constexpr uint32_t kNormalBlockMax = kBlockMaxNormal;  // 常规块 raw 上限(16KB,单源 staging.h,R9 去重)
constexpr uint32_t kSingleRecordMaxRaw = kBlockMaxRaw;  // 与块上限同源(20KB+6B,R8 去重)

// put_u32_le 见 logx_internal.h(测试可见)

// 在 raw 流里看下一条记录的字节数(头+payload);不足返回 0。
uint32_t peek_record_len(const uint8_t* p, uint32_t off, uint32_t total) {
    if (total - off < kHeaderLen) return 0;
    uint16_t magic = uint16_t(p[off + 2]) | (uint16_t(p[off + 3]) << 8);
    if (magic != kMagicEntry && magic != kMagicPad) return 0;
    uint32_t len = uint32_t(p[off]) | (uint32_t(p[off + 1]) << 8);
    uint32_t sz = kHeaderLen + len;
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
        if (total - pos < kHeaderLen) {
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
            if (total - cur < kHeaderLen) { cur = total; break; }
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
    // encode_block 已把 [cipher | tag] 合并到 enc.payload 尾部 16B (enc_ok 时),此处无需额外追加 tag
    // 5) 写盘
    long n = inst->pal->file_write(fd, frame.data(), frame.size());
    if (n != static_cast<long>(frame.size())) {
        inst->block_write_errors.fetch_add(1);
        emit_event(inst, LOGX_EV_IO, -1, "block short write");
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
        // 短写或写入失败后，若物理文件已增长（产生了撕裂尾部），回滚截断至写前偏移
        int64_t cur_sz = inst->pal->file_size(fd);
        if (cur_sz > sz) {
            if (inst->pal->file_truncate(fd, static_cast<uint64_t>(sz)) != 0) {
                emit_event(inst, LOGX_EV_IO, -1, "truncate torn tail failed");
            }
        }
        return r;
    }
    r.end_offset = r.start_offset + static_cast<uint64_t>(n);
    r.ok = true;
    return r;
}

// spec §11 断电检查点: staging mmap 同步 + 日志文件 fdatasync。
// 返回 0 表示全部成功; -1 表示至少一项失败(失败不阻塞, 仅打点发事件)。
// out_sync_time_ms: 可选输出完成同步时的系统时间戳 (毫秒, wallclock ms)。
int durability_sync(Instance* inst, uint64_t* out_sync_time_ms) {
    if (!inst) return -1;
    int ret = 0;
    // 1. staging mmap 同步
    if (inst->staging) {
        if (inst->staging->mmap_sync_now() != 0) {
            emit_event(inst, LOGX_EV_IO, -1, "durability sync: staging msync failed");
            ret = -1;
        }
    }
    // 2. 活跃日志文件 fdatasync
    int fd = inst->active_log_fd.load();
    if (fd >= 0) {
        if (inst->pal->file_fsync(fd) != 0) {
            emit_event(inst, LOGX_EV_IO, -1, "durability sync: file fsync failed");
            ret = -1;
        }
    }
    if (out_sync_time_ms) {
        *out_sync_time_ms = inst->pal->wallclock_ms();
    }
    return ret;
}

// ---- §C 文件路径 / 清理 ----
std::string make_log_path(const std::string& dir, uint32_t yyyymmdd, uint16_t split) {
    char buf[128];
    if (split == 0) {
        std::snprintf(buf, sizeof(buf), "%s/%08u.log", dir.c_str(), yyyymmdd);
    } else {
        std::snprintf(buf, sizeof(buf), "%s/%08u.log.%u", dir.c_str(), yyyymmdd, static_cast<unsigned>(split));
    }
    return std::string(buf);
}

bool parse_log_filename(const std::string& path_or_name, uint32_t* out_date, uint16_t* out_split) {
    if (path_or_name.empty()) return false;
    size_t base = path_or_name.find_last_of("/\\");
    std::string name = (base == std::string::npos) ? path_or_name : path_or_name.substr(base + 1);
    if (name.size() < 12) return false;
    if (name.find("staging") != std::string::npos) return false;

    for (int i = 0; i < 8; ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    char* endp = nullptr;
    unsigned long d = std::strtoul(name.c_str(), &endp, 10);
    if (endp != name.c_str() + 8 || d == 0 || d > 99991231u) return false;
    if (!valid_yyyymmdd(static_cast<uint32_t>(d))) return false;  // R-03 日历合法性

    if (name.compare(8, 4, ".log") != 0) return false;

    uint16_t split = 0;
    if (name.size() == 12) {
        split = 0;
    } else if (name.size() > 13 && name[12] == '.') {
        if (name[13] < '0' || name[13] > '9') return false;
        char* split_end = nullptr;
        unsigned long s = std::strtoul(name.c_str() + 13, &split_end, 10);
        if (split_end == name.c_str() + 13 || *split_end != '\0') return false;
        if (s > 65535u) return false;
        split = static_cast<uint16_t>(s);
    } else {
        return false;
    }

    if (out_date) *out_date = static_cast<uint32_t>(d);
    if (out_split) *out_split = split;
    return true;
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
    long n = inst->pal->file_write(fd, hdr, 32);
    if (n != 32) {
        inst->pal->file_truncate(fd, 0);
        return false;
    }
    return true;
}

constexpr uint32_t kBlockMaxRawPlausible = 16 * 1024 * 1024;  // 16MB 块上限防御

bool block_header_plausible(const uint8_t bhdr[34], uint64_t file_remaining_bytes,
                            uint32_t prev_seqno, bool has_prev_seqno,
                            uint32_t* out_seqno, uint32_t* out_raw_len,
                            uint32_t* out_payload_len, uint64_t* out_block_size) {
    if (!bhdr) return false;

    // 1. Magic 校验: 'LGXB'
    if (get_u32(bhdr) != kMagicBlock) return false;

    // 2. 版本校验: 仅支持 v1
    if (bhdr[4] != 1) return false;

    // 3. flag 保留位校验: 仅允许 bit0 (加密), bit1..2 (压缩算法)
    uint8_t flag = bhdr[5];
    if ((flag & ~0x07) != 0) return false;

    // 4. 压缩算法支持性与编译宏校验
    uint8_t algo = flag_algo(flag);
#if LOGX_WITH_ZSTD
    if (algo != kAlgoNone && algo != kAlgoZstd) return false;
#else
    if (algo != kAlgoNone) return false;
#endif

    // 5. seqno 严格单调递增
    uint32_t seqno = get_u32(bhdr + 6);
    if (seqno == 0) return false;
    if (has_prev_seqno && seqno <= prev_seqno) return false;

    // 6. raw_len 范围防御: [kHeaderLen, kBlockMaxRawPlausible]
    uint32_t raw_len = get_u32(bhdr + 10);
    if (raw_len < kHeaderLen || raw_len > kBlockMaxRawPlausible) return false;

    // 7. payload_len 范围防御
    uint32_t payload_len = get_u32(bhdr + 14);
    if (payload_len == 0 || payload_len > kBlockMaxRawPlausible) return false;

    // 8. 协议自洽性: 未压缩时 payload_len 必须严格等于 raw_len (无论是否加密)
    if (algo == kAlgoNone && payload_len != raw_len) return false;

    // 9. block_size 计算与文件剩余容量校验
    uint64_t block_size = 34ULL + payload_len + (flag_encrypted(flag) ? 16ULL : 0ULL);
    if (block_size > file_remaining_bytes) return false;

    if (out_seqno) *out_seqno = seqno;
    if (out_raw_len) *out_raw_len = raw_len;
    if (out_payload_len) *out_payload_len = payload_len;
    if (out_block_size) *out_block_size = block_size;
    return true;
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
    uint32_t prev_seqno = 0;
    bool has_prev_seqno = false;
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
        uint32_t seqno = 0, raw_len = 0, payload_len = 0;
        uint64_t block_size = 0;
        if (!block_header_plausible(bhdr, static_cast<uint64_t>(sz) - off,
                                    prev_seqno, has_prev_seqno,
                                    &seqno, &raw_len, &payload_len, &block_size)) {
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
        if (block_valid && flag_encrypted(bhdr[5])) {
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
        r.seqno = seqno;
        r.last_block_offset = off;
        r.last_block_end_offset = off + block_size;
        prev_seqno = seqno;
        has_prev_seqno = true;
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
                                 uint32_t date, uint16_t split_no,
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
        if (!inst->pal->random_bytes(salt, 8)) {
            emit_event(inst, LOGX_EV_IO, -1,
                       "secure random unavailable; refusing to create log file with low-entropy salt");
            inst->pal->file_close(fd);
            return -1;
        }
        if (!write_file_header(inst, fd, date, inst->kid, salt)) {
            inst->pal->file_close(fd); return -1;
        }
        std::memcpy(out_salt, salt, 8);
        // 新建文件后对文件和父目录 fsync 一次 (spec §10 / T4.0-12)
        if (inst->pal->file_fsync(fd) != 0) {
            emit_event(inst, LOGX_EV_IO, -1, "new file fsync failed");
        }
        if (inst->pal->file_dir_fsync(inst->log_dir.c_str()) != 0) {
            emit_event(inst, LOGX_EV_IO, -1, "new file dir fsync failed");
        }
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
    // R-03: 统一走 parse_log_filename(含日历合法性校验),不再内联 strtoul。
    auto parse_date = [](const std::string& path, uint32_t* out_date) -> bool {
        return parse_log_filename(path, out_date, nullptr);
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
    uint16_t current_split = 0;
    uint8_t  current_salt[8] = {0};
    int      current_fd = -1;
    uint64_t current_size = 0;
    bool     date_changed = true;   // 启动时强制为 true,触发首次 target 选择
    std::vector<uint8_t> scratch;   // P3-2: 常驻复用缓冲,避免每 75ms 轮询分配 128KB
};

void close_current(FlusherCtx* c, Pal* pal) {
    if (c->current_fd >= 0) {
        if (pal->file_fsync(c->current_fd) != 0) {
            if (c->inst) emit_event(c->inst, LOGX_EV_IO, -1, "close file fsync failed");
        }
        pal->file_close(c->current_fd);
        c->current_fd = -1;
        if (c->inst) {
            c->inst->active_log_fd.store(-1);
        }
    }
}

// 决定"这一轮的追加目标文件";失败/跨天时切换。
// 返回 fd;失败 -1。*out_salt 写到调用方缓冲。
int ensure_append_target(FlusherCtx* c, uint32_t today, Instance* inst) {
    if (!c->date_changed && c->current_fd >= 0) {
        if (!should_rotate_file_size(c->current_size, inst->max_file_size)) {
            return c->current_fd;
        }
    }

    close_current(c, inst->pal);
    if (c->current_path.empty()) {
        // 优先用 hint(追加目标缓存)
        std::string hint;
        if (inst->staging) {
            // 从 staging Header reserved[0..15] 读 cache
            const uint8_t* base = inst->staging->raw_base();
            AppendTargetCache tc;
            std::memcpy(&tc, base + 20, sizeof(tc));
            if (tc.date != 0) {
                hint = make_log_path(inst->log_dir, tc.date, atc_split(tc));
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
                if (!parse_log_filename(sr.path, &c->current_date, &c->current_split)) {
                    c->current_date = today;
                    c->current_split = 0;
                }
                c->current_size = sr.last_block_end_offset;
            }
        }
    }

    // 跨天:轮换到新文件并清理超期日志
    if (c->current_date != today) {
        c->current_path.clear();
        c->current_date = today;
        c->current_split = 0;
        // R16 F-5: 昨日尾文件的 size 不跨天沿用——若保留, 跨天后首个
        // ensure_append_target 会误判已达切分阈值, 当日首文件从 .log.1 起写。
        c->current_size = 0;
        retention_cleanup_impl(inst);
    }

    // 切分序号决策:如果当前 size 满足切分条件 (≥ max_file_size 或达到 4GiB 物理上限) → split+1 切到新文件。
    // 增加循环探测:若 make_log_path 目标已存在则持续 +1, 直至空闲分卷或达 65535 上限 (F-07 / WARN-4)
    if (should_rotate_file_size(c->current_size, inst->max_file_size)) {
        do {
            if (c->current_split >= 65535u) {
                // 达上限 65535, 停写并明确报警 (WARN-4 / F-07)
                close_current(c, inst->pal);
                emit_event(inst, LOGX_EV_IO, -1,
                           "daily split limit (65535) exhausted; writes suspended until date rollover");
                return kAppendTargetSplitExhausted;
            }
            c->current_split += 1;
        } while (inst->pal->file_size_path(
            make_log_path(inst->log_dir, c->current_date, c->current_split).c_str()) >= 0);
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
        inst->active_log_fd.store(c->current_fd);
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

    // 1) 锁内取快照判定 pending 字节数; 若无日志待刷, 零分配立即返回 (P3-2)
    uint32_t f = 0, snap = 0, bytes = 0;
    inst->pal->mutex_lock(inst->mu);
    bytes = inst->staging->snapshot_pending(&f, &snap);
    if (bytes == 0) {
        inst->pal->mutex_unlock(inst->mu);
        return;
    }

    // 复用 FlusherCtx 常驻 scratch, 消除每 75ms 一次的 malloc(128KB)+memset+free (P3-2)
    uint32_t staging_cap = inst->staging->data_size();
    if (c->scratch.size() < staging_cap) {
        c->scratch.resize(staging_cap);
    }

    uint32_t copied = inst->staging->copy_pending(f, snap, c->scratch.data(),
                                                  static_cast<uint32_t>(c->scratch.size()));
    if (copied != bytes) {
        inst->pal->mutex_unlock(inst->mu);
        emit_event(inst, LOGX_EV_IO, -1, "copy_pending mismatch");
        return;
    }
    inst->pal->mutex_unlock(inst->mu);

    // 3) 沿记录边界切块
    std::vector<PendingBlock> blocks;
    split_impl(c->scratch.data(), bytes, &blocks);

    // 4) 决定 flag:bit0 = encrypt,bit1-2 = compress(经 make_flag 组装)
    uint8_t flag = make_flag(inst->compress_algo, inst->encrypt_enable != 0);

    // 5) 逐块:压缩/加密 → write() → 清零该区间 → 提交 Header
    // 关键顺序(spec §4.2 第 4→5→6 步):先落数据,再清零,最后提交。
    AppendTargetCache atc{};
    atc.date = c->current_date;
    atc_set_split(&atc, c->current_split);

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
        atc_set_split(&atc, c->current_split);
        if (fd < 0) {
            if (fd != kAppendTargetSplitExhausted) {
                emit_event(inst, LOGX_EV_IO, -1, "append target open failed");
            }
            // 不再尝试,清零 cur_f..(cur_f+blk.raw_len) 以维持不变量
            inst->pal->mutex_lock(inst->mu);
            inst->staging->zero_range_locked(cur_f, blk.raw_len);
            uint32_t new_f = (cur_f + blk.raw_len) % inst->staging->data_size();
            uint32_t latest_w = get_u32(inst->staging->raw_base() + 16);
            inst->staging->commit_header_locked(get_u32(inst->staging->raw_base() + 8),
                                                new_f, latest_w, &atc);
            inst->pal->mutex_unlock(inst->mu);
            inst->flushed_bytes_total.fetch_add(blk.raw_len, std::memory_order_relaxed);
            cur_f = new_f;
            continue;
        }

        // 5.2 编码 + 写盘;seqno 取自当前 staging Header
        uint32_t cur_seqno = get_u32(inst->staging->raw_base() + 8);
        BlockWriteResult wr = write_and_record(inst, fd,
                                               c->scratch.data() + blk.raw_offset,
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
            inst->flushed_bytes_total.fetch_add(blk.raw_len, std::memory_order_relaxed);
            cur_f = new_f;
            continue;
        }

        // 5.3 清零 [cur_f, cur_f+raw_len) 与 5.4 提交 Header (保持锁内最新 write_pos)
        inst->pal->mutex_lock(inst->mu);
        inst->staging->zero_range_locked(cur_f, blk.raw_len);

        uint32_t new_seqno = cur_seqno + 1;
        if (!safe_assign_atc_offsets(wr.start_offset, wr.end_offset,
                                     &atc.last_block_offset, &atc.last_block_end_offset)) {
            emit_event(inst, LOGX_EV_IO, -1, "block offset exceeds atc capacity");
        }
        uint32_t new_f = (cur_f + blk.raw_len) % inst->staging->data_size();
        uint32_t latest_w = get_u32(inst->staging->raw_base() + 16);
        inst->staging->commit_header_locked(new_seqno, new_f, latest_w, &atc);
        inst->pal->mutex_unlock(inst->mu);

        c->current_size = wr.end_offset;
        inst->flushed_bytes_total.fetch_add(blk.raw_len, std::memory_order_relaxed);
        cur_f = new_f;
    }
}

void flusher_loop(Instance* inst) {
    FlusherCtx c{};
    c.inst = inst;
    inst->flusher_running.store(true);
    retention_cleanup_impl(inst);
    uint64_t last_sync_ms = inst->pal->wallclock_ms();
    while (!inst->flusher_exit.load()) {
        drain_once(&c, inst);

        // 运行期 30s 定时持久化检查点 (spec §11 / T4.0-6)
        uint64_t now_ms = inst->pal->wallclock_ms();
        if (now_ms >= last_sync_ms + 30000) {
            durability_sync(inst, nullptr);
            last_sync_ms = now_ms;
        }

        if (inst->flush_event) {
            uint32_t timeout_ms = (inst->staging && inst->staging->is_degraded())
                                      ? 5000
                                      : kFlushDebounceMs;
            inst->pal->ev_wait_timeout(inst->flush_event, timeout_ms);
        }
    }
    drain_once(&c, inst);
    durability_sync(inst, nullptr);
    close_current(&c, inst->pal);
    inst->flusher_running.store(false, std::memory_order_release);
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
        // R-03: 统一走 parse_log_filename(含日历合法性校验)
        uint32_t date = 0;
        uint16_t split = 0;
        if (!parse_log_filename(f, &date, &split)) continue;
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

int build_records(Instance* inst, int level, const char* tag,
                  const uint8_t* payload, uint32_t payload_len,
                  uint8_t* out_buf, uint32_t* out_total_bytes,
                  bool* out_truncated, uint64_t* out_dropped) {
    if (!inst) return kBuildRecordsInvalidInput;
    if (level < 0 || level > 5) return kBuildRecordsInvalidInput;
    uint64_t ts_ms = inst->pal->wallclock_ms();
    uint32_t pid_v = inst->pal->pid();
    uint64_t tid_v = inst->pal->tid();
    // B-1B 方案B: 超长(max_log_length 或 60KB 用户载荷硬上限)不再由调用方丢弃,
    // 而是截断保前缀后照常落环;kCannotFit(病态小上限)交回调用方走丢弃兜底。
    logx::FormatBoundedStatus st = logx::format_record_bounded(
        static_cast<uint8_t>(level), tag, payload, payload_len,
        ts_ms, pid_v, tid_v,
        out_buf, kMaxRecordTotal, out_total_bytes,
        inst->max_log_length, out_truncated, out_dropped);
    if (st == logx::FormatBoundedStatus::kInvalidInput) {
        return kBuildRecordsInvalidInput;  // E-02: 输入非法(level 越界 / tag 非法),非超长
    }
    if (st == logx::FormatBoundedStatus::kCannotFit) {
        return kBuildRecordsCannotFit;
    }
    return static_cast<int>(*out_total_bytes);
}


bool append_block(Instance* inst,
                  const uint8_t* file_salt, uint16_t kid,
                  const uint8_t* raw_block, uint32_t raw_len,
                  uint32_t seqno, uint8_t flag,
                  uint64_t* out_block_start_offset,
                  uint64_t* out_block_end_offset) {
    if (!inst || !inst->log_dir.size()) return false;
    // 单文件模式:按 hint 路径写;若文件不存在则创建并写 32B 文件头。
    // 仅单元测试用此接口绕过 Flusher 主循环(全仓调用点均传非空 file_salt)。
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
    if (file_salt) {
        std::memcpy(salt, file_salt, 8);
    } else if (sz == 0) {
        if (!inst->pal->random_bytes(salt, 8)) {
            emit_event(inst, LOGX_EV_IO, -1,
                       "secure random unavailable; refusing to create log file with low-entropy salt");
            inst->pal->file_close(fd);
            return false;
        }
    } else {
        // R18 F-1: 已存在文件且调用方未给 salt 时, 必须回读文件头 salt 复用。
        // 旧实现此分支不赋值 salt, 未初始化栈内容会被写进块头 hdr+22
        // (加密路径还会用它派生 GCM nonce), 属未初始化内存落盘。
        if (inst->pal->file_pread(fd, salt, 8, 12) != 8) { inst->pal->file_close(fd); return false; }
    }
    if (sz == 0) {
        if (!write_file_header(inst, fd, yyyymmdd, inst->kid, salt)) {
            inst->pal->file_close(fd); return false;
        }
        if (inst->pal->file_fsync(fd) != 0) {
            emit_event(inst, LOGX_EV_IO, -1, "new file fsync failed");
        }
        if (inst->pal->file_dir_fsync(inst->log_dir.c_str()) != 0) {
            emit_event(inst, LOGX_EV_IO, -1, "new file dir fsync failed");
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
    if (wr.ok) {
        inst->pal->file_fsync(fd);
    }
    inst->pal->file_close(fd);
    if (!wr.ok) return false;
    if (out_block_start_offset) *out_block_start_offset = wr.start_offset;
    if (out_block_end_offset)   *out_block_end_offset   = wr.end_offset;
    return true;
}

std::string current_log_path(Instance* inst, uint32_t yyyymmdd, uint16_t split_no) {
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
    uint32_t staging_capacity = (inst && inst->staging) ? inst->staging->data_size() : 0;
    uint64_t off = kFileHeaderSize;
    uint32_t prev_seqno = 0;
    bool has_prev_seqno = false;
    while (off + 34 <= static_cast<uint64_t>(sz)) {
        uint8_t bhdr[34];
        if (inst->pal->file_pread(fd, bhdr, 34, off) != 34) break;
        uint32_t seqno = 0, raw_len = 0, payload_len = 0;
        uint64_t block_size = 0;
        if (!block_header_plausible(bhdr, static_cast<uint64_t>(sz) - off,
                                    prev_seqno, has_prev_seqno,
                                    &seqno, &raw_len, &payload_len, &block_size)) {
            break;
        }
        // 校验块 CRC32(payload) 与 Tag 可读性 (宁丢不脏, 与 scan_one 保持一致)
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
        if (block_valid && flag_encrypted(bhdr[5])) {
            uint8_t tag[16];
            if (inst->pal->file_pread(fd, tag, 16, off + 34 + payload_len) != 16) {
                block_valid = false;
            }
        }
        if (!block_valid) break;

        if (seqno > upto_seqno) {
            if (staging_capacity > 0 && (*out_bytes + raw_len >= staging_capacity)) {
                // 水位回推上界防御: 累加 raw_len 达到或超过环形缓冲区容量，终止累加防错误推进水位
                break;
            }
            *out_bytes += raw_len;      // raw_len(spec §3.2 对账关键字段)
        }
        *out_last_block_offset = off;
        *out_last_block_end_offset = off + block_size;
        prev_seqno = seqno;
        has_prev_seqno = true;
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

bool flush_blocking(Instance* inst) {
    if (!inst) return true;
    if (inst->mu) inst->pal->mutex_lock(inst->mu);
    uint64_t target = inst->staged_bytes_total.load(std::memory_order_relaxed);
    if (inst->mu) inst->pal->mutex_unlock(inst->mu);

    uint64_t start_ms = inst->pal->wallclock_ms();
    if (inst->flush_event) inst->pal->ev_signal(inst->flush_event);

    while (inst->flushed_bytes_total.load(std::memory_order_relaxed) < target) {
        if (!inst->flusher_running.load(std::memory_order_acquire)) {
            emit_event(inst, LOGX_EV_IO, -1, "flush skipped: flusher not running");
            return false;
        }
        if (inst->pal->wallclock_ms() >= start_ms + kFlushBlockingTimeoutMs) {
            emit_event(inst, LOGX_EV_IO, -1, "flush timeout: flusher stalled; data may be pending");
            return false;
        }
        if (inst->flush_event) inst->pal->ev_signal(inst->flush_event);
        inst->pal->sleep_ms(5);
    }

    durability_sync(inst, nullptr);
    // P3-1: 方案 B: flush_blocking 严格专注落盘, 移除每次 flush 都扫盘的 retention 清理;
    // 过期清理已由实例启动阶段与后台跨天 Rollover 闭环保障。
    return true;
}

}  // namespace logx