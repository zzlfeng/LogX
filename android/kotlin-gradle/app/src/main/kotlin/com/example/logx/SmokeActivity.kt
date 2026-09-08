package com.example.logx

import android.app.Activity
import android.os.Bundle
import android.widget.LinearLayout
import android.widget.TextView
import java.io.File

/**
 * T5.4 consoleEcho 真机 smoke:加载 liblogx → init(consoleEcho=true) →
 * 写入中文/ASCII/各等级记录 → 屏幕显示每步结果,由 adb logcat 断言 echo 行。
 * 非业务示例代码,仅真机验证 JNI 全链路(含 liblog 链接与新 nativeInit 签名)。
 */
class SmokeActivity : Activity() {

    private val lines = mutableListOf<String>()

    private fun report(s: String) {
        lines.add(s)
        android.util.Log.i("SmokeTag", s)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val tv = TextView(this).apply { setPadding(32, 64, 32, 32) }
        root.addView(tv)
        setContentView(root)

        try {
            // 接入步骤 ①: native 库由 LogX 内部自动加载托管 (亦支持显式调用 LogX.loadLibrary())
            LogX.loadLibrary()
            report("LogX.loadLibrary() OK")

            val filesDir = getFilesDir()
            val logDir = File(filesDir, "logs")
            val stagingDir = File(filesDir, "logx_staging")
            logDir.mkdirs(); stagingDir.mkdirs()

            // 接入步骤 ②: init, consoleEcho=true (T5.4 开关)
            LogX.init(this, LogX.Config(
                logDir = logDir,
                stagingDir = stagingDir,
                compress = LogX.Compress.ZSTD,
                consoleEcho = true,
                errorHandler = { kind, code, msg -> report("onError kind=$kind code=$code msg=$msg") },
            ))
            report("init OK (compress=ZSTD, consoleEcho=true)")

            // 接入步骤 ③: 写入 —— 每条应同步镜像到 logcat (tag=SmokeTag, 保留业务 tag)
            LogX.i("SmokeTag", "echo 中文测试 line-1")
            LogX.w("SmokeTag", "echo ASCII warning line-2")
            LogX.e("SmokeTag", "echo 混合 mixed 中文/English line-3")
            LogX.d("SmokeTag", "echo debug level line-4")
            LogX.v("SmokeTag", "echo verbose level line-5")
            LogX.f("SmokeTag", "echo fatal level line-6")
            LogX.flush()
            report("6 records written + flush OK")

            report("SMOKE PASS")
        } catch (t: Throwable) {
            report("SMOKE FAIL: ${t.javaClass.simpleName}: ${t.message}")
        } finally {
            tv.text = lines.joinToString("\n")
        }
    }
}
