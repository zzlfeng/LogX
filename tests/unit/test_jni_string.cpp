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

#include <string>
#include <vector>

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
