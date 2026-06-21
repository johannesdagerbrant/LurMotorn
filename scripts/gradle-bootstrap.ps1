# Fallback when no system Gradle is present: fetch the pinned Gradle 8.9
# distribution, use it to generate the committable wrapper (gradlew.bat + jar),
# then build the debug APK via the wrapper. Idempotent.
$ErrorActionPreference = 'Stop'

$ver = '8.9'
$gradleHome = Join-Path $env:LOCALAPPDATA "Gradle\gradle-$ver"
$gradleBat  = Join-Path $gradleHome 'bin\gradle.bat'

if (-not (Test-Path $gradleBat)) {
    Write-Host "== Downloading Gradle $ver =="
    $zip = Join-Path $env:TEMP "gradle-$ver-bin.zip"
    if (-not (Test-Path $zip)) {
        Invoke-WebRequest -Uri "https://services.gradle.org/distributions/gradle-$ver-bin.zip" -OutFile $zip
    }
    $dest = Join-Path $env:LOCALAPPDATA 'Gradle'
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Expand-Archive -Path $zip -DestinationPath $dest -Force
}

$androidDir = Join-Path $PSScriptRoot '..\Games\Chess\Android'
Push-Location $androidDir
try {
    Write-Host "== Generating Gradle wrapper ($ver) =="
    & $gradleBat wrapper --gradle-version $ver --distribution-type bin
    if ($LASTEXITCODE -ne 0) { throw "gradle wrapper failed ($LASTEXITCODE)" }

    Write-Host '== Building debug APK (assembleDebug) =='
    & cmd /c 'gradlew.bat assembleDebug --no-daemon'
    if ($LASTEXITCODE -ne 0) { throw "assembleDebug failed ($LASTEXITCODE)" }
} finally { Pop-Location }

Write-Host ''
Write-Host 'Build finished. APK:'
Get-ChildItem -Recurse -Filter '*.apk' (Join-Path $androidDir 'app\build\outputs') |
    Select-Object -ExpandProperty FullName
