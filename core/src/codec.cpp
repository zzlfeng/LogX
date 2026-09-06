// codec.cpp - 编解码实现。
//
// 压缩:none / zstd(third_party/zstd vendor 源码,LOGX_WITH_ZSTD 接入);
// 加密:AES-256-GCM(M3 待接入,LOGX_WITH_AESGCM)。
//
// 降级链(spec §6):
//   - 编码侧压缩失败(zstd 运行时错误 / 算法未实现) → 自动回退 flag=none
//     继续写(encode_block 内处理);实际生效 flag 经 EncodedBlock.final_flag
//     传回,调用方(flusher)必须以 final_flag 写块头 hdr[5],否则块头声明的
//     压缩算法与 payload 真实编码不一致,构成解码必然失败的"毒丸块"(C-01);
//   - "配置请求编译期未接入/未实现的算法"由 logx_open fail-fast 拒绝,
//     不进入运行期降级路径;
//   - 解码侧任何失败(帧损坏 / 内容大小超契约上限 / GCM 失败)一律 false,
//     由调用方"宁丢不脏"丢弃该块。

#include "logx_codec.h"

#include <atomic>
#include <cstring>
#include <vector>

#include "logx_staging.h"  // crc32_*, kBlockMaxRaw(解压输出上限)
#include "logx_format.h"   // kMaxRecordTotal

#if LOGX_WITH_ZSTD
extern "C" {
#include <zstd.h>
}
#endif

#if LOGX_WITH_AESGCM
extern "C" {
#include <mbedtls/gcm.h>
}
#endif

namespace logx {

namespace {

// 解压输出上限:块 raw_len 契约上限(20KB + 6B 头)。超出即视为伪造/损坏帧,
// 拒绝分配(防 ZSTD_getFrameContentSize 声明值被坏块伪造导致的内存炸弹)。
constexpr unsigned long long kDecompressOutputMax =
    static_cast<unsigned long long>(kBlockMaxRaw) + 64;

// 压缩失败注入开关(测试 seam,见 logx_codec.h 声明)。
// atomic:测试线程写、Flusher 线程读,避免数据竞争。
std::atomic<bool> g_inject_compress_fail{false};

}  // namespace

void set_compress_fail_injection(bool enabled) {
    g_inject_compress_fail.store(enabled, std::memory_order_relaxed);
}

void make_nonce(uint32_t seqno, const uint8_t salt[8], uint8_t nonce_out[12]) {
    std::memcpy(nonce_out, salt, 8);
    // seqno 大端写 nonce 后 4B
    nonce_out[8]  = (seqno >> 24) & 0xff;
    nonce_out[9]  = (seqno >> 16) & 0xff;
    nonce_out[10] = (seqno >> 8) & 0xff;
    nonce_out[11] = (seqno) & 0xff;
}

bool compress_block(uint8_t algo, const uint8_t* in, uint32_t in_len,
                    std::vector<uint8_t>* out) {
    out->clear();
    if (algo == kAlgoNone) {
        out->assign(in, in + in_len);
        return true;
    }
    // 测试 seam:强制非 none 算法压缩失败(复现运行时降级链路)
    if (g_inject_compress_fail.load(std::memory_order_relaxed)) {
        out->clear();
        return false;
    }
#if LOGX_WITH_ZSTD
    if (algo == kAlgoZstd) {
        size_t bound = ZSTD_compressBound(in_len);
        out->resize(bound);
        size_t n = ZSTD_compress(out->data(), bound, in, in_len, 3);
        if (ZSTD_isError(n)) { out->clear(); return false; }
        out->resize(n);
        return true;
    }
#endif
    // 其他算法未实现
    return false;
}

bool decompress_block(uint8_t algo, const uint8_t* in, uint32_t in_len,
                      std::vector<uint8_t>* out) {
    out->clear();
    if (algo == kAlgoNone) {
        out->assign(in, in + in_len);
        return true;
    }
#if LOGX_WITH_ZSTD
    if (algo == kAlgoZstd) {
        unsigned long long total = ZSTD_getFrameContentSize(in, in_len);
        if (total == ZSTD_CONTENTSIZE_ERROR || total == ZSTD_CONTENTSIZE_UNKNOWN) {
            return false;
        }
        // 块内容大小有契约上限(kBlockMaxRaw);声明超限的帧一律拒绝,
        // 不按声明值分配内存(损坏/伪造块防御)。
        if (total > kDecompressOutputMax) {
            return false;
        }
        out->resize(static_cast<size_t>(total));
        size_t n = ZSTD_decompress(out->data(), total, in, in_len);
        if (ZSTD_isError(n)) { out->clear(); return false; }
        out->resize(n);
        return true;
    }
#endif
    return false;
}

bool encrypt_gcm(const uint8_t key[32], const uint8_t nonce[12],
                 const uint8_t* in, uint32_t in_len,
                 std::vector<uint8_t>* out_cipher_and_tag) {
#if LOGX_WITH_AESGCM
    out_cipher_and_tag->clear();
    if (!key || !nonce) return false;
    if (in_len > 0 && !in) return false;

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);

    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (ret != 0) {
        mbedtls_gcm_free(&gcm);
        return false;
    }

    out_cipher_and_tag->resize(in_len + 16);
    unsigned char* out_data = out_cipher_and_tag->data();
    unsigned char* tag_data = out_data + in_len;

    ret = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, in_len,
                                    nonce, 12,
                                    nullptr, 0,
                                    in, out_data,
                                    16, tag_data);
    mbedtls_gcm_free(&gcm);
    if (ret != 0) {
        out_cipher_and_tag->clear();
        return false;
    }
    return true;
#else
    (void)key; (void)nonce; (void)in; (void)in_len;
    (void)out_cipher_and_tag;
    return false;
#endif
}

bool decrypt_gcm(const uint8_t key[32], const uint8_t nonce[12],
                 const uint8_t* in, uint32_t in_len, const uint8_t tag[16],
                 std::vector<uint8_t>* out_plain) {
#if LOGX_WITH_AESGCM
    out_plain->clear();
    if (!key || !nonce || !tag) return false;
    if (in_len > 0 && !in) return false;

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);

    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (ret != 0) {
        mbedtls_gcm_free(&gcm);
        return false;
    }

    out_plain->resize(in_len);
    unsigned char* out_data = in_len > 0 ? out_plain->data() : nullptr;

    ret = mbedtls_gcm_auth_decrypt(&gcm, in_len,
                                   nonce, 12,
                                   nullptr, 0,
                                   tag, 16,
                                   in, out_data);
    mbedtls_gcm_free(&gcm);
    if (ret != 0) {
        out_plain->clear();
        return false;
    }
    return true;
#else
    (void)key; (void)nonce; (void)in; (void)in_len; (void)tag;
    (void)out_plain;
    return false;
#endif
}

bool encode_block(uint8_t flag,
                  const uint8_t* key32_or_null,
                  uint32_t seqno,
                  const uint8_t salt[8],
                  const uint8_t* raw, uint32_t raw_len,
                  EncodedBlock* out) {
    if (!out) return false;
    if (raw_len > 0 && !raw) return false;
    uint8_t comp = flag_algo(flag);
    bool enc = flag_encrypted(flag);
    if (enc && !key32_or_null) {
        // 加密请求但密钥缺失:fail-fast;调用方应降级到明文 flag。
        return false;
    }

    // 1) 压缩(若启用)。压缩失败(zstd 内部错误/资源不足)按 spec §6 降级:
    //    flag 去掉压缩位,明文继续写,绝不因 codec 故障阻塞/丢日志。
    std::vector<uint8_t> compd;
    if (comp != kAlgoNone) {
        if (!compress_block(comp, raw, raw_len, &compd)) {
            comp = kAlgoNone;
            compd.assign(raw, raw + raw_len);
        }
    } else {
        compd.assign(raw, raw + raw_len);
    }

    // 2) 加密(若启用)
    if (enc) {
        uint8_t nonce[12];
        make_nonce(seqno, salt, nonce);
        std::vector<uint8_t> cipher_and_tag;
        if (!encrypt_gcm(key32_or_null, nonce, compd.data(),
                         static_cast<uint32_t>(compd.size()),
                         &cipher_and_tag)) {
            return false;
        }
        out->payload = std::move(cipher_and_tag);
        out->enc_ok = true;
    } else {
        out->payload = std::move(compd);
        out->enc_ok = false;
    }

    uint8_t final_flag = flag & static_cast<uint8_t>(~(kFlagEncrypted | kFlagCompressMask));
    if (out->enc_ok) {
        final_flag |= kFlagEncrypted;
    }
    final_flag |= static_cast<uint8_t>(comp << kFlagCompressShift);
    out->final_flag = final_flag;

    return true;
}

bool decode_block(uint8_t flag,
                  const uint8_t* key32_or_null,
                  uint32_t seqno,
                  const uint8_t salt[8],
                  const uint8_t* payload, uint32_t payload_len,
                  const uint8_t* gcm_tag_or_null,
                  uint32_t expected_raw_len,
                  std::vector<uint8_t>* out_raw) {
    if (!out_raw) return false;
    if (payload_len > 0 && !payload) return false;
    uint8_t comp = flag_algo(flag);
    bool enc = flag_encrypted(flag);

    // 1) 解密(若启用);密文 = payload,tag = 块末尾 16B。
    std::vector<uint8_t> plain;
    if (enc) {
        if (!key32_or_null) return false;
        if (!gcm_tag_or_null) return false;
        uint8_t nonce[12];
        make_nonce(seqno, salt, nonce);
        // payload 在块中已经是 [cipher],tag 紧跟其后
        if (!decrypt_gcm(key32_or_null, nonce,
                         payload, payload_len,
                         gcm_tag_or_null, &plain)) {
            return false;
        }
    } else {
        plain.assign(payload, payload + payload_len);
    }

    // 2) 解压
    if (!decompress_block(comp, plain.data(), static_cast<uint32_t>(plain.size()), out_raw)) {
        return false;
    }
    // 3) 长度交叉校验:块头声明的 raw_len 与实际解出长度必须一致
    //    (块头不在 CRC 覆盖范围内,畸形 raw_len 在此拦截;0 = 跳过校验)。
    //    宁丢不脏:校验失败不得返回半截数据。
    if (expected_raw_len != 0 && out_raw->size() != expected_raw_len) {
        out_raw->clear();
        return false;
    }
    return true;
}

}  // namespace logx