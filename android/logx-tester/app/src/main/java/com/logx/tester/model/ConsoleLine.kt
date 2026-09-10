package com.logx.tester.model

enum class LineKind {
    WRITE_OK, DROP, DEGRADED, IO, RECOVERY, SYSTEM
}

data class ConsoleLine(
    val seq: Long,
    val tsMs: Long = System.currentTimeMillis(),
    val level: String = "",
    val tag: String = "",
    val text: String,
    val kind: LineKind = LineKind.SYSTEM
)

