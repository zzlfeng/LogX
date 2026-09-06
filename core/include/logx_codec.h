// logx_codec.h - 编解码抽象。
//
// 接口面与 spec §3.2 / §6 对齐:
//   - 压缩:none / zstd(M3 接入实库)
//   - 加密:AES-256-GCM(nonce = 8B 文件盐 + 4B seqno);M3 接入 mbedtls
//
// 接入真库时,在编译宏 LOGX_WITH_ZSTD / LOGX_WITH_AESGCM 打开分支即可,
// 函数签名不变。flag 字节定义(与 logx_decode.py 的常量保持一致):
//   bit0        = 加密(kFlagEncrypted)
//   bit1..2     = 压缩算法编号 kAlgo* 左移 kFlagCompressShift 位
//                 (00=none, 01=zstd, 10=zlib),掩码 kFlagCompressMask
// 组装/解析一律走 make_flag / flag_algo / flag_encrypted,禁止手写移位。

#ifndef LOGX_CODEC_H
#define LOGX_CODEC_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace logx {

constexpr uint8_t kFlagEncrypted = 0x1;  // bit0: 加密标志

// 压缩算法编号(写入 flag 字节前需左移 kFlagCompressShift 位)。
constexpr uint8_t kAlgoNone = 0;
constexpr uint8_t kAlgoZstd = 1;
constexpr uint8_t kAlgoZlib = 2;

constexpr uint8_t kFlagCompressShift = 1;     // 算法编号在 flag 字节中的移位量 (bit1..2)
constexpr uint8_t kFlagCompressMask  = 0x06;  // flag 字节中 bit1..2 掩码
constexpr uint8_t kAlgoMask          = 0x03;  // 解移位后的算法编号掩码

// flag 字节组装/解析的唯一入口,避免手写移位与魔法数。
inline constexpr uint8_t make_flag(uint8_t algo, bool enc) {
    return static_cast<uint8_t>(((algo << kFlagCompressShift) & kFlagCompressMask) |
                                (enc ? kFlagEncrypted : 0));
}
inline constexpr uint8_t flag_algo(uint8_t flag) {
    return static_cast<uint8_t>((flag >> kFlagCompressShift) & kAlgoMask);
}
inline constexpr bool flag_encrypted(uint8_t flag) {
    return (flag & kFlagEncrypted) != 0;
}

// 压缩:成功返回 true,out 持有压缩结果(可能比 in 大,失败时 out 清空)。
bool compress_block(uint8_t algo, const uint8_t* in, uint32_t in_len,
                    std::vector<uint8_t>* out);
bool decompress_block(uint8_t algo, const uint8_t* in, uint32_t in_len,
                      std::vector<uint8_t>* out);

// AES-256-GCM(key=32B);nonce=12B;返回密文 || tag(16B)。失败 = false。
bool encrypt_gcm(const uint8_t key[32], const uint8_t nonce[12],
                 const uint8_t* in, uint32_t in_len,
                 std::vector<uint8_t>* out_cipher_and_tag);
bool decrypt_gcm(const uint8_t key[32], const uint8_t nonce[12],
                 const uint8_t* in, uint32_t in_len,
                 const uint8_t tag[16],
                 std::vector<uint8_t>* out_plain);

// Nonce 派生: 12B nonce = 8B salt + 4B 大端 seqno
void make_nonce(uint32_t seqno, const uint8_t salt[8], uint8_t nonce_out[12]);

// 把压缩+加密封装进单一编码流程;flag 同时表达加密与压缩。
// payload_out = [compressed? plaintext : original][+encrypted tail if enc]
// 失败原因:codec 错 / 输出超限(理论上不会,raw ≤ 20KB + 6B 头)。
struct EncodedBlock {
    std::vector<uint8_t> payload;
    bool enc_ok = false;  // 仅当输入 encrypt=true 时有效
    uint8_t final_flag = 0;  // 实际编码落盘 flag(反映压缩/加密降级后的真实状态)
};
bool encode_block(uint8_t flag,
                  const uint8_t* key32_or_null,  // null = 关闭加密
                  uint32_t seqno,
                  const uint8_t salt[8],
                  const uint8_t* raw, uint32_t raw_len,
                  EncodedBlock* out);

// ---- 测试专用 seam(生产代码不得调用) ----
// 打开后 compress_block 对所有非 none 算法直接返回失败,用于在已接入真实
// zstd 的构建里复现"运行时压缩失败 → 降级 flag=none"链路(spec §6)。
void set_compress_fail_injection(bool enabled);

// 解码端:输入是"块在文件中的切片",输出 raw 字节。
// flag 同上;失败原因:crc / gcm / 压缩失败。
bool decode_block(uint8_t flag,
                  const uint8_t* key32_or_null,
                  uint32_t seqno,
                  const uint8_t salt[8],
                  const uint8_t* payload, uint32_t payload_len,
                  const uint8_t* gcm_tag_or_null,
                  const uint8_t* raw, uint32_t raw_len,
                  std::vector<uint8_t>* out_raw);

}  // namespace logx
#endif /* LOGX_CODEC_H */