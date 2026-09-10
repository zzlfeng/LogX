package com.logx.tester.engine

import com.zzlfeng.logx.LogX
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.asSharedFlow
import java.util.concurrent.atomic.AtomicLong

/**
 * LogXProbe: 写入结果探测与事件捕获哨兵
 *
 * 【核心架构假设 A · 同步回调约束】:
 * 本探针的 ThreadLocal 哨兵捕获机制强依赖 Native 层 `logx_write` 的同步执行语义：
 * 当发生丢弃时，底层 `emit_event` 在调用方线程内同步触发 `jni_error_trampoline`，
 * 进而直接同步回调 `LogX.onNativeError` -> `LogXProbe.onEvent`。
 * 若未来 Native 端改为异步派发队列或跨线程转发，ThreadLocal 槽位将无法命中，
 * 届时需重构为基于 RequestID 或无锁序列号追踪的关联映射表。
 *
 * 【延迟测算口径 B · 框架本底开销】:
 * [R.nanos] 统计的是调用端从 API 发起到 Native 返回的端到端耗时。
 * 虽已将 [Capture] 实例化与 ThreadLocal 设值移至计时起点 `t0` 之前，但在调用完成后
 * 仍包含一次 `finally { slot.set(null) }`（约 10ns）及 2 次 `System.nanoTime()` 系统调用底噪，
 * 对于微秒级日志写入影响在 1%~2% 以内，属于端到端实际感知延迟。
 *
 * 【问题 P2 解决方案 · 强类型错误码映射】:
 * 现已打通 C++ 内核 (logx_c.cpp) 真实错误码透传链路：
 *   - `br == kBuildRecordsInvalidInput` -> `emit_event(inst, LOGX_EV_DROP, LOGX_ERR_INVALID_ARG, "invalid log input")`
 *   - `total > inst->max_log_length`    -> `emit_event(inst, LOGX_EV_DROP, LOGX_ERR_TOO_LARGE, "log too large")`
 *   - `st == Staging::WriteStatus::kFull`-> `emit_event(inst, LOGX_EV_DROP, LOGX_ERR_NO_SPACE, "staging full")`
 * 上层优先根据 `c.code`（LogX.ERR_NO_SPACE / ERR_TOO_LARGE / ERR_INVALID_ARG）进行严格匹配，
 * 彻底消除了对错误消息文案的脆弱依赖；同时保留文案匹配以向后兼容。
 */
object LogXProbe {
    data class EventRow(val kind: LogX.EventKind, val code: Int, val msg: String)
    class Capture { var hit = false; var code = 0; var msg = "" }
    private val slot = ThreadLocal<Capture?>()

    sealed interface R {
        val nanos: Long
        data class Ok(override val nanos: Long) : R
        data class NoSpace(override val nanos: Long) : R
        data class TooLarge(override val nanos: Long) : R
        data class Invalid(override val nanos: Long) : R
        data class OtherDrop(override val nanos: Long, val msg: String) : R
    }

    // 问题3修复: 增加原子事件计数器，确保高丢弃压测下计数绝对不丢失
    val eventDegradedCount = AtomicLong(0)
    val eventDropCount = AtomicLong(0)
    val eventIoCount = AtomicLong(0)
    val eventRecoveryCount = AtomicLong(0)

    fun resetEventCounts() {
        eventDegradedCount.set(0)
        eventDropCount.set(0)
        eventIoCount.set(0)
        eventRecoveryCount.set(0)
    }

    private val _eventSink = MutableSharedFlow<EventRow>(
        extraBufferCapacity = 4096,
        onBufferOverflow = BufferOverflow.DROP_OLDEST
    )
    val eventSink: SharedFlow<EventRow> = _eventSink.asSharedFlow()

    // 注册进每次 LogX.Config(errorHandler = LogXProbe::onEvent)
    fun onEvent(kind: LogX.EventKind, code: Int, msg: String) {
        when (kind) {
            LogX.EventKind.DEGRADED -> eventDegradedCount.incrementAndGet()
            LogX.EventKind.DROP -> {
                eventDropCount.incrementAndGet()
                slot.get()?.let {
                    it.hit = true
                    it.code = code
                    it.msg = msg
                }
            }
            LogX.EventKind.IO -> eventIoCount.incrementAndGet()
            LogX.EventKind.RECOVERY -> eventRecoveryCount.incrementAndGet()
        }
        _eventSink.tryEmit(EventRow(kind, code, msg))   // 全局事件流，用于控制台展示
    }

    fun write(level: LogX.Level, tag: String, payload: ByteArray): R {
        val c = Capture()
        slot.set(c)
        val t0 = System.nanoTime()
        try {
            LogX.log(level, tag, payload)
        } finally {
            slot.set(null)
        }
        val dt = System.nanoTime() - t0
        // 问题 P2 修复: 优先按 Native 错误码常量进行强类型匹配，彻底消除文案脆弱性
        return when {
            !c.hit                             -> R.Ok(dt)
            c.code == LogX.ERR_NO_SPACE        -> R.NoSpace(dt)
            c.code == LogX.ERR_TOO_LARGE       -> R.TooLarge(dt)
            c.code == LogX.ERR_INVALID_ARG     -> R.Invalid(dt)
            // 向后兼容兜底 (若对接未升级的旧 native 库)
            c.msg.contains("staging full")     -> R.NoSpace(dt)
            c.msg.contains("log too large")    -> R.TooLarge(dt)
            c.msg.contains("invalid log input")-> R.Invalid(dt)
            else                               -> R.OtherDrop(dt, c.msg)
        }
    }
}

