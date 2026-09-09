// logx_jni.cpp - JNI 胶水。零逻辑,只做转发(参见 spec §10)。
//
// Kotlin facade LogX.kt 调用本文件暴露的 native 方法;C ABI 由 core 提供,
// JNI 只在两者之间做参数转换 / 字符串复制 / 异常抛出。
//
// 性能要点:
//   - 单条日志路径使用 GetStringUTFRegion 栈缓冲读取 tag,
//     搭配 GetPrimitiveArrayCritical 读取 payload, 实现 JNI 边界零堆内存分配 (Zero Heap Allocation)。
//   - keyProvider 回调里不能重入 LogX(JNI 调用栈已在 native,死锁风险)。
//   - 错误回调 / export 回调运行在调用线程,直接回调 Kotlin。

#include <jni.h>
#include <android/log.h>

#include "logx_c.h"
#include "logx_internal.h"
#include "jni_string_util.h"

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
// 2) [JNI 临界区安全]: 必须在 ReleasePrimitiveArrayCritical 之后调用,
//    绝不持有 Critical 进行 IPC 系统调用,防止拖慢 JVM GC 或引发死锁;
// 3) [格式串安全]: 统一使用 "%s" 格式化输出,防 payload 包含 % 字符导致崩溃。
void console_echo(int level, const char* tag, const char* msg) {
    const int priority = logx_level_to_priority(level);
    const char* log_tag = (tag && tag[0] != '\0') ? tag : LOGX_TAG;
    __android_log_print(priority, log_tag, "%s", msg ? msg : "");
}

struct JniKeyProviderContext {
    JavaVM* jvm = nullptr;
    jobject g_provider = nullptr;
    jmethodID mid_get_key = nullptr;
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

// J-02: 错误回调上下文按实例持有(经 cfg.err_user 传入 core,由 emit_event
// 回传),不再使用全局静态 g_err_ctx —— 多实例场景下任一实例关闭只影响自身,
// 其余实例的回调路径不受干扰;生命周期与实例严格同步。
static void jni_error_trampoline(void* user, logx_event_kind_e kind, int code, const char* msg) {
    auto* ctx = static_cast<const JniErrorContext*>(user);
    if (!ctx || !ctx->jvm || !ctx->g_logx_class || !ctx->mid_on_error) {
        return;
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
        env->DeleteLocalRef(jMsg);
        if (need_detach) {
            ctx->jvm->DetachCurrentThread();
        }
    }
}

static thread_local uint8_t g_tls_key_buf[32];

static const uint8_t* jni_key_provider_trampoline(uint16_t kid, void* user) {
    auto* ctx = static_cast<JniKeyProviderContext*>(user);
    if (!ctx || !ctx->jvm || !ctx->g_provider || !ctx->mid_get_key) {
        return nullptr;
    }

    JNIEnv* env = nullptr;
    jint getEnvRes = ctx->jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvRes == JNI_EDETACHED) {
#if defined(__ANDROID__)
        if (ctx->jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            return nullptr;
        }
#else
        if (ctx->jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) != JNI_OK) {
            return nullptr;
        }
#endif
    } else if (getEnvRes != JNI_OK || !env) {
        return nullptr;
    }

    jbyteArray jKeyArr = static_cast<jbyteArray>(
        env->CallObjectMethod(ctx->g_provider, ctx->mid_get_key, static_cast<jint>(kid))
    );

    const uint8_t* res = nullptr;
    if (jKeyArr) {
        jsize len = env->GetArrayLength(jKeyArr);
        if (len == 32) {
            env->GetByteArrayRegion(jKeyArr, 0, 32, reinterpret_cast<jbyte*>(g_tls_key_buf));
            res = g_tls_key_buf;
        }
        env->DeleteLocalRef(jKeyArr);
    }
    return res;
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

    jint res = env->CallIntMethod(ctx->cb, ctx->mid, jDate, jPath, static_cast<jlong>(size));

    env->DeleteLocalRef(jDate);
    env->DeleteLocalRef(jPath);
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
                                      jboolean jConsoleEcho) {
    if (!jLogDir || !jStagingPath) return 0;

    // T5.4: consoleEcho 开关在 init 时点定型(Kotlin facade 单实例契约下
    // 进程级存储无歧义;close→re-init 循环会重新走到这里刷新)。
    g_console_echo.store(jConsoleEcho == JNI_TRUE, std::memory_order_relaxed);

    const char* log_dir = env->GetStringUTFChars(jLogDir, nullptr);
    const char* staging_path = env->GetStringUTFChars(jStagingPath, nullptr);

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
                g_logx_class = static_cast<jclass>(env->NewGlobalRef(localLogX));
                g_mid_on_error = env->GetStaticMethodID(localLogX, "onNativeError", "(IILjava/lang/String;)V");
                env->DeleteLocalRef(localLogX);
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

    logx_instance_t* inst = nullptr;
    logx_status_e rc = logx_open(&cfg, &inst);

    env->ReleaseStringUTFChars(jLogDir, log_dir);
    env->ReleaseStringUTFChars(jStagingPath, staging_path);
    if (rc != LOGX_OK) {
        if (kp_ctx) {
            env->DeleteGlobalRef(kp_ctx->g_provider);
            delete kp_ctx;
        }
        delete err_ctx;   // J-02: 实例创建失败,回收本实例的错误上下文
        return 0;
    }
    return reinterpret_cast<jlong>(inst);
}

JNIEXPORT void JNICALL
Java_com_zzlfeng_logx_LogX_nativeClose(JNIEnv* env, jclass /*clazz*/, jlong handle) {
    if (handle == 0) return;
    auto* inst = reinterpret_cast<logx_instance_t*>(handle);

    // J-01: 只读出 kp_ctx,不释放、不置空 —— close 内部 flush_blocking 的
    // 最后一次 drain 仍需通过它取密钥(否则尾块静默降级明文)。
    auto* kp_ctx = static_cast<JniKeyProviderContext*>(inst->key_user);
    // J-02: 本实例的错误上下文同样在 close 完成前保持有效(关闭阶段事件不丢)。
    auto* inst_err_ctx = static_cast<JniErrorContext*>(inst->err_user);

    // 1) logx_close: flush_blocking(密钥全程可用) → join Flusher → delete inst
    logx_close(inst);

    // 2) Flusher 已 join, trampoline 不可能再被调用,现在释放才安全
    if (kp_ctx) {
        if (kp_ctx->g_provider) {
            env->DeleteGlobalRef(kp_ctx->g_provider);
        }
        delete kp_ctx;
    }

    // 3) J-02: 错误上下文随实例释放。g_logx_class 全局引用为类级常量,
    //    多实例共享,保留至进程退出,不在此清理(否则其余实例回调静默失效)。
    delete static_cast<JniErrorContext*>(inst_err_ctx);
}

JNIEXPORT void JNICALL
Java_com_zzlfeng_logx_LogX_nativeWrite(JNIEnv* env, jclass /*clazz*/, jlong handle,
                                       jint level, jstring jTag,
                                       jbyteArray jPayload) {
    if (!handle || !jTag) return;
    auto* inst = reinterpret_cast<logx_instance_t*>(handle);

    // 1) Tag 读取: 使用栈缓冲区 + GetStringUTFRegion 避免堆分配
    char tag_buf[128];
    const char* tag_ptr = tag_buf;
    jsize tag_len = env->GetStringLength(jTag);
    if (tag_len < static_cast<jsize>(sizeof(tag_buf) / 3)) {
        jsize utf8_len = env->GetStringUTFLength(jTag);
        if (utf8_len < static_cast<jsize>(sizeof(tag_buf))) {
            env->GetStringUTFRegion(jTag, 0, tag_len, tag_buf);
            tag_buf[utf8_len] = '\0';
        } else {
            tag_ptr = env->GetStringUTFChars(jTag, nullptr);
        }
    } else {
        tag_ptr = env->GetStringUTFChars(jTag, nullptr);
    }

    // 2) Payload 读取: 使用 GetPrimitiveArrayCritical 实现零拷贝/零堆分配
    jsize n = jPayload ? env->GetArrayLength(jPayload) : 0;
    const uint8_t* payload_ptr = nullptr;
    if (n > 0) {
        payload_ptr = reinterpret_cast<const uint8_t*>(env->GetPrimitiveArrayCritical(jPayload, nullptr));
    }

    logx_status_e wrc = logx_write(inst, level, tag_ptr, payload_ptr,
                                   payload_ptr ? static_cast<uint32_t>(n) : 0);

    // T5.4: consoleEcho —— 仅镜像成功落盘的记录(logcat 与文件一一对应);
    // 丢弃(NO_SPACE/TOO_LARGE)不 echo,由 ErrorHandler.DROP 事件通道上报。
    // [JNI 临界区安全]: 在 Critical 保护期内仅完成栈缓冲内存拷贝,
    // 拷贝后立即释放 Critical 解除 GC 阻塞,绝不在持有 Critical 时执行 logcat IPC。
    char echo_buf[2048];
    bool need_echo = (wrc == LOGX_OK && g_console_echo.load(std::memory_order_relaxed));
    if (need_echo) {
        uint32_t copy = (n > 0 && payload_ptr)
            ? (static_cast<uint32_t>(n) < (sizeof(echo_buf) - 1) ? static_cast<uint32_t>(n) : static_cast<uint32_t>(sizeof(echo_buf) - 1))
            : 0;
        if (copy > 0) {
            std::memcpy(echo_buf, payload_ptr, copy);
        }
        echo_buf[copy] = '\0';
    }

    if (payload_ptr) {
        env->ReleasePrimitiveArrayCritical(jPayload, const_cast<uint8_t*>(payload_ptr), JNI_ABORT);
    }

    // [Tag 修复 & 临界区外调用]: 在释放 Critical 之后调用 console_echo,
    // 传入业务指定的 tag_ptr (tag_ptr 此时仍有效, 在下方释放)。
    if (need_echo) {
        console_echo(level, tag_ptr, echo_buf);
    }

    if (tag_ptr != tag_buf) {
        env->ReleaseStringUTFChars(jTag, tag_ptr);
    }
}

JNIEXPORT void JNICALL
Java_com_zzlfeng_logx_LogX_nativeFlush(JNIEnv* /*env*/, jclass /*clazz*/, jlong handle) {
    if (handle != 0) {
        logx_flush(reinterpret_cast<logx_instance_t*>(handle));
    }
}

JNIEXPORT jint JNICALL
Java_com_zzlfeng_logx_LogX_nativeExport(JNIEnv* env, jclass /*clazz*/, jlong handle,
                                        jstring jFrom, jstring jTo, jobject jCallback) {
    if (!handle || !jFrom || !jTo || !jCallback) return -1;
    auto* inst = reinterpret_cast<logx_instance_t*>(handle);

    jclass cbClass = env->GetObjectClass(jCallback);
    jmethodID mid = env->GetMethodID(cbClass, "onExport", "(Ljava/lang/String;Ljava/lang/String;J)I");
    if (!mid) return -1;

    const char* from = env->GetStringUTFChars(jFrom, nullptr);
    const char* to = env->GetStringUTFChars(jTo, nullptr);

    ExportContext ctx{env, jCallback, mid};
    logx_status_e rc = logx_export(inst, from, to, export_trampoline, &ctx);

    env->ReleaseStringUTFChars(jFrom, from);
    env->ReleaseStringUTFChars(jTo, to);

    return (rc == LOGX_OK) ? 0 : static_cast<jint>(rc);
}

// 诊断统计:读取 core 原子计数器并以 long[] 返回给 Kotlin。
// 返回 5 元素数组 [dropNoSpace, dropTooLarge, truncatedBlocks, padRecords, blockWriteErrors],
// 与 logx_stats_t 非 reserved 字段一一对应;reserved 区保持全零契约(E-07),不上传。
// 使用 jlongArray 而非构造 Java 对象:JNI 侧零反射、零 jclass 缓存,Kotlin 侧
// 由 Stats data class 构造函数解包,两端各司其职。
JNIEXPORT jlongArray JNICALL
Java_com_zzlfeng_logx_LogX_nativeGetStats(JNIEnv* env, jclass /*clazz*/, jlong handle) {
    if (!handle) return nullptr;
    auto* inst = reinterpret_cast<logx_instance_t*>(handle);

    logx_stats_t st{};
    logx_get_stats(inst, &st);

    jlongArray arr = env->NewLongArray(5);
    if (!arr) return nullptr;  // OOM

    jlong buf[5] = {
        static_cast<jlong>(st.drop_no_space),
        static_cast<jlong>(st.drop_too_large),
        static_cast<jlong>(st.truncated_blocks),
        static_cast<jlong>(st.pad_records),
        static_cast<jlong>(st.block_write_errors),
    };
    env->SetLongArrayRegion(arr, 0, 5, buf);
    return arr;
}

}  // extern "C"