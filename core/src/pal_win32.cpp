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
// 路径编码:对外接收 UTF-8(const char*),内部统一转宽字符走 W 系 API
// (CreateFileW / DeleteFileW / FindFirstFileW / CreateDirectoryW 等),
// 支持任意 Unicode 路径(含中文/emoji),不再受当前 ANSI 代码页限制(P-1)。

// rand_s 需在 include 任何 CRT 头之前定义 _CRT_RAND_S
#define _CRT_RAND_S

#include "logx_pal.h"

#include <windows.h>

#include <fcntl.h>   // _O_RDWR / _O_BINARY
#include <io.h>      // _open_osfhandle / _get_osfhandle / _chsize_s / _close / _write
#include <process.h> // _beginthreadex
#include <stdlib.h>  // rand_s

#include <cassert>
#include <cerrno>
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

// P-1: 文件/目录 API 统一走宽字符(W)变体——路径以 UTF-8 入参, 先经
// MultiByteToWideChar(CP_UTF8) 转 UTF-16, 彻底移除 ANSI(A) 变体对"当前代码页
// 可表示字符"的限制(中文/emoji/任意 Unicode 路径不再乱码或打不开)。
// 编码非法(截断的多字节序列)时返回空串, 调用方判空即失败。
static std::wstring utf8_to_wide(const char* utf8) {
    if (!utf8 || !*utf8) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    // 注意: n 含结尾 NUL, 缓冲区须按 n 分配再截掉末尾 NUL(否则第二次调用写 N 个
    // 宽字符时越界 1 个 wchar_t)。
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, &w[0], n);
    w.resize(static_cast<size_t>(n - 1));
    return w;
}

static std::string wide_to_utf8(const wchar_t* w, int len) {
    if (!w || len <= 0) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, &s[0], n, nullptr, nullptr);
    return s;
}

// P-03: Win32 错误 → errno 转译,file_unlink/file_rmdir 失败时供调用方区分
// "路径不存在"(ENOENT)与"目录非空"(ENOTEMPTY)等语义,而非一律 -1。
static int win32_error_to_errno(DWORD e) {
    switch (e) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return ENOENT;
        case ERROR_DIR_NOT_EMPTY:
            return ENOTEMPTY;
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return EBUSY;
        case ERROR_ACCESS_DENIED:
            return EACCES;
        case ERROR_WRITE_PROTECT:
            return EROFS;
        case ERROR_NOT_READY:
            return EIO;
        default:
            return EIO;  // 未知错误:保守归入通用 IO 错误,而非丢失错误态
    }
}

// P-03: 失败时置 errno 并统一返回 -1;成功返回 0。
static int translate_win32(BOOL ok) {
    if (ok) return 0;
    errno = win32_error_to_errno(GetLastError());
    return -1;
}

// P-2 拍板落地: 统一为非递归互斥锁——将原先可重入的 CRITICAL_SECTION 替换为
// Windows 原生非递归的 SRWLOCK(Slim Reader/Writer Lock 独占模式)。
// 性能更高、内存开销更小且与 POSIX 端的 std::mutex 语义 100% 对齐,
// 消除"持锁再取锁在 Windows 绿灯而 Android 死锁"的跨平台分歧。
class WinMutex {
public:
    WinMutex() { InitializeSRWLock(&srw_); }
    ~WinMutex() = default;
    void lock() { AcquireSRWLockExclusive(&srw_); }
    void unlock() { ReleaseSRWLockExclusive(&srw_); }
private:
    SRWLOCK srw_ = SRWLOCK_INIT;
};

// 自动重置事件;wait 消费一次信号,与 posix Event 语义一致。
class Event {
public:
    Event() : h_(CreateEventA(nullptr, FALSE, FALSE, nullptr)) {}
    ~Event() { if (h_) CloseHandle(h_); }
    bool valid() const { return h_ != nullptr; }
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
        std::wstring wp = utf8_to_wide(path);
        if (wp.empty()) { errno = ENOENT; return -1; }
        DWORD disp = create_if_missing ? OPEN_ALWAYS : OPEN_EXISTING;
        HANDLE h = CreateFileW(wp.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            // P-16: file_open 失败也转译 errno(原先仅 unlink/rmdir 转译, 上层按
            // errno 诊断在 Windows 上会拿到陈旧值)。utf8 非法序列 → ENOENT。
            errno = win32_error_to_errno(GetLastError());
            return -1;
        }
        // P-10: 注释修正——MSVC CRT 的 _O_APPEND 是"用户态 seek+write", 并非内核级
        // 原子追加; 追加语义的正确性由 core 的"文件单写者"不变量(F-13 前提)兜底,
        // 此处不再 SetFilePointer(FILE_END), 免冗余且避免误导维护者。
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
        std::wstring wp = utf8_to_wide(path);
        // P-02: 改用 GetFileAttributesExW 读取 WIN32_FILE_ATTRIBUTE_DATA,
        // 不再为一次大小查询付出 CreateFile/CloseHandle 的内核句柄往返;
        // 该路径在 retention 扫盘与快照统计中高频调用。
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (wp.empty() || !GetFileAttributesExW(wp.c_str(), GetFileExInfoStandard, &fad)) return -1;
        // 64 位无符号拼接高/低 32 位，杜绝有符号移位与按位或的静态检查歧义
        uint64_t sz = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) |
                      static_cast<uint64_t>(fad.nFileSizeLow);
        return static_cast<int64_t>(sz);
    }
    int file_fsync(int fd) override {
        HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        if (h == INVALID_HANDLE_VALUE) return -1;
        // P-8: FlushFileBuffers 语义重于 fdatasync(数据+元数据同刷)——Windows 无
        // fdatasync 等价物, 用其替代并接受"只重不轻"。
        return FlushFileBuffers(h) ? 0 : -1;
    }
    int file_dir_fsync(const char* dir_path) override {
        // P-7: 与 POSIX 侧不对称——Windows 目录 fsync 需 FILE_ADD_FILE 写权限,
        // 只读目录会返 -1; POSIX 侧 O_RDONLY 即可。调用方(retention)接受该失败。
        std::wstring wp = utf8_to_wide(dir_path);
        HANDLE h = CreateFileW(wp.c_str(), FILE_LIST_DIRECTORY | FILE_ADD_FILE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return -1;
        BOOL ok = FlushFileBuffers(h);
        CloseHandle(h);
        return ok ? 0 : -1;
    }
    int file_unlink(const char* path) override {
        std::wstring wp = utf8_to_wide(path);
        // P-03: 失败转译 errno(ENOENT/EBUSY/EACCES...),调用方语义与 POSIX 对齐
        return translate_win32(DeleteFileW(wp.c_str()));
    }
    int file_rmdir(const char* dir_path) override {
        std::wstring wp = utf8_to_wide(dir_path);
        // P-03: 失败转译 errno;目录非空 → ENOTEMPTY,不存在 → ENOENT
        return translate_win32(RemoveDirectoryW(wp.c_str()));
    }
    int file_mkdirs(const char* dir_path) override {
        std::string s(dir_path ? dir_path : "");
        if (s.empty()) return -1;
        while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
        if (s.empty()) return -1;
        // 识别根前缀:盘符(C:) 或根斜杠(/)
        // P-15: 已知限制——UNC 前缀(\\server\share)与盘符相对路径(C:foo)未特殊
        // 处理: 前者会把 "\\server" 误当组件交给 CreateDirectory 报错, 后者按
        // 相对路径处理。core 当前传入的均为本地绝对目录, 不触发这两类; 备注留档。
        std::string root;
        size_t pos = 0;
        if (s.size() >= 2 && s[1] == ':') { root = s.substr(0, 2); pos = 2; }
        else if (s[0] == '/' || s[0] == '\\') { root = "/"; pos = 1; }
        std::string acc = root;
        while (pos <= s.size()) {
            char c = pos < s.size() ? s[pos] : 0;
            if (c == '/' || c == '\\' || c == 0) {
                if (!acc.empty() && acc != root && acc.size() > root.size()) {
                    std::wstring accW = utf8_to_wide(acc.c_str());
                    if (accW.empty()) return -1;
                    if (!CreateDirectoryW(accW.c_str(), nullptr)) {
                        DWORD e = GetLastError();
                        if (e != ERROR_ALREADY_EXISTS) return -1;
                        // P-14: ERROR_ALREADY_EXISTS 不区分"同名目录/同名文件"——
                        // 同名普通文件存在时原实现静默当成功, 失败推迟到后续 open
                        // 且难诊断。此处校验属性, 非目录即失败。
                        DWORD attrs = GetFileAttributesW(accW.c_str());
                        if (attrs == INVALID_FILE_ATTRIBUTES ||
                            !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return -1;
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
        std::wstring wdir = utf8_to_wide(dir_path);
        if (wdir.empty()) return out;
        std::wstring pat = wdir;
        if (!pat.empty() && pat.back() != L'/' && pat.back() != L'\\') pat.push_back(L'\\');
        pat += L"*";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return out;
        do {
            std::wstring nameW = fd.cFileName;
            if (nameW == L"." || nameW == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;  // 仅文件
            std::string name = wide_to_utf8(nameW.c_str(), static_cast<int>(nameW.size()));
            std::string full = std::string(dir_path);
            if (!full.empty() && full.back() != '/' && full.back() != '\\') full.push_back('/');
            full += name;
            out.push_back(full);  // 统一用 '/' 分隔,消费侧 find_last_of('/') 可解析
        } while (FindNextFileW(h, &fd));
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
        // P-5: 校验文件大小与 len 一致——CreateFileMapping(0,0)+MapViewOfFile(0)
        // 映射的是"文件当前全尺寸", 与传入 len 无关: 文件 < len 时视图小于 m.len,
        // core 按 len 写视图会越界触发 EXCEPTION_IN_PAGE_ERROR(POSIX 等价 SIGBUS)。
        // 返回失败让调用方(Staging::open)走堆降级, 不再"靠调用方纪律"。
        // 映射建立后同样不得再 truncate 该 fd——既有视图会失效/悬空(POSIX 无此坑)。
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(fh, &sz) ||
            sz.QuadPart < 0 || static_cast<uint64_t>(sz.QuadPart) != static_cast<uint64_t>(len)) {
            return m;
        }
        DWORD protect = writable ? PAGE_READWRITE : PAGE_READONLY;
        HANDLE map = CreateFileMappingA(fh, nullptr, protect, 0, 0, nullptr);
        if (!map) return m;
        DWORD access = writable ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ;
        void* base = MapViewOfFile(map, access, 0, 0, 0);
        if (!base) { CloseHandle(map); return m; }
        m.base = base;
        m.out_handle = reinterpret_cast<intptr_t>(map);
        return m;
    }
    int mmap_sync(const PalMmap& m, bool synchronous) override {
        if (!m.base) return -1;
        // P-21: synchronous=false(MS_ASYNC)为静默无操作——Windows 无异步 flush
        // 原语, 返回 0 表示"已交给系统"; 核心当前仅以 synchronous=true 调用。
        if (!synchronous) return 0;
        // P-22: FlushViewOfFile 只保证刷到系统文件缓存, 不等同 msync(MS_SYNC)的
        // "刷到物理盘"; 调用点(durability_sync)已配对 file_fsync 补偿, 见头契约。
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
        // P-9: WAIT_FAILED/ABANDONED 必须区分——原实现恒返回 0, 线程被强制终止
        // (TerminateThread)或句柄异常也会被当成 join 成功, 上层无从发现。
        DWORD rc = WaitForSingleObject(h, INFINITE);
        CloseHandle(h);  // 线程句柄一次性;join 后释放
        return (rc == WAIT_OBJECT_0) ? 0 : -1;
    }
    int thread_detach(void* handle) override {
        if (!handle || handle == INVALID_HANDLE_VALUE) return -1;
        HANDLE h = reinterpret_cast<HANDLE>(handle);
        return CloseHandle(h) ? 0 : -1;
    }

    void* ev_create() override {
        // P-6: CreateEventA 失败必须显式上报(nullptr)——否则 wait() 立即返回 /
        // wait_timeout() 恒 false, Flusher 等待循环退化为满 CPU 自旋且无任何
        // 错误信号。调用方(logx_open)已判空并整体失败。
        Event* e = new Event();
        if (!e->valid()) { delete e; return nullptr; }
        return e;
    }
    void  ev_destroy(void* h) override { delete static_cast<Event*>(h); }
    void  ev_signal(void* h) override { static_cast<Event*>(h)->signal_one(); }
    void  ev_wait(void* h) override { static_cast<Event*>(h)->wait(); }
    bool  ev_wait_timeout(void* h, uint32_t ms) override {
        return static_cast<Event*>(h)->wait_timeout(ms);
    }

    // ---- 原子 ----
    uint64_t atomic_add(uint64_t* p, int64_t delta) override {
        // P-06: Interlocked*64 家族要求目标 8 字节对齐,错位访问会触发硬件异常;
        // Debug 构建断言拦截,Release 依赖调用方(Instance 内计数器均为独立对齐成员)。
        assert((reinterpret_cast<uintptr_t>(p) & 7) == 0 &&
               "atomic_add64 requires 8-byte alignment");
        return static_cast<uint64_t>(InterlockedExchangeAdd64(
            reinterpret_cast<volatile LONG64*>(p), static_cast<LONG64>(delta)));
    }
    uint64_t atomic_load(uint64_t* p) override {
        assert((reinterpret_cast<uintptr_t>(p) & 7) == 0 &&
               "atomic_load64 requires 8-byte alignment");
        return static_cast<uint64_t>(InterlockedCompareExchange64(
            reinterpret_cast<volatile LONG64*>(p), 0, 0));
    }

    // ---- 时间 ----
    uint64_t monotonic_ns() override {
        // P-04: QPF 在系统运行期固定不变,仅进程内首次调用查询一次并缓存
        // (C++11 magic static 保证线程安全),省去每次调用的高频内核查询。
        static const uint64_t freq = [] {
            LARGE_INTEGER f;
            // P-12: QueryPerformanceFrequency 失败/返回 0 时兜底为 1, 避免后续
            // 除零(实际几乎不可能失败); 该常数在系统运行期固定不变。
            if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) return 1ULL;
            return static_cast<uint64_t>(f.QuadPart);
        }();
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        // 先除后乘:旧实现 now*1e9 在 QPC=10MHz 时开机约 31 分钟即溢出 uint64
        // 回绕,单调时钟跳变会让耗时轮询永假;拆分为整秒 + 余数两段消除溢出
        // (余数 < freq,余数*1e9 距 2^64 上限有数倍裕度)。
        uint64_t ticks = static_cast<uint64_t>(now.QuadPart);
        return (ticks / freq) * 1000000000ULL + (ticks % freq) * 1000000000ULL / freq;
    }
    uint64_t wallclock_ms() override {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        uint64_t ticks = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) |
                         static_cast<uint64_t>(ft.dwLowDateTime);
        // 1601-01-01 → 1970-01-01 的 100ns 间隔数
        constexpr uint64_t kEpochOffset = 116444736000000000ULL;
        // P-13: 系统时间早于 1970 时 ticks < kEpochOffset, 减法会无符号回绕成巨值;
        // clamp 0 保底(时间戳仅用于排序/过滤, 无意义时间不放大成无限远期)。
        return (ticks >= kEpochOffset) ? ((ticks - kEpochOffset) / 10000ULL) : 0;
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

    bool random_bytes(void* buf, size_t len) override {
        // rand_s 走系统 RNG (CryptGenRandom/RtlGenRandom)。
        if (!buf && len > 0) return false;
        uint8_t* p = static_cast<uint8_t*>(buf);
        size_t off = 0;
        while (off < len) {
            unsigned int v = 0;
            if (rand_s(&v) != 0) {
                std::memset(buf, 0, len);
                return false;
            }
            size_t n = (len - off < 4) ? (len - off) : 4;
            std::memcpy(p + off, &v, n);
            off += n;
        }
        return true;
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
