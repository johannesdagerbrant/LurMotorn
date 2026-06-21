# One-time Android toolchain setup for LurMotorn — CLI-only ("option 2", no
# Android Studio). Installs a JDK + Gradle (winget), the Android command-line
# tools, then the SDK packages (platform, build-tools, NDK, CMake), accepts
# licenses, sets ANDROID_HOME + PATH, and writes local.properties. Re-runnable.
#
# Run via: scripts\setup-android.bat
$ErrorActionPreference = 'Stop'

function Have($name) { [bool](Get-Command $name -ErrorAction SilentlyContinue) }
function Refresh-Path {
    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
                [Environment]::GetEnvironmentVariable('Path', 'User')
}
Refresh-Path

# 1) JDK 17 — Gradle and sdkmanager need a JDK.
if (-not (Have 'java')) {
    Write-Host '== Installing Microsoft OpenJDK 17 =='
    winget install --id Microsoft.OpenJDK.17 -e --accept-package-agreements --accept-source-agreements --silent --disable-interactivity
    Refresh-Path
}

# 2) Gradle.
if (-not (Have 'gradle')) {
    Write-Host '== Installing Gradle =='
    winget install --id Gradle.Gradle -e --accept-package-agreements --accept-source-agreements --silent --disable-interactivity
    Refresh-Path
}

# 3) SDK root + ANDROID_HOME.
$sdk = Join-Path $env:LOCALAPPDATA 'Android\Sdk'
New-Item -ItemType Directory -Force -Path $sdk | Out-Null
[Environment]::SetEnvironmentVariable('ANDROID_HOME', $sdk, 'User')
[Environment]::SetEnvironmentVariable('ANDROID_SDK_ROOT', $sdk, 'User')
$env:ANDROID_HOME = $sdk
$env:ANDROID_SDK_ROOT = $sdk

# 4) Android command-line tools (sdkmanager) under cmdline-tools\latest.
$cmdlineBin = Join-Path $sdk 'cmdline-tools\latest\bin'
if (-not (Test-Path (Join-Path $cmdlineBin 'sdkmanager.bat'))) {
    Write-Host '== Downloading Android command-line tools =='
    $zip = Join-Path $env:TEMP 'android-cmdline-tools.zip'
    # If this build number 404s, grab the current "command line tools" URL from
    # https://developer.android.com/studio#command-line-tools-only and update it.
    $url = 'https://dl.google.com/android/repository/commandlinetools-win-11076708_latest.zip'
    Invoke-WebRequest -Uri $url -OutFile $zip
    $tmp = Join-Path $env:TEMP 'android-cmdline-extract'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    Expand-Archive -Path $zip -DestinationPath $tmp
    New-Item -ItemType Directory -Force -Path (Join-Path $sdk 'cmdline-tools\latest') | Out-Null
    Copy-Item -Recurse -Force (Join-Path $tmp 'cmdline-tools\*') (Join-Path $sdk 'cmdline-tools\latest')
}
$sdkmanager = Join-Path $cmdlineBin 'sdkmanager.bat'

# 5) Accept all SDK licenses FIRST, then install packages. We feed "y" to
#    sdkmanager from a file via cmd's input redirect, because piping from
#    PowerShell to the sdkmanager .bat does not reliably reach its prompt.
$yesFile = Join-Path $env:TEMP 'lur_sdk_yes.txt'
Set-Content -LiteralPath $yesFile -Value ((1..200 | ForEach-Object { 'y' }) -join "`r`n") -Encoding ascii

Write-Host '== Accepting SDK licenses =='
cmd /c "`"$sdkmanager`" `"--sdk_root=$sdk`" --licenses < `"$yesFile`""

Write-Host '== Installing SDK packages (platform-tools, platform-35, build-tools, NDK, cmake) =='
cmd /c "`"$sdkmanager`" `"--sdk_root=$sdk`" `"platform-tools`" `"platforms;android-35`" `"build-tools;35.0.0`" `"ndk;27.2.12479018`" `"cmake;3.22.1`" < `"$yesFile`""

# 6) Add platform-tools (adb) + cmdline-tools to the user PATH.
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
foreach ($p in @((Join-Path $sdk 'platform-tools'), $cmdlineBin)) {
    if ($userPath -notlike "*$p*") { $userPath = "$userPath;$p" }
}
[Environment]::SetEnvironmentVariable('Path', $userPath, 'User')

# 7) local.properties so Gradle finds the SDK.
$androidDir = Join-Path $PSScriptRoot '..\Games\Chess\Android'
$lp = Join-Path $androidDir 'local.properties'
"sdk.dir=$($sdk -replace '\\', '\\')" | Set-Content -LiteralPath $lp -Encoding ascii

Write-Host ''
Write-Host "Android toolchain ready. ANDROID_HOME = $sdk"
Write-Host "Next (new shell, so PATH refreshes): scripts\android-build.bat"
