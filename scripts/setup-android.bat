@echo off
REM One-time Android SDK/NDK/JDK/Gradle setup (CLI-only; no Android Studio).
REM Delegates to PowerShell for the download/install logic.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup-android.ps1" %*
