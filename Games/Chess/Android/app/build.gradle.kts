plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.lurmotorn.onlychess"
    compileSdk = 35

    // The engine's Android platform sources (the BLE radio driver) compile INTO this app, exactly
    // as the C++ engine modules do via add_subdirectory. One copy, shared by every game: the class
    // lives in com.lurmotorn.engine and binds its natives by RegisterNatives, so it no longer has
    // to sit in this app's package.
    // Path is from the ANDROID PROJECT root (Games/<Game>/Android), the same anchor
    // cpp/CMakeLists.txt walks up from — keep the two in step if either moves.
    sourceSets["main"].kotlin.srcDir("$rootDir/../../../Modules/Transport/Platform/Android")
    // The app SHELL — the NativeActivity subclass and the manifest — is shared too (#198).
    // There was one Activity per game with 30 of 46 lines byte-identical and the rest the
    // same comments re-wrapped, and a manifest that differed by zero lines once the names
    // were normalized. Both are parameterized by the manifestPlaceholders below, so this
    // game states its identity ONCE, here, next to its application id.
    sourceSets["main"].kotlin.srcDir("$rootDir/../../Shared/Android/kotlin")
    sourceSets["main"].manifest.srcFile("$rootDir/../../Shared/Android/AndroidManifest.xml")

    defaultConfig {
        applicationId = "com.lurmotorn.onlychess"
        // Consumed by the shared manifest AND read back by LurActivity at runtime:
        // libName is the .so android_main lives in, logTag is what `logcat -s <tag>`
        // filters on. Neither has a default anywhere — a game that omits one fails
        // rather than inheriting another game's identity.
        manifestPlaceholders["libName"] = "onlychess"
        manifestPlaceholders["logTag"] = "OnlyChess"
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
                // Perf #89: the everyday install must be OPTIMIZED. Pass the single
                // LUR_CONFIG dial through; EngineFlags.cmake couples native
                // optimization to it (Development/Shipping -> RelWithDebInfo,
                // Debugging -> -O0 -g), overriding AGP's default Debug (-O0).
                // Build a slow, fully-debuggable native lib with -PlurConfig=Debugging.
                val lurConfig = (project.findProperty("lurConfig") as String?) ?: "Development"
                arguments += "-DANDROID_STL=c++_static"
                arguments += "-DLUR_CONFIG=$lurConfig"
                // LUR_AGENT (CLAUDE.md, #195): assistant-only remote control — the autoplay
                // hook's system property and its injected input, so an assistant can drive a
                // two-phone scenario without a human tapping. OFF unless asked for with
                // -PlurAgent=ON, absent from every ordinary build including Development, and
                // force-zeroed in Shipping by EngineFlags. A build made with this on must not
                // be handed to a player: rebuild without it (the code is then ABSENT, not
                // idle) and clear the channel — `adb shell setprop debug.lur.autoplay ""`.
                val lurAgent = (project.findProperty("lurAgent") as String?) ?: "OFF"
                arguments += "-DLUR_AGENT=$lurAgent"
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
