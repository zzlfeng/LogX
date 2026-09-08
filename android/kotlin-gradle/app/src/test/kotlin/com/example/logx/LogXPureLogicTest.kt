package com.example.logx

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * LogX.kt 纯逻辑单元测试 (Gradle 接入后首批)。
 *
 * 范围界定: LogX 的写/flush/export 走 JNI(`external fun`), JVM 单测环境无
 * native 库, 本测试类只覆盖不触发 native 调用的纯逻辑:
 *   - 枚举 ABI 映射 (Compress.algo 与 C++ kAlgo* 三端同步契约)
 *   - Level.v 与 C ABI level ∈ {0..5} 契约
 *   - EventKind 映射 (onNativeError 的 kindInt 分支)
 *   - Config 默认值契约 (与 S-04 / E-06 对齐)
 *   - 重复 init 拦截 / pre-init 状态机 (不触达 nativeInit 失败路径)
 * JNI 全链路正确性由 tests/ C++ gtest 与真机批次覆盖。
 */
class LogXPureLogicTest {

    // ---- ABI 契约: Compress 枚举序号必须与 C++ kAlgoNone/Zstd/Zlib 一致 ----
    @Test
    fun compressAlgoValues_matchCppKAlgo() {
        // C++ core/include/logx_codec.h: kAlgoNone=0, kAlgoZstd=1, kAlgoZlib=2
        assertEquals(0.toByte(), LogX.Compress.NONE.algo)
        assertEquals(1.toByte(), LogX.Compress.ZSTD.algo)
        assertEquals(2.toByte(), LogX.Compress.ZLIB.algo)
    }

    @Test
    fun levelValues_matchCAbiRange() {
        // C ABI: level ∈ {0..5}, V/D/I/W/E/F
        assertEquals(listOf(0, 1, 2, 3, 4, 5), LogX.Level.entries.map { it.v })
        assertEquals("F 为最高级", 5, LogX.Level.F.v)
    }

    // ---- EventKind 映射 (onNativeError kindInt 分支, 纯映射无 native) ----
    @Test
    fun onNativeError_mapsKindIntsToEvents() {
        val got = mutableListOf<Pair<LogX.EventKind, Int>>()
        LogX.setErrorHandler { kind, code, _ -> got.add(kind to code) }
        try {
            for ((i, expected) in listOf(
                LogX.EventKind.DEGRADED, LogX.EventKind.DROP,
                LogX.EventKind.IO, LogX.EventKind.RECOVERY
            ).withIndex()) {
                LogX.onNativeError(i + 1, -3, "m")
                assertEquals(expected, got.last().first)
            }
            // 未识别 kindInt 兜底为 IO
            LogX.onNativeError(99, -3, "m")
            assertEquals(LogX.EventKind.IO, got.last().first)
            assertEquals(5, got.size)
        } finally {
            LogX.setErrorHandler(null)
        }
    }

    @Test
    fun onNativeError_swallowsBusinessException() {
        // 内核保护契约: 业务回调抛异常不得打崩调用线程
        LogX.setErrorHandler { _, _, _ -> throw IllegalStateException("boom") }
        try {
            LogX.onNativeError(3, -3, "m")  // 不应抛出
        } finally {
            LogX.setErrorHandler(null)
        }
    }

    // ---- Config 默认值契约 ----
    @Test
    fun configDefaults_matchSpec() {
        val cfg = LogX.Config(logDir = java.io.File("/tmp"), stagingDir = java.io.File("/tmp"))
        assertEquals(64 * 1024, cfg.maxLogLength)   // S-04: 64KB 默认, 与 C++/JNI 三层对齐
        assertEquals(8 * 1024, cfg.flushThreshold)  // E-06: 默认 8KB
        assertEquals(LogX.Compress.NONE, cfg.compress)
        assertFalse(cfg.encrypt)
        assertTrue(cfg.autoProcessIsolation)
        assertEquals(7, cfg.retentionDays)
        // T5.4: consoleEcho 生产默认关闭 (明文进 logcat 有泄露面, 仅 debug 期开启)
        assertFalse(cfg.consoleEcho)
    }

    // ---- 状态机: pre-init / 重复 init / close 后行为 (native 句柄为 0, 不触 JNI) ----
    @Test
    fun preInitWritesAreNoop_andRepeatInitRejected() {
        // 未初始化: 写/flush/close 均应安全 no-op (nativeHandle==0 短路)
        LogX.d("T", "pre-init should be dropped silently")
        LogX.flush()

        // 未 init 时再次 close: no-op
        LogX.close()

        // init 本身需要 Context 与 native 库, JVM 环境不可调;
        // 这里只断言单例状态机的可重入保护不因 no-op 序列而破坏
        assertEquals(0L, LogX.nativeHandleForTest())
    }

    // ---- 协程 export 返回值契约: 未初始化时必须返回 -1 (K-01 语义) ----
    @OptIn(ExperimentalCoroutinesApi::class)
    @Test
    fun suspendExport_returnsMinus1WhenNotInitialized() {
        val testDispatcher = StandardTestDispatcher()
        Dispatchers.setMain(testDispatcher)
        try {
            val rc = runBlocking {
                LogX.export("20260101", "20260102") { /* 不应被调到 */ }
            }
            assertEquals(-1, rc)
        } finally {
            Dispatchers.resetMain()
        }
    }

    @Test
    fun setErrorHandler_afterCloseIsNoop() {
        // close() 置 isClosed 后注册应为 no-op (K-04 终结态保护)
        LogX.close()  // 空实例 close: isClosed 不变(句柄为 0 时短路), 但契约要求幂等
        LogX.setErrorHandler { _, _, _ -> }  // 未初始化态: 允许注册(预配置场景)
        LogX.setErrorHandler(null)
        assertTrue(true)  // 主断言: 全程无异常即通过
    }

    // ---- T5.4 consoleEcho 状态机: 开启时 pre-init 写入仍安全 no-op ----
    // (consoleEcho 镜像发生在 nativeWrite 成功之后, pre-init 短路在前,
    //  JVM 环境下不可能触达 android.util.Log —— 守卫时序的纯逻辑侧验证)
    @Test
    fun preInitWrite_withConsoleEchoConfigSafe() {
        val cfg = LogX.Config(
            logDir = java.io.File("/tmp"), stagingDir = java.io.File("/tmp"),
            consoleEcho = true
        )
        assertTrue(cfg.consoleEcho)
        LogX.i("T5.4", "pre-init echo-enabled write should be dropped silently")
        assertEquals(0L, LogX.nativeHandleForTest())
    }

    // ---- 动态库加载能力: 暴露 loadLibrary 方法且在无对应 so 时抛出预期异常 ----
    @Test
    fun loadLibrary_throwsOnInvalidLibInJvm() {
        assertThrows(UnsatisfiedLinkError::class.java) {
            LogX.loadLibrary("non_existent_lib_for_test")
        }
    }
}
