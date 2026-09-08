# LogX Kotlin Facade 编译工程 (T4.0-9 后半部分 / M5)

对仓库内交付源文件 [../kotlin/LogX.kt](../kotlin/LogX.kt) 建立 Gradle 编译与单测链,
终结"Kotlin 源码无编译验证"的历史缺口。

## 关键设计

- **单源引用**: `app/build.gradle.kts` 的 `sourceSets` 直接把 `android/kotlin/`
  挂为源目录,编译验证的就是交付源文件本身,无拷贝漂移;
- **纯编译验证 + 纯逻辑单测**: `LogX.kt` 的 `external fun` 由 JNI 实现
  (`logx_jni.cpp`),JVM 单测环境无 native 库,故单元测试只覆盖不依赖
  native 句柄的纯逻辑 (状态机/参数校验/枚举映射等),JNI 全链路由
  `tests/` 的 C++ gtest 与真机批次覆盖;
- **离线构建**: `gradle.properties` 开 `org.gradle.offline=true`,
  依赖 (AGP 8.5.1 / KGP 1.9.23 / coroutines 1.7.3) 全部命中本机缓存;
- **minSdk 21**: 与 spec §10 全库 API 21 基线一致。

## 构建

```bash
cd android/kotlin-gradle
gradle :app:compileDebugKotlin   # 仅编译验证 (最快)
gradle :app:testDebugUnitTest    # 编译 + 纯逻辑单测
```

要求: Gradle 8.9+、JDK 17、Android SDK (compileSdk 34)。
