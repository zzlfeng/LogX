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
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.logx.tester.model.ConsoleLine
import com.logx.tester.model.LineKind
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LogConsole(
    lines: List<ConsoleLine>,
    bufferCapacity: Int,
    onCapacityChange: (Int) -> Unit,
    onClear: () -> Unit,
    modifier: Modifier = Modifier
) {
    var autoScroll by remember { mutableStateOf(true) }
    val listState = rememberLazyListState()

    // 问题5修复: 以最后一条日志的序号为 key，即使达到缓存上限依然保持平滑自动滚动
    LaunchedEffect(lines.lastOrNull()?.seq, autoScroll) {
        if (autoScroll && lines.isNotEmpty()) {
            listState.scrollToItem(lines.size - 1)
        }
    }

    Card(
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f)),
        modifier = modifier.fillMaxWidth()
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            // 顶栏控制
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column {
                    Text("实时日志与事件流", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.Bold)
                    Text("当前缓存: ${lines.size}/$bufferCapacity 行 \n(100ms合并渲染)", fontSize = 11.sp, color = Color.Gray)
                }

                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("自动滚动", fontSize = 12.sp)
                    Switch(
                        checked = autoScroll,
                        onCheckedChange = { autoScroll = it }
                    )
                    OutlinedButton(onClick = onClear) {
                        Text("清空", fontSize = 12.sp)
                    }
                }
            }

            Spacer(modifier = Modifier.height(8.dp))

            // 日志展示列表
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(280.dp)
                    .background(Color(0xFF1E1E1E), RoundedCornerShape(6.dp))
                    .padding(8.dp)
            ) {
                if (lines.isEmpty()) {
                    Text("暂无日志记录", color = Color.Gray, fontSize = 12.sp, modifier = Modifier.align(Alignment.Center))
                } else {
                    val timeFormat = remember { SimpleDateFormat("HH:mm:ss.SSS", Locale.US) }
                    LazyColumn(state = listState, modifier = Modifier.fillMaxWidth()) {
                        items(lines, key = { it.seq }) { line ->
                            val textColor = when (line.kind) {
                                LineKind.WRITE_OK -> Color(0xFF81C784)
                                LineKind.DROP -> Color(0xFFE57373)
                                LineKind.DEGRADED -> Color(0xFFFFB74D)
                                LineKind.IO -> Color(0xFFBA68C8)
                                LineKind.RECOVERY -> Color(0xFF64B5F6)
                                LineKind.SYSTEM -> Color(0xFFB0BEC5)
                            }

                            val timeStr = remember(line.tsMs) { timeFormat.format(Date(line.tsMs)) }

                            Row(modifier = Modifier.fillMaxWidth().padding(vertical = 1.dp)) {
                                Text(
                                    text = "$timeStr ",
                                    color = Color.DarkGray,
                                    fontSize = 11.sp,
                                    fontFamily = FontFamily.Monospace
                                )
                                if (line.level.isNotEmpty()) {
                                    Text(
                                        text = "${line.level}/",
                                        color = textColor,
                                        fontSize = 11.sp,
                                        fontWeight = FontWeight.Bold,
                                        fontFamily = FontFamily.Monospace
                                    )
                                }
                                if (line.tag.isNotEmpty()) {
                                    Text(
                                        text = "[${line.tag}] ",
                                        color = Color.LightGray,
                                        fontSize = 11.sp,
                                        fontFamily = FontFamily.Monospace
                                    )
                                }
                                Text(
                                    text = line.text,
                                    color = textColor,
                                    fontSize = 11.sp,
                                    fontFamily = FontFamily.Monospace
                                )
                            }
                        }
                    }
                }
            }
        }
    }
}

