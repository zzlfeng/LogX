// logx_c.cpp - C ABI 实现 + export + 生命周期。
//
// 职责:
//   - 把 logx_c.h 的 C 函数翻译成 Instance 操作
//   - 实现 export(快照止点、解密、写临时明文文件)
//   - 持有唯一 Instance
//
// 持有:Instance + Staging + Flusher 线程(通过 Instance 字段)。

#include "logx_c.h"
#include "logx_internal.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace logx {

namespace {

// ---- export:从日志文件读块、解密、写明文 ----
//
// spec §7:先 flush,以调用时刻各文件长度为快照止点;明文临时文件写在 filesDir
// 独立临时目录(禁外部存储、禁 cacheDir);上传回调返回或抛出后立即删除。
//
// M1/M2 阶段:解密走 codec stub(codec.cpp LOGX_WITH_AESGCM=0 时直接失败),
// 因此 export 在加密文件上只能写"无法解密的标记行";M3 接入后即恢复。

// R-03: export 日期范围解析。与 parse_log_filename / valid_yyyymmdd 共用同一套
// 日历合法性校验,拒绝 20261399 / 20260231 等数值合法但日历不存在的日期。
bool parse_yyyymmdd(const char* s, uint32_t* out) {
    if (!s || std::strlen(s) != 8) return false;
    for (int i = 0; i < 8; ++i) if (s[i] < '0' || s[i] > '9') return false;
    char* endp = nullptr;
    unsigned long v = std::strtoul(s, &endp, 10);
    if (endp != s + 8 || v == 0 || v > 99991231u) return false;
    if (!valid_yyyymmdd(static_cast<uint32_t>(v))) return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

// E-03: 写出全部行并校验每次写入结果;任一短写/失败即整体失败,
// 上层据此放弃回调并上报,杜绝"临时文件没写完整仍返回 OK"。
bool write_all_lines(Instance* inst, int fd, const std::vector<std::string>& lines) {
    for (const auto& l : lines) {
        if (l.empty()) continue;
        long n = inst->pal->file_write(fd, l.data(), l.size());
        if (n != static_cast<long>(l.size())) return false;
    }
    return true;
}

// E-04: 临时明文文件按 日期 + PID + 随机数 命名——同一天多分卷/多次导出/
// 并发导出各自持有独立临时文件,消除 O_TRUNC 互踩覆盖。
// R16 F-6: 随机数/时间/PID 统一走 inst->pal(PAL 注入测试下行为一致);
// CSPRNG 失败回退原子递增计数器而非时间戳——同进程内严格单调不撞名,
// 与 PAL 契约"严禁低熵回退"(logx_pal.h)对齐。该值仅保证文件名唯一性,
// 非加密用途(加密侧 salt 的 CSPRNG 失败是 fail-fast 拒绝,见 flusher.cpp)。
std::string make_tmp_path(Instance* inst, uint32_t date) {
    static std::atomic<uint32_t> s_tmp_name_seq{0};
    uint32_t rnd = 0;
    if (!inst->pal->random_bytes(&rnd, sizeof(rnd))) {
        rnd = s_tmp_name_seq.fetch_add(1, std::memory_order_relaxed);
    }
    // F-6: 改动态拼接,不再用 160B 定长 snprintf——log_dir 过长时静默截断会
    // 产生错误路径(指向不存在的目录,导出回调拿到坏文件名)。
    char rndbuf[16];
    std::snprintf(rndbuf, sizeof(rndbuf), "%08x", rnd);
    return inst->log_dir + "/.tmp_export_" + std::to_string(date) + "_"
           + std::to_string(static_cast<unsigned long>(inst->pal->pid())) + "_" + rndbuf;
}

// 解码块 → 明文记录列表(原始 LGXE 序列 / 分片重组后格式化为可读行)。
// 为简化 M1/M2:不做语义解析,直接 dump LGXE 序列(按 spec,decoder 也这么干)。
bool decode_one_file(Instance* inst, const std::string& path, uint64_t end_size,
                     std::vector<std::string>* out_lines) {
    int fd = inst->pal->file_open(path.c_str(), false, false);
    if (fd < 0) return false;
    uint8_t hdr[32];
    if (inst->pal->file_pread(fd, hdr, 32, 0) != 32) { inst->pal->file_close(fd); return false; }
    if (get_u32(hdr) != kMagicFile) { inst->pal->file_close(fd); return false; }
    uint16_t kid_in_file = get_u16(hdr + 10);
    uint8_t salt[8];
    std::memcpy(salt, hdr + 12, 8);
    const uint8_t* key32 = invoke_key_provider(inst, kid_in_file);
    uint64_t off = 32;
    std::vector<uint8_t> carry_slice_buf;
    uint32_t prev_seqno = 0;
    bool has_prev_seqno = false;
    while (off + 34 <= end_size) {
        uint8_t bhdr[34];
        if (inst->pal->file_pread(fd, bhdr, 34, off) != 34) break;
        uint32_t seqno = 0, raw_len = 0, payload_len = 0;
        uint64_t block_size = 0;
        if (!block_header_plausible(bhdr, end_size - off,
                                    prev_seqno, has_prev_seqno,
                                    &seqno, &raw_len, &payload_len, &block_size)) {
            break;
        }
        prev_seqno = seqno;
        has_prev_seqno = true;
        uint32_t crc        = get_u32(bhdr + 18);
        uint8_t  flag       = bhdr[5];
        bool enc = flag_encrypted(flag);
        std::vector<uint8_t> payload(payload_len);
        if (inst->pal->file_pread(fd, payload.data(), payload_len, off + 34) !=
            static_cast<long>(payload_len)) break;
        std::vector<uint8_t> tag(16);
        if (enc) {
            if (inst->pal->file_pread(fd, tag.data(), 16, off + 34 + payload_len) != 16) break;
            if (!key32) {
                emit_event(inst, LOGX_EV_DEGRADED, -1, "export: missing key for kid");
                char line[128];
                std::snprintf(line, sizeof(line), "[skipped: kid=%u missing key (seq=%u)]\n", kid_in_file, seqno);
                out_lines->push_back(line);
                carry_slice_buf.clear();
                off += block_size;
                continue;
            }
        }
        // CRC 校验(payload)
        if (crc32_update(0, payload.data(), payload.size()) != crc) {
            char line[128];
            std::snprintf(line, sizeof(line), "[seq=%u crc fail]\n", seqno);
            out_lines->push_back(line);
            carry_slice_buf.clear();
            off += block_size;
            continue;
        }
        // 解码(raw bytes);raw_len 为块头声明长度,decode_block 内做长度交叉校验
        std::vector<uint8_t> raw;
        if (!decode_block(flag, key32, seqno, salt,
                          payload.data(), payload.size(),
                          enc ? tag.data() : nullptr,
                          raw_len, &raw)) {
            char line[128];
            std::snprintf(line, sizeof(line), "[seq=%u decode fail]\n", seqno);
            out_lines->push_back(line);
            carry_slice_buf.clear();
            off += block_size;
            continue;
        }
        // 把 raw 流按 LGXE 重组并还原"内部用户载荷"(支持单块多条记录与跨块分片流式解析)。
        // parse_records_stream 恒返回 true(损坏容灾在函数内部完成, 见 logx_format.h 契约)。
        std::vector<std::vector<uint8_t>> users;
        parse_records_stream(raw.data(), raw.size(), &users, &carry_slice_buf);
        static const char hex[] = "0123456789abcdef";
        for (const auto& user : users) {
            std::string hexline;
            hexline.reserve(user.size() * 2 + 32);
            char prefix[64];
            std::snprintf(prefix, sizeof(prefix), "seq=%u raw=%u ", seqno, raw_len);
            hexline.append(prefix);
            for (uint8_t b : user) {
                hexline.push_back(hex[b >> 4]);
                hexline.push_back(hex[b & 0xf]);
            }
            hexline.push_back('\n');
            out_lines->push_back(std::move(hexline));
        }
        off += block_size;
    }
    inst->pal->file_close(fd);
    return true;
}

// 按日期范围拉取 log_dir 下文件并解码 → 写临时文件 → 回调。
// 期间 Flusher 仍在写;本次 export 用调用时刻的文件长度作快照。
struct FileSnapshot {
    std::string path;
    uint64_t size;
};
std::vector<FileSnapshot> take_snapshot(Instance* inst,
                                       uint32_t from_date, uint32_t to_date) {
    auto files = inst->pal->file_list(inst->log_dir.c_str());
    // R-03: 统一走 parse_log_filename(含日历合法性校验),不再各自为政。
    auto parse_key = [](const std::string& f) -> std::pair<uint32_t, int> {
        uint32_t d = 0;
        uint16_t s = 0;
        if (!parse_log_filename(f, &d, &s)) return {0, -1};
        return {d, static_cast<int>(s)};
    };
    std::sort(files.begin(), files.end(), [&](const std::string& a, const std::string& b) {
        return parse_key(a) < parse_key(b);
    });
    std::vector<FileSnapshot> snaps;
    for (const auto& f : files) {
        uint32_t date = 0;
        uint16_t split = 0;
        if (!parse_log_filename(f, &date, &split)) continue;
        if (date < from_date || date > to_date) continue;
        int64_t sz = inst->pal->file_size_path(f.c_str());
        if (sz < 0) continue;
        snaps.push_back({f, static_cast<uint64_t>(sz)});
    }
    return snaps;
}

}  // namespace

logx_status_e export_range(Instance* inst, const ExportRequest& req) {
    if (!inst) return LOGX_ERR_STATE;
    uint32_t from, to;
    if (!parse_yyyymmdd(req.from.c_str(), &from)) return LOGX_ERR_INVALID_ARG;
    if (!parse_yyyymmdd(req.to.c_str(), &to))     return LOGX_ERR_INVALID_ARG;
    if (to < from) return LOGX_ERR_INVALID_ARG;

    // 1) flush (若超时或停滞则报警,但尽最大努力继续导出磁盘已有文件)
    if (!flush_blocking(inst)) {
        emit_event(inst, LOGX_EV_IO, LOGX_ERR_TIMEOUT, "export: flush stalled; proceeding with best effort");
    }

    // 2) 取快照
    auto snaps = take_snapshot(inst, from, to);
    if (snaps.empty()) return LOGX_ERR_NOT_FOUND;

    // 3) 写每个文件到一个临时明文文件;callback 后删除
    // E-03: 临时文件写失败与回调失败均计入最终返回码(任一失败 → LOGX_ERR_IO /
    // LOGX_ERR_CODEC),业务侧不再把"部分失败"误判为整体成功。
    inst->pal->file_mkdirs(inst->log_dir.c_str());
    logx_status_e final_status = LOGX_OK;
    for (const auto& s : snaps) {
        std::vector<std::string> lines;
        if (!decode_one_file(inst, s.path, s.size, &lines)) {
            emit_event(inst, LOGX_EV_IO, -1, "decode file failed");
            final_status = LOGX_ERR_IO;
            continue;
        }
        // R-03: 快照文件已通过 parse_log_filename 校验;临时文件名日期统一走同一入口
        uint32_t tmp_date = 0;
        uint16_t tmp_split = 0;
        if (!parse_log_filename(s.path, &tmp_date, &tmp_split)) continue;
        std::string tmp = make_tmp_path(inst, tmp_date);
        // O_CREAT | O_TRUNC:先创建,再截断到 0
        int fd = inst->pal->file_open(tmp.c_str(), true, false);
        if (fd < 0) {
            emit_event(inst, LOGX_EV_IO, -1, "export tmp create failed");
            final_status = LOGX_ERR_IO;
            continue;
        }
        if (inst->pal->file_truncate(fd, 0) != 0) {
            inst->pal->file_close(fd);
            inst->pal->file_unlink(tmp.c_str());
            emit_event(inst, LOGX_EV_IO, -1, "export tmp truncate failed");
            final_status = LOGX_ERR_IO;
            continue;
        }
        bool write_ok = write_all_lines(inst, fd, lines);
        int64_t tmp_size = inst->pal->file_size(fd);
        inst->pal->file_close(fd);
        if (!write_ok) {
            inst->pal->file_unlink(tmp.c_str());
            emit_event(inst, LOGX_EV_IO, -1, "export tmp write failed");
            final_status = LOGX_ERR_IO;
            continue;
        }
        // 回调日期字串:由已校验的 tmp_date 统一格式化 (R-03)
        char date_str[16];
        std::snprintf(date_str, sizeof(date_str), "%08u", static_cast<unsigned>(tmp_date));
        uint64_t export_size = tmp_size > 0 ? static_cast<uint64_t>(tmp_size) : 0;
        if (req.cb) {
            int cb_ret = req.cb(req.cb_user, date_str, tmp.c_str(), export_size);
            if (cb_ret != 0) {
                // E-03: 业务回调报告失败(返回非 0)时如实向上传播
                emit_event(inst, LOGX_EV_IO, cb_ret, "export callback failed");
                final_status = LOGX_ERR_CODEC;
            }
        }
        inst->pal->file_unlink(tmp.c_str());
    }
    return final_status;
}

namespace {

struct OpGuard {
    InstanceControlBlock* cb{nullptr};
    Instance*             inst{nullptr};

    explicit OpGuard(logx_instance_t* h) {
        if (!h) return;
        auto* c = reinterpret_cast<InstanceControlBlock*>(h);
        if (c->is_closing.load(std::memory_order_acquire)) return;

        // R45-02 (Dekker 组 1): 必须使用 seq_cst 确保 fetch_add(1) 与后续 is_closing.load
        // 不会与 logx_close_ex 的 is_closing.exchange(true, seq_cst) -> active_ops.load(seq_cst) 发生 Store-Load 重排
        c->active_ops.fetch_add(1, std::memory_order_seq_cst);
        if (c->is_closing.load(std::memory_order_seq_cst)) {
            // R45-01: 递增后的早退回滚若恰好将 active_ops 减至 0，必须触发递延认领检查，
            // 否则在回滚窗口内转入递延的 logx_close_ex 与 flusher_main 会双双漏认领导致永久泄漏。
            int rem = c->active_ops.fetch_sub(1, std::memory_order_seq_cst) - 1;
            if (rem == 0) {
                check_deferred_dispose_on_exit(c);
            }
            return;
        }

        Instance* i = c->inst.load(std::memory_order_acquire);
        if (!i) {
            int rem = c->active_ops.fetch_sub(1, std::memory_order_seq_cst) - 1;
            if (rem == 0) {
                check_deferred_dispose_on_exit(c);
            }
            return;
        }

        cb = c;
        inst = i;
    }

    ~OpGuard() {
        if (cb) {
            // R45-02 (Dekker 组 2/4): 必须使用 seq_cst 全序屏障
            int rem = cb->active_ops.fetch_sub(1, std::memory_order_seq_cst) - 1;
            if (rem == 0 && cb->is_closing.load(std::memory_order_seq_cst)) {
                check_deferred_dispose_on_exit(cb);
            }
        }
    }

    bool is_valid() const noexcept { return inst != nullptr; }
};

}  // namespace

void destroy_instance(InstanceControlBlock* cb, Instance* inst, bool is_flusher_thread) {
    if (!inst) return;
    Pal* pal = cb ? cb->pal : inst->pal;
    // 先做一次同步 flush,确保 staging 全部落块(仅非 Flusher 线程可调用, 杜绝自死锁)
    if (!is_flusher_thread) {
        flush_blocking(inst);
    }
    // 再让 Flusher 退出
    inst->flusher_exit.store(true, std::memory_order_release);
    if (inst->flush_event && pal) pal->ev_signal(inst->flush_event);
    if (inst->space_event && pal) pal->ev_signal(inst->space_event);
    if (inst->flusher_thread) {
        if (!is_flusher_thread) {
            if (pal) pal->thread_join(inst->flusher_thread);
        } else {
            if (pal) pal->thread_detach(inst->flusher_thread);
        }
        inst->flusher_thread = nullptr;
    }
    delete inst->staging;
    inst->staging = nullptr;
    if (inst->space_event && pal) {
        pal->ev_destroy(inst->space_event);
        inst->space_event = nullptr;
    }
    if (inst->flush_event && pal) {
        pal->ev_destroy(inst->flush_event);
        inst->flush_event = nullptr;
    }
    if (inst->mu && pal) {
        pal->mutex_destroy(inst->mu);
        inst->mu = nullptr;
    }
    auto on_destroy = cb ? cb->on_destroy : nullptr;
    void* key_user = cb ? cb->on_destroy_key_user : inst->key_user;
    void* err_user = cb ? cb->on_destroy_err_user : inst->err_user;

    delete inst;

    if (cb) {
        cb->flusher_tid.store(0, std::memory_order_release); // R44-08 / R45-09: 清零 flusher_tid 防 TID 复用误判
        cb->pal = nullptr; // R44-10: 清空 pal 避免陈旧句柄解引用已析构的 Pal
    }

    if (on_destroy) {
        on_destroy(key_user, err_user);
    }
}

// ---- C ABI 实现 ----

extern "C" {

logx_status_e logx_open(const logx_config_t* cfg, logx_instance_t** out_inst) {
    if (!out_inst) {
        return LOGX_ERR_INVALID_ARG;
    }
    *out_inst = nullptr;  // R19 F-10: 失败返回全部按契约写 NULL
    if (!cfg || !cfg->log_dir || !cfg->staging_path) {
        return LOGX_ERR_INVALID_ARG;
    }
    // B-6 方案 A: 严格下界 fail-fast 校验: 单条日志长度上限至少要能容纳
    // 最坏情况下的空记录 (kUserHdrSize 27B + kMaxTag 64B + kHeaderLen 6B = 97B)。
    // 低于此值属于病态小配置，连空日志都放不下，在 open 期直接拒绝。
    if (cfg->max_log_length < kMinLogLengthValid) {
        return LOGX_ERR_INVALID_ARG;
    }
    if (cfg->max_log_length > kMaxRecordTotal) {
        return LOGX_ERR_INVALID_ARG;
    }
    // F-4: 删除死代码——kMaxRecordTotal(64KB) 恒小于 kDataSizeDefault(130944),
    // 上一分支已保证 max_log_length ≤ kMaxRecordTotal,该"放不进环"校验不可达。
    // 压缩算法配置 fail-fast:请求编译期未接入/未实现的算法时拒绝启动,
    // 避免"每个块都触发运行期降级"的确定性场景(降级链仅用于运行时偶发失败,
    // 见 codec.cpp spec §6 注释)。
    if (cfg->compress_algo > kAlgoZlib) {
        return LOGX_ERR_INVALID_ARG;  // 未知算法(保留值)
    }
    if (cfg->compress_algo == kAlgoZlib) {
        return LOGX_ERR_INVALID_ARG;  // zlib 压缩尚未实现(compress_block 恒失败)
    }
#if !LOGX_WITH_ZSTD
    if (cfg->compress_algo == kAlgoZstd) {
        return LOGX_ERR_INVALID_ARG;  // 配置请求 zstd 但编译期未接入
    }
#endif
    // 加密配置 fail-fast: 未接入 AES-GCM 或开启加密却未提供 key_provider 时拒绝启动
#if !LOGX_WITH_AESGCM
    if (cfg->encrypt_enable != 0) {
        return LOGX_ERR_INVALID_ARG;  // 配置请求加密但编译期未接入 AES-GCM
    }
#endif
    if (cfg->encrypt_enable != 0 && !cfg->key_provider) {
        return LOGX_ERR_INVALID_ARG;  // 开启加密却未提供 key_provider
    }
    if (cfg->encrypt_enable != 0 && cfg->key_provider) {
        // R19 F-6: 开启加密时在 open 期对 kid 进行探测调用，若无对应密钥则 fail-fast 拒绝启动
        if (!cfg->key_provider(cfg->kid, cfg->user)) {
            return LOGX_ERR_INVALID_ARG;
        }
    }
    // T5.8: 分配控制块与实例数据主体
    InstanceControlBlock* cb = new (std::nothrow) InstanceControlBlock();
    if (!cb) return LOGX_ERR_NOMEM;

    Instance* inst = new (std::nothrow) Instance();
    if (!inst) {
        delete cb;
        return LOGX_ERR_NOMEM;
    }

    inst->cb = cb;
    cb->inst.store(inst, std::memory_order_relaxed);

    inst->log_dir = cfg->log_dir;
    inst->staging_path = cfg->staging_path;
    inst->max_log_length = cfg->max_log_length;
    inst->retention_days = cfg->retention_days ? cfg->retention_days : 7;
    inst->max_file_size  = cfg->max_file_size ? cfg->max_file_size : (10ULL * 1024 * 1024);
    inst->flush_threshold = (cfg->flush_threshold == UINT32_MAX)
        ? (8 * 1024)
        : cfg->flush_threshold;
    inst->staging_wait_ms = cfg->staging_wait_ms;
    inst->compress_algo = cfg->compress_algo;
    inst->encrypt_enable = (cfg->encrypt_enable != 0);
    inst->kid = cfg->kid;
    inst->key_provider = cfg->key_provider;
    inst->key_user = cfg->user;
    inst->err_fn = cfg->err_fn;
    inst->err_user = cfg->err_user;

    cb->on_destroy = cfg->on_destroy;
    cb->on_destroy_key_user = cfg->user;
    cb->on_destroy_err_user = cfg->err_user;

    inst->pal = cfg->pal_override != nullptr
        ? static_cast<Pal*>(cfg->pal_override)
        : &default_pal();
    cb->pal = inst->pal;

    inst->mu = inst->pal->mutex_create();
    inst->flush_event = inst->pal->ev_create();
    inst->space_event = inst->pal->ev_create();

    // R44-07 open 失败路径清理清单
    auto cleanup_open_failure = [&](logx_status_e err) -> logx_status_e {
        if (inst->staging) delete inst->staging;
        if (inst->space_event) inst->pal->ev_destroy(inst->space_event);
        if (inst->flush_event) inst->pal->ev_destroy(inst->flush_event);
        if (inst->mu) inst->pal->mutex_destroy(inst->mu);
        delete inst;
        delete cb;
        return err;
    };

    if (!inst->mu || !inst->flush_event || !inst->space_event) {
        return cleanup_open_failure(LOGX_ERR_STATE);
    }
    if (inst->pal->threading_init() != 0) {
        return cleanup_open_failure(LOGX_ERR_STATE);
    }

    // 创建 staging 文件目录
    {
        size_t pos = inst->staging_path.find_last_of("/\\");
        std::string dir = (pos == std::string::npos) ? std::string(".")
                                                    : inst->staging_path.substr(0, pos);
        inst->pal->file_mkdirs(dir.c_str());
        inst->pal->file_mkdirs(inst->log_dir.c_str());
    }

    inst->staging = new Staging(inst->pal, inst->staging_path, kDataSizeDefault);

    // 启动恢复
    if (!run_recovery(inst)) {
        return cleanup_open_failure(LOGX_ERR_IO);
    }

    if (inst->staging->is_degraded()) {
        emit_event(inst, LOGX_EV_DEGRADED, -1, "staging: mmap failed, degraded to heap");
    }

    // 启动 Flusher 线程
    if (inst->pal->thread_start(&flusher_main, inst, &inst->flusher_thread) != 0) {
        return cleanup_open_failure(LOGX_ERR_STATE);
    }
    inst->flusher_running.store(true, std::memory_order_release);

    *out_inst = reinterpret_cast<logx_instance_t*>(cb);
    return LOGX_OK;
}

logx_status_e logx_close_ex(logx_instance_t* h, bool* out_closed_immediately) {
    if (out_closed_immediately) *out_closed_immediately = false;
    if (!h) return LOGX_ERR_INVALID_ARG;
    auto* cb = reinterpret_cast<InstanceControlBlock*>(h);

    // 1. 原子置位 is_closing (仅首个调用者生效，后续并发 close 幂等返回 OK)
    // R45-02: 必须使用 seq_cst 与 OpGuard/emit_event/invoke_key_provider 构成 Dekker 全序屏障
    if (cb->is_closing.exchange(true, std::memory_order_seq_cst)) {
        return LOGX_OK;
    }

    // 当前线程是 is_closing.exchange(true) 唯一胜出者，且在 cb->close_requested.store(true) 之前
    // 任何其他线程均不可能抢先认领销毁，故此处访问 inst 与 cb->pal 绝对安全。
    Instance* inst = cb->inst.load(std::memory_order_acquire);
    Pal* pal = cb->pal;
    if (inst && inst->space_event && pal) {
        pal->ev_signal(inst->space_event);
    }

    uint64_t current_tid = pal ? pal->tid() : 0;
    bool on_flusher = (current_tid != 0 &&
                       cb->flusher_tid.load(std::memory_order_acquire) == current_tid);

    // 2. 检查一次静止条件 (仅非 Flusher 线程且外部在途操作与回调均为 0)
    // 决策二方案 B: 必须严格满足 active_ops == 0 && in_callback == 0
    if (!on_flusher &&
        cb->active_ops.load(std::memory_order_seq_cst) == 0 &&
        cb->in_callback.load(std::memory_order_seq_cst) == 0) {

        // ---- 同步快速路径 ----
        Instance* target = cb->inst.exchange(nullptr, std::memory_order_acq_rel);
        if (target != nullptr) {
            destroy_instance(cb, target, /*is_flusher_thread=*/false);
        }
        if (out_closed_immediately) *out_closed_immediately = true;
        return LOGX_OK;
    }

    // ---- 递延关闭路径 (非零即递延，绝不自旋等待) ----
    // R45-03:
    // (1) 非 Flusher 线程在 active_ops > 0 时严禁提前置位 inst->flusher_exit，
    //     否则 Flusher 提前退出会导致最后退出的写线程在 destroy_instance 中 flush_blocking 失败；
    // (2) 若在 Flusher 线程直接调用且无在途操作/回调，在置位 close_requested 前先行通知退出；
    //     常规 Flusher 回调栈内调用时 in_callback > 0，将由回调退出点 check_deferred_dispose_on_exit
    //     在确认 active_ops == 0 && in_callback == 0 后置位 flusher_exit；
    // (3) 严禁在 close_requested.store(true) 之后再解引用裸 inst 或 cb->pal（其他线程可能已抢先销毁）。
    if (on_flusher && inst &&
        cb->active_ops.load(std::memory_order_seq_cst) == 0 &&
        cb->in_callback.load(std::memory_order_seq_cst) == 0) {
        inst->flusher_exit.store(true, std::memory_order_release);
        if (inst->flush_event && pal) pal->ev_signal(inst->flush_event);
    }

    cb->close_requested.store(true, std::memory_order_seq_cst);

    // 兜底双检: 若活跃 op/callback 恰在 store(close_requested) 前退出完毕，且非 Flusher 线程，兜底认领
    if (!on_flusher &&
        cb->active_ops.load(std::memory_order_seq_cst) == 0 &&
        cb->in_callback.load(std::memory_order_seq_cst) == 0) {
        if (cb->close_requested.exchange(false, std::memory_order_seq_cst)) {
            Instance* target = cb->inst.exchange(nullptr, std::memory_order_acq_rel);
            if (target != nullptr) {
                destroy_instance(cb, target, /*is_flusher_thread=*/false);
                if (out_closed_immediately) *out_closed_immediately = true;
            }
        }
    }

    return LOGX_OK;
}

logx_status_e logx_close(logx_instance_t* h) {
    return logx_close_ex(h, nullptr);
}

logx_status_e logx_write(logx_instance_t* h, int level,
                         const char* tag, const uint8_t* payload, uint32_t payload_len) {
    if (!h) return LOGX_ERR_STATE;
    if (!tag) return LOGX_ERR_INVALID_ARG;

    OpGuard guard{h};
    if (!guard.is_valid()) return LOGX_ERR_STATE;
    Instance* inst = guard.inst;
    InstanceControlBlock* cb = guard.cb;
    if (!inst->staging) return LOGX_ERR_STATE;

    // 1) 锁外格式化(包含校验和计算)
    // E-02: 区分"输入非法"与"内容超长"两类失败——非法输入按 INVALID_ARG 上报,
    // 不再误入 TOO_LARGE 分支。
    // B-1B 方案B: 内容超长(max_log_length 或 60KB 用户载荷硬上限)不再整条丢弃,
    // 截断保留前缀后照常落环,返回 LOGX_OK;仅病态小上限(截断到空记录仍放不下)
    // 保留丢弃兜底,drop_too_large 只在该路径递增。
    // F-9: TLS 化调用方缓冲——每写线程持有 64KB 栈缓冲(与 format 内部 TLS scratch
    // 合计 ~128KB)对小栈线程(≤256KB)有溢出风险;thread_local 摊薄为各线程一次占用。
    static thread_local uint8_t buf[kMaxRecordTotal];
    uint32_t total = 0;
    bool truncated = false;
    uint64_t dropped_bytes = 0;
    int br = build_records(inst, level, tag, payload, payload_len, buf, &total,
                           &truncated, &dropped_bytes);
    if (br == kBuildRecordsInvalidInput) {
        emit_event(inst, LOGX_EV_DROP, LOGX_ERR_INVALID_ARG, "invalid log input");
        return cb->is_closing.load(std::memory_order_acquire) ? LOGX_OK : LOGX_ERR_INVALID_ARG;
    }
    if (br == kBuildRecordsCannotFit) {
        inst->drop_too_large.fetch_add(1);
        emit_event(inst, LOGX_EV_DROP, LOGX_ERR_TOO_LARGE, "log too large");
        return cb->is_closing.load(std::memory_order_acquire) ? LOGX_OK : LOGX_ERR_TOO_LARGE;
    }
    if (truncated) {
        inst->truncated_logs.fetch_add(1);
        // B-3 方案B/T5.6: 截断用独立事件种类 LOGX_EV_TRUNCATED 承载(code 取
        // LOGX_OK——有损写入成功,非错误),消除与病态丢弃(DROP/TOO_LARGE)共用
        // 错误码导致的下游文案耦合。与丢弃路径同款纪律:调用方线程、锁外同步
        // 发事件(P1-1 / J-04 安全性不变)。
        char msg[64];
        std::snprintf(msg, sizeof(msg), "log truncated (dropped %llu bytes)",
                      static_cast<unsigned long long>(dropped_bytes));
        emit_event(inst, LOGX_EV_TRUNCATED, LOGX_OK, msg);
        if (cb->is_closing.load(std::memory_order_acquire)) {
            return LOGX_OK;
        }
    }

    if (cb->is_closing.load(std::memory_order_acquire)) {
        return LOGX_OK;
    }

    // 2) 锁内写入 staging (B-1 方案 A: 环满支持锁外有界重试 / R32 方案 2: 事件驱动化)
    uint32_t consumed = 0;
    uint32_t pads = 0;
    Staging::WriteStatus st = Staging::WriteStatus::kFull;
    bool need_wake = false;
    bool retry_started = false;
    uint64_t start_retry_ms = 0;

    while (true) {
        inst->pal->mutex_lock(inst->mu);
        st = inst->staging->write_records(buf, total, &consumed, &pads);
        if (st == Staging::WriteStatus::kOk) {
            if (pads > 0) {
                inst->pad_records.fetch_add(pads, std::memory_order_relaxed);
            }
            inst->staged_bytes_total.fetch_add(consumed, std::memory_order_relaxed);
            uint32_t f, snap;
            uint32_t pending = inst->staging->snapshot_pending(&f, &snap);
            need_wake = inst->flush_threshold > 0 && pending >= inst->flush_threshold;
            inst->pal->mutex_unlock(inst->mu);

            // R32 方案 2 级联唤醒令牌 (Cascading Wake-up Token):
            // 若本写线程曾因满等待，写入成功后向 space_event 传递令牌，
            // 唤醒下一排队写线程探测剩余空间，消除单唤醒竞争饥饿
            if (retry_started && inst->space_event) {
                inst->pal->ev_signal(inst->space_event);
            }
            break;
        }
        inst->pal->mutex_unlock(inst->mu);

        // 空间不足: 检查重试条件 (staging_wait_ms == 0 时默认绝不阻塞)
        if (inst->staging_wait_ms == 0 || !inst->flusher_running.load(std::memory_order_acquire)) {
            break;
        }

        // P3-1 / Nit-1: 采用单调时钟防 NTP 跳变，辅以布尔标志规避 0 哨兵歧义
        uint64_t now_ms = inst->pal->monotonic_ns() / 1000000ULL;
        if (!retry_started) {
            retry_started = true;
            start_retry_ms = now_ms;
        } else if (now_ms >= start_retry_ms + inst->staging_wait_ms) {
            break;  // 超时
        }

        // 唤醒 Flusher 加速刷盘
        if (inst->flush_event) {
            inst->pal->ev_signal(inst->flush_event);
        }

        // P3-2: 正在关闭时跳出循环交由统一 kFull 记账，避免静默丢弃无痕迹
        // (同时向下一个排队写线程级联传递 space_event 唤醒，确保所有写线程快速退出)
        if (cb->is_closing.load(std::memory_order_acquire)) {
            if (inst->space_event) {
                inst->pal->ev_signal(inst->space_event);
            }
            break;
        }

        // R32 方案 2: 事件驱动等待 + 有界切片退避 (替代旧版固定 sleep_ms(2))
        // 计算剩余重试预算，切片等待 (至多 5ms) 保证即使 Flusher 偶发漏通知也能及时重试/检查 closing
        uint64_t elapsed_ms = now_ms - start_retry_ms;
        uint32_t remaining_ms = (elapsed_ms < inst->staging_wait_ms)
            ? static_cast<uint32_t>(inst->staging_wait_ms - elapsed_ms)
            : 0;
        if (remaining_ms == 0) {
            break;
        }
        uint32_t wait_slice_ms = std::min<uint32_t>(remaining_ms, 5u);
        if (inst->space_event) {
            inst->pal->ev_wait_timeout(inst->space_event, wait_slice_ms);
        } else {
            inst->pal->sleep_ms(1);
        }

        // 唤醒后再次检查 closing
        if (cb->is_closing.load(std::memory_order_acquire)) {
            if (inst->space_event) {
                inst->pal->ev_signal(inst->space_event);
            }
            break;
        }
    }

    // P1-1: emit_event 必须在解锁后触发, 杜绝回调内调用 close/flush 造成自死锁,
    // 同时避免回调内耗时阻断其他并发写线程。
    if (st == Staging::WriteStatus::kFull) {
        inst->drop_no_space.fetch_add(1);
        emit_event(inst, LOGX_EV_DROP, LOGX_ERR_NO_SPACE, "staging full");
    } else if (st == Staging::WriteStatus::kTooLarge) {
        // F-3: write_records 的单条超上限状态单独记账——open 已用 kMaxRecordTotal
        // 前置校验, 常态不可达;保留显式分支避免被误记入 NO_SPACE, 错误码语义对齐。
        inst->drop_too_large.fetch_add(1);
        emit_event(inst, LOGX_EV_DROP, LOGX_ERR_TOO_LARGE, "record too large for staging");
    }

    if (cb->is_closing.load(std::memory_order_acquire)) {
        return LOGX_OK;
    }

    if (need_wake) inst->pal->ev_signal(inst->flush_event);
    if (st == Staging::WriteStatus::kOk) return LOGX_OK;
    return st == Staging::WriteStatus::kTooLarge ? LOGX_ERR_TOO_LARGE : LOGX_ERR_NO_SPACE;
}

logx_status_e logx_flush(logx_instance_t* h) {
    if (!h) return LOGX_ERR_STATE;
    OpGuard guard{h};
    if (!guard.is_valid()) return LOGX_ERR_STATE;
    return flush_blocking(guard.inst) ? LOGX_OK : LOGX_ERR_TIMEOUT;
}

logx_status_e logx_export(logx_instance_t* h, const char* from, const char* to,
                          logx_export_cb cb, void* cb_user) {
    if (!h) return LOGX_ERR_STATE;
    if (!from || !to) return LOGX_ERR_INVALID_ARG;
    OpGuard guard{h};
    if (!guard.is_valid()) return LOGX_ERR_STATE;
    ExportRequest req;
    req.from = from; req.to = to; req.cb = cb; req.cb_user = cb_user;
    return export_range(guard.inst, req);
}

logx_status_e logx_set_error_handler(logx_instance_t* h, logx_error_fn fn, void* user) {
    if (!h) return LOGX_ERR_STATE;
    OpGuard guard{h};
    if (!guard.is_valid()) return LOGX_ERR_STATE;
    Instance* inst = guard.inst;
    InstanceControlBlock* cb = guard.cb;
    std::lock_guard<std::mutex> lk(inst->err_mu);
    inst->err_fn = fn;
    inst->err_user = user;
    if (cb) {
        cb->on_destroy_err_user = user; // R44-09 / R45-07: 同步更新销毁回调错误上下文快照
    }
    return LOGX_OK;
}

void logx_get_stats(logx_instance_t* h, logx_stats_t* out) {
    if (!out) return;
    if (!h) {
        std::memset(out, 0, sizeof(*out));
        return;
    }
    OpGuard guard{h};
    if (!guard.is_valid()) {
        std::memset(out, 0, sizeof(*out));
        return;
    }
    Instance* inst = guard.inst;
    out->drop_no_space    = inst->drop_no_space.load();
    out->drop_too_large   = inst->drop_too_large.load();
    out->truncated_blocks = inst->truncated_blocks.load();
    out->pad_records      = inst->pad_records.load();
    out->block_write_errors = inst->block_write_errors.load();
    out->truncated_logs   = inst->truncated_logs.load();
    for (size_t i = 0; i < sizeof(out->reserved) / sizeof(out->reserved[0]); ++i) {
        out->reserved[i] = 0;
    }
}

}  // extern "C"

}  // namespace logx