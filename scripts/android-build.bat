@echo off
REM Build the Android debug APK. Requires the Android SDK/NDK + JDK + Gradle
REM (run scripts\setup-android.bat once first).
setlocal
cd /d "%~dp0..\Games\Chess\Android"
if exist gradlew.bat (call gradlew.bat assembleDebug %*) else (call gradle assembleDebug %*)
