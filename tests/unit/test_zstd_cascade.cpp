// D2 级联 smoke:剔除 Level 5+ 重型匹配器后,越界 level 必须由上游
// ZSTD_adjustCParams_internal() 级联降级,压缩/解压往返不得崩溃或报错。
#include <gtest/gtest.h>
#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

std::vector<char> make_payload(size_t n) {
    static const char kPattern[] =
        "logx zstd cascade smoke 0123456789 abcdefghijklmnopqrstuvwxyz "
        "block format staging flusher recovery export import ";
    std::vector<char> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i] = kPattern[i % (sizeof(kPattern) - 1)];
    }
    return v;
}

void roundtrip(size_t n, int level) {
    const std::vector<char> src = make_payload(n);
    std::vector<char> dst(ZSTD_compressBound(n));
    const size_t csize =
        ZSTD_compress(dst.data(), dst.size(), src.data(), src.size(), level);
    ASSERT_FALSE(ZSTD_isError(csize))
        << "level=" << level << " srcSize=" << n
        << " error=" << ZSTD_getErrorName(csize);

    std::vector<char> out(std::max<size_t>(n, 1));
    const size_t dsize = ZSTD_decompress(out.data(), n, dst.data(), csize);
    ASSERT_FALSE(ZSTD_isError(dsize))
        << "level=" << level << " srcSize=" << n
        << " error=" << ZSTD_getErrorName(dsize);
    ASSERT_EQ(dsize, n) << "level=" << level;
    if (n != 0) {
        EXPECT_EQ(0, std::memcmp(out.data(), src.data(), n)) << "level=" << level;
    }
}

}  // namespace

TEST(ZstdCascadeSmoke, AllLevelsOnLogxBlockSize) {
    ASSERT_GE(ZSTD_maxCLevel(), 22);
    for (int level = 0; level <= ZSTD_maxCLevel(); ++level) {
        SCOPED_TRACE(level);
        roundtrip(20491, level);
    }
}

TEST(ZstdCascadeSmoke, BoundarySizesAtKeyLevels) {
    const size_t sizes[] = {0, 1, 1024, 16384, 20491, 131072};
    const int levels[] = {0, 3, 4, 19, 22};
    for (const int level : levels) {
        for (const size_t n : sizes) {
            SCOPED_TRACE(::testing::Message() << "level=" << level << " size=" << n);
            roundtrip(n, level);
        }
    }
}
