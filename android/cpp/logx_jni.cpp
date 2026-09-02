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

#include <cstring>
#include <string>
#include <vector>

#define LOGX_TAG "LogX"
#define LOGX_LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOGX_TAG, __VA_ARGS__)
#define LOGX_LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOGX_TAG, __VA_ARGS__)

namespace {

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
                                      jint kid, jobject /*keyProvider*/) {
    if (!jLogDir || !jStagingPath) return 0;

    const char* log_dir = env->GetStringUTFChars(jLogDir, nullptr);
    const char* staging_path = env->GetStringUTFChars(jStagingPath, nullptr);

    logx_config_t cfg{};
    cfg.log_dir = log_dir;
    cfg.staging_path = staging_path;
    cfg.max_log_length = 64 * 1024;     // spec §7 默认
    cfg.retention_days = 7;
    cfg.max_file_size = 10ULL * 1024 * 1024;
    cfg.flush_threshold = 8 * 1024;
    cfg.compress_algo = static_cast<uint8_t>(compressAlgo);
    cfg.encrypt_enable = static_cast<uint8_t>(encryptEnable);
    cfg.kid = static_cast<uint16_t>(kid);
    // keyProvider 在生产实现里应通过 JavaVM 全局引用 + JNI trampoline
    // 转发到 Kotlin 的 KeyProvider.invoke(kid): ByteArray。
    cfg.key_provider = nullptr;
    cfg.user = nullptr;

    logx_instance_t* inst = nullptr;
    logx_status_e rc = logx_open(&cfg, &inst);

    env->ReleaseStringUTFChars(jLogDir, log_dir);
    env->ReleaseStringUTFChars(jStagingPath, staging_path);
    if (rc != LOGX_OK) {
        return 0;
    }
    return reinterpret_cast<jlong>(inst);
}

JNIEXPORT void JNICALL
Java_com_example_logx_LogX_nativeClose(JNIEnv* /*env*/, jclass /*clazz*/, jlong handle) {
    if (handle != 0) {
        logx_close(reinterpret_cast<logx_instance_t*>(handle));
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