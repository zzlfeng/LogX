// pal_win32.cpp - Windows(MSVC) PAL 实现。
//
// 与 pal_posix.cpp 语义对齐,按 spec §12"重写本文件即可适配 Windows":
//   - 文件句柄统一以 CRT fd(int) 形式暴露(_open_osfhandle),调用方无需感知 HANDLE
//   - mmap 用 CreateFileMapping + MapViewOfFile 实现(MAP_SHARED 语义),
//     msync(MS_SYNC) = FlushViewOfFile
//   - 事件 = 自动重置事件(signal_one / wait 语义与 posix 版一致)
//   - 互斥 = CRITICAL_SECTION;线程 = _beginthreadex;原子 = Interlocked*
//   - 时间:QueryPerformanceCounter / GetSystemTimeAsFileTime / GetLocalTime
//   - 随机:rand_s(系统 RNG)
//
// 已知限制:文件/目录 API 用 ANSI(A) 版本,路径需为当前代码页可表示字符;
// UTF-8 路径支持(转 W API)留待后续。

// rand_s 需在 include 任何 CRT 头之前定义 _CRT_RAND_S
#define _CRT_RAND_S

#include "logx_pal.h"

#include <windows.h>

#include <fcntl.h>   // _O_RDWR / _O_BINARY
#include <io.h>      // _open_osfhandle / _get_osfhandle / _chsize_s / _close / _write
#include <process.h> // _beginthreadex
#include <stdlib.h>  // rand_s

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace logx {

static_assert(sizeof(void*) == sizeof(intptr_t),
              "HANDLE roundtrip requires pointer-width storage");

namespace {

// P-01: Win32 HANDLE 为 64 位指针宽度(void*),必须经 intptr_t / void* 无损往返,
// 禁止窄化截断进 32 位整型类型。

class WinMutex {
public:
    WinMutex() { InitializeCriticalSection(&cs_); }
    ~WinMutex() { DeleteCriticalSection(&cs_); }
    void lock() { EnterCriticalSection(&cs_); }
    void unlock() { LeaveCriticalSection(&cs_); }
private:
    CRITICAL_SECTION cs_;
};

// 自动重置事件;wait 消费一次信号,与 posix Event 语义一致。
class Event {
public:
    Event() : h_(CreateEventA(nullptr, FALSE, FALSE, nullptr)) {}
    ~Event() { if (h_) CloseHandle(h_); }
    void signal_one() { if (h_) SetEvent(h_); }
    void wait() { if (h_) WaitForSingleObject(h_, INFINITE); }
    bool wait_timeout(uint32_t ms) {
        if (!h_) return false;
        return WaitForSingleObject(h_, ms) == WAIT_OBJECT_0;
    }
private:
    HANDLE h_;
};

class WinPal : public Pal {
public:
    // ---- 文件 ----
    int file_open(const char* path, bool create_if_missing, bool append) override {
        DWORD disp = create_if_missing ? OPEN_ALWAYS : OPEN_EXISTING;
        HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return -1;
        if (append) {
            // O_APPEND 语义:后续写从文件尾开始
            SetFilePointer(h, 0, nullptr, FILE_END);
        }
        int flags = _O_RDWR | _O_BINARY;
        if (append) flags |= _O_APPEND;
        int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), flags);
        if (fd < 0) { CloseHandle(h); return -1; }
        return fd;
    }
    int file_close(int fd) override { return _close(fd); }
    int file_truncate(int fd, uint64_t len) override {
        if (_chsize_s(fd, static_cast<__int64>(len)) != 0) return -1;
        return 0;
    }
    long file_write(int fd, const void* buf, size_t len) override {
        const uint8_t* p = static_cast<const uint8_t*>(buf);
        size_t total = 0;
        while (total < len) {
            unsigned int chunk = static_cast<unsigned int>(
                (len - total > 0x7fffffff) ? 0x7fffffff : (len - total));
            int n = _write(fd, p + total, chunk);
            if (n < 0) return -1;
            if (n == 0) return -1;  // 磁盘满等
            total += static_cast<size_t>(n);
        }
        return static_cast<long>(total);
    }
    long file_pwrite(int fd, const void* buf, size_t len, uint64_t offset) override {
        HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        if (h == INVALID_HANDLE_VALUE) return -1;
        const uint8_t* p = static_cast<const uint8_t*>(buf);
        size_t total = 0;
        while (total < len) {
            OVERLAPPED ov{};
            uint64_t off = offset + total;
            ov.Offset = static_cast<DWORD>(off & 0xffffffffu);
            ov.OffsetHigh = static_cast<DWORD>(off >> 32);
            DWORD chunk = static_cast<DWORD>(
                (len - total > 0x7fffffff) ? 0x7fffffff : (len - total));
            DWORD n = 0;
            if (!WriteFile(h, p + total, chunk, &n, &ov)) return -1;
            if (n == 0) return -1;
            total += n;
        }
        return static_cast<long>(total);
    }
    long file_pread(int fd, void* buf, size_t len, uint64_t offset) override {
        HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        if (h == INVALID_HANDLE_VALUE) return -1;
        uint8_t* p = static_cast<uint8_t*>(buf);
        size_t total = 0;
        while (total < len) {
            OVERLAPPED ov{};
            uint64_t off = offset + total;
            ov.Offset = static_cast<DWORD>(off & 0xffffffffu);
            ov.OffsetHigh = static_cast<DWORD>(off >> 32);
            DWORD chunk = static_cast<DWORD>(
                (len - total > 0x7fffffff) ? 0x7fffffff : (len - total));
            DWORD n = 0;
            // 文件读越界返回 TRUE 且 n=0(EOF)
            if (!ReadFile(h, p + total, chunk, &n, &ov)) return -1;
            if (n == 0) break;
            total += n;
        }
        return static_cast<long>(total);
    }
    int64_t file_size(int fd) override {
        HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        if (h == INVALID_HANDLE_VALUE) return -1;
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(h, &sz)) return -1;
        return static_cast<int64_t>(sz.QuadPart);
    }
    int64_t file_size_path(const char* path) override {
        HANDLE h = CreateFileA(path, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return -1;
        LARGE_INTEGER sz;
        BOOL ok = GetFileSizeEx(h, &sz);
        CloseHandle(h);
        return ok ? static_cast<int64_t>(sz.QuadPart) : -1;
    }
    int file_fsync(int fd) override {
        HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        if (h == INVALID_HANDLE_VALUE) return -1;
        return FlushFileBuffers(h) ? 0 : -1;
    }
    int file_dir_fsync(const char* dir_path) override {
        // 目录句柄 + FlushFileBuffers ≈ 目录项 fsync (需包含写入相关权限以合法执行 FlushFileBuffers)
        HANDLE h = CreateFileA(dir_path, FILE_LIST_DIRECTORY | FILE_ADD_FILE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return -1;
        BOOL ok = FlushFileBuffers(h);
        CloseHandle(h);
        return ok ? 0 : -1;
    }
    int file_unlink(const char* path) override {
        return DeleteFileA(path) ? 0 : -1;
    }
    int file_rmdir(const char* dir_path) override {
        return RemoveDirectoryA(dir_path) ? 0 : -1;
    }
    int file_mkdirs(const char* dir_path) override {
        std::string s(dir_path ? dir_path : "");
        if (s.empty()) return -1;
        while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
        if (s.empty()) return -1;
        // 识别根前缀:盘符(C:) 或根斜杠(/)
        std::string root;
        size_t pos = 0;
        if (s.size() >= 2 && s[1] == ':') { root = s.substr(0, 2); pos = 2; }
        else if (s[0] == '/' || s[0] == '\\') { root = "/"; pos = 1; }
        std::string acc = root;
        while (pos <= s.size()) {
            char c = pos < s.size() ? s[pos] : 0;
            if (c == '/' || c == '\\' || c == 0) {
                if (!acc.empty() && acc != root && acc.size() > root.size()) {
                    if (!CreateDirectoryA(acc.c_str(), nullptr) &&
                        GetLastError() != ERROR_ALREADY_EXISTS) {
                        return -1;
                    }
                }
            }
            if (c != 0) acc.push_back(c);
            ++pos;
        }
        return 0;
    }
    std::vector<std::string> file_list(const char* dir_path) override {
        std::vector<std::string> out;
        std::string pat = std::string(dir_path);
        if (!pat.empty() && pat.back() != '/' && pat.back() != '\\') pat.push_back('\\');
        pat += "*";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return out;
        do {
            std::string name = fd.cFileName;
            if (name == "." || name == "..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;  // 仅文件
            std::string full = std::string(dir_path);
            if (!full.empty() && full.back() != '/' && full.back() != '\\') full.push_back('/');
            full += name;
            out.push_back(full);  // 统一用 '/' 分隔,消费侧 find_last_of('/') 可解析
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        return out;
    }

    // ---- mmap ----
    PalMmap mmap_open(int fd, size_t len, bool writable) override {
        PalMmap m;
        m.out_handle = -1;
        m.len = len;
        HANDLE fh = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        if (fh == INVALID_HANDLE_VALUE) return m;
        DWORD protect = writable ? PAGE_READWRITE : PAGE_READONLY;
        HANDLE map = CreateFileMappingA(fh, nullptr, protect, 0, 0, nullptr);
        if (!map) return m;  // 文件必须已 ftruncate 到期望大小
        DWORD access = writable ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ;
        void* base = MapViewOfFile(map, access, 0, 0, 0);
        if (!base) { CloseHandle(map); return m; }
        m.base = base;
        m.out_handle = reinterpret_cast<intptr_t>(map);
        return m;
    }
    int mmap_sync(const PalMmap& m, bool synchronous) override {
        if (!m.base) return -1;
        if (!synchronous) return 0;  // MS_ASYNC:交系统异步回写
        if (!FlushViewOfFile(m.base, 0)) return -1;  // 0 = 映射全部
        return 0;
    }
    int mmap_close(const PalMmap& m) override {
        if (!m.base) return -1;
        BOOL ok1 = UnmapViewOfFile(m.base);
        HANDLE map = reinterpret_cast<HANDLE>(m.out_handle);
        BOOL ok2 = (map != nullptr && map != INVALID_HANDLE_VALUE) ? CloseHandle(map) : TRUE;
        return (ok1 && ok2) ? 0 : -1;
    }

    // ---- 互斥 / 事件 ----
    void* mutex_create() override { return new WinMutex(); }
    void  mutex_destroy(void* h) override { delete static_cast<WinMutex*>(h); }
    void  mutex_lock(void* h) override { static_cast<WinMutex*>(h)->lock(); }
    void  mutex_unlock(void* h) override { static_cast<WinMutex*>(h)->unlock(); }

    int threading_init() override {
        // Windows 线程不需要显式 init;返回 0。
        return 0;
    }

    struct ThreadCtx { void* (*fn)(void*); void* arg; };
    static unsigned __stdcall thread_trampoline(void* p) {
        ThreadCtx* tc = static_cast<ThreadCtx*>(p);
        void* (*fn)(void*) = tc->fn;
        void* arg = tc->arg;
        delete tc;
        fn(arg);
        return 0;
    }
    int thread_start(void* (*fn)(void*), void* arg, void** out_handle) override {
        ThreadCtx* tc = new ThreadCtx{fn, arg};
        uintptr_t h = _beginthreadex(nullptr, 0, &thread_trampoline, tc, 0, nullptr);
        if (h == 0) { delete tc; return -1; }
        *out_handle = reinterpret_cast<void*>(h);  // 线程句柄(需 CloseHandle 释放)
        return 0;
    }
    int thread_join(void* handle) override {
        if (!handle || handle == INVALID_HANDLE_VALUE) return -1;
        HANDLE h = reinterpret_cast<HANDLE>(handle);
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);  // 线程句柄一次性;join 后释放
        return 0;
    }

    void* ev_create() override { return new Event(); }
    void  ev_destroy(void* h) override { delete static_cast<Event*>(h); }
    void  ev_signal(void* h) override { static_cast<Event*>(h)->signal_one(); }
    void  ev_wait(void* h) override { static_cast<Event*>(h)->wait(); }
    bool  ev_wait_timeout(void* h, uint32_t ms) override {
        return static_cast<Event*>(h)->wait_timeout(ms);
    }

    // ---- 原子 ----
    uint64_t atomic_add(uint64_t* p, int64_t delta) override {
        return static_cast<uint64_t>(InterlockedExchangeAdd64(
            reinterpret_cast<volatile LONG64*>(p), static_cast<LONG64>(delta)));
    }
    uint64_t atomic_load(uint64_t* p) override {
        return static_cast<uint64_t>(InterlockedCompareExchange64(
            reinterpret_cast<volatile LONG64*>(p), 0, 0));
    }

    // ---- 时间 ----
    uint64_t monotonic_ns() override {
        LARGE_INTEGER freq, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        return static_cast<uint64_t>(now.QuadPart) * 1000000000ULL /
               static_cast<uint64_t>(freq.QuadPart);
    }
    uint64_t wallclock_ms() override {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        uint64_t ticks = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) |
                         static_cast<uint64_t>(ft.dwLowDateTime);
        // 1601-01-01 → 1970-01-01 的 100ns 间隔数
        constexpr uint64_t kEpochOffset = 116444736000000000ULL;
        return (ticks - kEpochOffset) / 10000ULL;
    }
    void wallclock_date(uint32_t* yyyymmdd, uint32_t* hhmmss) override {
        SYSTEMTIME st;
        GetLocalTime(&st);
        if (yyyymmdd) {
            *yyyymmdd = static_cast<uint32_t>(st.wYear) * 10000 +
                        static_cast<uint32_t>(st.wMonth) * 100 +
                        static_cast<uint32_t>(st.wDay);
        }
        if (hhmmss) {
            *hhmmss = static_cast<uint32_t>(st.wHour) * 10000 +
                      static_cast<uint32_t>(st.wMinute) * 100 +
                      static_cast<uint32_t>(st.wSecond);
        }
    }
    void sleep_ms(uint32_t ms) override { Sleep(ms); }

    uint32_t pid() override { return static_cast<uint32_t>(GetCurrentProcessId()); }
    uint64_t tid() override { return static_cast<uint64_t>(GetCurrentThreadId()); }

    void random_bytes(void* buf, size_t len) override {
        // rand_s 走系统 RNG(CryptGenRandom);失败退化为时间+地址填充。
        uint8_t* p = static_cast<uint8_t*>(buf);
        size_t off = 0;
        while (off < len) {
            unsigned int v = 0;
            if (rand_s(&v) != 0) break;
            size_t n = (len - off < 4) ? (len - off) : 4;
            std::memcpy(p + off, &v, n);
            off += n;
        }
        if (off < len) {
            uint64_t t = monotonic_ns();
            uint64_t addr = reinterpret_cast<uint64_t>(buf);
            for (size_t i = off; i < len; ++i) {
                p[i] = static_cast<uint8_t>((t >> (i % 8 * 8)) ^ (addr >> (i % 8 * 8)));
            }
        }
    }

    void debug_log(const char* s) override {
        if (!s) return;
        std::fprintf(stderr, "[logx] %s\n", s);
    }
};

}  // namespace

Pal& default_pal() {
    static WinPal inst;
    return inst;
}

}  // namespace logx
