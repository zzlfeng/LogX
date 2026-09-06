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

#include <cstring>
#include <string>
#include <vector>

#define LOGX_TAG "LogX"
#define LOGX_LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOGX_TAG, __VA_ARGS__)
#define LOGX_LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOGX_TAG, __VA_ARGS__)

namespace {

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

static JniErrorContext g_err_ctx;

static void jni_error_trampoline(void* /*user*/, logx_event_kind_e kind, int code, const char* msg) {
    if (!g_err_ctx.jvm || !g_err_ctx.g_logx_class || !g_err_ctx.mid_on_error) {
        return;
    }
    JNIEnv* env = nullptr;
    bool need_detach = false;
    jint getEnvRes = g_err_ctx.jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvRes == JNI_EDETACHED) {
#if defined(__ANDROID__)
        if (g_err_ctx.jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            need_detach = true;
        }
#else
        if (g_err_ctx.jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) == JNI_OK) {
            need_detach = true;
        }
#endif
    } else if (getEnvRes != JNI_OK || !env) {
        return;
    }

    if (env) {
        jstring jMsg = env->NewStringUTF(msg ? msg : "");
        env->CallStaticVoidMethod(g_err_ctx.g_logx_class, g_err_ctx.mid_on_error,
                                  static_cast<jint>(kind), static_cast<jint>(code), jMsg);
        env->DeleteLocalRef(jMsg);
        if (need_detach) {
            g_err_ctx.jvm->DetachCurrentThread();
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
    jstring jDate = env->NewStringUTF(date ? date : "");
    jstring jPath = env->NewStringUTF(plaintext_path ? plaintext_path : "");

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
//       Object keyProvider);   // returns native handle
JNIEXPORT jlong JNICALL
Java_com_example_logx_LogX_nativeInit(JNIEnv* env, jclass /*clazz*/,
                                      jstring jLogDir, jstring jStagingPath,
                                      jint compressAlgo, jint encryptEnable,
                                      jint kid, jobject jKeyProvider,
                                      jint retentionDays, jlong maxFileSize,
                                      jint maxLogLength, jint flushThreshold) {
    if (!jLogDir || !jStagingPath) return 0;

    const char* log_dir = env->GetStringUTFChars(jLogDir, nullptr);
    const char* staging_path = env->GetStringUTFChars(jStagingPath, nullptr);

    JniKeyProviderContext* kp_ctx = nullptr;
    JavaVM* jvm = nullptr;
    env->GetJavaVM(&jvm);

    if (jvm && !g_err_ctx.g_logx_class) {
        jclass localLogX = env->FindClass("com/example/logx/LogX");
        if (localLogX) {
            g_err_ctx.jvm = jvm;
            g_err_ctx.g_logx_class = static_cast<jclass>(env->NewGlobalRef(localLogX));
            g_err_ctx.mid_on_error = env->GetStaticMethodID(localLogX, "onNativeError", "(IILjava/lang/String;)V");
            env->DeleteLocalRef(localLogX);
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
    cfg.flush_threshold = flushThreshold > 0 ? static_cast<uint32_t>(flushThreshold) : (8 * 1024);
    // compressAlgo 直传:Kotlin Compress 枚举(NONE=0/ZSTD=1/ZLIB=2)与 core 的
    // logx::kAlgoNone/kAlgoZstd/kAlgoZlib 严格一一对应,logx_open 内 fail-fast 校验;
    // 落盘时由 make_flag() 写入块头 flag 字节 bit1..2。
    cfg.compress_algo = static_cast<uint8_t>(compressAlgo);
    cfg.encrypt_enable = static_cast<uint8_t>(encryptEnable);
    cfg.kid = static_cast<uint16_t>(kid);
    cfg.key_provider = kp_ctx ? jni_key_provider_trampoline : nullptr;
    cfg.user = kp_ctx;
    cfg.err_fn = jni_error_trampoline;
    cfg.err_user = nullptr;

    logx_instance_t* inst = nullptr;
    logx_status_e rc = logx_open(&cfg, &inst);

    env->ReleaseStringUTFChars(jLogDir, log_dir);
    env->ReleaseStringUTFChars(jStagingPath, staging_path);
    if (rc != LOGX_OK) {
        if (kp_ctx) {
            env->DeleteGlobalRef(kp_ctx->g_provider);
            delete kp_ctx;
        }
        return 0;
    }
    return reinterpret_cast<jlong>(inst);
}

JNIEXPORT void JNICALL
Java_com_example_logx_LogX_nativeClose(JNIEnv* env, jclass /*clazz*/, jlong handle) {
    if (handle != 0) {
        auto* inst = reinterpret_cast<logx_instance_t*>(handle);
        auto* kp_ctx = static_cast<JniKeyProviderContext*>(inst->key_user);
        if (kp_ctx) {
            if (kp_ctx->g_provider) {
                env->DeleteGlobalRef(kp_ctx->g_provider);
            }
            delete kp_ctx;
            inst->key_user = nullptr;
        }
        if (g_err_ctx.g_logx_class) {
            env->DeleteGlobalRef(g_err_ctx.g_logx_class);
            g_err_ctx.g_logx_class = nullptr;
            g_err_ctx.mid_on_error = nullptr;
            g_err_ctx.jvm = nullptr;
        }
        logx_close(inst);
    }
}

JNIEXPORT void JNICALL
Java_com_example_logx_LogX_nativeWrite(JNIEnv* env, jclass /*clazz*/, jlong handle,
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

    logx_write(inst, level, tag_ptr, payload_ptr, payload_ptr ? static_cast<uint32_t>(n) : 0);

    if (payload_ptr) {
        env->ReleasePrimitiveArrayCritical(jPayload, const_cast<uint8_t*>(payload_ptr), JNI_ABORT);
    }
    if (tag_ptr != tag_buf) {
        env->ReleaseStringUTFChars(jTag, tag_ptr);
    }
}

JNIEXPORT void JNICALL
Java_com_example_logx_LogX_nativeFlush(JNIEnv* /*env*/, jclass /*clazz*/, jlong handle) {
    if (handle != 0) {
        logx_flush(reinterpret_cast<logx_instance_t*>(handle));
    }
}

JNIEXPORT jint JNICALL
Java_com_example_logx_LogX_nativeExport(JNIEnv* env, jclass /*clazz*/, jlong handle,
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

}  // extern "C"