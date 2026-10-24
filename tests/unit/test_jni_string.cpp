// test_jni_string.cpp - J-03 回归测试:UTF-8 → UTF-16 解码器(jni_string_util.h)。
//
// 该解码器是 JNI 层 make_jstring_utf16 的核心:替代 NewStringUTF 的
// Modified UTF-8 约束,容忍非 ASCII / 补充平面 / 嵌入 NUL 输入。
// 头文件为纯逻辑(无 JNI 依赖),桌面与 Android 单测均可直接覆盖。
//
// 覆盖:
//   - ASCII 恒等、空串
//   - Modified UTF-8 NUL 编码(C0 80 → U+0000)
//   - 2/3 字节 BMP 字符(中文等)
//   - 4 字节补充平面字符 → 代理对(emoji)
//   - 过长编码、代理区、越界(U+10FFFF 以上)拒绝 → U+FFFD
//   - 截断序列、非法前缀、裸续字节 → U+FFFD + 单字节重同步(不崩溃、不丢后续)
#include <gtest/gtest.h>

#include <jni_string_util.h>
#include <jni_key_cache.h>

#include <string>
#include <vector>

using logx_jni::cesu8_to_utf8_inplace;
using logx_jni::utf8_to_utf16;

namespace {

std::u16string to_u16(const std::vector<uint16_t>& v) {
    return std::u16string(v.begin(), v.end());
}

// 以原始字节构造输入(避免源码编码歧义)
std::string bytes(std::initializer_list<unsigned char> b) {
    return std::string(b.begin(), b.end());
}

}  // namespace

TEST(JniString, AsciiIdentityAndEmpty) {
    EXPECT_TRUE(utf8_to_utf16("", 0).empty());
    auto r = utf8_to_utf16("LogX 2026-09-08", 15);
    EXPECT_EQ(to_u16(r), u"LogX 2026-09-08");
}

TEST(JniString, ModifiedUtf8NulDecodesToU0000) {
    // C0 80 是 Modified UTF-8 对 NUL 的编码;标准 UTF-8 禁止,解码器按
    // Modified UTF-8 特例放行 → U+0000(而非 U+FFFD)
    auto s = bytes({0xC0, 0x80});
    auto r = utf8_to_utf16(s.data(), s.size());
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0], 0x0000);
}

TEST(JniString, TwoAndThreeByteBmp) {
    // "中" U+4E2D = E4 B8 AD;"日志" 两字
    auto s = bytes({0xE4, 0xB8, 0xAD, 0xE6, 0x97, 0xA5});
    auto r = utf8_to_utf16(s.data(), s.size());
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0], 0x4E2D);
    EXPECT_EQ(r[1], 0x65E5);

    // 欧元符 U+20AC = E2 82 AC
    auto e = bytes({0xE2, 0x82, 0xAC});
    auto re = utf8_to_utf16(e.data(), e.size());
    ASSERT_EQ(re.size(), 1u);
    EXPECT_EQ(re[0], 0x20AC);
}

TEST(JniString, FourByteSupplementaryToSurrogatePair) {
    // U+1F4A9 = F0 9F 92 A9 (emoji) → D83D DCA9
    auto s = bytes({0xF0, 0x9F, 0x92, 0xA9});
    auto r = utf8_to_utf16(s.data(), s.size());
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0], 0xD83D);
    EXPECT_EQ(r[1], 0xDCA9);

    // U+10FFFF 极值 = F4 8F BF BF
    auto m = bytes({0xF4, 0x8F, 0xBF, 0xBF});
    auto rm = utf8_to_utf16(m.data(), m.size());
    ASSERT_EQ(rm.size(), 2u);
    EXPECT_EQ(rm[0], 0xDBFF);
    EXPECT_EQ(rm[1], 0xDFFF);
}

TEST(JniString, MixedAsciiAndMultibyteRoundtrip) {
    // "tag:值:1" —— 混合 ASCII 与 3 字节中文
    auto s = bytes({'t', 'a', 'g', ':', 0xE5, 0x80, 0xBC, ':', '1'});
    auto r = utf8_to_utf16(s.data(), s.size());
    ASSERT_EQ(r.size(), 7u);
    EXPECT_EQ(to_u16(r), u"tag:\u503C:1");
}

TEST(JniString, OverlongEncodingsRejected) {
    // 过长 ASCII:C0 AF (/) —— C0 非法前缀拒收,AF 裸续字节拒收
    auto s1 = bytes({0xC0, 0xAF});
    auto r1 = utf8_to_utf16(s1.data(), s1.size());
    ASSERT_EQ(r1.size(), 2u);  // 每个非法字节各产出一个替换符
    EXPECT_EQ(r1[0], 0xFFFD);
    EXPECT_EQ(r1[1], 0xFFFD);

    // 过长 NUL 变体:E0 80 —— E0 低界越位(E0 要求第二字节 >= A0),
    // 解码器消费首字节重同步,后续 80 作为裸续字节再拒
    auto s2 = bytes({0xE0, 0x80});
    auto r2 = utf8_to_utf16(s2.data(), s2.size());
    ASSERT_EQ(r2.size(), 2u);
    EXPECT_EQ(r2[0], 0xFFFD);
    EXPECT_EQ(r2[1], 0xFFFD);

    // 过长 NUL:C1 80 → 拒绝(C1 属非法前缀,80 裸续字节)
    auto s3 = bytes({0xC1, 0x80});
    auto r3 = utf8_to_utf16(s3.data(), s3.size());
    ASSERT_EQ(r3.size(), 2u);
    EXPECT_EQ(r3[0], 0xFFFD);
    EXPECT_EQ(r3[1], 0xFFFD);
}

TEST(JniString, SurrogateAndOutOfRangeRejected) {
    // 代理区 U+D800 = ED A0 80 → ED 触发代理区低界(A0 < B0 需求),
    // 消费 ED 重同步;A0/80 作为裸续字节各产出替换符
    auto s1 = bytes({0xED, 0xA0, 0x80});
    auto r1 = utf8_to_utf16(s1.data(), s1.size());
    ASSERT_EQ(r1.size(), 3u);
    EXPECT_EQ(r1[0], 0xFFFD);
    EXPECT_EQ(r1[1], 0xFFFD);
    EXPECT_EQ(r1[2], 0xFFFD);

    // > U+10FFFF:F4 90 80 80 → F4 高界越位(90 > 8F),消费 F4 重同步,
    // 90/80/80 裸续字节逐一替换
    auto s2 = bytes({0xF4, 0x90, 0x80, 0x80});
    auto r2 = utf8_to_utf16(s2.data(), s2.size());
    ASSERT_EQ(r2.size(), 4u);
    EXPECT_EQ(r2[0], 0xFFFD);
    for (size_t i = 1; i < r2.size(); ++i) EXPECT_EQ(r2[i], 0xFFFD);
}

TEST(JniString, TruncatedAndGarbageResyncWithoutLoss) {
    // 截断:"中"(E4 B8 AD)丢尾字节 + 跟随合法 "a" → E4/B8 各产出替换符,
    // 'a' 不丢(消费 1 字节重同步策略,与核心 parse_records_stream 一致)
    auto s1 = bytes({0xE4, 0xB8, 'a'});
    auto r1 = utf8_to_utf16(s1.data(), s1.size());
    ASSERT_EQ(r1.size(), 3u);
    EXPECT_EQ(r1[0], 0xFFFD);
    EXPECT_EQ(r1[1], 0xFFFD);
    EXPECT_EQ(r1[2], u'a');

    // 非法前缀 0xFF + 合法 ASCII:逐字节滑动,后续不受影响
    auto s2 = bytes({0xFF, 'o', 'k'});
    auto r2 = utf8_to_utf16(s2.data(), s2.size());
    ASSERT_EQ(r2.size(), 3u);
    EXPECT_EQ(r2[0], 0xFFFD);
    EXPECT_EQ(r2[1], u'o');
    EXPECT_EQ(r2[2], u'k');

    // 裸续字节 80 后跟 2 字节合法序列
    auto s3 = bytes({0x80, 0xC3, 0xA9});
    auto r3 = utf8_to_utf16(s3.data(), s3.size());
    ASSERT_EQ(r3.size(), 2u);
    EXPECT_EQ(r3[0], 0xFFFD);
    EXPECT_EQ(r3[1], 0x00E9);  // é

    // 损坏中段不丢尾:"A" + F0 9F(截断 emoji) + "B" + "C"
    // F0 期待第二字节 >= 90,'B' 非法 → F0/9F 各产出替换符,B/C 完整保留
    auto s4 = bytes({'A', 0xF0, 0x9F, 'B', 'C'});
    auto r4 = utf8_to_utf16(s4.data(), s4.size());
    ASSERT_EQ(r4.size(), 5u);
    EXPECT_EQ(r4[0], u'A');
    EXPECT_EQ(r4[1], 0xFFFD);
    EXPECT_EQ(r4[2], 0xFFFD);
    EXPECT_EQ(r4[3], u'B');
    EXPECT_EQ(r4[4], u'C');
}

TEST(JniString, EmbeddedNulViaModifiedEncodingSurvives) {
    // JNI 场景:错误消息可能来自 snprintf 组装,内嵌 NUL 不应终止解码;
    // Modified UTF-8 的 C0 80 编码保证 NUL 可表达
    auto s = bytes({'x', 0xC0, 0x80, 'y'});
    auto r = utf8_to_utf16(s.data(), s.size());
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0], u'x');
    EXPECT_EQ(r[1], 0x0000);
    EXPECT_EQ(r[2], u'y');
}

// ---------------- CESU-8 → UTF-8 就地转码测试 (R40-01) ----------------

TEST(JniString, Cesu8AsciiAndEmpty) {
    std::string s;
    size_t n = cesu8_to_utf8_inplace(&s[0], 0);
    EXPECT_EQ(n, 0u);

    std::string ascii = "LogX_Tag_2026";
    size_t n_ascii = cesu8_to_utf8_inplace(&ascii[0], ascii.size());
    EXPECT_EQ(n_ascii, ascii.size());
    EXPECT_EQ(ascii, "LogX_Tag_2026");
}

TEST(JniString, Cesu8ModifiedNulToStandardNul) {
    // Modified UTF-8 NUL (C0 80) 就地转换为单个 0x00 字节
    auto s = bytes({'a', 0xC0, 0x80, 'b'});
    size_t n = cesu8_to_utf8_inplace(&s[0], s.size());
    EXPECT_EQ(n, 3u);
    s.resize(n);
    EXPECT_EQ(s[0], 'a');
    EXPECT_EQ(s[1], '\0');
    EXPECT_EQ(s[2], 'b');
}

TEST(JniString, Cesu8BmpChineseUnchanged) {
    // 3 字节 BMP 字符(中文等)在 CESU-8 与标准 UTF-8 下编码完全相同
    // "日志系统" = E6 97 A5 E5 BF 97 E7 B3 BB E7 BB 9F
    auto s = bytes({0xE6, 0x97, 0xA5, 0xE5, 0xBF, 0x97, 0xE7, 0xB3, 0xBB, 0xE7, 0xBB, 0x9F});
    size_t orig_len = s.size();
    size_t n = cesu8_to_utf8_inplace(&s[0], s.size());
    EXPECT_EQ(n, orig_len);
    EXPECT_EQ(s, bytes({0xE6, 0x97, 0xA5, 0xE5, 0xBF, 0x97, 0xE7, 0xB3, 0xBB, 0xE7, 0xBB, 0x9F}));
}

TEST(JniString, Cesu8EmojiSurrogatePairToUtf8) {
    // R40-01 核心回归用例:
    // 1) Emoji 😀 U+1F600:
    //    UTF-16 代理对: D83D DE00
    //    CESU-8 6 字节: ED A0 BD ED B8 80
    //    标准 UTF-8 4 字节: F0 9F 98 80
    auto s1 = bytes({0xED, 0xA0, 0xBD, 0xED, 0xB8, 0x80});
    size_t n1 = cesu8_to_utf8_inplace(&s1[0], s1.size());
    EXPECT_EQ(n1, 4u);
    s1.resize(n1);
    EXPECT_EQ(s1, bytes({0xF0, 0x9F, 0x98, 0x80}));

    // 2) Emoji 💩 U+1F4A9:
    //    UTF-16 代理对: D83D DCA9
    //    CESU-8 6 字节: ED A0 BD ED B2 A9
    //    标准 UTF-8 4 字节: F0 9F 92 A9
    auto s2 = bytes({0xED, 0xA0, 0xBD, 0xED, 0xB2, 0xA9});
    size_t n2 = cesu8_to_utf8_inplace(&s2[0], s2.size());
    EXPECT_EQ(n2, 4u);
    s2.resize(n2);
    EXPECT_EQ(s2, bytes({0xF0, 0x9F, 0x92, 0xA9}));

    // 3) 补充平面极小值 U+10000:
    //    UTF-16 代理对: D800 DC00
    //    CESU-8 6 字节: ED A0 80 ED B0 80
    //    标准 UTF-8 4 字节: F0 90 80 80
    auto s3 = bytes({0xED, 0xA0, 0x80, 0xED, 0xB0, 0x80});
    size_t n3 = cesu8_to_utf8_inplace(&s3[0], s3.size());
    EXPECT_EQ(n3, 4u);
    s3.resize(n3);
    EXPECT_EQ(s3, bytes({0xF0, 0x90, 0x80, 0x80}));

    // 4) Unicode 极大值 U+10FFFF:
    //    UTF-16 代理对: DBFF DFFF
    //    CESU-8 6 字节: ED AF BF ED BF BF
    //    标准 UTF-8 4 字节: F4 8F BF BF
    auto s4 = bytes({0xED, 0xAF, 0xBF, 0xED, 0xBF, 0xBF});
    size_t n4 = cesu8_to_utf8_inplace(&s4[0], s4.size());
    EXPECT_EQ(n4, 4u);
    s4.resize(n4);
    EXPECT_EQ(s4, bytes({0xF4, 0x8F, 0xBF, 0xBF}));
}

TEST(JniString, Cesu8MixedRoundtripWithUtf16) {
    // 构造混合串: "Tag_😀_日志_🎉"
    // 其中:
    //   "Tag_" (ASCII)
    //   😀: U+1F600 (D83D DE00) → CESU-8: ED A0 BD ED B8 80
    //   "_日志_" (ASCII + 中文)
    //   🎉: U+1F389 (D83C DF89) → CESU-8: ED A0 BC ED BE 89
    auto cesu8_str = bytes({
        'T', 'a', 'g', '_',
        0xED, 0xA0, 0xBD, 0xED, 0xB8, 0x80, // 😀
        '_', 0xE6, 0x97, 0xA5, 0xE5, 0xBF, 0x97, '_', // _日志_
        0xED, 0xA0, 0xBC, 0xED, 0xBE, 0x89  // 🎉
    });

    size_t n = cesu8_to_utf8_inplace(&cesu8_str[0], cesu8_str.size());
    cesu8_str.resize(n);

    // 标准 UTF-8 应包含 4 字节 emoji + 3 字节中文 + ASCII
    // 进一步喂入 utf8_to_utf16 解码器验证往返一致性
    auto u16 = utf8_to_utf16(cesu8_str.data(), cesu8_str.size());
    ASSERT_EQ(u16.size(), 4u + 2u + 1u + 2u + 1u + 2u); // 4('Tag_') + 2(😀) + 1('_') + 2('日志') + 1('_') + 2(🎉) = 12 uint16
    EXPECT_EQ(u16[4], 0xD83D);
    EXPECT_EQ(u16[5], 0xDE00);
    EXPECT_EQ(u16[7], 0x65E5); // 日
    EXPECT_EQ(u16[8], 0x5FD7); // 志
    EXPECT_EQ(u16[10], 0xD83C);
    EXPECT_EQ(u16[11], 0xDF89);
}

// ============================================================
// R41-03 / R42-02: JNI TLS 多槽密钥缓存属主与代数隔离测试 (防 ABA 假命中)
// ============================================================
TEST(JniKeyCache, OwnerAndGenerationIsolation) {
    logx_jni::TlsKeyCache<4> cache;
    int ctx_a = 1;
    int ctx_b = 2;

    uint8_t* k1_a = cache.allocate_slot(&ctx_a, /*gen=*/1, 7);
    ASSERT_NE(k1_a, nullptr);
    std::memset(k1_a, 0x11, 32);

    // 同 owner + 同 gen + 同 kid: 必须命中
    const uint8_t* hit_a = cache.lookup(&ctx_a, /*gen=*/1, 7);
    ASSERT_NE(hit_a, nullptr);
    EXPECT_EQ(hit_a[0], 0x11);
    EXPECT_EQ(hit_a, k1_a);

    // 不同 owner + 同 kid: 严禁跨实例命中 (R41-03)
    const uint8_t* miss_b = cache.lookup(&ctx_b, /*gen=*/1, 7);
    EXPECT_EQ(miss_b, nullptr);

    // R42-02: 同一内存地址 ctx_a 被重用 (ABA), 但代数 gen=2: 严禁假命中旧密钥!
    const uint8_t* miss_reused_addr = cache.lookup(&ctx_a, /*gen=*/2, 7);
    EXPECT_EQ(miss_reused_addr, nullptr);

    // 为 ctx_a (新代数 gen=2) 分配新密钥: 此时安全接管/复用
    uint8_t* k2_a = cache.allocate_slot(&ctx_a, /*gen=*/2, 7);
    ASSERT_NE(k2_a, nullptr);
    std::memset(k2_a, 0x33, 32);

    EXPECT_EQ(cache.lookup(&ctx_a, /*gen=*/2, 7)[0], 0x33);
    EXPECT_EQ(cache.lookup(&ctx_a, /*gen=*/1, 7), nullptr); // 旧代数已失效
}

// ============================================================
// R41-03 / R42-02: 属主与代数失效清理与密钥清零
// ============================================================
TEST(JniKeyCache, InvalidateOwnerAndZeroMemory) {
    logx_jni::TlsKeyCache<4> cache;
    int ctx_a = 1;
    int ctx_b = 2;

    uint8_t* k_a = cache.allocate_slot(&ctx_a, /*gen=*/10, 1);
    ASSERT_NE(k_a, nullptr);
    std::memset(k_a, 0xAA, 32);
    uint8_t* k_b = cache.allocate_slot(&ctx_b, /*gen=*/20, 2);
    ASSERT_NE(k_b, nullptr);
    std::memset(k_b, 0xBB, 32);

    // 释放 ctx_a (gen=10)
    cache.invalidate_owner(&ctx_a, /*gen=*/10);

    // ctx_a 失效且内存已被清零，槽位 kid 与元数据完全复位
    EXPECT_EQ(cache.lookup(&ctx_a, /*gen=*/10, 1), nullptr);
    EXPECT_EQ(cache.slot(0).kid, 0);
    EXPECT_FALSE(cache.slot(0).valid);
    for (int i = 0; i < 32; ++i) {
        EXPECT_EQ(k_a[i], 0);
    }

    // ctx_b 不受任何影响
    const uint8_t* hit_b = cache.lookup(&ctx_b, /*gen=*/20, 2);
    ASSERT_NE(hit_b, nullptr);
    EXPECT_EQ(hit_b[0], 0xBB);
}

// ============================================================
// R42-03 / ACT-3: 满槽容量耗尽显式返回 nullptr (严禁静默覆盖在持指针)
// ============================================================
TEST(JniKeyCache, FullCapacityReturnsNullptrAndSafeReuse) {
    logx_jni::TlsKeyCache<4> cache;
    int ctx = 1;
    uint64_t gen = 100;

    // 填满 4 槽
    for (uint16_t kid = 0; kid < 4; ++kid) {
        uint8_t* k = cache.allocate_slot(&ctx, gen, kid);
        ASSERT_NE(k, nullptr);
        k[0] = static_cast<uint8_t>(kid + 1);
    }
    for (uint16_t kid = 0; kid < 4; ++kid) {
        ASSERT_NE(cache.lookup(&ctx, gen, kid), nullptr);
        EXPECT_EQ(cache.lookup(&ctx, gen, kid)[0], kid + 1);
    }

    // 同一活跃 (ctx, gen) 请求第 5 个不同 kid: 槽位耗尽, 显式返回 nullptr (杜绝覆盖在持槽位)
    uint8_t* overflow_k = cache.allocate_slot(&ctx, gen, 99);
    EXPECT_EQ(overflow_k, nullptr);

    // 前 4 个槽位内容严禁被破坏
    for (uint16_t kid = 0; kid < 4; ++kid) {
        EXPECT_EQ(cache.lookup(&ctx, gen, kid)[0], kid + 1);
    }

    // 模拟新实例代数 gen=101 接入: 应当安全复用陈旧槽位
    uint8_t* new_gen_k = cache.allocate_slot(&ctx, /*gen=*/101, 55);
    ASSERT_NE(new_gen_k, nullptr);
    new_gen_k[0] = 0x88;
    EXPECT_EQ(cache.lookup(&ctx, /*gen=*/101, 55)[0], 0x88);
}

// ============================================================
// R43: 跨 owner 同 kid 并存时各自分配独立槽、双密钥互不干扰
// (恢复 R42 重构时删除的 OwnerIsolationAndHit 覆盖场景)
// ============================================================
TEST(JniKeyCache, CrossOwnerSameKidIndependentSlots) {
    logx_jni::TlsKeyCache<4> cache;
    int ctx_a = 1;
    int ctx_b = 2;

    uint8_t* k_a = cache.allocate_slot(&ctx_a, /*gen=*/1, 7);
    ASSERT_NE(k_a, nullptr);
    std::memset(k_a, 0x11, 32);

    // ctx_b 分配同 kid=7: 必须获得独立槽位, 不与 ctx_a 共享内存
    uint8_t* k_b = cache.allocate_slot(&ctx_b, /*gen=*/2, 7);
    ASSERT_NE(k_b, nullptr);
    std::memset(k_b, 0x22, 32);

    EXPECT_NE(k_a, k_b);
    EXPECT_EQ(cache.lookup(&ctx_a, /*gen=*/1, 7)[0], 0x11);
    EXPECT_EQ(cache.lookup(&ctx_b, /*gen=*/2, 7)[0], 0x22);

    // 一方失效不得影响另一方
    cache.invalidate_owner(&ctx_a, /*gen=*/1);
    EXPECT_EQ(cache.lookup(&ctx_a, /*gen=*/1, 7), nullptr);
    const uint8_t* hit_b = cache.lookup(&ctx_b, /*gen=*/2, 7);
    ASSERT_NE(hit_b, nullptr);
    EXPECT_EQ(hit_b[0], 0x22);
}

// ============================================================
// R43: 跨 owner 驱逐接管槽位时, 旧密钥字节必须先清零 (杜绝明文残留窗口)
// ============================================================
TEST(JniKeyCache, CrossOwnerEvictionZeroesEvictedKeyBeforeHandover) {
    logx_jni::TlsKeyCache<2> cache;
    int ctx_a = 1;
    int ctx_b = 2;

    // ctx_a 占满 2 槽并写入非零密钥
    uint8_t* k_a0 = cache.allocate_slot(&ctx_a, /*gen=*/1, 10);
    uint8_t* k_a1 = cache.allocate_slot(&ctx_a, /*gen=*/1, 11);
    ASSERT_NE(k_a0, nullptr);
    ASSERT_NE(k_a1, nullptr);
    std::memset(k_a0, 0xAA, 32);
    std::memset(k_a1, 0xBB, 32);

    // ctx_b 接入: 空槽已耗尽, 必须经步骤 2 跨 owner 驱逐一个 ctx_a 槽位
    uint8_t* k_b = cache.allocate_slot(&ctx_b, /*gen=*/2, 20);
    ASSERT_NE(k_b, nullptr);

    // 关键断言: 接管指针指向的槽位内旧密钥必须已被清零
    // (模拟真实时序——调用方拿到指针后、GetByteArrayRegion 覆写前的窗口)
    for (int i = 0; i < 32; ++i) {
        EXPECT_EQ(k_b[i], 0) << "byte " << i << " 残留被驱逐属主明文密钥";
    }

    // 驱逐只影响被接管槽: ctx_a 另一个槽位必须完好
    const bool a0_intact = cache.lookup(&ctx_a, /*gen=*/1, 10) != nullptr;
    const bool a1_intact = cache.lookup(&ctx_a, /*gen=*/1, 11) != nullptr;
    EXPECT_TRUE(a0_intact != a1_intact);  // 恰有一个幸存
    const uint8_t* survivor = a0_intact ? cache.lookup(&ctx_a, 1, 10)
                                        : cache.lookup(&ctx_a, 1, 11);
    EXPECT_EQ(survivor[0], a0_intact ? 0xAA : 0xBB);

    // 写入新密钥后三元匹配正常命中, 且与幸存旧槽互不干扰
    std::memset(k_b, 0xCC, 32);
    EXPECT_EQ(cache.lookup(&ctx_b, /*gen=*/2, 20)[0], 0xCC);
}

// ============================================================
// R45-12 / ACT-8: lookup 入口即清扫同 owner 异 gen 槽位，且支持按全局已销毁代数水位跨 owner 清零驱逐
// ============================================================
TEST(JniKeyCache, LookupEntryScrubsStaleAndDestroyedWatermarkSlots) {
    logx_jni::TlsKeyCache<4> cache;
    int ctx_a = 1;
    int ctx_b = 2;
    int ctx_c = 3;

    // 1. 验证同 owner 异 gen 在 lookup 入口即被清零驱逐（无需等到 allocate_slot）
    uint8_t* k_a1 = cache.allocate_slot(&ctx_a, /*gen=*/1, /*kid=*/7);
    ASSERT_NE(k_a1, nullptr);
    std::memset(k_a1, 0x5A, 32);
    ASSERT_TRUE(cache.slot(0).valid);

    // 模拟 ABA 地址复用后首次 lookup 失配（且假设随后 Kotlin 回调抛异常不走 allocate_slot）
    EXPECT_EQ(cache.lookup(&ctx_a, /*gen=*/2, /*kid=*/7), nullptr);
    EXPECT_FALSE(cache.slot(0).valid);
    for (int i = 0; i < 32; ++i) {
        EXPECT_EQ(cache.slot(0).key[i], 0) << "lookup 入口必须立即清零同 owner 异 gen 陈旧槽";
    }

    // 2. 验证跨线程 on_destroy 登记已销毁代数水位后，异 owner 在空槽未耗尽时也能在 lookup 入口清零旧实例残留槽
    std::atomic<uint64_t> watermark{0};
    uint8_t* k_old = cache.allocate_slot(&ctx_a, /*gen=*/3, /*kid=*/10);
    uint8_t* k_b_kid1 = cache.allocate_slot(&ctx_b, /*gen=*/4, /*kid=*/20);
    ASSERT_NE(k_old, nullptr);
    ASSERT_NE(k_b_kid1, nullptr);
    std::memset(k_old, 0xAA, 32);
    std::memset(k_b_kid1, 0xBB, 32);

    // 模拟实例 C (gen=5) 先于实例 B (gen=4) 乱序销毁，以及实例 A (gen=3) 在异线程销毁: 水位推进至 5
    logx_jni::advance_destroyed_watermark(watermark, 3);
    logx_jni::advance_destroyed_watermark(watermark, 5);
    logx_jni::advance_destroyed_watermark(watermark, 2); // 乱序小代数不得回退水位
    EXPECT_EQ(watermark.load(), 5u);

    // 存活实例 B (gen=4 <= watermark=5) 在本线程查询新 kid=21:
    // - 异 owner 的 ctx_a (gen=3 <= 5) 槽位必须被立即清零并驱逐；
    // - 当前调用方自身 ctx_b (gen=4 <= 5) 已缓存的 kid=20 槽位必须被豁免保留（闭合 J-6 单文件指针稳定规约）!
    EXPECT_EQ(cache.lookup(&ctx_b, /*gen=*/4, /*kid=*/21, watermark.load()), nullptr);
    EXPECT_FALSE(cache.slot(0).valid) << "异 owner 且 gen <= watermark 的槽位必须被驱逐";
    for (int i = 0; i < 32; ++i) {
        EXPECT_EQ(cache.slot(0).key[i], 0) << "异 owner 被水位驱逐时明文必须清零";
    }
    EXPECT_TRUE(cache.slot(1).valid) << "当前活跃调用方自身的已缓存槽位必须豁免保留";
    EXPECT_EQ(cache.slot(1).key[0], 0xBB);

    // 随后新实例 C2 (ctx_c, gen=6 > watermark=5) 在本线程 lookup 时，因 ctx_b 成为异 owner 且 gen=4 <= 5，也会被安全清零
    EXPECT_EQ(cache.lookup(&ctx_c, /*gen=*/6, /*kid=*/30, watermark.load()), nullptr);
    EXPECT_FALSE(cache.slot(1).valid);
    for (int i = 0; i < 32; ++i) {
        EXPECT_EQ(cache.slot(1).key[i], 0);
    }
}


