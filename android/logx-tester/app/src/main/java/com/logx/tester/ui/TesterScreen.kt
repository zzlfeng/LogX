package com.logx.tester.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
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
import com.logx.tester.TesterViewModel
import com.logx.tester.engine.EngineState
import com.logx.tester.model.PresetRegistry

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun TesterScreen(
    viewModel: TesterViewModel,
    modifier: Modifier = Modifier
) {
    val config by viewModel.config.collectAsState()
    val engineState by viewModel.engineState.collectAsState()
    val statusMsg by viewModel.statusMessage.collectAsState()
    val metrics by viewModel.metrics.collectAsState()
    val lastWriteResult by viewModel.lastWriteResult.collectAsState()
    val logFiles by viewModel.logFiles.collectAsState()
    val consoleLines by viewModel.consoleLines.collectAsState()

    val isStressRunning by viewModel.isStressRunning.collectAsState()
    val stressProgress by viewModel.stressProgress.collectAsState()
    val stressThroughput by viewModel.stressThroughput.collectAsState()
    val stressReport by viewModel.stressReport.collectAsState()

    var selectedPreset by remember { mutableStateOf(PresetRegistry.presets[0]) }
    var presetExpanded by remember { mutableStateOf(false) }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Column {
                            Text("LogX 调参测试器", fontWeight = FontWeight.Bold, fontSize = 18.sp)
                            Text(statusMsg, fontSize = 11.sp, color = Color.DarkGray)
                        }

                        // 运行状态徽章
                        Box(
                            modifier = Modifier
                                .background(
                                    if (engineState == EngineState.Running) Color(0xFF2E7D32) else Color(0xFF757575),
                                    RoundedCornerShape(12.dp)
                                )
                                .padding(horizontal = 10.dp, vertical = 4.dp)
                        ) {
                            Text(
                                if (engineState == EngineState.Running) "RUNNING" else "STOPPED",
                                color = Color.White,
                                fontWeight = FontWeight.Bold,
                                fontSize = 11.sp
                            )
                        }
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)
            )
        }
    ) { innerPadding ->
        Column(
            modifier = modifier
                .fillMaxSize()
                .padding(innerPadding)
                .verticalScroll(rememberScrollState())
                .padding(12.dp)
        ) {
            // 0. 预置测试场景一键载入卡片
            Card(
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.3f)),
                modifier = Modifier.fillMaxWidth()
            ) {
                Column(modifier = Modifier.padding(12.dp)) {
                    Text("预置测试场景 (15 个全覆盖)", fontWeight = FontWeight.Bold, fontSize = 15.sp)
                    Spacer(modifier = Modifier.height(8.dp))

                    ExposedDropdownMenuBox(
                        expanded = presetExpanded,
                        onExpandedChange = { presetExpanded = it }
                    ) {
                        OutlinedTextField(
                            value = selectedPreset.name,
                            onValueChange = {},
                            readOnly = true,
                            label = { Text("选择预置场景") },
                            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = presetExpanded) },
                            modifier = Modifier.menuAnchor().fillMaxWidth()
                        )
                        ExposedDropdownMenu(
                            expanded = presetExpanded,
                            onDismissRequest = { presetExpanded = false }
                        ) {
                            PresetRegistry.presets.forEach { p ->
                                DropdownMenuItem(
                                    text = {
                                        Column {
                                            Text(p.name, fontWeight = FontWeight.SemiBold)
                                            Text(p.summary, fontSize = 11.sp, color = Color.Gray)
                                        }
                                    },
                                    onClick = {
                                        selectedPreset = p
                                        presetExpanded = false
                                    }
                                )
                            }
                        }
                    }

                    Spacer(modifier = Modifier.height(8.dp))

                    Text("预期观测结果: ${selectedPreset.expectedOutcome}", fontSize = 12.sp, color = MaterialTheme.colorScheme.primary)

                    if (selectedPreset.suggestedAction.isNotEmpty()) {
                        Spacer(modifier = Modifier.height(4.dp))
                        Text("💡 建议动作: ${selectedPreset.suggestedAction}", fontSize = 12.sp, color = Color(0xFFE65100))
                    }

                    Spacer(modifier = Modifier.height(8.dp))

                    Button(
                        onClick = { viewModel.applyPreset(selectedPreset) },
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text("一键载入并重启 LogX")
                    }
                }
            }

            Spacer(modifier = Modifier.height(12.dp))

            // 1. 参数配置区
            ConfigPanel(
                config = config,
                onConfigChange = { modifierFn -> viewModel.updateConfig(modifierFn) }
            )

            Spacer(modifier = Modifier.height(12.dp))

            // 2. 动作操作区
            ActionPanel(
                engineState = engineState,
                lastWriteResult = lastWriteResult,
                logFiles = logFiles,
                onApplyAndRestart = { viewModel.applyAndRestart() },
                onStop = { viewModel.stopInstance() },
                onWriteSingle = { lvl, tag, msg, payloadSize -> viewModel.writeSingleLog(lvl, tag, msg, payloadSize) },
                onFlush = { viewModel.flushSync() },
                onExport = { from, to -> viewModel.exportLogs(from, to) },
                onRefreshFiles = { viewModel.refreshLogFiles() }
            )

            Spacer(modifier = Modifier.height(12.dp))

            // 3. 统计指标区
            MetricsPanel(
                metrics = metrics,
                onClearMetrics = { viewModel.clearMetrics() }
            )

            Spacer(modifier = Modifier.height(12.dp))

            // 4. 并发压测面板
            StressPanel(
                engineState = engineState,
                isRunning = isStressRunning,
                progress = stressProgress,
                throughput = stressThroughput,
                report = stressReport,
                onStart = { threads, total, payload, delay, flushInterval, recordEveryLine ->
                    viewModel.startStressTest(threads, total, payload, delay, flushInterval, recordEveryLine)
                },
                onStop = { viewModel.stopStressTest() }
            )

            Spacer(modifier = Modifier.height(12.dp))

            // 5. 实时日志控制台
            LogConsole(
                lines = consoleLines,
                bufferCapacity = viewModel.maxConsoleBuffer,
                onCapacityChange = { viewModel.maxConsoleBuffer = it },
                onClear = { viewModel.clearConsole() }
            )
        }
    }
}

