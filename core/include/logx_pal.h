/* logx_pal.h - Platform Abstraction Layer.
 *
 * 设计动机:核心保持零平台依赖。iOS 适配、Windows 适配、CocoaPods 打包均只需
 * 重新实现本文件里的接口;其余源码不改。Spec §2、§12。
 */
#ifndef LOGX_PAL_H
#define LOGX_PAL_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace logx {

/* mmap 描述。addr_hint 可选(NULL = 由系统选);len = 文件大小(已 ftruncate);
 * writable = true 表示 MAP_SHARED + PROT_WRITE;失败时 out_handle = -1。 */
struct PalMmap {
    void*  base = nullptr;
    int    out_handle = -1;  /* 句柄以便 msync/close */
    size_t len = 0;
};

class Pal {
public:
    virtual ~Pal() = default;

    /* ---- 文件系统 ---- */
    /* open(path, create_if_missing, append) -> fd;失败 = -1。flags 自动加 O_CLOEXEC。 */
    virtual int  file_open(const char* path, bool create_if_missing, bool append) = 0;
    virtual int  file_close(int fd) = 0;
    /* ftruncate 到 len;成功 0。 */
    virtual int  file_truncate(int fd, uint64_t len) = 0;
    /* 循环处理短写,返回写入字节数;失败 -1。 */
    virtual long file_write(int fd, const void* buf, size_t len) = 0;
    virtual long file_pwrite(int fd, const void* buf, size_t len, uint64_t offset) = 0;
    /* 循环处理短读(从 offset 起),返回读到的字节数;失败 -1。 */
    virtual long file_pread(int fd, void* buf, size_t len, uint64_t offset) = 0;
    /* 文件当前大小(fd);失败 -1。 */
    virtual int64_t file_size(int fd) = 0;
    /* 路径对应文件大小;失败 -1。 */
    virtual int64_t file_size_path(const char* path) = 0;
    virtual int  file_fsync(int fd) = 0;            /* fdatasync(fd) - 仅数据,不要元数据 */
    virtual int  file_dir_fsync(const char* dir_path) = 0;  /* 目录项 fsync */
    virtual int  file_unlink(const char* path) = 0;
    virtual int  file_mkdirs(const char* dir_path) = 0;
    /* 删除空目录(测试/清理用);失败 -1。 */
    virtual int  file_rmdir(const char* dir_path) = 0;

    /* 列出 dir 下所有条目(不含子目录);返回完整路径列表。 */
    virtual std::vector<std::string> file_list(const char* dir_path) = 0;

    /* ---- mmap ---- */
    /* 映射文件;失败时 out.base = nullptr。len 必须是文件实际大小。 */
    virtual PalMmap mmap_open(int fd, size_t len, bool writable) = 0;
    virtual int     mmap_sync(const PalMmap& m, bool synchronous) = 0; /* msync */
    virtual int     mmap_close(const PalMmap& m) = 0;

    /* ---- 并发原语 ---- */
    virtual void* mutex_create() = 0;
    virtual void  mutex_destroy(void* h) = 0;
    virtual void  mutex_lock(void* h) = 0;
    virtual void  mutex_unlock(void* h) = 0;

    /* 一次性初始化;同进程内多次调用幂等。返回 0 成功。 */
    virtual int  threading_init() = 0;

    /* 启动一次性线程;失败 -1;routine 抛异常视为内部错误,本 PAL 仅负责线程创建。 */
    virtual int  thread_start(void* (*fn)(void*), void* arg, void** out_handle) = 0;
    virtual int  thread_join(void* handle) = 0;

    /* 信号量 / 通知:用于 Flusher 唤醒;可能不依赖 OS 信号量,实现可自行选择
     * pthread_cond / eventfd / futex。本接口仅约定"唤醒正在 wait 的消费者"。 */
    virtual void* ev_create() = 0;
    virtual void  ev_destroy(void* h) = 0;
    virtual void  ev_signal(void* h) = 0;
    virtual void  ev_wait(void* h) = 0;   /* 进入等待,ev_signal 唤醒;不限制虚假唤醒 */
    virtual bool  ev_wait_timeout(void* h, uint32_t ms) = 0; /* 带超时等待,超时返回 false,被唤醒返回 true */

    /* ---- 原子 ---- */
    virtual uint64_t atomic_add(uint64_t* p, int64_t delta) = 0;
    virtual uint64_t atomic_load(uint64_t* p) = 0;

    /* ---- 时间 ---- */
    virtual uint64_t monotonic_ns() = 0;
    virtual uint64_t wallclock_ms() = 0;          /* yyyyMMdd HHmmss 字段分离用 */
    virtual void     wallclock_date(uint32_t* yyyymmdd, uint32_t* hhmmss) = 0;
    /* 休眠 ms(flush 同步自旋 / 重试退避用)。 */
    virtual void     sleep_ms(uint32_t ms) = 0;

    /* ---- 进程 / 线程标识(进格式化) ---- */
    virtual uint32_t pid() = 0;
    virtual uint64_t tid() = 0;

    /* ---- 随机(文件盐、nonce 高位) ---- */
    virtual void     random_bytes(void* buf, size_t len) = 0;

    /* ---- 日志(仅供内部调试,生产构建应禁用) ---- */
    virtual void     debug_log(const char* s) = 0;
};

/* 默认 POSIX 实现;调用方持有一个全局实例。 */
Pal& default_pal();

}  // namespace logx
#endif /* LOGX_PAL_H */