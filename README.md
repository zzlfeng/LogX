# LogX — mmap-based Android 崩溃安全日志库

> 实施代码,对应方案文档 [LogX日志库最终实施方案.md](../LogX日志库最终实施方案.md)(v4, 2026-08-31)。
> 技术路线对标 xlog(微信 mars) / MMKV / Logan(美团) 的核心机制,并做了架构简化。

## 工程结构

```
logx/
├── core/                  # 纯 C++17 静态库,零平台依赖(经 PAL 抽象)
│   ├── include/
│   │   ├── logx_c.h       # C ABI:logx_open / write / flush / export / close
│   │   ├── logx_codec.h   # 压缩 + AES-GCM 抽象
│   │   ├── logx_format.h  # 序列化、分片、校验和
│   │   ├── logx_staging.h # 双 Header + 环形缓冲
│   │   └── logx_pal.h     # Platform Abstraction Layer 接口
│   └── src/               # 实现
├── android/
│   ├── cpp/logx_jni.cpp   # JNI 薄胶水(零逻辑)
│   └── kotlin/LogX.kt     # Kotlin Facade
├── decoder/logx_decode.py # 离线解码工具
└── tests/unit/            # gtest 单测
```

## 构建

```bash
cd logx
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DLOGX_WITH_ZSTD=ON \
         -DLOGX_WITH_AESGCM=ON
cmake --build . -j
ctest --output-on-failure
```

> M1/M2 阶段 `LOGX_WITH_ZSTD` 与 `LOGX_WITH_AESGCM` 默认 OFF(flag=none 跑通);
> M3 阶段打开二者开关,接入 zstd 单文件版 + mbedtls 子集,文件格式不变。

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
    logDir      = File(filesDir, "logs"),
    stagingDir  = File(filesDir, "logx_staging"),
    level       = LogX.Level.I,
    retentionDays = 7,
    compress    = LogX.Compress.NONE,
    encrypt     = false,                  // M3 阶段置 true + 提供 KeyProvider
    kid         = 1,
    keyProvider = { kid -> keyStore.getKey(kid) },
))
// Activity.onStop / onBackground
LogX.flush()
LogX.export("20260801", "20260831") { date, path, size ->
    upload(date, path); 0
}
```

## 关键纪律(代码评审对照)

- mmap 必须 `MAP_SHARED`;ftruncate 预分配 128KB;`O_CLOEXEC`
- staging 放 filesDir(禁 cacheDir;系统清理会让 mmap 变孤儿)
- `msync(MS_SYNC)` 仅用于 staging;日志文件用 `fdatasync()`
- 退后台 + 30s 周期做一次刷盘(参见方案 §11)
- 单进程日志库;多进程按进程名后缀隔离目录
- 单条日志上限 64KB;超长整条丢弃 + 打点
- 重复 `init` 抛 `IllegalStateException`(不支持热替换)

## 已知未完成项(M3/M4 阶段)

- zstd 单文件版 / mbedtls AES-GCM 真接入(开关已就绪)
- JNI keyProvider trampoline(全局引用 + AttachCurrentThread)
- 50 线程压测 / monkey 72h 稳定性 / 断电注入(QEMU 快照)
- Android `allowBackup` / `dataExtractionRules` 工程化(规则示例见 `android/AndroidManifest-snippet.xml`)