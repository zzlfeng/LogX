# LogX 真机交互式调参测试 App — 实施计划

## 0. 目标与既定决策

新建独立 Compose 工程 `logx/android/logx-tester/`，在真机上通过 GUI 动态调整 LogX 库的全部可调参数，实时展示写入结果、事件流与统计指标。

| 决策项 | 结论 |
| --- | --- |
| UI 技术栈 | Jetpack Compose（Material3） |
| 工程位置 | 新建独立工程 `logx/android/logx-tester/`，**完全不动**现有 `android/kotlin-gradle/` |
| native 改动 | **零改动**。不改 C++/JNI，不重编 `liblogx.so` |
| 统计来源 | App 侧自行累加（ThreadLocal 哨兵 + ErrorHandler 同步事件） |
| 日志展示 | 仅写入回显 + 事件流；不实现 export hex→可读记录的解码器 |


---

## 1. 代码调研结论（已逐行核实，作为设计依据）

### 1.1 库的可调参数全集

来源 `android/kotlin/LogX.kt:76-97`（Config）与 `core/src/logx_c.cpp:286-322`（logx_open 校验）。
> 注：本计划所有 `LogX.kt` / `*.cpp` 行号均按 **2026-09-09 当前源码**核对；源码持续演进，若发现行号不匹配以符号/字符串（函数名、require 断言原文、事件消息原文）为准。

| 参数 | 类型 | 默认 | 校验/边界（决定控件量程与负向测试） |
| --- | --- | --- | --- |
| `logDir` | File | 必填 | 自动 mkdirs |
| `stagingDir` | File | 必填 | **路径含 `/cache/` 时 Kotlin `require` 抛异常**（LogX.kt:157-159） |
| `level` | Level V0..F5 | I | **库内无等级过滤**：`nativeInit` 参数表无 level，C 侧 `logx_config_t` 也无此字段；level 仅经 `nativeWrite` 写入每条记录头（`build_records` 校验 `>5→invalid`） |
| `retentionDays` | Int | 7 | JNI 层 `>0` 否则回落 7 |
| `maxFileSize` | Long | 10MB | JNI 层 `>0` 否则回落 10MB；另有 4GiB−16MB 物理硬上限 |
| `maxLogLength` | Int | 65536 | Kotlin `require(<130943)`；C 侧要求 `!=0` 且 `<=65536` 且 `<130944`，三条任一不满足 → init 失败 |
| `flushThreshold` | Int | 8192 | **三态**：`0`=禁用阈值自动唤醒；`-1`=取默认 8KB；其它=字节阈值 |
| `compress` | NONE0/ZSTD1/ZLIB2 | NONE | **ZLIB 被直接拒绝**（logx_c.cpp:306-308，未实现）→ init 失败 |
| `encrypt` | Boolean | false | **true 且 `keyProvider==null` → init 失败**（logx_c.cpp:320-322） |
| `kid` | Int(uint16) | 1 | 写入块头 KID |
| `keyProvider` | KeyProvider? | null | `getKey(kid)` 必须返回**正好 32 字节**，否则 JNI 视为 nullptr（logx_jni.cpp:160-164） |
| `autoProcessIsolation` | Boolean | true | 按进程名后缀重命名目录 |
| `consoleEcho` | Boolean | false | 每条**成功**记录镜像到 logcat（2KB 截断） |


预编译 so 编译开关：`LOGX_WITH_ZSTD=ON`、`LOGX_WITH_AESGCM=ON`；ABI 仅 `arm64-v8a`；`ANDROID_PLATFORM=android-24`；`ANDROID_STL=c++_shared`。

### 1.2 容量与边界常量（滑块量程依据）

- staging 总 128KB，双 Header 128B，**环形数据区 130944B**
- 单条上限 64KB；用户载荷（27B 内部头+tag+msg）上限 60KB；**单片 payload 上限 20KB，超过按 LGXE 分片**
- tag 硬上限 64B；常规块 raw ≤16KB，单条独占块 ≤20KB+6B
- Flusher debounce 75ms；`flush_blocking` 超时 5000ms → `LOGX_ERR_TIMEOUT(-10)`
- 错误码：0 OK / −1 INVALID_ARG / −2 STATE / −3 IO / −4 NO_SPACE / −5 TOO_LARGE / −6 CODEC / −8 NOT_FOUND / −10 TIMEOUT

### 1.3 生命周期硬约束（决定整个 App 架构）

`LogX` 是 **`object` 进程内单例**：

- `init()` 第二次调用 **抛 `IllegalStateException`**（LogX.kt:129-132）
- 失败路径回滚 `initCount=0`，允许改参数后重试（LogX.kt:180-184）
- `close()` 置 `nativeHandle=0`、`initCount=0`、`errorHandler=null`、`isClosed=true`

→ **「改参数生效」的唯一路径是 `close()` → `init()`**。且 `close()` 后 `setErrorHandler` 因 `isClosed` 直接 return 无效（LogX.kt:213-218），而 `init()` 开头会 `isClosed=false` 并从 `Config.errorHandler` 重新取回调 —— **因此每次 init 必须在 Config 里带上 errorHandler**。

### 1.4 写入结果的捕获机制（本方案的技术核心）

`logx_write`（logx_c.cpp:411-458）**全程在调用方线程同步执行**，失败时同步调 `emit_event`：

| 失败条件 | 事件消息（原文） | 返回码 |
| --- | --- | --- |
| level>5 / tag 空或>64B / 用户总长>60KB | `"invalid log input"` | INVALID_ARG |
| `total > max_log_length` | `"log too large"` | TOO_LARGE |
| staging 环空闲不足 | `"staging full"` | NO_SPACE |


而 `emit_event` → `jni_error_trampoline` → `CallStaticVoidMethod(LogX.onNativeError)` → `errorHandler.onError` **也在同一条线程上同步完成**（logx_internal.h:86-94 / logx_jni.cpp:93-128）。

→ **在 `LogX.log()` 调用前后用 ThreadLocal 哨兵包一层，即可精确还原每一条写入的结果**，无需任何 native 改动。

### 1.5 App 侧拿不到的三项 native 计数（须在 UI 明确标注 N/A）

| 计数 | 状态 | 原因 |
| --- | --- | --- |
| `truncated_blocks` | **拿不到** | 仅 recovery.cpp:251,257 递增；虽有 RECOVERY 事件伴随，但事件不含条数 |
| `pad_records` | **恒为 0** | 全 core **无任何递增点**（仅 logx_c.cpp:494 读取）→ 该指标当前未接线 |
| `block_write_errors` | **只能间接感知** | flusher.cpp:101/140/160 递增；仅 :140 伴随 `"block short write"` IO 事件，:101/:160 无事件 → 不保证一一对应 |


---

## 2. 工具链（镜像本机已验证工程 `D:\Project\Android_Project\ViewTest`）

`ViewTest` 是本机**已成功产出 debug APK** 的 Compose 工程（`app/build/intermediates/app_metadata/debug/app-metadata.properties` 记录 `androidGradlePluginVersion=9.2.1`），直接复用其配置矩阵，风险最低。

| 组件 | 版本 | 位置/依据 |
| --- | --- | --- |
| AGP | 9.2.1 | 缓存已有；ViewTest 实证 |
| Gradle | 9.4.1 | `~/.gradle/wrapper/dists/gradle-9.4.1-bin`（已解压 `.ok`） |
| Kotlin | 2.2.10 | KGP + compose-compiler-gradle-plugin 均 2.2.10 |
| compose-bom | 2026.02.01 | **不在本地缓存**，需联网（已实测 Google Maven 返回 200） |
| compileSdk | release(37) minorApiLevel 1 | `android-37.1` 平台已装 |
| minSdk / targetSdk | **24** / 37 | 24 是硬底线：so 以 android-24 编译 |
| JDK | Android Studio JBR **25.0.3** | `C:\ProgramFiles\Android\Android Studio\jbr`；⚠️ PATH 上的 java 是 **JDK 11**，`JAVA_HOME` 为空，必须显式指定 |
| NDK | 不需要 | 本方案不编译 C++ |


**AGP 9 关键差异**（已由 ViewTest 实证）：

- 仅需 2 个插件：`com.android.application` + `org.jetbrains.kotlin.plugin.compose`。**AGP 9 内置 Kotlin 支持**，无需 `org.jetbrains.kotlin.android`（ViewTest 有 9 个 `.kt` 文件且未声明该插件）
- `compileSdk` 改为块语法：`compileSdk { version = release(37) { minorApiLevel = 1 } }`
- `buildTypes.release` 用 `optimization { enable = false }`（替代 `isMinifyEnabled`）

---

## 3. 工程骨架

```
logx/android/logx-tester/
├── settings.gradle.kts                     # 镜像 ViewTest；rootProject.name = "logx-tester"
├── build.gradle.kts                        # root：仅 alias 两个插件 apply false
├── gradle.properties                       # jvmargs + configuration-cache=true（不要开 offline）
├── local.properties                        # sdk.dir=D:\DevEnv\Android\SDK（gitignore）
├── gradlew / gradlew.bat                   # 从 ViewTest 复制
├── gradle/
│   ├── libs.versions.toml                  # 版本目录（见 §4.1）
│   └── wrapper/{gradle-wrapper.jar, gradle-wrapper.properties}   # 从 ViewTest 复制
└── app/
    ├── build.gradle.kts                    # 见 §4.2
    └── src/main/
        ├── AndroidManifest.xml
        ├── jniLibs/arm64-v8a/
        │   ├── liblogx.so                  # 从 android/kotlin-gradle/app/src/main/jniLibs 复制
        │   └── libc++_shared.so            # 同上（必须一起打包）
        ├── res/values/{strings.xml, themes.xml}
        └── java/com/logx/tester/
            ├── MainActivity.kt
            ├── TesterViewModel.kt
            ├── engine/
            │   ├── LogXProbe.kt            # ★ ThreadLocal 哨兵，写入结果捕获
            │   ├── EngineController.kt     # ★ LogX 单例生命周期（close→init）
            │   ├── StressRunner.kt         # 并发压测执行器 + 延迟分位数
            │   └── TestKeys.kt             # 内置 32B 测试密钥 + hex 解析
            ├── model/
            │   ├── TesterConfig.kt         # UI 参数状态（→ LogX.Config 映射）
            │   ├── Metrics.kt              # 计数与延迟快照
            │   ├── ConsoleLine.kt          # 控制台行（immutable）
            │   └── Preset.kt               # 预置场景定义
            └── ui/
                ├── TesterScreen.kt         # 总布局（4 分区 + 压测面板）
                ├── ConfigPanel.kt          # 参数面板（4 折叠组）
                ├── ActionPanel.kt          # 动作区
                ├── MetricsPanel.kt         # 指标区
                ├── StressPanel.kt          # 压测面板
                ├── LogConsole.kt           # 实时日志（有界环形 + 批量刷新）
                └── theme/Theme.kt
```

**LogX.kt 单源引入**（不拷贝，避免源码漂移）：

```
sourceSets {
    getByName("main") {
        java.srcDir(rootDir.parentFile.resolve("kotlin"))   // → logx/android/kotlin
    }
}
```

⚠️ **`LogX` 原样引入，包名必须保持 `com.zzlfeng.logx`（不是 `com.example.logx`！）**：真实源码 `LogX.kt:1` 为 `package com.zzlfeng.logx`，JNI 符号是 `Java_com_zzlfeng_logx_LogX_nativeInit`（`logx_jni.cpp:204`），且 `logx_jni.cpp:229` 硬编码 `FindClass("com/zzlfeng/logx/LogX")`。源码原样引入即满足符号绑定；若误把 LogX 塞进 `com.example.logx`，JVM 会解析不存在的 `Java_com_example_logx_LogX_nativeInit` → 运行时 `UnsatisfiedLinkError`。App 自身 `namespace` 用 `com.logx.tester`（与库包名不同，无冲突），`applicationId` 任意。

---

## 4. 关键构建配置

### 4.1 `gradle/libs.versions.toml`

```
[versions]
agp = "9.2.1"
kotlin = "2.2.10"
composeBom = "2026.02.01"
activityCompose = "1.13.0"
lifecycle = "2.11.0"
coreKtx = "1.19.0"
coroutines = "1.8.0"

[libraries]
androidx-core-ktx            = { group = "androidx.core", name = "core-ktx", version.ref = "coreKtx" }
androidx-lifecycle-runtime-ktx = { group = "androidx.lifecycle", name = "lifecycle-runtime-ktx", version.ref = "lifecycle" }
androidx-lifecycle-viewmodel-compose = { group = "androidx.lifecycle", name = "lifecycle-viewmodel-compose", version.ref = "lifecycle" }
androidx-activity-compose    = { group = "androidx.activity", name = "activity-compose", version.ref = "activityCompose" }
androidx-compose-bom         = { group = "androidx.compose", name = "compose-bom", version.ref = "composeBom" }
androidx-compose-ui          = { group = "androidx.compose.ui", name = "ui" }
androidx-compose-ui-graphics = { group = "androidx.compose.ui", name = "ui-graphics" }
androidx-compose-ui-tooling  = { group = "androidx.compose.ui", name = "ui-tooling" }
androidx-compose-ui-tooling-preview = { group = "androidx.compose.ui", name = "ui-tooling-preview" }
androidx-compose-material3   = { group = "androidx.compose.material3", name = "material3" }
kotlinx-coroutines-android   = { group = "org.jetbrains.kotlinx", name = "kotlinx-coroutines-android", version.ref = "coroutines" }

[plugins]
android-application = { id = "com.android.application", version.ref = "agp" }
kotlin-compose      = { id = "org.jetbrains.kotlin.plugin.compose", version.ref = "kotlin" }
```

> `kotlinx-coroutines-android` 是必须的：`LogX.kt` 用到 `Dispatchers`/`withContext`/`runBlocking`（LogX.kt:10-13）。

### 4.2 `app/build.gradle.kts` 要点

```
plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}
android {
    namespace = "com.logx.tester"
    compileSdk { version = release(37) { minorApiLevel = 1 } }
    defaultConfig {
        applicationId = "com.logx.tester"
        minSdk = 24                 // 硬底线：liblogx.so 以 android-24 编译
        targetSdk = 37
        versionCode = 1; versionName = "1.0"
        ndk { abiFilters += "arm64-v8a" }   // 只有 arm64 预编译 so
    }
    buildTypes { release { optimization { enable = false } } }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures { compose = true }
    sourceSets { getByName("main") { java.srcDir(rootDir.parentFile.resolve("kotlin")) } }
}
```

### 4.3 `AndroidManifest.xml`

- 不申请任何权限（全部写 `filesDir`，无需存储权限）
- `android:allowBackup="false"`（与 spec §10 一致，避免明文 staging 进云备份）
- 单 Activity `.MainActivity`，`exported="true"` + LAUNCHER，`windowSoftInputMode="adjustResize"`（大量输入框）
- 主题：`themes.xml` 用 `parent="android:Theme.Material.Light.NoActionBar"`（与 ViewTest 一致，不引入 appcompat）

---

## 5. 核心实现设计

### 5.1 `LogXProbe.kt` — 写入结果捕获（技术核心）

```
object LogXProbe {
    class Capture { var hit = false; var code = 0; var msg = "" }
    private val slot = ThreadLocal<Capture?>()

    sealed interface R { val nanos: Long
        data class Ok(override val nanos: Long) : R
        data class NoSpace(override val nanos: Long) : R
        data class TooLarge(override val nanos: Long) : R
        data class Invalid(override val nanos: Long) : R
        data class OtherDrop(override val nanos: Long, val msg: String) : R
    }

    // 注册进每次 LogX.Config(errorHandler = LogXProbe::onEvent)
    fun onEvent(kind: LogX.EventKind, code: Int, msg: String) {
        if (kind == LogX.EventKind.DROP) {
            slot.get()?.let { it.hit = true; it.code = code; it.msg = msg }
        }
        eventSink.tryEmit(EventRow(kind, code, msg))   // 全局事件流，始终记录
    }

    fun write(level: LogX.Level, tag: String, payload: ByteArray): R {
        val c = Capture()
        slot.set(c)
        val t0 = System.nanoTime()
        try { LogX.log(level, tag, payload) } finally { slot.set(null) }
        val dt = System.nanoTime() - t0
        return when {
            !c.hit                            -> R.Ok(dt)
            c.msg.contains("staging full")     -> R.NoSpace(dt)
            c.msg.contains("log too large")    -> R.TooLarge(dt)
            c.msg.contains("invalid log input")-> R.Invalid(dt)
            else                               -> R.OtherDrop(dt, c.msg)
        }
    }
}
```

**正确性论证**（三条，缺一不可）：

1. `logx_write` 与其内部 `emit_event` 在**同一调用线程同步完成**（logx_c.cpp:411-458 + logx_internal.h:86-94），所以 DROP 事件必然落在 `slot.set(c)` 与 `slot.set(null)` 之间。
2. `ThreadLocal` 保证 50 线程并发压测下各线程哨兵互不干扰。
3. 来自 **Flusher 后台线程**的 DEGRADED/IO 事件（flusher.cpp 多处）在该线程 `slot` 为 null → 只进全局事件流，**不会污染写入判定**。

判定字符串取自源码原文：`logx_c.cpp:425 / :430 / :452`。

### 5.2 `EngineController.kt` — 单例生命周期

状态机：`Stopped → Starting → Running → Stopping → Stopped`

```
private val gate = Mutex()          // ★ 串行化 apply/stop，杜绝 close→init 竞态污染单例

suspend fun applyAndRestart(ctx: Context, cfg: TesterConfig): StartResult = gate.withLock {
    if (state == Running) { LogX.close(); state = Stopped }   // 参数生效的唯一路径
    return try {
        LogX.init(ctx, cfg.toLogXConfig(errorHandler = LogXProbe::onEvent))
        state = Running; StartResult.Ok
    } catch (t: Throwable) {                                   // 全部落到 UI，绝不崩溃
        state = Stopped                                        // LogX 已自行回滚 initCount
        StartResult.Failed(t::class.simpleName ?: "?", t.message ?: "")
    }
}

suspend fun stop() = gate.withLock {
    if (state == Running) { LogX.close(); state = Stopped }
}
```

> **为什么需要 `Mutex`**：`LogX.init` 是 `@Synchronized` 单例操作，但"close→init"两步之间无原子性；若两次快速点击让两个协程并行走 `applyAndRestart`，可能一个协程 close 后另一个已 init 成功，而前一个协程又发起第二次 init → 触发 `"LogX.init called more than once"`。以 `Mutex` 把 apply/stop 全部串行化即可消除该竞态（状态字段仍在主线程读写，`withLock` 内同步改状态亦安全）。

必须捕获的异常类型：

- `IllegalStateException("LogX native init failed")` ← nativeHandle==0（ZLIB / encrypt 无 keyProvider / maxLogLength 越界等 C 侧 INVALID_ARG）
- `IllegalArgumentException` ← Kotlin `require` 失败（cacheDir 路径、maxLogLength ≥ 130943）
- `IllegalStateException("LogX.init called more than once")` ← 状态机被绕过时的兜底
- `UnsatisfiedLinkError` ← so 缺失/ABI 不匹配

生命周期挂载点：状态放 `TesterViewModel`（跨配置变更存活），`onCleared()` 中 `LogX.close()`。**不要**在 `Activity.onDestroy` 里 close（旋转会误关）。

### 5.3 参数映射 `TesterConfig → LogX.Config`

- `logDir = File(filesDir, logDirName)`；`stagingDir = File(if (useCacheDir) cacheDir else filesDir, stagingDirName)`
—— `useCacheDir` 开关专门用于触发 `require` 失败的负向测试
- `flushThreshold` 直传三态原值（−1/0/N），UI 上标注语义
- `keyProvider` 由下拉模式生成：`内置固定32B` / `自定义hex(64字符)` / `返回null` / `返回错误长度(16B)` / **`不提供(null)`**
⚠️ 区分两个易混场景：**「不提供」→ init 直接失败**；**「返回null」→ init 成功**，但落盘时降级明文并触发 `DEGRADED "missing key; downgrade to plaintext"`（flusher.cpp:96/680）
- `level` **不传给 `nativeInit`（Config 里无对应 native 参数），且库内无任何等级过滤**——level 仅经 `nativeWrite` 写入每条记录头并在 `build_records` 校验 `>5→invalid`。因此改为 App 侧 gate：`if (level.v < minLevelGate.v) return`，UI 明确标注"App 侧过滤，库内无此能力"（表述上不应称其为"死参数"，以免误以为 level 完全无作用）

### 5.4 `StressRunner.kt`

- 执行器：`Dispatchers.Default.limitedParallelism(threads)` + `List<Deferred>`，`SupervisorJob` 可取消
- 延迟采样：**预分配 `LongArray(total)`**，每线程写入独立分片区间，避免运行期分配；结束后合并排序算 P50/P90/P99/P99.9/max
- 对齐 bench 方法论护栏：结束后若 `dropNoSpace+dropTooLarge+invalidInput > 0`，UI 顶部显示醒目**"数据失真：本轮有丢弃，吞吐/延迟结论无效"**（对应 logx_bench.cpp 的护栏）
- 可调项：线程数 1..50（滑块）、总条数 100..500000（对数档位）、payload 16..20480B（滑块，覆盖 20KB 分片边界）、写入间隔 0..10ms、周期 flush 开关 + 每 N 条
- **压测期间默认不逐条入控制台**（只每 N 条采样一行 + 聚合统计），另设"记录每条"开关默认 off — 否则 UI 必被淹没
- ★ **压测期间强制 `consoleEcho=false`**（与"记录每条"开关解耦，独立强制）：`consoleEcho` 开启时每条成功记录都镜像到 logcat（2KB 截断），50 万条压测会把 logcat 刷爆。压测启动时若无业务必要性，统一以 `consoleEcho=false` 的配置快照执行；如需看 echo 请用单条写入或在低条数下测。

### 5.5 实时日志控制台性能策略

- 有界环形缓冲，上限 2000 行（可调 500/2000/10000）
- 生产端 `MutableSharedFlow(extraBufferCapacity=4096, onBufferOverflow=DROP_OLDEST)`；ViewModel 侧按 **100ms 窗口批量合并**后一次性替换列表引用 → 避免每条 recompose
- `LazyColumn` + 稳定 `key`，行是 immutable `data class ConsoleLine(seq, tsMs, level, tag, text, kind)`
- 按等级/事件类型着色；自动滚动开关；清空按钮

---

## 6. UI 布局（四分区 + 压测面板）

**TopAppBar**：标题 + 实例状态徽章（Stopped/Running）+ 当前配置摘要（如 `ZSTD·加密·8KB·64KB`）

**① 参数面板**（4 个可折叠组）

| 组 | 参数 → 控件 | 量程/档位 |
| --- | --- | --- |
| A 路径与隔离 | logDir 名 → 输入框（默认 `logs`）；stagingDir 名 → 输入框（默认 `logx_staging`）；autoProcessIsolation → 开关；**staging 放 cacheDir** → 开关（负向测试） | — |
| B 容量与节奏 | maxLogLength → 滑块 **0..131072**（故意含越界区，实时提示合法区间 1..65536）；flushThreshold → 滑块 **−1..65536**（−1/0 两档特殊标注）；maxFileSize → 档位滑块 4KB/16KB/64KB/256KB/1MB/10MB/64MB；retentionDays → 滑块 0..30 | 见左 |
| C 编解码 | compress → 下拉 NONE/ZSTD/**ZLIB（标注：预期 init 失败）**；encrypt → 开关；kid → 输入框 0..65535；keyProvider 模式 → 下拉 5 选项；自定义密钥 → hex 输入框（64 字符校验） | — |
| D 输出 | consoleEcho → 开关；App 侧等级 gate → 下拉 V..F（标注"库内无过滤"） | — |


**② 动作区**

- `应用配置并重启实例` / `停止实例`
- 单条写入：等级下拉 + tag 输入（含 >64B 越界测试）+ msg 输入 + payload 字节数滑块 **0..70000**（覆盖 20KB 分片与 60KB 上限）→ 结果即时回显 `OK / NO_SPACE / TOO_LARGE / INVALID` + 耗时
- `flush`（显示阻塞耗时 ms，超时 5s 会返回 TIMEOUT）
- `export`（from/to 日期输入 → 显示返回码含义与产物字节数）
- `刷新落盘文件列表` → 展示 `YYYYMMDD.log(.N)` 与字节数（用于验证压缩比与文件切分，不做解码）

**③ 指标区**

- 四张计数卡：writeOk / dropNoSpace / dropTooLarge / invalidInput
- 延迟：P50 / P99 / max（µs）
- 事件计数：DEGRADED / DROP / IO / RECOVERY
- N/A 说明条：`truncated_blocks`、`pad_records`（库内恒 0）、`block_write_errors` 三项标灰并注明原因

**④ 实时日志控制台**（占屏幕下半部，可全屏展开）

**⑤ 压测面板**（独立折叠区）：参数 + 开始/停止 + 进度条 + 实时吞吐 logs/s + 结束后分位数报告 + 数据失真警示

---

## 7. 预置测试场景（一键载入，每个含"预期观测结果"）

| # | 场景 | 参数组合 | 预期结果（依据） |
| --- | --- | --- | --- |
| 1 | 基线 | NONE / 无加密 / flushThreshold=8192 | 全部 OK；落盘体积 ≈ payload + 34B块头 |
| 2 | ZSTD 压缩 | compress=ZSTD | OK；落盘体积明显小于 payload（bench 实测约 1.52x 压缩比） |
| 3 | ZSTD + 加密 | ZSTD + encrypt + 内置32B密钥 + kid=1 | OK；每块额外 16B GCM tag |
| 4 | **ZLIB 拒绝** | compress=ZLIB | **init 失败**，nativeHandle=0（logx_c.cpp:306-308 未实现） |
| 5 | **加密缺 keyProvider** | encrypt=true + keyProvider=不提供 | **init 失败**（logx_c.cpp:320-322） |
| 6 | 密钥返回 null 降级 | encrypt=true + keyProvider=返回null | **init 成功**，但落盘降级明文 + `DEGRADED "missing key; downgrade to plaintext"`（flusher.cpp:96/680） |
| 7 | **maxLogLength 三档越界** | 131072 → Kotlin require 失败；100000 → C 侧 INVALID_ARG；0 → INVALID_ARG | 三种失败信息各不相同，用于验证校验分层 |
| 8 | 超长 payload | payload=70000B | `INVALID`（用户总长>60KB → `"invalid log input"`） |
| 9 | 超 maxLogLength | maxLogLength=16384 + payload=30000B | `TOO_LARGE`（`"log too large"`） |
| 10 | **20KB 分片** | payload=25000B（>20KB 且 <60KB） | OK；内部拆多片 LGXE（首01/中10/末11），落盘正常 |
| 11 | 纯手动 flush | flushThreshold=0 + 连续写入 | 不按阈值唤醒 Flusher，仅靠 75ms debounce 兜底；手动 flush 才落盘 |
| 12 | **staging 环满** | flushThreshold=0 + 50线程 + payload=2048B 连续写 | 130944B 环快速填满 → `dropNoSpace` 持续上升 + DROP 事件流 |
| 13 | 文件切分 | maxFileSize=65536 + 写入约 1MB | 文件列表出现 `YYYYMMDD.log` / `.1` / `.2` … |
| 14 | staging 禁 cacheDir | 开启"staging 放 cacheDir" | Kotlin `require` 抛 IllegalArgumentException，UI 展示不崩溃 |
| 15 | tag 越界 | tag 长度 > 64B | `INVALID`（`"invalid log input"`） |


---

## 8. 构建与验证

```
# 1) 引导 wrapper（本机已有 9.4.1 dist，直接复制 ViewTest 的 wrapper）
cp /d/Project/Android_Project/ViewTest/gradlew     logx/android/logx-tester/
cp /d/Project/Android_Project/ViewTest/gradlew.bat logx/android/logx-tester/
cp -r /d/Project/Android_Project/ViewTest/gradle/wrapper logx/android/logx-tester/gradle/

# 2) 复制预编译 so（.gitignore 会忽略 *.so，属预期）
cp logx/android/kotlin-gradle/app/src/main/jniLibs/arm64-v8a/liblogx.so \
   logx/android/kotlin-gradle/app/src/main/jniLibs/arm64-v8a/libc++_shared.so \
   logx/android/logx-tester/app/src/main/jniLibs/arm64-v8a/

# 3) 构建（必须显式指定 JDK：PATH 上是 JDK 11，AGP 9 不接受）
cd logx/android/logx-tester
JAVA_HOME="/c/ProgramFiles/Android/Android Studio/jbr" ./gradlew :app:assembleDebug --no-daemon
```

**验证判据**：`BUILD SUCCESSFUL` + `app/build/outputs/apk/debug/app-debug.apk` 存在。

**当前 `adb devices` 为空** → 本次交付止于「构建通过」。真机验证步骤（待接设备后执行）：

```
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb logcat -s LogXTester:V SmokeTag:V   # consoleEcho 开启时可见镜像行
```

---

## 9. 风险清单（按可能性排序）

| # | 风险 | 缓解 |
| --- | --- | --- |
| 1 | compose-bom 2026.02.01 不在本地缓存，需联网 | 已实测 Google Maven 200 可达；**不要**设 `org.gradle.offline=true`。离线兜底：弃用 BOM 改显式版本 —— ui/foundation `1.10.4`、material3 `1.4.0`、runtime `1.11.0`、activity-compose `1.13.0`、lifecycle `2.11.0`（均已在缓存） |
| 2 | **★★★★ `java.srcDir(...kotlin)` 引入 Kotlin 文件未被 ViewTest 实证（全案唯一未验证的构建环节，成败关键）**。「AGP 9 无 kotlin-android 插件也能编译 .kt」已由 ViewTest 证实，但那是另一回事——不能证明 java 源集里新增的 `LogX.kt` 会被 AGP9 内置 Kotlin 编译到。 | 首次 `:app:assembleDebug` 即验证，失败则切换到兜底方案：**把 `LogX.kt` 复制进本工程自身源集**（如 `app/src/main/java/com/zzlfeng/logx/LogX.kt`，包名保持在 `com.zzlfeng.logx`），牺牲单源换取确定性；或在 `sourceSets` 用 Kotlin DSL 显式给 main 的 kotlin 源集追加 `srcDir`。是否复制的取舍在 README 注明 |
| 3 | JDK 选错（PATH 是 11，JAVA_HOME 空） | 每条 gradle 命令显式带 `JAVA_HOME=.../Android Studio/jbr`（JBR 25.0.3）；备选 `/d/DevEnv/Java/jdk-17.0.4.1` |
| 4 | jniLibs 的 so 被 gitignore，克隆后缺失 | 在 `app/build.gradle.kts` 加 `doFirst` 存在性检查，缺失时报明确错误并提示复制来源路径 |
| 5 | LogX 单例被重复 init 崩溃 | 所有 init/close 唯一经 `EngineController` 状态机，**并用 `Mutex` 串行化 apply/stop**（防两次快速点击触发并发 close→init 竞态）；状态存 ViewModel（跨配置变更），`onCleared` 才 close |
| 6 | `require`/`IllegalStateException` 直接崩溃 App | `applyAndRestart` 全量 try/catch，失败信息落 UI 控制台 |
| 7 | 压测跑在主线程导致 ANR | 强制 `Dispatchers.Default.limitedParallelism`；UI 仅接收聚合快照 |
| 8 | 高频写入把控制台刷爆导致掉帧 / 压测刷爆 logcat | 有界环形缓冲 + 100ms 批量合并 + 压测期默认不逐条记录；**压测时强制 `consoleEcho=false`**（见 §5.4） |
| 9 | 仅 arm64-v8a，x86_64 模拟器无法运行 | `abiFilters` 显式限定并在 README 注明必须 arm64 真机 |
| 10 | release 构建混淆破坏 JNI 符号（`onNativeError` 被 native 反射调用、`external fun`） | 只交付 debug；如需 release 补 `-keep class com.zzlfeng.logx.LogX { *; }` |
| 11 | `configuration-cache=true` 与某些插件冲突 | ViewTest 已开启且成功；若报错则关闭该项 |


---

## 10. 交付物清单

1. `logx/android/logx-tester/` 完整可构建 Compose 工程（约 18 个源文件 + 8 个构建/资源文件）
2. `app-debug.apk`（arm64-v8a）
3. `logx/android/logx-tester/README.md`：参数含义速查表、15 个预置场景的预期结果、构建与真机安装命令、三项 native 计数为何 N/A 的说明

**不改动**：`logx/core/`、`logx/decoder/`、`logx/android/cpp/`、`logx/android/kotlin/LogX.kt`、`logx/android/kotlin-gradle/`、`logx/CMakeLists.txt`