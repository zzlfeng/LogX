// staging.cpp - 环形 mmap staging 实现。
//
// 严格遵循 spec §3.1 / §5 的不变量:
//   1. 条目原子性(分片不被打断)
//   2. 写指针并发安全(全局 mutex)
//   3. 生产者与 Flusher 互不阻塞(清零在锁外)
//   4. 文件单写者
//   5. 任意时刻除 [flushed_len, write_pos) 外全环为零
//   6. 任意时刻被 kill,撕裂条目最多一条

#include "logx_staging.h"
#include "logx_format.h"  // kMagicEntry / kMagicPad(记录头 magic)

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>

namespace logx {

namespace {

// 标准 CRC32(IEEE 802.3),多项式 0xedb88320。
// 编译期生成查表(S-01):无运行时初始化即无数据竞争,热路径亦免去初始化检查分支。
struct Crc32Table {
    uint32_t v[256];
    constexpr Crc32Table() : v{} {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) {
                c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            }
            v[i] = c;
        }
    }
};

constexpr Crc32Table kCrc32Table{};

// 编译期静态断言: 校验关键特征项, 防止多项式或生成算法意外变更
static_assert(kCrc32Table.v[0] == 0x00000000u, "CRC32 table[0] mismatch");
static_assert(kCrc32Table.v[1] == 0x77073096u, "CRC32 table[1] mismatch");
static_assert(kCrc32Table.v[255] == 0x2d02ef8du, "CRC32 table[255] mismatch");

}  // namespace

uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc = (crc >> 8) ^ kCrc32Table.v[(crc ^ p[i]) & 0xff];
    }
    return ~crc;
}

uint32_t crc32_finalize(uint32_t crc) { return crc; }

Staging::Staging(Pal* pal, const std::string& path, uint32_t data_size)
    : pal_(pal), path_(path), data_size_(data_size),
      total_size_(data_size + kHeaderTotal), fd_(-1), base_(nullptr), data_base_(nullptr) {}

Staging::~Staging() { close(); }

int Staging::mmap_sync_now() {
    if (!base_) return 0;
    if (!is_heap_degraded_) {
        int r1 = pal_->mmap_sync(mm_, /*synchronous=*/true);
        int r2 = 0;
        if (fd_ >= 0) {
            r2 = pal_->file_fsync(fd_);
        }
        return (r1 == 0 && r2 == 0) ? 0 : -1;
    } else {
        if (fd_ >= 0) {
            pal_->file_pwrite(fd_, heap_buf_.data(), heap_buf_.size(), 0);
            return pal_->file_fsync(fd_);
        }
        return 0;
    }
}

void Staging::close() {
    if (base_) {
        if (!is_heap_degraded_) {
            pal_->mmap_sync(mm_, /*synchronous=*/true);  // 强同步 MS_SYNC / FlushViewOfFile
            pal_->mmap_close(mm_);
        } else {
            if (fd_ >= 0) {
                pal_->file_pwrite(fd_, heap_buf_.data(), heap_buf_.size(), 0);
            }
            heap_buf_.clear();
            heap_buf_.shrink_to_fit();
        }
        base_ = nullptr;
        data_base_ = nullptr;
    }
    if (fd_ >= 0) {
        pal_->file_fsync(fd_);
        pal_->file_close(fd_);
        fd_ = -1;
    }
    is_heap_degraded_ = false;
}

uint32_t Staging::compute_header_crc(const uint8_t* p) const {
    return crc32_update(0, p, kHeaderASize - 4);
}

void Staging::write_header_raw(uint8_t* p, const Header& h) const {
    put_u32(p + 0,  h.magic);
    put_u16(p + 4,  h.version);
    put_u16(p + 6,  h.flags);
    put_u32(p + 8,  h.seqno);
    put_u32(p + 12, h.flushed_len);
    put_u32(p + 16, h.write_pos);
    std::memcpy(p + 20, h.reserved, 40);
    put_u32(p + 60, h.header_crc);
}

void Staging::read_header(const uint8_t* p, Header* out) const {
    out->magic        = get_u32(p + 0);
    out->version      = get_u16(p + 4);
    out->flags        = get_u16(p + 6);
    out->seqno        = get_u32(p + 8);
    out->flushed_len  = get_u32(p + 12);
    out->write_pos    = get_u32(p + 16);
    std::memcpy(out->reserved, p + 20, 40);
    out->header_crc   = get_u32(p + 60);
}

bool Staging::pick_header(Header* out, AppendTargetCache* out_target) const {
    Header ha, hb;
    read_header(base_, &ha);
    read_header(base_ + kHeaderASize, &hb);
    uint32_t ca = compute_header_crc(base_);
    uint32_t cb = compute_header_crc(base_ + kHeaderASize);

    bool a_ok = (ha.magic == kMagicStaging) && (ca == ha.header_crc);
    bool b_ok = (hb.magic == kMagicStaging) && (cb == hb.header_crc);

    Header* chosen = nullptr;
    if (a_ok && b_ok) {
        // 取 seqno 较新者;相等取 A(因为 A 先写,更可能在较后 commit)
        if (ha.seqno >= hb.seqno) chosen = &ha; else chosen = &hb;
    } else if (a_ok) {
        chosen = &ha;
    } else if (b_ok) {
        chosen = &hb;
    } else {
        return false;
    }
    *out = *chosen;
    if (out_target) {
        std::memcpy(out_target, chosen->reserved, sizeof(AppendTargetCache));
    }
    return true;
}

bool Staging::open(RecoveryState* state, bool* recovered) {
    // 1. open(O_RDWR, create_if_missing)
    fd_ = pal_->file_open(path_.c_str(), true, false);
    if (fd_ < 0) {
        // 文件打开失败: 降级分配堆内存 (spec §2 / §10)
        try {
            heap_buf_.assign(total_size_, 0);
        } catch (...) {
            return false;
        }
        base_ = heap_buf_.data();
        data_base_ = base_ + kHeaderTotal;
        is_heap_degraded_ = true;
        std::memset(state, 0, sizeof(*state));
        state->rebuilt = false;
        *recovered = false;
        return true;
    }

    // 2. ftruncate 到 total_size_;保证文件大小固定,首字节为 0。
    if (pal_->file_truncate(fd_, total_size_) != 0) {
        pal_->file_close(fd_);
        fd_ = -1;
        try {
            heap_buf_.assign(total_size_, 0);
        } catch (...) {
            return false;
        }
        base_ = heap_buf_.data();
        data_base_ = base_ + kHeaderTotal;
        is_heap_degraded_ = true;
        std::memset(state, 0, sizeof(*state));
        state->rebuilt = false;
        *recovered = false;
        return true;
    }

    // 3. mmap(必须是 MAP_SHARED)。
    mm_ = pal_->mmap_open(fd_, total_size_, true);
    if (!mm_.base) {
        // mmap 失败: 触发堆内存降级 (spec §2 / §10)
        pal_->file_close(fd_);
        fd_ = -1;
        try {
            heap_buf_.assign(total_size_, 0);
        } catch (...) {
            return false;
        }
        base_ = heap_buf_.data();
        data_base_ = base_ + kHeaderTotal;
        is_heap_degraded_ = true;
        std::memset(state, 0, sizeof(*state));
        state->rebuilt = false;
        *recovered = false;
        return true;
    }
    base_ = static_cast<uint8_t*>(mm_.base);
    data_base_ = base_ + kHeaderTotal;

    // 4. 校验双 Header。
    Header h;
    AppendTargetCache t;
    if (!pick_header(&h, &t)) {
        // 双 Header 均坏:Header 区全零 = 全新文件(从未提交过 Header),
        // 按全新启动处理;否则整区重建,seqno 由上层接管文件 max(seqno)。
        bool fresh = true;
        for (uint32_t i = 0; i < kHeaderTotal; ++i) {
            if (base_[i] != 0) { fresh = false; break; }
        }
        wipe_all_locked();
        std::memset(state, 0, sizeof(*state));
        state->rebuilt = !fresh;
        *recovered = true;
        return true;
    }
    state->seqno       = h.seqno;
    state->flushed_len = h.flushed_len;
    state->write_pos   = h.write_pos;
    state->target      = t;
    state->rebuilt     = false;
    *recovered = (h.seqno != 0 || h.flushed_len != 0 || h.write_pos != 0);
    return true;
}

bool Staging::map_and_validate_locked() { return base_ != nullptr; }

void Staging::commit_header_locked(uint32_t seqno, uint32_t f, uint32_t w,
                                   const AppendTargetCache* target) {
    Header h;
    std::memset(&h, 0, sizeof(h));
    h.magic       = kMagicStaging;
    h.version     = 1;
    h.flags       = 0;
    h.seqno       = seqno;
    h.flushed_len = f;
    h.write_pos   = w;
    if (target) {
        std::memcpy(h.reserved, target, sizeof(AppendTargetCache));
    }
    // 先把 A 区域的前 60B 写好(CRC 待写),再算 CRC,再覆盖末 4B。
    // 读者取"CRC 对且 seqno 新者":此中间窗口里 A 的 CRC 域是旧值 → A 坏 →
    // 回退到 B(已就绪)→ 不会误用"裸"中间态。
    // 之后 A 与 B 同时持有新值。
    write_header_raw(base_, h);
    h.header_crc = compute_header_crc(base_);
    write_header_raw(base_, h);
    // spec §4.2/§10 (T4.0-3 / S-06): 双 Header 提交纪律为
    // "写 A 完整 → seq_cst 屏障 → 写 B 完整"。
    // seq_cst 屏障保证 A 的所有字节先于 B 的任意字节全局可见:
    //   - 读者只可能观察到 (A新,B旧) / (A新,B新) / (A旧,B旧) 三种组合,
    //     永远不会出现 (A旧,B新) 的逆序窗口;
    //   - 掉电场景下 pick_header 取 CRC 完整且 seqno 较新者,
    //     seq_cst 纪律把"半新半旧"窗口压缩到单个 Header 内部(由 CRC 把关)。
    std::atomic_thread_fence(std::memory_order_seq_cst);
    write_header_raw(base_ + kHeaderASize, h);
    std::atomic_thread_fence(std::memory_order_release);
}

void Staging::wipe_all_locked() {
    if (!base_) return;
    // 先把整文件置零(包含两份 Header)
    std::memset(base_, 0, total_size_);
    // 构造零 Header,但 magic/version 非零,CRC = crc32(前 60B)
    Header z;
    std::memset(&z, 0, sizeof(z));
    z.magic = kMagicStaging;
    z.version = 1;
    write_header_raw(base_, z);
    uint32_t crc = compute_header_crc(base_);
    put_u32(base_ + 60, crc);
    write_header_raw(base_ + kHeaderASize, z);
    put_u32(base_ + kHeaderASize + 60, crc);
}

Staging::WriteStatus Staging::write_records(const uint8_t* recs, uint32_t total,
                                            uint32_t* out_consumed_bytes,
                                            uint32_t* out_pad_records) {
    if (out_consumed_bytes) *out_consumed_bytes = 0;
    if (out_pad_records) *out_pad_records = 0;
    if (total == 0) return WriteStatus::kOk;
    if (total > data_size_ - 1) return WriteStatus::kTooLarge;

    // 会计上保留 1 字节空隙:free = (f - w - 1 + size) % size。
    // 空环时 free = size - 1 ≠ 0,避免空/满歧义。
    uint32_t f = get_u32(base_ + 12);  // flushed_len
    uint32_t w = get_u32(base_ + 16);  // write_pos
    uint32_t free = (f + data_size_ - w - 1) % data_size_;
    uint32_t consumed = total;

    if (w + total > data_size_) {
        // 尾部不足:垫 PAD 后回绕到 0;pad < 6B 留零回绕。
        uint32_t pad = data_size_ - w;  // 距环尾的字节数(1..data_size_-1)
        consumed = pad + total;
        // 检查空闲是否够放下 PAD + 本条
        if (free < pad + total) return WriteStatus::kFull;
        if (pad >= 6) {
            // 写入 LGXP PAD 占满至环尾:可连续多条,每条 = 6B 头 + payload(≤ 20KB);
            // payload = 剩余字节 - 6,保证不越界。
            uint32_t pos = w;
            while (pos + 6 <= data_size_) {
                uint32_t remain = data_size_ - pos;
                uint32_t this_pad = std::min<uint32_t>(kMaxSlicePayload, remain - kHeaderLen);
                format_pad(data_base_ + pos, this_pad);
                if (out_pad_records) (*out_pad_records)++;
                pos += 6 + this_pad;
            }
            if (pos < data_size_) {
                std::memset(data_base_ + pos, 0, data_size_ - pos);
            }
            w = 0;
        } else {
            // pad < 6:无记录可写,留零回绕;消费侧按"距段尾 <6B 跳过"。
            // 必须严格清零残留的 1..5 字节, 杜绝旧轮次内存脏数据!
            if (pad > 0) {
                std::memset(data_base_ + w, 0, pad);
            }
            w = 0;
        }
        // 重新计算 free(没变,但 w 改了)
        free = (f + data_size_ - w - 1) % data_size_;
        if (total > free) return WriteStatus::kFull;
    } else if (total > free) {
        return WriteStatus::kFull;
    }

    // memcpy 进环;跨回绕则两段。
    if (w + total <= data_size_) {
        std::memcpy(data_base_ + w, recs, total);
        w += total;
    } else {
        uint32_t first = data_size_ - w;
        std::memcpy(data_base_ + w, recs, first);
        std::memcpy(data_base_, recs + first, total - first);
        w = total - first;
    }
    // 写完回写 write_pos(尚未提交,本块还在飞行中)
    put_u32(base_ + 16, w);
    if (out_consumed_bytes) *out_consumed_bytes = consumed;
    return WriteStatus::kOk;
}

uint32_t Staging::snapshot_pending(uint32_t* out_f, uint32_t* out_snap) {
    uint32_t f = get_u32(base_ + 12);
    uint32_t w = get_u32(base_ + 16);
    *out_f = f;
    *out_snap = w;
    if (w >= f) return w - f;
    return data_size_ - f + w;  // 跨回绕
}

uint32_t Staging::copy_pending(uint32_t f, uint32_t snap, uint8_t* dst, uint32_t dst_capacity) {
    uint32_t total = (snap >= f) ? (snap - f) : (data_size_ - f + snap);
    if (dst_capacity < total) return 0;
    if (snap >= f) {
        std::memcpy(dst, data_base_ + f, total);
    } else {
        uint32_t first = data_size_ - f;
        std::memcpy(dst, data_base_ + f, first);
        std::memcpy(dst + first, data_base_, total - first);
    }
    return total;
}

void Staging::zero_range_locked(uint32_t f, uint32_t bytes) {
    if (bytes == 0) return;
    if (f + bytes <= data_size_) {
        std::memset(data_base_ + f, 0, bytes);
    } else {
        uint32_t first = data_size_ - f;
        std::memset(data_base_ + f, 0, first);
        std::memset(data_base_, 0, bytes - first);
    }
}

bool Staging::debug_check_invariant(uint32_t f, uint32_t w) const {
    // 任意字节 [0, data_size_) 若不在 [f, w) 区间内,必须为 0。
    // 跨回绕时活跃区 = [f, size) ∪ [0, w):需检查 [w, f) 与 [f+len1, size)。
    uint32_t len1, off1, len2, off2;
    if (w >= f) {
        len1 = w - f; off1 = f;
        len2 = 0;     off2 = 0;
    } else {
        len1 = data_size_ - f; off1 = f;
        len2 = w;              off2 = 0;
    }
    // 检查 [len2, off1)(跨回绕时跳过活跃的 [0, w) 段)
    for (uint32_t i = len2; i < off1; ++i) {
        if (data_base_[i] != 0) return false;
    }
    // 检查 [off1 + len1, data_size_)
    for (uint32_t i = off1 + len1; i < data_size_; ++i) {
        if (data_base_[i] != 0) return false;
    }
    (void)len2; (void)off2;
    return true;
}

}  // namespace logx