// jni_string_util.h - JNI 字符串构造工具:UTF-8 → UTF-16 解码。
//
// J-03: NewStringUTF 期望 Modified UTF-8(CESU-8 变体),对含非 ASCII 的
// 标准 UTF-8 输入(尤其 4 字节补充平面字符、嵌入 NUL 等)在个别 JVM 实现
// 上可能触发编码异常。统一策略:先解码为 UTF-16 码元序列,再经
// NewString(env) 构造 jstring,语义与 JVM 字符串原生表示对齐。
//
// 解码器为纯逻辑(无 JNI 依赖),供桌面单测直接覆盖;
// logx_jni.cpp 经 make_jstring_utf16() 消费。
#ifndef LOGX_JNI_STRING_UTIL_H
#define LOGX_JNI_STRING_UTIL_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace logx_jni {

// 标准 UTF-8 → UTF-16 码元序列(严格模式):
//   - 1~3 字节序列 → 单个 UTF-16 码元;4 字节序列 → 高低位代理对
//   - Modified UTF-8 的 NUL 编码(C0 80)解码为 U+0000
//   - 按 RFC 3629 拒绝:过长编码(C0/C1 前缀、E0/F0 的低界越位)、
//     代理区码点(ED A0..BF)、越出 U+10FFFF(F4 90..)
//   - 非法前缀 / 非续字节 / 序列截断 → 单个 U+FFFD 替换符并滑动 1 字节
//     重同步(与核心 parse_records_stream 的容灾策略一致),绝不崩溃
inline std::vector<uint16_t> utf8_to_utf16(const char* s, size_t len) {
    std::vector<uint16_t> out;
    out.reserve(len);
    const auto* p = reinterpret_cast<const uint8_t*>(s);
    const auto* end = p + len;
    while (p < end) {
        uint8_t b0 = *p;
        // Modified UTF-8 NUL:C0 80 → U+0000
        if (b0 == 0xC0 && p + 1 < end && p[1] == 0x80) {
            out.push_back(0);
            p += 2;
            continue;
        }
        int n = 0;          // 序列总字节数
        uint8_t c1_min = 0x80, c1_max = 0xBF;  // 第二字节合法区间(默认:续字节)
        uint32_t cp = 0;
        if (b0 < 0x80) {
            out.push_back(b0);
            ++p;
            continue;
        } else if (b0 >= 0xC2 && b0 <= 0xDF) {
            n = 2; cp = b0 & 0x1Fu;             // C2..DF:无过长编码空间
        } else if (b0 >= 0xE0 && b0 <= 0xEF) {
            n = 3; cp = b0 & 0x0Fu;
            if (b0 == 0xE0) c1_min = 0xA0;      // 拒绝过长(E0 80..9F)
            else if (b0 == 0xED) c1_max = 0x9F; // 拒绝代理区(ED A0..BF)
        } else if (b0 >= 0xF0 && b0 <= 0xF4) {
            n = 4; cp = b0 & 0x07u;
            if (b0 == 0xF0) c1_min = 0x90;      // 拒绝过长(F0 80..8F)
            else if (b0 == 0xF4) c1_max = 0x8F; // 拒绝越界(> U+10FFFF)
        }
        if (n == 0) {  // C0/C1/F5..FF 非法前缀,或 80..BF 裸续字节
            out.push_back(0xFFFD);
            ++p;
            continue;
        }
        bool ok = true;
        for (int i = 1; i < n; ++i) {
            if (p + i >= end) { ok = false; break; }            // 序列截断
            uint8_t c = p[i];
            if (c < 0x80 || c > 0xBF) { ok = false; break; }    // 非续字节
            uint8_t lo = (i == 1) ? c1_min : 0x80;
            uint8_t hi = (i == 1) ? c1_max : 0xBF;
            if (c < lo || c > hi) { ok = false; break; }        // 越位(过长/代理区/越界)
            cp = (cp << 6) | (c & 0x3Fu);
        }
        if (!ok) {
            out.push_back(0xFFFD);  // 只消费首字节,从下一字节重同步
            ++p;
            continue;
        }
        p += n;
        if (n == 4) {
            cp -= 0x10000;
            out.push_back(static_cast<uint16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<uint16_t>(cp));
        }
    }
    return out;
}

}  // namespace logx_jni

#endif /* LOGX_JNI_STRING_UTIL_H */
