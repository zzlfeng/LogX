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
 * 【问题记录 P2 · 为什么当前采用文案匹配而非错误码映射】:
 * 理论上应通过 Native 侧返回的错误码常量（如 LOGX_ERR_NO_SPACE / LOGX_ERR_TOO_LARGE）判定丢弃类型。
 * 但经核实当前发布的 C++ 内核源码（core/src/logx_c.cpp:425, 430, 452）：
 *   - `br == kBuildRecordsInvalidInput` -> `emit_event(inst, LOGX_EV_DROP, -1, "invalid log input")`
 *   - `total > inst->max_log_length`    -> `emit_event(inst, LOGX_EV_DROP, -1, "log too large")`
 *   - `st == Staging::WriteStatus::kFull`-> `emit_event(inst, LOGX_EV_DROP, -1, "staging full")`
 * 当前 Native 核心对所有 DROP 事件上报的 code 恒为 `-1`，且 JNI `nativeWrite` 返回值为 void 未透传 status_e。
 * 因此在不动 Native 二进制库的前提下，匹配 msg 文案是目前唯一可行的区分手段。
 * 后续演进路线：Native 库迭代时，将 `emit_event` 的 code 改为真实 status 码，届时上层即可平滑切换为按常量 code 匹配。
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
        // 问题 P2 待改进项：Native 端若升级 emit_event 错误码，可改由 c.code 映射
        return when {
            !c.hit                             -> R.Ok(dt)
            c.msg.contains("staging full")     -> R.NoSpace(dt)
            c.msg.contains("log too large")    -> R.TooLarge(dt)
            c.msg.contains("invalid log input")-> R.Invalid(dt)
            else                               -> R.OtherDrop(dt, c.msg)
        }
    }
}

