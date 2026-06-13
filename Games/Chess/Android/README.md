# OnlyChess — Android app

The Android shell for the chess game. It's deliberately thin: `NativeActivity`
hands control to C++ (`android_main` in `libonlychess.so`), which wires the
platform window to the renderer and runs the shared engine. The only Kotlin is
`BleShim` (the Bluetooth Low Energy radio shim) reached from C++ over JNI.

## What the skeleton does today

- Builds `libonlychess.so` from the **shared C++ core** (pulled in via CMake from
  the repo root) plus the Android glue.
- On launch, brings up a **stub** Vulkan renderer and runs a smoke test of the
  engine (logs "20 legal moves from the start position" via `logcat`).
- BLE transport + Vulkan rendering are **stubs with TODOs** — full implementations
  are tasks #8 (BLE) and #9 (Vulkan).

## Prerequisites

Nothing Android-related is required to build the *engine core* (that's `build.ps1`
at the repo root, VS-free). To build *this app* you need the Android SDK + NDK:

**Option A — Android Studio (GUI):** install it (e.g. `winget install Google.AndroidStudio`),
open `Games/Chess/Android` as a project, let it sync (it generates the Gradle
wrapper and can install the matching SDK/NDK/CMake), then Run.

**Option B — command-line only (VS-free style):** install the Android
`commandline-tools`, then:
```
sdkmanager "platform-tools" "platforms;android-35" "build-tools;35.0.0" "ndk;27.2.12479018" "cmake;3.22.1"
# accept licenses: sdkmanager --licenses
gradle wrapper            # once, to create gradlew + the wrapper jar
./gradlew assembleDebug   # build the APK
./gradlew installDebug    # install onto a connected device/emulator
```

Set `ndkVersion`/`compileSdk` in `app/build.gradle.kts` to match what you install.

> iOS is the sibling target (`Games/Chess/iOS`) and **requires a Mac** — see CLAUDE.md.
