package com.logx.consumer

import android.app.Activity
import android.os.Bundle
import android.util.Log
import com.zzlfeng.logx.LogX
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.random.Random

private const val TAG = "LogXConsumerSmoke"

private val KEY_KID1 =
    "1111111122222222333333334444444455555555666666667777777788888888"
        .chunked(2).map { it.toInt(16).toByte() }.toByteArray()

class MainActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Thread {
            var rc = 1
            try {
                rc = runSmoke()
            } catch (t: Throwable) {
                Log.e(TAG, "CONSUMER_SMOKE_FAIL", t)
            } finally {
                Log.i(TAG, "CONSUMER_SMOKE_${if (rc == 0) "OK" else "FAIL"} rc=$rc")
                runOnUiThread { finish() }
            }
        }.start()
    }

    private fun runSmoke(): Int {
        val logDir = File(filesDir, "logs")
        val stagingDir = File(filesDir, "staging")
        val external = getExternalFilesDir(null) ?: return 2
        val artifacts = File(external, "artifacts").apply { deleteRecursively(); mkdirs() }
        val exportDir = File(external, "export").apply { deleteRecursively(); mkdirs() }

        LogX.init(applicationContext, LogX.Config(
            logDir = logDir,
            stagingDir = stagingDir,
            compress = LogX.Compress.ZSTD,
            encrypt = true,
            kid = 1,
            keyProvider = LogX.KeyProvider { kid -> if (kid == 1) KEY_KID1 else null },
            flushThreshold = 4096,
            stagingWaitMs = 1000,
            maxFileSize = 64 * 1024,
            maxLogLength = 64 * 1024,
            autoProcessIsolation = false,
            errorHandler = LogX.ErrorHandler { kind, code, msg ->
                Log.w(TAG, "CONSUMER_EVENT kind=$kind code=$code msg=$msg")
            },
        ))

        var writeFails = 0
        fun write(level: LogX.Level, tag: String, payload: ByteArray) {
            val r = LogX.log(level, tag, payload)
            if (r != LogX.ERR_OK) {
                writeFails++
                Log.w(TAG, "CONSUMER_WRITE_FAIL tag=$tag rc=$r")
            }
        }

        val rnd = Random(20261017)
        for (i in 0 until 20) {
            write(LogX.Level.I, "Main", "hello-%02d 中文消息 payload".format(i).toByteArray())
        }
        write(LogX.Level.W, "Big", ByteArray(40 * 1024) { rnd.nextInt(256).toByte() })
        for (i in 0 until 120) {
            val prefix = "rnd-%03d:".format(i).toByteArray()
            val body = ByteArray(1000) { rnd.nextInt(256).toByte() }
            write(LogX.Level.I, "Split", prefix + body)
            if (i % 10 == 9) LogX.flush()
        }

        val flushRc = LogX.flush()
        val statsBefore = LogX.getStats()
        Log.i(TAG, "CONSUMER_STATS flush=$flushRc writeFails=$writeFails " +
            "dropNoSpace=${statsBefore?.dropNoSpace} dropTooLarge=${statsBefore?.dropTooLarge} " +
            "blockWriteErrors=${statsBefore?.blockWriteErrors} truncated=${statsBefore?.truncatedLogs}")

        val today = SimpleDateFormat("yyyyMMdd", Locale.US).format(Date())
        var exportFiles = 0
        val exportRc = LogX.export(today, today, LogX.ExportCallback { date, path, size ->
            val dst = File(exportDir, "plain_$date.txt")
            File(path).copyTo(dst, overwrite = true)
            exportFiles++
            Log.i(TAG, "CONSUMER_EXPORT date=$date size=$size path=${dst.absolutePath}")
            0
        })

        val logFiles = logDir.listFiles()?.filter { it.isFile && it.name.endsWith(".log") || it.name.contains(".log.") } ?: emptyList()
        for (f in logFiles) {
            f.copyTo(File(artifacts, f.name), overwrite = true)
        }
        val result = buildString {
            appendLine("exportRc=$exportRc")
            appendLine("exportFiles=$exportFiles")
            appendLine("writeFails=$writeFails")
            appendLine("flushRc=$flushRc")
            appendLine("dropNoSpace=${statsBefore?.dropNoSpace}")
            appendLine("dropTooLarge=${statsBefore?.dropTooLarge}")
            appendLine("blockWriteErrors=${statsBefore?.blockWriteErrors}")
            appendLine("truncated=${statsBefore?.truncatedLogs}")
            appendLine("logFiles=${logFiles.map { it.name }.sorted()}")
        }
        File(external, "result.txt").writeText(result)
        Log.i(TAG, "CONSUMER_RESULT\n$result")

        LogX.close()
        return if (exportRc == LogX.ERR_OK && exportFiles > 0 && writeFails == 0) 0 else 3
    }
}
