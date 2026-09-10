package com.logx.tester.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.logx.tester.engine.EngineState

@Composable
fun StressPanel(
    engineState: EngineState,
    isRunning: Boolean,
    progress: Int,
    throughput: Double,
    report: String?,
    onStart: (threads: Int, total: Int, payload: Int, delay: Long, flushInterval: Int, recordEveryLine: Boolean) -> Unit,
    onStop: () -> Unit,
    modifier: Modifier = Modifier
) {
    var isExpanded by remember { mutableStateOf(false) }

    var threads by remember { mutableStateOf(4) }
    val totalSteps = listOf(100, 1000, 5000, 10000, 50000, 100000, 500000)
    var totalStepIdx by remember { mutableStateOf(2) } // default 5000
    var payloadSize by remember { mutableStateOf(512) }
    var delayMs by remember { mutableStateOf(0L) }
    var enablePeriodicFlush by remember { mutableStateOf(false) }
    var flushInterval by remember { mutableStateOf(500) }
    var recordEveryLine by remember { mutableStateOf(false) }

    Card(
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f)),
        modifier = modifier.fillMaxWidth()
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable { isExpanded = !isExpanded },
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("并发压测面板", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold)
                    Text(
                        if (isRunning) "压测运行中... ($progress 条, ${"%.0f".format(throughput)} logs/s)" else "可调线程、条数、payload、周期 flush 与延迟",
                        style = MaterialTheme.typography.bodySmall,
                        color = if (isRunning) MaterialTheme.colorScheme.primary else Color.Gray
                    )
                }
                Text(if (isExpanded) "▲ 收起" else "▼ 展开", fontSize = 12.sp, color = MaterialTheme.colorScheme.primary)
            }

            if (isExpanded || isRunning) {
                Spacer(modifier = Modifier.height(10.dp))

                if (!isRunning) {
                    // 1. 线程数滑块 1..50
                    Column {
                        Text("并发线程数: $threads 线程", fontWeight = FontWeight.SemiBold)
                        Slider(
                            value = threads.toFloat(),
                            onValueChange = { threads = it.toInt() },
                            valueRange = 1f..50f,
                            steps = 48
                        )
                    }

                    // 2. 总条数
                    Column {
                        val currentTotal = totalSteps[totalStepIdx]
                        Text("总日志条数: $currentTotal 条", fontWeight = FontWeight.SemiBold)
                        Slider(
                            value = totalStepIdx.toFloat(),
                            onValueChange = { totalStepIdx = it.toInt().coerceIn(0, totalSteps.lastIndex) },
                            valueRange = 0f..(totalSteps.size - 1).toFloat(),
                            steps = totalSteps.size - 2
                        )
                    }

                    // 3. Payload 字节数
                    Column {
                        Text("单条 Payload 大小: $payloadSize 字节 (覆盖 20KB 分片边界)", fontWeight = FontWeight.SemiBold)
                        Slider(
                            value = payloadSize.toFloat(),
                            onValueChange = { payloadSize = it.toInt() },
                            valueRange = 16f..20480f,
                            steps = 100
                        )
                    }

                    // 4. 写入间隔
                    Column {
                        Text("线程写入休眠间隔: ${delayMs}ms", fontWeight = FontWeight.SemiBold)
                        Slider(
                            value = delayMs.toFloat(),
                            onValueChange = { delayMs = it.toLong() },
                            valueRange = 0f..10f,
                            steps = 9
                        )
                    }

                    // 5. 周期 Flush 开关
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Column(modifier = Modifier.weight(1f)) {
                            Text("周期性触发 Flush", fontWeight = FontWeight.SemiBold)
                            Text("每 $flushInterval 条调用一次 LogX.flush()", style = MaterialTheme.typography.bodySmall, color = Color.Gray)
                        }
                        Switch(
                            checked = enablePeriodicFlush,
                            onCheckedChange = { enablePeriodicFlush = it }
                        )
                    }

                    Spacer(modifier = Modifier.height(6.dp))

                    // 6. 记录每条日志开关
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Column(modifier = Modifier.weight(1f)) {
                            Text("逐条记录到控制台 (默认采样)", fontWeight = FontWeight.SemiBold)
                            Text("高吞吐下开启可能增加 UI 渲染开销", style = MaterialTheme.typography.bodySmall, color = Color.Gray)
                        }
                        Switch(
                            checked = recordEveryLine,
                            onCheckedChange = { recordEveryLine = it }
                        )
                    }

                    Spacer(modifier = Modifier.height(10.dp))
                }

                // 压测进行中 UI
                if (isRunning) {
                    val targetTotal = totalSteps[totalStepIdx]
                    val progressFraction = if (targetTotal > 0) (progress.toFloat() / targetTotal).coerceIn(0f, 1f) else 0f
                    Column(modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp)) {
                        Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                            Text("进度: $progress / $targetTotal", fontWeight = FontWeight.Bold, fontSize = 13.sp)
                            Text("当前吞吐: ${"%.1f".format(throughput)} logs/s", fontWeight = FontWeight.Bold, color = MaterialTheme.colorScheme.primary, fontSize = 13.sp)
                        }
                        Spacer(modifier = Modifier.height(4.dp))
                        LinearProgressIndicator(
                            progress = { progressFraction },
                            modifier = Modifier.fillMaxWidth().height(8.dp),
                        )
                    }
                }

                // 控制按钮
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (!isRunning) {
                        Button(
                            onClick = {
                                onStart(
                                    threads,
                                    totalSteps[totalStepIdx],
                                    payloadSize,
                                    delayMs,
                                    if (enablePeriodicFlush) flushInterval else 0,
                                    recordEveryLine
                                )
                            },
                            enabled = engineState == EngineState.Running,
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Text("启动压测 (${threads}线程 / ${totalSteps[totalStepIdx]}条)")
                        }
                    } else {
                        Button(
                            onClick = onStop,
                            colors = ButtonDefaults.buttonColors(containerColor = Color.Red),
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Text("中止压测")
                        }
                    }
                }

                // 压测分位数报告
                report?.let { rep ->
                    Spacer(modifier = Modifier.height(10.dp))
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .background(
                                if (rep.contains("数据失真")) Color(0xFFFFEBEE) else Color(0xFFE8F5E9),
                                RoundedCornerShape(6.dp)
                            )
                            .padding(10.dp)
                    ) {
                        Text(
                            text = rep,
                            fontSize = 12.sp,
                            color = if (rep.contains("数据失真")) Color(0xFFB71C1C) else Color(0xFF1B5E20),
                            fontWeight = FontWeight.Medium
                        )
                    }
                }
            }
        }
    }
}

