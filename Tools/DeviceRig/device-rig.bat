@echo off
REM Game-agnostic on-device run + debug rig (Android + iOS). Forwards to device-rig.ps1.
REM   Tools\DeviceRig\device-rig.bat -Action run -Matches 3
REM   Tools\DeviceRig\device-rig.bat -Action install -Peer ios -AppleId you@example.com
powershell -ExecutionPolicy Bypass -File "%~dp0device-rig.ps1" %*
