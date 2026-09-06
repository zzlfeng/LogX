// test_codec.cpp - 编解码单测(zstd 接入 + 降级链 + 坏帧防御)。
//
// 覆盖:
//   - zstd 压缩往返(可压缩数据体积真实缩水;随机数据不放大爆掉)
//   - encode_block 压缩失败自动降级 flag=none,final_flag 与 payload 编码状态
//     一致,块可正常解码(经 seam 注入 zstd 运行时失败 + kAlgoZlib 未实现路径
//     两种触发方式验证;加密组合用例在 AESGCM guard 内)
//   - decode_block 对损坏帧 / 伪造内容大小帧拒绝(宁丢不脏)
//   - decode_block 长度交叉校验: 输出长度与块头 raw_len 不符拒绝、传 0 跳过
//   - none 算法恒等往返;未知算法失败

#include "logx_internal.h"
#include "logx_codec.h"
#include <gtest/gtest.h>
#include <cstring>
#include <utility>
#include <vector>

using namespace logx;

namespace {

// 生成"高可压缩"数据(重复模式,zstd 压缩比高)。
std::vector<uint8_t> make_compressible(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i] = static_cast<uint8_t>('a' + (i % 7));
    }
    return v;
}

// 生成不可压缩的伪随机数据(LCG,避免测试内依赖随机库)。
std::vector<uint8_t> make_randomish(size_t n) {
    std::vector<uint8_t> v(n);
    uint32_t x = 0x12345678u;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        v[i] = static_cast<uint8_t>(x >> 24);
    }
    return v;
}

// 测试 Seam 的 RAII 守卫:确保即使用例中途断言失败(ASSERT_* return),
// 也会在析构时恢复全局注入状态,杜绝污染同进程后续用例。
struct ScopedCompressFailInjection {
    explicit ScopedCompressFailInjection(bool enabled = true) {
        set_compress_fail_injection(enabled);
    }
    ~ScopedCompressFailInjection() {
        set_compress_fail_injection(false);
    }
    ScopedCompressFailInjection(const ScopedCompressFailInjection&) = delete;
    ScopedCompressFailInjection& operator=(const ScopedCompressFailInjection&) = delete;
};

}  // namespace

// 覆盖 C-02: flag 字节位布局防退化测试。
// 锁定 make_flag / flag_algo / flag_encrypted 的线上一致性(bit0=加密,
// bit1..2=压缩算法),任何常量或移位改动破坏位布局时在此 fail-fast。
TEST(CodecTest, BlockFlagHelper_BitLayoutAndSemantics) {
    // 全枚举锁定: 线上 flag 字节格式为硬协议,值必须与 logx_decode.py 常量一致
    EXPECT_EQ(make_flag(kAlgoNone, false), 0x00);
    EXPECT_EQ(make_flag(kAlgoNone, true),  0x01);
    EXPECT_EQ(make_flag(kAlgoZstd, false), 0x02);
    EXPECT_EQ(make_flag(kAlgoZstd, true),  0x03);
    EXPECT_EQ(make_flag(kAlgoZlib, false), 0x04);
    EXPECT_EQ(make_flag(kAlgoZlib, true),  0x05);

    // 对称反解: 组装后经 flag_algo / flag_encrypted 还原
    for (uint8_t algo = kAlgoNone; algo <= kAlgoZlib; ++algo) {
        for (bool enc : {false, true}) {
            uint8_t f = make_flag(algo, enc);
            EXPECT_EQ(flag_algo(f), algo);
            EXPECT_EQ(flag_encrypted(f), enc);
        }
    }

    // 防御性截断: 超出 2-bit 的算法编号仅低 2 位生效,不会污染 bit3..7
    EXPECT_EQ(make_flag(0x07, false), kFlagCompressMask);
    EXPECT_EQ(flag_algo(make_flag(0x07, false)), kAlgoMask);
    EXPECT_EQ(make_flag(0x07, true), static_cast<uint8_t>(kFlagCompressMask | kFlagEncrypted));

    // 恒等式: make_flag 与手写位运算等价(锁定语义)
    EXPECT_EQ(make_flag(kAlgoZstd, true),
              static_cast<uint8_t>((kAlgoZstd << kFlagCompressShift) | kFlagEncrypted));
}

#if LOGX_WITH_ZSTD

TEST(CodecTest, ZstdRoundTrip_Compressible) {
    auto raw = make_compressible(16 * 1024);
    std::vector<uint8_t> comp;
    ASSERT_TRUE(compress_block(kAlgoZstd, raw.data(),
                               static_cast<uint32_t>(raw.size()), &comp));
    // 高重复模式必须真实缩水(否则"接入"就是假的)
    EXPECT_LT(comp.size(), raw.size() / 4);

    std::vector<uint8_t> out;
    ASSERT_TRUE(decompress_block(kAlgoZstd, comp.data(),
                                 static_cast<uint32_t>(comp.size()), &out));
    EXPECT_EQ(out, raw);
}

TEST(CodecTest, ZstdRoundTrip_Incompressible) {
    auto raw = make_randomish(4 * 1024);
    std::vector<uint8_t> comp;
    ASSERT_TRUE(compress_block(kAlgoZstd, raw.data(),
                               static_cast<uint32_t>(raw.size()), &comp));
    std::vector<uint8_t> out;
    ASSERT_TRUE(decompress_block(kAlgoZstd, comp.data(),
                                 static_cast<uint32_t>(comp.size()), &out));
    EXPECT_EQ(out, raw);
}

TEST(CodecTest, ZstdRejectsCorruptFrame) {
    auto raw = make_compressible(1024);
    std::vector<uint8_t> comp;
    ASSERT_TRUE(compress_block(kAlgoZstd, raw.data(),
                               static_cast<uint32_t>(raw.size()), &comp));
    // 破坏帧头 magic(zstd 默认帧不带内容校验和,中部翻转可能被静默容忍;
    // 真实防护靠块级 CRC32(spec §3.2),这里验证解码器自身对坏帧头的拒绝)。
    comp[0] ^= 0xFF;
    std::vector<uint8_t> out;
    EXPECT_FALSE(decompress_block(kAlgoZstd, comp.data(),
                                  static_cast<uint32_t>(comp.size()), &out));
}

TEST(CodecTest, ZstdRejectsFakeContentsize) {
    // 构造合法 zstd 帧后篡改 Frame_Header 里的内容大小字段不可靠,
    // 改为直接喂"全零 payload":getFrameContentSize 返回 ERROR/UNKNOWN。
    std::vector<uint8_t> garbage(64, 0);
    std::vector<uint8_t> out;
    EXPECT_FALSE(decompress_block(kAlgoZstd, garbage.data(),
                                  static_cast<uint32_t>(garbage.size()), &out));
}

TEST(CodecTest, EncodeBlockCompressesAndRoundTrips) {
    // 端到端:encode_block(zstd flag) → decode_block 往返一致(明文块)
    auto raw = make_compressible(16 * 1024);
    uint8_t salt[8] = {1,2,3,4,5,6,7,8};
    EncodedBlock enc;
    ASSERT_TRUE(encode_block(make_flag(kAlgoZstd, false), nullptr, /*seqno=*/7, salt,
                             raw.data(), static_cast<uint32_t>(raw.size()),
                             &enc));
    EXPECT_FALSE(enc.enc_ok);
    EXPECT_LT(enc.payload.size(), raw.size() / 4);

    std::vector<uint8_t> out;
    ASSERT_TRUE(decode_block(make_flag(kAlgoZstd, false), nullptr, 7, salt,
                             enc.payload.data(),
                             static_cast<uint32_t>(enc.payload.size()),
                             nullptr, static_cast<uint32_t>(raw.size()), &out));
    EXPECT_EQ(out, raw);

    // 验证压缩块的 expected_raw_len 交叉校验: 传错误长度拒绝且输出清空
    std::vector<uint8_t> out_bad;
    EXPECT_FALSE(decode_block(make_flag(kAlgoZstd, false), nullptr, 7, salt,
                              enc.payload.data(),
                              static_cast<uint32_t>(enc.payload.size()),
                              nullptr, static_cast<uint32_t>(raw.size() + 1), &out_bad));
    EXPECT_TRUE(out_bad.empty());
}

#endif  // LOGX_WITH_ZSTD

TEST(CodecTest, NoneRoundTrip) {
    auto raw = make_randomish(512);
    std::vector<uint8_t> out;
    ASSERT_TRUE(compress_block(kAlgoNone, raw.data(),
                               static_cast<uint32_t>(raw.size()), &out));
    EXPECT_EQ(out, raw);
}

TEST(CodecTest, UnknownAlgoFails) {
    auto raw = make_randomish(64);
    std::vector<uint8_t> out;
    // algo=3(保留值)与 zlib(2,未实现)都必须失败
    EXPECT_FALSE(compress_block(3, raw.data(),
                                static_cast<uint32_t>(raw.size()), &out));
    EXPECT_FALSE(compress_block(kAlgoZlib, raw.data(),
                                static_cast<uint32_t>(raw.size()), &out));
}

#if !LOGX_WITH_ZSTD
TEST(CodecTest, ZstdFlagFailsWhenNotBuilt) {
    // LOGX_WITH_ZSTD=0 时 algo=zstd 压缩失败
    auto raw = make_compressible(64);
    std::vector<uint8_t> out;
    EXPECT_FALSE(compress_block(kAlgoZstd, raw.data(),
                                static_cast<uint32_t>(raw.size()), &out));
}
#endif

#if LOGX_WITH_AESGCM

TEST(CodecTest, GCMRoundTrip_PlainAndCipher) {
    uint8_t key[32] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                       0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
                       0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
                       0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};
    uint8_t nonce[12] = {1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 1};
    auto plain = make_randomish(1024);

    std::vector<uint8_t> cipher_and_tag;
    ASSERT_TRUE(encrypt_gcm(key, nonce, plain.data(),
                            static_cast<uint32_t>(plain.size()),
                            &cipher_and_tag));
    EXPECT_EQ(cipher_and_tag.size(), plain.size() + 16);

    uint32_t cipher_len = static_cast<uint32_t>(plain.size());
    const uint8_t* cipher_ptr = cipher_and_tag.data();
    const uint8_t* tag_ptr = cipher_and_tag.data() + cipher_len;

    std::vector<uint8_t> decrypted;
    ASSERT_TRUE(decrypt_gcm(key, nonce, cipher_ptr, cipher_len, tag_ptr, &decrypted));
    EXPECT_EQ(decrypted, plain);
}

TEST(CodecTest, GCMRejectsWrongKey) {
    uint8_t key1[32] = {1, 2, 3};
    uint8_t key2[32] = {4, 5, 6};
    uint8_t nonce[12] = {1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 1};
    auto plain = make_randomish(256);

    std::vector<uint8_t> cipher_and_tag;
    ASSERT_TRUE(encrypt_gcm(key1, nonce, plain.data(),
                            static_cast<uint32_t>(plain.size()),
                            &cipher_and_tag));

    uint32_t cipher_len = static_cast<uint32_t>(plain.size());
    const uint8_t* cipher_ptr = cipher_and_tag.data();
    const uint8_t* tag_ptr = cipher_and_tag.data() + cipher_len;

    std::vector<uint8_t> decrypted;
    EXPECT_FALSE(decrypt_gcm(key2, nonce, cipher_ptr, cipher_len, tag_ptr, &decrypted));
}

TEST(CodecTest, GCMRejectsCorruptTagAndCiphertext) {
    uint8_t key[32] = {0xAA};
    uint8_t nonce[12] = {1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 2};
    auto plain = make_randomish(512);

    std::vector<uint8_t> cipher_and_tag;
    ASSERT_TRUE(encrypt_gcm(key, nonce, plain.data(),
                            static_cast<uint32_t>(plain.size()),
                            &cipher_and_tag));

    uint32_t cipher_len = static_cast<uint32_t>(plain.size());

    // 1) 篡改 Tag
    std::vector<uint8_t> bad_tag_buf = cipher_and_tag;
    bad_tag_buf.back() ^= 0xFF;
    std::vector<uint8_t> decrypted;
    EXPECT_FALSE(decrypt_gcm(key, nonce, bad_tag_buf.data(), cipher_len,
                             bad_tag_buf.data() + cipher_len, &decrypted));

    // 2) 篡改密文
    std::vector<uint8_t> bad_cipher_buf = cipher_and_tag;
    bad_cipher_buf[0] ^= 0xFF;
    EXPECT_FALSE(decrypt_gcm(key, nonce, bad_cipher_buf.data(), cipher_len,
                             bad_cipher_buf.data() + cipher_len, &decrypted));
}

#include "mbedtls/gcm.h"

TEST(CodecTest, GCM_OfficialNistVectors_Anchor) {
    // 零密钥基线 1: NIST GCM KAT 边界向量 (K=0^256, IV=0^96, 空明文, 0 AAD)
    {
        const uint8_t key[32] = {0};
        const uint8_t iv[12] = {0};
        const uint8_t expected_tag[16] = {
            0x53, 0x0f, 0x8a, 0xfb, 0xc7, 0x45, 0x36, 0xb9,
            0xa9, 0x63, 0xb4, 0xf1, 0xc4, 0xcb, 0x73, 0x8b};

        std::vector<uint8_t> cipher_and_tag;
        ASSERT_TRUE(encrypt_gcm(key, iv, nullptr, 0, &cipher_and_tag));
        ASSERT_EQ(cipher_and_tag.size(), 16u);
        EXPECT_EQ(std::memcmp(cipher_and_tag.data(), expected_tag, 16), 0)
            << "Zero-Key Empty Plaintext Tag mismatch";

        std::vector<uint8_t> dec;
        ASSERT_TRUE(decrypt_gcm(key, iv, nullptr, 0, cipher_and_tag.data(), &dec));
        EXPECT_TRUE(dec.empty());
    }

    // 零密钥基线 2: NIST GCM KAT 边界向量 (K=0^256, IV=0^96, 16B 全零明文, 0 AAD)
    {
        const uint8_t key[32] = {0};
        const uint8_t iv[12] = {0};
        const uint8_t plain[16] = {0};
        const uint8_t expected_cipher[16] = {
            0xce, 0xa7, 0x40, 0x3d, 0x4d, 0x60, 0x6b, 0x6e,
            0x07, 0x4e, 0xc5, 0xd3, 0xba, 0xf3, 0x9d, 0x18};
        const uint8_t expected_tag[16] = {
            0xd0, 0xd1, 0xc8, 0xa7, 0x99, 0x99, 0x6b, 0xf0,
            0x26, 0x5b, 0x98, 0xb5, 0xd4, 0x8a, 0xb9, 0x19};

        std::vector<uint8_t> cipher_and_tag;
        ASSERT_TRUE(encrypt_gcm(key, iv, plain, 16, &cipher_and_tag));
        ASSERT_EQ(cipher_and_tag.size(), 32u);
        EXPECT_EQ(std::memcmp(cipher_and_tag.data(), expected_cipher, 16), 0)
            << "Zero-Key 16B Plaintext Cipher mismatch";
        EXPECT_EQ(std::memcmp(cipher_and_tag.data() + 16, expected_tag, 16), 0)
            << "Zero-Key 16B Plaintext Tag mismatch";

        std::vector<uint8_t> dec;
        ASSERT_TRUE(decrypt_gcm(key, iv, cipher_and_tag.data(), 16,
                                cipher_and_tag.data() + 16, &dec));
        ASSERT_EQ(dec.size(), 16u);
        EXPECT_EQ(std::memcmp(dec.data(), plain, 16), 0);
    }

    // 官方规范锚点 1: McGrew-Viega 附录 B Test Case 13 (AES-256, 空明文, 0 AAD)
    {
        const uint8_t key[32] = {
            0xfe, 0xff, 0xe9, 0x92, 0x86, 0x65, 0x73, 0x1c,
            0x6d, 0x6a, 0x8f, 0x94, 0x67, 0x30, 0x83, 0x08,
            0xfe, 0xff, 0xe9, 0x92, 0x86, 0x65, 0x73, 0x1c,
            0x6d, 0x6a, 0x8f, 0x94, 0x67, 0x30, 0x83, 0x08};
        const uint8_t iv[12] = {
            0xca, 0xfe, 0xba, 0xbe, 0xfa, 0xce, 0xdb, 0xad, 0xde, 0xca, 0xf8, 0x88};
        const uint8_t expected_tag[16] = {
            0xfd, 0x2c, 0xaa, 0x16, 0xa5, 0x83, 0x2e, 0x76,
            0xaa, 0x13, 0x2c, 0x14, 0x53, 0xee, 0xda, 0x7e};

        std::vector<uint8_t> cipher_and_tag;
        ASSERT_TRUE(encrypt_gcm(key, iv, nullptr, 0, &cipher_and_tag));
        ASSERT_EQ(cipher_and_tag.size(), 16u);
        EXPECT_EQ(std::memcmp(cipher_and_tag.data(), expected_tag, 16), 0)
            << "McGrew-Viega TC13 Tag mismatch";

        std::vector<uint8_t> dec;
        ASSERT_TRUE(decrypt_gcm(key, iv, nullptr, 0, cipher_and_tag.data(), &dec));
        EXPECT_TRUE(dec.empty());
    }

    // 官方规范锚点 2: McGrew-Viega 附录 B Test Case 14 (AES-256, 64B 明文, 0 AAD)
    {
        const uint8_t key[32] = {
            0xfe, 0xff, 0xe9, 0x92, 0x86, 0x65, 0x73, 0x1c,
            0x6d, 0x6a, 0x8f, 0x94, 0x67, 0x30, 0x83, 0x08,
            0xfe, 0xff, 0xe9, 0x92, 0x86, 0x65, 0x73, 0x1c,
            0x6d, 0x6a, 0x8f, 0x94, 0x67, 0x30, 0x83, 0x08};
        const uint8_t iv[12] = {
            0xca, 0xfe, 0xba, 0xbe, 0xfa, 0xce, 0xdb, 0xad, 0xde, 0xca, 0xf8, 0x88};
        const uint8_t plain[64] = {
            0xd9, 0x31, 0x32, 0x25, 0xf8, 0x84, 0x06, 0xe5,
            0xa5, 0x59, 0x09, 0xc5, 0xaf, 0xf5, 0x26, 0x9a,
            0x86, 0xa7, 0xa9, 0x53, 0x15, 0x34, 0xf7, 0xda,
            0x2e, 0x4c, 0x30, 0x3d, 0x8a, 0x31, 0x8a, 0x72,
            0x1c, 0x3c, 0x0c, 0x95, 0x95, 0x68, 0x09, 0x53,
            0x2f, 0xcf, 0x0e, 0x24, 0x49, 0xa6, 0xb5, 0x25,
            0xb1, 0x6a, 0xed, 0xf5, 0xaa, 0x0d, 0xe6, 0x57,
            0xba, 0x63, 0x7b, 0x39, 0x1a, 0xaf, 0xd2, 0x55};
        const uint8_t expected_cipher[64] = {
            0x52, 0x2d, 0xc1, 0xf0, 0x99, 0x56, 0x7d, 0x07,
            0xf4, 0x7f, 0x37, 0xa3, 0x2a, 0x84, 0x42, 0x7d,
            0x64, 0x3a, 0x8c, 0xdc, 0xbf, 0xe5, 0xc0, 0xc9,
            0x75, 0x98, 0xa2, 0xbd, 0x25, 0x55, 0xd1, 0xaa,
            0x8c, 0xb0, 0x8e, 0x48, 0x59, 0x0d, 0xbb, 0x3d,
            0xa7, 0xb0, 0x8b, 0x10, 0x56, 0x82, 0x88, 0x38,
            0xc5, 0xf6, 0x1e, 0x63, 0x93, 0xba, 0x7a, 0x0a,
            0xbc, 0xc9, 0xf6, 0x62, 0x89, 0x80, 0x15, 0xad};
        const uint8_t expected_tag[16] = {
            0xb0, 0x94, 0xda, 0xc5, 0xd9, 0x34, 0x71, 0xbd,
            0xec, 0x1a, 0x50, 0x22, 0x70, 0xe3, 0xcc, 0x6c};

        std::vector<uint8_t> cipher_and_tag;
        ASSERT_TRUE(encrypt_gcm(key, iv, plain, 64, &cipher_and_tag));
        ASSERT_EQ(cipher_and_tag.size(), 80u);
        EXPECT_EQ(std::memcmp(cipher_and_tag.data(), expected_cipher, 64), 0)
            << "McGrew-Viega TC14 Cipher mismatch";
        EXPECT_EQ(std::memcmp(cipher_and_tag.data() + 64, expected_tag, 16), 0)
            << "McGrew-Viega TC14 Tag mismatch";

        std::vector<uint8_t> dec;
        ASSERT_TRUE(decrypt_gcm(key, iv, cipher_and_tag.data(), 64,
                                cipher_and_tag.data() + 64, &dec));
        ASSERT_EQ(dec.size(), 64u);
        EXPECT_EQ(std::memcmp(dec.data(), plain, 64), 0);
    }

    // 官方规范锚点 3: McGrew-Viega 附录 B Test Case 15 (AES-256, 64B 明文 + 20B AAD)
    {
        const uint8_t key[32] = {
            0xfe, 0xff, 0xe9, 0x92, 0x86, 0x65, 0x73, 0x1c,
            0x6d, 0x6a, 0x8f, 0x94, 0x67, 0x30, 0x83, 0x08,
            0xfe, 0xff, 0xe9, 0x92, 0x86, 0x65, 0x73, 0x1c,
            0x6d, 0x6a, 0x8f, 0x94, 0x67, 0x30, 0x83, 0x08};
        const uint8_t iv[12] = {
            0xca, 0xfe, 0xba, 0xbe, 0xfa, 0xce, 0xdb, 0xad, 0xde, 0xca, 0xf8, 0x88};
        const uint8_t aad[20] = {
            0xfe, 0xed, 0xfa, 0xce, 0xde, 0xad, 0xbe, 0xef,
            0xfe, 0xed, 0xfa, 0xce, 0xde, 0xad, 0xbe, 0xef,
            0xab, 0xad, 0xda, 0xd2};
        const uint8_t plain[64] = {
            0xd9, 0x31, 0x32, 0x25, 0xf8, 0x84, 0x06, 0xe5,
            0xa5, 0x59, 0x09, 0xc5, 0xaf, 0xf5, 0x26, 0x9a,
            0x86, 0xa7, 0xa9, 0x53, 0x15, 0x34, 0xf7, 0xda,
            0x2e, 0x4c, 0x30, 0x3d, 0x8a, 0x31, 0x8a, 0x72,
            0x1c, 0x3c, 0x0c, 0x95, 0x95, 0x68, 0x09, 0x53,
            0x2f, 0xcf, 0x0e, 0x24, 0x49, 0xa6, 0xb5, 0x25,
            0xb1, 0x6a, 0xed, 0xf5, 0xaa, 0x0d, 0xe6, 0x57,
            0xba, 0x63, 0x7b, 0x39, 0x1a, 0xaf, 0xd2, 0x55};
        const uint8_t expected_cipher[64] = {
            0x52, 0x2d, 0xc1, 0xf0, 0x99, 0x56, 0x7d, 0x07,
            0xf4, 0x7f, 0x37, 0xa3, 0x2a, 0x84, 0x42, 0x7d,
            0x64, 0x3a, 0x8c, 0xdc, 0xbf, 0xe5, 0xc0, 0xc9,
            0x75, 0x98, 0xa2, 0xbd, 0x25, 0x55, 0xd1, 0xaa,
            0x8c, 0xb0, 0x8e, 0x48, 0x59, 0x0d, 0xbb, 0x3d,
            0xa7, 0xb0, 0x8b, 0x10, 0x56, 0x82, 0x88, 0x38,
            0xc5, 0xf6, 0x1e, 0x63, 0x93, 0xba, 0x7a, 0x0a,
            0xbc, 0xc9, 0xf6, 0x62, 0x89, 0x80, 0x15, 0xad};
        const uint8_t expected_tag[16] = {
            0x2d, 0xf7, 0xcd, 0x67, 0x5b, 0x4f, 0x09, 0x16,
            0x3b, 0x41, 0xeb, 0xf9, 0x80, 0xa7, 0xf6, 0x38};

        mbedtls_gcm_context gcm;
        mbedtls_gcm_init(&gcm);
        ASSERT_EQ(mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256), 0);

        uint8_t out_cipher[64];
        uint8_t out_tag[16];
        ASSERT_EQ(mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, 64,
                                            iv, 12, aad, 20,
                                            plain, out_cipher, 16, out_tag), 0);
        EXPECT_EQ(std::memcmp(out_cipher, expected_cipher, 64), 0)
            << "McGrew-Viega TC15 Cipher mismatch";
        EXPECT_EQ(std::memcmp(out_tag, expected_tag, 16), 0)
            << "McGrew-Viega TC15 Tag mismatch";

        uint8_t out_plain[64];
        ASSERT_EQ(mbedtls_gcm_auth_decrypt(&gcm, 64, iv, 12, aad, 20,
                                           out_tag, 16, out_cipher, out_plain), 0);
        EXPECT_EQ(std::memcmp(out_plain, plain, 64), 0);

        // 防御性校验: add == NULL 但 add_len > 0 应安全返回错误而绝不崩溃
        EXPECT_EQ(mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, 64,
                                            iv, 12, nullptr, 20,
                                            plain, out_cipher, 16, out_tag),
                  MBEDTLS_ERR_GCM_BAD_INPUT);

        mbedtls_gcm_free(&gcm);
    }
}

#if defined(__aarch64__)

// 覆盖 C-04: ARMv8 Crypto Extensions 硬件路径真机验证。
// 用例直接调用 vendored mbedtls 的 C ABI(aesce.c 经 core 静态链接进本测试,
// 符号恒可用:aesce.c 的非 crypto 构建也会导出 stub)。仅要求 __aarch64__
// ——crypto 特性宏只授予了 aesce.c 这个 TU,test TU 编译期拿不到;
// CPU 是否真支持扩展由用例内 has_support() 运行时断言判定。
// 桌面 x86 构建下 __aarch64__ 未定义,整段不参与编译。
#include "mbedtls/aes.h"

extern "C" {
unsigned int mbedtls_aesce_has_support(void);
int aesce_crypt_ecb(mbedtls_aes_context*, const unsigned char[16], unsigned char[16]);
}

namespace {
// NIST SP 800-38A F.1.5 AES-256 ECB-AES256.Encrypt, 第 1 块
// key = 603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4
// plain = 6bc1bee22e409f96e93d7e117393172a
// cipher = f3eed1bdb5d2a03c064b5a7e3db181f8
const unsigned char kNistKey256[32] = {
    0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe, 0x2b, 0x73, 0xae, 0xf0, 0x85, 0x7d, 0x77, 0x81,
    0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61, 0x08, 0xd7, 0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4};
const unsigned char kNistPlain[16] = {
    0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
    0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a};
const unsigned char kNistCipher[16] = {
    0xf3, 0xee, 0xd1, 0xbd, 0xb5, 0xd2, 0xa0, 0x3c,
    0x06, 0x4b, 0x5a, 0x7e, 0x3d, 0xb1, 0x81, 0xf8};
}  // namespace

TEST(CodecTest, ArmCe_HardwarePathOnDevice) {
    // ① 真机探测必须通过(否则后续断言无意义,先证明在跑硬件路径)
    ASSERT_EQ(mbedtls_aesce_has_support(), 1u);

    // ② CE 与软实现全流程一致性自检(AES-256,生产唯一用法):
    //    匹配  → 硬件路径启用,后续断言验证正确性;
    //    不匹配 → 自检拦截,自动回退软路径(宁慢勿错的安全设计生效),
    //             用例以"安全回退"判定通过,硬件加速接入记为后续事项。
    {
        mbedtls_aes_context chk;
        mbedtls_aes_init(&chk);
        ASSERT_EQ(mbedtls_aes_setkey_enc(&chk, kNistKey256, 256), 0);
        EXPECT_EQ(chk.ce_mode, 1);
        if (chk.ce_mode == 0) {
            // 回退路径本身必须是正确的软实现(与 NIST 一致)
            unsigned char ref[16];
            ASSERT_EQ(mbedtls_aes_crypt_ecb_soft(&chk, MBEDTLS_AES_ENCRYPT,
                                                 kNistPlain, ref), 0);
            EXPECT_EQ(std::memcmp(ref, kNistCipher, 16), 0);
            mbedtls_aes_free(&chk);
            FAIL() << "CE self-check mismatch on hardware with AES support";
        }
        mbedtls_aes_free(&chk);
    }

    // ③ CE 加密与 NIST 向量一致(AES-256)
    mbedtls_aes_context ce_ctx;
    mbedtls_aes_init(&ce_ctx);
    ASSERT_EQ(mbedtls_aes_setkey_enc(&ce_ctx, kNistKey256, 256), 0);
    EXPECT_NE(ce_ctx.ce_mode, 0);   // 自检通过,密钥已转为 CE 形态
    unsigned char ce_out[16];
    ASSERT_EQ(aesce_crypt_ecb(&ce_ctx, kNistPlain, ce_out), 0);
    EXPECT_EQ(std::memcmp(ce_out, kNistCipher, 16), 0)
        << "CE output != NIST SP 800-38A F.1.5 vector";
    mbedtls_aes_free(&ce_ctx);

    // ④ CE 公共入口与软实现交叉一致(涵盖 128 / 192 / 256 三种密钥长度 × 多测试块)
    const unsigned char pt[3][16] = {
        {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff},
        {0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a},
        {0xff,0xee,0xdd,0xcc,0xbb,0xaa,0x99,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,0x00}};
    for (unsigned int bits : {128u, 192u, 256u}) {
        mbedtls_aes_context ce, soft;
        mbedtls_aes_init(&ce);
        mbedtls_aes_init(&soft);
        ASSERT_EQ(mbedtls_aes_setkey_enc(&ce, kNistKey256, bits), 0);
        ASSERT_EQ(mbedtls_aes_setkey_enc_soft(&soft, kNistKey256, bits), 0);
        EXPECT_NE(ce.ce_mode, 0);
        for (const auto& block : pt) {
            unsigned char a[16], b[16];
            ASSERT_EQ(mbedtls_aes_crypt_ecb(&ce, MBEDTLS_AES_ENCRYPT, block, a), 0);
            ASSERT_EQ(mbedtls_aes_crypt_ecb_soft(&soft, MBEDTLS_AES_ENCRYPT, block, b), 0);
            EXPECT_EQ(std::memcmp(a, b, 16), 0)
                << "CE/software mismatch (" << bits << "-bit)";
        }
        mbedtls_aes_free(&ce);
        mbedtls_aes_free(&soft);
    }

    // ⑤ GCM 全链路在 CE 路径下往返一致(生产真实用法)
    auto raw = make_compressible(2048);
    uint8_t salt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    EncodedBlock enc;
    ASSERT_TRUE(encode_block(make_flag(kAlgoNone, true), kNistKey256,
                             /*seqno=*/77, salt, raw.data(),
                             static_cast<uint32_t>(raw.size()), &enc));
    std::vector<uint8_t> dec;
    ASSERT_GT(enc.payload.size(), 16u);
    ASSERT_TRUE(decode_block(make_flag(kAlgoNone, true), kNistKey256, 77, salt,
                             enc.payload.data(),
                             static_cast<uint32_t>(enc.payload.size()) - 16,
                             enc.payload.data() + enc.payload.size() - 16,
                             static_cast<uint32_t>(raw.size()), &dec));
    EXPECT_EQ(dec, raw);
}

#endif  // __aarch64__ && crypto extensions

TEST(CodecTest, EncodeBlockWithZstdAndAESGCM_RoundTrips) {
    auto raw = make_compressible(8 * 1024);
    uint8_t key[32] = {0x55, 0x66, 0x77, 0x88};
    uint8_t salt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint32_t seqno = 100;

    uint8_t flag = make_flag(kAlgoZstd, true);

    EncodedBlock enc;
    ASSERT_TRUE(encode_block(flag, key, seqno, salt, raw.data(),
                             static_cast<uint32_t>(raw.size()), &enc));
    EXPECT_TRUE(enc.enc_ok);

    ASSERT_GT(enc.payload.size(), 16u);
    uint32_t payload_len = static_cast<uint32_t>(enc.payload.size()) - 16;
    const uint8_t* payload_ptr = enc.payload.data();
    const uint8_t* tag_ptr = enc.payload.data() + payload_len;

    std::vector<uint8_t> out;
    ASSERT_TRUE(decode_block(flag, key, seqno, salt,
                             payload_ptr, payload_len, tag_ptr,
                             static_cast<uint32_t>(raw.size()), &out));
    EXPECT_EQ(out, raw);
}

TEST(CodecTest, MakeNonce_ByteOrderAndSaltPreserved) {
    // 验证 spec §3.2: 12B Nonce 前 8B 为文件盐, 后 4B 为大端 seqno
    uint8_t salt[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint32_t seqno = 0x12345678u;
    uint8_t nonce[12] = {0};
    make_nonce(seqno, salt, nonce);

    EXPECT_EQ(std::memcmp(nonce, salt, 8), 0);
    EXPECT_EQ(nonce[8],  0x12);
    EXPECT_EQ(nonce[9],  0x34);
    EXPECT_EQ(nonce[10], 0x56);
    EXPECT_EQ(nonce[11], 0x78);
}

TEST(CodecTest, EncodeBlockMissingKey_DowngradesToPlaintext) {
    // 真实生产降级链路 (spec §6 / §10):
    // 开启加密但 key 为空 (如 KeyProvider 找不到 KID 或返回 null) 时,
    // 底层 append_block_raw 绝不中断写盘, 而是清除 kFlagEncrypted 位自动降级为明文落盘并打点。
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_downgrade_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());
    std::string path = dir + "/20260903.log";

    Instance inst{};
    inst.log_dir = dir;
    inst.pal = &pal;
    inst.compress_algo = kAlgoNone;
    inst.encrypt_enable = true;
    inst.kid = 99;

    uint8_t salt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    int fd = pal.file_open(path.c_str(), true, false);
    ASSERT_GE(fd, 0);

    // 写入文件头
    uint8_t hdr[32] = {};
    put_u32_le(hdr, kMagicFile);
    hdr[4] = 1; hdr[5] = kFlagEncrypted;
    put_u32_le(hdr + 6, 20260903u);
    put_u16(hdr + 10, 99);
    std::memcpy(hdr + 12, salt, 8);
    put_u32_le(hdr + 28, crc32_update(0, hdr, 28));
    ASSERT_EQ(pal.file_write(fd, hdr, 32), 32);

    auto raw = make_compressible(256);
    uint8_t enc_flag = kFlagEncrypted;  // 要求加密, 但传入 key = nullptr

    BlockWriteResult wr = write_and_record(&inst, fd, raw.data(),
                                           static_cast<uint32_t>(raw.size()),
                                           1, enc_flag, salt, /*key32_or_null=*/nullptr);
    EXPECT_TRUE(wr.ok);
    pal.file_close(fd);

    // 读取落盘的块头: 验证 kFlagEncrypted 被清除, 真实以明文落盘
    fd = pal.file_open(path.c_str(), false, false);
    uint8_t bhdr[34];
    EXPECT_EQ(pal.file_pread(fd, bhdr, 34, 32), 34);
    EXPECT_EQ(get_u32(bhdr), kMagicBlock);
    EXPECT_EQ(bhdr[5] & kFlagEncrypted, 0);  // 加密位已清除!

    uint32_t payload_len = get_u32(bhdr + 14);
    EXPECT_EQ(payload_len, raw.size());
    std::vector<uint8_t> payload_buf(payload_len);
    EXPECT_EQ(pal.file_pread(fd, payload_buf.data(), payload_len, 32 + 34), payload_len);
    pal.file_close(fd);

    // 验证明文内容与 raw 一致 (直接无密解码)
    std::vector<uint8_t> decoded;
    ASSERT_TRUE(decode_block(bhdr[5], nullptr, 1, salt,
                             payload_buf.data(), payload_len, nullptr,
                             static_cast<uint32_t>(raw.size()), &decoded));
    EXPECT_EQ(decoded, raw);

    // 清理
    pal.file_unlink(path.c_str());
    pal.file_rmdir(dir.c_str());
}

#endif  // LOGX_WITH_AESGCM

TEST(CodecTest, EncodeBlock_CompressionFailureDowngradesToNoneAndDecodesSuccessfully) {
    // 覆盖 C-01: 压缩失败降级链(spec §6)。
    // 用未实现的压缩算法(kAlgoZlib)触发降级;本用例不依赖 AESGCM,
    // 置于 LOGX_WITH_AESGCM guard 之外,默认构建也参与编译。
    auto raw = make_compressible(1024);
    uint8_t salt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t flag = make_flag(kAlgoZlib, false);

    EncodedBlock enc;
    // 1) 编码必须平滑降级成功, 绝不阻塞或丢日志
    ASSERT_TRUE(encode_block(flag, nullptr, /*seqno=*/10, salt,
                             raw.data(), static_cast<uint32_t>(raw.size()), &enc));

    // 2) 验证 final_flag: 压缩位已被清除为 kAlgoNone
    EXPECT_EQ(flag_algo(enc.final_flag), kAlgoNone);
    EXPECT_EQ(enc.final_flag & kFlagEncrypted, 0);

    // 3) 使用 enc.final_flag 能够正常解码还原
    std::vector<uint8_t> decoded;
    ASSERT_TRUE(decode_block(enc.final_flag, nullptr, 10, salt,
                             enc.payload.data(),
                             static_cast<uint32_t>(enc.payload.size()),
                             nullptr, static_cast<uint32_t>(raw.size()),
                             &decoded));
    EXPECT_EQ(decoded, raw);

    // 4) 反证/防退化断言: 若使用未降级的原始 flag 解码, 必然失败(毒丸块复现)
    std::vector<uint8_t> fail_out;
    EXPECT_FALSE(decode_block(flag, nullptr, 10, salt,
                              enc.payload.data(),
                              static_cast<uint32_t>(enc.payload.size()),
                              nullptr, static_cast<uint32_t>(raw.size()),
                              &fail_out));
}

TEST(CodecTest, Flusher_CompressionFailureDowngradesAndEmitsDegradedEvent) {
    // 覆盖 C-01 在 Flusher 链路的完整闭环:
    // 1. 验证落盘块头 hdr[5] 压缩位确已清零(非毒丸块)
    // 2. 验证触发了 LOGX_EV_DEGRADED 告警事件
    // 3. 验证落盘块可由 decode_block 正常还原
    // (不依赖 AESGCM,置于 guard 之外)
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_comp_downgrade_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());
    std::string path = dir + "/20260904.log";

    int degraded_ev_count = 0;
    std::string degraded_msg;
    logx_error_fn err_cb = [](void* user, logx_event_kind_e kind, int, const char* msg) {
        if (kind == LOGX_EV_DEGRADED) {
            auto* p = static_cast<std::pair<int*, std::string*>*>(user);
            (*p->first)++;
            *p->second = msg ? msg : "";
        }
    };
    std::pair<int*, std::string*> cb_user(&degraded_ev_count, &degraded_msg);

    Instance inst{};
    inst.log_dir = dir;
    inst.pal = &pal;
    inst.compress_algo = kAlgoNone;
    inst.encrypt_enable = false;
    inst.err_fn = err_cb;
    inst.err_user = &cb_user;

    uint8_t salt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    int fd = pal.file_open(path.c_str(), true, false);
    ASSERT_GE(fd, 0);

    // 写入文件头
    uint8_t hdr[32] = {};
    put_u32_le(hdr, kMagicFile);
    hdr[4] = 1; hdr[5] = 0;
    put_u32_le(hdr + 6, 20260904u);
    put_u16(hdr + 10, 0);
    std::memcpy(hdr + 12, salt, 8);
    put_u32_le(hdr + 28, crc32_update(0, hdr, 28));
    ASSERT_EQ(pal.file_write(fd, hdr, 32), 32);

    auto raw = make_compressible(512);
    uint8_t flag = make_flag(kAlgoZlib, false);  // 触发压缩降级

    BlockWriteResult wr = write_and_record(&inst, fd, raw.data(),
                                           static_cast<uint32_t>(raw.size()),
                                           1, flag, salt, nullptr);
    EXPECT_TRUE(wr.ok);
    pal.file_close(fd);

    // 验证触发了降级事件
    EXPECT_GT(degraded_ev_count, 0);
    EXPECT_NE(degraded_msg.find("compression failed"), std::string::npos);

    // 读取落盘的块头
    fd = pal.file_open(path.c_str(), false, false);
    uint8_t bhdr[34];
    EXPECT_EQ(pal.file_pread(fd, bhdr, 34, 32), 34);
    EXPECT_EQ(get_u32(bhdr), kMagicBlock);
    // 核心验证: 块头 hdr[5] 的压缩位已被清除为 kAlgoNone!
    EXPECT_EQ(flag_algo(bhdr[5]), kAlgoNone);

    uint32_t payload_len = get_u32(bhdr + 14);
    EXPECT_EQ(payload_len, raw.size());
    std::vector<uint8_t> payload_buf(payload_len);
    EXPECT_EQ(pal.file_pread(fd, payload_buf.data(), payload_len, 32 + 34), payload_len);
    pal.file_close(fd);

    // 验证明文内容能够成功解码
    std::vector<uint8_t> decoded;
    ASSERT_TRUE(decode_block(bhdr[5], nullptr, 1, salt,
                             payload_buf.data(), payload_len, nullptr,
                             static_cast<uint32_t>(raw.size()), &decoded));
    EXPECT_EQ(decoded, raw);

    // 清理
    pal.file_unlink(path.c_str());
    pal.file_rmdir(dir.c_str());
}

// 覆盖 C-03: decode_block 长度交叉校验(expected_raw_len)。
// 块头不在 CRC 覆盖范围内,畸形 raw_len 在此拦截;不依赖 AESGCM/ZSTD 宏。
TEST(CodecTest, DecodeBlock_RejectsLengthMismatch) {
    auto raw = make_compressible(512);
    uint8_t salt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint32_t raw_len = static_cast<uint32_t>(raw.size());

    EncodedBlock enc;
    ASSERT_TRUE(encode_block(make_flag(kAlgoNone, false), nullptr, /*seqno=*/20,
                             salt, raw.data(), raw_len, &enc));

    // 1) 基线: 传入正确长度,正常解码
    std::vector<uint8_t> out_ok;
    ASSERT_TRUE(decode_block(make_flag(kAlgoNone, false), nullptr, 20, salt,
                             enc.payload.data(),
                             static_cast<uint32_t>(enc.payload.size()),
                             nullptr, raw_len, &out_ok));
    EXPECT_EQ(out_ok, raw);

    // 2) 长度不符(偏大/偏小)必须拒绝,且不得返回半截数据
    std::vector<uint8_t> out_big;
    EXPECT_FALSE(decode_block(make_flag(kAlgoNone, false), nullptr, 20, salt,
                              enc.payload.data(),
                              static_cast<uint32_t>(enc.payload.size()),
                              nullptr, raw_len + 1, &out_big));
    EXPECT_TRUE(out_big.empty());

    std::vector<uint8_t> out_small;
    EXPECT_FALSE(decode_block(make_flag(kAlgoNone, false), nullptr, 20, salt,
                              enc.payload.data(),
                              static_cast<uint32_t>(enc.payload.size()),
                              nullptr, raw_len - 1, &out_small));
    EXPECT_TRUE(out_small.empty());

    // 3) 兼容语义: expected_raw_len=0 跳过长度校验
    std::vector<uint8_t> out_skip;
    ASSERT_TRUE(decode_block(make_flag(kAlgoNone, false), nullptr, 20, salt,
                             enc.payload.data(),
                             static_cast<uint32_t>(enc.payload.size()),
                             nullptr, 0, &out_skip));
    EXPECT_EQ(out_skip, raw);

    // 4) 极端边界: expected_raw_len=UINT32_MAX 必须拒绝且输出清空
    std::vector<uint8_t> out_huge;
    EXPECT_FALSE(decode_block(make_flag(kAlgoNone, false), nullptr, 20, salt,
                              enc.payload.data(),
                              static_cast<uint32_t>(enc.payload.size()),
                              nullptr, UINT32_MAX, &out_huge));
    EXPECT_TRUE(out_huge.empty());

    // 5) 空指针防御: out_raw 为空或 payload 为空但 len > 0 必须拒绝
    EXPECT_FALSE(decode_block(make_flag(kAlgoNone, false), nullptr, 20, salt,
                              enc.payload.data(),
                              static_cast<uint32_t>(enc.payload.size()),
                              nullptr, raw_len, nullptr));
    EXPECT_FALSE(decode_block(make_flag(kAlgoNone, false), nullptr, 20, salt,
                              nullptr, 100, nullptr, raw_len, &out_ok));
}

TEST(CodecTest, LogxOpen_CompressAlgo_FailFast) {
    // 覆盖 logx_open fail-fast:请求未知算法/未实现的 zlib/未开启宏时的 zstd
    // 一律返回 LOGX_ERR_INVALID_ARG 拒绝启动(spec §6)。
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_cfg_comp_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());
    std::string staging = dir + "/staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 4096;
    cfg.retention_days = 7;
    cfg.max_file_size = 10 * 1024 * 1024;
    cfg.flush_threshold = 8 * 1024;

    logx_instance_t* inst = nullptr;

    // 1. 未知算法 (> kAlgoZlib) 必须拒绝启动
    cfg.compress_algo = kAlgoZlib + 1;
    EXPECT_EQ(logx_open(&cfg, &inst), LOGX_ERR_INVALID_ARG);
    EXPECT_EQ(inst, nullptr);

    // 2. 未实现的 zlib (== kAlgoZlib) 必须拒绝启动
    cfg.compress_algo = kAlgoZlib;
    EXPECT_EQ(logx_open(&cfg, &inst), LOGX_ERR_INVALID_ARG);
    EXPECT_EQ(inst, nullptr);

#if !LOGX_WITH_ZSTD
    // 3. 编译期未接入 ZSTD 时, 配置 zstd 必须拒绝启动
    cfg.compress_algo = kAlgoZstd;
    EXPECT_EQ(logx_open(&cfg, &inst), LOGX_ERR_INVALID_ARG);
    EXPECT_EQ(inst, nullptr);
#else
    // 编译期已接入 ZSTD 时, 配置 zstd 能够正常启动
    cfg.compress_algo = kAlgoZstd;
    EXPECT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    ASSERT_NE(inst, nullptr);
    logx_close(inst);
    inst = nullptr;
#endif

    // 4. 配置 kAlgoNone 正常启动
    cfg.compress_algo = kAlgoNone;
    EXPECT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    ASSERT_NE(inst, nullptr);
    logx_close(inst);

    pal.file_unlink(staging.c_str());
    pal.file_rmdir(dir.c_str());
}

#if LOGX_WITH_AESGCM

TEST(CodecTest, EncodeBlock_CompressionDowngradeKeepsEncryption) {
    // 加密组合验证: 开启加密时压缩降级, 加密位保留而压缩位清零,
    // 密文块用降级后 flag 仍可解密还原
    auto raw = make_compressible(1024);
    uint8_t salt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t key[32] = {0x11, 0x22, 0x33, 0x44};
    uint8_t flag_enc = make_flag(kAlgoZlib, true);

    EncodedBlock enc;
    ASSERT_TRUE(encode_block(flag_enc, key, /*seqno=*/11, salt,
                             raw.data(), static_cast<uint32_t>(raw.size()), &enc));
    EXPECT_TRUE(enc.enc_ok);
    EXPECT_EQ(flag_algo(enc.final_flag), kAlgoNone);
    EXPECT_NE(enc.final_flag & kFlagEncrypted, 0);

    ASSERT_GT(enc.payload.size(), 16u);
    uint32_t ct_len = static_cast<uint32_t>(enc.payload.size()) - 16;
    std::vector<uint8_t> dec;
    ASSERT_TRUE(decode_block(enc.final_flag, key, 11, salt,
                             enc.payload.data(), ct_len,
                             enc.payload.data() + ct_len,
                             static_cast<uint32_t>(raw.size()),
                             &dec));
    EXPECT_EQ(dec, raw);
}

#endif  // LOGX_WITH_AESGCM

#if LOGX_WITH_ZSTD

TEST(CodecTest, EncodeBlock_ZstdRuntimeFailureDowngradesViaSeam) {
    // 已接入真实 zstd 的构建里,经测试 seam 强制 compress_block 失败,
    // 覆盖"zstd 运行时失败 → 降级"路径(zlib 未实现路径之外的真实场景)。
    // 使用 ScopedCompressFailInjection RAII 守卫,确保断言退出时自动清理。
    auto raw = make_compressible(4 * 1024);
    uint8_t salt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t flag = make_flag(kAlgoZstd, false);

    ScopedCompressFailInjection seam_guard(true);
    EncodedBlock enc;
    ASSERT_TRUE(encode_block(flag, nullptr, /*seqno=*/12, salt,
                             raw.data(), static_cast<uint32_t>(raw.size()), &enc));

    // 降级生效: 压缩位清零, payload 为未压缩原文(非 zstd 帧)
    EXPECT_EQ(flag_algo(enc.final_flag), kAlgoNone);
    EXPECT_EQ(enc.payload, raw);

    // 用降级后 flag 解码完整还原
    std::vector<uint8_t> decoded;
    ASSERT_TRUE(decode_block(enc.final_flag, nullptr, 12, salt,
                             enc.payload.data(),
                             static_cast<uint32_t>(enc.payload.size()),
                             nullptr, static_cast<uint32_t>(raw.size()),
                             &decoded));
    EXPECT_EQ(decoded, raw);
}

TEST(CodecTest, Flusher_ZstdRuntimeFailureDowngradesAndEmitsDegradedEventViaSeam) {
    // 真实 ZSTD 构建下经 seam 注入触发 Flusher 完整落盘降级与事件闭环
    Pal& pal = default_pal();
    std::string dir = std::string("logx_test_zstd_seam_down_") +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(rand());
    pal.file_mkdirs(dir.c_str());
    std::string path = dir + "/20260904.log";

    int degraded_ev_count = 0;
    std::string degraded_msg;
    logx_error_fn err_cb = [](void* user, logx_event_kind_e kind, int, const char* msg) {
        if (kind == LOGX_EV_DEGRADED) {
            auto* p = static_cast<std::pair<int*, std::string*>*>(user);
            (*p->first)++;
            *p->second = msg ? msg : "";
        }
    };
    std::pair<int*, std::string*> cb_user(&degraded_ev_count, &degraded_msg);

    Instance inst{};
    inst.log_dir = dir;
    inst.pal = &pal;
    inst.compress_algo = kAlgoZstd;
    inst.encrypt_enable = false;
    inst.err_fn = err_cb;
    inst.err_user = &cb_user;

    uint8_t salt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    int fd = pal.file_open(path.c_str(), true, false);
    ASSERT_GE(fd, 0);

    // 写入文件头
    uint8_t hdr[32] = {};
    put_u32_le(hdr, kMagicFile);
    hdr[4] = 1; hdr[5] = 0;
    put_u32_le(hdr + 6, 20260904u);
    put_u16(hdr + 10, 0);
    std::memcpy(hdr + 12, salt, 8);
    put_u32_le(hdr + 28, crc32_update(0, hdr, 28));
    ASSERT_EQ(pal.file_write(fd, hdr, 32), 32);

    auto raw = make_compressible(2048);
    uint8_t flag = make_flag(kAlgoZstd, false);

    ScopedCompressFailInjection seam_guard(true);
    BlockWriteResult wr = write_and_record(&inst, fd, raw.data(),
                                           static_cast<uint32_t>(raw.size()),
                                           1, flag, salt, nullptr);
    EXPECT_TRUE(wr.ok);
    pal.file_close(fd);

    // 验证触发了降级事件
    EXPECT_GT(degraded_ev_count, 0);
    EXPECT_NE(degraded_msg.find("compression failed"), std::string::npos);

    // 读取落盘的块头
    fd = pal.file_open(path.c_str(), false, false);
    uint8_t bhdr[34];
    EXPECT_EQ(pal.file_pread(fd, bhdr, 34, 32), 34);
    EXPECT_EQ(get_u32(bhdr), kMagicBlock);
    EXPECT_EQ(flag_algo(bhdr[5]), kAlgoNone);

    uint32_t payload_len = get_u32(bhdr + 14);
    EXPECT_EQ(payload_len, raw.size());
    std::vector<uint8_t> payload_buf(payload_len);
    EXPECT_EQ(pal.file_pread(fd, payload_buf.data(), payload_len, 32 + 34), payload_len);
    pal.file_close(fd);

    // 验证明文内容能够成功解码
    std::vector<uint8_t> decoded;
    ASSERT_TRUE(decode_block(bhdr[5], nullptr, 1, salt,
                             payload_buf.data(), payload_len, nullptr,
                             static_cast<uint32_t>(raw.size()), &decoded));
    EXPECT_EQ(decoded, raw);

    pal.file_unlink(path.c_str());
    pal.file_rmdir(dir.c_str());
}

#endif  // LOGX_WITH_ZSTD

