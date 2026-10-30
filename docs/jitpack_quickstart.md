# LogX JitPack 快速导入与使用流程

> 发布通道:JitPack(`https://jitpack.io`) | 坐标:`com.github.zzlfeng:LogX:v1.0.0-beta1` | 发布日期:2026-10-31
> 源码:GitHub [zzlfeng/LogX](https://github.com/zzlfeng/LogX) tag `v1.0.0-beta1`(commit `7c49883`,R49 收口后代码)
> 本批验证:JitPack 云端 NDK 29 全量构建 4m34s 全绿,AAR 677,453 B 与本地构建字节级一致
> 深入内容以 [integration_guide.md](./integration_guide.md)(逐 API 核对的权威文档)与 [ops_troubleshooting.md](./ops_troubleshooting.md) 为准;本文是可直接照抄的端到端快速路径。

---

## 第 0 步 · 前置条件

- Android Studio 工程,`minSdk 21+`,`compileSdk 34+`
- **arm64-v8a 真机**(本版 AAR 仅含此 ABI,模拟器不可用)

## 第 1 步 · 引入依赖

```kotlin
// settings.gradle.kts → dependencyResolutionManagement.repositories 里加:
maven { url = uri("https://jitpack.io") }
```

```kotlin
// app/build.gradle.kts
android {
    defaultConfig {
        ndk { abiFilters += "arm64-v8a" }   // 本版 AAR 仅此 ABI
    }
}
dependencies {
    implementation("com.github.zzlfeng:LogX:v1.0.0-beta1")
    // kotlinx-coroutines 由 POM 自动传递(compile 依赖),无需手动引入
}
```

Sync 后在 External Libraries 可见 `com.github.zzlfeng:LogX`,内含 `classes.jar`(Kotlin Facade)+ `liblogx.so` + `libc++_shared.so`。

> 后续升级只需换版本号:GitHub 打新 tag(如 `v1.0.0-beta2`)即自动发布,JitPack 首次访问该坐标时触发构建。

## 第 2 步 · 备份排除规则(必配,AAR 未携带)

staging 是**明文 mmap 文件**,必须排除出云备份/设备迁移。二选一:

**方案 A(推荐)**:把两个 XML 拷进 `app/src/main/res/xml/`,并在 `<application>` 合并四个属性。

`res/xml/data_extraction_rules.xml`(API 31+):

```xml
<data-extraction-rules>
    <cloud-backup>
        <exclude domain="file" path="logs/" />
        <exclude domain="file" path="logx_staging/" />
    </cloud-backup>
    <device-transfer>
        <exclude domain="file" path="logs/" />
        <exclude domain="file" path="logx_staging/" />
    </device-transfer>
</data-extraction-rules>
```

`res/xml/backup_rules.xml`(API 23~30):

```xml
<full-backup-content>
    <exclude domain="file" path="logs/" />
    <exclude domain="file" path="logx_staging/" />
</full-backup-content>
```

`AndroidManifest.xml`:

```xml
<application
    android:allowBackup="true"
    android:dataExtractionRules="@xml/data_extraction_rules"
    android:fullBackupContent="@xml/backup_rules"
    tools:targetApi="31" ... >
```

注意:

1. `domain="file"` 即 `getFilesDir()` 根,LogX 强制 staging 位于 filesDir 系,故无需其他 domain;
2. 自定义了 `Config.logDir` / `Config.stagingDir` 目录名时,必须同步修改规则文件里的 `path`;
3. 多进程自动隔离(`autoProcessIsolation=true`)会生成兄弟目录(`:push` 进程 → `logs_push/` 等),备份规则**不支持通配符**,须为每个后缀目录显式追加 `<exclude>`。

**方案 B(更简单)**:`android:allowBackup="false"` 整体禁用备份(含 adb backup 路径),两个 XML 都不用配。两者取其一。

## 第 3 步 · 初始化

```kotlin
// App.kt —— 在 Manifest 里注册 android:name=".App"
class App : Application() {
    override fun onCreate() {
        super.onCreate()
        LogX.init(
            this,
            LogX.Config(
                logDir = File(filesDir, "logs"),               // 正式日志目录
                stagingDir = File(filesDir, "logx_staging"),   // 必须 filesDir 系,禁 cacheDir
            ),
        )
        LogX.i("App", "started, version=${BuildConfig.VERSION_NAME}")
    }
}
```

生命周期规则:

- **整个进程只 init 一次**,重复 init 抛 `IllegalStateException`;init 失败完全回滚,修正后可直接重试;
- 换配置先 `close()`(建议放后台线程,最坏 ~5s)再 `init()`,**不要在主线程做 close→init 热切换**;
- init 前写日志不崩溃:返回 `ERR_STATE(-2)` 静默丢弃。

## 第 4 步 · 日常写日志

```kotlin
LogX.v("Net", "verbose detail")     // 任意线程、任意位置,微秒级
LogX.d("Net", "request sent")
LogX.i ("Pay", "order created")
LogX.w("Pay", "retry count=3")
LogX.e("Pay", "gateway timeout")
LogX.f("Pay", "fatal state")

// 退后台/关键节点手动刷盘(不刷也不会丢:崩溃后下次启动自动恢复 staging 残留)
override fun onStop() { LogX.flush() }
```

- tag 自动截断到 64B;单条超 `maxLogLength` 自动**截断保前缀**落盘(不丢不阻塞,发 TRUNCATED 事件);
- 环满默认**立即丢弃**并回报 DROP 事件(绝不阻塞业务);想容忍突发给 `stagingWaitMs > 0`;
- debug 构建想 logcat 直通:`Config(consoleEcho = true)`,**生产保持关闭**(logcat 任意可读,明文泄露面)。

## 第 5 步 · 生产口径配置(推荐)

```kotlin
LogX.init(this, LogX.Config(
    logDir = File(filesDir, "logs"),
    stagingDir = File(filesDir, "logx_staging"),
    compress = LogX.Compress.ZSTD,          // 块级压缩,体积约 -38%
    encrypt = true,                          // 块级 AES-256-GCM
    kid = 1,                                 // 密钥代际号
    keyProvider = { kid -> keyStoreRepo.loadRawKey(kid) },  // 返回 32B 裸密钥
))
```

`KeyProvider` 三条铁律:

1. 返回**裸密钥字节**(32B = AES-256),不是 AndroidKeyStore 的 key 对象;
2. 同一 `kid` 恒返回相同内容;
3. **历史 KID 必须持续可解**,否则旧分卷导不出。

返回 null 触发降级(open 期当前 KID 缺钥 → 拒绝启动;续写期缺钥 → 该分卷降级明文,密钥恢复后自动恢复密文),均发 `DEGRADED` 事件。回调可能运行在 Flusher 后台线程,须线程安全、禁止重入 LogX。AndroidKeyStore 包裹完整示例见 [integration_guide.md §5.2](./integration_guide.md);KID 轮换见 §5.3。

## 第 6 步 · 导出日志

```kotlin
scope.launch {
    val rc = LogX.export("20261001", "20261031") { file: File ->
        withContext(Dispatchers.IO) { uploader.upload(file) }
        // 明文临时文件,回调返回(或抛出)后立即被库删除——要留存就在回调里消费
    }
    // 0 成功 / -1 未初始化 / -8 时段无日志 / -6 解码或回调失败 / -3 IO
}
```

导出前内部先 flush;止点是**调用时刻**各文件长度的快照;`export` 是慢操作,`close()` 不等它(销毁递延到其退栈时)。

## 第 7 步 · 错误回调与运行统计

```kotlin
LogX.setErrorHandler { kind, code, msg -> metrics.record(kind, code, msg) }
```

| EventKind | 含义 | 典型动作 |
|---|---|---|
| `DEGRADED` | 功能降级:mmap 失败 / 加密降级 / 密钥缺失 | 查文件系统与 keyProvider 健康度 |
| `DROP` | 记录被丢弃(环满 NO_SPACE / 病态 TOO_LARGE) | 调大 staging 或 maxLogLength,见排障手册 §3 |
| `IO` | 磁盘 / 文件系统错误 | 查磁盘配额与权限 |
| `RECOVERY` | 启动恢复告警(截尾/重建/丢失计数) | 上次异常退出的正常善后,量级异常时深查 |
| `TRUNCATED` | 超长截断保前缀(**有损写入成功**,code=0) | 查业务大 payload 或调大 maxLogLength |

```kotlin
LogX.getStats()   // dropNoSpace / dropTooLarge / truncatedBlocks / truncatedLogs / blockWriteErrors…
```

回调运行在写线程或 Flusher 线程,**不得重入 LogX**(close 除外);init 前也可预设。

## 第 8 步 · 验证与离线解码

真机上确认落盘:

```bash
adb shell run-as <包名> ls files/logs/          # 看到 YYYYMMDD.log 分卷(单卷 10MB,超出切 .1/.2…)
adb shell run-as <包名> cat files/logs/20261031.log > 20261031.log
```

离线解码(`decoder/logx_decode.py`,与库内格式双实现锁定):

```bash
# 明文 / 仅压缩
python logx_decode.py 20261031.log --out out/

# 加密日志:把各代际密钥放进 kid-map(16 进制 32B 裸密钥)
python logx_decode.py logs_dir/ --from 20261001 --to 20261031 --kid-map 1=a3f5... --out out/
```

## 排障速查

| 现象 | 定位 |
|---|---|
| `LogX.i()` 不报错但文件里没有 | pre-init 丢弃(init 前调用返回 `ERR_STATE`);或多进程目录漂移(`:push` 进程写 `logs_push/`);或环满丢弃(查 `getStats().dropNoSpace`) |
| init 抛 `IllegalStateException` | 重复 init,或上实例在途操作 1s 未退栈;避免 close 后立即 init |
| 设备上没生成任何文件 | 确认 abiFilters 含 `arm64-v8a` 且跑在真机;init 异常信息会带原因 |
| 拉不到文件 | `run-as` 需 debug 包;release 包用导出接口或 root |
| 回调里崩溃 | 回调线程内除 `close` 外禁调任何其他 LogX 接口 |

## 附:Config 全参数速查

| 参数 | 默认 | 说明 |
|---|---|---|
| `logDir` / `stagingDir` | 必填 | 正式日志目录 / mmap staging(强制 filesDir 系) |
| `level` | `Level.I` | 最低写入级别,运行期 `setMinLevel()` 可调 |
| `retentionDays` | 7 | 过期分卷自动清理 |
| `maxFileSize` | 10MB | 单分卷上限,超出切 `.1/.2…` |
| `maxLogLength` | 64KB | 单条上限,**合法区间 `[97, 130943)`**,越界抛 `IllegalArgumentException` |
| `flushThreshold` | 8KB | 三态:`>0` 字节阈值 / `0` 禁用自动唤醒 / `-1` 取默认 |
| `compress` | NONE | `NONE / ZSTD / ZLIB` |
| `encrypt` | false | 开启必须给 `keyProvider` |
| `kid` | 1 | 密钥代际号 |
| `keyProvider` | null | `(kid: Int) -> ByteArray?` |
| `errorHandler` | null | 事件回调 |
| `autoProcessIsolation` | true | 多进程目录自动隔离 |
| `consoleEcho` | false | logcat 直通,仅 debug |
| `stagingWaitMs` | 0 | 环满阻塞上限;0 = 绝不阻塞直接丢弃 |

---

> 版本记录:`v1.0.0-beta1`(2026-10-31,本文件对应版本);后续发版在 GitHub 打 tag 后更新顶部坐标行即可。
