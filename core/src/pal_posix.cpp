// pal_posix.cpp - Linux/Android 共用的 PAL 实现。
// Windows/iOS 适配点:本文件按 spec §12 重新实现一遍即可。

#include "logx_pal.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace logx {

namespace {

class PosixMutex : public std::mutex {};

class Event {
public:
    Event() { pthread_mutex_init(&mu_, nullptr); pthread_cond_init(&cv_, nullptr); signaled_ = false; }
    ~Event() { pthread_cond_destroy(&cv_); pthread_mutex_destroy(&mu_); }
    void signal_one() {
        std::lock_guard<pthread_mutex_t> lk(mu_);
        signaled_ = true;
        pthread_cond_signal(&cv_);
    }
    void wait() {
        std::lock_guard<pthread_mutex_t> lk(mu_);
        while (!signaled_) pthread_cond_wait(&cv_, &mu_);
        signaled_ = false;
    }
private:
    pthread_mutex_t mu_;
    pthread_cond_t  cv_;
    bool signaled_;
};

class PosixPal : public Pal {
public:
    // ---- 文件 ----
    int file_open(const char* path, bool create_if_missing, bool append) override {
        int flags = O_CLOEXEC;
        if (append) flags |= O_APPEND;
        if (create_if_missing) flags |= O_CREAT | O_RDWR;
        else                   flags |= O_RDWR;
        int fd = ::open(path, flags, 0644);
        return fd;
    }
    int file_close(int fd) override { return ::close(fd); }
    int file_truncate(int fd, uint64_t len) override {
        return ::ftruncate(fd, static_cast<off_t>(len));
    }
    long file_write(int fd, const void* buf, size_t len) override {
        const uint8_t* p = static_cast<const uint8_t*>(buf);
        size_t total = 0;
        while (total < len) {
            ssize_t n = ::write(fd, p + total, len - total);
            if (n < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (n == 0) return -1;  // 极少:磁盘满 / append 模式被截
            total += static_cast<size_t>(n);
        }
        return static_cast<long>(total);
    }
    long file_pwrite(int fd, const void* buf, size_t len, uint64_t offset) override {
        const uint8_t* p = static_cast<const uint8_t*>(buf);
        size_t total = 0;
        while (total < len) {
            ssize_t n = ::pwrite(fd, p + total, len - total, static_cast<off_t>(offset + total));
            if (n < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (n == 0) return -1;
            total += static_cast<size_t>(n);
        }
        return static_cast<long>(total);
    }
    int file_fsync(int fd) override {
#ifdef __APPLE__
        return ::fcntl(fd, F_FULLFSYNC);
#else
        return ::fdatasync(fd);
#endif
    }
    int file_dir_fsync(const char* dir_path) override {
        int fd = ::open(dir_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) return -1;
        int r = ::fsync(fd);
        ::close(fd);
        return r;
    }
    int file_unlink(const char* path) override {
        return ::unlink(path);
    }
    int file_mkdirs(const char* dir_path) override {
        // 复制路径以便逐级创建
        std::string s(dir_path);
        if (s.empty()) return -1;
        // 跳过末尾 '/'
        if (s.back() == '/') s.pop_back();
        size_t pos = 0;
        if (s[0] == '/') pos = 1;  // 绝对路径
        std::string acc;
        if (pos == 1) acc = "/";
        while (pos <= s.size()) {
            char c = pos < s.size() ? s[pos] : 0;
            if (c == '/' || c == 0) {
                if (!acc.empty() && acc != "/") {
                    if (::mkdir(acc.c_str(), 0755) != 0 && errno != EEXIST) return -1;
                }
            }
            if (c != 0) acc.push_back(c);
            ++pos;
        }
        return 0;
    }
    std::vector<std::string> file_list(const char* dir_path) override {
        std::vector<std::string> out;
        DIR* d = ::opendir(dir_path);
        if (!d) return out;
        struct dirent* ent;
        while ((ent = ::readdir(d)) != nullptr) {
            std::string name = ent->d_name;
            if (name == "." || name == "..") continue;
            std::string full = std::string(dir_path);
            if (!full.empty() && full.back() != '/') full.push_back('/');
            full += name;
            // 仅文件:stat 检查
            struct stat st;
            if (::stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                out.push_back(full);
            }
        }
        ::closedir(d);
        return out;
    }

    // ---- mmap ----
    PalMmap mmap_open(int fd, size_t len, bool writable) override {
        PalMmap m;
        m.out_handle = fd;
        m.len = len;
        int prot = PROT_READ | (writable ? PROT_WRITE : 0);
        int flags = MAP_SHARED;
        void* base = ::mmap(nullptr, len, prot, flags, fd, 0);
        if (base == MAP_FAILED) {
            m.base = nullptr;
            m.out_handle = -1;
            return m;
        }
        m.base = base;
        return m;
    }
    int mmap_sync(const PalMmap& m, bool synchronous) override {
        if (!m.base) return -1;
        return ::msync(m.base, m.len, synchronous ? MS_SYNC : MS_ASYNC);
    }
    int mmap_close(const PalMmap& m) override {
        if (!m.base) return -1;
        return ::munmap(m.base, m.len);
    }

    // ---- 互斥 / 事件 ----
    void* mutex_create() override { return new PosixMutex(); }
    void  mutex_destroy(void* h) override { delete static_cast<PosixMutex*>(h); }
    void  mutex_lock(void* h) override { static_cast<PosixMutex*>(h)->lock(); }
    void  mutex_unlock(void* h) override { static_cast<PosixMutex*>(h)->unlock(); }

    int threading_init() override {
        // pthread 不需要显式 init;返回 0。
        return 0;
    }

    struct ThreadCtx { void* (*fn)(void*); void* arg; };
    static void* thread_trampoline(void* p) {
        ThreadCtx* tc = static_cast<ThreadCtx*>(p);
        void* (*fn)(void*) = tc->fn;
        void* arg = tc->arg;
        delete tc;
        return fn(arg);
    }
    int thread_start(void* (*fn)(void*), void* arg, void** out_handle) override {
        ThreadCtx* tc = new ThreadCtx();
        tc->fn = fn; tc->arg = arg;
        pthread_t t;
        int rc = ::pthread_create(&t, nullptr, &thread_trampoline, tc);
        if (rc != 0) { delete tc; return -1; }
        // detach 由外部 join 控制;此处保留 pthread_t 给调用方
        // 用一个动态分配的句柄存储 pthread_t(pthread_t 不是 trivial)
        pthread_t* pt = new pthread_t(t);
        *out_handle = pt;
        return 0;
    }
    int thread_join(void* handle) override {
        pthread_t* pt = static_cast<pthread_t*>(handle);
        int rc = ::pthread_join(*pt, nullptr);
        delete pt;
        return rc;
    }

    void* ev_create() override { return new Event(); }
    void  ev_destroy(void* h) override { delete static_cast<Event*>(h); }
    void  ev_signal(void* h) override { static_cast<Event*>(h)->signal_one(); }
    void  ev_wait(void* h) override { static_cast<Event*>(h)->wait(); }

    // ---- 原子 ----
    uint64_t atomic_add(uint64_t* p, int64_t delta) override {
        return std::atomic_fetch_add(reinterpret_cast<std::atomic<uint64_t>*>(p), delta);
    }
    uint64_t atomic_load(uint64_t* p) override {
        return std::atomic_load(reinterpret_cast<std::atomic<uint64_t>*>(p));
    }

    // ---- 时间 ----
    uint64_t monotonic_ns() override {
        struct timespec ts;
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
    }
    uint64_t wallclock_ms() override {
        struct timespec ts;
        ::clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
    }
    void wallclock_date(uint32_t* yyyymmdd, uint32_t* hhmmss) override {
        time_t now = ::time(nullptr);
        struct tm tm;
        ::localtime_r(&now, &tm);
        if (yyyymmdd) {
            *yyyymmdd = static_cast<uint32_t>((tm.tm_year + 1900) * 10000 +
                                              (tm.tm_mon + 1) * 100 + tm.tm_mday);
        }
        if (hhmmss) {
            *hhmmss = static_cast<uint32_t>(tm.tm_hour * 10000 + tm.tm_min * 100 + tm.tm_sec);
        }
    }

    uint32_t pid() override { return static_cast<uint32_t>(::getpid()); }
    uint64_t tid() override {
#ifdef SYS_gettid
        return static_cast<uint64_t>(::syscall(SYS_gettid));
#else
        return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
    }

    void random_bytes(void* buf, size_t len) override {
        // Linux/Android 上 /dev/urandom 总是可用;getrandom 是首选但非必需。
        int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            size_t off = 0;
            while (off < len) {
                ssize_t n = ::read(fd, static_cast<uint8_t*>(buf) + off, len - off);
                if (n > 0) { off += static_cast<size_t>(n); continue; }
                if (n < 0 && errno == EINTR) continue;
                break;
            }
            ::close(fd);
            if (off == len) return;
        }
        // 退化:用 monotonic 时间 + 地址做填充(仅测试用)
        uint64_t t = monotonic_ns();
        uint64_t addr = reinterpret_cast<uint64_t>(buf);
        for (size_t i = 0; i < len; ++i) {
            static_cast<uint8_t*>(buf)[i] = static_cast<uint8_t>((t >> (i % 8 * 8)) ^ (addr >> (i % 8 * 8)));
        }
    }

    void debug_log(const char* s) override {
        if (!s) return;
        std::fprintf(stderr, "[logx] %s\n", s);
    }
};

}  // namespace

Pal& default_pal() {
    static PosixPal inst;
    return inst;
}

}  // namespace logx