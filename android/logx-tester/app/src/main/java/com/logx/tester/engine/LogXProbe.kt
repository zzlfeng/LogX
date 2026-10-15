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
 *   - 超长(> max_log_length 或 >60KB 用户载荷硬上限, B-1B/T5.5) -> 截断保留前缀后
 *     照常落环并返回 LOGX_OK, 同步事件 `LOGX_EV_TRUNCATED`(code 取 LOGX_OK,
 *     msg "log truncated (dropped N bytes)") —— 有损写入, 非丢弃
 *   - 病态小 max_log_length(kBuildRecordsCannotFit) -> 整条丢弃, 同步事件
 *     `LOGX_EV_DROP / LOGX_ERR_TOO_LARGE` ("log too large")
 *   - `st == Staging::WriteStatus::kFull`-> `emit_event(inst, LOGX_EV_DROP, LOGX_ERR_NO_SPACE, "staging full")`
 * 上层优先根据 `c.kind` + `c.code`（LogX.ERR_NO_SPACE / ERR_TOO_LARGE / ERR_INVALID_ARG）
 * 进行严格匹配，彻底消除了对错误消息文案的脆弱依赖（文案匹配仅作向后兼容兜底）。
 * B-3 方案B/T5.6: 截断改用独立事件种类 LOGX_EV_TRUNCATED 承载, R5-3 记录的
 * "ERR_TOO_LARGE 共码须按 msg 前缀分流"文本耦合已消除。
 */
object LogXProbe {
    data class EventRow(val kind: LogX.EventKind, val code: Int, val msg: String)
    class Capture { var hit = false; var kind: LogX.EventKind? = null; var code = 0; var msg = "" }
    private val slot = ThreadLocal<Capture?>()

    sealed interface R {
        val nanos: Long
        data class Ok(override val nanos: Long) : R
        data class NoSpace(override val nanos: Long) : R
        data class TooLarge(override val nanos: Long) : R
        /** 超长截断保前缀(B-1B/T5.5): 有损写入成功, 记录已落盘; [msg] 含丢弃字节数 */
        data class Truncated(override val nanos: Long, val msg: String) : R
        data class Invalid(override val nanos: Long) : R
        data class OtherDrop(override val nanos: Long, val msg: String) : R
    }

    // 问题3修复: 增加原子事件计数器，确保高丢弃压测下计数绝对不丢失
    val eventDegradedCount = AtomicLong(0)
    val eventDropCount = AtomicLong(0)
    val eventIoCount = AtomicLong(0)
    val eventRecoveryCount = AtomicLong(0)
    val eventTruncatedCount = AtomicLong(0)

    fun resetEventCounts() {
        eventDegradedCount.set(0)
        eventDropCount.set(0)
        eventIoCount.set(0)
        eventRecoveryCount.set(0)
        eventTruncatedCount.set(0)
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
                    it.kind = kind
                    it.code = code
                    it.msg = msg
                }
            }
            LogX.EventKind.TRUNCATED -> {
                // B-3 方案B/T5.6: 截断=有损写入成功, 独立事件种类;计入哨兵供分类
                eventTruncatedCount.incrementAndGet()
                slot.get()?.let {
                    it.hit = true
                    it.kind = kind
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
        // T-1: 必须读取 LogX.log 返回码 —— 实例被并发 stop/restart 时 executeOp 返回
        // null → log() 返回 ERR_STATE 且不发任何事件; 仅靠 ThreadLocal 哨兵会把这批
        // 未实际写入的调用全部计为假 OK, 吞吐/延迟/丢弃指标集体失真。
        val rc: Int
        try {
            rc = LogX.log(level, tag, payload)
        } finally {
            // T-11: 用 remove() 替代 set(null)——set(null) 会在 ThreadLocalMap 留下
            // 常驻的 null-value entry, 池化线程(Dispatchers.IO)复用重装时缓慢泄漏 entry。
            slot.remove()
        }
        val dt = System.nanoTime() - t0
        // 问题 P2 修复: 优先按 Native 错误码常量强类型匹配，同时彻底解决 code == -1 兜底分支死代码问题
        return when {
            // T-1: 无同步事件但返回码非 0 —— 实例未就绪(ERR_STATE)等, 归入失败分支。
            !c.hit && rc != 0                 -> R.OtherDrop(dt, "log() rc=$rc (instance not ready)")
            !c.hit                             -> R.Ok(dt)
            // B-3 方案B/T5.6: 截断独立事件种类, 按 kind 分流(文案耦合消除)
            c.kind == LogX.EventKind.TRUNCATED -> R.Truncated(dt, c.msg)
            c.code == LogX.ERR_NO_SPACE        -> R.NoSpace(dt)
            // 走到此处的 TOO_LARGE = 病态小上限丢弃(B-3 后截断不再占用该码)
            c.code == LogX.ERR_TOO_LARGE       -> R.TooLarge(dt)
            // 兜底处理: code == -1 既对应真实 ERR_INVALID_ARG，也可能是旧 Native 库写死的兜底值
            c.code == LogX.ERR_INVALID_ARG || c.code == -1 -> when {
                c.msg.contains("staging full")     -> R.NoSpace(dt)
                c.msg.contains("log too large")    -> R.TooLarge(dt)
                c.msg.contains("invalid log input")-> R.Invalid(dt)
                else                               -> R.Invalid(dt)
            }
            c.msg.contains("staging full")     -> R.NoSpace(dt)
            c.msg.contains("log too large")    -> R.TooLarge(dt)
            c.msg.contains("invalid log input")-> R.Invalid(dt)
            else                               -> R.OtherDrop(dt, c.msg)
        }
    }
}

