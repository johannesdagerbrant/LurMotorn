# OnlyRps — Android app

The Android shell for RocksPapersScissors. Like the chess app it's deliberately
thin: `NativeActivity` hands control to C++ (`android_main` in `libonlyrps.so`),
which wires the platform window to the renderer and runs the shared engine + RPS
game code. The only Kotlin is `BleShim` (the Bluetooth Low Energy radio shim)
reached from C++ over JNI.

## Prerequisites

Nothing Android-related is required to build the *engine core* (that's `build.ps1`
at the repo root, VS-free). To build *this app* you need the Android SDK + NDK —
same setup as the chess app (`Games/Chess/Android/README.md`). A committed
`gradlew`/`gradlew.bat` wrapper exists here; from the repo the reliable loop is:

```
./gradlew assembleDebug   # build the APK
./gradlew installDebug    # install onto a connected device
```

## Build configurations (`LUR_CONFIG` is the one dial)

The native library's tooling/asserts **and** its optimization are both driven by a
single dial, `LUR_CONFIG` (Unreal-style ladder). Gradle forwards it to CMake, and
`cmake/EngineFlags.cmake` derives everything from it — so the everyday
`installDebug` ships **optimized** native code, not `-O0` (this is issue #89: the
phone was previously benchmarking an unoptimized build). Select it with
`-PlurConfig=<config>`:

| `-PlurConfig=` | `LUR_INTERNAL` (bots/soak) | `LUR_ASSERTS` | `LUR_SLOW` | Native opt (`CMAKE_BUILD_TYPE`) |
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

> iOS is the sibling target (`Games/RocksPapersScissors/iOS`) and **requires a
> Mac** — see CLAUDE.md.
