@echo off
REM Install the debug APK onto a connected device/emulator.
setlocal
cd /d "%~dp0..\Games\Chess\Android"
if exist gradlew.bat (call gradlew.bat installDebug %*) else (call gradle installDebug %*)
