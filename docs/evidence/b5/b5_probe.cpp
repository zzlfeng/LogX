// b5_probe.cpp - B-5 前置协议增强探针 (R12 独立探针先例: 零仓库改动, cl.exe 手工编译)
//
// 复刻 tests/unit/test_robustness.cpp Robustness.PalRmdirErrnoDistinction_NotEmptyVsMissing
// 的完整序列 (相对路径, 11 步), 每步 PAL 调用后:
//   1. 立即捕获 errno (在任何诊断/输出调用之前)
//   2. 诊断层 (不参与判定, 只留证据): CWD / GetFileAttributesA(dir2) /
//      GetFileAttributesA(fpath) / FindFirstFileA 枚举 dir2 / µs 时戳
//
// 与被测用例的差异 (仅两处, 均为仪器必需):
//   - 目录前缀 logx_test_b5p_ (防与正式用例互撞), 名内附轮次号 (进程内循环唯一性)
//   - 命中预期偏离时保留现场不清理, 退出码 2; 正常轮次由序列自身完成清理
//
// 观测目标 (2026-09-13 单次观测的模式): close 成功后 rmdir(dir2) 却返回 0,
// 即 occupied.bin 的 NTFS 目录项在 close 与 rmdir 之间消失/未落地.
// 原始 GetLastError 已在 PAL translate_win32 内被消费; 法证阶段只做
// 非破坏性补采 (属性/枚举/只读打开), 不重发 RemoveDirectoryA/DeleteFileA
// (重发在"目录实际为空但句柄被占"边缘态会真删现场).
// errno -> Win32 错误类映射由 translate_win32 契约反推 (ENOTEMPTY<-ERROR_DIR_NOT_EMPTY,
// ENOENT<-ERROR_FILE_NOT_FOUND/PATH_NOT_FOUND, EACCES<-ERROR_ACCESS_DENIED).

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
    unsigned long long t100 = u.QuadPart - 116444736000000000ULL;  // 1601 -> 1970
    unsigned long long sec = t100 / 10000000ULL;
    unsigned long long us = (t100 % 10000000ULL) / 10ULL;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%llu.%06llu", sec, us);
    return buf;
}

long long qpc() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

// 诊断层. pal_ret/pal_errno 须在调用前已捕获; 本函数内的 Win32 调用不参与判定.
void diag(std::FILE* log, const char* step, long pal_ret, int pal_errno,
          const std::string& dir2, const std::string& fpath) {
    char cwd[MAX_PATH];
    DWORD cwd_len = GetCurrentDirectoryA(MAX_PATH, cwd);
    DWORD a_dir2 = GetFileAttributesA(dir2.c_str());
    DWORD a_file = GetFileAttributesA(fpath.c_str());
    std::fprintf(log, "  [%s] t=%s qpc=%lld ret=%ld errno=%d(%s) dir2=%s file=%s cwd=%.*s",
                 step, now_us().c_str(), qpc(), pal_ret, pal_errno, std::strerror(pal_errno),
                 a_dir2 == INVALID_FILE_ATTRIBUTES ? "GONE" : "present",
                 a_file == INVALID_FILE_ATTRIBUTES ? "GONE" : "present",
                 (int)cwd_len, cwd);
    if (a_dir2 != INVALID_FILE_ATTRIBUTES) {
        WIN32_FIND_DATAA ffd;
        HANDLE h = FindFirstFileA((dir2 + "\\*").c_str(), &ffd);
        if (h == INVALID_HANDLE_VALUE) {
            std::fprintf(log, " dir2_list=<ENUM_FAIL>");
        } else {
            std::fprintf(log, " dir2_list=[");
            do {
                std::fprintf(log, "%s,", ffd.cFileName);
            } while (FindNextFileA(h, &ffd));
            FindClose(h);
            std::fprintf(log, "]");
        }
    }
    std::fprintf(log, "\n");
}

// 法证: 仅非破坏性调用. 返回时日志已 flush.
void forensics(std::FILE* log, const std::string& dir2, const std::string& fpath) {
    std::fprintf(log, "  [FORENSICS] t=%s\n", now_us().c_str());
    WIN32_FIND_DATAA ffd;
    HANDLE h = FindFirstFileA(".\\*", &ffd);
    if (h != INVALID_HANDLE_VALUE) {
        std::fprintf(log, "  [FORENSICS] cwd_entries=[");
        do {
            std::fprintf(log, "%s,", ffd.cFileName);
        } while (FindNextFileA(h, &ffd));
        FindClose(h);
        std::fprintf(log, "]\n");
    }
    h = FindFirstFileA((dir2 + "\\*").c_str(), &ffd);
    if (h != INVALID_HANDLE_VALUE) {
        std::fprintf(log, "  [FORENSICS] dir2_entries=[");
        do {
            std::fprintf(log, "%s(size=%lu,attr=0x%08lX),", ffd.cFileName, ffd.nFileSizeLow,
                         ffd.dwFileAttributes);
        } while (FindNextFileA(h, &ffd));
        FindClose(h);
        std::fprintf(log, "]\n");
    } else {
        std::fprintf(log, "  [FORENSICS] dir2_enum_fail lasterror=%lu\n", GetLastError());
    }
    HANDLE rf = CreateFileA(fpath.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (rf != INVALID_HANDLE_VALUE) {
        std::fprintf(log, "  [FORENSICS] fpath readable=yes\n");
        CloseHandle(rf);
    } else {
        std::fprintf(log, "  [FORENSICS] fpath readable=no lasterror=%lu\n", GetLastError());
    }
    std::fflush(log);
}

int one_round(std::FILE* log, int round) {
    logx::Pal& pal = logx::default_pal();
    std::string dir = std::string("logx_test_b5p_") + std::to_string(pal.pid()) + "_" +
                      std::to_string(pal.monotonic_ns() % 1000000) + "_" +
                      std::to_string(round);
    std::string dir2 = dir + "_ne";
    std::string fpath = dir2 + "/occupied.bin";
    int r = 0;
    int e = 0;
    long w = 0;
    int fd = -1;

    errno = 0;
    r = pal.file_mkdirs(dir.c_str());
    e = errno;
    diag(log, "01_mkdirs_dir", r, e, dir2, fpath);
    if (r != 0) { forensics(log, dir2, fpath); return 1; }

    errno = 0;
    r = pal.file_rmdir(dir.c_str());
    e = errno;
    diag(log, "02_rmdir_dir_empty", r, e, dir2, fpath);
    if (r != 0) { forensics(log, dir2, fpath); return 1; }

    errno = 0;
    r = pal.file_rmdir(dir.c_str());
    e = errno;
    diag(log, "03_rmdir_dir_missing", r, e, dir2, fpath);
    if (r != -1 || e != ENOENT) { forensics(log, dir2, fpath); return 1; }

    errno = 0;
    r = pal.file_mkdirs(dir2.c_str());
    e = errno;
    diag(log, "04_mkdirs_dir2", r, e, dir2, fpath);
    if (r != 0) { forensics(log, dir2, fpath); return 1; }

    errno = 0;
    fd = pal.file_open(fpath.c_str(), true, false);
    e = errno;
    diag(log, "05_open_occupied", fd, e, dir2, fpath);
    if (fd < 0) { forensics(log, dir2, fpath); return 1; }

    errno = 0;
    w = pal.file_write(fd, "x", 1);
    e = errno;
    diag(log, "06_write_1b", w, e, dir2, fpath);
    if (w != 1) {
        pal.file_close(fd);
        forensics(log, dir2, fpath);
        return 1;
    }

    errno = 0;
    r = pal.file_close(fd);
    e = errno;
    diag(log, "07_close", r, e, dir2, fpath);
    if (r != 0) { forensics(log, dir2, fpath); return 1; }

    errno = 0;
    r = pal.file_rmdir(dir2.c_str());
    e = errno;
    diag(log, "08_rmdir_dir2_nonempty", r, e, dir2, fpath);
    if (r != -1 || e != ENOTEMPTY) { forensics(log, dir2, fpath); return 1; }

    errno = 0;
    r = pal.file_unlink(fpath.c_str());
    e = errno;
    diag(log, "09_unlink_occupied", r, e, dir2, fpath);
    if (r != 0) { forensics(log, dir2, fpath); return 1; }

    errno = 0;
    r = pal.file_unlink(fpath.c_str());
    e = errno;
    diag(log, "10_unlink_occupied_again", r, e, dir2, fpath);
    if (r != -1 || e != ENOENT) { forensics(log, dir2, fpath); return 1; }

    errno = 0;
    r = pal.file_rmdir(dir2.c_str());
    e = errno;
    diag(log, "11_rmdir_dir2_final", r, e, dir2, fpath);
    if (r != 0) { forensics(log, dir2, fpath); return 1; }

    std::fprintf(log, "  round %d PASS\n", round);
    std::fflush(log);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    int iters = (argc > 1) ? std::atoi(argv[1]) : 1;
    std::FILE* log = std::fopen("b5_probe.log", "a");
    if (!log) {
        std::fprintf(stderr, "cannot open b5_probe.log\n");
        return 3;
    }
    int hits = 0;
    int done = 0;
    for (int i = 0; i < iters; ++i) {
        std::fprintf(log, "=== round %d begin t=%s\n", i, now_us().c_str());
        std::fflush(log);
        ++done;
        if (one_round(log, i) != 0) {
            ++hits;
            std::fprintf(log, "=== HIT at round %d, evidence preserved (no cleanup)\n", i);
            break;
        }
    }
    std::fprintf(log, "=== summary iters_requested=%d rounds_done=%d hits=%d\n", iters, done,
                 hits);
    std::fclose(log);
    std::printf("iters_requested=%d rounds_done=%d hits=%d\n", iters, done, hits);
    return hits ? 2 : 0;
}
