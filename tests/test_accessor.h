#ifndef LOGX_TEST_ACCESSOR_H
#define LOGX_TEST_ACCESSOR_H

#include <atomic>
#include "logx_c.h"
#include "logx_internal.h"

namespace logx {
namespace test {

inline InstanceControlBlock* test_get_cb(logx_instance_t* h) {
    return reinterpret_cast<InstanceControlBlock*>(h);
}

inline InstanceControlBlock* test_get_cb(Instance* inst) {
    return inst ? inst->cb : nullptr;
}

inline Instance* test_get_instance(logx_instance_t* h) {
    if (!h) return nullptr;
    auto* cb = reinterpret_cast<InstanceControlBlock*>(h);
    return cb->inst.load(std::memory_order_acquire);
}

inline Instance* test_get_instance(Instance* inst) {
    return inst;
}

inline void test_set_closing(logx_instance_t* h, bool closing) {
    if (auto* cb = test_get_cb(h)) {
        cb->is_closing.store(closing, std::memory_order_seq_cst);
    }
}

inline void test_set_closing(Instance* inst, bool closing) {
    if (inst && inst->cb) {
        inst->cb->is_closing.store(closing, std::memory_order_seq_cst);
    }
}

inline bool test_get_closing(logx_instance_t* h) {
    if (auto* cb = test_get_cb(h)) {
        return cb->is_closing.load(std::memory_order_seq_cst);
    }
    return false;
}

inline bool test_get_closing(Instance* inst) {
    if (inst && inst->cb) {
        return inst->cb->is_closing.load(std::memory_order_seq_cst);
    }
    return false;
}

inline uint64_t test_get_flusher_tid(logx_instance_t* h) {
    if (auto* cb = test_get_cb(h)) {
        return cb->flusher_tid.load(std::memory_order_acquire);
    }
    return 0;
}

inline uint64_t test_get_flusher_tid(Instance* inst) {
    if (inst && inst->cb) {
        return inst->cb->flusher_tid.load(std::memory_order_acquire);
    }
    return 0;
}

inline void test_set_retention_days(logx_instance_t* h, uint32_t days) {
    if (auto* inst = test_get_instance(h)) {
        inst->retention_days.store(days, std::memory_order_release);
    }
}

inline void test_set_retention_days(Instance* inst, uint32_t days) {
    if (inst) {
        inst->retention_days.store(days, std::memory_order_release);
    }
}

// R46 P2-04: 无数据竞争注入 key_provider 的测试辅助。
// 必须先暂停并 join Flusher 线程，在单线程静止态下修改 encrypt_enable/kid/key_user/key_provider，
// 再重新启动 Flusher 线程，彻底消除对运行中 Flusher 写入非原子字段的形式数据竞争(UB)。
inline void test_inject_key_provider_quiesced(logx_instance_t* h,
                                              uint16_t kid,
                                              const uint8_t* (*kp)(uint16_t, void*),
                                              void* key_user) {
    Instance* inst = test_get_instance(h);
    if (!inst || !inst->pal) return;
    if (inst->flusher_thread) {
        inst->flusher_exit.store(true, std::memory_order_release);
        if (inst->flush_event) inst->pal->ev_signal(inst->flush_event);
        inst->pal->thread_join(inst->flusher_thread);
        inst->flusher_thread = nullptr;
    }
    inst->flusher_exit.store(false, std::memory_order_release);
    inst->encrypt_enable = true;
    inst->kid = kid;
    inst->key_user = key_user;
    inst->key_provider = kp;
    if (inst->pal->thread_start(&flusher_main, inst, &inst->flusher_thread) == 0) {
        inst->flusher_running.store(true, std::memory_order_release);
    }
}

// 模拟进程强杀/崩溃：绕过正常 close flush, 强制停止 flusher 并释放资源。
// 注意 (R45-08): 本函数仅用于单线程崩溃恢复模拟测试，故意销毁控制块 (delete cb)
// 而不做墓碑保留，严禁用于正常并发关闭路径或陈旧句柄访问测试。
inline void test_simulate_crash_destroy(logx_instance_t*& h) {
    if (!h) return;
    auto* cb = test_get_cb(h);
    if (cb) {
        cb->is_closing.store(true, std::memory_order_seq_cst);
    }
    Instance* raw_inst = cb ? cb->inst.exchange(nullptr, std::memory_order_acq_rel) : nullptr;
    if (raw_inst) {
        raw_inst->flusher_exit.store(true, std::memory_order_release);
        if (raw_inst->flush_event && raw_inst->pal) {
            raw_inst->pal->ev_signal(raw_inst->flush_event);
        }
        if (raw_inst->space_event && raw_inst->pal) {
            raw_inst->pal->ev_signal(raw_inst->space_event);
        }
        if (raw_inst->flusher_thread) {
            raw_inst->pal->thread_join(raw_inst->flusher_thread);
            raw_inst->flusher_thread = nullptr;
        }
        delete raw_inst->staging;
        raw_inst->staging = nullptr;
        if (raw_inst->flush_event) raw_inst->pal->ev_destroy(raw_inst->flush_event);
        if (raw_inst->space_event) raw_inst->pal->ev_destroy(raw_inst->space_event);
        if (raw_inst->mu) raw_inst->pal->mutex_destroy(raw_inst->mu);
        delete raw_inst;
    }
    delete cb;
    h = nullptr;
}

}  // namespace test
}  // namespace logx

#endif  // LOGX_TEST_ACCESSOR_H

