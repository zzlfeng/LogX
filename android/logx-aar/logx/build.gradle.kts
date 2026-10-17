plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
    id("maven-publish")
}

// 默认 ON;仅 T-Z0 的「有/无 -g」A/B 对照可传 -PlogxKeepSymbols=OFF。
val logxKeepSymbols = (findProperty("logxKeepSymbols") as String?) ?: "ON"
// D6-B2: 写入热路径 TU 单独 -O2;可传 -PlogxHotpathO2=OFF 做方案 A/B 对照。
val logxHotpathO2 = (findProperty("logxHotpathO2") as String?) ?: "ON"

android {
    namespace = "com.zzlfeng.logx"
    compileSdk = 34
    // 与 build-android-tests 验证链同款 NDK(CMakeCache 实录 29.0.14206865)
    ndkVersion = "29.0.14206865"

    defaultConfig {
        minSdk = 21
        consumerProguardFiles("consumer-rules.pro")
        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DLOGX_AAR_NATIVE_MIN_SIZE=ON",
                    "-DLOGX_AAR_KEEP_SYMBOLS=$logxKeepSymbols",
                    "-DLOGX_HOTPATH_O2=$logxHotpathO2",
                    // 2026-10-17 拍板:c++_shared,使 liblogx.so 门禁与 benchmark 口径一致;
                    // AAR 会同时打包 libc++_shared.so,总 native 载荷单独记录。
                    "-DANDROID_STL=c++_shared",
                )
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
    // LogX.kt 源码单源:直接引用仓库内 android/kotlin/LogX.kt,
    // 保证被编译验证的就是交付源文件本身(与 kotlin-gradle 同哲学)
    sourceSets {
        getByName("main") {
            java.srcDir(rootDir.parentFile.resolve("kotlin"))
        }
    }
    publishing {
        singleVariant("release")
    }
}

dependencies {
    // 协程 export (suspend) 签名对外暴露 coroutines 类型 → api 语义。
    // 1.8.0 为本机 Gradle 缓存可离线满足的版本(与 kotlin-gradle 对齐)。
    api("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.0")
}

afterEvaluate {
    publishing {
        publications {
            create<MavenPublication>("release") {
                groupId = "com.zzlfeng"
                artifactId = "logx"
                version = "1.0.0"
                from(components["release"])
            }
        }
    }
}
