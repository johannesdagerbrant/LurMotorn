plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.lurmotorn.onlyrps"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.lurmotorn.onlyrps"
        minSdk = 26          // BLE + Vulkan baseline
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            // Real devices are arm64; x86_64 lets it run on the emulator.
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=c++_static"
            }
        }
    }

    // The native build is driven by our own CMake, which pulls in the shared
    // C++ engine + chess core from the repo root.
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    // Match an NDK you have installed (sdkmanager "ndk;<version>").
    ndkVersion = "27.2.12479018"

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
}

dependencies {
    // Intentionally none — no third-party libraries. Only the Android framework
    // (NativeActivity, Bluetooth, Vulkan) and our own C++ core.
}
