// logx_jni.cpp - JNI 胶水。零逻辑,只做转发(参见 spec §10)。
//
// Kotlin facade LogX.kt 调用本文件暴露的 native 方法;C ABI 由 core 提供,
// JNI 只在两者之间做参数转换 / 字符串复制 / 异常抛出。
//
// 性能要点:
//   - 单条日志路径使用 GetStringUTFRegion 栈缓冲读取 tag(超长 tag 才退化为
//     堆缓冲, 见 nativeWrite J-3),
//     搭配 GetByteArrayRegion 栈缓冲/thread_local 读取 payload, 实现 JNI 边界零堆内存分配 (Zero Heap Allocation),
//     同时彻底杜绝 GetPrimitiveArrayCritical 临界区内触发错误回调导致的 CheckJNI 崩溃。
//   - keyProvider 回调里不能重入 LogX(JNI 调用栈已在 native,死锁风险)。
//   - 错误回调 / export 回调运行在调用线程,直接回调 Kotlin。

#include <jni.h>
#include <android/log.h>

#include "logx_c.h"
#include "logx_internal.h"
#include "jni_string_util.h"
#include "jni_key_cache.h"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#define LOGX_TAG "LogX"

namespace {

// T5.4: consoleEcho logcat 直通开关。C ABI 不变 —— echo 是 JNI 交付层能力,
// 桌面 C++ 使用者不受影响。进程级语义与 Kotlin facade 的单实例 init 契约匹配;
// 关闭时 nativeWrite 仅付一次 relaxed 原子读,零分配零字符串操作。
std::atomic<bool> g_console_echo{false};

// level(0..5) → android.util.Log 优先级。仅 echo 路径使用。
inline int logx_level_to_priority(int level) {
    switch (level) {
        case 0: return ANDROID_LOG_VERBOSE;
        case 1: return ANDROID_LOG_DEBUG;
        case 2: return ANDROID_LOG_INFO;
        case 3: return ANDROID_LOG_WARN;
        case 4: return ANDROID_LOG_ERROR;
        default: return ANDROID_LOG_FATAL;  // 5 (Level.F) 与一切越界值兜底
    }
}

// T5.4: echo 一条已成功落盘的记录到 logcat。
// 镜像语义: 仅 nativeWrite 返回 OK 后调用 —— logcat 与日志文件一一对应;
// 丢弃/降级仍走 ErrorHandler 事件通道,不在 echo 里重复。
// 修复保障:
// 1) [Tag 修复]: 优先使用调用方传递的业务 tag,为空时回退为 LOGX_TAG,
//    确保 Android 开发者可按 Tag 过滤 logcat;
// 2) 镜像输出在 logx_write 落盘成功后调用, 确保 logcat 真实反映落盘日志;
// 3) [格式串安全]: 统一使用 "%s" 格式化输出,防 payload 包含 % 字符导致崩溃。
void console_echo(int level, const char* tag, const char* msg) {
    const int priority = logx_level_to_priority(level);
    const char* log_tag = (tag && tag[0] != '\0') ? tag : LOGX_TAG;
    __android_log_print(priority, log_tag, "%s", msg ? msg : "");
}

// R42-02 / ACT-2: 全局单调递增实例代数计数器，杜绝跨线程复用/ABA地址复用导致的陈旧密钥假命中
static std::atomic<uint64_t> g_jni_ctx_generation{1};

struct JniKeyProviderContext {
    JavaVM* jvm = nullptr;
    jobject g_provider = nullptr;
    jmethodID mid_get_key = nullptr;
    uint64_t gen = 0;  // 单调实例代数
};

struct JniErrorContext {
    JavaVM* jvm = nullptr;
    jclass g_logx_class = nullptr;
    jmethodID mid_on_error = nullptr;
};

// J-02: 类级常量(jclass/jmethodID)。jclass 持全局引用在进程退出时随 JVM
// 回收;jmethodID 语义等同 const 指针。仅这两项保留文件级静态 —— 可变状态
// (jvm 指针、每实例上下文)全部下放实例,多实例互不干扰。
static jclass g_logx_class = nullptr;
static jmethodID g_mid_on_error = nullptr;

// J-03: 统一经 UTF-16 构造 jstring。NewStringUTF 期望 Modified UTF-8,
// 标准 UTF-8 输入(如含 emoji/补充平面字符的消息)在个别 JVM 上可能触发
// 编码异常;先解码为 UTF-16 码元再 NewString,与 JVM 原生表示对齐。
// 解码逻辑在 jni_string_util.h(纯逻辑,桌面单测覆盖)。
static jstring make_jstring_utf16(JNIEnv* env, const char* utf8) {
    if (!utf8) return nullptr;
    std::vector<uint16_t> units = logx_jni::utf8_to_utf16(utf8, std::strlen(utf8));
    jstring s = env->NewString(reinterpret_cast<const jchar*>(units.data()),
                               static_cast<jsize>(units.size()));
    return s;
}

// J-3: jstring → 标准 UTF-8(CESU-8 经就地转码修正为 core 契约的标准 UTF-8)。
// 用于 tag / logDir / stagingPath / export 日期范围等按 UTF-8 契约入 core 的参数。
// GetStringUTFChars 返回 JVM 只读缓冲, 不可就地改写, 故先拷入 std::string 再转码。
// J-10: GetStringUTFChars 失败(如 OOM)时清除 pending 异常并返回 false,
//       调用方据此返回错误码而非让异常穿透到 Kotlin。
static bool jstring_to_utf8(JNIEnv* env, jstring js, std::string* out) {
    if (!js || !out) return false;
    const char* tmp = env->GetStringUTFChars(js, nullptr);
    if (!tmp) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return false;
    }
    size_t n = static_cast<size_t>(env->GetStringUTFLength(js));
    out->assign(tmp, n);
    env->ReleaseStringUTFChars(js, tmp);
    if (n == 0) return true;  // 空串: 免对空 std::string 取 &out[0](UB)
    size_t nl = logx_jni::cesu8_to_utf8_inplace(&(*out)[0], out->size());
    out->resize(nl);
    return true;
}

// J-2: 错误回调重入守卫。错误回调在调用线程同步执行;若用户回调内再次调用
// LogX(如 log/flush/close), 会经 core 再次进入本 trampoline, 在非递归 mutex
// 上构成同线程二次取锁死锁(mutex 契约见 logx_pal.h P-2 留档)。
// 同线程重入直接丢弃该事件并返回, 既避免死锁也防止回调栈无限增长。
static thread_local bool tls_in_error_callback = false;

struct ErrorCallbackGuard {
    ErrorCallbackGuard() { tls_in_error_callback = true; }
    ~ErrorCallbackGuard() { tls_in_error_callback = false; }
    ErrorCallbackGuard(const ErrorCallbackGuard&) = delete;
    ErrorCallbackGuard& operator=(const ErrorCallbackGuard&) = delete;
};

// J-02: 错误回调上下文按实例持有(经 cfg.err_user 传入 core,由 emit_event
// 回传),不再使用全局静态 g_err_ctx —— 多实例场景下任一实例关闭只影响自身,
// 其余实例的回调路径不受干扰;生命周期与实例严格同步。
static void jni_error_trampoline(void* user, logx_event_kind_e kind, int code, const char* msg) {
    // J-2: 重入守卫——同线程回调链上再次进入即丢弃(明细见 ErrorCallbackGuard 注释)。
    if (tls_in_error_callback) return;
    ErrorCallbackGuard reentry_guard;
    (void)reentry_guard;
    auto* ctx = static_cast<const JniErrorContext*>(user);
    if (!ctx || !ctx->jvm || !ctx->g_logx_class || !ctx->mid_on_error) {
        return;
    }
    JNIEnv* env = nullptr;
    bool need_detach = false;
    jint getEnvRes = ctx->jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvRes == JNI_EDETACHED) {
        // J-17(留档): Flusher 为原生线程, 每发一个事件都要 Attach/Detach 一次,
        // 属已知开销。保持"用完即 Detach"以确保线程真的退出时不会被 JVM 悬挂
        // (无线程退出钩子可用); 后续若实测占比显著, 可改为 thread_local 缓存
        // "已由本层 Attach"状态并延迟到线程退出(需配合 JNI_OnUnload/pthread key)。
#if defined(__ANDROID__)
        if (ctx->jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            need_detach = true;
        }
#else
        if (ctx->jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) == JNI_OK) {
            need_detach = true;
        }
#endif
    } else if (getEnvRes != JNI_OK || !env) {
        return;
    }

    if (env) {
        jstring jMsg = make_jstring_utf16(env, msg ? msg : "");
        if (!jMsg) {
            // J-03: 构造失败(如 OOM)时降级为空串占位,保证回调链路不中断
            jMsg = env->NewStringUTF("");
        }
        env->CallStaticVoidMethod(ctx->g_logx_class, ctx->mid_on_error,
                                  static_cast<jint>(kind), static_cast<jint>(code), jMsg);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        env->DeleteLocalRef(jMsg);
        if (need_detach) {
            ctx->jvm->DetachCurrentThread();
        }
    }
}

// J-6 / R41-03 / R41-04 / R45-12: thread_local 密钥多槽缓冲(kMaxTlsKeySlots=4) 与全局已销毁代数水位线。
// 支持单线程多 kid 独立缓冲, 且带 owner 属主指针隔离、已销毁代数水位跨 owner 惰性清零与空槽优先复用策略。
static constexpr size_t kMaxTlsKeySlots = 4;
static thread_local logx_jni::TlsKeyCache<kMaxTlsKeySlots> g_tls_key_cache;
static std::atomic<uint64_t> g_jni_destroyed_gen_watermark{0};

static const uint8_t* jni_key_provider_trampoline(uint16_t kid, void* user) {
    auto* ctx = static_cast<JniKeyProviderContext*>(user);
    if (!ctx || !ctx->jvm || !ctx->g_provider || !ctx->mid_get_key) {
        return nullptr;
    }

    const uint64_t watermark = g_jni_destroyed_gen_watermark.load(std::memory_order_relaxed);

    // 优先命中本线程缓存槽 (R42-02 / R45-12: 入口惰性清扫陈旧槽 + owner/gen/kid 三元匹配)
    if (const uint8_t* cached = g_tls_key_cache.lookup(ctx, ctx->gen, kid, watermark)) {
        return cached;
    }

    JNIEnv* env = nullptr;
    bool need_detach = false;
    jint getEnvRes = ctx->jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvRes == JNI_EDETACHED) {
#if defined(__ANDROID__)
        if (ctx->jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            need_detach = true;
        }
#else
        if (ctx->jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) == JNI_OK) {
            need_detach = true;
        }
#endif
    } else if (getEnvRes != JNI_OK || !env) {
        return nullptr;
    }
    if (!env) return nullptr;

    jbyteArray jKeyArr = static_cast<jbyteArray>(
        env->CallObjectMethod(ctx->g_provider, ctx->mid_get_key, static_cast<jint>(kid))
    );
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        jKeyArr = nullptr;
    }

    const uint8_t* res = nullptr;
    if (jKeyArr) {
        jsize len = env->GetArrayLength(jKeyArr);
        if (len == 32) {
            uint8_t* slot_key = g_tls_key_cache.allocate_slot(ctx, ctx->gen, kid, watermark);
            if (slot_key) {
                env->GetByteArrayRegion(jKeyArr, 0, 32, reinterpret_cast<jbyte*>(slot_key));
                res = slot_key;
            } else {
                // R42-03 / ACT-3: 满槽容量耗尽(超限), 绝不破坏已有有效槽位, 显式返回 nullptr 触发 core 缺钥降级
                res = nullptr;
            }
        }
        env->DeleteLocalRef(jKeyArr);
    }
    // R16 F-3: 与 jni_error_trampoline 对称——本线程 attach 的必须在本线程
    // detach, 否则 Flusher 线程退出时 ART 报 attached-thread 未 detach 告警。
    // 已 attach 的调用方线程(GetEnv==JNI_OK)不做 detach。
    if (need_detach) {
        ctx->jvm->DetachCurrentThread();
    }
    return res;
}

// J-1/F-2: 实例析构清理 trampoline。无论同步销毁还是 Flusher 线程/OpGuard 递延销毁,
// 均在 core 完全退出并销毁前夕被回调, 安全释放 Java 全局引用与 C++ 上下文。
static void jni_destroy_trampoline(void* key_user, void* err_user) {
    auto* kp_ctx = static_cast<JniKeyProviderContext*>(key_user);
    auto* inst_err_ctx = static_cast<JniErrorContext*>(err_user);

    // R41-03 / R42-02 / R45-12: 登记已销毁代数水位(供跨线程惰性清零残留槽)并即时清理当前线程缓存槽
    if (kp_ctx) {
        logx_jni::advance_destroyed_watermark(g_jni_destroyed_gen_watermark, kp_ctx->gen);
        g_tls_key_cache.invalidate_owner(kp_ctx, kp_ctx->gen);
    }

    if (kp_ctx) {
        if (kp_ctx->jvm && kp_ctx->g_provider) {
            JNIEnv* env = nullptr;
            bool need_detach = false;
            jint getEnvRes = kp_ctx->jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
            if (getEnvRes == JNI_EDETACHED) {
#if defined(__ANDROID__)
                if (kp_ctx->jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
                    need_detach = true;
                }
#else
                if (kp_ctx->jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) == JNI_OK) {
                    need_detach = true;
                }
#endif
            }
            if (env && kp_ctx->g_provider) {
                env->DeleteGlobalRef(kp_ctx->g_provider);
                kp_ctx->g_provider = nullptr;
            }
            if (need_detach) {
                kp_ctx->jvm->DetachCurrentThread();
            }
        }
        delete kp_ctx;
    }

    delete inst_err_ctx;
}

struct ExportContext {
    JNIEnv* env;
    jobject cb;
    jmethodID mid;
};

static int export_trampoline(void* user, const char* date, const char* plaintext_path, uint64_t size) {
    auto* ctx = static_cast<ExportContext*>(user);
    if (!ctx || !ctx->cb || !ctx->mid) return -1;

    JNIEnv* env = ctx->env;
    // J-03: 与错误回调一致,统一走 UTF-16 构造;路径含用户自定义 logDir 时
    // 可能出现非 ASCII 字节,NewStringUTF 的 Modified UTF-8 约束同样构成风险。
    jstring jDate = make_jstring_utf16(env, date ? date : "");
    jstring jPath = make_jstring_utf16(env, plaintext_path ? plaintext_path : "");
    // J-12: 构造失败(如 OOM)必须 null 兜底——原实现把 null 直接传给 Kotlin
    // 非空参数, NullPointerException 被下方 ExceptionClear 吞掉, 该分卷静默失败。
    // 清 pending 异常后再用 NewStringUTF 占位空串, 保证回调链路可继续。
    if (!jDate) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        jDate = env->NewStringUTF("");
    }
    if (!jPath) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        jPath = env->NewStringUTF("");
    }

    jint res = env->CallIntMethod(ctx->cb, ctx->mid, jDate, jPath, static_cast<jlong>(size));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        res = -1;
    }

    if (jDate) env->DeleteLocalRef(jDate);
    if (jPath) env->DeleteLocalRef(jPath);
    return static_cast<int>(res);
}

}  // namespace

extern "C" {

// Java LogX.init native 签名:
//   private static native long nativeInit(String logDir, String stagingPath,
//       int compressAlgo, int encryptEnable, int kid,
//       Object keyProvider, int retentionDays, long maxFileSize,
//       int maxLogLength, int flushThreshold, boolean consoleEcho);
//       // returns native handle
JNIEXPORT jlong JNICALL
Java_com_zzlfeng_logx_LogX_nativeInit(JNIEnv* env, jclass /*clazz*/,
                                      jstring jLogDir, jstring jStagingPath,
                                      jint compressAlgo, jint encryptEnable,
                                      jint kid, jobject jKeyProvider,
                                      jint retentionDays, jlong maxFileSize,
                                      jint maxLogLength, jint flushThreshold,
                                      jboolean jConsoleEcho,
                                      jint stagingWaitMs) {
    if (!jLogDir || !jStagingPath) return 0;

    // J-8: kid 为 jint, 静默 cast 到 uint16_t 会把负值/超 65535 变成另一合法
    // kid → 密钥错配且无告警。越界直接 fail-fast。
    if (kid < 0 || kid > 0xFFFF) return 0;
    // J-9: flushThreshold 仅允许 -1(默认)/ 0(禁用阈值自动唤醒)/ 正值;
    // ≤-2 的负值 cast 成巨大 uint32 会让阈值永不触发(未定义未拒绝), 直接拒绝。
    if (flushThreshold < -1) return 0;

    // T5.4: consoleEcho 开关在 init 时点定型(Kotlin facade 单实例契约下
    // 进程级存储无歧义;close→re-init 循环会重新走到这里刷新)。
    // J-18: g_console_echo 为进程级全局, 与 J-02"实例间零共享可变状态"的声明并存——
    // 这是有意的: 该开关仅映射到 logcat 镜像输出, 在单实例契约下无实害。
    g_console_echo.store(jConsoleEcho == JNI_TRUE, std::memory_order_relaxed);

    // J-3: 路径入 core 前经标准 UTF-8 转码(改 ANSI/CESU-8 问题见 jstring_to_utf8);
    // J-10: 读取失败时清 pending 异常并返回 0。
    std::string log_dir_s, staging_s;
    if (!jstring_to_utf8(env, jLogDir, &log_dir_s) ||
        !jstring_to_utf8(env, jStagingPath, &staging_s)) {
        return 0;
    }
    const char* log_dir = log_dir_s.c_str();
    const char* staging_path = staging_s.c_str();

    JniKeyProviderContext* kp_ctx = nullptr;
    JavaVM* jvm = nullptr;
    env->GetJavaVM(&jvm);

    // J-02: 每实例独立的错误回调上下文。g_logx_class/mid_on_error 是类级
    // 常量(GetStaticMethodID 结果进程内稳定),在此安全复用;实例间零共享可变状态。
    JniErrorContext* err_ctx = nullptr;
    if (jvm) {
        if (!g_logx_class) {
            jclass localLogX = env->FindClass("com/zzlfeng/logx/LogX");
            if (localLogX) {
                jmethodID mid = env->GetStaticMethodID(localLogX, "onNativeError", "(IILjava/lang/String;)V");
                if (mid) {
                    g_logx_class = static_cast<jclass>(env->NewGlobalRef(localLogX));
                    g_mid_on_error = mid;
                } else {
                    // J-7: 获取失败必须清 pending 异常——否则 nativeInit 返回后 JVM
                    // 向 Kotlin 抛 NoSuchMethodError。且不置 g_logx_class, 令下次
                    // nativeInit 重试(原实现 mid 永久 null → 错误事件永久静默丢失)。
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    __android_log_print(ANDROID_LOG_WARN, LOGX_TAG,
                                        "onNativeError not found; error events disabled");
                }
                env->DeleteLocalRef(localLogX);
            } else {
                if (env->ExceptionCheck()) env->ExceptionClear();
                __android_log_print(ANDROID_LOG_WARN, LOGX_TAG,
                                    "FindClass(LogX) failed; error events disabled");
            }
        }
        if (g_logx_class && g_mid_on_error) {
            err_ctx = new JniErrorContext{jvm, g_logx_class, g_mid_on_error};
        }
    }

    if (jKeyProvider && jvm) {
        jclass kpClass = env->GetObjectClass(jKeyProvider);
        jmethodID mid = env->GetMethodID(kpClass, "getKey", "(I)[B");
        if (mid) {
            kp_ctx = new JniKeyProviderContext();
            kp_ctx->jvm = jvm;
            kp_ctx->g_provider = env->NewGlobalRef(jKeyProvider);
            kp_ctx->mid_get_key = mid;
            kp_ctx->gen = g_jni_ctx_generation.fetch_add(1, std::memory_order_relaxed);
        }
    }

    logx_config_t cfg{};
    cfg.log_dir = log_dir;
    cfg.staging_path = staging_path;
    cfg.max_log_length = maxLogLength > 0 ? static_cast<uint32_t>(maxLogLength) : (64 * 1024);
    cfg.retention_days = retentionDays > 0 ? static_cast<uint32_t>(retentionDays) : 7;
    cfg.max_file_size = maxFileSize > 0 ? static_cast<uint64_t>(maxFileSize) : (10ULL * 1024 * 1024);
    // E-06: Kotlin Int 无法表达 C ABI 的 UINT32_MAX 哨兵,JNI 层约定
    // flushThreshold == -1 表示"取默认 8KB";0 直传 core = 禁用阈值自动唤醒。
    cfg.flush_threshold = (flushThreshold == -1)
        ? UINT32_MAX
        : static_cast<uint32_t>(flushThreshold);
    cfg.staging_wait_ms = stagingWaitMs > 0 ? static_cast<uint32_t>(stagingWaitMs) : 0;
    // compressAlgo 直传:Kotlin Compress 枚举(NONE=0/ZSTD=1/ZLIB=2)与 core 的
    // logx::kAlgoNone/kAlgoZstd/kAlgoZlib 严格一一对应,logx_open 内 fail-fast 校验;
    // 落盘时由 make_flag() 写入块头 flag 字节 bit1..2。
    cfg.compress_algo = static_cast<uint8_t>(compressAlgo);
    cfg.encrypt_enable = static_cast<uint8_t>(encryptEnable);
    cfg.kid = static_cast<uint16_t>(kid);
    cfg.key_provider = kp_ctx ? jni_key_provider_trampoline : nullptr;
    cfg.user = kp_ctx;
    cfg.err_fn = jni_error_trampoline;
    cfg.err_user = err_ctx;   // J-02: 逐实例上下文,生命周期与实例同步
    cfg.on_destroy = jni_destroy_trampoline;

    logx_instance_t* inst = nullptr;
    logx_status_e rc = logx_open(&cfg, &inst);

    if (rc != LOGX_OK) {
        if (kp_ctx) {
            // R46 P3-08: open 期探活可能已在当前宿主线程 TLS 填入密钥，open 失败时立即清零并登记水位
            logx_jni::advance_destroyed_watermark(g_jni_destroyed_gen_watermark, kp_ctx->gen);
            g_tls_key_cache.invalidate_owner(kp_ctx, kp_ctx->gen);
            env->DeleteGlobalRef(kp_ctx->g_provider);
            delete kp_ctx;
        }
        delete err_ctx;   // J-02: 实例创建失败,回收本实例的错误上下文
        return 0;
    }
    return reinterpret_cast<jlong>(inst);
}

JNIEXPORT void JNICALL
Java_com_zzlfeng_logx_LogX_nativeClose(JNIEnv* /*env*/, jclass /*clazz*/, jlong handle) {
    if (handle == 0) return;
    auto* inst = reinterpret_cast<logx_instance_t*>(handle);

    // J-1/F-2: 关闭实例。若为同步销毁, destroy_instance 内部已通过
    // cfg.on_destroy (jni_destroy_trampoline) 彻底释放 kp_ctx 与 inst_err_ctx;
    // 若为处于 Flusher 线程内部或活跃调用栈中的递延销毁, on_destroy 将递延至
    // Flusher 线程退出或 OpGuard 终结时刻安全触发, 此处绝不提前释放上下文(杜绝 UAF 与悬挂指针)。
    logx_close(inst);

    // R46 P3-08: 无论同步销毁(可能已在异线程完成)还是递延销毁，顺手按当前已销毁代数水位
    // 显式清扫调用 nativeClose 的宿主线程自身 TLS 密钥槽，收窄宿主线程空闲时的明文驻留窗口。
    g_tls_key_cache.scrub_by_watermark(g_jni_destroyed_gen_watermark.load(std::memory_order_acquire));
}

JNIEXPORT jint JNICALL
Java_com_zzlfeng_logx_LogX_nativeWrite(JNIEnv* env, jclass /*clazz*/, jlong handle,
                                       jint level, jstring jTag,
                                       jbyteArray jPayload) {
    if (!handle || !jTag) return LOGX_ERR_INVALID_ARG;
    auto* inst = reinterpret_cast<logx_instance_t*>(handle);

    // 1) Tag 读取: 栈缓冲区 + GetStringUTFRegion 避免堆分配;
    // J-3: JNI 产出的是 Modified UTF-8(CESU-8), 需就地转码为 core 契约的标准
    // UTF-8(补充平面字符 6→4 字节;输出恒 ≤ 输入, 就地写安全)。超长 tag 走
    // GetStringUTFChars 分支时 JVM 缓冲只读, 故先拷入 std::string 再转码。
    char tag_buf[128];
    std::string tag_heap;
    const char* tag_ptr = tag_buf;
    jsize tag_len = env->GetStringLength(jTag);
    if (tag_len < static_cast<jsize>(sizeof(tag_buf) / 3)) {
        jsize utf8_len = env->GetStringUTFLength(jTag);
        if (utf8_len < static_cast<jsize>(sizeof(tag_buf))) {
            env->GetStringUTFRegion(jTag, 0, tag_len, tag_buf);
            size_t nl = logx_jni::cesu8_to_utf8_inplace(tag_buf, static_cast<size_t>(utf8_len));
            tag_buf[nl] = '\0';
            tag_ptr = tag_buf;
        } else {
            // J-10: 失败(如 OOM)清 pending 异常并返回错误码, 不让异常穿透到 Kotlin。
            const char* tmp = env->GetStringUTFChars(jTag, nullptr);
            if (!tmp) {
                if (env->ExceptionCheck()) env->ExceptionClear();
                return LOGX_ERR_NOMEM;
            }
            tag_heap.assign(tmp, static_cast<size_t>(utf8_len));
            env->ReleaseStringUTFChars(jTag, tmp);
            size_t nl = logx_jni::cesu8_to_utf8_inplace(&tag_heap[0], tag_heap.size());
            tag_heap.resize(nl);
            tag_ptr = tag_heap.c_str();
        }
    } else {
        const char* tmp = env->GetStringUTFChars(jTag, nullptr);
        if (!tmp) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return LOGX_ERR_NOMEM;
        }
        tag_heap.assign(tmp, static_cast<size_t>(env->GetStringUTFLength(jTag)));
        env->ReleaseStringUTFChars(jTag, tmp);
        size_t nl = logx_jni::cesu8_to_utf8_inplace(&tag_heap[0], tag_heap.size());
        tag_heap.resize(nl);
        tag_ptr = tag_heap.c_str();
    }

    // 2) Payload 读取: 优先使用栈缓冲 + GetByteArrayRegion 避免堆分配与 GC 阻塞。
    // [CheckJNI 安全保障]: 严禁使用 GetPrimitiveArrayCritical。当 staging 环形缓冲写满(kFull)、
    // 日志超长或入参非法时, logx_write 会同步调用 emit_event 触发 jni_error_trampoline,
    // 进而执行 make_jstring_utf16 (env->NewString) 和 env->CallStaticVoidMethod。
    // 在持有 Critical 临界区期间执行任何 JNI 调用都会触发 CheckJNI 的致命崩溃:
    // "using JNI after critical get in call to NewString"。
    // 改用栈缓冲(4KB, 覆盖 99.9% 常规日志)配合 thread_local 缓冲(>4KB 时按需
    // 扩至 64KB, 与 core kMaxRecordTotal 上限一致; 超出部分截断, core 亦会拒绝),
    // 既保持热路径零堆分配(Zero Heap Allocation)与零 GC 抖动, 又彻底杜绝 JNI 临界区冲突与 GC 阻塞风险。
    constexpr jsize kStackPayloadSize = 4096;
    constexpr jsize kMaxPayloadCapacity = 64 * 1024;
    uint8_t stack_buf[kStackPayloadSize];
    static thread_local std::vector<uint8_t> tls_payload_buf;

    jsize n = jPayload ? env->GetArrayLength(jPayload) : 0;
    const uint8_t* payload_ptr = nullptr;
    uint32_t safe_payload_len = 0;
    if (n > 0) {
        if (n <= kStackPayloadSize) {
            payload_ptr = stack_buf;
            safe_payload_len = static_cast<uint32_t>(n);
            env->GetByteArrayRegion(jPayload, 0, n, reinterpret_cast<jbyte*>(stack_buf));
        } else {
            // 物理上限防卫: 单条日志 >64KB 在 core 中必被拒, 拷贝长度有界收敛, 防超大数组致 OOM
            jsize copy_len = (n <= kMaxPayloadCapacity) ? n : kMaxPayloadCapacity;
            if (tls_payload_buf.size() < static_cast<size_t>(copy_len)) {
                tls_payload_buf.resize(static_cast<size_t>(copy_len));
            }
            payload_ptr = tls_payload_buf.data();
            safe_payload_len = static_cast<uint32_t>(copy_len);
            env->GetByteArrayRegion(jPayload, 0, copy_len, reinterpret_cast<jbyte*>(tls_payload_buf.data()));
        }
    }

    // P2-2: 传参长度与实际缓冲区安全长度严格对齐, 杜绝缓冲区与长度不符导致的潜在越界读
    logx_status_e wrc = logx_write(inst, level, tag_ptr, payload_ptr,
                                   payload_ptr ? safe_payload_len : 0);

    // T5.4: consoleEcho —— 仅镜像成功落盘的记录(logcat 与文件一一对应);
    // 丢弃(NO_SPACE/TOO_LARGE)不 echo,由 ErrorHandler.DROP 事件通道上报。
    char echo_buf[2048];
    bool need_echo = (wrc == LOGX_OK && g_console_echo.load(std::memory_order_relaxed));
    if (need_echo) {
        uint32_t copy = (safe_payload_len > 0 && payload_ptr)
            ? (safe_payload_len < (sizeof(echo_buf) - 1) ? safe_payload_len : static_cast<uint32_t>(sizeof(echo_buf) - 1))
            : 0;
        if (copy > 0) {
            std::memcpy(echo_buf, payload_ptr, copy);
        }
        echo_buf[copy] = '\0';
        console_echo(level, tag_ptr, echo_buf);
    }

    // J-3: tag_ptr 现恒指向 tag_buf 或 tag_heap(均为本地内存), JVM 只读缓冲已即时释放,
    // 无需再 ReleaseStringUTFChars。
    return static_cast<jint>(wrc);
}

JNIEXPORT jint JNICALL
Java_com_zzlfeng_logx_LogX_nativeFlush(JNIEnv* /*env*/, jclass /*clazz*/, jlong handle) {
    if (handle == 0) return LOGX_ERR_STATE;
    return static_cast<jint>(logx_flush(reinterpret_cast<logx_instance_t*>(handle)));
}

JNIEXPORT jint JNICALL
Java_com_zzlfeng_logx_LogX_nativeExport(JNIEnv* env, jclass /*clazz*/, jlong handle,
                                        jstring jFrom, jstring jTo, jobject jCallback) {
    if (!handle || !jFrom || !jTo || !jCallback) return -1;
    auto* inst = reinterpret_cast<logx_instance_t*>(handle);

    jclass cbClass = env->GetObjectClass(jCallback);
    if (!cbClass) return -1;
    jmethodID mid = env->GetMethodID(cbClass, "onExport", "(Ljava/lang/String;Ljava/lang/String;J)I");
    env->DeleteLocalRef(cbClass);  // mid 为方法 id, 不受 local ref 删除影响
    if (!mid) {
        // J-11: GetMethodID 失败(签名不匹配)会置 pending 异常, 必须清除——
        // 否则异常穿透回 Kotlin, 调用方拿到 NoSuchMethodError 而非 -1。
        if (env->ExceptionCheck()) env->ExceptionClear();
        return -1;
    }

    // J-3: 日期范围按标准 UTF-8 契约入 core;J-10: 读取失败清异常并返回 -1。
    std::string from_s, to_s;
    if (!jstring_to_utf8(env, jFrom, &from_s) || !jstring_to_utf8(env, jTo, &to_s)) {
        return -1;
    }

    ExportContext ctx{env, jCallback, mid};
    logx_status_e rc = logx_export(inst, from_s.c_str(), to_s.c_str(), export_trampoline, &ctx);

    return static_cast<jint>(rc);
}

// 诊断统计:读取 core 原子计数器并以 long[] 返回给 Kotlin。
// 返回 6 元素数组 [dropNoSpace, dropTooLarge, truncatedBlocks, padRecords, blockWriteErrors,
// truncatedLogs],与 logx_stats_t 非 reserved 字段一一对应(truncatedLogs 为 R4-2
// 接线的 B-1B 超长截断保前缀计数);reserved 区保持全零契约(E-07),不上传。
// 使用 jlongArray 而非构造 Java 对象:JNI 侧零反射、零 jclass 缓存,Kotlin 侧
// 由 Stats data class 构造函数解包,两端各司其职。
JNIEXPORT jlongArray JNICALL
Java_com_zzlfeng_logx_LogX_nativeGetStats(JNIEnv* env, jclass /*clazz*/, jlong handle) {
    if (!handle) return nullptr;
    auto* inst = reinterpret_cast<logx_instance_t*>(handle);

    logx_stats_t st{};
    logx_get_stats(inst, &st);

    jlongArray arr = env->NewLongArray(6);
    if (!arr) return nullptr;  // OOM

    jlong buf[6] = {
        static_cast<jlong>(st.drop_no_space),
        static_cast<jlong>(st.drop_too_large),
        static_cast<jlong>(st.truncated_blocks),
        static_cast<jlong>(st.pad_records),
        static_cast<jlong>(st.block_write_errors),
        static_cast<jlong>(st.truncated_logs),
    };
    env->SetLongArrayRegion(arr, 0, 6, buf);
    return arr;
}

}  // extern "C"