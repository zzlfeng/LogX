# LogX — mmap-based Android 崩溃安全日志库

> 实施代码,对应方案文档 [LogX日志库最终实施方案.md](../LogX日志库最终实施方案.md)(v5, 2026-09-02)。
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
│   ├── unit/              # gtest 单测 (59 个用例: 格式/编解码/对账/保留期/未对齐/跨日跳变)
│   └── stress/            # 压测套件 (50 线程极限并发 + 15ms flush 竞争 + 50 轮崩溃注入自愈)
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

### Android 交叉编译与真机测试 (NDK + adb)

```bash
# 交叉编译 (以 arm64-v8a 为例)
cmake -S . -B build-android \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
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

```kotlin
// Application.onCreate
LogX.init(this, LogX.Config(
    logDir      = File(filesDir, "logs"),          // 框架内部自动根据进程名隔离子进程目录 (如 logs_push)
    stagingDir  = File(filesDir, "logx_staging"), // 必须 filesDir 系; 自动生成 staging_main.mmap 等
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
  - 全量 59 个单元测试 + 4 个压测用例在 Windows (MSVC) 与 Android 真机 (Redmi K30 Pro, arm64-v8a, Android 12) 上 100% 全部通过 (双端全绿)。

## 后续规划 (M5 阶段)

- Android `allowBackup` / `dataExtractionRules` 安全工程化配置
- JNI/Kotlin Facade 自动化编译与离线解码器端到端联调
- 性能基线测试与报告输出 (P99 延迟、压缩比、so 二进制体积)