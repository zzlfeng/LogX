package com.logx.tester.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.logx.tester.model.TesterMetrics

@Composable
fun MetricsPanel(
    metrics: TesterMetrics,
    onClearMetrics: () -> Unit,
    modifier: Modifier = Modifier
) {
    Card(
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f)),
        modifier = modifier.fillMaxWidth()
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text("统计指标区", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold)
                OutlinedButton(onClick = onClearMetrics) {
                    Text("重置计数", fontSize = 12.sp)
                }
            }

            Spacer(modifier = Modifier.height(8.dp))

            // 1. 4 张计数卡
            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                MetricCard(
                    title = "writeOk",
                    count = metrics.writeOk,
                    color = Color(0xFF2E7D32),
                    modifier = Modifier.weight(1f)
                )
                MetricCard(
                    title = "noSpace",
                    count = metrics.dropNoSpace,
                    color = Color(0xFFC62828),
                    modifier = Modifier.weight(1f)
                )
                MetricCard(
                    title = "tooLarge",
                    count = metrics.dropTooLarge,
                    color = Color(0xFFEF6C00),
                    modifier = Modifier.weight(1f)
                )
                MetricCard(
                    title = "invalid",
                    count = metrics.invalidInput,
                    color = Color(0xFF6A1B9A),
                    modifier = Modifier.weight(1f)
                )
            }

            Spacer(modifier = Modifier.height(10.dp))

            // 2. 延迟与吞吐
            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("写入延迟 (µs)", fontWeight = FontWeight.SemiBold, fontSize = 13.sp)
                    Text(
                        "P50: ${"%.1f".format(metrics.latency.p50Us)} | P90: ${"%.1f".format(metrics.latency.p90Us)} | P99: ${"%.1f".format(metrics.latency.p99Us)} | Max: ${"%.1f".format(metrics.latency.maxUs)}",
                        fontSize = 12.sp,
                        color = MaterialTheme.colorScheme.primary
                    )
                }
                Column(modifier = Modifier.weight(0.6f)) {
                    Text("瞬时/压测吞吐", fontWeight = FontWeight.SemiBold, fontSize = 13.sp)
                    Text("${"%.1f".format(metrics.throughputLogsPerSec)} logs/s", fontSize = 12.sp, color = MaterialTheme.colorScheme.secondary)
                }
            }

            Spacer(modifier = Modifier.height(10.dp))

            // 3. 事件流计数
            Text("库内核事件流累计 (ErrorHandler)", fontWeight = FontWeight.SemiBold, fontSize = 13.sp)
            Row(modifier = Modifier.fillMaxWidth().padding(top = 4.dp), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("DEGRADED: ${metrics.eventDegraded}", fontSize = 12.sp, color = Color(0xFFF57C00))
                Text("DROP: ${metrics.eventDrop}", fontSize = 12.sp, color = Color(0xFFD32F2F))
                Text("IO: ${metrics.eventIo}", fontSize = 12.sp, color = Color(0xFF7B1FA2))
                Text("RECOVERY: ${metrics.eventRecovery}", fontSize = 12.sp, color = Color(0xFF1976D2))
            }

            Spacer(modifier = Modifier.height(10.dp))

            // 4. Native 计数 N/A 声明
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(Color.LightGray.copy(alpha = 0.2f), RoundedCornerShape(4.dp))
                    .padding(8.dp)
            ) {
                Column {
                    Text("ℹ️ Native 统计指标透明度说明:", fontSize = 11.sp, fontWeight = FontWeight.SemiBold, color = Color.Gray)
                    Text("• truncated_blocks: N/A（仅 recovery 产生，事件不含条数）", fontSize = 11.sp, color = Color.Gray)
                    Text("• pad_records: N/A（库 core 内无递增点，当前恒为 0）", fontSize = 11.sp, color = Color.Gray)
                    Text("• block_write_errors: 间接感知（flusher 部分写错误无伴随事件）", fontSize = 11.sp, color = Color.Gray)
                }
            }
        }
    }
}

@Composable
private fun MetricCard(
    title: String,
    count: Long,
    color: Color,
    modifier: Modifier = Modifier
) {
    Box(
        modifier = modifier
            .background(color.copy(alpha = 0.1f), RoundedCornerShape(6.dp))
            .padding(vertical = 8.dp, horizontal = 4.dp),
        contentAlignment = Alignment.Center
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(title, fontSize = 11.sp, color = color, fontWeight = FontWeight.Medium)
            Text(count.toString(), fontSize = 15.sp, fontWeight = FontWeight.Bold, color = color)
        }
    }
}

