@echo off
REM Build + unit-test the shared C++ engine core (VS-free: CMake + Ninja + g++).
REM Canonical host build; the logic lives in build.ps1 at the repo root.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\build.ps1" %*
