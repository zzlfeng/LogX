// b5_probe2.cpp - B-12 方案 A 定性探针: 复刻 RetentionCleanup 创建循环的
// 「open(create) -> write -> close -> 紧邻 file_size_path」窗口, 每步采获原始证据.
//
// 关键事实 (B-12 立项依据): pal_win32.cpp file_size_path 失败路径直 return -1,
// 不消费 LastError/errno —— 调用后立即 GetLastError() 即 GetFileAttributesExA 原码:
//   2=ERROR_FILE_NOT_FOUND / 3=ERROR_PATH_NOT_FOUND  -> 文件真消失
//   32=ERROR_SHARING_VIOLATION                       -> AV/索引器瞬时独占
//   其余见 pal_win32.cpp win32_error_to_errno 契约
// 判读矩阵辅证: FindFirstFileA 枚举走目录视角, 与属性查询互相独立.
//
// 与全套件失败形态的关系: probe2 不含 logx 实例/Flusher —— 若 probe2 在同负载下命中,
// 触发因子为纯环境性 (文件创建 churn + AV 等); 若 probe2 长跑不命中而全套件命中,
// 则实例/Flusher 在场是必要条件, 探针需加实例层 (下一迭代).
//
// 退出码: 0=预算内未命中; 2=命中(现场保留: 目录不清理); 3=内部错误.
// 清理期 unlink 失败 (EBUSY=AV 句柄滞留信号) 记 WARN 计数, 不算命中.

#include <windows.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "logx_pal.h"

namespace {

std::string now_us() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.HighPart = ft.dwHighDateTime;
    u.LowPart = ft.dwLowDateTime;
    unsigned long long t100 = u.QuadPart - 116444736000000000ULL;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%llu.%06llu", t100 / 10000000ULL,
                  (t100 % 10000000ULL) / 10ULL);
    return buf;
}

struct Case {
    const char* name;
    const char* fname;
};

const Case kCases[] = {
    {"A", "20200101.log"},
    {"B", "20460830.log"},
    {"C", "20460903.log"},
    {"D", "20460910.log"},
};

// 命中法证: 仅非破坏性调用. 立即属性重查 + 目录视角枚举 + 全共享只读开 + 100ms 后复测.
void forensics(std::FILE* log, const std::string& dir, const std::string& fpath,
               int case_id, DWORD raw_le) {
    std::fprintf(log,
                 "  >>> HIT case=%s(%s) t=%s size_path=-1 raw_lasterror=%lu"
                 " (2/3=gone, 32=sharing_violation)\n",
                 kCases[case_id].name, kCases[case_id].fname, now_us().c_str(),
                 (unsigned long)raw_le);
    DWORD attr = GetFileAttributesA(fpath.c_str());
    std::fprintf(log, "  [F] attr_retry=0x%08lX lasterror=%lu\n", attr,
                 attr == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0UL);
    WIN32_FIND_DATAA ffd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &ffd);
    if (h != INVALID_HANDLE_VALUE) {
        std::fprintf(log, "  [F] dir_view=[");
        do {
            std::fprintf(log, "%s,", ffd.cFileName);
        } while (FindNextFileA(h, &ffd));
        FindClose(h);
        std::fprintf(log, "]\n");
    } else {
        std::fprintf(log, "  [F] dir_view_enum_fail lasterror=%lu\n", GetLastError());
    }
    HANDLE rf = CreateFileA(fpath.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (rf != INVALID_HANDLE_VALUE) {
        std::fprintf(log, "  [F] read_open=yes (no exclusive holder)\n");
        CloseHandle(rf);
    } else {
        std::fprintf(log, "  [F] read_open=no lasterror=%lu\n", GetLastError());
    }
    Sleep(100);
    std::fprintf(log, "  [F] after_100ms size_path=%lld attr=0x%08lX\n",
                 logx::default_pal().file_size_path(fpath.c_str()),
                 GetFileAttributesA(fpath.c_str()));
    std::fflush(log);
}

int one_round(std::FILE* log, int round, long* warns) {
    logx::Pal& pal = logx::default_pal();
    std::string dir = std::string("logx_test_b5p2_") + std::to_string(pal.pid()) + "_" +
                      std::to_string(pal.monotonic_ns() % 1000000) + "_" +
                      std::to_string(round);
    if (pal.file_mkdirs(dir.c_str()) != 0) {
        std::fprintf(log, "  round %d mkdirs_FAIL errno=%d\n", round, errno);
        return 3;
    }
    int hit = 0;
    std::string hit_path;
    int hit_case = -1;
    DWORD hit_le = 0;
    for (int i = 0; i < 4; ++i) {
        std::string p = dir + "/" + kCases[i].fname;
        int fd = pal.file_open(p.c_str(), true, false);
        if (fd < 0) {
            std::fprintf(log, "  round %d case=%s open_FAIL errno=%d\n", round,
                         kCases[i].name, errno);
            hit = 3;
            break;
        }
        if (pal.file_write(fd, "payload", 7) != 7) {
            std::fprintf(log, "  round %d case=%s write_FAIL errno=%d\n", round,
                         kCases[i].name, errno);
            pal.file_close(fd);
            hit = 3;
            break;
        }
        if (pal.file_close(fd) != 0) {
            std::fprintf(log, "  round %d case=%s close_FAIL errno=%d\n", round,
                         kCases[i].name, errno);
            hit = 3;
            break;
        }
        // B-12 观测窗口: close 后紧邻查询. 失败不消费 LastError, 立即采原码.
        long long sz = pal.file_size_path(p.c_str());
        if (sz < 0) {
            hit_le = GetLastError();
            hit = 2;
            hit_path = p;
            hit_case = i;
            break;
        }
    }
    if (hit == 2) {
        forensics(log, dir, hit_path, hit_case, hit_le);
    }
    // 清理 (命中轮跳过, 保留现场); unlink EBUSY 记 WARN (AV 句柄滞留信号)
    if (hit != 2) {
        for (int i = 0; i < 4; ++i) {
            std::string p = dir + "/" + kCases[i].fname;
            errno = 0;
            if (pal.file_unlink(p.c_str()) != 0) {
                std::fprintf(log, "  round %d WARN unlink=%s errno=%d(%s)\n", round,
                             kCases[i].fname, errno, std::strerror(errno));
                ++*warns;
            }
        }
        errno = 0;
        if (pal.file_rmdir(dir.c_str()) != 0) {
            std::fprintf(log, "  round %d WARN rmdir errno=%d(%s)\n", round, errno,
                         std::strerror(errno));
            ++*warns;
        }
    }
    return hit;
}

}  // namespace

int main(int argc, char** argv) {
    long rounds = (argc > 1) ? std::atol(argv[1]) : 1000;
    std::FILE* log = std::fopen("b5_probe2.log", "a");
    if (!log) {
        std::fprintf(stderr, "cannot open b5_probe2.log\n");
        return 3;
    }
    std::fprintf(log, "=== probe2 begin t=%s rounds=%ld\n", now_us().c_str(), rounds);
    std::fflush(log);
    long warns = 0;
    int rc = 0;
    long done = 0;
    for (long i = 0; i < rounds; ++i) {
        ++done;
        int r = one_round(log, static_cast<int>(i), &warns);
        if (r == 2) {
            std::fprintf(log, "=== HIT at round %ld, scene preserved\n", i);
            rc = 2;
            break;
        }
        if (r != 0) {
            rc = r;
            break;
        }
        if ((i + 1) % 200 == 0) {
            std::fprintf(log, "  heartbeat round %ld warns=%ld t=%s\n", i + 1, warns,
                         now_us().c_str());
            std::fflush(log);
        }
    }
    std::fprintf(log, "=== probe2 end rounds_done=%ld warns=%ld rc=%d\n", done, warns, rc);
    std::fclose(log);
    std::printf("rounds_requested=%ld rounds_done=%ld warns=%ld rc=%d\n", rounds, done, warns,
                rc);
    return rc;
}
