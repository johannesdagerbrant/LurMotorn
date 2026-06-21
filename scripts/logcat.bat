@echo off
REM Tail the app's native log output. Requires adb (platform-tools) on PATH.
adb logcat -s OnlyChess:* %*
