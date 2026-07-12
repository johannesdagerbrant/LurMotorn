# scripts/

Canonical entry points for common actions, so the build/run workflow is consistent
for humans and future agents. Prefer these over ad-hoc commands.

| Script | What it does |
|---|---|
| `build.bat` | Build + unit-test the shared C++ engine core (VS-free: CMake + Ninja + g++). Wraps `build.ps1`. |
| `clean.bat` | Delete host + Android build output for a from-scratch rebuild. |
| `setup-android.bat` | One-time: install JDK + Gradle + the Android SDK/NDK/CMake (CLI-only, no Android Studio). |
| `android-build.bat` | Build the Android debug APK (`assembleDebug`). |
| `android-install.bat` | Install the APK onto a connected device/emulator (`installDebug`). |
| `logcat.bat` | Tail the Android app's native log output (the `OnlyChess` tag). |
| `setup-ios-logging.bat` | One-time: install `pymobiledevice3` (reads a connected iPhone's syslog on Windows). |
| `ios-syslog.bat` | Tail the iOS app's live syslog over USB (the `OnlyChess:` tag) — the iPhone counterpart of `logcat.bat`. |

The shared C++ core needs **no** Android tooling — just `build.bat`. The `android-*`
scripts require `setup-android.bat` to have been run once.

The iOS app is built on macOS CI (Windows can't build it), but its **logs are
readable on Windows**: `setup-ios-logging.bat` once, then `ios-syslog.bat` streams
the connected iPhone's `OnlyChess:` log lines live — no Mac, Xcode, or on-device
HUD needed. Needs Apple Mobile Device Service (from iTunes) for the USB channel.
