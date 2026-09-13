// test_hotpath_alloc.cpp - "热路径零堆分配"契约的可回归断言。
//
// 背景:代码注释与 CHANGELOG 多处把「热路径零堆分配 / 零 GC 抖动」作为**结论**
// 陈述(logx_jni.cpp、CHANGELOG T5.5/T5.6 等),但既有测试没有任何分配计数,
// 该声明只能靠代码走查推断,缺可执行的守护。本文件把它变成可回归契约。
//
// 设计要点:
//   1) 在本 TU 替换全局 operator new / new[] 及其 delete,用 **thread_local**
//      计数器统计分配 —— 口径是"仅当前线程"。因此 Flusher 后台线程的分配
//      (压缩缓冲、路径拼接等)不会污染写入热路径的度量。未启用计数时所有请求
//      透传 std::malloc/std::free,行为与默认分配器等价。
//   2) thread_local POD(常量初始化)自身不触发堆分配,可作为零开销守卫。
//   3) 独立可执行目标(logx_hotpath_alloc_tests),不并入 logx_unit_tests ——
//      避免替换分配器改变其余 140+ 既有用例的运行环境。
//   4) 计数前先做**仪器自检**(故意分配一次并断言被捕获),防止替换失效导致
//      "零分配"假通过;每个计数窗口内还断言写入返回 LOGX_OK 并校验统计计数,
//      证明热路径真被执行(而非因参数非法早退)。
//
// 覆盖范围说明:仅替换非对齐 operator new / new[];C++17 对齐重载仍走默认实现,
// 不在计数口径内(core 热路径不做 over-aligned 分配)。

#include "logx_c.h"
#include "logx_internal.h"   // 传递包含 logx_pal.h
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>

using namespace logx;

// =========================================================================
// 全局分配探针
// =========================================================================
namespace {

// 线程局部计数状态:POD + 常量初始化 ⇒ 零堆分配、零动态初始化
thread_local bool     tl_probe_armed = false;
thread_local uint64_t tl_alloc_calls = 0;
thread_local uint64_t tl_alloc_bytes = 0;

// volatile 汇指针:阻断编译器把 new/delete 配对整体消除(allocator elision)
volatile void* g_probe_sink = nullptr;

inline void probe_account(std::size_t n) {
    if (tl_probe_armed) {
        ++tl_alloc_calls;
        tl_alloc_bytes += static_cast<uint64_t>(n);
    }
}

inline void* probe_malloc(std::size_t n) {
    return std::malloc(n ? n : 1);
}

void probe_arm() {
    tl_alloc_calls = 0;
    tl_alloc_bytes = 0;
    tl_probe_armed = true;
}
uint64_t probe_disarm() {
    tl_probe_armed = false;
    return tl_alloc_calls;
}
uint64_t probe_bytes() { return tl_alloc_bytes; }

// 仪器自检:确认计数确实生效。返回捕获到的分配次数(应为 >= 1)。
uint64_t probe_self_check() {
    probe_arm();
    {
        char* probe = new char[64];
        probe[0] = static_cast<char>(0x5A);
        g_probe_sink = probe;   // 逃逸 —— 阻断分配消除优化
        delete[] probe;
    }
    return probe_disarm();
}

std::atomic<uint64_t> s_hp_seq{0};

std::string make_dir(Pal& pal, const char* tag) {
    std::string dir = std::string("logx_test_") + tag + "_" +
                      std::to_string(pal.pid()) + "_" +
                      std::to_string(s_hp_seq.fetch_add(1));
    pal.file_mkdirs(dir.c_str());
    return dir;
}

void cleanup_dir(Pal& pal, const std::string& dir) {
    for (const auto& f : pal.file_list(dir.c_str())) {
        pal.file_unlink(f.c_str());
    }
    pal.file_rmdir(dir.c_str());
}

}  // namespace

// ---- 可替换全局分配函数(必须位于全局作用域) ----

void* operator new(std::size_t n) {
    probe_account(n);
    void* p = probe_malloc(n);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) {
    probe_account(n);
    void* p = probe_malloc(n);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    probe_account(n);
    return probe_malloc(n);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    probe_account(n);
    return probe_malloc(n);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

// =========================================================================
// 用例 1: 常规写入路径零堆分配
// =========================================================================
TEST(HotPathAlloc, LogxWrite_ZeroHeapAllocationOnCallerThread) {
    Pal& pal = default_pal();
    const std::string dir = make_dir(pal, "hpalloc");
    const std::string staging = dir + "/logx_staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 4096;
    cfg.encrypt_enable = 0;
    // UINT32_MAX = 生产默认 8KB 阈值自动唤醒。注意 logx_config_t 零初始化时该字段为 0,
    // 其语义是"禁用按阈值自动唤醒"(见 logx_open 的 E-06 注释);此处显式取默认值,
    // 以覆盖"唤醒 Flusher + ev_signal + 锁交接"的生产口径。
    cfg.flush_threshold = UINT32_MAX;

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    ASSERT_NE(inst, nullptr);

    // payload 必须在计数窗口外构造(std::string 自身会分配)
    const std::string payload(192, 'a');
    const uint8_t* pp = reinterpret_cast<const uint8_t*>(payload.data());
    const uint32_t plen = static_cast<uint32_t>(payload.size());

    // 预热:覆盖一次性懒初始化(TLS 块、Flusher 线程、内部缓冲),不计入窗口。
    // 每 256 条显式 flush,保证环(128KB)不溢出 —— 溢出会让 ASSERT 失败而非静默。
    for (int i = 0; i < 2000; ++i) {
        ASSERT_EQ(logx_write(inst, 3, "hp", pp, plen), LOGX_OK) << "warmup i=" << i;
        if ((i + 1) % 256 == 0) (void)logx_flush(inst);
    }
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    // ---- 仪器自检:探针必须能捕获分配 ----
    ASSERT_GE(probe_self_check(), 1u)
        << "分配计数探针未生效,后续\"零分配\"断言无意义";

    // ---- 计数窗口:仅 logx_write,无任何可能分配的旁路动作 ----
    // 400 × ~230B ≈ 92KB < 128KB 环容量,消除 kFull 早退噪声
    constexpr int kWindow = 400;
    int bad = 0;
    probe_arm();
    for (int i = 0; i < kWindow; ++i) {
        if (logx_write(inst, 3, "hp", pp, plen) != LOGX_OK) ++bad;
    }
    const uint64_t allocs = probe_disarm();
    const uint64_t bytes = probe_bytes();

    EXPECT_EQ(bad, 0) << "窗口内出现非 OK 返回,零分配结论无效(热路径未真正跑通)";

    printf("[alloc] 常规路径: logx_write × %d 条 → 调用线程堆分配 %llu 次 / %llu B\n",
           kWindow, (unsigned long long)allocs, (unsigned long long)bytes);

    EXPECT_EQ(allocs, 0u)
        << "热路径出现堆分配: " << allocs << " 次 / " << bytes << " B";

    logx_stats_t st{};
    logx_get_stats(inst, &st);
    EXPECT_EQ(st.truncated_logs, 0u);
    EXPECT_EQ(st.block_write_errors, 0u);

    ASSERT_EQ(logx_close(inst), LOGX_OK);
    cleanup_dir(pal, dir);
}

// =========================================================================
// 用例 2: 超长截断路径零堆分配(B-1B/T5.5/T5.6 新增分支)
// =========================================================================
// 截断路径要跑 solve_truncated_msg_budget + TLS trunc_buf 拷贝 + snprintf
// marker,是与常规路径不同的代码分支,须单独固化"零堆分配"契约。
TEST(HotPathAlloc, TruncatedPath_ZeroHeapAllocationOnCallerThread) {
    Pal& pal = default_pal();
    const std::string dir = make_dir(pal, "hpalloc_tr");
    const std::string staging = dir + "/logx_staging.mmap";

    logx_config_t cfg{};
    cfg.log_dir = dir.c_str();
    cfg.staging_path = staging.c_str();
    cfg.max_log_length = 512;    // 强制截断(payload 4096 > 512)
    cfg.encrypt_enable = 0;
    cfg.flush_threshold = UINT32_MAX;   // 生产默认 8KB 自动唤醒

    logx_instance_t* inst = nullptr;
    ASSERT_EQ(logx_open(&cfg, &inst), LOGX_OK);
    ASSERT_NE(inst, nullptr);

    const std::string payload(4096, 'b');
    const uint8_t* pp = reinterpret_cast<const uint8_t*>(payload.data());
    const uint32_t plen = static_cast<uint32_t>(payload.size());

    for (int i = 0; i < 200; ++i) {
        ASSERT_EQ(logx_write(inst, 3, "tr", pp, plen), LOGX_OK) << "warmup i=" << i;
        if ((i + 1) % 64 == 0) (void)logx_flush(inst);
    }
    ASSERT_EQ(logx_flush(inst), LOGX_OK);

    // 前提校验:截断路径确实被触发,否则本用例不成立
    logx_stats_t st0{};
    logx_get_stats(inst, &st0);
    ASSERT_GT(st0.truncated_logs, 0u) << "截断路径未被触发,用例前提不成立";

    ASSERT_GE(probe_self_check(), 1u)
        << "分配计数探针未生效,后续\"零分配\"断言无意义";

    // 100 × ~512B ≈ 51KB < 128KB 环容量,消除 kFull 早退噪声
    constexpr int kWindow = 100;
    int bad = 0;
    probe_arm();
    for (int i = 0; i < kWindow; ++i) {
        if (logx_write(inst, 3, "tr", pp, plen) != LOGX_OK) ++bad;
    }
    const uint64_t allocs = probe_disarm();
    const uint64_t bytes = probe_bytes();

    EXPECT_EQ(bad, 0) << "窗口内出现非 OK 返回,零分配结论无效";

    printf("[alloc] 截断路径: logx_write × %d 条 → 调用线程堆分配 %llu 次 / %llu B\n",
           kWindow, (unsigned long long)allocs, (unsigned long long)bytes);

    EXPECT_EQ(allocs, 0u)
        << "截断热路径出现堆分配: " << allocs << " 次 / " << bytes << " B";

    logx_stats_t st{};
    logx_get_stats(inst, &st);
    EXPECT_GE(st.truncated_logs, st0.truncated_logs + kWindow)
        << "窗口内截断计数未按预期增长(路径未真正跑通)";

    ASSERT_EQ(logx_close(inst), LOGX_OK);
    cleanup_dir(pal, dir);
}

}  // namespace
