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

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace logx_jni {

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

    const uint8_t* lookup(const void* owner, uint64_t gen, uint16_t kid) const {
        for (size_t i = 0; i < N; ++i) {
            if (slots_[i].valid && slots_[i].owner == owner && slots_[i].gen == gen && slots_[i].kid == kid) {
                return slots_[i].key;
            }
        }
        return nullptr;
    }

    uint8_t* allocate_slot(const void* owner, uint64_t gen, uint16_t kid) {
        // 0. R42-02: 惰性清扫同 owner 但 gen 陈旧的残留槽位(ABA 地址复用), 释放槽位并安全清零
        for (size_t i = 0; i < N; ++i) {
            if (slots_[i].valid && slots_[i].owner == owner && slots_[i].gen != gen) {
                slots_[i].valid = false;
                slots_[i].owner = nullptr;
                slots_[i].gen = 0;
                std::memset(slots_[i].key, 0, sizeof(slots_[i].key));
            }
        }

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
                slots_[i].valid = false;
                slots_[i].owner = nullptr;
                slots_[i].gen = 0;
                std::memset(slots_[i].key, 0, sizeof(slots_[i].key));
            }
        }
    }

    const Slot& slot(size_t idx) const { return slots_[idx]; }
    static constexpr size_t capacity() { return N; }

private:
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

