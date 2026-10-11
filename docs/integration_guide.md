# LogX Android 接入指南

> 对应计划表任务 T5.3；离线解码与线上排障见 [`docs/ops_troubleshooting.md`](./ops_troubleshooting.md)。
> 本文所有示例与 `android/kotlin/LogX.kt` 当前公开 API 逐一核对过（B-13 后基线，2026-10-11）。

## 1. 总览与能力边界

LogX 是 mmap 崩溃安全的 Android 日志库。一条日志的生命周期：

```
LogX.i(...)                      业务线程，微秒级
   │  写入 mmap staging 环形缓冲区（崩溃时数据已在页缓存，掉电/崩溃不依赖 write 系统调用）
   ▼
Flusher 后台线程                  满足任一条件即 drain：
   │   · staging 待刷量 ≥ flushThreshold（默认 8KB）
   │   · 75ms debounce 聚合窗口到期（低频写入不至于悬空）
   │   · 手动 LogX.flush()
   ▼
正式日志目录                       按日分卷 YYYYMMDD.log（单文件默认 10MB，超出切 .1/.2…）
   │   retentionDays 过期分卷在启动对账期与 Flusher 轮次自动清理
   ▼
LogX.export(...)                  按日期区间解密/解压为明文文本，回调逐分卷交付
```

核心保证与边界：

- **崩溃安全**：写入先进 mmap staging，进程被杀/崩溃后由下次启动的恢复对账（recovery）把残留块对齐进正式日志。`stagingWaitMs = 0`（默认）时环满**立即丢弃**并回报 NO_SPACE 事件——绝不阻塞业务线程；`stagingWaitMs > 0` 时在锁外有界退避等待 Flusher 腾空间。
- **平台要求**：minSdk 21（Android 5.0），NDK 26.1 交叉编译，`arm64-v8a`（其余 ABI 按同套 CMake 自行扩展）。
- **平台壳只做转发**：C ABI（`core/include/logx_c.h`）是稳定边界，iOS/Flutter/Windows 未来走同一套接口。
- **不是**：结构化/搜索型日志系统。明文行格式固定为 `日期 时间戳 pid tid [级别] tag: 消息`，检索靠离线解码后 grep。

## 2. 五分钟接入

### 2.1 集成物清单

当前以源码形态集成（Release AAR 为后续发布动作，尚未产出）：

| 部件 | 路径 | 说明 |
|---|---|---|
| core C++ 源码 | `core/src/` + `core/include/` | 经 CMake 构建为 `liblogx.so`（或直接取 `build-android/` 产物） |
| JNI 桥 | `android/cpp/logx_jni.cpp` | Kotlin ↔ C ABI 转发 + KeyProvider/回调桥接 |
| Kotlin API | `android/kotlin/LogX.kt` | 单例 `object LogX`，唯一需要的 Kotlin 文件 |
| 备份排除规则 | `android/res/xml/data_extraction_rules.xml`（API 31+）与 `backup_rules.xml`（API 23~30） | 必须随包携带，见 §8 |
| Manifest 片段 | `android/AndroidManifest-snippet.xml` | allowBackup 与规则注册说明 |

### 2.2 初始化与第一条日志

```kotlin
// App.kt
class App : Application() {
    override fun onCreate() {
        super.onCreate()
        LogX.init(
            this,
            LogX.Config(
                logDir = File(filesDir, "logs"),           // 正式密文日志目录
                stagingDir = File(filesDir, "logx_staging"), // mmap staging（必须 filesDir 系，禁 cacheDir）
            ),
        )
        LogX.i("App", "started, version=${BuildConfig.VERSION_NAME}")
    }
}
```

```kotlin
// 任意业务代码：六个级别直用
LogX.v("Net", "verbose detail")
LogX.d("Net", "request sent")
LogX.i ("Pay", "order created")
LogX.w("Pay", "retry count=3")
LogX.e("Pay", "gateway timeout")
LogX.f("Pay", "fatal state")
```

`init()` 会自动加载 `liblogx.so`（自定义 so 名时先调 `LogX.loadLibrary("你的名字")`）。初始化失败会抛异常，见 §4。

## 3. Config 全参数参考

```kotlin
data class Config(
    val logDir: File,                  // 必填：正式日志目录
    val stagingDir: File,              // 必填：staging 目录（强制 filesDir 系）
    val level: Level = Level.I,        // 最低写入级别；setMinLevel() 可运行期调整
    val retentionDays: Int = 7,        // 分卷保留天数，过期自动清理
    val maxFileSize: Long = 10MB,      // 单分卷上限，超出切 .1/.2…
    val maxLogLength: Int = 64KB,      // 单条上限（格式化后），见下方「易错点 1」
    val flushThreshold: Int = 8KB,     // 自动唤醒阈值，见下方「易错点 2」
    val compress: Compress = NONE,     // NONE / ZSTD / ZLIB，块级压缩
    val encrypt: Boolean = false,      // 块级 AES-256-GCM，开启必须给 keyProvider
    val kid: Int = 1,                  // 当前写入密钥 ID，轮换见 §5.3
    val keyProvider: KeyProvider? = null,
    val errorHandler: ErrorHandler? = null,
    val autoProcessIsolation: Boolean = true, // 多进程目录隔离，见 §8.2
    val consoleEcho: Boolean = false,  // logcat 直通，见下方「易错点 3」
    val stagingWaitMs: Int = 0,        // 环满阻塞上限（毫秒），0 = 绝不阻塞直接丢弃
)
```

**易错点 1：`maxLogLength` 合法区间是 `[97, 130943)`，越界直接抛 `IllegalArgumentException`。**
下界 97 = 用户头 27B + 最大 tag 64B + 片头 6B（低于它连空记录都放不下）；上界 130943 = 数据区大小 − 1。超长日志**不会丢**：截断保留前缀照常落盘，走 TRUNCATED 事件与 `truncatedLogs` 计数。

**易错点 2：`flushThreshold` 是三态语义。**
`> 0` = 字节阈值；`0` = 禁用按阈值自动唤醒（纯手动 flush 控制）；`-1` = 取默认 8KB（JNI 层映射 C ABI 的哨兵值）。低频日志场景若只依赖阈值，最坏要等 75ms debounce 窗口。

**易错点 3：`consoleEcho` 只给 debug 构建用。**
开启后每条成功落盘的记录同步镜像进 logcat（保留原 tag，级别映射 V/D/I/W/E/F），单条 >2KB 在 native 侧截断。logcat 经 adb 任意可读，**明文入 logcat 是泄露面**，生产构建保持默认关闭；被丢弃的记录不镜像（走 DROP 事件）。

## 4. 生命周期与线程模型

### 4.1 init / close 状态机

- **pre-init 写入直接丢弃**：`init` 前调用 `log/v/d/...` 返回 `ERR_STATE(-2)`，不崩溃、不抛异常。
- **重复 init 抛 `IllegalStateException`**：单例语义由 `initCount` 保证。换配置须先 `close()` 再 `init()`。
- **init 前奏**：新 init 会先等上一实例的在途操作退栈（上界 1s，超时抛 `IllegalStateException`），并在锁内完成遗留实例的同步销毁（含 flush 与 Flusher join，最坏 ~5s）。**不要在主线程做「close 后立刻 init」的热切换**。
- **init 失败完全回滚**：任何失败路径（含 native init 失败）都会把单例状态归零，修正配置后可直接重试。
- **close()**：等快操作（log/flush/getStats，微秒级）退栈至多 200ms 后同步销毁；**export 属慢操作不参与等待**——close 立即返回，真实销毁由 export 退栈时递延完成（句柄保证恰好销毁一次）。nativeClose 最坏 ~5s，建议在后台线程调用。
- **回调线程内调 close 是安全的**（不会自死锁），回调内除 close 外不得调用任何其他 LogX 接口。

### 4.2 热路径约束

- `log(level, tag, payload)` 的 payload 是原始字节（`v/d/i/w/e/f` 重载内部做 UTF-8 编码）；任意线程可并发调用，内部互斥。
- tag 有效长度 ≤ 64B，超限由库截断；`tag` 只需是普通字符串。
- 单条超过 `maxLogLength` → 截断保留前缀落盘（TRUNCATED 事件），不阻塞、不丢弃。
- 环满（`stagingWaitMs = 0`）→ 丢弃该条（DROP 事件 + `dropNoSpace` 计数，返回 `ERR_NO_SPACE`）。

### 4.3 flush 时机建议

库内自动触发（阈值 / 75ms debounce）之外，推荐在 `onStop()` / 退后台 / 关键业务节点手动 `LogX.flush()`。`flush()` 阻塞至进入时刻的水位全部落盘提交，返回 `ERR_OK(0)` 或状态码（如 `-10 ERR_TIMEOUT`，等待上界 5s）。崩溃安全兜底：未 flush 的 staging 残留由下次启动恢复对账捞回。

## 5. 密钥管理与 KeyStore 包裹

### 5.1 密钥模型

加密为块级 AES-256-GCM：`encrypt = true` 时**必须**提供 `keyProvider`，否则 init 失败。`LogX.KeyProvider` 只有一个方法：

```kotlin
fun interface KeyProvider {
    /** 返回 32B 裸密钥 (AES-256)；null = 该 KID 不可解。不得重入 LogX。 */
    fun getKey(kid: Int): ByteArray?
}
```

**关键规则**：

- 返回**裸密钥字节**（32B = AES-256），不是 AndroidKeyStore 的 key 对象——native 侧要做 GCM 运算。JNI 桥已做拷贝，Kotlin 层无需管理指针生命周期，但同一 `kid` 必须始终返回相同内容。
- `kid` 是密钥代际号（`Config.kid`，uint16）。**历史 KID 必须继续可解**，否则旧分卷导不出。
- 返回 null 的后果分三种：open 时当前 KID 不可解 → **拒绝启动**（fail-fast）；新建分卷时缺钥 → 该分卷**整卷明文落盘**（能力位已锁定，即便密钥稍后恢复也不再改口，直至按天/按大小轮换出新分卷）；续写已有加密分卷时缺钥 → 当期降级明文，每轮 drain 至多重试一次，密钥恢复后自动恢复密文。两类降级都会发 `DEGRADED` 事件。
- 回调可能运行在 **Flusher 后台线程**（JNI 桥已处理线程 attach/detach），实现内自行保证线程安全、禁止重入 LogX。

### 5.2 AndroidKeyStore 包裹示例

AndroidKeyStore 的 key 无法直接导出字节，标准做法是：**日志密钥（32B 随机）由 KeyStore 主密钥 GCM 包裹后存普通 SharedPreferences，`getKey()` 时解包还原**：

```kotlin
class KeystoreWrappedKeyProvider(
    private val prefs: SharedPreferences,
) : LogX.KeyProvider {

    override fun getKey(kid: Int): ByteArray? {
        if (kid != 1) return null
        return try { ensureWrappedKey() } catch (t: Throwable) {
            null // 密钥不可得 → 库侧按 §5.1 规则降级并发 DEGRADED 事件
        }
    }

    private fun masterKey(): SecretKey {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (ks.getKey("logx_master", null) as? SecretKey)?.let { return it }
        val kg = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        kg.init(
            KeyGenParameterSpec.Builder(
                "logx_master",
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
            )
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build(),
        )
        return kg.generateKey()
    }

    private fun ensureWrappedKey(): ByteArray {
        prefs.getString(PREF_KEY, null)?.let { stored ->
            val (ivB64, dataB64) = stored.split(":", limit = 2)
            val c = Cipher.getInstance("AES/GCM/NoPadding")
            c.init(Cipher.DECRYPT_MODE, masterKey(),
                GCMParameterSpec(128, Base64.decode(ivB64, Base64.NO_WRAP)))
            return c.doFinal(Base64.decode(dataB64, Base64.NO_WRAP))
        }
        val logKey = ByteArray(32).also { SecureRandom().nextBytes(it) }
        val c = Cipher.getInstance("AES/GCM/NoPadding")
        c.init(Cipher.ENCRYPT_MODE, masterKey())
        prefs.edit().putString(
            PREF_KEY,
            Base64.encodeToString(c.iv, Base64.NO_WRAP) + ":" +
                Base64.encodeToString(c.doFinal(logKey), Base64.NO_WRAP),
        ).apply()
        return logKey
    }

    private companion object { const val PREF_KEY = "logx_wrapped_key_k1" }
}
```

接入：

```kotlin
LogX.init(
    this,
    LogX.Config(
        logDir = File(filesDir, "logs"),
        stagingDir = File(filesDir, "logx_staging"),
        compress = LogX.Compress.ZSTD,
        encrypt = true,
        kid = 1,
        keyProvider = KeystoreWrappedKeyProvider(
            getSharedPreferences("logx_keys", MODE_PRIVATE),
        ),
    ),
)
```

注意：卸载清数据 = 包裹密钥作废 = 旧日志永久不可解，这是此类方案的预期语义。若业务要求密钥随账号迁移，把包裹存储换成账号维度即可，`KeyProvider` 契约不变。

### 5.3 KID 轮换

轮换 = `Config.kid` 递增 + `keyProvider` 同时应答新旧两代密钥。旧分卷按文件头记录的 KID 解密（无需迁移重写），新分卷用新 KID 写入。离线解码时把全部代际密钥都放进 `--kid-map`（见排障手册 §2）。

## 6. export 与协程示例

```kotlin
// 回调模式：from/to = yyyyMMdd 闭区间（本地时区）
val rc = LogX.export("20261001", "20261011") { date, plaintextPath, size ->
    // 每个日期分卷回调一次；plaintextPath 是明文临时文件，
    // 本回调返回（或抛出）后立即被库删除——需要留存请在此消费（如上传/拷贝）。
    upload(File(plaintextPath))   // 返回 0 = 已接收；非 0 = 业务失败（临时文件仍会删除）
}
```

```kotlin
// 协程模式（推荐）：自动 flush、结构化并发、异常如实透传
scope.launch {
    val rc = LogX.export("20261001", "20261011") { file: File ->
        withContext(Dispatchers.IO) { uploader.upload(file) }
    }
    when (rc) {
        0   -> { /* 全部导出成功 */ }
        -1  -> { /* 实例未初始化或已关闭 */ }
        -8  -> { /* LOGX_ERR_NOT_FOUND：时段内无日志文件 */ }
        -6  -> { /* LOGX_ERR_CODEC：block 抛异常或回调返回非 0（业务异常已原样向外抛） */ }
        -3  -> { /* LOGX_ERR_IO：临时文件写出失败 */ }
    }
}
```

语义细节：

- 导出前内部先 flush，止点为**调用时刻**各文件长度的快照（期间 Flusher 增量不计入本次）。
- 明文临时文件写在 `logDir` 内（`.tmp_export_*` 命名，日期+PID+随机数保证并发唯一），回调返回/抛出后立即删除——这也是备份规则把整个 `logs/` 目录排除的原因（§8）。
- `export` 是慢操作：`close()` 不会等它，销毁在其退栈时递延完成。
- 协程版：外部协程取消信号可传导中断；业务 block 抛出的异常会原样向外抛（临时文件仍安全删除），不会被状态码掩盖。

## 7. 错误回调与可观测性

```kotlin
LogX.setErrorHandler { kind, code, message ->
    // 运行在事件触发线程（写线程或 Flusher 线程）；不得重入 LogX（close 除外）
    metrics.record(kind, code, message)
}
```

`init()` 之前也可以预设回调（如提前监听启动期错误）；`close()` 之后注册不再生效。

**EventKind 分类与建议动作**（完整处置表见排障手册 §3）：

| EventKind | 含义 | 典型动作 |
|---|---|---|
| `DEGRADED` | 功能降级：mmap 失败 / 加密降级 / 密钥缺失 | 关注文件系统与 keyProvider 健康度 |
| `DROP` | 记录被丢弃（环满 NO_SPACE / 病态 maxLogLength TOO_LARGE） | 见排障手册 §3 调参 |
| `IO` | 磁盘 / 文件系统错误 | 查磁盘配额与权限 |
| `RECOVERY` | 启动恢复告警（截尾 / 重建 / 丢失计数） | 上次异常退出的正常善后，量级异常时深查 |
| `TRUNCATED` | 超长截断保前缀写入（**有损写入成功**，code = 0） | 检查业务大 payload，或调大 maxLogLength |

**Stats 快照**（`LogX.getStats(): Stats?`，init 前 / close 后返回 null）：

| 字段 | 含义 |
|---|---|
| `dropNoSpace` | staging 空闲不足丢弃次数 |
| `dropTooLarge` | 病态小 maxLogLength（截到空记录仍放不下）丢弃次数 |
| `truncatedBlocks` | 启动恢复截尾块计数（**诊断参考，非精确 SLA 指标**，撕裂场景下不必然等于真实丢块数） |
| `padRecords` | 环回绕垫 PAD 记录数（常态噪声，无需处理） |
| `blockWriteErrors` | 落盘短写 / IO 错误计数 |
| `truncatedLogs` | 超长截断保前缀写入次数（有损写入，非丢弃） |

## 8. Android 备份安全与多进程

### 8.1 备份排除规则（必配）

随包携带 `android/res/xml/data_extraction_rules.xml`（API 31+）与 `backup_rules.xml`（API 23~30），并在 Manifest 注册（片段见 `android/AndroidManifest-snippet.xml`）。排除理由：staging 是**明文** mmap 文件，密文日志也不该进云。集成方三点注意（XML 内注释同样有）：

1. `domain="file"` 即 `getFilesDir()` 根；staging 被代码强制在 filesDir 系，无需其他 domain。
2. **自定义了 `Config.logDir` / `Config.stagingDir` 目录名时，必须同步改 XML 里的 `path`**（默认排除 `logs/` 与 `logx_staging/`）。
3. 开启多进程隔离后会产生兄弟目录（见下），备份规则不支持通配符——为每个后缀目录显式追加 `<exclude>`，或干脆 `allowBackup="false"`。

### 8.2 多进程自动隔离

`autoProcessIsolation = true`（默认）时，非主进程自动改用带后缀的目录与 staging 文件：`:push` 进程 → `logs_push/` + `logx_staging_push/` + `logx_staging_push.bin`（其余进程名做字符净化）。主进程目录名不变。这避免多进程互踩同一 mmap 文件；各进程各自 init、互不知晓。需要跨进程聚合日志时，在导出/上报侧合并各目录。

## 9. 版本与兼容

- **C ABI 是稳定边界**：`logx_core` 的 `.so` 与 `logx_c.h` 必须同版本升级（Instance 经不透明指针跨越 ABI，布局稳定性由编译期 `static_assert` 守卫）。
- **`logx_stats_t` 追加式兼容**：新增统计指标只允许占用 `reserved` 区（调用方按旧 sizeof 分配的缓冲不会被越界写）；Kotlin 侧对 `.so` 版本错配做了优雅降级（字段数不足返回 null 而非崩溃）。
- **压缩枚举三端同步**：`Compress`（Kotlin）序号 = `logx_codec.h`（C++）= 块头 flag 位（落盘格式），改动需三端一致——集成方无需关心，升级库时整包升级即可。
- 分卷文件格式（LGXF/LGXB/LGXE）由解码器 `decoder/logx_decode.py` 与 C++ 导出路径双实现锁定，格式演进受「老解码器读新文件」纪律约束。

## 10. FAQ

**Q：init 抛 `IllegalStateException` 是什么？**
重复 init，或上一实例的在途操作 1s 内未退栈。检查是否在多处调 init、是否 close 后立即 init。

**Q：`LogX.i()` 没有抛异常但文件里没有？**
多为 pre-init 丢弃、进程隔离目录漂移（`logs_<后缀>/`）或环满丢弃，按排障手册 §4 速查表定位。

**Q：能用 logcat 替代文件日志吗？**
`consoleEcho` 只是把落盘记录镜像到 logcat，且仅限 debug（明文泄露面）。两者用途不同，不互替。

**Q：Flutter / iOS 能用吗？**
C ABI（`logx_c.h`）即为跨平台边界，平台壳只做转发不持逻辑；Android 之外的官方移植暂未交付。
