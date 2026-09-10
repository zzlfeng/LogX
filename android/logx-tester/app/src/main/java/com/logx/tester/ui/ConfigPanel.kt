package com.logx.tester.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
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
import com.zzlfeng.logx.LogX
import com.logx.tester.model.KeyProviderMode
import com.logx.tester.model.TesterConfig

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ConfigPanel(
    config: TesterConfig,
    onConfigChange: ((TesterConfig) -> TesterConfig) -> Unit,
    modifier: Modifier = Modifier
) {
    var expandedA by remember { mutableStateOf(false) }
    var expandedB by remember { mutableStateOf(false) }
    var expandedC by remember { mutableStateOf(false) }
    var expandedD by remember { mutableStateOf(false) }

    Column(modifier = modifier.fillMaxWidth()) {
        Text(
            text = "参数配置 (4 折叠组)",
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.Bold,
            modifier = Modifier.padding(bottom = 8.dp)
        )

        // 组 A: 路径与隔离
        CollapsibleGroupCard(
            title = "A. 路径与隔离",
            subtitle = "logDir=${config.logDirName}, staging=${config.stagingDirName}${if (config.useCacheDir) " (cacheDir)" else ""}",
            isExpanded = expandedA,
            onToggle = { expandedA = !expandedA }
        ) {
            OutlinedTextField(
                value = config.logDirName,
                onValueChange = { v -> onConfigChange { it.copy(logDirName = v) } },
                label = { Text("logDir (默认 'logs')") },
                modifier = Modifier.fillMaxWidth()
            )
            Spacer(modifier = Modifier.height(8.dp))
            OutlinedTextField(
                value = config.stagingDirName,
                onValueChange = { v -> onConfigChange { it.copy(stagingDirName = v) } },
                label = { Text("stagingDir (默认 'logx_staging')") },
                modifier = Modifier.fillMaxWidth()
            )
            Spacer(modifier = Modifier.height(8.dp))
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("自动多进程隔离 (autoProcessIsolation)", fontWeight = FontWeight.SemiBold)
                    Text("按进程名后缀创建子目录，防止多进程争抢", style = MaterialTheme.typography.bodySmall, color = Color.Gray)
                }
                Switch(
                    checked = config.autoProcessIsolation,
                    onCheckedChange = { v -> onConfigChange { it.copy(autoProcessIsolation = v) } }
                )
            }
            Spacer(modifier = Modifier.height(8.dp))
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("staging 放 cacheDir (负向测试)", fontWeight = FontWeight.SemiBold, color = if (config.useCacheDir) Color.Red else Color.Unspecified)
                    Text("预期触发 Kotlin require 抛异常: 禁 cacheDir", style = MaterialTheme.typography.bodySmall, color = Color.Gray)
                }
                Switch(
                    checked = config.useCacheDir,
                    onCheckedChange = { v -> onConfigChange { it.copy(useCacheDir = v) } }
                )
            }
        }

        Spacer(modifier = Modifier.height(8.dp))

        // 组 B: 容量与节奏
        CollapsibleGroupCard(
            title = "B. 容量与节奏",
            subtitle = "maxLogLen=${config.maxLogLength}B, flushThreshold=${config.flushThreshold}B, maxFileSize=${config.maxFileSize / 1024}KB",
            isExpanded = expandedB,
            onToggle = { expandedB = !expandedB }
        ) {
            // maxLogLength 滑块
            Column {
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Text("单条上限 maxLogLength: ${config.maxLogLength} 字节", fontWeight = FontWeight.SemiBold)
                    if (config.maxLogLength > 65536 || config.maxLogLength == 0) {
                        Text("⚠️ 超出合法区间 (1..65536)", color = Color.Red, fontSize = 12.sp)
                    }
                }
                Slider(
                    value = config.maxLogLength.toFloat(),
                    onValueChange = { v -> onConfigChange { it.copy(maxLogLength = v.toInt()) } },
                    valueRange = 0f..131072f,
                    steps = 127
                )
                Text("范围 0..131072B。100000 触发 C 侧校验；131072 触发 Kotlin require", style = MaterialTheme.typography.bodySmall, color = Color.Gray)
            }

            Spacer(modifier = Modifier.height(8.dp))

            // flushThreshold 滑块
            Column {
                val flushDesc = when (config.flushThreshold) {
                    -1 -> "-1 (取默认 8KB)"
                    0 -> "0 (纯手动控制，禁用自动唤醒)"
                    else -> "${config.flushThreshold} 字节"
                }
                Text("落盘阈值 flushThreshold: $flushDesc", fontWeight = FontWeight.SemiBold)
                Slider(
                    value = config.flushThreshold.toFloat(),
                    onValueChange = { v ->
                        val intVal = v.toInt()
                        onConfigChange { it.copy(flushThreshold = if (intVal < 0) -1 else intVal) }
                    },
                    valueRange = -1f..65536f,
                    steps = 65
                )
            }

            Spacer(modifier = Modifier.height(8.dp))

            // maxFileSize 滑块
            Column {
                val sizes = listOf(4 * 1024L, 16 * 1024L, 64 * 1024L, 256 * 1024L, 1024 * 1024L, 10 * 1024 * 1024L, 64 * 1024 * 1024L)
                val labels = listOf("4KB", "16KB", "64KB", "256KB", "1MB", "10MB", "64MB")
                val curIdx = sizes.indexOfFirst { it >= config.maxFileSize }.let { if (it == -1) 5 else it }
                Text("单文件上限 maxFileSize: ${labels[curIdx]}", fontWeight = FontWeight.SemiBold)
                Slider(
                    value = curIdx.toFloat(),
                    onValueChange = { v ->
                        val idx = v.toInt().coerceIn(0, sizes.lastIndex)
                        onConfigChange { it.copy(maxFileSize = sizes[idx]) }
                    },
                    valueRange = 0f..(sizes.size - 1).toFloat(),
                    steps = sizes.size - 2
                )
            }

            Spacer(modifier = Modifier.height(8.dp))

            // retentionDays
            Column {
                Text("日志保留天数 retentionDays: ${config.retentionDays} 天", fontWeight = FontWeight.SemiBold)
                Slider(
                    value = config.retentionDays.toFloat(),
                    onValueChange = { v -> onConfigChange { it.copy(retentionDays = v.toInt()) } },
                    valueRange = 0f..30f,
                    steps = 29
                )
            }
        }

        Spacer(modifier = Modifier.height(8.dp))

        // 组 C: 编解码
        CollapsibleGroupCard(
            title = "C. 编解码",
            subtitle = "算法=${config.compress}, 加密=${config.encrypt}, Key=${config.keyProviderMode.label}",
            isExpanded = expandedC,
            onToggle = { expandedC = !expandedC }
        ) {
            // compress
            var compExpanded by remember { mutableStateOf(false) }
            ExposedDropdownMenuBox(
                expanded = compExpanded,
                onExpandedChange = { compExpanded = it }
            ) {
                OutlinedTextField(
                    value = "${config.compress.name}${if (config.compress == LogX.Compress.ZLIB) " (⚠️预期 init 失败)" else ""}",
                    onValueChange = {},
                    readOnly = true,
                    label = { Text("压缩算法 (Compress)") },
                    trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = compExpanded) },
                    modifier = Modifier.menuAnchor().fillMaxWidth()
                )
                ExposedDropdownMenu(
                    expanded = compExpanded,
                    onDismissRequest = { compExpanded = false }
                ) {
                    LogX.Compress.values().forEach { c ->
                        DropdownMenuItem(
                            text = { Text("${c.name}${if (c == LogX.Compress.ZLIB) " (拒绝: 未实现)" else ""}") },
                            onClick = {
                                onConfigChange { it.copy(compress = c) }
                                compExpanded = false
                            }
                        )
                    }
                }
            }

            Spacer(modifier = Modifier.height(8.dp))

            // encrypt 开关
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("加密开启 (encrypt)", fontWeight = FontWeight.SemiBold)
                    Text("AES-128-GCM 加密，每块额外 16B 认证标头", style = MaterialTheme.typography.bodySmall, color = Color.Gray)
                }
                Switch(
                    checked = config.encrypt,
                    onCheckedChange = { v -> onConfigChange { it.copy(encrypt = v) } }
                )
            }

            Spacer(modifier = Modifier.height(8.dp))

            // kid
            OutlinedTextField(
                value = config.kid.toString(),
                onValueChange = { v ->
                    v.toIntOrNull()?.let { k -> onConfigChange { it.copy(kid = k.coerceIn(0, 65535)) } }
                },
                label = { Text("密钥标识 (KID, 0..65535)") },
                modifier = Modifier.fillMaxWidth()
            )

            Spacer(modifier = Modifier.height(8.dp))

            // keyProviderMode
            var kpExpanded by remember { mutableStateOf(false) }
            ExposedDropdownMenuBox(
                expanded = kpExpanded,
                onExpandedChange = { kpExpanded = it }
            ) {
                OutlinedTextField(
                    value = config.keyProviderMode.label,
                    onValueChange = {},
                    readOnly = true,
                    label = { Text("KeyProvider 模式") },
                    trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = kpExpanded) },
                    modifier = Modifier.menuAnchor().fillMaxWidth()
                )
                ExposedDropdownMenu(
                    expanded = kpExpanded,
                    onDismissRequest = { kpExpanded = false }
                ) {
                    KeyProviderMode.values().forEach { mode ->
                        DropdownMenuItem(
                            text = { Text(mode.label) },
                            onClick = {
                                onConfigChange { it.copy(keyProviderMode = mode) }
                                kpExpanded = false
                            }
                        )
                    }
                }
            }

            if (config.keyProviderMode == KeyProviderMode.CUSTOM_HEX) {
                Spacer(modifier = Modifier.height(8.dp))
                OutlinedTextField(
                    value = config.customKeyHex,
                    onValueChange = { v -> onConfigChange { it.copy(customKeyHex = v) } },
                    label = { Text("自定义 32 字节密钥 (64 位 Hex 字符)") },
                    isError = config.customKeyHex.length != 64,
                    supportingText = {
                        Text(if (config.customKeyHex.length == 64) "长度合法 (64字符)" else "当前长度: ${config.customKeyHex.length}/64")
                    },
                    modifier = Modifier.fillMaxWidth()
                )
            }
        }

        Spacer(modifier = Modifier.height(8.dp))

        // 组 D: 输出
        CollapsibleGroupCard(
            title = "D. 输出与门控",
            subtitle = "echo=${config.consoleEcho}, AppGate=${config.minLevelGate}",
            isExpanded = expandedD,
            onToggle = { expandedD = !expandedD }
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("logcat 镜像直通 (consoleEcho)", fontWeight = FontWeight.SemiBold)
                    Text("成功记录实时镜像到 logcat (2KB截断)", style = MaterialTheme.typography.bodySmall, color = Color.Gray)
                }
                Switch(
                    checked = config.consoleEcho,
                    onCheckedChange = { v -> onConfigChange { it.copy(consoleEcho = v) } }
                )
            }

            Spacer(modifier = Modifier.height(8.dp))

            // Level gate
            var lvlExpanded by remember { mutableStateOf(false) }
            ExposedDropdownMenuBox(
                expanded = lvlExpanded,
                onExpandedChange = { lvlExpanded = it }
            ) {
                OutlinedTextField(
                    value = "${config.minLevelGate.name} (等级值: ${config.minLevelGate.v})",
                    onValueChange = {},
                    readOnly = true,
                    label = { Text("App 侧过滤门控 (库内无等级过滤)") },
                    trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = lvlExpanded) },
                    modifier = Modifier.menuAnchor().fillMaxWidth()
                )
                ExposedDropdownMenu(
                    expanded = lvlExpanded,
                    onDismissRequest = { lvlExpanded = false }
                ) {
                    LogX.Level.values().forEach { lvl ->
                        DropdownMenuItem(
                            text = { Text("${lvl.name} (${lvl.v})") },
                            onClick = {
                                onConfigChange { it.copy(minLevelGate = lvl) }
                                lvlExpanded = false
                            }
                        )
                    }
                }
            }
        }
    }
}

@Composable
fun CollapsibleGroupCard(
    title: String,
    subtitle: String,
    isExpanded: Boolean,
    onToggle: () -> Unit,
    content: @Composable () -> Unit
) {
    Card(
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f)),
        modifier = Modifier.fillMaxWidth()
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable { onToggle() },
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(title, fontWeight = FontWeight.Bold, fontSize = 15.sp)
                    Text(subtitle, style = MaterialTheme.typography.bodySmall, color = Color.Gray)
                }
                Text(if (isExpanded) "▲ 收起" else "▼ 展开", fontSize = 12.sp, color = MaterialTheme.colorScheme.primary)
            }
            if (isExpanded) {
                Spacer(modifier = Modifier.height(12.dp))
                content()
            }
        }
    }
}

