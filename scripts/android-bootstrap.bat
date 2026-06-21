@echo off
REM One-shot Android bring-up: install the SDK toolchain (if needed) then build.
REM The first run downloads several GB. Runbook + known traps: GitHub issue #2.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0android-bootstrap.ps1" %*
