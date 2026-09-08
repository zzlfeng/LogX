plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.example.logx"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.example.logx"
        minSdk = 21      // LogX 全库 API 21 基线 (spec §10)
        targetSdk = 34
        compileOptions {
            sourceCompatibility = JavaVersion.VERSION_17
            targetCompatibility = JavaVersion.VERSION_17
        }
        kotlinOptions {
            jvmTarget = "17"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    // LogX.kt 源码单源:直接引用仓库内 android/kotlin/LogX.kt,
    // 保证被编译验证的就是交付源文件本身(而非拷贝)
    sourceSets {
        getByName("main") {
            java.srcDir(rootDir.parentFile.resolve("kotlin"))
        }
    }
}

dependencies {
    // kotlinx-coroutines:协程 export 依赖(与 LogX.kt import 对齐;
    // 1.8.0 为本机 Gradle 缓存可离线满足的版本)
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.0")
    // 单测:JUnit4 + coroutines-test
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.8.0")
}
