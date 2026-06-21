# OnlyChess — iOS app

The iOS shell for the chess game, sibling to `Games/Chess/Android`. Deliberately
thin: a single UIKit view controller (Obj-C++) hands all real work to the shared
C++ engine. **Requires a Mac to build** (iOS cannot be built on Windows) — so in
practice it is built on the **GitHub Actions macOS runner**, never opened in Xcode
locally.

## What the skeleton does today

- Builds an `OnlyChess.app` from the **shared C++ core** (pulled in via CMake from
  the repo root, exactly like the Android app) plus a thin Obj-C++ shim.
- On launch, runs a smoke test of the engine and logs / shows
  **"20 legal moves from the start position"** — proving the shared, perft-verified
  chess core compiles and runs on iOS.
- Brings up the **CoreBluetooth** BLE backend of `Lur::Transport::ITransport`
  (`IosBleTransport.mm`), advertising + scanning for the SHARED service/UUIDs in
  `BleProtocol.h` so it can later do a cross-platform BLE test against Android.
- **No renderer / Vulkan / MoltenVK** yet — graphics are deferred (issue #9).

## Why CMake (not XcodeGen / SwiftPM / a hand-written pbxproj)

The whole repo is CMake. This app's `CMakeLists.txt` pulls the shared core in with
the SAME `add_subdirectory(<repo root>)` the Android build uses — one source of
truth, no duplicated build description, no pre-built `.a` files to wire up. CMake
(>= 3.14) cross-compiles for iOS natively and **generates** the `.xcodeproj`, so no
human ever opens or hand-edits an Xcode project. XcodeGen would add a brew tool + a
second project schema; a hand-written `project.pbxproj` is famously fragile; SwiftPM
doesn't produce an app bundle. CMake is the minimal, reproducible, consistent choice.

## How it's built (CI, macOS runner, no signing)

```sh
# 1. Generate the Xcode project for the iOS Simulator (arm64), signing disabled.
cmake -S Games/Chess/iOS -B Games/Chess/iOS/build -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO

# 2. Build the generated project for the simulator (no cert needed).
xcodebuild -project Games/Chess/iOS/build/OnlyChess.xcodeproj \
  -scheme OnlyChess \
  -configuration Debug \
  -sdk iphonesimulator \
  CODE_SIGNING_ALLOWED=NO \
  build
```

`CMAKE_OSX_ARCHITECTURES=arm64` targets Apple-silicon simulators (GitHub's
`macos-latest` runners are arm64). To also cover Intel simulators, use
`arm64;x86_64`.

## Files

| File | Purpose |
| --- | --- |
| `CMakeLists.txt` | Builds the `.app`, pulls in the shared core, links Apple frameworks. |
| `Info.plist.in` | Bundle metadata + the mandatory `NSBluetooth*UsageDescription` strings. |
| `Sources/AppMain.mm` | UIKit entry point; runs the chess-core smoke test. |
| `Sources/IosBleTransport.mm` | CoreBluetooth backend of `Lur::Transport::ITransport`. |

> Android is the sibling target (`Games/Chess/Android`). See repo-root `CLAUDE.md`.
