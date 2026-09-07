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
std::string make_tmp_path(const std::string& log_dir, uint32_t date) {
    Pal& pal = default_pal();
    uint32_t rnd = 0;
    if (!pal.random_bytes(&rnd, sizeof(rnd))) rnd = static_cast<uint32_t>(pal.wallclock_ms());
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s/.tmp_export_%u_%lu_%08x",
                  log_dir.c_str(), date,
                  static_cast<unsigned long>(pal.pid()), rnd);
    return std::string(buf);
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
    const uint8_t* key32 = nullptr;
    bool key_lookup_attempted = false;
    bool file_encrypted = flag_encrypted(hdr[5]);
    if (file_encrypted) {
        key_lookup_attempted = true;
        if (!inst->key_provider) {
            emit_event(inst, LOGX_EV_DEGRADED, -1, "export: no key provider for encrypted file");
            out_lines->push_back("[skipped: no key provider]\n");
            inst->pal->file_close(fd);
            return true;
        }
        key32 = inst->key_provider(kid_in_file, inst->key_user);
        if (!key32) {
            emit_event(inst, LOGX_EV_DEGRADED, -1, "export: missing key for kid");
            char line[128];
            std::snprintf(line, sizeof(line), "[skipped: kid=%u missing key]\n", kid_in_file);
            out_lines->push_back(line);
            inst->pal->file_close(fd);
            return true;
        }
    }
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
            if (!key32 && !key_lookup_attempted && inst->key_provider) {
                key_lookup_attempted = true;
                key32 = inst->key_provider(kid_in_file, inst->key_user);
            }
        }
        // CRC 校验(payload)
        if (crc32_update(0, payload.data(), payload.size()) != crc) {
            char line[128];
            std::snprintf(line, sizeof(line), "[seq=%u crc fail]\n", seqno);
            out_lines->push_back(line);
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
            off += block_size;
            continue;
        }
        // 把 raw 流按 LGXE 重组并还原"内部用户载荷"(支持单块多条记录与跨块分片流式解析)。
        std::vector<std::vector<uint8_t>> users;
        if (parse_records_stream(raw.data(), raw.size(), &users, &carry_slice_buf)) {
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

    // 1) flush
    flush_blocking(inst);

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
        std::string tmp = make_tmp_path(inst->log_dir, tmp_date);
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

// ---- C ABI 实现 ----

extern "C" {

logx_status_e logx_open(const logx_config_t* cfg, logx_instance_t** out_inst) {
    if (!cfg || !cfg->log_dir || !cfg->staging_path || !out_inst) {
        return LOGX_ERR_INVALID_ARG;
    }
    if (cfg->max_log_length == 0) {
        return LOGX_ERR_INVALID_ARG;
    }
    if (cfg->max_log_length > kMaxRecordTotal) {
        return LOGX_ERR_INVALID_ARG;
    }
    if (cfg->max_log_length >= kDataSizeDefault) {
        // 必须 < 数据区 − 1,否则放不进环
        return LOGX_ERR_INVALID_ARG;
    }
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
    Instance* inst = new Instance();
    inst->log_dir = cfg->log_dir;
    inst->staging_path = cfg->staging_path;
    inst->max_log_length = cfg->max_log_length;
    inst->retention_days = cfg->retention_days ? cfg->retention_days : 7;
    inst->max_file_size  = cfg->max_file_size ? cfg->max_file_size : (10ULL * 1024 * 1024);
    // E-06: flush_threshold 语义显式化——
    //   0          = 禁用按阈值自动唤醒(完全由 logx_flush 手动控制落盘节奏);
    //   UINT32_MAX = 取默认 8KB;
    //   其它值     = 按配置字节阈值。
    // 旧版把 0 静默替换为 8KB,吞掉了"彻底关闭自动刷盘"的业务意图。
    inst->flush_threshold = (cfg->flush_threshold == UINT32_MAX)
        ? (8 * 1024)
        : cfg->flush_threshold;
    inst->compress_algo = cfg->compress_algo;
    inst->encrypt_enable = (cfg->encrypt_enable != 0);
    inst->kid = cfg->kid;
    inst->key_provider = cfg->key_provider;
    inst->key_user = cfg->user;
    inst->err_fn = cfg->err_fn;
    inst->err_user = cfg->err_user;
    inst->pal = &default_pal();
    inst->mu = inst->pal->mutex_create();
    inst->flush_event = inst->pal->ev_create();
    if (inst->pal->threading_init() != 0) {
        inst->pal->ev_destroy(inst->flush_event);
        inst->pal->mutex_destroy(inst->mu);
        delete inst;
        return LOGX_ERR_STATE;
    }

    // 创建 staging 文件目录
    // E-05: 同 day 兼容 '/' 与 '\\' 分隔符,Windows 反斜杠路径不再误判为纯文件名
    {
        size_t pos = inst->staging_path.find_last_of("/\\");
        std::string dir = (pos == std::string::npos) ? std::string(".")
                                                    : inst->staging_path.substr(0, pos);
        inst->pal->file_mkdirs(dir.c_str());
        inst->pal->file_mkdirs(inst->log_dir.c_str());
    }

    inst->staging = new Staging(inst->pal, inst->staging_path, kDataSizeDefault);

    // 启动恢复(在创建 Flusher 之前,确保 Header 与 staging 已就绪)
    run_recovery(inst);

    if (inst->staging->is_degraded()) {
        emit_event(inst, LOGX_EV_DEGRADED, -1, "staging: mmap failed, degraded to heap");
    }

    // 启动 Flusher 线程
    if (inst->pal->thread_start(&flusher_main, inst, &inst->flusher_thread) != 0) {
        delete inst->staging;
        inst->staging = nullptr;  // S-05: 防御性置空,杜绝潜在 double-free
        inst->pal->ev_destroy(inst->flush_event);
        inst->pal->mutex_destroy(inst->mu);
        delete inst;
        return LOGX_ERR_STATE;
    }
    inst->flusher_running.store(true, std::memory_order_release);

    *out_inst = inst;
    return LOGX_OK;
}

logx_status_e logx_close(logx_instance_t* h) {
    Instance* inst = static_cast<Instance*>(h);
    if (!inst) return LOGX_ERR_INVALID_ARG;
    // 先做一次同步 flush,确保 staging 全部落块(此期间 Flusher 仍在跑)
    flush_blocking(inst);
    // 再让 Flusher 退出
    inst->flusher_exit.store(true);
    if (inst->flush_event) inst->pal->ev_signal(inst->flush_event);
    if (inst->flusher_thread) {
        inst->pal->thread_join(inst->flusher_thread);
    }
    delete inst->staging;
    inst->staging = nullptr;
    if (inst->flush_event) inst->pal->ev_destroy(inst->flush_event);
    if (inst->mu) inst->pal->mutex_destroy(inst->mu);
    delete inst;
    return LOGX_OK;
}

logx_status_e logx_write(logx_instance_t* h, int level,
                         const char* tag, const uint8_t* payload, uint32_t payload_len) {
    Instance* inst = static_cast<Instance*>(h);
    if (!inst) return LOGX_ERR_STATE;
    if (!inst->staging) return LOGX_ERR_STATE;
    if (!tag) return LOGX_ERR_INVALID_ARG;

    // 1) 锁外格式化(包含校验和计算)
    // E-02: 区分"输入非法"与"内容超长"两类失败——非法输入按 INVALID_ARG 上报,
    // 不再误入 TOO_LARGE 分支;drop_too_large 只在真正的超长路径递增。
    uint8_t buf[kMaxRecordTotal];
    uint32_t total = 0;
    int br = build_records(inst, level, tag, payload, payload_len, buf, &total);
    if (br == kBuildRecordsInvalidInput) {
        emit_event(inst, LOGX_EV_DROP, -1, "invalid log input");
        return LOGX_ERR_INVALID_ARG;
    }
    if (total > inst->max_log_length) {
        inst->drop_too_large.fetch_add(1);
        emit_event(inst, LOGX_EV_DROP, -1, "log too large");
        return LOGX_ERR_TOO_LARGE;
    }

    // 2) 锁内写入 staging
    inst->pal->mutex_lock(inst->mu);
    uint32_t consumed = 0;
    auto st = inst->staging->write_records(buf, total, &consumed);
    bool need_wake = false;
    if (st == Staging::WriteStatus::kOk) {
        inst->staged_bytes_total.fetch_add(consumed, std::memory_order_relaxed);
        uint32_t f, snap;
        uint32_t pending = inst->staging->snapshot_pending(&f, &snap);
        // E-06 复审修正:flush_threshold == 0 表示"禁用按阈值自动唤醒"。
        // 旧判定 pending >= flush_threshold 在阈值 0 时因 pending >= 0 恒真,
        // 退化为每条写入均唤醒 Flusher——语义精确反转。
        // 守卫必须放在阈值上(threshold > 0)而非 pending 上:写入成功后
        // pending 恒 > 0,对 pending 加前置条件是死代码,拦不住恒真分支。
        // 阈值 0 时的落盘节奏由 Flusher 自身的 kFlushDebounceMs 轮询兜底。
        need_wake = inst->flush_threshold > 0 && pending >= inst->flush_threshold;
    } else if (st == Staging::WriteStatus::kFull) {
        inst->drop_no_space.fetch_add(1);
        emit_event(inst, LOGX_EV_DROP, -1, "staging full");
    }
    inst->pal->mutex_unlock(inst->mu);

    if (need_wake) inst->pal->ev_signal(inst->flush_event);
    return st == Staging::WriteStatus::kOk ? LOGX_OK : LOGX_ERR_NO_SPACE;
}

logx_status_e logx_flush(logx_instance_t* h) {
    Instance* inst = static_cast<Instance*>(h);
    if (!inst) return LOGX_ERR_STATE;
    return flush_blocking(inst) ? LOGX_OK : LOGX_ERR_TIMEOUT;
}

logx_status_e logx_export(logx_instance_t* h, const char* from, const char* to,
                          logx_export_cb cb, void* cb_user) {
    Instance* inst = static_cast<Instance*>(h);
    if (!inst) return LOGX_ERR_STATE;
    if (!from || !to) return LOGX_ERR_INVALID_ARG;
    ExportRequest req;
    req.from = from; req.to = to; req.cb = cb; req.cb_user = cb_user;
    return export_range(inst, req);
}

logx_status_e logx_set_error_handler(logx_instance_t* h, logx_error_fn fn, void* user) {
    Instance* inst = static_cast<Instance*>(h);
    if (!inst) return LOGX_ERR_STATE;
    // E-01: 与 emit_event 的读取路径经 err_mu 串行化,消除数据竞争;
    // 回调注册可被任意线程调用(见 logx_c.h 线程模型)。
    inst->err_mu.lock();
    inst->err_fn = fn;
    inst->err_user = user;
    inst->err_mu.unlock();
    return LOGX_OK;
}

void logx_get_stats(logx_instance_t* h, logx_stats_t* out) {
    if (!h || !out) return;
    Instance* inst = static_cast<Instance*>(h);
    out->drop_no_space    = inst->drop_no_space.load();
    out->drop_too_large   = inst->drop_too_large.load();
    out->truncated_blocks = inst->truncated_blocks.load();
    out->pad_records      = inst->pad_records.load();
    out->block_write_errors = inst->block_write_errors.load();
    // E-07: 预留位保持零值契约(未来字段占用前必须为零初始化输出)
    for (size_t i = 0; i < sizeof(out->reserved) / sizeof(out->reserved[0]); ++i) {
        out->reserved[i] = 0;
    }
}

}  // extern "C"

}  // namespace logx