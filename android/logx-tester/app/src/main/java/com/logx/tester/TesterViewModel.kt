package com.logx.tester

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.zzlfeng.logx.LogX
import com.logx.tester.engine.EngineController
import com.logx.tester.engine.EngineState
import com.logx.tester.engine.LogXProbe
import com.logx.tester.engine.StartResult
import com.logx.tester.engine.StressRunner
import com.logx.tester.model.ConsoleLine
import com.logx.tester.model.LatencyStats
import com.logx.tester.model.LineKind
import com.logx.tester.model.Preset
import com.logx.tester.model.TesterConfig
import com.logx.tester.model.TesterMetrics
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.atomic.AtomicLong

data class LogFileInfo(
    val name: String,
    val sizeBytes: Long,
    val lastModified: Long
)

class TesterViewModel(application: Application) : AndroidViewModel(application) {
    private val ctx = application.applicationContext

    // 配置与实例状态
    private val _config = MutableStateFlow(TesterConfig())
    val config: StateFlow<TesterConfig> = _config.asStateFlow()

    val engineState: StateFlow<EngineState> = EngineController.state

    private val _statusMessage = MutableStateFlow("就绪")
    val statusMessage: StateFlow<String> = _statusMessage.asStateFlow()

    // 统计指标
    private val _metrics = MutableStateFlow(TesterMetrics())
    val metrics: StateFlow<TesterMetrics> = _metrics.asStateFlow()

    // 单条写入即时回显
    private val _lastWriteResult = MutableStateFlow<String?>(null)
    val lastWriteResult: StateFlow<String?> = _lastWriteResult.asStateFlow()

    // 日志文件列表
    private val _logFiles = MutableStateFlow<List<LogFileInfo>>(emptyList())
    val logFiles: StateFlow<List<LogFileInfo>> = _logFiles.asStateFlow()

    // 压测状态
    private val _isStressRunning = MutableStateFlow(false)
    val isStressRunning: StateFlow<Boolean> = _isStressRunning.asStateFlow()

    private val _stressProgress = MutableStateFlow(0) // 已完成条数
    val stressProgress: StateFlow<Int> = _stressProgress.asStateFlow()

    private val _stressThroughput = MutableStateFlow(0.0) // logs/s
    val stressThroughput: StateFlow<Double> = _stressThroughput.asStateFlow()

    private val _stressReport = MutableStateFlow<String?>(null)
    val stressReport: StateFlow<String?> = _stressReport.asStateFlow()

    // 实时日志控制台
    private val lineSeq = AtomicLong(0)
    private val pendingLines = ConcurrentLinkedQueue<ConsoleLine>()
    private val _consoleLines = MutableStateFlow<List<ConsoleLine>>(emptyList())
    val consoleLines: StateFlow<List<ConsoleLine>> = _consoleLines.asStateFlow()

    var maxConsoleBuffer: Int = 2000

    private var batchFlushJob: Job? = null

    init {
        // 1. 监听全局事件流 (用于控制台展示)
        viewModelScope.launch {
            LogXProbe.eventSink.collect { ev ->
                val kind = when (ev.kind) {
                    LogX.EventKind.DROP -> LineKind.DROP
                    LogX.EventKind.DEGRADED -> LineKind.DEGRADED
                    LogX.EventKind.IO -> LineKind.IO
                    LogX.EventKind.RECOVERY -> LineKind.RECOVERY
                }
                appendConsoleLine(
                    level = ev.kind.name,
                    tag = "Event",
                    text = "[${ev.kind}] code=${ev.code}: ${ev.msg}",
                    kind = kind
                )
            }
        }

        // 2. 启动 100ms 批量合并定时协程，并同步原子事件计数与 Native 诊断快照
        batchFlushJob = viewModelScope.launch(Dispatchers.Default) {
            while (isActive) {
                delay(100)
                // 定期同步原子事件计数与 native 诊断快照
                _metrics.update { old ->
                    old.copy(
                        eventDegraded = LogXProbe.eventDegradedCount.get(),
                        eventDrop = LogXProbe.eventDropCount.get(),
                        eventIo = LogXProbe.eventIoCount.get(),
                        eventRecovery = LogXProbe.eventRecoveryCount.get(),
                        nativeStats = LogX.getStats() ?: old.nativeStats
                    )
                }

                if (pendingLines.isNotEmpty()) {
                    val batch = ArrayList<ConsoleLine>()
                    while (true) {
                        val line = pendingLines.poll() ?: break
                        batch.add(line)
                    }
                    if (batch.isNotEmpty()) {
                        _consoleLines.update { old ->
                            val combined = old + batch
                            if (combined.size > maxConsoleBuffer) {
                                combined.takeLast(maxConsoleBuffer)
                            } else {
                                combined
                            }
                        }
                    }
                }
            }
        }

        appendConsoleLine(tag = "System", text = "LogX Tester 初始化就绪，点击'应用配置并重启'以启动实例", kind = LineKind.SYSTEM)
    }

    fun updateConfig(modifier: (TesterConfig) -> TesterConfig) {
        _config.update(modifier)
    }

    fun applyPreset(preset: Preset) {
        _config.update(preset.configModifier)
        appendConsoleLine(tag = "Preset", text = "载入预置场景: ${preset.name} (${preset.summary})", kind = LineKind.SYSTEM)
        applyAndRestart()
    }

    fun applyAndRestart() {
        viewModelScope.launch {
            _statusMessage.value = "正在启动实例..."
            appendConsoleLine(tag = "System", text = "正在重启 LogX 单例...", kind = LineKind.SYSTEM)

            val res = EngineController.applyAndRestart(ctx, _config.value)
            when (res) {
                is StartResult.Ok -> {
                    val c = _config.value
                    _statusMessage.value = "运行中: ${c.compress}·${if (c.encrypt) "加密" else "明文"}·${c.flushThreshold}B·${c.maxLogLength}B"
                    appendConsoleLine(tag = "System", text = "LogX 启动成功", kind = LineKind.SYSTEM)
                    refreshLogFiles()
                }
                is StartResult.Failed -> {
                    _statusMessage.value = "启动失败: [${res.errorClass}] ${res.message}"
                    appendConsoleLine(tag = "System", text = "启动失败: [${res.errorClass}] ${res.message}", kind = LineKind.DROP)
                }
            }
        }
    }

    fun stopInstance() {
        viewModelScope.launch {
            EngineController.stop()
            _statusMessage.value = "已停止"
            appendConsoleLine(tag = "System", text = "LogX 实例已停止", kind = LineKind.SYSTEM)
        }
    }

    fun writeSingleLog(level: LogX.Level, tag: String, msgText: String, payloadBytes: Int) {
        if (engineState.value != EngineState.Running) {
            _lastWriteResult.value = "实例未运行"
            appendConsoleLine(tag = "Error", text = "写入失败：实例未运行，请先启动", kind = LineKind.DROP)
            return
        }

        // App 侧 level 过滤 (因为 LogX 库内无 level 过滤)
        if (level.v < _config.value.minLevelGate.v) {
            _lastWriteResult.value = "App 侧过滤 (当前门控: ${_config.value.minLevelGate.name})"
            appendConsoleLine(level = level.name, tag = tag, text = "被 App 门控过滤 (minLevel=${_config.value.minLevelGate})", kind = LineKind.SYSTEM)
            return
        }

        val payload = if (payloadBytes > 0) {
            val textBytes = msgText.toByteArray(Charsets.UTF_8)
            if (payloadBytes <= textBytes.size) {
                textBytes.copyOf(payloadBytes)
            } else {
                ByteArray(payloadBytes) { i ->
                    if (i < textBytes.size) textBytes[i] else (65 + (i % 26)).toByte()
                }
            }
        } else {
            msgText.toByteArray(Charsets.UTF_8)
        }

        val res = LogXProbe.write(level, tag, payload)
        val us = res.nanos / 1000.0

        when (res) {
            is LogXProbe.R.Ok -> {
                _metrics.update { it.copy(writeOk = it.writeOk + 1) }
                _lastWriteResult.value = "OK (${us} µs)"
                appendConsoleLine(level = level.name, tag = tag, text = "$msgText (${payload.size}B, ${us} µs)", kind = LineKind.WRITE_OK)
            }
            is LogXProbe.R.NoSpace -> {
                _metrics.update { it.copy(dropNoSpace = it.dropNoSpace + 1) }
                _lastWriteResult.value = "NO_SPACE (${us} µs)"
                appendConsoleLine(level = level.name, tag = tag, text = "DROP: staging full (${us} µs)", kind = LineKind.DROP)
            }
            is LogXProbe.R.TooLarge -> {
                _metrics.update { it.copy(dropTooLarge = it.dropTooLarge + 1) }
                _lastWriteResult.value = "TOO_LARGE (${us} µs)"
                appendConsoleLine(level = level.name, tag = tag, text = "DROP: log too large (${us} µs)", kind = LineKind.DROP)
            }
            is LogXProbe.R.Invalid -> {
                _metrics.update { it.copy(invalidInput = it.invalidInput + 1) }
                _lastWriteResult.value = "INVALID (${us} µs)"
                appendConsoleLine(level = level.name, tag = tag, text = "DROP: invalid log input (${us} µs)", kind = LineKind.DROP)
            }
            is LogXProbe.R.OtherDrop -> {
                _metrics.update { it.copy(otherDrop = it.otherDrop + 1) }
                _lastWriteResult.value = "DROP: ${res.msg} (${us} µs)"
                appendConsoleLine(level = level.name, tag = tag, text = "DROP: ${res.msg} (${us} µs)", kind = LineKind.DROP)
            }
        }
    }

    fun flushSync() {
        if (engineState.value != EngineState.Running) return
        viewModelScope.launch(Dispatchers.IO) {
            val t0 = System.currentTimeMillis()
            LogX.flush()
            val dt = System.currentTimeMillis() - t0
            appendConsoleLine(tag = "Flush", text = "手动 flush 完成，耗时 ${dt}ms", kind = LineKind.SYSTEM)
            refreshLogFiles()
        }
    }

    fun exportLogs(from: String, to: String) {
        if (engineState.value != EngineState.Running) return
        viewModelScope.launch(Dispatchers.IO) {
            appendConsoleLine(tag = "Export", text = "正在导出日志 ($from .. $to)...", kind = LineKind.SYSTEM)
            var totalBytes = 0L
            var fileCount = 0
            val code = LogX.export(from, to) { file ->
                fileCount++
                totalBytes += file.length()
                appendConsoleLine(tag = "Export", text = "导出一卷: ${file.name} (${file.length()} 字节)", kind = LineKind.SYSTEM)
            }
            val statusDesc = when (code) {
                0 -> "导出完成 (成功)"
                -1 -> "实例未初始化"
                -3 -> "LOGX_ERR_IO (IO 错误)"
                -6 -> "LOGX_ERR_CODEC (编解码/回调失败)"
                -8 -> "LOGX_ERR_NOT_FOUND (时段内无日志)"
                else -> "返回码: $code"
            }
            appendConsoleLine(tag = "Export", text = "$statusDesc, 共 $fileCount 文件, $totalBytes 字节", kind = LineKind.SYSTEM)
        }
    }

    fun refreshLogFiles() {
        viewModelScope.launch(Dispatchers.IO) {
            val cfg = _config.value
            val dir = File(ctx.filesDir, cfg.logDirName)
            val parent = dir.parentFile ?: ctx.filesDir

            // 搜索当前以及潜在隔离子目录下的所有 log 文件
            val subDirs = parent.listFiles()?.filter { it.isDirectory && it.name.startsWith(cfg.logDirName) } ?: emptyList()
            val dirsToSearch = (listOf(dir) + subDirs).distinct()

            val files = mutableListOf<LogFileInfo>()
            for (d in dirsToSearch) {
                d.listFiles()?.filter { it.isFile && (it.name.endsWith(".log") || it.name.contains(".log.")) }
                    ?.forEach { f ->
                        files.add(LogFileInfo(name = "${d.name}/${f.name}", sizeBytes = f.length(), lastModified = f.lastModified()))
                    }
            }
            _logFiles.value = files.sortedByDescending { it.lastModified }
        }
    }

    fun startStressTest(
        threads: Int,
        totalLogs: Int,
        payloadSize: Int,
        delayMs: Long,
        flushInterval: Int,
        recordEveryLine: Boolean
    ) {
        if (engineState.value != EngineState.Running) {
            appendConsoleLine(tag = "Stress", text = "压测失败：实例未运行", kind = LineKind.DROP)
            return
        }

        _isStressRunning.value = true
        _stressProgress.value = 0
        _stressThroughput.value = 0.0
        _stressReport.value = null

        appendConsoleLine(
            tag = "Stress",
            text = "启动并发压测: $threads 线程, 总计 $totalLogs 条, 单条 ${payloadSize}B, flushInterval=$flushInterval",
            kind = LineKind.SYSTEM
        )

        StressRunner.start(
            scope = viewModelScope,
            threads = threads,
            total = totalLogs,
            payloadSize = payloadSize,
            delayMs = delayMs,
            flushInterval = flushInterval,
            recordEveryLine = recordEveryLine,
            onProgress = { cur, tps ->
                _stressProgress.value = cur
                _stressThroughput.value = tps
            },
            onLine = { line ->
                appendConsoleLine(line.level, line.tag, line.text, line.kind)
            },
            onComplete = { resultMetrics ->
                _isStressRunning.value = false
                _metrics.update { current ->
                    current.copy(
                        writeOk = current.writeOk + resultMetrics.writeOk,
                        dropNoSpace = current.dropNoSpace + resultMetrics.dropNoSpace,
                        dropTooLarge = current.dropTooLarge + resultMetrics.dropTooLarge,
                        invalidInput = current.invalidInput + resultMetrics.invalidInput,
                        otherDrop = current.otherDrop + resultMetrics.otherDrop,
                        latency = resultMetrics.latency,
                        throughputLogsPerSec = resultMetrics.throughputLogsPerSec,
                        isDistorted = resultMetrics.isDistorted
                    )
                }

                val lat = resultMetrics.latency
                val report = buildString {
                    append("压测完成: 总写成功 ${resultMetrics.writeOk}, 吞吐 ${"%.1f".format(resultMetrics.throughputLogsPerSec)} logs/s\n")
                    append("延迟分位数 (µs): P50=${"%.1f".format(lat.p50Us)} | P90=${"%.1f".format(lat.p90Us)} | P99=${"%.1f".format(lat.p99Us)} | P99.9=${"%.1f".format(lat.p999Us)} | Max=${"%.1f".format(lat.maxUs)}")
                    if (resultMetrics.isDistorted) {
                        append("\n⚠️ 数据失真：本轮有丢弃(NoSpace=${resultMetrics.dropNoSpace}, TooLarge=${resultMetrics.dropTooLarge}, Invalid=${resultMetrics.invalidInput})，吞吐/延迟结论无效！")
                    }
                }
                _stressReport.value = report
                appendConsoleLine(tag = "Stress", text = report, kind = if (resultMetrics.isDistorted) LineKind.DROP else LineKind.SYSTEM)
                refreshLogFiles()
            }
        )
    }

    fun stopStressTest() {
        StressRunner.stop()
        _isStressRunning.value = false
        appendConsoleLine(tag = "Stress", text = "压测已手动中止", kind = LineKind.SYSTEM)
    }

    fun clearConsole() {
        pendingLines.clear()
        _consoleLines.value = emptyList()
    }

    fun refreshNativeStats() {
        val stats = LogX.getStats()
        _metrics.update { it.copy(nativeStats = stats) }
    }

    fun clearMetrics() {
        LogXProbe.resetEventCounts()
        _metrics.value = TesterMetrics(nativeStats = LogX.getStats())
        _stressReport.value = null
    }

    private fun appendConsoleLine(level: String = "", tag: String = "", text: String, kind: LineKind) {
        pendingLines.offer(
            ConsoleLine(
                seq = lineSeq.incrementAndGet(),
                tsMs = System.currentTimeMillis(),
                level = level,
                tag = tag,
                text = text,
                kind = kind
            )
        )
    }

    override fun onCleared() {
        super.onCleared()
        batchFlushJob?.cancel()
        StressRunner.stop()
        LogX.close()
    }
}

