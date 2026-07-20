@echo off
REM TraceAndroid entry point (issue #101). Forwards args to the PowerShell orchestrator.
REM Examples:
REM   Tools\TraceAndroid\trace-android.bat -Label after-perf2 -DurationSec 60
REM   Tools\TraceAndroid\trace-android.bat -App rps -NoPeer -DurationSec 30
powershell -ExecutionPolicy Bypass -File "%~dp0trace-android.ps1" %*
