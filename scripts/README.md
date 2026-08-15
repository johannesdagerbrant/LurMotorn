# scripts/

Canonical entry points for common actions, so the build/run workflow is consistent
for humans and future agents. Prefer these over ad-hoc commands.

**Several of these are chess-only** — marked *(chess)* below. They hardcode
`Games/Chess/Android` or the `OnlyChess` log tag and predate the second game; RPS builds
via its own Gradle wrapper and logs under `OnlyRps`. Unifying them is issue #198.

| Script | What it does |
|---|---|
| `build.bat` | Build + unit-test the shared C++ engine core (VS-free: CMake + Ninja + g++). Wraps `build.ps1`. |
| `clean.bat` | Delete host + Android build output for a from-scratch rebuild. |
| `desktop-build.ps1` | Build the chess desktop harness (two peers in one process). |
| `rps-desktop-build.ps1` | Build the RPS desktop harness. **The playtest-tuning loop** — use this, not `build.ps1`, while iterating on tunables; kill the old exe before relinking. |
| `setup-android.bat` | One-time: install JDK + Gradle + the Android SDK/NDK/CMake (CLI-only, no Android Studio). |
| `android-bootstrap.bat` | One-time Android environment bootstrap (wraps `android-bootstrap.ps1`; `gradle-bootstrap.ps1` is its Gradle half). |
| `android-build.bat` | *(chess)* Build the Android debug APK (`assembleDebug`). |
| `android-install.bat` | *(chess)* Install the APK onto a connected device/emulator (`installDebug`). |
| `logcat.bat` | *(chess)* Tail the Android app's native log output (the `OnlyChess` tag). |
| `gen-icon.ps1` | Turn an SVG into a cook-ready silhouette PNG (headless-Chromium rasterize + Pillow cleanup). `-Src author/name` fetches a game-icons.net icon (CC BY 3.0 — credit the author) or pass a local `.svg`; `-Name` sets the output. Then add it to the game's `LUR_COOK` marker + glyph enum and `build.ps1` re-cooks. (Helper: `icon_clean.py`.) |
| `gen-font.ps1` | Bake an OFL font into a committed MSDF atlas header via `msdf-atlas-gen` (offline; never linked into the app). |
| `gen-shaders.ps1` | Compile GLSL to committed SPIR-V headers. |
| `check-build-fingerprint.ps1` | CI guard: the build fingerprint is stable and wired. |
| `check-cvar-zero-overhead.ps1` | CI guard: `CVar<T>::Get()` folds to its `constexpr` default under `LUR_SHIPPING` (disassembly diff). |
| `setup-ios-logging.bat` | One-time: install `pymobiledevice3` (reads a connected iPhone's syslog on Windows). |
| `ios-syslog.bat` | *(chess)* Tail the iOS app's live syslog over USB (the `OnlyChess:` tag) — the iPhone counterpart of `logcat.bat`. |

The shared C++ core needs **no** Android tooling — just `build.bat`. The `android-*`
scripts require `setup-android.bat` to have been run once.

Note `build.bat` green says nothing about the platform layer: `Modules/*/Platform/*`, the
Vulkan backend and every `.mm` compile only in app builds. Compile-check Android after
touching those.

The iOS app is built on macOS CI (Windows can't build it), but its **logs are
readable on Windows**: `setup-ios-logging.bat` once, then `ios-syslog.bat` streams
the connected iPhone's `OnlyChess:` log lines live — no Mac, Xcode, or on-device
HUD needed. Needs Apple Mobile Device Service (from iTunes) for the USB channel.

## BLE dev rig (Windows ↔ Android, issue #58)

The **Workbench's third radio** — a real BLE central on Windows (under a debugger,
with `Lur::Log` + the flight recorder) talking the engine's real wire protocol to the
**unmodified** Android app. It lives in `Tools\BleDevRig\` (a dev instrument, not
shipped engine), driven by one command:

```
Tools\BleDevRig\dev-rig.bat            REM build + install + play + 1 reconnect + capture
```

It builds the C# WinRT radio (`BleRadio.exe`) + the `onlychess_desktop --ble`
endpoint, (re)connects wireless adb, installs the app and forces it to advertise as a
peripheral, launches the desktop peer, forces ≥1 link-death reconnect mid-run
(`svc bluetooth disable/enable`), tails both logs into `Tools\BleDevRig\.logs\`, and
pulls the flight-recorder evidence on exit. See `Tools\BleDevRig\README.md`.

**Operational notes (read before a soak session):**

- **Disable the BLE adapter's power management** — Device Manager → the Bluetooth
  adapter → Power Management → untick *"Allow the computer to turn off this device"*.
  Otherwise Windows suspends the radio mid-run and produces **phantom link deaths**
  that mimic the very bug class the rig hunts.
- **Check the adapter's LMP version** — adapter properties → Advanced → *LMP*. `9+`
  = Bluetooth 5; a 4.2-era laptop radio (LMP 8) caps PC↔phone throughput **below**
  the phone↔phone ceiling, so don't trust throughput numbers from an old radio.
- Windows' BLE stack ≠ iOS's, so this is a **development instrument, not final
  validation** — phone↔phone soak remains the truth.
