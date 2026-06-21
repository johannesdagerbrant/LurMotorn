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
| `logcat.bat` | Tail the app's native log output (the `OnlyChess` tag). |

The shared C++ core needs **no** Android tooling — just `build.bat`. The `android-*`
scripts require `setup-android.bat` to have been run once.
