package com.logx.tester.engine

import com.zzlfeng.logx.LogX
import com.logx.tester.model.ConsoleLine
import com.logx.tester.model.LatencyStats
import com.logx.tester.model.LineKind
import com.logx.tester.model.TesterMetrics
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
            val payload = ByteArray(payloadSize) { (it % 26 + 65).toByte() }
            val latencies = LongArray(total)
            val completedCount = AtomicInteger(0)
            val okCount = AtomicLong(0)
            val noSpaceCount = AtomicLong(0)
            val tooLargeCount = AtomicLong(0)
            val invalidCount = AtomicLong(0)
            val otherDropCount = AtomicLong(0)

            // 问题4修复: 使用 Dispatchers.IO.limitedParallelism 支持真正的多物理线程并发
            val dispatcher = Dispatchers.IO.limitedParallelism(threads)
            val perThread = total / threads
            val remainder = total % threads

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

            val deferreds = (0 until threads).map { threadIdx ->
                val threadCount = perThread + if (threadIdx == 0) remainder else 0
                val threadStartOffset = threadIdx * perThread + if (threadIdx > 0) remainder else 0

                async(dispatcher) {
                    for (i in 0 until threadCount) {
                        if (!isActive) break

                        val slotIdx = threadStartOffset + i
                        val res = LogXProbe.write(LogX.Level.I, "Stress", payload)

                        latencies[slotIdx] = res.nanos

                        when (res) {
                            is LogXProbe.R.Ok -> okCount.incrementAndGet()
                            is LogXProbe.R.NoSpace -> noSpaceCount.incrementAndGet()
                            is LogXProbe.R.TooLarge -> tooLargeCount.incrementAndGet()
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
                                is LogXProbe.R.NoSpace, is LogXProbe.R.TooLarge, is LogXProbe.R.Invalid, is LogXProbe.R.OtherDrop -> LineKind.DROP
                            }
                            val text = when (res) {
                                is LogXProbe.R.Ok -> "Stress #$cur OK (${res.nanos / 1000} µs)"
                                is LogXProbe.R.NoSpace -> "Stress #$cur DROP: staging full"
                                is LogXProbe.R.TooLarge -> "Stress #$cur DROP: log too large"
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

            try {
                deferreds.awaitAll()
            } catch (_: Exception) {
                // 捕获提前中止异常
            } finally {
                tickerJob.cancel()
            }

            // 压测完成或中止后，在 NonCancellable 块中汇总统计分位数
            withContext(NonCancellable) {
                val actualCompleted = completedCount.get()
                val totalElapsedMs = System.currentTimeMillis() - tStart
                val overallTps = if (totalElapsedMs > 0) (actualCompleted * 1000.0) / totalElapsedMs else 0.0

                // 问题2修复: 采用 filter { it > 0 } 排除提前中止时的未填充 0 值
                val validLatencies = latencies.filter { it > 0 }.toLongArray()
                Arrays.sort(validLatencies)

                val p50 = quantile(validLatencies, 0.50) / 1000.0
                val p90 = quantile(validLatencies, 0.90) / 1000.0
                val p99 = quantile(validLatencies, 0.99) / 1000.0
                val p999 = quantile(validLatencies, 0.999) / 1000.0
                val max = if (validLatencies.isNotEmpty()) validLatencies.last() / 1000.0 else 0.0

                val drops = noSpaceCount.get() + tooLargeCount.get() + invalidCount.get() + otherDropCount.get()
                val distorted = drops > 0

                val metrics = TesterMetrics(
                    writeOk = okCount.get(),
                    dropNoSpace = noSpaceCount.get(),
                    dropTooLarge = tooLargeCount.get(),
                    invalidInput = invalidCount.get(),
                    otherDrop = otherDropCount.get(),
                    latency = LatencyStats(
                        count = validLatencies.size.toLong(),
                        p50Us = p50,
                        p90Us = p90,
                        p99Us = p99,
                        p999Us = p999,
                        maxUs = max
                    ),
                    throughputLogsPerSec = overallTps,
                    isDistorted = distorted
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

