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
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
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
import com.zzlfeng.logx.LogX
import com.logx.tester.LogFileInfo
import com.logx.tester.engine.EngineState
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ActionPanel(
    engineState: EngineState,
    lastWriteResult: String?,
    logFiles: List<LogFileInfo>,
    onApplyAndRestart: () -> Unit,
    onStop: () -> Unit,
    onWriteSingle: (level: LogX.Level, tag: String, msg: String, payloadBytes: Int) -> Unit,
    onFlush: () -> Unit,
    onExport: (from: String, to: String) -> Unit,
    onRefreshFiles: () -> Unit,
    modifier: Modifier = Modifier
) {
    var selectedLevel by remember { mutableStateOf(LogX.Level.I) }
    var tagText by remember { mutableStateOf("TesterTag") }
    var msgText by remember { mutableStateOf("Test log message") }
    var payloadSize by remember { mutableStateOf(128) }

    val todayStr = remember { SimpleDateFormat("yyyyMMdd", Locale.US).format(Date()) }
    var exportFrom by remember { mutableStateOf(todayStr) }
    var exportTo by remember { mutableStateOf(todayStr) }

    Column(modifier = modifier.fillMaxWidth()) {
        Text(
            text = "操作动作区",
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.Bold,
            modifier = Modifier.padding(bottom = 8.dp)
        )

        // 1. 实例生命周期控制按钮
        Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(
                onClick = onApplyAndRestart,
                modifier = Modifier.weight(1.2f)
            ) {
                Text(if (engineState == EngineState.Running) "应用配置并重启" else "启动 LogX 实例")
            }
            OutlinedButton(
                onClick = onStop,
                enabled = engineState == EngineState.Running,
                colors = ButtonDefaults.outlinedButtonColors(contentColor = Color.Red),
                modifier = Modifier.weight(0.8f)
            ) {
                Text("停止实例")
            }
        }

        Spacer(modifier = Modifier.height(12.dp))

        // 2. 单条写入卡片
        Card(
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f)),
            modifier = Modifier.fillMaxWidth()
        ) {
            Column(modifier = Modifier.padding(12.dp)) {
                Text("单条写入测试", fontWeight = FontWeight.Bold)

                Spacer(modifier = Modifier.height(8.dp))

                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    // 等级下拉
                    var lvlExpanded by remember { mutableStateOf(false) }
                    ExposedDropdownMenuBox(
                        expanded = lvlExpanded,
                        onExpandedChange = { lvlExpanded = it },
                        modifier = Modifier.weight(1f)
                    ) {
                        OutlinedTextField(
                            value = selectedLevel.name,
                            onValueChange = {},
                            readOnly = true,
                            label = { Text("等级") },
                            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = lvlExpanded) },
                            modifier = Modifier.menuAnchor().fillMaxWidth()
                        )
                        ExposedDropdownMenu(
                            expanded = lvlExpanded,
                            onDismissRequest = { lvlExpanded = false }
                        ) {
                            LogX.Level.values().forEach { lvl ->
                                DropdownMenuItem(
                                    text = { Text(lvl.name) },
                                    onClick = {
                                        selectedLevel = lvl
                                        lvlExpanded = false
                                    }
                                )
                            }
                        }
                    }

                    // Tag 输入
                    OutlinedTextField(
                        value = tagText,
                        onValueChange = { tagText = it },
                        label = { Text("Tag (${tagText.length}B)") },
                        isError = tagText.length > 64,
                        supportingText = { if (tagText.length > 64) Text("⚠️ 超过 64B 上限", color = Color.Red) },
                        modifier = Modifier.weight(2f)
                    )
                }

                Spacer(modifier = Modifier.height(8.dp))

                OutlinedTextField(
                    value = msgText,
                    onValueChange = { msgText = it },
                    label = { Text("消息内容 / 前缀") },
                    modifier = Modifier.fillMaxWidth()
                )

                Spacer(modifier = Modifier.height(8.dp))

                // Payload 大小滑块
                Column {
                    Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                        Text("Payload 填充大小: $payloadSize 字节", fontWeight = FontWeight.SemiBold)
                        when {
                            payloadSize > 60000 -> Text("⚠️ >60KB (预期 INVALID)", color = Color.Red, fontSize = 12.sp)
                            payloadSize > 20480 -> Text("📦 >20KB (预期 LGXE 分片)", color = Color(0xFFE65100), fontSize = 12.sp)
                        }
                    }
                    Slider(
                        value = payloadSize.toFloat(),
                        onValueChange = { payloadSize = it.toInt() },
                        valueRange = 0f..70000f,
                        steps = 70
                    )
                }

                Spacer(modifier = Modifier.height(8.dp))

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Button(
                        onClick = { onWriteSingle(selectedLevel, tagText, msgText, payloadSize) },
                        enabled = engineState == EngineState.Running
                    ) {
                        Text("写入单条日志")
                    }

                    // 写入结果即时回显
                    lastWriteResult?.let { res ->
                        val (bgColor, textColor) = when {
                            res.startsWith("OK") -> Color(0xFFE8F5E9) to Color(0xFF2E7D32)
                            res.startsWith("NO_SPACE") -> Color(0xFFFFEBEE) to Color(0xFFC62828)
                            res.startsWith("TOO_LARGE") -> Color(0xFFFFF3E0) to Color(0xFFEF6C00)
                            res.startsWith("INVALID") -> Color(0xFFEDE7F6) to Color(0xFF6A1B9A)
                            else -> Color.LightGray to Color.DarkGray
                        }
                        Box(
                            modifier = Modifier
                                .background(bgColor, RoundedCornerShape(4.dp))
                                .padding(horizontal = 8.dp, vertical = 4.dp)
                        ) {
                            Text(res, color = textColor, fontWeight = FontWeight.Bold, fontSize = 12.sp)
                        }
                    }
                }
            }
        }

        Spacer(modifier = Modifier.height(12.dp))

        // 3. 存储与落盘操作
        Card(
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f)),
            modifier = Modifier.fillMaxWidth()
        ) {
            Column(modifier = Modifier.padding(12.dp)) {
                Text("落盘控制与导出", fontWeight = FontWeight.Bold)

                Spacer(modifier = Modifier.height(8.dp))

                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = onFlush,
                        enabled = engineState == EngineState.Running,
                        modifier = Modifier.weight(1f)
                    ) {
                        Text("手动 Flush (阻塞)")
                    }

                    OutlinedButton(
                        onClick = onRefreshFiles,
                        modifier = Modifier.weight(1f)
                    ) {
                        Text("刷新落盘文件")
                    }
                }

                Spacer(modifier = Modifier.height(8.dp))

                // Export
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    OutlinedTextField(
                        value = exportFrom,
                        onValueChange = { exportFrom = it },
                        label = { Text("From (yyyyMMdd)") },
                        modifier = Modifier.weight(1f)
                    )
                    OutlinedTextField(
                        value = exportTo,
                        onValueChange = { exportTo = it },
                        label = { Text("To (yyyyMMdd)") },
                        modifier = Modifier.weight(1f)
                    )
                    Button(
                        onClick = { onExport(exportFrom, exportTo) },
                        enabled = engineState == EngineState.Running
                    ) {
                        Text("导出")
                    }
                }

                // 文件列表展示
                if (logFiles.isNotEmpty()) {
                    Spacer(modifier = Modifier.height(8.dp))
                    Text("当前落盘文件列表 (${logFiles.size} 个):", fontWeight = FontWeight.SemiBold, fontSize = 13.sp)
                    Column(modifier = Modifier.fillMaxWidth()) {
                        logFiles.forEach { f ->
                            Row(
                                modifier = Modifier.fillMaxWidth().padding(vertical = 2.dp),
                                horizontalArrangement = Arrangement.SpaceBetween
                            ) {
                                Text(f.name, fontSize = 12.sp, color = MaterialTheme.colorScheme.primary)
                                Text("${f.sizeBytes} 字节", fontSize = 12.sp, color = Color.Gray)
                            }
                        }
                    }
                }
            }
        }
    }
}

