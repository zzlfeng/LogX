package com.example.logx

import android.content.Context
import java.io.File
import java.util.concurrent.atomic.AtomicLong

/**
 * LogX Kotlin Facade.
 *
 * 与 spec §7 的 Kotlin API 对齐;JNI 层只做转发,本类负责:
 *   - 参数装/拆(目录、压缩算法、加密开关、KID、KeyProvider 桥)
 *   - 进程内单例 + 重复 init 抛 IllegalStateException
 *   - export 临时文件路径(filesDir 系,禁 cacheDir 与外部存储)
 *
 * 注意:
 *   - 单条日志上限 64KB(格式化后);超过被丢弃 + 打点。
 *   - 首次 init 之前调用任何写接口一律丢弃 + 计数。
 *   - KeyProvider 返回 null → 该 KID 不可解,export 跳过该文件但不中断。
 */
object LogX {

    enum class Compress(val algo: Byte) {
        NONE(0), ZSTD(1), ZLIB(2)
    }
    enum class Level(val v: Int) { V(0), D(1), I(2), W(3), E(4), F(5) }

    enum class EventKind { DEGRADED, DROP, IO, RECOVERY }
    fun interface ErrorHandler {
        fun onError(kind: EventKind, code: Int, message: String)
    }
    fun interface KeyProvider {
        /** 返回 32B 裸密钥;null = 该 KID 不可解。不得重入 LogX。 */
        fun getKey(kid: Int): ByteArray?
    }
    fun interface ExportCallback {
        /** 返回 0 = 已成功接收并取走;非 0 = 业务侧处理失败。 */
        fun onExport(date: String, plaintextPath: String, size: Long): Int
    }

    data class Config(
        val logDir: File,
        val stagingDir: File,           // 单 staging 文件名固定 = logx_staging.bin
        val level: Level = Level.I,
        val retentionDays: Int = 7,
        val maxFileSize: Long = 10L * 1024 * 1024,
        val maxLogLength: Int = 64 * 1024,
        val flushThreshold: Int = 8 * 1024,
        val compress: Compress = Compress.NONE,
        val encrypt: Boolean = false,
        val kid: Int = 1,
        val keyProvider: KeyProvider? = null,
    )

    @Volatile private var nativeHandle: Long = 0L
    @Volatile private var errorHandler: ErrorHandler? = null
    private val initCount = AtomicLong(0)

    @Synchronized
    fun init(context: Context, config: Config) {
        if (initCount.incrementAndGet() != 1L) {
            // 重复 init:抛 IllegalStateException(参见 spec §7)。
            throw IllegalStateException("LogX.init called more than once")
        }
        val logDir = config.logDir.apply { mkdirs() }
        val stagingDir = config.stagingDir.apply { mkdirs() }
        val stagingFile = File(stagingDir, "logx_staging.bin")
        // staging 必须 filesDir 系;禁 cacheDir(系统清理会让 mmap 变孤儿,见 spec §10)
        require(!stagingDir.absolutePath.contains("/cache/")) {
            "staging dir must not be in cacheDir"
        }
        require(config.maxLogLength < 128 * 1024 - 128 - 1) {
            "maxLogLength must be < (dataSize - 1) = 130943"
        }
        nativeHandle = nativeInit(
            logDir.absolutePath,
            stagingFile.absolutePath,
            config.compress.algo.toInt(),
            if (config.encrypt) 1 else 0,
            config.kid,
            // KeyProvider 桥接:传 null,M3 阶段补全(JNI 全局引用 + trampoline)。
            null,
        )
        if (nativeHandle == 0L) {
            initCount.set(0)
            throw IllegalStateException("LogX native init failed")
        }
    }

    fun setErrorHandler(handler: ErrorHandler?) { errorHandler = handler }

    fun v(tag: String, msg: String) = log(Level.V, tag, msg.toByteArray())
    fun d(tag: String, msg: String) = log(Level.D, tag, msg.toByteArray())
    fun i(tag: String, msg: String) = log(Level.I, tag, msg.toByteArray())
    fun w(tag: String, msg: String) = log(Level.W, tag, msg.toByteArray())
    fun e(tag: String, msg: String) = log(Level.E, tag, msg.toByteArray())
    fun f(tag: String, msg: String) = log(Level.F, tag, msg.toByteArray())

    fun log(level: Level, tag: String, payload: ByteArray) {
        val h = nativeHandle
        if (h == 0L) return  // pre-init 调用:丢弃 + 原子计数(参见 spec §7)
        nativeWrite(h, level.v, tag, payload)
    }

    /** 阻塞至 staging 全部落块提交。退后台 / onStop 调用。 */
    fun flush() {
        val h = nativeHandle; if (h == 0L) return
        nativeFlush(h)
    }

    /**
     * 按日期范围导出。from/to = yyyyMMdd 闭区间;明文临时文件写在 filesDir 下独立
     * 临时目录(禁外部存储、禁 cacheDir);回调返回或抛出后立即删除。
     */
    fun export(from: String, to: String, cb: ExportCallback): Int {
        val h = nativeHandle; if (h == 0L) return -1
        return nativeExport(h, from, to, cb)
    }

    // ---- native 绑定(JNI 实现见 android/cpp/logx_jni.cpp) ----
    @JvmStatic private external fun nativeInit(
        logDir: String, stagingPath: String,
        compressAlgo: Int, encryptEnable: Int, kid: Int, keyProvider: Any?,
    ): Long
    @JvmStatic private external fun nativeClose(handle: Long)
    @JvmStatic private external fun nativeWrite(handle: Long, level: Int,
                                                 tag: String, payload: ByteArray)
    @JvmStatic private external fun nativeFlush(handle: Long)
    @JvmStatic private external fun nativeExport(handle: Long, from: String, to: String,
                                                 cb: ExportCallback): Int

    fun close() {
        val h = nativeHandle
        if (h != 0L) {
            nativeClose(h)
            nativeHandle = 0L
            initCount.set(0)
        }
    }
}