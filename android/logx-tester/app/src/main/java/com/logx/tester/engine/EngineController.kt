package com.logx.tester.engine

import android.content.Context
import com.zzlfeng.logx.LogX
import com.logx.tester.model.TesterConfig
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

enum class EngineState { Stopped, Running }

sealed interface StartResult {
    object Ok : StartResult
    data class Failed(val errorClass: String, val message: String) : StartResult
}

object EngineController {
    private val gate = Mutex()
    private val _state = MutableStateFlow(EngineState.Stopped)
    val state: StateFlow<EngineState> = _state.asStateFlow()

    suspend fun applyAndRestart(ctx: Context, cfg: TesterConfig): StartResult = gate.withLock {
        if (_state.value == EngineState.Running) {
            LogX.close()
            _state.value = EngineState.Stopped
        }
        return try {
            LogX.init(ctx, cfg.toLogXConfig(ctx, LogXProbe::onEvent))
            _state.value = EngineState.Running
            StartResult.Ok
        } catch (t: Throwable) {
            _state.value = EngineState.Stopped
            StartResult.Failed(t::class.simpleName ?: "Throwable", t.message ?: "Unknown error")
        }
    }

    suspend fun stop(): Unit = gate.withLock {
        if (_state.value == EngineState.Running) {
            LogX.close()
            _state.value = EngineState.Stopped
        }
    }
}

