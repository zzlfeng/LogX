// logx_jni.cpp - JNI 胶水。零逻辑,只做转发(参见 spec §10)。
//
// Kotlin facade LogX.kt 调用本文件暴露的 native 方法;C ABI 由 core 提供,
// JNI 只在两者之间做参数转换 / 字符串复制 / 异常抛出。
//
// 性能要点:
//   - 单条日志路径只走一次 GetStringUTFChars + ReleaseStringUTFChars;
//     tag 长度上限 64B,UTF-8 转换本身非常便宜。
//   - keyProvider 回调里不能重入 LogX(JNI 调用栈已在 native,死锁风险)。
//   - 错误回调 / export 回调运行在 native 线程,直接回调 Kotlin 时
//     需要 env->AttachCurrentThread;此处仅做接口,实际接入阶段再做。

#include <jni.h>
#include <android/log.h>

#include "logx_c.h"

#include <cstring>
#include <string>

#define LOGX_TAG "LogX"
#define LOGX_LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOGX_TAG, __VA_ARGS__)
#define LOGX_LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOGX_TAG, __VA_ARGS__)

namespace {

// keyProvider trampoline 接入点在生产实现里补齐:JavaVM::AttachCurrentThread
// 拿到 JNIEnv,再回调 Kotlin 的 KeyProvider.invoke(kid): ByteArray。
// 当前 stub 留 TODO,以免接口评审阶段被误用。

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
                                      jint kid, jobject keyProvider) {
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
    // 转发到 Kotlin 的 KeyProvider.invoke(kid): ByteArray。这里留 TODO。
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
    logx_close(reinterpret_cast<logx_instance_t*>(handle));
}

JNIEXPORT void JNICALL
Java_com_example_logx_LogX_nativeWrite(JNIEnv* env, jclass /*clazz*/, jlong handle,
                                      jint level, jstring jTag,
                                      jbyteArray jPayload) {
    const char* tag = env->GetStringUTFChars(jTag, nullptr);
    jsize n = jPayload ? env->GetArrayLength(jPayload) : 0;
    jbyte* payload = nullptr;
    if (n > 0) payload = env->GetByteArrayElements(jPayload, nullptr);
    logx_write(reinterpret_cast<logx_instance_t*>(handle), level, tag,
               reinterpret_cast<uint8_t*>(payload),
               payload ? static_cast<uint32_t>(n) : 0);
    if (payload) env->ReleaseByteArrayElements(jPayload, payload, JNI_ABORT);
    env->ReleaseStringUTFChars(jTag, tag);
}

JNIEXPORT void JNICALL
Java_com_example_logx_LogX_nativeFlush(JNIEnv* /*env*/, jclass /*clazz*/, jlong handle) {
    logx_flush(reinterpret_cast<logx_instance_t*>(handle));
}

}  // extern "C"