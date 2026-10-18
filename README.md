# LogX — mmap-based Android 崩溃安全日志库

> 实施代码,对应方案文档 [LogX日志库最终实施方案.md](./docs/LogX日志库最终实施方案.md)(v5, 2026-09-02)。
> 技术路线对标 xlog(微信 mars) / MMKV / Logan(美团) 的核心机制,并做了架构简化。

## 工程结构

```
logx/
├── core/                  # 纯 C++17 静态库,零平台依赖(经 PAL 抽象)
│   ├── include/
│   │   ├── logx_c.h       # C ABI:logx_open / write / flush / export / close
│   │   ├── logx_codec.h   # 压缩 + AES-GCM 抽象 (ARMv8 Crypto 加速)
│   │   ├── logx_format.h  # 序列化、分片、校验和 (TLS Scratch 原地拼装)
│   │   ├── logx_staging.h # 双 Header + 环形缓冲 (相对坐标系 [0, kDataSize))
│   │   └── logx_pal.h     # Platform Abstraction Layer 接口
│   └── src/               # 实现
├── android/
│   ├── cpp/logx_jni.cpp   # JNI 薄胶水 (GetStringUTFRegion 零堆分配)
│   └── kotlin/LogX.kt     # Kotlin Facade (协程 export, 自动进程名隔离)
├── decoder/logx_decode.py # 离线解码工具
├── tests/
│   ├── unit/              # gtest 单测 (127 个用例: 格式/编解码/对账/保留期/未对齐/跨日跳变/鲁棒性/JNI 字符串)
│   └── stress/            # 压测套件 (50 线程极限并发 + 15ms flush 竞争 + 50 轮崩溃注入自愈 + 3 组真实进程强杀恢复)
```

## 构建与测试

### 桌面端 (Windows MSVC)

```bash
cd logx
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DLOGX_WITH_ZSTD=ON \
         -DLOGX_WITH_AESGCM=ON
cmake --build . -j
ctest --output-on-failure
```

### Android 平台与交叉编译 (NDK + adb)

#### 平台与系统要求
- **Android 最低支持版本**: **Android 5.0 (API Level 21)**，支持全部主流 ABI（`arm64-v8a`、`armeabi-v7a`、`x86_64`、`x86`）。
- **代码编写标准**: 全库（Native C++、PAL 跨平台抽象层、JNI 桥接层、Kotlin Facade）**统一遵循 Android API 21 基线编写**。严禁直接调用无版本降级保护的高版本系统特性或 libc API；若使用高版本 Android 特性（如 API 28+ 的 `Application.getProcessName`、API 31+ 的 `dataExtractionRules`），必须通过 `Build.VERSION.SDK_INT` 分支提供健全的 API 21 降级回退通道。
- **宿主 App 接入**: `minSdkVersion 21` 或更高。

```bash
# 交叉编译 (最低支持 Android 5.0 / API 21, 以 arm64-v8a 为例)
cmake -S . -B build-android \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 \
  -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release \
  -DLOGX_BUILD_TESTS=ON -DLOGX_BUILD_STRESS=ON \
  -DLOGX_WITH_ZSTD=ON -DLOGX_WITH_AESGCM=ON -G Ninja

cmake --build build-android -j

# 推送二进制至设备并执行
adb shell mkdir -p /data/local/tmp/logx_test
adb push $ANDROID_NDK/.../libc++_shared.so /data/local/tmp/logx_test/
adb push build-android/liblogx.so /data/local/tmp/logx_test/
adb push build-android/tests/unit/logx_unit_tests /data/local/tmp/logx_test/
adb push build-android/tests/stress/logx_stress_tests /data/local/tmp/logx_test/
adb shell "chmod 755 /data/local/tmp/logx_test/*"

# 在真机上运行测试
adb shell "cd /data/local/tmp/logx_test && export LD_LIBRARY_PATH=. && ./logx_unit_tests"
adb shell "cd /data/local/tmp/logx_test && export LD_LIBRARY_PATH=. && ./logx_stress_tests"
```

> M1/M2 阶段 `LOGX_WITH_ZSTD` 与 `LOGX_WITH_AESGCM` 默认 OFF(flag=none 跑通);
> M3 阶段打开二者开关,接入 zstd 单文件版 + ARMv8 Crypto / mbedtls 兜底,文件格式不变。

## 关键不变量(参见方案 §5)

1. **条目原子性**:同条日志的所有分片连续,不被 PAD 打断
2. **写指针并发安全**:全局 mutex
3. **生产者与 Flusher 互不阻塞**:回收清零在锁外
4. **文件单写者**:Flusher 独占写入与切分/清理
5. **环形不变量**:任意时刻除 `[flushed_len, write_pos)` 外全环为零
6. **崩溃撕裂最小化**:任意 kill 撕裂条目 ≤ 1

## 使用示例(Android)

> 完整接入文档（Config 全参数、KeyStore 密钥包裹、生命周期/线程模型、备份与多进程）见 [docs/integration_guide.md](docs/integration_guide.md)；离线解码与线上排障见 [docs/ops_troubleshooting.md](docs/ops_troubleshooting.md)。

```kotlin
// Application.onCreate
LogX.init(this, LogX.Config(
    logDir      = File(filesDir, "logs"),          // 框架内部自动根据进程名隔离子进程目录 (如 logs_push)
    stagingDir  = File(filesDir, "logx_staging"), // 必须 filesDir 系; 自动生成 staging_main.mmap 等
    consoleEcho = BuildConfig.DEBUG,               // T5.4: debug 期镜像到 logcat (生产保持默认关闭)
    level       = LogX.Level.I,
    retentionDays = 7,
    compress    = LogX.Compress.ZSTD,
    encrypt     = true,                           // 提供 KeyProvider
    kid         = 1,
    keyProvider = { kid -> keyStore.getKey(kid) },
))

// Activity.onStop / onBackground
LogX.flush() // 阻塞等待当前目标水位落盘即返回, 防高频写入饥饿

// 导出上报 (协程模式, 自动管理明文临时文件生命周期)
lifecycleScope.launch(Dispatchers.IO) {
    LogX.export("20260901", "20260902") { file ->
        uploader.uploadSync(file)
    }
}
```

## 关键纪律(代码评审对照)

- mmap 必须 `MAP_SHARED`;ftruncate 预分配 128KB;`O_CLOEXEC`
- staging 坐标系统一为环内相对偏移 `[0, kDataSize)` (130944B)
- 双 Header 提交在 A 写入后必须加 `std::atomic_thread_fence(std::memory_order_seq_cst)` 再写 B
- JNI 字符串传递采用 `GetStringUTFRegion` 配合 TLS Buffer, 零堆内存分配
- staging 放 filesDir(禁 cacheDir;系统清理会让 mmap 变孤儿)
- `msync(MS_SYNC)` 仅用于 staging;日志文件用 `fdatasync()`
- 退后台 + 30s 周期做一次刷盘(参见方案 §11)
- 多进程 Safe-by-Default 隔离 (按进程名后缀分流目录)
- 切分序号升级为 2B (uint16_t, 上限 65535, 支撑 655GB/天)
- 单条日志上限 64KB;超长整条丢弃 + 打点
- 重复 `init` 抛 `IllegalStateException`(不支持热替换)

## 已完成的核心里程碑 (M1 ~ M4)

- ✅ **M1/M2 核心链路与恢复**: 环形 mmap staging、单 Flusher 线程、Σraw_len 精确回推、双 Header 容错、截断续写跳 1 空隙。
- ✅ **M3 压缩与加密闭环**: zstd 单文件集成、mbedtls AES-256-GCM 真加解密、12B Nonce 安全构造、JNI KeyProvider 全局引用桥接。
- ✅ **M4 生产加固与稳定性压测**: 
  - 日志保留期全时机自动清理 (启动/跨天/退后台) 与平年/闰年/月界防御；
  - 50 线程极限并发写入 + 15ms flush 强切竞争压测 (300,000+ logs/s)，seqno 严格单调递增，无死锁，对账零丢失；
  - 硬件级未对齐内存访问校验 (零 SIGBUS) 与 23:59:59 跨日/回拨时钟跳变仿真；
  - 崩溃注入与 50 轮随机多阶段崩溃恢复自愈测试，397/397 条日志 100% 挽回；
  - 根治环尾不足 6B 留零脏内存与滑动窗口重同步提前截断 Bug；
  - 全量单元测试 + 压测用例在 Windows (MSVC) 与 Android 真机 (Redmi K30 Pro, arm64-v8a, Android 12) 上 100% 全部通过 (双端全绿);2026-09-08 现状: 全开矩阵 127 单测 + 4 压测 + 3 真实进程强杀恢复,基础矩阵 108 单测。

## 后续规划 (M5 阶段)

> 2026-09-08: 三份代码审查报告 (C/F/S/R/E/P/J/K/D/T 全系列) 已全部闭环销号,进入 M5 交付阶段。

- ✅ **Android `allowBackup` / `dataExtractionRules` 备份安全工程化配置 (T5.1, 2026-09-08)**:
  交付 `android/res/xml/data_extraction_rules.xml` (API 31+, cloud-backup/device-transfer 双通道)、
  `android/res/xml/backup_rules.xml` (API 23~30) 与 `android/AndroidManifest-snippet.xml`
  完整集成片段, 将 `filesDir/logs/` 与 `filesDir/logx_staging/` 排除出云端备份与设备迁移,
  防止明文 staging 与密文日志进云; 附自定义目录/多进程隔离目录 (`logs_<后缀>/`) 边界说明
  与 `allowBackup="false"` 最严备选方案。
- ~~Kotlin Facade Gradle 自动化编译工程接入~~ (2026-09-08 已建成 `android/kotlin-gradle/`,见下文验证闭环)
- ~~离线解码器用实际引擎真机产出日志做最终联调~~ (2026-09-08 真机联调通过, 104/104 精确对账,见下文验证闭环)
- ✅ **性能基线测试与报告输出 (T5.2, 2026-09-08)**: 新增 `tests/bench/logx_bench.cpp` 基准 harness (`-DLOGX_BUILD_BENCH=ON`) 与 [docs/benchmark_report.md](docs/benchmark_report.md)。双端实测 (Windows x64 + Redmi K30 Pro): 写入延迟桌面 P50 0.2µs / P99 3.1µs (纯路径口径), 真机 P50 3.2µs; 压缩开关对写路径零影响; zstd 落盘体积 -38% (1.52x); so 体积 (arm64 strip, MinSizeRel 全开) 548.5KiB。含 v1.0 回归基线声明与 2µs/500KB 目标判定。
- ✅ **logcat 直通开关 consoleEcho (T5.4, 2026-09-09)**: JNI 层实现 (C ABI 零改动), `Config.consoleEcho` 开启后每条成功落盘记录按 level 六级映射镜像到 logcat (2KB 栈缓冲截断, 丢弃不镜像); 顶层 CMake 补链 liblog。kotlin-gradle 扩展为可安装 smoke 工程, 真机 6/6 echo 按等级正确 (含中文), 纯逻辑单测 9/9 全绿。生产默认关闭 (明文泄露面)。
- ✅ **接入指南与离线解码运维手册 (T5.3, 2026-10-11)**: 交付 [docs/integration_guide.md](docs/integration_guide.md)（五分钟接入、Config 14 参数全参考、生命周期与线程模型、AndroidKeyStore 包裹 KeyProvider 完整示例、export 协程语义、备份/多进程、版本兼容）与 [docs/ops_troubleshooting.md](docs/ops_troubleshooting.md)（取证流程、logx_decode.py 用法与错误分层、事件处置表、症状→根因速查、stats 口径、C ABI 附录、tester 联动）；README 使用示例节挂接两文档。Release AAR 为后续发布动作（M5 收尾项）。
- ✅ **zstd 源码级裁剪与发布 AAR 体积达标 (T-Z0~T-Z6 + D6-B2, 2026-10-17)**: `third_party/zstd` 0-diff，经 6 个 `ZSTD_EXCLUDE_*` + `ZSTD_STRIP_ERROR_STRINGS` + `ZSTD_NO_TRACE` 宏裁剪（shared 口径收益 148,920 B），AAR native 切 MinSizeRel `-Oz -DNDEBUG -g` + `c++_shared`；D6-B2 仅对 4 个写入热路径 TU 追加 `-O2`（zstd/mbedtls 保持 `-Oz`）。发布 AAR 内 `jni/arm64-v8a/liblogx.so` strip 后 **385,112 B < 512,000 B 门禁（余量 126,888 B）**。总 native 载荷 1,759,448 B（含 `libc++_shared.so`，单列披露）。验证：单测 197/197、真机四套件全绿、独立解码 104/104、AAR 消费端 141/141、level 0–22 级联 smoke PASS；D6-B2 真机纯路径 P50 **3,698 ns**、生产 zstd P50 **3,958 ns**（回到 baseline，压缩比 1.52x 不变）。详见 [docs/zstd_prune_plan.md](docs/zstd_prune_plan.md) 与 [docs/benchmark_report.md](docs/benchmark_report.md) §5.4/§7.1。
- 📋 **生命周期堆控制块抽象与 Dekker 状态机收敛 (T5.8, 2026-10-18 进行中)**: 当前状态回退为【未发布 / 进行中】。重开 1.x 重构，引入生命周期堆控制块（Control Block）抽象统一管辖引擎生命周期，以强/弱原子引用计数取代 `active_ops` / `in_callback` / `close_pending` / `close_on_flusher` 四态协同，根治 R41-05 留置的理论尾读 UAF 隐患与极高心智负担，验证通过后正式发布。