package com.zzlfeng.logx

import android.app.ActivityManager
import android.app.Application
import android.content.Context
import android.os.Build
import android.os.Process
import java.io.File
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import kotlin.coroutines.coroutineContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext

/**
 * LogX Kotlin Facade.
 *
 * 与 spec §7 的 Kotlin API (v5) 对齐; JNI 层只做转发, 本类负责:
 *   - 参数装/拆 (目录、压缩算法、加密开关、KID、KeyProvider 桥)
 *   - 多进程 Safe-by-Default 自动按进程名隔离子目录与 staging 文件
 *   - 进程内单例 + 重复 init 抛 IllegalStateException
 *   - export 临时文件路径 (filesDir 系, 禁 cacheDir 与外部存储) 配合协程自动生命周期管理
 *   - 最低兼容标准: Android 5.0 (API Level 21 / minSdk = 21)。
 *     高版本系统特性 (如 API 28+ Application.getProcessName) 均内置 API 21 降级回退分支。
 *
 * 注意:
 *   - 单条日志上限 64KB (格式化后); 超过被丢弃 + 打点。
 *   - 首次 init 之前调用任何写接口一律丢弃 + 计数。
 *   - KeyProvider 返回 null → 该 KID 不可解, export 跳过该文件但不中断。
 *   - consoleEcho (T5.4) 开启时每条成功落盘记录由 JNI 层镜像到 logcat,
 *     仅限 debug 期; 生产保持默认关闭。
 */
object LogX {

    /**
     * 错误码常量 (与 C ABI logx_status_e 严格对齐)。
     * 供错误回调 (ErrorHandler) 及状态码判定使用。
     */
    const val ERR_OK = 0
    const val ERR_INVALID_ARG = -1
    const val ERR_STATE = -2
    const val ERR_IO = -3
    const val ERR_NO_SPACE = -4
    const val ERR_TOO_LARGE = -5
    const val ERR_CODEC = -6
    const val ERR_NOMEM = -7
    const val ERR_NOT_FOUND = -8
    const val ERR_INTERNAL = -9
    const val ERR_TIMEOUT = -10

    /**
     * 压缩算法枚举。序号必须与 C++ core/include/logx_codec.h 的
     * kAlgoNone/kAlgoZstd/kAlgoZlib 严格一致(NONE=0/ZSTD=1/ZLIB=2),
     * 经 JNI 直传给 logx_open 校验,落盘时由 make_flag() 写入块头
     * flag 字节的 bit1..2(加密标志占 bit0)。修改需三端同步。
     */
    enum class Compress(val algo: Byte) {
        NONE(0), ZSTD(1), ZLIB(2)
    }
    enum class Level(val v: Int) { V(0), D(1), I(2), W(3), E(4), F(5) }

    enum class EventKind { DEGRADED, DROP, IO, RECOVERY }
    fun interface ErrorHandler {
        fun onError(kind: EventKind, code: Int, message: String)
    }
    fun interface KeyProvider {
        /** 返回 32B 裸密钥; null = 该 KID 不可解。不得重入 LogX。 */
        fun getKey(kid: Int): ByteArray?
    }
    fun interface ExportCallback {
        /** 返回 0 = 已成功接收并取走; 非 0 = 业务侧处理失败。 */
        fun onExport(date: String, plaintextPath: String, size: Long): Int
    }

    /**
     * 运行时诊断统计快照。字段与 C ABI `logx_stats_t` 非 reserved 成员一一对应,
     * 所有计数均为累计值(自 init 起),线程安全读取(原子快照)。
     */
    data class Stats(
        /** staging 缓冲区空闲不足导致的日志丢弃次数 */
        val dropNoSpace: Long,
        /** 单条日志超过 maxLogLength 被丢弃的次数 */
        val dropTooLarge: Long,
        /** 启动恢复时发现的截尾块计数 */
        val truncatedBlocks: Long,
        /** 环形缓冲区回绕时插入的 PAD 记录数 */
        val padRecords: Long,
        /** 写入短写或 IO 错误计数 */
        val blockWriteErrors: Long,
    )

    data class Config(
        val logDir: File,
        val stagingDir: File,
        val level: Level = Level.I,
        val retentionDays: Int = 7,
        val maxFileSize: Long = 10L * 1024 * 1024,
        val maxLogLength: Int = 64 * 1024,
        // E-06: 0 = 禁用按阈值自动唤醒(纯手动 flush 控制);-1 = 取默认 8KB;
        // 其它值 = 字节阈值。JNI 层以 -1 映射 C ABI 的 UINT32_MAX 默认哨兵。
        val flushThreshold: Int = 8 * 1024,
        val compress: Compress = Compress.NONE,
        val encrypt: Boolean = false,
        val kid: Int = 1,
        val keyProvider: KeyProvider? = null,
        val errorHandler: ErrorHandler? = null,
        val autoProcessIsolation: Boolean = true, // 默认开启多进程目录防呆隔离
        // T5.4: logcat 直通 —— 开启后每条成功落盘的记录同步镜像到 logcat
        // (保留调用方 tag、空 tag 回退 LogX, 优先级按 Level 映射; 单条 >2KB 在 native 侧截断,
        // logcat 单行 ~4KB 截断为系统行为)。仅限 debug 期使用: 明文进 logcat 有泄露面
        // (adb 任意可读), 生产保持默认关闭。丢弃记录不镜像(走 ErrorHandler.DROP)。
        val consoleEcho: Boolean = false,
    )

    @Volatile private var nativeHandle: Long = 0L
    @Volatile private var errorHandler: ErrorHandler? = null
    @Volatile private var isClosed: Boolean = false
    @Volatile private var minLevel: Level = Level.I
    private val activeOps = AtomicInteger(0)
    private val pendingCloseHandle = AtomicLong(0L)
    private val initCount = AtomicLong(0)
    @Volatile private var isLibraryLoaded: Boolean = false

    private val threadOpDepth = object : ThreadLocal<Int>() {
        override fun initialValue(): Int = 0
    }

    /**
     * 统一保护所有跨 JNI 的 native 操作 (log / flush / getStats / export)。
     *
     * 核心安全保障 (R2):
     * 1. 消除 TOCTOU 窄窗: 进入前先原子自增 activeOps，杜绝操作线程刚读到句柄被切走、
     *    而 close() 误以为无活跃线程直接 nativeClose 造成的 UAF。
     * 2. 递延销毁 (Deferred Close): 若 close() 时仍有慢操作或后台 export 正在执行，
     *    或在 ErrorHandler 回调线程中直接同步调用 close()，close() 绝不强制销毁句柄，
     *    而是将句柄存入 pendingCloseHandle，由最后一个退出的 executeOp 在 finally 中
     *    安全递延触发 nativeClose。
     * 3. 递归/回调防死锁: 记录 threadOpDepth，若当前线程已在 op 中 (如 native 回调
     *    onNativeError 触发的 onError 内调用 close)，close() 立即感知并跳过等待，
     *    杜绝自等待死锁。
     */
    private inline fun <T> executeOp(block: (Long) -> T): T? {
        if (isClosed) return null
        activeOps.incrementAndGet()
        val depth = threadOpDepth.get() ?: 0
        threadOpDepth.set(depth + 1)
        try {
            if (isClosed) return null
            val h = nativeHandle
            if (h == 0L) return null
            return block(h)
        } finally {
            if (depth == 0) {
                threadOpDepth.remove()
            } else {
                threadOpDepth.set(depth)
            }
            val remaining = activeOps.decrementAndGet()
            if (remaining == 0 && pendingCloseHandle.get() != 0L) {
                val handleToClose = pendingCloseHandle.getAndSet(0L)
                if (handleToClose != 0L) {
                    nativeClose(handleToClose)
                }
            }
        }
    }

    /** 动态设置最低日志记录级别。低于此级别的日志将在 Kotlin 侧零开销直接过滤。 */
    fun setMinLevel(level: Level) {
        minLevel = level
    }

    /** 获取当前最低日志记录级别。 */
    fun getMinLevel(): Level = minLevel

    init {
        try {
            System.loadLibrary("logx")
            isLibraryLoaded = true
        } catch (ignored: UnsatisfiedLinkError) {
            // 允许 JVM 单元测试等无 native 库环境加载此类; Android 运行时在 init() 时按需加载/抛出
        }
    }

    /**
     * 加载 native 动态库。
     * 默认情况下无需手动调用 (LogX 类加载及 init 时会自动尝试加载 "logx")。
     * 若使用 ReLinker 或自定义 so 名称，可在调用 init 之前主动调用此方法。
     */
    fun loadLibrary(libName: String = "logx") {
        System.loadLibrary(libName)
        isLibraryLoaded = true
    }

    /** [LogXPureLogicTest] 等白盒单测专用:只读暴露句柄,生产代码禁止使用。 */
    internal fun nativeHandleForTest(): Long = nativeHandle

    @Synchronized
    fun init(context: Context, config: Config) {
        if (initCount.incrementAndGet() != 1L) {
            // 重复 init: 抛 IllegalStateException (参见 spec §7)。
            throw IllegalStateException("LogX.init called more than once")
        }
        // 若前序实例仍有未退栈的后台/慢操作，等待其彻底退出并完成递延销毁
        val deadline = System.currentTimeMillis() + 1000
        while (activeOps.get() > 0 && System.currentTimeMillis() < deadline) {
            try {
                Thread.sleep(2)
            } catch (ignored: InterruptedException) {
                Thread.currentThread().interrupt()
                break
            }
        }
        if (activeOps.get() > 0) {
            initCount.set(0)
            throw IllegalStateException("Cannot init LogX: previous instance operations still active")
        }
        val lingering = pendingCloseHandle.getAndSet(0L)
        if (lingering != 0L) {
            nativeClose(lingering)
        }

        // K-03 / R3: 任何失败路径 (参数校验 / 目录创建 / nativeInit 返回 0) 都必须
        // 回滚单例状态 —— 否则 errorHandler 持有的业务回调对象因单例引用无法
        // 及时 GC; initCount 归零允许业务修正配置后重试 init; isClosed 保持/重置为 true。
        try {
            if (!isLibraryLoaded) {
                System.loadLibrary("logx")
                isLibraryLoaded = true
            }

            if (config.errorHandler != null) {
                errorHandler = config.errorHandler
            }

            val suffix = if (config.autoProcessIsolation) getProcessSuffix(context) else ""
            val targetLogDir = if (suffix.isEmpty()) config.logDir else File(config.logDir.parentFile, "${config.logDir.name}_$suffix")
            val targetStagingDir = if (suffix.isEmpty()) config.stagingDir else File(config.stagingDir.parentFile, "${config.stagingDir.name}_$suffix")

            val logDir = targetLogDir.apply { mkdirs() }
            val stagingDir = targetStagingDir.apply { mkdirs() }
            val stagingFileName = if (suffix.isEmpty()) "logx_staging.bin" else "logx_staging_$suffix.bin"
            val stagingFile = File(stagingDir, stagingFileName)

            // staging 必须 filesDir 系; 禁 cacheDir (系统清理会让 mmap 变孤儿, 见 spec §10)
            require(!stagingDir.absolutePath.contains("/cache/")) {
                "staging dir must not be in cacheDir"
            }
            require(config.maxLogLength < 128 * 1024 - 128 - 1) {
                "maxLogLength must be < (dataSize - 1) = 130943"
            }

            minLevel = config.level
            val handle = nativeInit(
                logDir.absolutePath,
                stagingFile.absolutePath,
                config.compress.algo.toInt(),
                if (config.encrypt) 1 else 0,
                config.kid,
                config.keyProvider,
                config.retentionDays,
                config.maxFileSize,
                config.maxLogLength,
                config.flushThreshold,
                config.consoleEcho
            )
            if (handle == 0L) {
                throw IllegalStateException("LogX native init failed")
            }
            nativeHandle = handle
            isClosed = false
        } catch (t: Throwable) {
            isClosed = true
            nativeHandle = 0L
            errorHandler = null
            initCount.set(0)
            throw t
        }
    }

    private fun getProcessSuffix(context: Context): String {
        val processName = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            Application.getProcessName()
        } else {
            val pid = Process.myPid()
            val am = context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager
            am?.runningAppProcesses?.firstOrNull { it.pid == pid }?.processName ?: ""
        }
        val pkgName = context.packageName
        return when {
            processName.isEmpty() || processName == pkgName -> ""
            processName.startsWith("$pkgName:") -> processName.removePrefix("$pkgName:")
            else -> processName.replace("[^a-zA-Z0-9_]".toRegex(), "_")
        }
    }

    /**
     * 注册/清除全局错误回调 (K-04: 与 init/close 同一把对象锁串行化,提供
     * 基本并发保护与状态时序约束)。
     *
     * 时序约束:
     *   - close() 之后注册的回调不再生效 (单例已终结,保持置 null 语义,防止悬挂引用);
     *   - 允许在 init() 之前预设全局回调 (如 Application.onCreate 提前监听启动期错误);
     *   - 与 init/close 并发调用时互斥串行,读方 [onNativeError] 经 @Volatile
     *     handler 获得高性能无锁可见性保证。
     */
    @Synchronized
    fun setErrorHandler(handler: ErrorHandler?) {
        // 已关闭实例: 回调通道随实例终结,保持 null,防止悬挂引用
        if (isClosed) return
        errorHandler = handler
    }

    @JvmStatic
    fun onNativeError(kindInt: Int, code: Int, message: String) {
        val kind = when (kindInt) {
            1 -> EventKind.DEGRADED
            2 -> EventKind.DROP
            3 -> EventKind.IO
            4 -> EventKind.RECOVERY
            else -> EventKind.IO
        }
        try {
            errorHandler?.onError(kind, code, message)
        } catch (t: Throwable) {
            // 保护内核不被业务异常打崩
        }
    }

    fun v(tag: String, msg: String) = log(Level.V, tag, msg.toByteArray())
    fun d(tag: String, msg: String) = log(Level.D, tag, msg.toByteArray())
    fun i(tag: String, msg: String) = log(Level.I, tag, msg.toByteArray())
    fun w(tag: String, msg: String) = log(Level.W, tag, msg.toByteArray())
    fun e(tag: String, msg: String) = log(Level.E, tag, msg.toByteArray())
    fun f(tag: String, msg: String) = log(Level.F, tag, msg.toByteArray())

    fun log(level: Level, tag: String, payload: ByteArray) {
        // P3-3: 方案 A: Kotlin 层前置过滤, 彻底消除不符合等级日志的 JNI 栈跨越与内存开销
        if (level.v < minLevel.v) return

        executeOp { h ->
            nativeWrite(h, level.v, tag, payload)
        }
    }

    /** 阻塞至进入时刻的目标水位落盘提交。退后台 / onStop 调用。 */
    fun flush() {
        executeOp { h ->
            nativeFlush(h)
        }
    }

    /**
     * 获取运行时诊断统计快照。init 之前或 close 之后调用返回 null。
     *
     * 线程安全:底层通过原子变量逐字段 load,返回的 [Stats] 是值语义不可变快照,
     * 调用方可安全跨线程传递;多次调用之间各字段可能独立递增(非全局快照原子性),
     * 但对监控 / 告警场景已足够。
     */
    fun getStats(): Stats? {
        return executeOp { h ->
            val arr = nativeGetStats(h) ?: return@executeOp null
            Stats(
                dropNoSpace = arr[0],
                dropTooLarge = arr[1],
                truncatedBlocks = arr[2],
                padRecords = arr[3],
                blockWriteErrors = arr[4],
            )
        }
    }

    /**
     * 按日期范围导出 (回调模式)。from/to = yyyyMMdd 闭区间;
     * 明文临时文件写在 filesDir 下独立临时目录 (禁外部存储、禁 cacheDir); 回调返回或抛出后立即删除。
     */
    fun export(from: String, to: String, cb: ExportCallback): Int {
        return executeOp { h ->
            nativeExport(h, from, to, cb)
        } ?: -1
    }

    /**
     * 协程化导出 (推荐): 自动处理 flush、解密导出与临时文件生命周期管理。
     *
     * 返回值与异常语义 (K-01 / K-02 优化修复):
     *   - 0  = LOGX_OK, 导出成功;
     *   - -1 = 实例未初始化或已关闭;
     *   - 其他负值 = native 状态码: -6 LOGX_ERR_CODEC = 业务 block 抛异常或
     *     回调返回非 0 (经 E-03 错误传播链路上报); -3 LOGX_ERR_IO = 临时文件
     *     写出失败; -8 LOGX_ERR_NOT_FOUND = 时段内无日志文件;
     *   - 结构化并发保障: runBlocking(parentContext) 绑定外层挂起点上下文，
     *     外部协程取消信号能及时传导到 block 中中断执行;
     *   - 防调度器死锁: 直接在当前 native 调用栈线程上执行事件循环，不向 Dispatchers.IO
     *     重复调度多余工作线程，杜绝并发限制下 (如 limitedParallelism) 的互锁死锁;
     *   - 异常如实传递: 若业务 block 抛出异常，临时文件仍安全删除，并优先将业务真实
     *     异常向外抛出，避免被通用状态码掩盖。
     */
    suspend fun export(from: String, to: String, block: suspend (File) -> Unit): Int {
        val parentContext = coroutineContext

        return withContext(Dispatchers.IO) {
            var userException: Throwable? = null
            val res = executeOp { h ->
                nativeExport(h, from, to) { _, plaintextPath, _ ->
                    val tempFile = File(plaintextPath)
                    if (userException != null) {
                        // 若前一分卷处理已抛出异常，快速失败返回 -1，避免后续分卷继续执行业务逻辑
                        return@nativeExport -1
                    }
                    val cbRet = try {
                        runBlocking(parentContext) {
                            block(tempFile)
                        }
                        0
                    } catch (t: Throwable) {
                        userException = t
                        -1
                    } finally {
                        try {
                            if (tempFile.exists()) tempFile.delete()
                        } catch (t: Throwable) {
                        }
                    }
                    cbRet
                }
            }
            userException?.let { throw it }
            res ?: -1
        }
    }

    // ---- native 绑定 (JNI 实现见 android/cpp/logx_jni.cpp) ----
    @JvmStatic private external fun nativeInit(
        logDir: String, stagingPath: String,
        compressAlgo: Int, encryptEnable: Int, kid: Int, keyProvider: Any?,
        retentionDays: Int, maxFileSize: Long, maxLogLength: Int, flushThreshold: Int,
        consoleEcho: Boolean
    ): Long
    @JvmStatic private external fun nativeClose(handle: Long)
    @JvmStatic private external fun nativeWrite(handle: Long, level: Int,
                                                 tag: String, payload: ByteArray)
    @JvmStatic private external fun nativeFlush(handle: Long)
    @JvmStatic private external fun nativeExport(handle: Long, from: String, to: String,
                                                 cb: ExportCallback): Int
    @JvmStatic private external fun nativeGetStats(handle: Long): LongArray?

    /**
     * 关闭实例并释放 native 资源 (K-04: 与 setErrorHandler 串行化,避免
     * 回调在 close 过程中被重新挂回)。
     *
     * 优雅退栈与递延销毁 (R2):
     * 1. 若当前调用者非正在执行 executeOp 的线程，最多等待 500ms 让正在写/导出/落盘的线程优雅退出;
     * 2. 若由 ErrorHandler (如 onNativeError) 内部同步调用 close()，当前线程本身即为 activeOp，
     *    感知 depth > 0 立即跳过等待，杜绝自死锁;
     * 3. 若等待结束 activeOps == 0，由 close() 立即调用 nativeClose 销毁底层句柄;
     * 4. 若超时后仍有慢操作未退出，不强制销毁句柄 (杜绝 UAF)，而是交由最后一个退出 executeOp 的
     *    线程在 finally 块中安全递延触发 nativeClose。
     */
    @Synchronized
    fun close() {
        val h = nativeHandle
        if (h == 0L) return
        isClosed = true
        nativeHandle = 0L
        initCount.set(0)
        // close 后清除回调引用并标记关闭,回调对象可及时释放 (与 K-03 同纪律)
        errorHandler = null

        // 登记待销毁句柄，保证必定被且仅被释放一次 (CAS / getAndSet 语义)
        pendingCloseHandle.set(h)

        val isCurrentThreadInOp = (threadOpDepth.get() ?: 0) > 0
        if (!isCurrentThreadInOp) {
            val deadline = System.currentTimeMillis() + 500
            while (activeOps.get() > 0 && System.currentTimeMillis() < deadline) {
                try {
                    Thread.sleep(1)
                } catch (ignored: InterruptedException) {
                    Thread.currentThread().interrupt()
                    break
                }
            }
        }

        if (activeOps.get() == 0) {
            val toClose = pendingCloseHandle.getAndSet(0L)
            if (toClose != 0L) {
                nativeClose(toClose)
            }
        }
    }
}