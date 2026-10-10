package com.logx.tester.model

import com.zzlfeng.logx.LogX

data class Preset(
    val id: Int,
    val name: String,
    val summary: String,
    val expectedOutcome: String,
    val configModifier: (TesterConfig) -> TesterConfig,
    val suggestedAction: String = ""
)

object PresetRegistry {
    val presets: List<Preset> = listOf(
        Preset(
            id = 1,
            name = "1. 基线配置",
            summary = "无压缩 / 无加密 / flushThreshold=8192",
            expectedOutcome = "全部写入 OK；落盘体积 ≈ payload + 34B块头",
            configModifier = {
                it.copy(
                    compress = LogX.Compress.NONE,
                    encrypt = false,
                    flushThreshold = 8192,
                    maxLogLength = 65536,
                    useCacheDir = false
                )
            }
        ),
        Preset(
            id = 2,
            name = "2. ZSTD 压缩",
            summary = "compress=ZSTD",
            expectedOutcome = "写入 OK；落盘体积明显小于 payload（约 1.5x 压缩比）",
            configModifier = {
                it.copy(
                    compress = LogX.Compress.ZSTD,
                    encrypt = false,
                    flushThreshold = 8192,
                    useCacheDir = false
                )
            }
        ),
        Preset(
            id = 3,
            name = "3. ZSTD + 加密",
            summary = "ZSTD + encrypt + 内置32B密钥 + kid=1",
            expectedOutcome = "写入 OK；每块额外 16B AES-GCM tag",
            configModifier = {
                it.copy(
                    compress = LogX.Compress.ZSTD,
                    encrypt = true,
                    kid = 1,
                    keyProviderMode = KeyProviderMode.BUILTIN_32B,
                    flushThreshold = 8192,
                    useCacheDir = false
                )
            }
        ),
        Preset(
            id = 4,
            name = "4. ZLIB 拒绝",
            summary = "compress=ZLIB（未实现）",
            expectedOutcome = "init 失败，nativeHandle=0 (logx_c.cpp:306-308)",
            configModifier = {
                it.copy(compress = LogX.Compress.ZLIB, useCacheDir = false)
            }
        ),
        Preset(
            id = 5,
            name = "5. 加密缺 keyProvider",
            summary = "encrypt=true + keyProvider=不提供(null)",
            expectedOutcome = "init 失败 (logx_c.cpp:320-322)",
            configModifier = {
                it.copy(
                    encrypt = true,
                    keyProviderMode = KeyProviderMode.NONE_NULL,
                    useCacheDir = false
                )
            }
        ),
        Preset(
            id = 6,
            name = "6. 密钥返回 null 降级",
            summary = "encrypt=true + keyProvider 返回 null",
            expectedOutcome = "init 成功，但落盘降级明文并产生 DEGRADED 事件 (flusher.cpp:96/680)",
            configModifier = {
                it.copy(
                    encrypt = true,
                    keyProviderMode = KeyProviderMode.RETURN_NULL,
                    flushThreshold = 8192,
                    useCacheDir = false
                )
            }
        ),
        Preset(
            id = 7,
            name = "7. maxLogLength 四档越界",
            summary = "校验分层：131072(Kotlin require) / 100000(C >64KB) / 96(C <97) / 0(C <97)",
            expectedOutcome = "四种越界分别被 Kotlin require 或 C 侧 INVALID_ARG 拦截，报错各不相同",
            configModifier = {
                it.copy(maxLogLength = 131072, useCacheDir = false)
            },
            suggestedAction = "在参数面板分别尝试调整 maxLogLength 为 131072、100000、96 和 0 并重启"
        ),
        Preset(
            id = 8,
            name = "8. 超长 payload",
            summary = "用户总长 > 60KB 触发截断保前缀(B-1B/T5.5)",
            expectedOutcome = "写入返回 OK, 原始前缀保留并追加截断 marker; getStats().truncatedLogs 计数 +1",
            configModifier = {
                it.copy(
                    compress = LogX.Compress.NONE,
                    maxLogLength = 65536,
                    useCacheDir = false
                )
            },
            suggestedAction = "在单条写入区将 payload 滑块拖至 70000B 并点击写入"
        ),
        Preset(
            id = 9,
            name = "9. 超 maxLogLength",
            summary = "maxLogLength=16384 + 单条写 30000B",
            expectedOutcome = "写入返回 OK(截断保前缀, \"log truncated (dropped N bytes)\")",
            configModifier = {
                it.copy(maxLogLength = 16384, useCacheDir = false)
            },
            suggestedAction = "应用后，在单条写入区写入 30000B payload"
        ),
        Preset(
            id = 10,
            name = "10. 20KB 分片",
            summary = "单条 25000B (>20KB 且 <60KB)",
            expectedOutcome = "内部拆为多片 LGXE 记录并成功落盘",
            configModifier = {
                it.copy(maxLogLength = 65536, useCacheDir = false)
            },
            suggestedAction = "应用后，在单条写入区写入 25000B payload"
        ),
        Preset(
            id = 11,
            name = "11. 纯手动 flush",
            summary = "flushThreshold=0 禁用阈值自动唤醒",
            expectedOutcome = "写入不按阈值自动落盘，仅靠 75ms debounce 兜底或手动 flush",
            configModifier = {
                it.copy(flushThreshold = 0, useCacheDir = false)
            },
            suggestedAction = "连续写入多条日志后观察落盘文件，随后点击 flush 按钮"
        ),
        Preset(
            id = 12,
            name = "12. staging 环满 (压测)",
            summary = "flushThreshold=0 + 50线程并发写 2048B",
            expectedOutcome = "130944B 环快速填满，产生大量 dropNoSpace 与 staging full DROP 事件",
            configModifier = {
                it.copy(flushThreshold = 0, useCacheDir = false)
            },
            suggestedAction = "在压测区设置 50 线程、payload 2048B、条数 50000 并开始压测"
        ),
        Preset(
            id = 13,
            name = "13. 文件切分",
            summary = "maxFileSize=65536 (64KB)",
            expectedOutcome = "写入约 500KB~1MB 后，落盘文件出现 YYYYMMDD.log、.1、.2 等分卷",
            configModifier = {
                it.copy(maxFileSize = 64 * 1024, flushThreshold = 8192, useCacheDir = false)
            },
            suggestedAction = "应用后，通过压测写入 1000 条 512B 日志，然后刷新落盘文件列表"
        ),
        Preset(
            id = 14,
            name = "14. staging 禁 cacheDir (负向测试)",
            summary = "useCacheDir=true 让 stagingDir 落在 cacheDir",
            expectedOutcome = "Kotlin require 抛出 IllegalArgumentException (\"staging dir must not be in cacheDir\")，UI 正常拦截不崩溃",
            configModifier = {
                it.copy(useCacheDir = true)
            }
        ),
        Preset(
            id = 15,
            name = "15. tag 越界",
            summary = "tag 长度 > 64B",
            expectedOutcome = "写入返回 INVALID (\"invalid log input\")",
            configModifier = {
                it.copy(useCacheDir = false)
            },
            suggestedAction = "在单条写入区输入 65 个字符以上的 tag 并尝试写入"
        )
    )
}

