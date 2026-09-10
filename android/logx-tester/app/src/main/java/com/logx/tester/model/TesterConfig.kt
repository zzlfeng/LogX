package com.logx.tester.model

import android.content.Context
import com.zzlfeng.logx.LogX
import com.logx.tester.engine.TestKeys
import java.io.File

enum class KeyProviderMode(val label: String) {
    BUILTIN_32B("内置固定32B"),
    CUSTOM_HEX("自定义hex(64字符)"),
    RETURN_NULL("返回null (降级测试)"),
    WRONG_LEN_16B("返回错误长度(16B)"),
    NONE_NULL("不提供 (null)")
}

data class TesterConfig(
    val logDirName: String = "logs",
    val stagingDirName: String = "logx_staging",
    val useCacheDir: Boolean = false, // 专门用于负向测试触发 require
    val minLevelGate: LogX.Level = LogX.Level.V, // App侧门控
    val retentionDays: Int = 7,
    val maxFileSize: Long = 10L * 1024 * 1024,
    val maxLogLength: Int = 64 * 1024,
    val flushThreshold: Int = 8 * 1024,
    val compress: LogX.Compress = LogX.Compress.NONE,
    val encrypt: Boolean = false,
    val kid: Int = 1,
    val keyProviderMode: KeyProviderMode = KeyProviderMode.BUILTIN_32B,
    val customKeyHex: String = TestKeys.DEFAULT_KEY_HEX,
    val autoProcessIsolation: Boolean = true,
    val consoleEcho: Boolean = false
) {
    fun toLogXConfig(ctx: Context, errorHandler: LogX.ErrorHandler): LogX.Config {
        val baseStagingDir = if (useCacheDir) ctx.cacheDir else ctx.filesDir
        val logDir = File(ctx.filesDir, logDirName)
        val stagingDir = File(baseStagingDir, stagingDirName)

        val provider: LogX.KeyProvider? = when (keyProviderMode) {
            KeyProviderMode.BUILTIN_32B -> LogX.KeyProvider { TestKeys.BUILTIN_KEY_BYTES }
            KeyProviderMode.CUSTOM_HEX -> {
                val bytes = TestKeys.hexToBytes(customKeyHex)
                LogX.KeyProvider { bytes }
            }
            KeyProviderMode.RETURN_NULL -> LogX.KeyProvider { null }
            KeyProviderMode.WRONG_LEN_16B -> LogX.KeyProvider { ByteArray(16) }
            KeyProviderMode.NONE_NULL -> null
        }

        return LogX.Config(
            logDir = logDir,
            stagingDir = stagingDir,
            level = minLevelGate,
            retentionDays = retentionDays,
            maxFileSize = maxFileSize,
            maxLogLength = maxLogLength,
            flushThreshold = flushThreshold,
            compress = compress,
            encrypt = encrypt,
            kid = kid,
            keyProvider = provider,
            errorHandler = errorHandler,
            autoProcessIsolation = autoProcessIsolation,
            consoleEcho = consoleEcho
        )
    }
}

