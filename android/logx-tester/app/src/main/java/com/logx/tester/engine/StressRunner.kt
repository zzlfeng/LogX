package com.logx.tester.engine

import com.zzlfeng.logx.LogX
import com.logx.tester.model.ConsoleLine
import com.logx.tester.model.LatencyStats
import com.logx.tester.model.LineKind
import com.logx.tester.model.TesterMetrics
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.Arrays
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

object StressRunner {
    private var stressJob: Job? = null

    val isRunning: Boolean
        get() = stressJob?.isActive == true

    fun stop() {
        stressJob?.cancel()
        stressJob = null
    }

    fun start(
        scope: CoroutineScope,
        threads: Int,
        total: Int,
        payloadSize: Int,
        delayMs: Long,
        flushInterval: Int,
        recordEveryLine: Boolean,
        onProgress: (completed: Int, throughput: Double) -> Unit,
        onLine: (ConsoleLine) -> Unit,
        onComplete: (TesterMetrics) -> Unit
    ) {
        stop()

        val job = scope.launch(Dispatchers.Default) {
            // T-15: 公共 API 入参防御 —— total 为负或过大时 LongArray 会抛异常/占用巨量内存。
            // 与 UI 上限解耦, 在此 coerce 到安全区间(最大 500 万条 ≈ 40MB); 线程数与载荷同样兜底。
            val safeTotal = total.coerceIn(0, 5_000_000)
            val safeThreads = threads.coerceIn(1, 256)
            val payload = ByteArray(payloadSize.coerceIn(0, 64 * 1024)) { (it % 26 + 65).toByte() }
            val latencies = LongArray(safeTotal)
            val completedCount = AtomicInteger(0)
            val okCount = AtomicLong(0)
            val noSpaceCount = AtomicLong(0)
            val tooLargeCount = AtomicLong(0)
            val invalidCount = AtomicLong(0)
            val otherDropCount = AtomicLong(0)
            val truncatedCount = AtomicLong(0)

            // 问题4修复: 使用 Dispatchers.IO.limitedParallelism 支持真正的多物理线程并发
            val dispatcher = Dispatchers.IO.limitedParallelism(safeThreads)
            val perThread = safeTotal / safeThreads
            val remainder = safeTotal % safeThreads

            val tStart = System.currentTimeMillis()

            // 问题1修复: 剥离出独立的 100ms 采样协程，不在热写入循环中更新，彻底消除多线程数据竞争
            val tickerJob = launch(Dispatchers.Default) {
                var lastTime = tStart
                var lastCount = 0
                while (isActive) {
                    delay(100)
                    val cur = completedCount.get()
                    val now = System.currentTimeMillis()
                    val elapsedSec = (now - lastTime) / 1000.0
                    val countDelta = cur - lastCount
                    val tps = if (elapsedSec > 0) countDelta / elapsedSec else 0.0
                    lastTime = now
                    lastCount = cur
                    onProgress(cur, tps)
                }
            }

            val deferreds = (0 until safeThreads).map { threadIdx ->
                val threadCount = perThread + if (threadIdx == 0) remainder else 0
                val threadStartOffset = threadIdx * perThread + if (threadIdx > 0) remainder else 0

                async(dispatcher) {
                    for (i in 0 until threadCount) {
                        if (!isActive) break

                        val slotIdx = threadStartOffset + i
                        val res = LogXProbe.write(LogX.Level.I, "Stress", payload)

                        // T-7: 仅成功(含截断保前缀)路径记录延迟样本 —— 失败/丢弃路径的耗时
                        // 不属于"写入端到端延迟", 计入会污染 P50/P99(序列化时靠 >0 过滤不掉)。
                        if (res is LogXProbe.R.Ok || res is LogXProbe.R.Truncated) {
                            latencies[slotIdx] = res.nanos
                        }

                        when (res) {
                            is LogXProbe.R.Ok -> okCount.incrementAndGet()
                            is LogXProbe.R.NoSpace -> noSpaceCount.incrementAndGet()
                            is LogXProbe.R.TooLarge -> tooLargeCount.incrementAndGet()
                            is LogXProbe.R.Truncated -> truncatedCount.incrementAndGet()
                            is LogXProbe.R.Invalid -> invalidCount.incrementAndGet()
                            is LogXProbe.R.OtherDrop -> otherDropCount.incrementAndGet()
                        }

                        val cur = completedCount.incrementAndGet()

                        // 周期性 flush 唤醒
                        if (flushInterval > 0 && cur % flushInterval == 0) {
                            LogX.flush()
                        }

                        // 日志控制台采样
                        if (recordEveryLine || cur % 1000 == 0 || cur == total || cur == 1) {
                            val kind = when (res) {
                                is LogXProbe.R.Ok -> LineKind.WRITE_OK
                                is LogXProbe.R.NoSpace, is LogXProbe.R.TooLarge, is LogXProbe.R.Truncated, is LogXProbe.R.Invalid, is LogXProbe.R.OtherDrop -> LineKind.DROP
                            }
                            val text = when (res) {
                                is LogXProbe.R.Ok -> "Stress #$cur OK (${res.nanos / 1000} µs)"
                                is LogXProbe.R.NoSpace -> "Stress #$cur DROP: staging full"
                                is LogXProbe.R.TooLarge -> "Stress #$cur DROP: log too large"
                                is LogXProbe.R.Truncated -> "Stress #$cur TRUNCATE: ${res.msg}"
                                is LogXProbe.R.Invalid -> "Stress #$cur DROP: invalid input"
                                is LogXProbe.R.OtherDrop -> "Stress #$cur DROP: ${res.msg}"
                            }
                            onLine(ConsoleLine(seq = cur.toLong(), level = "I", tag = "Stress", text = text, kind = kind))
                        }

                        if (delayMs > 0) {
                            delay(delayMs)
                        }
                    }
                }
            }

            var wasCancelled = false
            try {
                deferreds.awaitAll()
            } catch (_: CancellationException) {
                // T-8: 仅协程取消(CancellationException)判为"手动中止"——真异常
                // (OOM / 非法状态等)不再被吞掉并误标成 wasCancelled, 而是向上抛出可见。
                wasCancelled = true
            } finally {
                tickerJob.cancel()
            }

            // 压测完成或中止后，在 NonCancellable 块中汇总统计分位数
            withContext(NonCancellable) {
                // 问题2优化: 严格保留排除未填充 0 值逻辑，采用原生数组零装箱提取，杜绝数十万 Long 包装对象的堆内存抖动与 GC 停顿
                var validCount = 0
                for (i in latencies.indices) {
                    if (latencies[i] > 0L) validCount++
                }
                val validLatencies = LongArray(validCount)
                var validIdx = 0
                for (i in latencies.indices) {
                    val lat = latencies[i]
                    if (lat > 0L) {
                        validLatencies[validIdx++] = lat
                    }
                }
                Arrays.sort(validLatencies)

                val actualCompleted = validLatencies.size.toLong()
                val totalElapsedMs = System.currentTimeMillis() - tStart
                val overallTps = if (totalElapsedMs > 0) (actualCompleted * 1000.0) / totalElapsedMs else 0.0

                val p50 = quantile(validLatencies, 0.50) / 1000.0
                val p90 = quantile(validLatencies, 0.90) / 1000.0
                val p99 = quantile(validLatencies, 0.99) / 1000.0
                val p999 = quantile(validLatencies, 0.999) / 1000.0
                val max = if (validLatencies.isNotEmpty()) validLatencies.last() / 1000.0 else 0.0

                // R4-2: 截断保前缀是有损写入(前缀保留、超出部分丢失), 与丢弃一并判定失真
                val drops = noSpaceCount.get() + tooLargeCount.get() + invalidCount.get() +
                    otherDropCount.get() + truncatedCount.get()
                val distorted = drops > 0

                val metrics = TesterMetrics(
                    writeOk = okCount.get(),
                    dropNoSpace = noSpaceCount.get(),
                    dropTooLarge = tooLargeCount.get(),
                    invalidInput = invalidCount.get(),
                    otherDrop = otherDropCount.get(),
                    truncatedLogs = truncatedCount.get(),
                    latency = LatencyStats(
                        count = actualCompleted,
                        p50Us = p50,
                        p90Us = p90,
                        p99Us = p99,
                        p999Us = p999,
                        maxUs = max
                    ),
                    throughputLogsPerSec = overallTps,
                    isDistorted = distorted,
                    isInterrupted = wasCancelled || actualCompleted < total
                )

                onComplete(metrics)
            }
        }

        stressJob = job
    }

    private fun quantile(sorted: LongArray, q: Double): Long {
        if (sorted.isEmpty()) return 0
        val idx = ((sorted.size - 1) * q).toInt().coerceIn(0, sorted.size - 1)
        return sorted[idx]
    }
}

