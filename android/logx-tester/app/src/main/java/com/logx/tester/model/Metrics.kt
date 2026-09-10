package com.logx.tester.model

import com.zzlfeng.logx.LogX

data class LatencyStats(
    val count: Long = 0,
    val p50Us: Double = 0.0,
    val p90Us: Double = 0.0,
    val p99Us: Double = 0.0,
    val p999Us: Double = 0.0,
    val maxUs: Double = 0.0
)

data class TesterMetrics(
    val writeOk: Long = 0,
    val dropNoSpace: Long = 0,
    val dropTooLarge: Long = 0,
    val invalidInput: Long = 0,
    val otherDrop: Long = 0,
    val eventDegraded: Long = 0,
    val eventDrop: Long = 0,
    val eventIo: Long = 0,
    val eventRecovery: Long = 0,
    val latency: LatencyStats = LatencyStats(),
    val throughputLogsPerSec: Double = 0.0,
    val isDistorted: Boolean = false, // 有丢弃时标注数据失真
    val nativeStats: LogX.Stats? = null // 问题6: LogX.getStats() 底层诊断快照
)

