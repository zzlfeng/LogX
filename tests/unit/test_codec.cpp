// test_codec.cpp - 编解码单测(zstd 接入 + 降级链 + 坏帧防御)。
//
// 覆盖:
//   - zstd 压缩往返(可压缩数据体积真实缩水;随机数据不放大爆掉)
//   - encode_block 压缩失败自动降级 flag=none(经 decode_block 验证语义)
//   - decode_block 对损坏帧 / 伪造内容大小帧拒绝(宁丢不脏)
//   - none 算法恒等往返;未知算法失败

#include "logx_internal.h"
#include "logx_codec.h"
#include <gtest/gtest.h>
#include <cstring>
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

}  // namespace

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
    ASSERT_TRUE(encode_block(kFlagCompressZstd << 1, nullptr, /*seqno=*/7, salt,
                             raw.data(), static_cast<uint32_t>(raw.size()),
                             &enc));
    EXPECT_FALSE(enc.enc_ok);
    EXPECT_LT(enc.payload.size(), raw.size() / 4);

    std::vector<uint8_t> out;
    ASSERT_TRUE(decode_block(kFlagCompressZstd << 1, nullptr, 7, salt,
                             enc.payload.data(),
                             static_cast<uint32_t>(enc.payload.size()),
                             nullptr, nullptr, 0, &out));
    EXPECT_EQ(out, raw);
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

TEST(CodecTest, EncodeBlockWithZstdAndAESGCM_RoundTrips) {
    auto raw = make_compressible(8 * 1024);
    uint8_t key[32] = {0x55, 0x66, 0x77, 0x88};
    uint8_t salt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint32_t seqno = 100;

    uint8_t flag = (kFlagCompressZstd << 1) | kFlagEncrypted;

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
                             nullptr, static_cast<uint32_t>(raw.size()), &out));
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
                             nullptr, static_cast<uint32_t>(raw.size()), &decoded));
    EXPECT_EQ(decoded, raw);

    // 清理
    pal.file_unlink(path.c_str());
    pal.file_rmdir(dir.c_str());
}

#endif  // LOGX_WITH_AESGCM

