# LurMotorn

A from-scratch, near-dependency-free engine for **ultra-low-latency local multiplayer games**, and
its first game, **Chess**. Two phones — Android *or* iPhone — pair over Bluetooth Low Energy and
play locally, with the smallest possible data crossing the wire.

> Design north star: the most responsive local-multiplayer experience possible, built almost
> entirely ourselves — only the unavoidable OS frameworks (Android SDK / NDK, Apple SDK) and a
> single GPU API (**Vulkan everywhere**, via **MoltenVK** on iOS — the one sanctioned dependency,
> since no native GPU API spans both platforms).

## Architecture at a glance

```
LurMotorn/                      ← the engine (this repo root)
  Modules/                      ← game-agnostic engine modules
    Serialization/   ✅ pure C++  — BitWriter/BitReader/Varint  (the "slim bytes" toolkit)
    Sim/             ✅ pure C++  — deterministic fixed-tick loop + fixed-point math (gameplay)
    Math/            ✅ pure C++  — vec/mat/quat for render + scene transforms (float)
    Net/             ✅ pure C++  — session lifecycle, clock-sync (rollback netcode later)
    Transport/       ⚙  C++ interface + BLE backend  (Android: JNI→Kotlin · iOS: CoreBluetooth)
    Pairing/         ⚙  C++ interface + BLE discovery (no NFC — iOS can't peer-NFC with Android)
    Render/          ⚙  C++ interface + single Vulkan backend (MoltenVK on iOS), 3D-capable
    Input/           ⚙  per-platform touch glue
  Games/
    Chess/
      Core/          ✅ pure C++  — rules, legal move-gen, move codec  (shared verbatim)
      Android/       Android Studio project (Kotlin shim + C++ + Vulkan)
      iOS/           Xcode project (Swift shim + C++ + Vulkan via MoltenVK)
```

**The rule that keeps the engine reusable:** `Games/*` may depend on `Modules/*`; `Modules/*`
may **never** depend on `Games/*`. Everything that *can* be pure C++ is shared verbatim across
host, Android, and iOS; only the three things that touch hardware — radio, GPU, screen — get
per-OS backends behind a common C++ interface.

The renderer is **3D-capable by design** (meshes + depth + camera + materials); the 2D chess board
is the orthographic special case (`Render/Sprite2D.h`). Each module is laid out Unreal-style: public
headers in `Public/Lur/<Module>/`, private sources in `Private/`. See [CLAUDE.md](CLAUDE.md) for the
naming convention.

## Key decisions (and why)

| Decision | Rationale |
|---|---|
| **C++ core** | Compiles natively on both Android (NDK) and iOS. ~80% of the code is shared verbatim. |
| **BLE for transport + pairing** | The *only* serverless local P2P that works **between** iOS and Android. (Bluetooth Classic/RFCOMM is iOS-locked; NFC peer handover isn't supported on iOS.) |
| **Single Vulkan renderer (MoltenVK on iOS)** | No native GPU API spans both platforms, so rather than write Vulkan *and* Metal, we write Vulkan once and run it through MoltenVK on iOS. Keeps the renderer 3D-capable and single-source. MoltenVK is the one allowed dependency. |
| **Smallest possible payload** | A chess move is encoded as an index into the *deterministic* legal-move list — typically 4–6 bits. See `Modules/Serialization` + `Games/Chess/Core` (codec). |
| **No servers, ever** | Strictly local play. No backend, no internet relay. |

## Building the shared core on your desktop

The pure-C++ modules build and unit-test on your dev machine — no phone required, no Visual Studio.
One command:

```
powershell -ExecutionPolicy Bypass -File build.ps1
```

It configures (Ninja), builds, and runs the codec tests. The VS-free toolchain installs via winget:

```
winget install Kitware.CMake
winget install Ninja-build.Ninja
winget install BrechtSanders.WinLibs.POSIX.UCRT   # MinGW-w64 GCC (UCRT)
```

The Android and iOS apps are built from `Games/Chess/Android` (Gradle) and `Games/Chess/iOS`
(Xcode, linking MoltenVK) respectively; those wire the same C++ core in via each platform's native
build.
