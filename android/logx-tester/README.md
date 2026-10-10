# LogX 真机交互式调参测试 App (logx-tester)

用于在 Android 真机上动态调整 LogX 库的全部可调参数，实时测试写入结果、观察事件流与统计吞吐延迟。

## 1. 架构与设计原则

- **UI 技术栈**: Jetpack Compose (Material3)
- **Native 零改动**: 完全复用已有的 `liblogx.so` 和 `libc++_shared.so`，不改动任何 C++ 或 JNI 代码
- **精准结果捕获**: 基于 `LogXProbe`（`ThreadLocal` 哨兵模式）拦截调用线程上的同步 DROP 事件（`"staging full"`、`"log too large"`、`"invalid log input"`），精确还原每条日志的写入判定
- **防并发死锁与竞态**: `EngineController` 采用 `Mutex` 串行化 `applyAndRestart` 与 `stop`，状态跨配置变更存活于 `TesterViewModel`
- **压测护栏**: 压测期间强制 `consoleEcho=false` 防 logcat 刷爆；使用预分配 `LongArray` 与 `latencies.filter { it > 0 }` 防 GC 抖动与提前中止分位数失真；若压测期间出现任何丢弃，顶部显示“数据失真”醒目警示

### 1.1 核心架构假设与约束 (Architectural Assumptions)

1. **【核心假设 A · Native 同步回调】**:
   `LogXProbe` 的 `ThreadLocal` 哨兵机制强依赖底层调用栈的同步性：`LogX.log()` -> JNI `nativeWrite()` -> `logx_write()` -> `emit_event()` -> `jni_error_trampoline()` -> `LogX.onNativeError()` -> `LogXProbe.onEvent()`。全程在发起调用的同一线程上同步执行完毕。若未来 Native 核心重构为异步消息队列分发，此捕获机制需同步重构为基于全局 RequestID 或无锁序列号追踪的关联映射。
2. **【核心假设 B · 延迟测算口径】**:
   压测测得的延迟（P50/P90/P99）为从 Kotlin API 发起到 Native 返回的**端到端耗时**。除包含实际写入 staging 环耗时外，还包含 2 次 `System.nanoTime()` 与 1 次 `ThreadLocal.set(null)` 的纳秒级系统底噪（约 20~30ns），对于微秒级日志写入影响在 1%~2% 以内。

---

## 2. 构建与运行

### 2.1 环境要求
- **ABI**: 仅限 `arm64-v8a` 真机（预编译 so 为 arm64-v8a）
- **JDK**: JDK 17 或 JDK 21+（推荐 Android Studio 内置 JBR: `C:\ProgramFiles\Android\Android Studio\jbr`）
- **SDK**: `compileSdk = 37`, `minSdk = 24`

### 2.2 构建命令

```bash
cd logx/android/logx-tester

# Windows PowerShell (指定 JDK)
$env:JAVA_HOME = "C:\ProgramFiles\Android\Android Studio\jbr"
.\gradlew.bat :app:assembleDebug --no-daemon
```

构建成功产物位于：
`app/build/outputs/apk/debug/app-debug.apk`

### 2.3 安装与日志观察

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk

# 观察镜像日志 (当 consoleEcho 开启时)
adb logcat -s LogX:V TesterTag:V
```

---

## 3. 参数配置说明

| 参数 | 类型 | 默认值 | 边界与校验说明 |
| --- | --- | --- | --- |
| `logDir` | File | `logs` | 日志持久化主目录，位于 `filesDir` 下 |
| `stagingDir` | File | `logx_staging` | 暂存区 mmap 目录。**严禁包含 `/cache/`**，否则抛 `IllegalArgumentException` |
| `maxLogLength` | Int | 65536 | 单条日志总长上限。合法范围 `97..65536`；<97 或 >65536 在 C 侧校验失败，>=130943 在 Kotlin 侧被 require 拦截 |
| `flushThreshold` | Int | 8192 | 自动唤醒阈值。`0`=纯手动 flush；`-1`=取默认 8KB；其它=字节阈值 |
| `maxFileSize` | Long | 10MB | 单个日志文件大小硬上限，达到后自动切分滚动（生成 `.log.1`、`.log.2`） |
| `retentionDays` | Int | 7 | 日志保留期（天），超时文件在启动/退出时被清理 |
| `compress` | Enum | NONE | 压缩算法：`NONE(0)` / `ZSTD(1)` / `ZLIB(2)`（ZLIB 未实现，启动直接拒绝） |
| `encrypt` | Boolean | false | AES-128-GCM 加密开关。开启时若 `keyProvider` 为 null 则 init 直接拒绝 |
| `kid` | Int | 1 | 密钥标识 (0..65535) |
| `keyProvider` | Mode | 内置32B | 密钥提供方，支持内置 32B、自定义 64 字符 Hex、返回 null (降级测试) 等模式 |
| `autoProcessIsolation` | Boolean | true | 多进程目录隔离，根据进程名后缀自动创建独立子目录与 staging 文件 |
| `consoleEcho` | Boolean | false | 每条成功落盘的记录实时镜像输出到 logcat（截断 2KB） |
| `minLevelGate` | Level | V | **App 侧门控**。LogX 库内核无等级过滤功能，由 App 侧进行过滤 |

---

## 4. 预置场景预期结果速查表

| # | 场景 | 核心参数变更 | 预期观测结果 |
| --- | --- | --- | --- |
| 1 | 基线配置 | 无压缩 / 无加密 | 写入全部 OK；落盘体积 ≈ payload + 34B 块头 |
| 2 | ZSTD 压缩 | `compress=ZSTD` | 写入 OK；落盘文件大小明显缩减（约 1.5x 压缩比） |
| 3 | ZSTD + 加密 | `compress=ZSTD` + `encrypt=true` | 写入 OK；每块额外增加 16B GCM tag |
| 4 | ZLIB 拒绝 | `compress=ZLIB` | **init 失败**，返回 `nativeHandle=0`（库内未接入 zlib 压缩） |
| 5 | 加密缺 KeyProvider | `encrypt=true` + `keyProvider=null` | **init 失败**（C 侧校验拒绝） |
| 6 | 密钥返回 null 降级 | `keyProvider` 返回 null | **init 成功**，但落盘降级明文，产生 `DEGRADED "missing key; downgrading block to plaintext"` 事件 |
| 7 | maxLogLength 越界 | 131072 / 100000 / 96 / 0 | 131072 被 Kotlin require 拦截；100000、96 和 0 被 C 侧 `INVALID_ARG` 拦截（96 = B-6 下界 97-1） |
| 8 | 超长 payload | payload = 70000B | 写入返回 `INVALID`（用户总长 > 60KB 触发 `"invalid log input"`） |
| 9 | 超 maxLogLength | `maxLogLength=16384` + payload=30000B | 写入返回 `TOO_LARGE`（触发 `"log too large"`） |
| 10 | 20KB 分片 | payload = 25000B | 写入 OK，内部自动拆为多片 LGXE 记录并正确合并与落盘 |
| 11 | 纯手动 flush | `flushThreshold=0` | 不按阈值自动唤醒，仅靠 75ms debounce 兜底，手动点击 flush 立即落盘 |
| 12 | staging 环满 (压测) | `flushThreshold=0` + 50 线程并发 | 130944B 数据环迅速填满，产生大量 `dropNoSpace` 与 `staging full` DROP 事件 |
| 13 | 文件切分 | `maxFileSize=65536` | 写入超 64KB 后文件自动切分生成 `YYYYMMDD.log.1`、`.log.2` |
| 14 | staging 禁 cacheDir | `useCacheDir=true` | Kotlin `require` 抛出 `IllegalArgumentException`，UI 安全捕获 |
| 15 | tag 越界 | tag 长度 > 64B | 写入返回 `INVALID`（触发 `"invalid log input"`） |

---

## 5. 指标口径与 Native 原子诊断快照说明

UI 指标区划分了三个不同的观察维度，避免混淆统计口径：

1. **【调用端 API 写入判定】** (`writeOk`, `dropNoSpace`, `dropTooLarge`, `invalidInput`):
   统计 App 业务线程发起 `LogX.log()` 调用的单条执行结果。
2. **【内核事件广播累计】** (`DEGRADED`, `DROP`, `IO`, `RECOVERY`):
   来自 `LogX.ErrorHandler` 回调事件流。**此处的 DROP 事件与上方 API 的写入丢弃属于同一物理事件的底层广播视角，数值相对应，绝非二次丢弃，不可叠加求和**。
3. **【Native 核心原子计数快照】** (`LogX.getStats()`):
   直接通过 JNI 读取 C++ 底层 `std::atomic<uint64_t>` 计数器：
   - `dropNoSpace`: staging 环满丢弃次数（与 App 侧指标直接印证）；
   - `dropTooLarge`: 单条超限丢弃次数（与 App 侧指标直接印证）；
   - `truncatedBlocks`: 异常崩溃启动恢复时发现的截尾块数；
   - `padRecords`: 环形缓冲区回绕时插入的 PAD 记录数；
   - `blockWriteErrors`: flusher 落盘短写或 IO 错误计数。

---

## 6. 技术债务与问题解决记录 (Resolved Issues)

### P2 · `LogXProbe` 强类型错误码映射改造（已解决）

- **解决背景**:
  原实现通过 `c.msg.contains(...)` 匹配文案来判定丢弃原因，存在文案耦合脆弱性。
- **全链路改造方案**:
  1. **C++ 内核** (`core/src/logx_c.cpp`): 在 `logx_write` 的 3 处丢弃分支中，将硬编码的 `-1` 替换为 C ABI 错误码 `LOGX_ERR_INVALID_ARG(-1)`、`LOGX_ERR_TOO_LARGE(-5)`、`LOGX_ERR_NO_SPACE(-4)`；
  2. **动态库重编**: 重新编译 `arm64-v8a` 架构的 `liblogx.so` 并同步更新至 `jniLibs`；
  3. **Kotlin 常量导出** (`LogX.kt`): 导出与 C ABI `logx_status_e` 一一对应的 `LogX.ERR_*` 规范常量；
  4. **探针判定升级** (`LogXProbe.kt`): 优先按 `c.code`（`LogX.ERR_NO_SPACE` 等）进行强类型匹配，同时保留文本匹配作为向后兼容兜底，彻底消除了对 Native 文案的脆弱耦合。

