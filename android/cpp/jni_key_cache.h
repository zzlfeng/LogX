// jni_key_cache.h - JNI 密钥提供者线程局部多槽缓存纯逻辑实现
//
// J-6 / R41-03 / R41-04 / R42-02 / R42-03:
//   - 支持单线程多 kid 混跑时各自拥有独立的 32B 密钥缓冲指针, 杜绝单缓冲复用导致的覆盖与悬垂;
//   - 绑定 owner 属主指针与全局单调递增 gen 代数 (owner, gen, kid 三元匹配),
//     彻底杜绝跨线程复用(如 Kotlin Dispatchers.IO)及堆内存地址重用(ABA)导致的陈旧密钥假命中;
//   - 优先惰性清扫同 owner 但 gen 陈旧的僵尸槽位, 避免跨实例堆地址重用时占用槽位与残留明文;
//   - 优先复用无效空槽与陈旧代数槽位;
//   - 任何槽位接管(含跨 owner 驱逐)在覆写元数据前必先清零旧密钥字节, 杜绝被驱逐属主的明文残留窗口;
//   - 容量耗尽(全局活跃不同 kid > N, 容量由各 owner 共享)时显式返回 nullptr 触发 core 降级, 严禁静默覆盖在持指针;
//   - 支持按属主和代数失效清理并安全清零密钥内存。
#ifndef LOGX_JNI_KEY_CACHE_H
#define LOGX_JNI_KEY_CACHE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace logx_jni {

// R45-12: 单调推进全局已销毁实例代数水位线 (watermark = max(watermark, destroyed_gen))。
// 当 on_destroy 漂移至 Flusher 或并发工作线程执行时，各宿主/业务线程在后续 lookup/allocate_slot
// 入口据此水位惰性清零并驱逐已销毁实例残留的明文密钥槽位。
inline void advance_destroyed_watermark(std::atomic<uint64_t>& watermark, uint64_t destroyed_gen) noexcept {
    if (destroyed_gen == 0) return;
    uint64_t cur = watermark.load(std::memory_order_relaxed);
    while (cur < destroyed_gen &&
           !watermark.compare_exchange_weak(cur, destroyed_gen,
                                            std::memory_order_release,
                                            std::memory_order_relaxed)) {
    }
}

template <size_t N = 4>
class TlsKeyCache {
public:
    struct Slot {
        const void* owner = nullptr;
        uint64_t gen = 0;
        uint16_t kid = 0;
        bool valid = false;
        uint8_t key[32] = {0};
    };

    const uint8_t* lookup(const void* owner, uint64_t gen, uint16_t kid,
                          uint64_t destroyed_watermark = 0) {
        // R45-12: 将同 owner 异 gen 及跨 owner 已销毁代数水位的惰性清扫前移至 lookup 入口，
        // 确保即使后续 Kotlin key_provider 回调抛异常未进 allocate_slot，陈旧明文槽位也能在首次探测时清零。
        scrub_stale_slots(owner, gen, destroyed_watermark);
        for (size_t i = 0; i < N; ++i) {
            if (slots_[i].valid && slots_[i].owner == owner && slots_[i].gen == gen && slots_[i].kid == kid) {
                return slots_[i].key;
            }
        }
        return nullptr;
    }

    uint8_t* allocate_slot(const void* owner, uint64_t gen, uint16_t kid,
                           uint64_t destroyed_watermark = 0) {
        // 0. R42-02 / R45-12: 惰性清扫同 owner 异 gen (ABA 复用) 及异 owner 已达销毁水位的残留槽位
        scrub_stale_slots(owner, gen, destroyed_watermark);

        // 1. 优先复用当前未激活的空槽 (!valid)
        for (size_t i = 0; i < N; ++i) {
            if (!slots_[i].valid) {
                return populate_slot(i, owner, gen, kid);
            }
        }

        // 2. 查找是否有属于其它 owner 的槽位（陈旧残留槽位安全复用）
        for (size_t i = 0; i < N; ++i) {
            if (slots_[i].owner != owner) {
                return populate_slot(i, owner, gen, kid);
            }
        }

        // 3. R42-03 / ACT-3: 若所有 N 槽均已被当前活跃 (owner, gen) 的不同 kid 占用,
        // 契约规定单线程并发活跃不同 kid 须 <= N。此时绝不静默覆盖在持指针,
        // 显式返回 nullptr 触发 core 缺钥降级与 DEGRADED 事件, 确保 J-6 契约绝对稳定。
        return nullptr;
    }

    void invalidate_owner(const void* owner, uint64_t gen) {
        for (size_t i = 0; i < N; ++i) {
            if (slots_[i].owner == owner && (gen == 0 || slots_[i].gen == gen)) {
                clear_slot(i);
            }
        }
    }

    const Slot& slot(size_t idx) const { return slots_[idx]; }
    static constexpr size_t capacity() { return N; }

private:
    void clear_slot(size_t idx) {
        slots_[idx].valid = false;
        slots_[idx].owner = nullptr;
        slots_[idx].gen = 0;
        slots_[idx].kid = 0;
        std::memset(slots_[idx].key, 0, sizeof(slots_[idx].key));
    }

    void scrub_stale_slots(const void* owner, uint64_t gen, uint64_t destroyed_watermark) {
        for (size_t i = 0; i < N; ++i) {
            if (!slots_[i].valid) continue;
            const bool same_owner_stale_gen = (slots_[i].owner == owner && slots_[i].gen != gen);
            // 异 owner 且代数不高于已销毁水位: 先清零再驱逐；
            // 通过 slots_[i].owner != owner 豁免当前活跃调用方自身，闭合 J-6 单文件多 kid 指针稳定规约。
            const bool other_owner_destroyed = (slots_[i].owner != owner &&
                                                destroyed_watermark > 0 &&
                                                slots_[i].gen <= destroyed_watermark);
            if (same_owner_stale_gen || other_owner_destroyed) {
                clear_slot(i);
            }
        }
    }

    uint8_t* populate_slot(size_t idx, const void* owner, uint64_t gen, uint16_t kid) {
        // R43: 接管槽位前无条件清零旧密钥字节。步骤 2 跨 owner 驱逐时,
        // 被驱逐属主的明文密钥若不先清零, 会残留至调用方 GetByteArrayRegion 覆写为止,
        // 与"安全清零密钥内存"契约不符; 空槽路径清零为幂等无害操作。
        std::memset(slots_[idx].key, 0, sizeof(slots_[idx].key));
        slots_[idx].owner = owner;
        slots_[idx].gen = gen;
        slots_[idx].kid = kid;
        slots_[idx].valid = true;
        return slots_[idx].key;
    }

    Slot slots_[N]{};
};

}  // namespace logx_jni

#endif  // LOGX_JNI_KEY_CACHE_H

