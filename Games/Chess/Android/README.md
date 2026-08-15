# OnlyChess — Android app

The Android shell for the chess game. It's deliberately thin: an activity hands
control to C++ (`android_main` in `libonlychess.so`), which wires the platform
window to the renderer and runs the shared engine. The only Kotlin in *this folder*
is `OnlyChessActivity.kt`; the BLE radio shim it loads
(`Modules/Transport/Platform/Android/BleShim.kt`) lives in the engine and is shared
with RPS, bound from C++ via a `RegisterNatives` table at `JNI_OnLoad`.

## What it is today

A complete, playable chess app: full Vulkan rendering, a real BLE link to an
iPhone, per-opponent persistence, and the dev console. The platform layer it used
to own — BLE radio, Vulkan surface, audio device, entry point — now lives under
`Modules/*/Platform/Android/` and is shared with RPS.

Because the app target is out-of-tree, `app/src/main/cpp/CMakeLists.txt` re-applies
the derived `LUR_*` cache vars to its own target and sets the two required per-app
definitions, `LUR_LOG_TAG` (`OnlyChess`) and `LUR_BLE_SERVICE_UUID`. A new `LUR_*`
macro must be added here by hand — there is one such block per game per platform
(issue #198).

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

## Build configurations (`LUR_CONFIG` is the one dial)

The native library's tooling/asserts **and** its optimization are both driven by a
single dial, `LUR_CONFIG` (Unreal-style ladder). Gradle forwards it to CMake, and
`cmake/EngineFlags.cmake` derives everything from it — so the everyday
`installDebug` ships **optimized** native code, not `-O0`. Select it with
`-PlurConfig=<config>`:

| `-PlurConfig=` | `LUR_INTERNAL` (console, CVars) | `LUR_ASSERTS` | `LUR_SLOW` | Native opt (`CMAKE_BUILD_TYPE`) |
|---|---|---|---|---|
| `Development` *(default)* | on | on | off | `RelWithDebInfo` (**-O2 -g**) |
| `Debugging` | on | on | on | `Debug` (**-O0 -g**) |
| `Shipping` | off | off | off | `RelWithDebInfo` (-O2) |

```
./gradlew installDebug                        # optimized Development (the default loop)
./gradlew installDebug -PlurConfig=Debugging  # -O0 -g, slow checks, fully steppable
```

Asserts stay on in `Development` even though it's optimized — `LUR_ASSERTS` is
decoupled from `NDEBUG` on purpose (see `cmake/EngineFlags.cmake`). The host
unit-test loop (`build.ps1`) instead passes `-DLUR_FAST=ON` to keep `-O0` for fast
compiles (correctness, not speed).

> iOS is the sibling target (`Games/Chess/iOS`) and **requires a Mac** — see CLAUDE.md.
