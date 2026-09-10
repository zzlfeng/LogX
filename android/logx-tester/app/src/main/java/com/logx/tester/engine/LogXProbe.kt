package com.logx.tester.engine

import com.zzlfeng.logx.LogX
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.asSharedFlow

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

    private val _eventSink = MutableSharedFlow<EventRow>(extraBufferCapacity = 4096)
    val eventSink: SharedFlow<EventRow> = _eventSink.asSharedFlow()

    // 注册进每次 LogX.Config(errorHandler = LogXProbe::onEvent)
    fun onEvent(kind: LogX.EventKind, code: Int, msg: String) {
        if (kind == LogX.EventKind.DROP) {
            slot.get()?.let {
                it.hit = true
                it.code = code
                it.msg = msg
            }
        }
        _eventSink.tryEmit(EventRow(kind, code, msg))   // 全局事件流，始终记录
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
        return when {
            !c.hit                             -> R.Ok(dt)
            c.msg.contains("staging full")     -> R.NoSpace(dt)
            c.msg.contains("log too large")    -> R.TooLarge(dt)
            c.msg.contains("invalid log input")-> R.Invalid(dt)
            else                               -> R.OtherDrop(dt, c.msg)
        }
    }
}

