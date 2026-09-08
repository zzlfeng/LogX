# LogX Kotlin Facade 编译工程 (T4.0-9 后半部分 / M5)

对仓库内交付源文件 [../kotlin/LogX.kt](../kotlin/LogX.kt) 建立 Gradle 编译与单测链,
终结"Kotlin 源码无编译验证"的历史缺口。

> **T5.4 (2026-09-09) 起双用途**: 编译验证 + 最小真机 smoke。
> `SmokeActivity`(launcher)驱动 `consoleEcho=true` 全链路 ——
> LogX 自动 loadLibrary("logx") → init(ZSTD) → 写 6 条中英文记录 →
> `adb logcat` 断言 echo 行。预编译 so 由 `build-android/` 拷入
> `app/src/main/jniLibs/arm64-v8a/`(strip 后,含 libc++_shared)。

## 关键设计

- **单源引用**: `app/build.gradle.kts` 的 `sourceSets` 直接把 `android/kotlin/`
  挂为源目录,编译验证的就是交付源文件本身,无拷贝漂移;
- **纯编译验证 + 纯逻辑单测**: `LogX.kt` 的 `external fun` 由 JNI 实现
  (`logx_jni.cpp`),JVM 单测环境无 native 库,故单元测试只覆盖不依赖
  native 句柄的纯逻辑 (状态机/参数校验/枚举映射等),JNI 全链路由
  `tests/` 的 C++ gtest、真机批次与 SmokeActivity smoke 覆盖;
- **离线构建**: `gradle.properties` 开 `org.gradle.offline=true`,
  依赖 (AGP 8.5.1 / KGP 1.9.23 / coroutines 1.7.3) 全部命中本机缓存;
- **minSdk 21**: 与 spec §10 全库 API 21 基线一致。

## 构建

```bash
cd android/kotlin-gradle
gradle :app:compileDebugKotlin   # 仅编译验证 (最快)
gradle :app:testDebugUnitTest    # 编译 + 纯逻辑单测
gradle :app:assembleDebug        # smoke APK (需 jniLibs 已放入预编译 so)
```

要求: Gradle 8.9+、JDK 17、Android SDK (compileSdk 34)。
注: 本机 PATH 上的 gradle 若为 4.10.2 等旧版会解析失败,请用
`~/.gradle/wrapper/dists/gradle-8.9-all/*/gradle-8.9/bin/gradle.bat`
并设 `JAVA_HOME` 指向 JDK 17。
