plugins {
    id("com.android.application")
}

android {
    namespace = "com.logx.consumer"
    compileSdk {
        version = release(37) {
            minorApiLevel = 1
        }
    }

    defaultConfig {
        applicationId = "com.logx.consumer"
        minSdk = 24
        targetSdk = 37
        versionCode = 1
        versionName = "1.0"

        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
}

dependencies {
    implementation(files("../../logx-aar/logx/build/outputs/aar/logx-release.aar"))
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.0")
}
