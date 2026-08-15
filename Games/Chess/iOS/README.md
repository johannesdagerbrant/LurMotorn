# OnlyChess — iOS app

The iOS shell for the chess game, sibling to `Games/Chess/Android`. Deliberately
thin: an Obj-C++ entry point hands all real work to the shared C++ engine.
**Requires a Mac to build** (iOS cannot be built on Windows) — so in practice it is
built on the **GitHub Actions macOS runner**, never opened in Xcode locally.

## What it is today

A complete, playable chess app: full Vulkan rendering through **MoltenVK**, a real
BLE link to an Android phone, per-opponent persistence, and the dev console.

The shell itself is now only `Sources/AppMain.mm`. Everything that used to live here
has moved into the engine and is shared with RPS:

| Was here | Now |
| --- | --- |
| `Sources/IosBleTransport.mm` | `Modules/Transport/Platform/Ios/BleTransport.mm` |
| Vulkan surface creation | `Modules/Render/Platform/Ios/VulkanSurface.mm` |
| Audio device | `Modules/Audio/Platform/Ios/AudioDevice.mm` |
| `UIApplicationMain`, the app delegate, the Metal view, the #73 reattach heal | `Modules/App/Platform/Ios/` (`IosApp.mm`, `IosViewHost.mm`, `AppPlatform.mm`) |
| Session + persistence choreography | `Lur::App::GameHost` (`Modules/App`) |

## Why CMake (not XcodeGen / SwiftPM / a hand-written pbxproj)

The whole repo is CMake. This app's `CMakeLists.txt` pulls the shared core in with
the SAME `add_subdirectory(<repo root>)` the Android build uses — one source of
truth, no duplicated build description, no pre-built `.a` files to wire up. CMake
(>= 3.14) cross-compiles for iOS natively and **generates** the `.xcodeproj`, so no
human ever opens or hand-edits an Xcode project. XcodeGen would add a brew tool + a
second project schema; a hand-written `project.pbxproj` is famously fragile; SwiftPM
doesn't produce an app bundle. CMake is the minimal, reproducible, consistent choice.

## How it's built (CI, macOS runner, no signing)

The real recipe is `.github/workflows/macos-ci.yml` — it targets a **device**
(`iphoneos`, `generic/platform=iOS`), downloads MoltenVK, and packages an unsigned
`.ipa`. In outline:

```sh
cmake -S Games/Chess/iOS -B Games/Chess/iOS/build -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DMOLTENVK_DIR=<path to MoltenVK> \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO

xcodebuild -project Games/Chess/iOS/build/OnlyChess.xcodeproj \
  -scheme OnlyChess -configuration Debug \
  -sdk iphoneos -destination 'generic/platform=iOS' \
  CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" build
```

**`-configuration Debug` means `-O0`.** The Xcode generator is multi-config, so it
ignores the `LUR_CONFIG` → `CMAKE_BUILD_TYPE` coupling that gives Android its `-O2`.
Never compare perf against an Android build without fixing this first — it once
produced a 20× `sim.step` gap that was purely the build. Issue **#198** owns the fix.

An **agent** build (`-DLUR_AGENT=ON`, for the harness) comes from a
manual-dispatch-only job so it can never sit next to the player artifact:

```sh
gh workflow run "macOS CI" -f agent=true
```

## Files

| File | Purpose |
| --- | --- |
| `CMakeLists.txt` | Builds the `.app`, pulls in the shared core + engine platform layer, links Apple frameworks and MoltenVK. Sets the required `LUR_LOG_TAG` and `LUR_BLE_SERVICE_UUID`. |
| `Info.plist.in` | Bundle metadata + the mandatory `NSBluetooth*UsageDescription` strings. |
| `Sources/AppMain.mm` | Entry point: builds the game, hands the lifecycle to `Lur::App`. |

> Android is the sibling target (`Games/Chess/Android`). See repo-root `CLAUDE.md`.
