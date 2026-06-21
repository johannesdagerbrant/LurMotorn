# Android bring-up in one shot: install the SDK toolchain (if not already there),
# then build the debug app. After this you either have a built APK or clear
# compiler errors to fix.
#
# Run via scripts\android-bootstrap.bat — do it as a BACKGROUND task with the
# sandbox disabled; the first run downloads several GB (NDK, etc.).
# Full runbook + known traps: GitHub issue #2.
$ErrorActionPreference = 'Stop'

# 1) Install JDK + Gradle + Android SDK/NDK/CMake (idempotent; skips what exists).
& "$PSScriptRoot\setup-android.ps1"

# 2) Make the freshly-installed tools visible in this process.
$env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
            [Environment]::GetEnvironmentVariable('Path', 'User')

# 3) Build the debug app.
Push-Location (Join-Path $PSScriptRoot '..\Games\Chess\Android')
try {
    if (Test-Path '.\gradlew.bat') { & cmd /c 'gradlew.bat assembleDebug' }
    else                           { & gradle assembleDebug }
} finally { Pop-Location }

Write-Host ''
Write-Host 'Android bring-up finished. If it built, install + check the smoke test:'
Write-Host '  scripts\android-install.bat   (onto a connected device/emulator)'
Write-Host '  scripts\logcat.bat            (look for: 20 legal moves from the start position)'
