// codec.cpp - 编解码实现。
//
// 压缩:none / zstd(third_party/zstd vendor 源码,LOGX_WITH_ZSTD 接入);
// 加密:AES-256-GCM(M3 待接入,LOGX_WITH_AESGCM)。
//
// 降级链(spec §6):
//   - 编码侧压缩失败 → 自动回退 flag=none 继续写(encode_block 内处理);
//   - LOGX_WITH_ZSTD=0 时 flag=zstd 的块编码失败,调用方(flusher)报
//     block_write_errors 并按 IO 错误处理该块;
//   - 解码侧任何失败(帧损坏 / 内容大小超契约上限 / GCM 失败)一律 false,
//     由调用方"宁丢不脏"丢弃该块。

#include "logx_codec.h"

#include <cstring>
#include <vector>

#include "logx_staging.h"  // crc32_*, kBlockMaxRaw(解压输出上限)
#include "logx_format.h"   // kMaxRecordTotal

#if LOGX_WITH_ZSTD
extern "C" {
#include <zstd.h>
}
#endif

namespace logx {

namespace {

// 解压输出上限:块 raw_len 契约上限(20KB + 6B 头)。超出即视为伪造/损坏帧,
// 拒绝分配(防 ZSTD_getFrameContentSize 声明值被坏块伪造导致的内存炸弹)。
constexpr unsigned long long kDecompressOutputMax =
    static_cast<unsigned long long>(kBlockMaxRaw) + 64;

void make_nonce(uint32_t seqno, const uint8_t salt[8], uint8_t nonce_out[12]) {
    std::memcpy(nonce_out, salt, 8);
    // seqno 大端写 nonce 后 4B
    nonce_out[8]  = (seqno >> 24) & 0xff;
    nonce_out[9]  = (seqno >> 16) & 0xff;
    nonce_out[10] = (seqno >> 8) & 0xff;
    nonce_out[11] = (seqno) & 0xff;
}

}  // namespace

bool compress_block(uint8_t algo, const uint8_t* in, uint32_t in_len,
                    std::vector<uint8_t>* out) {
    out->clear();
    if (algo == kAlgoNone) {
        out->assign(in, in + in_len);
        return true;
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

bool encrypt_gcm(const uint8_t[32], const uint8_t[12],
                 const uint8_t*, uint32_t,
                 std::vector<uint8_t>* out_cipher_and_tag) {
#if LOGX_WITH_AESGCM
    // TODO M3:接入 mbedtls_gcm;固定 key+nonce 一次调用,out = ciphertext || tag(16B)。
    out_cipher_and_tag->clear();
    return false;  // 占位
#else
    // 没接真库时:不实现加密;调用方应走降级(flag bit0 = 0)。
    (void)out_cipher_and_tag;
    return false;
#endif
}

bool decrypt_gcm(const uint8_t[32], const uint8_t[12],
                 const uint8_t*, uint32_t, const uint8_t[16],
                 std::vector<uint8_t>* out_plain) {
#if LOGX_WITH_AESGCM
    (void)out_plain;
    return false;  // 占位
#else
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
    uint8_t comp = (flag >> 1) & 0x3;
    bool enc = (flag & kFlagEncrypted) != 0;
    if (enc && !key32_or_null) {
        // 加密请求但密钥缺失:fail-fast;调用方应降级到明文 flag。
        return false;
    }

    // 1) 压缩(若启用)。压缩失败(zstd 内部错误/资源不足)按 spec §6 降级:
    //    flag 去掉压缩位,明文继续写,绝不因 codec 故障阻塞/丢日志。
    std::vector<uint8_t> compd;
    if (comp != kAlgoNone &&
        !compress_block(comp, raw, raw_len, &compd)) {
        comp = kAlgoNone;
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
    return true;
}

bool decode_block(uint8_t flag,
                  const uint8_t* key32_or_null,
                  uint32_t seqno,
                  const uint8_t salt[8],
                  const uint8_t* payload, uint32_t payload_len,
                  const uint8_t* gcm_tag_or_null,
                  const uint8_t* raw /* raw_len 之前的字节,用于 crc32 校验 */,
                  uint32_t raw_len,
                  std::vector<uint8_t>* out_raw) {
    (void)raw;  // crc32 校验由调用方在传 payload 前用 raw + raw_len 算;此处只需解出 raw。
    (void)raw_len;
    uint8_t comp = (flag >> 1) & 0x3;
    bool enc = (flag & kFlagEncrypted) != 0;

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
    return true;
}

}  // namespace logx