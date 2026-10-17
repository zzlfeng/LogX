// jni_key_cache.h - JNI 密钥提供者线程局部多槽缓存纯逻辑实现
//
// J-6 / R41-03 / R41-04:
//   - 支持单线程多 kid 混跑时各自拥有独立的 32B 密钥缓冲指针, 杜绝单缓冲复用导致的覆盖与悬垂;
//   - 绑定 owner 属主指针(对应 JniKeyProviderContext* ctx), 杜绝线程复用时跨实例密钥污染;
//   - 优先复用无效空槽, 规避无谓轮转驱逐;
//   - 支持按属主失效清理并清零密钥内存。
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
        uint16_t kid = 0;
        bool valid = false;
        uint8_t key[32] = {0};
    };

    const uint8_t* lookup(const void* owner, uint16_t kid) const {
        for (size_t i = 0; i < N; ++i) {
            if (slots_[i].valid && slots_[i].owner == owner && slots_[i].kid == kid) {
                return slots_[i].key;
            }
        }
        return nullptr;
    }

    uint8_t* allocate_slot(const void* owner, uint16_t kid) {
        // R41-04: 优先复用当前未使用的空槽
        size_t slot_idx = N;
        for (size_t i = 0; i < N; ++i) {
            if (!slots_[i].valid) {
                slot_idx = i;
                break;
            }
        }
        if (slot_idx == N) {
            slot_idx = (next_++) % N;
        }
        slots_[slot_idx].owner = owner;
        slots_[slot_idx].kid = kid;
        slots_[slot_idx].valid = true;
        return slots_[slot_idx].key;
    }

    void invalidate_owner(const void* owner) {
        for (size_t i = 0; i < N; ++i) {
            if (slots_[i].owner == owner) {
                slots_[i].valid = false;
                slots_[i].owner = nullptr;
                std::memset(slots_[i].key, 0, sizeof(slots_[i].key));
            }
        }
    }

    const Slot& slot(size_t idx) const { return slots_[idx]; }
    static constexpr size_t capacity() { return N; }

private:
    Slot slots_[N]{};
    size_t next_ = 0;
};

}  // namespace logx_jni

#endif  // LOGX_JNI_KEY_CACHE_H

