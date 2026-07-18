@echo off
REM One-command Windows<->Android BLE dev loop (issue #58). Forwards to dev-rig.ps1.
REM   Tools\BleDevRig\dev-rig.bat [-Serial X] [-SkipBuild] [-Reconnects N] [-DurationSec S]
powershell -ExecutionPolicy Bypass -File "%~dp0dev-rig.ps1" %*
